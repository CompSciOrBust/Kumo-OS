#pragma once
#include <kumo/ipc/message.hpp>
#include <kumo/mutex.hpp>
#include <mutex>
#include <memory>
#include <span>

namespace kumo::ipc {
// Multiple named ports with single-threaded or concurrent dispatch. Capacities
// include returned-object sessions and cloned sessions as well as SM accepts.
class ObjectServer {
  public:
    static constexpr size_t MaxPorts = 4, DefaultSessions = 60, MaxSessions = 256, MaxDomains = MaxSessions,
                            DefaultObjects = 128, MaxWorkers = 16,
                            PointerBytes = 0x800;
    struct Limits {
        size_t sessions = DefaultSessions;
        // Each independent root uses a record before any domain conversion.
        size_t domains = DefaultSessions;
        // Counts converted roots and returned objects. Larger tables use the heap.
        size_t objects = DefaultObjects;
        size_t converted_domains = MaxDomains;
    };
    struct Service {
        const char *name;
        size_t max_sessions; // Sessions accepted directly from this port.
        void *context;
        ObjectRef (*open)(void *);
    };
    ObjectServer() = default;
    explicit ObjectServer(Limits limits) : limits_(limits) {}
    ObjectServer(const ObjectServer &) = delete;
    ObjectServer &operator=(const ObjectServer &) = delete;
    ~ObjectServer();
    Result Register(const Service &service);
    Result Run();
    struct WorkerOptions {
        size_t count;
        size_t stack_size = 64 * 1024;
        int priority = 44;
        int cpu = -2;
    };
    // Workers can dispatch different sessions of the same object at once.
    // Service implementations must synchronize their shared mutable state.
    Result RunConcurrent(WorkerOptions options);

  private:
    friend struct ObjectServerTestPeer;
    struct Domain {
        size_t references = 0;
        bool converted = false;
        ObjectRef root;
    };
    struct Session {
        Handle handle = INVALID_HANDLE;
        size_t port = 0;
        Domain *domain = nullptr;
        bool accepted = false;
        bool claimed = false;
    };
    struct Entry {
        Domain *domain = nullptr;
        u32 id = 0;
        ObjectRef object;
    };
    Limits limits_{};
    Service services_[MaxPorts]{};
    Handle ports_[MaxPorts]{};
    size_t port_count_ = 0;
    Session sessions_[MaxSessions]{};
    Domain domains_[MaxDomains]{};
    Entry inline_objects_[DefaultObjects]{};
    std::unique_ptr<Entry[]> large_objects_;
    std::span<Entry> Objects();
    u32 next_id_ = 1;
    size_t converted_domains_ = 0;
    kumo::Mutex mutex_{};
    bool started_ = false, sm_initialized_ = false;
    struct Pending {
        // A cloned session can erase the dispatched entry while its handler is
        // running. Keep the target and any handles it owns alive through send.
        ObjectRef target;
        Handle move = INVALID_HANDLE;
        size_t child = MaxSessions;
        Domain *domain = nullptr;
        u32 object_id = 0;
        bool conversion = false;
    };
    struct WorkerContext {
        alignas(16) unsigned char pointer_buffer[PointerBytes]{};
        Pending pending;
    };
    Result AddSession(Handle handle, size_t port, ObjectRef object, Domain *shared, bool accepted, size_t *index);
    Result NewSession(size_t port, ObjectRef object, Domain *shared, Handle *client, Pending &pending);
    void CloseSession(size_t index);
    Entry *Find(Domain *domain, u32 id);
    Result Insert(Domain *domain, ObjectRef object, u32 *id);
    void Erase(Domain *domain, u32 id);
    struct ConcurrentState;
    bool Process(size_t session, WorkerContext &worker, std::unique_lock<kumo::Mutex> *lock = nullptr);
    Result ServeReady(size_t session, WorkerContext &worker, bool concurrent, size_t &published_child);
    void CompleteReply(Pending &pending, bool sent);
    void PrepareReceive(WorkerContext &worker);
};
} // namespace kumo::ipc

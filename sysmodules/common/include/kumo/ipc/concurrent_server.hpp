#pragma once
#include <switch.h>
#include <kumo/ipc/message.hpp>
#include <type_traits>

namespace kumo::ipc {

struct ConcurrentService {
    const char *name;
    size_t max_sessions;
    void *context;
    void *(*open)(void *context);
    void (*retain)(void *object);
    void (*release)(void *object);
    bool (*dispatch)(void *object, ConcurrentRequest &request, ConcurrentReply &reply);
};

/* The embedding program starts worker_count threads calling Worker(), then
 * calls Run() on a separate thread. A worker serves one session at a time. */
class ConcurrentServer {
public:
    ConcurrentServer() = default;
    ConcurrentServer(const ConcurrentServer &) = delete;
    ConcurrentServer &operator=(const ConcurrentServer &) = delete;
    static constexpr size_t MaxPorts = 4;
    static constexpr size_t MaxWorkers = 63;
    ::Result Initialize(size_t worker_count);
    ::Result Register(const ConcurrentService &service);
    Result Worker();
    Result Run();
private:
    friend struct ServerTestPeer;
    struct Job { Handle handle; const ConcurrentService *service; void *object; };
    Mutex mutex_{};
    bool started_ = false;
    bool running_ = false;
    CondVar available_{};
    ConcurrentService services_[MaxPorts]{};
    Handle ports_[MaxPorts]{};
    size_t port_count_ = 0;
    size_t service_active_[MaxPorts]{};
    Job queue_[MaxWorkers]{};
    size_t queue_head_ = 0, queued_ = 0, active_ = 0, worker_count_ = 0;
    bool Enqueue(const Job &job);
    void Serve(const Job &job);
    bool Process(const Job &job, Handle &pending_move);
};
}

#include <kumo/prelude.hpp>
#include <kumo/mutex.hpp>
#include <kumo/storage.hpp>
#include <kumo/ipc/concurrent_server.hpp>
#include "bsd_server.hpp"
#include "bsd_dispatch.hpp"

namespace kumo::horizon::hznet::ipc {
    namespace {
        constexpr size_t WorkerCount = 62;
        constexpr size_t WorkerStackSize = (32 * 1024);
        kumo::ipc::ConcurrentServer g_server;
        alignas(0x1000) constinit u8 g_worker_stacks[WorkerCount][WorkerStackSize];
        constinit ::Thread g_workers[WorkerCount];
        constinit kumo::Mutex g_objects_mutex;

        struct SessionObject {
            BsdService service;
            size_t references;
            explicit SessionObject(bool system) : service(system), references(1) {}
        };
        constinit kumo::Storage<SessionObject> g_objects[WorkerCount]{};
        constinit bool g_used[WorkerCount]{};
        bool g_user = false, g_system = true;

        void *Open(void *context) {
            std::scoped_lock lock(g_objects_mutex);
            for (size_t i = 0; i < WorkerCount; ++i) {
                if (!g_used[i]) {
                    g_used[i] = true;
                    return g_objects[i].construct(*static_cast<bool *>(context));
                }
            }
            return nullptr;
        }
        void Retain(void *object) {
            std::scoped_lock lock(g_objects_mutex);
            ++static_cast<SessionObject *>(object)->references;
        }
        void Release(void *object) {
            std::scoped_lock lock(g_objects_mutex);
            auto *session = static_cast<SessionObject *>(object);
            if (--session->references) return;
            for (size_t i = 0; i < WorkerCount; ++i) {
                if (g_used[i] && g_objects[i].get() == session) {
                    g_objects[i].destroy();
                    g_used[i] = false;
                    return;
                }
            }
            KUMO_ABORT("unknown BSD session object");
        }
        bool Dispatch(void *object, kumo::ipc::ConcurrentRequest &request, kumo::ipc::ConcurrentReply &reply) {
            return DispatchBsd(static_cast<SessionObject *>(object)->service, request, reply);
        }
        void Worker(void *) { KUMO_ABORT_RESULT(g_server.Worker()); }
    }

    void InitializeServer() {
        KUMO_ABORT_UNLESS(::kumo::IsSuccess(g_server.Initialize(WorkerCount)));
        const kumo::ipc::ConcurrentService user{"bsd:u", 40, &g_user, Open, Retain, Release, Dispatch};
        const kumo::ipc::ConcurrentService system{"bsd:s", 22, &g_system, Open, Retain, Release, Dispatch};
        KUMO_ABORT_UNLESS(::kumo::IsSuccess(g_server.Register(user)));
        KUMO_ABORT_UNLESS(::kumo::IsSuccess(g_server.Register(system)));
        s32 priority = 0;
        KUMO_ABORT_UNLESS(::kumo::IsSuccess(::svcGetThreadPriority(&priority, CUR_THREAD_HANDLE)));
        for (size_t i = 0; i < WorkerCount; ++i) {
            KUMO_ABORT_UNLESS(::kumo::IsSuccess(::threadCreate(&g_workers[i], Worker, nullptr, g_worker_stacks[i], WorkerStackSize, priority, -2)));
            KUMO_ABORT_UNLESS(::kumo::IsSuccess(::threadStart(&g_workers[i])));
        }
    }
    void LoopServer() { KUMO_ABORT_RESULT(g_server.Run()); KUMO_ABORT("BSD server returned"); }
}

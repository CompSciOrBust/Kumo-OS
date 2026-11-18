#pragma once

/* A small, libnx-based CMIF server. The request and reply views are valid
 * only for the duration of the handler call. */
#include <kumo/ipc/message.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace kumo::ipc {

struct Service {
    const char *name;
    bool (*dispatch)(void *, ConcurrentRequest &, ConcurrentReply &);
    void *context;
};

/* Single worker, single port. Handlers must not block. Up to 63 sessions. */
class Server {
public:
    Server() = default;
    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;
    ::Result Register(const Service &service, size_t max_sessions);
    Result Run();
private:
    friend struct ServerTestPeer;
    Service service_{};
    Handle handles_[64]{};
    size_t count_ = 0;
    size_t max_sessions_ = 0;
    Handle pending_move_ = INVALID_HANDLE;
    Mutex mutex_{};
    bool started_ = false;
    bool Process();
};

}

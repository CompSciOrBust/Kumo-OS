#include <kumo/ipc/object_server.hpp>
#include <kumo/assert.hpp>
#include <algorithm>

namespace kumo::ipc {
namespace {
constexpr Result Invalid = MAKERESULT(Module_Libnx, LibnxError_BadInput);
constexpr size_t WaitHandles = 64;
// Reserve a wake event and room for all ports in the first wait set.
constexpr size_t SessionsPerWait = WaitHandles - 1 - ObjectServer::MaxPorts;
constexpr size_t MaxWaitSets = (ObjectServer::MaxSessions + SessionsPerWait - 1) / SessionsPerWait;
}

struct ObjectServer::ConcurrentState {
    struct WaitSet {
        ConcurrentState *state = nullptr;
        size_t index = 0;
        Handle writer = INVALID_HANDLE;
        Handle reader = INVALID_HANDLE;
    };

    ObjectServer &server;
    const size_t wait_count;
    WaitSet wait_sets[MaxWaitSets]{};
    CondVar ready{};
    size_t queue[MaxSessions]{};
    size_t head = 0;
    size_t queued = 0;
    Result failure = 0;

    // Shared state and wake events are updated under the server mutex.
    void Fail(Result rc) {
        if (failure)
            return;
        failure = rc;
        condvarWakeAll(&ready);
        for (size_t i = 0; i < wait_count; ++i)
            if (wait_sets[i].writer != INVALID_HANDLE)
                svcSignalEvent(wait_sets[i].writer);
    }

    void Notify(size_t slot) {
        const Result rc = svcSignalEvent(wait_sets[slot / SessionsPerWait].writer);
        if (R_FAILED(rc))
            Fail(rc);
    }

    void Worker() {
        WorkerContext worker;
        for (;;) {
            size_t slot;
            {
                std::unique_lock lock(server.mutex_);
                while (!queued && !failure) {
                    const Result rc = condvarWait(&ready, server.mutex_.native_handle());
                    if (R_FAILED(rc))
                        Fail(rc);
                }
                if (failure)
                    return;
                slot = queue[head];
                head = (head + 1) % MaxSessions;
                --queued;
            }

            size_t child = MaxSessions;
            const Result rc = server.ServeReady(slot, worker, true, child);
            std::scoped_lock lock(server.mutex_);
            if (R_FAILED(rc)) {
                Fail(rc);
                return;
            }
            if (!failure) {
                Notify(slot);
                if (child != MaxSessions && child / SessionsPerWait != slot / SessionsPerWait)
                    Notify(child);
            }
        }
    }

    void Wait(WaitSet &set) {
        const size_t start = set.index * SessionsPerWait;
        const size_t end = std::min(start + SessionsPerWait, server.limits_.sessions);
        const size_t ports = set.index == 0 ? server.port_count_ : 0;
        for (;;) {
            Handle handles[WaitHandles];
            size_t slots[WaitHandles];
            size_t count = 1;
            handles[0] = set.reader;
            {
                std::scoped_lock lock(server.mutex_);
                if (failure)
                    return;
                const Result rc = svcClearEvent(set.writer);
                if (R_FAILED(rc)) {
                    Fail(rc);
                    return;
                }
                for (size_t i = 0; i < ports; ++i)
                    handles[count++] = server.ports_[i];
                for (size_t slot = start; slot < end; ++slot) {
                    const auto &session = server.sessions_[slot];
                    if (session.domain && !session.claimed) {
                        slots[count] = slot;
                        handles[count++] = session.handle;
                    }
                }
            }

            s32 selected = -1;
            const Result rc = svcWaitSynchronization(&selected, handles, count, UINT64_MAX);
            if (R_FAILED(rc) || selected < 0 || static_cast<size_t>(selected) >= count) {
                std::scoped_lock lock(server.mutex_);
                Fail(R_FAILED(rc) ? rc : Invalid);
                return;
            }
            if (selected == 0)
                continue;
            if (static_cast<size_t>(selected) <= ports) {
                if (!Accept(selected - 1, handles[selected]))
                    return;
                continue;
            }

            std::scoped_lock lock(server.mutex_);
            if (failure)
                return;
            const size_t slot = slots[selected];
            // Each session slot has one waiter. Its handle is stable until
            // that waiter claims it and a worker finishes the request.
            server.sessions_[slot].claimed = true;
            queue[(head + queued) % MaxSessions] = slot;
            ++queued;
            condvarWakeOne(&ready);
        }
    }

    bool Accept(size_t port, Handle port_handle) {
        Handle handle = INVALID_HANDLE;
        const Result rc = svcAcceptSession(&handle, port_handle);
        if (R_FAILED(rc)) {
            if (R_VALUE(rc) == MAKERESULT(Module_Kernel, KernelError_NotFound))
                return true;
            std::scoped_lock lock(server.mutex_);
            Fail(rc);
            return false;
        }

        const auto &service = server.services_[port];
        auto object = service.open(service.context);
        std::scoped_lock lock(server.mutex_);
        size_t slot;
        if (failure || R_FAILED(server.AddSession(handle, port, std::move(object), nullptr, true, &slot))) {
            svcCloseHandle(handle);
            return !failure;
        }
        if (slot >= SessionsPerWait)
            Notify(slot);
        return true;
    }
};

Result ObjectServer::RunConcurrent(WorkerOptions options) {
    {
        std::scoped_lock lock(mutex_);
        if (started_ || !port_count_ || !options.count || options.count > MaxWorkers)
            return Invalid;
        started_ = true;
    }

    ConcurrentState state{*this, (limits_.sessions + SessionsPerWait - 1) / SessionsPerWait};
    condvarInit(&state.ready);
    for (size_t i = 0; i < state.wait_count; ++i) {
        auto &set = state.wait_sets[i];
        set.state = &state;
        set.index = i;
        const Result rc = svcCreateEvent(&set.writer, &set.reader);
        if (R_FAILED(rc)) {
            state.failure = rc;
            break;
        }
    }

    Thread threads[MaxWorkers + MaxWaitSets - 1]{};
    size_t created = 0, running = 0;
    const auto start = [&](ThreadFunc entry, void *arg) {
        Result rc = threadCreate(&threads[created], entry, arg, nullptr, options.stack_size,
                                 options.priority, options.cpu);
        if (R_SUCCEEDED(rc)) {
            rc = threadStart(&threads[created++]);
            if (R_SUCCEEDED(rc))
                ++running;
        }
        if (R_FAILED(rc)) {
            std::scoped_lock lock(mutex_);
            state.Fail(rc);
            return false;
        }
        return true;
    };

    if (!state.failure) {
        for (size_t i = 0; i < options.count; ++i) {
            if (!start([](void *arg) { static_cast<ConcurrentState *>(arg)->Worker(); }, &state))
                break;
        }
        if (running == options.count) {
            for (size_t i = 1; i < state.wait_count; ++i) {
                if (!start([](void *arg) {
                        auto &set = *static_cast<ConcurrentState::WaitSet *>(arg);
                        set.state->Wait(set);
                    }, &state.wait_sets[i]))
                    break;
            }
            state.Wait(state.wait_sets[0]);
        }
    }

    for (size_t i = 0; i < running; ++i)
        KUMO_ABORT_RESULT(threadWaitForExit(&threads[i]));
    for (size_t i = 0; i < created; ++i)
        threadClose(&threads[i]);
    for (size_t i = 0; i < state.wait_count; ++i) {
        const auto &set = state.wait_sets[i];
        if (set.reader != INVALID_HANDLE)
            svcCloseHandle(set.reader);
        if (set.writer != INVALID_HANDLE)
            svcCloseHandle(set.writer);
    }
    return state.failure;
}
} // namespace kumo::ipc

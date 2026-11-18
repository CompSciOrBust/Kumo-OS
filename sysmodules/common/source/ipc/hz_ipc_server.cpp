#include <kumo/ipc/server.hpp>
#include <kumo/ipc/process.hpp>
#include <mutex>

namespace kumo::ipc {
namespace {
constexpr ::Result InvalidRequest = MAKERESULT(Module_Libnx, LibnxError_BadInput);

}

::Result Server::Register(const Service &service, size_t max_sessions) {
    mutexLock(&mutex_);
    struct Unlock { Mutex *m; ~Unlock() { mutexUnlock(m); } } unlock{&mutex_};
    if (started_ || !service.name || !service.dispatch || max_sessions == 0 || max_sessions > 63 || count_ != 0)
        return InvalidRequest;
    ::Result rc = smInitialize();
    if (R_FAILED(rc)) return rc;
    rc = smRegisterService(&handles_[0], smEncodeName(service.name), false, static_cast<s32>(max_sessions));
    if (R_FAILED(rc)) { smExit(); return rc; }
    service_ = service;
    max_sessions_ = max_sessions;
    count_ = 1;
    return 0;
}

bool Server::Process() {
    return ProcessMessage(service_.context, service_.dispatch,
        [](Handle *) -> Result { return MAKERESULT(Module_Kernel, KernelError_NotImplemented); }, pending_move_);
}

Result Server::Run() {
    mutexLock(&mutex_);
    if (started_ || !count_) { mutexUnlock(&mutex_); return InvalidRequest; }
    started_ = true;
    mutexUnlock(&mutex_);
    Handle reply = INVALID_HANDLE;
    for (;;) {
        s32 index = -1;
        const Handle reply_target = reply;
        ::Result rc = svcReplyAndReceive(&index, handles_, static_cast<s32>(count_), reply_target, UINT64_MAX);
        if (pending_move_ != INVALID_HANDLE) {
            if (R_FAILED(rc) && index < 0) svcCloseHandle(pending_move_);
            pending_move_ = INVALID_HANDLE;
        }
        reply = INVALID_HANDLE;
        if (R_FAILED(rc) && R_VALUE(rc) != MAKERESULT(Module_Kernel, KernelError_ConnectionClosed)) return rc;
        if (R_SUCCEEDED(rc) && (index < 0 || static_cast<size_t>(index) >= count_)) return InvalidRequest;
        if (index < 0 || static_cast<size_t>(index) >= count_) {
            // Replying can fail after a client disconnects, before a wait index is returned.
            if (R_FAILED(rc) && reply_target != INVALID_HANDLE) {
                for (size_t i = 1; i < count_; ++i) {
                    if (handles_[i] == reply_target) {
                        svcCloseHandle(handles_[i]);
                        handles_[i] = handles_[--count_];
                        break;
                    }
                }
            }
            if (R_FAILED(rc) && reply_target == INVALID_HANDLE) return rc;
            continue;
        }
        if (index == 0 && R_FAILED(rc)) return rc;
        if (index == 0 && R_SUCCEEDED(rc)) {
            Handle session = INVALID_HANDLE;
            rc = svcAcceptSession(&session, handles_[0]);
            if (R_FAILED(rc) && R_VALUE(rc) != MAKERESULT(Module_Kernel, KernelError_NotFound)) return rc;
            if (R_SUCCEEDED(rc)) {
                if (count_ <= max_sessions_) handles_[count_++] = session;
                else svcCloseHandle(session);
            }
        } else if (index > 0) {
            if (R_SUCCEEDED(rc) && Process()) reply = handles_[index];
            else { svcCloseHandle(handles_[index]); handles_[index] = handles_[--count_]; }
        }
    }
}
}

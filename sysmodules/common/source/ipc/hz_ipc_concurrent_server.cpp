#include <kumo/ipc/concurrent_server.hpp>
#include <type_traits>
#include <kumo/ipc/process.hpp>

namespace kumo::ipc {
namespace {
constexpr ::Result Invalid = MAKERESULT(Module_Libnx, LibnxError_BadInput);
constexpr ::Result OutOfSessions = MAKERESULT(Module_Kernel, KernelError_OutOfSessions);
}

::Result ConcurrentServer::Initialize(size_t workers) {
    mutexLock(&mutex_);
    struct Unlock { Mutex *m; ~Unlock() { mutexUnlock(m); } } unlock{&mutex_};
    if (started_ || !workers || workers > MaxWorkers || worker_count_) return Invalid;
    ::Result rc = smInitialize();
    if (R_FAILED(rc)) return rc;
    condvarInit(&available_);
    worker_count_ = workers;
    return 0;
}

::Result ConcurrentServer::Register(const ConcurrentService &service) {
    mutexLock(&mutex_);
    struct Unlock { Mutex *m; ~Unlock() { mutexUnlock(m); } } unlock{&mutex_};
    if (started_ || !worker_count_ || port_count_ == MaxPorts || !service.name || !service.max_sessions || service.max_sessions > worker_count_ ||
        !service.open || !service.retain || !service.release || !service.dispatch) return Invalid;
    size_t reserved = service.max_sessions;
    for (size_t i = 0; i < port_count_; ++i) reserved += services_[i].max_sessions;
    if (reserved > worker_count_) return Invalid;
    Handle port = INVALID_HANDLE;
    ::Result rc = smRegisterService(&port, smEncodeName(service.name), false, static_cast<s32>(service.max_sessions));
    if (R_FAILED(rc)) return rc;
    services_[port_count_] = service;
    ports_[port_count_++] = port;
    return 0;
}

bool ConcurrentServer::Enqueue(const Job &job) {
    mutexLock(&mutex_);
    const size_t service_index = job.service - services_;
    if (service_active_[service_index] >= job.service->max_sessions || active_ >= worker_count_) { mutexUnlock(&mutex_); return false; }
    queue_[(queue_head_ + queued_) % MaxWorkers] = job;
    ++queued_;
    ++active_;
    ++service_active_[service_index];
    condvarWakeOne(&available_);
    mutexUnlock(&mutex_);
    return true;
}

Result ConcurrentServer::Worker() {
    mutexLock(&mutex_);
    if (!worker_count_ || !port_count_) { mutexUnlock(&mutex_); return Invalid; }
    started_ = true;
    mutexUnlock(&mutex_);
    for (;;) {
        mutexLock(&mutex_);
        while (!queued_) {
            const Result rc = condvarWait(&available_, &mutex_);
            if (R_FAILED(rc)) { mutexUnlock(&mutex_); return rc; }
        }
        Job job = queue_[queue_head_];
        queue_head_ = (queue_head_ + 1) % MaxWorkers;
        --queued_;
        mutexUnlock(&mutex_);
        Serve(job);
        job.service->release(job.object);
        mutexLock(&mutex_);
        --active_;
        --service_active_[job.service - services_];
        mutexUnlock(&mutex_);
    }
}

Result ConcurrentServer::Run() {
    mutexLock(&mutex_);
    if (running_ || !port_count_) { mutexUnlock(&mutex_); return Invalid; }
    started_ = running_ = true;
    mutexUnlock(&mutex_);
    for (;;) {
        s32 index = -1;
        ::Result rc = svcReplyAndReceive(&index, ports_, static_cast<s32>(port_count_), INVALID_HANDLE, UINT64_MAX);
        if (R_FAILED(rc)) return rc;
        if (index < 0 || static_cast<size_t>(index) >= port_count_) return Invalid;
        Handle session = INVALID_HANDLE;
        rc = svcAcceptSession(&session, ports_[index]);
        if (R_FAILED(rc)) {
            if (R_VALUE(rc) == MAKERESULT(Module_Kernel, KernelError_NotFound)) continue;
            return rc;
        }
        const ConcurrentService *service = &services_[index];
        void *object = service->open(service->context);
        if (!object) { svcCloseHandle(session); continue; }
        if (!Enqueue({session, service, object})) { service->release(object); svcCloseHandle(session); }
    }
}

void ConcurrentServer::Serve(const Job &job) {
    Handle reply = INVALID_HANDLE;
    Handle pending_move = INVALID_HANDLE;
    for (;;) {
        s32 index = -1;
        ::Result rc = svcReplyAndReceive(&index, &job.handle, 1, reply, UINT64_MAX);
        if (pending_move != INVALID_HANDLE) {
            // A successful reply transferred the moved handle to the client.
            // A failure reporting a wait index occurred after the reply; the
            // moved handle has already left this process and may be reused.
            if (R_FAILED(rc) && index < 0) svcCloseHandle(pending_move);
            pending_move = INVALID_HANDLE;
        }
        reply = INVALID_HANDLE;
        if (R_FAILED(rc) || index != 0 || !Process(job, pending_move)) break;
        reply = job.handle;
    }
    svcCloseHandle(job.handle);
}

bool ConcurrentServer::Process(const Job &job, Handle &pending_move) {
    return ProcessMessage(job.object, job.service->dispatch, [&](Handle *out) -> Result {
        Handle server = INVALID_HANDLE, client = INVALID_HANDLE;
        Result rc = svcCreateSession(&server, &client, 0, 0);
        if (R_FAILED(rc)) return rc;
        job.service->retain(job.object);
        if (!Enqueue({server, job.service, job.object})) {
            job.service->release(job.object);
            svcCloseHandle(server); svcCloseHandle(client);
            return OutOfSessions;
        }
        *out = client;
        return 0;
    }, pending_move);
}
}

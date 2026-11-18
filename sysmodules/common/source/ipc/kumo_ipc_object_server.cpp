#include <kumo/ipc/object_server.hpp>
#include <kumo/ipc/protocol.hpp>
#include <algorithm>

namespace kumo::ipc {
namespace {
constexpr Result Invalid = MAKERESULT(Module_Libnx, LibnxError_BadInput);
constexpr Result Exhausted = MAKERESULT(Module_Kernel, KernelError_OutOfResource);
constexpr Result Unsupported = MAKERESULT(Module_Kernel, KernelError_NotImplemented);
constexpr Result TargetNotFound = MAKERESULT(10, 261);
constexpr Result OutOfDomainEntries = MAKERESULT(10, 301);
constexpr Result OutOfDomains = MAKERESULT(11, 200);
bool Closed(Result rc) { return R_VALUE(rc) == MAKERESULT(Module_Kernel, KernelError_ConnectionClosed); }
} // namespace
ObjectServer::~ObjectServer() {
    for (size_t i = 0; i < MaxSessions; ++i)
        CloseSession(i);
    for (size_t i = 0; i < port_count_; ++i)
        svcCloseHandle(ports_[i]);
    if (sm_initialized_)
        smExit();
}
Result ObjectServer::Register(const Service &service) {
    std::scoped_lock lock(mutex_);
    if (started_ || !service.name || !service.open || !service.max_sessions ||
        service.max_sessions > limits_.sessions || port_count_ == MaxPorts || !limits_.sessions ||
        limits_.sessions > MaxSessions || !limits_.domains || limits_.domains > MaxDomains ||
        !limits_.converted_domains || limits_.converted_domains > MaxDomains ||
        !limits_.objects || limits_.objects > UINT32_MAX || limits_.objects > SIZE_MAX / sizeof(Entry))
        return Invalid;
    size_t total = service.max_sessions;
    for (size_t i = 0; i < port_count_; ++i)
        total += services_[i].max_sessions;
    if (total > limits_.sessions)
        return Invalid;
    if (limits_.objects > DefaultObjects && !large_objects_) {
        large_objects_.reset(new (std::nothrow) Entry[limits_.objects]);
        if (!large_objects_)
            return Exhausted;
    }
    if (!sm_initialized_) {
        Result rc = smInitialize();
        if (R_FAILED(rc))
            return rc;
        sm_initialized_ = true;
    }
    Handle port = INVALID_HANDLE;
    Result rc = smRegisterService(&port, smEncodeName(service.name), false, service.max_sessions);
    if (R_FAILED(rc))
        return rc;
    services_[port_count_] = service;
    ports_[port_count_++] = port;
    return 0;
}
Result ObjectServer::AddSession(Handle handle, size_t port, ObjectRef object, Domain *shared, bool accepted,
                                size_t *index) {
    size_t count = 0, free = MaxSessions;
    for (size_t i = 0; i < limits_.sessions; ++i) {
        if (sessions_[i].domain)
            count += sessions_[i].accepted && sessions_[i].port == port;
        else if (free == MaxSessions)
            free = i;
    }
    if (free == MaxSessions || (accepted && count >= services_[port].max_sessions))
        return Exhausted;
    Domain *domain = shared;
    if (!domain) {
        if (!object)
            return Exhausted;
        for (size_t i = 0; i < limits_.domains; ++i)
            if (!domains_[i].references) {
                domain = &domains_[i];
                break;
            }
        if (!domain)
            return Exhausted;
        domain->root = std::move(object);
        domain->converted = false;
    }
    ++domain->references;
    sessions_[free] = {handle, port, domain, accepted};
    *index = free;
    return 0;
}
Result ObjectServer::NewSession(size_t port, ObjectRef object, Domain *shared, Handle *client, Pending &pending) {
    Handle server = INVALID_HANDLE;
    Result rc = svcCreateSession(&server, client, 0, 0);
    if (R_FAILED(rc))
        return rc;
    size_t index;
    rc = AddSession(server, port, std::move(object), shared, false, &index);
    if (R_FAILED(rc)) {
        svcCloseHandle(server);
        svcCloseHandle(*client);
        *client = INVALID_HANDLE;
        return rc;
    }
    pending.child = index;
    return 0;
}
void ObjectServer::CloseSession(size_t index) {
    auto &session = sessions_[index];
    if (!session.domain)
        return;
    svcCloseHandle(session.handle);
    auto *domain = session.domain;
    session = {};
    if (--domain->references == 0) {
        if (domain->converted) --converted_domains_;
        for (auto &entry : Objects())
            if (entry.domain == domain)
                entry = {};
        domain->root = {};
        domain->converted = false;
    }
}
std::span<ObjectServer::Entry> ObjectServer::Objects() {
    if (large_objects_)
        return {large_objects_.get(), limits_.objects};
    return std::span(inline_objects_).first(std::min(limits_.objects, DefaultObjects));
}
ObjectServer::Entry *ObjectServer::Find(Domain *domain, u32 id) {
    for (auto &entry : Objects())
        if (entry.domain == domain && entry.id == id)
            return &entry;
    return nullptr;
}
Result ObjectServer::Insert(Domain *domain, ObjectRef object, u32 *id) {
    if (!object || !next_id_)
        return OutOfDomainEntries;
    for (auto &entry : Objects())
        if (!entry.domain) {
            *id = next_id_++;
            entry = {domain, *id, std::move(object)};
            return 0;
        }
    return OutOfDomainEntries;
}
void ObjectServer::Erase(Domain *domain, u32 id) {
    if (auto *entry = Find(domain, id))
        *entry = {};
}
void ObjectServer::CompleteReply(Pending &pending, bool sent) {
    if (!sent) {
        if (pending.move != INVALID_HANDLE)
            svcCloseHandle(pending.move);
        if (pending.child != MaxSessions)
            CloseSession(pending.child);
        if (pending.object_id) {
            if (pending.conversion) {
                if (auto *entry = Find(pending.domain, pending.object_id))
                    pending.domain->root = entry->object;
                pending.domain->converted = false;
                --converted_domains_;
            }
            Erase(pending.domain, pending.object_id);
        }
    }
    pending = {};
}
bool ObjectServer::Process(size_t index, WorkerContext &worker, std::unique_lock<kumo::Mutex> *lock) {
    auto &pending = worker.pending;
    auto &session = sessions_[index];
    auto *domain = session.domain;
    void *tls = armGetTls();
    Envelope envelope{};
    const bool valid = InspectEnvelope(tls, 0x100, envelope);
    // Save handles before overwriting TLS with a response.
    Handle handles[30]{};
    size_t count = envelope.handles_complete ? envelope.handle_count : 0;
    if (count)
        std::memcpy(handles, static_cast<unsigned char *>(tls) + envelope.handles_offset,
                    count * sizeof(Handle));
    ConcurrentRequest request{};
    request.copy_count = envelope.handles_complete ? envelope.special.num_copy_handles : 0;
    request.move_count = count - request.copy_count;
    std::memcpy(request.handles, handles, count * sizeof(Handle));
    struct Cleanup {
        ConcurrentRequest &r;
        ~Cleanup() {
            for (size_t i = 0; i < r.copy_count + r.move_count; ++i)
                if (r.handles[i] != INVALID_HANDLE)
                    svcCloseHandle(r.handles[i]);
        }
    } cleanup{request};
    if (!valid || envelope.header.type == CmifCommandType_Close ||
        !DecodeCmif(tls, 0x100, request.input, domain->converted))
        return false;
    request.data = static_cast<unsigned char *>(tls) + request.input.data_offset;
    request.pointer_buffer = worker.pointer_buffer;
    request.pointer_buffer_size = PointerBytes;
    ConcurrentReply reply{};
    struct ReplyCleanup {
        ConcurrentReply &r;
        ~ReplyCleanup() {
            if (r.move_handle != INVALID_HANDLE)
                svcCloseHandle(r.move_handle);
        }
    } reply_cleanup{reply};
    u32 out_object = 0;
    if (envelope.header.type == CmifCommandType_Control || envelope.header.type == CmifCommandType_ControlWithContext) {
        const auto &m = request.input.hipc.meta;
        if (count || m.send_pid || m.num_send_statics || m.num_send_buffers || m.num_recv_buffers ||
            m.num_exch_buffers)
            return false;
        switch (request.input.header.command_id) {
        case 0: {
            if (domain->converted) {
                reply.result = Invalid;
                break;
            }
            if (converted_domains_ == limits_.converted_domains) {
                reply.result = OutOfDomains;
                break;
            }
            u32 id;
            reply.result = Insert(domain, domain->root, &id);
            if (R_SUCCEEDED(reply.result)) {
                domain->root = {};
                domain->converted = true;
                ++converted_domains_;
                pending.domain = domain;
                pending.object_id = id;
                pending.conversion = true;
                reply.Write(id);
            }
            break;
        }
        case 1: {
            if (!domain->converted || request.input.data_size < 4) {
                reply.result = Invalid;
                break;
            }
            u32 id;
            std::memcpy(&id, request.data, 4);
            auto *entry = Find(domain, id);
            reply.result =
                entry ? NewSession(session.port, entry->object, nullptr, &reply.move_handle, pending) : TargetNotFound;
            break;
        }
        case 2:
        case 4:
            if (request.input.header.command_id == 4 && request.input.data_size < 4)
                return false;
            reply.result = NewSession(session.port, domain->root, domain->converted ? domain : nullptr,
                                      &reply.move_handle, pending);
            break;
        case 3:
            reply.Write<u16>(PointerBytes);
            break;
        default:
            reply.result = Unsupported;
            break;
        }
    } else if (request.input.domain_close) {
        if (count || request.input.hipc.meta.num_send_statics || request.input.hipc.meta.num_send_buffers ||
            request.input.hipc.meta.num_recv_buffers || request.input.hipc.meta.num_exch_buffers ||
            request.input.hipc.meta.send_pid)
            return false;
        Erase(domain, request.input.object_id);
    } else {
        auto &target = pending.target;
        target = domain->root;
        if (request.input.domain) {
            auto *entry = Find(domain, request.input.object_id);
            target = entry ? entry->object : ObjectRef{};
        }
        if (!target)
            reply.result = TargetNotFound;
        else {
            // The claimed session pins its domain. The retained target may
            // outlive its domain entry if a different session closes it.
            if (lock)
                lock->unlock();
            const bool handled = target->Dispatch(request, reply);
            if (lock)
                lock->lock();
            if (!handled)
                return false;
        }
        if (R_SUCCEEDED(reply.result) && reply.object) {
            if (reply.move_handle != INVALID_HANDLE)
                return false; // one returned object/moved handle per command
            if (request.input.domain) {
                reply.result = Insert(domain, std::move(reply.object), &out_object);
                if (R_SUCCEEDED(reply.result)) {
                    pending.domain = domain;
                    pending.object_id = out_object;
                }
            } else
                reply.result = NewSession(session.port, std::move(reply.object), nullptr, &reply.move_handle, pending);
        }
    }
    if (R_FAILED(reply.result)) {
        reply.size = 0;
        reply.copy_handle = reply.second_copy_handle = INVALID_HANDLE;
        reply.out_pointer = {};
        if (reply.move_handle != INVALID_HANDLE)
            svcCloseHandle(reply.move_handle);
        reply.move_handle = INVALID_HANDLE;
    }
    if (!EncodeReply(tls, reply.result, request.input.header.token, reply.data, reply.size, reply.copy_handle,
                     reply.move_handle, reply.second_copy_handle, reply.out_pointer, request.input.domain,
                     out_object))
        return false;
    pending.move = reply.move_handle;
    reply.move_handle = INVALID_HANDLE;
    return true;
}
void ObjectServer::PrepareReceive(WorkerContext &worker) {
    std::memset(armGetTls(), 0, 0x100);
    HipcMetadata meta{};
    meta.num_recv_statics = HIPC_AUTO_RECV_STATIC;
    auto request = hipcMakeRequest(armGetTls(), meta);
    request.recv_list[0] = hipcMakeRecvStatic(worker.pointer_buffer, PointerBytes);
}
Result ObjectServer::ServeReady(size_t index, WorkerContext &worker, bool concurrent, size_t &published_child) {
    // The scheduler exclusively assigns this session until we release it.
    const Handle handle = sessions_[index].handle;
    PrepareReceive(worker);
    s32 receive_index = -1;
    Result rc = svcReplyAndReceive(&receive_index, &handle, 1, INVALID_HANDLE, 0);

    std::unique_lock lock(mutex_, std::defer_lock);
    if (concurrent)
        lock.lock();
    if (R_FAILED(rc)) {
        sessions_[index].claimed = false;
        // Re-arm the receive template after a rejected transfer or readiness race.
        if (R_VALUE(rc) == MAKERESULT(Module_Kernel, 258) ||
            R_VALUE(rc) == MAKERESULT(Module_Kernel, KernelError_TimedOut))
            return 0;
        if (Closed(rc) || R_VALUE(rc) == MAKERESULT(Module_Kernel, KernelError_IpcMapFailed) ||
            R_VALUE(rc) == MAKERESULT(Module_Kernel, KernelError_IpcCmdbufTooSmall)) {
            CloseSession(index);
            return 0;
        }
        return rc;
    }

    // Keep the claim across Dispatch, which releases the object-table lock.
    sessions_[index].claimed = true;
    if (!Process(index, worker, concurrent ? &lock : nullptr)) {
        CompleteReply(worker.pending, false);
        CloseSession(index);
        return 0;
    }
    sessions_[index].claimed = false;

    // Sending with no wait handles is nonblocking. Keep new sessions and
    // domain IDs private to the transaction until the reply is sent.
    s32 unused = -1;
    rc = svcReplyAndReceive(&unused, nullptr, 0, handle, 0);
    const bool sent = R_SUCCEEDED(rc) || R_VALUE(rc) == MAKERESULT(Module_Kernel, KernelError_TimedOut);
    if (sent)
        published_child = worker.pending.child;
    CompleteReply(worker.pending, sent);
    if (sent)
        return 0;
    if (!Closed(rc))
        return rc;
    CloseSession(index);
    return 0;
}

Result ObjectServer::Run() {
    {
        std::scoped_lock lock(mutex_);
        if (started_ || !port_count_ || limits_.sessions > 64 - port_count_)
            return Invalid;
        started_ = true;
    }
    WorkerContext worker;
    for (;;) {
        Handle wait[64]{};
        size_t indices[64]{};
        size_t n = port_count_;
        for (size_t i = 0; i < port_count_; ++i)
            wait[i] = ports_[i];
        for (size_t i = 0; i < limits_.sessions; ++i)
            if (sessions_[i].domain) {
                indices[n] = i;
                wait[n++] = sessions_[i].handle;
            }
        s32 selected = -1;
        Result rc = svcWaitSynchronization(&selected, wait, n, UINT64_MAX);
        if (R_FAILED(rc))
            return rc;
        if (selected < 0 || static_cast<size_t>(selected) >= n)
            return Invalid;
        if (static_cast<size_t>(selected) < port_count_) {
            Handle handle = INVALID_HANDLE;
            rc = svcAcceptSession(&handle, wait[selected]);
            if (R_FAILED(rc)) {
                if (R_VALUE(rc) == MAKERESULT(Module_Kernel, KernelError_NotFound))
                    continue;
                return rc;
            }
            size_t index;
            auto object = services_[selected].open(services_[selected].context);
            rc = AddSession(handle, selected, std::move(object), nullptr, true, &index);
            if (R_FAILED(rc))
                svcCloseHandle(handle);
            continue;
        }
        size_t child = MaxSessions;
        rc = ServeReady(indices[selected], worker, false, child);
        if (R_FAILED(rc))
            return rc;
    }
}
} // namespace kumo::ipc

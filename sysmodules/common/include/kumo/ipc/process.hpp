#pragma once
#include <kumo/ipc/message.hpp>
namespace kumo::ipc {
template<class Dispatch, class Clone>
bool ProcessMessage(void *object, Dispatch dispatch, Clone clone, Handle &pending_move) {
    void *tls = armGetTls();
    Envelope envelope{};
    const bool valid = InspectEnvelope(tls, 0x100, envelope);
    ConcurrentRequest request{};
    request.copy_count = envelope.special.num_copy_handles;
    request.move_count = envelope.special.num_move_handles;
    if (envelope.handles_complete) {
        for (size_t i = 0; i < envelope.handle_count; ++i)
            std::memcpy(&request.handles[i], static_cast<unsigned char *>(tls) + envelope.handles_offset + i * sizeof(Handle), sizeof(Handle));
    } else {
        request.copy_count = request.move_count = 0;
    }
    struct Cleanup {
        ConcurrentRequest &request;
        ~Cleanup() {
            for (size_t i = 0; i < request.copy_count + request.move_count; ++i)
                if (request.handles[i] != INVALID_HANDLE) svcCloseHandle(request.handles[i]);
        }
    } cleanup{request};
    if (!valid || envelope.header.type == CmifCommandType_Close || !DecodeCmif(tls, 0x100, request.input)) return false;
    request.data = static_cast<const unsigned char *>(tls) + request.input.data_offset;
    ConcurrentReply reply{};
    struct ReplyCleanup {
        ConcurrentReply &reply;
        ~ReplyCleanup() { if (reply.move_handle != INVALID_HANDLE) svcCloseHandle(reply.move_handle); }
    } reply_cleanup{reply};
    if (envelope.header.type == CmifCommandType_Control) {
        const auto &meta = request.input.hipc.meta;
        if (meta.send_pid || request.copy_count || request.move_count || meta.num_send_statics ||
            meta.num_recv_statics || meta.num_send_buffers || meta.num_recv_buffers || meta.num_exch_buffers) return false;
        const u32 command = request.input.header.command_id;
        if (command == 4 && request.input.data_size < sizeof(u32)) return false;
        if (command == 3) {
            reply.Write<u16>(0);
        } else if (command == 2 || command == 4) {
            reply.result = clone(&reply.move_handle);
        } else reply.result = MAKERESULT(Module_Kernel, KernelError_NotImplemented);
    } else if (!dispatch(object, request, reply)) return false;
    if (reply.object) reply.result = MAKERESULT(Module_Kernel,KernelError_NotImplemented);
    if (R_FAILED(reply.result)) {
        reply.size = 0;
        reply.copy_handle = reply.second_copy_handle = INVALID_HANDLE;
        reply.out_pointer = {};
        if (reply.move_handle != INVALID_HANDLE) svcCloseHandle(reply.move_handle);
        reply.move_handle = INVALID_HANDLE;
    }
    if (!EncodeReply(tls, reply.result, request.input.header.token, reply.data, reply.size, reply.copy_handle, reply.move_handle, reply.second_copy_handle, reply.out_pointer)) {
        return false;
    }
    pending_move = reply.move_handle;
    reply.move_handle = INVALID_HANDLE;
    return true;
}
}

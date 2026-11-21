#pragma once

/* Bounded HIPC envelope inspection. This header has no runtime or sm
 * dependency, so it can also be exercised by host-side tests. */
extern "C" {
#include <switch/types.h>
#include <switch/sf/hipc.h>
#include <switch/sf/cmif.h>
}
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace kumo::ipc {

struct Envelope {
    HipcHeader header{};
    HipcSpecialHeader special{};
    size_t handles_offset = 0;
    size_t handle_count = 0;
    bool handles_complete = false;
};

inline bool InspectEnvelope(const void *message, size_t capacity, Envelope &out) {
    out = {};
    if (capacity < sizeof(HipcHeader)) return false;
    const auto *bytes = static_cast<const unsigned char *>(message);
    std::memcpy(&out.header, bytes, sizeof(out.header));
    size_t offset = sizeof(HipcHeader);
    auto advance = [&](size_t n) {
        if (n > capacity - offset) return false;
        offset += n;
        return true;
    };
    if (out.header.has_special_header) {
        if (!advance(sizeof(HipcSpecialHeader))) return false;
        std::memcpy(&out.special, bytes + sizeof(HipcHeader), sizeof(out.special));
        if (out.special.send_pid && !advance(sizeof(u64))) return false;
        out.handles_offset = offset;
        out.handle_count = out.special.num_copy_handles + out.special.num_move_handles;
        if (!advance(out.handle_count * sizeof(Handle))) return false;
        out.handles_complete = true;
    }
    if (!advance(out.header.num_send_statics * sizeof(HipcStaticDescriptor))) return false;
    if (!advance((out.header.num_send_buffers + out.header.num_recv_buffers + out.header.num_exch_buffers) * sizeof(HipcBufferDescriptor))) return false;
    if (!advance(out.header.num_data_words * sizeof(u32))) return false;
    // Modes 0, 1 and 2 have no explicit receive-list entries in the request.
    if (out.header.recv_static_mode > 2 && !advance((out.header.recv_static_mode - 2) * sizeof(HipcRecvListEntry))) return false;
    return true;
}

struct CmifInput {
    bool domain = false;
    bool domain_close = false;
    u32 object_id = 0;
    HipcParsedRequest hipc{};
    CmifInHeader header{};
    size_t data_offset = 0;
    size_t data_size = 0;
};

struct MappedBuffer {
    void *address;
    size_t size;
};

/* Auto-select requests contain both pointer and mapped descriptors. When a
 * server advertises a zero pointer-buffer size, libnx leaves the pointer
 * descriptors empty and uses the mapped descriptors. Direct MapAlias inputs
 * may precede any auto-select inputs. Call only after the
 * envelope has been inspected. Output pointer-size validation must be done
 * separately using the command's CMIF pointer-size table. */
inline bool DecodeMappedAutoBuffers(const HipcParsedRequest &request,
                                    MappedBuffer *inputs, size_t input_count,
                                    MappedBuffer *outputs, size_t output_count, size_t mapped_input_prefix = 0) {
    const auto &meta = request.meta;
    if (mapped_input_prefix > input_count || meta.num_send_statics != input_count - mapped_input_prefix ||
        meta.num_send_buffers != input_count ||
        meta.num_recv_statics != output_count || meta.num_recv_buffers != output_count ||
        meta.num_exch_buffers != 0) return false;
    if ((input_count && !inputs) || (output_count && !outputs)) return false;
    for (size_t i = 0; i < input_count; ++i) {
        if (i >= mapped_input_prefix &&
            hipcGetStaticSize(&request.data.send_statics[i - mapped_input_prefix]) != 0) return false;
        const auto &descriptor = request.data.send_buffers[i];
        inputs[i] = {hipcGetBufferAddress(&descriptor), hipcGetBufferSize(&descriptor)};
    }
    for (size_t i = 0; i < output_count; ++i) {
        const auto &descriptor = request.data.recv_buffers[i];
        outputs[i] = {hipcGetBufferAddress(&descriptor), hipcGetBufferSize(&descriptor)};
    }
    return true;
}

/* The CMIF size table starts after the raw payload and its full alignment
 * allowance, relative to HIPC data_words (not the aligned CMIF header). */
inline constexpr size_t OutPointerSizesOffset(size_t input_size, bool domain) {
    return (16 + (domain ? sizeof(CmifDomainInHeader) : 0) + sizeof(CmifInHeader) + input_size + 1) & ~size_t(1);
}

inline bool ReadOutPointerSize(const CmifInput &input, size_t fixed_input_size, size_t index, u16 &size) {
    const size_t words_size = input.hipc.meta.num_data_words * sizeof(u32);
    const size_t offset = OutPointerSizesOffset(fixed_input_size, input.domain);
    if (offset > words_size || index >= (words_size - offset) / sizeof(u16)) return false;
    const auto *words = reinterpret_cast<const unsigned char *>(input.hipc.data.data_words);
    std::memcpy(&size, words + offset + index * sizeof(size), sizeof(size));
    return true;
}

inline bool HasOnlyMappedAutoOutputs(const CmifInput &input, size_t fixed_input_size, size_t output_count) {
    for (size_t i = 0; i < output_count; ++i) {
        u16 size;
        if (!ReadOutPointerSize(input, fixed_input_size, i, size) || size != 0) return false;
    }
    return true;
}

/* InspectEnvelope must already have succeeded for this same buffer. */
inline bool DecodeCmif(void *message, size_t capacity, CmifInput &out, bool domain = false) {
    out = {};
    Envelope envelope{};
    if (!InspectEnvelope(message, capacity, envelope)) return false;
    const auto type = envelope.header.type;
    const bool request = type == CmifCommandType_Request || type == CmifCommandType_RequestWithContext;
    const bool control = type == CmifCommandType_Control || type == CmifCommandType_ControlWithContext;
    if (!request && !control) return false;
    // HIPC puts the PID at offset 12, which is not u64-aligned. Decode it
    // with memcpy rather than libnx's typed load (undefined in C++).
    const auto &h = envelope.header;
    const auto &special = envelope.special;
    auto &meta = out.hipc.meta;
    meta.type = h.type;
    meta.num_send_statics = h.num_send_statics;
    meta.num_send_buffers = h.num_send_buffers;
    meta.num_recv_buffers = h.num_recv_buffers;
    meta.num_exch_buffers = h.num_exch_buffers;
    meta.num_data_words = h.num_data_words;
    meta.num_recv_statics = h.recv_static_mode == 2 ? HIPC_AUTO_RECV_STATIC :
        (h.recv_static_mode > 2 ? h.recv_static_mode - 2 : 0);
    meta.send_pid = special.send_pid;
    meta.num_copy_handles = special.num_copy_handles;
    meta.num_move_handles = special.num_move_handles;
    size_t layout_offset = sizeof(HipcHeader);
    if (h.has_special_header) {
        layout_offset += sizeof(HipcSpecialHeader);
        if (special.send_pid) {
            std::memcpy(&out.hipc.pid, static_cast<unsigned char *>(message) + layout_offset, sizeof(u64));
            layout_offset += sizeof(u64);
        }
    }
    out.hipc.data = hipcCalcRequestLayout(meta, static_cast<unsigned char *>(message) + layout_offset);
    if (!out.hipc.data.data_words) return false;
    const auto base = reinterpret_cast<uintptr_t>(message);
    const auto words = reinterpret_cast<uintptr_t>(out.hipc.data.data_words);
    const size_t words_offset = static_cast<size_t>(words - base);
    const size_t words_end = words_offset + out.hipc.meta.num_data_words * sizeof(u32);
    size_t aligned = (words_offset + 15) & ~size_t(15);
    size_t payload_end = words_end;
    if (domain && request) {
        if (aligned > words_end || sizeof(CmifDomainInHeader) > words_end-aligned) return false;
        CmifDomainInHeader header{};
        std::memcpy(&header, static_cast<unsigned char *>(message)+aligned,sizeof(header));
        out.domain=true; out.object_id=header.object_id;
        aligned+=sizeof(header);
        if (header.num_in_objects != 0) return false; // NIFM takes no input objects.
        if (header.type == CmifDomainRequestType_Close) {
            if (header.data_size != 0) return false;
            out.domain_close=true; return true;
        }
        if (header.type != CmifDomainRequestType_SendMessage || header.data_size > words_end-aligned) return false;
        payload_end=aligned+header.data_size;
    }
    if (aligned > payload_end || sizeof(CmifInHeader) > payload_end - aligned) return false;
    std::memcpy(&out.header, static_cast<const unsigned char *>(message) + aligned, sizeof(out.header));
    if (out.header.magic != CMIF_IN_HEADER_MAGIC || out.header.version > 1) return false;
    out.data_offset = aligned + sizeof(CmifInHeader);
    out.data_size = payload_end - out.data_offset;
    return true;
}

inline bool EncodeReply(void *message, Result result, u32 token, const void *data,
                        size_t size, Handle copy, Handle move = INVALID_HANDLE,
                        Handle second_copy = INVALID_HANDLE, MappedBuffer pointer = {},
                        bool domain = false, u32 object_id = 0) {
    if (size > 128) return false;
    // Clear alignment and trailing padding so a previous request cannot leak.
    // Payload must be separate from the TLS message buffer.
    std::memset(message, 0, 0x100);
    HipcMetadata meta{};
    meta.num_data_words = static_cast<u32>((16 + (domain ? sizeof(CmifDomainOutHeader) : 0) + sizeof(CmifOutHeader) + size + (object_id ? 4 : 0) + 3) / 4);
    meta.num_copy_handles = (copy != INVALID_HANDLE) + (second_copy != INVALID_HANDLE);
    if (second_copy != INVALID_HANDLE && copy == INVALID_HANDLE) return false;
    meta.num_send_statics = pointer.address != nullptr || pointer.size != 0;
    meta.num_move_handles = move != INVALID_HANDLE;
    auto output = hipcMakeRequest(message, meta);
    if (meta.num_copy_handles) *output.copy_handles = copy;
    if (second_copy != INVALID_HANDLE) output.copy_handles[1] = second_copy;
    if (meta.num_send_statics) *output.send_statics = hipcMakeSendStatic(pointer.address,pointer.size,0);
    if (meta.num_move_handles) *output.move_handles = move;
    auto *header = static_cast<CmifOutHeader *>(cmifGetAlignedDataStart(output.data_words, message));
    if (domain) {
        auto *domain_header = reinterpret_cast<CmifDomainOutHeader *>(header);
        *domain_header = {}; domain_header->num_out_objects = object_id != 0;
        header = reinterpret_cast<CmifOutHeader *>(domain_header+1);
    }
    *header = CmifOutHeader{CMIF_OUT_HEADER_MAGIC, 0, result, token};
    std::memcpy(header + 1, data, size);
    if (object_id) std::memcpy(reinterpret_cast<unsigned char *>(header+1)+size,&object_id,4);
    return true;
}

}

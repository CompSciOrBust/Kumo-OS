#include <kumo/ipc/protocol.hpp>
#include <cassert>
#include <cstdio>

using namespace kumo::ipc;

int main() {
    alignas(16) unsigned char bytes[256]{};
    Envelope e{};
    HipcMetadata meta{};
    meta.type = 4;
    meta.num_data_words = 8;
    hipcMakeRequest(bytes, meta);
    assert(InspectEnvelope(bytes, sizeof(bytes), e));
    assert(e.header.type == 4 && e.header.num_data_words == 8);
    assert(!e.handles_complete && e.handle_count == 0);
    assert(!InspectEnvelope(bytes, sizeof(HipcHeader) - 1, e));
    assert(!InspectEnvelope(bytes, sizeof(HipcHeader) + 31, e));

    meta.send_pid = 1;
    meta.num_copy_handles = 1;
    meta.num_move_handles = 1;
    auto request = hipcMakeRequest(bytes, meta);
    request.copy_handles[0] = 42;
    request.move_handles[0] = 43;
    assert(InspectEnvelope(bytes, sizeof(bytes), e));
    assert(e.handles_complete && e.handle_count == 2);
    Handle value{};
    std::memcpy(&value, bytes + e.handles_offset, sizeof(value));
    assert(value == 42);
    assert(!InspectEnvelope(bytes, e.handles_offset + sizeof(Handle), e));
    assert(!e.handles_complete);
    assert(!InspectEnvelope(bytes, sizeof(HipcHeader) + sizeof(HipcSpecialHeader) + sizeof(u64) + 2 * sizeof(Handle) + 1, e));
    assert(e.handles_complete && e.handle_count == 2);

    meta = {};
    meta.num_send_statics = 1;
    meta.num_send_buffers = 1;
    meta.num_recv_buffers = 1;
    meta.num_exch_buffers = 1;
    meta.num_data_words = 8;
    meta.num_recv_statics = 1;
    hipcMakeRequest(bytes, meta);
    const size_t full = sizeof(HipcHeader) + sizeof(HipcStaticDescriptor) + 3 * sizeof(HipcBufferDescriptor) + 8 * sizeof(u32) + sizeof(HipcRecvListEntry);
    assert(InspectEnvelope(bytes, full, e));
    assert(!InspectEnvelope(bytes, full - 1, e));

    CmifRequestFormat format{};
    format.request_id = 17;
    format.data_size = sizeof(u32);
    auto cmif = cmifMakeRequest(bytes, format);
    const u32 input = 1234;
    std::memcpy(cmif.data, &input, sizeof(input));
    CmifInput decoded{};
    assert(DecodeCmif(bytes, sizeof(bytes), decoded));
    assert(decoded.header.command_id == 17 && decoded.data_size >= sizeof(input));
    u32 decoded_input = 0;
    std::memcpy(&decoded_input, bytes + decoded.data_offset, sizeof(decoded_input));
    assert(decoded_input == input);
    assert(!DecodeCmif(bytes, decoded.data_offset - 1, decoded));
    auto *cmif_header = reinterpret_cast<CmifInHeader *>(cmif.data) - 1;
    cmif_header->version = 2;
    assert(!DecodeCmif(bytes, sizeof(bytes), decoded));
    cmif_header->version = 1;
    assert(DecodeCmif(bytes, sizeof(bytes), decoded));
    cmif_header->magic = 0;
    assert(!DecodeCmif(bytes, sizeof(bytes), decoded));

    format = {};
    format.request_id = 10;
    format.num_in_auto_buffers = 1;
    format.num_out_auto_buffers = 1;
    auto auto_request = cmifMakeRequest(bytes, format);
    char input_buffer[7]{};
    char output_buffer[11]{};
    cmifRequestInAutoBuffer(&auto_request, input_buffer, sizeof(input_buffer), HipcBufferMode_Normal);
    cmifRequestOutAutoBuffer(&auto_request, output_buffer, sizeof(output_buffer), HipcBufferMode_Normal);
    assert(DecodeCmif(bytes, sizeof(bytes), decoded));
    MappedBuffer inputs[1]{}, outputs[1]{};
    assert(DecodeMappedAutoBuffers(decoded.hipc, inputs, 1, outputs, 1));
    assert(HasOnlyMappedAutoOutputs(decoded, 0, 1));
    assert(inputs[0].address == input_buffer && inputs[0].size == sizeof(input_buffer));
    assert(outputs[0].address == output_buffer && outputs[0].size == sizeof(output_buffer));
    assert(!DecodeMappedAutoBuffers(decoded.hipc, inputs, 0, outputs, 1));
    decoded.hipc.data.send_statics[0].size = 1;
    assert(!DecodeMappedAutoBuffers(decoded.hipc, inputs, 1, outputs, 1));
    decoded.hipc.data.send_statics[0].size = 0;
    auto *sizes = reinterpret_cast<u16 *>(reinterpret_cast<unsigned char *>(decoded.hipc.data.data_words) + 16 + sizeof(CmifInHeader));
    *sizes = 1;
    assert(!HasOnlyMappedAutoOutputs(decoded, 0, 1));
    *sizes = 0;

    // The serializer must reject oversized payloads before touching TLS.
    unsigned char payload[129]{};
    assert(!EncodeReply(bytes, 0, 7, payload, sizeof(payload), INVALID_HANDLE));
    assert(EncodeReply(bytes, 0, 7, payload, 128, INVALID_HANDLE));
    assert(InspectEnvelope(bytes, sizeof(bytes), e));

    // Probe arbitrary headers and every truncation point. This is especially
    // useful under ASan/UBSan: no malformed envelope may read beyond capacity.
    uint32_t random = 0x5eed1234;
    for (int sample = 0; sample < 10000; ++sample) {
        for (auto &byte : bytes) {
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;
            byte = static_cast<unsigned char>(random);
        }
        for (size_t capacity = 0; capacity <= sizeof(bytes); ++capacity) {
            CmifInput domain_input{};
            DecodeCmif(bytes, capacity, domain_input, true);
            InspectEnvelope(bytes, capacity, e);
            if (e.handles_complete) {
                assert(e.handles_offset <= capacity);
                assert(e.handle_count <= (capacity - e.handles_offset) / sizeof(Handle));
            }
        }
    }
    std::puts("IPC envelope tests passed");
}

#include <kumo/fs/storage_service.hpp>
#include <kumo/ipc/dispatch.hpp>
#include <limits>

namespace kumo::fs {
namespace {
struct Transfer {
    static constexpr size_t WireBytes = 0x10;
    s64 offset;
    s64 size;
};

struct RangeRequest {
    static constexpr size_t WireBytes = 0x18;
    s32 operation;
    u32 padding;
    s64 offset;
    s64 size;
};
static_assert(offsetof(RangeRequest, offset) == 8 && offsetof(RangeRequest, size) == 16);

Result CheckTransfer(const Transfer &request, size_t buffer_size) {
    KUMO_UNLESS(request.offset >= 0, InvalidOffset);
    KUMO_UNLESS(request.size >= 0 && static_cast<u64>(request.size) <= buffer_size, InvalidSize);
    return 0;
}
}

bool StorageService::Dispatch(ipc::ConcurrentRequest &request, ipc::ConcurrentReply &reply) {
    using namespace ipc;
    if (request.input.header.command_id > 5) return false;
    static constexpr CommandOptions transfer_options{.buffers = BufferMode::NonSecureMapped};
    static constexpr CommandTable commands({
        MakeCommand<StorageService, Transfer, NoData, 0, 1,
            [](StorageService &self, const Transfer &in, NoData &, Arguments<0, 1> &args) -> Result {
                const auto buffer = args.Out(0);
                KUMO_TRY(CheckTransfer(in, buffer.GetSize()));
                return self.storage_.Read(in.offset, {reinterpret_cast<std::byte *>(buffer.GetPointer()), static_cast<size_t>(in.size)});
            }>(0, transfer_options),
        MakeCommand<StorageService, Transfer, NoData, 1, 0,
            [](StorageService &self, const Transfer &in, NoData &, Arguments<1, 0> &args) -> Result {
                const auto buffer = args.In(0);
                KUMO_TRY(CheckTransfer(in, buffer.GetSize()));
                return self.storage_.Write(in.offset, {reinterpret_cast<const std::byte *>(buffer.GetPointer()), static_cast<size_t>(in.size)});
            }>(1, transfer_options),
        MakeCommand<StorageService, NoData, NoData, 0, 0,
            [](StorageService &self, const NoData &, NoData &, Arguments<0, 0> &) {
                return self.storage_.Flush();
            }>(2),
        MakeCommand<StorageService, s64, NoData, 0, 0,
            [](StorageService &self, const s64 &size, NoData &, Arguments<0, 0> &) -> Result {
                KUMO_UNLESS(size >= 0, InvalidSize);
                return self.storage_.SetSize(size);
            }>(3),
        MakeCommand<StorageService, NoData, s64, 0, 0,
            [](StorageService &self, const NoData &, s64 &out, Arguments<0, 0> &) -> Result {
                u64 size;
                KUMO_TRY(self.storage_.GetSize(size));
                KUMO_UNLESS(size <= static_cast<u64>(std::numeric_limits<s64>::max()), InvalidSize);
                out = static_cast<s64>(size);
                return 0;
            }>(4),
        MakeCommand<StorageService, RangeRequest, RangeInfo, 0, 0,
            [](StorageService &self, const RangeRequest &in, RangeInfo &out, Arguments<0, 0> &) -> Result {
                if (in.operation == 2) return self.storage_.Invalidate(in.offset, in.size);
                if (in.operation == 3) {
                    RangeInfo info{};
                    KUMO_TRY(self.storage_.QueryRange(in.offset, in.size, info));
                    out.aes_ctr_key_type = info.aes_ctr_key_type;
                    out.speed_emulation_type = info.speed_emulation_type;
                }
                return 0;
            }>(5),
    });
    return commands.Dispatch(this, request, reply);
}

} // namespace kumo::fs

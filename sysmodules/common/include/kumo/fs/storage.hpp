#pragma once
#include <kumo/result.hpp>
#include <cstddef>
#include <span>

namespace kumo::fs {

constexpr Result Error(u32 description) { return MAKERESULT(2, description); }

inline constexpr Result PathNotFound = Error(1);
inline constexpr Result OutOfRange = Error(3005);
inline constexpr Result AllocationFailed = Error(3423);
inline constexpr Result InvalidOffset = Error(6061);
inline constexpr Result InvalidSize = Error(6062);
inline constexpr Result UnsupportedOperation = Error(6300);

// The subtraction form also checks ranges whose end would overflow.
constexpr bool Contains(u64 extent, u64 offset, u64 size) {
    return offset <= extent && size <= extent - offset;
}

struct RangeInfo {
    static constexpr size_t WireBytes = 0x40;
    s32 aes_ctr_key_type;
    s32 speed_emulation_type;
    u8 reserved[0x38];
};
static_assert(sizeof(RangeInfo) == RangeInfo::WireBytes);

class Storage {
public:
    virtual ~Storage() = default;
    virtual Result Read(u64 offset, std::span<std::byte> output) = 0;
    virtual Result GetSize(u64 &size) = 0;
    virtual Result Write(u64, std::span<const std::byte>) { return UnsupportedOperation; }
    virtual Result Flush() { return UnsupportedOperation; }
    virtual Result SetSize(u64) { return UnsupportedOperation; }
    virtual Result QueryRange(s64, s64, RangeInfo &) { return UnsupportedOperation; }
    virtual Result Invalidate(s64, s64) { return UnsupportedOperation; }
};

// A fixed extent in borrowed storage. Its backing object must outlive the view.
class StorageSlice final : public Storage {
public:
    Result Open(Storage &storage, u64 offset, u64 size);
    Result Read(u64 offset, std::span<std::byte> output) override;
    Result GetSize(u64 &size) override;
    Result Write(u64 offset, std::span<const std::byte> input) override;
    Result Flush() override;
    Result QueryRange(s64 offset, s64 size, RangeInfo &info) override;
    Result Invalidate(s64 offset, s64 size) override;

private:
    Storage *storage_ = nullptr;
    u64 offset_ = 0;
    u64 size_ = 0;
};

enum class OpenMode : u32 { Read = 1, Write = 2, ReadWrite = 3, AllowAppend = 4 };

// File reads can stop at EOF. The Storage interface still uses exact reads.
class FileView final : public Storage {
public:
    Result Read(u64 offset, std::span<std::byte> output) override;
    Result Write(u64 offset, std::span<const std::byte> input) override;
    Result Flush() override;
    Result SetSize(u64 size) override;
    Result GetSize(u64 &size) override { return storage_.GetSize(size); }
    Result QueryRange(s64 offset, s64 size, RangeInfo &info) override { return storage_.QueryRange(offset, size, info); }
    Result Invalidate(s64 offset, s64 size) override { return storage_.Invalidate(offset, size); }
    Result ReadFile(u64 offset, std::span<std::byte> output, size_t &read);

private:
    friend class PartitionFileSystem;
    friend class RomFileSystem;
    StorageSlice storage_;
    u32 mode_ = static_cast<u32>(OpenMode::Read);
    Result unsupported_write_ = Error(6367);
};

enum class EntryType : u8 { Directory = 0, File = 1 };

} // namespace kumo::fs

#pragma once
#include <kumo/fs/storage.hpp>
#include <memory>

namespace kumo::fs::detail {

// A bounded BKTR index. Patch offsets are applied before integrity verification,
// while compression metadata and payload are read through verified storage.
class NcaMapping {
  public:
    enum class Kind { Indirect, Compression };
    Result Open(Storage &storage, std::span<const std::byte> bucket, Kind kind, size_t budget);
    Result Read(Storage &storage, u64 offset, std::span<std::byte> output) const;
    Result QueryRange(Storage &storage, s64 offset, s64 size, RangeInfo &info) const;
    u64 Size() const { return size_; }

  private:
    enum class Encoding { Plain, Zero, Lz4, MissingBase };
    struct Entry {
        u64 logical, physical;
        u32 encoded_size;
        Encoding encoding;
    };

    std::unique_ptr<Entry[]> entries_;
    size_t count_ = 0;
    u64 size_ = 0;
    size_t Find(u64 offset) const;
    u64 End(size_t index) const { return index + 1 < count_ ? entries_[index + 1].logical : size_; }
};

}

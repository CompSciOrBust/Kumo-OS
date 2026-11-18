#pragma once
#include <kumo/fs/storage.hpp>
#include <array>
#include <memory>

namespace kumo::fs {
namespace detail { class NcaMapping; }

inline constexpr Result NcaFsHeaderHashFailed = Error(4520);
inline constexpr Result NcaDataHashFailed = Error(4535);
inline constexpr Result UnsupportedNcaHashType = Error(4522);
inline constexpr Result UnsupportedNcaEncryption = Error(4523);

// Unsigned plaintext NCAs use hashes for corruption detection. The caller
// supplies a trusted archive if authenticity is required.
class PlaintextNca {
public:
    enum class FileSystemType : u8 { RomFs = 0, PartitionFs = 1 };
    enum class ContentType : u8 { Program, Meta, Control, Manual, Data, PublicData };
    struct Limits {
        size_t hash_table_bytes;
        size_t block_bytes;
    };

    class Section final : public Storage {
    public:
        Section();
        ~Section() override;
        Section(Section &&) noexcept;
        Section &operator=(Section &&) noexcept;
        FileSystemType Type() const { return type_; }
        Result Read(u64 offset, std::span<std::byte> output) override;
        Result GetSize(u64 &size) override;
        Result Flush() override { return data_.Flush(); }
        Result QueryRange(s64 offset, s64 size, RangeInfo &info) override;
        Result Invalidate(s64 offset, s64 size) override { return data_.Invalidate(offset, size); }

    private:
        friend class PlaintextNca;
        struct Layer {
            u64 offset;
            u64 size;
            size_t block_size;
        };
        Result ReadIntegrityBlock(size_t level, u64 index, std::span<std::byte> block);
        class VerifiedView;
        Result ReadVerified(u64 offset, std::span<std::byte> output);
        Result QueryVerified(s64 offset, s64 size, RangeInfo &info);
        std::unique_ptr<Storage> mapped_input_;
        std::unique_ptr<detail::NcaMapping> compression_;
        StorageSlice data_;
        std::array<Layer, 6> layers_{};
        std::array<std::byte, 32> master_hash_{};
        size_t layer_count_ = 0;
        std::unique_ptr<std::byte[]> hashes_;
        u64 size_ = 0;
        u32 block_size_ = 0;
        FileSystemType type_{};
    };

    Result Open(Storage &storage);
    bool HasSection(u32 index) const;
    ContentType Type() const { return static_cast<ContentType>(std::to_integer<u8>(header_[0x205])); }
    u64 ProgramId() const;

    // The backing storage must outlive both the reader and its open sections.
    // Reads authenticate complete blocks before copying the requested bytes.
    Result OpenSection(u32 index, Limits limits, Section &section) const;

private:
    Result OpenSectionData(Storage &input, std::span<const std::byte> header, Limits limits, Section &section) const;
    Result OpenIntegritySection(Storage &input, std::span<const std::byte> header, u64 extent,
                                Limits limits, Section &section) const;
    Storage *storage_ = nullptr;
    std::array<std::byte, 0x400> header_{};
};

} // namespace kumo::fs

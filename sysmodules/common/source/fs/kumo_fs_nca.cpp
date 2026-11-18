#include <kumo/fs/nca.hpp>
#include "kumo_fs_format.hpp"
#include "kumo_fs_nca_mapping.hpp"
#include <algorithm>
#include <bit>
#include <cstring>
#include <utility>

namespace kumo::fs {
namespace {
using detail::LoadLittleEndian;
constexpr u64 FirstSectionOffset = 0xc00;
constexpr size_t FsHeaderSize = 0x200;
constexpr size_t HashSize = 32;
constexpr Result InvalidHeader = Error(4528);
constexpr Result SectionOutOfRange = Error(4510);

bool MatchesHash(std::span<const std::byte> data, const std::byte *expected) {
    std::byte hash[HashSize];
    sha256CalculateHash(hash, data.data(), data.size());
    return std::memcmp(hash, expected, sizeof(hash)) == 0;
}
}

namespace {
class IndirectInput final : public Storage {
public:
    StorageSlice raw;
    detail::NcaMapping mapping;

    Result Read(u64 offset, std::span<std::byte> output) override { return mapping.Read(raw, offset, output); }
    Result GetSize(u64 &size) override { size = mapping.Size(); return 0; }
    Result QueryRange(s64 offset, s64 size, RangeInfo &info) override {
        return mapping.QueryRange(raw, offset, size, info);
    }
    Result Invalidate(s64 offset, s64 size) override { return raw.Invalidate(offset, size); }
    Result Flush() override { return raw.Flush(); }
};
}

PlaintextNca::Section::Section() = default;
PlaintextNca::Section::~Section() = default;
PlaintextNca::Section::Section(Section &&other) noexcept {
    *this = std::move(other);
}

PlaintextNca::Section &PlaintextNca::Section::operator=(Section &&other) noexcept {
    if (this != &other) {
        mapped_input_ = std::move(other.mapped_input_);
        compression_ = std::move(other.compression_);
        data_ = std::exchange(other.data_, {});
        layers_ = other.layers_;
        master_hash_ = other.master_hash_;
        layer_count_ = std::exchange(other.layer_count_, 0);
        hashes_ = std::move(other.hashes_);
        size_ = std::exchange(other.size_, 0);
        block_size_ = std::exchange(other.block_size_, 0);
        type_ = other.type_;
    }
    return *this;
}

class PlaintextNca::Section::VerifiedView final : public Storage {
    Section &section_;
public:
    explicit VerifiedView(Section &section) : section_(section) {}
    Result Read(u64 offset, std::span<std::byte> output) override { return section_.ReadVerified(offset, output); }
    Result GetSize(u64 &size) override { size = section_.size_; return 0; }
    Result QueryRange(s64 offset, s64 size, RangeInfo &info) override {
        return section_.QueryVerified(offset, size, info);
    }
};

Result PlaintextNca::Open(Storage &storage) {
    u64 size;
    KUMO_TRY(storage.GetSize(size));
    KUMO_UNLESS(size >= FirstSectionOffset, InvalidHeader);
    decltype(header_) header;
    KUMO_TRY(storage.Read(0, header));
    const auto *magic = header.data() + 0x200;
    if (std::memcmp(magic, "NCA", 3) == 0 && magic[3] >= std::byte{'0'} && magic[3] <= std::byte{'2'})
        return Error(50);
    KUMO_UNLESS(std::memcmp(magic, "NCA3", 4) == 0, Error(4517));
    KUMO_UNLESS(header[0x221] <= std::byte{1}, Error(4543));
    KUMO_UNLESS(LoadLittleEndian<u32>(header.data() + 0x21c) >= 0xb0000, Error(50));
    KUMO_UNLESS(header[0x207] < std::byte{3} || header[0x207] == std::byte{0xff}, Error(4521));
    const u64 content_size = LoadLittleEndian<u64>(header.data() + 0x208);
    KUMO_UNLESS(content_size >= FirstSectionOffset && content_size <= size, InvalidHeader);
    for (u32 i = 0; i < 4; ++i) {
        const auto *info = header.data() + 0x240 + 0x10 * i;
        const u64 start = u64{LoadLittleEndian<u32>(info)} * 0x200;
        const u64 end = u64{LoadLittleEndian<u32>(info + 4)} * 0x200;
        if (start == 0 && end == 0) continue;
        KUMO_UNLESS(start >= FirstSectionOffset && start < end && end <= content_size, SectionOutOfRange);
    }
    header_ = header;
    storage_ = &storage;
    return 0;
}

u64 PlaintextNca::ProgramId() const {
    return LoadLittleEndian<u64>(header_.data() + 0x210);
}

bool PlaintextNca::HasSection(u32 index) const {
    return storage_ && index < 4 && LoadLittleEndian<u32>(header_.data() + 0x240 + 0x10 * index) != 0;
}

Result PlaintextNca::OpenSection(u32 index, Limits limits, Section &section) const {
    KUMO_UNLESS(HasSection(index), OutOfRange);
    std::byte header[FsHeaderSize];
    KUMO_TRY(storage_->Read(0x400 + FsHeaderSize * index, header));
    KUMO_UNLESS(MatchesHash(header, header_.data() + 0x280 + HashSize * index), NcaFsHeaderHashFailed);

    KUMO_UNLESS(header[2] <= std::byte{1}, Error(4512));
    KUMO_UNLESS(header[4] == std::byte{1}, UnsupportedNcaEncryption);
    // These layers change address translation or require metadata verification.
    KUMO_UNLESS(header[5] == std::byte{0} && LoadLittleEndian<u64>(header + 0x128) == 0 &&
                LoadLittleEndian<u16>(header + 0x170) == 0, UnsupportedOperation);
    const auto *info = header_.data() + 0x240 + 0x10 * index;
    const u64 start = u64{LoadLittleEndian<u32>(info)} * 0x200;
    const u64 extent = u64{LoadLittleEndian<u32>(info + 4)} * 0x200 - start;

    // Keep the section-relative view at a stable address even when the
    // caller reopens the Section object.
    std::unique_ptr<Storage> input;
    if (LoadLittleEndian<u64>(header + 0x108) != 0) {
        auto indirect = std::unique_ptr<IndirectInput>(new (std::nothrow) IndirectInput);
        KUMO_UNLESS(indirect, AllocationFailed);
        KUMO_TRY(indirect->raw.Open(*storage_, start, extent));
        KUMO_TRY(indirect->mapping.Open(indirect->raw, std::span(header + 0x100, 0x20),
                                        detail::NcaMapping::Kind::Indirect, limits.hash_table_bytes));
        input = std::move(indirect);
    } else {
        auto raw = std::unique_ptr<StorageSlice>(new (std::nothrow) StorageSlice);
        KUMO_UNLESS(raw, AllocationFailed);
        KUMO_TRY(raw->Open(*storage_, start, extent));
        input = std::move(raw);
    }

    Section candidate;
    candidate.mapped_input_ = std::move(input);
    KUMO_TRY(OpenSectionData(*candidate.mapped_input_, header, limits, candidate));
    if (LoadLittleEndian<u64>(header + 0x180) != 0) {
        auto compression = std::unique_ptr<detail::NcaMapping>(new (std::nothrow) detail::NcaMapping);
        KUMO_UNLESS(compression, AllocationFailed);
        Section::VerifiedView verified(candidate);
        KUMO_TRY(compression->Open(verified, std::span(header + 0x178, 0x20),
                                   detail::NcaMapping::Kind::Compression, limits.hash_table_bytes));
        candidate.compression_ = std::move(compression);
    }

    section = std::move(candidate);
    return 0;
}

Result PlaintextNca::OpenSectionData(Storage &input, std::span<const std::byte> header, Limits limits,
                                     Section &section) const {
    u64 extent;
    KUMO_TRY(input.GetSize(extent));
    if (header[3] == std::byte{3}) return OpenIntegritySection(input, header, extent, limits, section);
    KUMO_UNLESS(header[3] == std::byte{2}, UnsupportedNcaHashType);
    const u32 block_size = LoadLittleEndian<u32>(header.data() + 0x28);
    KUMO_UNLESS(std::has_single_bit(block_size) && block_size >= HashSize && block_size <= 0x40000000, Error(4532));
    KUMO_UNLESS(LoadLittleEndian<u32>(header.data() + 0x2c) == 2, Error(4533));
    const u64 table_offset = LoadLittleEndian<u64>(header.data() + 0x30);
    const u64 table_size = LoadLittleEndian<u64>(header.data() + 0x38);
    const u64 data_offset = LoadLittleEndian<u64>(header.data() + 0x40);
    const u64 data_size = LoadLittleEndian<u64>(header.data() + 0x48);
    KUMO_UNLESS(Contains(extent, table_offset, table_size) && Contains(extent, data_offset, data_size),
                SectionOutOfRange);
    const u64 block_count = data_size / block_size + (data_size % block_size != 0);
    KUMO_UNLESS(table_size % HashSize == 0 && table_size <= block_size &&
                block_count <= table_size / HashSize, Error(4534));
    KUMO_UNLESS(table_size <= limits.hash_table_bytes && block_size <= limits.block_bytes, AllocationFailed);

    auto hashes = std::unique_ptr<std::byte[]>(new (std::nothrow) std::byte[table_size]);
    KUMO_UNLESS(hashes, AllocationFailed);
    std::span table(hashes.get(), static_cast<size_t>(table_size));
    if (!table.empty()) KUMO_TRY(input.Read(table_offset, table));
    KUMO_UNLESS(MatchesHash(table, header.data() + 8), NcaDataHashFailed);

    StorageSlice data;
    KUMO_TRY(data.Open(input, data_offset, data_size));
    section.data_ = data;
    section.layer_count_ = 0;
    section.hashes_ = std::move(hashes);
    section.size_ = data_size;
    section.block_size_ = block_size;
    section.type_ = static_cast<FileSystemType>(std::to_integer<u8>(header[2]));
    return 0;
}

Result PlaintextNca::OpenIntegritySection(Storage &input, std::span<const std::byte> header, u64 extent,
                                          Limits limits, Section &section) const {
    KUMO_UNLESS(std::memcmp(header.data() + 8, "IVFC", 4) == 0, Error(4602));
    KUMO_UNLESS(LoadLittleEndian<u32>(header.data() + 0x10) == HashSize, Error(4529));
    const u32 count = LoadLittleEndian<u32>(header.data() + 0x14);
    KUMO_UNLESS(count >= 2 && count <= section.layers_.size() + 1, Error(4541));
    decltype(section.layers_) layers{};
    u64 parent_size = HashSize;
    size_t scratch_bytes = 0;
    for (u32 i = 0; i < count - 1; ++i) {
        const auto *record = header.data() + 0x18 + i * 0x18;
        const u64 offset = LoadLittleEndian<u64>(record);
        const u64 size = LoadLittleEndian<u64>(record + 8);
        const u32 order = LoadLittleEndian<u32>(record + 16);
        KUMO_UNLESS(order >= 5 && order <= 30, Error(4529));
        const size_t block_size = size_t{1} << order;
        KUMO_UNLESS(Contains(extent, offset, size), Error(4511));
        const u64 blocks = size / block_size + (size % block_size != 0);
        KUMO_UNLESS(blocks <= parent_size / HashSize, Error(4529));
        KUMO_UNLESS(block_size <= limits.block_bytes - scratch_bytes, AllocationFailed);
        scratch_bytes += block_size;
        layers[i] = {offset, size, block_size};
        parent_size = size;
    }

    StorageSlice data;
    KUMO_TRY(data.Open(input, 0, extent));
    section.data_ = data;
    section.hashes_.reset();
    section.layers_ = layers;
    section.layer_count_ = count - 1;
    section.size_ = layers[count - 2].size;
    section.block_size_ = static_cast<u32>(layers[count - 2].block_size);
    section.type_ = static_cast<FileSystemType>(std::to_integer<u8>(header[2]));
    std::memcpy(section.master_hash_.data(), header.data() + 0xc8, HashSize);
    return 0;
}

Result PlaintextNca::Section::ReadIntegrityBlock(size_t level, u64 index, std::span<std::byte> block) {
    const auto &layer = layers_[level];
    const u64 offset = index * layer.block_size;
    const size_t bytes = static_cast<size_t>(std::min<u64>(layer.block_size, layer.size - offset));
    std::array<std::byte, HashSize> expected = master_hash_;
    if (level != 0) {
        const size_t parent_block_size = layers_[level - 1].block_size;
        const u64 hash_offset = index * HashSize;
        auto parent = std::unique_ptr<std::byte[]>(new (std::nothrow) std::byte[parent_block_size]);
        KUMO_UNLESS(parent, AllocationFailed);
        KUMO_TRY(ReadIntegrityBlock(level - 1, hash_offset / parent_block_size, {parent.get(), parent_block_size}));
        std::memcpy(expected.data(), parent.get() + hash_offset % parent_block_size, HashSize);
    }
    KUMO_TRY(data_.Read(layer.offset + offset, block.first(bytes)));
    std::fill(block.begin() + bytes, block.end(), std::byte{});
    KUMO_UNLESS(MatchesHash(block, expected.data()), Error(level + 1 == layer_count_ ? 4613 : 4604));
    return 0;
}

Result PlaintextNca::Section::GetSize(u64 &size) {
    KUMO_UNLESS(hashes_ || layer_count_, OutOfRange);
    size = compression_ ? compression_->Size() : size_;
    return 0;
}

Result PlaintextNca::Section::QueryRange(s64 offset, s64 size, RangeInfo &info) {
    if (compression_) {
        VerifiedView view(*this);
        return compression_->QueryRange(view, offset, size, info);
    }
    return QueryVerified(offset, size, info);
}

Result PlaintextNca::Section::QueryVerified(s64 offset, s64 size, RangeInfo &info) {
    if (!size) {
        info = {};
        return 0;
    }
    KUMO_UNLESS(offset >= 0 && size >= 0, InvalidSize);
    KUMO_UNLESS(Contains(size_, offset, size), OutOfRange);
    if (!layer_count_) {
        KUMO_UNLESS(offset % block_size_ == 0 && size % block_size_ == 0, Error(6001));
        return data_.QueryRange(offset, size, info);
    }
    const u64 base = layers_[layer_count_ - 1].offset;
    KUMO_UNLESS(base <= u64{INT64_MAX} - offset, OutOfRange);
    return data_.QueryRange(base + offset, size, info);
}

Result PlaintextNca::Section::Read(u64 offset, std::span<std::byte> output) {
    if (!compression_) return ReadVerified(offset, output);
    VerifiedView view(*this);
    const Result result = compression_->Read(view, offset, output);
    if (R_FAILED(result)) std::fill(output.begin(), output.end(), std::byte{});
    return result;
}

Result PlaintextNca::Section::ReadVerified(u64 offset, std::span<std::byte> output) {
    KUMO_UNLESS((hashes_ || layer_count_) && Contains(size_, offset, output.size()), OutOfRange);
    if (output.empty()) return 0;
    auto block = std::unique_ptr<std::byte[]>(new (std::nothrow) std::byte[block_size_]);
    KUMO_UNLESS(block, AllocationFailed);
    auto unread = output;
    while (!unread.empty()) {
        const u64 index = offset / block_size_;
        const u64 start = index * block_size_;
        const size_t within_block = static_cast<size_t>(offset - start);
        const size_t block_bytes = static_cast<size_t>(std::min<u64>(block_size_, size_ - start));
        const auto bytes = std::span(block.get(), block_bytes);
        Result result;
        if (layer_count_) {
            result = ReadIntegrityBlock(layer_count_ - 1, index, {block.get(), block_size_});
        } else {
            result = data_.Read(start, bytes);
            if (result == 0 && !MatchesHash(bytes, hashes_.get() + index * HashSize)) result = NcaDataHashFailed;
        }
        if (result != 0) {
            std::fill(output.begin(), output.end(), std::byte{});
            return result;
        }
        const size_t count = std::min(unread.size(), block_bytes - within_block);
        std::memcpy(unread.data(), bytes.data() + within_block, count);
        unread = unread.subspan(count);
        offset += count;
    }
    return 0;
}

} // namespace kumo::fs

#include "kumo_fs_nca_mapping.hpp"
#include "kumo_fs_format.hpp"
#include <algorithm>
#include <cstring>

namespace kumo::fs::detail {
namespace {
constexpr size_t NodeSize = 0x4000, NodeHeaderSize = 0x10;
constexpr size_t MaxCompressedBlock = 0x10000;
constexpr Result InvalidCompression = Error(5326);
constexpr Result InvalidEntryOffset = Error(4036);

Result DecodeLz4(std::span<const std::byte> input, std::span<std::byte> output) {
    size_t src = 0, dst = 0;
    auto length = [&](size_t &value) {
        if (value != 15)
            return true;
        while (src < input.size()) {
            const u8 next = std::to_integer<u8>(input[src++]);
            value += next;
            if (next != 255)
                return true;
        }
        return false;
    };
    while (src < input.size()) {
        const u8 token = std::to_integer<u8>(input[src++]);
        size_t literals = token >> 4;
        KUMO_UNLESS(length(literals) && literals <= input.size() - src && literals <= output.size() - dst,
                    InvalidCompression);
        std::memcpy(output.data() + dst, input.data() + src, literals);
        src += literals;
        dst += literals;
        if (src == input.size())
            break;

        KUMO_UNLESS(input.size() - src >= 2, InvalidCompression);
        const u16 distance = LoadLittleEndian<u16>(input.data() + src);
        src += 2;
        size_t match = token & 15;
        KUMO_UNLESS(distance && distance <= dst && length(match) && match + 4 <= output.size() - dst,
                    InvalidCompression);
        // LZ4 permits a match to consume bytes produced by that same match.
        for (size_t count = match + 4; count; --count, ++dst) {
            output[dst] = output[dst - distance];
        }
    }
    KUMO_UNLESS(dst == output.size(), InvalidCompression);
    return 0;
}
}

Result NcaMapping::Open(Storage &storage, std::span<const std::byte> bucket, Kind kind, size_t budget) {
    KUMO_UNLESS(bucket.size() == 0x20, InvalidSize);
    KUMO_UNLESS(std::memcmp(bucket.data() + 0x10, "BKTR", 4) == 0, Error(4032));
    KUMO_UNLESS(LoadLittleEndian<u32>(bucket.data() + 0x14) <= 1, Error(3002));
    const u32 count = LoadLittleEndian<u32>(bucket.data() + 0x18);
    KUMO_UNLESS(count > 0 && count <= INT32_MAX, Error(4033));

    const u64 table = LoadLittleEndian<u64>(bucket.data());
    const u64 bytes = LoadLittleEndian<u64>(bucket.data() + 8);
    const size_t stride = kind == Kind::Compression ? 24 : 20;
    const size_t per_node = (NodeSize - NodeHeaderSize) / stride;
    const size_t leaves = (count + per_node - 1) / per_node;
    // Within the mapping budget the root directly indexes every leaf.
    KUMO_UNLESS(leaves <= (NodeSize - NodeHeaderSize) / sizeof(u64) && (1 + leaves) * NodeSize <= budget &&
                    count <= budget / sizeof(Entry),
                AllocationFailed);
    u64 extent;
    KUMO_TRY(storage.GetSize(extent));
    KUMO_UNLESS(Contains(extent, table, bytes) && (1 + leaves) * NodeSize <= bytes,
                Error(kind == Kind::Compression ? 4547 : 4511));

    auto entries = std::unique_ptr<Entry[]>(new (std::nothrow) Entry[count]);
    auto root = std::unique_ptr<std::byte[]>(new (std::nothrow) std::byte[NodeSize]);
    auto leaf = std::unique_ptr<std::byte[]>(new (std::nothrow) std::byte[NodeSize]);
    KUMO_UNLESS(entries && root && leaf, AllocationFailed);
    KUMO_TRY(storage.Read(table, {root.get(), NodeSize}));
    KUMO_UNLESS(LoadLittleEndian<u32>(root.get()) == 0, Error(4038));
    KUMO_UNLESS(LoadLittleEndian<u32>(root.get() + 4) == leaves, Error(4034));
    const u64 logical_size = LoadLittleEndian<u64>(root.get() + 8);
    KUMO_UNLESS(logical_size > 0 && logical_size <= INT64_MAX, Error(4035));

    size_t index = 0;
    for (size_t i = 0; i < leaves; ++i) {
        KUMO_TRY(storage.Read(table + (i + 1) * NodeSize, {leaf.get(), NodeSize}));
        KUMO_UNLESS(LoadLittleEndian<u32>(leaf.get()) == i, Error(4038));
        const size_t leaf_count = std::min<size_t>(per_node, count - index);
        KUMO_UNLESS(LoadLittleEndian<u32>(leaf.get() + 4) == leaf_count, Error(4034));
        const auto *offsets = root.get() + NodeHeaderSize + i * sizeof(u64);
        const u64 first = LoadLittleEndian<u64>(offsets);
        const u64 end = i + 1 < leaves ? LoadLittleEndian<u64>(offsets + sizeof(u64)) : logical_size;
        KUMO_UNLESS(LoadLittleEndian<u64>(leaf.get() + 8) == end && first < end && end <= logical_size, Error(4037));

        for (size_t j = 0; j < leaf_count; ++j, ++index) {
            const auto *record = leaf.get() + NodeHeaderSize + j * stride;
            const u64 logical = LoadLittleEndian<u64>(record);
            const u64 physical = LoadLittleEndian<u64>(record + 8);
            const u64 next = j + 1 < leaf_count ? LoadLittleEndian<u64>(record + stride) : end;
            KUMO_UNLESS((j ? logical > first : logical == first) && logical < next && next <= end &&
                            (index || logical == 0),
                        InvalidEntryOffset);
            const u64 length = next - logical;
            const u32 type = LoadLittleEndian<u32>(record + 16);
            Entry entry{logical, physical, 0, Encoding::Plain};
            if (kind == Kind::Indirect) {
                KUMO_UNLESS(type <= 1, InvalidEntryOffset);
                if (type == 0)
                    entry.encoding = Encoding::MissingBase;
            } else {
                switch (type) {
                case 0:
                    break;
                case 1:
                    entry.encoding = Encoding::Zero;
                    break;
                case 3:
                    entry.encoding = Encoding::Lz4;
                    entry.encoded_size = LoadLittleEndian<u32>(record + 20);
                    KUMO_UNLESS(length <= MaxCompressedBlock && entry.encoded_size > 0 &&
                                    entry.encoded_size <= MaxCompressedBlock && physical % 16 == 0 &&
                                    Contains(table, physical, entry.encoded_size),
                                InvalidCompression);
                    break;
                default:
                    return InvalidCompression;
                }
            }
            if (entry.encoding == Encoding::Plain) {
                KUMO_UNLESS(Contains(table, physical, length), InvalidEntryOffset);
            }
            entries[index] = entry;
        }
    }

    entries_ = std::move(entries);
    size_ = logical_size;
    count_ = count;
    return 0;
}

size_t NcaMapping::Find(u64 offset) const {
    const auto *entry = std::upper_bound(entries_.get(), entries_.get() + count_, offset,
                                         [](u64 value, const Entry &candidate) { return value < candidate.logical; });
    return entry - entries_.get() - 1;
}

Result NcaMapping::Read(Storage &storage, u64 offset, std::span<std::byte> output) const {
    KUMO_UNLESS(Contains(size_, offset, output.size()), OutOfRange);
    if (output.empty())
        return 0;
    for (size_t i = Find(offset); !output.empty(); ++i) {
        const auto &entry = entries_[i];
        const u64 relative = offset - entry.logical;
        const size_t size = std::min<u64>(output.size(), End(i) - offset);
        auto dest = output.first(size);
        switch (entry.encoding) {
        case Encoding::Plain:
            KUMO_TRY(storage.Read(entry.physical + relative, dest));
            break;
        case Encoding::Zero:
            std::fill(dest.begin(), dest.end(), std::byte{});
            break;
        case Encoding::Lz4: {
            const size_t block_size = End(i) - entry.logical;
            auto work = std::unique_ptr<std::byte[]>(new (std::nothrow) std::byte[entry.encoded_size + block_size]);
            KUMO_UNLESS(work, AllocationFailed);
            std::span encoded(work.get(), entry.encoded_size);
            std::span decoded(work.get() + encoded.size(), block_size);
            KUMO_TRY(storage.Read(entry.physical, encoded));
            KUMO_TRY(DecodeLz4(encoded, decoded));
            std::memcpy(dest.data(), decoded.data() + relative, size);
            break;
        }
        case Encoding::MissingBase:
            // Standalone FS opens a patch without an original archive.
            return OutOfRange;
        }
        offset += size;
        output = output.subspan(size);
    }
    return 0;
}

Result NcaMapping::QueryRange(Storage &storage, s64 offset, s64 size, RangeInfo &info) const {
    KUMO_UNLESS(offset >= 0 && size >= 0 && Contains(size_, offset, size), OutOfRange);
    info = {};
    KUMO_SUCCEED_IF(size == 0);
    const u64 end = static_cast<u64>(offset) + size;
    for (size_t i = Find(offset); static_cast<u64>(offset) < end; ++i) {
        const auto &entry = entries_[i];
        const u64 next = std::min(end, End(i));
        if (entry.encoding == Encoding::MissingBase)
            return OutOfRange;
        if (entry.encoding != Encoding::Zero) {
            RangeInfo part{};
            if (entry.encoding == Encoding::Lz4) {
                KUMO_TRY(storage.QueryRange(entry.physical, entry.encoded_size, part));
            } else {
                KUMO_TRY(storage.QueryRange(entry.physical + (offset - entry.logical), next - offset, part));
            }
            info.aes_ctr_key_type |= part.aes_ctr_key_type;
            info.speed_emulation_type |= part.speed_emulation_type;
        }
        offset = next;
    }
    return 0;
}
}

#include "fs_filesystem.hpp"
#include "fs_gpt.hpp"
#include "../blockdev/fs_i_block_device.hpp"

namespace kumo::horizon::hzfs::partition {

    namespace {

        constexpr u64 GptSignature = 0x5452415020494645ul; /* "EFI PART" */

        constexpr size_t MaxEntryCount     = 256;
        constexpr size_t MinEntrySize      = 128;
        constexpr size_t MaxEntryTableSize = (64 * 1024);

        struct GptHeader {
            u64 signature;
            u32 revision;
            u32 header_size;
            u32 header_crc32;
            u32 reserved;
            u64 my_lba;
            u64 alternate_lba;
            u64 first_usable_lba;
            u64 last_usable_lba;
            u8  disk_guid[16];
            u64 partition_entry_lba;
            u32 partition_entry_count;
            u32 partition_entry_size;
            u32 partition_entry_array_crc32;
        };
        /* The header is 92 bytes on disk; the struct is padded to 96. */
        constexpr size_t GptHeaderSize = 92;
        static_assert(offsetof(GptHeader, partition_entry_array_crc32) + sizeof(u32) == GptHeaderSize);

        struct GptEntry {
            u8  type_guid[16];
            u8  unique_guid[16];
            u64 first_lba;
            u64 last_lba;   /* inclusive */
            u64 attributes;
            u16 name[PartitionNameLength];
        };
        static_assert(sizeof(GptEntry) == 128);

        /* Scratch space for one parse at a time; ReadGpt runs during single-threaded start-up. */
        constinit u8 g_block_buffer[blockdev::MaxBlockSize] = {};
        constinit u8 g_entry_buffer[MaxEntryTableSize] = {};

        /* CRC-32 as used by GPT (IEEE 802.3, reflected). */
        u32 CalculateCrc32(const void *data, size_t size) {
            const u8 *p = static_cast<const u8 *>(data);
            u32 crc = 0xFFFFFFFF;
            for (size_t i = 0; i < size; ++i) {
                crc ^= p[i];
                for (int bit = 0; bit < 8; ++bit) {
                    crc = (crc >> 1) ^ (0xEDB88320 & (0 - (crc & 1)));
                }
            }
            return ~crc;
        }

        bool IsZero(const u8 *data, size_t size) {
            for (size_t i = 0; i < size; ++i) {
                if (data[i] != 0) {
                    return false;
                }
            }
            return true;
        }

        /* Read and validate the header at `lba` and its entry array. */
        Result ReadGptAt(PartitionTable *out, fs::IStorage *disk, u32 block_size, u64 lba, u64 disk_blocks) {
            /* Read the header block. */
            KUMO_TRY(disk->Read(static_cast<s64>(lba * block_size), std::as_writable_bytes(std::span(g_block_buffer, block_size))));

            GptHeader header;
            std::memcpy(std::addressof(header), g_block_buffer, sizeof(header));

            /* Validate the header. */
            KUMO_UNLESS(header.signature == GptSignature,                                           fs::ResultPartitionNotFound());
            KUMO_UNLESS(GptHeaderSize <= header.header_size && header.header_size <= block_size,     fs::ResultPartitionNotFound());
            KUMO_UNLESS(header.my_lba == lba,                                                       fs::ResultPartitionNotFound());

            /* The header checksum is computed with its own field zeroed. */
            const u32 expected_header_crc = header.header_crc32;
            std::memset(g_block_buffer + offsetof(GptHeader, header_crc32), 0, sizeof(u32));
            KUMO_UNLESS(CalculateCrc32(g_block_buffer, header.header_size) == expected_header_crc, fs::ResultPartitionNotFound());

            /* Validate the entry array's shape. */
            KUMO_UNLESS(MinEntrySize <= header.partition_entry_size,             fs::ResultPartitionNotFound());
            KUMO_UNLESS(0 < header.partition_entry_count && header.partition_entry_count <= MaxEntryCount, fs::ResultPartitionNotFound());

            const size_t table_size = static_cast<size_t>(header.partition_entry_count) * header.partition_entry_size;
            KUMO_UNLESS(table_size <= MaxEntryTableSize,                         fs::ResultPartitionNotFound());
            KUMO_UNLESS(header.partition_entry_lba < disk_blocks,                fs::ResultPartitionNotFound());

            /* Read and check the entry array. */
            KUMO_TRY(disk->Read(static_cast<s64>(header.partition_entry_lba * block_size), std::as_writable_bytes(std::span(g_entry_buffer, table_size))));
            KUMO_UNLESS(CalculateCrc32(g_entry_buffer, table_size) == header.partition_entry_array_crc32, fs::ResultPartitionNotFound());

            /* Collect the partitions that are in use. */
            out->count = 0;
            for (u32 i = 0; i < header.partition_entry_count && out->count < MaxPartitions; ++i) {
                GptEntry entry;
                std::memcpy(std::addressof(entry), g_entry_buffer + static_cast<size_t>(i) * header.partition_entry_size, sizeof(entry));

                /* An all-zero type GUID marks an unused slot. */
                if (IsZero(entry.type_guid, sizeof(entry.type_guid))) {
                    continue;
                }

                /* Skip entries that do not describe a range on this disk. */
                if (entry.first_lba > entry.last_lba || entry.last_lba >= disk_blocks) {
                    continue;
                }

                PartitionEntry &dst = out->entries[out->count++];
                dst.first_lba   = entry.first_lba;
                dst.block_count = entry.last_lba - entry.first_lba + 1;

                size_t n = 0;
                for (; n < PartitionNameLength && entry.name[n] != 0; ++n) {
                    dst.name[n] = (entry.name[n] < 0x80) ? static_cast<char>(entry.name[n]) : '?';
                }
                dst.name[n] = '\0';
            }

            KUMO_SUCCEED();
        }

    }

    Result ReadGpt(PartitionTable *out, fs::IStorage *disk, u32 block_size) {
        KUMO_ASSERT(block_size <= blockdev::MaxBlockSize);

        u64 disk_size;
        KUMO_TRY(disk->GetSize(disk_size));

        const u64 disk_blocks = static_cast<u64>(disk_size) / block_size;
        KUMO_UNLESS(disk_blocks >= 3, fs::ResultPartitionNotFound());

        /* Try the primary header, then the backup in the last block. */
        KUMO_SUCCEED_IF(R_SUCCEEDED(ReadGptAt(out, disk, block_size, 1, disk_blocks)));
        KUMO_RETURN(ReadGptAt(out, disk, block_size, disk_blocks - 1, disk_blocks));
    }

}

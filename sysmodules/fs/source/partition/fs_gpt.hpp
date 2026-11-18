/*
 * GUID Partition Table reader.
 */
#pragma once
#include "fs_filesystem.hpp"

namespace kumo::horizon::hzfs::partition {

    constexpr inline size_t MaxPartitions       = 32;
    constexpr inline size_t PartitionNameLength = 36;

    struct PartitionEntry {
        /* The GPT stores names as UTF-16; characters outside ASCII are replaced with '?'. */
        char name[PartitionNameLength + 1];
        u64  first_lba;
        u64  block_count;
    };

    struct PartitionTable {
        size_t count;
        PartitionEntry entries[MaxPartitions];
    };

    /* Read the GPT of a disk with the given logical block size. The primary header is used if */
    /* it is valid, otherwise the backup at the end of the disk. Fails with */
    /* fs::ResultPartitionNotFound if the disk has no valid GPT. */
    Result ReadGpt(PartitionTable *out, fs::IStorage *disk, u32 block_size);

}

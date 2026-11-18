/*
 * The FAT volumes FS can mount, each backed by a named partition of the
 * system storage. A volume is mounted the first time it is opened.
 */
#pragma once
#include "fs_filesystem.hpp"

namespace kumo::horizon::hzfs::fat {

    /* The value is also the FatFs drive number. */
    enum Volume : int {
        Volume_SdCard = 0,          /* partition "SDCARD" */
        Volume_System,              /* partition "SYSTEM" */
        Volume_User,                /* partition "USER" */
        Volume_Safe,                /* partition "SAFE" */
        Volume_CalibrationFile,     /* partition "PRODINFOF" */

        Volume_Count,
    };

    /* The volume holding a BIS partition's filesystem. Fails for partitions that have none. */
    Result GetBisVolume(Volume *out, fs::BisPartitionId id);

    /* The volume's filesystem, mounting it if needed. Fails if the partition is missing or unformatted. */
    Result OpenVolume(std::shared_ptr<fs::fsa::IFileSystem> *out, Volume volume);

    /* Create a new, empty filesystem on the volume's partition, destroying its contents. */
    Result FormatVolume(Volume volume);

}

/*
 * The partitions of the system storage, and how Horizon's names for them map
 * onto it.
 *
 * The system disk uses a GPT whose partition names are the ones the Switch
 * uses on its eMMC (SYSTEM, USER, SAFE, PRODINFO, PRODINFOF, BCPKG2-...).
 * One further name, SDCARD, stands in for the SD card slot.
 */
#pragma once
#include "fs_filesystem.hpp"

namespace kumo::horizon::hzfs::partition {

    constexpr inline const char SdCardPartitionName[] = "SDCARD";

    /* Read the system storage's partition table. Call once, after the drivers have probed. */
    void Initialize();

    /* Logical block size of the system storage, or 0 if there is none. */
    u32 GetBlockSize();

    /* Storage for the partition with this GPT name, or nullptr. */
    fs::IStorage *FindPartition(const char *name);

    /* GPT partition name for a BIS partition id, or nullptr if the id has no partition of its own. */
    const char *GetBisPartitionName(fs::BisPartitionId id);

    /* Storage for a BIS partition id. UserDataRoot is the whole system storage. */
    Result GetBisStorage(fs::IStorage **out, fs::BisPartitionId id);

}

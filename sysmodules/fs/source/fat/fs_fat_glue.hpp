/*
 * The functions FatFs needs from its host: block I/O on its numbered drives,
 * one mutex per volume, and the result conversion shared by the rest of fat/.
 */
#pragma once
#include "fs_filesystem.hpp"

extern "C" {
    #include <ff.h>
}

namespace kumo::horizon::hzfs::fat {

    constexpr inline int MaxDrives = FF_VOLUMES;

    /* Make `storage` FatFs drive number `drive`. Each drive holds one volume, with no partition table of its own. */
    // FatFs has a process-wide sharing table. Mount and format bypass its
    // internal volume locks, so hold this lock across all FatFs operations.
    class FatAccess {
    public:
        void lock();
        void unlock();
    };
    extern FatAccess g_fat_access;

    void SetDriveStorage(int drive, fs::IStorage *storage, u32 sector_size);

    /* Convert a FatFs result. FR_DISK_ERR becomes the storage error that caused it. */
    Result ConvertResult(FRESULT fr, int drive);

}

#include "fs_filesystem.hpp"
#include "fs_fat_glue.hpp"

extern "C" {
    #include <diskio.h>
}

namespace kumo::horizon::hzfs::fat {

    namespace {

        struct Drive {
            fs::IStorage *storage;
            u32 sector_size;
            u32 last_error;     /* value of the last storage Result; 0 is success */
        };

        constinit RMutex g_fat_mutex{};
        constinit Drive g_drives[MaxDrives] = {};

        /* One mutex per volume, plus one FatFs uses for its open-file table. */
        constinit kumo::Mutex g_mutexes[FF_VOLUMES + 1];

        inline Drive *GetDrive(BYTE pdrv) {
            return (pdrv < MaxDrives && g_drives[pdrv].storage != nullptr) ? std::addressof(g_drives[pdrv]) : nullptr;
        }

    }

    FatAccess g_fat_access;
    void FatAccess::lock() { rmutexLock(&g_fat_mutex); }
    void FatAccess::unlock() { rmutexUnlock(&g_fat_mutex); }

    void SetDriveStorage(int drive, fs::IStorage *storage, u32 sector_size) {
        KUMO_ASSERT(0 <= drive && drive < MaxDrives);
        KUMO_ASSERT(FF_MIN_SS <= sector_size && sector_size <= FF_MAX_SS);

        g_drives[drive] = { storage, sector_size, 0 };
    }

    Result ConvertResult(FRESULT fr, int drive) {
        switch (fr) {
            case FR_OK:
                KUMO_SUCCEED();
            case FR_DISK_ERR:
                /* Report what the storage said, if it said anything. */
                if (0 <= drive && drive < MaxDrives && g_drives[drive].last_error != 0) {
                    KUMO_RETURN(g_drives[drive].last_error);
                }
                KUMO_THROW(fs::ResultUnexpected());
            case FR_INT_ERR:
            case FR_NO_FILESYSTEM:
                KUMO_THROW(fs::ResultFatFileSystemCorrupted());
            case FR_NOT_READY:
            case FR_INVALID_DRIVE:
            case FR_NOT_ENABLED:
                KUMO_THROW(fs::ResultPartitionNotFound());
            case FR_NO_FILE:
            case FR_NO_PATH:
                KUMO_THROW(fs::ResultPathNotFound());
            case FR_INVALID_NAME:
                KUMO_THROW(fs::ResultInvalidPathFormat());
            case FR_EXIST:
                KUMO_THROW(fs::ResultPathAlreadyExists());
            case FR_DENIED:
            case FR_WRITE_PROTECTED:
                /* Callers that know why access was refused convert FR_DENIED themselves. */
                KUMO_THROW(fs::ResultPermissionDenied());
            case FR_LOCKED:
            case FR_TIMEOUT:
                KUMO_THROW(fs::ResultTargetLocked());
            case FR_NOT_ENOUGH_CORE:
                KUMO_THROW(fs::ResultAllocationMemoryFailed());
            case FR_TOO_MANY_OPEN_FILES:
                KUMO_THROW(fs::ResultOpenCountLimit());
            case FR_INVALID_PARAMETER:
                KUMO_THROW(fs::ResultInvalidArgument());
            case FR_INVALID_OBJECT:
            case FR_MKFS_ABORTED:
            default:
                KUMO_THROW(fs::ResultUnexpected());
        }
    }

}

/* ===== Functions called by FatFs ===== */

using namespace kumo::horizon;
using namespace kumo::horizon::hzfs::fat;

extern "C" {

    DSTATUS disk_initialize(BYTE pdrv) {
        return GetDrive(pdrv) != nullptr ? 0 : STA_NOINIT;
    }

    DSTATUS disk_status(BYTE pdrv) {
        return GetDrive(pdrv) != nullptr ? 0 : STA_NOINIT;
    }

    DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
        Drive *drive = GetDrive(pdrv);
        if (drive == nullptr) {
            return RES_NOTRDY;
        }

        drive->last_error = drive->storage->Read(static_cast<s64>(sector) * drive->sector_size, std::as_writable_bytes(std::span(buff, static_cast<size_t>(count) * drive->sector_size)));
        return drive->last_error == 0 ? RES_OK : RES_ERROR;
    }

    DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
        Drive *drive = GetDrive(pdrv);
        if (drive == nullptr) {
            return RES_NOTRDY;
        }

        drive->last_error = drive->storage->Write(static_cast<s64>(sector) * drive->sector_size, std::as_bytes(std::span(buff, static_cast<size_t>(count) * drive->sector_size)));
        return drive->last_error == 0 ? RES_OK : RES_ERROR;
    }

    DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
        Drive *drive = GetDrive(pdrv);
        if (drive == nullptr) {
            return RES_NOTRDY;
        }

        switch (cmd) {
            case CTRL_SYNC:
                drive->last_error = drive->storage->Flush();
                return drive->last_error == 0 ? RES_OK : RES_ERROR;
            case GET_SECTOR_COUNT:
                {
                    u64 size;
                    drive->last_error = drive->storage->GetSize(size);
                    if (drive->last_error != 0) {
                        return RES_ERROR;
                    }
                    *static_cast<LBA_t *>(buff) = static_cast<LBA_t>(size / drive->sector_size);
                    return RES_OK;
                }
            case GET_SECTOR_SIZE:
                *static_cast<WORD *>(buff) = static_cast<WORD>(drive->sector_size);
                return RES_OK;
            case GET_BLOCK_SIZE:
                /* No erase block to align to. */
                *static_cast<DWORD *>(buff) = 1;
                return RES_OK;
            default:
                return RES_PARERR;
        }
    }

    /* Volume locking (FF_FS_REENTRANT). The mutexes are static, so creation cannot fail. */
    int ff_mutex_create(int vol) {
        (void)vol;
        return 1;
    }

    void ff_mutex_delete(int vol) {
        (void)vol;
    }

    int ff_mutex_take(int vol) {
        g_mutexes[vol].lock();
        return 1;
    }

    void ff_mutex_give(int vol) {
        g_mutexes[vol].unlock();
    }

}

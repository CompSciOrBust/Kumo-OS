#include "fs_filesystem.hpp"
#include "fs_fat_volumes.hpp"
#include "fs_fat_file_system.hpp"
#include "../fs_log.hpp"
#include "../partition/fs_partitions.hpp"

namespace kumo::horizon::hzfs::fat {

    namespace {

        static_assert(Volume_Count <= MaxDrives);

        constexpr const char * const PartitionNames[Volume_Count] = {
            [Volume_SdCard]          = partition::SdCardPartitionName,
            [Volume_System]          = "SYSTEM",
            [Volume_User]            = "USER",
            [Volume_Safe]            = "SAFE",
            [Volume_CalibrationFile] = "PRODINFOF",
        };

        constinit bool g_attached[Volume_Count] = {};
        constinit std::shared_ptr<FatFileSystem> g_file_systems[Volume_Count] = {};

        /* Working memory for f_mkfs; larger is faster. */
        constinit u8 g_format_buffer[(64 * 1024)] = {};

        /* Point the volume's FatFs drive at its partition. */
        Result AttachVolume(Volume volume) {
            KUMO_SUCCEED_IF(g_attached[volume]);

            fs::IStorage *storage = partition::FindPartition(PartitionNames[volume]);
            KUMO_UNLESS(storage != nullptr, fs::ResultPartitionNotFound());

            SetDriveStorage(volume, storage, partition::GetBlockSize());
            g_attached[volume] = true;
            KUMO_SUCCEED();
        }

    }

    Result GetBisVolume(Volume *out, fs::BisPartitionId id) {
        switch (id) {
            case fs::BisPartitionId::CalibrationFile:       *out = Volume_CalibrationFile; KUMO_SUCCEED();
            case fs::BisPartitionId::SafeMode:              *out = Volume_Safe;            KUMO_SUCCEED();
            case fs::BisPartitionId::User:                  *out = Volume_User;            KUMO_SUCCEED();
            case fs::BisPartitionId::System:
            case fs::BisPartitionId::SystemProperPartition: *out = Volume_System;          KUMO_SUCCEED();
            default:
                KUMO_THROW(fs::ResultInvalidArgument());
        }
    }

    Result OpenVolume(std::shared_ptr<fs::fsa::IFileSystem> *out, Volume volume) {
        KUMO_ASSERT(0 <= volume && volume < Volume_Count);

        std::scoped_lock lock(g_fat_access);

        if (g_file_systems[volume] == nullptr) {
            KUMO_TRY(AttachVolume(volume));

            auto file_system = fs::AllocateShared<FatFileSystem>(static_cast<int>(volume));
            KUMO_UNLESS(file_system != nullptr, fs::ResultAllocationMemoryFailed());

            if (const Result rc = file_system->Mount(); R_FAILED(rc)) {
                Log("[fs] partition \"%s\": no usable FAT filesystem (rc=0x%x)\n", PartitionNames[volume], rc);
                KUMO_RETURN(rc);
            }

            Log("[fs] mounted partition \"%s\"\n", PartitionNames[volume]);
            g_file_systems[volume] = std::move(file_system);
        }

        if (!g_file_systems[volume]->IsMounted()) KUMO_TRY(g_file_systems[volume]->Mount());
        *out = g_file_systems[volume];
        KUMO_SUCCEED();
    }

    Result FormatVolume(Volume volume) {
        KUMO_ASSERT(0 <= volume && volume < Volume_Count);

        std::scoped_lock lock(g_fat_access);

        KUMO_TRY(AttachVolume(volume));

        /* Any filesystem object already handed out goes stale: its files and directories fail from here on. */
        if (g_file_systems[volume] != nullptr) {
            g_file_systems[volume]->Unmount();
        }

        /* The partition is the whole FatFs drive, so no partition table is written inside it (FM_SFD). */
        /* FatFs picks FAT12/16/32 or exFAT from the size. */
        const char path[] = { static_cast<char>('0' + volume), ':', '\0' };
        const MKFS_PARM parameters = { .fmt = FM_ANY | FM_SFD, .n_fat = 0, .align = 0, .n_root = 0, .au_size = 0 };
        KUMO_TRY(ConvertResult(f_mkfs(path, std::addressof(parameters), g_format_buffer, sizeof(g_format_buffer)), volume));

        Log("[fs] formatted partition \"%s\"\n", PartitionNames[volume]);

        /* Bring an existing filesystem object back on the new filesystem. */
        if (g_file_systems[volume] != nullptr) {
            KUMO_TRY(g_file_systems[volume]->Mount());
        }
        KUMO_SUCCEED();
    }

}

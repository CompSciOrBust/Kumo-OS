#include "fs_filesystem.hpp"
#include "fs_save_data.hpp"
#include "fs_save_filesystem.hpp"
#include "../fat/fs_fat_volumes.hpp"
#include "../partition/fs_partitions.hpp"

namespace kumo::horizon::hzfs::savedata {

    namespace {

        constexpr const char SaveDirectory[] = "/save";
        constexpr const char ExtraDataName[] = "extra";
        constexpr const char LockFileName[]  = ".lock";

        constexpr size_t SavePathLengthMax = 64;

        /* Serializes creation, deletion and extra data access. Mounted save data have their own locking. */
        constinit kumo::Mutex g_mutex;

        Result GetVolume(fat::Volume *out, fs::SaveDataSpaceId space_id) {
            switch (space_id) {
                case fs::SaveDataSpaceId::System:
                case fs::SaveDataSpaceId::ProperSystem:
                    *out = fat::Volume_System;
                    break;
                case fs::SaveDataSpaceId::User:
                    *out = fat::Volume_User;
                    break;
                case fs::SaveDataSpaceId::SafeMode:
                    *out = fat::Volume_Safe;
                    break;
                case fs::SaveDataSpaceId::SdSystem:
                case fs::SaveDataSpaceId::SdUser:
                    KUMO_UNLESS(partition::FindPartition(partition::SdCardPartitionName) != nullptr, fs::ResultSdCardNotPresent());
                    *out = fat::Volume_SdCard;
                    break;
                default:
                    KUMO_THROW(fs::ResultInvalidSaveDataSpaceId());
            }
            KUMO_SUCCEED();
        }

        Result CheckAttribute(const fs::SaveDataAttribute &attribute) {
            KUMO_UNLESS(attribute.type == fs::SaveDataType::System, fs::ResultInvalidArgument());
            KUMO_UNLESS(attribute.system_save_data_id != fs::InvalidSystemSaveDataId, fs::ResultInvalidArgument());
            KUMO_SUCCEED();
        }

        /* "/save/<id>", or "/save/<id>_<user>" for a save data that belongs to a user. */
        void MakeSavePath(char *dst, size_t dst_size, const fs::SaveDataAttribute &attribute, const char *leaf = nullptr) {
            int len;
            if (attribute.user_id == fs::InvalidUserId) {
                len = std::snprintf(dst, dst_size, "%s/%016" PRIx64, SaveDirectory, attribute.system_save_data_id);
            } else {
                len = std::snprintf(dst, dst_size, "%s/%016" PRIx64 "_%016" PRIx64 "%016" PRIx64, SaveDirectory, attribute.system_save_data_id, attribute.user_id.data[1], attribute.user_id.data[0]);
            }
            if (leaf != nullptr) {
                len += std::snprintf(dst + len, dst_size - len, "/%s", leaf);
            }
            KUMO_ASSERT(len > 0 && static_cast<size_t>(len) < dst_size);
        }

        Result MakePath(fs::Path *out, const char *path) {
            KUMO_RETURN(out->InitializeWithNormalization(path));
        }

        /* A save data exists once its extra data has been written, which is the last step of creating it. */
        Result ReadExtraDataImpl(fs::SaveDataExtraData *out, fs::fsa::IFileSystem *volume_fs, const fs::SaveDataAttribute &attribute) {
            char path[SavePathLengthMax];
            MakeSavePath(path, sizeof(path), attribute, ExtraDataName);

            fs::Path fs_path;
            KUMO_TRY(MakePath(std::addressof(fs_path), path));

            std::unique_ptr<fs::fsa::IFile> file;
            { const Result rc = volume_fs->OpenFile(std::addressof(file), fs_path, fs::OpenMode_Read); KUMO_TRY(rc == fs::ResultPathNotFound() ? fs::ResultTargetNotFound() : rc); }

            size_t read;
            KUMO_TRY(file->Read(std::addressof(read), 0, out, sizeof(*out), fs::ReadOption()));
            KUMO_UNLESS(read == sizeof(*out), fs::ResultTargetNotFound());
            KUMO_SUCCEED();
        }

        Result WriteExtraDataImpl(fs::fsa::IFileSystem *volume_fs, const fs::SaveDataAttribute &attribute, const fs::SaveDataExtraData &extra_data) {
            char path[SavePathLengthMax];
            MakeSavePath(path, sizeof(path), attribute, ExtraDataName);

            fs::Path fs_path;
            KUMO_TRY(MakePath(std::addressof(fs_path), path));

            { const Result rc = volume_fs->CreateFile(fs_path, sizeof(extra_data), 0); if (rc != fs::ResultPathAlreadyExists()) { KUMO_TRY(rc); } }

            std::unique_ptr<fs::fsa::IFile> file;
            KUMO_TRY(volume_fs->OpenFile(std::addressof(file), fs_path, fs::OpenMode_Write));
            KUMO_RETURN(file->Write(0, std::addressof(extra_data), sizeof(extra_data), fs::WriteOption::Flush));
        }

        Result DeleteDirectoryIfExists(fs::fsa::IFileSystem *volume_fs, const char *path) {
            fs::Path fs_path;
            KUMO_TRY(MakePath(std::addressof(fs_path), path));

            { const Result rc = volume_fs->DeleteDirectoryRecursively(fs_path); if (rc != fs::ResultPathNotFound()) { KUMO_TRY(rc); } }
            KUMO_SUCCEED();
        }

        Result CreateDirectoryIfMissing(fs::fsa::IFileSystem *volume_fs, const char *path) {
            fs::Path fs_path;
            KUMO_TRY(MakePath(std::addressof(fs_path), path));

            { const Result rc = volume_fs->CreateDirectory(fs_path); if (rc != fs::ResultPathAlreadyExists()) { KUMO_TRY(rc); } }
            KUMO_SUCCEED();
        }

    }

    Result CreateSystemSaveData(const fs::SaveDataAttribute &attribute, const fs::SaveDataCreationInfo &creation_info) {
        KUMO_TRY(CheckAttribute(attribute));

        fat::Volume volume;
        KUMO_TRY(GetVolume(std::addressof(volume), creation_info.space_id));

        std::shared_ptr<fs::fsa::IFileSystem> volume_fs;
        KUMO_TRY(fat::OpenVolume(std::addressof(volume_fs), volume));

        std::scoped_lock lk(g_mutex);

        auto extra_data = fs::AllocateShared<fs::SaveDataExtraData>();
        KUMO_UNLESS(extra_data != nullptr, fs::ResultAllocationMemoryFailedInFileSystemProxyImplA());
        const Result read = ReadExtraDataImpl(extra_data.get(), volume_fs.get(), attribute);
        KUMO_UNLESS(R_FAILED(read), fs::ResultPathAlreadyExists());
        if (read != fs::ResultTargetNotFound()) return read;

        /* Anything at the path now is the remains of a creation that did not finish. */
        char path[SavePathLengthMax];
        MakeSavePath(path, sizeof(path), attribute);
        KUMO_TRY(DeleteDirectoryIfExists(volume_fs.get(), path));

        KUMO_TRY(CreateDirectoryIfMissing(volume_fs.get(), SaveDirectory));
        KUMO_TRY(CreateDirectoryIfMissing(volume_fs.get(), path));

        *extra_data = {};
        extra_data->attr           = attribute;
        extra_data->owner_id       = creation_info.owner_id;
        extra_data->flags          = creation_info.flags;
        extra_data->available_size = creation_info.size;
        extra_data->journal_size   = creation_info.journal_size;
        KUMO_RETURN(WriteExtraDataImpl(volume_fs.get(), attribute, *extra_data));
    }

    Result OpenSystemSaveData(std::shared_ptr<fs::fsa::IFileSystem> *out, fs::SaveDataSpaceId space_id, const fs::SaveDataAttribute &attribute) {
        KUMO_TRY(CheckAttribute(attribute));

        fat::Volume volume;
        KUMO_TRY(GetVolume(std::addressof(volume), space_id));

        std::shared_ptr<fs::fsa::IFileSystem> volume_fs;
        KUMO_TRY(fat::OpenVolume(std::addressof(volume_fs), volume));

        std::scoped_lock lk(g_mutex);

        {
            auto extra_data = fs::AllocateShared<fs::SaveDataExtraData>();
            KUMO_UNLESS(extra_data != nullptr, fs::ResultAllocationMemoryFailedInFileSystemProxyImplA());
            KUMO_TRY(ReadExtraDataImpl(extra_data.get(), volume_fs.get(), attribute));
        }

        char path[SavePathLengthMax];
        MakeSavePath(path, sizeof(path), attribute);

        fs::Path fs_path;
        KUMO_TRY(MakePath(std::addressof(fs_path), path));

        auto directory_fs = fs::AllocateShared<fs::fsa::SubDirectoryFileSystem>(std::move(volume_fs));
        KUMO_UNLESS(directory_fs != nullptr, fs::ResultAllocationMemoryFailedInFileSystemProxyImplA());
        KUMO_TRY(directory_fs->Initialize(fs_path));

        return MountJournal(out, std::move(directory_fs));
    }

    Result DeleteSystemSaveData(fs::SaveDataSpaceId space_id, const fs::SaveDataAttribute &attribute) {
        KUMO_TRY(CheckAttribute(attribute));

        fat::Volume volume;
        KUMO_TRY(GetVolume(std::addressof(volume), space_id));

        std::shared_ptr<fs::fsa::IFileSystem> volume_fs;
        KUMO_TRY(fat::OpenVolume(std::addressof(volume_fs), volume));

        std::scoped_lock lk(g_mutex);

        char path[SavePathLengthMax];
        MakeSavePath(path, sizeof(path), attribute);

        fs::Path fs_path;
        KUMO_TRY(MakePath(std::addressof(fs_path), path));

        /* A mounted save data holds its lock file open for writing, so a second such open fails. */
        /* Check before deleting anything: a recursive delete would stop at the lock file, but only after removing other files. */
        {
            char lock_path[SavePathLengthMax];
            MakeSavePath(lock_path, sizeof(lock_path), attribute, LockFileName);

            fs::Path lock_fs_path;
            KUMO_TRY(MakePath(std::addressof(lock_fs_path), lock_path));

            std::unique_ptr<fs::fsa::IFile> lock_file;
            const Result rc = volume_fs->OpenFile(&lock_file, lock_fs_path, fs::OpenMode_ReadWrite);
            KUMO_UNLESS(R_SUCCEEDED(rc) || rc == fs::ResultPathNotFound(), fs::ResultTargetLocked());
        }

        { const Result rc = volume_fs->DeleteDirectoryRecursively(fs_path); KUMO_TRY(rc == fs::ResultPathNotFound() ? fs::ResultTargetNotFound() : rc); }
        KUMO_SUCCEED();
    }

    Result ReadExtraData(fs::SaveDataExtraData *out, fs::SaveDataSpaceId space_id, const fs::SaveDataAttribute &attribute) {
        KUMO_TRY(CheckAttribute(attribute));

        fat::Volume volume;
        KUMO_TRY(GetVolume(std::addressof(volume), space_id));

        std::shared_ptr<fs::fsa::IFileSystem> volume_fs;
        KUMO_TRY(fat::OpenVolume(std::addressof(volume_fs), volume));

        std::scoped_lock lk(g_mutex);
        KUMO_RETURN(ReadExtraDataImpl(out, volume_fs.get(), attribute));
    }

    Result WriteExtraDataFlags(fs::SaveDataSpaceId space_id, const fs::SaveDataAttribute &attribute, u32 flags) {
        KUMO_TRY(CheckAttribute(attribute));

        fat::Volume volume;
        KUMO_TRY(GetVolume(std::addressof(volume), space_id));

        std::shared_ptr<fs::fsa::IFileSystem> volume_fs;
        KUMO_TRY(fat::OpenVolume(std::addressof(volume_fs), volume));

        std::scoped_lock lk(g_mutex);

        auto extra_data = fs::AllocateShared<fs::SaveDataExtraData>();
        KUMO_UNLESS(extra_data != nullptr, fs::ResultAllocationMemoryFailedInFileSystemProxyImplA());
        KUMO_TRY(ReadExtraDataImpl(extra_data.get(), volume_fs.get(), attribute));

        extra_data->flags = flags;
        KUMO_RETURN(WriteExtraDataImpl(volume_fs.get(), attribute, *extra_data));
    }

}

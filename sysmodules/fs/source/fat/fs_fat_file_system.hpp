/*
 * A mounted FAT or exFAT volume as an fs::fsa::IFileSystem, on top of FatFs.
 * FS exposes the volume through its Kumo IPC bindings.
 */
#pragma once
#include "fs_filesystem.hpp"
#include "fs_fat_glue.hpp"

namespace kumo::horizon::hzfs::fat {

    class FatFileSystem : public fs::fsa::IFileSystem {
        KUMO_NON_COPYABLE(FatFileSystem);
        KUMO_NON_MOVEABLE(FatFileSystem);
        private:
            int m_drive;
            bool m_mounted;
            FATFS m_fatfs;
        public:
            /* `drive` is the FatFs drive number; its storage must already be set with SetDriveStorage. */
            explicit FatFileSystem(int drive);
            virtual ~FatFileSystem() override;

            /* Read the volume. Fails if it holds no FAT or exFAT filesystem. */
            Result Mount();
            void Unmount();
            bool IsMounted() const { return m_mounted; }
        private:
            virtual Result DoCreateFile(const fs::Path &path, s64 size, int flags) override;
            virtual Result DoDeleteFile(const fs::Path &path) override;
            virtual Result DoCreateDirectory(const fs::Path &path) override;
            virtual Result DoDeleteDirectory(const fs::Path &path) override;
            virtual Result DoDeleteDirectoryRecursively(const fs::Path &path) override;
            virtual Result DoRenameFile(const fs::Path &old_path, const fs::Path &new_path) override;
            virtual Result DoRenameDirectory(const fs::Path &old_path, const fs::Path &new_path) override;
            virtual Result DoGetEntryType(fs::DirectoryEntryType *out, const fs::Path &path) override;
            virtual Result DoOpenFile(std::unique_ptr<fs::fsa::IFile> *out_file, const fs::Path &path, fs::OpenMode mode) override;
            virtual Result DoOpenDirectory(std::unique_ptr<fs::fsa::IDirectory> *out_dir, const fs::Path &path, fs::OpenDirectoryMode mode) override;
            virtual Result DoCommit() override;
            virtual Result DoGetFreeSpaceSize(s64 *out, const fs::Path &path) override;
            virtual Result DoGetTotalSpaceSize(s64 *out, const fs::Path &path) override;
            virtual Result DoCleanDirectoryRecursively(const fs::Path &path) override;
            virtual Result DoGetFileTimeStampRaw(fs::FileTimeStampRaw *out, const fs::Path &path) override;
            virtual Result DoFlush() override;
        private:
            Result Rename(const fs::Path &old_path, const fs::Path &new_path, bool directory);
            Result GetSpace(s64 *out_free, s64 *out_total);
    };

}

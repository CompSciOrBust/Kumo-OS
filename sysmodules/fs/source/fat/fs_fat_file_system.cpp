#include "fs_filesystem.hpp"
#include "fs_fat_file_system.hpp"

extern "C" {
    #include <diskio.h>
}

namespace kumo::horizon::hzfs::fat {

    namespace {

        /* "N:" + a Horizon path + terminator. */
        constexpr size_t FatPathSize = 2 + fs::EntryNameLengthMax + 1;

        /* Largest count passed to one f_read/f_write call, whose counts are 32-bit. */
        constexpr size_t MaxTransferSize = (1024ULL * 1024 * 1024);

        constexpr bool IsRootPath(const char *fat_path) {
            return fat_path[2] == '/' && fat_path[3] == '\0';
        }

        bool HasValidHandle(const FFOBJID &object) {
            return object.fs && object.fs->fs_type && object.id == object.fs->id;
        }

        /* Build the FatFs path ("N:/dir/file") for a normalized Horizon path. */
        Result MakeFatPath(char *dst, int drive, const fs::Path &path) {
            const char *src  = path.GetString();
            const size_t len = std::strlen(src);
            KUMO_UNLESS(len > 0 && src[0] == '/',  fs::ResultInvalidPathFormat());
            KUMO_UNLESS(2 + len + 1 <= FatPathSize, fs::ResultTooLongPath());

            dst[0] = static_cast<char>('0' + drive);
            dst[1] = ':';
            std::memcpy(dst + 2, src, len + 1);
            KUMO_SUCCEED();
        }

        /* Look up an entry. The root directory has no entry of its own, so it is synthesized. */
        Result GetEntryInfo(FILINFO *out, const char *fat_path, int drive) {
            if (IsRootPath(fat_path)) {
                std::memset(out, 0, sizeof(*out));
                out->fattrib = AM_DIR;
                KUMO_SUCCEED();
            }
            KUMO_RETURN(ConvertResult(f_stat(fat_path, out), drive));
        }

        /* Look up an entry that must be a file (or must be a directory). */
        Result CheckEntryType(const char *fat_path, int drive, bool directory) {
            FILINFO info;
            KUMO_TRY(GetEntryInfo(std::addressof(info), fat_path, drive));
            KUMO_UNLESS(((info.fattrib & AM_DIR) != 0) == directory, fs::ResultPathNotFound());
            KUMO_SUCCEED();
        }

        /* Delete everything inside a directory, leaving the directory itself. */
        /* Iterative, so the depth of the tree does not cost stack: `path` grows and shrinks in place. */
        Result CleanDirectory(char *path, int drive) {
            const size_t base_length = std::strlen(path);

            while (true) {
                /* Look at the first entry of the directory `path` currently names. */
                FILINFO info;
                {
                    DIR dir;
                    KUMO_TRY(ConvertResult(f_opendir(std::addressof(dir), path), drive));
                    const FRESULT fr = f_readdir(std::addressof(dir), std::addressof(info));
                    f_closedir(std::addressof(dir));
                    KUMO_TRY(ConvertResult(fr, drive));
                }

                const size_t length = std::strlen(path);

                if (info.fname[0] == '\0') {
                    /* This directory is empty. If it is the one we were asked to clean, we are done. */
                    if (length == base_length) {
                        KUMO_SUCCEED();
                    }

                    /* Otherwise remove it and go back to its parent. */
                    KUMO_TRY(ConvertResult(f_unlink(path), drive));
                    *std::strrchr(path, '/') = '\0';
                    if (std::strlen(path) == 2) {
                        /* The parent is the root, whose path keeps its slash. */
                        path[2] = '/';
                        path[3] = '\0';
                    }
                    continue;
                }

                /* Name the entry. */
                const bool at_root     = IsRootPath(path);
                const size_t name_len  = std::strlen(info.fname);
                const size_t separator = at_root ? 0 : 1;
                KUMO_UNLESS(length + separator + name_len + 1 <= FatPathSize, fs::ResultTooLongPath());
                if (!at_root) {
                    path[length] = '/';
                }
                std::memcpy(path + length + separator, info.fname, name_len + 1);

                if ((info.fattrib & AM_DIR) != 0) {
                    /* Descend: the next pass works on this subdirectory. */
                    continue;
                }

                /* Delete the file and restore the directory's path. */
                KUMO_TRY(ConvertResult(f_unlink(path), drive));
                path[length] = '\0';
            }
        }

        /* Seconds since 1970-01-01 for a FAT date and time, read as UTC. */
        s64 ConvertFatTimeStamp(WORD fdate, WORD ftime) {
            const s64 year  = 1980 + ((fdate >> 9) & 0x7F);
            const s64 month = std::clamp<s64>((fdate >> 5) & 0xF, 1, 12);
            const s64 day   = std::clamp<s64>(fdate & 0x1F, 1, 31);

            /* Days from the civil calendar (Howard Hinnant's algorithm). */
            const s64 y   = year - (month <= 2 ? 1 : 0);
            const s64 era = (y >= 0 ? y : y - 399) / 400;
            const s64 yoe = y - era * 400;
            const s64 doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
            const s64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
            const s64 days = era * 146097 + doe - 719468;

            return days * 86400 + ((ftime >> 11) & 0x1F) * 3600 + ((ftime >> 5) & 0x3F) * 60 + (ftime & 0x1F) * 2;
        }

        class FatFile : public fs::fsa::IFile {
            KUMO_NON_COPYABLE(FatFile);
            KUMO_NON_MOVEABLE(FatFile);
            private:
                int m_drive;
                fs::OpenMode m_mode;
                bool m_open;
                FIL m_file;
            public:
                FatFile(int drive, fs::OpenMode mode) : m_drive(drive), m_mode(mode), m_open(false), m_file() { /* ... */ }

                virtual ~FatFile() override {
                    std::scoped_lock lock(g_fat_access);
                    if (m_open) {
                        f_close(std::addressof(m_file));
                    }
                }

                Result Open(const char *fat_path) {
                    std::scoped_lock lock(g_fat_access);
                    BYTE flags = FA_OPEN_EXISTING;
                    if ((m_mode & fs::OpenMode_Read)  != 0) { flags |= FA_READ;  }
                    if ((m_mode & fs::OpenMode_Write) != 0) { flags |= FA_WRITE; }

                    KUMO_TRY(ConvertResult(f_open(std::addressof(m_file), fat_path, flags), m_drive));
                    m_open = true;
                    KUMO_SUCCEED();
                }
            private:
                virtual Result DoRead(size_t *out, s64 offset, void *buffer, size_t size, const fs::ReadOption &option) override {
                    std::scoped_lock lock(g_fat_access);

                    /* Clamp the read to the end of the file. */
                    size_t read_size = 0;
                    KUMO_TRY(this->DryRead(std::addressof(read_size), offset, size, option, m_mode));

                    KUMO_TRY(ConvertResult(f_lseek(std::addressof(m_file), static_cast<FSIZE_t>(offset)), m_drive));

                    u8 *dst = static_cast<u8 *>(buffer);
                    size_t total = 0;
                    while (total < read_size) {
                        const UINT cur = static_cast<UINT>(std::min(read_size - total, MaxTransferSize));
                        UINT done = 0;
                        KUMO_TRY(ConvertResult(f_read(std::addressof(m_file), dst + total, cur, std::addressof(done)), m_drive));
                        total += done;
                        if (done < cur) {
                            break;
                        }
                    }

                    *out = total;
                    KUMO_SUCCEED();
                }

                virtual Result DoGetSize(s64 *out) override {
                    std::scoped_lock lock(g_fat_access);
                    KUMO_UNLESS(HasValidHandle(m_file.obj), fs::ResultUnexpected());
                    *out = static_cast<s64>(f_size(std::addressof(m_file)));
                    KUMO_SUCCEED();
                }

                virtual Result DoFlush() override {
                    std::scoped_lock lock(g_fat_access);

                    KUMO_SUCCEED_IF((m_mode & fs::OpenMode_Write) == 0);
                    KUMO_RETURN(ConvertResult(f_sync(std::addressof(m_file)), m_drive));
                }

                virtual Result DoWrite(s64 offset, const void *buffer, size_t size, const fs::WriteOption &option) override {
                    std::scoped_lock lock(g_fat_access);

                    /* Check that we may write, and that we may grow the file if this write would. */
                    bool append = false;
                    KUMO_TRY(this->DryWrite(std::addressof(append), offset, size, option, m_mode));

                    /* Seeking past the end extends the file, so a short seek means the volume is full. */
                    KUMO_TRY(ConvertResult(f_lseek(std::addressof(m_file), static_cast<FSIZE_t>(offset)), m_drive));
                    KUMO_UNLESS(f_tell(std::addressof(m_file)) == static_cast<FSIZE_t>(offset), fs::ResultNotEnoughFreeSpace());

                    const u8 *src = static_cast<const u8 *>(buffer);
                    size_t total = 0;
                    while (total < size) {
                        const UINT cur = static_cast<UINT>(std::min(size - total, MaxTransferSize));
                        UINT done = 0;
                        KUMO_TRY(ConvertResult(f_write(std::addressof(m_file), src + total, cur, std::addressof(done)), m_drive));
                        KUMO_UNLESS(done == cur, fs::ResultNotEnoughFreeSpace());
                        total += done;
                    }

                    if (fs::HasFlushFlag(option)) {
                        KUMO_TRY(ConvertResult(f_sync(std::addressof(m_file)), m_drive));
                    }
                    KUMO_SUCCEED();
                }

                virtual Result DoSetSize(s64 size) override {
                    std::scoped_lock lock(g_fat_access);

                    KUMO_TRY(this->DrySetSize(size, m_mode));

                    /* Seeking past the end grows the file; truncating at the new position shrinks it. */
                    KUMO_TRY(ConvertResult(f_lseek(std::addressof(m_file), static_cast<FSIZE_t>(size)), m_drive));
                    KUMO_UNLESS(f_tell(std::addressof(m_file)) == static_cast<FSIZE_t>(size), fs::ResultNotEnoughFreeSpace());
                    KUMO_RETURN(ConvertResult(f_truncate(std::addressof(m_file)), m_drive));
                }

                virtual Result DoOperateRange(void *dst, size_t dst_size, fs::OperationId op_id, s64 offset, s64 size, const void *src, size_t src_size) override {
                    (void)offset; (void)size; (void)src; (void)src_size;

                    switch (op_id) {
                        case fs::OperationId::Invalidate:
                            KUMO_SUCCEED();
                        case fs::OperationId::QueryRange:
                            KUMO_UNLESS(dst != nullptr,                         fs::ResultNullptrArgument());
                            KUMO_UNLESS(dst_size == sizeof(fs::QueryRangeInfo), fs::ResultInvalidSize());
                            std::memset(dst, 0, sizeof(fs::QueryRangeInfo));
                            KUMO_SUCCEED();
                        default:
                            KUMO_THROW(fs::ResultUnsupportedOperation());
                    }
                }
        };

        class FatDirectory : public fs::fsa::IDirectory {
            KUMO_NON_COPYABLE(FatDirectory);
            KUMO_NON_MOVEABLE(FatDirectory);
            private:
                int m_drive;
                fs::OpenDirectoryMode m_mode;
                bool m_open;
                DIR m_dir;
                char m_path[FatPathSize];
            public:
                FatDirectory(int drive, fs::OpenDirectoryMode mode) : m_drive(drive), m_mode(mode), m_open(false), m_dir(), m_path() { /* ... */ }

                virtual ~FatDirectory() override {
                    std::scoped_lock lock(g_fat_access);
                    if (m_open) {
                        f_closedir(std::addressof(m_dir));
                    }
                }

                Result Open(const char *fat_path) {
                    std::scoped_lock lock(g_fat_access);
                    KUMO_TRY(ConvertResult(f_opendir(std::addressof(m_dir), fat_path), m_drive));
                    m_open = true;
                    strlcpy(m_path, fat_path, sizeof(m_path));
                    KUMO_SUCCEED();
                }
            private:
                bool IsWanted(const FILINFO &info) const {
                    return (info.fattrib & AM_DIR) != 0 ? (m_mode & fs::OpenDirectoryMode_Directory) != 0
                                                        : (m_mode & fs::OpenDirectoryMode_File)      != 0;
                }

                virtual Result DoRead(s64 *out_count, fs::DirectoryEntry *out_entries, s64 max_entries) override {
                    std::scoped_lock lock(g_fat_access);
                    s64 count = 0;
                    while (count < max_entries) {
                        FILINFO info;
                        KUMO_TRY(ConvertResult(f_readdir(std::addressof(m_dir), std::addressof(info)), m_drive));
                        if (info.fname[0] == '\0') {
                            break;
                        }
                        if (!this->IsWanted(info)) {
                            continue;
                        }

                        const bool is_directory = (info.fattrib & AM_DIR) != 0;

                        fs::DirectoryEntry &entry = out_entries[count++];
                        std::memset(std::addressof(entry), 0, sizeof(entry));
                        strlcpy(entry.name, info.fname, sizeof(entry.name));
                        entry.type = is_directory ? fs::DirectoryEntryType_Directory : fs::DirectoryEntryType_File;
                        if (!is_directory && (m_mode & fs::OpenDirectoryMode_NotRequireFileSize) == 0) {
                            entry.file_size = static_cast<s64>(info.fsize);
                        }
                    }

                    *out_count = count;
                    KUMO_SUCCEED();
                }

                virtual Result DoGetEntryCount(s64 *out) override {
                    std::scoped_lock lock(g_fat_access);
                    KUMO_UNLESS(HasValidHandle(m_dir.obj), fs::ResultUnexpected());
                    /* Count with a second handle so the caller's read position is not disturbed. */
                    DIR dir;
                    KUMO_TRY(ConvertResult(f_opendir(std::addressof(dir), m_path), m_drive));
                    const kumo::ScopeExit close_dir([&] { f_closedir(std::addressof(dir)); });

                    s64 count = 0;
                    while (true) {
                        FILINFO info;
                        KUMO_TRY(ConvertResult(f_readdir(std::addressof(dir), std::addressof(info)), m_drive));
                        if (info.fname[0] == '\0') {
                            break;
                        }
                        if (this->IsWanted(info)) {
                            ++count;
                        }
                    }

                    *out = count;
                    KUMO_SUCCEED();
                }
        };

    }

    FatFileSystem::FatFileSystem(int drive) : m_drive(drive), m_mounted(false), m_fatfs() {
        KUMO_ASSERT(0 <= drive && drive < MaxDrives);
    }

    FatFileSystem::~FatFileSystem() {
        this->Unmount();
    }

    Result FatFileSystem::Mount() {
        std::scoped_lock lock(g_fat_access);
        const char path[] = { static_cast<char>('0' + m_drive), ':', '\0' };

        /* Mount immediately, so a missing or damaged filesystem is reported here. */
        const Result rc = ConvertResult(f_mount(std::addressof(m_fatfs), path, 1), m_drive);
        if (R_FAILED(rc)) {
            // Even a failed immediate mount registers the FATFS pointer.
            f_unmount(path);
            return rc;
        }
        m_mounted = true;
        KUMO_SUCCEED();
    }

    void FatFileSystem::Unmount() {
        std::scoped_lock lock(g_fat_access);
        if (m_mounted) {
            const char path[] = { static_cast<char>('0' + m_drive), ':', '\0' };
            f_unmount(path);
            m_mounted = false;
        }
    }

    Result FatFileSystem::DoCreateFile(const fs::Path &path, s64 size, int flags) {
        std::scoped_lock lock(g_fat_access);
        KUMO_UNLESS(m_mounted, fs::ResultPartitionNotFound());
        (void)flags;

        char fat_path[FatPathSize];
        KUMO_TRY(MakeFatPath(fat_path, m_drive, path));

        /* Create the file. FatFs refuses with FR_DENIED when the directory or volume is full. */
        FIL file;
        const FRESULT fr = f_open(std::addressof(file), fat_path, FA_CREATE_NEW | FA_WRITE);
        KUMO_UNLESS(fr != FR_DENIED, fs::ResultNotEnoughFreeSpace());
        KUMO_TRY(ConvertResult(fr, m_drive));

        /* Give it its initial size. */
        Result rc = 0;
        if (size > 0) {
            rc = ConvertResult(f_lseek(std::addressof(file), static_cast<FSIZE_t>(size)), m_drive);
            if (R_SUCCEEDED(rc) && f_tell(std::addressof(file)) != static_cast<FSIZE_t>(size)) {
                rc = fs::ResultNotEnoughFreeSpace();
            }
        }

        const Result close_rc = ConvertResult(f_close(std::addressof(file)), m_drive);
        if (R_SUCCEEDED(rc)) {
            rc = close_rc;
        }

        /* Do not leave a partly-sized file behind. */
        if (R_FAILED(rc)) {
            f_unlink(fat_path);
        }
        KUMO_RETURN(rc);
    }

    Result FatFileSystem::DoDeleteFile(const fs::Path &path) {
        std::scoped_lock lock(g_fat_access);
        KUMO_UNLESS(m_mounted, fs::ResultPartitionNotFound());
        char fat_path[FatPathSize];
        KUMO_TRY(MakeFatPath(fat_path, m_drive, path));
        KUMO_TRY(CheckEntryType(fat_path, m_drive, false));

        KUMO_RETURN(ConvertResult(f_unlink(fat_path), m_drive));
    }

    Result FatFileSystem::DoCreateDirectory(const fs::Path &path) {
        std::scoped_lock lock(g_fat_access);
        KUMO_UNLESS(m_mounted, fs::ResultPartitionNotFound());
        char fat_path[FatPathSize];
        KUMO_TRY(MakeFatPath(fat_path, m_drive, path));
        KUMO_UNLESS(!IsRootPath(fat_path), fs::ResultPathAlreadyExists());

        const FRESULT fr = f_mkdir(fat_path);
        KUMO_UNLESS(fr != FR_DENIED, fs::ResultNotEnoughFreeSpace());
        KUMO_RETURN(ConvertResult(fr, m_drive));
    }

    Result FatFileSystem::DoDeleteDirectory(const fs::Path &path) {
        std::scoped_lock lock(g_fat_access);
        KUMO_UNLESS(m_mounted, fs::ResultPartitionNotFound());
        char fat_path[FatPathSize];
        KUMO_TRY(MakeFatPath(fat_path, m_drive, path));
        KUMO_UNLESS(!IsRootPath(fat_path), fs::ResultDirectoryNotEmpty());
        KUMO_TRY(CheckEntryType(fat_path, m_drive, true));

        /* FatFs refuses to remove a directory that still has entries. */
        const FRESULT fr = f_unlink(fat_path);
        KUMO_UNLESS(fr != FR_DENIED, fs::ResultDirectoryNotEmpty());
        KUMO_RETURN(ConvertResult(fr, m_drive));
    }

    Result FatFileSystem::DoDeleteDirectoryRecursively(const fs::Path &path) {
        std::scoped_lock lock(g_fat_access);
        KUMO_UNLESS(m_mounted, fs::ResultPartitionNotFound());
        char fat_path[FatPathSize];
        KUMO_TRY(MakeFatPath(fat_path, m_drive, path));
        KUMO_UNLESS(!IsRootPath(fat_path), fs::ResultDirectoryNotEmpty());
        KUMO_TRY(CheckEntryType(fat_path, m_drive, true));

        KUMO_TRY(CleanDirectory(fat_path, m_drive));
        KUMO_RETURN(ConvertResult(f_unlink(fat_path), m_drive));
    }

    Result FatFileSystem::DoCleanDirectoryRecursively(const fs::Path &path) {
        std::scoped_lock lock(g_fat_access);
        KUMO_UNLESS(m_mounted, fs::ResultPartitionNotFound());
        char fat_path[FatPathSize];
        KUMO_TRY(MakeFatPath(fat_path, m_drive, path));
        KUMO_TRY(CheckEntryType(fat_path, m_drive, true));

        KUMO_RETURN(CleanDirectory(fat_path, m_drive));
    }

    Result FatFileSystem::Rename(const fs::Path &old_path, const fs::Path &new_path, bool directory) {
        char old_fat_path[FatPathSize];
        char new_fat_path[FatPathSize];
        KUMO_TRY(MakeFatPath(old_fat_path, m_drive, old_path));
        KUMO_TRY(MakeFatPath(new_fat_path, m_drive, new_path));
        KUMO_UNLESS(!IsRootPath(old_fat_path), fs::ResultPathNotFound());
        KUMO_UNLESS(!IsRootPath(new_fat_path), fs::ResultPathAlreadyExists());
        KUMO_TRY(CheckEntryType(old_fat_path, m_drive, directory));

        KUMO_RETURN(ConvertResult(f_rename(old_fat_path, new_fat_path), m_drive));
    }

    Result FatFileSystem::DoRenameFile(const fs::Path &old_path, const fs::Path &new_path) {
        std::scoped_lock lock(g_fat_access);
        KUMO_UNLESS(m_mounted, fs::ResultPartitionNotFound());
        KUMO_RETURN(this->Rename(old_path, new_path, false));
    }

    Result FatFileSystem::DoRenameDirectory(const fs::Path &old_path, const fs::Path &new_path) {
        std::scoped_lock lock(g_fat_access);
        KUMO_UNLESS(m_mounted, fs::ResultPartitionNotFound());
        KUMO_RETURN(this->Rename(old_path, new_path, true));
    }

    Result FatFileSystem::DoGetEntryType(fs::DirectoryEntryType *out, const fs::Path &path) {
        std::scoped_lock lock(g_fat_access);
        KUMO_UNLESS(m_mounted, fs::ResultPartitionNotFound());
        char fat_path[FatPathSize];
        KUMO_TRY(MakeFatPath(fat_path, m_drive, path));

        FILINFO info;
        KUMO_TRY(GetEntryInfo(std::addressof(info), fat_path, m_drive));

        *out = (info.fattrib & AM_DIR) != 0 ? fs::DirectoryEntryType_Directory : fs::DirectoryEntryType_File;
        KUMO_SUCCEED();
    }

    Result FatFileSystem::DoOpenFile(std::unique_ptr<fs::fsa::IFile> *out_file, const fs::Path &path, fs::OpenMode mode) {
        std::scoped_lock lock(g_fat_access);
        KUMO_UNLESS(m_mounted, fs::ResultPartitionNotFound());
        char fat_path[FatPathSize];
        KUMO_TRY(MakeFatPath(fat_path, m_drive, path));

        /* FatFs reports a directory opened as a file as "no file", which becomes PathNotFound. */
        std::unique_ptr<FatFile> file(new (std::nothrow) FatFile(m_drive, mode));
        KUMO_UNLESS(file != nullptr, fs::ResultAllocationMemoryFailed());
        KUMO_TRY(file->Open(fat_path));

        *out_file = std::move(file);
        KUMO_SUCCEED();
    }

    Result FatFileSystem::DoOpenDirectory(std::unique_ptr<fs::fsa::IDirectory> *out_dir, const fs::Path &path, fs::OpenDirectoryMode mode) {
        std::scoped_lock lock(g_fat_access);
        KUMO_UNLESS(m_mounted, fs::ResultPartitionNotFound());
        char fat_path[FatPathSize];
        KUMO_TRY(MakeFatPath(fat_path, m_drive, path));

        std::unique_ptr<FatDirectory> dir(new (std::nothrow) FatDirectory(m_drive, mode));
        KUMO_UNLESS(dir != nullptr, fs::ResultAllocationMemoryFailed());
        KUMO_TRY(dir->Open(fat_path));

        *out_dir = std::move(dir);
        KUMO_SUCCEED();
    }

    Result FatFileSystem::DoCommit() {
        std::scoped_lock lock(g_fat_access);
        KUMO_UNLESS(m_mounted, fs::ResultPartitionNotFound());
        /* FAT has no transactions; committing makes sure everything written so far is on the disk. */
        KUMO_RETURN(this->DoFlush());
    }

    Result FatFileSystem::DoFlush() {
        std::scoped_lock lock(g_fat_access);
        KUMO_UNLESS(m_mounted, fs::ResultPartitionNotFound());
        KUMO_UNLESS(disk_ioctl(static_cast<BYTE>(m_drive), CTRL_SYNC, nullptr) == RES_OK, ConvertResult(FR_DISK_ERR, m_drive));
        KUMO_SUCCEED();
    }

    Result FatFileSystem::GetSpace(s64 *out_free, s64 *out_total) {
        const char path[] = { static_cast<char>('0' + m_drive), ':', '\0' };

        DWORD free_clusters = 0;
        FATFS *fatfs = nullptr;
        KUMO_TRY(ConvertResult(f_getfree(path, std::addressof(free_clusters), std::addressof(fatfs)), m_drive));

        const s64 cluster_size = static_cast<s64>(fatfs->csize) * fatfs->ssize;
        *out_free  = static_cast<s64>(free_clusters) * cluster_size;
        *out_total = static_cast<s64>(fatfs->n_fatent - 2) * cluster_size;
        KUMO_SUCCEED();
    }

    Result FatFileSystem::DoGetFreeSpaceSize(s64 *out, const fs::Path &path) {
        std::scoped_lock lock(g_fat_access);
        KUMO_UNLESS(m_mounted, fs::ResultPartitionNotFound());
        (void)path;
        s64 total;
        KUMO_RETURN(this->GetSpace(out, std::addressof(total)));
    }

    Result FatFileSystem::DoGetTotalSpaceSize(s64 *out, const fs::Path &path) {
        std::scoped_lock lock(g_fat_access);
        KUMO_UNLESS(m_mounted, fs::ResultPartitionNotFound());
        (void)path;
        s64 free;
        KUMO_RETURN(this->GetSpace(std::addressof(free), out));
    }

    Result FatFileSystem::DoGetFileTimeStampRaw(fs::FileTimeStampRaw *out, const fs::Path &path) {
        std::scoped_lock lock(g_fat_access);
        KUMO_UNLESS(m_mounted, fs::ResultPartitionNotFound());
        char fat_path[FatPathSize];
        KUMO_TRY(MakeFatPath(fat_path, m_drive, path));

        FILINFO info;
        KUMO_TRY(GetEntryInfo(std::addressof(info), fat_path, m_drive));

        /* Only the modification time is kept (FF_FS_CRTIME is off), so it stands in for all three. */
        const s64 modified = ConvertFatTimeStamp(info.fdate, info.ftime);
        *out = {};
        out->create        = modified;
        out->modify        = modified;
        out->access        = modified;
        out->is_local_time = false;
        KUMO_SUCCEED();
    }

}

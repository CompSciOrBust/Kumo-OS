#include "fs_nca.hpp"
#include <kumo/fs/partition_fs.hpp>
#include <kumo/fs/rom_fs.hpp>

namespace kumo::horizon::hzfs::nca {
namespace {
using namespace kumo::horizon::fs;
using namespace kumo::horizon::fs::fsa;
namespace archive = kumo::fs;
constexpr size_t MetadataLimit = 4 * 1024 * 1024;

class FileStorage final : public IStorage {
    std::shared_ptr<IFileSystem> base_;
    std::unique_ptr<IFile> file_;
    kumo::Mutex mutex_;

  public:
    explicit FileStorage(std::shared_ptr<IFileSystem> base) : base_(std::move(base)) {}
    Result Open(const char *path) {
        Path normalized;
        KUMO_TRY(normalized.InitializeWithNormalization(path));
        return base_->OpenFile(&file_, normalized, OpenMode_Read);
    }
    Result Read(u64 offset, std::span<std::byte> output) override {
        KUMO_UNLESS(offset <= INT64_MAX, ResultInvalidOffset());
        std::scoped_lock lock(mutex_);
        size_t bytes;
        KUMO_TRY(file_->Read(&bytes, offset, output.data(), output.size(), ReadOption::None));
        KUMO_UNLESS(bytes == output.size(), ResultOutOfRange());
        return 0;
    }
    Result GetSize(u64 &out) override {
        std::scoped_lock lock(mutex_);
        s64 size;
        KUMO_TRY(file_->GetSize(&size));
        out = size;
        return 0;
    }
    Result Write(u64 offset, std::span<const std::byte> input) override {
        KUMO_UNLESS(offset <= INT64_MAX, ResultInvalidOffset());
        std::scoped_lock lock(mutex_);
        return file_->Write(offset, input.data(), input.size(), WriteOption::None);
    }
    Result Flush() override {
        std::scoped_lock lock(mutex_);
        return file_->Flush();
    }
    Result QueryRange(s64 offset, s64 size, QueryRangeInfo &out) override {
        std::scoped_lock lock(mutex_);
        return file_->OperateRange(&out, sizeof(out), OperationId::QueryRange, offset, size);
    }
    Result Invalidate(s64 offset, s64 size) override {
        std::scoped_lock lock(mutex_);
        return file_->OperateRange(nullptr, 0, OperationId::Invalidate, offset, size);
    }
};

class ArchiveFile final : public IFile {
    std::shared_ptr<IFileSystem> owner_;
    OpenMode mode_;
    bool rom_;

  public:
    archive::FileView view;
    ArchiveFile(std::shared_ptr<IFileSystem> owner, OpenMode mode, bool rom)
        : owner_(std::move(owner)), mode_(mode), rom_(rom) {}

  private:
    Result DoRead(size_t *out, s64 offset, void *buffer, size_t size, const ReadOption &) override {
        return view.ReadFile(offset, {static_cast<std::byte *>(buffer), size}, *out);
    }
    Result DoWrite(s64 offset, const void *buffer, size_t size, const WriteOption &) override {
        return view.Write(offset, {static_cast<const std::byte *>(buffer), size});
    }
    Result DoGetSize(s64 *out) override {
        u64 size;
        KUMO_TRY(view.GetSize(size));
        *out = size;
        return 0;
    }
    Result DoSetSize(s64 size) override { return view.SetSize(size); }
    Result DoFlush() override { return view.Flush(); }
    Result DoOperateRange(void *out, size_t out_size, OperationId id, s64 offset, s64 size, const void *,
                          size_t) override {
        const Result unsupported = archive::Error(rom_ ? 6368 : 6377);
        if (id == OperationId::Invalidate) {
            KUMO_UNLESS(mode_ & OpenMode_Read, ResultReadNotPermitted());
            KUMO_UNLESS(!(mode_ & OpenMode_Write), unsupported);
            return view.Invalidate(0, INT64_MAX);
        }
        KUMO_UNLESS(id == OperationId::QueryRange, unsupported);
        if (rom_)
            KUMO_UNLESS(offset >= 0, ResultInvalidOffset());
        s64 extent;
        KUMO_TRY(DoGetSize(&extent));
        KUMO_UNLESS(offset >= 0 && offset <= extent, ResultOutOfRange());
        if (rom_ && (size < 0 || size > extent - offset))
            size = extent - offset;
        KUMO_UNLESS(size >= 0 && size <= extent - offset, ResultInvalidSize());
        KUMO_UNLESS(out, ResultNullptrArgument());
        KUMO_UNLESS(out_size == sizeof(QueryRangeInfo), ResultInvalidSize());
        QueryRangeInfo info{};
        KUMO_TRY(view.QueryRange(offset, size, info));
        std::memcpy(out, &info, sizeof(info));
        return 0;
    }
};

template <class Format> class ArchiveDirectory final : public IDirectory {
    std::shared_ptr<IFileSystem> owner_;
    bool sizes_;

  public:
    typename Format::Directory directory;
    ArchiveDirectory(std::shared_ptr<IFileSystem> owner, bool sizes) : owner_(std::move(owner)), sizes_(sizes) {}

  private:
    Result DoRead(s64 *out, DirectoryEntry *entries, s64 capacity) override {
        *out = 0;
        while (*out < capacity) {
            typename Format::Entry entry;
            bool found;
            KUMO_TRY(directory.Next(entry, found));
            if (!found)
                break;
            KUMO_UNLESS(entry.name.size() <= EntryNameLengthMax, ResultTooLongPath());
            auto &dest = entries[(*out)++];
            dest = {};
            std::memcpy(dest.name, entry.name.data(), entry.name.size());
            if constexpr (std::is_same_v<Format, archive::RomFileSystem>) {
                dest.type = static_cast<s8>(entry.type);
            } else {
                dest.type = DirectoryEntryType_File;
            }
            dest.file_size = sizes_ ? entry.size : 0;
        }
        return 0;
    }
    Result DoGetEntryCount(s64 *out) override {
        if constexpr (std::is_same_v<Format, archive::RomFileSystem>) {
            u64 count;
            KUMO_TRY(directory.GetEntryCount(count));
            *out = count;
        } else {
            *out = directory.EntryCount();
        }
        return 0;
    }
};

template <class Format>
class ArchiveFileSystem final : public IFileSystem, public std::enable_shared_from_this<ArchiveFileSystem<Format>> {
    std::shared_ptr<IStorage> storage_;
    Format format_;
    static constexpr bool IsRom = std::is_same_v<Format, archive::RomFileSystem>;

  public:
    explicit ArchiveFileSystem(std::shared_ptr<IStorage> storage) : storage_(std::move(storage)) {}
    Result Open() { return format_.Open(*storage_, MetadataLimit); }

  private:
    Result DoGetEntryType(DirectoryEntryType *out, const Path &path) override {
        archive::EntryType type;
        KUMO_TRY(format_.GetEntryType(path.GetString(), type));
        *out = static_cast<DirectoryEntryType>(type);
        return 0;
    }
    Result DoOpenFile(std::unique_ptr<IFile> *out, const Path &path, OpenMode mode) override {
        auto file = std::unique_ptr<ArchiveFile>(new (std::nothrow) ArchiveFile(this->shared_from_this(), mode, IsRom));
        KUMO_UNLESS(file, ResultAllocationMemoryFailed());
        if constexpr (IsRom) {
            KUMO_UNLESS(mode == OpenMode_Read, ResultInvalidOpenMode());
            KUMO_TRY(format_.OpenFile(path.GetString(), file->view));
        } else {
            KUMO_TRY(format_.OpenFile(path.GetString(), file->view, static_cast<archive::OpenMode>(mode)));
        }
        *out = std::move(file);
        return 0;
    }
    Result DoOpenDirectory(std::unique_ptr<IDirectory> *out, const Path &path, OpenDirectoryMode mode) override {
        auto dir = std::unique_ptr<ArchiveDirectory<Format>>(new (std::nothrow) ArchiveDirectory<Format>(
            this->shared_from_this(), !(mode & OpenDirectoryMode_NotRequireFileSize)));
        KUMO_UNLESS(dir, ResultAllocationMemoryFailed());
        if constexpr (IsRom) {
            KUMO_TRY(format_.OpenDirectory(path.GetString(), dir->directory, mode & OpenDirectoryMode_Directory,
                                           mode & OpenDirectoryMode_File));
        } else {
            KUMO_TRY(format_.OpenDirectory(path.GetString(), dir->directory, mode & OpenDirectoryMode_File));
        }
        *out = std::move(dir);
        return 0;
    }
    Result DoCommit() override { return 0; }
    Result DoGetFreeSpaceSize(s64 *out, const Path &) override {
        if constexpr (!IsRom)
            return ResultNotImplemented();
        *out = 0;
        return 0;
    }
    Result DoGetTotalSpaceSize(s64 *, const Path &) override { return ResultNotImplemented(); }
    Result DoGetFileTimeStampRaw(FileTimeStampRaw *, const Path &) override { return ResultNotImplemented(); }
    static constexpr Result ReadOnly = archive::Error(IsRom ? 6364 : 6374);
    Result DoCreateFile(const Path &, s64, int) override { return ReadOnly; }
    Result DoDeleteFile(const Path &) override { return ReadOnly; }
    Result DoCreateDirectory(const Path &) override { return ReadOnly; }
    Result DoDeleteDirectory(const Path &) override { return ReadOnly; }
    Result DoDeleteDirectoryRecursively(const Path &) override { return ReadOnly; }
    Result DoCleanDirectoryRecursively(const Path &) override { return ReadOnly; }
    Result DoRenameFile(const Path &, const Path &) override { return ReadOnly; }
    Result DoRenameDirectory(const Path &, const Path &) override { return ReadOnly; }
};

template <class Format> Result Mount(std::shared_ptr<IFileSystem> *out, std::shared_ptr<IStorage> storage) {
    auto fs = AllocateShared<ArchiveFileSystem<Format>>(std::move(storage));
    KUMO_UNLESS(fs, ResultAllocationMemoryFailed());
    KUMO_TRY(fs->Open());
    *out = std::move(fs);
    return 0;
}

struct OwnedSection {
    std::shared_ptr<Reader> reader;
    archive::PlaintextNca::Section section;
};

Result OpenOwnedSection(std::shared_ptr<OwnedSection> *out, std::shared_ptr<Reader> reader, s32 index) {
    auto owner = AllocateShared<OwnedSection>();
    KUMO_UNLESS(owner, ResultAllocationMemoryFailedInNcaFileSystemServiceImplB());
    owner->reader = std::move(reader);
    KUMO_TRY(owner->reader->archive.OpenSection(index, {MetadataLimit, MetadataLimit}, owner->section));
    *out = std::move(owner);
    return 0;
}
}

Result OpenReader(std::shared_ptr<Reader> *out, std::shared_ptr<IFileSystem> base, const char *path) {
    auto storage = AllocateShared<FileStorage>(std::move(base));
    auto reader = AllocateShared<Reader>();
    KUMO_UNLESS(storage && reader, ResultAllocationMemoryFailedInNcaFileSystemServiceImplB());
    KUMO_TRY(storage->Open(path));
    reader->storage = std::move(storage);
    KUMO_TRY(reader->archive.Open(*reader->storage));
    *out = std::move(reader);
    return 0;
}

Result OpenRomStorage(std::shared_ptr<IStorage> *out, std::shared_ptr<Reader> reader, s32 index) {
    std::shared_ptr<OwnedSection> owner;
    KUMO_TRY(OpenOwnedSection(&owner, std::move(reader), index));
    KUMO_UNLESS(owner->section.Type() == archive::PlaintextNca::FileSystemType::RomFs, ResultPreconditionViolation());
    *out = std::shared_ptr<IStorage>(owner, &owner->section);
    return 0;
}

Result OpenSectionFileSystem(std::shared_ptr<IFileSystem> *out, std::shared_ptr<Reader> reader, s32 index) {
    std::shared_ptr<OwnedSection> owner;
    KUMO_TRY(OpenOwnedSection(&owner, std::move(reader), index));
    auto storage = std::shared_ptr<IStorage>(owner, &owner->section);
    if (owner->section.Type() == archive::PlaintextNca::FileSystemType::RomFs) {
        return Mount<archive::RomFileSystem>(out, std::move(storage));
    }
    return Mount<archive::PartitionFileSystem>(out, std::move(storage));
}

Result OpenPackageFileSystem(std::shared_ptr<IFileSystem> *out, std::shared_ptr<IFileSystem> base, const char *path) {
    auto storage = AllocateShared<FileStorage>(std::move(base));
    KUMO_UNLESS(storage, ResultAllocationMemoryFailedInNcaFileSystemServiceImplB());
    KUMO_TRY(storage->Open(path));
    return Mount<archive::PartitionFileSystem>(out, std::move(storage));
}
}

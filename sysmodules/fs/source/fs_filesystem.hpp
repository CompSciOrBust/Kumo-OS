#pragma once
#include "fs_types.hpp"

namespace kumo::horizon::fs {
class Path {
    char text_[0x301] = "/";

  public:
    const char *GetString() const { return text_; }
    Result InitializeWithNormalization(std::string_view path);
};

namespace fsa {
class IFile {
  protected:
    virtual Result DoRead(size_t *, s64, void *, size_t, const ReadOption &) = 0;
    virtual Result DoWrite(s64, const void *, size_t, const WriteOption &) = 0;
    virtual Result DoGetSize(s64 *) = 0;
    virtual Result DoSetSize(s64) = 0;
    virtual Result DoFlush() = 0;
    virtual Result DoOperateRange(void *, size_t, OperationId, s64, s64, const void *, size_t) {
        return ResultUnsupportedOperation();
    }
    Result DryRead(size_t *, s64, size_t, const ReadOption &, OpenMode);
    Result DryWrite(bool *, s64, size_t, const WriteOption &, OpenMode);
    Result DrySetSize(s64, OpenMode);

  public:
    virtual ~IFile() = default;
    Result Read(size_t *out, s64 offset, void *buffer, size_t size, const ReadOption &option);
    Result Write(s64 offset, const void *buffer, size_t size, const WriteOption &option);
    Result GetSize(s64 *out) { return DoGetSize(out); }
    Result SetSize(s64 size) { return size < 0 ? ResultInvalidSize() : DoSetSize(size); }
    Result Flush() { return DoFlush(); }
    Result OperateRange(void *out, size_t out_size, OperationId id, s64 offset, s64 size, const void *in = nullptr,
                        size_t in_size = 0) {
        return DoOperateRange(out, out_size, id, offset, size, in, in_size);
    }
};

class IDirectory {
  protected:
    virtual Result DoRead(s64 *, DirectoryEntry *, s64) = 0;
    virtual Result DoGetEntryCount(s64 *) = 0;

  public:
    virtual ~IDirectory() = default;
    Result Read(s64 *out, DirectoryEntry *entries, s64 count) { return DoRead(out, entries, count); }
    Result GetEntryCount(s64 *out) { return DoGetEntryCount(out); }
};

class IFileSystem {
  public:
    virtual ~IFileSystem() = default;
    Result CreateFile(const Path &path, s64 size, int flags) {
        return size < 0 ? ResultInvalidSize() : DoCreateFile(path, size, flags);
    }
    Result OpenFile(std::unique_ptr<IFile> *out, const Path &path, OpenMode mode);
    Result OpenDirectory(std::unique_ptr<IDirectory> *out, const Path &path, OpenDirectoryMode mode);
#define FS_OPERATION(name, parameters, args)                                                                           \
    Result name parameters { return Do##name args; }                                                                   \
                                                                                                                       \
  protected:                                                                                                           \
    virtual Result Do##name parameters { return ResultUnsupportedOperation(); }                                        \
                                                                                                                       \
  public:
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
    FS_OPERATION(DeleteFile, (const Path &path), (path))
    FS_OPERATION(CreateDirectory, (const Path &path), (path))
    FS_OPERATION(DeleteDirectory, (const Path &path), (path))
    FS_OPERATION(DeleteDirectoryRecursively, (const Path &path), (path))
    FS_OPERATION(CleanDirectoryRecursively, (const Path &path), (path))
    FS_OPERATION(RenameFile, (const Path &old_path, const Path &new_path), (old_path, new_path))
    FS_OPERATION(RenameDirectory, (const Path &old_path, const Path &new_path), (old_path, new_path))
    FS_OPERATION(GetEntryType, (DirectoryEntryType * out, const Path &path), (out, path))
    FS_OPERATION(GetFreeSpaceSize, (s64 * out, const Path &path), (out, path))
    FS_OPERATION(GetTotalSpaceSize, (s64 * out, const Path &path), (out, path))
    FS_OPERATION(GetFileTimeStampRaw, (FileTimeStampRaw * out, const Path &path), (out, path))
    FS_OPERATION(Commit, (), ())
    FS_OPERATION(Flush, (), ())
#pragma GCC diagnostic pop
#undef FS_OPERATION
  protected:
    virtual Result DoCreateFile(const Path &, s64, int) { return ResultUnsupportedOperation(); }
    virtual Result DoOpenFile(std::unique_ptr<IFile> *, const Path &, OpenMode) = 0;
    virtual Result DoOpenDirectory(std::unique_ptr<IDirectory> *, const Path &, OpenDirectoryMode) = 0;
};

class SubDirectoryFileSystem : public IFileSystem {
  protected:
    std::shared_ptr<IFileSystem> base_;
    Path root_;
    Result Resolve(Path &out, const Path &path) const;

  public:
    explicit SubDirectoryFileSystem(std::shared_ptr<IFileSystem> base) : base_(std::move(base)) {}
    Result Initialize(const Path &path);

  private:
    Result DoCreateFile(const Path &, s64, int) override;
    Result DoDeleteFile(const Path &) override;
    Result DoCreateDirectory(const Path &) override;
    Result DoDeleteDirectory(const Path &) override;
    Result DoDeleteDirectoryRecursively(const Path &) override;
    Result DoCleanDirectoryRecursively(const Path &) override;
    Result DoRenameFile(const Path &, const Path &) override;
    Result DoRenameDirectory(const Path &, const Path &) override;
    Result DoGetEntryType(DirectoryEntryType *, const Path &) override;
    Result DoOpenFile(std::unique_ptr<IFile> *, const Path &, OpenMode) override;
    Result DoOpenDirectory(std::unique_ptr<IDirectory> *, const Path &, OpenDirectoryMode) override;
    Result DoGetFreeSpaceSize(s64 *, const Path &) override;
    Result DoGetTotalSpaceSize(s64 *, const Path &) override;
    Result DoGetFileTimeStampRaw(FileTimeStampRaw *, const Path &) override;
    Result DoCommit() override { return base_->Commit(); }
    Result DoFlush() override { return base_->Flush(); }
};
}
}

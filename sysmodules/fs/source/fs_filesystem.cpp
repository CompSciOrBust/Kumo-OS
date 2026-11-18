#include "fs_filesystem.hpp"

namespace kumo::horizon::fs {
Result Path::InitializeWithNormalization(std::string_view path) {
    KUMO_UNLESS(path.size() < sizeof(text_), ResultTooLongPath());
    KUMO_UNLESS(!path.empty() && path.front() == '/', ResultInvalidPathFormat());

    // FatFs also interprets backslashes, drive prefixes and trailing dots/spaces.
    char normalized[sizeof(text_)] = "/";
    size_t end = 1;
    for (size_t pos = 1; pos < path.size();) {
        const auto next = path.find('/', pos);
        const auto component = path.substr(pos, next == path.npos ? next : next - pos);
        pos = next == path.npos ? path.size() : next + 1;
        if (component.empty() || component == ".") {
            continue;
        }
        if (component == "..") {
            KUMO_UNLESS(end > 1, ResultInvalidPathFormat());
            while (normalized[--end] != '/') {
            }
            end = std::max<size_t>(1, end);
            continue;
        }
        KUMO_UNLESS(component.back() != '.' && component.back() != ' ', ResultInvalidPathFormat());
        for (unsigned char c : component) {
            KUMO_UNLESS(c >= 0x20 && std::string_view("\\:\"*?<>|").find(c) == path.npos, ResultInvalidPathFormat());
        }
        if (end > 1) {
            normalized[end++] = '/';
        }
        std::memcpy(normalized + end, component.data(), component.size());
        end += component.size();
    }
    normalized[end] = '\0';
    std::memcpy(text_, normalized, end + 1);
    return 0;
}

namespace fsa {
Result IFile::Read(size_t *out, s64 offset, void *buffer, size_t size, const ReadOption &option) {
    *out = 0;
    KUMO_SUCCEED_IF(size == 0);
    KUMO_UNLESS(buffer, ResultNullptrArgument());
    KUMO_UNLESS(offset >= 0 && size <= static_cast<u64>(INT64_MAX - offset), ResultOutOfRange());
    return DoRead(out, offset, buffer, size, option);
}

Result IFile::Write(s64 offset, const void *buffer, size_t size, const WriteOption &option) {
    if (size == 0) {
        return HasFlushFlag(option) ? Flush() : 0;
    }
    KUMO_UNLESS(buffer, ResultNullptrArgument());
    KUMO_UNLESS(offset >= 0 && size <= static_cast<u64>(INT64_MAX - offset), ResultOutOfRange());
    return DoWrite(offset, buffer, size, option);
}

Result IFile::DryRead(size_t *out, s64 offset, size_t size, const ReadOption &, OpenMode mode) {
    KUMO_UNLESS(mode & OpenMode_Read, ResultReadNotPermitted());
    s64 extent;
    KUMO_TRY(DoGetSize(&extent));
    KUMO_UNLESS(offset <= extent, ResultOutOfRange());
    *out = std::min<u64>(size, extent - offset);
    return 0;
}

Result IFile::DryWrite(bool *append, s64 offset, size_t size, const WriteOption &, OpenMode mode) {
    KUMO_TRY(DrySetSize(0, mode));
    s64 extent;
    KUMO_TRY(DoGetSize(&extent));
    *append = static_cast<u64>(offset) + size > static_cast<u64>(extent);
    KUMO_UNLESS(!*append || (mode & OpenMode_AllowAppend), ResultFileExtensionWithoutOpenModeAllowAppend());
    return 0;
}

Result IFile::DrySetSize(s64, OpenMode mode) { return mode & OpenMode_Write ? 0 : ResultWriteNotPermitted(); }

Result IFileSystem::OpenFile(std::unique_ptr<IFile> *out, const Path &path, OpenMode mode) {
    KUMO_UNLESS((mode & OpenMode_ReadWrite) && !(mode & ~7u), ResultInvalidOpenMode());
    return DoOpenFile(out, path, mode);
}

Result IFileSystem::OpenDirectory(std::unique_ptr<IDirectory> *out, const Path &path, OpenDirectoryMode mode) {
    KUMO_UNLESS((mode & OpenDirectoryMode_All) &&
                    !(mode & ~(OpenDirectoryMode_All | OpenDirectoryMode_NotRequireFileSize)),
                ResultInvalidOpenMode());
    return DoOpenDirectory(out, path, mode);
}

Result SubDirectoryFileSystem::Initialize(const Path &path) {
    DirectoryEntryType type;
    KUMO_TRY(base_->GetEntryType(&type, path));
    KUMO_UNLESS(type == DirectoryEntryType_Directory, ResultPathNotFound());
    root_ = path;
    return 0;
}

Result SubDirectoryFileSystem::Resolve(Path &out, const Path &path) const {
    if (path.GetString()[1] == '\0') {
        out = root_;
        return 0;
    }
    char combined[0x301];
    const char *prefix = root_.GetString()[1] ? root_.GetString() : "";
    const int size = std::snprintf(combined, sizeof(combined), "%s%s", prefix, path.GetString());
    KUMO_UNLESS(size >= 0 && static_cast<size_t>(size) < sizeof(combined), ResultTooLongPath());
    return out.InitializeWithNormalization(combined);
}

#define FORWARD_PATH(name, signature, args)                                                                            \
    Result SubDirectoryFileSystem::Do##name signature {                                                                \
        Path resolved;                                                                                                 \
        KUMO_TRY(Resolve(resolved, path));                                                                             \
        return base_->name args;                                                                                       \
    }
FORWARD_PATH(CreateFile, (const Path &path, s64 size, int flags), (resolved, size, flags))
FORWARD_PATH(DeleteFile, (const Path &path), (resolved))
FORWARD_PATH(CreateDirectory, (const Path &path), (resolved))
FORWARD_PATH(CleanDirectoryRecursively, (const Path &path), (resolved))
FORWARD_PATH(GetEntryType, (DirectoryEntryType * out, const Path &path), (out, resolved))
FORWARD_PATH(OpenFile, (std::unique_ptr<IFile> * out, const Path &path, OpenMode mode), (out, resolved, mode))
FORWARD_PATH(OpenDirectory, (std::unique_ptr<IDirectory> * out, const Path &path, OpenDirectoryMode mode),
             (out, resolved, mode))
FORWARD_PATH(GetFreeSpaceSize, (s64 * out, const Path &path), (out, resolved))
FORWARD_PATH(GetTotalSpaceSize, (s64 * out, const Path &path), (out, resolved))
FORWARD_PATH(GetFileTimeStampRaw, (FileTimeStampRaw * out, const Path &path), (out, resolved))
#undef FORWARD_PATH

#define FORWARD_DELETE(name)                                                                                           \
    Result SubDirectoryFileSystem::Do##name(const Path &path) {                                                        \
        KUMO_UNLESS(path.GetString()[1], ResultDirectoryNotEmpty());                                                   \
        Path resolved;                                                                                                 \
        KUMO_TRY(Resolve(resolved, path));                                                                             \
        return base_->name(resolved);                                                                                  \
    }
FORWARD_DELETE(DeleteDirectory)
FORWARD_DELETE(DeleteDirectoryRecursively)
#undef FORWARD_DELETE

#define FORWARD_RENAME(name)                                                                                           \
    Result SubDirectoryFileSystem::Do##name(const Path &old_path, const Path &new_path) {                              \
        KUMO_UNLESS(old_path.GetString()[1], ResultPathNotFound());                                                    \
        KUMO_UNLESS(new_path.GetString()[1], ResultPathAlreadyExists());                                               \
        Path old_resolved, new_resolved;                                                                               \
        KUMO_TRY(Resolve(old_resolved, old_path));                                                                     \
        KUMO_TRY(Resolve(new_resolved, new_path));                                                                     \
        return base_->name(old_resolved, new_resolved);                                                                \
    }
FORWARD_RENAME(RenameFile)
FORWARD_RENAME(RenameDirectory)
#undef FORWARD_RENAME
}
}

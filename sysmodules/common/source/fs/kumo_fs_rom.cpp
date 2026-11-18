#include <kumo/fs/rom_fs.hpp>
#include "kumo_fs_format.hpp"
#include <algorithm>
#include <bit>

namespace kumo::fs {
namespace {
using detail::LoadLittleEndian;
constexpr u32 End = UINT32_MAX;
constexpr size_t HeaderSize = 0x50;
constexpr size_t DirectoryRecordSize = 0x18;
constexpr size_t FileRecordSize = 0x20;
constexpr Result InvalidMetadata = Error(4000);
constexpr Result InvalidPathFormat = Error(6005);

bool IsComponent(std::string_view name) {
    return !name.empty() && name != "." && name != ".." &&
           name.find('/') == std::string_view::npos && name.find('\0') == std::string_view::npos;
}
}

Result RomFileSystem::Open(Storage &storage, size_t metadata_limit) {
    u64 extent;
    KUMO_TRY(storage.GetSize(extent));
    KUMO_UNLESS(extent >= HeaderSize, InvalidMetadata);
    std::byte header[HeaderSize];
    KUMO_TRY(storage.Read(0, header));
    KUMO_UNLESS(LoadLittleEndian<u64>(header) == HeaderSize, InvalidMetadata);
    const u64 data_offset = LoadLittleEndian<u64>(header + 0x48);
    KUMO_UNLESS(data_offset >= HeaderSize && data_offset <= extent, InvalidMetadata);

    RomFileSystem image;
    u64 data_end = extent;
    const auto load_table = [&](std::unique_ptr<std::byte[]> &output, size_t &bytes, size_t field) -> Result {
        const u64 offset = LoadLittleEndian<u64>(header + field);
        const u64 size = LoadLittleEndian<u64>(header + field + 8);
        KUMO_UNLESS(offset >= data_offset && Contains(extent, offset, size) && size <= UINT32_MAX,
                    InvalidMetadata);
        KUMO_UNLESS(size <= metadata_limit, AllocationFailed);
        metadata_limit -= size;
        if (size != 0) {
            output.reset(new (std::nothrow) std::byte[size]);
            KUMO_UNLESS(output, AllocationFailed);
            KUMO_TRY(storage.Read(offset, {output.get(), static_cast<size_t>(size)}));
            data_end = std::min(data_end, offset);
        }
        bytes = size;
        return 0;
    };
    KUMO_TRY(load_table(image.directories_.buckets, image.directories_.bucket_bytes, 0x08));
    KUMO_TRY(load_table(image.directories_.entries, image.directories_.entry_bytes, 0x18));
    KUMO_TRY(load_table(image.files_.buckets, image.files_.bucket_bytes, 0x28));
    KUMO_TRY(load_table(image.files_.entries, image.files_.entry_bytes, 0x38));
    KUMO_UNLESS(image.directories_.bucket_bytes && image.directories_.bucket_bytes % 4 == 0 &&
                image.files_.bucket_bytes && image.files_.bucket_bytes % 4 == 0, InvalidMetadata);
    Record root;
    KUMO_TRY(image.GetRecord(EntryType::Directory, 0, root));
    KUMO_UNLESS(root.name.empty() && LoadLittleEndian<u32>(root.fields) == 0, InvalidMetadata);
    storage_ = &storage;
    data_offset_ = data_offset;
    data_bytes_ = data_end - data_offset;
    directories_ = std::move(image.directories_);
    files_ = std::move(image.files_);
    return 0;
}

Result RomFileSystem::GetRecord(EntryType type, u32 offset, Record &record) const {
    const bool directory = type == EntryType::Directory;
    const auto &table = directory ? directories_ : files_;
    const size_t fixed_size = directory ? DirectoryRecordSize : FileRecordSize;
    KUMO_UNLESS(offset % 4 == 0 && Contains(table.entry_bytes, offset, fixed_size), InvalidMetadata);
    const auto *fields = table.entries.get() + offset;
    const u32 length = LoadLittleEndian<u32>(fields + fixed_size - 4);
    KUMO_UNLESS(Contains(table.entry_bytes, u64{offset} + fixed_size, length), InvalidMetadata);
    const std::string_view name(reinterpret_cast<const char *>(fields + fixed_size), length);
    KUMO_UNLESS((directory && offset == 0) || IsComponent(name), InvalidMetadata);
    record = {fields, name};
    return 0;
}

Result RomFileSystem::Find(EntryType type, u32 parent, std::string_view name, u32 &offset) const {
    const auto &table = type == EntryType::Directory ? directories_ : files_;
    u32 hash = parent ^ 123456789;
    for (unsigned char c : name) hash = std::rotr(hash, 5) ^ c;
    const size_t bucket = hash % (table.bucket_bytes / 4);
    u32 current = LoadLittleEndian<u32>(table.buckets.get() + bucket * 4);
    const size_t fixed_size = type == EntryType::Directory ? DirectoryRecordSize : FileRecordSize;
    for (size_t remaining = table.entry_bytes / fixed_size; current != End; --remaining) {
        KUMO_UNLESS(remaining, InvalidMetadata);
        Record record;
        KUMO_TRY(GetRecord(type, current, record));
        if (LoadLittleEndian<u32>(record.fields) == parent && record.name == name) {
            offset = current;
            return 0;
        }
        current = LoadLittleEndian<u32>(record.fields + fixed_size - 8);
    }
    return PathNotFound;
}

Result RomFileSystem::ResolveDirectory(std::string_view path, u32 &offset) const {
    KUMO_UNLESS(storage_, PathNotFound);
    KUMO_UNLESS(path.starts_with('/'), InvalidPathFormat);
    path.remove_prefix(1);
    u32 parent = 0;
    while (!path.empty()) {
        const size_t delimiter = path.find('/');
        const auto component = path.substr(0, delimiter);
        KUMO_UNLESS(IsComponent(component), InvalidPathFormat);
        KUMO_TRY(Find(EntryType::Directory, parent, component, parent));
        if (delimiter == std::string_view::npos) break;
        path.remove_prefix(delimiter + 1);
    }
    offset = parent;
    return 0;
}

Result RomFileSystem::FindFile(std::string_view path, Record &record) const {
    const size_t delimiter = path.find_last_of('/');
    KUMO_UNLESS(delimiter != std::string_view::npos, InvalidPathFormat);
    const auto name = path.substr(delimiter + 1);
    KUMO_UNLESS(!name.empty(), PathNotFound);
    KUMO_UNLESS(IsComponent(name), InvalidPathFormat);
    u32 parent;
    KUMO_TRY(ResolveDirectory(path.substr(0, delimiter == 0 ? 1 : delimiter), parent));
    u32 offset;
    KUMO_TRY(Find(EntryType::File, parent, name, offset));
    return GetRecord(EntryType::File, offset, record);
}

Result RomFileSystem::GetFileExtent(const Record &record, u64 &offset, u64 &size) const {
    const u64 relative = LoadLittleEndian<u64>(record.fields + 8);
    const u64 bytes = LoadLittleEndian<u64>(record.fields + 16);
    KUMO_UNLESS(Contains(data_bytes_, relative, bytes), InvalidMetadata);
    offset = data_offset_ + relative;
    size = bytes;
    return 0;
}

Result RomFileSystem::OpenFile(std::string_view path, FileView &file) const {
    Record record;
    KUMO_TRY(FindFile(path, record));
    u64 offset, size;
    KUMO_TRY(GetFileExtent(record, offset, size));
    KUMO_TRY(file.storage_.Open(*storage_, offset, size));
    file.mode_ = static_cast<u32>(OpenMode::Read);
    file.unsupported_write_ = Error(6367);
    return 0;
}

Result RomFileSystem::GetEntryType(std::string_view path, EntryType &type) const {
    u32 offset;
    const Result directory_result = ResolveDirectory(path, offset);
    if (directory_result == 0) {
        type = EntryType::Directory;
        return 0;
    }
    if (directory_result != PathNotFound) return directory_result;
    Record record;
    KUMO_TRY(FindFile(path, record));
    type = EntryType::File;
    return 0;
}

Result RomFileSystem::OpenDirectory(std::string_view path, Directory &directory,
                                   bool include_directories, bool include_files) const {
    u32 offset;
    KUMO_TRY(ResolveDirectory(path, offset));
    Record record;
    KUMO_TRY(GetRecord(EntryType::Directory, offset, record));
    directory.fs_ = this;
    directory.parent_ = offset;
    directory.directory_ = include_directories ? LoadLittleEndian<u32>(record.fields + 8) : End;
    directory.file_ = include_files ? LoadLittleEndian<u32>(record.fields + 12) : End;
    directory.first_directory_ = directory.directory_;
    directory.first_file_ = directory.file_;
    directory.remaining_directories_ = directories_.entry_bytes / DirectoryRecordSize;
    directory.remaining_files_ = files_.entry_bytes / FileRecordSize;
    return 0;
}

Result RomFileSystem::Directory::Next(Entry &entry, bool &found) {
    KUMO_UNLESS(fs_, PathNotFound);
    if (directory_ == End && file_ == End) {
        found = false;
        return 0;
    }
    const bool is_directory = directory_ != End;
    u32 &current = is_directory ? directory_ : file_;
    size_t &remaining = is_directory ? remaining_directories_ : remaining_files_;
    const EntryType type = is_directory ? EntryType::Directory : EntryType::File;
    KUMO_UNLESS(remaining, InvalidMetadata);
    Record record;
    KUMO_TRY(fs_->GetRecord(type, current, record));
    KUMO_UNLESS(LoadLittleEndian<u32>(record.fields) == parent_ && (!is_directory || current != 0),
                InvalidMetadata);
    u64 size = 0;
    if (!is_directory) {
        u64 offset;
        KUMO_TRY(fs_->GetFileExtent(record, offset, size));
    }
    entry = {record.name, type, size};
    current = LoadLittleEndian<u32>(record.fields + 4);
    --remaining;
    found = true;
    return 0;
}

Result RomFileSystem::Directory::GetEntryCount(u64 &count) const {
    KUMO_UNLESS(fs_, PathNotFound);
    auto cursor = *this;
    cursor.directory_ = first_directory_;
    cursor.file_ = first_file_;
    cursor.remaining_directories_ = fs_->directories_.entry_bytes / DirectoryRecordSize;
    cursor.remaining_files_ = fs_->files_.entry_bytes / FileRecordSize;
    u64 total = 0;
    Entry entry;
    bool found;
    do {
        KUMO_TRY(cursor.Next(entry, found));
        if (found) ++total;
    } while (found);
    count = total;
    return 0;
}

} // namespace kumo::fs

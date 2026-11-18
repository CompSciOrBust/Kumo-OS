#pragma once
#include <kumo/fs/storage.hpp>
#include <memory>
#include <string_view>

namespace kumo::fs {

// Read-only RomFS. Open with the authenticated data storage of an NCA section,
// or with a standalone image. The backing storage must outlive open files.
class RomFileSystem {
public:
    RomFileSystem() = default;
    RomFileSystem(const RomFileSystem &) = delete;
    RomFileSystem &operator=(const RomFileSystem &) = delete;

    struct Entry {
        std::string_view name;
        EntryType type;
        u64 size;
    };
    class Directory {
    public:
        Result Next(Entry &entry, bool &found);
        Result GetEntryCount(u64 &count) const;

    private:
        friend class RomFileSystem;
        const RomFileSystem *fs_ = nullptr;
        u32 parent_ = 0;
        u32 directory_ = 0;
        u32 file_ = 0;
        u32 first_directory_ = 0;
        u32 first_file_ = 0;
        size_t remaining_directories_ = 0;
        size_t remaining_files_ = 0;
    };

    Result Open(Storage &storage, size_t metadata_limit);
    Result OpenFile(std::string_view absolute_path, FileView &file) const;
    // The filesystem and its metadata must outlive directory cursors.
    Result OpenDirectory(std::string_view absolute_path, Directory &directory,
                         bool include_directories = true, bool include_files = true) const;
    Result GetEntryType(std::string_view absolute_path, EntryType &type) const;

private:
    struct Table {
        std::unique_ptr<std::byte[]> entries;
        std::unique_ptr<std::byte[]> buckets;
        size_t entry_bytes = 0;
        size_t bucket_bytes = 0;
    };
    struct Record {
        const std::byte *fields;
        std::string_view name;
    };
    Result GetRecord(EntryType type, u32 offset, Record &record) const;
    Result Find(EntryType type, u32 parent, std::string_view name, u32 &offset) const;
    Result ResolveDirectory(std::string_view path, u32 &offset) const;
    Result FindFile(std::string_view path, Record &record) const;
    Result GetFileExtent(const Record &record, u64 &offset, u64 &size) const;

    Storage *storage_ = nullptr;
    Table directories_;
    Table files_;
    u64 data_offset_ = 0;
    u64 data_bytes_ = 0;
};

} // namespace kumo::fs

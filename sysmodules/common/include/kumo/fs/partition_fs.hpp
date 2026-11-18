#pragma once
#include <kumo/fs/storage.hpp>
#include <memory>
#include <string_view>

namespace kumo::fs {

// PFS0 metadata. Names borrowed from the image remain valid until
// re-open or destruction. Files borrow the original storage directly.
class PartitionFileSystem {
public:
    PartitionFileSystem() = default;
    PartitionFileSystem(const PartitionFileSystem &) = delete;
    PartitionFileSystem &operator=(const PartitionFileSystem &) = delete;

    using File = FileView;

    struct Entry {
        std::string_view name;
        u64 size;
    };

    class Directory {
    public:
        Result Next(Entry &entry, bool &found);
        u32 EntryCount() const { return count_; }

    private:
        friend class PartitionFileSystem;
        const PartitionFileSystem *fs_ = nullptr;
        u32 index_ = 0;
        u32 count_ = 0;
    };

    // metadata_limit bounds allocation from an untrusted header.
    Result Open(Storage &storage, size_t metadata_limit);
    u32 EntryCount() const { return count_; }
    Result GetEntry(u32 index, Entry &entry) const;
    Result GetEntryType(std::string_view absolute_path, EntryType &type) const;
    Result OpenFile(std::string_view absolute_path, File &file, OpenMode mode = OpenMode::Read) const;
    Result OpenDirectory(std::string_view absolute_path, Directory &directory, bool include_files = true) const;

private:
    Storage *storage_ = nullptr;
    std::unique_ptr<std::byte[]> metadata_;
    u64 data_offset_ = 0;
    u32 count_ = 0;
};

} // namespace kumo::fs

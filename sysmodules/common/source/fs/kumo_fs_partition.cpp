#include <kumo/fs/partition_fs.hpp>
#include "kumo_fs_format.hpp"
#include <cstring>

namespace kumo::fs {
namespace {
using detail::LoadLittleEndian;
constexpr size_t HeaderSize = 0x10;
constexpr size_t EntrySize = 0x18;
constexpr Result InvalidSignature = Error(4644);
constexpr Result InvalidEntry = Error(4646);
}

Result PartitionFileSystem::Open(Storage &storage, size_t metadata_limit) {
    u64 size;
    KUMO_TRY(storage.GetSize(size));
    KUMO_UNLESS(size >= HeaderSize, InvalidSignature);
    std::byte header[HeaderSize];
    KUMO_TRY(storage.Read(0, header));
    KUMO_UNLESS(std::memcmp(header, "PFS0", 4) == 0, InvalidSignature);

    const u32 count = LoadLittleEndian<u32>(header + 4);
    const u32 names_size = LoadLittleEndian<u32>(header + 8);
    const u64 entries_size = u64{count} * EntrySize;
    const u64 metadata_size = entries_size + names_size;
    KUMO_UNLESS(Contains(size, HeaderSize, metadata_size), InvalidEntry);
    KUMO_UNLESS(metadata_size <= metadata_limit, AllocationFailed);
    auto metadata = std::unique_ptr<std::byte[]>(new (std::nothrow) std::byte[metadata_size]);
    KUMO_UNLESS(metadata, AllocationFailed);
    if (metadata_size != 0)
        KUMO_TRY(storage.Read(HeaderSize, {metadata.get(), static_cast<size_t>(metadata_size)}));

    const u64 data_offset = HeaderSize + metadata_size;
    for (u32 i = 0; i < count; ++i) {
        const auto *entry = metadata.get() + size_t{i} * EntrySize;
        const u32 name_offset = LoadLittleEndian<u32>(entry + 16);
        KUMO_UNLESS(name_offset < names_size, InvalidEntry);
        const auto *name = reinterpret_cast<const char *>(metadata.get() + entries_size + name_offset);
        const auto *terminator = static_cast<const char *>(std::memchr(name, 0, names_size - name_offset));
        KUMO_UNLESS(terminator && terminator > name, InvalidEntry);
        KUMO_UNLESS(Contains(size - data_offset, LoadLittleEndian<u64>(entry),
                             LoadLittleEndian<u64>(entry + 8)), InvalidEntry);
    }

    storage_ = &storage;
    metadata_ = std::move(metadata);
    data_offset_ = data_offset;
    count_ = count;
    return 0;
}

Result PartitionFileSystem::GetEntry(u32 index, Entry &entry) const {
    KUMO_UNLESS(index < count_, OutOfRange);
    const auto *record = metadata_.get() + size_t{index} * EntrySize;
    const auto *names = reinterpret_cast<const char *>(metadata_.get() + size_t{count_} * EntrySize);
    entry = {names + LoadLittleEndian<u32>(record + 16), LoadLittleEndian<u64>(record + 8)};
    return 0;
}

Result PartitionFileSystem::OpenFile(std::string_view path, File &file, OpenMode mode) const {
    const auto bits = static_cast<u32>(mode);
    KUMO_UNLESS((bits & 3) && !(bits & ~7u), Error(6072));
    KUMO_UNLESS(path.starts_with('/'), Error(6005));
    path.remove_prefix(1);
    KUMO_UNLESS(path.find('/') == std::string_view::npos, PathNotFound);
    for (u32 i = 0; i < count_; ++i) {
        Entry entry;
        KUMO_TRY(GetEntry(i, entry));
        if (path == entry.name) {
            const auto *record = metadata_.get() + size_t{i} * EntrySize;
            KUMO_TRY(file.storage_.Open(*storage_, data_offset_ + LoadLittleEndian<u64>(record), entry.size));
            file.mode_ = bits;
            file.unsupported_write_ = Error(6376);
            return 0;
        }
    }
    return PathNotFound;
}

Result PartitionFileSystem::OpenDirectory(std::string_view path, Directory &directory, bool include_files) const {
    KUMO_UNLESS(storage_ && path == "/", PathNotFound);
    directory.fs_ = this;
    directory.index_ = 0;
    directory.count_ = include_files ? count_ : 0;
    return 0;
}

Result PartitionFileSystem::Directory::Next(Entry &entry, bool &found) {
    KUMO_UNLESS(fs_, PathNotFound);
    if (index_ == count_) {
        found = false;
        return 0;
    }
    KUMO_TRY(fs_->GetEntry(index_, entry));
    ++index_;
    found = true;
    return 0;
}

Result PartitionFileSystem::GetEntryType(std::string_view path, EntryType &type) const {
    KUMO_UNLESS(storage_, PathNotFound);
    if (path == "/") {
        type = EntryType::Directory;
    } else {
        File file;
        KUMO_TRY(OpenFile(path, file));
        type = EntryType::File;
    }
    return 0;
}

} // namespace kumo::fs

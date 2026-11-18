#include <kumo/fs/storage.hpp>
#include <algorithm>

namespace kumo::fs {

Result StorageSlice::Open(Storage &storage, u64 offset, u64 size) {
    u64 extent;
    KUMO_TRY(storage.GetSize(extent));
    KUMO_UNLESS(&storage != this && Contains(extent, offset, size), OutOfRange);
    storage_ = &storage;
    offset_ = offset;
    size_ = size;
    return 0;
}

Result StorageSlice::Read(u64 offset, std::span<std::byte> output) {
    KUMO_UNLESS(storage_ && Contains(size_, offset, output.size()), OutOfRange);
    if (output.empty()) return 0;
    return storage_->Read(offset_ + offset, output);
}

Result StorageSlice::GetSize(u64 &size) {
    KUMO_UNLESS(storage_, OutOfRange);
    size = size_;
    return 0;
}

Result StorageSlice::Write(u64 offset, std::span<const std::byte> input) {
    KUMO_UNLESS(storage_ && Contains(size_, offset, input.size()), OutOfRange);
    if (input.empty()) return 0;
    return storage_->Write(offset_ + offset, input);
}

Result StorageSlice::Flush() {
    KUMO_UNLESS(storage_, OutOfRange);
    return storage_->Flush();
}

Result StorageSlice::QueryRange(s64 offset, s64 size, RangeInfo &info) {
    KUMO_UNLESS(storage_, OutOfRange);
    KUMO_UNLESS(offset >= 0, InvalidOffset);
    KUMO_UNLESS(size >= 0, InvalidSize);
    KUMO_UNLESS(Contains(size_, offset, size) && offset_ <= u64{INT64_MAX} - offset, OutOfRange);
    return storage_->QueryRange(offset_ + offset, size, info);
}

Result StorageSlice::Invalidate(s64, s64) {
    KUMO_UNLESS(storage_, OutOfRange);
    // Invalidate the backing cache: hash metadata or shared cache lines can
    // depend on data outside this view.
    return storage_->Invalidate(0, INT64_MAX);
}

Result FileView::Read(u64 offset, std::span<std::byte> output) {
    KUMO_UNLESS(mode_ & static_cast<u32>(OpenMode::Read), Error(6202));
    return storage_.Read(offset, output);
}

Result FileView::Write(u64 offset, std::span<const std::byte> input) {
    KUMO_UNLESS(mode_ & static_cast<u32>(OpenMode::Write), Error(6203));
    u64 size;
    KUMO_TRY(GetSize(size));
    KUMO_UNLESS(offset <= size, OutOfRange);
    if (input.size() > size - offset)
        return (mode_ & static_cast<u32>(OpenMode::AllowAppend)) ? unsupported_write_ : Error(6201);
    return storage_.Write(offset, input);
}

Result FileView::Flush() {
    if (!(mode_ & static_cast<u32>(OpenMode::Write))) return 0;
    return storage_.Flush();
}

Result FileView::SetSize(u64) {
    return (mode_ & static_cast<u32>(OpenMode::Write)) ? unsupported_write_ : Error(6203);
}

Result FileView::ReadFile(u64 offset, std::span<std::byte> output, size_t &read) {
    KUMO_UNLESS(mode_ & static_cast<u32>(OpenMode::Read), Error(6202));
    u64 size;
    KUMO_TRY(GetSize(size));
    KUMO_UNLESS(offset <= size, OutOfRange);
    const size_t count = static_cast<size_t>(std::min<u64>(size - offset, output.size()));
    KUMO_TRY(storage_.Read(offset, output.first(count)));
    read = count;
    return 0;
}

} // namespace kumo::fs

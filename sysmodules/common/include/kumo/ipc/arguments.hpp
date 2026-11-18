#pragma once
#include <switch.h>
#include <cstddef>

namespace kumo::ipc {

template<class T> class Out {
    T *value_;
public:
    explicit Out(T *value) : value_(value) {}
    void SetValue(const T &value) const { *value_ = value; }
};

class InBuffer {
    const u8 *data_;
    size_t size_;
public:
    InBuffer(const void *data, size_t size) : data_(static_cast<const u8 *>(data)), size_(size) {}
    const u8 *GetPointer() const { return data_; }
    size_t GetSize() const { return size_; }
};
class OutBuffer {
    u8 *data_;
    size_t size_;
public:
    OutBuffer(void *data, size_t size) : data_(static_cast<u8 *>(data)), size_(size) {}
    u8 *GetPointer() const { return data_; }
    size_t GetSize() const { return size_; }
};
struct ClientProcessId { u64 value; u64 GetValue() const { return value; } };

/* A copied input handle is owned by its receiver until detached. */
template<class Tag> class OwnedHandle {
    Handle handle_;
public:
    explicit OwnedHandle(Handle handle) : handle_(handle) {}
    OwnedHandle(const OwnedHandle &) = delete;
    OwnedHandle &operator=(const OwnedHandle &) = delete;
    OwnedHandle(OwnedHandle &&other) : handle_(other.handle_) { other.handle_ = INVALID_HANDLE; }
    ~OwnedHandle() { if (handle_ != INVALID_HANDLE) svcCloseHandle(handle_); }
    Handle GetOsHandle() const { return handle_; }
    Handle Release() { Handle result = handle_; handle_ = INVALID_HANDLE; return result; }
    void Detach() { handle_ = INVALID_HANDLE; }
};
// A moved input/output handle has the same local ownership rules.
struct CopyTag;
struct MoveTag;
using CopyHandle = OwnedHandle<CopyTag>;
using MoveHandle = OwnedHandle<MoveTag>;
struct BorrowedHandle { Handle value; };
}

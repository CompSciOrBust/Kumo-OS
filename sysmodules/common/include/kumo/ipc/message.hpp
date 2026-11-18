#pragma once
#include <switch.h>
#include <kumo/ipc/protocol.hpp>
#include <type_traits>
#include <kumo/ipc/object.hpp>

namespace kumo::ipc {
/* A received handle is owned by the request until TakeHandle is called. */
struct ConcurrentRequest {
    CmifInput input{};
    const unsigned char *data = nullptr;
    void *pointer_buffer = nullptr;
    size_t pointer_buffer_size = 0;
    Handle handles[30]{};
    size_t copy_count = 0;
    size_t move_count = 0;
    Handle TakeHandle(size_t index) {
        if (index >= copy_count + move_count) return INVALID_HANDLE;
        Handle result = handles[index];
        handles[index] = INVALID_HANDLE;
        return result;
    }
};

struct ConcurrentReply {
    alignas(8) unsigned char data[128]{};
    size_t size = 0;
    ::Result result = 0;
    Handle copy_handle = INVALID_HANDLE;
    Handle second_copy_handle = INVALID_HANDLE;
    MappedBuffer out_pointer{};
    ObjectRef object;
    Handle move_handle = INVALID_HANDLE;
    template<class T> void Write(const T &value) {
        static_assert(std::is_trivially_copyable_v<T> && sizeof(T) <= 128);
        std::memcpy(data, &value, sizeof(value));
        size = sizeof(value);
    }
};

}

#pragma once
#include "fs_filesystem.hpp"
#include <kumo/ipc/dispatch.hpp>

namespace kumo::horizon::hzfs::ipc {
using namespace kumo::ipc;

inline constexpr size_t PathBytes = 0x301;
inline constexpr CommandOptions PathBuffer{.buffers = BufferMode::FixedPointer, .input_pointer_size = PathBytes};
inline constexpr CommandOptions Mapped{.buffers = BufferMode::Mapped, .normal_map_alias = true};
inline constexpr CommandOptions NonSecure{.buffers = BufferMode::NonSecureMapped};
inline constexpr CommandOptions Pid{.pid = true};

inline Result DecodePath(InBuffer buffer, std::string_view &out) {
    const auto *data = reinterpret_cast<const char *>(buffer.GetPointer());
    const auto *end = static_cast<const char *>(std::memchr(data, 0, buffer.GetSize()));
    KUMO_UNLESS(end, fs::ResultTooLongPath());
    out = {data, static_cast<size_t>(end - data)};
    return 0;
}

inline Result DecodePath(InBuffer buffer, fs::Path &path) {
    std::string_view raw;
    KUMO_TRY(DecodePath(buffer, raw));
    return path.InitializeWithNormalization(raw);
}

template <class T, class... Args> Result ReturnObject(ConcurrentReply &reply, Args &&...args) {
    reply.object = MakeObject<T>(std::forward<Args>(args)...);
    KUMO_UNLESS(reply.object, fs::ResultAllocationMemoryFailedInFileSystemProxyImplA());
    return 0;
}

}

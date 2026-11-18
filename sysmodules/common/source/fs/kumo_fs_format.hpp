#pragma once
#include <kumo/fs/storage.hpp>

namespace kumo::fs::detail {

template<class T> T LoadLittleEndian(const std::byte *bytes) {
    T value = 0;
    for (size_t i = 0; i < sizeof(T); ++i)
        value |= static_cast<T>(std::to_integer<unsigned>(bytes[i])) << (i * 8);
    return value;
}

} // namespace kumo::fs::detail

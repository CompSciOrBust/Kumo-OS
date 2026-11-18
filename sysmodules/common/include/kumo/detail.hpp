#pragma once
#include <kumo/result.hpp>
#include <kumo/assert.hpp>
#include <cstddef>
#include <cstdint>

namespace kumo::detail {
template<class T> constexpr T AlignDown(T value, size_t alignment) { return value & ~static_cast<T>(alignment - 1); }
template<class T> constexpr T AlignUp(T value, size_t alignment) { return AlignDown<T>(value + alignment - 1, alignment); }
template<class T> constexpr bool IsAligned(T value, size_t alignment) { return (value & (alignment - 1)) == 0; }
constexpr bool IsPowerOfTwo(size_t value) { return value && !(value & (value - 1)); }
}

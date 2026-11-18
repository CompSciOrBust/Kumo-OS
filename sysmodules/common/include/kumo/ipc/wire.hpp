#pragma once
#include <type_traits>
#include <cstddef>
#include <bit>
namespace kumo::ipc {
// Records opt in with WireBytes after auditing every field: fixed-width
// integers/arrays/nested wire records only; all padding must be explicit.
// C++ cannot reflect record fields, so this opt-in is a reviewed ABI contract.
template<class T> constexpr bool IsWireType = [] {
    if constexpr (std::is_integral_v<T>) return !std::is_same_v<T, bool> && (sizeof(T)==1 || sizeof(T)==2 || sizeof(T)==4 || sizeof(T)==8);
    else if constexpr (requires { T::WireBytes; })
        return std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T> &&
            std::has_unique_object_representations_v<T> && sizeof(T)==T::WireBytes;
    else return false;
}();
static_assert(std::endian::native == std::endian::little, "CMIF requires little-endian serialization");
}

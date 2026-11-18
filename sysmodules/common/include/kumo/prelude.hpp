#pragma once
#include <kumo/result.hpp>
#include <kumo/assert.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <type_traits>
#include <utility>
#define KUMO_NON_COPYABLE(T) T(const T &) = delete; T &operator=(const T &) = delete
#define KUMO_NON_MOVEABLE(T) T(T &&) = delete; T &operator=(T &&) = delete

#include <kumo/result.hpp>

// Kernel errors must retain Horizon's module/description encoding. Reserved
// upper bits do not affect comparisons, but they do not mean success either.
static_assert(::kumo::IsSuccess(0));
static_assert(::kumo::IsFailure(1));
static_assert(::kumo::KernelError(101) == 0xCA01);
static_assert(::kumo::KernelError(103) == 0xCE01);
static_assert(::kumo::KernelError(127) == 0xFE01);
static_assert(::kumo::IsResult(0xFFC0FE01, ::kumo::KernelError(127)));
static_assert(!::kumo::IsSuccess(0xFFC00000));
static_assert(!::kumo::IsResult(::kumo::KernelError(126), ::kumo::KernelError(127)));

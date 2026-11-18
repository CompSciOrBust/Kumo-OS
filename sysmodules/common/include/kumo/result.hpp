#pragma once
#include <switch.h>

namespace kumo {
constexpr ::Result KernelError(u32 description) { return MAKERESULT(Module_Kernel, description); }
constexpr bool IsSuccess(::Result result) { return result == 0; }
constexpr bool IsFailure(::Result result) { return result != 0; }
constexpr bool IsResult(::Result result, ::Result expected) { return R_VALUE(result) == R_VALUE(expected); }
}
#ifndef KUMO_TRY
#define KUMO_TRY(expr) do { const ::Result hz_result_ = (expr); if (hz_result_ != 0) return hz_result_; } while (0)
#endif
#define KUMO_RETURN(expr) return (expr)
#define KUMO_SUCCEED() return 0
#define KUMO_SUCCEED_IF(condition) do { if (condition) return 0; } while (0)
#define KUMO_THROW(result) return (result)
#ifndef KUMO_UNLESS
#define KUMO_UNLESS(condition, result) do { if (!(condition)) return (result); } while (0)
#endif

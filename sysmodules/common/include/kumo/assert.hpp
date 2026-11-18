#pragma once
#include <switch.h>
#include <cstring>
#include <cstdio>

namespace kumo {
[[noreturn]] inline void Abort(const char *message) {
    if (message) ::svcOutputDebugString(message, std::strlen(message));
    ::svcBreak(BreakReason_Panic, 0, 0);
    for (;;) ::svcSleepThread(1000000000);
}
[[noreturn]] inline void AbortAt(const char *message, const char *file, int line, Result result = 0) {
    char text[512];
    std::snprintf(text, sizeof(text), "%s:%d: %s (result=0x%08x)\n", file, line, message, static_cast<unsigned>(result));
    Abort(text);
}
template<class... T> constexpr void Unused(const T &...) {}
}
#define KUMO_ABORT(message) ::kumo::AbortAt(message, __FILE__, __LINE__)
#define KUMO_ABORT_UNLESS(condition) do { if (!(condition)) ::kumo::AbortAt("Assertion failed: " #condition, __FILE__, __LINE__); } while (0)
#define KUMO_ABORT_RESULT(expr) do { const ::Result hz_result_ = (expr); if (hz_result_ != 0) ::kumo::AbortAt("Operation failed: " #expr, __FILE__, __LINE__, hz_result_); } while (0)
#ifndef KUMO_ASSERT
#define KUMO_ASSERT(condition) KUMO_ABORT_UNLESS(condition)
#endif
#define KUMO_UNUSED(...) ::kumo::Unused(__VA_ARGS__)

/*
 * Debug logging to the kernel debug log (svcOutputDebugString).
 */
#pragma once
#include <kumo/prelude.hpp>

namespace kumo::horizon::hznet {

    void Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

}

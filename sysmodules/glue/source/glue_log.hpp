/*
 * Debug logging to the kernel debug log (svcOutputDebugString).
 */
#pragma once
#include <kumo/prelude.hpp>

namespace kumo::horizon::glue {

    void Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

}

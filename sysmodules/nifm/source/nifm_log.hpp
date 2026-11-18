/*
 * Debug logging to the kernel debug log (svcOutputDebugString).
 */
#pragma once
#include <switch.h>

namespace kumo::horizon::nifm {

    void Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

}

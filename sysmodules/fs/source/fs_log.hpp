/*
 * Debug logging to the kernel debug log (svcOutputDebugString).
 */
#pragma once

namespace kumo::horizon::hzfs {

    void Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

}

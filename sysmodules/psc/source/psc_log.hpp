/*
 * Debug logging to the kernel debug log (svcOutputDebugString).
 */
#pragma once

namespace kumo::horizon::psc::srv {

    void Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

}

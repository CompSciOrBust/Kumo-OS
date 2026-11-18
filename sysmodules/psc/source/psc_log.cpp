#include <switch.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include "psc_log.hpp"

namespace kumo::horizon::psc::srv {

    void Log(const char *fmt, ...) {
        char buf[0x100];
        std::va_list vl;
        va_start(vl, fmt);
        const int len = std::vsnprintf(buf, sizeof(buf), fmt, vl);
        va_end(vl);

        if (len > 0) {
            ::svcOutputDebugString(buf, std::min<size_t>(len, sizeof(buf) - 1));
        }
    }
}

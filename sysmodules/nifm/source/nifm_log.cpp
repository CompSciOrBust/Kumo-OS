#include <kumo/prelude.hpp>
#include "nifm_log.hpp"

namespace kumo::horizon::nifm {

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

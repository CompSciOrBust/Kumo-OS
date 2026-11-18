/*
 * What lwIP needs from the system in NO_SYS mode: a millisecond clock, random
 * numbers, an assertion handler, and the check that its caller holds the
 * stack lock.
 */
#include <kumo/prelude.hpp>
#include "../bsd_log.hpp"
#include "../stack/bsd_stack.hpp"

extern "C" {

    #include "lwip/sys.h"

    u32_t sys_now(void) {
        // Horizon's system tick runs at 19.2 MHz; division avoids overflow.
        return static_cast<u32_t>(::svcGetSystemTick() / 19200);
    }

    unsigned int hz_lwip_rand(void) {
        unsigned int value;
        ::randomGet(std::addressof(value), sizeof(value));
        return value;
    }

    void hz_lwip_assert_failed(const char *message, const char *file, int line) {
        kumo::horizon::hznet::Log("[bsdsockets] lwIP assertion failed: %s (%s:%d)\n", message, file, line);
        KUMO_ABORT("lwIP assertion failed");
    }

    void hz_lwip_assert_core_locked(void) {
        KUMO_ABORT_UNLESS(kumo::horizon::hznet::stack::IsLockedByCurrentThread());
    }

}

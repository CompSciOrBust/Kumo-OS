/*
 * lwIP compiler/platform port for the bsdsockets system module (AArch64, GCC, newlib).
 */
#pragma once

/* lwIP defines ssize_t as int unless the system is seen to have one. newlib has, but only */
/* advertises SSIZE_MAX to POSIX sources. */
#include <limits.h>
#include <sys/types.h>
#ifndef SSIZE_MAX
#define SSIZE_MAX LONG_MAX
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Implemented in bsd_lwip_sys.cpp. */
void hz_lwip_assert_failed(const char *message, const char *file, int line) __attribute__((noreturn));
void hz_lwip_assert_core_locked(void);
unsigned int hz_lwip_rand(void);

#ifdef __cplusplus
}
#endif

/* No diagnostics: LWIP_DEBUG is off, and the module logs what matters itself. */
#define LWIP_PLATFORM_DIAG(x)   do { } while (0)
#define LWIP_PLATFORM_ASSERT(x) hz_lwip_assert_failed((x), __FILE__, __LINE__)

/* Sequence numbers, ports and DHCP transaction IDs. */
#define LWIP_RAND() ((u32_t)hz_lwip_rand())

/* The module supplies errno values on the wire itself; lwIP's are not used (no sockets layer). */
#define LWIP_ERRNO_STDINCLUDE 1

/*
 * The network clock's source: a minimal SNTP client (RFC 4330) over bsd:u,
 * on a thread of its own. See docs/time.md#sntp.
 */
#pragma once
#include <kumo/prelude.hpp>

/*
 * The servers to ask, in order, as IPv4 addresses (there is no DNS service),
 * "a.b.c.d" or "a.b.c.d:port", separated by spaces or commas. The default is
 * Compute Engine's metadata server, which serves NTP, then Google's public NTP
 * server by its anycast address (time1.google.com). Change it at build time:
 *
 *   make -C sysmodules/psc clean
 *   make -C sysmodules/psc nx_release ATMOSPHERE_CHECKED_LIBSTRATOSPHERE=1 KUMO_NTP_SERVERS="10.0.2.2:47123"
 */
#ifndef KUMO_NTP_SERVERS
#define KUMO_NTP_SERVERS "169.254.169.254 216.239.35.0"
#endif

namespace kumo::horizon::timesrv::srv {

    /* Starts the thread: it waits for bsd:u and for an address, asks the servers, and sets the network clock. */
    void StartSntpClient();

}

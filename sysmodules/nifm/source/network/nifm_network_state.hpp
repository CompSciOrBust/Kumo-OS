/*
 * What nifm knows about the network: a snapshot of the wired interface, kept
 * current by a thread that watches the sockets module through bsdcfg.
 *
 * nifm does not own the interface; the sockets module does. nifm brings it up
 * once at start-up and afterwards only reads its state, and every answer nifm
 * gives (the current address, the connection status, a request's state) is
 * derived from this snapshot.
 */
#pragma once
#include <switch.h>

namespace kumo::horizon::nifm::network {

    struct NetworkState {
        bool present;           /* the sockets module is reachable and has a network device */
        bool link_up;
        bool configured;        /* the interface has been brought up and not taken down */
        bool has_address;
        bool dhcp;
        u8 mac[6];
        u16 mtu;
        /* Network byte order: the bytes in the order they are written. All zero while there is no address. */
        u8 address[4];
        u8 netmask[4];
        u8 gateway[4];
        u8 dns[2][4];
    };

    /* Called, from the monitor thread, each time the snapshot changes. */
    using ChangeHandler = void (*)(const NetworkState &state);

    /* Start the thread that connects to bsdcfg, brings the interface up, and watches it. */
    void StartMonitor(ChangeHandler handler);

    /* The latest snapshot. */
    NetworkState GetState();

}

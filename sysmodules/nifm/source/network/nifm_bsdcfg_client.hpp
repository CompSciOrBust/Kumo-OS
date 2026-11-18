/*
 * nifm's client of bsdcfg, the interface-configuration service of the sockets
 * module. The command layouts are this project's own, defined in
 * sysmodules/bsdsockets/source/ipc/bsdcfg_interface.hpp and documented in
 * docs/bsdsockets.md; the structures here must match that file.
 */
#pragma once
#include <switch.h>

namespace kumo::horizon::nifm::network {

    constexpr inline u32 IfStateFlag_LinkUp     = 1u << 0;
    constexpr inline u32 IfStateFlag_Configured = 1u << 1;
    constexpr inline u32 IfStateFlag_HasAddress = 1u << 2;
    constexpr inline u32 IfStateFlag_Dhcp       = 1u << 3;

    struct IfState {
        u32 flags;
        u32 mtu;
        u8 mac[6];
        u8 padding[2];
        u32 address;        /* network byte order, as are the next four */
        u32 netmask;
        u32 gateway;
        u32 dns[2];
        u32 reserved;
    };
    static_assert(sizeof(IfState) == 0x28);

    /* Connect to bsdcfg. Fails until the sockets module has registered it. */
    ::Result ConnectToBsdConfig();

    /* Configure the interface by DHCP and get the event that is signalled whenever any interface changes. */
    ::Result SetIfUpDhcpWithEvent(::Handle *out_event, u32 index);

    ::Result GetIfState(IfState *out, u32 index);

}

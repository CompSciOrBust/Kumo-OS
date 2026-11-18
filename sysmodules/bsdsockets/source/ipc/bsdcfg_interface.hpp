/*
 * The interface of bsdcfg ("nn::bsdsocket::cfg::ServerInterface"): command
 * IDs and argument layouts.
 *
 * The command IDs and names are the official ones
 * (https://switchbrew.org/wiki/Sockets_services). The wiki documents almost
 * no layouts: only that SetIfUp and SetIfUpWithEvent take 0x28 bytes and a
 * type-5 input buffer, and that commands 13 to 15 take a type-5 input buffer
 * and are stubs that return 0. Everything else below, the contents of those
 * 0x28 bytes included, is this project's own. The only client is our nifm,
 * so nothing here has to match what Nintendo's nifm sends; docs/bsdsockets.md
 * lists which layouts are ours.
 *
 */
#pragma once
#include <kumo/prelude.hpp>

namespace kumo::horizon::hznet::ipc {

    /* SetIfUp's and SetIfUpWithEvent's fixed input. */
    struct IfUpInput {
        static constexpr size_t WireBytes = 40;
        u32 index;                  /* the interface: the order the NICs were found in; 0 is the primary one */
        u32 flags;                  /* IfUpFlag_* */
        u32 reserved[8];
    };
    static_assert(sizeof(IfUpInput) == 0x28 && alignof(IfUpInput) == 4);

    constexpr inline u32 IfUpFlag_Dhcp = 1u << 0;    /* configure by DHCP; the buffer is then ignored */

    /* The type-5 buffer of SetIfUp: what an interface without DHCP is given. Network byte order. */
    struct IfStaticConfig {
        u32 address;
        u32 netmask;
        u32 gateway;
        u32 dns[2];
    };
    static_assert(sizeof(IfStaticConfig) == 0x14);

    /* GetIfState's output. */
    struct IfState {
        static constexpr size_t WireBytes = 40;
        u32 flags;                  /* IfStateFlag_* */
        u32 mtu;
        u8 mac[6];
        u8 padding[2];
        /* Network byte order; zero when IfStateFlag_HasAddress is clear. */
        u32 address;
        u32 netmask;
        u32 gateway;
        u32 dns[2];
        u32 reserved;
    };
    static_assert(sizeof(IfState) == 0x28 && alignof(IfState) == 4);

    constexpr inline u32 IfStateFlag_LinkUp     = 1u << 0;   /* the NIC reports a link */
    constexpr inline u32 IfStateFlag_Configured = 1u << 1;   /* SetIfUp has been called and SetIfDown has not */
    constexpr inline u32 IfStateFlag_HasAddress = 1u << 2;   /* an address is assigned (for DHCP: a lease is held) */
    constexpr inline u32 IfStateFlag_Dhcp       = 1u << 3;   /* configured by DHCP */

}


/*
 * The interface of bsd:u and bsd:s ("nn::socket::sf::IClient"): command IDs
 * and argument layouts.
 *
 * Atmosphere has no table for this service, so this one is written from
 * https://switchbrew.org/wiki/Sockets_services and from what libnx sends
 * (nx/source/services/bsd.c). Three things about it are easy to get wrong:
 *
 *   - Every command except 0 and 1 succeeds at the IPC level and returns two
 *     integers, the BSD return value and an errno, followed by any other
 *     output. A failing call is "success, -1, errno".
 *   - Buffers are "auto-select" (type 0x21 in, 0x22 out): the client sends
 *     each as a pointer or a mapping depending on its size.
 *   - The service framework rejects a request whose buffer, handle or
 *     process-ID counts differ from the table, by closing the session. For
 *     the commands whose layout is not documented (see the bottom of the
 *     table) the declaration here is the best available guess: no buffers.
 */
#pragma once
#include <kumo/prelude.hpp>
#include <kumo/ipc/arguments.hpp>

namespace kumo::horizon::hznet::ipc {

    /* "LibraryConfigData": the buffer sizes a client asks for in RegisterClient. */
    struct LibraryConfigData {
        u32 version;
        u32 tcp_tx_buf_size;
        u32 tcp_rx_buf_size;
        u32 tcp_tx_buf_max_size;
        u32 tcp_rx_buf_max_size;
        u32 udp_tx_buf_size;
        u32 udp_rx_buf_size;
        u32 sb_efficiency;
    };
    static_assert(sizeof(LibraryConfigData) == 0x20 && alignof(LibraryConfigData) == 4);

    /* Select's fixed input: nfds and an optional struct timeval. */
    struct SelectInput {
        static constexpr size_t WireBytes = 32;
        bool IsValidWire() const { return timeout_is_null <= 1; }
        s32 nfds;
        s32 padding;
        s64 timeout_sec;
        s64 timeout_usec;
        u8 timeout_is_null;
        u8 padding2[7];
    };
    static_assert(sizeof(SelectInput) == 0x20 && alignof(SelectInput) == 8);
    static_assert(offsetof(SelectInput, timeout_sec) == 8 && offsetof(SelectInput, timeout_is_null) == 24);

    /* RecvMMsg's fixed input. */
    struct RecvMMsgInput {
        s32 fd;
        s32 vlen;
        s32 flags;
        s32 padding;
        s64 timeout_sec;
        s64 timeout_nsec;
    };
    static_assert(sizeof(RecvMMsgInput) == 0x20 && alignof(RecvMMsgInput) == 8);

}

#define HZNET_BSD_OUT kumo::ipc::Out<s32> out_ret, kumo::ipc::Out<s32> out_errno
#define HZNET_BSD_IN  const kumo::ipc::InBuffer &
#define HZNET_BSD_OUTBUF const kumo::ipc::OutBuffer &


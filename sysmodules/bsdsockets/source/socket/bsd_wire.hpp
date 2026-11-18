/*
 * The numbers bsd:u and bsd:s carry on the wire.
 *
 * Horizon's socket stack is FreeBSD's, so socket constants (address
 * families, options, flags, poll events, ioctl numbers) are FreeBSD's; libnx
 * ships FreeBSD headers for that reason. Error numbers are the exception:
 * the service reports Linux errno values, which libnx converts to newlib's.
 * O_NONBLOCK in Fcntl is likewise the Linux value.
 *
 * None of these match lwIP's or newlib's own definitions, so they are spelled
 * out here and never taken from a system header.
 */
#pragma once
#include <kumo/prelude.hpp>

namespace kumo::horizon::hznet::wire {

    /* Linux errno values. */
    enum Errno : s32 {
        Errno_Success         = 0,
        Errno_EPERM           = 1,
        Errno_ENOENT          = 2,
        Errno_EINTR           = 4,
        Errno_EIO             = 5,
        Errno_EBADF           = 9,
        Errno_EAGAIN          = 11,
        Errno_ENOMEM          = 12,
        Errno_EFAULT          = 14,
        Errno_EINVAL          = 22,
        Errno_ENFILE          = 23,
        Errno_EMFILE          = 24,
        Errno_ENOTTY          = 25,
        Errno_EPIPE           = 32,
        Errno_ENOTSOCK        = 88,
        Errno_EDESTADDRREQ    = 89,
        Errno_EMSGSIZE        = 90,
        Errno_EPROTOTYPE      = 91,
        Errno_ENOPROTOOPT     = 92,
        Errno_EPROTONOSUPPORT = 93,
        Errno_ESOCKTNOSUPPORT = 94,
        Errno_EOPNOTSUPP      = 95,
        Errno_EAFNOSUPPORT    = 97,
        Errno_EADDRINUSE      = 98,
        Errno_EADDRNOTAVAIL   = 99,
        Errno_ENETDOWN        = 100,
        Errno_ENETUNREACH     = 101,
        Errno_ECONNABORTED    = 103,
        Errno_ECONNRESET      = 104,
        Errno_ENOBUFS         = 105,
        Errno_EISCONN         = 106,
        Errno_ENOTCONN        = 107,
        Errno_ETIMEDOUT       = 110,
        Errno_ECONNREFUSED    = 111,
        Errno_EHOSTUNREACH    = 113,
        Errno_EALREADY        = 114,
        Errno_EINPROGRESS     = 115,
        Errno_ECANCELED       = 125,
    };

    /* Address families and socket types. */
    constexpr inline s32 AF_UNSPEC_ = 0;
    constexpr inline s32 AF_INET_   = 2;

    constexpr inline s32 SOCK_STREAM_    = 1;
    constexpr inline s32 SOCK_DGRAM_     = 2;
    constexpr inline s32 SOCK_RAW_       = 3;
    constexpr inline s32 SOCK_SEQPACKET_ = 5;
    constexpr inline s32 SOCK_TypeMask   = 0xF;
    constexpr inline s32 SOCK_CLOEXEC_   = 0x10000000;
    constexpr inline s32 SOCK_NONBLOCK_  = 0x20000000;

    constexpr inline s32 IPPROTO_IP_  = 0;
    constexpr inline s32 IPPROTO_TCP_ = 6;
    constexpr inline s32 IPPROTO_UDP_ = 17;

    /* struct sockaddr_in, FreeBSD layout (a length byte, then a one-byte family). */
    struct SockAddrIn {
        u8  len;
        u8  family;
        u16 port;       /* network byte order */
        u32 address;    /* network byte order */
        u8  zero[8];
    };
    static_assert(sizeof(SockAddrIn) == 16);

    /* The part of a sockaddr_in that must be present for it to be usable. */
    constexpr inline size_t SockAddrInMinSize = 8;

    /* send/recv flags. */
    constexpr inline s32 MSG_PEEK_     = 0x00000002;
    constexpr inline s32 MSG_WAITALL_  = 0x00000040;
    constexpr inline s32 MSG_DONTWAIT_ = 0x00000080;
    constexpr inline s32 MSG_NOSIGNAL_ = 0x00020000;

    constexpr inline s32 SHUT_RD_   = 0;
    constexpr inline s32 SHUT_WR_   = 1;
    constexpr inline s32 SHUT_RDWR_ = 2;

    /* Socket options. */
    constexpr inline s32 SOL_SOCKET_ = 0xFFFF;

    constexpr inline s32 SO_ACCEPTCONN_ = 0x0002;
    constexpr inline s32 SO_REUSEADDR_  = 0x0004;
    constexpr inline s32 SO_KEEPALIVE_  = 0x0008;
    constexpr inline s32 SO_BROADCAST_  = 0x0020;
    constexpr inline s32 SO_LINGER_     = 0x0080;
    constexpr inline s32 SO_REUSEPORT_  = 0x0200;
    constexpr inline s32 SO_SNDBUF_     = 0x1001;
    constexpr inline s32 SO_RCVBUF_     = 0x1002;
    constexpr inline s32 SO_SNDTIMEO_   = 0x1005;
    constexpr inline s32 SO_RCVTIMEO_   = 0x1006;
    constexpr inline s32 SO_ERROR_      = 0x1007;
    constexpr inline s32 SO_TYPE_       = 0x1008;

    constexpr inline s32 TCP_NODELAY_   = 1;
    constexpr inline s32 TCP_MAXSEG_    = 2;
    constexpr inline s32 TCP_KEEPIDLE_  = 256;
    constexpr inline s32 TCP_KEEPINTVL_ = 512;
    constexpr inline s32 TCP_KEEPCNT_   = 1024;

    constexpr inline s32 IP_TOS_ = 3;
    constexpr inline s32 IP_TTL_ = 4;

    struct Linger {
        s32 onoff;
        s32 linger;
    };

    /* struct timeval as newlib on AArch64 defines it. */
    struct TimeVal {
        s64 sec;
        s64 usec;
    };
    static_assert(sizeof(TimeVal) == 16);

    /* fcntl. */
    constexpr inline s32 F_GETFL_    = 3;
    constexpr inline s32 F_SETFL_    = 4;
    constexpr inline s32 O_NONBLOCK_ = 0x800;

    /* ioctl requests: _IOR('f', n, int) and _IOW('f', n, int). */
    constexpr inline u32 FIONREAD_  = 0x4004667F;
    constexpr inline u32 FIONWRITE_ = 0x40046677;
    constexpr inline u32 FIONSPACE_ = 0x40046676;
    constexpr inline u32 FIONBIO_   = 0x8004667E;

    /* poll. */
    struct PollFd {
        s32 fd;
        s16 events;
        s16 revents;
    };
    static_assert(sizeof(PollFd) == 8);

    constexpr inline s16 POLLIN_     = 0x0001;
    constexpr inline s16 POLLPRI_    = 0x0002;
    constexpr inline s16 POLLOUT_    = 0x0004;
    constexpr inline s16 POLLERR_    = 0x0008;
    constexpr inline s16 POLLHUP_    = 0x0010;
    constexpr inline s16 POLLNVAL_   = 0x0020;
    constexpr inline s16 POLLRDNORM_ = 0x0040;
    constexpr inline s16 POLLWRBAND_ = 0x0100;

}

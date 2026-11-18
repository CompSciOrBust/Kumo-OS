/*
 * Test client for the sockets system module. Runs as a KIP, uses libnx's
 * socket API (socketInitialize and the POSIX calls it provides) the way an
 * application would, and reports each check on the kernel debug log.
 *
 * Three groups of checks:
 *   - on the loopback interface, which need nothing outside the guest;
 *   - against sysmodules/tests/net_test/host_server.py through QEMU's user-mode
 *     network, where the host is 10.0.2.2;
 *   - inbound, where that server connects to this KIP through QEMU's
 *     forwarded ports.
 * The last two are skipped, and reported as skipped, when there is no lease
 * or the host server does not answer.
 */
#include <switch.h>
#include <kumo/assert.hpp>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

/* libnx leaves the service header out of switch.h, and it has no C linkage guard of its own. */
extern "C" {
    #include <switch/services/bsd.h>
}

namespace {

    namespace {

        /* QEMU's user-mode network: the host, as seen from the guest. */
        constexpr u32 HostAddress = 0x0A000202;     /* 10.0.2.2 */
        constexpr u32 Loopback    = 0x7F000001;     /* 127.0.0.1 */

        /* Ports of host_server.py. */
        constexpr u16 HostPort_UdpEcho   = 47001;
        constexpr u16 HostPort_TcpEcho   = 47002;
        constexpr u16 HostPort_TcpSink   = 47003;
        constexpr u16 HostPort_TcpSource = 47004;
        constexpr u16 HostPort_Control   = 47005;
        constexpr u16 HostPort_Closed    = 47009;

        /* Ports this KIP listens on. QEMU forwards host ports 47010 (TCP) and 47011 (UDP) to them. */
        constexpr u16 GuestPort_TcpIn = 7010;
        constexpr u16 GuestPort_UdpIn = 7011;

        /* Ports used on the loopback interface. */
        constexpr u16 LoopPort_Tcp      = 7020;
        constexpr u16 LoopPort_UdpA     = 7021;
        constexpr u16 LoopPort_UdpB     = 7022;
        constexpr u16 LoopPort_Select   = 7023;
        constexpr u16 LoopPort_Blocked  = 7024;
        constexpr u16 LoopPort_Abandon  = 7025;
        constexpr u16 LoopPort_Self     = 7026;

        /* Linux errno values, which is what the service puts on the wire (bsd* calls report them raw). */
        constexpr int LinuxEPERM      = 1;
        constexpr int LinuxENOPROTOOPT = 92;
        constexpr int LinuxEOPNOTSUPP  = 95;

        constinit int g_failures = 0;
        constinit int g_checks   = 0;
        constinit int g_skipped  = 0;

        void Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

        void Log(const char *fmt, ...) {
            char buf[0x100];
            std::va_list vl;
            va_start(vl, fmt);
            const int len = std::vsnprintf(buf, sizeof(buf), fmt, vl);
            va_end(vl);
            ::svcOutputDebugString(buf, std::min<size_t>(len, sizeof(buf) - 1));
        }

        /* `value` is whatever explains a failure: usually errno, sometimes a count. */
        void Check(const char *what, bool ok, long value = 0) {
            ++g_checks;
            if (!ok) {
                ++g_failures;
            }
            Log("[net_test] %s: %s (%ld)\n", ok ? "PASS" : "FAIL", what, value);
        }

        void Skip(const char *what, const char *why) {
            ++g_skipped;
            Log("[net_test] SKIP: %s (%s)\n", what, why);
        }

        void FillPattern(u8 *dst, size_t size, u32 seed, size_t start = 0) {
            for (size_t i = 0; i < size; ++i) {
                const size_t n = start + i;
                dst[i] = static_cast<u8>((n * 31 + seed) ^ (n >> 8));
            }
        }

        bool VerifyPattern(const u8 *src, size_t size, u32 seed, size_t start) {
            for (size_t i = 0; i < size; ++i) {
                const size_t n = start + i;
                if (src[i] != static_cast<u8>((n * 31 + seed) ^ (n >> 8))) {
                    return false;
                }
            }
            return true;
        }

        u32 Adler32(const u8 *data, size_t size) {
            u32 a = 1, b = 0;
            for (size_t i = 0; i < size; ++i) {
                a = (a + data[i]) % 65521;
                b = (b + a) % 65521;
            }
            return (b << 16) | a;
        }

        struct sockaddr_in MakeAddress(u32 address, u16 port) {
            struct sockaddr_in sa = {};
            sa.sin_len         = sizeof(sa);
            sa.sin_family      = AF_INET;
            sa.sin_port        = htons(port);
            sa.sin_addr.s_addr = htonl(address);
            return sa;
        }

        const struct sockaddr *AsSockAddr(const struct sockaddr_in &sa) {
            return reinterpret_cast<const struct sockaddr *>(std::addressof(sa));
        }

        struct sockaddr *AsSockAddr(struct sockaddr_in &sa) {
            return reinterpret_cast<struct sockaddr *>(std::addressof(sa));
        }

        s64 ElapsedMilliSeconds(u64 since) {
            return static_cast<s64>(armTicksToNs(svcGetSystemTick() - since) / 1000000);
        }

        void SetReceiveTimeout(int fd, int milliseconds) {
            struct timeval tv = { milliseconds / 1000, (milliseconds % 1000) * 1000 };
            ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, std::addressof(tv), sizeof(tv));
        }

        /* Read exactly `size` bytes, or fewer if the stream ends or fails first. */
        ssize_t ReceiveAll(int fd, void *buffer, size_t size) {
            size_t total = 0;
            while (total < size) {
                const ssize_t n = ::recv(fd, static_cast<u8 *>(buffer) + total, size - total, 0);
                if (n <= 0) {
                    return total > 0 ? static_cast<ssize_t>(total) : n;
                }
                total += n;
            }
            return total;
        }

        int ConnectTcp(u32 address, u16 port) {
            const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) {
                return -1;
            }
            const auto sa = MakeAddress(address, port);
            if (::connect(fd, AsSockAddr(sa), sizeof(sa)) != 0) {
                const int saved = errno;
                ::close(fd);
                errno = saved;
                return -1;
            }
            return fd;
        }

        constinit u8 g_out[1048576 + 12345] = {};
        constinit u8 g_in[1048576 + 12345]  = {};

        /* ===== Memory libnx asks for ===== */
        /* socketInitialize allocates its transfer memory, and newlib a small record per descriptor. */

        alignas(0x1000) constinit u8 g_malloc_memory[8 * 1024 * 1024];

        /* ===== A second thread, for the calls that must block ===== */

        struct BlockingCall {
            enum Kind { Kind_Recv, Kind_Accept, Kind_Poll, Kind_RawAccept } kind;
            int fd;
            ::Service *service;     /* Kind_RawAccept: the session to issue the Accept command on */
            std::atomic<bool> done;
            ssize_t result;
            int error;
            u8 data[16];
        };

        constexpr size_t HelperStackSize = 32768;
        alignas(0x1000) constinit u8 g_helper_stack[HelperStackSize];
        constinit Thread g_helper_thread;

        void HelperThreadFunction(void *arg) {
            BlockingCall *call = static_cast<BlockingCall *>(arg);
            switch (call->kind) {
                case BlockingCall::Kind_Recv:
                    call->result = ::recv(call->fd, call->data, sizeof(call->data), 0);
                    break;
                case BlockingCall::Kind_Accept:
                    call->result = ::accept(call->fd, nullptr, nullptr);
                    break;
                case BlockingCall::Kind_Poll: {
                    struct pollfd pfd = { call->fd, POLLIN, 0 };
                    call->result = ::poll(std::addressof(pfd), 1, -1);
                    break;
                }
                case BlockingCall::Kind_RawAccept: {
                    /* The Accept command, issued by hand on a session libnx knows nothing about. */
                    struct { int ret; int error; u32 address_size; } out = { -1, -1, 0 };
                    struct sockaddr_in address = {};
                    const Result rc = serviceDispatchInOut(call->service, 12, call->fd, out,
                        .buffer_attrs = { SfBufferAttr_HipcAutoSelect | SfBufferAttr_Out },
                        .buffers      = { { std::addressof(address), sizeof(address) } },
                    );
                    call->result = R_SUCCEEDED(rc) ? out.ret : -2;
                    call->error  = R_SUCCEEDED(rc) ? out.error : static_cast<int>(rc);
                    call->done   = true;
                    return;
                }
            }
            call->error = errno;
            call->done  = true;
        }

        void StartBlockingCall(BlockingCall *call, BlockingCall::Kind kind, int fd) {
            call->kind   = kind;
            call->fd     = fd;
            call->done   = false;
            call->result = 0;
            call->error  = 0;
            KUMO_ABORT_RESULT(threadCreate(&g_helper_thread, HelperThreadFunction, call, g_helper_stack, HelperStackSize, 49, -2));
            KUMO_ABORT_RESULT(threadStart(&g_helper_thread));
        }

        bool FinishBlockingCall(BlockingCall *call, int timeout_ms) {
            const u64 start = svcGetSystemTick();
            while (!call->done && ElapsedMilliSeconds(start) < timeout_ms) {
                svcSleepThread(INT64_C(1000000) * (5));
            }
            const bool done = call->done;
            if (done) {
                KUMO_ABORT_RESULT(threadWaitForExit(&g_helper_thread));
                threadClose(&g_helper_thread);
            }
            /* A call that never returns leaves its thread behind; the checks that follow will say so. */
            return done;
        }

        /* ===== The service itself ===== */

        bool TestService() {
            const Result rc = ::socketInitialize(nullptr);
            Check("socketInitialize (RegisterClient, StartMonitoring, session clones)", R_SUCCEEDED(rc), rc);
            if (R_FAILED(rc)) {
                return false;
            }

            const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            Check("socket(AF_INET, SOCK_STREAM)", fd >= 0, errno);
            Check("close", ::close(fd) == 0, errno);

            /* Commands the module does not implement answer an error; they must not close the session. */
            int name[2] = { 4, 0 };
            size_t old_size = 0;
            int ret = ::bsdSysctl(name, 2, nullptr, std::addressof(old_size), nullptr, 0);
            Check("Sysctl answers EOPNOTSUPP", ret == -1 && R_SUCCEEDED(g_bsdResult) && g_bsdErrno == LinuxEOPNOTSUPP, g_bsdErrno);

            ret = ::bsdOpen("/dev/bpf", 0);
            Check("Open answers EOPNOTSUPP", ret == -1 && R_SUCCEEDED(g_bsdResult) && g_bsdErrno == LinuxEOPNOTSUPP, g_bsdErrno);

            struct { int ret; int error; } out = {};
            Result raw = serviceDispatchOut(::bsdGetServiceSession(), 31, out);
            Check("a command with no implementation (31, EventFd) answers EOPNOTSUPP", R_SUCCEEDED(raw) && out.ret == -1 && out.error == LinuxEOPNOTSUPP, raw);

            out = {};
            raw = serviceDispatchOut(::bsdGetServiceSession(), 201, out);
            Check("the last command in the table (201) answers EOPNOTSUPP", R_SUCCEEDED(raw) && out.ret == -1 && out.error == LinuxEOPNOTSUPP, raw);

            const int raw_fd = ::bsdSocket(AF_INET, SOCK_DGRAM, 0);
            Check("the session still works afterwards", raw_fd >= 0, g_bsdErrno);

            ret = ::bsdDuplicateSocket(raw_fd);
            Check("DuplicateSocket is refused on bsd:u (EPERM)", ret == -1 && g_bsdErrno == LinuxEPERM, g_bsdErrno);
            ::bsdClose(raw_fd);

            // Poll requires an input and an output buffer. A malformed request
            // must close only its own session, not crash the service process.
            ::Service malformed{};
            Result connect = ::smGetService(std::addressof(malformed), "bsd:u");
            Result bad = connect;
            if (R_SUCCEEDED(connect)) {
                const struct { u32 count; s32 timeout; } input{0, 0};
                bad = serviceDispatchInOut(std::addressof(malformed), 6, input, out);
                ::serviceClose(std::addressof(malformed));
            }
            const int after_bad = ::bsdSocket(AF_INET, SOCK_DGRAM, 0);
            Check("malformed poll closes its session and the service survives", R_SUCCEEDED(connect) && R_FAILED(bad) && after_bad >= 0, bad);
            if (after_bad >= 0) ::bsdClose(after_bad);

            return true;
        }

        /* ===== Error cases ===== */

        void TestErrors() {
            int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
            Check("socket(AF_INET6) fails with EAFNOSUPPORT", fd == -1 && errno == EAFNOSUPPORT, errno);

            fd = ::socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
            Check("socket(SOCK_RAW) fails with EPROTONOSUPPORT", fd == -1 && errno == EPROTONOSUPPORT, errno);

            fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_UDP);
            Check("socket(SOCK_STREAM, IPPROTO_UDP) fails with EPROTONOSUPPORT", fd == -1 && errno == EPROTONOSUPPORT, errno);

            int ret = ::bsdClose(100);
            Check("Close of a descriptor that is not open fails with EBADF", ret == -1 && g_bsdErrno == 9, g_bsdErrno);

            ret = ::bsdRecv(-1, g_in, 16, 0);
            Check("Recv on a negative descriptor fails with EBADF", ret == -1 && g_bsdErrno == 9, g_bsdErrno);

            fd = ::socket(AF_INET, SOCK_STREAM, 0);
            ret = ::send(fd, "x", 1, 0);
            Check("send on an unconnected stream socket fails with ENOTCONN", ret == -1 && errno == ENOTCONN, errno);

            ret = ::recv(fd, g_in, 16, 0);
            Check("recv on an unconnected stream socket fails with ENOTCONN", ret == -1 && errno == ENOTCONN, errno);

            struct sockaddr_in peer = {};
            socklen_t peer_len = sizeof(peer);
            ret = ::getpeername(fd, AsSockAddr(peer), std::addressof(peer_len));
            Check("getpeername on an unconnected socket fails with ENOTCONN", ret == -1 && errno == ENOTCONN, errno);

            /* 192.0.2.1 (TEST-NET-1) is nobody's address, and certainly not ours. */
            auto sa = MakeAddress(0xC0000201, 9000);
            ret = ::bind(fd, AsSockAddr(sa), sizeof(sa));
            Check("bind to an address this host does not have fails with EADDRNOTAVAIL", ret == -1 && errno == EADDRNOTAVAIL, errno);

            sa.sin_family = AF_INET6;
            ret = ::bind(fd, AsSockAddr(sa), sizeof(sa));
            Check("bind with the wrong address family fails with EAFNOSUPPORT", ret == -1 && errno == EAFNOSUPPORT, errno);

            /* Asked of the service directly: libnx has no newlib equivalent for this errno and reports 10092. */
            int value = 1;
            const int raw = ::bsdSocket(AF_INET, SOCK_STREAM, 0);
            ret = ::bsdSetSockOpt(raw, SOL_SOCKET, 0x7777, std::addressof(value), sizeof(value));
            Check("setsockopt with an unknown option fails with ENOPROTOOPT", ret == -1 && g_bsdErrno == LinuxENOPROTOOPT, g_bsdErrno);
            ::bsdClose(raw);

            ret = ::shutdown(fd, SHUT_RDWR);
            Check("shutdown of an unconnected stream socket fails with ENOTCONN", ret == -1 && errno == ENOTCONN, errno);
            ::close(fd);

            fd = ::socket(AF_INET, SOCK_DGRAM, 0);
            ret = ::listen(fd, 1);
            Check("listen on a datagram socket fails with EOPNOTSUPP", ret == -1 && errno == EOPNOTSUPP, errno);

            ret = ::send(fd, "x", 1, 0);
            Check("send on an unconnected datagram socket fails with EDESTADDRREQ", ret == -1 && errno == EDESTADDRREQ, errno);

            sa = MakeAddress(Loopback, 9);
            ret = ::sendto(fd, g_out, 70000, 0, AsSockAddr(sa), sizeof(sa));
            Check("sendto of a datagram larger than UDP allows fails with EMSGSIZE", ret == -1 && errno == EMSGSIZE, errno);
            ::close(fd);
        }

        /* ===== Socket options ===== */

        void TestOptions() {
            const int fd = ::socket(AF_INET, SOCK_STREAM, 0);

            int value = 0;
            socklen_t len = sizeof(value);
            int ret = ::getsockopt(fd, SOL_SOCKET, SO_TYPE, std::addressof(value), std::addressof(len));
            Check("getsockopt(SO_TYPE) reports SOCK_STREAM", ret == 0 && value == SOCK_STREAM && len == sizeof(value), errno);

            value = 1;
            ret = ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, std::addressof(value), sizeof(value));
            int read_back = 0;
            len = sizeof(read_back);
            const int ret2 = ::getsockopt(fd, SOL_SOCKET, SO_REUSEADDR, std::addressof(read_back), std::addressof(len));
            Check("SO_REUSEADDR can be set and read back", ret == 0 && ret2 == 0 && read_back == 1, errno);

            value = 1;
            ret = ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, std::addressof(value), sizeof(value));
            read_back = 0;
            len = sizeof(read_back);
            ::getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, std::addressof(read_back), std::addressof(len));
            Check("TCP_NODELAY can be set and read back", ret == 0 && read_back == 1, errno);

            struct timeval tv = { 2, 500000 };
            ret = ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, std::addressof(tv), sizeof(tv));
            struct timeval tv_back = {};
            len = sizeof(tv_back);
            ::getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, std::addressof(tv_back), std::addressof(len));
            Check("SO_RCVTIMEO can be set and read back", ret == 0 && tv_back.tv_sec == 2 && tv_back.tv_usec == 500000, errno);

            const struct timeval too_large = { INT64_MAX, 0 };
            ret = ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, std::addressof(too_large), sizeof(too_large));
            Check("SO_RCVTIMEO rejects a duration that overflows nanoseconds", ret == -1 && errno == EINVAL, errno);

            value = -1;
            len = sizeof(value);
            ret = ::getsockopt(fd, SOL_SOCKET, SO_ERROR, std::addressof(value), std::addressof(len));
            Check("SO_ERROR is zero on a new socket", ret == 0 && value == 0, value);

            int flags = ::fcntl(fd, F_GETFL, 0);
            Check("a new socket is blocking", flags >= 0 && (flags & O_NONBLOCK) == 0, flags);
            ret = ::fcntl(fd, F_SETFL, O_NONBLOCK);
            flags = ::fcntl(fd, F_GETFL, 0);
            Check("fcntl(F_SETFL, O_NONBLOCK) is reported by F_GETFL", ret == 0 && (flags & O_NONBLOCK) != 0, flags);

            ::close(fd);
        }

        /* ===== TCP on the loopback interface ===== */

        void TestLoopbackTcp() {
            const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
            int value = 1;
            ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, std::addressof(value), sizeof(value));

            auto sa = MakeAddress(Loopback, LoopPort_Tcp);
            int ret = ::bind(listener, AsSockAddr(sa), sizeof(sa));
            Check("bind a stream socket to 127.0.0.1", ret == 0, errno);
            ret = ::listen(listener, 4);
            Check("listen", ret == 0, errno);

            struct sockaddr_in name = {};
            socklen_t name_len = sizeof(name);
            ret = ::getsockname(listener, AsSockAddr(name), std::addressof(name_len));
            Check("getsockname reports the bound address", ret == 0 && name_len == sizeof(name) && name.sin_family == AF_INET && ntohs(name.sin_port) == LoopPort_Tcp && ntohl(name.sin_addr.s_addr) == Loopback, errno);

            const int second = ::socket(AF_INET, SOCK_STREAM, 0);
            ret = ::bind(second, AsSockAddr(sa), sizeof(sa));
            Check("binding a second socket to the same port fails with EADDRINUSE", ret == -1 && errno == EADDRINUSE, errno);
            ::close(second);

            /* Nothing is waiting yet. */
            ::fcntl(listener, F_SETFL, O_NONBLOCK);
            ret = ::accept(listener, nullptr, nullptr);
            Check("non-blocking accept with no connection fails with EAGAIN", ret == -1 && errno == EAGAIN, errno);
            ::fcntl(listener, F_SETFL, 0);

            /* A non-blocking connect completes in the background. */
            const int client = ::socket(AF_INET, SOCK_STREAM, 0);
            ::fcntl(client, F_SETFL, O_NONBLOCK);
            ret = ::connect(client, AsSockAddr(sa), sizeof(sa));
            Check("non-blocking connect returns EINPROGRESS", ret == -1 && errno == EINPROGRESS, errno);

            struct pollfd pfd = { client, POLLOUT, 0 };
            ret = ::poll(std::addressof(pfd), 1, 3000);
            Check("poll reports the connecting socket writable once connected", ret == 1 && (pfd.revents & POLLOUT) != 0, pfd.revents);

            value = -1;
            socklen_t len = sizeof(value);
            ::getsockopt(client, SOL_SOCKET, SO_ERROR, std::addressof(value), std::addressof(len));
            Check("SO_ERROR is zero after the connect succeeds", value == 0, value);

            pfd = { listener, POLLIN, 0 };
            ret = ::poll(std::addressof(pfd), 1, 3000);
            Check("poll reports the listening socket readable", ret == 1 && (pfd.revents & POLLIN) != 0, pfd.revents);

            struct sockaddr_in peer = {};
            socklen_t peer_len = sizeof(peer);
            const int server = ::accept(listener, AsSockAddr(peer), std::addressof(peer_len));
            Check("accept returns the connection and the peer's address", server >= 0 && peer_len == sizeof(peer) && ntohl(peer.sin_addr.s_addr) == Loopback && peer.sin_port != 0, errno);

            struct sockaddr_in client_name = {};
            name_len = sizeof(client_name);
            ::getsockname(client, AsSockAddr(client_name), std::addressof(name_len));
            Check("the address accept returned is the client's own", client_name.sin_port == peer.sin_port, ntohs(client_name.sin_port));

            /* read and write, which reach the service as its Read and Write commands. */
            ssize_t n = ::write(client, "abc", 3);
            std::memset(g_in, 0, 16);
            const ssize_t m = ::read(server, g_in, 16);
            Check("write on one end, read on the other", n == 3 && m == 3 && std::memcmp(g_in, "abc", 3) == 0, errno);

            /* The client end is non-blocking: nothing has been sent to it yet. */
            n = ::recv(client, g_in, 16, 0);
            Check("non-blocking recv with no data fails with EAGAIN", n == -1 && errno == EAGAIN, errno);

            n = ::send(server, "defgh", 5, 0);
            pfd = { client, POLLIN, 0 };
            ret = ::poll(std::addressof(pfd), 1, 3000);
            int pending = -1;
            const int ioctl_ret = ::ioctl(client, FIONREAD, std::addressof(pending));
            Check("poll reports data, and FIONREAD its size", n == 5 && ret == 1 && (pfd.revents & POLLIN) != 0 && ioctl_ret == 0 && pending == 5, pending);

            std::memset(g_in, 0, 16);
            n = ::recv(client, g_in, 16, MSG_PEEK);
            const ssize_t n2 = ::recv(client, g_in + 16, 2, 0);
            const ssize_t n3 = ::recv(client, g_in + 18, 16, 0);
            Check("MSG_PEEK leaves the data; partial reads consume it in order", n == 5 && std::memcmp(g_in, "defgh", 5) == 0 && n2 == 2 && n3 == 3 && std::memcmp(g_in + 16, "defgh", 5) == 0, errno);

            /* Half-close: the client stops sending, and can still receive. */
            ret = ::shutdown(client, SHUT_WR);
            n = ::recv(server, g_in, 16, 0);
            Check("after shutdown(SHUT_WR) the peer reads end of stream", ret == 0 && n == 0, errno);

            n = ::send(client, "x", 1, 0);
            Check("send after shutdown(SHUT_WR) fails with EPIPE", n == -1 && errno == EPIPE, errno);

            n = ::send(server, "late", 4, 0);
            pfd = { client, POLLIN, 0 };
            ::poll(std::addressof(pfd), 1, 3000);
            std::memset(g_in, 0, 16);
            const ssize_t late = ::recv(client, g_in, 16, 0);
            Check("the half-closed end still receives", n == 4 && late == 4 && std::memcmp(g_in, "late", 4) == 0, errno);

            /* Full close of the server end. */
            ::close(server);
            pfd = { client, POLLIN, 0 };
            ret = ::poll(std::addressof(pfd), 1, 3000);
            n = ::recv(client, g_in, 16, 0);
            Check("after the peer closes, poll wakes and recv reads end of stream", ret == 1 && n == 0, pfd.revents);

            ::close(client);
            ::close(listener);

            /* The port is free again. */
            const int again = ::socket(AF_INET, SOCK_STREAM, 0);
            ret = ::bind(again, AsSockAddr(sa), sizeof(sa));
            Check("the port can be bound again after close", ret == 0, errno);
            ::close(again);

            /* Nobody listens on this port now, so lwIP answers the SYN with a reset. */
            const int refused = ::socket(AF_INET, SOCK_STREAM, 0);
            ret = ::connect(refused, AsSockAddr(sa), sizeof(sa));
            Check("connect to a closed loopback port fails with ECONNREFUSED", ret == -1 && errno == ECONNREFUSED, errno);
            ::close(refused);
        }

        /* ===== UDP on the loopback interface ===== */

        void TestLoopbackUdp() {
            const int a = ::socket(AF_INET, SOCK_DGRAM, 0);
            const int b = ::socket(AF_INET, SOCK_DGRAM, 0);
            const auto addr_a = MakeAddress(Loopback, LoopPort_UdpA);
            const auto addr_b = MakeAddress(Loopback, LoopPort_UdpB);
            int ret = ::bind(a, AsSockAddr(addr_a), sizeof(addr_a));
            const int ret2 = ::bind(b, AsSockAddr(addr_b), sizeof(addr_b));
            Check("bind two datagram sockets to 127.0.0.1", ret == 0 && ret2 == 0, errno);

            SetReceiveTimeout(a, 3000);
            SetReceiveTimeout(b, 3000);

            ssize_t n = ::sendto(a, "datagram one", 12, 0, AsSockAddr(addr_b), sizeof(addr_b));
            struct sockaddr_in from = {};
            socklen_t from_len = sizeof(from);
            std::memset(g_in, 0, 64);
            ssize_t m = ::recvfrom(b, g_in, 64, 0, AsSockAddr(from), std::addressof(from_len));
            Check("sendto and recvfrom carry a datagram and its source address", n == 12 && m == 12 && std::memcmp(g_in, "datagram one", 12) == 0 && from_len == sizeof(from) && ntohl(from.sin_addr.s_addr) == Loopback && ntohs(from.sin_port) == LoopPort_UdpA, errno);

            /* A read shorter than its buffer must not put data from an earlier call in the rest of it. */
            /* The rest is either untouched or one repeated byte: the kernel fills the unused part of a */
            /* mapped buffer's partial page (with zero, or with a marker byte on a debug kernel). */
            FillPattern(g_out, 64, 3);
            ::sendto(a, g_out, 64, 0, AsSockAddr(addr_b), sizeof(addr_b));
            ::recv(b, g_in, 64, 0);
            ::sendto(a, "tiny", 4, 0, AsSockAddr(addr_b), sizeof(addr_b));
            std::memset(g_in, 0xAA, 64);
            m = ::recv(b, g_in, 64, 0);
            bool clean = m == 4 && std::memcmp(g_in, "tiny", 4) == 0;
            for (size_t i = 5; i < 64 && clean; ++i) {
                clean = g_in[i] == g_in[4];
            }
            Check("a short read leaves no earlier data in the rest of the buffer", clean, g_in[4]);

            /* Datagram boundaries are kept, and a short read discards the rest of its datagram. */
            FillPattern(g_out, 100, 7);
            ::sendto(a, g_out, 100, 0, AsSockAddr(addr_b), sizeof(addr_b));
            ::sendto(a, "second", 6, 0, AsSockAddr(addr_b), sizeof(addr_b));
            n = ::recv(b, g_in, 10, 0);
            std::memset(g_in + 16, 0, 16);
            m = ::recv(b, g_in + 16, 64, 0);
            Check("a short read truncates its datagram and the next read gets the next one", n == 10 && VerifyPattern(g_in, 10, 7, 0) && m == 6 && std::memcmp(g_in + 16, "second", 6) == 0, errno);

            n = ::recv(b, g_in, 16, MSG_DONTWAIT);
            Check("recv with MSG_DONTWAIT and no datagram fails with EAGAIN", n == -1 && errno == EAGAIN, errno);

            /* A zero-length datagram is a datagram. */
            n = ::sendto(a, g_out, 0, 0, AsSockAddr(addr_b), sizeof(addr_b));
            m = ::recv(b, g_in, 16, 0);
            Check("a zero-length datagram arrives as a read of zero bytes", n == 0 && m == 0, errno);

            /* connect on a datagram socket sets the default destination. */
            ret = ::connect(b, AsSockAddr(addr_a), sizeof(addr_a));
            n = ::send(b, "reply", 5, 0);
            std::memset(g_in, 0, 16);
            m = ::recv(a, g_in, 16, 0);
            struct sockaddr_in peer = {};
            socklen_t peer_len = sizeof(peer);
            const int ret3 = ::getpeername(b, AsSockAddr(peer), std::addressof(peer_len));
            Check("a connected datagram socket sends with send and has a peer name", ret == 0 && n == 5 && m == 5 && std::memcmp(g_in, "reply", 5) == 0 && ret3 == 0 && ntohs(peer.sin_port) == LoopPort_UdpA, errno);

            /* A receive timeout. */
            SetReceiveTimeout(a, 300);
            const u64 start = svcGetSystemTick();
            n = ::recv(a, g_in, 16, 0);
            const s64 elapsed = ElapsedMilliSeconds(start);
            Check("a blocking recv gives up after SO_RCVTIMEO with EAGAIN", n == -1 && errno == EAGAIN && 250 <= elapsed && elapsed < 2000, elapsed);

            ::close(a);
            ::close(b);
        }

        /* ===== poll and select ===== */

        void TestPollAndSelect() {
            const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
            const auto sa = MakeAddress(Loopback, LoopPort_Select);
            ::bind(fd, AsSockAddr(sa), sizeof(sa));

            struct pollfd pfd = { fd, POLLIN, 0 };
            int ret = ::poll(std::addressof(pfd), 1, 0);
            Check("poll with a zero timeout returns at once with nothing ready", ret == 0, pfd.revents);

            u64 start = svcGetSystemTick();
            ret = ::poll(std::addressof(pfd), 1, 300);
            s64 elapsed = ElapsedMilliSeconds(start);
            Check("poll waits out its timeout when nothing happens", ret == 0 && 250 <= elapsed && elapsed < 2000, elapsed);

            pfd = { fd, POLLOUT, 0 };
            ret = ::poll(std::addressof(pfd), 1, 1000);
            Check("a datagram socket polls writable", ret == 1 && (pfd.revents & POLLOUT) != 0, pfd.revents);

            /* A descriptor that is not open. poll is given the service's descriptor numbers directly here. */
            struct pollfd bad = { 99, POLLIN, 0 };
            ret = ::bsdPoll(std::addressof(bad), 1, 0);
            Check("poll marks a descriptor that is not open with POLLNVAL", ret == 1 && (bad.revents & POLLNVAL) != 0, bad.revents);

            ::sendto(fd, "x", 1, 0, AsSockAddr(sa), sizeof(sa));
            pfd = { fd, POLLIN, 0 };
            ret = ::poll(std::addressof(pfd), 1, 3000);
            Check("poll reports a datagram socket readable when a datagram arrives", ret == 1 && (pfd.revents & POLLIN) != 0, pfd.revents);

            /* libnx implements select() with poll; this is the same thing through that path. */
            fd_set readable;
            FD_ZERO(std::addressof(readable));
            FD_SET(fd, std::addressof(readable));
            struct timeval tv = { 1, 0 };
            ret = ::select(fd + 1, std::addressof(readable), nullptr, nullptr, std::addressof(tv));
            Check("select reports the socket readable", ret == 1 && FD_ISSET(fd, std::addressof(readable)), errno);
            ::recv(fd, g_in, 16, 0);
            ::close(fd);

            /* The service's own Select command, which libnx's select() does not use. */
            const int raw = ::bsdSocket(AF_INET, SOCK_DGRAM, 0);
            ::bsdBind(raw, AsSockAddr(sa), sizeof(sa));

            FD_ZERO(std::addressof(readable));
            FD_SET(raw, std::addressof(readable));
            tv = { 0, 200000 };
            start = svcGetSystemTick();
            ret = ::bsdSelect(raw + 1, std::addressof(readable), nullptr, nullptr, std::addressof(tv));
            elapsed = ElapsedMilliSeconds(start);
            Check("the Select command times out with nothing ready", ret == 0 && !FD_ISSET(raw, std::addressof(readable)) && 150 <= elapsed && elapsed < 2000, elapsed);

            ::bsdSendTo(raw, "y", 1, 0, AsSockAddr(sa), sizeof(sa));
            fd_set writable;
            FD_ZERO(std::addressof(readable));
            FD_ZERO(std::addressof(writable));
            FD_SET(raw, std::addressof(readable));
            FD_SET(raw, std::addressof(writable));
            tv = { 2, 0 };
            /* The datagram crosses the loopback interface; give it a moment so both conditions hold together. */
            svcSleepThread(INT64_C(1000000) * (50));
            ret = ::bsdSelect(raw + 1, std::addressof(readable), std::addressof(writable), nullptr, std::addressof(tv));
            Check("the Select command reports a socket readable and writable", ret == 2 && FD_ISSET(raw, std::addressof(readable)) && FD_ISSET(raw, std::addressof(writable)), ret);
            ::bsdClose(raw);
        }

        /* ===== Blocking calls: one must not hold up another, and closing must wake them ===== */

        void TestBlockingOnLoopback() {
            static BlockingCall call;

            /* A thread blocked in accept. */
            const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
            const auto sa = MakeAddress(Loopback, LoopPort_Blocked);
            ::bind(listener, AsSockAddr(sa), sizeof(sa));
            ::listen(listener, 2);

            StartBlockingCall(std::addressof(call), BlockingCall::Kind_Accept, listener);
            svcSleepThread(INT64_C(1000000) * (200));
            Check("accept blocks while there is no connection", !call.done);

            /* Meanwhile this thread uses the service freely. */
            const int other = ::socket(AF_INET, SOCK_DGRAM, 0);
            const auto other_sa = MakeAddress(Loopback, LoopPort_UdpA);
            ::bind(other, AsSockAddr(other_sa), sizeof(other_sa));
            SetReceiveTimeout(other, 3000);
            ssize_t n = ::sendto(other, "meanwhile", 9, 0, AsSockAddr(other_sa), sizeof(other_sa));
            const ssize_t m = ::recv(other, g_in, 16, 0);
            Check("other calls are served while accept is blocked", n == 9 && m == 9 && !call.done, errno);
            ::close(other);

            /* A connection wakes it. */
            const int client = ConnectTcp(Loopback, LoopPort_Blocked);
            const bool finished = FinishBlockingCall(std::addressof(call), 3000);
            Check("a connection wakes the blocked accept", client >= 0 && finished && call.result >= 0, call.error);
            const int server = finished ? static_cast<int>(call.result) : -1;

            /* A thread blocked in recv; data wakes it. */
            if (server >= 0) {
                StartBlockingCall(std::addressof(call), BlockingCall::Kind_Recv, server);
                svcSleepThread(INT64_C(1000000) * (200));
                const bool blocked = !call.done;
                n = ::send(client, "wake", 4, 0);
                const bool woke = FinishBlockingCall(std::addressof(call), 3000);
                Check("recv blocks until data arrives, then returns it", blocked && n == 4 && woke && call.result == 4 && std::memcmp(call.data, "wake", 4) == 0, call.error);

                /* A thread blocked in recv; closing its descriptor from another thread wakes it with an error. */
                StartBlockingCall(std::addressof(call), BlockingCall::Kind_Recv, server);
                svcSleepThread(INT64_C(1000000) * (200));
                const bool blocked2 = !call.done;
                ::close(server);
                const bool woke2 = FinishBlockingCall(std::addressof(call), 3000);
                Check("closing a descriptor wakes a recv blocked on it with EBADF", blocked2 && woke2 && call.result == -1 && call.error == EBADF, call.error);
            } else {
                Skip("blocked recv woken by data and by close", "no accepted connection");
            }

            /* A thread blocked in poll with no timeout; a connection wakes it. */
            StartBlockingCall(std::addressof(call), BlockingCall::Kind_Poll, listener);
            svcSleepThread(INT64_C(1000000) * (200));
            const bool blocked3 = !call.done;
            const int client2 = ConnectTcp(Loopback, LoopPort_Blocked);
            const bool woke3 = FinishBlockingCall(std::addressof(call), 3000);
            Check("poll with no timeout blocks until its descriptor is ready", blocked3 && client2 >= 0 && woke3 && call.result == 1, call.error);

            ::close(client);
            ::close(client2);
            ::close(listener);
        }

        /* ===== The network: lease, then the host ===== */

        /* The address this host would send from to reach the QEMU host: zero until DHCP has given us one. */
        u32 GetLocalAddress() {
            const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
            if (fd < 0) {
                return 0;
            }
            const auto sa = MakeAddress(HostAddress, HostPort_UdpEcho);
            u32 address = 0;
            if (::connect(fd, AsSockAddr(sa), sizeof(sa)) == 0) {
                struct sockaddr_in name = {};
                socklen_t len = sizeof(name);
                if (::getsockname(fd, AsSockAddr(name), std::addressof(len)) == 0) {
                    address = ntohl(name.sin_addr.s_addr);
                }
            }
            ::close(fd);
            return address;
        }

        u32 WaitForLease() {
            const u64 start = svcGetSystemTick();
            u32 address = 0;
            while ((address = GetLocalAddress()) == 0 && ElapsedMilliSeconds(start) < 15000) {
                svcSleepThread(INT64_C(1000000) * (100));
            }
            Check("the interface gets an address by DHCP", address != 0, ElapsedMilliSeconds(start));
            if (address != 0) {
                Log("[net_test] address %u.%u.%u.%u\n", (address >> 24) & 0xFF, (address >> 16) & 0xFF, (address >> 8) & 0xFF, address & 0xFF);
            }
            return address;
        }

        /* One datagram to the host's echo server and back. */
        bool UdpEcho(int fd, const u8 *data, size_t size) {
            const auto sa = MakeAddress(HostAddress, HostPort_UdpEcho);
            if (::sendto(fd, data, size, 0, AsSockAddr(sa), sizeof(sa)) != static_cast<ssize_t>(size)) {
                return false;
            }
            struct sockaddr_in from = {};
            socklen_t from_len = sizeof(from);
            const ssize_t n = ::recvfrom(fd, g_in, sizeof(g_in), 0, AsSockAddr(from), std::addressof(from_len));
            return n == static_cast<ssize_t>(size) && std::memcmp(g_in, data, size) == 0 && ntohl(from.sin_addr.s_addr) == HostAddress && ntohs(from.sin_port) == HostPort_UdpEcho;
        }

        bool IsHostServerReachable() {
            const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
            SetReceiveTimeout(fd, 1000);
            bool reachable = false;
            for (int i = 0; i < 3 && !reachable; ++i) {
                reachable = UdpEcho(fd, reinterpret_cast<const u8 *>("probe"), 5);
            }
            ::close(fd);
            return reachable;
        }

        void TestHostUdp() {
            const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
            SetReceiveTimeout(fd, 3000);

            Check("UDP: a small datagram is echoed by the host", UdpEcho(fd, reinterpret_cast<const u8 *>("hello over udp"), 14), errno);

            FillPattern(g_out, 1400, 21);
            Check("UDP: a 1400-byte datagram is echoed intact", UdpEcho(fd, g_out, 1400), errno);

            /* Larger than the MTU: IP fragments it on the way out and reassembles the reply. */
            FillPattern(g_out, 6000, 22);
            Check("UDP: a 6000-byte datagram is fragmented, echoed and reassembled", UdpEcho(fd, g_out, 6000), errno);

            bool all = true;
            for (u32 i = 0; i < 50 && all; ++i) {
                FillPattern(g_out, 64 + i * 7, 100 + i);
                all = UdpEcho(fd, g_out, 64 + i * 7);
            }
            Check("UDP: fifty datagrams of different sizes are echoed in turn", all, errno);

            ::close(fd);
        }

        void TestHostTcp(u32 local_address) {
            /* Connect out. */
            int fd = ConnectTcp(HostAddress, HostPort_TcpEcho);
            Check("TCP: connect to the host", fd >= 0, errno);
            if (fd < 0) {
                return;
            }

            struct sockaddr_in peer = {}, name = {};
            socklen_t len = sizeof(peer);
            int ret = ::getpeername(fd, AsSockAddr(peer), std::addressof(len));
            len = sizeof(name);
            const int ret2 = ::getsockname(fd, AsSockAddr(name), std::addressof(len));
            Check("TCP: getpeername is the host, getsockname our leased address", ret == 0 && ret2 == 0 && ntohl(peer.sin_addr.s_addr) == HostAddress && ntohs(peer.sin_port) == HostPort_TcpEcho && ntohl(name.sin_addr.s_addr) == local_address && name.sin_port != 0, errno);

            /* Echo, in pieces that each span several segments. */
            SetReceiveTimeout(fd, 10000);
            bool all = true;
            size_t echoed = 0;
            for (u32 round = 0; round < 20 && all; ++round) {
                const size_t size = 3000 + round * 1500;
                FillPattern(g_out, size, round);
                all = ::send(fd, g_out, size, 0) == static_cast<ssize_t>(size) && ReceiveAll(fd, g_in, size) == static_cast<ssize_t>(size) && std::memcmp(g_in, g_out, size) == 0;
                echoed += size;
            }
            Check("TCP: twenty messages of 3000 to 31500 bytes are echoed intact", all, echoed);

            /* While this connection is idle, a recv on it blocks, and must not hold up anything else. */
            static BlockingCall call;
            StartBlockingCall(std::addressof(call), BlockingCall::Kind_Recv, fd);
            svcSleepThread(INT64_C(1000000) * (200));
            const bool blocked = !call.done;

            const int udp = ::socket(AF_INET, SOCK_DGRAM, 0);
            SetReceiveTimeout(udp, 3000);
            const bool udp_ok = UdpEcho(udp, reinterpret_cast<const u8 *>("not stalled"), 11);
            ::close(udp);
            const int second = ConnectTcp(HostAddress, HostPort_TcpEcho);
            const bool second_ok = second >= 0 && ::send(second, "2nd", 3, 0) == 3 && ReceiveAll(second, g_in, 3) == 3 && std::memcmp(g_in, "2nd", 3) == 0;
            ::close(second);
            Check("TCP: with a recv blocked, UDP and a second connection still work", blocked && udp_ok && second_ok && !call.done, errno);

            const ssize_t sent = ::send(fd, "wake", 4, 0);
            const bool woke = FinishBlockingCall(std::addressof(call), 5000);
            Check("TCP: the blocked recv returns when the host answers", sent == 4 && woke && call.result == 4 && std::memcmp(call.data, "wake", 4) == 0, call.error);

            /* Non-blocking mode on a real connection. */
            ::fcntl(fd, F_SETFL, O_NONBLOCK);
            ssize_t n = ::recv(fd, g_in, 16, 0);
            Check("TCP: non-blocking recv on an idle connection fails with EAGAIN", n == -1 && errno == EAGAIN, errno);

            struct pollfd pfd = { fd, POLLIN | POLLOUT, 0 };
            ret = ::poll(std::addressof(pfd), 1, 1000);
            Check("TCP: an idle connection polls writable and not readable", ret == 1 && (pfd.revents & POLLOUT) != 0 && (pfd.revents & POLLIN) == 0, pfd.revents);

            ::send(fd, "ping", 4, 0);
            pfd = { fd, POLLIN, 0 };
            ret = ::poll(std::addressof(pfd), 1, 5000);
            n = ::recv(fd, g_in, 16, 0);
            Check("TCP: poll wakes when the echo arrives, and recv then succeeds", ret == 1 && (pfd.revents & POLLIN) != 0 && n == 4 && std::memcmp(g_in, "ping", 4) == 0, errno);
            ::fcntl(fd, F_SETFL, 0);

            /* A receive timeout on a stream. */
            SetReceiveTimeout(fd, 300);
            const u64 start = svcGetSystemTick();
            n = ::recv(fd, g_in, 16, 0);
            const s64 elapsed = ElapsedMilliSeconds(start);
            Check("TCP: a blocking recv gives up after SO_RCVTIMEO with EAGAIN", n == -1 && errno == EAGAIN && 250 <= elapsed && elapsed < 2000, elapsed);

            ::close(fd);

            /* A megabyte out, in one blocking send, which must queue all of it. */
            fd = ConnectTcp(HostAddress, HostPort_TcpSink);
            if (fd >= 0) {
                const size_t size = sizeof(g_out);
                FillPattern(g_out, size, 33);
                SetReceiveTimeout(fd, 20000);

                n = ::send(fd, g_out, size, 0);
                ret = ::shutdown(fd, SHUT_WR);

                u8 reply[8] = {};
                const ssize_t m = ReceiveAll(fd, reply, sizeof(reply));
                const u32 count    = (static_cast<u32>(reply[0]) << 24) | (reply[1] << 16) | (reply[2] << 8) | reply[3];
                const u32 checksum = (static_cast<u32>(reply[4]) << 24) | (reply[5] << 16) | (reply[6] << 8) | reply[7];
                Check("TCP: 1 MiB sent in one call arrives complete and intact (count and Adler-32 from the host)", n == static_cast<ssize_t>(size) && ret == 0 && m == 8 && count == size && checksum == Adler32(g_out, size), count);

                n = ::recv(fd, g_in, 16, 0);
                Check("TCP: end of stream after the host closes", n == 0, errno);
                ::close(fd);
            } else {
                Check("TCP: connect to the host's sink", false, errno);
            }

            /* A megabyte in. */
            fd = ConnectTcp(HostAddress, HostPort_TcpSource);
            if (fd >= 0) {
                const u32 seed = 44;
                const u32 size = sizeof(g_in);
                const u8 header[8] = { 0, 0, 0, static_cast<u8>(seed), static_cast<u8>(size >> 24), static_cast<u8>(size >> 16), static_cast<u8>(size >> 8), static_cast<u8>(size) };
                SetReceiveTimeout(fd, 20000);
                ::send(fd, header, sizeof(header), 0);

                size_t total = 0;
                bool intact = true;
                while (true) {
                    n = ::recv(fd, g_in, 65536, 0);
                    if (n <= 0) {
                        break;
                    }
                    intact = intact && VerifyPattern(g_in, n, seed, total);
                    total += n;
                }
                Check("TCP: 1 MiB received in 64 KiB reads is complete and intact", n == 0 && total == size && intact, total);
                ::close(fd);
            } else {
                Check("TCP: connect to the host's source", false, errno);
            }

            /* A connection to our own leased address never leaves the machine, but takes the same path */
            /* through IP as one that does. */
            {
                const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
                const auto any = MakeAddress(INADDR_ANY, LoopPort_Self);
                ::bind(listener, AsSockAddr(any), sizeof(any));
                ::listen(listener, 1);
                SetReceiveTimeout(listener, 5000);

                const int self = ConnectTcp(local_address, LoopPort_Self);
                const int accepted = ::accept(listener, nullptr, nullptr);
                n = ::send(self, "to myself", 9, 0);
                std::memset(g_in, 0, 16);
                SetReceiveTimeout(accepted, 5000);
                const ssize_t m = ::recv(accepted, g_in, 16, 0);
                Check("TCP: connect to our own leased address", self >= 0 && accepted >= 0 && n == 9 && m == 9 && std::memcmp(g_in, "to myself", 9) == 0, errno);
                ::close(self);
                ::close(accepted);
                ::close(listener);
            }

            /* A port nobody listens on. */
            fd = ConnectTcp(HostAddress, HostPort_Closed);
            Check("TCP: connect to a closed port on the host fails with ECONNREFUSED", fd == -1 && errno == ECONNREFUSED, errno);
            if (fd >= 0) {
                ::close(fd);
            }

            /* The same, non-blocking: the failure is reported through poll and SO_ERROR. */
            fd = ::socket(AF_INET, SOCK_STREAM, 0);
            ::fcntl(fd, F_SETFL, O_NONBLOCK);
            const auto closed = MakeAddress(HostAddress, HostPort_Closed);
            ret = ::connect(fd, AsSockAddr(closed), sizeof(closed));
            pfd = { fd, POLLOUT, 0 };
            const int polled = ::poll(std::addressof(pfd), 1, 10000);
            int error = 0;
            len = sizeof(error);
            ::getsockopt(fd, SOL_SOCKET, SO_ERROR, std::addressof(error), std::addressof(len));
            Check("TCP: a refused non-blocking connect shows in poll and SO_ERROR", ret == -1 && errno == EINPROGRESS && polled == 1 && (pfd.revents & (POLLERR | POLLHUP)) != 0 && error == ECONNREFUSED, error);
            ::close(fd);
        }

        /* ===== Inbound: the host connects to us through QEMU's forwarded ports ===== */

        bool AskHost(int control, const char *request, const char *expected) {
            const auto sa = MakeAddress(HostAddress, HostPort_Control);
            ::sendto(control, request, std::strlen(request), 0, AsSockAddr(sa), sizeof(sa));
            if (expected == nullptr) {
                return true;
            }
            char reply[128] = {};
            const ssize_t n = ::recv(control, reply, sizeof(reply) - 1, 0);
            reply[std::max<ssize_t>(n, 0)] = 0;
            if (n <= 0 || std::strcmp(reply, expected) != 0) {
                Log("[net_test] host answered \"%s\" to %s\n", n > 0 ? reply : "(nothing)", request);
                return false;
            }
            return true;
        }

        void TestInbound() {
            const int control = ::socket(AF_INET, SOCK_DGRAM, 0);
            SetReceiveTimeout(control, 15000);

            /* TCP. */
            const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
            int value = 1;
            ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, std::addressof(value), sizeof(value));
            const auto any = MakeAddress(INADDR_ANY, GuestPort_TcpIn);
            int ret = ::bind(listener, AsSockAddr(any), sizeof(any));
            const int ret2 = ::listen(listener, 4);
            Check("inbound: listen on all addresses", ret == 0 && ret2 == 0, errno);

            /* accept() blocks until the host gets through; the timeout only bounds a failed run. */
            SetReceiveTimeout(listener, 15000);
            AskHost(control, "TCPIN", nullptr);

            struct sockaddr_in peer = {};
            socklen_t peer_len = sizeof(peer);
            const int conn = ::accept(listener, AsSockAddr(peer), std::addressof(peer_len));
            Check("inbound: accept a connection from the host", conn >= 0 && peer.sin_family == AF_INET && peer.sin_addr.s_addr != 0, errno);

            if (conn >= 0) {
                constexpr const char Greeting[] = "hello from the host\n";
                constexpr const char Reply[]    = "hello from the guest\n";
                SetReceiveTimeout(conn, 10000);

                std::memset(g_in, 0, 64);
                ssize_t n = ReceiveAll(conn, g_in, sizeof(Greeting) - 1);
                Check("inbound: receive the host's greeting", n == sizeof(Greeting) - 1 && std::memcmp(g_in, Greeting, sizeof(Greeting) - 1) == 0, errno);

                n = ::send(conn, Reply, sizeof(Reply) - 1, 0);
                const ssize_t end = ::recv(conn, g_in, 16, 0);
                Check("inbound: reply, then read end of stream when the host finishes", n == sizeof(Reply) - 1 && end == 0, errno);
                ::close(conn);

                char reply[128] = {};
                n = ::recv(control, reply, sizeof(reply) - 1, 0);
                reply[std::max<ssize_t>(n, 0)] = 0;
                Check("inbound: the host confirms it received our reply", n > 0 && std::strcmp(reply, "TCPIN-OK") == 0, errno);
                if (n > 0 && std::strcmp(reply, "TCPIN-OK") != 0) {
                    Log("[net_test] host answered \"%s\"\n", reply);
                }
            } else {
                Skip("inbound TCP exchange", "no connection arrived; is the port forwarded?");
            }
            ::close(listener);

            /* UDP. */
            const int udp = ::socket(AF_INET, SOCK_DGRAM, 0);
            const auto udp_any = MakeAddress(INADDR_ANY, GuestPort_UdpIn);
            ret = ::bind(udp, AsSockAddr(udp_any), sizeof(udp_any));
            SetReceiveTimeout(udp, 10000);

            const bool asked = AskHost(control, "UDPIN", "UDPIN-SENT");
            constexpr const char Datagram[] = "datagram from the host";
            std::memset(g_in, 0, 64);
            struct sockaddr_in from = {};
            socklen_t from_len = sizeof(from);
            const ssize_t n = ::recvfrom(udp, g_in, 64, 0, AsSockAddr(from), std::addressof(from_len));
            Check("inbound: receive a datagram the host sent to our bound port", ret == 0 && asked && n == sizeof(Datagram) - 1 && std::memcmp(g_in, Datagram, sizeof(Datagram) - 1) == 0, errno);

            ::close(udp);
            ::close(control);
        }

        /* ===== A client that disappears ===== */

        /*
         * When a process dies, the kernel closes its sessions. The service cannot see that on a session
         * whose request it is still working on, which is why every client opens a second session that
         * only exists to be closed (StartMonitoring). This plays a second client by hand: it registers,
         * blocks in Accept, and then closes its monitoring session, as the kernel would for a dead
         * process. The blocked call must return and the client's sockets must be closed.
         */
        void TestAbandonedClient() {
            ::Service session = {}, monitor = {};
            Result rc = ::smGetService(std::addressof(session), "bsd:u");
            if (R_SUCCEEDED(rc)) {
                rc = ::smGetService(std::addressof(monitor), "bsd:u");
            }

            ::TransferMemory tmem = {};
            if (R_SUCCEEDED(rc)) {
                rc = ::tmemCreate(std::addressof(tmem), 0x1000, Perm_None);
            }

            /* RegisterClient and StartMonitoring, as libnx sends them. */
            if (R_SUCCEEDED(rc)) {
                const struct { u32 config[8]; u64 pid_placeholder; u64 tmem_size; } in = { { 9, 0x8000, 0x10000, 0x40000, 0x40000, 0x2400, 0xA500, 4 }, 0, 0x1000 };
                s32 out = -1;
                rc = serviceDispatchInOut(std::addressof(session), 0, in, out, .in_send_pid = true, .in_num_handles = 1, .in_handles = { tmem.handle });
            }
            if (R_SUCCEEDED(rc)) {
                const u64 pid_placeholder = 0;
                rc = serviceDispatchIn(std::addressof(monitor), 1, pid_placeholder, .in_send_pid = true);
            }
            Check("a second client registers on sessions of its own", R_SUCCEEDED(rc), rc);
            if (R_FAILED(rc)) {
                return;
            }

            /* Its own listening socket. Descriptor numbers are per client, so this is its descriptor 0. */
            struct { int ret; int error; } out = { -1, -1 };
            const struct { int domain; int type; int protocol; } socket_in = { AF_INET, SOCK_STREAM, 0 };
            serviceDispatchInOut(std::addressof(session), 2, socket_in, out);
            const int fd = out.ret;

            const auto sa = MakeAddress(Loopback, LoopPort_Abandon);
            serviceDispatchInOut(std::addressof(session), 13, fd, out, .buffer_attrs = { SfBufferAttr_HipcAutoSelect | SfBufferAttr_In }, .buffers = { { std::addressof(sa), sizeof(sa) } });
            const int bound = out.ret;

            const struct { int fd; int backlog; } listen_in = { fd, 1 };
            serviceDispatchInOut(std::addressof(session), 18, listen_in, out);
            Check("the second client has its own descriptor table and a listening socket", fd == 0 && bound == 0 && out.ret == 0, out.error);

            static BlockingCall call;
            call.service = std::addressof(session);
            StartBlockingCall(std::addressof(call), BlockingCall::Kind_RawAccept, fd);
            svcSleepThread(INT64_C(1000000) * (200));
            const bool blocked = !call.done;

            /* The process "dies". */
            ::serviceClose(std::addressof(monitor));

            const bool woke = FinishBlockingCall(std::addressof(call), 3000);
            Check("closing the monitoring session fails the client's blocked call with ECANCELED", blocked && woke && call.result == -1 && call.error == 125, call.error);

            /* Its listener is gone: the port is free for someone else. */
            const int other = ::socket(AF_INET, SOCK_STREAM, 0);
            const int ret = ::bind(other, AsSockAddr(sa), sizeof(sa));
            Check("the abandoned client's sockets were closed", ret == 0, errno);
            ::close(other);

            /* What is left of it cannot make new sockets. */
            out = { 0, 0 };
            serviceDispatchInOut(std::addressof(session), 2, socket_in, out);
            Check("an abandoned client is refused new sockets", out.ret == -1 && out.error == 125, out.error);

            ::serviceClose(std::addressof(session));
            ::tmemClose(std::addressof(tmem));
        }

        /* ===== Leaving and coming back ===== */

        void TestSystemDuplicate() {
            ::Service session{};
            ::TransferMemory tmem{};
            Result rc = ::smGetService(std::addressof(session), "bsd:s");
            if (R_SUCCEEDED(rc)) rc = ::tmemCreate(std::addressof(tmem), 0x1000, Perm_None);
            if (R_SUCCEEDED(rc)) {
                const struct { u32 config[8]; u64 pid; u64 size; } in = { {9, 0x8000, 0x10000, 0x40000, 0x40000, 0x2400, 0xa500, 4}, 0, 0x1000 };
                u64 client_id = 0;
                rc = serviceDispatchInOut(std::addressof(session), 0, in, client_id, .in_send_pid = true, .in_num_handles = 1, .in_handles = {tmem.handle});
            }
            struct { s32 value, error; } out{};
            const struct { s32 domain, type, protocol; } socket_in{AF_INET, SOCK_STREAM, 0};
            s32 first = -1, second = -1, duplicate = -1;
            if (R_SUCCEEDED(rc)) {
                rc = serviceDispatchInOut(std::addressof(session), 2, socket_in, out); first = out.value;
            }
            if (R_SUCCEEDED(rc)) {
                rc = serviceDispatchInOut(std::addressof(session), 2, socket_in, out); second = out.value;
            }
            if (R_SUCCEEDED(rc)) rc = serviceDispatchInOut(std::addressof(session), 26, first, out);
            if (R_SUCCEEDED(rc)) {
                const struct { s32 fd; u32 padding; u64 reserved; } in{second, 0, 0};
                rc = serviceDispatchInOut(std::addressof(session), 27, in, out); duplicate = out.value;
            }
            Check("bsd:s duplicates the requested descriptor", R_SUCCEEDED(rc) && first == 0 && second == 1 && duplicate >= 0 && out.error == 0, rc);
            if (serviceIsActive(std::addressof(session))) {
                if (duplicate >= 0) serviceDispatchInOut(std::addressof(session), 26, duplicate, out);
                if (second >= 0) serviceDispatchInOut(std::addressof(session), 26, second, out);
                ::serviceClose(std::addressof(session));
            }
            if (tmem.handle != INVALID_HANDLE) ::tmemClose(std::addressof(tmem));
        }

        void TestReinitialize() {
            /* Leave sockets open on purpose: the service must close them when the client goes. */
            const int leaked = ::socket(AF_INET, SOCK_STREAM, 0);
            const auto sa = MakeAddress(Loopback, LoopPort_Tcp);
            const int ret = ::bind(leaked, AsSockAddr(sa), sizeof(sa));
            const int ret2 = ::listen(leaked, 1);

            /* Only the service's side is being tested, so drop the client without closing the descriptor. */
            ::socketExit();

            const Result rc = ::socketInitialize(nullptr);
            Check("socketExit, then socketInitialize again", ret == 0 && ret2 == 0 && R_SUCCEEDED(rc), rc);
            if (R_FAILED(rc)) {
                return;
            }

            /* If the old listener had survived, this bind would fail with EADDRINUSE. */
            const int fd = ::bsdSocket(AF_INET, SOCK_STREAM, 0);
            const int bound = ::bsdBind(fd, AsSockAddr(sa), sizeof(sa));
            Check("sockets a client left open were closed when it disconnected", fd >= 0 && bound == 0, g_bsdErrno);
            ::bsdClose(fd);
        }

    }

    void RunTest() {
        Log("[net_test] starting\n");

        if (TestService()) {
            TestErrors();
            TestOptions();
            TestLoopbackTcp();
            TestLoopbackUdp();
            TestPollAndSelect();
            TestBlockingOnLoopback();
            TestAbandonedClient();
            TestSystemDuplicate();

            const u32 address = WaitForLease();
            if (address == 0) {
                Skip("everything that needs the network", "no DHCP lease");
            } else if (!IsHostServerReachable()) {
                Skip("UDP and TCP against the host, and inbound connections", "host_server.py does not answer on 10.0.2.2");
            } else {
                TestHostUdp();
                TestHostTcp(address);
                TestInbound();
            }

            TestReinitialize();
        }

        Log("[net_test] done: %d checks, %d failure(s), %d skipped\n", g_checks, g_failures, g_skipped);

        /* Idle forever; this KIP is immortal. */
        while (true) {
            svcSleepThread(INT64_C(1000000000) * (60));
        }
    }

}

extern "C" {
    u32 __nx_applet_type = AppletType_None;
    void __libnx_initheap() {
        extern char *fake_heap_start, *fake_heap_end;
        fake_heap_start = reinterpret_cast<char *>(g_malloc_memory);
        fake_heap_end = fake_heap_start + sizeof(g_malloc_memory);
    }
    void __appInit() {
        SecmonArgs config{{0xC3000002, 65011}};
        svcCallSecureMonitor(&config);
        KUMO_ASSERT(config.X[0] == 0 && config.X[1] != 0);
        hosversionSet(BIT(31) | static_cast<u32>(config.X[1] >> 8));
        KUMO_ABORT_RESULT(smInitialize());
    }
    void __appExit() { smExit(); }
    void __libnx_exit(int) { for (;;) svcSleepThread(1000000000); }
}
int main(int, char **) { RunTest(); return 0; }

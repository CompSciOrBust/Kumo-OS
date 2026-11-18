#include <kumo/prelude.hpp>
#include "timesrv_sntp.hpp"
#include "timesrv_core.hpp"
#include "../../source/psc_log.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>

/* libnx leaves the service header out of switch.h, and it has no C linkage guard of its own. */
extern "C" {
    #include <switch/services/bsd.h>
}

namespace kumo::horizon::timesrv::srv {

    namespace {

        using psc::srv::Log;

        constexpr int MaxServers = 8;

        constexpr s64 NanoSecondsPerSecond = INT64_C(1000000000);
        constexpr u64 NtpToUnixSeconds     = 2208988800ull;     /* 1900-01-01 to 1970-01-01 */

        constexpr int    AttemptsPerServer = 2;
        constexpr int    ReplyTimeoutMs    = 2000;
        constexpr s64    ResyncSeconds     = 3600;              /* after a success */
        constexpr s64    FirstRetrySeconds = 5;                 /* after a failure, doubling up to... */
        constexpr s64    MaxRetrySeconds   = 120;

        constexpr s64    EarliestPlausibleTime = 946684800;     /* 2000-01-01: anything earlier is a broken server */

        struct Server {
            u32 address;    /* host order */
            u16 port;
        };

        constinit Server g_servers[MaxServers] = {};
        constinit int g_server_count = 0;

        /* A transfer memory buffer for bsdInitialize. bsdsockets ignores the sizes, but libnx wants the buffer. */
        constexpr size_t TransferMemorySize = 0x11000;
        alignas(4096) constinit u8 g_transfer_memory[0x20000];

        constexpr size_t ThreadStackSize = (32 * 1024);
        alignas(4096) constinit u8 g_thread_stack[ThreadStackSize];
        constinit Thread g_thread;

        bool ParseDecimal(const char *&p, u32 max, u32 *out) {
            if (*p < '0' || *p > '9') {
                return false;
            }
            u32 value = 0;
            while (*p >= '0' && *p <= '9') {
                value = value * 10 + static_cast<u32>(*p - '0');
                if (value > max) {
                    return false;
                }
                ++p;
            }
            *out = value;
            return true;
        }

        void ParseServers(const char *text) {
            const char *p = text;
            while (*p != '\0' && g_server_count < MaxServers) {
                if (*p == ' ' || *p == ',' || *p == '\t') {
                    ++p;
                    continue;
                }

                u32 address = 0;
                bool ok = true;
                for (int i = 0; i < 4 && ok; ++i) {
                    u32 octet = 0;
                    ok = ParseDecimal(p, 255, std::addressof(octet));
                    address = (address << 8) | octet;
                    if (ok && i < 3) {
                        ok = (*p == '.');
                        if (ok) {
                            ++p;
                        }
                    }
                }

                u32 port = 123;
                if (ok && *p == ':') {
                    ++p;
                    ok = ParseDecimal(p, 65535, std::addressof(port)) && port != 0;
                }
                ok = ok && (*p == '\0' || *p == ' ' || *p == ',' || *p == '\t');

                if (ok) {
                    g_servers[g_server_count++] = Server{ address, static_cast<u16>(port) };
                } else {
                    Log("[psc] time: SNTP: ignoring a malformed server in KUMO_NTP_SERVERS\n");
                    while (*p != '\0' && *p != ' ' && *p != ',' && *p != '\t') {
                        ++p;
                    }
                }
            }
        }

        u32 ReadBe32(const u8 *p) {
            return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) | (static_cast<u32>(p[2]) << 8) | static_cast<u32>(p[3]);
        }

        s64 FloorDiv(s64 a, s64 b) {
            s64 q = a / b;
            if ((a % b != 0) && ((a < 0) != (b < 0))) {
                --q;
            }
            return q;
        }

        /* One request to one server; true if a valid reply set the network clock. */
        bool QueryServer(const Server &server) {
            const int fd = ::bsdSocket(AF_INET, SOCK_DGRAM, 0);
            if (fd < 0) {
                return false;
            }

            struct sockaddr_in sa = {};
            sa.sin_len         = sizeof(sa);
            sa.sin_family      = AF_INET;
            sa.sin_port        = htons(server.port);
            sa.sin_addr.s_addr = htonl(server.address);

            bool done = false;
            for (int attempt = 0; attempt < AttemptsPerServer && !done; ++attempt) {
                /* LI 0, version 4, mode 3 (client). The transmit timestamp is a nonce the server echoes as the originate timestamp. */
                u8 request[48] = {};
                request[0] = 0x23;
                const u64 nonce = ::armGetSystemTick() ^ (static_cast<u64>(attempt) << 56) ^ 0x5A5A5A5A5A5A5A5Aull;
                std::memcpy(request + 40, std::addressof(nonce), sizeof(nonce));

                const s64 sent_ns = GetSteadyNanoSeconds();
                if (::bsdSendTo(fd, request, sizeof(request), 0, reinterpret_cast<const struct sockaddr *>(std::addressof(sa)), sizeof(sa)) != static_cast<ssize_t>(sizeof(request))) {
                    /* No address yet (ENETUNREACH) or no route; the caller waits and tries again. */
                    break;
                }

                /* A reply from anyone else is ignored, until the time is up. */
                const s64 deadline_ns = sent_ns + static_cast<s64>(ReplyTimeoutMs) * 1000000;
                while (!done) {
                    const s64 remaining_ms = (deadline_ns - GetSteadyNanoSeconds()) / 1000000;
                    if (remaining_ms <= 0) {
                        break;
                    }

                    struct pollfd pfd = {};
                    pfd.fd     = fd;
                    pfd.events = POLLIN;
                    if (::bsdPoll(std::addressof(pfd), 1, static_cast<int>(remaining_ms)) <= 0) {
                        break;
                    }

                    u8 reply[128];
                    struct sockaddr_in from = {};
                    socklen_t from_length = sizeof(from);
                    const ssize_t length = ::bsdRecvFrom(fd, reply, sizeof(reply), 0, reinterpret_cast<struct sockaddr *>(std::addressof(from)), std::addressof(from_length));
                    const s64 received_ns = GetSteadyNanoSeconds();
                    if (length < 48) {
                        continue;
                    }

                    /* A server's answer to this request: mode 4, not "unsynchronized" (LI 3), stratum 1 to 15 (0 is a kiss-of-death), our nonce echoed. */
                    const u8 mode = reply[0] & 7;
                    const u8 leap = reply[0] >> 6;
                    const u8 stratum = reply[1];
                    if (ntohl(from.sin_addr.s_addr) != server.address || mode != 4 || leap == 3 || stratum < 1 || stratum > 15) {
                        continue;
                    }
                    if (std::memcmp(reply + 24, request + 40, 8) != 0) {
                        continue;
                    }

                    /* The server's transmit time, as NTP seconds since 1900 (era 0 until 2036, era 1 after: the top bit tells). */
                    u64 ntp_seconds = ReadBe32(reply + 40);
                    const u32 fraction = ReadBe32(reply + 44);
                    if (ntp_seconds == 0) {
                        continue;
                    }
                    if ((ntp_seconds & 0x80000000ull) == 0) {
                        ntp_seconds += (1ull << 32);
                    }
                    const s64 unix_seconds = static_cast<s64>(ntp_seconds - NtpToUnixSeconds);
                    if (unix_seconds < EarliestPlausibleTime) {
                        continue;
                    }
                    const s64 fraction_ns = static_cast<s64>((static_cast<u64>(fraction) * 1000000000ull) >> 32);

                    /* The reply took half the round trip to arrive. */
                    const s64 server_ns_when_received = unix_seconds * NanoSecondsPerSecond + fraction_ns + (received_ns - sent_ns) / 2;
                    const s64 offset_ns = server_ns_when_received - received_ns;

                    /* The clocks count whole seconds. Rounding so that the error is within a second either way. */
                    const SteadyClockTimePoint steady = { received_ns / NanoSecondsPerSecond, GetSteadySourceId() };
                    const s64 offset_seconds = FloorDiv(offset_ns, NanoSecondsPerSecond) + 1;
                    UpdateNetworkClock(offset_seconds + steady.time_point, steady);

                    Log("[psc] time: SNTP: %u.%u.%u.%u:%u answered stratum %u, round trip %lld ms; network time set to %lld\n",
                        static_cast<unsigned>(server.address >> 24), static_cast<unsigned>((server.address >> 16) & 0xFF), static_cast<unsigned>((server.address >> 8) & 0xFF), static_cast<unsigned>(server.address & 0xFF),
                        static_cast<unsigned>(server.port), static_cast<unsigned>(stratum), static_cast<long long>((received_ns - sent_ns) / 1000000), static_cast<long long>(offset_seconds + steady.time_point));
                    done = true;
                }
            }

            ::bsdClose(fd);
            return done;
        }

        void SntpThreadFunction(void *) {
            bool bsd_ready = false;
            s64 retry_seconds = FirstRetrySeconds;
            int failures = 0;

            while (true) {
                if (!bsd_ready) {
                    /* bsdsockets may not have registered yet: try again in a moment. */
                    BsdInitConfig config = {};
                    config.version             = ::bsdGetDefaultInitConfig()->version;
                    config.tmem_buffer         = g_transfer_memory;
                    config.tmem_buffer_size    = TransferMemorySize;
                    config.tcp_tx_buf_size     = 0x2000;
                    config.tcp_rx_buf_size     = 0x2000;
                    config.tcp_tx_buf_max_size = 0;
                    config.tcp_rx_buf_max_size = 0;
                    config.udp_tx_buf_size     = 0x2400;
                    config.udp_rx_buf_size     = 0xA500;
                    config.sb_efficiency       = 1;
                    const Result rc = ::bsdInitialize(std::addressof(config), 1, 1 /* bsd:u */);
                    if (R_FAILED(rc)) {
                        svcSleepThread(2LL * 1000000000);
                        continue;
                    }
                    bsd_ready = true;
                    Log("[psc] time: SNTP: connected to bsd:u, %d server(s)\n", g_server_count);
                }

                bool synced = false;
                for (int i = 0; i < g_server_count && !synced; ++i) {
                    synced = QueryServer(g_servers[i]);
                }

                if (synced) {
                    failures = 0;
                    retry_seconds = FirstRetrySeconds;
                    svcSleepThread(ResyncSeconds * 1000000000LL);
                } else {
                    if (failures == 0 || (failures % 10) == 0) {
                        Log("[psc] time: SNTP: no server answered (no network address yet, or no route, or no server reachable); the clocks keep their current time and are retried every %lld s or more\n", static_cast<long long>(retry_seconds));
                    }
                    ++failures;
                    svcSleepThread(retry_seconds * 1000000000LL);
                    retry_seconds = std::min<s64>(retry_seconds * 2, MaxRetrySeconds);
                }
            }
        }

    }

    void StartSntpClient() {
        ParseServers(KUMO_NTP_SERVERS);
        if (g_server_count == 0) {
            Log("[psc] time: SNTP: no usable server in KUMO_NTP_SERVERS (\"%s\"); the network clock stays unset\n", KUMO_NTP_SERVERS);
            return;
        }

        s32 priority = 0;
        KUMO_ABORT_RESULT(svcGetThreadPriority(&priority, CUR_THREAD_HANDLE));
        KUMO_ABORT_RESULT(threadCreate(&g_thread, SntpThreadFunction, nullptr, g_thread_stack, sizeof(g_thread_stack), priority, -2));
        KUMO_ABORT_RESULT(threadStart(&g_thread));
    }

}

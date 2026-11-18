#include <kumo/prelude.hpp>
#include "bsd_socket.hpp"
#include "../stack/bsd_stack.hpp"

extern "C" {
    #include "lwip/tcp.h"
    #include "lwip/udp.h"
    #include "lwip/pbuf.h"
    #include "lwip/ip4.h"
    #include "lwip/ip4_addr.h"
    #include "lwip/priv/tcp_priv.h"
}

namespace kumo::horizon::hznet::socket {

    namespace {

        using namespace wire;

        enum class Kind : u8 {
            Tcp,
            Udp,
        };

        /* The life of a TCP socket. UDP sockets stay in Fresh. */
        enum class State : u8 {
            Fresh,          /* created; perhaps bound */
            Connecting,     /* connect() issued, handshake in progress */
            Connected,
            Listening,
            Disconnected,   /* was connecting or connected; lwIP has freed the connection */
        };

        /* Datagrams a UDP socket holds for its reader. Beyond either limit, arriving datagrams are dropped, */
        /* which is what UDP does when a receiver falls behind. */
        constexpr size_t MaxQueuedDatagrams     = 64;
        constexpr size_t MaxQueuedDatagramBytes = (256 * 1024);

        /* Connections a listening socket holds for accept(), whatever backlog the client asks for. */
        constexpr int MaxBacklog = 32;

        constexpr size_t MaxUdpPayload = 65507;

        /* The most one send or recv transfers: what fits in the signed 32-bit value it returns. */
        constexpr size_t MaxTransferSize = 0x7FFFFFFF;

        /* The most poll() looks at in one call. */
        constexpr u32 MaxPollDescriptors = 1024;

        struct Datagram {
            struct pbuf *data;
            u32 address;    /* network byte order */
            u16 port;       /* host byte order */
        };

        struct Deadline {
            bool infinite;
            u64 end;
        };

    }

    struct Socket {
        /* Bookkeeping. A socket is freed when no descriptor names it and no call is using it. */
        bool in_use;
        Kind kind;
        State state;
        bool closed;            /* torn down: every descriptor is gone, lwIP resources released */
        bool non_blocking;
        bool bound;
        bool rx_shutdown;
        bool tx_shutdown;
        bool rx_fin;            /* the peer has finished sending */
        int descriptor_count;
        int use_count;

        /* Options kept here rather than in lwIP. */
        s32 pending_error;      /* SO_ERROR */
        std::chrono::nanoseconds rx_timeout;    /* zero: none */
        std::chrono::nanoseconds tx_timeout;
        Linger linger;
        s32 sndbuf;
        s32 rcvbuf;

        /* Addresses, kept so they survive the connection (network order address, host order port). */
        u32 local_address;
        u16 local_port;
        u32 remote_address;
        u16 remote_port;
        bool has_remote;

        /* TCP. `tcp` is null once lwIP has freed the connection. */
        struct tcp_pcb *tcp;
        struct pbuf *rx_head;   /* received, unread data: a chain linked through pbuf::next */
        struct pbuf *rx_tail;
        size_t rx_offset;       /* bytes of rx_head already read */
        size_t rx_bytes;

        /* TCP, listening. */
        Socket *accept_queue[MaxBacklog];
        int accept_count;
        int backlog;
        Socket *listener;       /* for a connection waiting in a listener's queue */

        /* UDP. */
        struct udp_pcb *udp;
        Datagram datagrams[MaxQueuedDatagrams];
        size_t datagram_head;
        size_t datagram_count;
        size_t datagram_bytes;
    };

    namespace {

        constinit Socket g_sockets[MaxSockets] = {};

        /* ---- Small helpers ---- */

        constexpr CallResult Ok(s32 value = 0) { return { value, Errno_Success }; }
        constexpr CallResult Fail(s32 error) { return { -1, error }; }

        s32 ConvertLwipError(err_t err) {
            switch (err) {
                case ERR_OK:         return Errno_Success;
                case ERR_MEM:        return Errno_ENOBUFS;
                case ERR_BUF:        return Errno_ENOBUFS;
                case ERR_TIMEOUT:    return Errno_ETIMEDOUT;
                case ERR_RTE:        return Errno_ENETUNREACH;
                case ERR_INPROGRESS: return Errno_EINPROGRESS;
                case ERR_VAL:        return Errno_EINVAL;
                case ERR_WOULDBLOCK: return Errno_EAGAIN;
                case ERR_USE:        return Errno_EADDRINUSE;
                case ERR_ALREADY:    return Errno_EALREADY;
                case ERR_ISCONN:     return Errno_EISCONN;
                case ERR_CONN:       return Errno_ENOTCONN;
                case ERR_IF:         return Errno_ENETDOWN;
                case ERR_ABRT:       return Errno_ECONNABORTED;
                case ERR_RST:        return Errno_ECONNRESET;
                case ERR_CLSD:       return Errno_ENOTCONN;
                case ERR_ARG:        return Errno_EINVAL;
                default:             return Errno_EIO;
            }
        }

        Deadline MakeDeadline(std::chrono::nanoseconds timeout) {
            if (timeout.count() <= 0) {
                return { true, 0 };
            }
            const u64 ns = static_cast<u64>(timeout.count());
            const u64 ticks = (ns / 625) * 12 + (ns % 625) * 12 / 625;
            return { false, ::svcGetSystemTick() + ticks };
        }

        /* Sleep until something changes. Returns false once the deadline has passed. */
        bool Wait(const Deadline &deadline) {
            /* Even an unbounded wait wakes now and then, so no missed notification can strand a caller. */
            constexpr auto MaxSlice = std::chrono::nanoseconds(1'000'000'000);

            if (deadline.infinite) {
                stack::WaitForActivity(MaxSlice);
                return true;
            }

            const u64 now = ::svcGetSystemTick();
            if (now >= deadline.end) {
                return false;
            }
            const u64 remaining = deadline.end - now;
            const auto slice = remaining >= 19'200'000
                ? MaxSlice
                : std::chrono::nanoseconds(static_cast<s64>(::armTicksToNs(remaining)));
            stack::WaitForActivity(slice);
            return true;
        }

        std::chrono::nanoseconds FromTimeVal(const TimeVal &tv) {
            return std::chrono::seconds(tv.sec) + std::chrono::microseconds(tv.usec);
        }

        TimeVal ToTimeVal(std::chrono::nanoseconds span) {
            return { std::chrono::duration_cast<std::chrono::seconds>(span).count(), std::chrono::duration_cast<std::chrono::microseconds>(span).count() % 1'000'000 };
        }

        /* Parse a client's sockaddr_in. */
        s32 ParseAddress(u32 *out_address, u16 *out_port, const void *address, size_t size) {
            if (address == nullptr || size < SockAddrInMinSize) {
                return Errno_EINVAL;
            }

            SockAddrIn in = {};
            std::memcpy(std::addressof(in), address, std::min(size, sizeof(in)));
            if (in.family != AF_INET_) {
                return Errno_EAFNOSUPPORT;
            }

            *out_address = in.address;
            *out_port    = __builtin_bswap16(in.port);
            return Errno_Success;
        }

        void WriteAddress(void *out, size_t size, u32 *out_length, u32 address, u16 port) {
            SockAddrIn result = {};
            result.len     = sizeof(result);
            result.family  = AF_INET_;
            result.port    = __builtin_bswap16(port);
            result.address = address;

            /* A buffer that is too small gets a truncated address, and the full length is still reported. */
            if (out != nullptr && size > 0) {
                std::memcpy(out, std::addressof(result), std::min(size, sizeof(result)));
            }
            if (out_length != nullptr) {
                *out_length = sizeof(result);
            }
        }

        ip_addr_t ToIpAddr(u32 address) {
            ip_addr_t result;
            ip4_addr_set_u32(ip_2_ip4(std::addressof(result)), address);
            return result;
        }

        /* ---- Socket allocation and lifetime ---- */

        Socket *AllocateSocket(Kind kind) {
            for (auto &socket : g_sockets) {
                if (!socket.in_use) {
                    socket = {};
                    socket.in_use = true;
                    socket.kind   = kind;
                    socket.state  = State::Fresh;
                    socket.sndbuf = TCP_SND_BUF;
                    socket.rcvbuf = TCP_WND;
                    return std::addressof(socket);
                }
            }
            return nullptr;
        }

        void FreeReceivedData(Socket *socket) {
            /* The chain is linked through next; lwIP frees along it. */
            if (socket->rx_head != nullptr) {
                pbuf_free(socket->rx_head);
            }
            socket->rx_head   = nullptr;
            socket->rx_tail   = nullptr;
            socket->rx_offset = 0;
            socket->rx_bytes  = 0;

            for (size_t i = 0; i < socket->datagram_count; ++i) {
                pbuf_free(socket->datagrams[(socket->datagram_head + i) % MaxQueuedDatagrams].data);
            }
            socket->datagram_head  = 0;
            socket->datagram_count = 0;
            socket->datagram_bytes = 0;
        }

        void FreeIfUnused(Socket *socket) {
            if (socket->closed && socket->descriptor_count == 0 && socket->use_count == 0) {
                socket->in_use = false;
            }
        }

        void DetachCallbacks(struct tcp_pcb *pcb) {
            tcp_arg(pcb, nullptr);
            tcp_recv(pcb, nullptr);
            tcp_sent(pcb, nullptr);
            tcp_err(pcb, nullptr);
        }

        /* Release everything a socket holds in lwIP. Called when its last descriptor is closed. */
        void TearDown(Socket *socket) {
            KUMO_ASSERT(!socket->closed);
            socket->closed = true;

            if (socket->kind == Kind::Udp) {
                if (socket->udp != nullptr) {
                    udp_remove(socket->udp);
                    socket->udp = nullptr;
                }
            } else if (socket->state == State::Listening) {
                /* Connections nobody accepted are reset. */
                for (int i = 0; i < socket->accept_count; ++i) {
                    Socket *child = socket->accept_queue[i];
                    child->listener = nullptr;
                    TearDown(child);
                }
                socket->accept_count = 0;

                if (socket->tcp != nullptr) {
                    tcp_arg(socket->tcp, nullptr);
                    tcp_accept(socket->tcp, nullptr);
                    tcp_close(socket->tcp);
                    socket->tcp = nullptr;
                }
            } else if (socket->tcp != nullptr) {
                struct tcp_pcb *pcb = socket->tcp;
                socket->tcp = nullptr;
                DetachCallbacks(pcb);

                /* As on BSD: closing with unread data, or with a zero linger time, resets the connection. */
                /* Otherwise lwIP finishes sending what is queued and closes in the background. */
                const bool reset = socket->state == State::Connecting || socket->rx_bytes > 0 || (socket->linger.onoff != 0 && socket->linger.linger == 0);
                if (reset || tcp_close(pcb) != ERR_OK) {
                    tcp_abort(pcb);
                }
            }

            FreeReceivedData(socket);
            stack::NotifyActivity();
            stack::KickNetworkThread();
            FreeIfUnused(socket);
        }

        /* Keeps a socket alive across a call that may wait. */
        class SocketUse {
            KUMO_NON_COPYABLE(SocketUse);
            KUMO_NON_MOVEABLE(SocketUse);
            private:
                Socket *m_socket;
            public:
                explicit SocketUse(Socket *socket) : m_socket(socket) {
                    if (m_socket != nullptr) {
                        ++m_socket->use_count;
                    }
                }
                ~SocketUse() {
                    if (m_socket != nullptr) {
                        --m_socket->use_count;
                        FreeIfUnused(m_socket);
                    }
                }
        };

        Socket *GetSocket(DescriptorTable &table, s32 fd) {
            if (fd < 0 || fd >= MaxDescriptorsPerClient || table.m_cancelled) {
                return nullptr;
            }
            return table.m_sockets[fd];
        }

        s32 FindFreeDescriptor(DescriptorTable &table) {
            for (s32 fd = 0; fd < MaxDescriptorsPerClient; ++fd) {
                if (table.m_sockets[fd] == nullptr) {
                    return fd;
                }
            }
            return -1;
        }

        void ReleaseDescriptor(DescriptorTable &table, s32 fd) {
            Socket *socket = table.m_sockets[fd];
            table.m_sockets[fd] = nullptr;
            if (--socket->descriptor_count == 0) {
                TearDown(socket);
            }
        }

        /* What every waiting call checks first each time it wakes. Returns an errno, or zero to carry on. */
        s32 CheckInterrupted(DescriptorTable &table, Socket *socket) {
            if (table.m_cancelled) {
                return Errno_ECANCELED;
            }
            if (socket != nullptr && socket->closed) {
                /* Another thread of the client closed the descriptor while this one was waiting on it. */
                return Errno_EBADF;
            }
            return Errno_Success;
        }

        s32 TakePendingError(Socket *socket) {
            const s32 error = socket->pending_error;
            socket->pending_error = Errno_Success;
            return error;
        }

        void RefreshAddresses(Socket *socket) {
            if (socket->kind == Kind::Tcp && socket->tcp != nullptr) {
                socket->local_address = ip4_addr_get_u32(ip_2_ip4(std::addressof(socket->tcp->local_ip)));
                socket->local_port    = socket->tcp->local_port;
                if (socket->state == State::Connected || socket->state == State::Connecting) {
                    socket->remote_address = ip4_addr_get_u32(ip_2_ip4(std::addressof(socket->tcp->remote_ip)));
                    socket->remote_port    = socket->tcp->remote_port;
                    socket->has_remote     = true;
                }
            } else if (socket->kind == Kind::Udp && socket->udp != nullptr) {
                socket->local_address = ip4_addr_get_u32(ip_2_ip4(std::addressof(socket->udp->local_ip)));
                socket->local_port    = socket->udp->local_port;

                /* An unbound, connected socket sends from the address of the interface that routes to its peer. */
                if (socket->local_address == 0 && socket->has_remote) {
                    const ip_addr_t remote = ToIpAddr(socket->remote_address);
                    if (struct netif *netif = ip4_route(ip_2_ip4(std::addressof(remote))); netif != nullptr) {
                        socket->local_address = ip4_addr_get_u32(netif_ip4_addr(netif));
                    }
                }
            }
        }

        /* ---- lwIP callbacks. lwIP calls these with the stack lock held (from the network thread, */
        /* or from inside a call this layer made). ---- */

        err_t OnTcpReceived(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
            Socket *socket = static_cast<Socket *>(arg);
            KUMO_UNUSED(err);

            if (p == nullptr) {
                /* The peer sent FIN. */
                socket->rx_fin = true;
                stack::NotifyActivity();
                return ERR_OK;
            }

            if (socket->rx_shutdown) {
                /* The reader has gone; take the data off the peer's hands and drop it. */
                tcp_recved(pcb, p->tot_len);
                pbuf_free(p);
                return ERR_OK;
            }

            /* Append to the unread chain. The window is reopened only as the client reads (tcp_recved), */
            /* so a slow reader holds the peer back instead of filling our memory. */
            socket->rx_bytes += p->tot_len;
            struct pbuf *last = p;
            while (last->next != nullptr) {
                last = last->next;
            }
            if (socket->rx_head == nullptr) {
                socket->rx_head = p;
            } else {
                socket->rx_tail->next = p;
            }
            socket->rx_tail = last;

            stack::NotifyActivity();
            return ERR_OK;
        }

        err_t OnTcpSent(void *arg, struct tcp_pcb *pcb, u16_t length) {
            KUMO_UNUSED(arg, pcb, length);
            /* Space has opened up in the send buffer. */
            stack::NotifyActivity();
            return ERR_OK;
        }

        /* The connection is gone and lwIP has already freed it. */
        void OnTcpError(void *arg, err_t err) {
            Socket *socket = static_cast<Socket *>(arg);
            socket->tcp = nullptr;

            if (socket->state == State::Connecting) {
                /* lwIP reports a refused connection as a reset, and one that got no answer as an abort. */
                socket->pending_error = (err == ERR_RST) ? Errno_ECONNREFUSED : (err == ERR_ABRT) ? Errno_ETIMEDOUT : ConvertLwipError(err);
            } else if (err == ERR_CLSD) {
                /* Both directions closed in order. That is an end of stream, not an error. */
                socket->rx_fin = true;
            } else {
                /* An abort from lwIP's side means the peer stopped answering: retransmissions or keepalives ran out. */
                socket->pending_error = (err == ERR_RST) ? Errno_ECONNRESET : (err == ERR_ABRT) ? Errno_ETIMEDOUT : ConvertLwipError(err);
            }
            socket->state = State::Disconnected;

            stack::NotifyActivity();
        }

        err_t OnTcpConnected(void *arg, struct tcp_pcb *pcb, err_t err) {
            Socket *socket = static_cast<Socket *>(arg);
            KUMO_UNUSED(pcb, err);

            socket->state = State::Connected;
            RefreshAddresses(socket);
            stack::NotifyActivity();
            return ERR_OK;
        }

        void AttachCallbacks(Socket *socket, struct tcp_pcb *pcb) {
            tcp_arg(pcb, socket);
            tcp_recv(pcb, OnTcpReceived);
            tcp_sent(pcb, OnTcpSent);
            tcp_err(pcb, OnTcpError);
        }

        err_t OnTcpAccepted(void *arg, struct tcp_pcb *new_pcb, err_t err) {
            Socket *listener = static_cast<Socket *>(arg);
            if (err != ERR_OK || new_pcb == nullptr || listener == nullptr) {
                return ERR_VAL;
            }

            /* Refusing makes lwIP abort the new connection. */
            if (listener->accept_count >= listener->backlog) {
                return ERR_MEM;
            }
            Socket *child = AllocateSocket(Kind::Tcp);
            if (child == nullptr) {
                return ERR_MEM;
            }

            child->tcp          = new_pcb;
            child->state        = State::Connected;
            child->bound        = true;
            child->listener     = listener;
            child->non_blocking = listener->non_blocking;
            AttachCallbacks(child, new_pcb);
            RefreshAddresses(child);

            /* Count the connection against lwIP's backlog until accept() takes it. */
            tcp_backlog_delayed(new_pcb);

            listener->accept_queue[listener->accept_count++] = child;
            stack::NotifyActivity();
            return ERR_OK;
        }

        void OnUdpReceived(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *address, u16_t port) {
            Socket *socket = static_cast<Socket *>(arg);
            KUMO_UNUSED(pcb);

            if (socket->rx_shutdown || socket->datagram_count >= MaxQueuedDatagrams || socket->datagram_bytes + p->tot_len > MaxQueuedDatagramBytes) {
                pbuf_free(p);
                return;
            }

            socket->datagrams[(socket->datagram_head + socket->datagram_count) % MaxQueuedDatagrams] = { p, ip4_addr_get_u32(ip_2_ip4(address)), port };
            ++socket->datagram_count;
            socket->datagram_bytes += p->tot_len;
            stack::NotifyActivity();
        }

        /* ---- Reading from a TCP socket's unread chain ---- */

        size_t CopyReceived(Socket *socket, void *buffer, size_t size, bool consume) {
            u8 *out = static_cast<u8 *>(buffer);
            size_t copied = 0;

            struct pbuf *p = socket->rx_head;
            size_t offset = socket->rx_offset;
            while (p != nullptr && copied < size) {
                const size_t chunk = std::min<size_t>(size - copied, p->len - offset);
                std::memcpy(out + copied, static_cast<const u8 *>(p->payload) + offset, chunk);
                copied += chunk;
                offset += chunk;

                if (offset == p->len) {
                    struct pbuf *next = p->next;
                    if (consume) {
                        /* Unlink before freeing, so only this buffer goes. */
                        p->next = nullptr;
                        pbuf_free(p);
                        socket->rx_head = next;
                        if (next == nullptr) {
                            socket->rx_tail = nullptr;
                        }
                    }
                    p = next;
                    offset = 0;
                }
            }

            if (consume) {
                socket->rx_offset = offset;
                socket->rx_bytes -= copied;

                /* Tell the peer it may send that much more. */
                size_t remaining = copied;
                while (remaining > 0 && socket->tcp != nullptr) {
                    const u16_t step = static_cast<u16_t>(std::min<size_t>(remaining, 0xFFFF));
                    tcp_recved(socket->tcp, step);
                    remaining -= step;
                }
            }
            return copied;
        }

        /* ---- Readiness, shared by poll and the blocking calls ---- */

        bool IsReadable(const Socket *socket) {
            if (socket->kind == Kind::Udp) {
                return socket->datagram_count > 0 || socket->rx_shutdown;
            }
            switch (socket->state) {
                case State::Listening:    return socket->accept_count > 0;
                case State::Connected:    return socket->rx_bytes > 0 || socket->rx_fin || socket->rx_shutdown || socket->pending_error != 0;
                case State::Disconnected: return true;
                default:                  return false;
            }
        }

        bool IsWritable(const Socket *socket) {
            if (socket->kind == Kind::Udp) {
                return true;
            }
            switch (socket->state) {
                /* Writable means a write would not block; one that would fail at once qualifies. */
                case State::Connected:    return socket->tx_shutdown || socket->tcp == nullptr || tcp_sndbuf(socket->tcp) >= TCP_MSS;
                case State::Disconnected: return true;
                default:                  return false;
            }
        }

        s16 GetPollEvents(const Socket *socket, s16 wanted) {
            s16 events = 0;
            if (IsReadable(socket)) {
                events |= wanted & (POLLIN_ | POLLRDNORM_);
            }
            if (IsWritable(socket)) {
                events |= wanted & (POLLOUT_ | POLLWRBAND_);
            }
            if (socket->pending_error != 0) {
                events |= POLLERR_;
            }
            if (socket->kind == Kind::Tcp && (socket->state == State::Disconnected || (socket->rx_fin && socket->tx_shutdown))) {
                events |= POLLHUP_;
            }
            return events;
        }

        /* ---- TCP and UDP halves of the data calls ---- */

        CallResult ReceiveTcp(DescriptorTable &table, Socket *socket, void *buffer, size_t size, s32 flags) {
            if (socket->state == State::Fresh || socket->state == State::Listening || socket->state == State::Connecting) {
                return Fail(Errno_ENOTCONN);
            }
            if (size == 0) {
                return Ok(0);
            }

            const bool peek      = (flags & MSG_PEEK_) != 0;
            const bool wait_all  = (flags & MSG_WAITALL_) != 0 && !peek;
            const bool dont_wait = socket->non_blocking || (flags & MSG_DONTWAIT_) != 0;
            const Deadline deadline = MakeDeadline(socket->rx_timeout);

            size_t total = 0;
            while (true) {
                if (const s32 error = CheckInterrupted(table, socket); error != 0) {
                    return Fail(error);
                }

                if (socket->rx_bytes > 0) {
                    total += CopyReceived(socket, static_cast<u8 *>(buffer) + total, size - total, !peek);
                    stack::KickNetworkThread();
                    if (!wait_all || total == size) {
                        return Ok(static_cast<s32>(total));
                    }
                    continue;
                }

                /* Nothing to read. Is there ever going to be? */
                if (socket->rx_fin || socket->rx_shutdown) {
                    return Ok(static_cast<s32>(total));
                }
                if (socket->pending_error != 0) {
                    if (total > 0) {
                        return Ok(static_cast<s32>(total));
                    }
                    return Fail(TakePendingError(socket));
                }
                if (socket->state == State::Disconnected) {
                    return Ok(static_cast<s32>(total));
                }

                if (dont_wait) {
                    return total > 0 ? Ok(static_cast<s32>(total)) : Fail(Errno_EAGAIN);
                }
                if (!Wait(deadline)) {
                    return total > 0 ? Ok(static_cast<s32>(total)) : Fail(Errno_EAGAIN);
                }
            }
        }

        CallResult ReceiveUdp(DescriptorTable &table, Socket *socket, void *buffer, size_t size, s32 flags, void *out_address, size_t address_size, u32 *out_address_length) {
            const bool peek      = (flags & MSG_PEEK_) != 0;
            const bool dont_wait = socket->non_blocking || (flags & MSG_DONTWAIT_) != 0;
            const Deadline deadline = MakeDeadline(socket->rx_timeout);

            while (true) {
                if (const s32 error = CheckInterrupted(table, socket); error != 0) {
                    return Fail(error);
                }

                if (socket->datagram_count > 0) {
                    Datagram &datagram = socket->datagrams[socket->datagram_head];

                    /* A datagram is read whole or truncated; what does not fit is lost. */
                    const size_t copied = pbuf_copy_partial(datagram.data, buffer, static_cast<u16_t>(std::min<size_t>(size, datagram.data->tot_len)), 0);
                    if (out_address != nullptr || out_address_length != nullptr) {
                        WriteAddress(out_address, address_size, out_address_length, datagram.address, datagram.port);
                    }

                    if (!peek) {
                        socket->datagram_bytes -= datagram.data->tot_len;
                        pbuf_free(datagram.data);
                        socket->datagram_head = (socket->datagram_head + 1) % MaxQueuedDatagrams;
                        --socket->datagram_count;
                    }
                    return Ok(static_cast<s32>(copied));
                }

                if (socket->rx_shutdown) {
                    return Ok(0);
                }
                if (dont_wait || !Wait(deadline)) {
                    return Fail(Errno_EAGAIN);
                }
            }
        }

        CallResult SendTcp(DescriptorTable &table, Socket *socket, const void *buffer, size_t size, s32 flags) {
            if (socket->state == State::Fresh || socket->state == State::Listening || socket->state == State::Connecting) {
                return Fail(Errno_ENOTCONN);
            }

            const bool dont_wait = socket->non_blocking || (flags & MSG_DONTWAIT_) != 0;
            const Deadline deadline = MakeDeadline(socket->tx_timeout);
            const u8 *data = static_cast<const u8 *>(buffer);

            size_t total = 0;
            while (true) {
                if (const s32 error = CheckInterrupted(table, socket); error != 0) {
                    return Fail(error);
                }

                /* A connection that can no longer carry data: report why, once, then EPIPE. */
                if (socket->tx_shutdown || socket->state == State::Disconnected || socket->tcp == nullptr) {
                    if (total > 0) {
                        return Ok(static_cast<s32>(total));
                    }
                    const s32 error = TakePendingError(socket);
                    return Fail(error != 0 ? error : Errno_EPIPE);
                }
                if (total == size) {
                    return Ok(static_cast<s32>(total));
                }

                /* Queue as much as the send buffer takes. lwIP copies it, so the client's buffer is not needed afterwards. */
                bool queued = false;
                while (total < size) {
                    const size_t space = tcp_sndbuf(socket->tcp);
                    if (space == 0) {
                        break;
                    }
                    const u16_t chunk = static_cast<u16_t>(std::min<size_t>({ size - total, space, 0x8000 }));
                    const u8_t write_flags = TCP_WRITE_FLAG_COPY | ((total + chunk < size) ? TCP_WRITE_FLAG_MORE : 0);
                    const err_t err = tcp_write(socket->tcp, data + total, chunk, write_flags);
                    if (err == ERR_MEM) {
                        /* Out of segments or memory for now; acknowledgements will free some. */
                        break;
                    }
                    if (err != ERR_OK) {
                        if (total > 0) {
                            return Ok(static_cast<s32>(total));
                        }
                        return Fail(ConvertLwipError(err));
                    }
                    total += chunk;
                    queued = true;
                }
                if (queued) {
                    tcp_output(socket->tcp);
                    stack::KickNetworkThread();
                }
                if (total == size) {
                    return Ok(static_cast<s32>(total));
                }

                /* The buffer is full. A non-blocking send reports what it queued; a blocking one waits to queue the rest. */
                if (dont_wait) {
                    return total > 0 ? Ok(static_cast<s32>(total)) : Fail(Errno_EAGAIN);
                }
                if (!Wait(deadline)) {
                    return total > 0 ? Ok(static_cast<s32>(total)) : Fail(Errno_EAGAIN);
                }
            }
        }

        CallResult SendUdp(Socket *socket, const void *buffer, size_t size, const void *address, size_t address_size) {
            if (socket->tx_shutdown) {
                return Fail(Errno_EPIPE);
            }
            if (size > MaxUdpPayload) {
                return Fail(Errno_EMSGSIZE);
            }

            u32 destination = 0;
            u16 port = 0;
            const bool has_destination = address != nullptr && address_size > 0;
            if (has_destination) {
                if (const s32 error = ParseAddress(std::addressof(destination), std::addressof(port), address, address_size); error != 0) {
                    return Fail(error);
                }
            } else if (!socket->has_remote) {
                return Fail(Errno_EDESTADDRREQ);
            }

            struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, static_cast<u16_t>(size), PBUF_RAM);
            if (p == nullptr) {
                return Fail(Errno_ENOBUFS);
            }
            if (size > 0) {
                pbuf_take(p, buffer, static_cast<u16_t>(size));
            }

            err_t err;
            if (has_destination) {
                const ip_addr_t ip = ToIpAddr(destination);
                err = udp_sendto(socket->udp, p, std::addressof(ip), port);
            } else {
                err = udp_send(socket->udp, p);
            }
            pbuf_free(p);

            if (err != ERR_OK) {
                return Fail(ConvertLwipError(err));
            }

            /* lwIP binds an unbound socket to a port on its first send. */
            socket->bound = true;
            stack::KickNetworkThread();
            return Ok(static_cast<s32>(size));
        }

        CallResult ShutdownSocket(Socket *socket, s32 how) {
            if (how != SHUT_RD_ && how != SHUT_WR_ && how != SHUT_RDWR_) {
                return Fail(Errno_EINVAL);
            }
            if (socket->kind == Kind::Tcp && socket->state != State::Connected && socket->state != State::Disconnected) {
                return Fail(Errno_ENOTCONN);
            }

            if (how == SHUT_RD_ || how == SHUT_RDWR_) {
                socket->rx_shutdown = true;

                /* Unread data is discarded, and the peer is given the window back for it. */
                if (socket->kind == Kind::Tcp && socket->tcp != nullptr) {
                    size_t remaining = socket->rx_bytes;
                    while (remaining > 0) {
                        const u16_t step = static_cast<u16_t>(std::min<size_t>(remaining, 0xFFFF));
                        tcp_recved(socket->tcp, step);
                        remaining -= step;
                    }
                }
                FreeReceivedData(socket);
            }

            if ((how == SHUT_WR_ || how == SHUT_RDWR_) && !socket->tx_shutdown) {
                socket->tx_shutdown = true;

                /* Send FIN once what is queued has gone. The receive side stays open. */
                if (socket->kind == Kind::Tcp && socket->tcp != nullptr) {
                    tcp_shutdown(socket->tcp, 0, 1);
                    stack::KickNetworkThread();
                }
            }

            stack::NotifyActivity();
            return Ok();
        }

        template<typename T>
        bool ReadOption(T *out, const void *value, size_t size) {
            if (value == nullptr || size < sizeof(T)) {
                return false;
            }
            std::memcpy(out, value, sizeof(T));
            return true;
        }

        template<typename T>
        CallResult WriteOption(const T &value, void *out, size_t size, u32 *out_length) {
            const size_t length = std::min(size, sizeof(T));
            if (out != nullptr && length > 0) {
                std::memcpy(out, std::addressof(value), length);
            }
            *out_length = static_cast<u32>(length);
            return Ok();
        }

        /* lwIP keeps these option bits in the same place for TCP and UDP. */
        u8_t *GetIpOptions(Socket *socket) {
            if (socket->kind == Kind::Udp) {
                return socket->udp != nullptr ? std::addressof(socket->udp->so_options) : nullptr;
            }
            return socket->tcp != nullptr ? std::addressof(socket->tcp->so_options) : nullptr;
        }

    }

    CallResult Create(DescriptorTable &table, s32 domain, s32 type, s32 protocol) {
        stack::ScopedLock lk;

        /* A client that has been abandoned gets no new sockets: nothing would ever close them. */
        if (table.m_cancelled) {
            return Fail(Errno_ECANCELED);
        }
        if (domain != AF_INET_) {
            return Fail(Errno_EAFNOSUPPORT);
        }

        Kind kind;
        switch (type & SOCK_TypeMask) {
            case SOCK_STREAM_:
                if (protocol != 0 && protocol != IPPROTO_TCP_) {
                    return Fail(Errno_EPROTONOSUPPORT);
                }
                kind = Kind::Tcp;
                break;
            case SOCK_DGRAM_:
                if (protocol != 0 && protocol != IPPROTO_UDP_) {
                    return Fail(Errno_EPROTONOSUPPORT);
                }
                kind = Kind::Udp;
                break;
            case SOCK_RAW_:
            case SOCK_SEQPACKET_:
                /* Raw sockets (ICMP, for ping) are not implemented. */
                return Fail(Errno_EPROTONOSUPPORT);
            default:
                return Fail(Errno_ESOCKTNOSUPPORT);
        }
        if ((type & ~(SOCK_TypeMask | SOCK_CLOEXEC_ | SOCK_NONBLOCK_)) != 0) {
            return Fail(Errno_EINVAL);
        }

        const s32 fd = FindFreeDescriptor(table);
        if (fd < 0) {
            return Fail(Errno_EMFILE);
        }
        Socket *socket = AllocateSocket(kind);
        if (socket == nullptr) {
            return Fail(Errno_ENFILE);
        }

        if (kind == Kind::Tcp) {
            socket->tcp = tcp_new_ip_type(IPADDR_TYPE_V4);
            if (socket->tcp != nullptr) {
                AttachCallbacks(socket, socket->tcp);
            }
        } else {
            socket->udp = udp_new_ip_type(IPADDR_TYPE_V4);
            if (socket->udp != nullptr) {
                udp_recv(socket->udp, OnUdpReceived, socket);
            }
        }
        if (socket->tcp == nullptr && socket->udp == nullptr) {
            socket->in_use = false;
            return Fail(Errno_ENOBUFS);
        }

        socket->non_blocking     = (type & SOCK_NONBLOCK_) != 0;
        socket->descriptor_count = 1;
        table.m_sockets[fd]      = socket;
        return Ok(fd);
    }

    CallResult Close(DescriptorTable &table, s32 fd) {
        stack::ScopedLock lk;

        if (GetSocket(table, fd) == nullptr) {
            return Fail(Errno_EBADF);
        }
        ReleaseDescriptor(table, fd);
        return Ok();
    }

    CallResult Duplicate(DescriptorTable &table, s32 fd) {
        stack::ScopedLock lk;

        Socket *socket = GetSocket(table, fd);
        if (socket == nullptr) {
            return Fail(Errno_EBADF);
        }
        const s32 new_fd = FindFreeDescriptor(table);
        if (new_fd < 0) {
            return Fail(Errno_EMFILE);
        }

        ++socket->descriptor_count;
        table.m_sockets[new_fd] = socket;
        return Ok(new_fd);
    }

    CallResult Bind(DescriptorTable &table, s32 fd, const void *address, size_t address_size) {
        stack::ScopedLock lk;

        Socket *socket = GetSocket(table, fd);
        if (socket == nullptr) {
            return Fail(Errno_EBADF);
        }

        u32 ip;
        u16 port;
        if (const s32 error = ParseAddress(std::addressof(ip), std::addressof(port), address, address_size); error != 0) {
            return Fail(error);
        }
        if (socket->bound || (socket->kind == Kind::Tcp && socket->state != State::Fresh)) {
            return Fail(Errno_EINVAL);
        }

        /* lwIP would accept any address; a socket can only be bound to one this host has. */
        if (ip != 0 && !stack::IsLocalAddress(ip)) {
            return Fail(Errno_EADDRNOTAVAIL);
        }

        const ip_addr_t lwip_address = ToIpAddr(ip);
        const err_t err = (socket->kind == Kind::Tcp) ? tcp_bind(socket->tcp, std::addressof(lwip_address), port) : udp_bind(socket->udp, std::addressof(lwip_address), port);
        if (err != ERR_OK) {
            return Fail(ConvertLwipError(err));
        }

        socket->bound = true;
        RefreshAddresses(socket);
        return Ok();
    }

    CallResult Connect(DescriptorTable &table, s32 fd, const void *address, size_t address_size) {
        stack::ScopedLock lk;

        Socket *socket = GetSocket(table, fd);
        if (socket == nullptr) {
            return Fail(Errno_EBADF);
        }
        SocketUse use(socket);

        u32 ip;
        u16 port;
        if (const s32 error = ParseAddress(std::addressof(ip), std::addressof(port), address, address_size); error != 0) {
            return Fail(error);
        }
        const ip_addr_t lwip_address = ToIpAddr(ip);

        if (socket->kind == Kind::Udp) {
            /* For UDP this only records the peer: the default destination, and the only source accepted. */
            const err_t err = udp_connect(socket->udp, std::addressof(lwip_address), port);
            if (err != ERR_OK) {
                return Fail(ConvertLwipError(err));
            }
            socket->remote_address = ip;
            socket->remote_port    = port;
            socket->has_remote     = true;
            socket->bound          = true;
            RefreshAddresses(socket);
            return Ok();
        }

        switch (socket->state) {
            case State::Fresh:        break;
            case State::Connecting:   return Fail(Errno_EALREADY);
            case State::Connected:    return Fail(Errno_EISCONN);
            case State::Listening:    return Fail(Errno_EOPNOTSUPP);
            case State::Disconnected: {
                /* A non-blocking connect that failed reports its error here once, if nobody took it from SO_ERROR. */
                const s32 error = TakePendingError(socket);
                return Fail(error != 0 ? error : Errno_ECONNABORTED);
            }
        }
        if (ip == 0 || port == 0) {
            return Fail(Errno_EADDRNOTAVAIL);
        }

        socket->state = State::Connecting;
        const err_t err = tcp_connect(socket->tcp, std::addressof(lwip_address), port, OnTcpConnected);
        if (err != ERR_OK) {
            socket->state = State::Fresh;
            return Fail(ConvertLwipError(err));
        }
        socket->bound = true;
        RefreshAddresses(socket);
        stack::KickNetworkThread();

        if (socket->non_blocking) {
            return Fail(Errno_EINPROGRESS);
        }

        /* lwIP gives up on its own after its SYN retransmissions; SO_SNDTIMEO can make that sooner. */
        const Deadline deadline = MakeDeadline(socket->tx_timeout);
        while (socket->state == State::Connecting) {
            if (const s32 error = CheckInterrupted(table, socket); error != 0) {
                return Fail(error);
            }
            if (!Wait(deadline)) {
                /* The attempt carries on, as after a non-blocking connect. */
                return Fail(Errno_EINPROGRESS);
            }
        }
        if (const s32 error = CheckInterrupted(table, socket); error != 0) {
            return Fail(error);
        }

        if (socket->state == State::Connected) {
            return Ok();
        }
        const s32 error = TakePendingError(socket);
        return Fail(error != 0 ? error : Errno_ECONNREFUSED);
    }

    CallResult Listen(DescriptorTable &table, s32 fd, s32 backlog) {
        stack::ScopedLock lk;

        Socket *socket = GetSocket(table, fd);
        if (socket == nullptr) {
            return Fail(Errno_EBADF);
        }
        if (socket->kind != Kind::Tcp) {
            return Fail(Errno_EOPNOTSUPP);
        }

        const int limit = std::clamp<s32>(backlog, 1, MaxBacklog);
        if (socket->state == State::Listening) {
            /* Calling listen again only changes the backlog. */
            socket->backlog = limit;
            return Ok();
        }
        if (socket->state != State::Fresh) {
            return Fail(Errno_EINVAL);
        }

        /* Listening on an unbound socket picks a port, as on BSD. */
        if (!socket->bound) {
            if (const err_t err = tcp_bind(socket->tcp, IP4_ADDR_ANY, 0); err != ERR_OK) {
                return Fail(ConvertLwipError(err));
            }
            socket->bound = true;
        }

        /* lwIP replaces the connection block with a smaller listening one. On failure the original is untouched. */
        err_t err = ERR_OK;
        struct tcp_pcb *listen_pcb = tcp_listen_with_backlog_and_err(socket->tcp, static_cast<u8_t>(limit), std::addressof(err));
        if (listen_pcb == nullptr) {
            return Fail(ConvertLwipError(err));
        }

        socket->tcp     = listen_pcb;
        socket->state   = State::Listening;
        socket->backlog = limit;
        tcp_arg(listen_pcb, socket);
        tcp_accept(listen_pcb, OnTcpAccepted);
        RefreshAddresses(socket);
        return Ok();
    }

    CallResult Accept(DescriptorTable &table, s32 fd, void *out_address, size_t address_size, u32 *out_address_length) {
        stack::ScopedLock lk;

        Socket *socket = GetSocket(table, fd);
        if (socket == nullptr) {
            return Fail(Errno_EBADF);
        }
        SocketUse use(socket);

        if (socket->kind != Kind::Tcp) {
            return Fail(Errno_EOPNOTSUPP);
        }
        if (socket->state != State::Listening) {
            return Fail(Errno_EINVAL);
        }

        const Deadline deadline = MakeDeadline(socket->rx_timeout);
        while (true) {
            if (const s32 error = CheckInterrupted(table, socket); error != 0) {
                return Fail(error);
            }

            if (socket->accept_count > 0) {
                const s32 new_fd = FindFreeDescriptor(table);
                if (new_fd < 0) {
                    return Fail(Errno_EMFILE);
                }

                Socket *child = socket->accept_queue[0];
                std::memmove(socket->accept_queue, socket->accept_queue + 1, (socket->accept_count - 1) * sizeof(socket->accept_queue[0]));
                --socket->accept_count;

                /* The connection may already have been reset by its peer; the client finds that out when it uses it. */
                child->listener = nullptr;
                if (child->tcp != nullptr) {
                    tcp_backlog_accepted(child->tcp);
                }
                child->descriptor_count = 1;
                table.m_sockets[new_fd] = child;

                WriteAddress(out_address, address_size, out_address_length, child->remote_address, child->remote_port);
                return Ok(new_fd);
            }

            if (socket->non_blocking || !Wait(deadline)) {
                return Fail(Errno_EAGAIN);
            }
        }
    }

    CallResult Receive(DescriptorTable &table, s32 fd, void *buffer, size_t size, s32 flags, void *out_address, size_t address_size, u32 *out_address_length) {
        stack::ScopedLock lk;

        if (out_address_length != nullptr) {
            *out_address_length = 0;
        }

        Socket *socket = GetSocket(table, fd);
        if (socket == nullptr) {
            return Fail(Errno_EBADF);
        }
        SocketUse use(socket);

        /* The result is a 32-bit count. */
        size = std::min<size_t>(size, MaxTransferSize);

        if (socket->kind == Kind::Udp) {
            return ReceiveUdp(table, socket, buffer, size, flags, out_address, address_size, out_address_length);
        }
        return ReceiveTcp(table, socket, buffer, size, flags);
    }

    CallResult Send(DescriptorTable &table, s32 fd, const void *buffer, size_t size, s32 flags, const void *address, size_t address_size) {
        stack::ScopedLock lk;

        Socket *socket = GetSocket(table, fd);
        if (socket == nullptr) {
            return Fail(Errno_EBADF);
        }
        SocketUse use(socket);

        if (socket->kind == Kind::Udp) {
            return SendUdp(socket, buffer, size, address, address_size);
        }
        /* A destination given for a stream socket is ignored. The result is a 32-bit count, so a larger */
        /* request is sent in part, as a short write. */
        return SendTcp(table, socket, buffer, std::min<size_t>(size, MaxTransferSize), flags);
    }

    CallResult Shutdown(DescriptorTable &table, s32 fd, s32 how) {
        stack::ScopedLock lk;

        Socket *socket = GetSocket(table, fd);
        if (socket == nullptr) {
            return Fail(Errno_EBADF);
        }
        return ShutdownSocket(socket, how);
    }

    CallResult ShutdownAll(DescriptorTable &table, s32 how) {
        stack::ScopedLock lk;

        if (how != SHUT_RD_ && how != SHUT_WR_ && how != SHUT_RDWR_) {
            return Fail(Errno_EINVAL);
        }
        for (Socket *socket : table.m_sockets) {
            if (socket != nullptr) {
                /* Sockets that are not connected have nothing to shut down. */
                static_cast<void>(ShutdownSocket(socket, how));
            }
        }
        return Ok();
    }

    CallResult GetSockName(DescriptorTable &table, s32 fd, void *out_address, size_t address_size, u32 *out_address_length) {
        stack::ScopedLock lk;

        *out_address_length = 0;
        Socket *socket = GetSocket(table, fd);
        if (socket == nullptr) {
            return Fail(Errno_EBADF);
        }

        RefreshAddresses(socket);
        WriteAddress(out_address, address_size, out_address_length, socket->local_address, socket->local_port);
        return Ok();
    }

    CallResult GetPeerName(DescriptorTable &table, s32 fd, void *out_address, size_t address_size, u32 *out_address_length) {
        stack::ScopedLock lk;

        *out_address_length = 0;
        Socket *socket = GetSocket(table, fd);
        if (socket == nullptr) {
            return Fail(Errno_EBADF);
        }

        const bool connected = (socket->kind == Kind::Udp) ? socket->has_remote : (socket->state == State::Connected);
        if (!connected) {
            return Fail(Errno_ENOTCONN);
        }

        WriteAddress(out_address, address_size, out_address_length, socket->remote_address, socket->remote_port);
        return Ok();
    }

    CallResult GetSockOpt(DescriptorTable &table, s32 fd, s32 level, s32 name, void *out_value, size_t value_size, u32 *out_value_length) {
        stack::ScopedLock lk;

        *out_value_length = 0;
        Socket *socket = GetSocket(table, fd);
        if (socket == nullptr) {
            return Fail(Errno_EBADF);
        }

        const auto WriteInt = [&](s32 value) { return WriteOption(value, out_value, value_size, out_value_length); };
        const u8_t *ip_options = GetIpOptions(socket);

        if (level == SOL_SOCKET_) {
            switch (name) {
                case SO_ACCEPTCONN_: return WriteInt(socket->state == State::Listening);
                case SO_REUSEADDR_:
                case SO_REUSEPORT_:  return WriteInt(ip_options != nullptr && (*ip_options & SOF_REUSEADDR) != 0);
                case SO_KEEPALIVE_:  return WriteInt(ip_options != nullptr && (*ip_options & SOF_KEEPALIVE) != 0);
                case SO_BROADCAST_:  return WriteInt(ip_options != nullptr && (*ip_options & SOF_BROADCAST) != 0);
                case SO_LINGER_:     return WriteOption(socket->linger, out_value, value_size, out_value_length);
                case SO_SNDBUF_:     return WriteInt(socket->sndbuf);
                case SO_RCVBUF_:     return WriteInt(socket->rcvbuf);
                case SO_SNDTIMEO_:   return WriteOption(ToTimeVal(socket->tx_timeout), out_value, value_size, out_value_length);
                case SO_RCVTIMEO_:   return WriteOption(ToTimeVal(socket->rx_timeout), out_value, value_size, out_value_length);
                case SO_ERROR_:      return WriteInt(TakePendingError(socket));
                case SO_TYPE_:       return WriteInt(socket->kind == Kind::Tcp ? SOCK_STREAM_ : SOCK_DGRAM_);
                default:             return Fail(Errno_ENOPROTOOPT);
            }
        }

        if (level == IPPROTO_TCP_ && socket->kind == Kind::Tcp) {
            /* These live in the connection, which a listening or finished socket does not have. */
            if (socket->tcp == nullptr || socket->state == State::Listening) {
                return Fail(Errno_EINVAL);
            }
            switch (name) {
                case TCP_NODELAY_:   return WriteInt(tcp_nagle_disabled(socket->tcp) != 0);
                case TCP_MAXSEG_:    return WriteInt(tcp_mss(socket->tcp));
                case TCP_KEEPIDLE_:  return WriteInt(socket->tcp->keep_idle / 1000);
                case TCP_KEEPINTVL_: return WriteInt(socket->tcp->keep_intvl / 1000);
                case TCP_KEEPCNT_:   return WriteInt(socket->tcp->keep_cnt);
                default:             return Fail(Errno_ENOPROTOOPT);
            }
        }

        if (level == IPPROTO_IP_) {
            const bool is_udp = socket->kind == Kind::Udp;
            if ((is_udp && socket->udp == nullptr) || (!is_udp && socket->tcp == nullptr)) {
                return Fail(Errno_EINVAL);
            }
            switch (name) {
                case IP_TOS_: return WriteInt(is_udp ? socket->udp->tos : socket->tcp->tos);
                case IP_TTL_: return WriteInt(is_udp ? socket->udp->ttl : socket->tcp->ttl);
                default:      return Fail(Errno_ENOPROTOOPT);
            }
        }

        return Fail(Errno_ENOPROTOOPT);
    }

    CallResult SetSockOpt(DescriptorTable &table, s32 fd, s32 level, s32 name, const void *value, size_t value_size) {
        stack::ScopedLock lk;

        Socket *socket = GetSocket(table, fd);
        if (socket == nullptr) {
            return Fail(Errno_EBADF);
        }

        s32 int_value = 0;
        const bool has_int = ReadOption(std::addressof(int_value), value, value_size);
        u8_t *ip_options = GetIpOptions(socket);

        const auto SetIpOption = [&](u8_t bit) {
            if (!has_int) {
                return Fail(Errno_EINVAL);
            }
            if (ip_options == nullptr) {
                return Fail(Errno_EINVAL);
            }
            if (int_value != 0) {
                *ip_options |= bit;
            } else {
                *ip_options &= static_cast<u8_t>(~bit);
            }
            return Ok();
        };

        if (level == SOL_SOCKET_) {
            switch (name) {
                case SO_REUSEADDR_:
                case SO_REUSEPORT_: return SetIpOption(SOF_REUSEADDR);
                case SO_KEEPALIVE_: return SetIpOption(SOF_KEEPALIVE);
                case SO_BROADCAST_: return SetIpOption(SOF_BROADCAST);
                case SO_LINGER_: {
                    Linger linger;
                    if (!ReadOption(std::addressof(linger), value, value_size)) {
                        return Fail(Errno_EINVAL);
                    }
                    socket->linger = linger;
                    return Ok();
                }
                case SO_SNDBUF_:
                case SO_RCVBUF_:
                    /* Recorded and reported back. The buffers themselves are lwIP's and fixed in size. */
                    if (!has_int || int_value <= 0) {
                        return Fail(Errno_EINVAL);
                    }
                    (name == SO_SNDBUF_ ? socket->sndbuf : socket->rcvbuf) = int_value;
                    return Ok();
                case SO_SNDTIMEO_:
                case SO_RCVTIMEO_: {
                    TimeVal tv;
                    if (!ReadOption(std::addressof(tv), value, value_size) || tv.sec < 0 || tv.usec < 0 || tv.usec >= 1'000'000 || tv.sec > (INT64_MAX - tv.usec * 1000) / 1'000'000'000) {
                        return Fail(Errno_EINVAL);
                    }
                    (name == SO_SNDTIMEO_ ? socket->tx_timeout : socket->rx_timeout) = FromTimeVal(tv);
                    return Ok();
                }
                default:
                    return Fail(Errno_ENOPROTOOPT);
            }
        }

        if (level == IPPROTO_TCP_ && socket->kind == Kind::Tcp) {
            if (!has_int || socket->tcp == nullptr || socket->state == State::Listening) {
                return Fail(Errno_EINVAL);
            }
            switch (name) {
                case TCP_NODELAY_:
                    if (int_value != 0) {
                        tcp_nagle_disable(socket->tcp);
                    } else {
                        tcp_nagle_enable(socket->tcp);
                    }
                    return Ok();
                case TCP_KEEPIDLE_:
                case TCP_KEEPINTVL_:
                    if (int_value <= 0 || int_value > 0x7FFFFFFF / 1000) {
                        return Fail(Errno_EINVAL);
                    }
                    (name == TCP_KEEPIDLE_ ? socket->tcp->keep_idle : socket->tcp->keep_intvl) = static_cast<u32_t>(int_value) * 1000;
                    return Ok();
                case TCP_KEEPCNT_:
                    if (int_value <= 0) {
                        return Fail(Errno_EINVAL);
                    }
                    socket->tcp->keep_cnt = static_cast<u32_t>(int_value);
                    return Ok();
                default:
                    return Fail(Errno_ENOPROTOOPT);
            }
        }

        if (level == IPPROTO_IP_) {
            const bool is_udp = socket->kind == Kind::Udp;
            if (!has_int || int_value < 0 || int_value > 255 || (is_udp && socket->udp == nullptr) || (!is_udp && socket->tcp == nullptr)) {
                return Fail(Errno_EINVAL);
            }
            switch (name) {
                case IP_TOS_: (is_udp ? socket->udp->tos : socket->tcp->tos) = static_cast<u8_t>(int_value); return Ok();
                case IP_TTL_: (is_udp ? socket->udp->ttl : socket->tcp->ttl) = static_cast<u8_t>(int_value); return Ok();
                default:      return Fail(Errno_ENOPROTOOPT);
            }
        }

        return Fail(Errno_ENOPROTOOPT);
    }

    CallResult Fcntl(DescriptorTable &table, s32 fd, s32 command, s32 flags) {
        stack::ScopedLock lk;

        Socket *socket = GetSocket(table, fd);
        if (socket == nullptr) {
            return Fail(Errno_EBADF);
        }

        switch (command) {
            case F_GETFL_:
                return Ok(socket->non_blocking ? O_NONBLOCK_ : 0);
            case F_SETFL_:
                /* O_NONBLOCK is the only flag there is. */
                if ((flags & ~O_NONBLOCK_) != 0) {
                    return Fail(Errno_EINVAL);
                }
                socket->non_blocking = (flags & O_NONBLOCK_) != 0;
                return Ok();
            default:
                return Fail(Errno_EOPNOTSUPP);
        }
    }

    CallResult Ioctl(DescriptorTable &table, s32 fd, u32 request, const void *in, size_t in_size, void *out, size_t out_size) {
        stack::ScopedLock lk;

        Socket *socket = GetSocket(table, fd);
        if (socket == nullptr) {
            return Fail(Errno_EBADF);
        }

        const auto WriteInt = [&](s32 value) {
            if (out == nullptr || out_size < sizeof(value)) {
                return Fail(Errno_EFAULT);
            }
            std::memcpy(out, std::addressof(value), sizeof(value));
            return Ok();
        };

        switch (request) {
            case FIONREAD_:
                /* Bytes a read would return: all of a stream's unread data, or the next datagram. */
                if (socket->kind == Kind::Udp) {
                    return WriteInt(socket->datagram_count > 0 ? socket->datagrams[socket->datagram_head].data->tot_len : 0);
                }
                return WriteInt(static_cast<s32>(socket->rx_bytes));
            case FIONWRITE_:
                /* Bytes queued and not yet acknowledged. */
                return WriteInt((socket->kind == Kind::Tcp && socket->tcp != nullptr && socket->state == State::Connected) ? static_cast<s32>(TCP_SND_BUF - tcp_sndbuf(socket->tcp)) : 0);
            case FIONSPACE_:
                return WriteInt((socket->kind == Kind::Tcp && socket->tcp != nullptr && socket->state == State::Connected) ? static_cast<s32>(tcp_sndbuf(socket->tcp)) : (socket->kind == Kind::Udp ? static_cast<s32>(MaxUdpPayload) : 0));
            case FIONBIO_: {
                s32 value;
                if (!ReadOption(std::addressof(value), in, in_size)) {
                    return Fail(Errno_EFAULT);
                }
                socket->non_blocking = value != 0;
                return Ok();
            }
            default:
                /* Interface and BPF requests are not implemented. */
                return Fail(Errno_EOPNOTSUPP);
        }
    }

    CallResult Poll(DescriptorTable &table, void *fds, u32 count, s32 timeout_ms) {
        stack::ScopedLock lk;

        if (count > MaxPollDescriptors || (count > 0 && fds == nullptr)) {
            return Fail(Errno_EINVAL);
        }

        const Deadline deadline = (timeout_ms < 0) ? Deadline{ true, 0 } : Deadline{ false, ::svcGetSystemTick() + ::armNsToTicks(static_cast<u64>(timeout_ms) * 1000000) };

        u8 *entries = static_cast<u8 *>(fds);
        while (true) {
            if (table.m_cancelled) {
                return Fail(Errno_ECANCELED);
            }

            /* Descriptors are looked up afresh on every pass, so one closed meanwhile shows up as POLLNVAL. */
            s32 ready = 0;
            for (u32 i = 0; i < count; ++i) {
                PollFd entry;
                std::memcpy(std::addressof(entry), entries + i * sizeof(entry), sizeof(entry));

                entry.revents = 0;
                if (entry.fd >= 0) {
                    if (Socket *socket = GetSocket(table, entry.fd); socket != nullptr) {
                        entry.revents = GetPollEvents(socket, entry.events);
                    } else {
                        entry.revents = POLLNVAL_;
                    }
                }
                if (entry.revents != 0) {
                    ++ready;
                }

                std::memcpy(entries + i * sizeof(entry), std::addressof(entry), sizeof(entry));
            }

            if (ready > 0 || timeout_ms == 0) {
                return Ok(ready);
            }
            if (!Wait(deadline)) {
                return Ok(0);
            }
        }
    }

    void Abandon(DescriptorTable &table) {
        stack::ScopedLock lk;

        for (s32 fd = 0; fd < MaxDescriptorsPerClient; ++fd) {
            if (table.m_sockets[fd] != nullptr) {
                ReleaseDescriptor(table, fd);
            }
        }

        /* From here on every descriptor lookup fails and every waiting call returns. */
        table.m_cancelled = true;
        stack::NotifyActivity();
    }

    void CloseAll(DescriptorTable &table) {
        stack::ScopedLock lk;

        for (s32 fd = 0; fd < MaxDescriptorsPerClient; ++fd) {
            if (table.m_sockets[fd] != nullptr) {
                ReleaseDescriptor(table, fd);
            }
        }
    }

}

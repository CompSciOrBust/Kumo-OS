/*
 * BSD sockets on lwIP's callback API.
 *
 * lwIP supplies TCP, UDP and IP; this layer supplies what a socket is to a
 * client: a descriptor, queues of received data, blocking and non-blocking
 * calls, timeouts, options, and poll. lwIP's own sockets layer is not used:
 * it wants a thread per blocked call that cannot be cancelled, and this
 * layer has to drop a dead client's calls and descriptors cleanly.
 *
 * Every function here takes the stack lock itself. A call that has to wait
 * sleeps on the stack's activity condition, releasing the lock, and is woken
 * when any socket changes; it then re-checks its own condition.
 *
 * Results are (value, error): the value is what the BSD call returns, -1 on
 * failure, and the error is a Linux errno (wire::Errno), zero on success.
 */
#pragma once
#include <kumo/prelude.hpp>
#include "bsd_wire.hpp"

namespace kumo::horizon::hznet::socket {

    struct Socket;

    /* Descriptors a single client can have open. */
    constexpr inline int MaxDescriptorsPerClient = 128;

    /* Sockets across all clients. */
    constexpr inline int MaxSockets = 384;

    struct CallResult {
        s32 value;
        s32 error;
    };

    /* One client's descriptors. Descriptor numbers are private to a client, as file descriptors are to a process. */
    class DescriptorTable {
        KUMO_NON_COPYABLE(DescriptorTable);
        KUMO_NON_MOVEABLE(DescriptorTable);
        public:
            Socket *m_sockets[MaxDescriptorsPerClient];
            bool m_cancelled;
        public:
            constexpr DescriptorTable() : m_sockets(), m_cancelled(false) { /* ... */ }
    };

    CallResult Create(DescriptorTable &table, s32 domain, s32 type, s32 protocol);
    CallResult Close(DescriptorTable &table, s32 fd);
    CallResult Duplicate(DescriptorTable &source, s32 fd, DescriptorTable &destination);

    CallResult Bind(DescriptorTable &table, s32 fd, const void *address, size_t address_size);
    CallResult Connect(DescriptorTable &table, s32 fd, const void *address, size_t address_size);
    CallResult Listen(DescriptorTable &table, s32 fd, s32 backlog);
    CallResult Accept(DescriptorTable &table, s32 fd, void *out_address, size_t address_size, u32 *out_address_length);

    /* `out_address` may be null. For Receive on a stream socket the address is not filled in. */
    CallResult Receive(DescriptorTable &table, s32 fd, void *buffer, size_t size, s32 flags, void *out_address, size_t address_size, u32 *out_address_length);
    /* `address` may be null: the socket must then be connected. */
    CallResult Send(DescriptorTable &table, s32 fd, const void *buffer, size_t size, s32 flags, const void *address, size_t address_size);

    CallResult Shutdown(DescriptorTable &table, s32 fd, s32 how);
    CallResult ShutdownAll(DescriptorTable &table, s32 how);

    CallResult GetSockName(DescriptorTable &table, s32 fd, void *out_address, size_t address_size, u32 *out_address_length);
    CallResult GetPeerName(DescriptorTable &table, s32 fd, void *out_address, size_t address_size, u32 *out_address_length);

    CallResult GetSockOpt(DescriptorTable &table, s32 fd, s32 level, s32 name, void *out_value, size_t value_size, u32 *out_value_length);
    CallResult SetSockOpt(DescriptorTable &table, s32 fd, s32 level, s32 name, const void *value, size_t value_size);

    CallResult Fcntl(DescriptorTable &table, s32 fd, s32 command, s32 flags);
    CallResult Ioctl(DescriptorTable &table, s32 fd, u32 request, const void *in, size_t in_size, void *out, size_t out_size);

    /* `fds` is read and written in place; it need not be aligned. A negative timeout waits forever. */
    CallResult Poll(DescriptorTable &table, void *fds, u32 count, s32 timeout_ms);

    /* The client is gone: fail every call of its that is waiting, now and in future, and close all its descriptors. */
    void Abandon(DescriptorTable &table);

    /* Close all descriptors of a client that has disconnected in an orderly way. */
    void CloseAll(DescriptorTable &table);

}

/*
 * Clients of the socket service.
 *
 * A client is one RegisterClient call: normally one process. It owns a
 * descriptor table, and every session cloned from the one that registered
 * shares it, which is how libnx issues calls in parallel.
 *
 * A client also opens a second, separate session and calls StartMonitoring
 * on it, then never uses it again. That session exists to be closed: when
 * the process dies the kernel closes it, and the service learns that the
 * client is gone even while one of its calls is blocked. See Abandon.
 */
#pragma once
#include <kumo/prelude.hpp>
#include "../socket/bsd_socket.hpp"

namespace kumo::horizon::hznet::ipc {

    constexpr inline int MaxClients = 32;

    class Client {
        public:
            socket::DescriptorTable m_table;
            u64 m_process_id;
            int m_reference_count;
            bool m_in_use;
            bool m_registered;
            bool m_monitored;
    };

    /* A new client holding one reference, or nullptr when there are too many. */
    Client *CreateClient();

    /* Record the process that registered a client. */
    bool RegisterClient(Client *client, u64 process_id);

    /* The kernel-provided PID recorded during registration, or zero before registration. */
    u64 GetClientProcessId(Client *client);

    /* Hold the sole registered client for a PID while using its descriptor table. */
    Client *AcquireClientByProcessId(u64 process_id);

    /* The registered, not yet monitored client of a process, with a reference added; nullptr if none. */
    Client *AcquireClientForMonitoring(u64 process_id);

    /* Drop a reference. The last one closes the client's sockets. */
    void ReleaseClient(Client *client);

    /* The client's process is gone: fail its waiting calls and close its sockets now. */
    void AbandonClient(Client *client);

}

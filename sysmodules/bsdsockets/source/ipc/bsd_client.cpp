#include <kumo/prelude.hpp>
#include <kumo/mutex.hpp>
#include <kumo/storage.hpp>
#include "bsd_client.hpp"

namespace kumo::horizon::hznet::ipc {

    namespace {

        constinit kumo::Mutex g_mutex;
        constinit kumo::Storage<Client> g_client_storage[MaxClients] = {};
        constinit bool g_client_allocated[MaxClients] = {};

    }

    Client *CreateClient() {
        std::scoped_lock lk(g_mutex);

        for (int i = 0; i < MaxClients; ++i) {
            if (!g_client_allocated[i]) {
                g_client_allocated[i] = true;

                Client *client = g_client_storage[i].construct();
                client->m_process_id      = UINT64_MAX;
                client->m_reference_count = 1;
                client->m_in_use          = true;
                client->m_registered      = false;
                client->m_monitored       = false;
                return client;
            }
        }
        return nullptr;
    }

    bool RegisterClient(Client *client, u64 process_id) {
        std::scoped_lock lk(g_mutex);
        if (client->m_registered) {
            return client->m_process_id == process_id;
        }
        client->m_process_id = process_id;
        client->m_registered = true;
        return true;
    }

    u64 GetClientProcessId(Client *client) {
        std::scoped_lock lk(g_mutex);
        return client->m_registered ? client->m_process_id : 0;
    }

    Client *AcquireClientByProcessId(u64 process_id) {
        std::scoped_lock lk(g_mutex);

        Client *match = nullptr;
        for (int i = 0; i < MaxClients; ++i) {
            if (!g_client_allocated[i]) {
                continue;
            }
            Client *client = g_client_storage[i].get();
            if (!client->m_registered || client->m_process_id != process_id || client->m_reference_count == 0) {
                continue;
            }
            /* A PID alone cannot distinguish independent registrations. */
            if (match != nullptr) {
                return nullptr;
            }
            match = client;
        }
        if (match != nullptr) {
            ++match->m_reference_count;
        }
        return match;
    }

    Client *AcquireClientForMonitoring(u64 process_id) {
        std::scoped_lock lk(g_mutex);

        for (int i = 0; i < MaxClients; ++i) {
            if (!g_client_allocated[i]) {
                continue;
            }
            Client *client = g_client_storage[i].get();
            if (client->m_registered && !client->m_monitored && client->m_process_id == process_id && client->m_reference_count > 0) {
                client->m_monitored = true;
                ++client->m_reference_count;
                return client;
            }
        }
        return nullptr;
    }

    void ReleaseClient(Client *client) {
        {
            std::scoped_lock lk(g_mutex);
            if (--client->m_reference_count > 0) {
                return;
            }
        }

        /* Nothing can reach the client any more, so no lock is needed to finish it off. */
        socket::CloseAll(client->m_table);

        std::scoped_lock lk(g_mutex);
        for (int i = 0; i < MaxClients; ++i) {
            if (g_client_allocated[i] && g_client_storage[i].get() == client) {
                g_client_storage[i].destroy();
                g_client_allocated[i] = false;
                return;
            }
        }
        KUMO_ABORT("unknown BSD client");
    }

    void AbandonClient(Client *client) {
        socket::Abandon(client->m_table);
    }

}

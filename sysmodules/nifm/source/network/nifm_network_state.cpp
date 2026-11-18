#include <kumo/prelude.hpp>
#include <kumo/mutex.hpp>
#include "nifm_network_state.hpp"
#include "nifm_bsdcfg_client.hpp"
#include "../nifm_log.hpp"

namespace kumo::horizon::nifm::network {

    namespace {

        /* The interface nifm manages: the first network device the sockets module found. */
        constexpr u32 WiredInterfaceIndex = 0;

        constinit ::kumo::Mutex g_mutex;
        constinit NetworkState g_state = {};
        constinit ChangeHandler g_handler = nullptr;

        /* How often to look again when nothing signals a change, and how long to wait before retrying */
        /* a connection to the sockets module. The event makes the first of these a safety net only. */
        constexpr u64 PollIntervalNs  = 1'000'000'000;
        constexpr s64 RetryIntervalNs = 200'000'000;

        constexpr size_t ThreadStackSize = 16 * 1024;
        alignas(0x1000) constinit u8 g_thread_stack[ThreadStackSize];
        constinit ::Thread g_thread;

        NetworkState ToNetworkState(const IfState &s) {
            NetworkState state = {};
            state.present     = true;
            state.link_up     = (s.flags & IfStateFlag_LinkUp)     != 0;
            state.configured  = (s.flags & IfStateFlag_Configured) != 0;
            state.has_address = (s.flags & IfStateFlag_HasAddress) != 0;
            state.dhcp        = (s.flags & IfStateFlag_Dhcp)       != 0;
            state.mtu         = static_cast<u16>(s.mtu);
            std::memcpy(state.mac, s.mac, sizeof(state.mac));
            std::memcpy(state.address, std::addressof(s.address), 4);
            std::memcpy(state.netmask, std::addressof(s.netmask), 4);
            std::memcpy(state.gateway, std::addressof(s.gateway), 4);
            std::memcpy(state.dns[0],  std::addressof(s.dns[0]), 4);
            std::memcpy(state.dns[1],  std::addressof(s.dns[1]), 4);
            return state;
        }

        /* Store a new snapshot, and tell the handler if it differs from the last. */
        void Publish(const NetworkState &next) {
            {
                std::scoped_lock lk(g_mutex);
                if (std::memcmp(std::addressof(next), std::addressof(g_state), sizeof(next)) == 0) {
                    return;
                }
                g_state = next;
            }

            Log("[nifm] network: %s, link %s, %s\n", next.present ? "device present" : "no device", next.link_up ? "up" : "down",
                next.has_address ? "has an address" : "no address");
            g_handler(next);
        }

        void MonitorThread(void *) {
            /* No sockets module, no network: until bsdcfg answers, requests fail as "not connected". */
            KUMO_ABORT_RESULT(::smInitialize());
            while (R_FAILED(ConnectToBsdConfig())) {
                ::svcSleepThread(RetryIntervalNs);
            }

            /* The decision to bring the interface up, and how, is nifm's. With no settings service to read */
            /* a profile from, it is the profile this module builds in; see nifm_profile.hpp. */
            ::Event change_event{};
            bool have_event = false;
            if (::Handle handle; R_SUCCEEDED(SetIfUpDhcpWithEvent(std::addressof(handle), WiredInterfaceIndex))) {
                ::eventLoadRemote(&change_event, handle, false);
                have_event = true;
            } else {
                Log("[nifm] bsdcfg: could not bring the interface up; a system without a network device looks like this\n");
            }

            while (true) {
                IfState raw = {};
                NetworkState next = {};
                if (R_SUCCEEDED(GetIfState(std::addressof(raw), WiredInterfaceIndex))) {
                    next = ToNetworkState(raw);
                }
                Publish(next);

                if (have_event) {
                    ::eventWait(&change_event, PollIntervalNs);
                    ::eventClear(&change_event);
                } else {
                    ::svcSleepThread(PollIntervalNs);
                }
            }
        }

    }

    void StartMonitor(ChangeHandler handler) {
        g_handler = handler;

        s32 priority = 0;
        KUMO_ABORT_RESULT(::svcGetThreadPriority(&priority, CUR_THREAD_HANDLE));
        KUMO_ABORT_RESULT(::threadCreate(&g_thread, MonitorThread, nullptr, g_thread_stack, ThreadStackSize, priority, -2));
        KUMO_ABORT_RESULT(::threadStart(&g_thread));
    }

    NetworkState GetState() {
        std::scoped_lock lk(g_mutex);
        return g_state;
    }

}

#include <kumo/prelude.hpp>
#include <kumo/mutex.hpp>
#include <kumo/storage.hpp>
#include "bsd_stack.hpp"
#include "../bsd_log.hpp"
#include "../netdev/bsd_net_device_registry.hpp"

extern "C" {
    #include "lwip/init.h"
    #include "lwip/netif.h"
    #include "lwip/pbuf.h"
    #include "lwip/timeouts.h"
    #include "lwip/dhcp.h"
    #include "lwip/dns.h"
    #include "lwip/etharp.h"
    #include "lwip/ip4_addr.h"
    #include "netif/ethernet.h"
}

namespace kumo::horizon::hznet::stack {

    namespace {

        constinit kumo::Mutex g_lock;
        constinit ::CondVar g_activity;

        /* Signalled whenever an interface's link, address or configuration changes; see GetStateChangedEvent. */
        constinit ::Event g_state_changed_event;

        /* ---- Interfaces ---- */

        class Interface final : public netdev::IFrameReceiver {
            public:
                struct netif m_netif;
                netdev::INetDevice *m_device;
                int m_index;
                bool m_configured;
                bool m_use_dhcp;
                bool m_had_address;
                u32 m_logged_address;
                u32 m_logged_netmask;
                u32 m_logged_gateway;
                u64 m_rx_dropped;
            public:
                virtual void OnFrameReceived(const void *frame, size_t size) override {
                    /* Copy the frame into pool buffers; the driver reuses its own as soon as we return. */
                    struct pbuf *p = pbuf_alloc(PBUF_RAW, static_cast<u16_t>(size), PBUF_POOL);
                    if (p == nullptr) {
                        ++m_rx_dropped;
                        return;
                    }
                    pbuf_take(p, frame, static_cast<u16_t>(size));

                    if (m_netif.input(p, std::addressof(m_netif)) != ERR_OK) {
                        pbuf_free(p);
                    }
                }
        };

        constinit kumo::Storage<Interface> g_interface_storage[netdev::MaxNetDevices] = {};
        constinit Interface *g_interfaces[netdev::MaxNetDevices] = {};
        constinit int g_interface_count = 0;

        /* The 127.0.0.1 interface lwIP creates for itself. */
        constinit struct netif *g_loopback = nullptr;

        /* lwIP calls this to transmit a frame. `p` may be a chain. */
        err_t LinkOutput(struct netif *netif, struct pbuf *p) {
            Interface *interface = static_cast<Interface *>(netif->state);

            constexpr size_t MaxPieces = 8;
            netdev::FramePiece pieces[MaxPieces];
            size_t count = 0;

            /* A chain with more pieces than that is flattened first. */
            static constinit u8 s_flat[netdev::MaxFrameSize];
            for (struct pbuf *q = p; q != nullptr; q = q->next) {
                if (count == MaxPieces) {
                    if (p->tot_len > sizeof(s_flat)) {
                        return ERR_BUF;
                    }
                    pbuf_copy_partial(p, s_flat, p->tot_len, 0);
                    pieces[0] = { s_flat, p->tot_len };
                    count = 1;
                    break;
                }
                pieces[count++] = { q->payload, q->len };
            }

            const ::Result rc = interface->m_device->Send(pieces, count);
            if (::kumo::IsResult(rc, KERNELRESULT(ResourceExhausted))) {
                /* The device's queue is full. TCP retransmits; for anything else the packet is lost, as on a busy wire. */
                return ERR_MEM;
            }
            return ::kumo::IsSuccess(rc) ? ERR_OK : ERR_IF;
        }

        err_t InitializeNetif(struct netif *netif) {
            Interface *interface = static_cast<Interface *>(netif->state);

            netif->name[0]    = 'e';
            netif->name[1]    = 'n';
            netif->hostname   = "horizon";
            netif->output     = etharp_output;
            netif->linkoutput = LinkOutput;
            netif->mtu        = static_cast<u16_t>(interface->m_device->GetMtu());
            netif->hwaddr_len = ETH_HWADDR_LEN;
            interface->m_device->GetMacAddress(netif->hwaddr);
            netif->flags      = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
            return ERR_OK;
        }

        /* lwIP calls this when an interface's address or up/down state changes. */
        void OnNetifStatusChanged(struct netif *netif) {
            Interface *interface = static_cast<Interface *>(netif->state);
            const bool has_address = netif_is_up(netif) && !ip4_addr_isany_val(*netif_ip4_addr(netif));

            const u32 a = has_address ? ip4_addr_get_u32(netif_ip4_addr(netif)) : 0;
            const u32 m = has_address ? ip4_addr_get_u32(netif_ip4_netmask(netif)) : 0;
            const u32 g = has_address ? ip4_addr_get_u32(netif_ip4_gw(netif)) : 0;

            /* lwIP reports every change separately (address, then netmask, then gateway); log each state once. */
            const bool changed = a != interface->m_logged_address || m != interface->m_logged_netmask || g != interface->m_logged_gateway;
            interface->m_logged_address = a;
            interface->m_logged_netmask = m;
            interface->m_logged_gateway = g;

            if (has_address && changed) {
                const u32 d = ip4_addr_get_u32(ip_2_ip4(dns_getserver(0)));
                const u8 *ab = reinterpret_cast<const u8 *>(std::addressof(a)), *mb = reinterpret_cast<const u8 *>(std::addressof(m));
                const u8 *gb = reinterpret_cast<const u8 *>(std::addressof(g)), *db = reinterpret_cast<const u8 *>(std::addressof(d));
                Log("[bsdsockets] interface %d: %s address %u.%u.%u.%u, netmask %u.%u.%u.%u, gateway %u.%u.%u.%u, dns %u.%u.%u.%u\n",
                    interface->m_index, interface->m_use_dhcp ? "DHCP" : "static",
                    ab[0], ab[1], ab[2], ab[3], mb[0], mb[1], mb[2], mb[3], gb[0], gb[1], gb[2], gb[3], db[0], db[1], db[2], db[3]);
            } else if (!has_address && interface->m_had_address) {
                Log("[bsdsockets] interface %d: address removed\n", interface->m_index);
            }
            interface->m_had_address = has_address;

            KUMO_ABORT_UNLESS(::kumo::IsSuccess(::eventFire(&g_state_changed_event)));
            NotifyActivity();
        }

        Interface *GetInterface(int index) {
            return (0 <= index && index < g_interface_count) ? g_interfaces[index] : nullptr;
        }

        /* ---- The network thread ---- */

        constexpr size_t NetworkThreadStackSize = (64 * 1024);
        alignas(0x1000) constinit u8 g_network_thread_stack[NetworkThreadStackSize];
        constinit ::Thread g_network_thread;
        constinit ::Event g_kick_event;
        constinit std::atomic<bool> g_kick_event_ready = false;

        /* How long the thread sleeps when a pass found nothing to do. With no interrupts, this */
        /* bounds how late an incoming frame is noticed. */
        constexpr auto IdlePollInterval = std::chrono::milliseconds(1);

        /* How often the link state of each device is read. */
        constexpr auto LinkCheckInterval = std::chrono::milliseconds(500);

        void CheckLinks() {
            for (int i = 0; i < g_interface_count; ++i) {
                Interface *interface = g_interfaces[i];
                const bool up = interface->m_device->IsLinkUp();
                if (up != static_cast<bool>(netif_is_link_up(std::addressof(interface->m_netif)))) {
                    Log("[bsdsockets] interface %d: link %s\n", i, up ? "up" : "down");
                    if (up) {
                        netif_set_link_up(std::addressof(interface->m_netif));
                    } else {
                        netif_set_link_down(std::addressof(interface->m_netif));
                    }
                    KUMO_ABORT_UNLESS(::kumo::IsSuccess(::eventFire(&g_state_changed_event)));
                }
            }
        }

        void NetworkThreadFunction(void *) {
            u64 last_link_check = ::svcGetSystemTick();

            while (true) {
                int work = 0;
                {
                    ScopedLock lk;

                    /* Take in frames. */
                    for (int i = 0; i < g_interface_count; ++i) {
                        work += g_interfaces[i]->m_device->Poll(g_interfaces[i]);
                    }

                    /* Deliver what this host sent to itself. */
                    for (struct netif *netif = netif_list; netif != nullptr; netif = netif->next) {
                        if (netif->loop_first != nullptr) {
                            netif_poll(netif);
                            ++work;
                        }
                    }

                    /* Retransmissions, delayed ACKs, ARP and DHCP all run from lwIP's timers. */
                    sys_check_timeouts();

                    if (const u64 now = ::svcGetSystemTick(); now - last_link_check >= ::armNsToTicks(static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(LinkCheckInterval).count()))) {
                        last_link_check = now;
                        CheckLinks();
                    }

                    /* Timers can change socket state too (a connection timing out), and the cost of a */
                    /* spurious wake-up is one re-check, so waiters are woken on every busy pass. */
                    if (work > 0) {
                        NotifyActivity();
                    }
                }

                /* While there is traffic, keep going; otherwise sleep until the next tick or a kick. */
                if (work == 0) {
                    ::eventWait(&g_kick_event, static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(IdlePollInterval).count()));
                }
            }
        }

    }

    void Lock() { g_lock.lock(); }
    void Unlock() { g_lock.unlock(); }
    bool IsLockedByCurrentThread() { return g_lock.IsLockedByCurrentThread(); }

    void WaitForActivity(std::chrono::nanoseconds timeout) {
        KUMO_ASSERT(IsLockedByCurrentThread());
        const s64 nanoseconds = timeout.count();
        ::condvarWaitTimeout(&g_activity, g_lock.native_handle(), nanoseconds > 0 ? static_cast<u64>(nanoseconds) : 0);
    }

    void NotifyActivity() {
        ::condvarWakeAll(&g_activity);
    }

    void KickNetworkThread() {
        if (g_kick_event_ready.load(std::memory_order_acquire)) {
            KUMO_ABORT_UNLESS(::kumo::IsSuccess(::eventFire(&g_kick_event)));
        }
    }

    ::Handle GetStateChangedEvent() {
        return g_state_changed_event.revent;
    }

    void Initialize() {
        ScopedLock lk;

        /* Manual clear: the readable handle is handed to clients, who reset it themselves. */
        KUMO_ABORT_UNLESS(::kumo::IsSuccess(::eventCreate(&g_state_changed_event, false)));

        lwip_init();

        /* lwip_init creates the loopback interface; it is the only one so far. */
        g_loopback = netif_list;

        for (int i = 0; i < netdev::GetNetDeviceCount(); ++i) {
            Interface *interface = g_interface_storage[i].construct();
            interface->m_device      = netdev::GetNetDevice(i);
            interface->m_index       = i;
            interface->m_configured  = false;
            interface->m_use_dhcp    = false;
            interface->m_had_address = false;
            interface->m_logged_address = 0;
            interface->m_logged_netmask = 0;
            interface->m_logged_gateway = 0;
            interface->m_rx_dropped  = 0;

            KUMO_ABORT_UNLESS(netif_add(std::addressof(interface->m_netif), nullptr, nullptr, nullptr, interface, InitializeNetif, ethernet_input) != nullptr);
            netif_set_status_callback(std::addressof(interface->m_netif), OnNetifStatusChanged);

            if (interface->m_device->IsLinkUp()) {
                netif_set_link_up(std::addressof(interface->m_netif));
            }

            g_interfaces[g_interface_count++] = interface;
        }

        /* Packets with no better route leave through the primary interface. */
        if (g_interface_count > 0) {
            netif_set_default(std::addressof(g_interfaces[0]->m_netif));
        }
    }

    void StartNetworkThread() {
        KUMO_ABORT_UNLESS(::kumo::IsSuccess(::eventCreate(&g_kick_event, true)));
        g_kick_event_ready.store(true, std::memory_order_release);

        s32 priority = 0;
        KUMO_ABORT_UNLESS(::kumo::IsSuccess(::svcGetThreadPriority(&priority, CUR_THREAD_HANDLE)));
        KUMO_ABORT_UNLESS(::kumo::IsSuccess(::threadCreate(&g_network_thread, NetworkThreadFunction, nullptr, g_network_thread_stack, NetworkThreadStackSize, priority, -2)));
        KUMO_ABORT_UNLESS(::kumo::IsSuccess(::threadStart(&g_network_thread)));
    }

    ::Result SetInterfaceUp(int index, const Ipv4Config &config) {
        ScopedLock lk;

        Interface *interface = GetInterface(index);
        KUMO_UNLESS(interface != nullptr, KERNELRESULT(NotFound));
        struct netif *netif = std::addressof(interface->m_netif);

        /* Asking for DHCP on an interface that is already doing DHCP changes nothing. Restarting would drop */
        /* a working lease, and this is what happens when a client repeats the start-up default. */
        if (interface->m_configured && interface->m_use_dhcp && config.use_dhcp) {
            KUMO_SUCCEED();
        }

        /* Reconfiguring starts from a clean interface. */
        if (interface->m_configured) {
            if (interface->m_use_dhcp) {
                dhcp_release_and_stop(netif);
            }
            netif_set_down(netif);
            netif_set_addr(netif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4);
        }

        interface->m_configured = true;
        interface->m_use_dhcp   = config.use_dhcp;

        if (config.use_dhcp) {
            netif_set_up(netif);
            KUMO_UNLESS(dhcp_start(netif) == ERR_OK, KERNELRESULT(ResourceExhausted));
        } else {
            ip4_addr_t address, netmask, gateway;
            ip4_addr_set_u32(std::addressof(address), config.address);
            ip4_addr_set_u32(std::addressof(netmask), config.netmask);
            ip4_addr_set_u32(std::addressof(gateway), config.gateway);
            netif_set_addr(netif, std::addressof(address), std::addressof(netmask), std::addressof(gateway));

            for (u8 i = 0; i < std::size(config.dns); ++i) {
                ip_addr_t server;
                ip4_addr_set_u32(ip_2_ip4(std::addressof(server)), config.dns[i]);
                dns_setserver(i, std::addressof(server));
            }
            netif_set_up(netif);
        }

        KUMO_SUCCEED();
    }

    ::Result SetInterfaceDown(int index) {
        ScopedLock lk;

        Interface *interface = GetInterface(index);
        KUMO_UNLESS(interface != nullptr, KERNELRESULT(NotFound));
        struct netif *netif = std::addressof(interface->m_netif);

        if (interface->m_configured) {
            if (interface->m_use_dhcp) {
                dhcp_release_and_stop(netif);
            }
            netif_set_down(netif);
            netif_set_addr(netif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4);
            interface->m_configured = false;
        }
        KUMO_SUCCEED();
    }

    ::Result RenewDhcp(int index) {
        ScopedLock lk;

        Interface *interface = GetInterface(index);
        KUMO_UNLESS(interface != nullptr, KERNELRESULT(NotFound));
        KUMO_UNLESS(interface->m_configured && interface->m_use_dhcp, KERNELRESULT(InvalidState));
        KUMO_UNLESS(dhcp_renew(std::addressof(interface->m_netif)) == ERR_OK, KERNELRESULT(InvalidState));
        KUMO_SUCCEED();
    }

    ::Result GetInterfaceState(InterfaceState *out, int index) {
        ScopedLock lk;

        *out = {};
        Interface *interface = GetInterface(index);
        KUMO_UNLESS(interface != nullptr, KERNELRESULT(NotFound));
        struct netif *netif = std::addressof(interface->m_netif);

        out->exists      = true;
        out->link_up     = netif_is_link_up(netif);
        out->configured  = interface->m_configured;
        out->use_dhcp    = interface->m_use_dhcp;
        out->has_address = netif_is_up(netif) && !ip4_addr_isany_val(*netif_ip4_addr(netif));
        out->mtu         = netif->mtu;
        std::memcpy(out->mac, netif->hwaddr, sizeof(out->mac));
        if (out->has_address) {
            out->address = ip4_addr_get_u32(netif_ip4_addr(netif));
            out->netmask = ip4_addr_get_u32(netif_ip4_netmask(netif));
            out->gateway = ip4_addr_get_u32(netif_ip4_gw(netif));
            for (u8 i = 0; i < std::size(out->dns); ++i) {
                out->dns[i] = ip4_addr_get_u32(ip_2_ip4(dns_getserver(i)));
            }
        }
        KUMO_SUCCEED();
    }

    bool IsLocalAddress(u32 address) {
        KUMO_ASSERT(IsLockedByCurrentThread());
        for (struct netif *netif = netif_list; netif != nullptr; netif = netif->next) {
            if (netif_is_up(netif) && !ip4_addr_isany_val(*netif_ip4_addr(netif)) && ip4_addr_get_u32(netif_ip4_addr(netif)) == address) {
                return true;
            }
        }
        return false;
    }

}

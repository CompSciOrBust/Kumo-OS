/*
 * The protocol stack: lwIP, the lock that serialises every use of it, the
 * thread that polls the network devices and runs lwIP's timers, and the
 * configuration of the interfaces.
 *
 * Locking rule: lwIP is not thread-safe and has no thread of its own here.
 * Whoever calls into lwIP, or touches a socket, holds the stack lock. Code
 * that has to wait for the network (a blocking recv, say) waits on the
 * lock's condition variable with WaitForActivity, which is signalled
 * whenever the state of any socket or interface may have changed.
 */
#pragma once
#include <kumo/prelude.hpp>
#include <chrono>

struct netif;

namespace kumo::horizon::hznet::stack {

    /* ---- The stack lock ---- */

    void Lock();
    void Unlock();
    bool IsLockedByCurrentThread();

    class ScopedLock {
        KUMO_NON_COPYABLE(ScopedLock);
        KUMO_NON_MOVEABLE(ScopedLock);
        public:
            inline ScopedLock() { Lock(); }
            inline ~ScopedLock() { Unlock(); }
    };

    /* Wait, with the lock held, until something may have changed or the timeout passes. */
    /* The lock is released while waiting. Wake-ups can be spurious: re-check the condition. */
    void WaitForActivity(std::chrono::nanoseconds timeout);

    /* Wake every thread in WaitForActivity. Call with the lock held. */
    void NotifyActivity();

    /* Ask the network thread to run now instead of at its next tick. Call after queuing */
    /* something it must deliver (loopback traffic). */
    void KickNetworkThread();

    /* ---- Start-up ---- */

    /* Initialise lwIP and create an interface for every registered network device. */
    void Initialize();

    /* Start the network thread. */
    void StartNetworkThread();

    /* ---- Interface configuration ---- */
    /* This is what the bsdcfg service drives: SetIfUp, SetIfDown, GetIfState and DhcpRenew map onto */
    /* these. The module also calls SetInterfaceUp itself at start-up, so that a system without nifm has an address. */

    struct Ipv4Config {
        bool use_dhcp;
        /* Used when use_dhcp is false. Network byte order. */
        u32 address;
        u32 netmask;
        u32 gateway;
        u32 dns[2];
    };

    struct InterfaceState {
        bool exists;
        bool link_up;
        bool configured;    /* brought up with SetInterfaceUp */
        bool has_address;   /* an address is assigned (for DHCP: a lease is held) */
        bool use_dhcp;
        u8  mac[6];
        u32 mtu;
        /* Network byte order; zero when has_address is false. */
        u32 address;
        u32 netmask;
        u32 gateway;
        u32 dns[2];
    };

    /* Signalled (manual clear) whenever an interface's link, address or configuration changes. Its */
    /* readable handle is what bsdcfg's SetIfUpWithEvent gives clients. */
    ::Handle GetStateChangedEvent();

    constexpr inline Ipv4Config DhcpConfig = { .use_dhcp = true, .address = 0, .netmask = 0, .gateway = 0, .dns = { 0, 0 } };

    ::Result SetInterfaceUp(int index, const Ipv4Config &config);
    ::Result SetInterfaceDown(int index);
    ::Result RenewDhcp(int index);
    ::Result GetInterfaceState(InterfaceState *out, int index);

    /* ---- For the socket layer; call with the lock held ---- */

    /* Whether `address` (network byte order) is assigned to one of this host's interfaces, loopback included. */
    bool IsLocalAddress(u32 address);

}

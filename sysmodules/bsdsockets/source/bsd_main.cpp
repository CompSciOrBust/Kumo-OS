/*
 * Sockets system module for Horizon on generic aarch64 hardware.
 *
 * Layers, top to bottom:
 *   ipc/      bsd:u, bsd:s, bsdcfg and sfdnsres session objects, client bookkeeping, the server loops
 *   socket/   BSD socket semantics (descriptors, blocking, options) on lwIP's callback API
 *   stack/    lwIP, the stack lock, the network thread, interface configuration
 *   lwip/     the lwIP port: options, system glue, wrappers that compile vendor/lwip
 *   netdev/   network-device interface and registry
 *   drivers/  one directory per NIC, bound by probing
 */
#include <switch.h>
#include <kumo/platform.hpp>
#include "bsd_log.hpp"
#include "drivers/bsd_drivers.hpp"
#include "netdev/bsd_net_device_registry.hpp"
#include "stack/bsd_stack.hpp"
#include "ipc/bsd_server.hpp"
#include "ipc/bsdcfg_server.hpp"
#include "ipc/dns_server.hpp"

extern "C" {
    u32 __nx_applet_type = AppletType_None;

    // libnx and newlib allocate small thread and runtime records from here.
    void __libnx_initheap() {
        alignas(16) static unsigned char heap[1024 * 1024];
        extern char *fake_heap_start;
        extern char *fake_heap_end;
        fake_heap_start = reinterpret_cast<char *>(heap);
        fake_heap_end = reinterpret_cast<char *>(heap + sizeof(heap));
    }

    void __appInit() {
        const ::Result rc = ::smInitialize();
        if (rc != 0) ::diagAbortWithResult(rc);
    }

    void __appExit() { ::smExit(); }

    void __libnx_exit(int) {
        // A KIP is an immortal process; reaching exit signals a fatal error.
        ::diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_ShouldNotHappen));
        __builtin_unreachable();
    }
}

int main(int, char **) {
    namespace net = kumo::horizon::hznet;
    net::Log("[bsdsockets] starting\n");
    ::kumo::platform::Initialize(::kumo::platform::IoPool_PciNetwork);
    net::drivers::ProbeAll();
    net::stack::Initialize();
    net::stack::StartNetworkThread();
    if (net::netdev::GetNetDeviceCount() > 0) {
        const ::Result rc = net::stack::SetInterfaceUp(0, net::stack::DhcpConfig);
        if (rc != 0) net::Log("[bsdsockets] interface 0: could not start DHCP (rc=0x%x)\n", rc);
    }
    net::ipc::InitializeServer();
    net::ipc::StartConfigServer();
    net::ipc::StartDnsServer();
    net::Log("[bsdsockets] serving bsd:u, bsd:s, bsdcfg, sfdnsres\n");
    net::ipc::LoopServer();
}

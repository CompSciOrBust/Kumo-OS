/*
 * Network interface manager for Horizon on generic aarch64 hardware.
 * nifm serves the system's network status using the sockets module's bsdcfg.
 */
#include <switch.h>
#include "nifm_log.hpp"
#include "ipc/nifm_server.hpp"
#include "ipc/nifm_request.hpp"
#include "network/nifm_network_state.hpp"

extern "C" {
    u32 __nx_applet_type = AppletType_None;

    // libnx, newlib and the IPC server allocate their runtime records here.
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
    namespace nifm = kumo::horizon::nifm;
    nifm::Log("[nifm] starting\n");

    // Serve first, so requests can wait while the network comes up.
    const ::Result rc = nifm::InitializeServer();
    if (rc != 0) ::diagAbortWithResult(rc);
    nifm::network::StartMonitor(nifm::NotifyNetworkChanged);

    nifm::Log("[nifm] serving nifm:u, nifm:s, nifm:a\n");
    nifm::LoopServer();
}

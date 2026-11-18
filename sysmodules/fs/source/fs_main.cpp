#include <kumo/platform.hpp>
#include "fs_log.hpp"
#include "partition/fs_partitions.hpp"
#include "drivers/fs_drivers.hpp"
#include "ipc/fs_server.hpp"

extern "C" {
u32 __nx_applet_type = AppletType_None;
void __appInit() {
    // HorizonUEFI exposes the target firmware through the kernel's GetConfig SMC.
    SecmonArgs config{{0xC3000002, 65011}};
    svcCallSecureMonitor(&config);
    KUMO_ASSERT(config.X[0] == 0 && config.X[1] != 0);
    hosversionSet(BIT(31) | static_cast<u32>(config.X[1] >> 8));
    KUMO_ABORT_RESULT(smInitialize());
}
void __appExit() { smExit(); }
void __libnx_exit(int) { KUMO_ABORT("FS exited"); }
}

int main() {
    using namespace kumo::horizon::hzfs;
    Log("[fs] starting\n");
    kumo::platform::Initialize(kumo::platform::IoPool_PciStorage);
    drivers::ProbeAll();
    partition::Initialize();
    ipc::InitializeServer();
    Log("[fs] serving fsp-srv, fsp-pr, fsp-ldr\n");
    ipc::LoopServer();
}

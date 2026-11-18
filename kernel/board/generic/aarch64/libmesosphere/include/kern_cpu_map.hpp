#pragma once
#include <mesosphere/kern_common.hpp>

namespace ams::kern::board::generic::aarch64::impl::cpu {

    /* Userland numbers cores as on the Switch: programs name virtual cores 0-3 (and 63), with */
    /* applications on 0-2 and system modules on 3. The kernel converts them to physical cores */
    /* through this map at every userland boundary (thread creation, core masks, */
    /* svcGetCurrentProcessorNumber, svcGetInfo). */
    /* */
    /* This board runs on however many cores the machine has, up to cpu::NumCores, so the map is */
    /* set at boot from the number of cores that started (InitializePresentCores, called by */
    /* TurnOnAllCores in kern_init_core.cpp before the other cores run). Policy: */
    /*   virtual 3 and 63 (system)       -> the last present core */
    /*   virtual 0-2 (applications)      -> the other present cores, in turn; core 0 if there is only one */
    /*   virtual 4-62 (unused by Horizon) -> physical core 0, as on the other boards */
    /* With four cores this is the identity, the same map as nintendo/nx and qemu/virt. */
    /* */
    /* Storage. Zero (everything on core 0) until InitializePresentCores; read-only afterwards. */
    extern constinit s32 g_virtual_to_physical_core_map[BITSIZEOF(u64)];

    /* The map, read-only, under the name every board provides. */
    constexpr inline const s32 (&VirtualToPhysicalCoreMap)[BITSIZEOF(u64)] = g_virtual_to_physical_core_map;

    /* Sets the number of present cores (physical cores 0 to num_cores - 1, clamped to */
    /* 1..cpu::NumCores) and fills the map from it. Called once, on core 0, before any other */
    /* core reaches HorizonKernelMain. */
    void InitializePresentCores(s32 num_cores);

}

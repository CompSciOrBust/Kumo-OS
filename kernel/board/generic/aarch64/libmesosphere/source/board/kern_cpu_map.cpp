#include <mesosphere.hpp>

namespace ams::kern::arch::arm64::cpu {

    /* Declared in kern_cpu.hpp (kernel patch 0014). One until the secondary cores have been */
    /* started: only the boot core is known to run before then. */
    constinit s32 g_num_present_cores = 1;

}

namespace ams::kern::board::generic::aarch64::impl::cpu {

    constinit s32 g_virtual_to_physical_core_map[BITSIZEOF(u64)] = {};

    void InitializePresentCores(s32 num_cores) {
        namespace arch_cpu = ams::kern::arch::arm64::cpu;

        num_cores = std::max<s32>(1, std::min<s32>(num_cores, static_cast<s32>(arch_cpu::NumCores)));

        /* System modules get the last core to themselves when there are two or more; */
        /* applications share the rest. With one core everything runs on core 0. */
        const s32 system_core   = num_cores - 1;
        const s32 num_app_cores = std::max<s32>(1, num_cores - 1);

        for (size_t i = 0; i < util::size(g_virtual_to_physical_core_map); ++i) {
            g_virtual_to_physical_core_map[i] = 0;
        }
        for (s32 i = 0; i < 3; ++i) {
            g_virtual_to_physical_core_map[i] = i % num_app_cores;
        }
        g_virtual_to_physical_core_map[3]                  = system_core;
        g_virtual_to_physical_core_map[BITSIZEOF(u64) - 1] = system_core;

        arch_cpu::g_num_present_cores = num_cores;
    }

}

#pragma once
#include <mesosphere/kern_common.hpp>
#include <mesosphere/kern_k_system_control_base.hpp>

namespace ams::kern::board::generic::aarch64 {

    constexpr inline uintptr_t BootParamsPhysAddr = 0x40000000ULL;
    constexpr inline u64       BootParamsMagic    = 0x48425030ULL;  /* "HBP0" */

    constexpr inline size_t MaxPciMmioRanges = 4;

    struct KBootPciMmioRange {
        u64 base;
        u64 size;
    };

    /* Must match bootloader/horizon_boot_params.h. Fields are only ever appended. */
    struct KBootParameters {
        u64 magic;
        u64 dram_base;
        u64 dram_size;
        u64 gic_distributor_base;
        u64 gic_redistributor_base;
        u64 gic_redistributor_size;
        u64 uart_base;
        u32 uart_clock_hz;
        u32 cpu_count;
        u64 pci_ecam_base;      /* 0 = no PCI host */
        u32 pci_start_bus;
        u32 pci_end_bus;        /* inclusive */
        u32 pci_mmio_count;
        u32 pmu_interrupt;      /* PMU overflow interrupt from ACPI MADT; 0 = the firmware describes no PMU */
        KBootPciMmioRange pci_mmio[MaxPciMmioRanges];
    };

    /* The PCI host, copied out of the boot parameters during early init. */
    struct PciHostInfo {
        u64 ecam_base;
        u32 start_bus;
        u32 end_bus;
        u32 mmio_count;
        KBootPciMmioRange mmio[MaxPciMmioRanges];
    };

    extern PciHostInfo g_pci_host_info;

    /* KBootParameters.cpu_count (the number of processors in the ACPI MADT; 0 = unknown), copied */
    /* out of the boot parameters during early init. */
    extern u32 g_boot_cpu_count;

    class KSystemControl : public KSystemControlBase {
        public:
            class Init : public KSystemControlBase::Init {
                private:
                    friend class KSystemControlBase::Init;
                private:
                    static void CpuOnImpl(u64 core_id, uintptr_t entrypoint, uintptr_t arg);
                public:
                    static size_t GetRealMemorySize();
                    static size_t GetIntendedMemorySize();
                    static bool TryCpuOn(u64 core_id, const ams::kern::init::KInitArguments *args);

                    /* How many cores TurnOnAllCores should try to run, the boot core included: the */
                    /* firmware's count, or cpu::NumCores if it gave none, and never more than NumCores. */
                    static s32 GetNumCoresToStart();
            };
        public:
            /* Called before kernel initialization to adjust the kernel's feature switches for this machine. */
            static NOINLINE void ConfigureKTargetSystem();

            /* The base's, then a report of the machine's counter rate and of the cores started, once */
            /* the debug log is up. */
            static void InitializePhase1();

            static void CallSecureMonitorFromUser(ams::svc::lp64::SecureMonitorArguments *args);
    };

}

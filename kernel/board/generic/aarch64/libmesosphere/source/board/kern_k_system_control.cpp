#include <mesosphere.hpp>

extern "C" {
    volatile u64 g_qemu_spin_table[8] __attribute__((aligned(16)));
}

namespace ams::kern::init {
    void StartOtherCore(const ams::kern::init::KInitArguments *init_args);
}

namespace ams::kern::board::generic::aarch64 {

    namespace {

        constinit bool g_has_firmware_rng_seed = false;
        constinit u8 g_firmware_rng_seed[BootRngSeedSize] = {};

        /* DRAM size reported by the bootloader, clamped to what the kernel is built for. */
        /* Falls back to MainMemorySize if the bootloader did not report one. */
        size_t GetBootMemorySize() {
            const auto *p = reinterpret_cast<const KBootParameters *>(BootParamsPhysAddr);
            if (p->magic == BootParamsMagic && p->dram_size != 0) {
                return std::min(static_cast<size_t>(p->dram_size), ams::kern::MainMemorySizeMax);
            }
            return ams::kern::MainMemorySize;
        }

        /* PSCI function IDs (Arm DEN 0022). */
        constexpr u64 PsciFunctionId_SystemOff   = 0x84000008;
        constexpr u64 PsciFunctionId_SystemReset = 0x84000009;
        constexpr u64 PsciFunctionId_CpuOn64     = 0xC4000003;

        constexpr s64 PsciResult_Success = 0;

        /* The first CPU_ON that failed, if any: its target (affinity) and PSCI result. */
        constinit u64 g_cpu_on_failed_target = 0;
        constinit s64 g_cpu_on_failed_result = PsciResult_Success;

        /* Calls the PSCI firmware and returns its result from x0. */
        /* The conduit is HVC, which is what QEMU virt (DTB: psci/method = "hvc") and Compute Engine's */
        /* T2A use. The firmware states its conduit in the ACPI FADT (ARM boot flags, PSCI_USE_HVC); */
        /* using that would need the bootloader to pass it in the boot parameters. */
        /* SMCCC 1.0 lets the callee corrupt x4-x17, so they are clobbered here as well. */
        ALWAYS_INLINE s64 CallPsci(u64 function_id, u64 arg0 = 0, u64 arg1 = 0, u64 arg2 = 0) {
            register u64 x0 asm("x0") = function_id;
            register u64 x1 asm("x1") = arg0;
            register u64 x2 asm("x2") = arg1;
            register u64 x3 asm("x3") = arg2;
            __asm__ __volatile__(
                "hvc #0"
                : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                :
                : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory"
            );
            return static_cast<s64>(x0);
        }

        /* Powers the machine off or resets it with PSCI SYSTEM_OFF or SYSTEM_RESET. Both act on the */
        /* whole system whatever the other cores are doing, so they are not stopped first. */
        /* Neither call returns on success; if one does, its PSCI result is returned. */
        s64 CallPsciSystemPower(u64 function_id) {
            KScopedInterruptDisable di;
            return CallPsci(function_id);
        }

    }

    /* Real and intended size must agree: KSystemControlBase::Init::GetKernelPhysicalBaseAddress */
    /* moves the DRAM base by half their difference, and this board needs the base to stay at */
    /* MainMemoryAddress, where the boot parameters are. */
    size_t KSystemControl::Init::GetRealMemorySize() {
        return GetBootMemorySize();
    }

    size_t KSystemControl::Init::GetIntendedMemorySize() {
        return GetBootMemorySize();
    }

    void KSystemControl::Init::CpuOnImpl(u64 core_id, uintptr_t entrypoint, uintptr_t arg) {
        /* PSCI64 CPU_ON: x1 = target cpu, x2 = entry point, x3 = context. */
        CallPsci(PsciFunctionId_CpuOn64, core_id, static_cast<u64>(entrypoint), static_cast<u64>(arg));
    }

    bool KSystemControl::Init::TryCpuOn(u64 core_id, const ams::kern::init::KInitArguments *args) {
        /* Resolve physical addresses for the entry point and argument block. */
        KPhysicalAddress entrypoint = Null<KPhysicalAddress>;
        while (!cpu::GetPhysicalAddressReadable(std::addressof(entrypoint), reinterpret_cast<uintptr_t>(::ams::kern::init::StartOtherCore), true)) { /* ... */ }

        KPhysicalAddress args_addr = Null<KPhysicalAddress>;
        while (!cpu::GetPhysicalAddressReadable(std::addressof(args_addr), reinterpret_cast<uintptr_t>(args), true)) { /* ... */ }

        cpu::StoreDataCacheForInitArguments(args, sizeof(*args));

        const s64 result = CallPsci(PsciFunctionId_CpuOn64, core_id, static_cast<u64>(GetInteger(entrypoint)), static_cast<u64>(GetInteger(args_addr)));
        if (result != PsciResult_Success) {
            /* Kept for InitializePhase1 to report: this runs before the debug log exists. */
            g_cpu_on_failed_target = core_id;
            g_cpu_on_failed_result = result;
            return false;
        }
        return true;
    }

    s32 KSystemControl::Init::GetNumCoresToStart() {
        const s32 max_cores = static_cast<s32>(cpu::NumCores);
        if (g_boot_cpu_count == 0) {
            return max_cores;
        }
        return std::min<s32>(static_cast<s32>(g_boot_cpu_count), max_cores);
    }

    void KSystemControl::InitializePhase1() {
        if (g_has_firmware_rng_seed) {
            KTargetSystem::SetInitialized();
            KSystemControlBase::InitializePhase1Base(g_firmware_rng_seed, sizeof(g_firmware_rng_seed));
            crypto::ClearMemory(g_firmware_rng_seed, sizeof(g_firmware_rng_seed));
        } else {
            KSystemControlBase::InitializePhase1();
        }
        MESOSPHERE_LOG("Secure random SMC: %s\n", g_has_firmware_rng_seed ? "seeded by UEFI" : "disabled (no UEFI seed)");

        /* The hardware timer converts this rate to the nominal tick rate (kernel patch 0012), so */
        /* the line records what the kernel is scaling from. It is printed here because the timer */
        /* is set up before the debug log exists. */
        u64 frequency;
        __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(frequency));
        MESOSPHERE_LOG("Counter frequency %lu Hz; kernel ticks are %ld Hz\n", frequency, static_cast<s64>(ams::svc::TicksPerSecond));

        /* The cores were started by TurnOnAllCores (kern_init_core.cpp, kernel patch 0014), also */
        /* before the debug log existed. */
        const s32 num_present = static_cast<s32>(cpu::GetNumPresentCores());
        if (g_boot_cpu_count != 0) {
            MESOSPHERE_LOG("%u CPUs present, using %d\n", g_boot_cpu_count, num_present);
        } else {
            MESOSPHERE_LOG("CPU count not reported by the firmware; using %d\n", num_present);
        }
        if (g_cpu_on_failed_result != PsciResult_Success) {
            MESOSPHERE_LOG("PSCI CPU_ON for CPU %lx failed with %ld; it and any later CPUs are not used\n", g_cpu_on_failed_target, g_cpu_on_failed_result);
        }
        MESOSPHERE_LOG("Userland cores 0, 1, 2, 3 run on physical cores %d, %d, %d, %d\n", cpu::VirtualToPhysicalCoreMap[0], cpu::VirtualToPhysicalCoreMap[1], cpu::VirtualToPhysicalCoreMap[2], cpu::VirtualToPhysicalCoreMap[3]);
    }

    void KSystemControl::ConfigureKTargetSystem() {
        volatile auto *ts = const_cast<volatile KTargetSystem::KTargetSystemData *>(std::addressof(KTargetSystem::s_data));

        /* The kernel gives userland access to the performance counters by writing PMUSERENR_EL0 on */
        /* every core. A hypervisor may trap the PMU registers, making that write an undefined */
        /* instruction, while still reporting a PMU in ID_AA64DFR0_EL1 (Google Compute Engine's Arm */
        /* instances do). So the CPU's own answer is not enough: the firmware must describe a PMU */
        /* too, which it does by giving every processor a performance interrupt in the ACPI MADT. */
        /* ID_AA64DFR0_EL1.PMUVer, bits 11:8, is 0 when there is no PMU and 0xF when it is */
        /* implementation defined. */
        /* This runs from the kernel's constructors, while physical memory is still identity mapped. */
        const auto *p = reinterpret_cast<const KBootParameters *>(BootParamsPhysAddr);
        if (p->magic == BootParamsMagic && p->rng_seed_magic == BootRngSeedMagic) {
            std::memcpy(g_firmware_rng_seed, p->rng_seed, sizeof(g_firmware_rng_seed));
            auto *boot_params = const_cast<KBootParameters *>(p);
            crypto::ClearMemory(boot_params->rng_seed, sizeof(boot_params->rng_seed));
            boot_params->rng_seed_magic = 0;
            g_has_firmware_rng_seed = true;
        }
        const bool firmware_has_pmu = p->magic == BootParamsMagic && p->pmu_interrupt != 0;

        const u64 pmu_version = (cpu::GetIdAa64Dfr0El1() >> 8) & 0xF;
        const bool cpu_has_pmu = pmu_version != 0x0 && pmu_version != 0xF;

        if (!firmware_has_pmu || !cpu_has_pmu) {
            ts->disable_user_pmu_access = true;
        }
    }

    void KSystemControl::CallSecureMonitorFromUser(ams::svc::lp64::SecureMonitorArguments *args) {
        /* There is no EL3 secure monitor on this board, so (like qemu-virt) we emulate */
        /* the SMCs userland needs here in the kernel. Unknown calls return NotImplemented. */
        constexpr u64 UserFunctionId_SetConfig           = 0xC3000401;
        constexpr u64 UserFunctionId_GetConfig           = 0xC3000002;
        constexpr u64 UserFunctionId_GenerateRandomBytes = 0xC3000006;

        /* Nintendo's configuration items. */
        constexpr u64 ConfigItem_DisableProgramVerification    = 1;
        constexpr u64 ConfigItem_DramId                        = 2;
        constexpr u64 ConfigItem_SecurityEngineInterruptNumber = 3;
        constexpr u64 ConfigItem_FuseVersion                   = 4;
        constexpr u64 ConfigItem_HardwareType                  = 5;
        constexpr u64 ConfigItem_HardwareState                 = 6;
        constexpr u64 ConfigItem_IsRecoveryBoot                = 7;
        constexpr u64 ConfigItem_DeviceId                      = 8;
        constexpr u64 ConfigItem_BootReason                    = 9;
        constexpr u64 ConfigItem_MemoryMode                    = 10;
        constexpr u64 ConfigItem_IsDevelopmentFunctionEnabled  = 11;
        constexpr u64 ConfigItem_KernelConfiguration           = 12;
        constexpr u64 ConfigItem_IsChargerHiZModeEnabled       = 13;
        constexpr u64 ConfigItem_RetailInteractiveDisplayState = 14;
        constexpr u64 ConfigItem_RegulatorType                 = 15;
        constexpr u64 ConfigItem_DeviceUniqueKeyGeneration     = 16;

        /* Atmosphere's extensions. */
        constexpr u64 ConfigItem_ExosphereApiVersion            = 65000;
        constexpr u64 ConfigItem_ExosphereNeedsReboot           = 65001;
        constexpr u64 ConfigItem_ExosphereNeedsShutdown         = 65002;
        constexpr u64 ConfigItem_ExosphereGitCommitHash         = 65003;
        constexpr u64 ConfigItem_ExosphereHasRcmBugPatch        = 65004;
        constexpr u64 ConfigItem_ExosphereBlankProdInfo         = 65005;
        constexpr u64 ConfigItem_ExosphereAllowCalWrites        = 65006;
        constexpr u64 ConfigItem_ExosphereEmummcType            = 65007;
        constexpr u64 ConfigItem_ExospherePayloadAddress        = 65008;
        constexpr u64 ConfigItem_ExosphereLogConfiguration      = 65009;
        constexpr u64 ConfigItem_ExosphereForceEnableUsb30      = 65010;
        constexpr u64 ConfigItem_ExosphereSupportedHosVersion   = 65011;
        constexpr u64 ConfigItem_ExosphereApproximateApiVersion = 65012;

        /* Items specific to this board. Nintendo's items are small numbers and Atmosphere's */
        /* extensions start at 65000; these use their own range ("HZ") to stay clear of both. */
        constexpr u64 ConfigItem_HzPciEcamBase  = 0x485A0001;   /* physical address; fails if there is no PCI host */
        constexpr u64 ConfigItem_HzPciBusRange  = 0x485A0002;   /* start bus | (end bus << 8) */

        /* Values of ExosphereNeedsReboot, as Exosphere defines them (secmon_user_power_management.hpp). */
        constexpr u64 UserRebootType_None         = 0;
        constexpr u64 UserRebootType_ToRcm        = 1;
        constexpr u64 UserRebootType_ToPayload    = 2;
        constexpr u64 UserRebootType_ToFatalError = 3;
        constexpr u64 UserRebootType_ByPmic       = 4;

        constexpr u64 SmcResult_Success             = 0;
        constexpr u64 SmcResult_NotImplemented      = 1;
        constexpr u64 SmcResult_InvalidArgument     = 2;

        /* Key generation reported to userland; matches the qemu-virt board. */
        constexpr u64 TargetKeyGeneration = 13;

        /* spl creates an interrupt event for the Switch's security engine, which signals the */
        /* end of an asynchronous crypto operation. This board has no such engine and no such */
        /* operations, so the interrupt never needs to fire; it only has to be one spl may claim. */
        /* 44 is the Switch's number, which is the one spl's capabilities permit. */
        constexpr u64 SecurityEngineInterruptNumber = 44;

        const u64 api_version = (static_cast<u64>(ATMOSPHERE_RELEASE_VERSION_MAJOR & 0xFF) << 56) |
                                (static_cast<u64>(ATMOSPHERE_RELEASE_VERSION_MINOR & 0xFF) << 48) |
                                (static_cast<u64>(ATMOSPHERE_RELEASE_VERSION_MICRO & 0xFF) << 40) |
                                (TargetKeyGeneration                                       << 32) |
                                (static_cast<u64>(GetTargetFirmware())                     <<  0);

        switch (args->r[0]) {
            case UserFunctionId_SetConfig:
                {
                    /* As on Exosphere, r[1] is the item and r[3] the value; r[2] (a signature) is unused. */
                    const u64 item  = args->r[1];
                    const u64 value = args->r[3];

                    /* Which PSCI call to make, if any. */
                    u64 psci_function_id = 0;
                    switch (item) {
                        case ConfigItem_ExosphereNeedsReboot:
                            /* The Switch can reboot into RCM, into a payload, into the fatal error screen, */
                            /* or through the PMIC. None of these exist here, so every one is a plain reset. */
                            switch (value) {
                                case UserRebootType_None:
                                    break;
                                case UserRebootType_ToRcm:
                                case UserRebootType_ToPayload:
                                case UserRebootType_ToFatalError:
                                case UserRebootType_ByPmic:
                                    MESOSPHERE_LOG("SetConfig SMC from %s: rebooting (reboot type %lu, PSCI SYSTEM_RESET)\n", GetCurrentProcess().GetName(), value);
                                    psci_function_id = PsciFunctionId_SystemReset;
                                    break;
                                default:
                                    args->r[0] = SmcResult_InvalidArgument;
                                    return;
                            }
                            break;
                        case ConfigItem_ExosphereNeedsShutdown:
                            /* Any value but 0 is a request to shut down. */
                            if (value != 0) {
                                MESOSPHERE_LOG("SetConfig SMC from %s: powering off (PSCI SYSTEM_OFF)\n", GetCurrentProcess().GetName());
                                psci_function_id = PsciFunctionId_SystemOff;
                            }
                            break;
                        default:
                            MESOSPHERE_LOG("Unhandled SetConfig SMC from %s: item=%lu value=%lx (returning NotImplemented)\n", GetCurrentProcess().GetName(), item, value);
                            args->r[0] = SmcResult_NotImplemented;
                            return;
                    }

                    if (psci_function_id != 0) {
                        /* This does not return unless the firmware refuses. */
                        const s64 psci_result = CallPsciSystemPower(psci_function_id);
                        MESOSPHERE_LOG("PSCI call %08lx returned %ld; the machine was not powered off or reset\n", psci_function_id, psci_result);
                        args->r[0] = SmcResult_NotImplemented;
                        return;
                    }

                    args->r[0] = SmcResult_Success;
                }
                break;
            case UserFunctionId_GetConfig:
                {
                    u64 value = 0;
                    bool known = true;
                    switch (args->r[1]) {
                        /* There are no signing keys yet, so programs are not signature-checked. */
                        case ConfigItem_DisableProgramVerification:    value = 1; break;
                        /* A development unit with development functions: nothing here is a retail console. */
                        case ConfigItem_HardwareState:                 value = 0; break;
                        case ConfigItem_IsDevelopmentFunctionEnabled:  value = 1; break;
                        /* There is no value for "not a Switch"; 0 is the original model. */
                        case ConfigItem_HardwareType:                  value = 0; break;
                        case ConfigItem_SecurityEngineInterruptNumber: value = SecurityEngineInterruptNumber; break;
                        /* Standard memory arrangement. */
                        case ConfigItem_MemoryMode:                    value = 1; break;
                        case ConfigItem_DramId:
                        case ConfigItem_FuseVersion:
                        case ConfigItem_IsRecoveryBoot:
                        case ConfigItem_DeviceId:
                        case ConfigItem_BootReason:
                        case ConfigItem_KernelConfiguration:
                        case ConfigItem_IsChargerHiZModeEnabled:
                        case ConfigItem_RetailInteractiveDisplayState:
                        case ConfigItem_RegulatorType:
                        case ConfigItem_DeviceUniqueKeyGeneration:
                            value = 0;
                            break;
                        case ConfigItem_ExosphereApiVersion:
                        case ConfigItem_ExosphereApproximateApiVersion:
                            value = api_version;
                            break;
                        case ConfigItem_ExosphereSupportedHosVersion:  value = static_cast<u64>(GetTargetFirmware()); break;
                        /* The Switch boot-ROM bug that this asks about does not exist here. */
                        case ConfigItem_ExosphereHasRcmBugPatch:       value = 1; break;
                        case ConfigItem_ExosphereNeedsReboot:
                        case ConfigItem_ExosphereNeedsShutdown:
                        case ConfigItem_ExosphereGitCommitHash:
                        case ConfigItem_ExosphereBlankProdInfo:
                        case ConfigItem_ExosphereAllowCalWrites:
                        case ConfigItem_ExosphereEmummcType:
                        case ConfigItem_ExospherePayloadAddress:
                        case ConfigItem_ExosphereLogConfiguration:
                        case ConfigItem_ExosphereForceEnableUsb30:
                            value = 0;
                            break;
                        case ConfigItem_HzPciEcamBase:
                            known = g_pci_host_info.ecam_base != 0;
                            value = g_pci_host_info.ecam_base;
                            break;
                        case ConfigItem_HzPciBusRange:
                            known = g_pci_host_info.ecam_base != 0;
                            value = static_cast<u64>(g_pci_host_info.start_bus) | (static_cast<u64>(g_pci_host_info.end_bus) << 8);
                            break;
                        default:
                            MESOSPHERE_LOG("Unhandled GetConfig SMC from %s: item=%lu (returning NotImplemented)\n", GetCurrentProcess().GetName(), args->r[1]);
                            known = false;
                            break;
                    }

                    if (known) {
                        args->r[1] = value;
                        args->r[0] = SmcResult_Success;
                    } else {
                        args->r[0] = SmcResult_NotImplemented;
                    }
                }
                break;
            case UserFunctionId_GenerateRandomBytes:
                {
                    /* The bytes are returned in the argument registers after the result. */
                    const size_t size = args->r[1];
                    if (size > sizeof(args->r) - sizeof(args->r[0])) {
                        args->r[0] = SmcResult_InvalidArgument;
                        break;
                    }

                    if (!g_has_firmware_rng_seed) {
                        args->r[0] = SmcResult_NotImplemented;
                        break;
                    }

                    /* SPL also relies on the existing 56-byte result register area. */
                    KSystemControl::GenerateRandom(std::addressof(args->r[1]), util::size(args->r) - 1);
                    args->r[0] = SmcResult_Success;
                }
                break;
            default:
                MESOSPHERE_LOG("Unhandled SMC from %s: [%016lx %016lx %016lx %016lx %016lx %016lx %016lx %016lx] (returning NotImplemented)\n", GetCurrentProcess().GetName(), args->r[0], args->r[1], args->r[2], args->r[3], args->r[4], args->r[5], args->r[6], args->r[7]);
                args->r[0] = SmcResult_NotImplemented;
                break;
        }
    }

}

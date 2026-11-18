#include <mesosphere.hpp>

namespace ams::kern {

    namespace {

        /* Reserve the 1 MB gap [MainMemoryAddress, kernel_load_base) used for boot params. */
        constexpr size_t ReservedEarlyDramSize = 0x100000;

        template<typename... T> requires (std::same_as<T, KMemoryRegionAttr> && ...)
        constexpr ALWAYS_INLINE KMemoryRegionType GetMemoryRegionType(KMemoryRegionType base, T... attr) {
            return util::FromUnderlying<KMemoryRegionType>(util::ToUnderlying(base) | (util::ToUnderlying<T>(attr) | ...));
        }

        void InsertPoolPartitionRegionIntoBothTrees(size_t start, size_t size, KMemoryRegionType phys_type, KMemoryRegionType virt_type, u32 &cur_attr) {
            const u32 attr = cur_attr++;

            MESOSPHERE_INIT_ABORT_UNLESS(KMemoryLayout::GetPhysicalMemoryRegionTree().Insert(start, size, phys_type, attr));
            const KMemoryRegion *phys = KMemoryLayout::GetPhysicalMemoryRegionTree().FindByTypeAndAttribute(phys_type, attr);
            MESOSPHERE_INIT_ABORT_UNLESS(phys != nullptr);
            MESOSPHERE_INIT_ABORT_UNLESS(phys->GetEndAddress() != 0);
            MESOSPHERE_INIT_ABORT_UNLESS(KMemoryLayout::GetVirtualMemoryRegionTree().Insert(phys->GetPairAddress(), size, virt_type, attr));
        }

    }

    namespace board::generic::aarch64 {
        constinit u32 g_uart_clock_hz = 0;
        constinit PciHostInfo g_pci_host_info = {};
        constinit u32 g_boot_cpu_count = 0;
    }

    namespace init {

        void SetupDevicePhysicalMemoryRegions() {
            using namespace ams::kern::board::generic::aarch64;

            const auto *p = reinterpret_cast<const KBootParameters *>(BootParamsPhysAddr);
            MESOSPHERE_INIT_ABORT_UNLESS(p->magic == BootParamsMagic);
            g_uart_clock_hz = p->uart_clock_hz;

            /* Kept for TurnOnAllCores, which runs after the identity mapping of the boot parameters is gone. */
            g_boot_cpu_count = p->cpu_count;

            /* Keep the PCI host description for the I/O pool and for userland queries. */
            if (p->pci_ecam_base != 0 && p->pci_start_bus <= p->pci_end_bus && p->pci_end_bus <= 0xFF) {
                g_pci_host_info.ecam_base  = p->pci_ecam_base;
                g_pci_host_info.start_bus  = p->pci_start_bus;
                g_pci_host_info.end_bus    = p->pci_end_bus;
                g_pci_host_info.mmio_count = std::min<u32>(p->pci_mmio_count, MaxPciMmioRanges);
                for (u32 i = 0; i < g_pci_host_info.mmio_count; ++i) {
                    g_pci_host_info.mmio[i] = p->pci_mmio[i];
                }
            }

            MESOSPHERE_INIT_ABORT_UNLESS(p->gic_distributor_base    != 0);
            MESOSPHERE_INIT_ABORT_UNLESS(p->gic_redistributor_base  != 0);
            MESOSPHERE_INIT_ABORT_UNLESS(p->gic_redistributor_size  != 0);
            MESOSPHERE_INIT_ABORT_UNLESS(p->uart_base               != 0);

            /* Map the GICv3 distributor, redistributors (one 0x20000-byte frame per core), and UART. */
            MESOSPHERE_INIT_ABORT_UNLESS(KMemoryLayout::GetPhysicalMemoryRegionTree().Insert(p->gic_distributor_base,   0x10000,                      GetMemoryRegionType(KMemoryRegionType_InterruptDistributor,   KMemoryRegionAttr_ShouldKernelMap)));
            MESOSPHERE_INIT_ABORT_UNLESS(KMemoryLayout::GetPhysicalMemoryRegionTree().Insert(p->gic_redistributor_base, p->gic_redistributor_size,    GetMemoryRegionType(KMemoryRegionType_InterruptRedistributor, KMemoryRegionAttr_ShouldKernelMap)));
            MESOSPHERE_INIT_ABORT_UNLESS(KMemoryLayout::GetPhysicalMemoryRegionTree().Insert(p->uart_base,              0x1000,                       GetMemoryRegionType(KMemoryRegionType_Uart,                   KMemoryRegionAttr_ShouldKernelMap)));
        }

        void SetupDramPhysicalMemoryRegions() {
            const size_t intended_memory_size                   = KSystemControl::Init::GetIntendedMemorySize();
            const KPhysicalAddress physical_memory_base_address = KSystemControl::Init::GetKernelPhysicalBaseAddress(ams::kern::MainMemoryAddress);

            MESOSPHERE_INIT_ABORT_UNLESS(KMemoryLayout::GetPhysicalMemoryRegionTree().Insert(GetInteger(physical_memory_base_address), intended_memory_size, KMemoryRegionType_Dram));
            if constexpr (ReservedEarlyDramSize > 0) {
                MESOSPHERE_INIT_ABORT_UNLESS(KMemoryLayout::GetPhysicalMemoryRegionTree().Insert(GetInteger(physical_memory_base_address), ReservedEarlyDramSize, KMemoryRegionType_DramReservedEarly));
            }
        }

        static void DbgPrint(const char *s) {
            const KMemoryRegion *r = KMemoryLayout::GetPhysicalMemoryRegionTree().FindFirstDerived(KMemoryRegionType_Uart);
            if (r == nullptr) return;
            volatile unsigned int *dr = reinterpret_cast<volatile unsigned int *>(r->GetPairAddress());
            while (*s) { *dr = (unsigned int)(*s++); }
        }

        void SetupPoolPartitionMemoryRegions() {
            DbgPrint("[POOL] start\n");
            const auto dram_extents = KMemoryLayout::GetMainMemoryPhysicalExtents();
            MESOSPHERE_INIT_ABORT_UNLESS(dram_extents.GetEndAddress() != 0);
            DbgPrint("[POOL] dram extents ok\n");

            const KMemoryRegion *pool_partitions_region = KMemoryLayout::GetPhysicalMemoryRegionTree().FindByTypeAndAttribute(KMemoryRegionType_DramPoolPartition, 0);
            MESOSPHERE_INIT_ABORT_UNLESS(pool_partitions_region != nullptr);
            DbgPrint("[POOL] pool partition region found\n");

            const uintptr_t pool_partitions_start = pool_partitions_region->GetAddress();
            const uintptr_t pool_end              = pool_partitions_region->GetEndAddress();
            MESOSPHERE_INIT_ABORT_UNLESS(pool_end == dram_extents.GetEndAddress());
            DbgPrint("[POOL] pool end matches dram end\n");

            const KMemoryRegion *kernel_dram_region = KMemoryLayout::GetPhysicalMemoryRegionTree().FindFirstDerived(KMemoryRegionType_DramKernelBase);
            MESOSPHERE_INIT_ABORT_UNLESS(kernel_dram_region != nullptr);
            DbgPrint("[POOL] kernel dram region found\n");

            const size_t application_pool_size       = KSystemControl::Init::GetApplicationPoolSize();
            const size_t applet_pool_size            = KSystemControl::Init::GetAppletPoolSize();
            const size_t unsafe_system_pool_min_size = KSystemControl::Init::GetMinimumNonSecureSystemPoolSize();

            const uintptr_t application_pool_start   = pool_end - application_pool_size;
            const uintptr_t applet_pool_start        = application_pool_start - applet_pool_size;
            const uintptr_t unsafe_system_pool_start = util::AlignDown(applet_pool_start - unsafe_system_pool_min_size, PageSize);
            const size_t    unsafe_system_pool_size  = applet_pool_start - unsafe_system_pool_start;

            const uintptr_t dram_midpoint = (dram_extents.GetAddress() + dram_extents.GetEndAddress()) / 2;
            u32 cur_pool_attr = 0;
            size_t total_overhead_size = 0;

            if (application_pool_size > 0) {
                if (dram_extents.GetEndAddress() <= dram_midpoint || dram_midpoint <= application_pool_start) {
                    InsertPoolPartitionRegionIntoBothTrees(application_pool_start, application_pool_size, KMemoryRegionType_DramApplicationPool, KMemoryRegionType_VirtualDramApplicationPool, cur_pool_attr);
                    total_overhead_size += KMemoryManager::CalculateManagementOverheadSize(application_pool_size);
                } else {
                    const size_t first_application_pool_size  = dram_midpoint - application_pool_start;
                    const size_t second_application_pool_size = application_pool_start + application_pool_size - dram_midpoint;
                    InsertPoolPartitionRegionIntoBothTrees(application_pool_start, first_application_pool_size,  KMemoryRegionType_DramApplicationPool, KMemoryRegionType_VirtualDramApplicationPool, cur_pool_attr);
                    InsertPoolPartitionRegionIntoBothTrees(dram_midpoint,          second_application_pool_size, KMemoryRegionType_DramApplicationPool, KMemoryRegionType_VirtualDramApplicationPool, cur_pool_attr);
                    total_overhead_size += KMemoryManager::CalculateManagementOverheadSize(first_application_pool_size);
                    total_overhead_size += KMemoryManager::CalculateManagementOverheadSize(second_application_pool_size);
                }
            }

            if (applet_pool_size > 0) {
                InsertPoolPartitionRegionIntoBothTrees(applet_pool_start, applet_pool_size, KMemoryRegionType_DramAppletPool, KMemoryRegionType_VirtualDramAppletPool, cur_pool_attr);
                total_overhead_size += KMemoryManager::CalculateManagementOverheadSize(applet_pool_size);
            }

            if (unsafe_system_pool_size > 0) {
                InsertPoolPartitionRegionIntoBothTrees(unsafe_system_pool_start, unsafe_system_pool_size, KMemoryRegionType_DramSystemNonSecurePool, KMemoryRegionType_VirtualDramSystemNonSecurePool, cur_pool_attr);
                total_overhead_size += KMemoryManager::CalculateManagementOverheadSize(unsafe_system_pool_size);
            }

            total_overhead_size += KMemoryManager::CalculateManagementOverheadSize((unsafe_system_pool_start - pool_partitions_start) - total_overhead_size);

            const uintptr_t system_pool_start = pool_partitions_start + total_overhead_size;
            const size_t    system_pool_size  = unsafe_system_pool_start - system_pool_start;
            InsertPoolPartitionRegionIntoBothTrees(system_pool_start, system_pool_size, KMemoryRegionType_DramSystemPool, KMemoryRegionType_VirtualDramSystemPool, cur_pool_attr);

            const uintptr_t pool_management_start = pool_partitions_start;
            const size_t    pool_management_size  = total_overhead_size;
            u32 pool_management_attr = 0;
            InsertPoolPartitionRegionIntoBothTrees(pool_management_start, pool_management_size, KMemoryRegionType_DramPoolManagement, KMemoryRegionType_VirtualDramPoolManagement, pool_management_attr);
        }

    }

}

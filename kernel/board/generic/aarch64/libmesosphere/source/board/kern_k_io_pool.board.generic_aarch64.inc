/* I/O pool for the generic-aarch64 board. Included by kern_k_io_pool.cpp. */
/* */
/* Every pool covers the PCI host: its ECAM window and the memory ranges the */
/* firmware assigned to devices. Other boards hardcode their ranges; here they */
/* come from the bootloader, so the table is filled in on first use. */

static_assert(ams::kern::board::generic::aarch64::MaxPciMmioRanges == 4);

constinit IoRegionExtents g_io_region_extents[1 + ams::kern::board::generic::aarch64::MaxPciMmioRanges] = {
    { Null<KPhysicalAddress>, 0 }, /* ECAM */
    { Null<KPhysicalAddress>, 0 }, /* Device memory ranges */
    { Null<KPhysicalAddress>, 0 },
    { Null<KPhysicalAddress>, 0 },
    { Null<KPhysicalAddress>, 0 },
};

bool IsValidIoPoolTypeImpl(ams::svc::IoPoolType pool_type) {
    if (pool_type != ams::svc::IoPoolType_PciStorage && pool_type != ams::svc::IoPoolType_PciNetwork) {
        return false;
    }

    /* Refreshing the table is idempotent, so no lock is needed. */
    const auto &pci = ams::kern::board::generic::aarch64::g_pci_host_info;
    if (pci.ecam_base == 0) {
        return false;
    }

    g_io_region_extents[0] = { KPhysicalAddress(pci.ecam_base), (static_cast<size_t>(pci.end_bus) - pci.start_bus + 1) << 20 };
    for (u32 i = 0; i < pci.mmio_count; ++i) {
        g_io_region_extents[1 + i] = { KPhysicalAddress(pci.mmio[i].base), static_cast<size_t>(pci.mmio[i].size) };
    }

    return true;
}

#include "efi_spec_defs.h"
#include "acpi_defs.h"
#include "horizon_boot_params.h"
#include "efi_utils.h"
#include "kip.h"

int matching_guid(EFI_GUID a, EFI_GUID b) {
    if (a.Data1 != b.Data1) return 0;
    if (a.Data2 != b.Data2) return 0;
    if (a.Data3 != b.Data3) return 0;
    for (int i = 0; i < 8; i++) if (a.Data4[i] != b.Data4[i]) return 0;
    return 1;
}

int find_config_table(EFI_SYSTEM_TABLE* ST, EFI_GUID GUIDToFind) {
    int index = -1;
    for (ULL i = 0; i < ST->NumberOfTableEntries; i++) {
        if (matching_guid(GUIDToFind, ST->ConfigurationTable[i].VendorGuid)) {
            index = i;
            break;
        }
    }
    return index;
}

static EFI_STATUS find_hardware_tables(EFI_SYSTEM_TABLE *ST, EFI_TEXT_OUT *console,
                                        void **fdt_out, void **acpi_out) {
    int FDTIndex = find_config_table(ST, FDT_GUID);
    if (FDTIndex != -1) *fdt_out = ST->ConfigurationTable[FDTIndex].VendorTable;

    int ACPIIndex = find_config_table(ST, ACPI_20_GUID);
    if (ACPIIndex != -1) *acpi_out = ST->ConfigurationTable[ACPIIndex].VendorTable;

    if (*fdt_out) print(console, "Found FDT!\n");
    else print(console, "Could not find FDT!\n");

    if (*acpi_out) print(console, "Found ACPI!\n");
    else print(console, "Could not find ACPI!\n");

    if (FDTIndex == -1 && ACPIIndex == -1) {
        print(console, "Cannot proceed with boot!\n");
        return EFI_NOT_FOUND;
    }
    return EFI_SUCCESS;
}

static void* find_acpi_table(void *xsdt_ptr, const char sig[4]) {
    ACPI_TABLE_HEADER *xsdt = (ACPI_TABLE_HEADER *)xsdt_ptr;
    unsigned int count = (xsdt->length - sizeof(ACPI_TABLE_HEADER)) / 8;
    unsigned long long *entries = (unsigned long long *)((char *)xsdt + sizeof(ACPI_TABLE_HEADER));
    for (unsigned int i = 0; i < count; i++) {
        ACPI_TABLE_HEADER *t = (ACPI_TABLE_HEADER *)(ULL)entries[i];
        if (t->signature[0] == sig[0] && t->signature[1] == sig[1] &&
            t->signature[2] == sig[2] && t->signature[3] == sig[3])
            return t;
    }
    return (void*)0;
}

static void parse_madt(void *madt_ptr, KBootParameters *params) {
    ACPI_MADT *madt = (ACPI_MADT *)madt_ptr;
    char *ptr = (char *)madt + sizeof(ACPI_MADT);
    char *end = (char *)madt + madt->header.length;
    int have_pmu = 1;
    unsigned int pmu_interrupt = 0;
    while (ptr < end) {
        ACPI_MADT_SUBTABLE *sub = (ACPI_MADT_SUBTABLE *)ptr;
        if (sub->type == ACPI_MADT_TYPE_GICC) {
            params->cpu_count++;
            // The PMU is usable only if every processor has one.
            ACPI_MADT_GICC *gicc = (ACPI_MADT_GICC *)ptr;
            if (sub->length < sizeof(ACPI_MADT_GICC) || gicc->performance_interrupt_gsiv == 0) {
                have_pmu = 0;
            } else {
                pmu_interrupt = gicc->performance_interrupt_gsiv;
            }
        } else if (sub->type == ACPI_MADT_TYPE_GICD) {
            params->gic_distributor_base = ((ACPI_MADT_GICD *)ptr)->base_address;
        } else if (sub->type == ACPI_MADT_TYPE_GICR) {
            ACPI_MADT_GICR *gicr = (ACPI_MADT_GICR *)ptr;
            params->gic_redistributor_base = gicr->discovery_range_base;
            params->gic_redistributor_size = gicr->discovery_range_length;
        }
        ptr += sub->length;
    }
    params->pmu_interrupt = have_pmu ? pmu_interrupt : 0;
}

static void parse_spcr(void *spcr_ptr, KBootParameters *params) {
    ACPI_SPCR *spcr = (ACPI_SPCR *)spcr_ptr;
    params->uart_base = spcr->base_address.address;
    params->uart_clock_hz = (spcr->header.revision >= 3) ? spcr->uart_clock_hz : 24000000;
}

// The ECAM window: where PCI configuration space is memory-mapped.
static void parse_mcfg(void *mcfg_ptr, KBootParameters *params) {
    ACPI_MCFG *mcfg = (ACPI_MCFG *)mcfg_ptr;
    unsigned int count = (mcfg->header.length - sizeof(ACPI_MCFG)) / sizeof(ACPI_MCFG_ALLOCATION);
    // Only PCI segment group 0 is supported.
    for (unsigned int i = 0; i < count; i++) {
        if (mcfg->allocations[i].segment_group == 0) {
            params->pci_ecam_base = mcfg->allocations[i].base_address;
            params->pci_start_bus = mcfg->allocations[i].start_bus;
            params->pci_end_bus   = mcfg->allocations[i].end_bus;
            return;
        }
    }
}

// The memory ranges the firmware gave to devices behind each PCI host bridge.
// Every BAR it assigned lies inside one of them.
static void find_pci_mmio_ranges(EFI_BOOT_SERVICES *BS, KBootParameters *params) {
    ULL handle_count = 0;
    EFI_HANDLE *handles = (void*)0;
    if (BS->LocateHandleBuffer(EFI_LOCATE_BY_PROTOCOL, (EFI_GUID *)&PCI_ROOT_BRIDGE_IO_GUID,
                               (void*)0, &handle_count, &handles) != EFI_SUCCESS)
        return;

    for (ULL i = 0; i < handle_count; i++) {
        EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *bridge = (void*)0;
        if (BS->HandleProtocol(handles[i], (EFI_GUID *)&PCI_ROOT_BRIDGE_IO_GUID, (void **)&bridge) != EFI_SUCCESS)
            continue;
        if (bridge->SegmentNumber != 0)
            continue;

        void *resources = (void*)0;
        if (bridge->Configuration(bridge, &resources) != EFI_SUCCESS || !resources)
            continue;

        ACPI_QWORD_ADDRESS_SPACE_DESCRIPTOR *d = (ACPI_QWORD_ADDRESS_SPACE_DESCRIPTOR *)resources;
        for (; d->desc == ACPI_ADDRESS_SPACE_DESCRIPTOR; d++) {
            if (d->res_type != ACPI_ADDRESS_SPACE_TYPE_MEM || d->addr_len == 0)
                continue;
            if (params->pci_mmio_count >= HORIZON_BOOT_PARAMS_MAX_PCI_MMIO)
                return;
            // The descriptor holds bus addresses; the translation offset is what
            // must be added to a CPU address to get one.
            KBootPciMmioRange *r = &params->pci_mmio[params->pci_mmio_count++];
            r->base = d->addr_range_min - d->addr_translation_offset;
            r->size = d->addr_len;
        }
    }
}

// Memory types that are ordinary RAM the kernel may take over once boot
// services have exited. Reserved, unusable, MMIO and persistent ranges are not.
static int is_ram_type(unsigned int type) {
    switch (type) {
        case EfiLoaderCode:
        case EfiLoaderData:
        case EfiBootServicesCode:
        case EfiBootServicesData:
        case EfiRuntimeServicesCode:
        case EfiRuntimeServicesData:
        case EfiConventionalMemory:
        case EfiACPIReclaimMemory:
        case EfiACPIMemoryNVS:
            return 1;
        default:
            return 0;
    }
}

// The kernel treats DRAM as one contiguous block starting at the boot params
// address. Measure that block from the UEFI memory map: walk forward from the
// base for as long as the map describes RAM, and stop at the first hole.
// Returns 0 (= use the kernel default) if the map cannot be read or no RAM
// starts at the base.
static ULL detect_dram_size(EFI_BOOT_SERVICES *BS, EFI_TEXT_OUT *console) {
    const ULL base = HORIZON_BOOT_PARAMS_PHYS_ADDR;

    ULL mapSize = 0, mapKey, descSize;
    unsigned int descVer;
    BS->GetMemoryMap(&mapSize, 0, &mapKey, &descSize, &descVer);

    // Allocating the buffer can itself add entries to the map.
    mapSize += 8 * descSize;
    void *map = 0;
    if (BS->AllocatePool(EfiLoaderData, mapSize, &map) != EFI_SUCCESS) {
        print(console, "Failed to allocate memory map, using default DRAM size\n");
        return 0;
    }
    if (BS->GetMemoryMap(&mapSize, map, &mapKey, &descSize, &descVer) != EFI_SUCCESS) {
        print(console, "Failed to read memory map, using default DRAM size\n");
        ((EFI_STATUS (*)(void*))BS->FreePool)(map);
        return 0;
    }

    // The map is not guaranteed to be sorted, so repeatedly look for the RAM
    // entry that contains the current end and extend past it.
    ULL end = base;
    for (;;) {
        ULL next = end;
        for (ULL off = 0; off + descSize <= mapSize; off += descSize) {
            EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((char *)map + off);
            ULL d_end = d->PhysicalStart + (d->NumberOfPages << 12);
            if (is_ram_type(d->Type) && d->PhysicalStart <= end && d_end > next)
                next = d_end;
        }
        if (next == end) break;
        end = next;
    }

    ((EFI_STATUS (*)(void*))BS->FreePool)(map);

    // Whole megabytes only.
    return (end - base) & ~0xFFFFFULL;
}

static void collect_rng_seed(EFI_BOOT_SERVICES *BS, EFI_TEXT_OUT *console, KBootParameters *params) {
    void *interface = 0;
    params->rng_seed_magic = 0;
    EFI_STATUS status = BS->LocateProtocol(&EFI_RNG_PROTOCOL_GUID, 0, &interface);
    if (status == EFI_SUCCESS) {
        EFI_RNG_PROTOCOL *rng = interface;
        status = rng->GetRNG(rng, 0, sizeof(params->rng_seed), params->rng_seed);
        if (status == EFI_SUCCESS) {
            params->rng_seed_magic = HORIZON_BOOT_RNG_SEED_MAGIC;
            return;
        }
    }

    // Firmware can leave partial output on failure.
    for (unsigned int i = 0; i < sizeof(params->rng_seed); ++i)
        params->rng_seed[i] = 0;
    print(console, "UEFI RNG unavailable; secure random SMC will be disabled\n");
}

static EFI_STATUS setup_boot_params(EFI_BOOT_SERVICES *BS, void *acpi,
                                     EFI_TEXT_OUT *console) {
    EFI_PHYSICAL_ADDRESS addr = HORIZON_BOOT_PARAMS_PHYS_ADDR;
    EFI_STATUS status = BS->AllocatePages(AllocateAddress, EfiLoaderData, 1, &addr);
    if (status != EFI_SUCCESS) {
        print(console, "Failed to allocate boot params page\n");
        return status;
    }

    KBootParameters *p = (KBootParameters *)(ULL)addr;
    p->magic                   = HORIZON_BOOT_PARAMS_MAGIC;
    p->dram_base               = 0;
    p->dram_size               = 0;
    p->gic_distributor_base    = 0;
    p->gic_redistributor_base  = 0;
    p->gic_redistributor_size  = 0;
    p->uart_base               = 0;
    p->uart_clock_hz           = 0;
    p->cpu_count               = 0;
    p->pci_ecam_base           = 0;
    p->pci_start_bus           = 0;
    p->pci_end_bus             = 0;
    p->pci_mmio_count          = 0;
    p->pmu_interrupt           = 0;
    for (int i = 0; i < HORIZON_BOOT_PARAMS_MAX_PCI_MMIO; i++) {
        p->pci_mmio[i].base = 0;
        p->pci_mmio[i].size = 0;
    }

    collect_rng_seed(BS, console, p);

    ACPI_RSDP *rsdp = (ACPI_RSDP *)acpi;
    void *xsdt = (void *)(ULL)rsdp->xsdt_address;

    void *madt = find_acpi_table(xsdt, "APIC");
    if (madt) {
        print(console, "Parsing MADT\n");
        parse_madt(madt, p);
    } else {
        print(console, "MADT not found!\n");
    }

    void *spcr = find_acpi_table(xsdt, "SPCR");
    if (spcr) {
        print(console, "Parsing SPCR\n");
        parse_spcr(spcr, p);
    } else {
        print(console, "SPCR not found, using default UART 0x09000000\n");
        p->uart_base = 0x09000000ULL;
        p->uart_clock_hz = 24000000;
    }

    void *mcfg = find_acpi_table(xsdt, "MCFG");
    if (mcfg) {
        print(console, "Parsing MCFG\n");
        parse_mcfg(mcfg, p);
        find_pci_mmio_ranges(BS, p);
    } else {
        print(console, "MCFG not found, no PCI host\n");
    }

    p->dram_size = detect_dram_size(BS, console);

    print_hex_labeled(console, "GIC dist:", p->gic_distributor_base);
    print_hex_labeled(console, "GIC redis:", p->gic_redistributor_base);
    print_hex_labeled(console, "GIC rdsz:", p->gic_redistributor_size);
    print_hex_labeled(console, "UART:", p->uart_base);
    print_hex_labeled(console, "CPUs:", p->cpu_count);
    print_hex_labeled(console, "PMU irq:", p->pmu_interrupt);
    print_hex_labeled(console, "DRAM size:", p->dram_size);
    print_hex_labeled(console, "PCI ECAM:", p->pci_ecam_base);
    print_hex_range(console, "PCI buses:", p->pci_start_bus, p->pci_end_bus);
    for (unsigned int i = 0; i < p->pci_mmio_count; i++) {
        print_hex_with_size(console, "PCI MMIO:", p->pci_mmio[i].base, p->pci_mmio[i].size);
    }

    return EFI_SUCCESS;
}

static EFI_STATUS open_root(EFI_BOOT_SERVICES *BS, EFI_HANDLE ImageHandle,
                              EFI_TEXT_OUT *console, EFI_FILE_PROTOCOL **root_out) {
    EFI_LOADED_IMAGE_PROTOCOL *loadedImage = 0;
    EFI_STATUS status = BS->HandleProtocol(ImageHandle, &LOADED_IMAGE_GUID,
                                            (void **)&loadedImage);
    if (status != EFI_SUCCESS) {
        print(console, "Could not get handle to loaded image\n");
        return status;
    }

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fileSystem = 0;
    status = BS->HandleProtocol(loadedImage->DeviceHandle, &SIMPLE_FS_GUID,
                                 (void **)&fileSystem);
    if (status != EFI_SUCCESS) {
        print(console, "Could not get handle to the file system\n");
        return status;
    }

    status = fileSystem->OpenVolume(fileSystem, root_out);
    if (status != EFI_SUCCESS)
        print(console, "OpenVolume failed\n");
    return status;
}


static EFI_STATUS load_kernel(EFI_BOOT_SERVICES *BS, EFI_FILE_PROTOCOL *root,
                                EFI_TEXT_OUT *console, ULL extra_bytes,
                                EFI_PHYSICAL_ADDRESS *addr_out, ULL *size_out) {
    EFI_FILE_PROTOCOL *kernelFile = 0;
    EFI_STATUS status = root->Open(root, &kernelFile, L"\\kernel.bin",
                                    EFI_FILE_MODE_READ, 0);
    if (status != EFI_SUCCESS) {
        print(console, "kernel.bin not found\n");
        return status;
    }

    unsigned long infoBuffer[64];
    ULL infoSize = sizeof(infoBuffer);
    status = kernelFile->GetInfo(kernelFile, (EFI_GUID *)&FILE_INFO_GUID,
                                  &infoSize, infoBuffer);
    if (status != EFI_SUCCESS) {
        print(console, "Unable to get information about kernel file\n");
        return status;
    }
    ULL kernelSize = ((EFI_FILE_INFO *)infoBuffer)->FileSize;

    // extra_bytes is the KIP payload; one more page covers re-aligning
    // kernel_ldr after the grown INI1 (see inject_kips).
    ULL pages = (kernelSize + extra_bytes + 0x1000ULL + 0xFFFULL) >> 12;
    EFI_PHYSICAL_ADDRESS loadAddress = 0x40100000ULL;
    status = BS->AllocatePages(AllocateAddress, EfiLoaderData, pages, &loadAddress);
    if (status != EFI_SUCCESS) {
        print(console, "Failed to allocate pages for the kernel\n");
        return status;
    }

    status = kernelFile->Read(kernelFile, &kernelSize, (void*)loadAddress);
    if (status != EFI_SUCCESS) {
        print(console, "Failed to copy kernel into memory\n");
        return status;
    }

    kernelFile->Close(kernelFile);
    *addr_out = loadAddress;
    *size_out = kernelSize;
    return EFI_SUCCESS;
}


static EFI_STATUS exit_boot_services(EFI_BOOT_SERVICES *BS, EFI_HANDLE ImageHandle,
                                      EFI_TEXT_OUT *console) {
    ULL mapSize = 0, mapKey, descSize;
    unsigned int descVer;
    void *map = 0;
    BS->GetMemoryMap(&mapSize, 0, &mapKey, &descSize, &descVer);

    mapSize += 8 * descSize;
    EFI_STATUS status = BS->AllocatePool(EfiLoaderData, mapSize, (void **)&map);
    if (status != EFI_SUCCESS) {
        print(console, "Failed to allocate memory pool\n");
        return status;
    }

    for (int attempt = 0; attempt < 3; attempt++) {
        ULL size = mapSize;
        if (BS->GetMemoryMap(&size, map, &mapKey, &descSize, &descVer) != EFI_SUCCESS) continue;
        status = BS->ExitBootServices(ImageHandle, mapKey);
        if (status == EFI_SUCCESS) return EFI_SUCCESS;
    }

    print(console, "Failed to exit boot services\n");
    return status;
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE* ST) {
    EFI_BOOT_SERVICES* BS = ST->BootServices;
    EFI_TEXT_OUT* console = ST->ConOut;

    print(console, "HorizonUEFI init...\n");
                                                                            

    void *FDT = 0, *ACPI = 0;
    EFI_STATUS status = find_hardware_tables(ST, console, &FDT, &ACPI);
    if (status != EFI_SUCCESS) return status;

    if (ACPI) {
        status = setup_boot_params(BS, ACPI, console);
        if (status != EFI_SUCCESS) return status;
    }

    EFI_FILE_PROTOCOL *root = 0;
    status = open_root(BS, ImageHandle, console, &root);
    if (status != EFI_SUCCESS) return status;

    KipEntry kips[MAX_KIPS];
    int kip_count = 0;
    ULL kips_total = 0;
    collect_kips(BS, root, console, kips, &kip_count, &kips_total);

    EFI_PHYSICAL_ADDRESS loadAddress;
    ULL kernelSize;
    status = load_kernel(BS, root, console, kips_total, &loadAddress, &kernelSize);
    root->Close(root);
    if (status != EFI_SUCCESS) return status;
    print_address_range(console, "kernel", loadAddress, loadAddress + kernelSize);

    inject_kips(console, (void *)loadAddress, kernelSize, kips, kip_count);

    for (int i = 0; i < kip_count; i++)
        ((EFI_STATUS (*)(void*))BS->FreePool)(kips[i].data);

    status = exit_boot_services(BS, ImageHandle, console);
    if (status != EFI_SUCCESS) return status;

    void (*kernelStart)(void *) = (void (*)(void *))loadAddress;
    // Mesosphere takes in a FDT but does nothing with it, so it doesn't matter if it's invalid
    kernelStart(FDT);
    __builtin_unreachable();
}

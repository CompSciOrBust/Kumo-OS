/*
 * Platform services shared by every system module that drives hardware:
 * where the PCI host is, how to reach device memory, and how to turn a
 * buffer into an address a device can use for DMA.
 *
 * This is the only place that knows how a module learns about the machine.
 * The bootloader reads the PCI host from ACPI and UEFI and passes it to the
 * kernel; the kernel reports it through GetConfig and lets a driver process
 * map device memory through an I/O pool.
 */
#pragma once
#include <switch.h>
#include <cstddef>
#include <cstdint>

namespace kumo::platform {

    /* Which of the kernel's I/O pools a module maps device memory from. The kernel allows */
    /* one live pool of each kind, so each driver process uses its own. Values match the */
    /* kernel's svc::IoPoolType for the generic-aarch64 board. */
    enum IoPool : u32 {
        IoPool_PciStorage = 0,
        IoPool_PciNetwork = 1,
    };

    struct PciHostInfo {
        u64 ecam_physical_address;
        u8  start_bus;
        u8  end_bus;    /* inclusive */
    };

    /* Choose this process's I/O pool. Call once, before anything else here. */
    void Initialize(IoPool pool);

    /* Returns false when the machine has no PCI host. */
    bool GetPciHostInfo(PciHostInfo *out);

    /* Map a range of device memory and return its virtual address. The mapping is permanent. */
    /* A range may be mapped only once, and must lie inside the PCI host's memory. */
    ::Result MapIo(uintptr_t *out, u64 physical_address, size_t size);

    /* Physical address backing a virtual address in this process. */
    ::Result GetPhysicalAddress(u64 *out, const void *address);

    /* As GetPhysicalAddress, and also how many bytes from `address` onward are physically */
    /* contiguous with it (at least to the end of its page). */
    ::Result GetPhysicalRange(u64 *out_address, size_t *out_contiguous_size, const void *address);

}

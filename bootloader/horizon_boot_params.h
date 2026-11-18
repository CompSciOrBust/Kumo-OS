#pragma once

#define HORIZON_BOOT_PARAMS_MAGIC      0x48425030ULL  // "HBP0"
#define HORIZON_BOOT_PARAMS_PHYS_ADDR  0x40000000ULL

// PCI memory windows the kernel lets driver processes map, besides the ECAM itself.
#define HORIZON_BOOT_PARAMS_MAX_PCI_MMIO  4

typedef struct {
    unsigned long long base;
    unsigned long long size;
} KBootPciMmioRange;

// Fields are only ever appended, so an older kernel reads a newer struct correctly.
typedef struct {
    unsigned long long magic;
    unsigned long long dram_base;              // 0 = use kernel default
    unsigned long long dram_size;              // contiguous RAM from the params address; 0 = use kernel default
    unsigned long long gic_distributor_base;
    unsigned long long gic_redistributor_base;
    unsigned long long gic_redistributor_size;
    unsigned long long uart_base;
    unsigned int       uart_clock_hz;
    unsigned int       cpu_count;         // as reported by MADT; 0 = unknown
    unsigned long long pci_ecam_base;     // from ACPI MCFG; 0 = no PCI host
    unsigned int       pci_start_bus;
    unsigned int       pci_end_bus;       // inclusive
    unsigned int       pci_mmio_count;    // valid entries in pci_mmio
    unsigned int       pmu_interrupt;     // PMU overflow interrupt from MADT; 0 = the firmware describes no PMU
    KBootPciMmioRange  pci_mmio[HORIZON_BOOT_PARAMS_MAX_PCI_MMIO];  // host bridge memory resources, from UEFI
} KBootParameters;

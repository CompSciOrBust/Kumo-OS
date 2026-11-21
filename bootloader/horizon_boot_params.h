#pragma once

#define HORIZON_BOOT_PARAMS_MAGIC      0x48425030ULL  // "HBP0"
#define HORIZON_BOOT_PARAMS_PHYS_ADDR  0x40000000ULL

#define HORIZON_BOOT_RNG_SEED_MAGIC 0x48425030524e4731ULL  // "HBP0RNG1"
#define HORIZON_BOOT_RNG_SEED_SIZE  32

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
    unsigned long long rng_seed_magic;    // set only after EFI_RNG_PROTOCOL succeeds
    unsigned char      rng_seed[HORIZON_BOOT_RNG_SEED_SIZE];
} KBootParameters;

_Static_assert(__builtin_offsetof(KBootParameters, rng_seed_magic) == 152, "boot RNG marker offset");
_Static_assert(sizeof(KBootParameters) == 192, "boot parameter size");

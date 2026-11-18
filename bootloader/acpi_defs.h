#pragma once

typedef struct __attribute__((packed)) {
    unsigned char      address_space_id;
    unsigned char      register_bit_width;
    unsigned char      register_bit_offset;
    unsigned char      access_size;
    unsigned long long address;
} ACPI_GAS;

typedef struct __attribute__((packed)) {
    char          signature[4];
    unsigned int  length;
    unsigned char revision;
    unsigned char checksum;
    char          oem_id[6];
    char          oem_table_id[8];
    unsigned int  oem_revision;
    unsigned int  creator_id;
    unsigned int  creator_revision;
} ACPI_TABLE_HEADER;

typedef struct __attribute__((packed)) {
    char               signature[8];
    unsigned char      checksum;
    char               oem_id[6];
    unsigned char      revision;
    unsigned int       rsdt_address;
    unsigned int       length;
    unsigned long long xsdt_address;
    unsigned char      extended_checksum;
    unsigned char      reserved[3];
} ACPI_RSDP;

typedef struct __attribute__((packed)) {
    ACPI_TABLE_HEADER header;
    unsigned int      local_ic_address;
    unsigned int      flags;
} ACPI_MADT;

typedef struct __attribute__((packed)) {
    unsigned char type;
    unsigned char length;
} ACPI_MADT_SUBTABLE;

#define ACPI_MADT_TYPE_GICC  0x0B
#define ACPI_MADT_TYPE_GICD  0x0C
#define ACPI_MADT_TYPE_GICR  0x0E

// GIC CPU interface: one per processor. Only the leading fields are declared.
typedef struct __attribute__((packed)) {
    unsigned char      type;             // 0x0B
    unsigned char      length;
    unsigned short     reserved;
    unsigned int       cpu_interface_number;
    unsigned int       acpi_processor_uid;
    unsigned int       flags;            // bit 0: enabled
    unsigned int       parking_protocol_version;
    unsigned int       performance_interrupt_gsiv;  // PMU overflow interrupt; 0 = no PMU
} ACPI_MADT_GICC;

#define ACPI_MADT_GICC_ENABLED 0x1

typedef struct __attribute__((packed)) {
    unsigned char      type;             // 0x0C
    unsigned char      length;           // 24
    unsigned short     reserved;
    unsigned int       gic_id;
    unsigned long long base_address;
    unsigned int       global_irq_base;
    unsigned char      version;
    unsigned char      reserved2[3];
} ACPI_MADT_GICD;

typedef struct __attribute__((packed)) {
    unsigned char      type;             // 0x0E
    unsigned char      length;           // 16
    unsigned short     reserved;
    unsigned long long discovery_range_base;
    unsigned int       discovery_range_length;
} ACPI_MADT_GICR;

typedef struct __attribute__((packed)) {
    ACPI_TABLE_HEADER header;
    unsigned char     interface_type;
    unsigned char     reserved1[3];
    ACPI_GAS          base_address;
    unsigned char     interrupt_type;
    unsigned char     irq;
    unsigned int      gsiv;
    unsigned char     baud_rate;
    unsigned char     parity;
    unsigned char     stop_bits;
    unsigned char     flow_control;
    unsigned char     terminal_type;
    unsigned char     language;
    unsigned short    pci_device_id;
    unsigned short    pci_vendor_id;
    unsigned char     pci_bus;
    unsigned char     pci_device;
    unsigned char     pci_function;
    unsigned int      pci_flags;
    unsigned char     pci_segment;
    unsigned int      uart_clock_hz;     // revision 3+
} ACPI_SPCR;

// MCFG: PCI Express memory-mapped configuration space (ECAM).
typedef struct __attribute__((packed)) {
    unsigned long long base_address;
    unsigned short     segment_group;
    unsigned char      start_bus;
    unsigned char      end_bus;
    unsigned int       reserved;
} ACPI_MCFG_ALLOCATION;

typedef struct __attribute__((packed)) {
    ACPI_TABLE_HEADER    header;
    unsigned long long   reserved;
    ACPI_MCFG_ALLOCATION allocations[];
} ACPI_MCFG;

// QWORD address space descriptor, as returned by
// EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL.Configuration(). The list ends with an end tag (0x79).
#define ACPI_ADDRESS_SPACE_DESCRIPTOR  0x8A
#define ACPI_END_TAG_DESCRIPTOR        0x79
#define ACPI_ADDRESS_SPACE_TYPE_MEM    0

typedef struct __attribute__((packed)) {
    unsigned char      desc;
    unsigned short     len;
    unsigned char      res_type;
    unsigned char      gen_flag;
    unsigned char      specific_flag;
    unsigned long long addr_space_granularity;
    unsigned long long addr_range_min;
    unsigned long long addr_range_max;
    unsigned long long addr_translation_offset;
    unsigned long long addr_len;
} ACPI_QWORD_ADDRESS_SPACE_DESCRIPTOR;

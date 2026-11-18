/*
 * PCI access for driver system modules.
 *
 * This library does not own the bus. The UEFI firmware has already numbered
 * the buses and assigned every BAR, so a module only scans, picks the devices
 * of its own class, and touches nothing else. That is what lets FS and a
 * future network module each link this library without a bus-manager process.
 */
#pragma once
#include <switch.h>
#include <cstddef>
#include <cstdint>

namespace kumo::pci {

    struct Location {
        u8 bus;
        u8 device;
        u8 function;
    };

    struct DeviceInfo {
        Location location;
        u16 vendor_id;
        u16 device_id;
        u8  base_class;
        u8  sub_class;
        u8  prog_if;
        u8  revision;
    };

    struct BarInfo {
        u64  address;
        u64  size;
        bool is_memory;
        bool is_64_bit;
    };

    /* Locate and map the PCI host. Safe to call more than once. Fails if this machine has none we can reach. */
    ::Result Initialize();

    /* Fill `out` with up to `max` devices, in bus order. Returns how many exist in total. */
    int EnumerateDevices(DeviceInfo *out, int max);

    /* The `index`-th device (from zero) with the given class code. */
    bool FindDeviceByClass(DeviceInfo *out, u8 base_class, u8 sub_class, u8 prog_if, int index);

    /* The `index`-th device (from zero) with the given vendor and device ID. */
    bool FindDeviceById(DeviceInfo *out, u16 vendor_id, u16 device_id, int index);

    /* Capability list. Returns the configuration-space offset of the next capability with the given ID, */
    /* searching after `after` (pass 0 to start from the head of the list), or 0 if there is none. */
    u32 FindCapability(const Location &loc, u8 capability_id, u32 after = 0);

    /* Configuration space. */
    u8   ReadConfig8(const Location &loc, u32 offset);
    u16  ReadConfig16(const Location &loc, u32 offset);
    u32  ReadConfig32(const Location &loc, u32 offset);
    void WriteConfig16(const Location &loc, u32 offset, u16 value);
    void WriteConfig32(const Location &loc, u32 offset, u32 value);

    /* Base address registers, as assigned by the firmware. */
    ::Result GetBar(BarInfo *out, const Location &loc, int bar);
    ::Result MapBar(uintptr_t *out_address, size_t *out_size, const Location &loc, int bar);

    /* Turn on memory decoding and bus mastering (DMA) for a device. */
    void EnableMemoryAndBusMaster(const Location &loc);

    constexpr inline u32 ConfigOffset_VendorId   = 0x00;
    constexpr inline u32 ConfigOffset_DeviceId   = 0x02;
    constexpr inline u32 ConfigOffset_Command    = 0x04;
    constexpr inline u32 ConfigOffset_Revision   = 0x08;
    constexpr inline u32 ConfigOffset_ProgIf     = 0x09;
    constexpr inline u32 ConfigOffset_SubClass   = 0x0A;
    constexpr inline u32 ConfigOffset_BaseClass  = 0x0B;
    constexpr inline u32 ConfigOffset_HeaderType = 0x0E;
    constexpr inline u32 ConfigOffset_Bar0       = 0x10;
    constexpr inline u32 ConfigOffset_SubsystemVendorId = 0x2C;
    constexpr inline u32 ConfigOffset_SubsystemId       = 0x2E;
    constexpr inline u32 ConfigOffset_CapabilityPointer = 0x34;
    constexpr inline u32 ConfigOffset_Status     = 0x06;

    constexpr inline u16 Status_CapabilityList = (1 << 4);

    constexpr inline u8 CapabilityId_Msi            = 0x05;
    constexpr inline u8 CapabilityId_VendorSpecific = 0x09;
    constexpr inline u8 CapabilityId_PciExpress     = 0x10;
    constexpr inline u8 CapabilityId_MsiX           = 0x11;

    constexpr inline u16 Command_IoSpace     = (1 << 0);
    constexpr inline u16 Command_MemorySpace = (1 << 1);
    constexpr inline u16 Command_BusMaster   = (1 << 2);

}

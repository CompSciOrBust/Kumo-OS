/*
 * Virtio over PCI, for driver system modules: the modern (virtio 1.0+)
 * transport and a split virtqueue.
 *
 * Only what a polled driver needs is here. There is no interrupt support
 * (the kernel has no MSI/MSI-X yet), and no legacy (0.9.5) I/O-port
 * interface: transitional devices are driven through their modern
 * capabilities, which QEMU always provides.
 *
 * This library owns no memory. A driver supplies the pages for each queue and
 * for its buffers, so a module that links this code without using it pays
 * nothing.
 */
#pragma once
#include <switch.h>
#include <cstddef>
#include <cstdint>
#include <kumo/pci.hpp>

namespace kumo::virtio {

    constexpr inline u16 PciVendorId = 0x1AF4;

    /* Device types, as in the virtio specification. */
    constexpr inline u16 DeviceType_Net   = 1;
    constexpr inline u16 DeviceType_Block = 2;

    /* Device status bits. */
    constexpr inline u8 Status_Acknowledge = 0x01;
    constexpr inline u8 Status_Driver      = 0x02;
    constexpr inline u8 Status_DriverOk    = 0x04;
    constexpr inline u8 Status_FeaturesOk  = 0x08;
    constexpr inline u8 Status_NeedsReset  = 0x40;
    constexpr inline u8 Status_Failed      = 0x80;

    /* Feature bits that do not depend on the device type. */
    constexpr inline u64 Feature_Version1 = (UINT64_C(1) << 32);

    constexpr inline size_t PageSize = 0x1000;

    /* The `index`-th virtio device of the given type on the PCI bus, modern or transitional. */
    bool FindDevice(pci::DeviceInfo *out, u16 device_type, int index);

    /* The modern virtio-pci transport of one device. */
    class Transport {
        private:
            pci::Location m_location;
            uintptr_t m_bars[6];
            uintptr_t m_common;
            uintptr_t m_notify;
            uintptr_t m_device;
            u32 m_notify_multiplier;
            u32 m_device_config_size;
        public:
            constexpr Transport() : m_location(), m_bars(), m_common(0), m_notify(0), m_device(0), m_notify_multiplier(0), m_device_config_size(0) { /* ... */ }

            /* Find and map the device's virtio structures, and enable memory decoding and bus mastering. */
            ::Result Initialize(const pci::DeviceInfo &device);

            /* Reset the device and wait for it to complete. */
            ::Result Reset();

            u8   GetStatus() const;
            void AddStatus(u8 bits);

            /* Offer `wanted & device features`, and check that the device accepted them. `required` must all be available. */
            ::Result NegotiateFeatures(u64 *out_features, u64 wanted, u64 required);

            u16 GetQueueCount() const;

            /* The largest size the device supports for a queue; zero if the queue does not exist. */
            u16 GetQueueMaxSize(u16 queue) const;

            /* Tell the device where a queue's three areas are (physical addresses) and enable it. */
            /* Returns the address the driver writes the queue index to when it has added buffers. */
            ::Result EnableQueue(volatile u16 **out_notify, u16 queue, u16 size, u64 descriptors, u64 driver_area, u64 device_area);

            /* Device-specific configuration. */
            u32 GetDeviceConfigSize() const { return m_device_config_size; }
            u8  ReadDeviceConfig8(u32 offset) const;
            u16 ReadDeviceConfig16(u32 offset) const;
            u32 ReadDeviceConfig32(u32 offset) const;
        private:
            ::Result MapStructure(uintptr_t *out, u32 *out_length, u32 capability);
    };

    /*
     * A split virtqueue in which every request is a single buffer. Descriptor
     * `i` always describes buffer `i`, so there is no descriptor allocation:
     * the driver sets each descriptor up once and afterwards passes buffer
     * numbers back and forth.
     *
     * The three areas are one page each, which limits a queue to MaxSize
     * entries and means no area has to be physically contiguous across pages.
     */
    class Queue {
        public:
            static constexpr u16 MaxSize = 256;
        private:
            struct Descriptor {
                u64 address;
                u32 length;
                u16 flags;
                u16 next;
            };
            struct UsedElement {
                u32 id;
                u32 length;
            };

            Descriptor *m_descriptors;
            volatile u16 *m_avail;   /* flags, idx, ring[size] */
            volatile u16 *m_used;    /* flags, idx, then UsedElement ring[size] */
            volatile u16 *m_notify;
            u16 m_size;
            u16 m_avail_index;
            u16 m_last_used_index;
            u16 m_index;
        public:
            constexpr Queue() : m_descriptors(nullptr), m_avail(nullptr), m_used(nullptr), m_notify(nullptr), m_size(0), m_avail_index(0), m_last_used_index(0), m_index(0) { /* ... */ }

            /* `memory` is three zeroed, page-aligned pages owned by the caller for the life of the device. */
            /* `size` is clamped to MaxSize and to what the device supports. */
            ::Result Initialize(Transport *transport, u16 queue, u16 size, void *memory);

            u16 GetSize() const { return m_size; }

            /* Describe buffer `id`. `device_writable` is true for buffers the device fills (receive). */
            void SetBuffer(u16 id, u64 physical_address, u32 length, bool device_writable);
            void SetBufferLength(u16 id, u32 length);

            /* Hand buffer `id` to the device. Call Kick() after one or more of these. */
            void Submit(u16 id);
            void Kick();

            /* Take back one buffer the device has finished with. */
            bool PopUsed(u16 *out_id, u32 *out_length);
    };

}

#include <kumo/detail.hpp>
#include <kumo/mutex.hpp>
#include <algorithm>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <kumo/virtio.hpp>
#include <kumo/platform.hpp>

namespace kumo::virtio {

    namespace {

        /* struct virtio_pci_cap, a vendor-specific PCI capability. */
        constexpr u32 Cap_Length     = 2;
        constexpr u32 Cap_ConfigType = 3;
        constexpr u32 Cap_Bar        = 4;
        constexpr u32 Cap_Offset     = 8;
        constexpr u32 Cap_StructLength = 12;
        constexpr u32 Cap_NotifyMultiplier = 16;

        constexpr u8 ConfigType_Common = 1;
        constexpr u8 ConfigType_Notify = 2;
        constexpr u8 ConfigType_Device = 4;

        /* struct virtio_pci_common_cfg. */
        constexpr u32 Common_DeviceFeatureSelect = 0x00;
        constexpr u32 Common_DeviceFeature       = 0x04;
        constexpr u32 Common_DriverFeatureSelect = 0x08;
        constexpr u32 Common_DriverFeature       = 0x0C;
        constexpr u32 Common_MsixConfig          = 0x10;
        constexpr u32 Common_NumQueues           = 0x12;
        constexpr u32 Common_DeviceStatus        = 0x14;
        constexpr u32 Common_QueueSelect         = 0x16;
        constexpr u32 Common_QueueSize           = 0x18;
        constexpr u32 Common_QueueMsixVector     = 0x1A;
        constexpr u32 Common_QueueEnable         = 0x1C;
        constexpr u32 Common_QueueNotifyOff      = 0x1E;
        constexpr u32 Common_QueueDesc           = 0x20;
        constexpr u32 Common_QueueDriver         = 0x28;
        constexpr u32 Common_QueueDevice         = 0x30;
        constexpr u32 Common_Size                = 0x38;

        constexpr u16 NoMsixVector = 0xFFFF;

        /* Transitional devices use IDs 0x1000-0x103F with the type in the subsystem ID; modern ones use 0x1040 + type. */
        constexpr u16 TransitionalDeviceIdMin = 0x1000;
        constexpr u16 TransitionalDeviceIdMax = 0x103F;
        constexpr u16 ModernDeviceIdBase      = 0x1040;

        constexpr u16 DescriptorFlag_Write = 2;
        constexpr u16 AvailFlag_NoInterrupt = 1;
        constexpr u16 UsedFlag_NoNotify = 1;

        template<typename T> inline T Read(uintptr_t address) { return *reinterpret_cast<const volatile T *>(address); }
        template<typename T> inline void Write(uintptr_t address, T value) { *reinterpret_cast<volatile T *>(address) = value; }

        inline void MemoryBarrier() {
            __asm__ __volatile__("dmb sy" ::: "memory");
        }

    }

    bool FindDevice(pci::DeviceInfo *out, u16 device_type, int index) {
        constexpr int MaxDevices = 64;
        static constinit pci::DeviceInfo s_devices[MaxDevices];
        static constinit ::kumo::Mutex s_mutex;

        std::scoped_lock lk(s_mutex);
        const int count = std::min(pci::EnumerateDevices(s_devices, MaxDevices), MaxDevices);
        for (int i = 0; i < count; ++i) {
            const auto &d = s_devices[i];
            if (d.vendor_id != PciVendorId) {
                continue;
            }

            bool match = false;
            if (d.device_id == ModernDeviceIdBase + device_type) {
                match = true;
            } else if (TransitionalDeviceIdMin <= d.device_id && d.device_id <= TransitionalDeviceIdMax) {
                match = pci::ReadConfig16(d.location, pci::ConfigOffset_SubsystemId) == device_type;
            }

            if (match && index-- == 0) {
                *out = d;
                return true;
            }
        }
        return false;
    }

    ::Result Transport::MapStructure(uintptr_t *out, u32 *out_length, u32 capability) {
        const u8  bar    = pci::ReadConfig8(m_location, capability + Cap_Bar);
        const u32 offset = pci::ReadConfig32(m_location, capability + Cap_Offset);
        const u32 length = pci::ReadConfig32(m_location, capability + Cap_StructLength);
        KUMO_UNLESS(bar < std::size(m_bars), MAKERESULT(Module_Kernel, KernelError_OutOfRange));

        /* Several structures normally share one BAR, and a range can be mapped only once. */
        if (m_bars[bar] == 0) {
            size_t size;
            KUMO_TRY(pci::MapBar(std::addressof(m_bars[bar]), std::addressof(size), m_location, bar));
        }

        pci::BarInfo info;
        KUMO_TRY(pci::GetBar(std::addressof(info), m_location, bar));
        KUMO_UNLESS(static_cast<u64>(offset) + length <= info.size, MAKERESULT(Module_Kernel, KernelError_OutOfRange));

        *out = m_bars[bar] + offset;
        if (out_length != nullptr) {
            *out_length = length;
        }
        return 0;
    }

    ::Result Transport::Initialize(const pci::DeviceInfo &device) {
        m_location = device.location;

        /* The virtio structures are named by vendor-specific capabilities. The first of each type wins. */
        u32 common = 0, notify = 0, device_config = 0;
        bool visited[256]{};
        for (u32 cap = pci::FindCapability(m_location, pci::CapabilityId_VendorSpecific); cap != 0; cap = pci::FindCapability(m_location, pci::CapabilityId_VendorSpecific, cap)) {
            KUMO_UNLESS(cap < 256 && !visited[cap], MAKERESULT(Module_Kernel, KernelError_InvalidState));
            visited[cap] = true;
            const u8 length = pci::ReadConfig8(m_location, cap + Cap_Length);
            const u8 type   = pci::ReadConfig8(m_location, cap + Cap_ConfigType);
            if (length < 16) {
                continue;
            }

            /* Structures in I/O space cannot be used here. */
            pci::BarInfo info;
            const u8 bar = pci::ReadConfig8(m_location, cap + Cap_Bar);
            if (bar >= std::size(m_bars) || R_FAILED(pci::GetBar(std::addressof(info), m_location, bar)) || !info.is_memory || info.size == 0) {
                continue;
            }

            if (type == ConfigType_Common && common == 0) {
                common = cap;
            } else if (type == ConfigType_Notify && notify == 0 && length >= 20) {
                notify = cap;
            } else if (type == ConfigType_Device && device_config == 0) {
                device_config = cap;
            }
        }

        /* A device with no modern interface (legacy only) is not supported. */
        KUMO_UNLESS(common != 0 && notify != 0, MAKERESULT(Module_Kernel, KernelError_NotFound));

        pci::EnableMemoryAndBusMaster(m_location);

        u32 common_length;
        KUMO_TRY(this->MapStructure(std::addressof(m_common), std::addressof(common_length), common));
        KUMO_UNLESS(common_length >= Common_Size, MAKERESULT(Module_Kernel, KernelError_OutOfRange));

        KUMO_TRY(this->MapStructure(std::addressof(m_notify), nullptr, notify));
        m_notify_multiplier = pci::ReadConfig32(m_location, notify + Cap_NotifyMultiplier);

        if (device_config != 0) {
            KUMO_TRY(this->MapStructure(std::addressof(m_device), std::addressof(m_device_config_size), device_config));
        }

        return 0;
    }

    ::Result Transport::Reset() {
        Write<u8>(m_common + Common_DeviceStatus, 0);

        /* The device reports zero once the reset is complete. */
        const u64 start = ::svcGetSystemTick();
        while (Read<u8>(m_common + Common_DeviceStatus) != 0) {
            KUMO_UNLESS((::svcGetSystemTick() - start) < 19200000, MAKERESULT(Module_Kernel, KernelError_TimedOut));
            ::svcSleepThread(1000000);
        }
        return 0;
    }

    u8 Transport::GetStatus() const {
        return Read<u8>(m_common + Common_DeviceStatus);
    }

    void Transport::AddStatus(u8 bits) {
        Write<u8>(m_common + Common_DeviceStatus, Read<u8>(m_common + Common_DeviceStatus) | bits);
    }

    ::Result Transport::NegotiateFeatures(u64 *out_features, u64 wanted, u64 required) {
        Write<u32>(m_common + Common_DeviceFeatureSelect, 0);
        u64 offered = Read<u32>(m_common + Common_DeviceFeature);
        Write<u32>(m_common + Common_DeviceFeatureSelect, 1);
        offered |= static_cast<u64>(Read<u32>(m_common + Common_DeviceFeature)) << 32;

        KUMO_UNLESS((offered & required) == required, MAKERESULT(Module_Kernel, KernelError_NotFound));

        const u64 features = offered & (wanted | required);
        Write<u32>(m_common + Common_DriverFeatureSelect, 0);
        Write<u32>(m_common + Common_DriverFeature, static_cast<u32>(features));
        Write<u32>(m_common + Common_DriverFeatureSelect, 1);
        Write<u32>(m_common + Common_DriverFeature, static_cast<u32>(features >> 32));

        /* The device may refuse the combination; it says so by not keeping FEATURES_OK. */
        this->AddStatus(Status_FeaturesOk);
        KUMO_UNLESS((this->GetStatus() & Status_FeaturesOk) != 0, MAKERESULT(Module_Kernel, KernelError_InvalidState));

        /* No interrupts are used. */
        Write<u16>(m_common + Common_MsixConfig, NoMsixVector);

        *out_features = features;
        return 0;
    }

    u16 Transport::GetQueueCount() const {
        return Read<u16>(m_common + Common_NumQueues);
    }

    u16 Transport::GetQueueMaxSize(u16 queue) const {
        Write<u16>(m_common + Common_QueueSelect, queue);
        return Read<u16>(m_common + Common_QueueSize);
    }

    ::Result Transport::EnableQueue(volatile u16 **out_notify, u16 queue, u16 size, u64 descriptors, u64 driver_area, u64 device_area) {
        Write<u16>(m_common + Common_QueueSelect, queue);
        KUMO_UNLESS(Read<u16>(m_common + Common_QueueSize) >= size, MAKERESULT(Module_Kernel, KernelError_OutOfRange));
        KUMO_UNLESS(Read<u16>(m_common + Common_QueueEnable) == 0,   MAKERESULT(Module_Kernel, KernelError_InvalidState));

        Write<u16>(m_common + Common_QueueSize, size);
        Write<u16>(m_common + Common_QueueMsixVector, NoMsixVector);
        Write<u32>(m_common + Common_QueueDesc,       static_cast<u32>(descriptors));
        Write<u32>(m_common + Common_QueueDesc + 4,   static_cast<u32>(descriptors >> 32));
        Write<u32>(m_common + Common_QueueDriver,     static_cast<u32>(driver_area));
        Write<u32>(m_common + Common_QueueDriver + 4, static_cast<u32>(driver_area >> 32));
        Write<u32>(m_common + Common_QueueDevice,     static_cast<u32>(device_area));
        Write<u32>(m_common + Common_QueueDevice + 4, static_cast<u32>(device_area >> 32));

        const u16 notify_off = Read<u16>(m_common + Common_QueueNotifyOff);
        *out_notify = reinterpret_cast<volatile u16 *>(m_notify + static_cast<uintptr_t>(notify_off) * m_notify_multiplier);

        Write<u16>(m_common + Common_QueueEnable, 1);
        return 0;
    }

    u8  Transport::ReadDeviceConfig8(u32 offset) const  { KUMO_ASSERT(offset + 1 <= m_device_config_size); return Read<u8>(m_device + offset); }
    u16 Transport::ReadDeviceConfig16(u32 offset) const { KUMO_ASSERT(offset + 2 <= m_device_config_size); return Read<u16>(m_device + offset); }
    u32 Transport::ReadDeviceConfig32(u32 offset) const { KUMO_ASSERT(offset + 4 <= m_device_config_size); return Read<u32>(m_device + offset); }

    ::Result Queue::Initialize(Transport *transport, u16 queue, u16 size, void *memory) {
        const u16 max_size = transport->GetQueueMaxSize(queue);
        KUMO_UNLESS(max_size != 0, MAKERESULT(Module_Kernel, KernelError_NotFound));

        KUMO_UNLESS(size != 0, MAKERESULT(Module_Kernel, KernelError_InvalidSize));
        /* Queue sizes are powers of two. */
        size = std::min<u16>(std::min(size, max_size), MaxSize);
        size = static_cast<u16>(1u << (31 - __builtin_clz(static_cast<u32>(size))));
        static_assert(MaxSize * sizeof(Descriptor) <= PageSize);
        static_assert(3 * sizeof(u16) + MaxSize * sizeof(UsedElement) <= PageSize);

        u8 *pages = static_cast<u8 *>(memory);
        KUMO_ASSERT(::kumo::detail::IsAligned(reinterpret_cast<uintptr_t>(pages), PageSize));

        m_descriptors     = reinterpret_cast<Descriptor *>(pages);
        m_avail           = reinterpret_cast<volatile u16 *>(pages + PageSize);
        m_used            = reinterpret_cast<volatile u16 *>(pages + 2 * PageSize);
        m_size            = size;
        m_avail_index     = 0;
        m_last_used_index = 0;
        m_index           = queue;

        /* We poll, so the device need not interrupt when it uses a buffer. */
        m_avail[0] = AvailFlag_NoInterrupt;

        u64 physical[3];
        for (int i = 0; i < 3; ++i) {
            KUMO_TRY(platform::GetPhysicalAddress(std::addressof(physical[i]), pages + i * PageSize));
        }

        MemoryBarrier();
        return (transport->EnableQueue(std::addressof(m_notify), queue, size, physical[0], physical[1], physical[2]));
    }

    void Queue::SetBuffer(u16 id, u64 physical_address, u32 length, bool device_writable) {
        KUMO_ASSERT(id < m_size);
        m_descriptors[id] = { physical_address, length, static_cast<u16>(device_writable ? DescriptorFlag_Write : 0), 0 };
    }

    void Queue::SetBufferLength(u16 id, u32 length) {
        KUMO_ASSERT(id < m_size);
        m_descriptors[id].length = length;
    }

    void Queue::Submit(u16 id) {
        m_avail[2 + (m_avail_index % m_size)] = id;
        ++m_avail_index;

        /* The ring entry must be visible before the index that publishes it. */
        MemoryBarrier();
        m_avail[1] = m_avail_index;
    }

    void Queue::Kick() {
        MemoryBarrier();
        if ((m_used[0] & UsedFlag_NoNotify) == 0) {
            *m_notify = m_index;
        }
    }

    bool Queue::PopUsed(u16 *out_id, u32 *out_length) {
        if (m_used[1] == m_last_used_index) {
            return false;
        }

        // Acquire device writes published by the used index before reading entries or payload.
        MemoryBarrier();
        const volatile UsedElement *ring = reinterpret_cast<const volatile UsedElement *>(m_used + 2);
        const volatile UsedElement &element = ring[m_last_used_index % m_size];
        *out_id     = static_cast<u16>(element.id);
        *out_length = element.length;
        ++m_last_used_index;
        return true;
    }

}

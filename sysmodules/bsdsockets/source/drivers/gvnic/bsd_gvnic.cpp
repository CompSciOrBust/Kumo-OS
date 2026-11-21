/*
 * Google Virtual NIC (gVNIC) driver: the network card of Google Compute
 * Engine, and the only one Arm (T2A, C4A) instances offer.
 *
 * The device is set up through an admin queue of commands; frames then move
 * through a transmit and a receive ring in the "GQI" queue format, with a
 * queue page list (QPL): the driver registers a list of pages once, and every
 * descriptor names a byte offset into that list instead of a memory address.
 * That suits this module, whose memory is contiguous only page by page and
 * which copies every frame anyway.
 *
 * Deliberately minimal, like the virtio-net driver:
 *   - one device, one transmit and one receive queue
 *   - no offloads: lwIP computes and checks every checksum
 *   - polled. The device is given two notification blocks, because it
 *     requires them, and both are masked: it never interrupts
 *   - 256 frame buffers each way, two to a page, whatever size the device's
 *     rings are. A ring slot `i` always uses buffer `i % 256`; the driver never
 *     has more than 256 buffers outstanding, so a slot's buffer is always free
 *     when the slot comes round again
 *   - frames larger than one buffer (jumbo frames, which a VPC network can be
 *     configured for) are dropped; the MTU given to lwIP is at most 1500, so
 *     TCP peers never send them
 *
 * Rings and buffers come from the drivers' shared DMA arena.
 */
#include <atomic>
#include <kumo/prelude.hpp>
#include <kumo/scope_exit.hpp>
#include <kumo/dma.hpp>
#include <kumo/pci.hpp>
#include "bsd_gvnic_abi.hpp"
#include "../bsd_drivers.hpp"
#include "../../bsd_log.hpp"
#include "../../netdev/bsd_net_device_registry.hpp"

namespace kumo::horizon::hznet::drivers::gvnic {

    namespace {

        using kumo::dma::PageSize;
        using abi::Be16;
        using abi::Be32;
        using abi::Be64;

        /* Frame buffers: two per page. A buffer holds the device's pad and a whole frame. */
        constexpr size_t BufferSize       = PageSize / 2;
        constexpr u32    MaxBuffers       = 256;
        static_assert(abi::RxPad + netdev::MaxFrameSize <= BufferSize);

        /* Rings larger than this are refused, to bound the memory they take from the arena. */
        constexpr u32 MaxRingSize = 4096;

        constexpr u32 TxQueuePageListId = 0;
        constexpr u32 RxQueuePageListId = 1;

        /* Notification blocks: one for the transmit queue, one for the receive queue. */
        constexpr u32 NotifyBlockCount = 2;
        constexpr u32 TxNotifyBlock    = 0;
        constexpr u32 RxNotifyBlock    = 1;

        constexpr u32 DefaultMtu = 1500;
        constexpr u32 MinMtu     = 68;

        /* How long to wait for the device: 5 seconds, in 100 us steps. Linux allows an admin */
        /* command 2 seconds; the device answers this driver's at once (docs/bsdsockets.md). */
        constexpr u64      TimeoutSeconds = 5;
        constexpr s64 PollIntervalNs = 100'000;
        constexpr u64 RecoveryDelayNs = 1'000'000'000;
        constexpr u32 ResetStatus = abi::DeviceStatus_ResetRequested | abi::DeviceStatus_IsReset;

        /* Poll `done` until it returns true or the timeout passes. */
        template<typename F>
        bool WaitFor(F done) {
            const u64 start = ::armGetSystemTick();
            const u64 timeout = TimeoutSeconds * ::armGetSystemTickFreq();
            do {
                if (done()) {
                    return true;
                }
                ::svcSleepThread(PollIntervalNs);
            } while (::armGetSystemTick() - start < timeout);
            return done();
        }

        constexpr const char DriverVersion[] = "GVE-1.0.0\n";

        inline void MemoryBarrier() {
            __asm__ __volatile__("dmb sy" ::: "memory");
        }

        /* Reads and writes of memory the device also writes. */
        template<typename T> inline T ReadShared(const T *p) { return *reinterpret_cast<const volatile T *>(p); }

        constexpr bool IsPowerOfTwo(u32 v) { return v != 0 && (v & (v - 1)) == 0; }

        /* Allocate from the drivers' arena the first time; on a later setup attempt, zero and reuse what */
        /* was allocated then, so that a retry does not run the arena out. */
        ::Result AllocateOnce(kumo::dma::Buffer *buffer, size_t size, size_t alignment = PageSize) {
            if (buffer->IsValid()) {
                KUMO_UNLESS(buffer->size == size, KERNELRESULT(InvalidSize));
                std::memset(buffer->address, 0, buffer->size);
                KUMO_SUCCEED();
            }
            KUMO_RETURN(GetDmaArena().Allocate(buffer, size, alignment));
        }

        class GvnicDevice final : public netdev::INetDevice {
            private:
                kumo::pci::Location m_location;
                u8 m_revision;
                uintptr_t m_registers;
                uintptr_t m_doorbells;
                size_t m_doorbells_size;
                std::atomic<bool> m_ready{false};
                u64 m_recovery_started = 0;

                /* Admin queue, and pages the admin commands point the device at. */
                kumo::dma::Buffer m_admin_queue;
                kumo::dma::Buffer m_descriptor_page;
                kumo::dma::Buffer m_tx_page_list;
                kumo::dma::Buffer m_rx_page_list;
                u32 m_admin_produced;

                /* What the device told us. */
                u8 m_mac[netdev::MacAddressSize];
                u32 m_mtu;
                u32 m_device_mtu;
                u32 m_counter_count;
                u16 m_options_seen;

                /* Shared with the device. */
                kumo::dma::Buffer m_counters;         /* u32 event counters, big-endian */
                kumo::dma::Buffer m_irq_doorbells;    /* IrqDoorbell[NotifyBlockCount] */
                kumo::dma::Buffer m_queue_resources;  /* QueueResources: [0] transmit, [1] receive */

                /* Transmit. */
                kumo::dma::Buffer m_tx_ring;          /* TxPacketDescriptor[m_tx_ring_size] */
                kumo::dma::Buffer m_tx_buffers;
                u32 m_tx_ring_size;
                u32 m_tx_buffer_count;
                u32 m_tx_doorbell;
                u32 m_tx_counter;
                u32 m_tx_posted;                    /* descriptors handed to the device, free-running */
                u32 m_tx_completed;                 /* descriptors the device has finished with */
                u64 m_tx_dropped;

                /* Receive. */
                kumo::dma::Buffer m_rx_ring;          /* RxDescriptor[m_rx_ring_size] */
                kumo::dma::Buffer m_rx_data_ring;     /* u64 byte offsets into the page list, one per slot */
                kumo::dma::Buffer m_rx_buffers;
                u32 m_rx_ring_size;
                u32 m_rx_buffer_count;
                u32 m_rx_doorbell;
                u32 m_rx_next;                      /* next slot the device fills, free-running */
                u32 m_rx_filled;                    /* slots handed to the device, free-running */
                u8  m_rx_sequence;                  /* sequence number expected in the next slot */
                bool m_rx_discarding;               /* inside a frame that spans buffers */
                u64 m_rx_dropped;
            public:
                constexpr GvnicDevice()
                    : m_location(), m_revision(0), m_registers(0), m_doorbells(0), m_doorbells_size(0),
                      m_admin_queue(), m_descriptor_page(), m_tx_page_list(), m_rx_page_list(), m_admin_produced(0),
                      m_mac(), m_mtu(DefaultMtu), m_device_mtu(0), m_counter_count(0), m_options_seen(0),
                      m_counters(), m_irq_doorbells(), m_queue_resources(),
                      m_tx_ring(), m_tx_buffers(), m_tx_ring_size(0), m_tx_buffer_count(0), m_tx_doorbell(0), m_tx_counter(0), m_tx_posted(0), m_tx_completed(0), m_tx_dropped(0),
                      m_rx_ring(), m_rx_data_ring(), m_rx_buffers(), m_rx_ring_size(0), m_rx_buffer_count(0), m_rx_doorbell(0), m_rx_next(0), m_rx_filled(0), m_rx_sequence(1), m_rx_discarding(false), m_rx_dropped(0)
                { /* ... */ }

                ::Result Initialize(const kumo::pci::DeviceInfo &pci_device);
            public:
                virtual const char *GetName() const override { return "gvnic0"; }
                virtual void GetMacAddress(u8 *out) const override { std::memcpy(out, m_mac, sizeof(m_mac)); }
                virtual u32 GetMtu() const override { return m_mtu; }
                virtual bool IsLinkUp() override;
                virtual ::Result Send(const netdev::FramePiece *pieces, size_t piece_count) override;
                virtual int Poll(netdev::IFrameReceiver *receiver) override;
                virtual void Maintain() override;
            private:
                u32 ReadRegister(u32 offset) const { return Be32(*reinterpret_cast<const volatile u32 *>(m_registers + offset)); }
                void WriteRegister(u32 offset, u32 value) { *reinterpret_cast<volatile u32 *>(m_registers + offset) = Be32(value); }
                void WriteRegister16(u32 offset, u16 value) { *reinterpret_cast<volatile u16 *>(m_registers + offset) = Be16(value); }
                void WriteRegister8(u32 offset, u8 value) { *reinterpret_cast<volatile u8 *>(m_registers + offset) = value; }

                ::Result RingDoorbell(u32 index, u32 value);
                u32 ReadCounter(u32 index) const { return Be32(ReadShared(m_counters.As<u32>() + index)); }

                ::Result BringUp();
                ::Result ResetDevice();
                void ReleaseDevice();
                ::Result SetUpAdminQueue();
                ::Result ExecuteAdminCommand(abi::AdminOpcode opcode, const void *payload, size_t payload_size);
                ::Result DescribeDevice();
                ::Result ConfigureResources();
                ::Result RegisterPageList(u32 id, kumo::dma::Buffer *list, const kumo::dma::Buffer &pages);
                ::Result CreateTxQueue();
                ::Result CreateRxQueue();

                void ReclaimTransmitted();
        };

        ::Result GvnicDevice::RingDoorbell(u32 index, u32 value) {
            KUMO_UNLESS((static_cast<size_t>(index) + 1) * sizeof(u32) <= m_doorbells_size, KERNELRESULT(OutOfRange));

            /* Everything written to shared memory must reach the device before the doorbell does. */
            MemoryBarrier();
            *reinterpret_cast<volatile u32 *>(m_doorbells + index * sizeof(u32)) = Be32(value);
            KUMO_SUCCEED();
        }

        ::Result GvnicDevice::ResetDevice() {
            if (m_revision < 1) {
                /* Revision 0 is reset by taking its admin queue away. */
                if (this->ReadRegister(abi::Register_AdminQueuePfn) == 0) {
                    KUMO_SUCCEED();
                }
                this->WriteRegister(abi::Register_AdminQueuePfn, 0);
                KUMO_SUCCEED_IF(WaitFor([&] { return this->ReadRegister(abi::Register_AdminQueuePfn) == 0; }));
            } else {
                this->WriteRegister(abi::Register_DriverStatus, abi::DriverStatus_Reset);
                KUMO_SUCCEED_IF(WaitFor([&] { return (this->ReadRegister(abi::Register_DeviceStatus) & abi::DeviceStatus_IsReset) != 0; }));
            }

            Log("[bsdsockets] gvnic: the device did not reset\n");
            KUMO_THROW(KERNELRESULT(TimedOut));
        }

        void GvnicDevice::ReleaseDevice() {
            /* After a failure: stop the device using our memory. Best effort. */
            if (m_revision < 1) {
                this->WriteRegister(abi::Register_AdminQueuePfn, 0);
            } else {
                this->WriteRegister(abi::Register_DriverStatus, abi::DriverStatus_Reset);
            }
        }

        ::Result GvnicDevice::SetUpAdminQueue() {
            KUMO_TRY(AllocateOnce(std::addressof(m_admin_queue), abi::AdminQueueSize));
            m_admin_produced = 0;

            const u64 address = m_admin_queue.physical_address;
            if (m_revision < 1) {
                this->WriteRegister(abi::Register_AdminQueuePfn, static_cast<u32>(address / PageSize));
            } else {
                this->WriteRegister16(abi::Register_AdminQueueLength, abi::AdminQueueSize);
                this->WriteRegister(abi::Register_AdminQueueBaseHigh, static_cast<u32>(address >> 32));
                this->WriteRegister(abi::Register_AdminQueueBaseLow,  static_cast<u32>(address));
                this->WriteRegister(abi::Register_DriverStatus, abi::DriverStatus_Run);
            }
            KUMO_SUCCEED();
        }

        ::Result GvnicDevice::ExecuteAdminCommand(abi::AdminOpcode opcode, const void *payload, size_t payload_size) {
            KUMO_ABORT_UNLESS(sizeof(abi::AdminCommandHeader) + payload_size <= abi::AdminCommandSize);

            /* Commands run one at a time, so the slot after the last one is always free. */
            u8 *command = m_admin_queue.At((m_admin_produced % abi::AdminCommandCount) * abi::AdminCommandSize);
            std::memset(command, 0, abi::AdminCommandSize);
            const abi::AdminCommandHeader header = { Be32(opcode), Be32(abi::AdminStatus_Unset) };
            std::memcpy(command, std::addressof(header), sizeof(header));
            std::memcpy(command + sizeof(header), payload, payload_size);

            ++m_admin_produced;
            MemoryBarrier();
            this->WriteRegister(abi::Register_AdminQueueDoorbell, m_admin_produced);

            const bool finished = WaitFor([&] {
                /* The first command follows RUN immediately. Let the device finish leaving reset. */
                return (m_admin_produced > 1 && (this->ReadRegister(abi::Register_DeviceStatus) & ResetStatus) != 0) ||
                       this->ReadRegister(abi::Register_AdminQueueCounter) == m_admin_produced;
            });
            const u32 device_status = this->ReadRegister(abi::Register_DeviceStatus);
            if ((device_status & ResetStatus) != 0) {
                Log("[bsdsockets] gvnic: reset during admin command 0x%x (device status 0x%x)\n", static_cast<unsigned>(opcode), device_status);
                KUMO_THROW(KERNELRESULT(InvalidState));
            }
            if (!finished) {
                Log("[bsdsockets] gvnic: admin command 0x%x timed out after %llu s (completed %u of %u, device status 0x%x, driver status 0x%x)\n",
                    static_cast<unsigned>(opcode), static_cast<unsigned long long>(TimeoutSeconds), this->ReadRegister(abi::Register_AdminQueueCounter), m_admin_produced,
                    this->ReadRegister(abi::Register_DeviceStatus), this->ReadRegister(abi::Register_DriverStatus));
                KUMO_THROW(KERNELRESULT(TimedOut));
            }

            MemoryBarrier();
            const u32 status = Be32(ReadShared(reinterpret_cast<const u32 *>(command + offsetof(abi::AdminCommandHeader, status))));
            if (status != abi::AdminStatus_Passed) {
                Log("[bsdsockets] gvnic: admin command 0x%x failed with status 0x%x\n", static_cast<unsigned>(opcode), status);
                KUMO_THROW(KERNELRESULT(InvalidState));
            }
            KUMO_SUCCEED();
        }

        ::Result GvnicDevice::DescribeDevice() {
            m_options_seen = 0;
            KUMO_TRY(AllocateOnce(std::addressof(m_descriptor_page), PageSize));
            const kumo::dma::Buffer &page = m_descriptor_page;

            const abi::DescribeDevice command = {
                .descriptor_address = Be64(page.physical_address),
                .descriptor_version = Be32(1),
                .available_length   = Be32(PageSize),
            };
            KUMO_TRY(this->ExecuteAdminCommand(abi::AdminOpcode_DescribeDevice, std::addressof(command), sizeof(command)));

            abi::DeviceDescriptor descriptor;
            std::memcpy(std::addressof(descriptor), page.address, sizeof(descriptor));

            std::memcpy(m_mac, descriptor.mac, sizeof(m_mac));
            m_device_mtu    = Be16(descriptor.mtu);
            m_counter_count = Be16(descriptor.counters);

            /* The device may make the receive ring shorter than it says by having fewer data slots. */
            m_tx_ring_size = Be16(descriptor.tx_queue_entries);
            m_rx_ring_size = Be16(descriptor.rx_queue_entries);
            const u32 rx_slots = Be16(descriptor.rx_pages_per_qpl);
            if (rx_slots != 0 && rx_slots < m_rx_ring_size) {
                m_rx_ring_size = rx_slots;
            }

            /* Gigabit Ethernet MTU at most: lwIP's buffers and ours hold a 1518-byte frame. */
            m_mtu = (MinMtu <= m_device_mtu && m_device_mtu < DefaultMtu) ? m_device_mtu : DefaultMtu;

            /* Note which options the device offers; only the queue formats matter here. */
            const size_t total = std::min<size_t>(Be16(descriptor.total_length), PageSize);
            size_t offset = sizeof(descriptor);
            const u16 count = Be16(descriptor.num_device_options);
            for (u16 i = 0; i < count && offset + sizeof(abi::DeviceOptionHeader) <= total; ++i) {
                abi::DeviceOptionHeader option;
                std::memcpy(std::addressof(option), page.At(offset), sizeof(option));
                const u16 id = Be16(option.option_id);
                if (id < 16) {
                    m_options_seen |= static_cast<u16>(1u << id);
                }
                offset += sizeof(option) + Be16(option.option_length);
            }

            const u64 max_pages = Be64(descriptor.max_registered_pages);
            Log("[bsdsockets] gvnic: rings of %u (rx) and %u (tx), mtu %u, %u counters, up to %llu pages, options 0x%04x\n",
                m_rx_ring_size, m_tx_ring_size, m_device_mtu, m_counter_count, static_cast<unsigned long long>(max_pages), m_options_seen);

            KUMO_UNLESS(IsPowerOfTwo(m_tx_ring_size) && m_tx_ring_size <= MaxRingSize, ::kumo::KernelError(127));
            KUMO_UNLESS(IsPowerOfTwo(m_rx_ring_size) && m_rx_ring_size <= MaxRingSize, ::kumo::KernelError(127));
            KUMO_UNLESS(m_counter_count != 0, ::kumo::KernelError(127));

            m_tx_buffer_count = std::min(m_tx_ring_size, MaxBuffers);
            m_rx_buffer_count = std::min(m_rx_ring_size, MaxBuffers);
            const u32 tx_pages_per_qpl = Be16(descriptor.tx_pages_per_qpl);
            if (tx_pages_per_qpl != 0 && tx_pages_per_qpl * 2 < m_tx_buffer_count) {
                /* Keep the count a power of two, so that a slot's buffer is `slot % count`. */
                while (tx_pages_per_qpl * 2 < m_tx_buffer_count) {
                    m_tx_buffer_count /= 2;
                }
            }
            KUMO_UNLESS(m_tx_buffer_count >= 2 && m_rx_buffer_count >= 2, ::kumo::KernelError(127));
            KUMO_UNLESS((m_tx_buffer_count + m_rx_buffer_count) / 2 <= max_pages, ::kumo::KernelError(127));

            if ((m_options_seen & ((1u << abi::DeviceOption_DqoRda) | (1u << abi::DeviceOption_DqoQpl))) != 0 && (m_options_seen & (1u << abi::DeviceOption_GqiQpl)) == 0) {
                /* Linux falls back to GQI-QPL whenever nothing else is chosen, so try it, but say why if it fails. */
                Log("[bsdsockets] gvnic: the device offers DQO and does not list GQI-QPL, the only format this driver speaks\n");
            }
            KUMO_SUCCEED();
        }

        ::Result GvnicDevice::ConfigureResources() {
            KUMO_TRY(AllocateOnce(std::addressof(m_counters), m_counter_count * sizeof(u32)));
            KUMO_TRY(AllocateOnce(std::addressof(m_irq_doorbells), NotifyBlockCount * sizeof(abi::IrqDoorbell)));

            Log("[bsdsockets] gvnic: configuring: device status 0x%x, admin queue 0x%llx, counters 0x%llx, irq doorbells 0x%llx\n",
                this->ReadRegister(abi::Register_DeviceStatus), static_cast<unsigned long long>(m_admin_queue.physical_address),
                static_cast<unsigned long long>(m_counters.physical_address), static_cast<unsigned long long>(m_irq_doorbells.physical_address));

            const abi::ConfigureDeviceResources command = {
                .counter_array          = Be64(m_counters.physical_address),
                .irq_doorbell_array     = Be64(m_irq_doorbells.physical_address),
                .num_counters           = Be32(m_counter_count),
                .num_irq_doorbells      = Be32(NotifyBlockCount),
                .irq_doorbell_stride    = Be32(sizeof(abi::IrqDoorbell)),
                .notify_block_msix_base = Be32(0),
                .queue_format           = abi::QueueFormat_GqiQpl,
                .padding                = {},
            };
            KUMO_TRY(this->ExecuteAdminCommand(abi::AdminOpcode_ConfigureDeviceResources, std::addressof(command), sizeof(command)));

            /* MSI-X is never enabled, so nothing could arrive anyway, but keep the device from trying. */
            MemoryBarrier();
            for (u32 i = 0; i < NotifyBlockCount; ++i) {
                const u32 index = Be32(ReadShared(std::addressof(m_irq_doorbells.As<abi::IrqDoorbell>()[i].index)));
                KUMO_TRY(this->RingDoorbell(index, abi::IrqDoorbell_Mask));
            }
            KUMO_SUCCEED();
        }

        ::Result GvnicDevice::RegisterPageList(u32 id, kumo::dma::Buffer *list_buffer, const kumo::dma::Buffer &pages) {
            const u32 page_count = pages.size / PageSize;

            KUMO_TRY(AllocateOnce(list_buffer, page_count * sizeof(u64), alignof(u64)));
            const kumo::dma::Buffer &list = *list_buffer;
            for (u32 i = 0; i < page_count; ++i) {
                list.As<u64>()[i] = Be64(pages.PhysicalAt(i * PageSize));
            }

            const abi::RegisterPageList command = {
                .page_list_id      = Be32(id),
                .num_pages         = Be32(page_count),
                .page_address_list = Be64(list.physical_address),
                .page_size         = Be64(PageSize),
            };
            KUMO_RETURN(this->ExecuteAdminCommand(abi::AdminOpcode_RegisterPageList, std::addressof(command), sizeof(command)));
        }

        ::Result GvnicDevice::CreateTxQueue() {
            const abi::CreateTxQueue command = {
                .queue_id             = Be32(0),
                .reserved             = 0,
                .queue_resources      = Be64(m_queue_resources.PhysicalAt(0 * sizeof(abi::QueueResources))),
                .ring                 = Be64(m_tx_ring.physical_address),
                .page_list_id         = Be32(TxQueuePageListId),
                .notify_id            = Be32(TxNotifyBlock),
                .completion_ring      = 0,
                .ring_size            = Be16(m_tx_ring_size),
                .completion_ring_size = 0,
                .padding              = {},
            };
            KUMO_TRY(this->ExecuteAdminCommand(abi::AdminOpcode_CreateTxQueue, std::addressof(command), sizeof(command)));

            const auto *resources = m_queue_resources.As<abi::QueueResources>() + 0;
            m_tx_doorbell = Be32(ReadShared(std::addressof(resources->doorbell_index)));
            m_tx_counter  = Be32(ReadShared(std::addressof(resources->counter_index)));
            KUMO_UNLESS(m_tx_counter < m_counter_count, KERNELRESULT(OutOfRange));
            KUMO_UNLESS((static_cast<size_t>(m_tx_doorbell) + 1) * sizeof(u32) <= m_doorbells_size, KERNELRESULT(OutOfRange));

            /* Both counts start from zero with the queue: the device's counter is zero until it completes one. */
            m_tx_posted    = 0;
            m_tx_completed = 0;
            KUMO_SUCCEED();
        }

        ::Result GvnicDevice::CreateRxQueue() {
            /* Slot `i` receives into buffer `i % count`, at that buffer's offset in the page list. */
            for (u32 i = 0; i < m_rx_ring_size; ++i) {
                m_rx_data_ring.As<u64>()[i] = Be64(static_cast<u64>(i % m_rx_buffer_count) * BufferSize);
            }

            const abi::CreateRxQueue command = {
                .queue_id           = Be32(0),
                .index              = Be32(0),
                .reserved           = 0,
                .notify_id          = Be32(RxNotifyBlock),
                .queue_resources    = Be64(m_queue_resources.PhysicalAt(1 * sizeof(abi::QueueResources))),
                .descriptor_ring    = Be64(m_rx_ring.physical_address),
                .data_ring          = Be64(m_rx_data_ring.physical_address),
                .page_list_id       = Be32(RxQueuePageListId),
                .ring_size          = Be16(m_rx_ring_size),
                .packet_buffer_size = Be16(BufferSize),
                .buffer_ring_size   = 0,
                .enable_rsc         = 0,
                .padding1           = 0,
                .header_buffer_size = 0,
                .padding2           = {},
            };
            KUMO_TRY(this->ExecuteAdminCommand(abi::AdminOpcode_CreateRxQueue, std::addressof(command), sizeof(command)));

            const auto *resources = m_queue_resources.As<abi::QueueResources>() + 1;
            m_rx_doorbell = Be32(ReadShared(std::addressof(resources->doorbell_index)));
            KUMO_UNLESS((static_cast<size_t>(m_rx_doorbell) + 1) * sizeof(u32) <= m_doorbells_size, KERNELRESULT(OutOfRange));

            /* Hand the device one slot per buffer. */
            m_rx_next       = 0;
            m_rx_sequence   = 1;
            m_rx_discarding = false;
            m_rx_filled   = m_rx_buffer_count;
            KUMO_RETURN(this->RingDoorbell(m_rx_doorbell, m_rx_filled));
        }

        ::Result GvnicDevice::Initialize(const kumo::pci::DeviceInfo &pci_device) {
            m_location = pci_device.location;
            m_revision = pci_device.revision;

            size_t registers_size;
            KUMO_TRY(kumo::pci::MapBar(std::addressof(m_registers), std::addressof(registers_size), m_location, abi::RegisterBar));
            KUMO_UNLESS(registers_size >= abi::Register_AdminQueueLength + sizeof(u16), KERNELRESULT(InvalidSize));
            KUMO_TRY(kumo::pci::MapBar(std::addressof(m_doorbells), std::addressof(m_doorbells_size), m_location, abi::DoorbellBar));
            kumo::pci::EnableMemoryAndBusMaster(m_location);

            /* The device sets up an interrupt per notification block, and Linux always enables MSI-X */
            /* before asking it to. Enable it with the whole function masked: the device may then */
            /* record interrupts as pending, but never sends one. */
            if (const u32 msix = kumo::pci::FindCapability(m_location, kumo::pci::CapabilityId_MsiX); msix != 0) {
                constexpr u16 MsixControl_FunctionMask = (1u << 14);
                constexpr u16 MsixControl_Enable       = (1u << 15);
                const u16 control = kumo::pci::ReadConfig16(m_location, msix + 2);
                kumo::pci::WriteConfig16(m_location, msix + 2, control | MsixControl_FunctionMask);
                kumo::pci::WriteConfig16(m_location, msix + 2, control | MsixControl_FunctionMask | MsixControl_Enable);
                Log("[bsdsockets] gvnic: MSI-X enabled and masked, %u vectors\n", (control & 0x7FF) + 1u);
            }

            /* Allow transient device resets during setup to settle before rebuilding the queues. */
            constexpr int SetupAttempts = 3;
            ::Result rc = 0;
            for (int attempt = 1; attempt <= SetupAttempts; ++attempt) {
                rc = this->BringUp();
                if (::kumo::IsSuccess(rc)) {
                    break;
                }
                Log("[bsdsockets] gvnic: setup attempt %d of %d failed (rc=0x%x, device status 0x%x)\n", attempt, SetupAttempts, rc, this->ReadRegister(abi::Register_DeviceStatus));
                if (attempt < SetupAttempts) {
                    ::svcSleepThread(RecoveryDelayNs);
                }
            }
            KUMO_TRY(rc);
            m_ready.store(true, std::memory_order_release);

            Log("[bsdsockets] gvnic: %02x:%02x.%x revision %u, queue format GQI-QPL, %u (rx) and %u (tx) buffers, link %s\n",
                m_location.bus, m_location.device, m_location.function, m_revision, m_rx_buffer_count, m_tx_buffer_count, this->IsLinkUp() ? "up" : "down");
            KUMO_SUCCEED();
        }

        ::Result GvnicDevice::BringUp() {
            KUMO_TRY(this->ResetDevice());

            for (const char *c = DriverVersion; *c != '\x00'; ++c) {
                this->WriteRegister8(abi::Register_DriverVersion, static_cast<u8>(*c));
            }

            KUMO_TRY(this->SetUpAdminQueue());
            auto release_guard = ::kumo::ScopeExit([&] { this->ReleaseDevice(); });

            KUMO_TRY(this->DescribeDevice());
            KUMO_TRY(this->ConfigureResources());

            /* Rings, frame buffers, and the page through which the device reports each queue's doorbell. */
            KUMO_TRY(AllocateOnce(std::addressof(m_queue_resources), 2 * sizeof(abi::QueueResources)));
            KUMO_TRY(AllocateOnce(std::addressof(m_tx_ring),      m_tx_ring_size * sizeof(abi::TxPacketDescriptor)));
            KUMO_TRY(AllocateOnce(std::addressof(m_rx_ring),      m_rx_ring_size * sizeof(abi::RxDescriptor)));
            KUMO_TRY(AllocateOnce(std::addressof(m_rx_data_ring), m_rx_ring_size * sizeof(u64)));
            KUMO_TRY(AllocateOnce(std::addressof(m_tx_buffers),   m_tx_buffer_count * BufferSize));
            KUMO_TRY(AllocateOnce(std::addressof(m_rx_buffers),   m_rx_buffer_count * BufferSize));

            KUMO_TRY(this->RegisterPageList(TxQueuePageListId, std::addressof(m_tx_page_list), m_tx_buffers));
            KUMO_TRY(this->RegisterPageList(RxQueuePageListId, std::addressof(m_rx_page_list), m_rx_buffers));
            KUMO_TRY(this->CreateTxQueue());
            KUMO_TRY(this->CreateRxQueue());
            KUMO_UNLESS((this->ReadRegister(abi::Register_DeviceStatus) & ResetStatus) == 0, KERNELRESULT(InvalidState));

            release_guard.Cancel();
            KUMO_SUCCEED();
        }

        bool GvnicDevice::IsLinkUp() {
            if (!m_ready.load(std::memory_order_acquire)) {
                return false;
            }
            const u32 status = this->ReadRegister(abi::Register_DeviceStatus);
            return (status & (ResetStatus | abi::DeviceStatus_LinkUp)) == abi::DeviceStatus_LinkUp;
        }

        void GvnicDevice::Maintain() {
            if (m_ready.load(std::memory_order_acquire) ||
                ::armGetSystemTick() - m_recovery_started < ::armGetSystemTickFreq()) {
                return;
            }

            /* Poll has stopped frame I/O, so setup can sleep without holding the stack lock. */
            const ::Result rc = this->BringUp();
            if (::kumo::IsSuccess(rc)) {
                Log("[bsdsockets] gvnic: recovered from device reset\n");
                m_ready.store(true, std::memory_order_release);
            } else {
                Log("[bsdsockets] gvnic: reset recovery failed (rc=0x%x, device status 0x%x)\n", rc, this->ReadRegister(abi::Register_DeviceStatus));
                m_recovery_started = ::armGetSystemTick();
            }
        }

        void GvnicDevice::ReclaimTransmitted() {
            /* The counter is how many descriptors the device has finished; one per frame here. */
            const u32 completed = this->ReadCounter(m_tx_counter);
            if (static_cast<u32>(completed - m_tx_completed) <= static_cast<u32>(m_tx_posted - m_tx_completed)) {
                m_tx_completed = completed;
            }
        }

        ::Result GvnicDevice::Send(const netdev::FramePiece *pieces, size_t piece_count) {
            KUMO_UNLESS(this->IsLinkUp(), KERNELRESULT(InvalidState));
            const size_t size = netdev::GetFrameSize(pieces, piece_count);
            KUMO_UNLESS(0 < size && size <= netdev::MaxFrameSize, KERNELRESULT(InvalidSize));

            if (m_tx_posted - m_tx_completed >= m_tx_buffer_count) {
                this->ReclaimTransmitted();
            }
            if (m_tx_posted - m_tx_completed >= m_tx_buffer_count) {
                ++m_tx_dropped;
                KUMO_THROW(KERNELRESULT(ResourceExhausted));
            }

            /* Copy the frame into the buffer that belongs to this slot, padded to the Ethernet minimum. */
            constexpr size_t MinFrameSize = 60;
            const u32 buffer = m_tx_posted % m_tx_buffer_count;
            u8 *data = m_tx_buffers.At(buffer * BufferSize);
            netdev::CopyFrame(data, pieces, piece_count);
            const size_t length = std::max(size, MinFrameSize);
            if (size < length) {
                std::memset(data + size, 0, length - size);
            }

            auto *descriptor = m_tx_ring.As<abi::TxPacketDescriptor>() + (m_tx_posted & (m_tx_ring_size - 1));
            *descriptor = {
                .type_flags         = 0,
                .l4_checksum_offset = 0,
                .l4_header_offset   = 0,
                .descriptor_count   = 1,
                .length             = Be16(static_cast<u16>(length)),
                .segment_length     = Be16(static_cast<u16>(length)),
                .segment_address    = Be64(static_cast<u64>(buffer) * BufferSize),
            };

            ++m_tx_posted;
            KUMO_RETURN(this->RingDoorbell(m_tx_doorbell, m_tx_posted));
        }

        int GvnicDevice::Poll(netdev::IFrameReceiver *receiver) {
            if (!m_ready.load(std::memory_order_acquire)) {
                return 0;
            }
            if (const u32 status = this->ReadRegister(abi::Register_DeviceStatus); (status & ResetStatus) != 0) {
                m_ready.store(false, std::memory_order_release);
                m_recovery_started = ::armGetSystemTick();
                Log("[bsdsockets] gvnic: device reset detected (device status 0x%x)\n", status);
                return 0;
            }
            this->ReclaimTransmitted();

            int delivered = 0;
            u32 consumed  = 0;
            const auto *ring = m_rx_ring.As<abi::RxDescriptor>();
            while (true) {
                const abi::RxDescriptor *descriptor = ring + (m_rx_next & (m_rx_ring_size - 1));

                /* The device writes the sequence number last; a slot is ready when it is the one expected. */
                const u16 flags = Be16(ReadShared(std::addressof(descriptor->flags_sequence)));
                if ((flags & abi::RxSequenceMask) != m_rx_sequence) {
                    break;
                }
                MemoryBarrier();

                const size_t length = Be16(ReadShared(std::addressof(descriptor->length)));
                const bool continues = (flags & abi::RxFlag_PacketContinues) != 0;

                /* A frame that spans buffers is dropped whole: every piece until the one that ends it. */
                const bool usable = !m_rx_discarding && !continues && (flags & abi::RxFlag_Error) == 0 && abi::RxPad < length && length <= BufferSize;
                if (usable) {
                    const u32 buffer = m_rx_next % m_rx_buffer_count;
                    receiver->OnFrameReceived(m_rx_buffers.At(buffer * BufferSize + abi::RxPad), length - abi::RxPad);
                    ++delivered;
                } else {
                    ++m_rx_dropped;
                }
                m_rx_discarding = continues;

                ++m_rx_next;
                m_rx_sequence = abi::NextRxSequence(m_rx_sequence);
                ++consumed;
            }

            /* The buffers just read are free again: give their slots, one ring's worth ahead, back. */
            if (consumed > 0) {
                m_rx_filled += consumed;
                KUMO_ABORT_RESULT(this->RingDoorbell(m_rx_doorbell, m_rx_filled));
            }
            return delivered;
        }

        constinit GvnicDevice g_device;

    }

    ::Result Probe() {
        kumo::pci::DeviceInfo pci_device;
        if (!kumo::pci::FindDeviceById(std::addressof(pci_device), abi::PciVendorId, abi::PciDeviceId, 0)) {
            KUMO_SUCCEED();
        }

        KUMO_TRY(g_device.Initialize(pci_device));
        KUMO_RETURN(netdev::RegisterNetDevice(std::addressof(g_device)));
    }

}

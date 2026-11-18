/*
 * virtio-net driver, over the modern virtio-pci transport (hz/virtio.hpp).
 * Covers QEMU's virtio-net-pci, both transitional (the default) and
 * modern-only, and any other hypervisor's virtio 1.0 network device.
 *
 * Deliberately minimal:
 *   - one device, one receive and one transmit queue
 *   - no offloads: no checksum offload, segmentation offload or merged
 *     receive buffers, so every frame is one buffer with a 12-byte header
 *   - polled; no interrupts
 *   - every frame is copied between the caller's memory and a buffer of our
 *     own, whose physical address the device is given
 *
 * Queues and buffers come from the drivers' shared DMA arena.
 */
#include <kumo/prelude.hpp>
#include <kumo/scope_exit.hpp>
#include <kumo/dma.hpp>
#include <kumo/pci.hpp>
#include <kumo/virtio.hpp>
#include "../bsd_drivers.hpp"
#include "../../bsd_log.hpp"
#include "../../netdev/bsd_net_device_registry.hpp"

namespace kumo::horizon::hznet::drivers::virtio_net {

    namespace {

        using kumo::virtio::PageSize;

        /* Feature bits. */
        constexpr u64 Feature_Mac    = (UINT64_C(1) << 5);
        constexpr u64 Feature_Status = (UINT64_C(1) << 16);
        constexpr u64 Feature_Mtu    = (UINT64_C(1) << 3);

        /* struct virtio_net_config. */
        constexpr u32 Config_Mac    = 0;
        constexpr u32 Config_Status = 6;
        constexpr u32 Config_Mtu    = 10;

        constexpr u16 NetStatus_LinkUp = 1;

        constexpr u16 Queue_Receive  = 0;
        constexpr u16 Queue_Transmit = 1;

        /* struct virtio_net_hdr_v1: precedes every frame in both directions. All zero means "no offload". */
        constexpr size_t HeaderSize = 12;

        constexpr u16 QueueSize = kumo::virtio::Queue::MaxSize;

        /* Two buffers per page, so a buffer never crosses a page and needs one physical address. */
        constexpr size_t BufferSize = PageSize / 2;
        static_assert(HeaderSize + netdev::MaxFrameSize <= BufferSize);

        constexpr u32 DefaultMtu = 1500;

        class VirtioNetDevice final : public netdev::INetDevice {
            private:
                kumo::virtio::Transport m_transport;
                kumo::virtio::Queue m_rx;
                kumo::virtio::Queue m_tx;
                u64 m_features;
                u8 m_mac[netdev::MacAddressSize];
                u32 m_mtu;
                u16 m_tx_free[QueueSize];
                u16 m_tx_free_count;
                u64 m_tx_dropped;
                kumo::dma::Buffer m_rx_buffers;
                kumo::dma::Buffer m_tx_buffers;
            public:
                constexpr VirtioNetDevice() : m_transport(), m_rx(), m_tx(), m_features(0), m_mac(), m_mtu(DefaultMtu), m_tx_free(), m_tx_free_count(0), m_tx_dropped(0), m_rx_buffers(), m_tx_buffers() { /* ... */ }

                ::Result Initialize(const kumo::pci::DeviceInfo &pci_device);
            public:
                virtual const char *GetName() const override { return "virtio-net0"; }
                virtual void GetMacAddress(u8 *out) const override { std::memcpy(out, m_mac, sizeof(m_mac)); }
                virtual u32 GetMtu() const override { return m_mtu; }
                virtual bool IsLinkUp() override;
                virtual ::Result Send(const netdev::FramePiece *pieces, size_t piece_count) override;
                virtual int Poll(netdev::IFrameReceiver *receiver) override;
            private:
                void ReclaimTransmitted();
        };

        ::Result VirtioNetDevice::Initialize(const kumo::pci::DeviceInfo &pci_device) {
            KUMO_TRY(m_transport.Initialize(pci_device));

            /* The initialization sequence of the virtio specification, section 3.1.1. */
            KUMO_TRY(m_transport.Reset());
            m_transport.AddStatus(kumo::virtio::Status_Acknowledge);
            m_transport.AddStatus(kumo::virtio::Status_Driver);

            auto fail_guard = ::kumo::ScopeExit([&] { m_transport.AddStatus(kumo::virtio::Status_Failed); });

            KUMO_TRY(m_transport.NegotiateFeatures(std::addressof(m_features), Feature_Mac | Feature_Status | Feature_Mtu, kumo::virtio::Feature_Version1));

            /* The address the hypervisor assigned. Without one, make up a locally administered address. */
            if ((m_features & Feature_Mac) != 0) {
                for (size_t i = 0; i < sizeof(m_mac); ++i) {
                    m_mac[i] = m_transport.ReadDeviceConfig8(Config_Mac + i);
                }
            } else {
                ::randomGet(m_mac, sizeof(m_mac));
                m_mac[0] = (m_mac[0] & 0xFC) | 0x02;
            }

            if ((m_features & Feature_Mtu) != 0) {
                const u32 mtu = m_transport.ReadDeviceConfig16(Config_Mtu);
                if (68 <= mtu && mtu < DefaultMtu) {
                    m_mtu = mtu;
                }
            }

            /* Memory the device reads and writes directly. */
            auto &arena = GetDmaArena();
            kumo::dma::Buffer rx_queue_memory, tx_queue_memory;
            KUMO_TRY(arena.Allocate(std::addressof(rx_queue_memory), 3 * PageSize));
            KUMO_TRY(arena.Allocate(std::addressof(tx_queue_memory), 3 * PageSize));

            KUMO_TRY(m_rx.Initialize(std::addressof(m_transport), Queue_Receive,  QueueSize, rx_queue_memory.address));
            KUMO_TRY(m_tx.Initialize(std::addressof(m_transport), Queue_Transmit, QueueSize, tx_queue_memory.address));

            KUMO_TRY(arena.Allocate(std::addressof(m_rx_buffers), m_rx.GetSize() * BufferSize));
            KUMO_TRY(arena.Allocate(std::addressof(m_tx_buffers), m_tx.GetSize() * BufferSize));

            /* Give the device every receive buffer. */
            for (u16 i = 0; i < m_rx.GetSize(); ++i) {
                m_rx.SetBuffer(i, m_rx_buffers.PhysicalAt(i * BufferSize), BufferSize, true);
                m_rx.Submit(i);
            }

            /* Transmit buffers start out free. */
            for (u16 i = 0; i < m_tx.GetSize(); ++i) {
                m_tx.SetBuffer(i, m_tx_buffers.PhysicalAt(i * BufferSize), BufferSize, false);
                m_tx_free[m_tx_free_count++] = i;
            }

            fail_guard.Cancel();
            m_transport.AddStatus(kumo::virtio::Status_DriverOk);
            m_rx.Kick();

            Log("[bsdsockets] virtio-net: %02x:%02x.%x, features 0x%llx, queues of %u (rx) and %u (tx)\n", pci_device.location.bus, pci_device.location.device, pci_device.location.function, static_cast<unsigned long long>(m_features), m_rx.GetSize(), m_tx.GetSize());
            KUMO_SUCCEED();
        }

        bool VirtioNetDevice::IsLinkUp() {
            if ((m_features & Feature_Status) == 0) {
                return true;
            }
            return (m_transport.ReadDeviceConfig16(Config_Status) & NetStatus_LinkUp) != 0;
        }

        void VirtioNetDevice::ReclaimTransmitted() {
            u16 id;
            u32 length;
            while (m_tx.PopUsed(std::addressof(id), std::addressof(length))) {
                KUMO_ABORT_UNLESS(id < m_tx.GetSize() && m_tx_free_count < m_tx.GetSize());
                m_tx_free[m_tx_free_count++] = id;
            }
        }

        ::Result VirtioNetDevice::Send(const netdev::FramePiece *pieces, size_t piece_count) {
            const size_t size = netdev::GetFrameSize(pieces, piece_count);
            KUMO_UNLESS(0 < size && size <= netdev::MaxFrameSize, KERNELRESULT(InvalidSize));

            if (m_tx_free_count == 0) {
                this->ReclaimTransmitted();
            }
            if (m_tx_free_count == 0) {
                ++m_tx_dropped;
                KUMO_THROW(KERNELRESULT(ResourceExhausted));
            }

            const u16 id = m_tx_free[--m_tx_free_count];
            u8 *buffer = m_tx_buffers.At(id * BufferSize);

            std::memset(buffer, 0, HeaderSize);
            netdev::CopyFrame(buffer + HeaderSize, pieces, piece_count);

            m_tx.SetBufferLength(id, static_cast<u32>(HeaderSize + size));
            m_tx.Submit(id);
            m_tx.Kick();
            KUMO_SUCCEED();
        }

        int VirtioNetDevice::Poll(netdev::IFrameReceiver *receiver) {
            this->ReclaimTransmitted();

            int delivered = 0;
            u16 id;
            u32 length;
            while (m_rx.PopUsed(std::addressof(id), std::addressof(length))) {
                KUMO_ABORT_UNLESS(id < m_rx.GetSize());

                /* A frame shorter than the header, or longer than the buffer, is a device error; drop it. */
                if (HeaderSize < length && length <= BufferSize) {
                    receiver->OnFrameReceived(m_rx_buffers.At(id * BufferSize + HeaderSize), length - HeaderSize);
                    ++delivered;
                }

                /* The buffer goes straight back to the device. */
                m_rx.Submit(id);
            }
            if (delivered > 0) {
                m_rx.Kick();
            }
            return delivered;
        }

        constinit VirtioNetDevice g_device;

    }

    ::Result Probe() {
        kumo::pci::DeviceInfo pci_device;
        if (!kumo::virtio::FindDevice(std::addressof(pci_device), kumo::virtio::DeviceType_Net, 0)) {
            KUMO_SUCCEED();
        }

        const ::Result rc = g_device.Initialize(pci_device);
        if (::kumo::IsResult(rc, KERNELRESULT(NotFound))) {
            Log("[bsdsockets] virtio-net: %02x:%02x.%x has no virtio 1.0 interface (legacy-only devices are not supported)\n", pci_device.location.bus, pci_device.location.device, pci_device.location.function);
        }
        KUMO_TRY(rc);

        KUMO_RETURN(netdev::RegisterNetDevice(std::addressof(g_device)));
    }

}

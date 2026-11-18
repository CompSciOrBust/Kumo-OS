#include <kumo/prelude.hpp>
#include <kumo/pci.hpp>
#include <kumo/platform.hpp>
#include "bsd_drivers.hpp"
#include "../bsd_log.hpp"
#include "../netdev/bsd_net_device_registry.hpp"

namespace kumo::horizon::hznet::drivers {

    namespace {

        struct Driver {
            const char *name;
            ::Result (*probe)();
        };

        /* Probe order decides device numbering; the first device found is the primary interface. */
        constexpr const Driver Drivers[] = {
            { "virtio-net", virtio_net::Probe },
            { "gvnic",      gvnic::Probe      },
        };

        /* Shared by every driver, since in practice a machine has one kind of NIC. Sized for the */
        /* largest: virtio-net needs about 1 MiB, gVNIC about 1.2 MiB with 1024-entry rings. */
        constexpr size_t DmaArenaSize = (2 * 1024 * 1024);

        alignas(kumo::dma::PageSize) constinit u8 g_dma_memory[DmaArenaSize] = {};
        constinit kumo::dma::Arena g_dma_arena;

    }

    kumo::dma::Arena &GetDmaArena() {
        return g_dma_arena;
    }

    void ProbeAll() {
        g_dma_arena.Initialize(g_dma_memory, sizeof(g_dma_memory));

        kumo::platform::PciHostInfo host;
        if (!kumo::platform::GetPciHostInfo(std::addressof(host))) {
            Log("[bsdsockets] no PCI host available\n");
            return;
        }
        if (const ::Result rc = kumo::pci::Initialize(); ::kumo::IsFailure(rc)) {
            Log("[bsdsockets] PCI host at 0x%llx could not be mapped (rc=0x%x)\n", static_cast<unsigned long long>(host.ecam_physical_address), rc);
            return;
        }

        for (const auto &driver : Drivers) {
            /* One failing driver must not stop the others from binding. */
            const ::Result rc = driver.probe();
            if (::kumo::IsFailure(rc)) {
                Log("[bsdsockets] driver %s: probe failed (rc=0x%x)\n", driver.name, rc);
            }
        }

        const int count = netdev::GetNetDeviceCount();
        for (int i = 0; i < count; ++i) {
            auto *device = netdev::GetNetDevice(i);
            u8 mac[netdev::MacAddressSize];
            device->GetMacAddress(mac);
            Log("[bsdsockets] net device %d: %s, %02x:%02x:%02x:%02x:%02x:%02x, mtu %u\n", i, device->GetName(), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], device->GetMtu());
        }
        if (count == 0) {
            Log("[bsdsockets] no network device found; only the loopback interface exists\n");
        }
        Log("[bsdsockets] DMA memory: %zu of %zu KiB used\n", g_dma_arena.GetUsedSize() / (1 * 1024), g_dma_arena.GetSize() / (1 * 1024));
    }

}

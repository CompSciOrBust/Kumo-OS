#include "fs_filesystem.hpp"
#include <kumo/pci.hpp>
#include <kumo/platform.hpp>
#include "fs_drivers.hpp"
#include "../fs_log.hpp"
#include "../blockdev/fs_block_device_registry.hpp"

namespace kumo::horizon::hzfs::drivers {

    namespace {

        struct Driver {
            const char *name;
            Result (*probe)();
        };

        /* Probe order decides device numbering; the first device found is the system storage. */
        constexpr const Driver Drivers[] = {
            { "nvme", nvme::Probe },
            /* The RAM disk stays last, so it is the system storage only when no real disk exists. */
            { "ram",  ram::Probe },
        };

    }

    void ProbeAll() {
        /* Report what is on the PCI bus, if this machine has one. */
        kumo::platform::PciHostInfo host;
        if (!kumo::platform::GetPciHostInfo(std::addressof(host))) {
            Log("[fs] no PCI host available\n");
        } else if (const Result rc = kumo::pci::Initialize(); R_FAILED(rc)) {
            Log("[fs] PCI host at 0x%llx could not be mapped (rc=0x%x)\n", static_cast<unsigned long long>(host.ecam_physical_address), rc);
        } else {
            constexpr int MaxListed = 32;
            static kumo::pci::DeviceInfo devices[MaxListed];
            const int total = kumo::pci::EnumerateDevices(devices, MaxListed);
            for (int i = 0; i < std::min(total, MaxListed); ++i) {
                const auto &d = devices[i];
                Log("[fs] pci %02x:%02x.%x: %04x:%04x class %02x:%02x:%02x\n", d.location.bus, d.location.device, d.location.function, d.vendor_id, d.device_id, d.base_class, d.sub_class, d.prog_if);
            }
        }

        for (const auto &driver : Drivers) {
            /* One failing driver must not stop the others from binding. */
            const Result rc = driver.probe();
            if (R_FAILED(rc)) {
                Log("[fs] driver %s: probe failed (rc=0x%x)\n", driver.name, rc);
            }
        }

        const int count = blockdev::GetBlockDeviceCount();
        for (int i = 0; i < count; ++i) {
            auto *device = blockdev::GetBlockDeviceStorage(i)->GetDevice();
            Log("[fs] block device %d: %s, %llu blocks of %u bytes\n", i, device->GetName(), static_cast<unsigned long long>(device->GetBlockCount()), device->GetBlockSize());
        }
    }

}

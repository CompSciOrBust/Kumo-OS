/*
 * RAM-backed block device. It exists so the IPC and filesystem layers can be
 * developed and tested before any hardware driver works; its contents are
 * lost on reboot.
 */
#include "fs_filesystem.hpp"
#include "../fs_drivers.hpp"
#include "../../blockdev/fs_block_device_registry.hpp"

namespace kumo::horizon::hzfs::drivers::ram {

    namespace {

        constexpr u32    BlockSize = 512;
        constexpr size_t DiskSize  = (16 * 1024 * 1024);

        /* Marker at the start of the disk so a client can tell it is reading this device. */
        constexpr const char Signature[] = "HZFS-RAMDISK";

        alignas(0x1000) constinit u8 g_disk[DiskSize] = {};

        class RamBlockDevice final : public blockdev::IBlockDevice {
            public:
                virtual const char *GetName() const override { return "ram0"; }
                virtual u32 GetBlockSize() const override { return BlockSize; }
                virtual u64 GetBlockCount() const override { return DiskSize / BlockSize; }

                virtual Result Read(u64 lba, void *buffer, u32 block_count) override {
                    KUMO_UNLESS(lba <= GetBlockCount() && block_count <= GetBlockCount() - lba, fs::ResultOutOfRange());
                    std::memcpy(buffer, g_disk + lba * BlockSize, static_cast<size_t>(block_count) * BlockSize);
                    KUMO_SUCCEED();
                }

                virtual Result Write(u64 lba, const void *buffer, u32 block_count) override {
                    KUMO_UNLESS(lba <= GetBlockCount() && block_count <= GetBlockCount() - lba, fs::ResultOutOfRange());
                    std::memcpy(g_disk + lba * BlockSize, buffer, static_cast<size_t>(block_count) * BlockSize);
                    KUMO_SUCCEED();
                }

                virtual Result Flush() override { KUMO_SUCCEED(); }
        };

        constinit std::optional<RamBlockDevice> g_device = {};

    }

    Result Probe() {
        std::memcpy(g_disk, Signature, sizeof(Signature));

        g_device.emplace();
        blockdev::RegisterBlockDevice(&*g_device);
        KUMO_SUCCEED();
    }

}

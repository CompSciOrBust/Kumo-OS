/*
 * Block-device interface: the boundary between the hardware-independent part
 * of FS and the storage drivers. Nothing above this interface may know which
 * controller (RAM, virtio-blk, NVMe, virtio-scsi, ...) backs a device.
 */
#pragma once
#include "fs_filesystem.hpp"

namespace kumo::horizon::hzfs::blockdev {

    class IBlockDevice {
        public:
            virtual ~IBlockDevice() { /* ... */ }

            /* Short human-readable name, e.g. "ram0" or "nvme0n1". */
            virtual const char *GetName() const = 0;

            /* Logical block size in bytes. Must be a power of two, at most MaxBlockSize. */
            virtual u32 GetBlockSize() const = 0;

            /* Number of logical blocks on the device. */
            virtual u64 GetBlockCount() const = 0;

            /* Transfer whole blocks. The buffer holds block_count * GetBlockSize() bytes. */
            /* The buffer has no alignment guarantee; drivers that need DMA-safe memory must bounce. */
            virtual Result Read(u64 lba, void *buffer, u32 block_count) = 0;
            virtual Result Write(u64 lba, const void *buffer, u32 block_count) = 0;

            /* Commit any volatile write cache to stable storage. */
            virtual Result Flush() = 0;
    };

    constexpr inline u32 MaxBlockSize = 4096;

}

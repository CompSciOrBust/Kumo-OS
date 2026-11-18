/*
 * Adapts a block device to the byte-addressed fs::IStorage that the rest of
 * FS (partition tables, FAT, RomFS, save data) is written against.
 */
#pragma once
#include "fs_filesystem.hpp"
#include "fs_i_block_device.hpp"

namespace kumo::horizon::hzfs::blockdev {

    class BlockDeviceStorage : public fs::IStorage {
        KUMO_NON_COPYABLE(BlockDeviceStorage);
        KUMO_NON_MOVEABLE(BlockDeviceStorage);
        private:
            IBlockDevice *m_device;
            u32 m_block_size;
            u64 m_size;
            kumo::Mutex m_mutex;
            u8 m_bounce[MaxBlockSize];
        public:
            explicit BlockDeviceStorage(IBlockDevice *device);

            IBlockDevice *GetDevice() const { return m_device; }
        public:
            virtual Result Read(u64 offset, std::span<std::byte> output) override;
            virtual Result Write(u64 offset, std::span<const std::byte> input) override;
            virtual Result Flush() override;
            virtual Result SetSize(u64 size) override;
            virtual Result GetSize(u64 &out) override;
            virtual Result QueryRange(s64, s64, fs::QueryRangeInfo &) override;
            Result Invalidate(s64, s64) override;
    };

}

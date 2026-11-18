#include "fs_filesystem.hpp"
#include "fs_block_device_storage.hpp"

namespace kumo::horizon::hzfs::blockdev {

    namespace {

        /* Upper bound on blocks per driver request, so one large read cannot monopolise a device. */
        constexpr u32 MaxBlocksPerRequest = 256;

    }

    BlockDeviceStorage::BlockDeviceStorage(IBlockDevice *device) : m_device(device), m_block_size(device->GetBlockSize()), m_size(device->GetBlockCount() * m_block_size), m_mutex() {
        KUMO_ASSERT(std::has_single_bit(m_block_size));
        KUMO_ASSERT(m_block_size <= MaxBlockSize);
        KUMO_ASSERT(device->GetBlockCount() <= INT64_MAX / m_block_size);
    }

    Result BlockDeviceStorage::Read(u64 offset, std::span<std::byte> output) {
        size_t size = output.size();
        void *buffer = output.data();
        /* Succeed immediately on a zero-sized read. */
        KUMO_SUCCEED_IF(size == 0);

        /* Check arguments. */
        KUMO_UNLESS(buffer != nullptr, fs::ResultNullptrArgument());
        KUMO_UNLESS(kumo::fs::Contains(m_size, offset, size), fs::ResultOutOfRange());

        std::scoped_lock lk(m_mutex);

        u8 *dst = static_cast<u8 *>(buffer);
        while (size > 0) {
            const u64 lba      = static_cast<u64>(offset) / m_block_size;
            const u32 in_block = static_cast<u64>(offset) % m_block_size;

            size_t cur;
            if (in_block != 0 || size < m_block_size) {
                /* Partial block: go through the bounce buffer. */
                KUMO_TRY(m_device->Read(lba, m_bounce, 1));
                cur = std::min<size_t>(m_block_size - in_block, size);
                std::memcpy(dst, m_bounce + in_block, cur);
            } else {
                /* Whole blocks: transfer directly. */
                const u32 blocks = static_cast<u32>(std::min<u64>(size / m_block_size, MaxBlocksPerRequest));
                KUMO_TRY(m_device->Read(lba, dst, blocks));
                cur = static_cast<size_t>(blocks) * m_block_size;
            }

            dst    += cur;
            offset += cur;
            size   -= cur;
        }

        KUMO_SUCCEED();
    }

    Result BlockDeviceStorage::Write(u64 offset, std::span<const std::byte> input) {
        size_t size = input.size();
        const void *buffer = input.data();
        /* Succeed immediately on a zero-sized write. */
        KUMO_SUCCEED_IF(size == 0);

        /* Check arguments. */
        KUMO_UNLESS(buffer != nullptr, fs::ResultNullptrArgument());
        KUMO_UNLESS(kumo::fs::Contains(m_size, offset, size), fs::ResultOutOfRange());

        std::scoped_lock lk(m_mutex);

        const u8 *src = static_cast<const u8 *>(buffer);
        while (size > 0) {
            const u64 lba      = static_cast<u64>(offset) / m_block_size;
            const u32 in_block = static_cast<u64>(offset) % m_block_size;

            size_t cur;
            if (in_block != 0 || size < m_block_size) {
                /* Partial block: read, modify, write. */
                KUMO_TRY(m_device->Read(lba, m_bounce, 1));
                cur = std::min<size_t>(m_block_size - in_block, size);
                std::memcpy(m_bounce + in_block, src, cur);
                KUMO_TRY(m_device->Write(lba, m_bounce, 1));
            } else {
                const u32 blocks = static_cast<u32>(std::min<u64>(size / m_block_size, MaxBlocksPerRequest));
                KUMO_TRY(m_device->Write(lba, src, blocks));
                cur = static_cast<size_t>(blocks) * m_block_size;
            }

            src    += cur;
            offset += cur;
            size   -= cur;
        }

        KUMO_SUCCEED();
    }

    Result BlockDeviceStorage::Flush() {
        std::scoped_lock lk(m_mutex);
        KUMO_RETURN(m_device->Flush());
    }

    Result BlockDeviceStorage::SetSize(u64 size) {
        /* A block device cannot be resized. */
        (void)size;
        KUMO_THROW(fs::ResultUnsupportedOperation());
    }

    Result BlockDeviceStorage::GetSize(u64 &out) {
        out = m_size;
        KUMO_SUCCEED();
    }

    Result BlockDeviceStorage::QueryRange(s64, s64, fs::QueryRangeInfo &info) {
        info = {};
        return 0;
    }

    Result BlockDeviceStorage::Invalidate(s64, s64) {
        return 0;
    }

}

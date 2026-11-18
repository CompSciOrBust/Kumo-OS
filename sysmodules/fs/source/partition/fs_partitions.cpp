#include "fs_filesystem.hpp"
#include "fs_partitions.hpp"
#include "fs_gpt.hpp"
#include "../fs_log.hpp"
#include "../blockdev/fs_block_device_registry.hpp"

namespace kumo::horizon::hzfs::partition {

    namespace {

        constinit PartitionTable g_table = {};
        class PartitionStorage final : public fs::IStorage {
            kumo::fs::StorageSlice slice_;
        public:
            Result Open(fs::IStorage &storage, u64 offset, u64 size) {
                return slice_.Open(storage, offset, size);
            }
            Result Read(u64 offset, std::span<std::byte> output) override {
                return output.empty() ? 0 : slice_.Read(offset, output);
            }
            Result Write(u64 offset, std::span<const std::byte> input) override {
                return input.empty() ? 0 : slice_.Write(offset, input);
            }
            Result Flush() override { return slice_.Flush(); }
            Result GetSize(u64 &size) override { return slice_.GetSize(size); }
            Result SetSize(u64) override { return kumo::fs::Error(6302); }
            Result Invalidate(s64, s64) override { return 0; }
            Result QueryRange(s64 offset, s64 size, fs::QueryRangeInfo &info) override {
                // Legacy BIS views report the raw device's range flags. They
                // validate the signed arithmetic even beyond the partition.
                if (size != 0) {
                    KUMO_UNLESS(offset >= 0, fs::ResultInvalidOffset());
                    KUMO_UNLESS(size >= 0, fs::ResultInvalidSize());
                    KUMO_UNLESS(size <= INT64_MAX - offset, fs::ResultOutOfRange());
                }
                info = {};
                return 0;
            }
        };

        constinit PartitionStorage g_storages[MaxPartitions] = {};
        constinit u32 g_block_size = 0;

    }

    void Initialize() {
        blockdev::BlockDeviceStorage *system = blockdev::GetSystemStorage();
        if (system == nullptr) {
            return;
        }
        g_block_size = system->GetDevice()->GetBlockSize();

        if (const Result rc = ReadGpt(std::addressof(g_table), system, g_block_size); R_FAILED(rc)) {
            g_table.count = 0;
            Log("[fs] %s: no valid GPT (rc=0x%x)\n", system->GetDevice()->GetName(), rc);
            return;
        }

        for (size_t i = 0; i < g_table.count; ++i) {
            const PartitionEntry &entry = g_table.entries[i];
            KUMO_ABORT_RESULT(g_storages[i].Open(*system, entry.first_lba * g_block_size, entry.block_count * g_block_size));
            Log("[fs] partition %zu: \"%s\", %llu blocks at LBA %llu\n", i, entry.name, static_cast<unsigned long long>(entry.block_count), static_cast<unsigned long long>(entry.first_lba));
        }
    }

    u32 GetBlockSize() {
        return g_block_size;
    }

    fs::IStorage *FindPartition(const char *name) {
        for (size_t i = 0; i < g_table.count; ++i) {
            if (std::strcmp(g_table.entries[i].name, name) == 0) {
                return &g_storages[i];
            }
        }
        return nullptr;
    }

    const char *GetBisPartitionName(fs::BisPartitionId id) {
        switch (id) {
            case fs::BisPartitionId::BootConfigAndPackage2Part1: return "BCPKG2-1-Normal-Main";
            case fs::BisPartitionId::BootConfigAndPackage2Part2: return "BCPKG2-2-Normal-Sub";
            case fs::BisPartitionId::BootConfigAndPackage2Part3: return "BCPKG2-3-SafeMode-Main";
            case fs::BisPartitionId::BootConfigAndPackage2Part4: return "BCPKG2-4-SafeMode-Sub";
            case fs::BisPartitionId::BootConfigAndPackage2Part5: return "BCPKG2-5-Repair-Main";
            case fs::BisPartitionId::BootConfigAndPackage2Part6: return "BCPKG2-6-Repair-Sub";
            case fs::BisPartitionId::CalibrationBinary:          return "PRODINFO";
            case fs::BisPartitionId::CalibrationFile:            return "PRODINFOF";
            case fs::BisPartitionId::SafeMode:                   return "SAFE";
            case fs::BisPartitionId::User:                       return "USER";
            case fs::BisPartitionId::System:                     return "SYSTEM";
            /* On the Switch these are other views of the SYSTEM partition. Nothing here is encrypted, so they are the same. */
            case fs::BisPartitionId::SystemProperEncryption:     return "SYSTEM";
            case fs::BisPartitionId::SystemProperPartition:      return "SYSTEM";
            default:                                             return nullptr;
        }
    }

    Result GetBisStorage(fs::IStorage **out, fs::BisPartitionId id) {
        /* The whole user area of the eMMC is the whole system storage here. */
        if (id == fs::BisPartitionId::UserDataRoot) {
            fs::IStorage *system = blockdev::GetSystemStorage();
            KUMO_UNLESS(system != nullptr, fs::ResultPartitionNotFound());

            *out = system;
            KUMO_SUCCEED();
        }

        /* There are no eMMC boot partitions, so ids without a name are not found. */
        const char *name = GetBisPartitionName(id);
        KUMO_UNLESS(name != nullptr, fs::ResultPartitionNotFound());

        fs::IStorage *storage = FindPartition(name);
        KUMO_UNLESS(storage != nullptr, fs::ResultPartitionNotFound());

        *out = storage;
        KUMO_SUCCEED();
    }

}

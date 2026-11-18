#include "fs_filesystem.hpp"
#include "fs_block_device_registry.hpp"

namespace kumo::horizon::hzfs::blockdev {

    namespace {

        constinit std::optional<BlockDeviceStorage> g_storages[MaxBlockDevices] = {};
        constinit int g_count = 0;
        constinit kumo::Mutex g_mutex;

    }

    void RegisterBlockDevice(IBlockDevice *device) {
        KUMO_ASSERT(device != nullptr);

        std::scoped_lock lk(g_mutex);
        KUMO_ASSERT(g_count < MaxBlockDevices);

        g_storages[g_count].emplace(device);
        ++g_count;
    }

    int GetBlockDeviceCount() {
        std::scoped_lock lk(g_mutex);
        return g_count;
    }

    BlockDeviceStorage *GetBlockDeviceStorage(int index) {
        std::scoped_lock lk(g_mutex);
        if (index < 0 || index >= g_count) {
            return nullptr;
        }
        return &*g_storages[index];
    }

    BlockDeviceStorage *GetSystemStorage() {
        return GetBlockDeviceStorage(0);
    }

}

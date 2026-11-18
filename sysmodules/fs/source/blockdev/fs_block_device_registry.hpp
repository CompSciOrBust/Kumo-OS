/*
 * Registry of the block devices found at start-up. Drivers add devices here
 * from their probe function; the IPC layer looks them up by index.
 */
#pragma once
#include "fs_filesystem.hpp"
#include "fs_i_block_device.hpp"
#include "fs_block_device_storage.hpp"

namespace kumo::horizon::hzfs::blockdev {

    constexpr inline int MaxBlockDevices = 8;

    /* Called by drivers. The device must outlive the process (FS is immortal). */
    void RegisterBlockDevice(IBlockDevice *device);

    int GetBlockDeviceCount();

    /* Byte-addressed view of device `index`, or nullptr if there is no such device. */
    BlockDeviceStorage *GetBlockDeviceStorage(int index);

    /* The device the system partitions live on. Currently the first one registered. */
    BlockDeviceStorage *GetSystemStorage();

}

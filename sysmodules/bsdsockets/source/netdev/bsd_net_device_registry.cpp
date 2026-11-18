#include <kumo/prelude.hpp>
#include "bsd_net_device_registry.hpp"

namespace kumo::horizon::hznet::netdev {

    namespace {

        constinit INetDevice *g_devices[MaxNetDevices] = {};
        constinit int g_device_count = 0;

    }

    ::Result RegisterNetDevice(INetDevice *device) {
        KUMO_ABORT_UNLESS(device != nullptr);
        KUMO_UNLESS(g_device_count < MaxNetDevices, KERNELRESULT(ResourceExhausted));

        g_devices[g_device_count++] = device;
        KUMO_SUCCEED();
    }

    int GetNetDeviceCount() {
        return g_device_count;
    }

    INetDevice *GetNetDevice(int index) {
        KUMO_ABORT_UNLESS(0 <= index && index < g_device_count);
        return g_devices[index];
    }

}

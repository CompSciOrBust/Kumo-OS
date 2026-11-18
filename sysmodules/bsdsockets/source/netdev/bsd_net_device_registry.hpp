/*
 * The network devices found at start-up, in probe order. Device 0 is the
 * primary interface.
 */
#pragma once
#include <kumo/prelude.hpp>
#include "bsd_i_net_device.hpp"

namespace kumo::horizon::hznet::netdev {

    constexpr inline int MaxNetDevices = 4;

    /* Called by drivers from their probe. The device must live for the life of the process. */
    ::Result RegisterNetDevice(INetDevice *device);

    int GetNetDeviceCount();
    INetDevice *GetNetDevice(int index);

}

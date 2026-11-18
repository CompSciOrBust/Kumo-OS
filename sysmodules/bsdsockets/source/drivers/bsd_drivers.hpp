/*
 * Driver table. Each NIC driver contributes one probe function that detects
 * its hardware and registers every device it finds with
 * netdev::RegisterNetDevice. A probe that finds nothing succeeds.
 *
 * To add a driver: create drivers/<name>/, declare its probe here, and add
 * one line to the table in bsd_drivers.cpp. Take memory the device reads or
 * writes from GetDmaArena() rather than from arrays of the driver's own, so
 * that only the driver that binds uses any.
 */
#pragma once
#include <kumo/prelude.hpp>
#include <kumo/dma.hpp>

namespace kumo::horizon::hznet::drivers {

    namespace virtio_net { ::Result Probe(); }
    namespace gvnic      { ::Result Probe(); }

    /* The memory every driver takes its rings and frame buffers from (see DmaArenaSize in */
    /* bsd_drivers.cpp). Only probes allocate from it; whatever a probe takes, it keeps, even */
    /* when it then fails. */
    kumo::dma::Arena &GetDmaArena();

    /* Run every driver's probe, in table order. */
    void ProbeAll();

}

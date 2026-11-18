/*
 * Driver table. Each storage driver contributes one probe function that
 * detects its hardware and registers every device it finds with
 * blockdev::RegisterBlockDevice. A probe that finds nothing succeeds.
 *
 * To add a driver: create drivers/<name>/, declare its probe here, and add
 * one line to the table in fs_drivers.cpp.
 */
#pragma once
#include "fs_filesystem.hpp"

namespace kumo::horizon::hzfs::drivers {

    namespace nvme { Result Probe(); }
    namespace ram  { Result Probe(); }

    /* Run every driver's probe, in table order. */
    void ProbeAll();

}

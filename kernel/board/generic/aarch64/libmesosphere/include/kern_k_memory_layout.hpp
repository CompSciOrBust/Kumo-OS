#pragma once
#include <mesosphere/kern_common.hpp>
#include <mesosphere/kern_k_typed_address.hpp>

namespace ams::kern {

    constexpr inline KPhysicalAddress MainMemoryAddress = 0x40000000;

    constexpr inline size_t MainMemorySize    = 4_GB;
    constexpr inline size_t MainMemorySizeMax = 8_GB;

    constexpr inline u32 MinimumMemoryManagerAlignmentShifts[] = {
        0, 0, 0, 0
    };

}

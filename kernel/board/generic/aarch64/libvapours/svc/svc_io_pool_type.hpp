#pragma once
#include <vapours/svc/svc_types_common.hpp>

namespace ams::svc::board::generic::aarch64 {

    /* Both pools allow the same ranges: the PCI ECAM window and the device memory the */
    /* firmware assigned. There are two because the kernel permits one live pool per type, */
    /* and storage and network drivers live in different processes. */
    /* Values are part of the userland ABI for this board: append, never renumber. */
    enum IoPoolType : u32 {
        IoPoolType_PciStorage = 0,
        IoPoolType_PciNetwork = 1,

        IoPoolType_Count      = 2,
    };

}

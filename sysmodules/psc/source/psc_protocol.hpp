/* Power-control wire types and limits shared by the manager and IPC server. */
#pragma once
#include <cstddef>
#include <cstdint>

namespace kumo::horizon::psc::srv {

    constexpr std::size_t MaxModules = 64;
    constexpr std::size_t MaxDependencies = 32;
    constexpr std::uint32_t MaxDependencyLevels = 20;

    enum PmTransitionOrder : std::uint32_t {
        PmTransitionOrder_ToLowerPowerState = 0,
        PmTransitionOrder_ToHigherPowerState = 1,
    };

    // The wiki's TimeAfter size overlaps Result; use an eight-byte value.
    // This structure is currently only returned by an unsupported command.
    struct PmModuleTransitionInformation {
        std::int64_t time_before;
        std::int64_t time_after;
        std::uint32_t result;
        std::uint32_t state_before;
        std::uint32_t state_after;
        std::uint32_t reserved;
    };
    static_assert(sizeof(PmModuleTransitionInformation) == 0x20);
    static_assert(offsetof(PmModuleTransitionInformation, result) == 0x10);
}

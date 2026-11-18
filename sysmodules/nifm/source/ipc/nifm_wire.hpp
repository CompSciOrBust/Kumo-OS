#pragma once
#include <kumo/ipc/wire.hpp>
#include <switch/types.h>
#include <cstddef>

// Inline CMIF records for the Kumo NIFM server. Fixed pointer buffers are
// described by command policy rather than serialized as inline records.
namespace kumo::horizon::nifm::wire {
struct Uuid { static constexpr size_t WireBytes = 16; u8 bytes[16]; };
struct Requirement { static constexpr size_t WireBytes = 0x24; u8 bytes[0x24]; };
struct Ipv4Address { static constexpr size_t WireBytes = 4; u8 bytes[4]; };
struct IpConfigInfo { static constexpr size_t WireBytes = 0x16; u8 bytes[0x16]; };
struct ConnectionStatus { static constexpr size_t WireBytes = 3; u8 interface_type, wifi_strength, status; };
struct AppletInfo { static constexpr size_t WireBytes = 12; u32 applet_id, mode, size; };
struct Boolean {
    static constexpr size_t WireBytes = 1;
    u8 value;
    constexpr bool IsValidWire() const { return value <= 1; }
};
static_assert(kumo::ipc::IsWireType<Uuid> && kumo::ipc::IsWireType<Requirement>);
static_assert(kumo::ipc::IsWireType<Ipv4Address> && kumo::ipc::IsWireType<IpConfigInfo>);
static_assert(kumo::ipc::IsWireType<ConnectionStatus> && kumo::ipc::IsWireType<AppletInfo>);
static_assert(kumo::ipc::IsWireType<Boolean>);
static_assert(offsetof(AppletInfo, mode) == 4 && offsetof(AppletInfo, size) == 8);
static_assert(offsetof(ConnectionStatus, status) == 2);
}

/*
 * The structures nifm's commands exchange, in the layouts libnx expects
 * (nx/include/switch/services/nifm.h, https://switchbrew.org/wiki/Network_Interface_services).
 * Their fields are byte arrays wherever libnx's are, so none has padding.
 */
#pragma once
#include <switch.h>

namespace kumo::horizon::nifm {

    constexpr inline u32 ServiceTypeUser   = 0;
    constexpr inline u32 ServiceTypeSystem = 1;
    constexpr inline u32 ServiceTypeAdmin  = 2;

    /* NetworkInterfaceType */
    constexpr inline u8 InterfaceType_Ethernet = 2;

    /* RequestState */
    enum RequestState : u32 {
        RequestState_Invalid  = 0,
        RequestState_Free     = 1,      /* not submitted, cancelled or failed */
        RequestState_OnHold   = 2,      /* submitted, waiting for the network */
        RequestState_Accepted = 3,      /* the network is available */
    };

    /* The "connection status" libnx reports; Connected is 4 and the values below it are stages of connecting. */
    constexpr inline u8 InternetConnectionStatus_Connecting = 0;
    constexpr inline u8 InternetConnectionStatus_Connected  = 4;

    /* In and out through a pointer buffer, because libnx sends them that way (type 0x19 and 0x1A). */
    struct ClientId {
        u32 id;
    };
    static_assert(sizeof(ClientId) == 4);

    struct Uuid {
        u8 data[0x10];
    };

    struct Ipv4Address {
        u8 bytes[4];
    };

    struct IpAddressSetting {
        u8 is_automatic;
        u8 address[4];
        u8 netmask[4];
        u8 gateway[4];
    };
    static_assert(sizeof(IpAddressSetting) == 0xD);

    struct DnsSetting {
        u8 is_automatic;
        u8 primary[4];
        u8 secondary[4];
    };
    static_assert(sizeof(DnsSetting) == 0x9);

    /* GetCurrentIpConfigInfo's output: the two settings back to back. */
    struct IpConfigInfo {
        IpAddressSetting ip;
        DnsSetting dns;
    };
    static_assert(sizeof(IpConfigInfo) == 0x16);

    struct ProxySetting {
        u8 enabled;
        u8 reserved;
        u16 port;
        char server[0x64];
        u8 auto_auth_enabled;
        char user[0x20];
        char password[0x20];
        u8 reserved2;
    };
    static_assert(sizeof(ProxySetting) == 0xAA);

    struct IpSettingData {
        IpAddressSetting ip;
        DnsSetting dns;
        ProxySetting proxy;
        u16 mtu;
    };
    static_assert(sizeof(IpSettingData) == 0xC2);

    struct WirelessSettingData {
        u8 ssid_length;
        char ssid[0x20];
        u8 unknown[3];
        u8 passphrase[0x41];
    };
    static_assert(sizeof(WirelessSettingData) == 0x65);

    struct SfNetworkProfileData {
        IpSettingData ip_setting;
        Uuid id;
        char name[0x40];
        u8 profile_type;            /* NetworkProfileType: bit 0 is a profile saved by the user */
        u8 interface_type;          /* NetworkInterfaceType */
        u8 is_auto_connect;
        u8 is_large_capacity;
        WirelessSettingData wireless;
        u8 reserved;
    };
    static_assert(sizeof(SfNetworkProfileData) == 0x17C);

    struct SfNetworkProfileBasicInfo {
        Uuid id;
        char name[0x40];
        u8 profile_type;
        u8 interface_type;
        u8 ssid_length;
        char ssid[0x20];
        u8 authentication;
        u8 encryption;
    };
    static_assert(sizeof(SfNetworkProfileBasicInfo) == 0x75);

    /* GetInternetConnectionStatus's output. */
    struct InternetConnectionStatusData {
        u8 interface_type;
        u8 wifi_strength;
        u8 status;
    };

    /* IRequest::SetRequirement's input (0x20 bytes, and 4 more from 3.0.0). It is accepted and not interpreted. */
    struct Requirement {
        u8 data[0x24];
    };

    /* GetAppletInfo's output. */
    struct AppletInfo {
        u32 applet_id;
        u32 mode;
        u32 size;
    };

}

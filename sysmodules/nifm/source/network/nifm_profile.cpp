#include <cstring>
#include "nifm_profile.hpp"

namespace kumo::horizon::nifm::network {

    namespace {

        /* Fixed, so that a program that remembers the profile's ID finds it again after a reboot. The bytes spell "HorizonWired". */
        constexpr Uuid WiredProfileId = { { 'H', 'o', 'r', 'i', 'z', 'o', 'n', 'W', 'i', 'r', 'e', 'd', 0, 0, 0, 1 } };

        constexpr const char WiredProfileName[] = "Wired connection";

        /* NetworkProfileType: a profile the user saved. */
        constexpr u8 ProfileType_User = 1u << 0;

    }

    void GetWiredProfile(SfNetworkProfileData *out, const NetworkState &state) {
        *out = {};

        out->ip_setting.ip.is_automatic = 1;
        std::memcpy(out->ip_setting.ip.address, state.address, 4);
        std::memcpy(out->ip_setting.ip.netmask, state.netmask, 4);
        std::memcpy(out->ip_setting.ip.gateway, state.gateway, 4);
        out->ip_setting.dns.is_automatic = 1;
        std::memcpy(out->ip_setting.dns.primary,   state.dns[0], 4);
        std::memcpy(out->ip_setting.dns.secondary, state.dns[1], 4);
        out->ip_setting.mtu = state.mtu;

        out->id = WiredProfileId;
        std::memcpy(out->name, WiredProfileName, sizeof(WiredProfileName));
        out->profile_type    = ProfileType_User;
        out->interface_type  = InterfaceType_Ethernet;
        out->is_auto_connect = 1;
    }

    void GetWiredProfileBasicInfo(SfNetworkProfileBasicInfo *out) {
        *out = {};
        out->id = WiredProfileId;
        std::memcpy(out->name, WiredProfileName, sizeof(WiredProfileName));
        out->profile_type   = ProfileType_User;
        out->interface_type = InterfaceType_Ethernet;
    }

    bool IsWiredProfileId(const Uuid &id) {
        return std::memcmp(id.data, WiredProfileId.data, sizeof(id.data)) == 0;
    }

}

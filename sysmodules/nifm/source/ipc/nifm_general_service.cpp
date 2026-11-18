#include <kumo/prelude.hpp>
#include "nifm_general_service.hpp"
#include "nifm_request.hpp"
#include "../nifm_results.hpp"
#include "../network/nifm_network_state.hpp"
#include "../network/nifm_profile.hpp"

namespace kumo::horizon::nifm {

    namespace {

        Ipv4Address ToIpv4Address(const u8 (&bytes)[4]) {
            Ipv4Address address;
            std::memcpy(address.bytes, bytes, sizeof(bytes));
            return address;
        }

    }

    Result GeneralService::CheckSystemPermission() const {
        KUMO_UNLESS(m_service_type != ServiceTypeUser, ResultInvalidPermission());
        KUMO_SUCCEED();
    }

    Result GeneralService::GetClientId(ClientId &out) {
        out = ClientId{m_client_id};
        KUMO_SUCCEED();
    }

    Result GeneralService::GetCurrentNetworkProfile(SfNetworkProfileData &out) {
        const network::NetworkState state = network::GetState();
        KUMO_UNLESS(state.has_address, ResultNetworkNotAvailable());

        SfNetworkProfileData profile;
        network::GetWiredProfile(std::addressof(profile), state);
        out = profile;
        KUMO_SUCCEED();
    }

    Result GeneralService::EnumerateNetworkProfiles(s32 &out_count, SfNetworkProfileBasicInfo *out, size_t capacity, u8 type) {
        /* The one profile is a user profile; asking for other kinds (nifm:a can) finds none. */
        constexpr u8 NetworkProfileType_User = 1u << 0;
        if ((type & NetworkProfileType_User) == 0 || capacity == 0) {
            out_count = 0;
            KUMO_SUCCEED();
        }

        network::GetWiredProfileBasicInfo(std::addressof(out[0]));
        out_count = 1;
        KUMO_SUCCEED();
    }

    Result GeneralService::GetNetworkProfile(SfNetworkProfileData &out, Uuid id) {
        KUMO_UNLESS(network::IsWiredProfileId(id), ResultWrongInput());

        SfNetworkProfileData profile;
        network::GetWiredProfile(std::addressof(profile), network::GetState());
        out = profile;
        KUMO_SUCCEED();
    }

    Result GeneralService::GetCurrentIpAddress(Ipv4Address &out) {
        const network::NetworkState state = network::GetState();
        KUMO_UNLESS(state.has_address, ResultNetworkNotAvailable());

        out = ToIpv4Address(state.address);
        KUMO_SUCCEED();
    }

    Result GeneralService::GetCurrentIpConfigInfo(IpConfigInfo &out) {
        const network::NetworkState state = network::GetState();
        KUMO_UNLESS(state.has_address, ResultNetworkNotAvailable());

        IpConfigInfo info = {};
        info.ip.is_automatic  = state.dhcp ? 1 : 0;
        std::memcpy(info.ip.address, state.address, 4);
        std::memcpy(info.ip.netmask, state.netmask, 4);
        std::memcpy(info.ip.gateway, state.gateway, 4);
        info.dns.is_automatic = state.dhcp ? 1 : 0;
        std::memcpy(info.dns.primary,   state.dns[0], 4);
        std::memcpy(info.dns.secondary, state.dns[1], 4);
        out = info;
        KUMO_SUCCEED();
    }

    Result GeneralService::SetWirelessCommunicationEnabled(bool enabled) {
        KUMO_TRY(this->CheckSystemPermission());

        /* There is no wireless device to switch. */
        (void)enabled;
        KUMO_SUCCEED();
    }

    Result GeneralService::IsWirelessCommunicationEnabled(bool &out) {
        out = false;
        KUMO_SUCCEED();
    }

    Result GeneralService::GetInternetConnectionStatus(InternetConnectionStatusData &out) {
        const network::NetworkState state = network::GetState();

        /* As on the Switch, there is no status to report unless the network is connected or being connected. */
        u8 status;
        if (state.present && state.has_address) {
            status = InternetConnectionStatus_Connected;
        } else if (state.present && state.configured && state.link_up) {
            status = InternetConnectionStatus_Connecting;
        } else {
            KUMO_THROW(ResultNetworkNotAvailable());
        }

        out = InternetConnectionStatusData{ InterfaceType_Ethernet, 0, status };
        KUMO_SUCCEED();
    }

    Result GeneralService::IsEthernetCommunicationEnabled(bool &out) {
        out = true;
        KUMO_SUCCEED();
    }

    Result GeneralService::IsAnyInternetRequestAccepted(bool &out, const ClientId &client_id) {
        /* Any client's request counts; the client's own ID changes nothing. */
        (void)client_id;
        out = IsAnyRequestAccepted();
        KUMO_SUCCEED();
    }

    Result GeneralService::IsAnyForegroundRequestAccepted(bool &out) {
        /* There is no foreground: every request is one. */
        out = IsAnyRequestAccepted();
        KUMO_SUCCEED();
    }

    /* There is no sleep mode, so there is nothing to prepare for or recover from. */
    Result GeneralService::PutToSleep() { KUMO_SUCCEED(); }
    Result GeneralService::WakeUp()     { KUMO_SUCCEED(); }

    Result GeneralService::SetWowlDelayedWakeTime(s32 time) {
        KUMO_TRY(this->CheckSystemPermission());

        /* Wake-on-wireless: no wireless device. */
        (void)time;
        KUMO_SUCCEED();
    }

}

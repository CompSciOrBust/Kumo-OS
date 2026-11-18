/*
 * IGeneralService: what programs ask nifm about the network, and where they
 * create requests. One object per client; nifm:u, nifm:s and nifm:a differ in
 * which commands they accept (see the permission checks).
 */
#pragma once
#include "../nifm_types.hpp"
#include <cstddef>

namespace kumo::horizon::nifm {

    class GeneralService {
        private:
            const u32 m_service_type;       /* ServiceType* of the port this was opened through */
            const u32 m_client_id;
        public:
            GeneralService(u32 service_type, u32 client_id) : m_service_type(service_type), m_client_id(client_id) { /* ... */ }
        private:
            /* nifm:s and nifm:a only. */
            Result CheckSystemPermission() const;
        public:
            Result GetClientId(ClientId &out);
            Result GetCurrentNetworkProfile(SfNetworkProfileData &out);
            Result EnumerateNetworkProfiles(s32 &out_count, SfNetworkProfileBasicInfo *out, size_t capacity, u8 type);
            Result GetNetworkProfile(SfNetworkProfileData &out, Uuid id);
            Result GetCurrentIpAddress(Ipv4Address &out);
            Result GetCurrentIpConfigInfo(IpConfigInfo &out);
            Result SetWirelessCommunicationEnabled(bool enabled);
            Result IsWirelessCommunicationEnabled(bool &out);
            Result GetInternetConnectionStatus(InternetConnectionStatusData &out);
            Result IsEthernetCommunicationEnabled(bool &out);
            Result IsAnyInternetRequestAccepted(bool &out, const ClientId &client_id);
            Result IsAnyForegroundRequestAccepted(bool &out);
            Result PutToSleep();
            Result WakeUp();
            Result SetWowlDelayedWakeTime(s32 time);

    };

}

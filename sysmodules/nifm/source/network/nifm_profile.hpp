/*
 * The network profile.
 *
 * On the Switch nifm reads saved profiles (name, IP settings, Wi-Fi
 * credentials) from the settings and system save data services and connects
 * according to the one a request names. Neither exists here, and the only
 * network is the wired interface the sockets module found, so there is one
 * profile, built in: Ethernet, automatic IP and DNS.
 *
 * This is where saved profiles would plug in. Their IP setting would also
 * decide how the interface is brought up (a static address instead of DHCP),
 * which nifm_network_state.cpp does today with a fixed choice.
 */
#pragma once
#include <switch.h>
#include "nifm_network_state.hpp"
#include "../nifm_types.hpp"

namespace kumo::horizon::nifm::network {

    /* The wired profile as it looks now: the settings the interface currently has. */
    void GetWiredProfile(SfNetworkProfileData *out, const NetworkState &state);

    void GetWiredProfileBasicInfo(SfNetworkProfileBasicInfo *out);

    bool IsWiredProfileId(const Uuid &id);

}

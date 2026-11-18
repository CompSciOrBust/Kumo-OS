/*
 * IRequest: a program's ask for the network.
 *
 * A request is Free until it is submitted. Submitting while the interface has
 * an address makes it Accepted at once; while the address is still being
 * acquired it is OnHold and becomes Accepted when the address arrives; with no
 * network device it stays Free and GetResult reports "network not available".
 * An Accepted request whose address is later lost goes back to Free with
 * "connection reset". Cancel returns any request to Free.
 *
 * Every request keeps itself current by being told of each change in the
 * network (NotifyNetworkChanged) and signals its events when its own state
 * changes.
 */
#pragma once
#include "../nifm_types.hpp"
#include <cstddef>
#include "../network/nifm_network_state.hpp"

namespace kumo::horizon::nifm {

    constexpr inline size_t MaxRequests = 64;

    class Request {
        private:
            ::Event m_state_event{};      /* signalled when the request's state or result changes */
            ::Event m_network_event{};    /* signalled when the network the request depends on changes */
            RequestState m_state;
            Result m_result;
            bool m_has_socket;
            s32 m_socket_fd;
        public:
            Request();
            ~Request();

            /* The registry's use: with its lock held, react to the network having changed. */
            void OnNetworkChanged(const network::NetworkState &state);
            bool IsAccepted() const { return m_state == RequestState_Accepted; }
        private:
            void SetState(RequestState state, Result result);
            void Evaluate(const network::NetworkState &state);
        public:
            Result GetRequestState(u32 &out);
            Result GetResult();
            Result GetSystemEventReadableHandles(Handle &out_state_event, Handle &out_network_event);
            Result Cancel();
            Result Submit();
            Result RegisterSocketDescriptor(s32 fd);
            Result UnregisterSocketDescriptor(s32 fd);

            /* Accepted and ignored: nothing here has a requirement a request could change. */
            Result SetRequirement(Requirement requirement);
            Result SetRequirementPreset(u32 preset);
            Result SetPriority(u8 priority);
            Result SetNetworkProfileId(Uuid id);
            Result SetRejectable(bool value);
            Result SetConnectionConfirmationOption(u8 option);
            Result SetPersistent(bool value);
            Result SetInstant(bool value);
            Result SetSustainable(bool value);
            Result SetRawPriority(u8 priority);
            Result SetGreedy(bool value);
            Result SetSharable(bool value);
            Result SetRequirementByRevision(u32 revision);
            Result SetKeptInSleep(bool value);

            /* Not supported. */
            Result GetRequirement();
            Result GetRevision();
            Result GetAdditionalInfo();
            Result GetNetworkAccessStatus();
    };

    /* How many requests exist. */
    size_t GetRequestCount();

    /* Whether any request is Accepted. */
    bool IsAnyRequestAccepted();

    /* Tell every request the network changed. Called by the monitor thread. */
    void NotifyNetworkChanged(const network::NetworkState &state);

}

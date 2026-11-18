#include <kumo/prelude.hpp>
#include <kumo/mutex.hpp>
#include <kumo/assert.hpp>
#include "nifm_request.hpp"
#include "../nifm_results.hpp"

namespace kumo::horizon::nifm {

    namespace {

        /* Every request, so that a change in the network reaches all of them. Requests are created and */
        /* destroyed by the IPC thread and updated by the monitor thread. */
        constinit ::kumo::Mutex g_mutex;
        constinit Request *g_requests[MaxRequests] = {};
        constinit size_t g_request_count = 0;

    }

    Request::Request() : m_state(RequestState_Free), m_result(ResultNotSubmitted()), m_has_socket(false), m_socket_fd(0)
    {
        KUMO_ABORT_RESULT(::eventCreate(&m_state_event, false));
        KUMO_ABORT_RESULT(::eventCreate(&m_network_event, false));
        std::scoped_lock lk(g_mutex);
        KUMO_ABORT_UNLESS(g_request_count < MaxRequests);
        g_requests[g_request_count++] = this;
    }

    Request::~Request() {
        std::scoped_lock lk(g_mutex);
        ::eventClose(&m_state_event);
        ::eventClose(&m_network_event);
        for (size_t i = 0; i < g_request_count; ++i) {
            if (g_requests[i] == this) {
                g_requests[i] = g_requests[--g_request_count];
                return;
            }
        }
    }

    void Request::SetState(RequestState state, Result result) {
        m_state  = state;
        m_result = result;
        if (state != RequestState_Accepted) {
            m_has_socket = false;
        }
        KUMO_ABORT_RESULT(::eventFire(&m_state_event));
    }

    void Request::Evaluate(const network::NetworkState &state) {
        if (m_state == RequestState_OnHold) {
            if (!state.present) {
                this->SetState(RequestState_Free, ResultNetworkNotAvailable());
            } else if (state.has_address) {
                this->SetState(RequestState_Accepted, 0);
            }
        } else if (m_state == RequestState_Accepted && !state.has_address) {
            this->SetState(RequestState_Free, ResultConnectionReset());
        }
    }

    void Request::OnNetworkChanged(const network::NetworkState &state) {
        KUMO_ABORT_RESULT(::eventFire(&m_network_event));
        this->Evaluate(state);
    }

    Result Request::GetRequestState(u32 &out) {
        std::scoped_lock lk(g_mutex);
        out = m_state;
        KUMO_SUCCEED();
    }

    Result Request::GetResult() {
        std::scoped_lock lk(g_mutex);
        KUMO_RETURN(m_result);
    }

    Result Request::GetSystemEventReadableHandles(Handle &out_state_event, Handle &out_network_event) {
        out_state_event = m_state_event.revent;
        out_network_event = m_network_event.revent;
        KUMO_SUCCEED();
    }

    Result Request::Cancel() {
        std::scoped_lock lk(g_mutex);
        if (m_state != RequestState_Free) {
            this->SetState(RequestState_Free, ResultNotSubmitted());
        }
        KUMO_SUCCEED();
    }

    Result Request::Submit() {
        std::scoped_lock lk(g_mutex);

        if (m_state != RequestState_Free) {
            KUMO_SUCCEED();
        }

        /* Whatever the network is now, it is what this request is judged on; later changes reach it through */
        /* NotifyNetworkChanged, which waits for this lock. */
        const network::NetworkState state = network::GetState();
        if (!state.present) {
            /* The submission itself succeeds; a request that cannot be served says so through GetResult. */
            this->SetState(RequestState_Free, ResultNetworkNotAvailable());
        } else {
            this->SetState(RequestState_OnHold, 0);
            this->Evaluate(state);
        }
        KUMO_SUCCEED();
    }

    Result Request::RegisterSocketDescriptor(s32 fd) {
        std::scoped_lock lk(g_mutex);

        /* One socket per request, and only on a request that has the network. */
        KUMO_UNLESS(m_state == RequestState_Accepted, KERNELRESULT(InvalidState));
        KUMO_UNLESS(!m_has_socket, KERNELRESULT(InvalidState));

        m_has_socket = true;
        m_socket_fd  = fd;
        KUMO_SUCCEED();
    }

    Result Request::UnregisterSocketDescriptor(s32 fd) {
        std::scoped_lock lk(g_mutex);

        KUMO_UNLESS(m_state == RequestState_Accepted, KERNELRESULT(InvalidState));
        KUMO_UNLESS(m_has_socket && m_socket_fd == fd, ResultWrongInput());

        m_has_socket = false;
        KUMO_SUCCEED();
    }

    Result Request::SetRequirement(Requirement requirement)                 { (void)requirement; KUMO_SUCCEED(); }
    Result Request::SetRequirementPreset(u32 preset)                        { (void)preset; KUMO_SUCCEED(); }
    Result Request::SetPriority(u8 priority)                                { (void)priority; KUMO_SUCCEED(); }
    Result Request::SetNetworkProfileId(Uuid id)                            { (void)id; KUMO_SUCCEED(); }
    Result Request::SetRejectable(bool value)                               { (void)value; KUMO_SUCCEED(); }
    Result Request::SetConnectionConfirmationOption(u8 option)              { (void)option; KUMO_SUCCEED(); }
    Result Request::SetPersistent(bool value)                               { (void)value; KUMO_SUCCEED(); }
    Result Request::SetInstant(bool value)                                  { (void)value; KUMO_SUCCEED(); }
    Result Request::SetSustainable(bool value)                              { (void)value; KUMO_SUCCEED(); }
    Result Request::SetRawPriority(u8 priority)                             { (void)priority; KUMO_SUCCEED(); }
    Result Request::SetGreedy(bool value)                                   { (void)value; KUMO_SUCCEED(); }
    Result Request::SetSharable(bool value)                                 { (void)value; KUMO_SUCCEED(); }
    Result Request::SetRequirementByRevision(u32 revision)                  { (void)revision; KUMO_SUCCEED(); }
    Result Request::SetKeptInSleep(bool value)                              { (void)value; KUMO_SUCCEED(); }

    Result Request::GetRequirement()        { KUMO_THROW(kumo::KernelError(127)); }
    Result Request::GetRevision()           { KUMO_THROW(kumo::KernelError(127)); }
    Result Request::GetAdditionalInfo()     { KUMO_THROW(kumo::KernelError(127)); }
    Result Request::GetNetworkAccessStatus(){ KUMO_THROW(kumo::KernelError(127)); }

    size_t GetRequestCount() {
        std::scoped_lock lk(g_mutex);
        return g_request_count;
    }

    bool IsAnyRequestAccepted() {
        std::scoped_lock lk(g_mutex);
        for (size_t i = 0; i < g_request_count; ++i) {
            if (g_requests[i]->IsAccepted()) {
                return true;
            }
        }
        return false;
    }

    void NotifyNetworkChanged(const network::NetworkState &state) {
        std::scoped_lock lk(g_mutex);
        for (size_t i = 0; i < g_request_count; ++i) {
            g_requests[i]->OnNetworkChanged(state);
        }
    }

}

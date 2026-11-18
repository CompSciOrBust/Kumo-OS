#include <kumo/prelude.hpp>
#include "bsdcfg_service.hpp"
#include "../stack/bsd_stack.hpp"

namespace kumo::horizon::hznet::ipc {

    namespace {

        /* Reading this as an index is safe: the stack rejects any that is out of range. */
        int ToIndex(u32 index) {
            return index > 0x7FFFFFFF ? -1 : static_cast<int>(index);
        }

    }

    ::Result BsdConfigService::SetIfUp(IfUpInput in, const void *config, size_t config_size) {
        stack::Ipv4Config ip = stack::DhcpConfig;

        if ((in.flags & IfUpFlag_Dhcp) == 0) {
            KUMO_UNLESS(config_size >= sizeof(IfStaticConfig), KERNELRESULT(InvalidSize));

            IfStaticConfig fixed;
            std::memcpy(std::addressof(fixed), config, sizeof(fixed));
            ip = { .use_dhcp = false, .address = fixed.address, .netmask = fixed.netmask, .gateway = fixed.gateway, .dns = { fixed.dns[0], fixed.dns[1] } };
        }

        KUMO_RETURN(stack::SetInterfaceUp(ToIndex(in.index), ip));
    }

    ::Result BsdConfigService::SetIfUpWithEvent(IfUpInput in, const void *config, size_t config_size, ::Handle *out_event) {
        KUMO_TRY(this->SetIfUp(in, config, config_size));

        /* One event for every interface and every client; a client re-reads the state when it fires. */
        *out_event = stack::GetStateChangedEvent();
        KUMO_SUCCEED();
    }

    ::Result BsdConfigService::CancelIf(u32 index) {
        /* What can be cancelled is an interface that is still waiting for its address. One that has */
        /* an address is left as it is, and so is one nobody has configured. */
        stack::InterfaceState state;
        KUMO_TRY(stack::GetInterfaceState(std::addressof(state), ToIndex(index)));

        if (state.configured && !state.has_address) {
            KUMO_RETURN(stack::SetInterfaceDown(ToIndex(index)));
        }
        KUMO_SUCCEED();
    }

    ::Result BsdConfigService::SetIfDown(u32 index) {
        KUMO_RETURN(stack::SetInterfaceDown(ToIndex(index)));
    }

    ::Result BsdConfigService::GetIfState(IfState *out_state, u32 index) {
        stack::InterfaceState state;
        KUMO_TRY(stack::GetInterfaceState(std::addressof(state), ToIndex(index)));

        IfState out = {};
        out.flags = (state.link_up     ? IfStateFlag_LinkUp     : 0)
                  | (state.configured  ? IfStateFlag_Configured : 0)
                  | (state.has_address ? IfStateFlag_HasAddress : 0)
                  | (state.use_dhcp    ? IfStateFlag_Dhcp       : 0);
        out.mtu     = state.mtu;
        std::memcpy(out.mac, state.mac, sizeof(out.mac));
        out.address = state.address;
        out.netmask = state.netmask;
        out.gateway = state.gateway;
        out.dns[0]  = state.dns[0];
        out.dns[1]  = state.dns[1];

        *out_state = out;
        KUMO_SUCCEED();
    }

    ::Result BsdConfigService::DhcpRenew(u32 index) {
        KUMO_RETURN(stack::RenewDhcp(ToIndex(index)));
    }

}

#include <kumo/prelude.hpp>
#include "nifm_bsdcfg_client.hpp"

namespace kumo::horizon::nifm::network {

    namespace {

        constexpr u32 CommandSetIfUpWithEvent = 1;
        constexpr u32 CommandGetIfState       = 4;

        constexpr u32 IfUpFlag_Dhcp = 1u << 0;

        struct IfUpInput {
            u32 index;
            u32 flags;
            u32 reserved[8];
        };
        static_assert(sizeof(IfUpInput) == 0x28);

        /* The buffer of SetIfUp, which DHCP does not use. It is sent anyway, zeroed, so that the call has the */
        /* same shape whatever it configures. */
        struct IfStaticConfig {
            u32 address;
            u32 netmask;
            u32 gateway;
            u32 dns[2];
        };

        constinit ::Service g_session = {};

    }

    ::Result ConnectToBsdConfig() {
        return ::smGetService(std::addressof(g_session), "bsdcfg");
    }

    ::Result SetIfUpDhcpWithEvent(::Handle *out_event, u32 index) {
        const IfUpInput in = { .index = index, .flags = IfUpFlag_Dhcp, .reserved = {} };
        const IfStaticConfig config = {};
        ::Handle handle = INVALID_HANDLE;

        KUMO_TRY(serviceDispatchIn(std::addressof(g_session), CommandSetIfUpWithEvent, in,
            .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
            .buffers      = { { std::addressof(config), sizeof(config) } },
            .out_handle_attrs = { SfOutHandleAttr_HipcCopy },
            .out_handles      = std::addressof(handle),
        ));

        *out_event = handle;
        return 0;
    }

    ::Result GetIfState(IfState *out, u32 index) {
        return serviceDispatchInOut(std::addressof(g_session), CommandGetIfState, index, *out);
    }

}

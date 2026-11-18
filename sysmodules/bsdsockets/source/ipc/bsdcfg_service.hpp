/*
 * The session object of bsdcfg. It is a thin layer over the interface
 * configuration functions in stack/bsd_stack.hpp: each command checks its
 * arguments and calls one of them.
 */
#pragma once
#include <kumo/prelude.hpp>
#include "bsdcfg_interface.hpp"

namespace kumo::horizon::hznet::ipc {

    class BsdConfigService {
        public:
            ::Result SetIfUp(IfUpInput in, const void *config, size_t config_size);
            ::Result SetIfUpWithEvent(IfUpInput in, const void *config, size_t config_size, ::Handle *out_event);
            ::Result CancelIf(u32 index);
            ::Result SetIfDown(u32 index);
            ::Result GetIfState(IfState *out_state, u32 index);
            ::Result DhcpRenew(u32 index);

    };

}

/*
 * The IPC server of bsdcfg. It is a separate server: the one that
 * serves bsd:u and bsd:s uses all 64 of the ports and sessions one manager
 * can hold.
 */
#pragma once
#include <kumo/prelude.hpp>

namespace kumo::horizon::hznet::ipc {

    /* Register bsdcfg and start the thread that serves it. */
    void StartConfigServer();

}

/*
 * The IPC server: registers bsd:u and bsd:s and runs the request loop.
 */
#pragma once
#include <kumo/prelude.hpp>

namespace kumo::horizon::hznet::ipc {

    void InitializeServer();
    [[noreturn]] void LoopServer();

}

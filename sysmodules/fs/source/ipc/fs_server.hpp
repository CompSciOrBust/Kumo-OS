#pragma once

namespace kumo::horizon::hzfs::ipc {
void InitializeServer();
[[noreturn]] void LoopServer();
}

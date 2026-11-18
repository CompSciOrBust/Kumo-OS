#pragma once
#include "fs_commands.hpp"

namespace kumo::horizon::hzfs::ipc {
Result ReturnFileSystem(ConcurrentReply &reply, std::shared_ptr<fs::fsa::IFileSystem> fs);
}

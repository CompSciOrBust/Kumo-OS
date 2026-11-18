#include "fs_server.hpp"
#include "fs_file_system_proxy.hpp"
#include <kumo/assert.hpp>
#include <kumo/ipc/object_server.hpp>

namespace kumo::horizon::hzfs::ipc {
namespace {
kumo::ipc::ObjectServer server({.sessions = 128, .domains = 128, .objects = 0x4000, .converted_domains = 64});
}

void InitializeServer() {
    KUMO_ABORT_RESULT(server.Register({"fsp-srv", 59, nullptr, OpenFileSystemProxy}));
    KUMO_ABORT_RESULT(server.Register({"fsp-pr", 1, nullptr, OpenProgramRegistry}));
    KUMO_ABORT_RESULT(server.Register({"fsp-ldr", 1, nullptr, OpenLoaderProxy}));
}

void LoopServer() {
    s32 priority;
    KUMO_ABORT_RESULT(svcGetThreadPriority(&priority, CUR_THREAD_HANDLE));
    KUMO_ABORT_RESULT(server.RunConcurrent({.count = 4, .priority = priority}));
    KUMO_ABORT("FS server exited");
}
}

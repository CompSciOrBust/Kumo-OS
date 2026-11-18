#pragma once
#include <kumo/ipc/object.hpp>

namespace kumo::horizon::hzfs::ipc {
kumo::ipc::ObjectRef OpenFileSystemProxy(void *);
kumo::ipc::ObjectRef OpenLoaderProxy(void *);
kumo::ipc::ObjectRef OpenProgramRegistry(void *);
}

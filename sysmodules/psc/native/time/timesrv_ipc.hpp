#pragma once
#include <kumo/ipc/object.hpp>
#include <switch.h>
namespace kumo::horizon::timesrv::srv {
void InitializeTimeServer();
kumo::ipc::ObjectRef OpenSystemStatic(void *);
kumo::ipc::ObjectRef OpenSystemUserStatic(void *);
}

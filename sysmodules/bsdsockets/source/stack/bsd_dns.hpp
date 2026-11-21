#pragma once
#include <kumo/prelude.hpp>

namespace kumo::horizon::hznet::dns {

enum class Status { Success, NotFound, TemporaryFailure, Cancelled, InvalidHandle };

using HostName = char[256];

bool ParseAddress(const char *name, u32 &address);
Status Resolve(const char *name, u64 pid, u32 cancel_handle, u32 &address);
Status Reverse(u32 address, u64 pid, u32 cancel_handle, HostName &name);
Status FinishImmediateRequest(u64 pid, u32 cancel_handle);
u32 AllocateCancelHandle(u64 pid);
void Cancel(u64 pid, u32 handle);

}

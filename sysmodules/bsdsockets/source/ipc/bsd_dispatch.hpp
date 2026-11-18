#pragma once
#include <kumo/ipc/concurrent_server.hpp>
#include "bsd_service.hpp"
namespace kumo::horizon::hznet::ipc {
    bool DispatchBsd(BsdService &service, kumo::ipc::ConcurrentRequest &request, kumo::ipc::ConcurrentReply &reply);
}

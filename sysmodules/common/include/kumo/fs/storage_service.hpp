#pragma once
#include <kumo/fs/storage.hpp>
#include <kumo/ipc/object.hpp>

namespace kumo::fs {

// IStorage CMIF object for process-lifetime or otherwise externally owned storage.
class StorageService final : public ipc::ServiceObject {
public:
    explicit StorageService(Storage &storage) : storage_(storage) {}
    bool Dispatch(ipc::ConcurrentRequest &request, ipc::ConcurrentReply &reply) override;

private:
    Storage &storage_;
};

} // namespace kumo::fs

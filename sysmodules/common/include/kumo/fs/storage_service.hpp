#pragma once
#include <kumo/fs/storage.hpp>
#include <kumo/ipc/object.hpp>
#include <memory>
#include <utility>

namespace kumo::fs {

// IStorage CMIF object for borrowed or shared storage.
class StorageService final : public ipc::ServiceObject {
public:
    explicit StorageService(Storage &storage) : storage_(storage) {}
    explicit StorageService(std::shared_ptr<Storage> storage) : owner_(std::move(storage)), storage_(*owner_) {}
    bool Dispatch(ipc::ConcurrentRequest &request, ipc::ConcurrentReply &reply) override;

private:
    std::shared_ptr<Storage> owner_;
    Storage &storage_;
};

} // namespace kumo::fs

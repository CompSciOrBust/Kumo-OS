#pragma once
#include <atomic>
#include <new>
#include <utility>
namespace kumo::ipc {
struct ConcurrentRequest;
struct ConcurrentReply;
// Implementations are shared by cloned sessions/domain entries. A pool-backed
// implementation may override Destroy; default objects use the module's heap.
class ServiceObject {
    std::atomic<unsigned> references_{1};

  protected:
    virtual ~ServiceObject() = default;
    virtual void Destroy() { delete this; }

  public:
    ServiceObject() = default;
    ServiceObject(const ServiceObject &) = delete;
    ServiceObject &operator=(const ServiceObject &) = delete;
    virtual bool Dispatch(ConcurrentRequest &, ConcurrentReply &) = 0;
    void Retain() { references_.fetch_add(1, std::memory_order_relaxed); }
    void Release() {
        if (references_.fetch_sub(1, std::memory_order_acq_rel) == 1)
            Destroy();
    }
};
class ObjectRef {
    ServiceObject *object_ = nullptr;

  public:
    ObjectRef() = default;
    static ObjectRef Adopt(ServiceObject *object) {
        ObjectRef r;
        r.object_ = object;
        return r;
    }
    ObjectRef(const ObjectRef &other) : object_(other.object_) {
        if (object_)
            object_->Retain();
    }
    ObjectRef(ObjectRef &&other) noexcept : object_(std::exchange(other.object_, nullptr)) {}
    ObjectRef &operator=(ObjectRef other) noexcept {
        std::swap(object_, other.object_);
        return *this;
    }
    ~ObjectRef() {
        if (object_)
            object_->Release();
    }
    explicit operator bool() const { return object_ != nullptr; }
    ServiceObject *operator->() const { return object_; }
};
template <class T, class... Args> ObjectRef MakeObject(Args &&...args) {
    return ObjectRef::Adopt(new (std::nothrow) T(std::forward<Args>(args)...));
}
} // namespace kumo::ipc

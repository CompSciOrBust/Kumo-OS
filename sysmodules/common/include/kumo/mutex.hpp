#pragma once
#include <switch.h>

namespace kumo {
/* BasicLockable adapter for libnx's zero-initialized mutex. */
class Mutex {
    ::Mutex mutex_{};
public:
    constexpr Mutex() = default;
    Mutex(const Mutex &) = delete;
    Mutex &operator=(const Mutex &) = delete;
    void lock() { ::mutexLock(&mutex_); }
    void unlock() { ::mutexUnlock(&mutex_); }
    bool IsLockedByCurrentThread() { return ::mutexIsLockedByCurrentThread(&mutex_); }
    ::Mutex *native_handle() { return &mutex_; }
};
}

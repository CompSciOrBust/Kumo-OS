#pragma once
#include <cstddef>
#include <memory>
#include <new>
#include <utility>

namespace kumo {
/* Storage for an explicitly constructed object. The caller tracks whether
 * an object is present and destroys it before reusing the storage. */
template<class T> class Storage {
    alignas(T) std::byte bytes_[sizeof(T)]{};
public:
    template<class... Args> T *construct(Args &&...args) {
        return std::construct_at(reinterpret_cast<T *>(bytes_), std::forward<Args>(args)...);
    }
    T *get() { return std::launder(reinterpret_cast<T *>(bytes_)); }
    void destroy() { std::destroy_at(get()); }
};
}

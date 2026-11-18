#pragma once
#include <utility>

namespace kumo {
template<class F> class ScopeExit {
    F function_;
    bool active_ = true;
public:
    explicit ScopeExit(F function) : function_(std::move(function)) {}
    ScopeExit(const ScopeExit &) = delete;
    ScopeExit &operator=(const ScopeExit &) = delete;
    ~ScopeExit() { if (active_) function_(); }
    void Cancel() { active_ = false; }
};
template<class F> ScopeExit(F) -> ScopeExit<F>;
}

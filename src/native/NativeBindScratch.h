#pragma once
#include <memory>

namespace kharvox::native {
// Retain CPU work-list capacity between binds. A nested invocation must never
// overwrite an outer bind's descriptor write pointers. Other threads have
// independent storage; no GPU resource or descriptor lifetime lives here.
template<class T> class BindScratchLease {
    struct Slot { T value; bool busy{}; };
    static Slot& slot() { static thread_local Slot value; return value; }
    Slot& shared = slot();
    std::unique_ptr<T> nested;
    bool ownsShared{};
public:
    BindScratchLease() {
        if (shared.busy) nested = std::make_unique<T>();
        else { shared.busy = true; ownsShared = true; }
    }
    ~BindScratchLease() { if (ownsShared) shared.busy = false; }
    BindScratchLease(const BindScratchLease&) = delete;
    BindScratchLease& operator=(const BindScratchLease&) = delete;
    T& get() { return ownsShared ? shared.value : *nested; }
};
}

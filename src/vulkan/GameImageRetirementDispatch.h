#pragma once
#include "../openxr/GameImageLifetime.h"
#include "../common/StallDiagnostics.h"
#include <vulkan/vulkan.h>
#include <atomic>

namespace kharvox {
// The game-facing wrapper must be outside Native's bookkeeping wrappers.
// Internal downstream mirror retirement intentionally bypasses this entry.
template<class Handle> struct GameImageRetirementEntry {
    using Function = void(VKAPI_PTR*)(VkDevice,Handle,const VkAllocationCallbacks*);
    static inline std::atomic<Function> next{};
    static void VKAPI_PTR call(VkDevice device,Handle handle,const VkAllocationCallbacks* allocator) {
        DiagnosticDuration total(std::is_same_v<Handle,VkImageView>
            ? "destroy-image-view-total" : "destroy-image-total");
        gameImageLifetime().retire(GameImageLifetime::key(handle),[&]{
            DiagnosticDuration downstream(std::is_same_v<Handle,VkImageView>
                ? "destroy-image-view-downstream" : "destroy-image-downstream");
            next.load(std::memory_order_acquire)(device,handle,allocator);
        });
    }
    static PFN_vkVoidFunction wrap(PFN_vkVoidFunction function) {
        next.store(reinterpret_cast<Function>(function),std::memory_order_release);
        return reinterpret_cast<PFN_vkVoidFunction>(&call);
    }
};
}

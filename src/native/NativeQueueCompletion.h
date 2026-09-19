#pragma once
#include <vulkan/vulkan.h>

namespace kharvox::native {
inline VkResult waitForDeviceIdle(VkDevice device, PFN_vkDeviceWaitIdle wait,
                                 void (*lock)(), void (*unlock)()) {
    if (!device || !wait || !lock || !unlock) return VK_ERROR_INITIALIZATION_FAILED;
    lock();
    const auto result = wait(device);
    unlock();
    return result;
}
}

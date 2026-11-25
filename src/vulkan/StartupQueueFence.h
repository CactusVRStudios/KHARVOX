#pragma once

#include <vulkan/vulkan.h>

namespace kharvox {

// The caller serializes access to the fence, which starts signaled. Submit
// takes the normal queue lock only for vkQueueSubmit, never for a host wait.
template<class Submit>
VkResult waitForStartupQueue(VkDevice device, VkFence fence,
    PFN_vkWaitForFences wait, PFN_vkResetFences reset, Submit submit) {
    // After a slow frame the previous marker can still be in flight.
    VkResult result = wait(device, 1, &fence, VK_TRUE, 0);
    if (result != VK_SUCCESS) return result;
    result = reset(device, 1, &fence);
    if (result != VK_SUCCESS) return result;
    result = submit(fence);
    if (result != VK_SUCCESS) return result;
    return wait(device, 1, &fence, VK_TRUE, 3000000000ULL);
}

}

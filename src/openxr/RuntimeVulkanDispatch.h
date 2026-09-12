#pragma once
#include <vulkan/vulkan.h>
#include <cstring>

namespace kharvox {
inline bool selectXrGraphicsQueue(VkQueue current,VkQueue candidate,VkQueueFlags flags) {
    // XR copies also record graphics commands; compute/transfer-only queues
    // cannot replace the graphics queue when DOOM enumerates auxiliary queues.
    return candidate && (flags & VK_QUEUE_GRAPHICS_BIT) && (!current || current==candidate);
}

inline PFN_vkCreateDevice resolveLayerCreateDevice(PFN_vkGetInstanceProcAddr next, VkInstance instance) {
    // vkCreateDevice is an instance command. Null-instance lookup is only
    // specified for global commands and is not a portable driver shortcut.
    return next && instance ? reinterpret_cast<PFN_vkCreateDevice>(next(instance, "vkCreateDevice")) : nullptr;
}
template<class Handle> inline void finishRuntimeVulkanCreate(bool xrSucceeded, VkResult* result, Handle* output) {
    if (!result || !output) return;
    if (!xrSucceeded || (*result == VK_SUCCESS && !*output)) *result = VK_ERROR_INITIALIZATION_FAILED;
    if (*result != VK_SUCCESS) *output = VK_NULL_HANDLE;
}
inline bool isPhysicalDeviceCommand(const char* name) {
    return name && (!std::strncmp(name, "vkGetPhysicalDevice", 19) ||
        !std::strcmp(name, "vkEnumerateDeviceExtensionProperties") ||
        !std::strcmp(name, "vkEnumerateDeviceLayerProperties"));
}

enum class RuntimeDispatchRoute { Downstream, CreateDevice, SessionLoader, PhysicalLoader };
struct RuntimeDispatchResult {
    PFN_vkVoidFunction function{};
    RuntimeDispatchRoute route{RuntimeDispatchRoute::Downstream};
};

// A physical handle must stay at the dispatch level which enumerated it.
// The simulator enumerates through our downstream callback and retains that
// callback for session/swapchain creation. Steam's retained session callback
// and the existing non-simulator physical-device route use the public loader.
inline RuntimeDispatchResult resolveRuntimeVulkanProc(
    VkInstance instance, const char* name, bool sessionLoader, bool simulator,
    PFN_vkGetInstanceProcAddr next, PFN_vkGetInstanceProcAddr loader,
    PFN_vkVoidFunction createDevice) {
    if (!name) return {};
    if (sessionLoader && loader)
        return {loader(instance, name), RuntimeDispatchRoute::SessionLoader};
    if (!std::strcmp(name, "vkCreateDevice") && createDevice)
        return {createDevice, RuntimeDispatchRoute::CreateDevice};
    if (isPhysicalDeviceCommand(name) && !simulator && loader)
        return {loader(instance, name), RuntimeDispatchRoute::PhysicalLoader};
    // Do not fall back to public exports if a downstream command is absent.
    return {next ? next(instance, name) : nullptr, RuntimeDispatchRoute::Downstream};
}
} // namespace kharvox

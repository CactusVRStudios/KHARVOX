#pragma once
#include <vulkan/vulkan.h>
#include <cstring>

namespace kharvox {
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

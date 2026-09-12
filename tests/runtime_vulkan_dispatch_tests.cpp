#include "../src/openxr/RuntimeVulkanDispatch.h"
#include <cstdlib>
#include <iostream>

namespace {
VkInstance instance = reinterpret_cast<VkInstance>(uintptr_t(1));
VkPhysicalDevice downstreamPhysical = reinterpret_cast<VkPhysicalDevice>(uintptr_t(2));
VkPhysicalDevice publicPhysical = reinterpret_cast<VkPhysicalDevice>(uintptr_t(3));
void require(bool ok) { if (!ok) std::abort(); }
VKAPI_ATTR void VKAPI_CALL downstreamMemory(VkPhysicalDevice p, VkPhysicalDeviceMemoryProperties* out) {
    require(p == downstreamPhysical); out->memoryTypeCount = 7;
}
VKAPI_ATTR void VKAPI_CALL publicMemory(VkPhysicalDevice p, VkPhysicalDeviceMemoryProperties* out) {
    require(p == publicPhysical); out->memoryTypeCount = 9;
}
VKAPI_ATTR void VKAPI_CALL createAdapter() {}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL downstream(VkInstance i, const char* n) {
    require(i == instance);
    return kharvox::isPhysicalDeviceCommand(n) ? reinterpret_cast<PFN_vkVoidFunction>(downstreamMemory) : nullptr;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL loader(VkInstance i, const char* n) {
    require(i == instance);
    return kharvox::isPhysicalDeviceCommand(n) ? reinterpret_cast<PFN_vkVoidFunction>(publicMemory) : nullptr;
}
}
int main() {
    using namespace kharvox;
    for (const char* command : {"vkGetPhysicalDeviceMemoryProperties", "vkGetPhysicalDeviceProperties2",
         "vkEnumerateDeviceExtensionProperties", "vkEnumerateDeviceLayerProperties"}) {
        auto sim = resolveRuntimeVulkanProc(instance, command, false, true, downstream, loader, createAdapter);
        require(sim.function == reinterpret_cast<PFN_vkVoidFunction>(downstreamMemory));
        auto existing = resolveRuntimeVulkanProc(instance, command, false, false, downstream, loader, createAdapter);
        require(existing.function == reinterpret_cast<PFN_vkVoidFunction>(publicMemory));
        require(existing.route == RuntimeDispatchRoute::PhysicalLoader);
    }
    VkPhysicalDeviceMemoryProperties props{};
    // Repeat after device creation, when the create adapter is no longer active.
    for (auto adapter : {PFN_vkVoidFunction(createAdapter), PFN_vkVoidFunction(nullptr)}) {
        auto sim = resolveRuntimeVulkanProc(instance, "vkGetPhysicalDeviceMemoryProperties", false, true,
            downstream, loader, adapter);
        reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(sim.function)(downstreamPhysical, &props);
        require(props.memoryTypeCount == 7);
    }
    auto steam = resolveRuntimeVulkanProc(instance, "vkGetPhysicalDeviceMemoryProperties", true, false,
        downstream, loader, nullptr);
    reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(steam.function)(publicPhysical, &props);
    require(props.memoryTypeCount == 9 && steam.route == RuntimeDispatchRoute::SessionLoader);
    require(resolveRuntimeVulkanProc(instance, "vkCreateDevice", false, true,
        downstream, loader, createAdapter).function == createAdapter);
    require(!resolveRuntimeVulkanProc(instance, "vkGetPhysicalDeviceMemoryProperties", false, true,
        nullptr, loader, nullptr).function);
    require(!resolveRuntimeVulkanProc(instance, "vkUnknownCommand", false, true,
        downstream, loader, nullptr).function);
    require(!resolveRuntimeVulkanProc(instance, nullptr, false, true, downstream, loader, nullptr).function);
    std::cout << "Runtime Vulkan dispatch tests passed\n";
}

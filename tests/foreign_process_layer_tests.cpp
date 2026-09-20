#include <windows.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <filesystem>
#include <vector>

namespace {
void require(bool value) { if (!value) std::abort(); }
int instanceKey, deviceKey;
struct Handle { void* dispatch; } instanceHandle{&instanceKey}, physicalHandle{&instanceKey}, deviceHandle{&deviceKey};
const VkInstance instance = reinterpret_cast<VkInstance>(&instanceHandle);
const VkPhysicalDevice physical = reinterpret_cast<VkPhysicalDevice>(&physicalHandle);
const VkDevice device = reinterpret_cast<VkDevice>(&deviceHandle);
const VkInstanceCreateInfo* expectedInstance;
const VkDeviceCreateInfo* expectedDevice;
const VkAllocationCallbacks* expectedAllocator;
VkResult downstreamResult = VK_SUCCESS;
int instanceDestroyed{}, deviceDestroyed{};
VKAPI_ATTR VkResult VKAPI_CALL createInstance(const VkInstanceCreateInfo* info, const VkAllocationCallbacks* allocator, VkInstance* out) {
    require(info == expectedInstance && allocator == expectedAllocator);
    require(*out == instance); // Loader-owned seed must survive layer forwarding.
    require(info->enabledExtensionCount == 0 && info->pApplicationInfo->apiVersion == VK_API_VERSION_1_0);
    if (downstreamResult == VK_SUCCESS) *out = instance;
    return downstreamResult;
}
VKAPI_ATTR VkResult VKAPI_CALL createDevice(VkPhysicalDevice gpu, const VkDeviceCreateInfo* info, const VkAllocationCallbacks* allocator, VkDevice* out) {
    require(gpu == physical && info == expectedDevice && allocator == expectedAllocator);
    require(*out == device); // Do not zero loader-owned storage before the call.
    require(info->enabledExtensionCount == 0);
    if (downstreamResult == VK_SUCCESS) *out = device;
    return downstreamResult;
}
VKAPI_ATTR void VKAPI_CALL destroyInstance(VkInstance value, const VkAllocationCallbacks* allocator) {
    require(value == instance && allocator == expectedAllocator); ++instanceDestroyed;
}
VKAPI_ATTR void VKAPI_CALL destroyDevice(VkDevice value, const VkAllocationCallbacks* allocator) {
    require(value == device && allocator == expectedAllocator); ++deviceDestroyed;
}
VKAPI_ATTR void VKAPI_CALL untouchedCommand() {}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL gipa(VkInstance, const char* name) {
    if (!std::strcmp(name, "vkCreateInstance")) return reinterpret_cast<PFN_vkVoidFunction>(createInstance);
    if (!std::strcmp(name, "vkCreateDevice")) return reinterpret_cast<PFN_vkVoidFunction>(createDevice);
    if (!std::strcmp(name, "vkDestroyInstance")) return reinterpret_cast<PFN_vkVoidFunction>(destroyInstance);
    return reinterpret_cast<PFN_vkVoidFunction>(untouchedCommand);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL gdpa(VkDevice, const char* name) {
    if (!std::strcmp(name, "vkDestroyDevice")) return reinterpret_cast<PFN_vkVoidFunction>(destroyDevice);
    return reinterpret_cast<PFN_vkVoidFunction>(untouchedCommand);
}
}

void realLoaderTest(const char* layerPath) {
    SetEnvironmentVariableA("VK_LAYER_PATH", std::filesystem::path(layerPath).parent_path().string().c_str());
    SetEnvironmentVariableA("KHARVOX_ENABLE_LAYER", "1");
    SetEnvironmentVariableA("KHARVOX_DISABLE_LAYER", nullptr);
    SetEnvironmentVariableA("XR_RUNTIME_JSON", "C:\\missing-kharvox-test-runtime.json");
    auto loader = LoadLibraryA("vulkan-1.dll"); require(loader != nullptr);
    auto getProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
    require(getProc != nullptr);
    auto create = reinterpret_cast<PFN_vkCreateInstance>(getProc(nullptr, "vkCreateInstance"));
    for (bool enable : {false, true}) {
        const char* layerName = "VK_LAYER_KHARVOX_OPENXR";
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.pApplicationName = "Foreign Vulkan game"; app.apiVersion = VK_API_VERSION_1_0;
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; info.pApplicationInfo = &app;
        info.enabledLayerCount = enable ? 1 : 0; info.ppEnabledLayerNames = &layerName;
        VkInstance value{}; require(create(&info, nullptr, &value) == VK_SUCCESS);
        if (enable) require(GetModuleHandleA("KharvoxLayer.dll") != nullptr);
        auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(getProc(value, "vkEnumeratePhysicalDevices"));
        uint32_t count{}; require(enumerate(value, &count, nullptr) == VK_SUCCESS && count > 0);
        std::vector<VkPhysicalDevice> gpus(count); require(enumerate(value, &count, gpus.data()) == VK_SUCCESS);
        auto families = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(getProc(value, "vkGetPhysicalDeviceQueueFamilyProperties"));
        families(gpus[0], &count, nullptr); std::vector<VkQueueFamilyProperties> queues(count); families(gpus[0], &count, queues.data());
        uint32_t family{}; while (family < count && !(queues[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
        require(family < count);
        float priority = 1.f;
        VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; queue.queueFamilyIndex = family; queue.queueCount = 1; queue.pQueuePriorities = &priority;
        VkDeviceCreateInfo gpuInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; gpuInfo.queueCreateInfoCount = 1; gpuInfo.pQueueCreateInfos = &queue;
        auto createGpu = reinterpret_cast<PFN_vkCreateDevice>(getProc(value, "vkCreateDevice"));
        VkDevice gpu{}; require(createGpu(gpus[0], &gpuInfo, nullptr, &gpu) == VK_SUCCESS);
        auto getDevice = reinterpret_cast<PFN_vkGetDeviceProcAddr>(getProc(value, "vkGetDeviceProcAddr"));
        require(reinterpret_cast<PFN_vkDeviceWaitIdle>(getDevice(gpu, "vkDeviceWaitIdle"))(gpu) == VK_SUCCESS);
        reinterpret_cast<PFN_vkDestroyDevice>(getDevice(gpu, "vkDestroyDevice"))(gpu, nullptr);
        reinterpret_cast<PFN_vkDestroyInstance>(getProc(value, "vkDestroyInstance"))(value, nullptr);
        std::cout << (enable ? "Forced KHARVOX layer" : "Baseline") << ": real Vulkan instance/device passed\n";
    }
    FreeLibrary(loader);
}

int main(int argc, char** argv) {
    if (argc == 3 && !std::strcmp(argv[2], "--real-loader")) { realLoaderTest(argv[1]); return 0; }
    require(argc == 2);
    // Even a forcibly enabled/discovered layer must not modify a foreign app.
    SetEnvironmentVariableA("KHARVOX_ENABLE_LAYER", "1");
    SetEnvironmentVariableA("XR_RUNTIME_JSON", "C:\\missing-kharvox-test-runtime.json");
    auto library = LoadLibraryA(argv[1]); require(library != nullptr);
    auto negotiate = reinterpret_cast<PFN_vkNegotiateLoaderLayerInterfaceVersion>(GetProcAddress(library, "vkNegotiateLoaderLayerInterfaceVersion"));
    require(negotiate != nullptr);
    VkNegotiateLayerInterface layerInterface{}; layerInterface.sType = LAYER_NEGOTIATE_INTERFACE_STRUCT; layerInterface.loaderLayerInterfaceVersion = 2;
    require(negotiate(&layerInterface) == VK_SUCCESS);
    auto create = reinterpret_cast<PFN_vkCreateInstance>(layerInterface.pfnGetInstanceProcAddr(nullptr, "vkCreateInstance"));
    VkAllocationCallbacks allocator{}; expectedAllocator = &allocator;
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "DOOM"; // Application metadata must not bypass the EXE check.
    application.apiVersion = VK_API_VERSION_1_0;
    VkLayerInstanceLink instanceLink{}; instanceLink.pfnNextGetInstanceProcAddr = gipa;
    VkLayerInstanceCreateInfo instanceChain{}; instanceChain.sType = VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO;
    instanceChain.function = VK_LAYER_LINK_INFO;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; info.pNext = &instanceChain; info.pApplicationInfo = &application;
    expectedInstance = &info;
    for (auto result : {VK_ERROR_INCOMPATIBLE_DRIVER, VK_SUCCESS}) {
        downstreamResult = result; instanceChain.u.pLayerInfo = &instanceLink;
        VkInstance output=instance; require(create(&info, &allocator, &output) == result);
        require(instanceChain.u.pLayerInfo == nullptr);
        if (result == VK_SUCCESS) require(output == instance);
    }
    require(layerInterface.pfnGetInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR") == reinterpret_cast<PFN_vkVoidFunction>(untouchedCommand));
    auto createGpu = reinterpret_cast<PFN_vkCreateDevice>(layerInterface.pfnGetInstanceProcAddr(instance, "vkCreateDevice"));
    VkLayerDeviceLink deviceLink{}; deviceLink.pfnNextGetInstanceProcAddr = gipa; deviceLink.pfnNextGetDeviceProcAddr = gdpa;
    VkLayerDeviceCreateInfo deviceChain{}; deviceChain.sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO; deviceChain.function = VK_LAYER_LINK_INFO;
    VkDeviceCreateInfo gpuInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; gpuInfo.pNext = &deviceChain; expectedDevice = &gpuInfo;
    for (auto result : {VK_ERROR_FEATURE_NOT_PRESENT, VK_SUCCESS}) {
        downstreamResult = result; deviceChain.u.pLayerInfo = &deviceLink;
        VkDevice output=device; require(createGpu(physical, &gpuInfo, &allocator, &output) == result);
        require(deviceChain.u.pLayerInfo == nullptr);
        if (result == VK_SUCCESS) require(output == device);
    }
    for (auto name : {"vkQueuePresentKHR", "vkQueueSubmit2KHR", "vkQueueWaitIdle", "vkQueueBindSparse", "vkDeviceWaitIdle", "vkQueueBeginDebugUtilsLabelEXT", "vkCreateSwapchainKHR", "vkCmdDraw", "vkDestroyImage"})
        require(layerInterface.pfnGetDeviceProcAddr(device, name) == reinterpret_cast<PFN_vkVoidFunction>(untouchedCommand));
    for (auto name : {"vkQueuePresentKHR", "vkQueueSubmit2KHR", "vkQueueWaitIdle", "vkQueueBindSparse", "vkDeviceWaitIdle"})
        require(layerInterface.pfnGetInstanceProcAddr(instance, name) == reinterpret_cast<PFN_vkVoidFunction>(untouchedCommand));
    reinterpret_cast<PFN_vkDestroyDevice>(layerInterface.pfnGetDeviceProcAddr(device, "vkDestroyDevice"))(device, &allocator);
    reinterpret_cast<PFN_vkDestroyInstance>(layerInterface.pfnGetInstanceProcAddr(instance, "vkDestroyInstance"))(instance, &allocator);
    require(deviceDestroyed == 1 && instanceDestroyed == 1);
    FreeLibrary(library);
    std::cout << "Foreign process: unmodified instance/device creation, dispatch, errors and destruction passed\n";
}

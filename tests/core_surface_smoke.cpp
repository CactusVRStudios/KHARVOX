#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <windows.h>
#include "../src/vulkan/IndependentSurface.h"
#include "../src/vulkan/CoreSurfaceWindow.h"
#include <cstdio>
#include <stdexcept>
#include <vector>

void check(VkResult result,const char* name){if(result!=VK_SUCCESS){std::printf("FAIL %s result=%d\n",name,result);throw std::runtime_error(name);}}
void presentCheck(VkResult result,const char* name){if(result==VK_SUBOPTIMAL_KHR){std::printf("INFO %s SUBOPTIMAL; continuing explicitly scaled swapchain\n",name);return;}check(result,name);}
#define CALL(name, ...) check(name(__VA_ARGS__),#name)
int main(){try{
    auto library=LoadLibraryW(L"vulkan-1.dll");
    auto gipa=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(library,"vkGetInstanceProcAddr"));
    if(!gipa)throw std::runtime_error("Vulkan loader unavailable");
    auto vkCreateInstance=reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr,"vkCreateInstance"));
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};application.pApplicationName="KHARVOX core presentation smoke";application.apiVersion=VK_API_VERSION_1_1;
    const char* extensions[]={VK_KHR_SURFACE_EXTENSION_NAME,VK_KHR_WIN32_SURFACE_EXTENSION_NAME,VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME};
    VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};create.pApplicationInfo=&application;create.enabledExtensionCount=2;create.ppEnabledExtensionNames=extensions;
    VkInstance instance{};CALL(vkCreateInstance,&create,nullptr,&instance);
#define INSTANCE(name) auto name=reinterpret_cast<PFN_##name>(gipa(instance,#name));if(!name)throw std::runtime_error(#name)
    INSTANCE(vkDestroyInstance);INSTANCE(vkEnumeratePhysicalDevices);INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties);INSTANCE(vkGetPhysicalDeviceSurfaceSupportKHR);INSTANCE(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);INSTANCE(vkCreateWin32SurfaceKHR);INSTANCE(vkDestroySurfaceKHR);INSTANCE(vkGetPhysicalDeviceSurfaceFormatsKHR);INSTANCE(vkCreateDevice);INSTANCE(vkGetDeviceProcAddr);
    WNDCLASSW windowClass{};windowClass.lpfnWndProc=DefWindowProcW;windowClass.hInstance=GetModuleHandleW(nullptr);windowClass.lpszClassName=L"KharvoxCoreSurfaceSmoke";RegisterClassW(&windowClass);
    HWND window=CreateWindowW(windowClass.lpszClassName,L"KHARVOX presentation test",WS_POPUP,0,0,640,360,nullptr,nullptr,windowClass.hInstance,nullptr);
    if(!window)throw std::runtime_error("CreateWindow");
    VkWin32SurfaceCreateInfoKHR win32{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};win32.hinstance=windowClass.hInstance;win32.hwnd=window;
    VkSurfaceKHR surface{};CALL(vkCreateWin32SurfaceKHR,instance,&win32,nullptr,&surface);
    uint32_t count{};CALL(vkEnumeratePhysicalDevices,instance,&count,nullptr);std::vector<VkPhysicalDevice> physicals(count);CALL(vkEnumeratePhysicalDevices,instance,&count,physicals.data());
    VkPhysicalDevice physical{};uint32_t family{};
    for(auto candidate:physicals){uint32_t families{};vkGetPhysicalDeviceQueueFamilyProperties(candidate,&families,nullptr);std::vector<VkQueueFamilyProperties> props(families);vkGetPhysicalDeviceQueueFamilyProperties(candidate,&families,props.data());for(uint32_t f=0;f<families;++f){VkBool32 supported{};CALL(vkGetPhysicalDeviceSurfaceSupportKHR,candidate,f,surface,&supported);if(supported&&(props[f].queueFlags&VK_QUEUE_GRAPHICS_BIT)){physical=candidate;family=f;break;}}if(physical)break;}
    if(!physical)throw std::runtime_error("No graphics/present device");
    float priority=1;VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};queueInfo.queueFamilyIndex=family;queueInfo.queueCount=1;queueInfo.pQueuePriorities=&priority;
    const char* deviceExtensions[]={VK_KHR_SWAPCHAIN_EXTENSION_NAME,VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME};
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};deviceInfo.pNext=nullptr;deviceInfo.queueCreateInfoCount=1;deviceInfo.pQueueCreateInfos=&queueInfo;deviceInfo.enabledExtensionCount=1;deviceInfo.ppEnabledExtensionNames=deviceExtensions;
    VkDevice device{};CALL(vkCreateDevice,physical,&deviceInfo,nullptr,&device);
#define DEVICE(name) auto name=reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device,#name));if(!name)throw std::runtime_error(#name)
    DEVICE(vkDestroyDevice);DEVICE(vkGetDeviceQueue);DEVICE(vkCreateSwapchainKHR);DEVICE(vkDestroySwapchainKHR);DEVICE(vkGetSwapchainImagesKHR);DEVICE(vkAcquireNextImageKHR);DEVICE(vkQueuePresentKHR);DEVICE(vkCreateCommandPool);DEVICE(vkDestroyCommandPool);DEVICE(vkAllocateCommandBuffers);DEVICE(vkResetCommandBuffer);DEVICE(vkBeginCommandBuffer);DEVICE(vkEndCommandBuffer);DEVICE(vkCmdPipelineBarrier);DEVICE(vkCmdClearColorImage);DEVICE(vkQueueSubmit);DEVICE(vkQueueWaitIdle);DEVICE(vkCreateSemaphore);DEVICE(vkDestroySemaphore);DEVICE(vkCreateFence);DEVICE(vkDestroyFence);DEVICE(vkWaitForFences);DEVICE(vkResetFences);
    VkQueue queue{};vkGetDeviceQueue(device,family,0,&queue);
    const auto source=kharvox::independentSourceExtent(3060,3264,1.f,16384);
    if(!kharvox::matchCoreSurfaceWindow(window,source))throw std::runtime_error("Core window size");
    VkSurfaceCapabilitiesKHR actual{};CALL(vkGetPhysicalDeviceSurfaceCapabilitiesKHR,physical,surface,&actual);
    if(actual.currentExtent.width!=source.width||actual.currentExtent.height!=source.height)throw std::runtime_error("Core WSI extent differs");
    CALL(vkGetPhysicalDeviceSurfaceFormatsKHR,physical,surface,&count,nullptr);std::vector<VkSurfaceFormatKHR> formats(count);CALL(vkGetPhysicalDeviceSurfaceFormatsKHR,physical,surface,&count,formats.data());
    VkSwapchainCreateInfoKHR swapInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};swapInfo.pNext=nullptr;swapInfo.surface=surface;swapInfo.minImageCount=actual.minImageCount;swapInfo.imageFormat=formats[0].format;swapInfo.imageColorSpace=formats[0].colorSpace;swapInfo.imageExtent=source;swapInfo.imageArrayLayers=1;swapInfo.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;swapInfo.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;swapInfo.preTransform=actual.currentTransform;swapInfo.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;swapInfo.presentMode=VK_PRESENT_MODE_FIFO_KHR;swapInfo.clipped=VK_TRUE;
    VkSwapchainKHR swapchain{};CALL(vkCreateSwapchainKHR,device,&swapInfo,nullptr,&swapchain);
    CALL(vkGetSwapchainImagesKHR,device,swapchain,&count,nullptr);std::vector<VkImage> images(count);CALL(vkGetSwapchainImagesKHR,device,swapchain,&count,images.data());
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};poolInfo.queueFamilyIndex=family;poolInfo.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool{};CALL(vkCreateCommandPool,device,&poolInfo,nullptr,&pool);
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};allocation.commandPool=pool;allocation.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;allocation.commandBufferCount=1;
    VkCommandBuffer commands{};CALL(vkAllocateCommandBuffers,device,&allocation,&commands);
    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};VkSemaphore available{},ready{};CALL(vkCreateSemaphore,device,&semaphoreInfo,nullptr,&available);CALL(vkCreateSemaphore,device,&semaphoreInfo,nullptr,&ready);
    for(auto size:{VkExtent2D{640,360},VkExtent2D{1280,720},VkExtent2D{1920,1080},VkExtent2D{2560,1440}}){
        SetWindowPos(window,nullptr,0,0,size.width,size.height,SWP_NOACTIVATE|SWP_NOZORDER);MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
        if(!kharvox::matchCoreSurfaceWindow(window,source))throw std::runtime_error("Core resize failed");
        // WSI may require recreation after a native window change. The render
        // extent must remain headset-sized across every such recreation.
        swapInfo.oldSwapchain=swapchain;VkSwapchainKHR replacement{};CALL(vkCreateSwapchainKHR,device,&swapInfo,nullptr,&replacement);
        vkDestroySwapchainKHR(device,swapchain,nullptr);swapchain=replacement;
        CALL(vkGetSwapchainImagesKHR,device,swapchain,&count,nullptr);images.resize(count);CALL(vkGetSwapchainImagesKHR,device,swapchain,&count,images.data());
        for(int frame=0;frame<3;++frame){
            uint32_t index{};presentCheck(vkAcquireNextImageKHR(device,swapchain,UINT64_MAX,available,VK_NULL_HANDLE,&index),"acquire");
            CALL(vkResetCommandBuffer,commands,0);VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};CALL(vkBeginCommandBuffer,commands,&begin);
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.image=images[index];barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(commands,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
            VkClearColorValue color{{0.05f,0.1f,0.2f,1.f}};vkCmdClearColorImage(commands,images[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&color,1,&barrier.subresourceRange);
            barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=0;
            vkCmdPipelineBarrier(commands,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&barrier);CALL(vkEndCommandBuffer,commands);
            VkPipelineStageFlags waitStage=VK_PIPELINE_STAGE_TRANSFER_BIT;VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.waitSemaphoreCount=1;submit.pWaitSemaphores=&available;submit.pWaitDstStageMask=&waitStage;submit.commandBufferCount=1;submit.pCommandBuffers=&commands;submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&ready;CALL(vkQueueSubmit,queue,1,&submit,VK_NULL_HANDLE);
            VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};present.pNext=nullptr;present.waitSemaphoreCount=1;present.pWaitSemaphores=&ready;present.swapchainCount=1;present.pSwapchains=&swapchain;present.pImageIndices=&index;presentCheck(vkQueuePresentKHR(queue,&present),"present");
            CALL(vkQueueWaitIdle,queue);
        }
        RECT client{};GetClientRect(window,&client);std::printf("PASS source=%ux%u client=%ldx%ld frames=3 render size preserved across recreation\n",source.width,source.height,client.right,client.bottom);
    }
    vkDestroySemaphore(device,ready,nullptr);vkDestroySemaphore(device,available,nullptr);vkDestroyCommandPool(device,pool,nullptr);vkDestroySwapchainKHR(device,swapchain,nullptr);vkDestroyDevice(device,nullptr);vkDestroySurfaceKHR(instance,surface,nullptr);vkDestroyInstance(instance,nullptr);DestroyWindow(window);FreeLibrary(library);return 0;
}catch(const std::exception& error){std::printf("FAILED: %s\n",error.what());return 1;}}

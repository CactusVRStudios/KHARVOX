#include "../src/hands/HandSceneDepthCopy.h"
#include <cstring>
#include <windows.h>
#include <array>
#include <vector>
#include <stdexcept>
#include <iostream>
#include <cmath>

static void check(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
static void ok(VkResult value){check(value==VK_SUCCESS,"Vulkan operation failed");}
#ifndef KHARVOX_TEST_DEPTH_LAYER
#define KHARVOX_TEST_DEPTH_LAYER 0
#endif
constexpr uint32_t sourceLayer=KHARVOX_TEST_DEPTH_LAYER;
int main(){try{
    auto loader=LoadLibraryW(L"vulkan-1.dll");check(loader,"Vulkan loader unavailable");
    auto gipa=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader,"vkGetInstanceProcAddr"));
    auto createInstance=reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr,"vkCreateInstance"));
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ici.pApplicationInfo=&app;
    VkInstance instance{};ok(createInstance(&ici,nullptr,&instance));
#define INSTANCE(name) auto name=reinterpret_cast<PFN_##name>(gipa(instance,#name));check(name,#name)
    INSTANCE(vkEnumeratePhysicalDevices);INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties);
    INSTANCE(vkGetPhysicalDeviceMemoryProperties);INSTANCE(vkGetPhysicalDeviceProperties);
    INSTANCE(vkCreateDevice);INSTANCE(vkDestroyInstance);INSTANCE(vkGetDeviceProcAddr);
    uint32_t count{};ok(vkEnumeratePhysicalDevices(instance,&count,nullptr));check(count,"No Vulkan GPU");
    std::vector<VkPhysicalDevice> physicals(count);ok(vkEnumeratePhysicalDevices(instance,&count,physicals.data()));
    VkPhysicalDevice physical{};uint32_t family=UINT32_MAX;
    for(auto candidate:physicals){
        uint32_t n{};vkGetPhysicalDeviceQueueFamilyProperties(candidate,&n,nullptr);
        std::vector<VkQueueFamilyProperties> props(n);vkGetPhysicalDeviceQueueFamilyProperties(candidate,&n,props.data());
        for(uint32_t i=0;i<n;++i)if(props[i].queueFlags&VK_QUEUE_GRAPHICS_BIT){physical=candidate;family=i;break;}
        if(physical)break;
    }
    check(physical,"No graphics queue");
    VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(physical,&props);std::cout<<props.deviceName<<'\n';
    float priority=1;VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qci.queueFamilyIndex=family;qci.queueCount=1;qci.pQueuePriorities=&priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dci.queueCreateInfoCount=1;dci.pQueueCreateInfos=&qci;
    VkDevice device{};ok(vkCreateDevice(physical,&dci,nullptr,&device));
#define DEVICE(name) auto name=reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device,#name));check(name,#name)
    DEVICE(vkGetDeviceQueue);DEVICE(vkDestroyDevice);DEVICE(vkCreateCommandPool);DEVICE(vkDestroyCommandPool);
    DEVICE(vkAllocateCommandBuffers);DEVICE(vkResetCommandBuffer);DEVICE(vkBeginCommandBuffer);DEVICE(vkEndCommandBuffer);
    DEVICE(vkQueueSubmit);DEVICE(vkQueueWaitIdle);DEVICE(vkCreateBuffer);DEVICE(vkDestroyBuffer);
    DEVICE(vkGetBufferMemoryRequirements);DEVICE(vkAllocateMemory);DEVICE(vkFreeMemory);DEVICE(vkBindBufferMemory);
    DEVICE(vkMapMemory);DEVICE(vkUnmapMemory);DEVICE(vkCreateImage);DEVICE(vkDestroyImage);
    DEVICE(vkGetImageMemoryRequirements);DEVICE(vkBindImageMemory);DEVICE(vkCmdPipelineBarrier);
    DEVICE(vkCmdClearDepthStencilImage);DEVICE(vkCmdCopyImageToBuffer);DEVICE(vkCmdCopyImage);
    struct {PFN_vkCmdPipelineBarrier cmdPipelineBarrier;PFN_vkCmdCopyImage cmdCopyImage;} dispatch{vkCmdPipelineBarrier,vkCmdCopyImage};
    VkQueue queue{};vkGetDeviceQueue(device,family,0,&queue);
    VkPhysicalDeviceMemoryProperties memoryProps{};vkGetPhysicalDeviceMemoryProperties(physical,&memoryProps);
    auto memoryType=[&](uint32_t bits,VkMemoryPropertyFlags flags){for(uint32_t i=0;i<memoryProps.memoryTypeCount;++i)if((bits&(1u<<i))&&(memoryProps.memoryTypes[i].propertyFlags&flags)==flags)return i;throw std::runtime_error("No memory type");};
    constexpr uint32_t pixels=64,groupBytes=pixels*9;
    std::array<VkImage,2> images{};std::array<VkDeviceMemory,2> imageMemory{};
    for(size_t i=0;i<images.size();++i){
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=VK_FORMAT_D24_UNORM_S8_UINT;
        ci.extent={8,8,1};ci.mipLevels=1;ci.arrayLayers=i==0?sourceLayer+1:1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;
        ci.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ok(vkCreateImage(device,&ci,nullptr,&images[i]));VkMemoryRequirements req{};vkGetImageMemoryRequirements(device,images[i],&req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=memoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        ok(vkAllocateMemory(device,&ai,nullptr,&imageMemory[i]));ok(vkBindImageMemory(device,images[i],imageMemory[i],0));
    }
    VkBuffer buffer{};VkDeviceMemory bufferMemory{};
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=groupBytes*2;bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    ok(vkCreateBuffer(device,&bi,nullptr,&buffer));VkMemoryRequirements req{};vkGetBufferMemoryRequirements(device,buffer,&req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=memoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    ok(vkAllocateMemory(device,&ai,nullptr,&bufferMemory));ok(vkBindBufferMemory(device,buffer,bufferMemory,0));
    VkCommandPool pool{};VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pi.queueFamilyIndex=family;ok(vkCreateCommandPool(device,&pi,nullptr,&pool));
    VkCommandBuffer cb{};VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;ok(vkAllocateCommandBuffers(device,&ca,&cb));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};ok(vkBeginCommandBuffer(cb,&begin));
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,0,1,0,1};
    auto transition=[&](VkImage image,VkImageLayout from,VkImageLayout to){
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=image;b.oldLayout=from;b.newLayout=to;b.subresourceRange=range;
        if(image==images[0])b.subresourceRange.layerCount=sourceLayer+1;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.srcAccessMask=from==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
    };
    transition(images[0],VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearDepthStencilValue clear{0.75f,0x33};vkCmdClearDepthStencilImage(cb,images[0],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear,1,&range);
    auto sourceRange=range;sourceRange.baseArrayLayer=sourceLayer;
    clear={0.25f,0x5a};vkCmdClearDepthStencilImage(cb,images[0],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear,1,&sourceRange);
    transition(images[0],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    for(uint32_t iteration=0;iteration<2;++iteration){
        if(iteration){
            transition(images[1],VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkClearDepthStencilValue overwrite{0.75f,0};vkCmdClearDepthStencilImage(cb,images[1],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&overwrite,1,&range);
            transition(images[1],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        }
        kharvox::hands::copyHandSceneDepth(dispatch,cb,images[0],images[1],{8,8},range.aspectMask,iteration!=0,sourceLayer);
        for(uint32_t i=0;i<2;++i){
            transition(images[i],VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy copy{};copy.bufferOffset=iteration*groupBytes+i*pixels*4;copy.imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};copy.imageExtent={8,8,1};
            if(i==0)copy.imageSubresource.baseArrayLayer=sourceLayer;
            vkCmdCopyImageToBuffer(cb,images[i],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&copy);
            if(i==0){copy.bufferOffset=iteration*groupBytes+pixels*8;copy.imageSubresource.aspectMask=VK_IMAGE_ASPECT_STENCIL_BIT;vkCmdCopyImageToBuffer(cb,images[i],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&copy);}
            transition(images[i],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        }
    }
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
    ok(vkEndCommandBuffer(cb));VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&cb;ok(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));ok(vkQueueWaitIdle(queue));
    void* mapped{};ok(vkMapMemory(device,bufferMemory,0,VK_WHOLE_SIZE,0,&mapped));auto bytes=static_cast<const unsigned char*>(mapped);
    // D24's unused byte is unspecified. Compare only the 24 depth bits.
    auto depth=[&](size_t offset){uint32_t value{};std::memcpy(&value,bytes+offset,4);return value&0xffffffu;};
    for(uint32_t i=0;i<pixels;++i){
        const auto reference=depth(i*4);check(reference>0&&reference<0xffffffu,"Source depth unexpectedly empty");
        check(reference==depth(pixels*4+i*4)&&reference==depth(groupBytes+i*4)&&reference==depth(groupBytes+pixels*4+i*4),"Depth copy/refresh mismatch");
        check(bytes[pixels*8+i]==0x5a&&bytes[groupBytes+pixels*8+i]==0x5a,"Source stencil modified");
    }
    vkUnmapMemory(device,bufferMemory);vkDestroyCommandPool(device,pool,nullptr);vkDestroyBuffer(device,buffer,nullptr);vkFreeMemory(device,bufferMemory,nullptr);
    for(size_t i=0;i<images.size();++i){vkDestroyImage(device,images[i],nullptr);vkFreeMemory(device,imageMemory[i],nullptr);}
    vkDestroyDevice(device,nullptr);vkDestroyInstance(instance,nullptr);FreeLibrary(loader);
    std::cout<<"D24S8 private hand depth copy/refresh preserves source depth and stencil\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

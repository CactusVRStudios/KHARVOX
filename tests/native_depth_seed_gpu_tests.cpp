#include <vulkan/vulkan.h>
#include "../src/native/NativeDepthSeedPolicy.h"
#include <cstring>
#include <windows.h>
#include <array>
#include <vector>
#include <stdexcept>
#include <iostream>
#include <cmath>

static void check(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
static void ok(VkResult value){check(value==VK_SUCCESS,"Vulkan operation failed");}
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
    DEVICE(vkCreateRenderPass);DEVICE(vkDestroyRenderPass);DEVICE(vkCreateImageView);DEVICE(vkDestroyImageView);
    DEVICE(vkCreateFramebuffer);DEVICE(vkDestroyFramebuffer);DEVICE(vkCmdBeginRenderPass);DEVICE(vkCmdEndRenderPass);
    VkQueue queue{};vkGetDeviceQueue(device,family,0,&queue);
    VkPhysicalDeviceMemoryProperties memoryProps{};vkGetPhysicalDeviceMemoryProperties(physical,&memoryProps);
    auto memoryType=[&](uint32_t bits,VkMemoryPropertyFlags flags){for(uint32_t i=0;i<memoryProps.memoryTypeCount;++i)if((bits&(1u<<i))&&(memoryProps.memoryTypes[i].propertyFlags&flags)==flags)return i;throw std::runtime_error("No memory type");};
    constexpr uint32_t pixels=64,groupBytes=pixels*9;
    std::array<VkImage,2> images{};std::array<VkDeviceMemory,2> imageMemory{};
    for(size_t i=0;i<images.size();++i){
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=VK_FORMAT_D24_UNORM_S8_UINT;
        ci.extent={8,8,1};ci.mipLevels=1;ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;
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
    VkCommandPool pool{};VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;pi.queueFamilyIndex=family;ok(vkCreateCommandPool(device,&pi,nullptr,&pool));
    std::array<VkCommandBuffer,2> commands{};
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=2;ok(vkAllocateCommandBuffers(device,&ca,commands.data()));
    VkAttachmentDescription attachment{};attachment.format=VK_FORMAT_D24_UNORM_S8_UINT;attachment.samples=VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp=attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;attachment.storeOp=attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE;
    attachment.initialLayout=attachment.finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference depthReference{0,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.pDepthStencilAttachment=&depthReference;
    VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};passInfo.attachmentCount=1;passInfo.pAttachments=&attachment;passInfo.subpassCount=1;passInfo.pSubpasses=&sub;
    VkRenderPass pass{};ok(vkCreateRenderPass(device,&passInfo,nullptr,&pass));
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,0,1,0,1};
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};viewInfo.image=images[1];viewInfo.viewType=VK_IMAGE_VIEW_TYPE_2D;viewInfo.format=attachment.format;viewInfo.subresourceRange=range;
    VkImageView view{};ok(vkCreateImageView(device,&viewInfo,nullptr,&view));
    VkFramebufferCreateInfo fbInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fbInfo.renderPass=pass;fbInfo.attachmentCount=1;fbInfo.pAttachments=&view;fbInfo.width=fbInfo.height=8;fbInfo.layers=1;
    VkFramebuffer fb{};ok(vkCreateFramebuffer(device,&fbInfo,nullptr,&fb));
    auto transition=[&](VkCommandBuffer cb,VkImage image,VkImageLayout old,VkImageLayout next){
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=image;b.oldLayout=old;b.newLayout=next;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange=range;
        b.srcAccessMask=old==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
    };
    for(uint32_t fixed=0;fixed<2;++fixed){
        for(auto cb:commands){if(fixed)ok(vkResetCommandBuffer(cb,0));VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};ok(vkBeginCommandBuffer(cb,&bi));}
        const auto early=commands[0],late=commands[1];
        transition(early,images[0],fixed?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearDepthStencilValue left{0.25f,0x5a};vkCmdClearDepthStencilImage(early,images[0],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&left,1,&range);
        transition(early,images[0],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        transition(early,images[1],fixed?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        // CPU order: left producer, seed in LAST command, then right producer
        // appended to EARLIER command. This is the game's multi-buffer shape.
        const bool omit=fixed&&kharvox::native::sceneDepthClearsItself(true,true,true,true);
        if(!omit){
            transition(late,images[1],VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageCopy copy{};copy.srcSubresource=copy.dstSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};copy.extent={8,8,1};
            vkCmdCopyImage(late,images[0],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,images[1],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
            transition(late,images[1],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        }
        VkClearValue right{};right.depthStencil={0.75f,0x3c};
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};begin.renderPass=pass;begin.framebuffer=fb;begin.renderArea.extent={8,8};begin.clearValueCount=1;begin.pClearValues=&right;
        vkCmdBeginRenderPass(early,&begin,VK_SUBPASS_CONTENTS_INLINE);vkCmdEndRenderPass(early);
        ok(vkEndCommandBuffer(early));
        transition(late,images[1],VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy read{};read.bufferOffset=fixed*pixels*4;read.imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};read.imageExtent={8,8,1};
        vkCmdCopyImageToBuffer(late,images[1],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&read);
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(late,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
        ok(vkEndCommandBuffer(late));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=2;submit.pCommandBuffers=commands.data();
        ok(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));ok(vkQueueWaitIdle(queue));
    }
    void* mapped{};ok(vkMapMemory(device,bufferMemory,0,VK_WHOLE_SIZE,0,&mapped));
    const auto* bytes=static_cast<const unsigned char*>(mapped);
    for(uint32_t i=0;i<pixels;++i){
        uint32_t old{},fixed{};std::memcpy(&old,bytes+i*4,4);std::memcpy(&fixed,bytes+pixels*4+i*4,4);
        const double a=double(old&0xffffff)/0xffffff,b=double(fixed&0xffffff)/0xffffff;
        check(std::abs(a-0.25)<0.00001,"old multi-command seed did not reproduce left-depth overwrite");
        check(std::abs(b-0.75)<0.00001,"right depth was overwritten despite full-clear seed omission");
    }
    for(int missing=0;missing<4;++missing){std::array<bool,4> p{true,true,true,true};p[missing]=false;check(!kharvox::native::sceneDepthClearsItself(p[0],p[1],p[2],p[3]),"shadow, partial or LOAD attachment incorrectly skips seed");}
    vkUnmapMemory(device,bufferMemory);vkDestroyFramebuffer(device,fb,nullptr);vkDestroyImageView(device,view,nullptr);vkDestroyRenderPass(device,pass,nullptr);
    vkDestroyCommandPool(device,pool,nullptr);vkDestroyBuffer(device,buffer,nullptr);vkFreeMemory(device,bufferMemory,nullptr);
    for(size_t i=0;i<images.size();++i){vkDestroyImage(device,images[i],nullptr);vkFreeMemory(device,imageMemory[i],nullptr);}
    vkDestroyDevice(device,nullptr);vkDestroyInstance(instance,nullptr);FreeLibrary(loader);
    std::cout<<"GPU reproduced late left-depth overwrite; full-clear omission preserves right depth\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

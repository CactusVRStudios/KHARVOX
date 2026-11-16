#include "../src/hands/HandRenderer.h"
#include "../src/hands/WeaponSupportConfig.h"
#include "../src/hands/WeaponSupportPolicy.h"
#include "../src/hud/OffhandHudPolicy.h"
#include "../src/openxr/OpenXRBootstrap.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <set>
#include <vector>

// Only game-state probes are stubbed. GLBs, morph data, shaders, the renderer,
// Vulkan driver and both render paths are the production implementations.
bool KharvoxHudOffhandCalibrationActive(){return false;}
std::array<bool,2> publishedFreeBounds{};
void KharvoxHudSetFreeHandBounds(bool left,const kharvox::OffhandHudBounds& bounds,bool valid){
    if(valid){
        for(int i=0;i<3;++i)if(!std::isfinite(bounds.minimum[i])||!std::isfinite(bounds.maximum[i])
            ||bounds.minimum[i]>=bounds.maximum[i])throw std::runtime_error("Invalid production free-hand HUD envelope");
        if(!bounds.sectionMask)throw std::runtime_error("Missing production HUD mesh sections");
        for(unsigned s=0;s<8;++s)if(bounds.sectionMask&(1u<<s))for(int i=0;i<3;++i){
            const auto& part=bounds.sections[s];
            if(!std::isfinite(part[i])||!std::isfinite(part[3+i])||part[i]>part[3+i]
                ||part[i]<bounds.minimum[i]-.001f||part[3+i]>bounds.maximum[i]+.001f)
                throw std::runtime_error("Invalid HUD section or section outside aggregate armor bounds");
        }
        publishedFreeBounds[left?0:1]=true;
    }
}
unsigned publishedHudTemplates{};
void KharvoxHudSetSupportGeometry(unsigned,bool,const kharvox::hands::SupportHudGeometry& geometry){
    if(geometry.valid){
        if(!geometry.hudSectionMask)throw std::runtime_error("Missing locked-hand HUD mesh sections");
        ++publishedHudTemplates;
        auto weapon=kharvox::hands::math::identity();
        float origin[3]{},axis[9]{};
        if(!kharvox::hands::supportHudFrame(weapon,geometry,39.3701f,origin,axis))
            throw std::runtime_error("Published support HUD wrist has invalid physical units or handedness");
    }
}
const char* KharvoxWeaponKindKey(KharvoxWeaponKind kind){
    static const char*names[]{"unknown","pistol","shotgun","heavy_assault_rifle","plasma_rifle",
        "rocket_launcher","super_shotgun","gauss_cannon","chaingun","bfg","chainsaw",
        "fists","assault_rifle","arc_cannon","mancubus_gland"};
    const auto i=static_cast<unsigned>(kind);return i<15?names[i]:names[0];
}
namespace {
void check(bool value,const char*reason){if(!value)throw std::runtime_error(reason);}
void ok(VkResult result){check(result==VK_SUCCESS,"Vulkan call failed");}
PFN_vkCreateImage nativeCreateImage{};
PFN_vkDestroyImage nativeDestroyImage{};
std::set<VkImage> sampledImages;
unsigned sampledImagesCreated{},sampledImagesDestroyed{};
PFN_vkAllocateMemory nativeAllocateMemory{};
PFN_vkFreeMemory nativeFreeMemory{};
std::map<VkDeviceMemory,VkDeviceSize> allocations;
VkDeviceSize allocatedBytes{},peakAllocatedBytes{};
unsigned allocationsCreated{};
VkResult VKAPI_CALL countAllocateMemory(VkDevice device,const VkMemoryAllocateInfo*info,
    const VkAllocationCallbacks*allocator,VkDeviceMemory*memory){
    const auto result=nativeAllocateMemory(device,info,allocator,memory);
    if(result==VK_SUCCESS){allocations.emplace(*memory,info->allocationSize);
        allocatedBytes+=info->allocationSize;peakAllocatedBytes=std::max(peakAllocatedBytes,allocatedBytes);
        ++allocationsCreated;}
    return result;
}
void VKAPI_CALL countFreeMemory(VkDevice device,VkDeviceMemory memory,const VkAllocationCallbacks*allocator){
    const auto found=allocations.find(memory);
    if(found!=allocations.end()){allocatedBytes-=found->second;allocations.erase(found);}
    nativeFreeMemory(device,memory,allocator);
}
VkResult VKAPI_CALL countCreateImage(VkDevice device,const VkImageCreateInfo*info,
    const VkAllocationCallbacks*allocator,VkImage*image){
    const auto result=nativeCreateImage(device,info,allocator,image);
    if(result==VK_SUCCESS&&(info->usage&VK_IMAGE_USAGE_SAMPLED_BIT)){
        sampledImages.insert(*image);++sampledImagesCreated;
    }
    return result;
}
void VKAPI_CALL countDestroyImage(VkDevice device,VkImage image,const VkAllocationCallbacks*allocator){
    if(sampledImages.erase(image))++sampledImagesDestroyed;
    nativeDestroyImage(device,image,allocator);
}
}
int main(int argc,char**argv){try{
    check(argc>=2,"Expected runtime directory [--expect-shared-atlas] [--benchmark] [--extent=1536x1536]");
    bool expectShared=false,benchmark=false,expectSupport=false,expectPrimary=false,expectOneHandPrimary=false,expectGrenadeVisual=false;uint32_t width=256,height=256;
    unsigned expectedSupportCount=9;
    bool explicitExtent=false;
    for(int i=2;i<argc;++i){
        const std::string argument=argv[i];
        if(argument=="--expect-shared-atlas")expectShared=true;
        else if(argument=="--expect-grenade-visual")expectGrenadeVisual=true;
        else if(argument=="--expect-support-profiles")expectSupport=true;
        else if(argument=="--expect-two-pump-profiles"){expectSupport=true;expectedSupportCount=2;}
        else if(argument=="--expect-manual-primary")expectPrimary=true;
        else if(argument=="--expect-one-hand-primary"){expectPrimary=true;expectOneHandPrimary=true;}
        else if(argument=="--benchmark")benchmark=true;
        else if(argument.rfind("--extent=",0)==0){
            const auto extent=argument.substr(9);const auto split=extent.find('x');
            check(split!=std::string::npos,"Extent must use WIDTHxHEIGHT");
            size_t usedWidth{},usedHeight{};
            const auto w=std::stoul(extent.substr(0,split),&usedWidth);
            const auto h=std::stoul(extent.substr(split+1),&usedHeight);
            check(usedWidth==split&&usedHeight==extent.size()-split-1
                &&w>=64&&w<=8192&&h>=64&&h<=8192,"Extent must be between 64 and 8192 per dimension");
            width=static_cast<uint32_t>(w);height=static_cast<uint32_t>(h);explicitExtent=true;
        }else check(false,"Unknown hand GPU test argument");
    }
    check(!explicitExtent||benchmark,"--extent requires --benchmark");
    if(benchmark&&!explicitExtent)width=height=1536;
    const auto root=std::filesystem::absolute(argv[1]);
    SetEnvironmentVariableA("KHARVOX_HAND_CALIBRATION","off");
    const auto loader=LoadLibraryW(L"vulkan-1.dll");check(loader,"Vulkan loader unavailable");
    const auto gipa=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader,"vkGetInstanceProcAddr"));
    const auto create=reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr,"vkCreateInstance"));
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ici.pApplicationInfo=&app;
    VkInstance instance{};ok(create(&ici,nullptr,&instance));
#define INSTANCE(name) const auto name=reinterpret_cast<PFN_##name>(gipa(instance,#name))
    INSTANCE(vkEnumeratePhysicalDevices);INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties);
    INSTANCE(vkGetPhysicalDeviceMemoryProperties);INSTANCE(vkGetPhysicalDeviceProperties);
    INSTANCE(vkCreateDevice);INSTANCE(vkGetDeviceProcAddr);INSTANCE(vkDestroyInstance);
    uint32_t count{};ok(vkEnumeratePhysicalDevices(instance,&count,nullptr));check(count>0,"No Vulkan GPU");
    std::vector<VkPhysicalDevice> devices(count);ok(vkEnumeratePhysicalDevices(instance,&count,devices.data()));
    const auto physical=devices[0];vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,nullptr);
    std::vector<VkQueueFamilyProperties> queues(count);vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,queues.data());
    uint32_t family{};while(family<count&&!(queues[family].queueFlags&VK_QUEUE_GRAPHICS_BIT))++family;
    check(family<count,"No graphics queue");
    VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(physical,&properties);
    std::cout<<properties.deviceName<<'\n';
    float priority=1;VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex=family;qci.queueCount=1;qci.pQueuePriorities=&priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dci.queueCreateInfoCount=1;dci.pQueueCreateInfos=&qci;
    VkDevice device{};ok(vkCreateDevice(physical,&dci,nullptr,&device));
#define DEVICE(name) const auto name=reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device,#name))
    DEVICE(vkDestroyDevice);DEVICE(vkGetDeviceQueue);DEVICE(vkCmdClearColorImage);
    DEVICE(vkCmdClearDepthStencilImage);DEVICE(vkCmdCopyImageToBuffer);
    DEVICE(vkCreateQueryPool);DEVICE(vkDestroyQueryPool);DEVICE(vkCmdResetQueryPool);
    DEVICE(vkCmdWriteTimestamp);DEVICE(vkGetQueryPoolResults);
    KharvoxVulkanDispatch d{};
#define LOAD(field,name) d.field=reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device,#name));check(d.field,#name)
    LOAD(createCommandPool,vkCreateCommandPool);LOAD(destroyCommandPool,vkDestroyCommandPool);
    LOAD(allocateCommandBuffers,vkAllocateCommandBuffers);LOAD(resetCommandBuffer,vkResetCommandBuffer);
    LOAD(beginCommandBuffer,vkBeginCommandBuffer);LOAD(endCommandBuffer,vkEndCommandBuffer);
    LOAD(queueSubmit,vkQueueSubmit);LOAD(queueWaitIdle,vkQueueWaitIdle);
    LOAD(createBuffer,vkCreateBuffer);LOAD(destroyBuffer,vkDestroyBuffer);
    LOAD(getBufferMemoryRequirements,vkGetBufferMemoryRequirements);LOAD(allocateMemory,vkAllocateMemory);
    LOAD(freeMemory,vkFreeMemory);LOAD(bindBufferMemory,vkBindBufferMemory);
    nativeAllocateMemory=d.allocateMemory;nativeFreeMemory=d.freeMemory;
    d.allocateMemory=countAllocateMemory;d.freeMemory=countFreeMemory;
    LOAD(mapMemory,vkMapMemory);LOAD(unmapMemory,vkUnmapMemory);
    LOAD(createImage,vkCreateImage);LOAD(destroyImage,vkDestroyImage);
    nativeCreateImage=d.createImage;nativeDestroyImage=d.destroyImage;
    d.createImage=countCreateImage;d.destroyImage=countDestroyImage;
    LOAD(getImageMemoryRequirements,vkGetImageMemoryRequirements);LOAD(bindImageMemory,vkBindImageMemory);
    LOAD(createImageView,vkCreateImageView);LOAD(destroyImageView,vkDestroyImageView);
    LOAD(createSampler,vkCreateSampler);LOAD(destroySampler,vkDestroySampler);
    LOAD(createShaderModule,vkCreateShaderModule);LOAD(destroyShaderModule,vkDestroyShaderModule);
    LOAD(createDescriptorSetLayout,vkCreateDescriptorSetLayout);LOAD(destroyDescriptorSetLayout,vkDestroyDescriptorSetLayout);
    LOAD(createDescriptorPool,vkCreateDescriptorPool);LOAD(destroyDescriptorPool,vkDestroyDescriptorPool);
    LOAD(allocateDescriptorSets,vkAllocateDescriptorSets);LOAD(updateDescriptorSets,vkUpdateDescriptorSets);
    LOAD(createPipelineLayout,vkCreatePipelineLayout);LOAD(destroyPipelineLayout,vkDestroyPipelineLayout);
    LOAD(createGraphicsPipelines,vkCreateGraphicsPipelines);LOAD(destroyPipeline,vkDestroyPipeline);
    LOAD(createRenderPass,vkCreateRenderPass);LOAD(destroyRenderPass,vkDestroyRenderPass);
    LOAD(createFramebuffer,vkCreateFramebuffer);LOAD(destroyFramebuffer,vkDestroyFramebuffer);
    LOAD(cmdPipelineBarrier,vkCmdPipelineBarrier);LOAD(cmdCopyBufferToImage,vkCmdCopyBufferToImage);
    LOAD(cmdCopyImage,vkCmdCopyImage);LOAD(cmdBeginRenderPass,vkCmdBeginRenderPass);
    LOAD(cmdEndRenderPass,vkCmdEndRenderPass);LOAD(cmdBindPipeline,vkCmdBindPipeline);
    LOAD(cmdBindDescriptorSets,vkCmdBindDescriptorSets);LOAD(cmdBindVertexBuffers,vkCmdBindVertexBuffers);
    LOAD(cmdBindIndexBuffer,vkCmdBindIndexBuffer);LOAD(cmdDrawIndexed,vkCmdDrawIndexed);
    LOAD(cmdPushConstants,vkCmdPushConstants);LOAD(cmdSetViewport,vkCmdSetViewport);LOAD(cmdSetScissor,vkCmdSetScissor);
    d.getPhysicalDeviceMemoryProperties=vkGetPhysicalDeviceMemoryProperties;
    VkQueue queue{};vkGetDeviceQueue(device,family,0,&queue);
    VkPhysicalDeviceMemoryProperties memory{};vkGetPhysicalDeviceMemoryProperties(physical,&memory);
    auto memoryType=[&](uint32_t bits,VkMemoryPropertyFlags flags){
        for(uint32_t i=0;i<memory.memoryTypeCount;++i)
            if((bits&(1u<<i))&&(memory.memoryTypes[i].propertyFlags&flags)==flags)return i;
        throw std::runtime_error("No memory type");};
    const VkDeviceSize pixelBytes=VkDeviceSize(width)*height*4;
    std::array<VkImage,5> images{};std::array<VkDeviceMemory,5> imageMemory{};std::array<VkImageView,5> views{};
    for(int i=0;i<5;++i){
        const bool depth=i==4;
        const auto format=depth?VK_FORMAT_D32_SFLOAT:i<2?VK_FORMAT_R8G8B8A8_SRGB:VK_FORMAT_R8G8B8A8_UNORM;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=format;
        ci.extent={width,height,1};ci.mipLevels=ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;
        ci.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT
            |(depth?VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT:VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        ok(d.createImage(device,&ci,nullptr,&images[i]));VkMemoryRequirements requirements{};
        d.getImageMemoryRequirements(device,images[i],&requirements);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=requirements.size;
        ai.memoryTypeIndex=memoryType(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        ok(d.allocateMemory(device,&ai,nullptr,&imageMemory[i]));ok(d.bindImageMemory(device,images[i],imageMemory[i],0));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=images[i];vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=format;
        vi.subresourceRange={VkImageAspectFlags(depth?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT),0,1,0,1};ok(d.createImageView(device,&vi,nullptr,&views[i]));
    }
    VkBuffer readback{};VkDeviceMemory readMemory{};
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=pixelBytes;bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    ok(d.createBuffer(device,&bi,nullptr,&readback));VkMemoryRequirements requirements{};d.getBufferMemoryRequirements(device,readback,&requirements);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=requirements.size;
    ai.memoryTypeIndex=memoryType(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    ok(d.allocateMemory(device,&ai,nullptr,&readMemory));ok(d.bindBufferMemory(device,readback,readMemory,0));
    VkCommandPool pool{};VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pci.queueFamilyIndex=family;
    pci.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;ok(d.createCommandPool(device,&pci,nullptr,&pool));
    VkCommandBuffer cb{};VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=pool;
    ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;ok(d.allocateCommandBuffers(device,&ca,&cb));
    using namespace kharvox::hands;
    {
        HandRenderer renderer;std::string messages;
        check(renderer.initialize(physical,device,queue,family,d,VK_FORMAT_R8G8B8A8_SRGB,
            {{{width,height},{width,height}}},{{{images[0]},{images[1]}}},root.wstring(),
            [&](const std::string&text){messages+=text+'\n';}),"Hand renderer initialization failed");
        if(expectShared)check(sampledImagesCreated==(expectGrenadeVisual?6u:3u),"Unexpected atlas sharing or grenade texture count");
        check(renderer.hasGrenadeVisualPose()==expectGrenadeVisual,"Authored grenade configuration did not match the expected state");
        if(!benchmark)check(messages.find("left authored index/grip/thumb morphs loaded")!=std::string::npos
            &&messages.find("right authored index/grip/thumb morphs loaded")!=std::string::npos,"Morphs were not loaded by production renderer");
        bool leftHanded=false;
        bool reviewedSupport=false,validSupport=true,primaryDraw=false,primaryAcquired=true;
        KharvoxWeaponKind supportWeapon=KharvoxWeaponKind::Shotgun;
        float supportWeaponScale=1.f;std::array<float,3> supportAnchor{};
        std::array<math::Mat4,2> testJoints{};unsigned testJointMask{};
        bool syntheticHudColor=false;
        bool grenadeTwoHanded=false;
        float syntheticSceneDepth=1.f;
        auto render=[&](int eye,bool integrated,HandModelKind kind,FingerPose fingers,bool grenade){
            ok(d.resetCommandBuffer(cb,0));VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};ok(d.beginCommandBuffer(cb,&begin));
            const int target=(integrated?2:0)+eye;
            auto barrier=[&](int index,VkImageLayout from,VkImageLayout to){
                VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=images[index];b.oldLayout=from;b.newLayout=to;
                b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
                b.subresourceRange={VkImageAspectFlags(index==4?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT),0,1,0,1};
                b.srcAccessMask=from==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT;
                b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
                d.cmdPipelineBarrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
            };
            barrier(target,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            // A precomposed UI color with no depth contribution isolates the
            // composition mechanism; this is not a native SWF capture.
            const VkClearColorValue clear{{0,syntheticHudColor?1.f:0.f,0,1}};const VkImageSubresourceRange colorRange{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            vkCmdClearColorImage(cb,images[target],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear,1,&colorRange);
            barrier(target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            HandEyeView view{};view.pose.valid=true;view.pose.position[0]=eye==0?-.032f:.032f;
            view.angleLeft=view.angleDown=-.7f;view.angleRight=view.angleUp=.7f;
            HandPose grip{{0,0,-.6f},{0,0,0,1},true};
            HandPose left=leftHanded?HandPose{}:grip,right=leftHanded?grip:HandPose{};
            HandVisibilityOutput visibility{leftHanded?HandModelKind::None:kind,leftHanded?kind:HandModelKind::None};
            if(primaryDraw){
                left=leftHanded?grip:HandPose{};right=leftHanded?HandPose{}:grip;
                visibility={leftHanded?kind:HandModelKind::None,leftHanded?HandModelKind::None:kind};
            }
            HandGameplayState state{KharvoxWeaponKind::Shotgun,leftHanded};state.interaction.fingers[leftHanded?1:0]=fingers;
            state.interaction.support=grip;state.interaction.grenadeHeld=grenade;
            state.twoHanded=grenadeTwoHanded;
            if(reviewedSupport){
                state.weapon=supportWeapon;state.twoHanded=primaryAcquired;
                auto model=math::identity();
                model.m[0]=0;model.m[1]=0;model.m[2]=-.01f;
                model.m[4]=-.01f;model.m[5]=0;model.m[6]=0;
                model.m[8]=0;model.m[9]=.01f;model.m[10]=0;model.m[14]=-1.f;
                for(int row=0;row<3;++row){
                    for(int column=0;column<3;++column){
                        model.m[12+row]+=model.m[column*4+row]*supportAnchor[column]*(1.f-supportWeaponScale);
                        model.m[column*4+row]*=supportWeaponScale;
                    }
                }
                state.interaction.supportModel=model;state.interaction.supportModelValid=validSupport;
                state.weaponJoints=testJoints;state.weaponJointMask=testJointMask;
            }
            if(integrated){
                barrier(4,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                const VkImageSubresourceRange depthRange{VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};const VkClearDepthStencilValue depth{syntheticSceneDepth,0};
                vkCmdClearDepthStencilImage(cb,images[4],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&depth,1,&depthRange);
                barrier(4,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
                HandSceneTarget targetInfo{};targetInfo.colorImage=images[target];targetInfo.colorView=views[target];
                targetInfo.colorFormat=VK_FORMAT_R8G8B8A8_UNORM;targetInfo.depthImage=images[4];targetInfo.depthView=views[4];
                targetInfo.depthFormat=VK_FORMAT_D32_SFLOAT;targetInfo.depthLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                targetInfo.extent={width,height};targetInfo.copyDepthForHands=true;
                check(renderer.recordSceneIntegrated(cb,targetInfo,view,left,right,visibility,state),"Scene-integrated draw failed");
            }else renderer.record(cb,eye,0,view,left,right,visibility,state);
            barrier(target,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={width,height,1};
            vkCmdCopyImageToBuffer(cb,images[target],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback,1,&copy);
            VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
            d.cmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
            ok(d.endCommandBuffer(cb));VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&cb;
            ok(d.queueSubmit(queue,1,&submit,VK_NULL_HANDLE));ok(d.queueWaitIdle(queue));
            void*mapped{};ok(d.mapMemory(device,readMemory,0,pixelBytes,0,&mapped));
            std::vector<std::uint8_t> pixels(pixelBytes);std::memcpy(pixels.data(),mapped,pixels.size());d.unmapMemory(device,readMemory);
            renderer.finishSceneIntegratedFrame();
            bool colored=false;for(size_t i=0;i<pixels.size();i+=4)colored|=pixels[i]>10||pixels[i+1]>10||pixels[i+2]>10;
            check(colored,"Hand produced no visible pixels");return pixels;
        };
        if(benchmark){
            constexpr uint32_t warmup=30,samples=120;
            const auto validBits=queues[family].timestampValidBits;
            check(validBits>0&&properties.limits.timestampPeriod>0,"Graphics queue does not support GPU timestamps");
            VkQueryPool queries{};VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            queryInfo.queryType=VK_QUERY_TYPE_TIMESTAMP;queryInfo.queryCount=samples*2;
            ok(vkCreateQueryPool(device,&queryInfo,nullptr,&queries));
            unsigned warmAllocations{},warmTextures{};
            for(uint32_t frame=0;frame<warmup+samples;++frame){
                ok(d.resetCommandBuffer(cb,0));VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                ok(d.beginCommandBuffer(cb,&begin));
                if(frame==0)vkCmdResetQueryPool(cb,queries,0,samples*2);
                // Clear synthetic scene attachments outside the measured interval.
                // No scene render, CPU readback, OpenXR compositor or game work is timed.
                for(int target=2;target<5;++target){
                    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                    barrier.image=images[target];barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;
                    barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
                    barrier.subresourceRange={VkImageAspectFlags(target==4?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT),0,1,0,1};
                    barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
                    d.cmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
                    if(target==4){const VkClearDepthStencilValue clear{1,0};
                        vkCmdClearDepthStencilImage(cb,images[target],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear,1,&barrier.subresourceRange);
                    }else{const VkClearColorValue clear{{0,0,0,1}};
                        vkCmdClearColorImage(cb,images[target],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear,1,&barrier.subresourceRange);}
                    barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    barrier.newLayout=target==4?VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
                    barrier.dstAccessMask=target==4?VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                        :VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                    d.cmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&barrier);
                }
                if(frame>=warmup)vkCmdWriteTimestamp(cb,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,queries,(frame-warmup)*2);
                for(int eye=0;eye<2;++eye){
                    HandEyeView view{};view.pose.valid=true;view.pose.position[0]=eye==0?-.032f:.032f;
                    view.angleLeft=view.angleDown=-.7f;view.angleRight=view.angleUp=.7f;
                    const HandPose left{{-.12f,-.06f,-.6f},{0,0,0,1},true};
                    const HandPose right{{.12f,-.06f,-.6f},{0,0,0,1},true};
                    const HandVisibilityOutput visibility{expectSupport?HandModelKind::GunHolding:HandModelKind::Fist,HandModelKind::GunHolding};
                    HandGameplayState state{KharvoxWeaponKind::Shotgun,false};state.interaction.fingers[0]={.2f,.3f,.2f};
                    if(expectSupport){
                        state.twoHanded=true;state.interaction.supportModelValid=true;
                        auto& model=state.interaction.supportModel;model=math::identity();
                        model.m[0]=0;model.m[1]=0;model.m[2]=-.01f;
                        model.m[4]=-.01f;model.m[5]=0;model.m[6]=0;
                        model.m[8]=0;model.m[9]=.01f;model.m[10]=0;model.m[14]=-1.f;
                    }
                    HandSceneTarget target{};target.colorImage=images[2+eye];target.colorView=views[2+eye];
                    target.colorFormat=VK_FORMAT_R8G8B8A8_UNORM;target.depthImage=images[4];target.depthView=views[4];
                    target.depthFormat=VK_FORMAT_D32_SFLOAT;target.depthLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                    target.extent={width,height};target.copyDepthForHands=true;
                    check(renderer.recordSceneIntegrated(cb,target,view,left,right,visibility,state),"Benchmark scene-integrated draw failed");
                }
                if(frame>=warmup)vkCmdWriteTimestamp(cb,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,queries,(frame-warmup)*2+1);
                ok(d.endCommandBuffer(cb));VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                submit.commandBufferCount=1;submit.pCommandBuffers=&cb;
                ok(d.queueSubmit(queue,1,&submit,VK_NULL_HANDLE));ok(d.queueWaitIdle(queue));
                // Production lifetime rule: borrowing framebuffers outlive GPU work.
                renderer.finishSceneIntegratedFrame();
                if(frame==warmup-1){warmAllocations=allocationsCreated;warmTextures=sampledImagesCreated;}
            }
            std::array<uint64_t,samples*2> timestamps{};
            ok(vkGetQueryPoolResults(device,queries,0,samples*2,sizeof(timestamps),timestamps.data(),sizeof(uint64_t),
                VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT));
            const uint64_t mask=validBits==64?~uint64_t(0):(uint64_t(1)<<validBits)-1;
            double totalMs{},maximumMs{};
            for(uint32_t frame=0;frame<samples;++frame){
                const auto ticks=(timestamps[frame*2+1]-timestamps[frame*2])&mask;
                const double ms=double(ticks)*properties.limits.timestampPeriod/1000000.0;
                totalMs+=ms;maximumMs=std::max(maximumMs,ms);
            }
            check(allocationsCreated==warmAllocations&&sampledImagesCreated==warmTextures,"Benchmark allocated GPU memory or textures after warmup");
            std::cout<<"[HAND-GPU-BENCHMARK] extent="<<width<<'x'<<height<<" eyes=2 handsPerEye=2 warmup="<<warmup
                <<" samples="<<samples<<" meanMs="<<totalMs/samples<<" maxMs="<<maximumMs
                <<" timestampValidBits="<<validBits<<" timestampPeriodNs="<<properties.limits.timestampPeriod
                <<" sampledImages="<<sampledImagesCreated<<" liveAllocations="<<allocations.size()
                <<" liveAllocationBytes="<<allocatedBytes<<" peakAllocationBytes="<<peakAllocatedBytes
                <<" allocationsAfterWarmup="<<allocationsCreated-warmAllocations<<'\n'
                <<"scope=two production scene-integrated hand passes, two private-depth copies, four hand draws; excludes scene clears, game, compositor, CPU recording and queue waits; one query readback after all samples\n"
                <<"runtime="<<root.u8string()<<'\n';
            vkDestroyQueryPool(device,queries,nullptr);
        }else for(bool handedness:{false,true})for(bool integrated:{false,true})for(int eye=0;eye<2;++eye){
            leftHanded=handedness;
            const auto open=render(eye,integrated,HandModelKind::Fist,{0,0,0},false);
            const auto closed=render(eye,integrated,HandModelKind::Fist,{1,1,1},false);
            check(open!=closed,"Finger input did not deform the GPU mesh");
            const auto index=render(eye,integrated,HandModelKind::Fist,{1,0,0},false);
            check(index!=open&&index!=closed,"Index finger is not independent of grip");
            const auto gunA=render(eye,integrated,HandModelKind::GunHolding,{0,0,0},false);
            const auto gunB=render(eye,integrated,HandModelKind::GunHolding,{1,1,1},false);
            check(gunA==gunB,"Locked support hand fingers moved");
            const auto empty=render(eye,integrated,HandModelKind::Fist,{1,.75f,.75f},false);
            const auto grenade=render(eye,integrated,HandModelKind::Fist,{1,.75f,.75f},true);
            if(expectGrenadeVisual&&leftHanded)check(empty==grenade,"Visual grenade prototype changed left-handed mode");
            else check(empty!=grenade,"Held grenade is not visible in this eye/render path");
        }
        if(expectGrenadeVisual){
            leftHanded=false;
            for(bool integrated:{false,true})for(int eye=0;eye<2;++eye){
                const auto free=render(eye,integrated,HandModelKind::Fist,{0,0,0},false);
                const auto held=render(eye,integrated,HandModelKind::Fist,{0,0,0},true);
                check(free!=held,"Authored hand/grenade did not replace the free hand");
                check(held==render(eye,integrated,HandModelKind::Fist,{1,1,1},true),"Validated grenade fingers were animated");
                check(free==render(eye,integrated,HandModelKind::Fist,{0,0,0},false),"Release did not restore the free hand");
                grenadeTwoHanded=true;
                check(render(eye,integrated,HandModelKind::Fist,{0,0,0},true)
                    ==render(eye,integrated,HandModelKind::Fist,{0,0,0},false),"Two-hand grip did not suppress grenade visual");
                grenadeTwoHanded=false;
                leftHanded=true;
                check(render(eye,integrated,HandModelKind::Fist,{0,0,0},true)
                    ==render(eye,integrated,HandModelKind::Fist,{0,0,0},false),"Grenade prototype changed handedness fallback");
                leftHanded=false;
            }
            std::cout<<"Authored grenade visual: frozen fingers, release, two-hand suppression and handedness fallback passed in both eyes and both render paths\n";
        }
        if(!benchmark){
            leftHanded=false;
            for(int eye=0;eye<2;++eye){
                syntheticHudColor=false;syntheticSceneDepth=1.f;
                const auto plain=render(eye,true,HandModelKind::Fist,{0,0,0},false);
                syntheticHudColor=true;
                const auto composed=render(eye,true,HandModelKind::Fist,{0,0,0},false);
                unsigned overwritten{},preserved{};
                for(size_t i=0;i<plain.size();i+=4){
                    // Black atlas texels are opaque arm pixels too. Compare
                    // two backgrounds instead of assuming non-black = arm.
                    const bool hand=std::equal(plain.begin()+i,plain.begin()+i+3,composed.begin()+i);
                    if(hand){
                        ++overwritten;
                    }else{
                        check(plain[i]==0&&plain[i+1]==0&&plain[i+2]==0
                            &&composed[i]==0&&composed[i+1]==255&&composed[i+2]==0,
                            "Uncovered UI color was lost");++preserved;
                    }
                }
                check(overwritten&&preserved,"Occlusion probe must cover both arm and overhang pixels");
                // A nearer scene depth still prevents the production hand
                // draw. This does not establish native HUD/world occlusion.
                syntheticSceneDepth=0.f;
                const auto wall=render(eye,true,HandModelKind::Fist,{0,0,0},false);
                for(size_t i=0;i<wall.size();i+=4)
                    check(wall[i]==0&&wall[i+1]==255&&wall[i+2]==0,"Hand scene depth test stopped occluding");
                std::cout<<"[HUD-OCCLUSION-PROBE] synthetic=1 eye="<<eye
                    <<" overwrittenByArm="<<overwritten<<" preservedOutsideArm="<<preserved
                    <<" nearerSceneDepthBlocksHand=1 nativeHudPipelineIdentified=0\n";
            }
            syntheticHudColor=false;syntheticSceneDepth=1.f;
        }
        if(expectSupport&&!benchmark){
            std::array<WeaponSupportProfile,15> profiles{};
            std::ifstream config(root/L"support_hands.cfg");
            check(readWeaponSupportProfiles(config,profiles)==expectedSupportCount,"Unexpected reviewed weapon profile count");
            if(expectedSupportCount==2)check(publishedHudTemplates==4&&publishedFreeBounds[0]&&publishedFreeBounds[1],
                "Free/locked HUD wrist templates were not published for both pumps and hands");
            for(int weapon=2;weapon<=10;++weapon)for(bool handedness:{false,true}){
                supportWeapon=static_cast<KharvoxWeaponKind>(weapon);leftHanded=handedness;
                const bool expected=profiles[weapon].hashes[leftHanded?1:0]!=0;
                check(renderer.hasSupportProfile(supportWeapon,leftHanded)==expected,"Reviewed support profile missing or hash rejected");
                if(!expected){
                    reviewedSupport=false;
                    for(bool integrated:{false,true})for(int eye=0;eye<2;++eye){
                        const auto a=render(eye,integrated,HandModelKind::GunHolding,{0,0,0},false);
                        const auto b=render(eye,integrated,HandModelKind::GunHolding,{1,1,1},false);
                        check(a==b,"Legacy locked support changed with finger input");
                        const auto free=render(eye,integrated,HandModelKind::Fist,{0,0,0},false);
                        const auto closed=render(eye,integrated,HandModelKind::Fist,{1,1,1},false);
                        check(free!=closed,"Legacy free fingers lost their animation");
                    }
                    std::cout<<"Legacy support retained: "<<KharvoxWeaponKindKey(supportWeapon)<<" right hand\n";
                    continue;
                }
                reviewedSupport=true;
                supportAnchor=handedness?profiles[weapon].rightPoint:profiles[weapon].point;
                testJointMask=0;
                for(unsigned hand=0;hand<2;++hand){
                    const int slot=profiles[weapon].jointSlot[hand];
                    if(slot>=0){check(inverseSupportSimilarity(profiles[weapon].jointBindInverse[hand],testJoints[slot]),
                        "Invalid native joint bind");testJointMask|=1u<<slot;}
                }
                for(bool integrated:{false,true})for(int eye=0;eye<2;++eye){
                    validSupport=true;
                    const auto open=render(eye,integrated,HandModelKind::GunHolding,{0,0,0},false);
                    const auto closed=render(eye,integrated,HandModelKind::GunHolding,{1,1,1},false);
                    check(open==closed,"Reviewed locked support pose changed with finger input");
                    if(profiles[weapon].metersPerAssetUnit>0){
                        auto coverage=[](const std::vector<std::uint8_t>& pixels){size_t count{};
                            for(size_t i=0;i<pixels.size();i+=4)count+=pixels[i]>10||pixels[i+1]>10||pixels[i+2]>10;
                            return count;};
                        const auto reference=coverage(open);
                        for(float scale:{.5f,2.f}){
                            supportWeaponScale=scale;
                            const auto scaled=coverage(render(eye,integrated,HandModelKind::GunHolding,{0,0,0},false));
                            const auto difference=reference>scaled?reference-scaled:scaled-reference;
                            check(difference<=std::max(size_t(4),reference/100),"Physical glove changed screen coverage with weapon scale");
                        }
                        supportWeaponScale=1.f;
                    }
                    validSupport=false;
                    const auto fallback=render(eye,integrated,HandModelKind::GunHolding,{0,0,0},false);
                    check(fallback!=open,"Native weapon model transform did not select the reviewed support asset");
                    const auto free=render(eye,integrated,HandModelKind::Fist,{0,0,0},false);
                    const auto fist=render(eye,integrated,HandModelKind::Fist,{1,1,1},false);
                    check(free!=fist,"Free support fingers stopped working with reviewed profiles installed");
                }
            }
            reviewedSupport=false;
            std::cout<<"Reviewed support: "<<expectedSupportCount<<" weapons, both hands/eyes, overlay + scene integration, fixed physical scale, frozen locked fingers and animated free fingers passed\n";
        }
        if(expectPrimary){
            std::array<WeaponSupportProfile,15> profiles{};
            std::ifstream config(root/L"primary_hands.cfg");
            const auto primaryCount=readWeaponSupportProfiles(config,profiles);
            check(expectOneHandPrimary?primaryCount>=1:primaryCount==1,"Expected manual shotgun primary");
            check(messages.find("manual weapon-local primary profiles="+std::to_string(primaryCount))!=std::string::npos,
                "Manual primary profile was not loaded");
            unsigned testedPrimaryCount{};
            for(size_t weapon=1;weapon<profiles.size();++weapon){
                if(!profiles[weapon].valid||(!expectOneHandPrimary&&weapon!=static_cast<size_t>(KharvoxWeaponKind::Shotgun)))continue;
                check(renderer.hasPrimaryProfile(static_cast<KharvoxWeaponKind>(weapon)),"Loaded primary profile is not available");
                ++testedPrimaryCount;
                primaryDraw=true;reviewedSupport=true;leftHanded=false;primaryAcquired=true;
                supportWeapon=static_cast<KharvoxWeaponKind>(weapon);supportWeaponScale=1.f;
                supportAnchor=profiles[weapon].rightPoint;
                testJoints={};testJointMask=0;
                const auto&primaryProfile=profiles[weapon];
                const int primarySlot=primaryProfile.jointSlot[1];
                if(primarySlot>=0){check(inverseSupportSimilarity(primaryProfile.jointBindInverse[1],testJoints[primarySlot]),
                    "Invalid primary joint bind");testJointMask|=1u<<primarySlot;}
                for(bool integrated:{false,true})for(int eye=0;eye<2;++eye){
                    validSupport=true;
                    const auto locked=render(eye,integrated,HandModelKind::GunHolding,{0,0,0},false);
                    check(locked==render(eye,integrated,HandModelKind::GunHolding,{1,1,1},false),
                        "Manual primary fingers changed while locked");
                    if(expectOneHandPrimary){
                        primaryAcquired=false;
                        const auto single=render(eye,integrated,HandModelKind::GunHolding,{1,1,1},false);
                        check(single==locked,"One/two-hand transition moved or changed the primary pose");
                        primaryAcquired=true;
                        check(single==render(eye,integrated,HandModelKind::GunHolding,{0,0,0},false),
                            "Reacquiring support changed the primary pose");
                        primaryAcquired=false;
                        check(single==render(eye,integrated,HandModelKind::GunHolding,{0,0,0},false),
                            "Releasing support changed the primary pose");
                        primaryDraw=false;
                        const auto free=render(eye,integrated,HandModelKind::Fist,{0,0,0},false);
                        validSupport=false;
                        check(free==render(eye,integrated,HandModelKind::Fist,{0,0,0},false),
                            "Free support hand was attached to the one-handed weapon");
                        validSupport=true;
                        check(free!=render(eye,integrated,HandModelKind::Fist,{1,1,1},false),
                            "Free support fingers stopped animating");
                        primaryDraw=true;
                    }
                    if(primarySlot>=0){
                        testJoints[primarySlot].m[12]+=3;
                        check(locked!=render(eye,integrated,HandModelKind::GunHolding,{0,0,0},false),
                            "Native base joint did not move the primary hand");
                        testJoints[primarySlot].m[12]-=3;
                        testJointMask&=~(1u<<primarySlot);
                        check(locked!=render(eye,integrated,HandModelKind::GunHolding,{0,0,0},false),
                            "Missing native joint did not use calibrated fallback");
                        testJointMask|=1u<<primarySlot;
                    }
                    validSupport=false;
                    check(locked!=render(eye,integrated,HandModelKind::GunHolding,{0,0,0},false),
                        "Invalid native frame did not restore the calibrated primary");
                    leftHanded=true;validSupport=true;
                    const auto gaucher=render(eye,integrated,HandModelKind::GunHolding,{0,0,0},false);
                    validSupport=false;
                    check(gaucher==render(eye,integrated,HandModelKind::GunHolding,{0,0,0},false),
                        "Physical-right prototype changed the left-handed primary");
                    leftHanded=false;primaryAcquired=true;
                }
                check(messages.find(std::string("manual primary active weapon=")+KharvoxWeaponKindKey(supportWeapon)+" hand=right")!=std::string::npos,
                    "The production draw did not select the manual primary mesh");
                std::cout<<"Primary pose transitions passed: "<<KharvoxWeaponKindKey(supportWeapon)<<'\n';
            }
            check(testedPrimaryCount==(expectOneHandPrimary?primaryCount:1),"Not every authored primary was exercised");
            primaryDraw=true;reviewedSupport=true;leftHanded=false;primaryAcquired=false;
            for(size_t weapon=1;weapon<profiles.size();++weapon){
                if(profiles[weapon].valid)continue;
                supportWeapon=static_cast<KharvoxWeaponKind>(weapon);
                check(!renderer.hasPrimaryProfile(supportWeapon),"Missing primary profile reported as available");
                for(bool integrated:{false,true})for(int eye=0;eye<2;++eye){
                    validSupport=true;
                    const auto before=render(eye,integrated,HandModelKind::GunHolding,{0,0,0},false);
                    validSupport=false;
                    check(before==render(eye,integrated,HandModelKind::GunHolding,{0,0,0},false),
                        "Weapon without a primary profile lost its calibrated fallback");
                }
            }
            primaryAcquired=true;
            primaryDraw=false;reviewedSupport=false;
            std::cout<<"Manual primary: "<<testedPrimaryCount<<" weapons, both eyes, overlay + scene integration, frozen fingers, "
                <<"invalid-frame fallback and left-handed paths passed\n";
            if(expectOneHandPrimary)std::cout<<"One/two/one-hand transitions and animated free support passed for all loaded profiles\n";
        }
    }
    check(sampledImages.empty()&&sampledImagesCreated==sampledImagesDestroyed,"Shared hand textures leaked at shutdown");
    std::cout<<"Shared hand textures: created="<<sampledImagesCreated<<" destroyed="<<sampledImagesDestroyed<<'\n';
    d.destroyCommandPool(device,pool,nullptr);d.destroyBuffer(device,readback,nullptr);d.freeMemory(device,readMemory,nullptr);
    for(int i=0;i<5;++i){d.destroyImageView(device,views[i],nullptr);d.destroyImage(device,images[i],nullptr);d.freeMemory(device,imageMemory[i],nullptr);}
    check(allocations.empty()&&allocatedBytes==0,"GPU memory allocation leaked at shutdown");
    vkDestroyDevice(device,nullptr);vkDestroyInstance(instance,nullptr);FreeLibrary(loader);
    if(!benchmark)std::cout<<"Production hand renderer: finger deformation, independent index, frozen support pose and visible held grenade passed in both eyes, both handedness modes, overlay + scene-integrated paths\n";
    return 0;
}catch(const std::exception&error){std::cerr<<error.what()<<'\n';return 1;}}

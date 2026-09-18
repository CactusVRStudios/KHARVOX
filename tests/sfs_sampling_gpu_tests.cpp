#include <vulkan/vulkan.h>
#include "../src/sfs/StereoResources.h"
#include "../src/sfs/ShaderCompiler.h"
#ifdef KHARVOX_SFS_TEST_RUNTIME
#include "../src/sfs/NativeSfs.h"
#include "../src/native/NativeStereo.h"
#endif
#include <fstream>
#include <cstring>
#include <windows.h>
#include <array>
#include <vector>
#include <stdexcept>
#include <iostream>
#include <cmath>

static void check(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
static void ok(VkResult value){check(value==VK_SUCCESS,"Vulkan operation failed");}
int main(int argc,char** argv){try{
#ifdef KHARVOX_SFS_TEST_RUNTIME
    SetEnvironmentVariableA("KHARVOX_SFS_NATIVE_PROBE","1");
    SetEnvironmentVariableA("KHARVOX_SFS_PROFILE",nullptr);
#ifdef KHARVOX_SFS_TEST_AFFINE
    SetEnvironmentVariableA("KHARVOX_SFS_NATIVE_VR","1");
#else
    SetEnvironmentVariableA("KHARVOX_SFS_NATIVE_VR",nullptr);
#endif
#endif
    check(argc==3,"Expected vertex and fragment SPIR-V fixtures");
    auto loader=LoadLibraryW(L"vulkan-1.dll");check(loader,"Vulkan loader unavailable");
    auto gipa=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader,"vkGetInstanceProcAddr"));
    auto createInstance=reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr,"vkCreateInstance"));
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ici.pApplicationInfo=&app;
    VkInstance instance{};ok(createInstance(&ici,nullptr,&instance));
#define INSTANCE(name) auto name=reinterpret_cast<PFN_##name>(gipa(instance,#name));check(name,#name)
    INSTANCE(vkEnumeratePhysicalDevices);INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties);
    INSTANCE(vkGetPhysicalDeviceMemoryProperties);INSTANCE(vkGetPhysicalDeviceProperties);
    INSTANCE(vkGetPhysicalDeviceFeatures2);INSTANCE(vkCreateDevice);INSTANCE(vkDestroyInstance);INSTANCE(vkGetDeviceProcAddr);
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
    VkPhysicalDeviceMultiviewFeatures mv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};features.pNext=&mv;
    vkGetPhysicalDeviceFeatures2(physical,&features);check(mv.multiview,"GPU lacks multiview");
    mv.multiviewGeometryShader=mv.multiviewTessellationShader=VK_FALSE;dci.pNext=&mv;
    VkDevice device{};ok(vkCreateDevice(physical,&dci,nullptr,&device));
#ifdef KHARVOX_SFS_TEST_RUNTIME
    VkPhysicalDeviceMemoryProperties sfsMemory{};vkGetPhysicalDeviceMemoryProperties(physical,&sfsMemory);
    check(kharvox::sfs::initialize(device,physical,vkGetDeviceProcAddr,sfsMemory),"SFS runtime initialization failed");
#ifdef KHARVOX_SFS_TEST_AFFINE
    kharvox::native::FramePose pose{};pose.head.orientation.w=1;pose.worldScale=1;pose.gameplay=true;
    XrFovf projection{-.7853981634f,.7853981634f,.7853981634f,-.7853981634f};
    for(unsigned e=0;e<2;++e){pose.views[e].pose=pose.head;pose.views[e].pose.position.x=e?.032f:-.032f;pose.views[e].fov=projection;}
    kharvox::sfs::prepare(device,pose,projection);kharvox::sfs::beginFrame(device);
#endif
    auto resolve=[&](const char* name){return kharvox::sfs::wrapProc(device,name,vkGetDeviceProcAddr(device,name));};
#else
    auto resolve=[&](const char* name){return vkGetDeviceProcAddr(device,name);};
#endif
#define DEVICE(name) auto name=reinterpret_cast<PFN_##name>(resolve(#name));check(name,#name)
    DEVICE(vkGetDeviceQueue);DEVICE(vkDestroyDevice);DEVICE(vkCreateCommandPool);DEVICE(vkDestroyCommandPool);
    DEVICE(vkAllocateCommandBuffers);DEVICE(vkResetCommandBuffer);DEVICE(vkBeginCommandBuffer);DEVICE(vkEndCommandBuffer);
    DEVICE(vkQueueSubmit);DEVICE(vkQueueWaitIdle);DEVICE(vkCreateBuffer);DEVICE(vkDestroyBuffer);
    DEVICE(vkCreateFence);DEVICE(vkDestroyFence);DEVICE(vkWaitForFences);
    DEVICE(vkGetBufferMemoryRequirements);DEVICE(vkAllocateMemory);DEVICE(vkFreeMemory);DEVICE(vkBindBufferMemory);
    DEVICE(vkMapMemory);DEVICE(vkUnmapMemory);DEVICE(vkCreateImage);DEVICE(vkDestroyImage);
    DEVICE(vkGetImageMemoryRequirements);DEVICE(vkBindImageMemory);DEVICE(vkCmdPipelineBarrier);
    DEVICE(vkCmdClearDepthStencilImage);DEVICE(vkCmdCopyImageToBuffer);DEVICE(vkCmdCopyImage);
    DEVICE(vkCreateRenderPass);DEVICE(vkDestroyRenderPass);DEVICE(vkCreateImageView);DEVICE(vkDestroyImageView);
    DEVICE(vkCreateFramebuffer);DEVICE(vkDestroyFramebuffer);DEVICE(vkCmdBeginRenderPass);DEVICE(vkCmdEndRenderPass);
    DEVICE(vkCreateShaderModule);DEVICE(vkDestroyShaderModule);DEVICE(vkCreateSampler);DEVICE(vkDestroySampler);
    DEVICE(vkCreateDescriptorSetLayout);DEVICE(vkDestroyDescriptorSetLayout);DEVICE(vkCreateDescriptorPool);DEVICE(vkDestroyDescriptorPool);
    DEVICE(vkAllocateDescriptorSets);DEVICE(vkUpdateDescriptorSets);DEVICE(vkCreatePipelineLayout);DEVICE(vkDestroyPipelineLayout);
    DEVICE(vkCreateGraphicsPipelines);DEVICE(vkDestroyPipeline);DEVICE(vkCmdBindPipeline);DEVICE(vkCmdBindDescriptorSets);DEVICE(vkCmdDraw);DEVICE(vkCmdClearColorImage);
    VkQueue queue{};vkGetDeviceQueue(device,family,0,&queue);
    VkPhysicalDeviceMemoryProperties memoryProps{};vkGetPhysicalDeviceMemoryProperties(physical,&memoryProps);
    auto memoryType=[&](uint32_t bits,VkMemoryPropertyFlags flags){for(uint32_t i=0;i<memoryProps.memoryTypeCount;++i)if((bits&(1u<<i))&&(memoryProps.memoryTypes[i].propertyFlags&flags)==flags)return i;throw std::runtime_error("No memory type");};
    kharvox::sfs::Images registry;
    auto makeImage=[&](VkDevice d,const VkImageCreateInfo& info,const VkAllocationCallbacks* a,VkImage* out,PFN_vkCreateImage next){
#ifdef KHARVOX_SFS_TEST_RUNTIME
        auto r=next(d,&info,a,out);if(r==VK_SUCCESS)registry.track(*out,kharvox::sfs::stereoImage(info)?2:1);return r;
#else
        return registry.create(d,info,a,out,next);
#endif
    };
    VkImageCreateInfo input{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    input.imageType=VK_IMAGE_TYPE_2D;input.format=VK_FORMAT_D32_SFLOAT;
    input.extent={8,8,1};input.mipLevels=1;input.arrayLayers=1;input.samples=VK_SAMPLE_COUNT_1_BIT;
    input.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImage image{};ok(makeImage(device,input,nullptr,&image,vkCreateImage));
    VkMemoryRequirements req{};vkGetImageMemoryRequirements(device,image,&req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};alloc.allocationSize=req.size;
    alloc.memoryTypeIndex=memoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory imageMemory{};ok(vkAllocateMemory(device,&alloc,nullptr,&imageMemory));ok(vkBindImageMemory(device,image,imageMemory,0));
    VkImageViewCreateInfo original{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};original.image=image;
    original.viewType=VK_IMAGE_VIEW_TYPE_2D;original.format=input.format;
    original.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
    auto transformed=registry.viewInfo(original);
    VkImageView view{};ok(vkCreateImageView(device,&transformed,nullptr,&view));
#ifdef KHARVOX_SFS_TEST_RUNTIME
    VkImageView leftAttachment{},rightAttachment{},cachedAttachment{};
    check(kharvox::sfs::eyeAttachmentView(device,view,0,leftAttachment),"Left hand attachment unavailable");
    check(kharvox::sfs::eyeAttachmentView(device,view,1,rightAttachment),"Right hand attachment unavailable");
    check(leftAttachment!=rightAttachment&&leftAttachment!=view&&rightAttachment!=view,"Eye attachment views alias");
    check(kharvox::sfs::eyeAttachmentView(device,view,1,cachedAttachment)&&cachedAttachment==rightAttachment,"Eye attachment cache unstable");
    check(!kharvox::sfs::eyeAttachmentView(device,view,2,cachedAttachment),"Invalid attachment eye accepted");
#endif
    VkAttachmentDescription attachment{};attachment.format=input.format;attachment.samples=VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    attachment.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;attachment.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference reference{0,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;subpass.pDepthStencilAttachment=&reference;
    VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};passInfo.attachmentCount=1;passInfo.pAttachments=&attachment;passInfo.subpassCount=1;passInfo.pSubpasses=&subpass;
    kharvox::sfs::RenderPassPlan plan(passInfo,true);check(plan.valid(),"Invalid stereo plan");
    VkRenderPass pass{};
#ifdef KHARVOX_SFS_TEST_RUNTIME
    ok(vkCreateRenderPass(device,&passInfo,nullptr,&pass));
#else
    ok(vkCreateRenderPass(device,&plan.info(),nullptr,&pass));
#endif
    VkFramebufferCreateInfo fbInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fbInfo.renderPass=pass;fbInfo.attachmentCount=1;fbInfo.pAttachments=&view;fbInfo.width=fbInfo.height=8;fbInfo.layers=1;
    VkFramebuffer framebuffer{};ok(vkCreateFramebuffer(device,&fbInfo,nullptr,&framebuffer));

    struct Texture {VkImage image{};VkDeviceMemory memory{};VkImageView view{};uint32_t layers{};};
    auto makeTexture=[&](bool stereo){
        Texture t;VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=VK_FORMAT_R32_SFLOAT;ci.extent={4,4,1};ci.mipLevels=ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;
        ci.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|(stereo?VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT:0);
        ok(makeImage(device,ci,nullptr,&t.image,vkCreateImage));t.layers=registry.layers(t.image);
        VkMemoryRequirements requirements{};vkGetImageMemoryRequirements(device,t.image,&requirements);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=requirements.size;ai.memoryTypeIndex=memoryType(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        ok(vkAllocateMemory(device,&ai,nullptr,&t.memory));ok(vkBindImageMemory(device,t.image,t.memory,0));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=t.image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=ci.format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vi=registry.shaderViewInfo(vi);ok(vkCreateImageView(device,&vi,nullptr,&t.view));return t;
    };
    auto stereoTexture=makeTexture(true),monoTexture=makeTexture(false);
    check(stereoTexture.layers==2&&monoTexture.layers==1,"Mono texture was incorrectly duplicated");
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};si.magFilter=si.minFilter=VK_FILTER_NEAREST;si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler{};ok(vkCreateSampler(device,&si,nullptr,&sampler));
    VkDescriptorSetLayoutBinding bindings[2]{{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},{1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};dl.bindingCount=2;dl.pBindings=bindings;
    VkDescriptorSetLayout layout{};ok(vkCreateDescriptorSetLayout(device,&dl,nullptr,&layout));
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2};VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dp.maxSets=1;dp.poolSizeCount=1;dp.pPoolSizes=&ps;
    VkDescriptorPool descriptorPool{};ok(vkCreateDescriptorPool(device,&dp,nullptr,&descriptorPool));
    VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};da.descriptorPool=descriptorPool;da.descriptorSetCount=1;da.pSetLayouts=&layout;
    VkDescriptorSet descriptor{};ok(vkAllocateDescriptorSets(device,&da,&descriptor));
    VkDescriptorImageInfo images[2]{{sampler,stereoTexture.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{sampler,monoTexture.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet writes[2]{};for(uint32_t i=0;i<2;++i){writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[i].dstSet=descriptor;writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;writes[i].pImageInfo=&images[i];}vkUpdateDescriptorSets(device,2,writes,0,nullptr);
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&layout;
    VkPipelineLayout pipelineLayout{};ok(vkCreatePipelineLayout(device,&pl,nullptr,&pipelineLayout));
    auto module=[&](const char* path){std::ifstream f(path,std::ios::binary|std::ios::ate);check(bool(f),"Shader fixture missing");auto size=f.tellg();check(size>=20&&size%4==0,"Invalid fixture size");std::vector<uint32_t> words(size_t(size)/4);f.seekg(0);f.read(reinterpret_cast<char*>(words.data()),size);auto compiled=kharvox::sfs::compileStereoShader(words);
#ifdef KHARVOX_SFS_TEST_RUNTIME
        compiled.words=words;
#endif
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};mi.codeSize=compiled.words.size()*4;mi.pCode=compiled.words.data();VkShaderModule m{};ok(vkCreateShaderModule(device,&mi,nullptr,&m));return m;};
    VkShaderModule vertex=module(argv[1]),fragment=module(argv[2]);
    VkPipelineShaderStageCreateInfo stages[2]{};for(auto& stage:stages){stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;stage.pName="main";}stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vertex;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fragment;
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0,0,8,8,0,1};VkRect2D scissor{{0,0},{8,8}};VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;vp.pViewports=&viewport;vp.pScissors=&scissor;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.lineWidth=1;VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};depth.depthTestEnable=depth.depthWriteEnable=VK_TRUE;depth.depthCompareOp=VK_COMPARE_OP_ALWAYS;VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};pi.stageCount=2;pi.pStages=stages;pi.pVertexInputState=&vi;pi.pInputAssemblyState=&ia;pi.pViewportState=&vp;pi.pRasterizationState=&raster;pi.pMultisampleState=&ms;pi.pDepthStencilState=&depth;pi.pColorBlendState=&blend;pi.layout=pipelineLayout;pi.renderPass=pass;
    VkPipeline pipeline{};ok(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&pi,nullptr,&pipeline));
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=512;bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buffer{};ok(vkCreateBuffer(device,&bi,nullptr,&buffer));vkGetBufferMemoryRequirements(device,buffer,&req);
    alloc.allocationSize=req.size;alloc.memoryTypeIndex=memoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory bufferMemory{};ok(vkAllocateMemory(device,&alloc,nullptr,&bufferMemory));ok(vkBindBufferMemory(device,buffer,bufferMemory,0));
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};poolInfo.queueFamilyIndex=family;
    VkCommandPool pool{};ok(vkCreateCommandPool(device,&poolInfo,nullptr,&pool));
    VkCommandBufferAllocateInfo cbInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};cbInfo.commandPool=pool;cbInfo.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cbInfo.commandBufferCount=1;
    VkCommandBuffer command{};ok(vkAllocateCommandBuffers(device,&cbInfo,&command));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};ok(vkBeginCommandBuffer(command,&begin));
    auto fill=[&](const Texture& t,float left,float right){
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=t.image;b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,t.layers};
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
        for(uint32_t eye=0;eye<t.layers;++eye){VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT,0,1,eye,1};VkClearColorValue c{};c.float32[0]=eye?right:left;vkCmdClearColorImage(command,t.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&c,1,&r);}
        b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&b);
    };
    fill(stereoTexture,.25f,.75f);fill(monoTexture,.125f,.125f);
    VkClearValue clear{};clear.depthStencil.depth=0;
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};rp.renderPass=pass;rp.framebuffer=framebuffer;rp.renderArea.extent={8,8};rp.clearValueCount=1;rp.pClearValues=&clear;
#ifdef KHARVOX_SFS_TEST_RUNTIME
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout,0,1,&descriptor,0,nullptr);
    vkCmdBeginRenderPass(command,&rp,VK_SUBPASS_CONTENTS_INLINE);
#else
    vkCmdBeginRenderPass(command,&rp,VK_SUBPASS_CONTENTS_INLINE);vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout,0,1,&descriptor,0,nullptr);
#endif
    vkCmdDraw(command,3,1,0,0);vkCmdEndRenderPass(command);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    VkBufferImageCopy read{};read.imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,2};read.imageExtent={8,8,1};
    vkCmdCopyImageToBuffer(command,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&read);
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    ok(vkEndCommandBuffer(command));VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command;
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VkFence fence{};ok(vkCreateFence(device,&fenceInfo,nullptr,&fence));
    ok(vkQueueSubmit(queue,1,&submit,fence));ok(vkWaitForFences(device,1,&fence,VK_TRUE,10000000000ull));
    void* mapped{};ok(vkMapMemory(device,bufferMemory,0,512,0,&mapped));
    for(unsigned pixel=0;pixel<128;++pixel){float expected=pixel<64?.375f:.875f;
#ifdef KHARVOX_SFS_TEST_AFFINE
        expected+=pixel<64?.032f:-.032f;
#endif
        check(std::abs(static_cast<float*>(mapped)[pixel]-expected)<1e-6f,"Stereo sampling, mono clamping or affine eye correction produced incorrect pixels");}
    vkUnmapMemory(device,bufferMemory);vkDestroyFence(device,fence,nullptr);
#ifdef KHARVOX_SFS_TEST_AFFINE
    // Use real image handles with a bookkeeping-only swapchain. No WSI calls:
    // exercise acquisition, pending predictions, resize and handle reuse.
    const auto chain=reinterpret_cast<VkSwapchainKHR>(uintptr_t(1));
    VkImage trackedImages[]{image,stereoTexture.image};
    kharvox::native::StereoFrame captured{};
    kharvox::sfs::swapchainImages(device,chain,2,trackedImages);
    check(!kharvox::sfs::pair(device,image,{8,8},VK_FORMAT_D32_SFLOAT,captured),"Unacquired image accepted");
    kharvox::sfs::beginFrame(device,chain,0);
    check(kharvox::sfs::pair(device,image,{8,8},VK_FORMAT_D32_SFLOAT,captured)&&captured.pose.serial==pose.serial,"Acquired pose missing");
    pose.serial=2;pose.source={22,3,kharvox::native::SceneDomain::Gameplay};
    pose.controllersValid={true,true};pose.controllers[1].position={.2f,1.2f,-.4f};
    kharvox::sfs::prepare(device,pose,projection);
    kharvox::sfs::beginFrame(device,chain,1); // copy not retired: old uniforms
    check(kharvox::sfs::pair(device,trackedImages[1],{8,8},VK_FORMAT_D32_SFLOAT,captured)&&captured.pose.serial!=2,"Pending pose labeled as rendered");
    kharvox::sfs::copyCompleted(device);kharvox::sfs::beginFrame(device,chain,1);
    check(kharvox::sfs::pair(device,trackedImages[1],{8,8},VK_FORMAT_D32_SFLOAT,captured)&&captured.pose.serial==2&&captured.pose.controllersValid[1]&&captured.pose.controllers[1].position.x==.2f,"Frame controller snapshot lost");
    check(kharvox::sfs::pair(device,image,{8,8},VK_FORMAT_D32_SFLOAT,captured)&&captured.pose.serial!=2,"Other acquired image relabeled by newer prediction");
    kharvox::sfs::swapchainImages(device,chain,2,trackedImages);
    check(kharvox::sfs::pair(device,trackedImages[1],{8,8},VK_FORMAT_D32_SFLOAT,captured)&&captured.pose.serial==2,"Repeated enumeration discarded source");
    kharvox::sfs::swapchainDestroyed(device,chain);
    kharvox::sfs::swapchainImages(device,chain,2,trackedImages);
    check(!kharvox::sfs::pair(device,trackedImages[1],{8,8},VK_FORMAT_D32_SFLOAT,captured),"Recreated swapchain reused old pose");
    kharvox::sfs::beginFrame(device,chain,2);
    check(!kharvox::sfs::pair(device,trackedImages[1],{8,8},VK_FORMAT_D32_SFLOAT,captured),"Invalid index published a source");
    kharvox::sfs::beginFrame(device,chain,1);
    check(kharvox::sfs::pair(device,trackedImages[1],{8,8},VK_FORMAT_D32_SFLOAT,captured),"Recreated swapchain did not recover");
    kharvox::sfs::swapchainDestroyed(device,chain);
#endif

    vkDestroyPipeline(device,pipeline,nullptr);vkDestroyShaderModule(device,vertex,nullptr);vkDestroyShaderModule(device,fragment,nullptr);
    vkDestroyPipelineLayout(device,pipelineLayout,nullptr);vkDestroyDescriptorPool(device,descriptorPool,nullptr);vkDestroyDescriptorSetLayout(device,layout,nullptr);vkDestroySampler(device,sampler,nullptr);
    for(auto t:{stereoTexture,monoTexture}){vkDestroyImageView(device,t.view,nullptr);registry.destroy(device,t.image,nullptr,vkDestroyImage);vkFreeMemory(device,t.memory,nullptr);}
    vkDestroyCommandPool(device,pool,nullptr);vkDestroyBuffer(device,buffer,nullptr);vkFreeMemory(device,bufferMemory,nullptr);
    vkDestroyFramebuffer(device,framebuffer,nullptr);vkDestroyRenderPass(device,pass,nullptr);vkDestroyImageView(device,view,nullptr);
    registry.destroy(device,image,nullptr,vkDestroyImage);vkFreeMemory(device,imageMemory,nullptr);
#ifdef KHARVOX_SFS_TEST_RUNTIME
    kharvox::sfs::shutdown(device);
#endif
    vkDestroyDevice(device,nullptr);vkDestroyInstance(instance,nullptr);FreeLibrary(loader);
    std::cout<<"Typed SFS shader/resource integration: distinct eye pixels and shared mono texture passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

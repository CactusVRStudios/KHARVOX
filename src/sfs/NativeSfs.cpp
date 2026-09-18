#include "NativeSfs.h"
#include "ShaderCompiler.h"
#include "ShaderProfile.h"
#include "PipelineIdentity.h"
#include "StereoResources.h"
#include "FrameProjection.h"
#include "../native/NativeStereo.h"
#include "../common/RuntimeLog.h"
#include <windows.h>
#include <array>
#include <atomic>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <type_traits>

namespace kharvox::sfs {
namespace {
// Developer-only native multiview/OpenXR prototype. Headset validation and
// complete profile lighting corrections are required before a playable release.
void note(const std::string& text){kharvox::writeRuntimeLog("[SFS]",text,true);}
struct CommandState {
    bool stereo{};
    VkPipeline compute{};
    std::map<uint64_t,std::function<void()>> bindings;
};
struct State {
    VkDevice device{};PFN_vkGetDeviceProcAddr gdpa{};
    std::recursive_mutex mutex;
    Images images;
    std::unordered_map<VkSwapchainKHR,std::vector<VkImage>> swapchains;
    VkBuffer params{};VkDeviceMemory paramsMemory{};
    FrameUniforms pendingUniforms{};
    native::FramePose pendingPose{},renderPose{};
    bool pending{},completed{true},frameValid{};
    std::unordered_map<VkShaderModule,std::vector<uint32_t>> shaders;
    std::unordered_map<std::string,VkShaderModule> compiled;
    std::unordered_map<VkRenderPass,VkRenderPass> passes;
    std::unordered_map<VkImageView,uint32_t> viewLayers;
    std::unordered_map<VkImageView,VkImageViewCreateInfo> viewInfos;
    std::unordered_map<VkImageView,std::array<VkImageView,2>> eyeViews;
    std::unordered_map<VkFramebuffer,bool> framebufferStereo;
    std::unordered_map<VkPipeline,VkPipeline> stereoPipelines;
    std::unordered_map<VkPipeline,bool> computeStereo;
    std::unordered_map<VkDescriptorSetLayout,uint32_t> dynamicCounts;
    std::unordered_map<VkDescriptorSet,uint32_t> setDynamicCounts;
    std::unordered_map<VkDescriptorSet,VkDescriptorPool> setPools;
    std::unordered_map<VkCommandBuffer,CommandState> commands;
    std::unordered_map<VkCommandBuffer,VkCommandPool> commandPools;
    std::filesystem::path profile;
    template<class T>T fn(const char* name){auto p=reinterpret_cast<T>(gdpa(device,name));if(!p)throw std::runtime_error(std::string("Missing Vulkan entry ")+name);return p;}
};
std::mutex devicesMutex;
std::unordered_map<void*,std::shared_ptr<State>> devices;
template<class T>void* dispatchKey(T handle){return handle?*reinterpret_cast<void**>(handle):nullptr;}
template<class T>std::shared_ptr<State> state(T handle){
    std::lock_guard<std::mutex> lock(devicesMutex);
    if constexpr(std::is_same_v<T,VkDevice>){
        // The loader can replace a newly-created device's dispatch table after
        // our CreateDevice returns. Keep identity by the device handle as well.
        std::shared_ptr<State> found;
        for(const auto& entry:devices)if(entry.second->device==handle){found=entry.second;break;}
        if(found){devices[dispatchKey(handle)]=found;return found;}
        throw std::runtime_error("Unregistered SFS device");
    }
    auto it=devices.find(dispatchKey(handle));if(it==devices.end())throw std::runtime_error("SFS device not initialized handle="+std::to_string(reinterpret_cast<uintptr_t>(handle))+" dispatch="+std::to_string(reinterpret_cast<uintptr_t>(dispatchKey(handle))));return it->second;
}
#define FN(name) s->fn<PFN_##name>(#name)
#define RESULT_BEGIN try {auto s=state(d);std::lock_guard<std::recursive_mutex> lock(s->mutex);
#define RESULT_END }catch(const std::exception& e){note(std::string(__FUNCTION__)+": "+e.what());return VK_ERROR_INITIALIZATION_FAILED;}
// Command hooks cannot return VkResult. Fail the owned diagnostic process on an
// unsupported command rather than record an invalid command or wait forever.
[[noreturn]] void commandFailure(const char* message){note(message);RaiseFailFastException(nullptr,nullptr,0);std::terminate();}
#define COMMAND_BEGIN try {auto s=state(cb);std::lock_guard<std::recursive_mutex> lock(s->mutex);
#define COMMAND_END }catch(const std::exception& e){commandFailure(e.what());}

VkShaderModule compiledModule(const std::shared_ptr<State>& s,VkShaderModule original,uint64_t variant,bool& stereoCompute){
    const auto found=s->shaders.find(original);if(found==s->shaders.end())throw std::runtime_error("Untracked game shader module");
    const auto& words=found->second;const auto primary=profileHash(words.data(),uint32_t(words.size()*4));
    VkShaderStageFlagBits stage{};
    for(size_t i=5;i<words.size();){auto n=words[i]>>16;if(!n||n>words.size()-i)throw std::runtime_error("Invalid game SPIR-V");if((words[i]&65535)==15){stage=words[i+1]==0?VK_SHADER_STAGE_VERTEX_BIT:words[i+1]==4?VK_SHADER_STAGE_FRAGMENT_BIT:words[i+1]==5?VK_SHADER_STAGE_COMPUTE_BIT:VkShaderStageFlagBits(0);break;}i+=n;}
    if(!stage)throw std::runtime_error("Unsupported SFS shader execution model");
    const auto replacement=loadProfileShader(s->profile,primary,variant,stage);
    const auto& input=replacement?replacement.words:words;
    stereoCompute=hasStereoStorageOutput(input);
    const auto key=shaderKey(primary)+"_"+shaderKey(variant);
    auto cached=s->compiled.find(key);if(cached!=s->compiled.end())return cached->second;
    ShaderCompileOptions options;options.computeStereo=stereoCompute&&!replacement;
    options.vertexProjection=needsStereoProjection(input,bool(replacement));
    auto shader=compileStereoShader(input,options);
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};info.codeSize=shader.words.size()*4;info.pCode=shader.words.data();
    VkShaderModule result{};auto r=FN(vkCreateShaderModule)(s->device,&info,nullptr,&result);
    if(r!=VK_SUCCESS)throw std::runtime_error("Stereo shader module creation failed");
    s->compiled.emplace(key,result);return result;
}
VKAPI_ATTR VkResult VKAPI_CALL createShader(VkDevice d,const VkShaderModuleCreateInfo* i,const VkAllocationCallbacks* a,VkShaderModule* out){RESULT_BEGIN
    auto r=FN(vkCreateShaderModule)(d,i,a,out);if(r==VK_SUCCESS)s->shaders[*out]={i->pCode,i->pCode+i->codeSize/4};return r;
RESULT_END}
VKAPI_ATTR void VKAPI_CALL destroyShader(VkDevice d,VkShaderModule shader,const VkAllocationCallbacks* a){auto s=state(d);std::lock_guard<std::recursive_mutex> lock(s->mutex);s->shaders.erase(shader);FN(vkDestroyShaderModule)(d,shader,a);}
VKAPI_ATTR VkResult VKAPI_CALL createImage(VkDevice d,const VkImageCreateInfo* i,const VkAllocationCallbacks* a,VkImage* out){RESULT_BEGIN
    auto info=*i;
    if(stereoImage(info)&&(info.usage&VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))info.usage|=VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    return s->images.create(d,info,a,out,FN(vkCreateImage));
RESULT_END}
VKAPI_ATTR void VKAPI_CALL destroyImage(VkDevice d,VkImage image,const VkAllocationCallbacks* a){auto s=state(d);s->images.destroy(d,image,a,FN(vkDestroyImage));}
VKAPI_ATTR VkResult VKAPI_CALL createView(VkDevice d,const VkImageViewCreateInfo* i,const VkAllocationCallbacks* a,VkImageView* out){RESULT_BEGIN
    auto info=s->images.shaderViewInfo(*i);auto r=FN(vkCreateImageView)(d,&info,a,out);if(r==VK_SUCCESS){s->viewLayers[*out]=(s->images.layers(i->image)==2&&info.subresourceRange.baseArrayLayer==0&&info.subresourceRange.layerCount>=2)?2:1;if(!info.pNext)s->viewInfos[*out]=info;}return r;
RESULT_END}
VKAPI_ATTR void VKAPI_CALL destroyView(VkDevice d,VkImageView view,const VkAllocationCallbacks* a){auto s=state(d);std::lock_guard<std::recursive_mutex> lock(s->mutex);auto eyes=s->eyeViews.find(view);if(eyes!=s->eyeViews.end()){for(auto eye:eyes->second)if(eye)FN(vkDestroyImageView)(d,eye,nullptr);s->eyeViews.erase(eyes);}s->viewInfos.erase(view);s->viewLayers.erase(view);FN(vkDestroyImageView)(d,view,a);}
VKAPI_ATTR VkResult VKAPI_CALL createPass(VkDevice d,const VkRenderPassCreateInfo* i,const VkAllocationCallbacks* a,VkRenderPass* out){RESULT_BEGIN
    RenderPassPlan plan(*i,true);if(!plan.valid())return VK_ERROR_FEATURE_NOT_PRESENT;
    auto r=FN(vkCreateRenderPass)(d,i,a,out);if(r!=VK_SUCCESS)return r;VkRenderPass stereo{};r=FN(vkCreateRenderPass)(d,&plan.info(),a,&stereo);
    if(r!=VK_SUCCESS){FN(vkDestroyRenderPass)(d,*out,a);*out=VK_NULL_HANDLE;return r;}s->passes[*out]=stereo;return VK_SUCCESS;
RESULT_END}
VKAPI_ATTR void VKAPI_CALL destroyPass(VkDevice d,VkRenderPass pass,const VkAllocationCallbacks* a){auto s=state(d);std::lock_guard<std::recursive_mutex> lock(s->mutex);auto it=s->passes.find(pass);if(it!=s->passes.end()){FN(vkDestroyRenderPass)(d,it->second,a);s->passes.erase(it);}FN(vkDestroyRenderPass)(d,pass,a);}
VKAPI_ATTR VkResult VKAPI_CALL createFramebuffer(VkDevice d,const VkFramebufferCreateInfo* i,const VkAllocationCallbacks* a,VkFramebuffer* out){RESULT_BEGIN
    auto info=*i;bool stereo=i->attachmentCount!=0;
    if(i->flags&VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT)return VK_ERROR_FEATURE_NOT_PRESENT;
    for(uint32_t j=0;j<i->attachmentCount;++j){auto v=s->viewLayers.find(i->pAttachments[j]);if(v==s->viewLayers.end()||v->second<2)stereo=false;}
    auto pass=s->passes.find(i->renderPass);if(pass==s->passes.end())return VK_ERROR_INITIALIZATION_FAILED;
    if(stereo)info.renderPass=pass->second;auto r=FN(vkCreateFramebuffer)(d,&info,a,out);if(r==VK_SUCCESS)s->framebufferStereo[*out]=stereo;return r;
RESULT_END}
VKAPI_ATTR void VKAPI_CALL destroyFramebuffer(VkDevice d,VkFramebuffer fb,const VkAllocationCallbacks* a){auto s=state(d);std::lock_guard<std::recursive_mutex> lock(s->mutex);s->framebufferStereo.erase(fb);FN(vkDestroyFramebuffer)(d,fb,a);}
VKAPI_ATTR VkResult VKAPI_CALL createLayout(VkDevice d,const VkDescriptorSetLayoutCreateInfo* i,const VkAllocationCallbacks* a,VkDescriptorSetLayout* out){RESULT_BEGIN
    std::vector<VkDescriptorSetLayoutBinding> bindings;if(i->bindingCount)bindings.assign(i->pBindings,i->pBindings+i->bindingCount);uint32_t dynamic=0;
    for(const auto& b:bindings){if(b.binding==30||b.binding==31)throw std::runtime_error("Game descriptor binding 30/31 conflicts with SFS");if(b.descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC||b.descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)dynamic+=b.descriptorCount;}
    if(i->pNext)throw std::runtime_error("SFS descriptor layout extension chain not supported by probe");
    for(uint32_t binding:{30u,31u})bindings.push_back({binding,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_COMPUTE_BIT,nullptr});auto info=*i;info.bindingCount=uint32_t(bindings.size());info.pBindings=bindings.data();auto r=FN(vkCreateDescriptorSetLayout)(d,&info,a,out);if(r==VK_SUCCESS)s->dynamicCounts[*out]=dynamic;return r;
RESULT_END}
VKAPI_ATTR void VKAPI_CALL destroyLayout(VkDevice d,VkDescriptorSetLayout layout,const VkAllocationCallbacks* a){auto s=state(d);std::lock_guard<std::recursive_mutex> lock(s->mutex);s->dynamicCounts.erase(layout);FN(vkDestroyDescriptorSetLayout)(d,layout,a);}
VKAPI_ATTR VkResult VKAPI_CALL createPool(VkDevice d,const VkDescriptorPoolCreateInfo* i,const VkAllocationCallbacks* a,VkDescriptorPool* out){RESULT_BEGIN
    if(i->maxSets>UINT32_MAX/2)return VK_ERROR_OUT_OF_HOST_MEMORY;
    std::vector<VkDescriptorPoolSize> sizes;if(i->poolSizeCount)sizes.assign(i->pPoolSizes,i->pPoolSizes+i->poolSizeCount);sizes.push_back({VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,i->maxSets*2});auto info=*i;info.poolSizeCount=uint32_t(sizes.size());info.pPoolSizes=sizes.data();return FN(vkCreateDescriptorPool)(d,&info,a,out);
RESULT_END}
VKAPI_ATTR VkResult VKAPI_CALL allocateSets(VkDevice d,const VkDescriptorSetAllocateInfo* i,VkDescriptorSet* out){RESULT_BEGIN
    auto r=FN(vkAllocateDescriptorSets)(d,i,out);if(r!=VK_SUCCESS)return r;
    VkDescriptorBufferInfo buffer{s->params,0,sizeof(FrameUniforms)};
    std::vector<VkWriteDescriptorSet> writes(size_t(i->descriptorSetCount)*2);
    for(uint32_t j=0;j<i->descriptorSetCount;++j){s->setPools[out[j]]=i->descriptorPool;s->setDynamicCounts[out[j]]=s->dynamicCounts.at(i->pSetLayouts[j]);for(uint32_t k=0;k<2;++k){auto& w=writes[size_t(j)*2+k];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=out[j];w.dstBinding=30+k;w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;w.pBufferInfo=&buffer;}}
    FN(vkUpdateDescriptorSets)(d,uint32_t(writes.size()),writes.data(),0,nullptr);return VK_SUCCESS;
RESULT_END}
void forgetPool(const std::shared_ptr<State>& s,VkDescriptorPool pool){
    for(auto it=s->setPools.begin();it!=s->setPools.end();)if(it->second==pool){s->setDynamicCounts.erase(it->first);it=s->setPools.erase(it);}else ++it;
}
VKAPI_ATTR VkResult VKAPI_CALL freeSets(VkDevice d,VkDescriptorPool pool,uint32_t count,const VkDescriptorSet* sets){RESULT_BEGIN
    auto r=FN(vkFreeDescriptorSets)(d,pool,count,sets);if(r==VK_SUCCESS)for(uint32_t i=0;i<count;++i){s->setDynamicCounts.erase(sets[i]);s->setPools.erase(sets[i]);}return r;
RESULT_END}
VKAPI_ATTR VkResult VKAPI_CALL resetPool(VkDevice d,VkDescriptorPool pool,VkDescriptorPoolResetFlags flags){RESULT_BEGIN
    auto r=FN(vkResetDescriptorPool)(d,pool,flags);if(r==VK_SUCCESS)forgetPool(s,pool);return r;
RESULT_END}
VKAPI_ATTR void VKAPI_CALL destroyPool(VkDevice d,VkDescriptorPool pool,const VkAllocationCallbacks* allocator){
    auto s=state(d);std::lock_guard<std::recursive_mutex> lock(s->mutex);forgetPool(s,pool);FN(vkDestroyDescriptorPool)(d,pool,allocator);
}
VKAPI_ATTR VkResult VKAPI_CALL graphics(VkDevice d,VkPipelineCache cache,uint32_t count,const VkGraphicsPipelineCreateInfo* infos,const VkAllocationCallbacks* a,VkPipeline* out){RESULT_BEGIN
    for(uint32_t j=0;j<count;++j)out[j]=VK_NULL_HANDLE;
    for(uint32_t j=0;j<count;++j){auto info=infos[j];std::vector<VkPipelineShaderStageCreateInfo> stages(info.pStages,info.pStages+info.stageCount);const auto seed=pipelineSeed(info);
        for(auto& stage:stages){const auto& code=s->shaders.at(stage.module);const auto variant=profileHash(code.data(),uint32_t(code.size()*4),seed);bool compute{};stage.module=compiledModule(s,stage.module,variant,compute);}info.pStages=stages.data();
        // Derivative batch indices must not escape their original batch.
        if(info.flags&VK_PIPELINE_CREATE_DERIVATIVE_BIT)throw std::runtime_error("SFS derivative pipelines need batch remapping");
        auto r=FN(vkCreateGraphicsPipelines)(d,cache,1,&info,a,&out[j]);if(r!=VK_SUCCESS)return r;
        info.renderPass=s->passes.at(info.renderPass);VkPipeline stereo{};r=FN(vkCreateGraphicsPipelines)(d,cache,1,&info,a,&stereo);if(r!=VK_SUCCESS){FN(vkDestroyPipeline)(d,out[j],a);out[j]=VK_NULL_HANDLE;return r;}s->stereoPipelines[out[j]]=stereo;
    }return VK_SUCCESS;
RESULT_END}
VKAPI_ATTR VkResult VKAPI_CALL compute(VkDevice d,VkPipelineCache cache,uint32_t count,const VkComputePipelineCreateInfo* infos,const VkAllocationCallbacks* a,VkPipeline* out){RESULT_BEGIN
    for(uint32_t j=0;j<count;++j)out[j]=VK_NULL_HANDLE;
    for(uint32_t j=0;j<count;++j){auto info=infos[j];bool stereo{};info.stage.module=compiledModule(s,info.stage.module,0,stereo);if(info.flags&VK_PIPELINE_CREATE_DERIVATIVE_BIT)return VK_ERROR_FEATURE_NOT_PRESENT;auto r=FN(vkCreateComputePipelines)(d,cache,1,&info,a,&out[j]);if(r!=VK_SUCCESS)return r;s->computeStereo[out[j]]=stereo;}return VK_SUCCESS;
RESULT_END}
VKAPI_ATTR void VKAPI_CALL destroyPipeline(VkDevice d,VkPipeline pipeline,const VkAllocationCallbacks* a){auto s=state(d);std::lock_guard<std::recursive_mutex> lock(s->mutex);auto it=s->stereoPipelines.find(pipeline);if(it!=s->stereoPipelines.end()){FN(vkDestroyPipeline)(d,it->second,a);s->stereoPipelines.erase(it);}s->computeStereo.erase(pipeline);FN(vkDestroyPipeline)(d,pipeline,a);}
VKAPI_ATTR VkResult VKAPI_CALL beginCommand(VkCommandBuffer cb,const VkCommandBufferBeginInfo* i){try{auto s=state(cb);std::lock_guard<std::recursive_mutex> lock(s->mutex);if(i->pInheritanceInfo&&i->pInheritanceInfo->renderPass)return VK_ERROR_FEATURE_NOT_PRESENT;s->commands[cb]={};return FN(vkBeginCommandBuffer)(cb,i);}catch(const std::exception& e){note(e.what());return VK_ERROR_INITIALIZATION_FAILED;}}
VKAPI_ATTR VkResult VKAPI_CALL allocateCommands(VkDevice d,const VkCommandBufferAllocateInfo* i,VkCommandBuffer* out){RESULT_BEGIN
    auto r=FN(vkAllocateCommandBuffers)(d,i,out);if(r==VK_SUCCESS)for(uint32_t j=0;j<i->commandBufferCount;++j)s->commandPools[out[j]]=i->commandPool;return r;
RESULT_END}
VKAPI_ATTR void VKAPI_CALL freeCommands(VkDevice d,VkCommandPool pool,uint32_t count,const VkCommandBuffer* commands){auto s=state(d);std::lock_guard<std::recursive_mutex> lock(s->mutex);for(uint32_t j=0;j<count;++j){s->commands.erase(commands[j]);s->commandPools.erase(commands[j]);}FN(vkFreeCommandBuffers)(d,pool,count,commands);}
VKAPI_ATTR void VKAPI_CALL destroyCommandPool(VkDevice d,VkCommandPool pool,const VkAllocationCallbacks* a){auto s=state(d);std::lock_guard<std::recursive_mutex> lock(s->mutex);for(auto it=s->commandPools.begin();it!=s->commandPools.end();)if(it->second==pool){s->commands.erase(it->first);it=s->commandPools.erase(it);}else ++it;FN(vkDestroyCommandPool)(d,pool,a);}
VKAPI_ATTR void VKAPI_CALL bindPipeline(VkCommandBuffer cb,VkPipelineBindPoint point,VkPipeline pipeline){COMMAND_BEGIN
    auto& command=s->commands[cb];if(point==VK_PIPELINE_BIND_POINT_COMPUTE){command.compute=pipeline;FN(vkCmdBindPipeline)(cb,point,pipeline);return;}
    auto bind=[s,cb,pipeline]{auto& c=s->commands[cb];auto found=s->stereoPipelines.find(pipeline);if(found==s->stereoPipelines.end())throw std::runtime_error("Untracked SFS graphics pipeline");s->fn<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,c.stereo?found->second:pipeline);};command.bindings[0]=bind;bind();
COMMAND_END}
VKAPI_ATTR void VKAPI_CALL bindSets(VkCommandBuffer cb,VkPipelineBindPoint point,VkPipelineLayout layout,uint32_t first,uint32_t count,const VkDescriptorSet* sets,uint32_t dynamicCount,const uint32_t* dynamic){COMMAND_BEGIN
    uint32_t offset=0;for(uint32_t j=0;j<count;++j){auto set=sets[j];const auto n=s->setDynamicCounts.at(set);if(n>dynamicCount-offset)throw std::runtime_error("SFS dynamic descriptor offset mismatch");std::vector<uint32_t> values;if(n)values.assign(dynamic+offset,dynamic+offset+n);offset+=n;
        if(point==VK_PIPELINE_BIND_POINT_GRAPHICS)s->commands[cb].bindings[0x10000ull+first+j]=[s,cb,point,layout,index=first+j,set,values]{s->fn<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(cb,point,layout,index,1,&set,uint32_t(values.size()),values.data());};}
    if(offset!=dynamicCount)throw std::runtime_error("SFS unexpected dynamic descriptor offsets");FN(vkCmdBindDescriptorSets)(cb,point,layout,first,count,sets,dynamicCount,dynamic);
COMMAND_END}
VKAPI_ATTR void VKAPI_CALL bindVertices(VkCommandBuffer cb,uint32_t first,uint32_t count,const VkBuffer* buffers,const VkDeviceSize* offsets){COMMAND_BEGIN
    for(uint32_t j=0;j<count;++j)s->commands[cb].bindings[0x20000ull+first+j]=[s,cb,index=first+j,b= buffers[j],o=offsets[j]]{s->fn<PFN_vkCmdBindVertexBuffers>("vkCmdBindVertexBuffers")(cb,index,1,&b,&o);};FN(vkCmdBindVertexBuffers)(cb,first,count,buffers,offsets);
COMMAND_END}
VKAPI_ATTR void VKAPI_CALL bindIndex(VkCommandBuffer cb,VkBuffer buffer,VkDeviceSize offset,VkIndexType type){COMMAND_BEGIN
    auto f=[s,cb,buffer,offset,type]{s->fn<PFN_vkCmdBindIndexBuffer>("vkCmdBindIndexBuffer")(cb,buffer,offset,type);};s->commands[cb].bindings[0x30000]=f;f();
COMMAND_END}
VKAPI_ATTR void VKAPI_CALL viewport(VkCommandBuffer cb,uint32_t first,uint32_t count,const VkViewport* values){COMMAND_BEGIN
    for(uint32_t j=0;j<count;++j)s->commands[cb].bindings[0x40000ull+first+j]=[s,cb,index=first+j,v=values[j]]{s->fn<PFN_vkCmdSetViewport>("vkCmdSetViewport")(cb,index,1,&v);};FN(vkCmdSetViewport)(cb,first,count,values);
COMMAND_END}
VKAPI_ATTR void VKAPI_CALL scissor(VkCommandBuffer cb,uint32_t first,uint32_t count,const VkRect2D* values){COMMAND_BEGIN
    for(uint32_t j=0;j<count;++j)s->commands[cb].bindings[0x50000ull+first+j]=[s,cb,index=first+j,v=values[j]]{s->fn<PFN_vkCmdSetScissor>("vkCmdSetScissor")(cb,index,1,&v);};FN(vkCmdSetScissor)(cb,first,count,values);
COMMAND_END}
VKAPI_ATTR void VKAPI_CALL push(VkCommandBuffer cb,VkPipelineLayout layout,VkShaderStageFlags flags,uint32_t offset,uint32_t size,const void* values){COMMAND_BEGIN
    for(uint32_t stage=1;stage<=VK_SHADER_STAGE_COMPUTE_BIT;stage<<=1)if(flags&stage)for(uint32_t pos=0;pos<size;pos+=4){uint32_t value{};std::memcpy(&value,static_cast<const char*>(values)+pos,4);auto key=0x100000ull+uint64_t(stage)*0x10000+offset+pos;
        s->commands[cb].bindings[key]=[s,cb,layout,stage,at=offset+pos,value]{s->fn<PFN_vkCmdPushConstants>("vkCmdPushConstants")(cb,layout,stage,at,4,&value);};}FN(vkCmdPushConstants)(cb,layout,flags,offset,size,values);
COMMAND_END}
VKAPI_ATTR void VKAPI_CALL beginPass(VkCommandBuffer cb,const VkRenderPassBeginInfo* i,VkSubpassContents contents){COMMAND_BEGIN
    auto info=*i;auto& command=s->commands[cb];command.stereo=s->framebufferStereo.at(i->framebuffer);if(command.stereo)info.renderPass=s->passes.at(i->renderPass);FN(vkCmdBeginRenderPass)(cb,&info,contents);for(const auto& binding:command.bindings)binding.second();
COMMAND_END}
VKAPI_ATTR void VKAPI_CALL endPass(VkCommandBuffer cb){COMMAND_BEGIN FN(vkCmdEndRenderPass)(cb);s->commands[cb].stereo=false;COMMAND_END}
VKAPI_ATTR void VKAPI_CALL nextPass(VkCommandBuffer cb,VkSubpassContents contents){COMMAND_BEGIN FN(vkCmdNextSubpass)(cb,contents);for(const auto& binding:s->commands[cb].bindings)binding.second();COMMAND_END}
VKAPI_ATTR void VKAPI_CALL lineWidth(VkCommandBuffer cb,float width){COMMAND_BEGIN
    auto f=[s,cb,width]{s->fn<PFN_vkCmdSetLineWidth>("vkCmdSetLineWidth")(cb,width);};s->commands[cb].bindings[0x60000]=f;f();
COMMAND_END}
VKAPI_ATTR void VKAPI_CALL depthBias(VkCommandBuffer cb,float constant,float clamp,float slope){COMMAND_BEGIN
    auto f=[s,cb,constant,clamp,slope]{s->fn<PFN_vkCmdSetDepthBias>("vkCmdSetDepthBias")(cb,constant,clamp,slope);};s->commands[cb].bindings[0x60001]=f;f();
COMMAND_END}
VKAPI_ATTR void VKAPI_CALL blendConstants(VkCommandBuffer cb,const float* values){COMMAND_BEGIN
    const std::array<float,4> constants{values[0],values[1],values[2],values[3]};auto f=[s,cb,constants]{s->fn<PFN_vkCmdSetBlendConstants>("vkCmdSetBlendConstants")(cb,constants.data());};s->commands[cb].bindings[0x60002]=f;f();
COMMAND_END}
VKAPI_ATTR void VKAPI_CALL depthBounds(VkCommandBuffer cb,float min,float max){COMMAND_BEGIN
    auto f=[s,cb,min,max]{s->fn<PFN_vkCmdSetDepthBounds>("vkCmdSetDepthBounds")(cb,min,max);};s->commands[cb].bindings[0x60003]=f;f();
COMMAND_END}
#define STENCIL_WRAPPER(handler,api,slot) \
VKAPI_ATTR void VKAPI_CALL handler(VkCommandBuffer cb,VkStencilFaceFlags faces,uint32_t value){COMMAND_BEGIN \
    for(uint32_t face=VK_STENCIL_FACE_FRONT_BIT;face<=VK_STENCIL_FACE_BACK_BIT;face<<=1)if(faces&face){auto f=[s,cb,face,value]{s->fn<PFN_##api>(#api)(cb,face,value);};s->commands[cb].bindings[slot+face]=f;f();} \
COMMAND_END}
STENCIL_WRAPPER(stencilCompare,vkCmdSetStencilCompareMask,0x61000)
STENCIL_WRAPPER(stencilWrite,vkCmdSetStencilWriteMask,0x62000)
STENCIL_WRAPPER(stencilReference,vkCmdSetStencilReference,0x63000)
#undef STENCIL_WRAPPER
VKAPI_ATTR void VKAPI_CALL dispatch(VkCommandBuffer cb,uint32_t x,uint32_t y,uint32_t z){COMMAND_BEGIN
    uint32_t depth{};if(!dispatchDepth(z,s->computeStereo.at(s->commands[cb].compute),65535,depth))throw std::runtime_error("Stereo dispatch exceeds limit");FN(vkCmdDispatch)(cb,x,y,depth);
COMMAND_END}
VkImageSubresourceRange range(const std::shared_ptr<State>& s,VkImage image,VkImageSubresourceRange value){if(s->images.layers(image)==2&&value.baseArrayLayer==0&&value.layerCount==1)value.layerCount=2;return value;}
VKAPI_ATTR void VKAPI_CALL barriers(VkCommandBuffer cb,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags deps,uint32_t nm,const VkMemoryBarrier* m,uint32_t nb,const VkBufferMemoryBarrier* b,uint32_t ni,const VkImageMemoryBarrier* i){COMMAND_BEGIN
    std::vector<VkImageMemoryBarrier> images;if(ni)images.assign(i,i+ni);for(auto& image:images)image.subresourceRange=range(s,image.image,image.subresourceRange);FN(vkCmdPipelineBarrier)(cb,src,dst,deps,nm,m,nb,b,ni,images.data());
COMMAND_END}
VKAPI_ATTR void VKAPI_CALL clearColor(VkCommandBuffer cb,VkImage image,VkImageLayout layout,const VkClearColorValue* value,uint32_t count,const VkImageSubresourceRange* ranges){COMMAND_BEGIN
    std::vector<VkImageSubresourceRange> copies(ranges,ranges+count);for(auto& r:copies)r=range(s,image,r);FN(vkCmdClearColorImage)(cb,image,layout,value,count,copies.data());
COMMAND_END}
VKAPI_ATTR void VKAPI_CALL clearDepth(VkCommandBuffer cb,VkImage image,VkImageLayout layout,const VkClearDepthStencilValue* value,uint32_t count,const VkImageSubresourceRange* ranges){COMMAND_BEGIN
    std::vector<VkImageSubresourceRange> copies(ranges,ranges+count);for(auto& r:copies)r=range(s,image,r);FN(vkCmdClearDepthStencilImage)(cb,image,layout,value,count,copies.data());
COMMAND_END}
template<class T>std::vector<T> copyRegions(const std::shared_ptr<State>& s,VkImage src,VkImage dst,uint32_t count,const T* regions){
    std::vector<T> result;for(uint32_t j=0;j<count;++j){auto region=regions[j];result.push_back(region);if(s->images.layers(dst)==2&&region.dstSubresource.baseArrayLayer==0&&region.dstSubresource.layerCount==1){region.dstSubresource.baseArrayLayer=1;if(s->images.layers(src)==2)region.srcSubresource.baseArrayLayer=1;result.push_back(region);}}return result;
}
VKAPI_ATTR void VKAPI_CALL copyImage(VkCommandBuffer cb,VkImage src,VkImageLayout sl,VkImage dst,VkImageLayout dl,uint32_t count,const VkImageCopy* regions){COMMAND_BEGIN auto r=copyRegions(s,src,dst,count,regions);FN(vkCmdCopyImage)(cb,src,sl,dst,dl,uint32_t(r.size()),r.data());COMMAND_END}
VKAPI_ATTR void VKAPI_CALL blitImage(VkCommandBuffer cb,VkImage src,VkImageLayout sl,VkImage dst,VkImageLayout dl,uint32_t count,const VkImageBlit* regions,VkFilter filter){COMMAND_BEGIN auto r=copyRegions(s,src,dst,count,regions);FN(vkCmdBlitImage)(cb,src,sl,dst,dl,uint32_t(r.size()),r.data(),filter);COMMAND_END}
VKAPI_ATTR void VKAPI_CALL resolveImage(VkCommandBuffer cb,VkImage src,VkImageLayout sl,VkImage dst,VkImageLayout dl,uint32_t count,const VkImageResolve* regions){COMMAND_BEGIN auto r=copyRegions(s,src,dst,count,regions);FN(vkCmdResolveImage)(cb,src,sl,dst,dl,uint32_t(r.size()),r.data());COMMAND_END}
VKAPI_ATTR void VKAPI_CALL uploadImage(VkCommandBuffer cb,VkBuffer buffer,VkImage image,VkImageLayout layout,uint32_t count,const VkBufferImageCopy* regions){COMMAND_BEGIN
    std::vector<VkBufferImageCopy> copies;for(uint32_t j=0;j<count;++j){auto r=regions[j];copies.push_back(r);if(s->images.layers(image)==2&&r.imageSubresource.baseArrayLayer==0&&r.imageSubresource.layerCount==1){r.imageSubresource.baseArrayLayer=1;copies.push_back(r);}}FN(vkCmdCopyBufferToImage)(cb,buffer,image,layout,uint32_t(copies.size()),copies.data());
COMMAND_END}
}
bool nativeProbeEnabled(){static const bool enabled=[] {char value[8]{};return GetEnvironmentVariableA("KHARVOX_SFS_NATIVE_PROBE",value,8)==1&&value[0]=='1';}();return enabled;}
bool initialize(VkDevice d,VkPhysicalDevice,PFN_vkGetDeviceProcAddr gdpa,const VkPhysicalDeviceMemoryProperties& memory){
    if(!nativeProbeEnabled())return true;
    auto s=std::make_shared<State>();
    try{s->device=d;s->gdpa=gdpa;wchar_t path[32768]{};auto n=GetEnvironmentVariableW(L"KHARVOX_SFS_PROFILE",path,32768);if(n&&n<32768)s->profile=path;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=sizeof(FrameUniforms);bi.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;if(FN(vkCreateBuffer)(d,&bi,nullptr,&s->params)!=VK_SUCCESS)return false;
        VkMemoryRequirements r{};FN(vkGetBufferMemoryRequirements)(d,s->params,&r);uint32_t index=UINT32_MAX;for(uint32_t j=0;j<memory.memoryTypeCount;++j)if((r.memoryTypeBits&(1u<<j))&&(memory.memoryTypes[j].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){index=j;break;}
        if(index==UINT32_MAX)throw std::runtime_error("No coherent SFS parameter memory");VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=r.size;ai.memoryTypeIndex=index;if(FN(vkAllocateMemory)(d,&ai,nullptr,&s->paramsMemory)!=VK_SUCCESS)throw std::runtime_error("SFS parameter allocation failed");if(FN(vkBindBufferMemory)(d,s->params,s->paramsMemory,0)!=VK_SUCCESS)throw std::runtime_error("SFS parameter bind failed");
        void* mapped{};if(FN(vkMapMemory)(d,s->paramsMemory,0,sizeof(FrameUniforms),0,&mapped)!=VK_SUCCESS)throw std::runtime_error("SFS parameter map failed");FrameUniforms initial;std::memcpy(mapped,&initial,sizeof(initial));FN(vkUnmapMemory)(d,s->paramsMemory);
        {std::lock_guard<std::mutex> lock(devicesMutex);devices[dispatchKey(d)]=s;}note(vrEnabled()?"native SFS experimental OpenXR producer initialized":"native multiview probe initialized; fixed identity projection; NOT VR");return true;
    }catch(const std::exception& e){note(e.what());if(s->params)FN(vkDestroyBuffer)(d,s->params,nullptr);if(s->paramsMemory)FN(vkFreeMemory)(d,s->paramsMemory,nullptr);return false;}
}
void shutdown(VkDevice d){if(!nativeProbeEnabled())return;std::shared_ptr<State> s;try{s=state(d);}catch(const std::exception&){return;}std::lock_guard<std::recursive_mutex> lock(s->mutex);s->commands.clear();for(auto& entry:s->eyeViews)for(auto eye:entry.second)if(eye)FN(vkDestroyImageView)(d,eye,nullptr);s->eyeViews.clear();for(auto& module:s->compiled)FN(vkDestroyShaderModule)(d,module.second,nullptr);FN(vkDestroyBuffer)(d,s->params,nullptr);FN(vkFreeMemory)(d,s->paramsMemory,nullptr);std::lock_guard<std::mutex> devicesLock(devicesMutex);for(auto it=devices.begin();it!=devices.end();)if(it->second==s)it=devices.erase(it);else ++it;}
bool vrEnabled(){static const bool enabled=[] {char value[8]{};return GetEnvironmentVariableA("KHARVOX_SFS_NATIVE_VR",value,8)==1&&value[0]=='1';}();return nativeProbeEnabled()&&enabled;}
bool eyeAttachmentView(VkDevice d,VkImageView original,uint32_t eye,VkImageView& result){
    result=VK_NULL_HANDLE;if(!nativeProbeEnabled()||eye>1)return false;
    auto s=state(d);std::lock_guard<std::recursive_mutex> lock(s->mutex);
    auto found=s->viewInfos.find(original);if(found==s->viewInfos.end())return false;
    auto info=found->second;
    if(info.viewType!=VK_IMAGE_VIEW_TYPE_2D_ARRAY||info.subresourceRange.layerCount<2)return false;
    auto& cached=s->eyeViews[original][eye];
    if(!cached){info.viewType=VK_IMAGE_VIEW_TYPE_2D;info.subresourceRange.baseArrayLayer+=eye;info.subresourceRange.layerCount=1;
        if(FN(vkCreateImageView)(d,&info,nullptr,&cached)!=VK_SUCCESS)return false;}
    result=cached;return true;
}
void prepare(VkDevice d,const native::FramePose& pose,const XrFovf& source){
    if(!vrEnabled())return;auto s=state(d);std::lock_guard<std::recursive_mutex> lock(s->mutex);
    FrameUniforms uniforms;
    if(!frameProjection(pose.head,source,pose.views,pose.worldScale,pose.gameplay||pose.cinematic||pose.scripted,uniforms))commandFailure("SFS headset projection unsupported: requires parallel eye cameras");
    s->pendingUniforms=uniforms;s->pendingPose=pose;s->pending=true;
}
void copyCompleted(VkDevice d){if(!vrEnabled())return;auto s=state(d);std::lock_guard<std::recursive_mutex> lock(s->mutex);s->completed=true;}
void beginFrame(VkDevice d){
    if(!vrEnabled())return;auto s=state(d);std::lock_guard<std::recursive_mutex> lock(s->mutex);
    if(!s->pending||!s->completed)return;
    // The prototype shares a uniform buffer across recorded command buffers.
    // Retire all previous readers before writing; a frame ring can replace this
    // conservative wait once multiple queued game frames have explicit ownership.
    if(FN(vkDeviceWaitIdle)(d)!=VK_SUCCESS)commandFailure("SFS frame parameter retirement failed");
    void* mapped{};if(FN(vkMapMemory)(d,s->paramsMemory,0,sizeof(FrameUniforms),0,&mapped)!=VK_SUCCESS)commandFailure("SFS frame parameter map failed");
    std::memcpy(mapped,&s->pendingUniforms,sizeof(FrameUniforms));FN(vkUnmapMemory)(d,s->paramsMemory);
    s->renderPose=s->pendingPose;s->frameValid=true;s->pending=false;s->completed=false;
}
bool pair(VkDevice d,VkImage image,VkExtent2D extent,VkFormat format,native::StereoFrame& result){
    if(!vrEnabled())return false;auto s=state(d);std::lock_guard<std::recursive_mutex> lock(s->mutex);
    if(!s->frameValid||s->images.layers(image)!=2)return false;
    result={};result.pose=s->renderPose;result.generation=s->renderPose.serial;
    for(uint32_t e=0;e<2;++e)result.eyes[e]={image,extent,format,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,s->renderPose.views[e].pose,s->renderPose.views[e].fov,e,s->renderPose.serial};
    return true;
}
void swapchainImages(VkDevice d,VkSwapchainKHR chain,uint32_t count,const VkImage* images){
    if(!nativeProbeEnabled())return;
    auto s=state(d);std::lock_guard<std::recursive_mutex> lock(s->mutex);
    auto& tracked=s->swapchains[chain];
    for(auto image:tracked)s->images.destroy(d,image,nullptr,nullptr);
    tracked.assign(images,images+count);
    for(auto image:tracked)s->images.track(image,2);
}
void swapchainDestroyed(VkDevice d,VkSwapchainKHR chain){
    if(!nativeProbeEnabled())return;
    auto s=state(d);std::lock_guard<std::recursive_mutex> lock(s->mutex);
    auto found=s->swapchains.find(chain);if(found==s->swapchains.end())return;
    for(auto image:found->second)s->images.destroy(d,image,nullptr,nullptr);
    s->swapchains.erase(found);
}
PFN_vkVoidFunction wrapProc(VkDevice d,const char* name,PFN_vkVoidFunction next){if(!nativeProbeEnabled()||!next)return next;
    try{state(d);}catch(const std::exception&){return next;}
#define HOOK(api,handler) if(!std::strcmp(name,#api))return reinterpret_cast<PFN_vkVoidFunction>(&handler)
    HOOK(vkCreateShaderModule,createShader);HOOK(vkDestroyShaderModule,destroyShader);
    HOOK(vkCreateImage,createImage);HOOK(vkDestroyImage,destroyImage);HOOK(vkCreateImageView,createView);HOOK(vkDestroyImageView,destroyView);
    HOOK(vkCreateRenderPass,createPass);HOOK(vkDestroyRenderPass,destroyPass);HOOK(vkCreateFramebuffer,createFramebuffer);HOOK(vkDestroyFramebuffer,destroyFramebuffer);
    HOOK(vkCreateDescriptorSetLayout,createLayout);HOOK(vkDestroyDescriptorSetLayout,destroyLayout);HOOK(vkCreateDescriptorPool,createPool);HOOK(vkAllocateDescriptorSets,allocateSets);
    HOOK(vkFreeDescriptorSets,freeSets);HOOK(vkResetDescriptorPool,resetPool);HOOK(vkDestroyDescriptorPool,destroyPool);
    HOOK(vkCreateGraphicsPipelines,graphics);HOOK(vkCreateComputePipelines,compute);HOOK(vkDestroyPipeline,destroyPipeline);
    HOOK(vkBeginCommandBuffer,beginCommand);HOOK(vkCmdBindPipeline,bindPipeline);HOOK(vkCmdBindDescriptorSets,bindSets);HOOK(vkCmdBindVertexBuffers,bindVertices);HOOK(vkCmdBindIndexBuffer,bindIndex);
    HOOK(vkAllocateCommandBuffers,allocateCommands);HOOK(vkFreeCommandBuffers,freeCommands);HOOK(vkDestroyCommandPool,destroyCommandPool);
    HOOK(vkCmdSetViewport,viewport);HOOK(vkCmdSetScissor,scissor);HOOK(vkCmdPushConstants,push);HOOK(vkCmdBeginRenderPass,beginPass);HOOK(vkCmdEndRenderPass,endPass);HOOK(vkCmdDispatch,dispatch);
    HOOK(vkCmdNextSubpass,nextPass);HOOK(vkCmdSetLineWidth,lineWidth);HOOK(vkCmdSetDepthBias,depthBias);HOOK(vkCmdSetBlendConstants,blendConstants);HOOK(vkCmdSetDepthBounds,depthBounds);
    HOOK(vkCmdSetStencilCompareMask,stencilCompare);HOOK(vkCmdSetStencilWriteMask,stencilWrite);HOOK(vkCmdSetStencilReference,stencilReference);
    HOOK(vkCmdPipelineBarrier,barriers);HOOK(vkCmdClearColorImage,clearColor);HOOK(vkCmdClearDepthStencilImage,clearDepth);
    HOOK(vkCmdCopyImage,copyImage);HOOK(vkCmdBlitImage,blitImage);HOOK(vkCmdResolveImage,resolveImage);HOOK(vkCmdCopyBufferToImage,uploadImage);
#undef HOOK
    return next;
}
}

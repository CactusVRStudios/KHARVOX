#include "NativeSfs.h"
#include "NativeDispatch.h"
#include "SourceRing.h"
#include "PushReplay.h"
#include "CommandCpuTiming.h"
#include "ShaderCompiler.h"
#include "ShaderProfile.h"
#include "PipelineIdentity.h"
#include "ShadowProjection.h"
#include "StereoResources.h"
#include "FrameProjection.h"
#include "SourcePoseHistory.h"
#include "../native/NativeStereo.h"
#include "../common/RuntimeLog.h"
#include "../common/DiagnosticLogging.h"
#include <windows.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
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
    VkPipeline graphics{};
    struct Descriptor {VkPipelineLayout layout{};VkDescriptorSet set{};std::vector<uint32_t> dynamic;};
    std::vector<Descriptor> descriptors;
    std::map<uint64_t,std::function<void()>> bindings;
    PushReplay pushes;
    std::vector<VkImageMemoryBarrier> imageBarriers;
    std::vector<VkImageSubresourceRange> clearRanges;
};
struct State {
    VkDevice device{};NativeDispatch dispatch;
    std::unique_ptr<SourceRing> sources;
    // Command buffers are externally synchronized by Vulkan callers. Parallel
    // recorders share resource metadata; only creation/retirement needs exclusivity.
    std::shared_mutex mutex;
    Images images;
    std::unordered_map<VkSwapchainKHR,std::vector<VkImage>> swapchains;
    VkBuffer params{};VkDeviceMemory paramsMemory{};
    FrameUniforms pendingUniforms{};
    native::FramePose pendingPose{},renderPose{};
    std::unordered_map<VkImage,native::FramePose> imagePoses;
    std::unordered_map<VkImage,FrameUniforms> imageUniforms;
    SourcePoseHistory sourcePoseHistory;
    FrameUniforms renderUniforms;
    uint64_t sourcePoseSamples{};
    bool pending{},completed{true},frameValid{};
    bool profileTiming{};
    uint64_t profiledFrames{},retireNs{},uploadNs{},maxRetireNs{};
    std::unordered_map<VkShaderModule,std::vector<uint32_t>> shaders;
    std::unordered_map<std::string,VkShaderModule> compiled;
    std::unordered_map<VkRenderPass,VkRenderPass> passes;
    std::unordered_map<VkImageView,uint32_t> viewLayers;
    std::unordered_map<VkImageView,VkImageViewCreateInfo> viewInfos;
    std::unordered_map<VkImageView,std::array<VkImageView,2>> eyeViews;
    std::unordered_map<VkFramebuffer,bool> framebufferStereo;
    std::unordered_map<VkPipeline,VkPipeline> stereoPipelines;
    std::unordered_map<VkPipeline,bool> computeStereo;
    std::unordered_map<VkPipeline,std::array<VkPipeline,2>> indirectPipelines;
    std::unordered_map<VkFramebuffer,bool> mixedFramebuffers;
    std::atomic<uint64_t> indirectMono{},indirectStereo{},mixedPasses{};
    uint32_t mixedDiagnostics{};
    uint32_t materialDiagnostics{};
    std::unordered_map<VkDescriptorSetLayout,uint32_t> dynamicCounts;
    std::unordered_map<VkDescriptorSet,uint32_t> setDynamicCounts;
    std::unordered_map<VkDescriptorSet,VkDescriptorPool> setPools;
    std::unordered_map<VkCommandBuffer,CommandState> commands;
    std::unordered_map<VkCommandBuffer,VkCommandPool> commandPools;
    std::filesystem::path profile;
};
std::mutex devicesMutex;
std::unordered_map<void*,std::shared_ptr<State>> devices;
std::atomic<uint64_t> deviceGeneration{1};
template<class T>void* dispatchKey(T handle){return handle?*reinterpret_cast<void**>(handle):nullptr;}
template<class T>std::shared_ptr<State> state(T handle){
    struct Cache {void* key{};uint64_t generation{};std::weak_ptr<State> state;};
    thread_local Cache cache;
    const auto generation=deviceGeneration.load(std::memory_order_acquire);
    const auto key=dispatchKey(handle);
    if(cache.key==key&&cache.generation==generation){
        if(auto cached=cache.state.lock()){
            if constexpr(std::is_same_v<T,VkDevice>){if(cached->device==handle)return cached;}
            else return cached;
        }
    }
    std::lock_guard<std::mutex> lock(devicesMutex);
    if constexpr(std::is_same_v<T,VkDevice>){
        // The loader can replace a newly-created device's dispatch table after
        // our CreateDevice returns. Keep identity by the device handle as well.
        std::shared_ptr<State> found;
        for(const auto& entry:devices)if(entry.second->device==handle){found=entry.second;break;}
        if(found){devices[key]=found;cache={key,generation,found};return found;}
        throw std::runtime_error("Unregistered SFS device");
    }
    auto it=devices.find(key);if(it==devices.end())throw std::runtime_error("SFS device not initialized handle="+std::to_string(reinterpret_cast<uintptr_t>(handle))+" dispatch="+std::to_string(reinterpret_cast<uintptr_t>(key)));cache={key,generation,it->second};return it->second;
}
#define FN(name) NativeDispatch::require(s->dispatch.name,#name)
#define RESULT_BEGIN try {auto s=state(d);std::unique_lock<std::shared_mutex> lock(s->mutex);
#define RESULT_END }catch(const std::exception& e){note(std::string(__FUNCTION__)+": "+e.what());return VK_ERROR_INITIALIZATION_FAILED;}
// Command hooks cannot return VkResult. Fail the owned diagnostic process on an
// unsupported command rather than record an invalid command or wait forever.
[[noreturn]] void commandFailure(const char* message){note(message);RaiseFailFastException(nullptr,nullptr,0);std::terminate();}
#define COMMAND_BEGIN try {CommandCpuTiming timing;auto s=state(cb);std::shared_lock<std::shared_mutex> lock(s->mutex);timing.acquired();
#define COMMAND_END }catch(const std::exception& e){commandFailure(e.what());}

VkShaderModule compiledModule(const std::shared_ptr<State>& s,VkShaderModule original,uint64_t variant,bool& stereoCompute,bool shadowProjection=false,int indirectEye=-1){
    const auto found=s->shaders.find(original);if(found==s->shaders.end())throw std::runtime_error("Untracked game shader module");
    const auto& words=found->second;const auto primary=profileHash(words.data(),uint32_t(words.size()*4));
    VkShaderStageFlagBits stage{};
    for(size_t i=5;i<words.size();){auto n=words[i]>>16;if(!n||n>words.size()-i)throw std::runtime_error("Invalid game SPIR-V");if((words[i]&65535)==15){stage=words[i+1]==0?VK_SHADER_STAGE_VERTEX_BIT:words[i+1]==4?VK_SHADER_STAGE_FRAGMENT_BIT:words[i+1]==5?VK_SHADER_STAGE_COMPUTE_BIT:VkShaderStageFlagBits(0);break;}i+=n;}
    if(!stage)throw std::runtime_error("Unsupported SFS shader execution model");
    const auto replacement=loadProfileShader(s->profile,primary,variant,stage);
    const auto& input=replacement?replacement.words:words;
    stereoCompute=hasStereoStorageOutput(input);
    const bool sharedShadow=shadowProjection&&!replacement&&stage==VK_SHADER_STAGE_VERTEX_BIT;
    const auto key=shaderKey(primary)+"_"+shaderKey(variant)+(sharedShadow?"_shadow":"")+(indirectEye>=0?"_indirect"+std::to_string(indirectEye):"");
    auto cached=s->compiled.find(key);if(cached!=s->compiled.end())return cached->second;
    ShaderCompileOptions options;options.computeStereo=stereoCompute&&!replacement;
    options.profileReplacement=bool(replacement);
    options.screenSpaceUi=doomUiShader(primary);
    options.indirectEye=indirectEye;
    options.vertexProjection=!sharedShadow&&needsStereoProjection(input,bool(replacement));
    auto shader=compileStereoShader(input,options);
    note("shader="+key+" stage="+std::to_string(stage)+" profile="+(replacement?"matched":"generic")+
         " projection="+std::to_string(shader.vertexProjectionApplied)+" screenUi="+std::to_string(shader.screenSpaceUiApplied)+" sharedShadow="+std::to_string(sharedShadow)+" stereoCompute="+std::to_string(stereoCompute)+
         " clusters="+std::to_string(shader.clusterCorrections)+" world="+std::to_string(shader.worldCorrections)+" refraction="+std::to_string(shader.refractionCorrections)+" temporal="+std::to_string(shader.temporalCorrections));
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};info.codeSize=shader.words.size()*4;info.pCode=shader.words.data();
    VkShaderModule result{};auto r=FN(vkCreateShaderModule)(s->device,&info,nullptr,&result);
    if(r!=VK_SUCCESS)throw std::runtime_error("Stereo shader module creation failed");
    s->compiled.emplace(key,result);return result;
}
VKAPI_ATTR VkResult VKAPI_CALL createShader(VkDevice d,const VkShaderModuleCreateInfo* i,const VkAllocationCallbacks* a,VkShaderModule* out){RESULT_BEGIN
    auto r=FN(vkCreateShaderModule)(d,i,a,out);if(r==VK_SUCCESS)s->shaders[*out]={i->pCode,i->pCode+i->codeSize/4};return r;
RESULT_END}
VKAPI_ATTR void VKAPI_CALL destroyShader(VkDevice d,VkShaderModule shader,const VkAllocationCallbacks* a){auto s=state(d);std::unique_lock<std::shared_mutex> lock(s->mutex);s->shaders.erase(shader);FN(vkDestroyShaderModule)(d,shader,a);}
VKAPI_ATTR VkResult VKAPI_CALL createImage(VkDevice d,const VkImageCreateInfo* i,const VkAllocationCallbacks* a,VkImage* out){RESULT_BEGIN
    auto info=*i;
    if(stereoImage(info)&&(info.usage&VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))info.usage|=VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    return s->images.create(d,info,a,out,FN(vkCreateImage));
RESULT_END}
VKAPI_ATTR void VKAPI_CALL destroyImage(VkDevice d,VkImage image,const VkAllocationCallbacks* a){auto s=state(d);s->images.destroy(d,image,a,FN(vkDestroyImage));}
VKAPI_ATTR VkResult VKAPI_CALL createView(VkDevice d,const VkImageViewCreateInfo* i,const VkAllocationCallbacks* a,VkImageView* out){RESULT_BEGIN
    auto info=s->images.shaderViewInfo(*i);auto r=FN(vkCreateImageView)(d,&info,a,out);if(r==VK_SUCCESS){s->viewLayers[*out]=(s->images.layers(i->image)==2&&info.subresourceRange.baseArrayLayer==0&&info.subresourceRange.layerCount>=2)?2:1;if(!info.pNext)s->viewInfos[*out]=info;}return r;
RESULT_END}
VKAPI_ATTR void VKAPI_CALL destroyView(VkDevice d,VkImageView view,const VkAllocationCallbacks* a){auto s=state(d);std::unique_lock<std::shared_mutex> lock(s->mutex);auto eyes=s->eyeViews.find(view);if(eyes!=s->eyeViews.end()){for(auto eye:eyes->second)if(eye)FN(vkDestroyImageView)(d,eye,nullptr);s->eyeViews.erase(eyes);}s->viewInfos.erase(view);s->viewLayers.erase(view);FN(vkDestroyImageView)(d,view,a);}
VKAPI_ATTR VkResult VKAPI_CALL createPass(VkDevice d,const VkRenderPassCreateInfo* i,const VkAllocationCallbacks* a,VkRenderPass* out){RESULT_BEGIN
    auto input=*i;std::vector<VkAttachmentDescription> attachments;
    if(s->sources&&i->attachmentCount){attachments.assign(i->pAttachments,i->pAttachments+i->attachmentCount);for(auto& attachment:attachments){
        if(attachment.initialLayout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)attachment.initialLayout=VK_IMAGE_LAYOUT_GENERAL;
        if(attachment.finalLayout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)attachment.finalLayout=VK_IMAGE_LAYOUT_GENERAL;
    }input.pAttachments=attachments.data();}
    RenderPassPlan plan(input,true);if(!plan.valid())return VK_ERROR_FEATURE_NOT_PRESENT;
    auto r=FN(vkCreateRenderPass)(d,&input,a,out);if(r!=VK_SUCCESS)return r;VkRenderPass stereo{};r=FN(vkCreateRenderPass)(d,&plan.info(),a,&stereo);
    if(r!=VK_SUCCESS){FN(vkDestroyRenderPass)(d,*out,a);*out=VK_NULL_HANDLE;return r;}s->passes[*out]=stereo;return VK_SUCCESS;
RESULT_END}
VKAPI_ATTR void VKAPI_CALL destroyPass(VkDevice d,VkRenderPass pass,const VkAllocationCallbacks* a){auto s=state(d);std::unique_lock<std::shared_mutex> lock(s->mutex);auto it=s->passes.find(pass);if(it!=s->passes.end()){FN(vkDestroyRenderPass)(d,it->second,a);s->passes.erase(it);}FN(vkDestroyRenderPass)(d,pass,a);}
VKAPI_ATTR VkResult VKAPI_CALL createFramebuffer(VkDevice d,const VkFramebufferCreateInfo* i,const VkAllocationCallbacks* a,VkFramebuffer* out){RESULT_BEGIN
    auto info=*i;bool stereo=i->attachmentCount!=0;
    if(i->flags&VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT)return VK_ERROR_FEATURE_NOT_PRESENT;
    for(uint32_t j=0;j<i->attachmentCount;++j){auto v=s->viewLayers.find(i->pAttachments[j]);if(v==s->viewLayers.end()||v->second<2)stereo=false;}
    uint32_t stereoAttachments{};for(uint32_t j=0;j<i->attachmentCount;++j){auto v=s->viewLayers.find(i->pAttachments[j]);if(v!=s->viewLayers.end()&&v->second>=2)++stereoAttachments;}
    const bool mixed=stereoAttachments&&stereoAttachments<i->attachmentCount;
    auto pass=s->passes.find(i->renderPass);if(pass==s->passes.end())return VK_ERROR_INITIALIZATION_FAILED;
    if(stereo)info.renderPass=pass->second;auto r=FN(vkCreateFramebuffer)(d,&info,a,out);if(r==VK_SUCCESS){
        s->framebufferStereo[*out]=stereo;s->mixedFramebuffers[*out]=mixed;
        if(mixed&&s->mixedDiagnostics++<24){
            note("[SFS-MIXED] framebuffer="+std::to_string(reinterpret_cast<uintptr_t>(*out))+" size="+std::to_string(i->width)+"x"+std::to_string(i->height)+" stereoAttachments="+std::to_string(stereoAttachments)+" total="+std::to_string(i->attachmentCount)+" mode=mono");
            for(uint32_t j=0;j<i->attachmentCount;++j){auto v=s->viewInfos.find(i->pAttachments[j]);if(v!=s->viewInfos.end())note("[SFS-MIXED] attachment="+std::to_string(j)+" image="+std::to_string(reinterpret_cast<uintptr_t>(v->second.image))+" format="+std::to_string(v->second.format)+" baseLayer="+std::to_string(v->second.subresourceRange.baseArrayLayer)+" viewLayers="+std::to_string(v->second.subresourceRange.layerCount));}
        }
    }return r;
RESULT_END}
VKAPI_ATTR void VKAPI_CALL destroyFramebuffer(VkDevice d,VkFramebuffer fb,const VkAllocationCallbacks* a){auto s=state(d);std::unique_lock<std::shared_mutex> lock(s->mutex);s->framebufferStereo.erase(fb);s->mixedFramebuffers.erase(fb);FN(vkDestroyFramebuffer)(d,fb,a);}
VKAPI_ATTR VkResult VKAPI_CALL createLayout(VkDevice d,const VkDescriptorSetLayoutCreateInfo* i,const VkAllocationCallbacks* a,VkDescriptorSetLayout* out){RESULT_BEGIN
    std::vector<VkDescriptorSetLayoutBinding> bindings;if(i->bindingCount)bindings.assign(i->pBindings,i->pBindings+i->bindingCount);uint32_t dynamic=0;
    for(const auto& b:bindings){if(b.binding==30||b.binding==31)throw std::runtime_error("Game descriptor binding 30/31 conflicts with SFS");if(b.descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC||b.descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)dynamic+=b.descriptorCount;}
    if(i->pNext)throw std::runtime_error("SFS descriptor layout extension chain not supported by probe");
    for(uint32_t binding:{30u,31u})bindings.push_back({binding,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_COMPUTE_BIT,nullptr});auto info=*i;info.bindingCount=uint32_t(bindings.size());info.pBindings=bindings.data();auto r=FN(vkCreateDescriptorSetLayout)(d,&info,a,out);if(r==VK_SUCCESS)s->dynamicCounts[*out]=dynamic;return r;
RESULT_END}
VKAPI_ATTR void VKAPI_CALL destroyLayout(VkDevice d,VkDescriptorSetLayout layout,const VkAllocationCallbacks* a){auto s=state(d);std::unique_lock<std::shared_mutex> lock(s->mutex);s->dynamicCounts.erase(layout);FN(vkDestroyDescriptorSetLayout)(d,layout,a);}
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
    auto s=state(d);std::unique_lock<std::shared_mutex> lock(s->mutex);forgetPool(s,pool);FN(vkDestroyDescriptorPool)(d,pool,allocator);
}
VKAPI_ATTR VkResult VKAPI_CALL graphics(VkDevice d,VkPipelineCache cache,uint32_t count,const VkGraphicsPipelineCreateInfo* infos,const VkAllocationCallbacks* a,VkPipeline* out){RESULT_BEGIN
    for(uint32_t j=0;j<count;++j)out[j]=VK_NULL_HANDLE;
    for(uint32_t j=0;j<count;++j){auto info=infos[j];std::vector<VkPipelineShaderStageCreateInfo> stages(info.pStages,info.pStages+info.stageCount);const auto seed=pipelineSeed(info);
        // Correlate transparent material state with exact profile variants.
        // Creation-only and bounded: no string formatting on draw/record paths.
        if(kharvox::extendedDiagnosticsEnabled()&&s->materialDiagnostics<512
            &&info.pColorBlendState&&info.pColorBlendState->attachmentCount){
            bool blended=false;
            for(uint32_t k=0;k<info.pColorBlendState->attachmentCount;++k)
                blended|=info.pColorBlendState->pAttachments[k].blendEnable!=0;
            if(blended){
                ++s->materialDiagnostics;
                std::string signature;
                for(const auto& stage:stages){const auto& code=s->shaders.at(stage.module);
                    signature+=" stage"+std::to_string(stage.stage)+"="+shaderKey(profileHash(code.data(),uint32_t(code.size()*4)))
                        +"_"+shaderKey(profileHash(code.data(),uint32_t(code.size()*4),seed));}
                const auto* depth=info.pDepthStencilState;
                note("[SFS-MATERIAL] translucent subpass="+std::to_string(info.subpass)
                    +" depthTest="+std::to_string(depth?depth->depthTestEnable:0)
                    +" depthWrite="+std::to_string(depth?depth->depthWriteEnable:0)
                    +" depthCompare="+std::to_string(depth?depth->depthCompareOp:0)+signature);
                for(uint32_t k=0;k<info.pColorBlendState->attachmentCount;++k){const auto& b=info.pColorBlendState->pAttachments[k];
                    note("[SFS-MATERIAL] attachment="+std::to_string(k)+" blend="+std::to_string(b.blendEnable)
                        +" src="+std::to_string(b.srcColorBlendFactor)+" dst="+std::to_string(b.dstColorBlendFactor)
                        +" op="+std::to_string(b.colorBlendOp)+" mask="+std::to_string(b.colorWriteMask));}
            }
        }
        for(auto& stage:stages){const auto& code=s->shaders.at(stage.module);const auto variant=profileHash(code.data(),uint32_t(code.size()*4),seed);bool compute{};stage.module=compiledModule(s,stage.module,variant,compute,doomShadowProjection(info));}info.pStages=stages.data();
        // Derivative batch indices must not escape their original batch.
        if(info.flags&VK_PIPELINE_CREATE_DERIVATIVE_BIT)throw std::runtime_error("SFS derivative pipelines need batch remapping");
        auto r=FN(vkCreateGraphicsPipelines)(d,cache,1,&info,a,&out[j]);if(r!=VK_SUCCESS)return r;
        info.renderPass=s->passes.at(info.renderPass);VkPipeline stereo{};r=FN(vkCreateGraphicsPipelines)(d,cache,1,&info,a,&stereo);if(r!=VK_SUCCESS){FN(vkDestroyPipeline)(d,out[j],a);out[j]=VK_NULL_HANDLE;return r;}s->stereoPipelines[out[j]]=stereo;
    }return VK_SUCCESS;
RESULT_END}
VKAPI_ATTR VkResult VKAPI_CALL compute(VkDevice d,VkPipelineCache cache,uint32_t count,const VkComputePipelineCreateInfo* infos,const VkAllocationCallbacks* a,VkPipeline* out){RESULT_BEGIN
    for(uint32_t j=0;j<count;++j)out[j]=VK_NULL_HANDLE;
    for(uint32_t j=0;j<count;++j){
        auto info=infos[j];const auto original=info.stage.module;bool stereo{};
        if(info.flags&VK_PIPELINE_CREATE_DERIVATIVE_BIT)return VK_ERROR_FEATURE_NOT_PRESENT;
        info.stage.module=compiledModule(s,original,0,stereo);
        const auto& code=s->shaders.at(original);
        const bool indirect=stereo&&!loadProfileShader(s->profile,profileHash(code.data(),uint32_t(code.size()*4)),0,VK_SHADER_STAGE_COMPUTE_BIT);
        std::array<VkShaderModule,2> modules{};
        if(indirect)for(int eye=0;eye<2;++eye){bool ignored{};modules[eye]=compiledModule(s,original,0,ignored,false,eye);}
        auto r=FN(vkCreateComputePipelines)(d,cache,1,&info,a,&out[j]);if(r!=VK_SUCCESS)return r;
        std::array<VkPipeline,2> eyes{};
        if(indirect)for(int eye=0;eye<2;++eye){info.stage.module=modules[eye];r=FN(vkCreateComputePipelines)(d,cache,1,&info,a,&eyes[eye]);
            if(r!=VK_SUCCESS){for(auto p:eyes)if(p)FN(vkDestroyPipeline)(d,p,a);FN(vkDestroyPipeline)(d,out[j],a);out[j]=VK_NULL_HANDLE;return r;}}
        if(indirect)s->indirectPipelines[out[j]]=eyes;
        s->computeStereo[out[j]]=stereo;
    }return VK_SUCCESS;
RESULT_END}
VKAPI_ATTR void VKAPI_CALL destroyPipeline(VkDevice d,VkPipeline pipeline,const VkAllocationCallbacks* a){auto s=state(d);std::unique_lock<std::shared_mutex> lock(s->mutex);auto it=s->stereoPipelines.find(pipeline);if(it!=s->stereoPipelines.end()){FN(vkDestroyPipeline)(d,it->second,a);s->stereoPipelines.erase(it);}auto indirect=s->indirectPipelines.find(pipeline);if(indirect!=s->indirectPipelines.end()){for(auto eye:indirect->second)FN(vkDestroyPipeline)(d,eye,a);s->indirectPipelines.erase(indirect);}s->computeStereo.erase(pipeline);FN(vkDestroyPipeline)(d,pipeline,a);}
VKAPI_ATTR VkResult VKAPI_CALL beginCommand(VkCommandBuffer cb,const VkCommandBufferBeginInfo* i){try{auto s=state(cb);std::shared_lock<std::shared_mutex> lock(s->mutex);if(i->pInheritanceInfo&&i->pInheritanceInfo->renderPass)return VK_ERROR_FEATURE_NOT_PRESENT;auto& command=s->commands.at(cb);command.stereo=false;command.compute=VK_NULL_HANDLE;command.graphics=VK_NULL_HANDLE;for(auto& descriptor:command.descriptors)descriptor.set=VK_NULL_HANDLE;command.bindings.clear();command.pushes.clear();return FN(vkBeginCommandBuffer)(cb,i);}catch(const std::exception& e){note(e.what());return VK_ERROR_INITIALIZATION_FAILED;}}
VKAPI_ATTR VkResult VKAPI_CALL allocateCommands(VkDevice d,const VkCommandBufferAllocateInfo* i,VkCommandBuffer* out){RESULT_BEGIN
    auto r=FN(vkAllocateCommandBuffers)(d,i,out);if(r==VK_SUCCESS)for(uint32_t j=0;j<i->commandBufferCount;++j){s->commandPools[out[j]]=i->commandPool;s->commands.try_emplace(out[j]);}return r;
RESULT_END}
VKAPI_ATTR void VKAPI_CALL freeCommands(VkDevice d,VkCommandPool pool,uint32_t count,const VkCommandBuffer* commands){auto s=state(d);std::unique_lock<std::shared_mutex> lock(s->mutex);for(uint32_t j=0;j<count;++j){s->commands.erase(commands[j]);s->commandPools.erase(commands[j]);}FN(vkFreeCommandBuffers)(d,pool,count,commands);}
VKAPI_ATTR void VKAPI_CALL destroyCommandPool(VkDevice d,VkCommandPool pool,const VkAllocationCallbacks* a){auto s=state(d);std::unique_lock<std::shared_mutex> lock(s->mutex);for(auto it=s->commandPools.begin();it!=s->commandPools.end();)if(it->second==pool){s->commands.erase(it->first);it=s->commandPools.erase(it);}else ++it;FN(vkDestroyCommandPool)(d,pool,a);}
void bindGraphics(const std::shared_ptr<State>& s,VkCommandBuffer cb){
    auto& command=s->commands.at(cb);if(!command.graphics)return;
    auto found=s->stereoPipelines.find(command.graphics);if(found==s->stereoPipelines.end())throw std::runtime_error("Untracked SFS graphics pipeline");
    FN(vkCmdBindPipeline)(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,command.stereo?found->second:command.graphics);
}
VKAPI_ATTR void VKAPI_CALL bindPipeline(VkCommandBuffer cb,VkPipelineBindPoint point,VkPipeline pipeline){COMMAND_BEGIN
    auto& command=s->commands.at(cb);if(point==VK_PIPELINE_BIND_POINT_COMPUTE){command.compute=pipeline;FN(vkCmdBindPipeline)(cb,point,pipeline);return;}
    command.graphics=pipeline;bindGraphics(s,cb);
COMMAND_END}
VKAPI_ATTR void VKAPI_CALL bindSets(VkCommandBuffer cb,VkPipelineBindPoint point,VkPipelineLayout layout,uint32_t first,uint32_t count,const VkDescriptorSet* sets,uint32_t dynamicCount,const uint32_t* dynamic){COMMAND_BEGIN
    auto& descriptors=s->commands.at(cb).descriptors;
    if(point==VK_PIPELINE_BIND_POINT_GRAPHICS&&descriptors.size()<size_t(first)+count)descriptors.resize(size_t(first)+count);
    uint32_t offset=0;for(uint32_t j=0;j<count;++j){auto set=sets[j];const auto n=s->setDynamicCounts.at(set);if(n>dynamicCount-offset)throw std::runtime_error("SFS dynamic descriptor offset mismatch");
        if(point==VK_PIPELINE_BIND_POINT_GRAPHICS){auto& cached=descriptors[first+j];cached.layout=layout;cached.set=set;cached.dynamic.clear();if(n)cached.dynamic.assign(dynamic+offset,dynamic+offset+n);}offset+=n;}
    if(offset!=dynamicCount)throw std::runtime_error("SFS unexpected dynamic descriptor offsets");FN(vkCmdBindDescriptorSets)(cb,point,layout,first,count,sets,dynamicCount,dynamic);
COMMAND_END}
VKAPI_ATTR void VKAPI_CALL bindVertices(VkCommandBuffer cb,uint32_t first,uint32_t count,const VkBuffer* buffers,const VkDeviceSize* offsets){COMMAND_BEGIN
    for(uint32_t j=0;j<count;++j)s->commands.at(cb).bindings[0x20000ull+first+j]=[s,cb,index=first+j,b= buffers[j],o=offsets[j]]{FN(vkCmdBindVertexBuffers)(cb,index,1,&b,&o);};FN(vkCmdBindVertexBuffers)(cb,first,count,buffers,offsets);
COMMAND_END}
VKAPI_ATTR void VKAPI_CALL bindIndex(VkCommandBuffer cb,VkBuffer buffer,VkDeviceSize offset,VkIndexType type){COMMAND_BEGIN
    auto f=[s,cb,buffer,offset,type]{FN(vkCmdBindIndexBuffer)(cb,buffer,offset,type);};s->commands.at(cb).bindings[0x30000]=f;f();
COMMAND_END}
VKAPI_ATTR void VKAPI_CALL viewport(VkCommandBuffer cb,uint32_t first,uint32_t count,const VkViewport* values){COMMAND_BEGIN
    for(uint32_t j=0;j<count;++j)s->commands.at(cb).bindings[0x40000ull+first+j]=[s,cb,index=first+j,v=values[j]]{FN(vkCmdSetViewport)(cb,index,1,&v);};FN(vkCmdSetViewport)(cb,first,count,values);
COMMAND_END}
VKAPI_ATTR void VKAPI_CALL scissor(VkCommandBuffer cb,uint32_t first,uint32_t count,const VkRect2D* values){COMMAND_BEGIN
    for(uint32_t j=0;j<count;++j)s->commands.at(cb).bindings[0x50000ull+first+j]=[s,cb,index=first+j,v=values[j]]{FN(vkCmdSetScissor)(cb,index,1,&v);};FN(vkCmdSetScissor)(cb,first,count,values);
COMMAND_END}
VKAPI_ATTR void VKAPI_CALL push(VkCommandBuffer cb,VkPipelineLayout layout,VkShaderStageFlags flags,uint32_t offset,uint32_t size,const void* values){COMMAND_BEGIN
    s->commands.at(cb).pushes.write(layout,flags,offset,size,values);FN(vkCmdPushConstants)(cb,layout,flags,offset,size,values);
COMMAND_END}
void replayBindings(const std::shared_ptr<State>& s,VkCommandBuffer cb){
    bindGraphics(s,cb);auto& command=s->commands.at(cb);
    for(uint32_t index=0;index<command.descriptors.size();++index){const auto& binding=command.descriptors[index];if(binding.set)FN(vkCmdBindDescriptorSets)(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,binding.layout,index,1,&binding.set,uint32_t(binding.dynamic.size()),binding.dynamic.data());}
    for(const auto& binding:command.bindings)binding.second();
    command.pushes.replay([&](VkPipelineLayout layout,VkShaderStageFlags flags,uint32_t offset,uint32_t size,const void* values){FN(vkCmdPushConstants)(cb,layout,flags,offset,size,values);});
}
VKAPI_ATTR void VKAPI_CALL beginPass(VkCommandBuffer cb,const VkRenderPassBeginInfo* i,VkSubpassContents contents){COMMAND_BEGIN
    auto info=*i;auto& command=s->commands.at(cb);command.stereo=s->framebufferStereo.at(i->framebuffer);if(s->mixedFramebuffers.at(i->framebuffer))s->mixedPasses.fetch_add(1,std::memory_order_relaxed);if(command.stereo)info.renderPass=s->passes.at(i->renderPass);FN(vkCmdBeginRenderPass)(cb,&info,contents);replayBindings(s,cb);
COMMAND_END}
VKAPI_ATTR void VKAPI_CALL endPass(VkCommandBuffer cb){COMMAND_BEGIN FN(vkCmdEndRenderPass)(cb);s->commands.at(cb).stereo=false;COMMAND_END}
VKAPI_ATTR void VKAPI_CALL nextPass(VkCommandBuffer cb,VkSubpassContents contents){COMMAND_BEGIN FN(vkCmdNextSubpass)(cb,contents);replayBindings(s,cb);COMMAND_END}
VKAPI_ATTR void VKAPI_CALL lineWidth(VkCommandBuffer cb,float width){COMMAND_BEGIN
    auto f=[s,cb,width]{FN(vkCmdSetLineWidth)(cb,width);};s->commands.at(cb).bindings[0x60000]=f;f();
COMMAND_END}
VKAPI_ATTR void VKAPI_CALL depthBias(VkCommandBuffer cb,float constant,float clamp,float slope){COMMAND_BEGIN
    auto f=[s,cb,constant,clamp,slope]{FN(vkCmdSetDepthBias)(cb,constant,clamp,slope);};s->commands.at(cb).bindings[0x60001]=f;f();
COMMAND_END}
VKAPI_ATTR void VKAPI_CALL blendConstants(VkCommandBuffer cb,const float* values){COMMAND_BEGIN
    const std::array<float,4> constants{values[0],values[1],values[2],values[3]};auto f=[s,cb,constants]{FN(vkCmdSetBlendConstants)(cb,constants.data());};s->commands.at(cb).bindings[0x60002]=f;f();
COMMAND_END}
VKAPI_ATTR void VKAPI_CALL depthBounds(VkCommandBuffer cb,float min,float max){COMMAND_BEGIN
    auto f=[s,cb,min,max]{FN(vkCmdSetDepthBounds)(cb,min,max);};s->commands.at(cb).bindings[0x60003]=f;f();
COMMAND_END}
#define STENCIL_WRAPPER(handler,api,slot) \
VKAPI_ATTR void VKAPI_CALL handler(VkCommandBuffer cb,VkStencilFaceFlags faces,uint32_t value){COMMAND_BEGIN \
    for(uint32_t face=VK_STENCIL_FACE_FRONT_BIT;face<=VK_STENCIL_FACE_BACK_BIT;face<<=1)if(faces&face){auto f=[s,cb,face,value]{FN(api)(cb,face,value);};s->commands.at(cb).bindings[slot+face]=f;f();} \
COMMAND_END}
STENCIL_WRAPPER(stencilCompare,vkCmdSetStencilCompareMask,0x61000)
STENCIL_WRAPPER(stencilWrite,vkCmdSetStencilWriteMask,0x62000)
STENCIL_WRAPPER(stencilReference,vkCmdSetStencilReference,0x63000)
#undef STENCIL_WRAPPER
VKAPI_ATTR void VKAPI_CALL dispatch(VkCommandBuffer cb,uint32_t x,uint32_t y,uint32_t z){COMMAND_BEGIN
    uint32_t depth{};if(!dispatchDepth(z,s->computeStereo.at(s->commands.at(cb).compute),65535,depth))throw std::runtime_error("Stereo dispatch exceeds limit");FN(vkCmdDispatch)(cb,x,y,depth);
COMMAND_END}
VKAPI_ATTR void VKAPI_CALL dispatchIndirect(VkCommandBuffer cb,VkBuffer buffer,VkDeviceSize offset){COMMAND_BEGIN
    const auto pipeline=s->commands.at(cb).compute;
    if(!s->computeStereo.at(pipeline)){
        s->indirectMono.fetch_add(1,std::memory_order_relaxed);
        FN(vkCmdDispatchIndirect)(cb,buffer,offset);return;
    }
    const auto found=s->indirectPipelines.find(pipeline);
    if(found==s->indirectPipelines.end())throw std::runtime_error("SFS indirect compute uses an unsupported profile replacement; refusing left-eye-only output");
    s->indirectStereo.fetch_add(1,std::memory_order_relaxed);
    // Counts remain GPU-owned and unchanged. Each variant uses the full original
    // workgroup grid and writes its own eye layer; shared-buffer-only work stays mono.
    for(auto eye:found->second){FN(vkCmdBindPipeline)(cb,VK_PIPELINE_BIND_POINT_COMPUTE,eye);FN(vkCmdDispatchIndirect)(cb,buffer,offset);}
    FN(vkCmdBindPipeline)(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
COMMAND_END}
VkImageSubresourceRange range(const std::shared_ptr<State>& s,VkImage image,VkImageSubresourceRange value){if(s->images.layers(image)==2&&value.baseArrayLayer==0&&value.layerCount==1)value.layerCount=2;return value;}
VKAPI_ATTR void VKAPI_CALL barriers(VkCommandBuffer cb,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags deps,uint32_t nm,const VkMemoryBarrier* m,uint32_t nb,const VkBufferMemoryBarrier* b,uint32_t ni,const VkImageMemoryBarrier* i){COMMAND_BEGIN
    auto& images=s->commands.at(cb).imageBarriers;images.clear();if(ni)images.assign(i,i+ni);for(auto& image:images){image.subresourceRange=range(s,image.image,image.subresourceRange);
        if(s->sources&&s->sources->ownsImage(image.image)){
            if(image.oldLayout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)image.oldLayout=VK_IMAGE_LAYOUT_GENERAL;
            if(image.newLayout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)image.newLayout=VK_IMAGE_LAYOUT_GENERAL;
        }
    }FN(vkCmdPipelineBarrier)(cb,src,dst,deps,nm,m,nb,b,ni,images.data());
COMMAND_END}
VKAPI_ATTR void VKAPI_CALL clearColor(VkCommandBuffer cb,VkImage image,VkImageLayout layout,const VkClearColorValue* value,uint32_t count,const VkImageSubresourceRange* ranges){COMMAND_BEGIN
    auto& copies=s->commands.at(cb).clearRanges;copies.clear();if(count)copies.assign(ranges,ranges+count);for(auto& r:copies)r=range(s,image,r);FN(vkCmdClearColorImage)(cb,image,layout,value,count,copies.data());
COMMAND_END}
VKAPI_ATTR void VKAPI_CALL clearDepth(VkCommandBuffer cb,VkImage image,VkImageLayout layout,const VkClearDepthStencilValue* value,uint32_t count,const VkImageSubresourceRange* ranges){COMMAND_BEGIN
    auto& copies=s->commands.at(cb).clearRanges;copies.clear();if(count)copies.assign(ranges,ranges+count);for(auto& r:copies)r=range(s,image,r);FN(vkCmdClearDepthStencilImage)(cb,image,layout,value,count,copies.data());
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
    try{s->device=d;s->dispatch.load(d,gdpa);
        char timing[8]{};s->profileTiming=GetEnvironmentVariableA("KHARVOX_SFS_PROFILE_TIMING",timing,8)==1&&timing[0]=='1';wchar_t path[32768]{};auto n=GetEnvironmentVariableW(L"KHARVOX_SFS_PROFILE",path,32768);if(n&&n<32768)s->profile=path;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=sizeof(FrameUniforms);bi.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;if(FN(vkCreateBuffer)(d,&bi,nullptr,&s->params)!=VK_SUCCESS)return false;
        VkMemoryRequirements r{};FN(vkGetBufferMemoryRequirements)(d,s->params,&r);uint32_t index=UINT32_MAX;for(uint32_t j=0;j<memory.memoryTypeCount;++j)if((r.memoryTypeBits&(1u<<j))&&(memory.memoryTypes[j].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){index=j;break;}
        if(index==UINT32_MAX)throw std::runtime_error("No coherent SFS parameter memory");VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=r.size;ai.memoryTypeIndex=index;if(FN(vkAllocateMemory)(d,&ai,nullptr,&s->paramsMemory)!=VK_SUCCESS)throw std::runtime_error("SFS parameter allocation failed");if(FN(vkBindBufferMemory)(d,s->params,s->paramsMemory,0)!=VK_SUCCESS)throw std::runtime_error("SFS parameter bind failed");
        void* mapped{};if(FN(vkMapMemory)(d,s->paramsMemory,0,sizeof(FrameUniforms),0,&mapped)!=VK_SUCCESS)throw std::runtime_error("SFS parameter map failed");FrameUniforms initial;std::memcpy(mapped,&initial,sizeof(initial));FN(vkUnmapMemory)(d,s->paramsMemory);
        {std::lock_guard<std::mutex> lock(devicesMutex);devices[dispatchKey(d)]=s;deviceGeneration.fetch_add(1,std::memory_order_release);}note(vrEnabled()?"native SFS experimental OpenXR producer initialized":"native multiview probe initialized; fixed identity projection; NOT VR");return true;
    }catch(const std::exception& e){note(e.what());if(s->params)FN(vkDestroyBuffer)(d,s->params,nullptr);if(s->paramsMemory)FN(vkFreeMemory)(d,s->paramsMemory,nullptr);return false;}
}
void shutdown(VkDevice d){if(!nativeProbeEnabled())return;std::shared_ptr<State> s;try{s=state(d);}catch(const std::exception&){return;}std::unique_lock<std::shared_mutex> lock(s->mutex);s->commands.clear();if(s->sources){if(FN(vkDeviceWaitIdle)(d)!=VK_SUCCESS)commandFailure("SFS source shutdown retirement failed");s->sources->clearAfterDeviceIdle();}for(auto& entry:s->eyeViews)for(auto eye:entry.second)if(eye)FN(vkDestroyImageView)(d,eye,nullptr);s->eyeViews.clear();for(auto& module:s->compiled)FN(vkDestroyShaderModule)(d,module.second,nullptr);FN(vkDestroyBuffer)(d,s->params,nullptr);FN(vkFreeMemory)(d,s->paramsMemory,nullptr);std::lock_guard<std::mutex> devicesLock(devicesMutex);for(auto it=devices.begin();it!=devices.end();)if(it->second==s)it=devices.erase(it);else ++it;deviceGeneration.fetch_add(1,std::memory_order_release);}
bool vrEnabled(){static const bool enabled=[] {char value[8]{};return GetEnvironmentVariableA("KHARVOX_SFS_NATIVE_VR",value,8)==1&&value[0]=='1';}();return nativeProbeEnabled()&&enabled;}
bool eyeAttachmentView(VkDevice d,VkImageView original,uint32_t eye,VkImageView& result){
    result=VK_NULL_HANDLE;if(!nativeProbeEnabled()||eye>1)return false;
    auto s=state(d);std::unique_lock<std::shared_mutex> lock(s->mutex);
    auto found=s->viewInfos.find(original);if(found==s->viewInfos.end())return false;
    auto info=found->second;
    if(info.viewType!=VK_IMAGE_VIEW_TYPE_2D_ARRAY||info.subresourceRange.layerCount<2)return false;
    auto& cached=s->eyeViews[original][eye];
    if(!cached){info.viewType=VK_IMAGE_VIEW_TYPE_2D;info.subresourceRange.baseArrayLayer+=eye;info.subresourceRange.layerCount=1;
        if(FN(vkCreateImageView)(d,&info,nullptr,&cached)!=VK_SUCCESS)return false;}
    result=cached;return true;
}
void prepare(VkDevice d,const native::FramePose& pose,const XrFovf& source){
    if(!vrEnabled())return;auto s=state(d);std::unique_lock<std::shared_mutex> lock(s->mutex);
    FrameUniforms uniforms;
    if(!frameProjection(pose.head,source,pose.views,pose.worldScale,pose.gameplay||pose.cinematic||pose.scripted,uniforms))commandFailure("SFS headset projection unsupported: requires parallel eye cameras");
    s->pendingUniforms=uniforms;s->pendingPose=pose;s->pending=true;
    s->sourcePoseHistory.remember(pose,uniforms);
}
void copyCompleted(VkDevice d){if(!vrEnabled())return;auto s=state(d);std::unique_lock<std::shared_mutex> lock(s->mutex);s->completed=true;}
void beginFrame(VkDevice d,VkSwapchainKHR chain,uint32_t imageIndex){
    if(!vrEnabled())return;auto s=state(d);std::unique_lock<std::shared_mutex> lock(s->mutex);
    if(s->pending&&s->completed){
    // The prototype shares a uniform buffer across recorded command buffers.
    // Retire all previous readers before writing; a frame ring can replace this
    // conservative wait once multiple queued game frames have explicit ownership.
    const auto clockNow=[] {return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());};
    const auto retireStart=s->profileTiming?clockNow():0;
    if(FN(vkDeviceWaitIdle)(d)!=VK_SUCCESS)commandFailure("SFS frame parameter retirement failed");
    s->pendingUniforms.previousClip=s->frameValid?s->renderUniforms.clip:s->pendingUniforms.clip;
    s->pendingUniforms.previousTranslation=s->frameValid?s->renderUniforms.translation:s->pendingUniforms.translation;
    const auto uploadStart=s->profileTiming?clockNow():0;
    void* mapped{};if(FN(vkMapMemory)(d,s->paramsMemory,0,sizeof(FrameUniforms),0,&mapped)!=VK_SUCCESS)commandFailure("SFS frame parameter map failed");
    std::memcpy(mapped,&s->pendingUniforms,sizeof(FrameUniforms));FN(vkUnmapMemory)(d,s->paramsMemory);
    if(s->profileTiming){
        const auto retire=uploadStart-retireStart;
        s->retireNs+=retire;s->uploadNs+=clockNow()-uploadStart;
        if(retire>s->maxRetireNs)s->maxRetireNs=retire;
        if(++s->profiledFrames==120){
            const auto indirectMono=s->indirectMono.exchange(0,std::memory_order_relaxed),indirectStereo=s->indirectStereo.exchange(0,std::memory_order_relaxed),mixed=s->mixedPasses.exchange(0,std::memory_order_relaxed);
            note("[SFS-PATHS] frames=120 indirectShared="+std::to_string(indirectMono)+" indirectStereo="+std::to_string(indirectStereo)+" mixedMonoPasses="+std::to_string(mixed));
            const auto samples=CommandCpuTiming::samples.exchange(0,std::memory_order_relaxed);
            const auto wait=CommandCpuTiming::waitNs.exchange(0,std::memory_order_relaxed);
            const auto body=CommandCpuTiming::bodyNs.exchange(0,std::memory_order_relaxed);
            if(samples)note("command CPU timing frames=120 sampleStride=64 samples="+std::to_string(samples)
                +" sampledLookupLockMeanUs="+std::to_string(double(wait)/double(samples)/1000.0)
                +" sampledHookBodyMeanUs="+std::to_string(double(body)/double(samples)/1000.0)
                +" estimatedAggregateHookMsPerFrame="+std::to_string(double(wait+body)*64.0/120000000.0));
            note("parameter timing frames=120 deviceIdleMeanMs="+std::to_string(double(s->retireNs)/120000000.0)
                +" deviceIdleMaxMs="+std::to_string(double(s->maxRetireNs)/1000000.0)
                +" uploadMeanMs="+std::to_string(double(s->uploadNs)/120000000.0));
            s->profiledFrames=s->retireNs=s->uploadNs=s->maxRetireNs=0;
        }
    }
    s->renderPose=s->pendingPose;s->frameValid=true;s->pending=false;s->completed=false;
    s->renderUniforms=s->pendingUniforms;
    }
    // An acquired image uses the uniforms actually installed above, not the
    // newest pending XR prediction. Other swapchain images retain their pose.
    const auto found=s->swapchains.find(chain);
    if(found!=s->swapchains.end()&&imageIndex<found->second.size()){
        const auto image=found->second[imageIndex];
        if(s->frameValid){s->imagePoses[image]=s->renderPose;s->imageUniforms[image]=s->renderUniforms;}
        else {s->imagePoses.erase(image);s->imageUniforms.erase(image);}
    }
}
bool pair(VkDevice d,VkImage image,VkExtent2D extent,VkFormat format,native::StereoFrame& result,const AerSourceObservation* observed){
    if(!vrEnabled())return false;auto s=state(d);std::unique_lock<std::shared_mutex> lock(s->mutex);
    const auto found=s->imagePoses.find(image);
    if(found==s->imagePoses.end()||s->images.layers(image)!=2)return false;
    auto pose=found->second;
    if(observed&&pose.gameplay){
        const bool qualified=s->sourcePoseHistory.resolve(*observed,pose,s->imageUniforms.at(image),pose);
        if(++s->sourcePoseSamples<=8||s->sourcePoseSamples%120==0)
            note("[SFS-SOURCE-POSE] acquired="+std::to_string(found->second.source.poseId)
                +" observed="+std::to_string(observed->key.poseId)+" qualified="+std::to_string(qualified)
                +" count="+std::to_string(observed->count)+" ambiguous="+std::to_string(observed->ambiguous));
        if(observed->count&&!qualified)return false;
    }
    result={};result.pose=pose;result.generation=pose.serial;
    for(uint32_t e=0;e<2;++e)result.eyes[e]={image,extent,format,sourceLayout(d,image,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR),pose.views[e].pose,pose.views[e].fov,e,pose.serial};
    return true;
}
void swapchainImages(VkDevice d,VkSwapchainKHR chain,uint32_t count,const VkImage* images){
    if(!nativeProbeEnabled())return;
    auto s=state(d);std::unique_lock<std::shared_mutex> lock(s->mutex);
    auto& tracked=s->swapchains[chain];
    // Re-enumerating the same swapchain must not discard a valid acquisition.
    if(tracked==std::vector<VkImage>(images,images+count))return;
    for(auto image:tracked){s->imagePoses.erase(image);s->imageUniforms.erase(image);s->images.destroy(d,image,nullptr,nullptr);}
    tracked.assign(images,images+count);
    for(auto image:tracked)s->images.track(image,2);
}
void swapchainDestroyed(VkDevice d,VkSwapchainKHR chain){
    if(!nativeProbeEnabled())return;
    auto s=state(d);std::unique_lock<std::shared_mutex> lock(s->mutex);
    auto found=s->swapchains.find(chain);if(found==s->swapchains.end())return;
    for(auto image:found->second){s->imagePoses.erase(image);s->imageUniforms.erase(image);s->images.destroy(d,image,nullptr,nullptr);}
    s->swapchains.erase(found);
}
bool sourceRingRequested(){static const bool enabled=[] {char value[8]{};return GetEnvironmentVariableA("KHARVOX_SFS_SOURCE_RING",value,8)==1&&value[0]=='1';}();return vrEnabled()&&enabled;}
bool configureSourceRing(VkDevice d,PFN_vkGetDeviceProcAddr resolver,const VkPhysicalDeviceMemoryProperties& memory,VkQueue queue,void(*lock)(),void(*unlock)()){
    auto s=state(d);std::unique_lock<std::shared_mutex> guard(s->mutex);
    auto sources=std::make_unique<SourceRing>();if(!sources->initialize(d,queue,resolver,memory,lock,unlock))return false;
    s->sources=std::move(sources);s->profileTiming=true;CommandCpuTiming::enabled.store(true,std::memory_order_relaxed);note("source ring ACTIVE: application-owned stereo images (engine-requested count), GENERAL layout, same-device OpenXR, desktop WSI bypass");return true;
}
bool sourceRingActive(VkDevice d){if(!nativeProbeEnabled())return false;try{return bool(state(d)->sources);}catch(const std::exception&){return false;}}
bool sourceSwapchain(VkDevice d,VkSwapchainKHR chain){return sourceRingActive(d)&&state(d)->sources->owns(chain);}
VkResult createSourceSwapchain(VkDevice d,const VkSwapchainCreateInfoKHR& info,VkSwapchainKHR* output){return state(d)->sources->create(info,output);}
VkResult sourceImages(VkDevice d,VkSwapchainKHR chain,uint32_t* count,VkImage* images){return state(d)->sources->enumerate(chain,count,images);}
VkResult acquireSource(VkDevice d,VkSwapchainKHR chain,uint64_t timeout,VkSemaphore sem,VkFence fence,uint32_t* index){return state(d)->sources->acquire(chain,timeout,sem,fence,index);}
VkResult presentSource(VkDevice d,VkQueue queue,const VkPresentInfoKHR& info,bool consumed){return state(d)->sources->present(queue,info,consumed);}
void destroySourceSwapchain(VkDevice d,VkSwapchainKHR chain){if(state(d)->sources->destroy(chain)!=VK_SUCCESS)commandFailure("SFS source destruction retirement failed");}
VkImageLayout sourceLayout(VkDevice d,VkImage image,VkImageLayout layout){
    if(layout!=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR||!sourceRingActive(d))return layout;
    return state(d)->sources->ownsImage(image)?VK_IMAGE_LAYOUT_GENERAL:layout;
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
    HOOK(vkCmdSetViewport,viewport);HOOK(vkCmdSetScissor,scissor);HOOK(vkCmdPushConstants,push);HOOK(vkCmdBeginRenderPass,beginPass);HOOK(vkCmdEndRenderPass,endPass);HOOK(vkCmdDispatch,dispatch);HOOK(vkCmdDispatchIndirect,dispatchIndirect);
    HOOK(vkCmdNextSubpass,nextPass);HOOK(vkCmdSetLineWidth,lineWidth);HOOK(vkCmdSetDepthBias,depthBias);HOOK(vkCmdSetBlendConstants,blendConstants);HOOK(vkCmdSetDepthBounds,depthBounds);
    HOOK(vkCmdSetStencilCompareMask,stencilCompare);HOOK(vkCmdSetStencilWriteMask,stencilWrite);HOOK(vkCmdSetStencilReference,stencilReference);
    HOOK(vkCmdPipelineBarrier,barriers);HOOK(vkCmdClearColorImage,clearColor);HOOK(vkCmdClearDepthStencilImage,clearDepth);
    HOOK(vkCmdCopyImage,copyImage);HOOK(vkCmdBlitImage,blitImage);HOOK(vkCmdResolveImage,resolveImage);HOOK(vkCmdCopyBufferToImage,uploadImage);
#undef HOOK
    return next;
}
}

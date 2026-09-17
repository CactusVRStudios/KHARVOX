#pragma once
#include "PipelineIdentity.h"
#include <windows.h>
#include <vulkan/vulkan.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <map>
#include <vector>

namespace kharvox::sfs {
// Read-only producer capture for reconstructing profile lookup. Never mutate
// SPIR-V or turn a capture failure into a Vulkan/game failure.
inline const std::filesystem::path& captureRoot() {
    static const auto root=[] {wchar_t path[32768]{};auto n=GetEnvironmentVariableW(L"KHARVOX_SFS_CAPTURE_DIRECTORY",path,32768);return n&&n<32768?std::filesystem::path(path):std::filesystem::path{};}();
    return root;
}
struct CaptureState {
    std::mutex mutex;
    std::map<std::pair<VkDevice,VkShaderModule>,std::vector<uint32_t>> modules;
    size_t bytes{};
    size_t records{};
    size_t diskBytes{};
};
inline CaptureState& captureState(){static CaptureState s;return s;}
inline void forgetShader(VkDevice device,VkShaderModule module) noexcept {
    try {if(captureRoot().empty())return;auto& s=captureState();std::lock_guard<std::mutex> lock(s.mutex);auto it=s.modules.find({device,module});if(it!=s.modules.end()){s.bytes-=it->second.size()*4;s.modules.erase(it);}}catch(...){}
}
inline void forgetDevice(VkDevice device) noexcept {
    try {if(captureRoot().empty())return;auto& s=captureState();std::lock_guard<std::mutex> lock(s.mutex);for(auto it=s.modules.begin();it!=s.modules.end();)if(it->first.first==device){s.bytes-=it->second.size()*4;it=s.modules.erase(it);}else ++it;}catch(...){}
}
inline void capturePipelines(VkDevice device,uint32_t count,const VkGraphicsPipelineCreateInfo* infos) noexcept {
    try {
        if(captureRoot().empty()||!infos)return;
        auto& s=captureState();std::lock_guard<std::mutex> lock(s.mutex);
        std::filesystem::create_directories(captureRoot());
        std::ofstream out(captureRoot()/"pipeline-variants.tsv",std::ios::app);
        for(uint32_t i=0;i<count&&s.records<100000;++i){
            const auto& p=infos[i];if(!p.pStages)continue;
            auto hash=pipelineSeed(p);
            const auto packet=pipelinePacket(p);
            const auto packetFile=captureRoot()/(shaderKey(hash)+".pipeline");
            if(!std::filesystem::exists(packetFile)){std::ofstream binary(packetFile,std::ios::binary);binary.write(reinterpret_cast<const char*>(packet.data()),sizeof(packet));}
            for(uint32_t j=0;j<p.stageCount&&s.records<100000;++j){
                auto it=s.modules.find({device,p.pStages[j].module});if(it==s.modules.end())break;
                const auto& words=it->second;auto size=uint32_t(words.size()*4);
                const auto variant=profileHash(words.data(),size,hash);
                out<<shaderKey(profileHash(words.data(),size))<<'\t'<<shaderKey(variant)<<'\t'<<p.pStages[j].stage<<'\n';
                ++s.records;
            }
        }
    }catch(...){}
}
inline void captureShader(VkDevice device,VkShaderModule module,const VkShaderModuleCreateInfo* info) noexcept {
    try {
        const auto& root=captureRoot();
        if(root.empty()||!info||!info->pCode||info->codeSize<20||
           info->codeSize>16*1024*1024||info->codeSize%4||info->pCode[0]!=0x07230203)return;
        auto& state=captureState();
        std::lock_guard<std::mutex> lock(state.mutex);
        if(state.bytes+info->codeSize>128*1024*1024)return;
        auto& words=state.modules[{device,module}];state.bytes-=words.size()*4;
        words.assign(info->pCode,info->pCode+info->codeSize/4);state.bytes+=info->codeSize;
        std::filesystem::create_directories(root);
        const auto file=root/(shaderKey(profileHash(info->pCode,uint32_t(info->codeSize)))+".spv");
        if(std::filesystem::exists(file))return;
        if(state.diskBytes+info->codeSize>128*1024*1024)return;
        std::ofstream output(file,std::ios::binary);
        output.write(reinterpret_cast<const char*>(info->pCode),info->codeSize);
        if(output)state.diskBytes+=info->codeSize;
    }catch(...){}
}
}


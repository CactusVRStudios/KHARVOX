#pragma once
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

namespace kharvox::native {
class NativePushReplay {
    struct Write {
        VkPipelineLayout layout;
        VkShaderStageFlags stages;
        uint32_t offset;
        std::vector<uint32_t> words;
    };
    std::vector<Write> writes;
public:
    size_t size()const{return writes.size();}
    void write(VkPipelineLayout layout,VkShaderStageFlags stages,uint32_t offset,uint32_t size,const void* data){
        if(!layout||!stages||!data||!size||((offset|size)&3u)||offset>65536||size>65536-offset)
            throw std::runtime_error("Unsupported native push constant range");
        if(!writes.empty()){
            auto& last=writes.back();
            if(last.layout==layout&&last.stages==stages&&last.offset==offset&&last.words.size()==size/4){
                std::memcpy(last.words.data(),data,size);return;
            }
        }
        Write next{layout,stages,offset,std::vector<uint32_t>(size/4)};
        std::memcpy(next.words.data(),data,size);
        // ponytail: prune same-layout covered writes; cross-layout pruning needs compatibility metadata.
        writes.erase(std::remove_if(writes.begin(),writes.end(),[&](const Write& old){
            return old.layout==layout&&(old.stages&stages)==old.stages&&offset<=old.offset&&
                uint64_t(offset)+size>=uint64_t(old.offset)+old.words.size()*4;
        }),writes.end());
        writes.push_back(std::move(next));
    }
    template<class Emit> void replay(const Emit& emit)const{
        for(const auto& value:writes)
            emit(value.layout,value.stages,value.offset,uint32_t(value.words.size()*4),value.words.data());
    }
};

template<class Replay,class Emit>
void recordNativePush(Replay& replay,VkPipelineLayout layout,VkShaderStageFlags stages,
                      uint32_t offset,uint32_t size,const void* data,const Emit& emit){
    replay.pushState.write(layout,stages,offset,size,data);
    if(replay.final&&!replay.replaying){
        std::vector<uint32_t> words(size/4);
        std::memcpy(words.data(),data,size);
        replay.commands.emplace_back([=,words=std::move(words)]{emit(layout,stages,offset,size,words.data());});
    }
    emit(layout,stages,offset,size,data);
}
}

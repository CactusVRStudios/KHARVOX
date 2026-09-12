#pragma once
#include "NativeBindingMemo.h"
#include <memory>
#include <vector>
#include <vulkan/vulkan.h>

namespace kharvox::native {
// A plan owns metadata only. Mirror identity, initialization and layout must
// be resolved again at execution; they can change without a metadata mutation.
struct ImageLayoutRequirement {
    VkImage source{};
    VkImageLayout want{};
    VkImageSubresourceRange range{};
    VkFormat format{};
};
using ImageLayoutPlan=std::vector<ImageLayoutRequirement>;

template<class Key,size_t Limit=1024> class ImagePlanCache {
    BindingMemo<Key,std::shared_ptr<const ImageLayoutPlan>,Limit> memo;
public:
    auto find(const Key& key,std::optional<uint64_t> revision)const {
        const auto hit=memo.find(key,revision);
        return hit?*hit:std::shared_ptr<const ImageLayoutPlan>{};
    }
    void put(const Key& key,std::shared_ptr<const ImageLayoutPlan> plan,
        std::optional<uint64_t> before,std::optional<uint64_t> after){
        memo.put(key,std::move(plan),before,after);
    }
    void clear(){memo.clear();}
};

// A render pass accumulates all image inputs it has observed. The same set
// needs merging only once per stable metadata revision, including sets that
// stay bound while another set is updated/rebound. Overflow only loses hits.
template<class Key,size_t Limit=512> class ImageInputVisits {
    std::map<Key,uint64_t> seen;
public:
    bool contains(const Key& key,std::optional<uint64_t> revision)const {
        if(!revision)return false;
        const auto it=seen.find(key);return it!=seen.end()&&it->second==*revision;
    }
    void remember(const Key& key,std::optional<uint64_t> before,
        std::optional<uint64_t> after){
        if(!before||before!=after)return;
        const auto it=seen.find(key);
        if(it!=seen.end())it->second=*before;
        else if(seen.size()<Limit)seen.emplace(key,*before);
    }
    void clear(){seen.clear();}
};

template<class Mirror,class Resolve,class Layout,class Written,class Transition>
void visitImageLayoutPlan(const ImageLayoutPlan& plan,bool initialize,
    Resolve resolve,Layout layout,Written written,Transition transition){
    for(const auto& input:plan){
        const Mirror image=resolve(input.source);
        if(!image)continue;
        const auto have=layout(image);
        if(have==input.want||input.want==VK_IMAGE_LAYOUT_UNDEFINED)continue;
        if(!initialize&&!written(image))continue;
        transition(input,image,have);
    }
}
}

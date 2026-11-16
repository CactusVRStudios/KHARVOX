#pragma once
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <unordered_map>

namespace kharvox {

struct TextureQualityStatistics {
    std::uint64_t samplers{},linearMin{},linearMag{},linearMip{},anisotropic{};
    float minimumLodBias{},maximumLodBias{},maximumAnisotropy{};
    float minimumSamplerLod{},maximumSamplerLod{};
    std::uint64_t views{},nonzeroBaseMipViews{},uploadRegions{},blitRegions{},linearBlits{};
    std::uint64_t uploadMipTransitions{},untrackedImages{},outOfRangeMips{};
    std::array<std::uint64_t,32> uploadedMips{},blitDestinationMips{},viewBaseMips{};
};

// These are Vulkan API observations, not proof of which mip a fragment shader
// sampled. In particular, virtual-texture atlases can encode many logical
// mips inside one Vulkan mip. No page-token bit layout is assumed here.
class TextureQualityDiagnostics {
    TextureQualityStatistics statistics_{};
    std::unordered_map<std::uint64_t,std::uint32_t> lastUploadMip_;
    void mip(std::array<std::uint64_t,32>& histogram,std::uint32_t level) {
        if(level<histogram.size())++histogram[level];
        else ++statistics_.outOfRangeMips;
    }
public:
    const TextureQualityStatistics& statistics() const { return statistics_; }
    void sampler(const VkSamplerCreateInfo& info) {
        auto& s=statistics_;
        if(!s.samplers){
            s.minimumLodBias=s.maximumLodBias=info.mipLodBias;
            s.minimumSamplerLod=info.minLod;s.maximumSamplerLod=info.maxLod;
        }
        ++s.samplers;s.linearMin+=info.minFilter==VK_FILTER_LINEAR;
        s.linearMag+=info.magFilter==VK_FILTER_LINEAR;
        s.linearMip+=info.mipmapMode==VK_SAMPLER_MIPMAP_MODE_LINEAR;
        s.anisotropic+=info.anisotropyEnable!=0;
        s.minimumLodBias=std::min(s.minimumLodBias,info.mipLodBias);
        s.maximumLodBias=std::max(s.maximumLodBias,info.mipLodBias);
        s.minimumSamplerLod=std::min(s.minimumSamplerLod,info.minLod);
        s.maximumSamplerLod=std::max(s.maximumSamplerLod,info.maxLod);
        if(info.anisotropyEnable)s.maximumAnisotropy=std::max(s.maximumAnisotropy,info.maxAnisotropy);
    }
    void view(const VkImageViewCreateInfo& info) {
        ++statistics_.views;statistics_.nonzeroBaseMipViews+=info.subresourceRange.baseMipLevel!=0;
        mip(statistics_.viewBaseMips,info.subresourceRange.baseMipLevel);
    }
    void upload(std::uint64_t image,std::uint32_t count,const VkBufferImageCopy* regions) {
        if(!regions||!count)return;
        auto previous=lastUploadMip_.find(image);
        if(previous==lastUploadMip_.end()&&lastUploadMip_.size()<1024)
            previous=lastUploadMip_.emplace(image,regions[0].imageSubresource.mipLevel).first;
        for(std::uint32_t index=0;index<count;++index){
            const auto level=regions[index].imageSubresource.mipLevel;
            ++statistics_.uploadRegions;mip(statistics_.uploadedMips,level);
            if(previous!=lastUploadMip_.end()){
                statistics_.uploadMipTransitions+=previous->second!=level;
                previous->second=level;
            }else ++statistics_.untrackedImages;
        }
    }
    void blit(std::uint32_t count,const VkImageBlit* regions,VkFilter filter) {
        if(!regions)return;
        statistics_.blitRegions+=count;
        if(filter==VK_FILTER_LINEAR)statistics_.linearBlits+=count;
        for(std::uint32_t index=0;index<count;++index)
            mip(statistics_.blitDestinationMips,regions[index].dstSubresource.mipLevel);
    }
    void forgetImage(std::uint64_t image) { lastUploadMip_.erase(image); }
};
} // namespace kharvox

#pragma once
#include "TextureQualityDiagnostics.h"
#include "../common/RuntimeLog.h"
#include "../common/VulkanSfs.h"
#include <mutex>
#include <sstream>

namespace kharvox {
struct TextureQualityLogState {
    std::mutex mutex;
    TextureQualityDiagnostics observations;
    std::uint64_t firstTick{},nextReport{};
};
inline TextureQualityLogState& textureQualityLogState() {
    static TextureQualityLogState state;
    return state;
}
template<class Observation>
inline void observeTextureQuality(Observation observation) noexcept {
    if(!extendedDiagnosticsEnabled())return;
    try {
        auto& state=textureQualityLogState();
        std::lock_guard<std::mutex> guard(state.mutex);
        observation(state.observations);
    } catch(...) { /* Diagnostics must not change rendering behaviour. */ }
}
// Flush from the common XR frame path, including frames without uploads.
inline void reportTextureQuality() noexcept {
    if(!extendedDiagnosticsEnabled())return;
    try {
        auto& state=textureQualityLogState();
        std::lock_guard<std::mutex> guard(state.mutex);
        const auto now=GetTickCount64();
        if(now<state.nextReport)return;
        if(!state.firstTick)state.firstTick=now;
        state.nextReport=now+1000;
        const auto& s=state.observations.statistics();
        std::ostringstream out;
        out<<"[TEXTURE-STATS] renderer="<<(vulkanSfsEnabled()?"SFS":"AER")
            <<" pid="<<GetCurrentProcessId()
            <<" elapsed_ms="<<now-state.firstTick<<" samplers="<<s.samplers
            <<" linear_min="<<s.linearMin<<" linear_mag="<<s.linearMag
            <<" linear_mip="<<s.linearMip<<" anisotropic="<<s.anisotropic
            <<" max_anisotropy="<<s.maximumAnisotropy
            <<" lod_bias_min="<<s.minimumLodBias<<" lod_bias_max="<<s.maximumLodBias
            <<" sampler_min_lod="<<s.minimumSamplerLod<<" sampler_max_lod="<<s.maximumSamplerLod
            <<" views="<<s.views<<" nonzero_base_mip_views="<<s.nonzeroBaseMipViews
            <<" upload_regions="<<s.uploadRegions<<" upload_mip_transitions="<<s.uploadMipTransitions
            <<" blit_regions="<<s.blitRegions<<" linear_blits="<<s.linearBlits
            <<" untracked_images="<<s.untrackedImages<<" out_of_range_mips="<<s.outOfRangeMips;
        auto histogram=[&](const char* name,const auto& values){
            out<<' '<<name<<'=';
            for(std::size_t i=0;i<values.size();++i)if(values[i])out<<i<<':'<<values[i]<<',';
        };
        histogram("uploaded_mips",s.uploadedMips);histogram("blit_destination_mips",s.blitDestinationMips);
        histogram("view_base_mips",s.viewBaseMips);
        out<<" gpu_residency=unavailable shader_selected_mip=unavailable virtual_page_mip=opaque";
        writeRuntimeLog("Vulkan",out.str());
    } catch(...) { /* Diagnostic I/O must not interrupt the XR frame. */ }
}
} // namespace kharvox

#pragma once
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace kharvox {
inline bool headsetSizedSource(bool externalSfs, bool nativeProbe, bool nativeVr) {
    return !externalSfs && (!nativeProbe || nativeVr);
}
template<class T> const T* surfaceChain(const void* next, VkStructureType type) {
    auto entry=static_cast<const VkBaseInStructure*>(next);
    while(entry){if(entry->sType==type)return reinterpret_cast<const T*>(entry);entry=entry->pNext;}
    return nullptr;
}
// Keep the game's established 16:9 camera carrier while supplying at least
// the requested per-eye pixel density on both axes. Desktop size is no input.
inline VkExtent2D independentSourceExtent(uint32_t eyeWidth, uint32_t eyeHeight,
                                         float scale, uint32_t limit) {
    if (!eyeWidth || !eyeHeight || !std::isfinite(scale) || scale <= 0) return {};
    const double height = std::max(double(eyeHeight), double(eyeWidth) * 9.0 / 16.0) * scale;
    const double units = std::ceil(height / 18.0);
    if (units * 32.0 > limit || units * 18.0 > limit) return {};
    return {uint32_t(units * 32.0), uint32_t(units * 18.0)};
}

inline bool supportsIndependentExtent(const VkSurfacePresentScalingCapabilitiesEXT& caps,
                                      VkExtent2D extent) {
    return (caps.supportedPresentScaling & VK_PRESENT_SCALING_STRETCH_BIT_EXT)
        && extent.width && extent.height
        && extent.width >= caps.minScaledImageExtent.width
        && extent.height >= caps.minScaledImageExtent.height
        && extent.width <= caps.maxScaledImageExtent.width
        && extent.height <= caps.maxScaledImageExtent.height;
}

inline void exposeIndependentExtent(VkSurfaceCapabilitiesKHR& caps, VkExtent2D extent) {
    caps.currentExtent = caps.minImageExtent = caps.maxImageExtent = extent;
}
// SUBOPTIMAL is a successful presentation. An intentionally differently sized
// mirror must not make the engine repeatedly discard its headset-sized source.
// OUT_OF_DATE, surface loss and device loss retain their recovery semantics.
inline VkResult independentSurfaceResult(VkResult result,bool independent) {
    return independent&&result==VK_SUBOPTIMAL_KHR?VK_SUCCESS:result;
}
}

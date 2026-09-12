#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>

namespace kharvox::native {
inline uint32_t snapshotMemoryType(const VkPhysicalDeviceMemoryProperties& memory,
                                  uint32_t compatible, bool gpuCopy) {
 constexpr auto required=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
 uint32_t fallback=UINT32_MAX;
 for(uint32_t i=0;i<memory.memoryTypeCount&&i<VK_MAX_MEMORY_TYPES;++i){
  const auto flags=memory.memoryTypes[i].propertyFlags;
  if(!(compatible&(1u<<i))||(flags&required)!=required)continue;
  if(fallback==UINT32_MAX)fallback=i;
  if(!gpuCopy)return i; // Preserve the established CPU upload allocation.
  if(flags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)return i;
 }
 return fallback;
}
}

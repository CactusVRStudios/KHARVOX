#pragma once
#include "NativeDeferredMemory.h"
#include <variant>
#include <vulkan/vulkan.h>

namespace kharvox::native {
struct NativeMemoryFree {
 VkDevice device{};VkDeviceMemory memory{};const VkAllocationCallbacks* allocator{};PFN_vkFreeMemory function{};
 bool operator==(const NativeMemoryFree& other)const{return device==other.device&&memory==other.memory;}
};
struct NativeImageDestroy {
 VkDevice device{};VkImage image{};const VkAllocationCallbacks* allocator{};PFN_vkDestroyImage function{};
 bool operator==(const NativeImageDestroy& other)const{return device==other.device&&image==other.image;}
};
// One ordered queue for both operations. Equal raw handles of different Vulkan
// object types are distinct; each request retains its correctly typed dispatch.
using NativeRetirementRequest=std::variant<NativeMemoryFree,NativeImageDestroy>;
using NativeRetirementQueue=DeferredMemoryFreeQueue<NativeRetirementRequest>;
template<class ForgetMemory,class ForgetImage>
void retireNativeResource(const NativeRetirementRequest& request,ForgetMemory forgetMemory,ForgetImage forgetImage){
 if(const auto* memory=std::get_if<NativeMemoryFree>(&request)){
  forgetMemory(memory->memory);
  memory->function(memory->device,memory->memory,memory->allocator);
 }else{
  const auto& image=std::get<NativeImageDestroy>(request);
  forgetImage(image.image);
  image.function(image.device,image.image,image.allocator);
 }
}
}

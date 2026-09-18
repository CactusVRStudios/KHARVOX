#pragma once
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
namespace kharvox::native {struct FramePose;struct StereoFrame;}
namespace kharvox::sfs {
#ifdef KHARVOX_HAVE_SFS_COMPILER
bool nativeProbeEnabled();
bool initialize(VkDevice,VkPhysicalDevice,PFN_vkGetDeviceProcAddr,const VkPhysicalDeviceMemoryProperties&);
void shutdown(VkDevice);
PFN_vkVoidFunction wrapProc(VkDevice d,const char*,PFN_vkVoidFunction);
void swapchainImages(VkDevice,VkSwapchainKHR,uint32_t,const VkImage*);
void swapchainDestroyed(VkDevice,VkSwapchainKHR);
bool vrEnabled();
void prepare(VkDevice,const kharvox::native::FramePose&,const XrFovf&);
void copyCompleted(VkDevice);
void beginFrame(VkDevice);
bool pair(VkDevice,VkImage,VkExtent2D,VkFormat,kharvox::native::StereoFrame&);
bool eyeAttachmentView(VkDevice,VkImageView,uint32_t,VkImageView&);
#else
inline bool nativeProbeEnabled(){return false;}
inline bool initialize(VkDevice,VkPhysicalDevice,PFN_vkGetDeviceProcAddr,const VkPhysicalDeviceMemoryProperties&){return true;}
inline void shutdown(VkDevice){}
inline PFN_vkVoidFunction wrapProc(VkDevice d,const char*,PFN_vkVoidFunction next){return next;}
inline void swapchainImages(VkDevice,VkSwapchainKHR,uint32_t,const VkImage*){}
inline void swapchainDestroyed(VkDevice,VkSwapchainKHR){}
inline bool vrEnabled(){return false;}
inline void prepare(VkDevice,const kharvox::native::FramePose&,const XrFovf&){}
inline void copyCompleted(VkDevice){}
inline void beginFrame(VkDevice){}
inline bool pair(VkDevice,VkImage,VkExtent2D,VkFormat,kharvox::native::StereoFrame&){return false;}
inline bool eyeAttachmentView(VkDevice,VkImageView,uint32_t,VkImageView&){return false;}
#endif
}

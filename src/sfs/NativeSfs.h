#pragma once
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
namespace kharvox {struct AerSourceObservation;}
namespace kharvox::native {struct FramePose;struct StereoFrame;}
namespace kharvox::sfs {
#ifdef KHARVOX_HAVE_SFS_COMPILER
bool sourceRingRequested();
bool configureSourceRing(VkDevice,PFN_vkGetDeviceProcAddr,const VkPhysicalDeviceMemoryProperties&,VkQueue,void(*)(),void(*)());
bool sourceRingActive(VkDevice);
bool sourceSwapchain(VkDevice,VkSwapchainKHR);
VkResult createSourceSwapchain(VkDevice,const VkSwapchainCreateInfoKHR&,VkSwapchainKHR*);
VkResult sourceImages(VkDevice,VkSwapchainKHR,uint32_t*,VkImage*);
VkResult acquireSource(VkDevice,VkSwapchainKHR,uint64_t,VkSemaphore,VkFence,uint32_t*);
VkResult presentSource(VkDevice,VkQueue,const VkPresentInfoKHR&,bool);
void destroySourceSwapchain(VkDevice,VkSwapchainKHR);
VkImageLayout sourceLayout(VkDevice,VkImage,VkImageLayout);
bool nativeProbeEnabled();
bool initialize(VkDevice,VkPhysicalDevice,PFN_vkGetDeviceProcAddr,const VkPhysicalDeviceMemoryProperties&);
void shutdown(VkDevice);
PFN_vkVoidFunction wrapProc(VkDevice d,const char*,PFN_vkVoidFunction);
void swapchainImages(VkDevice,VkSwapchainKHR,uint32_t,const VkImage*);
void swapchainDestroyed(VkDevice,VkSwapchainKHR);
bool vrEnabled();
void prepare(VkDevice,const kharvox::native::FramePose&,const XrFovf&);
void copyCompleted(VkDevice);
void beginFrame(VkDevice,VkSwapchainKHR=VK_NULL_HANDLE,uint32_t imageIndex=0);
bool pair(VkDevice,VkImage,VkExtent2D,VkFormat,kharvox::native::StereoFrame&,const AerSourceObservation* =nullptr);
bool eyeAttachmentView(VkDevice,VkImageView,uint32_t,VkImageView&);
#else
inline bool sourceRingRequested(){return false;}
inline bool configureSourceRing(VkDevice,PFN_vkGetDeviceProcAddr,const VkPhysicalDeviceMemoryProperties&,VkQueue,void(*)(),void(*)()){return false;}
inline bool sourceRingActive(VkDevice){return false;}
inline bool sourceSwapchain(VkDevice,VkSwapchainKHR){return false;}
inline VkResult createSourceSwapchain(VkDevice,const VkSwapchainCreateInfoKHR&,VkSwapchainKHR*){return VK_ERROR_FEATURE_NOT_PRESENT;}
inline VkResult sourceImages(VkDevice,VkSwapchainKHR,uint32_t*,VkImage*){return VK_ERROR_FEATURE_NOT_PRESENT;}
inline VkResult acquireSource(VkDevice,VkSwapchainKHR,uint64_t,VkSemaphore,VkFence,uint32_t*){return VK_ERROR_FEATURE_NOT_PRESENT;}
inline VkResult presentSource(VkDevice,VkQueue,const VkPresentInfoKHR&,bool){return VK_ERROR_FEATURE_NOT_PRESENT;}
inline void destroySourceSwapchain(VkDevice,VkSwapchainKHR){}
inline VkImageLayout sourceLayout(VkDevice,VkImage,VkImageLayout layout){return layout;}
inline bool nativeProbeEnabled(){return false;}
inline bool initialize(VkDevice,VkPhysicalDevice,PFN_vkGetDeviceProcAddr,const VkPhysicalDeviceMemoryProperties&){return true;}
inline void shutdown(VkDevice){}
inline PFN_vkVoidFunction wrapProc(VkDevice d,const char*,PFN_vkVoidFunction next){return next;}
inline void swapchainImages(VkDevice,VkSwapchainKHR,uint32_t,const VkImage*){}
inline void swapchainDestroyed(VkDevice,VkSwapchainKHR){}
inline bool vrEnabled(){return false;}
inline void prepare(VkDevice,const kharvox::native::FramePose&,const XrFovf&){}
inline void copyCompleted(VkDevice){}
inline void beginFrame(VkDevice,VkSwapchainKHR=VK_NULL_HANDLE,uint32_t=0){}
inline bool pair(VkDevice,VkImage,VkExtent2D,VkFormat,kharvox::native::StereoFrame&,const AerSourceObservation* =nullptr){return false;}
inline bool eyeAttachmentView(VkDevice,VkImageView,uint32_t,VkImageView&){return false;}
#endif
}

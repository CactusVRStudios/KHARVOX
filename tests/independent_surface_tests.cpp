#include "../src/vulkan/IndependentSurface.h"
#include <cstdlib>
#include <limits>

void require(bool value){if(!value)std::abort();}
int main(){
    require(kharvox::headsetSizedSource(false,false,false)); // AER
    require(kharvox::headsetSizedSource(false,true,true)); // native SFS VR
    require(!kharvox::headsetSizedSource(false,true,false)); // desktop probe
    require(!kharvox::headsetSizedSource(true,false,false)); // external provider
    const auto psvr2=kharvox::independentSourceExtent(2804,2860,1.f,16384);
    require(psvr2.width==5088&&psvr2.height==2862);
    require(kharvox::independentSurfaceResult(VK_SUBOPTIMAL_KHR,true)==VK_SUCCESS);
    require(kharvox::independentSurfaceResult(VK_SUBOPTIMAL_KHR,false)==VK_SUBOPTIMAL_KHR);
    for(auto error:{VK_ERROR_OUT_OF_DATE_KHR,VK_ERROR_SURFACE_LOST_KHR,VK_ERROR_DEVICE_LOST})
        require(kharvox::independentSurfaceResult(error,true)==error);
    const auto extent=kharvox::independentSourceExtent(3060,3264,1.f,16384);
    require(extent.width>=3060&&extent.height>=3264);
    require(uint64_t(extent.width)*9==uint64_t(extent.height)*16);
    for(auto desktop:{VkExtent2D{640,480},VkExtent2D{1920,1080},VkExtent2D{3840,2160},VkExtent2D{7680,4320}}){
        VkSurfaceCapabilitiesKHR caps{};caps.currentExtent=caps.minImageExtent=caps.maxImageExtent=desktop;
        caps.minImageCount=2;caps.supportedUsageFlags=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        kharvox::exposeIndependentExtent(caps,extent);
        require(caps.currentExtent.width==extent.width&&caps.maxImageExtent.height==extent.height);
        require(caps.minImageCount==2&&caps.supportedUsageFlags==VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    }
    require(!kharvox::independentSourceExtent(3060,3264,10.f,16384).width);
    require(!kharvox::independentSourceExtent(0,3264,1.f,16384).width);
    require(!kharvox::independentSourceExtent(3060,3264,std::numeric_limits<float>::infinity(),16384).width);
    VkSurfacePresentScalingCapabilitiesEXT scaling{VK_STRUCTURE_TYPE_SURFACE_PRESENT_SCALING_CAPABILITIES_EXT};
    scaling.minScaledImageExtent={1,1};scaling.maxScaledImageExtent={16384,16384};
    require(!kharvox::supportsIndependentExtent(scaling,extent));
    scaling.supportedPresentScaling=VK_PRESENT_SCALING_STRETCH_BIT_EXT;
    require(kharvox::supportsIndependentExtent(scaling,extent));
    scaling.maxScaledImageExtent={1920,1080};
    require(!kharvox::supportsIndependentExtent(scaling,extent));
    VkSurfacePresentModeEXT mode{VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_EXT};
    VkPhysicalDeviceFeatures2 prefix{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};prefix.pNext=&mode;
    require(kharvox::surfaceChain<VkSurfacePresentModeEXT>(&prefix,mode.sType)==&mode);
    require(!kharvox::surfaceChain<VkSurfacePresentModeEXT>(nullptr,mode.sType));
}

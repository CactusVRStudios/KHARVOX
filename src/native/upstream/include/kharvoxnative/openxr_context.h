#pragma once
#include <Windows.h>
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
namespace kharvoxnative {
struct VulkanState { VkInstance instance{}; VkPhysicalDevice physical_device{}; VkDevice device{}; VkQueue queue{}; uint32_t queue_family{UINT32_MAX},queue_index{}; };
class OpenXRContext { public:
 bool initialize(const VulkanState&) {return false;} void shutdown() {}
 bool ready() const {return false;} bool session_running() const {return false;}
 XrSessionState session_state() const {return XR_SESSION_STATE_UNKNOWN;}
 int layer_kind() const {return -1;}
 static const char* session_state_name(XrSessionState) {return "KHARVOX-owned";}
 static const char* layer_kind_name(int) {return "KHARVOX-owned";}
 void poll(VkImage={},VkExtent2D={},VkFormat=VK_FORMAT_UNDEFINED,VkImage={},VkExtent2D={},VkFormat=VK_FORMAT_UNDEFINED,VkImageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VkFilter=VK_FILTER_LINEAR,bool=false) {}
};
OpenXRContext& xr(); VulkanState& vulkan_state();
}

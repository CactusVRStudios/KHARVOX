#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <string>

namespace kharvox::hands {

// A non-owning snapshot of the DOOM framebuffer attachments associated with
// one native swapchain image. The layer keeps the Vulkan objects alive; the
// hand renderer only borrows them while recording the Present copy command.
struct HandSceneTarget {
    VkImage colorImage{};
    VkImageView colorView{};
    VkFormat colorFormat{VK_FORMAT_UNDEFINED};
    VkImage depthImage{};
    VkImageView depthView{};
    VkFormat depthFormat{VK_FORMAT_UNDEFINED};
    VkImageLayout depthLayout{VK_IMAGE_LAYOUT_UNDEFINED};
    VkExtent2D extent{};
    VkSampleCountFlagBits samples{VK_SAMPLE_COUNT_1_BIT};
    bool reverseDepth{};
    // Native mirrors survive across frames. Hand self-depth must never be
    // written back into their engine-owned depth/stencil contents.
    bool copyDepthForHands{};
    uint32_t depthArrayLayer{};
};

bool handSceneTrackingEnabled();
bool handSceneDepthFormat(VkFormat format);
VkImageAspectFlags handSceneDepthAspect(VkFormat format);

void handSceneImageCreated(VkImage image, const VkImageCreateInfo& info);
void handSceneImageDestroyed(VkImage image);
void handSceneImageBarriers(std::uint32_t count,const VkImageMemoryBarrier* barriers);
void handSceneImageViewCreated(VkImageView view,
                               const VkImageViewCreateInfo& info);
void handSceneImageViewDestroyed(VkImageView view);
void handSceneFramebufferCreated(VkFramebuffer framebuffer,
                                 const VkFramebufferCreateInfo& info);
void handSceneFramebufferDestroyed(VkFramebuffer framebuffer);
void handSceneRenderPassCreated(VkRenderPass renderPass,
                                const VkRenderPassCreateInfo& info);
void handSceneRenderPass2Created(VkRenderPass renderPass,
                                 const VkRenderPassCreateInfo2& info);
void handSceneRenderPassDestroyed(VkRenderPass renderPass);
void handSceneGraphicsPipelinesCreated(
    std::uint32_t count, const VkGraphicsPipelineCreateInfo* infos,
    const VkPipeline* pipelines);
void handScenePipelineDestroyed(VkPipeline pipeline);
void handSceneBeginRenderPass(VkCommandBuffer commandBuffer,
                              const VkRenderPassBeginInfo* info);
void handSceneBindPipeline(VkCommandBuffer commandBuffer,
                           VkPipelineBindPoint bindPoint,
                           VkPipeline pipeline);
void handSceneNextSubpass(VkCommandBuffer commandBuffer);
void handSceneEndRenderPass(VkCommandBuffer commandBuffer);

bool handSceneTargetForColor(VkImage colorImage, VkExtent2D requiredExtent,
                             HandSceneTarget& target);
std::string handSceneTargetDiagnostic(VkImage colorImage,
                                      VkExtent2D requiredExtent);
void handSceneDeviceDestroyed();

} // namespace kharvox::hands

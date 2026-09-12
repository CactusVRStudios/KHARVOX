#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

namespace kharvox::hudgpu {

enum class DrawKind {
    Direct,
    Indexed,
    Indirect,
    IndexedIndirect
};

struct QuadCandidate {
    VkImage image{VK_NULL_HANDLE};
    VkExtent2D extent{};
    VkFormat format{VK_FORMAT_UNDEFINED};
    VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
    std::uint64_t serial{};
};

// The draw-correlation capture is a developer-only diagnostic. HUD11 keeps it
// dormant unless an explicit marker is present beside the runtime DLL.
bool enabled();

void swapchainCreated(VkSwapchainKHR swapchain, const VkSwapchainCreateInfoKHR& info);
void swapchainImages(VkSwapchainKHR swapchain, std::uint32_t count, const VkImage* images);
void swapchainDestroyed(VkSwapchainKHR swapchain);

void imageCreated(VkImage image, const VkImageCreateInfo& info);
void imageDestroyed(VkImage image);
void imageViewCreated(VkImageView view, const VkImageViewCreateInfo& info);
void imageViewDestroyed(VkImageView view);
void framebufferCreated(VkFramebuffer framebuffer, const VkFramebufferCreateInfo& info);
void framebufferDestroyed(VkFramebuffer framebuffer);
void renderPassCreated(
    VkRenderPass renderPass, std::uint32_t attachmentCount,
    const VkAttachmentDescription* attachments);
void renderPass2Created(
    VkRenderPass renderPass, std::uint32_t attachmentCount,
    const VkAttachmentDescription2* attachments);
void renderPassDestroyed(VkRenderPass renderPass);
void graphicsPipelinesCreated(
    std::uint32_t count, const VkGraphicsPipelineCreateInfo* infos, const VkPipeline* pipelines);
void pipelineDestroyed(VkPipeline pipeline);
void updateDescriptorSets(
    std::uint32_t writeCount, const VkWriteDescriptorSet* writes,
    std::uint32_t copyCount, const VkCopyDescriptorSet* copies);

void beginRenderPass(VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo* info);
void nextSubpass(VkCommandBuffer commandBuffer);
void endRenderPass(VkCommandBuffer commandBuffer);
void bindPipeline(
    VkCommandBuffer commandBuffer, VkPipelineBindPoint bindPoint, VkPipeline pipeline);
void bindDescriptorSets(
    VkCommandBuffer commandBuffer, VkPipelineBindPoint bindPoint,
    std::uint32_t firstSet, std::uint32_t setCount, const VkDescriptorSet* sets);
void bindVertexBuffers(
    VkCommandBuffer commandBuffer, std::uint32_t firstBinding,
    std::uint32_t bindingCount, const VkBuffer* buffers, const VkDeviceSize* offsets);
void bindIndexBuffer(
    VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkIndexType indexType);
void setViewport(
    VkCommandBuffer commandBuffer, std::uint32_t count, const VkViewport* viewports);
void setScissor(
    VkCommandBuffer commandBuffer, std::uint32_t count, const VkRect2D* scissors);
void draw(
    VkCommandBuffer commandBuffer, DrawKind kind,
    std::uint64_t submittedDraws, std::uint64_t workItems);

void present(std::uint64_t presentSerial);
bool getQuadCandidate(QuadCandidate& candidate);
void deviceDestroyed();

} // namespace kharvox::hudgpu

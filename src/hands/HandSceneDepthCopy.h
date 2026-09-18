#pragma once
#include <vulkan/vulkan.h>
#include <array>

namespace kharvox::hands {
// The caller has transitioned the borrowed scene depth to attachment layout.
// Copy only depth; stencil in the private target is unused. The source's depth
// and stencil contents are never written, and its entry layout is restored.
template<class Dispatch>
void copyHandSceneDepth(const Dispatch& vk,VkCommandBuffer cb,VkImage source,
    VkImage destination,VkExtent2D extent,VkImageAspectFlags aspects,bool initialized,uint32_t sourceLayer=0){
    std::array<VkImageMemoryBarrier,2> b{};
    for(auto& item:b){
        item.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        item.srcQueueFamilyIndex=item.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        item.subresourceRange={aspects,0,1,0,1};
    }
    b[0].image=source;b[0].oldLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    b[0].newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b[0].subresourceRange.baseArrayLayer=sourceLayer;
    b[0].srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT;
    b[0].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    b[1].image=destination;
    b[1].oldLayout=initialized?VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;
    b[1].newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b[1].srcAccessMask=initialized?VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT:0;
    b[1].dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    vk.cmdPipelineBarrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,2,b.data());
    VkImageCopy copy{};copy.srcSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};
    copy.dstSubresource=copy.srcSubresource;copy.extent={extent.width,extent.height,1};
    copy.srcSubresource.baseArrayLayer=sourceLayer;
    vk.cmdCopyImage(cb,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,destination,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    for(auto& item:b){
        item.oldLayout=item.newLayout;item.newLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        item.srcAccessMask=item.dstAccessMask;
        item.dstAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }
    vk.cmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,0,0,nullptr,0,nullptr,2,b.data());
}
}

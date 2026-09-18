#pragma once
#include <vulkan/vulkan.h>
namespace kharvox::sfs {
// DOOM's profile supplies unshifted variants for biased, depth-writing shadow
// draws and shifted variants of the same shader for camera depth. Use this
// classification for generic and unshifted replacement shaders. Explicit
// stereo profile shaders still override it: some biased HUD passes need stereo.
inline bool doomShadowProjection(const VkGraphicsPipelineCreateInfo& p) {
    return p.pRasterizationState && p.pDepthStencilState && p.pColorBlendState &&
        p.pRasterizationState->depthBiasEnable && !p.pRasterizationState->rasterizerDiscardEnable &&
        p.pDepthStencilState->depthTestEnable && p.pDepthStencilState->depthWriteEnable &&
        p.pDepthStencilState->depthCompareOp==VK_COMPARE_OP_LESS_OR_EQUAL &&
        p.pColorBlendState->attachmentCount==0;
}
}

#include "../src/sfs/PipelineIdentity.h"
#include "../src/sfs/ShadowProjection.h"
#include <iostream>
int main(){
    // Real DOOM startup fixed-state fixture. Its seed resolves existing provider
    // shader variants; it was not generated from this test's expected fields.
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};vi.vertexBindingDescriptionCount=1;vi.vertexAttributeDescriptionCount=7;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.frontFace=VK_FRONT_FACE_CLOCKWISE;rs.lineWidth=1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};ds.depthTestEnable=1;ds.depthCompareOp=VK_COMPARE_OP_LESS_OR_EQUAL;ds.depthBoundsTestEnable=ds.stencilTestEnable=1;ds.maxDepthBounds=1;
    ds.front={VK_STENCIL_OP_KEEP,VK_STENCIL_OP_DECREMENT_AND_WRAP,VK_STENCIL_OP_DECREMENT_AND_WRAP,VK_COMPARE_OP_ALWAYS,0,UINT32_MAX,0};ds.back=ds.front;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};cb.attachmentCount=1;
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dy.dynamicStateCount=4;
    VkGraphicsPipelineCreateInfo p{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};p.stageCount=2;p.pVertexInputState=&vi;p.pInputAssemblyState=&ia;p.pViewportState=&vp;p.pRasterizationState=&rs;p.pMultisampleState=&ms;p.pDepthStencilState=&ds;p.pColorBlendState=&cb;p.pDynamicState=&dy;
    const auto hash=kharvox::sfs::pipelineSeed(p);
    if(hash!=0x153c58f2d959c77cULL){std::cerr<<std::hex<<hash;return 1;}
    // Handle identity and attachment equations are intentionally not in this key.
    p.layout=reinterpret_cast<VkPipelineLayout>(uintptr_t(0x1234));
    if(kharvox::sfs::pipelineSeed(p)!=hash)return 2;
    ds.depthWriteEnable=1;
    if(kharvox::sfs::pipelineSeed(p)==hash)return 3;
    cb.attachmentCount=0;rs.depthBiasEnable=1;
    if(!kharvox::sfs::doomShadowProjection(p))return 4;
    rs.depthBiasEnable=0; // Camera depth prepass still needs stereo.
    if(kharvox::sfs::doomShadowProjection(p))return 5;
    rs.depthBiasEnable=1;cb.attachmentCount=1; // Biased color/decal pass.
    if(kharvox::sfs::doomShadowProjection(p))return 6;
    cb.attachmentCount=0;ds.depthWriteEnable=0;
    if(kharvox::sfs::doomShadowProjection(p))return 7;
    std::cout<<"DOOM pipeline profile identity passed\n";
}

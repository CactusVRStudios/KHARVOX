#include "../src/hands/HandSceneDepthTracker.h"
#include "../src/native/NativeHandSceneTarget.h"
#include "../src/hands/HandSceneDepthCopy.h"
#include <map>
#include <set>
#include <windows.h>
#include <array>
#include <cassert>
template<class T>T handle(uintptr_t value){return reinterpret_cast<T>(value);}
int main(){
    SetEnvironmentVariableA("KHARVOX_SHOW_HANDS","1");
    using namespace kharvox::hands;
    const VkExtent2D extent{640,480};
    std::array<VkImage,2> colors{},depths{};
    std::array<VkImageView,2> colorViews{},depthViews{};
    for(uintptr_t e=0;e<2;++e){
        colors[e]=handle<VkImage>(10+e);depths[e]=handle<VkImage>(20+e);
        colorViews[e]=handle<VkImageView>(30+e);depthViews[e]=handle<VkImageView>(40+e);
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.extent={640,480,1};image.samples=VK_SAMPLE_COUNT_1_BIT;
        image.format=VK_FORMAT_R8G8B8A8_UNORM;image.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        handSceneImageCreated(colors[e],image);
        image.format=VK_FORMAT_D32_SFLOAT;image.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        handSceneImageCreated(depths[e],image);
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image=colors[e];view.format=VK_FORMAT_R8G8B8A8_UNORM;view.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
        handSceneImageViewCreated(colorViews[e],view);
        view.image=depths[e];view.format=VK_FORMAT_D32_SFLOAT;view.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
        handSceneImageViewCreated(depthViews[e],view);
        std::array<VkAttachmentDescription,2> attachments{};
        attachments[0].format=VK_FORMAT_R8G8B8A8_UNORM;
        attachments[0].finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachments[1].format=VK_FORMAT_D32_SFLOAT;
        attachments[1].loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[1].finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.attachmentCount=2;rp.pAttachments=attachments.data();
        const auto pass=handle<VkRenderPass>(50+e);handSceneRenderPassCreated(pass,rp);
        std::array<VkImageView,2> views{colorViews[e],depthViews[e]};
        VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fb.renderPass=pass;fb.width=640;fb.height=480;fb.attachmentCount=2;fb.pAttachments=views.data();
        const auto framebuffer=handle<VkFramebuffer>(60+e);handSceneFramebufferCreated(framebuffer,fb);
        std::array<VkClearValue,2> clears{};clears[1].depthStencil.depth=e==0?1.f:0.f;
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass=pass;begin.framebuffer=framebuffer;begin.clearValueCount=2;begin.pClearValues=clears.data();
        auto cb=handle<VkCommandBuffer>(70+e);handSceneBeginRenderPass(cb,&begin);handSceneEndRenderPass(cb);
    }
    HandSceneTarget left{},right{};
    assert(handSceneTargetForColor(colors[0],extent,left));
    assert(handSceneTargetForColor(colors[1],extent,right));
    assert(left.depthImage==depths[0]&&right.depthImage==depths[1]);
    assert(left.depthView==depthViews[0]&&right.depthView==depthViews[1]);
    assert(!left.reverseDepth&&right.reverseDepth);
    // A later LOAD pass has no clear or primary pipeline binds (the draws
    // live in secondaries). It must retain the reversed prepass convention.
    std::array<VkAttachmentDescription,2> loadAttachments{};
    loadAttachments[0].format=VK_FORMAT_R8G8B8A8_UNORM;
    loadAttachments[1].format=VK_FORMAT_D32_SFLOAT;
    loadAttachments[1].loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;
    loadAttachments[1].finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkRenderPassCreateInfo loadPass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    loadPass.attachmentCount=2;loadPass.pAttachments=loadAttachments.data();
    handSceneRenderPassCreated(handle<VkRenderPass>(51),loadPass);
    VkRenderPassBeginInfo loadBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    loadBegin.renderPass=handle<VkRenderPass>(51);loadBegin.framebuffer=handle<VkFramebuffer>(61);
    handSceneBeginRenderPass(handle<VkCommandBuffer>(71),&loadBegin);
    handSceneEndRenderPass(handle<VkCommandBuffer>(71));
    assert(handSceneTargetForColor(colors[1],extent,right)&&right.reverseDepth);
    // Native-owned mirrors have no game-tracker registration. The resolver
    // must use exact source-view identities and the owner's current layout.
    struct Mirror {VkImage image;VkImageView view;VkFormat format;VkExtent2D extent;};
    std::map<VkImageView,Mirror> mirrors{
        {right.colorView,{handle<VkImage>(110),handle<VkImageView>(130),right.colorFormat,extent}},
        {right.depthView,{handle<VkImage>(120),handle<VkImageView>(140),right.depthFormat,extent}}};
    std::map<VkImage,VkImageLayout> layouts{{handle<VkImage>(120),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    std::set<VkImage> written{handle<VkImage>(120)};
    HandSceneTarget mirrored{};
    auto resolve=[&](VkImage color){return kharvox::native::resolveHandSceneMirror(right,color,mirrors,layouts,written,mirrored);};
    assert(resolve(handle<VkImage>(110)));
    assert(mirrored.colorView==handle<VkImageView>(130)&&mirrored.depthView==handle<VkImageView>(140));
    assert(mirrored.depthImage!=right.depthImage&&mirrored.reverseDepth&&mirrored.depthLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    assert(mirrored.copyDepthForHands&&!right.copyDepthForHands);
    // Exercise the production command sequence: engine depth is only a copy
    // source, both depth/stencil aspects return to the caller's entry layout,
    // and the private image is refreshed on both first use and reuse.
    for(bool initialized:{false,true}){
        unsigned barriers=0,copies=0;
        const auto source=mirrored.depthImage,destination=handle<VkImage>(150);
        auto pipelineBarrier=[&](VkCommandBuffer,VkPipelineStageFlags,VkPipelineStageFlags,VkDependencyFlags,
            uint32_t,const VkMemoryBarrier*,uint32_t,const VkBufferMemoryBarrier*,uint32_t count,const VkImageMemoryBarrier* b){
            assert(count==2&&b[0].image==source&&b[1].image==destination);
            assert(b[0].subresourceRange.aspectMask==(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT));
            if(barriers++==0){
                assert(b[0].newLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL&&b[0].dstAccessMask==VK_ACCESS_TRANSFER_READ_BIT);
                assert(b[1].oldLayout==(initialized?VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED));
            }else{
                assert(b[0].newLayout==VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
                assert(b[1].newLayout==VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
            }
        };
        auto copyImage=[&](VkCommandBuffer,VkImage src,VkImageLayout,VkImage dst,VkImageLayout,uint32_t count,const VkImageCopy* copy){
            assert(barriers==1&&src==source&&dst==destination&&count==1);
            assert(copy->srcSubresource.aspectMask==VK_IMAGE_ASPECT_DEPTH_BIT);
            assert(copy->extent.width==640&&copy->extent.height==480);++copies;
        };
        struct {decltype(pipelineBarrier) cmdPipelineBarrier;decltype(copyImage) cmdCopyImage;} dispatch{pipelineBarrier,copyImage};
        copyHandSceneDepth(dispatch,handle<VkCommandBuffer>(170),source,destination,extent,
            VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,initialized);
        assert(barriers==2&&copies==1);
    }
    assert(!resolve(colors[0])&&!mirrored.depthImage);
    written.clear();assert(!resolve(handle<VkImage>(110)));written.insert(handle<VkImage>(120));
    layouts.clear();assert(!resolve(handle<VkImage>(110)));
    layouts[handle<VkImage>(120)]=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    mirrors[right.depthView].extent.width=320;assert(!resolve(handle<VkImage>(110)));
    mirrors.erase(right.depthView);assert(!resolve(handle<VkImage>(110)));
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.image=depths[1];barrier.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
    barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    auto colorBarrier=barrier;
    colorBarrier.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
    handSceneImageBarriers(0,&barrier);
    handSceneImageBarriers(1,nullptr);
    handSceneImageBarriers(1,&colorBarrier);
    assert(handSceneTargetForColor(colors[1],extent,right)&&right.depthLayout==VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    const VkImageMemoryBarrier mixed[]={colorBarrier,barrier,colorBarrier};
    handSceneImageBarriers(3,mixed);
    assert(handSceneTargetForColor(colors[1],extent,right)&&right.depthLayout==barrier.newLayout);
    assert(handSceneTargetForColor(colors[0],extent,left)&&left.depthLayout==VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    assert(!handSceneTargetForColor(colors[0],{320,240},left));
    handSceneImageViewDestroyed(depthViews[1]);
    assert(!handSceneTargetForColor(colors[1],extent,right));
    assert(handSceneTargetForColor(colors[0],extent,left));
    handSceneImageDestroyed(depths[0]);
    assert(!handSceneTargetForColor(colors[0],extent,left));
    handSceneDeviceDestroyed();
}

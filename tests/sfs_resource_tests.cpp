#include "../src/sfs/StereoResources.h"
#include <stdexcept>
#include <iostream>
using namespace kharvox::sfs;
void check(bool b){if(!b)throw std::runtime_error("SFS resource contract failed");}
static VkResult outcome=VK_SUCCESS;
static VkImageCreateInfo received{};
static VKAPI_ATTR VkResult VKAPI_CALL createImage(VkDevice,const VkImageCreateInfo* i,
    const VkAllocationCallbacks*,VkImage* output) {
    received=*i;
    if(outcome==VK_SUCCESS)*output=reinterpret_cast<VkImage>(uintptr_t(42));
    return outcome;
}
static VKAPI_ATTR void VKAPI_CALL destroyImage(VkDevice,VkImage,const VkAllocationCallbacks*){}
int main(){try{
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType=VK_IMAGE_TYPE_2D;info.arrayLayers=1;
    info.usage=VK_IMAGE_USAGE_SAMPLED_BIT;
    check(!stereoImage(info));
    info.usage|=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;check(stereoImage(info));
    auto copy=imageInfo(info);check(copy.arrayLayers==2&&info.arrayLayers==1);
    info.imageType=VK_IMAGE_TYPE_3D;check(!stereoImage(info));
    info.imageType=VK_IMAGE_TYPE_2D;info.arrayLayers=6;check(!stereoImage(info));
    info.arrayLayers=1;Images images;VkImage image{};
    check(images.create({},info,nullptr,&image,createImage)==VK_SUCCESS);
    check(received.arrayLayers==2);
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image=image;view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.subresourceRange.layerCount=1;
    check(images.viewInfo(view).viewType==VK_IMAGE_VIEW_TYPE_2D_ARRAY);
    images.destroy({},image,nullptr,destroyImage);
    check(images.viewInfo(view).viewType==VK_IMAGE_VIEW_TYPE_2D);
    outcome=VK_ERROR_OUT_OF_DEVICE_MEMORY;
    check(images.create({},info,nullptr,&image,createImage)==outcome);
    check(images.viewInfo(view).viewType==VK_IMAGE_VIEW_TYPE_2D);
    info.initialLayout=VK_IMAGE_LAYOUT_PREINITIALIZED;
    check(images.create({},info,nullptr,&image,createImage)==VK_ERROR_FORMAT_NOT_SUPPORTED);
    VkSubpassDescription subpasses[2]{};
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.subpassCount=2;rp.pSubpasses=subpasses;
    RenderPassPlan plan(rp,true);check(plan.valid());
    auto mv=static_cast<const VkRenderPassMultiviewCreateInfo*>(plan.info().pNext);
    check(mv->subpassCount==2&&mv->pViewMasks[0]==3&&mv->pViewMasks[1]==3);
    check(rp.pNext==nullptr&&mv->pCorrelationMasks[0]==3);
    RenderPassPlan duplicate(plan.info(),true);check(!duplicate.valid());
    RenderPassPlan mono(rp,false);check(mono.info().pNext==nullptr);
    uint32_t depth=99;check(dispatchDepth(12,true,64,depth)&&depth==24);
    check(dispatchDepth(12,false,64,depth)&&depth==12);
    check(!dispatchDepth(UINT32_MAX,true,UINT32_MAX,depth)&&depth==12);
    check(!dispatchDepth(40,true,64,depth)&&depth==12);
    std::cout<<"SFS allocation, view, render-pass and dispatch contracts passed\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

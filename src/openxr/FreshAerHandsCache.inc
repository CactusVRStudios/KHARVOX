// Two hand-free world images preserve the published pair while the normal
// AER source cache is overwritten by the next left capture. Never use an XR
// swapchain image as persistent storage after releasing it to the runtime.
bool ensureFreshHandsWorldCache(VkExtent2D extent){
    if(s.freshHandsWorld[0]&&s.freshHandsWorld[1]
        &&s.freshHandsExtent.width==extent.width&&s.freshHandsExtent.height==extent.height)
        return true;
    static VkExtent2D failedExtent{};
    if(failedExtent.width==extent.width&&failedExtent.height==extent.height)return false;
    failedExtent=extent;
    {QueueAccessScope queueAccess;
        if(!s.vk.queueWaitIdle||s.vk.queueWaitIdle(s.queue)!=VK_SUCCESS)return false;
    }
    auto clear=[](){
        for(int e=0;e<2;++e){
            if(s.freshHandsWorld[e])s.vk.destroyImage(s.device,s.freshHandsWorld[e],nullptr);
            if(s.freshHandsMemory[e])s.vk.freeMemory(s.device,s.freshHandsMemory[e],nullptr);
        }
        s.freshHandsWorld={};s.freshHandsMemory={};s.freshHandsExtent={};
        s.freshHandsInitialized=false;s.freshHandsWorldValid=false;
    };
    clear();
    VkPhysicalDeviceMemoryProperties properties{};
    s.vk.getPhysicalDeviceMemoryProperties(s.physical,&properties);
    for(int e=0;e<2;++e){
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType=VK_IMAGE_TYPE_2D;ci.format=static_cast<VkFormat>(s.format);
        ci.extent={extent.width,extent.height,1};ci.mipLevels=1;ci.arrayLayers=1;
        ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;
        ci.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        if(s.vk.createImage(s.device,&ci,nullptr,&s.freshHandsWorld[e])!=VK_SUCCESS){clear();return false;}
        VkMemoryRequirements requirements{};
        s.vk.getImageMemoryRequirements(s.device,s.freshHandsWorld[e],&requirements);
        uint32_t type=UINT32_MAX;
        for(uint32_t i=0;i<properties.memoryTypeCount;++i)
            if((requirements.memoryTypeBits&(1u<<i))&&(properties.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){type=i;break;}
        if(type==UINT32_MAX){clear();return false;}
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize=requirements.size;ai.memoryTypeIndex=type;
        if(s.vk.allocateMemory(s.device,&ai,nullptr,&s.freshHandsMemory[e])!=VK_SUCCESS
            ||s.vk.bindImageMemory(s.device,s.freshHandsWorld[e],s.freshHandsMemory[e],0)!=VK_SUCCESS){clear();return false;}
    }
    s.freshHandsExtent=extent;
    failedExtent={};
    log("[FRESH-AER-HANDS] clean world pair cache allocated "+std::to_string(extent.width)+"x"+std::to_string(extent.height));
    return true;
}

// Native subresource ledger. Included in the imported TU's private namespace.
// Callers hold mirror_mutex. The legacy scalar map remains a conservative
// whole-image summary for its existing descriptor readiness gate.
struct NativeImageLayouts {uint32_t mips{},layers{};std::vector<VkImageLayout> values;VkFormat format{};};
std::unordered_map<VkImage,NativeImageLayouts> native_image_layouts;
void register_mirror_layouts(VkImage image,uint32_t mips,uint32_t layers,VkImageLayout layout,VkFormat format=VK_FORMAT_UNDEFINED){
 if(!mips||!layers)kharvox::native::fail("zero mirror subresource dimensions");
 native_image_layouts[image]={mips,layers,std::vector<VkImageLayout>(size_t(mips)*layers,layout),format};mirror_image_layouts[image]=layout;
}
VkImageSubresourceRange checked_mirror_range(VkImage image,VkImageSubresourceRange range){
 auto it=native_image_layouts.find(image);if(it==native_image_layouts.end())kharvox::native::fail("mirror subresource ledger absent");auto& d=it->second;
 if(range.baseMipLevel>=d.mips||range.baseArrayLayer>=d.layers)kharvox::native::fail("mirror subresource base outside allocation");
 if(range.levelCount==VK_REMAINING_MIP_LEVELS)range.levelCount=d.mips-range.baseMipLevel;
 if(range.layerCount==VK_REMAINING_ARRAY_LAYERS)range.layerCount=d.layers-range.baseArrayLayer;
 if(!range.levelCount||range.levelCount>d.mips-range.baseMipLevel||!range.layerCount||range.layerCount>d.layers-range.baseArrayLayer)kharvox::native::fail("mirror subresource range outside allocation");
 return range;
}
void summarize_mirror_layout(VkImage image){auto& d=native_image_layouts.at(image);auto layout=d.values.front();for(auto value:d.values)if(value!=layout){layout=VK_IMAGE_LAYOUT_UNDEFINED;break;}mirror_image_layouts[image]=layout;}
void set_mirror_range_layout(VkImage image,VkImageSubresourceRange range,VkImageLayout layout){
 range=checked_mirror_range(image,range);auto& d=native_image_layouts.at(image);
 for(uint32_t layer=range.baseArrayLayer;layer<range.baseArrayLayer+range.layerCount;++layer)for(uint32_t mip=range.baseMipLevel;mip<range.baseMipLevel+range.levelCount;++mip)d.values[size_t(layer)*d.mips+mip]=layout;
 summarize_mirror_layout(image);
}
void append_mirror_transitions(std::vector<VkImageMemoryBarrier>& out,VkImageMemoryBarrier barrier,bool allowDiscard=false){
 auto range=checked_mirror_range(barrier.image,barrier.subresourceRange);auto& d=native_image_layouts.at(barrier.image);
 if(d.format==VK_FORMAT_D16_UNORM_S8_UINT||d.format==VK_FORMAT_D24_UNORM_S8_UINT||d.format==VK_FORMAT_D32_SFLOAT_S8_UINT)range.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT;
 for(uint32_t layer=range.baseArrayLayer;layer<range.baseArrayLayer+range.layerCount;++layer)for(uint32_t mip=range.baseMipLevel;mip<range.baseMipLevel+range.levelCount;++mip){
  auto b=barrier;b.subresourceRange={range.aspectMask,mip,1,layer,1};auto& old=d.values[size_t(layer)*d.mips+mip];if(!(allowDiscard&&b.oldLayout==VK_IMAGE_LAYOUT_UNDEFINED))b.oldLayout=old;out.push_back(b);old=b.newLayout;
 }
 summarize_mirror_layout(barrier.image);
}

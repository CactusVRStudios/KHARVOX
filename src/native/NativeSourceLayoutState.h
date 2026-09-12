#pragma once
#include <vulkan/vulkan.h>
#include <optional>
#include <unordered_map>
#include <vector>

namespace kharvox::native {
// Original images need their own ledger: redirecting an attachment does not
// perform that render pass's implicit transitions on the original image.
class SourceLayoutState {
 struct Image {uint32_t mips,layers;std::vector<VkImageLayout> values;};
 std::unordered_map<VkImage,Image> images;
 static bool rangeFor(const Image& image,VkImageSubresourceRange& r){
  if(r.baseMipLevel>=image.mips||r.baseArrayLayer>=image.layers)return false;
  if(r.levelCount==VK_REMAINING_MIP_LEVELS)r.levelCount=image.mips-r.baseMipLevel;
  if(r.layerCount==VK_REMAINING_ARRAY_LAYERS)r.layerCount=image.layers-r.baseArrayLayer;
  return r.levelCount&&r.layerCount&&r.levelCount<=image.mips-r.baseMipLevel&&r.layerCount<=image.layers-r.baseArrayLayer;
 }
public:
 void define(VkImage image,uint32_t mips,uint32_t layers,VkImageLayout initial){
  forget(image);if(mips&&layers)images.emplace(image,Image{mips,layers,std::vector<VkImageLayout>(size_t(mips)*layers,initial)});
 }
 void forget(VkImage image){images.erase(image);}
 bool observe(VkImage image,VkImageSubresourceRange range,VkImageLayout layout){
  auto i=images.find(image);if(i==images.end()||!rangeFor(i->second,range))return false;
  for(uint32_t l=range.baseArrayLayer;l<range.baseArrayLayer+range.layerCount;++l)for(uint32_t m=range.baseMipLevel;m<range.baseMipLevel+range.levelCount;++m)i->second.values[size_t(l)*i->second.mips+m]=layout;
  return true;
 }
 std::optional<VkImageLayout> whole(VkImage image)const{
  auto i=images.find(image);if(i==images.end())return {};auto layout=i->second.values.front();
  for(auto v:i->second.values)if(v!=layout)return {};return layout;
 }
 // Keep all of the engine's barriers and discard requests. Repair only a
 // known, initialized source layout. Mixed mips are split rather than being
 // summarized as UNDEFINED (which would silently discard their contents).
 uint32_t transition(const VkImageMemoryBarrier& input,bool repair,std::vector<VkImageMemoryBarrier>& output){
  auto i=images.find(input.image);auto range=input.subresourceRange;
  if(i==images.end()||!rangeFor(i->second,range)){output.push_back(input);return 0;}
  uint32_t changes=0;auto& image=i->second;
  if(!repair){observe(input.image,range,input.newLayout);output.push_back(input);return 0;}
  for(uint32_t l=range.baseArrayLayer;l<range.baseArrayLayer+range.layerCount;++l)for(uint32_t m=range.baseMipLevel;m<range.baseMipLevel+range.levelCount;++m){
   auto b=input;auto& actual=image.values[size_t(l)*image.mips+m];b.subresourceRange={range.aspectMask,m,1,l,1};
   if(b.oldLayout!=VK_IMAGE_LAYOUT_UNDEFINED&&actual!=VK_IMAGE_LAYOUT_UNDEFINED&&b.oldLayout!=actual){b.oldLayout=actual;++changes;}
   actual=b.newLayout;output.push_back(b);
  }
  return changes;
 }
};
}

#include <vulkan/vulkan.h>
#include <unordered_map>
#include <vector>
#include <stdexcept>
#include <iostream>
namespace kharvox::native {[[noreturn]] void fail(const char* text){throw std::runtime_error(text);}}
namespace {
std::unordered_map<VkImage,VkImageLayout> mirror_image_layouts;
#include "../src/native/NativeImageLayouts.inc"
}
int main(){
 int failures=0;auto check=[&](bool value){if(!value)++failures;};
 auto rejects=[&](auto fn){try{fn();++failures;}catch(const std::runtime_error&){}};
 const auto image=reinterpret_cast<VkImage>(uintptr_t(1));
 register_mirror_layouts(image,11,2,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_FORMAT_R8G8B8A8_UNORM);
 // Rendering changes only mip 0/layer 0. Sampling all mips must recognize
 // the mixed state, and transition each subresource from its real layout.
 set_mirror_range_layout(image,{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1},VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
 check(mirror_image_layouts[image]==VK_IMAGE_LAYOUT_UNDEFINED);
 VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=image;b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,VK_REMAINING_MIP_LEVELS,0,VK_REMAINING_ARRAY_LAYERS};
 std::vector<VkImageMemoryBarrier> barriers;append_mirror_transitions(barriers,b);
 check(barriers.size()==22);check(barriers[0].oldLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
 for(size_t i=1;i<barriers.size();++i)check(barriers[i].oldLayout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
 check(mirror_image_layouts[image]==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
 // The two levels of a mip blit require independent source/destination states.
 barriers.clear();b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,2,1,1,1};append_mirror_transitions(barriers,b);
 b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.subresourceRange.baseMipLevel=3;append_mirror_transitions(barriers,b);
 check(barriers.size()==2);check(native_image_layouts[image].values[13]==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);check(native_image_layouts[image].values[14]==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);check(native_image_layouts[image].values[2]==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
 rejects([&]{checked_mirror_range(image,{VK_IMAGE_ASPECT_COLOR_BIT,11,1,0,1});});
 rejects([&]{checked_mirror_range(image,{VK_IMAGE_ASPECT_COLOR_BIT,10,2,0,1});});
 rejects([&]{checked_mirror_range(image,{VK_IMAGE_ASPECT_COLOR_BIT,0,1,2,1});});
 rejects([&]{checked_mirror_range(image,{VK_IMAGE_ASPECT_COLOR_BIT,0,0,0,1});});
 auto range=checked_mirror_range(image,{VK_IMAGE_ASPECT_COLOR_BIT,7,VK_REMAINING_MIP_LEVELS,1,VK_REMAINING_ARRAY_LAYERS});check(range.levelCount==4&&range.layerCount==1);
 const auto depth=reinterpret_cast<VkImage>(uintptr_t(2));register_mirror_layouts(depth,1,1,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,VK_FORMAT_D24_UNORM_S8_UINT);
 barriers.clear();b.image=depth;b.newLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;b.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};append_mirror_transitions(barriers,b);
 check(barriers[0].subresourceRange.aspectMask==(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT));
 // An explicit discard remains explicit; ordinary transitions never turn a
 // mixed/unknown scalar summary into an UNDEFINED old layout and lose pixels.
 barriers.clear();b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;append_mirror_transitions(barriers,b,true);check(barriers[0].oldLayout==VK_IMAGE_LAYOUT_UNDEFINED);
 std::cout<<"Native image subresource rules failures="<<failures<<'\n';return failures?1:0;
}

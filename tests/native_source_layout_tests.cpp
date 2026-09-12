#include "../src/native/NativeSourceLayoutState.h"
#include <iostream>
using namespace kharvox::native;
int main(){int failures=0;auto check=[&](bool ok){if(!ok)++failures;};
 SourceLayoutState state;const auto image=reinterpret_cast<VkImage>(uintptr_t(1));
 state.define(image,1,1,VK_IMAGE_LAYOUT_UNDEFINED);
 VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
 VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=image;b.subresourceRange=range;
 std::vector<VkImageMemoryBarrier> out;
 // The left pass performs implicit attachment transitions, then an explicit
 // sampled-read transition. The redirected right pass does not do the first
 // transition on the original. Keep its barrier, with the actual old layout.
 check(state.observe(image,range,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL));
 b.oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
 check(state.transition(b,false,out)==0);out.clear();check(state.transition(b,true,out)==1);
 check(out.size()==1&&out[0].image==image&&out[0].oldLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL&&out[0].newLayout==b.newLayout);
 // A later compute-use transition can change the original state seen by the
 // next left frame, so that barrier also needs the proven source layout.
 b.oldLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_GENERAL;out.clear();check(state.transition(b,true,out)==0);
 out.clear();check(state.transition(b,true,out)==1&&out[0].oldLayout==VK_IMAGE_LAYOUT_GENERAL);
 // Source lifetime: no state from a retired handle may initialize its reuse.
 state.forget(image);check(!state.whole(image));out.clear();check(state.transition(b,true,out)==0&&out[0].oldLayout==b.oldLayout);
 state.define(image,3,2,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
 check(state.observe(image,{VK_IMAGE_ASPECT_COLOR_BIT,1,1,1,1},VK_IMAGE_LAYOUT_GENERAL));check(!state.whole(image));
 b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,VK_REMAINING_MIP_LEVELS,0,VK_REMAINING_ARRAY_LAYERS};b.oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
 out.clear();check(state.transition(b,true,out)==1&&out.size()==6);check(out[4].oldLayout==VK_IMAGE_LAYOUT_GENERAL&&out[4].subresourceRange.baseMipLevel==1&&out[4].subresourceRange.baseArrayLayer==1);
 check(state.whole(image)==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
 b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;out.clear();check(state.transition(b,true,out)==0);for(auto& v:out)check(v.oldLayout==VK_IMAGE_LAYOUT_UNDEFINED);
 check(!state.observe(image,{VK_IMAGE_ASPECT_COLOR_BIT,3,1,0,1},VK_IMAGE_LAYOUT_GENERAL));
 check(!state.observe(image,{VK_IMAGE_ASPECT_COLOR_BIT,0,1,2,1},VK_IMAGE_LAYOUT_GENERAL));
 state.define(image,1,1,VK_IMAGE_LAYOUT_UNDEFINED);out.clear();b.subresourceRange=range;b.oldLayout=VK_IMAGE_LAYOUT_GENERAL;check(state.transition(b,true,out)==0&&out[0].oldLayout==VK_IMAGE_LAYOUT_GENERAL);
 std::cout<<"Native original-image layout and lifetime failures="<<failures<<'\n';return failures?1:0;
}

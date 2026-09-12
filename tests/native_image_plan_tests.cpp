#include "../src/native/NativeImagePlan.h"
#include <cstdlib>
#include <map>
using namespace kharvox::native;
static void require(bool ok){if(!ok)std::abort();}
int main(){
 BindingMutationClock clock;
 ImagePlanCache<int,1> cache;
 auto original=std::make_shared<ImageLayoutPlan>();
 original->push_back({reinterpret_cast<VkImage>(1),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1},VK_FORMAT_R8G8B8A8_UNORM});
 cache.put(1,original,clock.stamp(),clock.stamp());
 auto pinned=cache.find(1,clock.stamp());require(pinned&&pinned->size()==1);
 // A new mirror or a changed layout needs no descriptor metadata mutation.
 // The SAME cached plan must observe both changes on its next execution.
 VkImage mirror{};VkImageLayout current=VK_IMAGE_LAYOUT_UNDEFINED;bool written=false;
 int transitions=0;VkImage last{};
 auto run=[&](bool initialize=false){visitImageLayoutPlan<VkImage>(*pinned,initialize,
  [&](VkImage source){require(source==reinterpret_cast<VkImage>(1));return mirror;},
  [&](VkImage){return current;},[&](VkImage){return written;},
  [&](const ImageLayoutRequirement& input,VkImage image,VkImageLayout have){require(have==current);++transitions;last=image;current=input.want;});};
 run();require(transitions==0);
 mirror=reinterpret_cast<VkImage>(2);run();require(transitions==0);
 written=true;run();require(transitions==1&&last==mirror);
 run();require(transitions==1);
 current=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;run();require(transitions==2);
 mirror=reinterpret_cast<VkImage>(3);current=VK_IMAGE_LAYOUT_GENERAL;run();require(transitions==3&&last==mirror);
 written=false;current=VK_IMAGE_LAYOUT_UNDEFINED;run(true);require(transitions==4);
 // A descriptor/view/framebuffer mutation invalidates the owned metadata.
 const auto before=clock.stamp();clock.begin();require(!cache.find(1,clock.stamp()));
 cache.put(2,original,before,clock.stamp());clock.end();require(!cache.find(1,clock.stamp()));require(!cache.find(2,clock.stamp()));
 auto replacement=std::make_shared<ImageLayoutPlan>();
 cache.put(1,replacement,clock.stamp(),clock.stamp());require(cache.find(1,clock.stamp())->empty());
 require(pinned->size()==1); // outstanding invocations retain their own plan
 cache.put(2,original,clock.stamp(),clock.stamp());require(!cache.find(2,clock.stamp()));
 cache.clear();require(!cache.find(1,clock.stamp())&&pinned->size()==1);

 ImageInputVisits<int,2> visits;
 auto stamp=clock.stamp();visits.remember(10,stamp,stamp);visits.remember(20,stamp,stamp);
 require(visits.contains(10,stamp)&&visits.contains(20,stamp));
 // Set 10 remains bound while set 20 is changed. Reconsider BOTH sets under
 // the new metadata revision, not just the most recently rebound range.
 clock.begin();require(!visits.contains(10,clock.stamp()));clock.end();
 require(!visits.contains(10,clock.stamp())&&!visits.contains(20,clock.stamp()));
 visits.remember(10,stamp,clock.stamp());require(!visits.contains(10,clock.stamp()));
 stamp=clock.stamp();visits.remember(10,stamp,stamp);require(visits.contains(10,stamp));
 visits.remember(30,stamp,stamp);require(!visits.contains(30,stamp)); // bounded miss, not false success
 visits.clear();require(!visits.contains(10,stamp)); // new pass needs the full union again
 return 0;
}

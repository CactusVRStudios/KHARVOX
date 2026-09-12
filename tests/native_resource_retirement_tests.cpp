#include "../src/native/NativeResourceRetirement.h"
#include <cstdint>
#include <vector>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
using namespace kharvox::native;
static VkDevice device=reinterpret_cast<VkDevice>(uintptr_t(1));
static VkDeviceMemory memory=reinterpret_cast<VkDeviceMemory>(uintptr_t(2));
static VkImage image=reinterpret_cast<VkImage>(uintptr_t(2)); // same bits, different type
static bool memoryLive=true,imageLive=true;
static std::vector<int> calls;
static const VkAllocationCallbacks* expectedAllocator{};
static void VKAPI_CALL freeMemory(VkDevice d,VkDeviceMemory m,const VkAllocationCallbacks* a){
 assert(d==device&&m==memory&&a==expectedAllocator&&!memoryLive);calls.push_back(1);
}
static void VKAPI_CALL destroyImage(VkDevice d,VkImage i,const VkAllocationCallbacks* a){
 assert(d==device&&i==image&&a==expectedAllocator&&!imageLive);calls.push_back(2);
}
int main(){
 NativeRetirementQueue queue;
 using Result=NativeRetirementQueue::Result;
 auto retire=[](const NativeRetirementRequest& request){
  retireNativeResource(request,[](VkDeviceMemory m){assert(m==memory&&memoryLive);memoryLive=false;},
   [](VkImage i){assert(i==image&&imageLive);imageLive=false;});
 };
 // SteamVR can request free-memory then destroy-image in the same outstanding
 // frame. Both metadata and objects must survive, and both dispatches must run
 // once in caller order, after the owner confirms retirement.
 for(bool reverse:{false,true}){
  memoryLive=imageLive=true;calls.clear();
  assert(queue.begin([]{}));
  NativeRetirementRequest m=NativeMemoryFree{device,memory,nullptr,freeMemory};
  NativeRetirementRequest i=NativeImageDestroy{device,image,nullptr,destroyImage};
  assert(queue.release(reverse?i:m,true,retire)==Result::Deferred);
  assert(queue.release(reverse?m:i,true,retire)==Result::Deferred);
  assert(queue.release(i,true,retire)==Result::Refused);
  assert(queue.release(m,true,retire)==Result::Refused);
  assert(memoryLive&&imageLive&&calls.empty());
  assert(queue.complete(retire,[]{})==2);
  assert(!memoryLive&&!imageLive);
  assert((calls==(reverse?std::vector<int>{2,1}:std::vector<int>{1,2})));
  assert(queue.complete(retire,[]{})==0);
 }
 // A callback-bearing request can run synchronously outside a frame, but
 // must never capture caller-owned callback data for deferred use.
 VkAllocationCallbacks callbacks{};expectedAllocator=&callbacks;imageLive=true;
 NativeRetirementRequest custom=NativeImageDestroy{device,image,&callbacks,destroyImage};
 assert(queue.begin([]{}));
 assert(queue.release(custom,false,retire)==Result::Refused);
 assert(imageLive&&queue.complete(retire,[]{})==0);
 assert(queue.release(custom,false,retire)==Result::Released);
 assert(!imageLive);
}

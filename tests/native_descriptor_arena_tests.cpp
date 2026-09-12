#include "../src/native/NativeDescriptorArena.h"
#include <cstdlib>
#include <cstdint>
#include <iostream>
using namespace kharvox::native;
static void require(bool ok){if(!ok)std::abort();}
template<class T> static T handle(uintptr_t n){return reinterpret_cast<T>(n);}
static const auto device=handle<VkDevice>(1);
static const auto layout=handle<VkDescriptorSetLayout>(2);
struct Mock {
 uintptr_t nextPool=100,nextSet=10000;
 uint32_t createCalls{},allocateCalls{},freeCalls{},destroyCalls{};
 VkResult allocationFailure=VK_SUCCESS,freeFailure=VK_SUCCESS,creationFailure=VK_SUCCESS;
 bool fragmentFirst{};
 uint32_t firstPageLimit=1024;
 std::map<VkDescriptorPool,uint32_t> counts;
 std::map<VkDescriptorSet,VkDescriptorPool> owners;
} mock;
static VKAPI_ATTR VkResult VKAPI_CALL create(VkDevice,const VkDescriptorPoolCreateInfo* info,const VkAllocationCallbacks*,VkDescriptorPool* pool){
 ++mock.createCalls;require(info->maxSets==1024&&(info->flags&VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT));
 if(mock.creationFailure!=VK_SUCCESS)return mock.creationFailure;
 *pool=handle<VkDescriptorPool>(mock.nextPool++);mock.counts[*pool]=0;return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL allocate(VkDevice,const VkDescriptorSetAllocateInfo* info,VkDescriptorSet* set){
 ++mock.allocateCalls;*set=VK_NULL_HANDLE;require(info->descriptorSetCount==1&&mock.counts.count(info->descriptorPool));
 if(mock.allocationFailure!=VK_SUCCESS)return mock.allocationFailure;
 if(mock.fragmentFirst&&info->descriptorPool==handle<VkDescriptorPool>(100))return VK_ERROR_FRAGMENTED_POOL;
 auto& count=mock.counts.at(info->descriptorPool);if(count==(info->descriptorPool==handle<VkDescriptorPool>(100)?mock.firstPageLimit:1024))return VK_ERROR_OUT_OF_POOL_MEMORY;
 *set=handle<VkDescriptorSet>(mock.nextSet++);mock.owners[*set]=info->descriptorPool;++count;return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL freeSets(VkDevice,VkDescriptorPool pool,uint32_t count,const VkDescriptorSet* sets){
 ++mock.freeCalls;for(uint32_t i=0;i<count;++i)require(mock.owners.at(sets[i])==pool);
 if(mock.freeFailure!=VK_SUCCESS)return mock.freeFailure;
 for(uint32_t i=0;i<count;++i)mock.owners.erase(sets[i]);mock.counts.at(pool)-=count;return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL destroy(VkDevice,VkDescriptorPool pool,const VkAllocationCallbacks*){
 ++mock.destroyCalls;require(mock.counts.erase(pool)==1);
 for(auto it=mock.owners.begin();it!=mock.owners.end();)if(it->second==pool)it=mock.owners.erase(it);else ++it;
}
static DescriptorArena makeArena(){DescriptorArena arena;require(arena.initialize(device,{create,allocate,freeSets,destroy}));return arena;}
int main(){
 // Regression: the real run failed with 1024 live clones and zero retired.
 auto arena=makeArena();std::vector<VkDescriptorSet> sets;
 for(int i=0;i<1100;++i){VkDescriptorSet set{};require(arena.allocate(layout,&set)==VK_SUCCESS);sets.push_back(set);}
 require(arena.pageCount()==2&&arena.allocatedCount()==1100);
 require(arena.poolFor(sets[0])!=arena.poolFor(sets[1024]));
 std::vector<VkDescriptorSet> crossPool{sets[0],sets[1024]};
 require(arena.freeCompleted(crossPool,false)==VK_NOT_READY&&mock.freeCalls==0);
 require(arena.destroyCompleted(false)==VK_NOT_READY&&mock.destroyCalls==0);
 require(arena.freeCompleted({sets[0],sets[0]},true)==VK_ERROR_UNKNOWN&&mock.freeCalls==0);
 require(arena.freeCompleted({handle<VkDescriptorSet>(999)},true)==VK_ERROR_UNKNOWN&&mock.freeCalls==0);
 mock.freeFailure=VK_ERROR_UNKNOWN;
 require(arena.freeCompleted({sets[0]},true)==VK_ERROR_UNKNOWN&&arena.allocatedCount()==1100);
 mock.freeFailure=VK_SUCCESS;
 require(arena.freeCompleted(crossPool,true)==VK_SUCCESS&&arena.allocatedCount()==1098);
 // Slot reuse must preserve page ownership and cannot grow another page.
 mock.nextSet=reinterpret_cast<uintptr_t>(sets[0]);VkDescriptorSet recycled{};
 require(arena.allocate(layout,&recycled)==VK_SUCCESS&&recycled==sets[0]);
 require(arena.pageCount()==2&&arena.poolFor(recycled)==handle<VkDescriptorPool>(100));
 require(!arena.initialize(handle<VkDevice>(3),{create,allocate,freeSets,destroy}));
 require(arena.destroyCompleted(true)==VK_SUCCESS&&mock.owners.empty()&&mock.counts.empty());
 // Real device/host-memory errors must not be hidden by page growth.
 mock={};arena=makeArena();VkDescriptorSet set{};require(arena.allocate(layout,&set)==VK_SUCCESS);
 mock.allocationFailure=VK_ERROR_OUT_OF_DEVICE_MEMORY;
 require(arena.allocate(layout,&set)==VK_ERROR_OUT_OF_DEVICE_MEMORY&&mock.createCalls==1&&set==VK_NULL_HANDLE);
 mock.allocationFailure=VK_ERROR_OUT_OF_HOST_MEMORY;
 require(arena.allocate(layout,&set)==VK_ERROR_OUT_OF_HOST_MEMORY&&mock.createCalls==1);
 require(arena.destroyCompleted(true)==VK_SUCCESS);
 // Fragmented/type-exhausted pages may move to a fresh page; no infinite retry.
 mock={};arena=makeArena();require(arena.allocate(layout,&set)==VK_SUCCESS);mock.fragmentFirst=true;
 require(arena.allocate(layout,&set)==VK_SUCCESS&&arena.pageCount()==2);
 const auto calls=mock.allocateCalls;require(arena.allocate(layout,&set)==VK_SUCCESS&&mock.allocateCalls==calls+1);
 require(arena.destroyCompleted(true)==VK_SUCCESS);
 // A descriptor-type budget can run out long before maxSets. Freeing through
 // its real parent clears the exhausted-layout hint and permits page reuse.
 mock={};mock.firstPageLimit=1;arena=makeArena();VkDescriptorSet first{};
 require(arena.allocate(layout,&first)==VK_SUCCESS);
 require(arena.allocate(layout,&set)==VK_SUCCESS&&arena.pageCount()==2&&arena.allocatedCount()==2);
 require(arena.freeCompleted({first},true)==VK_SUCCESS);
 require(arena.allocate(layout,&set)==VK_SUCCESS&&arena.poolFor(set)==handle<VkDescriptorPool>(100));
 require(arena.destroyCompleted(true)==VK_SUCCESS);
 mock={};arena=makeArena();mock.creationFailure=VK_ERROR_OUT_OF_HOST_MEMORY;
 require(arena.allocate(layout,&set)==VK_ERROR_OUT_OF_HOST_MEMORY&&!arena.pageCount()&&mock.createCalls==1);
 require(arena.destroyCompleted(true)==VK_SUCCESS);
 // Sustained live demand remains bounded even if a source never retires.
 mock={};arena=makeArena();
 for(uint32_t i=0;i<DescriptorArena::setsPerPage*DescriptorArena::maxPages;++i)require(arena.allocate(layout,&set)==VK_SUCCESS);
 require(arena.allocate(layout,&set)==VK_ERROR_OUT_OF_POOL_MEMORY&&mock.createCalls==DescriptorArena::maxPages);
 require(arena.destroyCompleted(true)==VK_SUCCESS&&mock.owners.empty());
 std::cout<<"Descriptor arena: >1024 live sets, per-pool frees, GPU completion, recycling, errors and growth bound passed\n";
}

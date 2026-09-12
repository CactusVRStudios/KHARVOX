#pragma once
#include <vulkan/vulkan.h>
#include <array>
#include <cstddef>
#include <map>
#include <set>
#include <vector>

namespace kharvox::native {
// Caller owns synchronization. Pages grow only for pool exhaustion, never for
// device/host allocation failures. Every set retains its actual parent pool.
class DescriptorArena {
public:
 static constexpr uint32_t setsPerPage=1024, maxPages=16;
 struct Dispatch {
  PFN_vkCreateDescriptorPool create{};
  PFN_vkAllocateDescriptorSets allocate{};
  PFN_vkFreeDescriptorSets free{};
  PFN_vkDestroyDescriptorPool destroy{};
 };
private:
 struct Page {VkDescriptorPool pool{};uint32_t live{};std::set<VkDescriptorSetLayout> exhausted;};
 VkDevice device{};Dispatch api{};
 std::vector<Page> pages;
 std::map<VkDescriptorSet,size_t> owners;
 VkResult allocateFrom(size_t index,VkDescriptorSetLayout layout,VkDescriptorSet* result){
  auto& page=pages[index];
  VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  info.descriptorPool=page.pool;info.descriptorSetCount=1;info.pSetLayouts=&layout;
  const auto status=api.allocate(device,&info,result);
  if(status==VK_SUCCESS){
   if(!*result||owners.count(*result))return VK_ERROR_UNKNOWN;
   owners.emplace(*result,index);++page.live;
  }else if(status==VK_ERROR_OUT_OF_POOL_MEMORY||status==VK_ERROR_FRAGMENTED_POOL){
   page.exhausted.insert(layout);
  }
  return status;
 }
public:
 bool initialize(VkDevice value,Dispatch dispatch){
  if(device)return device==value;
  if(!value||!dispatch.create||!dispatch.allocate||!dispatch.free||!dispatch.destroy)return false;
  device=value;api=dispatch;return true;
 }
 size_t pageCount()const{return pages.size();}
 bool ownsDevice(VkDevice value)const{return device&&device==value;}
 size_t allocatedCount()const{return owners.size();}
 size_t capacity()const{return pages.size()*setsPerPage;}
 VkDescriptorPool poolFor(VkDescriptorSet set)const{auto it=owners.find(set);return it==owners.end()?VK_NULL_HANDLE:pages[it->second].pool;}
 VkResult allocate(VkDescriptorSetLayout layout,VkDescriptorSet* result){
  if(!result||!device||!layout)return VK_ERROR_INITIALIZATION_FAILED;
  *result=VK_NULL_HANDLE;
  for(size_t i=0;i<pages.size();++i){
   if(pages[i].live>=setsPerPage||pages[i].exhausted.count(layout))continue;
   const auto status=allocateFrom(i,layout,result);
   if(status!=VK_ERROR_OUT_OF_POOL_MEMORY&&status!=VK_ERROR_FRAGMENTED_POOL)return status;
  }
  if(pages.size()>=maxPages)return VK_ERROR_OUT_OF_POOL_MEMORY;
  const std::array<VkDescriptorPoolSize,8> sizes{{
   {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,8192},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,4096},
   {VK_DESCRIPTOR_TYPE_SAMPLER,2048},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,2048},
   {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,2048},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,1024},
   {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,4096},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,4096}}};
  VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  info.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;info.maxSets=setsPerPage;
  info.poolSizeCount=uint32_t(sizes.size());info.pPoolSizes=sizes.data();VkDescriptorPool pool{};
  const auto status=api.create(device,&info,nullptr,&pool);
  if(status!=VK_SUCCESS)return status;
  if(!pool)return VK_ERROR_UNKNOWN;
  pages.push_back({pool});
  return allocateFrom(pages.size()-1,layout,result);
 }
 VkResult freeCompleted(const std::vector<VkDescriptorSet>& sets,bool gpuComplete){
  if(sets.empty())return VK_SUCCESS;
  if(!gpuComplete)return VK_NOT_READY;
  std::map<size_t,std::vector<VkDescriptorSet>> groups;std::set<VkDescriptorSet> unique;
  for(auto set:sets){auto it=owners.find(set);if(it==owners.end()||!unique.insert(set).second)return VK_ERROR_UNKNOWN;groups[it->second].push_back(set);}
  for(const auto&[index,group]:groups){
   auto& page=pages[index];const auto status=api.free(device,page.pool,uint32_t(group.size()),group.data());
   if(status!=VK_SUCCESS)return status;
   for(auto set:group)owners.erase(set);
   page.live-=uint32_t(group.size());page.exhausted.clear();
  }
  return VK_SUCCESS;
 }
 VkResult destroyCompleted(bool gpuComplete){
  if(!pages.empty()&&!gpuComplete)return VK_NOT_READY;
  for(const auto& page:pages)api.destroy(device,page.pool,nullptr);
  owners.clear();pages.clear();device=VK_NULL_HANDLE;api={};return VK_SUCCESS;
 }
};
}

static DescriptorArena redirectedArena;
static std::mutex redirectedArenaMutex;
void logDescriptorArenaStats(){
 std::scoped_lock l(redirectedArenaMutex);
 kharvoxnative::log::info(std::format("Native descriptor arena live={} pages={} capacity={} made={} freed={} retiredPending={} allocationFailures={} retirement=verifiedDeviceCompletion",redirectedArena.allocatedCount(),redirectedArena.pageCount(),redirectedArena.capacity(),read_redirect_clones.load(),rr_clones_freed.load(),rr_retire_queue_depth.load(),rr_clone_alloc_failures.load()));
}
VkResult allocateRedirectedDescriptor(VkDevice device,VkDescriptorSetLayout layout,VkDescriptorSet* set){
 std::scoped_lock l(redirectedArenaMutex);
 if(!redirectedArena.ownsDevice(device)&&!redirectedArena.initialize(device,{
  load_device_proc<PFN_vkCreateDescriptorPool>(device,"vkCreateDescriptorPool"),
  real_allocate_descriptor_sets,
  load_device_proc<PFN_vkFreeDescriptorSets>(device,"vkFreeDescriptorSets"),
  load_device_proc<PFN_vkDestroyDescriptorPool>(device,"vkDestroyDescriptorPool")}))return VK_ERROR_INITIALIZATION_FAILED;
 const auto before=redirectedArena.pageCount();
 const auto result=redirectedArena.allocate(layout,set);
 if(before!=redirectedArena.pageCount())kharvoxnative::log::info(std::format("Native descriptor arena pages={} capacity={} allocated={} maxPages={} sourceLifetimeTracking=true",redirectedArena.pageCount(),redirectedArena.capacity(),redirectedArena.allocatedCount(),DescriptorArena::maxPages));
 if(result!=VK_SUCCESS){rr_clone_alloc_failures.fetch_add(1);kharvoxnative::log::error(std::format("Native descriptor arena refused result={} pages={} allocated={} boundedCapacity={}",int(result),redirectedArena.pageCount(),redirectedArena.allocatedCount(),DescriptorArena::setsPerPage*DescriptorArena::maxPages));}
 return result;
}
// Only called after the existing owner's successful vkDeviceWaitIdle. Never
// free through the first page merely because that was the historical pool.
static void freeRetiredRedirectedDescriptors(){
 std::vector<VkDescriptorSet> sets;for(const auto& r:retired_clones)sets.push_back(r.clone);
 if(sets.empty())return;
 std::scoped_lock l(redirectedArenaMutex);
 if(redirectedArena.freeCompleted(sets,true)!=VK_SUCCESS)fail("native descriptor page reclamation failed after GPU completion");
 rr_clones_freed.fetch_add(uint32_t(sets.size()),std::memory_order_relaxed);
 retired_clones.clear();rr_retire_queue_depth.store(0,std::memory_order_relaxed);
}
static void destroyRedirectedArena(VkDevice device){
 std::scoped_lock l(redirectedArenaMutex);
 if(!redirectedArena.pageCount())return;
 if(!real_device_wait_idle||real_device_wait_idle(device)!=VK_SUCCESS)fail("native descriptor arena teardown completion failed");
 if(redirectedArena.destroyCompleted(true)!=VK_SUCCESS)fail("native descriptor arena teardown failed");
}

static std::mutex descriptorLifetimeMutex;
static DescriptorPoolLedger descriptorPoolLedger;
static uint64_t forgottenSourceDescriptors{};
void recordDescriptorPoolAllocation(VkDescriptorPool pool,uint32_t count,const VkDescriptorSet* sets){
 std::scoped_lock l(descriptorLifetimeMutex);for(uint32_t i=0;i<count;++i)descriptorPoolLedger.allocated(reinterpret_cast<uintptr_t>(pool),reinterpret_cast<uintptr_t>(sets[i]));
}
void forgetSourceDescriptors(uint32_t count,const VkDescriptorSet* sets){
 {std::scoped_lock l(read_redirect_mutex);for(uint32_t i=0;i<count;++i){auto set=sets[i];drop_redirected_set(set);sets_needing_no_redirect.erase(set);sets_with_untracked_bindings.erase(set);descriptor_set_layouts.erase(set);descriptor_set_alloc_frame.erase(set);descriptor_set_update_frame.erase(set);}}
 {std::scoped_lock l(swapchains_mutex);for(uint32_t i=0;i<count;++i){descriptor_sets.erase(sets[i]);descriptor_image_sets.erase(sets[i]);descriptor_set_rings.erase(sets[i]);}}
 {std::scoped_lock l(descriptorLifetimeMutex);for(uint32_t i=0;i<count;++i)descriptorPoolLedger.freed(reinterpret_cast<uintptr_t>(sets[i]));forgottenSourceDescriptors+=count;}
 // Clones are only RETIRED here. The owner frees them after verified device
 // completion; source lifetime alone is not proof that GPU consumers finished.
}
void forgetDescriptorPool(VkDescriptorPool pool){
 std::vector<uint64_t> members;{std::scoped_lock l(descriptorLifetimeMutex);members=descriptorPoolLedger.reset(reinterpret_cast<uintptr_t>(pool));}
 std::vector<VkDescriptorSet> sets;sets.reserve(members.size());for(auto member:members)sets.push_back(reinterpret_cast<VkDescriptorSet>(uintptr_t(member)));
 if(!sets.empty())forgetSourceDescriptors(uint32_t(sets.size()),sets.data());
}

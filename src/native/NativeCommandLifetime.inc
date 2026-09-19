static std::map<VkCommandBuffer,VkCommandPool> commandPools;
static void resetCommandReplay(VkCommandBuffer cb){
 replayStates.erase(cb);
 resetStorageSnapshotBindings(cb);
 rightEyeBindingCommands.erase(cb);
}
static void forgetCommandReplay(VkCommandBuffer cb){resetCommandReplay(cb);boundStorageSnapshots.erase(cb);}
static void guardLoadingCommandReuse(VkCommandBuffer cb){if(loadingDrain.active()&&loadingDrain.command()==reinterpret_cast<uintptr_t>(cb))fail("loading command reused before owner completion");}
static VkResult VKAPI_CALL replayAllocateCommands(VkDevice device,const VkCommandBufferAllocateInfo* info,VkCommandBuffer* commands){
 std::scoped_lock lock(replayMutex);
 const auto result=hook_allocate_command_buffers(device,info,commands);
 if(result==VK_SUCCESS)for(uint32_t i=0;i<info->commandBufferCount;++i){forgetCommandReplay(commands[i]);commandPools[commands[i]]=info->commandPool;}
 return result;
}
static VkResult VKAPI_CALL replayBeginCommand(VkCommandBuffer cb,const VkCommandBufferBeginInfo* info){
 std::scoped_lock lock(replayMutex);guardLoadingCommandReuse(cb);resetCommandReplay(cb);
 const auto result=nextBeginCommand(cb,info);replayStates[cb].recording=result==VK_SUCCESS;return result;
}
static PFN_vkResetCommandBuffer nextResetCommand{};
static VkResult VKAPI_CALL replayResetCommand(VkCommandBuffer cb,VkCommandBufferResetFlags flags){
 std::scoped_lock lock(replayMutex);guardLoadingCommandReuse(cb);
 const auto result=nextResetCommand(cb,flags);if(result==VK_SUCCESS)resetCommandReplay(cb);return result;
}
static PFN_vkResetCommandPool nextResetCommandPool{};
static VkResult VKAPI_CALL replayResetCommandPool(VkDevice device,VkCommandPool pool,VkCommandPoolResetFlags flags){
 std::scoped_lock lock(replayMutex);
 if(loadingDrain.active())fail("command pool reset overlaps loading drain");
 const auto result=nextResetCommandPool(device,pool,flags);
 if(result==VK_SUCCESS)for(const auto&[cb,owner]:commandPools)if(owner==pool)resetCommandReplay(cb);
 return result;
}
static PFN_vkFreeCommandBuffers nextFreeCommands{};
static void VKAPI_CALL replayFreeCommands(VkDevice device,VkCommandPool pool,uint32_t count,const VkCommandBuffer* commands){
 std::scoped_lock lock(replayMutex);
 for(uint32_t i=0;i<count;++i)guardLoadingCommandReuse(commands[i]);
 nextFreeCommands(device,pool,count,commands);
 for(uint32_t i=0;i<count;++i){forgetCommandReplay(commands[i]);commandPools.erase(commands[i]);}
}
static void VKAPI_CALL replayDestroyCommandPool(VkDevice device,VkCommandPool pool,const VkAllocationCallbacks* allocator){
 std::scoped_lock lock(replayMutex);
 hook_destroy_command_pool(device,pool,allocator);
 for(auto it=commandPools.begin();it!=commandPools.end();){
  if(it->second==pool){forgetCommandReplay(it->first);it=commandPools.erase(it);}else ++it;
 }
}
void releaseStorageBindingBookkeeping(){
 beforeDestroy("command bookkeeping release",0);std::scoped_lock lock(replayMutex);
 replayStates.clear();commandPools.clear();boundStorageSnapshots.clear();snapshotBatchCache.clear();rightEyeBindingCommands.clear();
}

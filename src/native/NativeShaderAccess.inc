// Native-only reflection. No shader bytecode is modified or saved.
static bool readOnlyStorageSnapshotTest(){static const bool enabled=!kharvox::runtimeFileExists(L"native_test_shared_storage_inputs");return enabled;}
static std::mutex shaderAccessMutex;
static std::map<VkShaderModule,ShaderStorageAccess> shaderStorageAccess;
static std::map<VkPipeline,ShaderStorageAccess> pipelineStorageAccess;
static SnapshotAccessClasses snapshotAccessClasses;
static std::map<VkPipeline,SnapshotAccessClasses::Pipeline> pipelineSnapshotClasses;
static std::optional<uint64_t> snapshotClass(VkPipeline pipeline,uint32_t set,VkPipelineBindPoint point){
 if(point!=VK_PIPELINE_BIND_POINT_GRAPHICS||!readOnlyStorageSnapshotTest())return uint64_t(1);
 std::scoped_lock lock(shaderAccessMutex);
 const auto it=pipelineSnapshotClasses.find(pipeline);
 return it==pipelineSnapshotClasses.end()?std::nullopt:it->second.forSet(set);
}
static std::map<VkPipeline,std::set<std::pair<uint32_t,uint32_t>>> reportedStorageAccess;
static std::map<VkCommandBuffer,std::map<uint32_t,StorageBindingSet>> boundStorageSnapshots;
void releaseStorageBindingBookkeeping(){
 // Pool destruction implicitly frees its command buffers. At this verified
 // non-outstanding boundary it is safe to release all CPU-only capacity;
 // live command buffers repopulate it on their next descriptor binds.
 beforeDestroy("storage binding bookkeeping release",0);std::scoped_lock lock(replayMutex);boundStorageSnapshots.clear();snapshotBatchCache.clear();
}
static void resetStorageSnapshotBindings(VkCommandBuffer cb){
 snapshotBatchCache.invalidate(cb);
 const auto command=boundStorageSnapshots.find(cb);if(command==boundStorageSnapshots.end())return;
 for(auto&[set,bindings]:command->second)bindings.clear();
}
static void clearStorageSnapshotBindings(VkCommandBuffer cb,uint32_t first,uint32_t count){
 snapshotBatchCache.invalidate(cb);
 if(!readOnlyStorageSnapshotTest())return;
 const auto command=boundStorageSnapshots.find(cb);if(command==boundStorageSnapshots.end())return;
 for(uint32_t i=0;i<count;++i){auto set=command->second.find(first+i);if(set!=command->second.end())set->second.clear();}
}
static void validateStorageSnapshotsForDraw(VkCommandBuffer cb){
 cpu::Scope profile(cpu::ValidateStorageDraw);
 if(!readOnlyStorageSnapshotTest())return;
 auto active=boundStorageSnapshots.find(cb);if(active==boundStorageSnapshots.end()||
  std::none_of(active->second.begin(),active->second.end(),[](const auto& set){return !set.second.empty();}))return;
 std::scoped_lock lock(shaderAccessMutex);auto pipeline=pipelineStorageAccess.find(replayStates[cb].pipeline);
 if(pipeline==pipelineStorageAccess.end())fail("storage snapshot draw uses an unobserved pipeline");
 for(const auto&[set,bindings]:active->second)for(auto binding:bindings)
  if(!permitsReadOnlySnapshot(pipeline->second,{set,binding}))fail("pipeline change would make a CPU snapshot writable or unproven");
}
static VkResult VKAPI_CALL accessCreateShader(VkDevice device,const VkShaderModuleCreateInfo* info,const VkAllocationCallbacks* allocator,VkShaderModule* module){
 const auto result=hook_create_shader_module(device,info,allocator,module);
 if(result==VK_SUCCESS&&info&&module&&readOnlyStorageSnapshotTest()){
  auto access=info->pNext||info->codeSize%4?ShaderStorageAccess{}:reflectStorageAccess(info->pCode,info->codeSize/4);
  std::scoped_lock lock(shaderAccessMutex);shaderStorageAccess[*module]=std::move(access);
 }
 return result;
}
static void VKAPI_CALL accessDestroyShader(VkDevice device,VkShaderModule module,const VkAllocationCallbacks* allocator){
 if(readOnlyStorageSnapshotTest()){std::scoped_lock lock(shaderAccessMutex);shaderStorageAccess.erase(module);}
 hook_destroy_shader_module(device,module,allocator);
}
static VkResult VKAPI_CALL accessCreateGraphics(VkDevice device,VkPipelineCache cache,uint32_t count,const VkGraphicsPipelineCreateInfo* infos,const VkAllocationCallbacks* allocator,VkPipeline* pipelines){
 const auto result=hook_create_graphics_pipelines(device,cache,count,infos,allocator,pipelines);
 if(result==VK_SUCCESS&&infos&&pipelines&&readOnlyStorageSnapshotTest()){
  std::scoped_lock lock(shaderAccessMutex);
  for(uint32_t i=0;i<count;++i){if(!pipelines[i])continue;ShaderStorageAccess merged;merged.valid=infos[i].stageCount>0&&!infos[i].pNext;
   for(uint32_t j=0;j<infos[i].stageCount;++j){const auto& stage=infos[i].pStages[j];auto shader=shaderStorageAccess.find(stage.module);
    if(stage.pNext||shader==shaderStorageAccess.end()||!shader->second.valid){merged.valid=false;continue;}
    for(const auto&[key,access]:shader->second.bindings){auto[it,added]=merged.bindings.emplace(key,access);if(!added)it->second.readOnly&=access.readOnly;}
   }
   pipelineSnapshotClasses[pipelines[i]]=snapshotAccessClasses.classify(merged);
   pipelineStorageAccess[pipelines[i]]=std::move(merged);reportedStorageAccess.erase(pipelines[i]);
  }
 }
 return result;
}
static void VKAPI_CALL accessDestroyPipeline(VkDevice device,VkPipeline pipeline,const VkAllocationCallbacks* allocator){
 if(readOnlyStorageSnapshotTest()){std::scoped_lock lock(shaderAccessMutex);pipelineStorageAccess.erase(pipeline);pipelineSnapshotClasses.erase(pipeline);reportedStorageAccess.erase(pipeline);}
 hook_destroy_pipeline(device,pipeline,allocator);
}
static bool readOnlyStorageBinding(VkCommandBuffer cb,uint32_t set,uint32_t binding){
 if(!readOnlyStorageSnapshotTest())return false;
 std::scoped_lock lock(shaderAccessMutex);auto pipeline=pipelineStorageAccess.find(replayStates[cb].pipeline);
 if(pipeline==pipelineStorageAccess.end()||!pipeline->second.valid)fail("CPU storage input uses an unproven shader access declaration");
 auto access=pipeline->second.bindings.find({set,binding});if(access==pipeline->second.bindings.end()||!access->second.readOnly)return false;
 // Formatting this diagnostic on every bind costs CPU time even after the
 // first report. Pipeline lifetime owns the reflection; keep its report keys
 // there so handle recycling cannot suppress the new pipeline's evidence.
 if(reportedStorageAccess[replayStates[cb].pipeline].insert({set,binding}).second)
  kharvoxnative::log::info(std::format("Native readonly CPU storage snapshot eligible by live shader NonWritable: set={} binding={} name={}",set,binding,access->second.name));
 return true;
}

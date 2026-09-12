// Keep CPU-written frame inputs immutable for each recorded eye until the
// owner's verified whole-device completion. The
// game keeps ownership of its mapped memory; we never restore over its writes.
struct UniformBindScratch {
 struct Replacement{uint32_t binding;VkDescriptorBufferInfo buffer;VkDescriptorType type;};
 struct ImageReplacement{uint32_t binding;VkDescriptorImageInfo image;VkDescriptorType type;};
 std::vector<Replacement> replacements;
 std::vector<ImageReplacement> imageReplacements;
 std::vector<uint32_t> bindings;
 DescriptorReuseKey reuseKey;
 std::vector<VkCopyDescriptorSet> copies;
 std::vector<VkWriteDescriptorSet> writes;
 void clear(){replacements.clear();imageReplacements.clear();bindings.clear();reuseKey.clear();copies.clear();writes.clear();}
};
struct UniformSnapshot {VkBuffer buffer{};VkDeviceMemory memory{};void* mapped{};VkDeviceSize size{};std::vector<unsigned char> staging;};
static std::map<std::pair<VkBuffer,uint32_t>,UniformSnapshot> uniformSnapshots;
static std::set<std::pair<VkBuffer,uint32_t>> usedUniformSnapshots;
static VkDescriptorPool uniformPool{};
static uint64_t uniformCopiesSkipped{},uniformUpdatesSkipped{};
static uint32_t uniformCloneCount{};
static std::map<DescriptorReuseKey,VkDescriptorSet> uniformDescriptorCache;
static uint32_t uniformCloneHits{};
static bool reuseUniformDescriptors{};
static uint32_t reusedLightTableBindings{};
static uint32_t readOnlyStorageBindings{};
static bool compareStaticInputs{},reuseLeftStaticInputs{};
using UniformMemoKey=std::array<uintptr_t,8>;
struct UniformMemoValue {VkDescriptorSet set{};std::vector<uint32_t> storageBindings;};
static BindingMemo<UniformMemoKey,UniformMemoValue> uniformBindMemo;
using SnapshotClassKey=std::array<uintptr_t,3>;
static BindingMemo<SnapshotClassKey,std::optional<uint64_t>,512> snapshotClassMemo;
static uint32_t reusedStaticBindings{},unmatchedStaticBindings{};
static std::array<std::set<VkCommandBuffer>,2> pendingSnapshotConsumers;
static bool gpuInputSnapshotTest(){static const bool enabled=kharvox::rendererDefaults::gpuInputCopies;return enabled;}
static bool uniformSnapshotTest(){static const bool enabled=!kharvox::runtimeFileExists(L"native_test_shared_uniform_inputs");return enabled&&stereoPreparationEnabled();}
static bool reuseLightTableTest(){static const bool enabled=kharvox::runtimeFileExists(L"native_test_reuse_light_tables");return enabled;}
static bool snapshotBatchTest(){static const bool enabled=kharvox::rendererDefaults::snapshotBatch;return enabled;}
static uint64_t snapshotBatchHits{},snapshotBatchMisses{},snapshotBatchPublished{};
#include "NativeDynamicAudit.inc"
void beginUniformSnapshots(){if(uniformSnapshotTest()){
 snapshotBatchCache.clear();snapshotBatchHits=snapshotBatchMisses=snapshotBatchPublished=0;
 uniformBindMemo.clear(); // No result crosses a frame, even if handles repeat.
 snapshotClassMemo.clear();
 beginSharedShadows();
 beginDynamicAudit();
 reuseUniformDescriptors=kharvox::rendererDefaults::descriptorReuse;
 if(!uniformDescriptorCache.empty())fail("native descriptor cache survived frame retirement");
 static const bool compare=kharvox::runtimeFileExists(L"native_compare_static_inputs");compareStaticInputs=compare;
 if(compare){const bool reuse=kharvox::runtimeFileExists(L"native_test_left_static_inputs");if(reuse!=reuseLeftStaticInputs){reuseLeftStaticInputs=reuse;kharvoxnative::log::warn(std::format("Native static-input comparison frame={} leftInputsForBoth={} (diagnostic only; per-eye dynamic camera inputs unchanged)",renderingPose.serial,reuse));}}
 reusedStaticBindings=unmatchedStaticBindings=0;
 usedUniformSnapshots.clear();
 {std::scoped_lock lock(replayMutex);for(auto&[cb,sets]:boundStorageSnapshots)for(auto&[set,bindings]:sets)bindings.clear();}
 for(auto& pending:pendingSnapshotConsumers)pending.clear();
}}
static UniformSnapshot& uniformSnapshot(VkBuffer source,uint32_t eye,VkDeviceSize size){
 auto& snapshot=uniformSnapshots[{source,eye}];
 if(snapshot.buffer){if(snapshot.size!=size)fail("uniform snapshot buffer identity changed");return snapshot;}
 auto& v=kharvoxnative::vulkan_state();
 auto requirements=load_device_proc<PFN_vkGetBufferMemoryRequirements>(v.device,"vkGetBufferMemoryRequirements");
 auto allocate=load_device_proc<PFN_vkAllocateMemory>(v.device,"vkAllocateMemory");
 auto bind=load_device_proc<PFN_vkBindBufferMemory>(v.device,"vkBindBufferMemory");
 auto map=load_device_proc<PFN_vkMapMemory>(v.device,"vkMapMemory");
 auto properties=reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(instanceProc("vkGetPhysicalDeviceMemoryProperties"));
 if(!requirements||!allocate||!bind||!map||!properties)fail("uniform snapshot dispatch missing");
 VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};ci.size=size;ci.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|(gpuInputSnapshotTest()?VK_BUFFER_USAGE_TRANSFER_DST_BIT:0);ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
 if(real_create_buffer(v.device,&ci,nullptr,&snapshot.buffer)!=VK_SUCCESS)fail("uniform snapshot buffer creation failed");
 VkMemoryRequirements req{};requirements(v.device,snapshot.buffer,&req);VkPhysicalDeviceMemoryProperties props{};properties(v.physical_device,&props);
 // GPU snapshots should also be read locally by the shaders. The policy
 // preserves the CPU path's old choice and a coherent fallback elsewhere.
 const uint32_t type=snapshotMemoryType(props,req.memoryTypeBits,gpuInputSnapshotTest());
 if(type==UINT32_MAX)fail("uniform snapshot coherent memory unavailable");
 VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=type;
 if(allocate(v.device,&ai,nullptr,&snapshot.memory)!=VK_SUCCESS||bind(v.device,snapshot.buffer,snapshot.memory,0)!=VK_SUCCESS||map(v.device,snapshot.memory,0,VK_WHOLE_SIZE,0,&snapshot.mapped)!=VK_SUCCESS)fail("uniform snapshot allocation failed");
 snapshot.size=size;snapshot.staging.resize(size_t(size));
 if(cpu::detailedEnabled()){
  MEMORY_BASIC_INFORMATION sourceInfo{},destinationInfo{};auto data=stageMappedBytes(source,0,size_t(size));
  if(data)VirtualQuery(data,&sourceInfo,sizeof(sourceInfo));VirtualQuery(snapshot.mapped,&destinationInfo,sizeof(destinationInfo));
  kharvoxnative::log::info(std::format("Native mapped snapshot source={} eye={} bytes={} sourceProtect=0x{:x} destinationProtect=0x{:x} destinationMemoryFlags=0x{:x} streamingCopy={}",reinterpret_cast<void*>(source),eye,size,sourceInfo.Protect,destinationInfo.Protect,props.memoryTypes[type].propertyFlags,streamingMappedCopyAvailable()));
 }
 return snapshot;
}
#include "NativeGpuInputCopies.inc"
static void dumpNativeFrozenInputs(const std::string& prefix){
 if(!kharvox::runtimeFileExists(L"capture_native_shadow_inputs"))return;
 // These are the immutable shader inputs, not the game's source mapping
 // after the second eye has rewritten it. The caller has verified whole
 // device completion; GPU snapshots also include a HOST_READ dependency.
 if(inputCopyPending||!outstanding)fail("frozen input capture before completion");
 std::ofstream index(kharvox::logPathA((prefix+"frozen_inputs.tsv").c_str()));
 index<<"frame\teye\tsource\tsnapshot\tbytes\tfile\n";
 size_t bytes{},records{},omitted{};constexpr size_t budget=2*1024*1024;
 for(auto key:usedUniformSnapshots){
  auto& snapshot=uniformSnapshots.at(key);
  // Sizes identify candidates seen in previous shader captures, not proof
  // that every allocation of this size contains a shadow or light table.
  if(snapshot.size!=65520&&snapshot.size!=65536&&snapshot.size!=49152)continue;
  if(snapshot.size>budget-bytes){++omitted;continue;}
  if(!snapshot.mapped)fail("frozen input capture mapping missing");
  std::vector<unsigned char> data(size_t(snapshot.size));
  copyMappedInput(data.data(),snapshot.mapped,data.size());
  auto name=prefix+std::format("frozen_input_{:03}_eye{}.raw",records,key.second);
  std::ofstream file(kharvox::logPathA(name.c_str()),std::ios::binary);
  file.write(reinterpret_cast<const char*>(data.data()),data.size());
  index<<renderingPose.serial<<'\t'<<key.second<<'\t'<<reinterpret_cast<uintptr_t>(key.first)<<'\t'<<reinterpret_cast<uintptr_t>(snapshot.buffer)<<'\t'<<snapshot.size<<'\t'<<name<<'\n';
  if(!file||!index)fail("frozen input capture write failed");
  bytes+=data.size();++records;
 }
 index<<"# omittedByBudget="<<omitted<<" bytes="<<bytes<<" completionVerified=true\n";
 kharvoxnative::log::info(std::format("Native frozen input capture frame={} records={} bytes={} omitted={} completionVerified=true (diagnostic; exclude capture from timings)",renderingPose.serial,records,bytes,omitted));
}
static void validateSharedShadowInputs(uint32_t eye){
 static const bool probe=kharvox::runtimeFileExists(L"native_probe_shared_shadow_inputs");
 static bool probeCaptured{};
 static uint32_t probeChecks{};
 if((!sharedShadowInputs&&(!probe||probeCaptured||probeChecks>=120))||eye!=1)return;
 if(!sharedShadowInputs)++probeChecks;
 if(sharedShadowTableBindings&&!sharedShadowAtlasBindings)fail("shared shadow tables have no paired atlas binding");
 if(sharedShadowAtlasBindings&&!sharedShadowTableBindings)fail("shared shadow atlas has no paired table binding");
 if(inputCopyPending)fail("shared shadow verification before input completion");
 if(sharedLightingInputs){
  // The full comparison consumes the left lists AND tables AND atlas. Right
  // ordering is allowed to differ because none of those right lists is mixed
  // into this bundle. Same-frame ownership was checked at each binding; shader
  // NonWritable is rechecked at every draw. Keep the narrow comparison's
  // matrix/table equality guard unchanged below.
  if(sharedShadowAtlasBindings&&(!sharedLightingUniformBindings||!sharedLightingStorageBindings))fail("complete lighting comparison lacks uniform or cluster-list coverage");
  static uint32_t completeFrames{};
  if(++completeFrames<=5||completeFrames%120==0)kharvoxnative::log::info(std::format("Native complete static lighting VERIFIED frame={} uniforms={} storage={} shadowTables={} atlasBindings={} sameFrameLeftInputs=true separateEyeCameras=true diagnosticOnly=true",renderingPose.serial,sharedLightingUniformBindings,sharedLightingStorageBindings,sharedShadowTableBindings,sharedShadowAtlasBindings));
  return;
 }
 for(auto key:usedUniformSnapshots){
  if(key.second!=1)continue;
  auto& right=uniformSnapshots.at(key);if(right.size!=65520&&right.size!=65536&&right.size!=49152)continue;
  if(!usedUniformSnapshots.contains({key.first,0})){if(sharedShadowInputs)fail("shared shadow input has no same-frame left counterpart");continue;}
  auto& left=uniformSnapshots.at({key.first,0});if(left.size!=right.size)fail("shared shadow input sizes differ");
  copyMappedInput(left.staging.data(),left.mapped,size_t(left.size));
  copyMappedInput(right.staging.data(),right.mapped,size_t(right.size));
  const bool match=right.size==65520?sharedShadowTableCompatible(left.staging,right.staging):left.staging==right.staging;
  if(!match){
   const auto offset=size_t(std::mismatch(left.staging.begin(),left.staging.end(),right.staging.begin()).first-left.staging.begin());
   const auto detail=std::format("frame={} source={} bytes={} firstDifferentByte={} sharedInputsActive={}",renderingPose.serial,reinterpret_cast<uintptr_t>(key.first),right.size,offset,sharedShadowInputs);
   if(!probeCaptured){
    for(uint32_t side=0;side<2;++side){const auto& bytes=side?right.staging:left.staging;std::ofstream file(kharvox::logPathA(side?"native_shadow_guard_right.raw":"native_shadow_guard_left.raw"),std::ios::binary);file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());}
    std::ofstream(kharvox::logPathA("native_shadow_guard.txt"))<<detail<<"\nInput copy complete; captured before shader consumers. Comparison rejects different world matrices or 64/48KiB input tables.\n";
    probeCaptured=true;kharvoxnative::log::warn("Native shadow input compatibility probe: "+detail);
   }
   if(sharedShadowInputs)fail(("shared shadow inputs incompatible: "+detail+"; restart AER").c_str());
   return; // passive probe has changed no descriptors or renderer output
  }
 }
 static uint32_t frames{};if(++frames<=5||frames%120==0)kharvoxnative::log::info(std::format("Native shared shadow inputs VERIFIED frame={} enabled={} tableBindings={} atlasBindings={} views={} matricesAndLightTablesMatch=true sameFrameLeftSnapshot=true sourceLayoutsVerified=true",renderingPose.serial,sharedShadowInputs,sharedShadowTableBindings,sharedShadowAtlasBindings,sharedShadowSourceViews.size()));
}
void finishUniformSnapshots(uint32_t eye){
 cpu::Scope profile(cpu::SnapshotCopy);
 finishDynamicAudit(eye);
 if(!uniformSnapshotTest())return;
 if(gpuInputSnapshotTest()){finishGpuInputSnapshots(eye);validateSharedShadowInputs(eye);return;}
 std::scoped_lock sl(swapchains_mutex);
 for(auto key:usedUniformSnapshots)if(key.second==eye){auto& snapshot=uniformSnapshots.at(key);auto data=stageMappedBytes(key.first,0,size_t(snapshot.size));if(!data)fail("uniform snapshot source mapping disappeared");
  // Do not interleave WC source reads and WC destination writes: that drains
  // streaming-load buffers on the tested hardware. Read into persistent cached
  // RAM first, then upload sequentially to the unchanged coherent allocation.
  copyMappedInput(snapshot.staging.data(),data,size_t(snapshot.size));
  std::memcpy(snapshot.mapped,snapshot.staging.data(),size_t(snapshot.size));_mm_sfence();
  if(cpu::current.active)cpu::current.copiedBytes+=snapshot.size;
 }
 validateSharedShadowInputs(eye);
}
void retireUniformSnapshots(){
 snapshotBatchCache.clear(); // No resolved clone handle survives pool retirement.
 uniformBindMemo.clear(); // Drop CPU references before the descriptor pool reset.
 if(!uniformSnapshotTest()||!uniformPool)return;
 const bool resetPool=true;
 if(resetPool){auto reset=load_device_proc<PFN_vkResetDescriptorPool>(kharvoxnative::vulkan_state().device,"vkResetDescriptorPool");
  if(!reset||reset(kharvoxnative::vulkan_state().device,uniformPool,0)!=VK_SUCCESS)fail("uniform snapshot descriptor retirement failed");}

 uniformCopiesSkipped=uniformUpdatesSkipped=0;
 uniformDescriptorCache.clear();
 static uint32_t frames{};if(++frames<=4||frames%300==0)kharvoxnative::log::info(std::format("Native immutable frame inputs: frame={} buffers={} clones={} reusedLeftLightTableBindings={} readOnlyStorageBindings={} retiredAfterDeviceIdle=true",renderingPose.serial,usedUniformSnapshots.size(),uniformCloneCount,reusedLightTableBindings,readOnlyStorageBindings));
 if(compareStaticInputs&&(frames<=4||frames%60==0))kharvoxnative::log::info(std::format("Native static-input comparison coverage frame={} leftInputsForBoth={} reusedBindings={} unmatchedBindings={} (diagnostic, not a production fix)",renderingPose.serial,reuseLeftStaticInputs,reusedStaticBindings,unmatchedStaticBindings));
 uniformCloneCount=0;
 if(frames<=4||frames%60==0)kharvoxnative::log::info(std::format("Native descriptor reuse frame={} enabled={} hits={} retiredAfterDeviceIdle=true",renderingPose.serial,reuseUniformDescriptors,uniformCloneHits));
 uniformCloneHits=0;
 reusedLightTableBindings=0;
 readOnlyStorageBindings=0;
}
void forgetUniformSnapshots(VkBuffer source){
 if(!uniformSnapshotTest())return;
 auto&v=kharvoxnative::vulkan_state();
 for(uint32_t eye=0;eye<2;++eye){auto it=uniformSnapshots.find({source,eye});if(it==uniformSnapshots.end())continue;if(outstanding)fail("uniform snapshot source retired during recording");
  auto unmap=load_device_proc<PFN_vkUnmapMemory>(v.device,"vkUnmapMemory");auto free=load_device_proc<PFN_vkFreeMemory>(v.device,"vkFreeMemory");if(!unmap||!free)fail("uniform snapshot retirement dispatch missing");
  unmap(v.device,it->second.memory);real_destroy_buffer(v.device,it->second.buffer,nullptr);free(v.device,it->second.memory,nullptr);uniformSnapshots.erase(it);
 }
}
static void forwardNativeDescriptorBind(VkCommandBuffer cb,VkPipelineBindPoint point,VkPipelineLayout layout,uint32_t first,uint32_t count,const VkDescriptorSet* sets,uint32_t dynamicCount,const uint32_t* dynamic){
 cpu::Scope profile(cpu::SnapshotForward);real_cmd_bind_descriptor_sets(cb,point,layout,first,count,sets,dynamicCount,dynamic);
}
static void forwardSnapshotBatch(VkCommandBuffer cb,VkPipelineBindPoint point,VkPipelineLayout layout,uint32_t first,uint32_t count,const VkDescriptorSet* effective,const VkDescriptorSet* result,uint32_t dynamicCount,const uint32_t* dynamic,uint32_t eye){
 if(!std::equal(result,result+count,effective)){
  if(GetCurrentThreadId()!=renderThread)fail("uniform snapshot recording escaped render thread");
  std::scoped_lock sl(submissionMutex);if(submittedCommands.count(cb))fail("uniform snapshot bound after submission");recordedCommands.insert(cb);pendingSnapshotConsumers[eye].insert(cb);
 }
 forwardNativeDescriptorBind(cb,point,layout,first,count,result,dynamicCount,dynamic);
}
static void bindNativeDescriptorsFull(VkCommandBuffer cb,VkPipelineBindPoint point,VkPipelineLayout layout,uint32_t first,uint32_t count,const VkDescriptorSet* source,const VkDescriptorSet* effective,uint32_t dynamicCount,const uint32_t* dynamic,VkDescriptorSet* resolved){
 if(!uniformSnapshotTest()||!outstanding||(!inside_real_frame_root&&!inside_frame_root_double)){
  if(point==VK_PIPELINE_BIND_POINT_GRAPHICS)clearStorageSnapshotBindings(cb,first,count);
  forwardNativeDescriptorBind(cb,point,layout,first,count,effective,dynamicCount,dynamic);return;
 }
 const uint32_t eye=inside_frame_root_double?1:0;auto&v=kharvoxnative::vulkan_state();
 // Most game binds contain one set. Keep that temporary list on the stack,
 // with an unrestricted overflow path for larger valid binding groups.
 std::array<VkDescriptorSet,8> inlineResult;std::vector<VkDescriptorSet> overflowResult;
 if(count>inlineResult.size())overflowResult.resize(count);
 std::span<VkDescriptorSet> result(count>inlineResult.size()?overflowResult.data():inlineResult.data(),count);
 std::copy_n(effective,count,result.begin());
 observeDynamicAudit(count,source,dynamicCount,dynamic);
 // Device dispatch was resolved atomically before native hook activation.
 // Resolving it again on every draw-set bind needlessly enters the loader.
 auto update=real_update_descriptor_sets;auto alloc=real_allocate_descriptor_sets;
 if(!update||!alloc)fail("uniform snapshot descriptor dispatch missing");
 BindScratchLease<UniformBindScratch> scratchLease;
 auto& scratch=scratchLease.get();
 static const bool memoRequested=!kharvox::runtimeFileExists(L"native_disable_bind_memo");
 static const bool legacyMemo=kharvox::runtimeFileExists(L"native_legacy_input_memo");
 if(cpu::current.active)cpu::current.snapshotMemoLegacy=legacyMemo;
 // Causal shared-input modes have extra binding-time side effects; retain
 // their full path. Ordinary per-eye snapshots can reuse validated results.
 const bool memoEnabled=memoRequested&&reuseUniformDescriptors&&!compareStaticInputs&&
  !reuseLeftStaticInputs&&!sharedShadowInputs&&!sharedLightingInputs&&!reuseLightTableTest();
 for(uint32_t i=0;i<count;++i){
  // Equivalent NonWritable declarations yield identical replacement lists.
  // Keep all other r143 key fields and the full mutation stamp. Unobserved or
  // invalid pipelines cannot hit the memo and retain the existing refusals.
  const auto pipeline=replayStates[cb].pipeline;
  auto revision=memoEnabled?bindingMutationClock.stamp():std::optional<uint64_t>{};
  std::optional<uint64_t> accessClass;
  if(memoEnabled&&!legacyMemo){
   cpu::Scope classProfile(cpu::SnapshotClassLookup);
   const SnapshotClassKey classKey{reinterpret_cast<uintptr_t>(pipeline),first+i,uintptr_t(point)};bool direct{};
   if(const auto hit=snapshotClassMemo.findFast(classKey,revision,bindingMemoBucket(classKey),direct)){
    accessClass=*hit;if(cpu::current.active)++cpu::current.snapshotClassHits;
   }else{
    accessClass=snapshotClass(pipeline,first+i,point);
    snapshotClassMemo.put(classKey,accessClass,revision,bindingMutationClock.stamp());
    if(cpu::current.active)++cpu::current.snapshotClassMisses;
   }
  }
  if(!legacyMemo&&(!accessClass||revision!=bindingMutationClock.stamp()))revision.reset();
  const UniformMemoKey memoKey{reinterpret_cast<uintptr_t>(source[i]),reinterpret_cast<uintptr_t>(effective[i]),
   legacyMemo?reinterpret_cast<uintptr_t>(pipeline):uintptr_t(accessClass.value_or(0)),reinterpret_cast<uintptr_t>(layout),first+i,uintptr_t(point),eye,mirror_generation.load(std::memory_order_relaxed)};
  const UniformMemoValue* hit{};bool direct{};
  {cpu::Scope lookup(cpu::SnapshotMemoLookup);hit=uniformBindMemo.findFast(memoKey,revision,bindingMemoBucket(memoKey),direct);}
  if(hit){
   cpu::Scope restore(cpu::SnapshotMemoRestore);
   result[i]=hit->set;
   if(cpu::current.active&&direct)++cpu::current.snapshotDirectHits;
   if(point==VK_PIPELINE_BIND_POINT_GRAPHICS){
    const bool changed=restoreStorageBindings(boundStorageSnapshots,cb,first+i,hit->storageBindings);
    readOnlyStorageBindings+=uint32_t(hit->storageBindings.size());
    if(cpu::current.active){if(changed)++cpu::current.snapshotStorageReplaced;else ++cpu::current.snapshotStorageKept;}
   }
   if(hit->set!=effective[i]){++uniformCloneHits;if(cpu::current.active)++cpu::current.snapshotMemoCloneHits;}
   else if(cpu::current.active)++cpu::current.snapshotMemoPassThroughHits;
   if(cpu::current.active)++cpu::current.bindingMemoHits;
   continue;
  }
  cpu::Scope missProfile(cpu::SnapshotMemoMiss);
  if(point==VK_PIPELINE_BIND_POINT_GRAPHICS)clearStorageSnapshotBindings(cb,first+i,1);
  if(memoEnabled&&cpu::current.active)++cpu::current.bindingMemoMisses;
  const auto remember=[&]{
   if(!revision||(legacyMemo&&result[i]==effective[i]))return;
   UniformMemoValue value{result[i],{}};
   auto command=boundStorageSnapshots.find(cb);
   if(point==VK_PIPELINE_BIND_POINT_GRAPHICS&&command!=boundStorageSnapshots.end())if(auto set=command->second.find(first+i);set!=command->second.end())
    value.storageBindings.assign(set->second.begin(),set->second.end());
   uniformBindMemo.put(memoKey,std::move(value),revision,bindingMutationClock.stamp());
  };
  scratch.clear();
  auto& replacements=scratch.replacements;auto& bindings=scratch.bindings;
  auto& imageReplacements=scratch.imageReplacements;auto& reuseKey=scratch.reuseKey;
  {std::scoped_lock sl(swapchains_mutex);auto ds=descriptor_sets.find(source[i]);
   if(ds==descriptor_sets.end()&&!(sharedShadowInputs&&eye&&point==VK_PIPELINE_BIND_POINT_GRAPHICS)){remember();continue;}
   static const decltype(descriptor_sets)::mapped_type emptyBufferRecords;
   const auto& bufferRecords=ds==descriptor_sets.end()?emptyBufferRecords:ds->second;
   // Dynamic rings and image-only sets cannot need an immutable CPU input.
   // Reject them before allocating a key or a list node for every binding.
   if(!(sharedShadowInputs&&eye&&point==VK_PIPELINE_BIND_POINT_GRAPHICS)&&std::none_of(bufferRecords.begin(),bufferRecords.end(),[point](const auto& entry){return entry.second.type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER||(point==VK_PIPELINE_BIND_POINT_GRAPHICS&&entry.second.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER); })){remember();continue;}
   if(reuseUniformDescriptors){reuseKey.reserve(128);reuseKey={reinterpret_cast<uintptr_t>(source[i]),reinterpret_cast<uintptr_t>(effective[i])};}
   bindings.reserve(bufferRecords.size());
   if(reuseUniformDescriptors)reuseKey.push_back(bufferRecords.size());
   for(auto&[binding,d]:bufferRecords){bindings.push_back(binding);
    if(reuseUniformDescriptors)descriptorKeyBuffer(reuseKey,binding,d.type,reinterpret_cast<uintptr_t>(d.buffer),d.offset,d.range);
    if(d.type!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER&&!(d.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER&&point==VK_PIPELINE_BIND_POINT_GRAPHICS))continue;
    auto bi=buffers.find(d.buffer);
    // Dynamic UBO ring allocations have distinct offsets per eye. Freeze
    // complete CPU-written UBOs and shader-proven read-only storage inputs.
    const bool storage=d.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER&&point==VK_PIPELINE_BIND_POINT_GRAPHICS
      &&bi!=buffers.end()&&bi->second.size&&stageMappedBytes(d.buffer,0,size_t(bi->second.size))&&readOnlyStorageBinding(cb,first+i,binding);
    if(d.type!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER&&!storage)continue;
    if(bi==buffers.end()||!bi->second.size)fail("frame input buffer was not captured");
    if(!stageMappedBytes(d.buffer,0,size_t(bi->second.size)))continue;
    if(bi->second.size>(storage?4*1024*1024:65536))fail("CPU frame input exceeds validated snapshot capacity");
    if(storage){++readOnlyStorageBindings;boundStorageSnapshots[cb][first+i].insert(binding);}
    // Causal comparison only: the captured light-array blocks are 64/48 KiB.
    // Keep their left order when the right pass reads left-seeded index lists.
    // Shadow tables (65520 bytes) keep their own eye's snapshot.
    const bool pairedInput=usedUniformSnapshots.count({d.buffer,0})!=0;
    const bool compareLeft=eye&&reuseLeftStaticInputs;
    const bool sharedShadow=eye&&sharedShadowInputs&&point==VK_PIPELINE_BIND_POINT_GRAPHICS&&d.type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER&&bi->second.size==65520;
    const bool sharedLighting=eye&&sharedLightingInputs&&point==VK_PIPELINE_BIND_POINT_GRAPHICS;
    if(sharedLighting){
     if(!sharedLightingSnapshotEligible(size_t(bi->second.size),d.type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,storage))fail("complete lighting comparison encountered an unvalidated static input shape");
     if(!pairedInput)fail("complete lighting comparison has no same-frame left input");
     if(storage)++sharedLightingStorageBindings;else ++sharedLightingUniformBindings;
    }
    if(sharedShadow){
     if(!pairedInput)fail("shared shadow table has no current left snapshot");
     // Retain the right data for the post-recording compatibility check.
     // No shader consumes this verification copy when the comparison is on.
     if(!sharedLighting){uniformSnapshot(d.buffer,eye,bi->second.size);usedUniformSnapshots.insert({d.buffer,eye});}++sharedShadowTableBindings;
    }
    if(compareLeft){if(pairedInput)++reusedStaticBindings;else ++unmatchedStaticBindings;}
    const uint32_t snapshotEye=eye&&pairedInput&&(sharedLighting||sharedShadow||compareLeft||(reuseLightTableTest()&&(bi->second.size==65536||bi->second.size==49152)))?0:eye;
    if(eye!=snapshotEye)++reusedLightTableBindings;
    if(d.offset>=bi->second.size)fail("snapshot descriptor offset exceeds source buffer");
    const auto range=std::min(d.range,bi->second.size-d.offset);
    if(!range)fail("snapshot descriptor range is empty");
    auto& snapshot=uniformSnapshot(d.buffer,snapshotEye,bi->second.size);usedUniformSnapshots.insert({d.buffer,snapshotEye});replacements.push_back({binding,{snapshot.buffer,d.offset,range},d.type});
   }
   if(replacements.empty()&&!(sharedShadowInputs&&eye&&point==VK_PIPELINE_BIND_POINT_GRAPHICS)){remember();continue;}
   // Each captured map already has unique binding numbers. Only a collision
   // between buffer and image maps remains to check; no per-bind tree needed.
   auto images=descriptor_image_sets.find(source[i]);if(reuseUniformDescriptors)reuseKey.push_back(images==descriptor_image_sets.end()?0:images->second.size());if(images!=descriptor_image_sets.end())for(auto&[binding,d]:images->second){
    if(bufferRecords.contains(binding))fail("uniform snapshot descriptor type collision");bindings.push_back(binding);
    if(sharedShadowInputs&&eye&&point==VK_PIPELINE_BIND_POINT_GRAPHICS&&sharedShadowImage(d.image_view,d.layout)){
     if(d.count!=1||(d.type!=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE&&d.type!=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER))fail("shared shadow view is not one sampled image");
     imageReplacements.push_back({binding,{d.sampler,d.image_view,d.layout},d.type});++sharedShadowAtlasBindings;
    }
    if(reuseUniformDescriptors){if(d.count!=1)fail("native descriptor cache cannot represent descriptor arrays");descriptorKeyImage(reuseKey,binding,d.type,reinterpret_cast<uintptr_t>(d.image_view),reinterpret_cast<uintptr_t>(d.sampler),d.layout);}
   }
   if(replacements.empty()&&imageReplacements.empty()){remember();continue;}
  }
  VkDescriptorSetLayout setLayout{};{std::scoped_lock rl(read_redirect_mutex);auto found=descriptor_set_layouts.find(source[i]);if(found==descriptor_set_layouts.end())fail("uniform snapshot layout unavailable");setLayout=found->second;}
  validateLayout(setLayout);
  if(reuseUniformDescriptors){
   reuseKey.push_back(reinterpret_cast<uintptr_t>(setLayout));reuseKey.push_back(replacements.size());
   for(const auto& replacement:replacements)descriptorKeyBuffer(reuseKey,replacement.binding,replacement.type,reinterpret_cast<uintptr_t>(replacement.buffer.buffer),replacement.buffer.offset,replacement.buffer.range);
   reuseKey.push_back(imageReplacements.size());for(const auto& replacement:imageReplacements)descriptorKeyImage(reuseKey,replacement.binding,replacement.type,reinterpret_cast<uintptr_t>(replacement.image.imageView),reinterpret_cast<uintptr_t>(replacement.image.sampler),replacement.image.imageLayout);
   if(auto hit=uniformDescriptorCache.find(reuseKey);hit!=uniformDescriptorCache.end()){result[i]=hit->second;++uniformCloneHits;remember();continue;}
  }
  if(!uniformPool){
   auto create=load_device_proc<PFN_vkCreateDescriptorPool>(v.device,"vkCreateDescriptorPool");if(!create)fail("uniform snapshot pool dispatch missing");
   std::array<VkDescriptorPoolSize,11> sizes{};for(uint32_t t=0;t<sizes.size();++t)sizes[t]={VkDescriptorType(t),131072};
   VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pi.maxSets=16384;pi.poolSizeCount=uint32_t(sizes.size());pi.pPoolSizes=sizes.data();
   if(create(v.device,&pi,nullptr,&uniformPool)!=VK_SUCCESS)fail("uniform snapshot pool creation failed");
  }
  auto allocate=[&](VkDescriptorSetLayout layout,uint32_t count,VkDescriptorSet* sets){
   std::array<VkDescriptorSetLayout,1> layouts;std::fill_n(layouts.begin(),count,layout);
   VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=uniformPool;ai.descriptorSetCount=count;ai.pSetLayouts=layouts.data();return alloc(v.device,&ai,sets);
  };
  const auto allocated=allocate(setLayout,1,&result[i]);
  if(allocated!=VK_SUCCESS)fail("uniform snapshot descriptor capacity exceeded");++uniformCloneCount;
  auto& copies=scratch.copies;for(auto binding:bindings){
   VkCopyDescriptorSet c{VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET};c.srcSet=effective[i];c.srcBinding=binding;c.dstSet=result[i];c.dstBinding=binding;c.descriptorCount=1;copies.push_back(c);}
  update(v.device,0,nullptr,uint32_t(copies.size()),copies.data());
  auto& writes=scratch.writes;for(auto&[binding,buffer,type]:replacements){VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=result[i];w.dstBinding=binding;w.descriptorCount=1;w.descriptorType=type;w.pBufferInfo=&buffer;writes.push_back(w);}
  for(auto&[binding,image,type]:imageReplacements){VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=result[i];w.dstBinding=binding;w.descriptorCount=1;w.descriptorType=type;w.pImageInfo=&image;writes.push_back(w);}
  update(v.device,uint32_t(writes.size()),writes.data(),0,nullptr);
  // The cache owns its key; retaining scratch capacity must not alias it.
  if(reuseUniformDescriptors)uniformDescriptorCache.emplace(reuseKey,result[i]);
  remember();
 }
 if(resolved)std::copy(result.begin(),result.end(),resolved);
 forwardSnapshotBatch(cb,point,layout,first,count,effective,result.data(),dynamicCount,dynamic,eye);
}
void bindNativeDescriptors(VkCommandBuffer cb,VkPipelineBindPoint point,VkPipelineLayout layout,uint32_t first,uint32_t count,const VkDescriptorSet* source,const VkDescriptorSet* effective,uint32_t dynamicCount,const uint32_t* dynamic){

 cpu::Scope profile(cpu::SnapshotBind);
 if(!snapshotBatchTest()){bindNativeDescriptorsFull(cb,point,layout,first,count,source,effective,dynamicCount,dynamic,nullptr);return;}
 using Cache=decltype(snapshotBatchCache);
 static const bool memoAllowed=!kharvox::runtimeFileExists(L"native_disable_bind_memo")&&!kharvox::runtimeFileExists(L"native_legacy_input_memo");
 const bool eligible=count&&count<=4&&uniformSnapshotTest()&&outstanding&&point==VK_PIPELINE_BIND_POINT_GRAPHICS&&
  (inside_real_frame_root||inside_frame_root_double)&&memoAllowed&&reuseUniformDescriptors&&!compareStaticInputs&&
  !reuseLeftStaticInputs&&!sharedShadowInputs&&!sharedLightingInputs&&!reuseLightTableTest();
 const auto revision=eligible?bindingMutationClock.stamp():std::optional<uint64_t>{};
 const uint32_t eye=inside_frame_root_double?1:0;
 std::optional<Cache::Key> key;
 if(revision){const auto pipeline=replayStates[cb].pipeline;
  key=Cache::key({*revision,reinterpret_cast<uintptr_t>(pipeline),reinterpret_cast<uintptr_t>(layout),first,uintptr_t(point),eye,mirror_generation.load(std::memory_order_relaxed),renderingPose.serial},{source,count},{effective,count});
 }
 const auto attempt=snapshotBatchCache.begin(cb,key);
 Cache::Result resolved{};
 if(attempt.hit){
  ++snapshotBatchHits;resolved=*attempt.hit;
  observeDynamicAudit(count,source,dynamicCount,dynamic);
  // The last completed bind installed exactly these storage-binding masks.
  // Resets, clears and intervening binds invalidate the entry. Draw-time
  // shader access validation and all producer/submission guards still run.
  forwardSnapshotBatch(cb,point,layout,first,count,effective,resolved.data(),dynamicCount,dynamic,eye);
 }else{
  if(key)++snapshotBatchMisses;
  bindNativeDescriptorsFull(cb,point,layout,first,count,source,effective,dynamicCount,dynamic,key?resolved.data():nullptr);
 }
 if(key&&revision==bindingMutationClock.stamp()&&replayStates[cb].pipeline==reinterpret_cast<VkPipeline>(key->context[1])&&mirror_generation.load(std::memory_order_relaxed)==key->context[6])
  if(snapshotBatchCache.publish(cb,attempt.token,*key,resolved))++snapshotBatchPublished;
}
static void reportSnapshotBatches(){
 if(!snapshotBatchTest()||!cpu::current.active)return;
 static uint64_t samples{};if(++samples<=8||samples%120==0)
  kharvoxnative::log::info(std::format("Native snapshot batch frame={} pid={} hits={} misses={} published={} fullStereo={} liveDynamicOffsets=true submissionGuards=true copiesUnchanged=true",renderingPose.serial,GetCurrentProcessId(),snapshotBatchHits,snapshotBatchMisses,snapshotBatchPublished,!leftEyeOnlyDiagnostic()));
 if(trace::active())trace::row("snapshot-batch-summary",0,{snapshotBatchHits,snapshotBatchMisses,snapshotBatchPublished});
}

// Isolated performance comparison. The game stream is still being recorded.
// Copy CPU-written inputs on the owner queue and wait for this private fence
// BEFORE the next eye may rewrite their source. No Present semaphore is used.
// The existing per-eye destinations and final device-completion retirement stay.
static VkCommandPool inputCopyPool{};static VkCommandBuffer inputCopyCommand{};static VkFence inputCopyFence{};static bool inputCopyPending{};
static void releaseGpuInputCopies(VkDevice device){
 if(inputCopyPending)fail("GPU input resources destroyed before fence completion");
 if(!inputCopyPool)return;
 auto destroyPool=load_device_proc<PFN_vkDestroyCommandPool>(device,"vkDestroyCommandPool");auto destroyFence=load_device_proc<PFN_vkDestroyFence>(device,"vkDestroyFence");if(!destroyPool||!destroyFence)fail("GPU input copy retirement dispatch missing");
 destroyFence(device,inputCopyFence,nullptr);destroyPool(device,inputCopyPool,nullptr);inputCopyPool={};inputCopyCommand={};inputCopyFence={};
}
static void finishGpuInputSnapshots(uint32_t eye){
 auto& v=kharvoxnative::vulkan_state();
 if(eye>1||!outstanding||GetCurrentThreadId()!=renderThread||!v.queue||!nativeQueueLock||!nativeQueueUnlock)fail("GPU input snapshot boundary is not owned by the inline render thread");
 struct Copy {VkBuffer source{},target{};VkDeviceSize size{};UniformSnapshot* snapshot{};};
 std::vector<Copy> copies;
 {std::scoped_lock lock(swapchains_mutex);
  for(auto key:usedUniformSnapshots)if(key.second==eye){auto& snapshot=uniformSnapshots.at(key);auto source=buffers.find(key.first);
   if(source==buffers.end()||!(source->second.usage&VK_BUFFER_USAGE_TRANSFER_SRC_BIT)||source->second.size!=snapshot.size||!snapshot.size||snapshot.size%4)fail("GPU input snapshot source transfer contract failed");
   copies.push_back({key.first,snapshot.buffer,snapshot.size,&snapshot});
  }
 }
 if(copies.empty())return;
#define INPUT_PROC(api) auto api=load_device_proc<PFN_##api>(v.device,#api);if(!api)fail("GPU input snapshot dispatch missing: " #api)
 INPUT_PROC(vkCreateCommandPool);INPUT_PROC(vkAllocateCommandBuffers);INPUT_PROC(vkResetCommandPool);INPUT_PROC(vkBeginCommandBuffer);INPUT_PROC(vkEndCommandBuffer);INPUT_PROC(vkCreateFence);INPUT_PROC(vkResetFences);INPUT_PROC(vkWaitForFences);
#undef INPUT_PROC
 auto check=[](VkResult result){if(result!=VK_SUCCESS)fail("GPU input snapshot copy or completion failed");};
 auto& pool=inputCopyPool;auto& command=inputCopyCommand;auto& fence=inputCopyFence;
 if(!pool){VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pi.flags=VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;pi.queueFamilyIndex=v.queue_family;check(vkCreateCommandPool(v.device,&pi,nullptr,&pool));
  VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ai.commandPool=pool;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=1;check(vkAllocateCommandBuffers(v.device,&ai,&command));
  VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};check(vkCreateFence(v.device,&fi,nullptr,&fence));
 }
 // The previous invocation returned only after this exact fence completed.
 check(vkResetCommandPool(v.device,pool,0));check(vkResetFences(v.device,1,&fence));
 VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;check(vkBeginCommandBuffer(command,&begin));
 const auto copyTiming=beginGpuSpan(command,"input-snapshot-copy",eye,0,0,0);
 VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};before.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT|VK_ACCESS_MEMORY_WRITE_BIT;before.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
 real_cmd_pipeline_barrier(command,VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&before,0,nullptr,0,nullptr);
 uint64_t bytes{};for(const auto& copy:copies){VkBufferCopy region{0,0,copy.size};real_cmd_copy_buffer(command,copy.source,copy.target,1,&region);bytes+=copy.size;}
 VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};after.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;after.dstAccessMask=VK_ACCESS_UNIFORM_READ_BIT|VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_HOST_READ_BIT;
 real_cmd_pipeline_barrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,0,1,&after,0,nullptr,0,nullptr);noteGpuCopy(copyTiming,bytes,copies.size());endGpuSpan(copyTiming);check(vkEndCommandBuffer(command));
 const auto copyWaitBefore=cpu::current.ns[cpu::GpuInputWait];
 {
  cpu::Scope profile(cpu::GpuInputWait);
  struct QueueLock {QueueLock(){nativeQueueLock();}~QueueLock(){nativeQueueUnlock();}} queue;
  {std::scoped_lock lock(submissionMutex);for(auto cb:pendingSnapshotConsumers[eye])if(submittedCommands.count(cb))fail("immutable input consumer submitted before snapshot initialization");}
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command;
  inputCopyPending=true;{trace::SubmitScope capture("input-submit",v.queue,1,&submit,fence);const auto result=real_queue_submit(v.queue,1,&submit,fence);capture.result(result);check(result);}check(vkWaitForFences(v.device,1,&fence,VK_TRUE,5'000'000'000ull));inputCopyPending=false;
 }
 noteGpuCopyWait(copyTiming,cpu::current.ns[cpu::GpuInputWait]-copyWaitBefore);
 pendingSnapshotConsumers[eye].clear();
 if(cpu::current.active)cpu::current.copiedBytes+=bytes;
 static uint32_t completedCopies{},verifiedCopies{};
 static const bool verify=kharvox::runtimeFileExists(L"native_verify_gpu_input_copies");
 if(verify&&verifiedCopies<8){
  std::scoped_lock lock(swapchains_mutex);
  for(const auto& copy:copies){auto data=stageMappedBytes(copy.source,0,size_t(copy.size));if(!data)fail("GPU input verification source disappeared");
   std::vector<unsigned char> actual(size_t(copy.size));copyMappedInput(actual.data(),copy.snapshot->mapped,actual.size());copyMappedInput(copy.snapshot->staging.data(),data,actual.size());
   if(actual!=copy.snapshot->staging)fail("GPU input snapshot differs from CPU source bytes");
  }
  ++verifiedCopies;kharvoxnative::log::info(std::format("Native GPU input bytes VERIFIED frame={} eye={} buffers={} bytes={} (diagnostic batch; exclude this frame from performance analysis)",renderingPose.serial,eye,copies.size(),bytes));
 }
 if(++completedCopies<=8||completedCopies%600==0)kharvoxnative::log::info(std::format("Native GPU input snapshot frame={} eye={} buffers={} bytes={} privateFenceComplete=true consumersUnsubmitted=true presentWaitsUsed=0",renderingPose.serial,eye,copies.size(),bytes));
}

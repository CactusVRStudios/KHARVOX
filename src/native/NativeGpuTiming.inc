// Bounded timestamp-only diagnostic. Reuses owner completion, never waits for
// query results and never reads images. Spans may overlap: do not sum them.
static bool gpuTimingRequested(){static const bool value=kharvox::extendedDiagnosticsEnabled()&&kharvox::runtimeFileExists(L"profile_native_gpu_passes");return value||trace::enabled();}
static constexpr uint32_t gpuTimingMaxSpans=512;
struct GpuTimingSpan {VkCommandBuffer cb{};std::string name;uint32_t eye{},width{},height{};uintptr_t job{};bool ended{};uint64_t cpuStart{},cpuNs{},drawCalls{},directWorkItems{},indirectCalls{},descriptorBinds{};uintptr_t framebuffer{};bool depthOnly{};uint64_t copyBytes{},copyBuffers{},copySubmitWaitNs{};uint32_t framebufferLayers{},minAttachmentLayers{};int viewMask{-1};};
// CPU metadata observations only, bounded independently of timestamp slots.
struct GpuResourceAudit {
 uint64_t order{};uint32_t span{UINT32_MAX},eye{};VkCommandBuffer cb{};
 std::string event;uintptr_t image{},target{},view{},framebuffer{};
 uint32_t width{},height{},format{},usage{},descriptorType{};
 int load{-1},store{-1},stencilLoad{-1},stencilStore{-1};bool fullArea{},input{},preserved{};
};
static constexpr size_t gpuAuditLimit=8192;
static std::vector<GpuResourceAudit> gpuResourceAudit;
static uint64_t gpuAuditOrder{},gpuAuditDropped{};
static std::set<std::tuple<VkCommandBuffer,uint32_t,VkImageView,VkDescriptorType>> gpuDescriptorAuditSeen;
static void appendGpuAudit(GpuResourceAudit row){
 if(gpuResourceAudit.size()>=gpuAuditLimit){++gpuAuditDropped;return;}
 row.order=++gpuAuditOrder;gpuResourceAudit.push_back(std::move(row));
}
static VkQueryPool gpuTimingPool{};
static PFN_vkCmdWriteTimestamp gpuWriteTimestamp{};
static PFN_vkCmdResetQueryPool gpuResetQueries{};
static PFN_vkGetQueryPoolResults gpuQueryResults{};
static PFN_vkDestroyQueryPool gpuDestroyQueries{};
static std::vector<GpuTimingSpan> gpuSpans;
static std::map<VkCommandBuffer,uint32_t> gpuActivePasses;
static bool gpuTimingActive{},gpuTimingUnsupported{};
static uint32_t gpuTimingBits{},gpuTimingSamples{},gpuTimingSkipped{};
static uint64_t gpuTimingEligible{};
static double gpuTimingPeriod{};
static bool initializeGpuTiming(){
 if(gpuTimingPool)return true;if(gpuTimingUnsupported)return false;
 auto& v=kharvoxnative::vulkan_state();
 auto props=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(instanceProc("vkGetPhysicalDeviceProperties"));
 auto queues=reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(instanceProc("vkGetPhysicalDeviceQueueFamilyProperties"));
 auto create=load_device_proc<PFN_vkCreateQueryPool>(v.device,"vkCreateQueryPool");
 gpuWriteTimestamp=load_device_proc<PFN_vkCmdWriteTimestamp>(v.device,"vkCmdWriteTimestamp");
 gpuResetQueries=load_device_proc<PFN_vkCmdResetQueryPool>(v.device,"vkCmdResetQueryPool");
 gpuQueryResults=load_device_proc<PFN_vkGetQueryPoolResults>(v.device,"vkGetQueryPoolResults");
 gpuDestroyQueries=load_device_proc<PFN_vkDestroyQueryPool>(v.device,"vkDestroyQueryPool");
 if(!props||!queues||!create||!gpuWriteTimestamp||!gpuResetQueries||!gpuQueryResults||!gpuDestroyQueries){gpuTimingUnsupported=true;kharvoxnative::log::warn("Native GPU timing unavailable: missing dispatch");return false;}
 VkPhysicalDeviceProperties p{};props(v.physical_device,&p);uint32_t count{};queues(v.physical_device,&count,nullptr);std::vector<VkQueueFamilyProperties> q(count);queues(v.physical_device,&count,q.data());
 if(v.queue_family>=count||!q[v.queue_family].timestampValidBits||!(p.limits.timestampPeriod>0)){gpuTimingUnsupported=true;kharvoxnative::log::warn("Native GPU timing unavailable on owner queue");return false;}
 gpuTimingBits=q[v.queue_family].timestampValidBits;gpuTimingPeriod=p.limits.timestampPeriod;
 VkQueryPoolCreateInfo ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};ci.queryType=VK_QUERY_TYPE_TIMESTAMP;ci.queryCount=gpuTimingMaxSpans*2;
 if(create(v.device,&ci,nullptr,&gpuTimingPool)!=VK_SUCCESS){gpuTimingUnsupported=true;kharvoxnative::log::warn("Native GPU timing allocation refused; diagnostic disabled");return false;}
 kharvoxnative::log::info(std::format("Native GPU timing initialized bits={} periodNs={} maxSpans={} maxSamples=96 noExtraWait=true",gpuTimingBits,gpuTimingPeriod,gpuTimingMaxSpans));return true;
}
static void beginGpuTimingFrame(){
 if(!gpuTimingRequested())return;
 if(gpuTimingActive)fail("GPU timing previous frame not completed");
 gpuSpans.clear();gpuActivePasses.clear();gpuTimingSkipped=0;gpuResourceAudit.clear();gpuDescriptorAuditSeen.clear();gpuAuditOrder=gpuAuditDropped=0;
 const bool ready=phase()==Phase::Ready;if(ready)++gpuTimingEligible;
 if(trace::enabled()){const bool supported=initializeGpuTiming();gpuTimingActive=ready&&trace::active()&&supported;}
 else gpuTimingActive=gpuTimingSampleDue(ready,gpuTimingEligible,gpuTimingSamples)&&initializeGpuTiming();
}
static uint32_t beginGpuSpan(VkCommandBuffer cb,const char* name,uint32_t eye,uint32_t width=0,uint32_t height=0,uintptr_t job=0){
 if(!gpuTimingActive)return UINT32_MAX;
 if(gpuSpans.size()>=gpuTimingMaxSpans){++gpuTimingSkipped;return UINT32_MAX;}
 const uint32_t index=uint32_t(gpuSpans.size());gpuSpans.push_back({cb,name,eye,width,height,job,false});
 // Reset only this span's slots in the SAME command buffer before its writes.
 // No assumption about command buffer submission ordering is needed for reset.
 gpuResetQueries(cb,gpuTimingPool,index*2,2);
 gpuWriteTimestamp(cb,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,gpuTimingPool,index*2);
 return index;
}
static void endGpuSpan(uint32_t index){
 if(index==UINT32_MAX)return;
 if(!gpuTimingActive||index>=gpuSpans.size()||gpuSpans[index].ended)fail("GPU timing span identity mismatch");
 auto& span=gpuSpans[index];gpuWriteTimestamp(span.cb,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,gpuTimingPool,index*2+1);span.ended=true;
}
static void activateGpuPass(VkCommandBuffer cb,uint32_t index,const VkRenderPassBeginInfo* info){
 if(index==UINT32_MAX||!info)return;
 if(gpuActivePasses.count(cb))fail("GPU timing nested pass");
 auto& span=gpuSpans[index];span.framebuffer=reinterpret_cast<uintptr_t>(info->framebuffer);
 {std::scoped_lock fl(framebuffer_depth_view_mutex);auto f=framebuffer_infos.find(info->framebuffer);
  if(f!=framebuffer_infos.end()){
   span.width=f->second.width;span.height=f->second.height;span.depthOnly=!f->second.attachments.empty();


   for(auto view:f->second.attachments){auto v=image_view_infos.find(view);span.depthOnly&=v!=image_view_infos.end()&&v->second.format>=VK_FORMAT_D16_UNORM&&v->second.format<=VK_FORMAT_D32_SFLOAT_S8_UINT;}
  }
 }
 {
  std::scoped_lock fl(framebuffer_depth_view_mutex);auto f=framebuffer_infos.find(info->framebuffer);auto pass=passDefinitions.find(info->renderPass);
  if(f!=framebuffer_infos.end())for(size_t i=0;i<f->second.attachments.size();++i){
   GpuResourceAudit row;row.span=index;row.eye=span.eye;row.cb=cb;row.event="attachment";row.framebuffer=span.framebuffer;
   auto view=f->second.attachments[i];row.view=reinterpret_cast<uintptr_t>(view);
   auto vi=image_view_infos.find(view);if(vi!=image_view_infos.end()){
    row.image=reinterpret_cast<uintptr_t>(vi->second.image);row.format=uint32_t(vi->second.format);
    auto image=created_image_infos.find(vi->second.image);if(image!=created_image_infos.end()){
     row.width=image->second.extent.width;row.height=image->second.extent.height;row.usage=image->second.usage;
     row.fullArea=info->renderArea.offset.x==0&&info->renderArea.offset.y==0&&info->renderArea.extent.width==row.width&&info->renderArea.extent.height==row.height&&image->second.mips==1&&image->second.layers==1&&vi->second.range.baseMipLevel==0&&vi->second.range.baseArrayLayer==0;
    }
   }
   if(pass!=passDefinitions.end()&&pass->second.supported&&i<pass->second.attachments.size()){
    const auto& d=pass->second;row.load=int(d.attachments[i].loadOp);row.store=int(d.attachments[i].storeOp);row.stencilLoad=int(d.attachments[i].stencilLoadOp);row.stencilStore=int(d.attachments[i].stencilStoreOp);
    row.input=std::any_of(d.inputs.begin(),d.inputs.end(),[i](const auto& ref){return ref.attachment==i;});
    row.preserved=std::find(d.preserves.begin(),d.preserves.end(),uint32_t(i))!=d.preserves.end();
   }
   appendGpuAudit(std::move(row));
  }
 }
 gpuActivePasses[cb]=index;span.cpuStart=cpu::now();
}
static void beginGpuPass(VkCommandBuffer cb,const VkRenderPassBeginInfo* info){
 if(!gpuTimingActive||!outstanding||!info)return;
 const bool right=inside_frame_root_double;
 auto index=beginGpuSpan(cb,rootDone?"final-original":"render-pass",right?1:0,0,0,right?doubled_current_job_rva:real_current_job_rva);
 activateGpuPass(cb,index,info);
}
static void noteGpuCopy(uint32_t index,uint64_t bytes,uint64_t buffers){
 if(index==UINT32_MAX)return;
 if(!gpuTimingActive||index>=gpuSpans.size())fail("GPU copy timing identity mismatch");
 gpuSpans[index].copyBytes=bytes;gpuSpans[index].copyBuffers=buffers;
}
static void noteGpuCopyWait(uint32_t index,uint64_t ns){
 if(index==UINT32_MAX)return;
 if(!gpuTimingActive||index>=gpuSpans.size())fail("GPU copy wait identity mismatch");
 gpuSpans[index].copySubmitWaitNs=ns;
}
static void noteGpuImageSeed(uint32_t index,VkImage source,VkImage target,VkImageView view,uint32_t format,uint32_t usage,const char* reason){
 if(index==UINT32_MAX)return;
 auto& span=gpuSpans.at(index);GpuResourceAudit row;row.span=index;row.eye=1;row.cb=span.cb;row.event=reason;
 row.image=reinterpret_cast<uintptr_t>(source);row.target=reinterpret_cast<uintptr_t>(target);row.view=reinterpret_cast<uintptr_t>(view);
 row.width=span.width;row.height=span.height;row.format=format;row.usage=usage;appendGpuAudit(std::move(row));
}
static void noteGpuDescriptorImages(VkCommandBuffer cb,uint32_t count,const VkDescriptorSet* sets){
 if(!gpuTimingActive||(!inside_frame_root_double&&!inside_final_pass_double))return;
 uint32_t span=UINT32_MAX;auto active=gpuActivePasses.find(cb);if(active!=gpuActivePasses.end())span=active->second;
 // Do not nest the descriptor metadata and framebuffer metadata locks.
 std::vector<std::pair<VkImageView,VkDescriptorType>> views;
 {std::scoped_lock sl(swapchains_mutex);for(uint32_t i=0;i<count;++i){auto set=descriptor_image_sets.find(sets[i]);if(set==descriptor_image_sets.end())continue;
  for(const auto& [binding,d]:set->second)if(d.image_view){
   if(gpuDescriptorAuditSeen.size()>=gpuAuditLimit){++gpuAuditDropped;continue;}
   if(gpuDescriptorAuditSeen.emplace(cb,span,d.image_view,d.type).second)views.emplace_back(d.image_view,d.type);
  }
 }}
 std::scoped_lock fl(framebuffer_depth_view_mutex);
 for(const auto& [view,type]:views){GpuResourceAudit row;row.cb=cb;row.span=span;row.eye=1;row.event="descriptor-bind-potential-read";row.view=reinterpret_cast<uintptr_t>(view);row.descriptorType=uint32_t(type);
  auto vi=image_view_infos.find(view);if(vi!=image_view_infos.end()){row.image=reinterpret_cast<uintptr_t>(vi->second.image);row.format=uint32_t(vi->second.format);
   auto image=created_image_infos.find(vi->second.image);if(image!=created_image_infos.end()){row.width=image->second.extent.width;row.height=image->second.extent.height;row.usage=image->second.usage;}}
  appendGpuAudit(std::move(row));
 }
}
// Only the existing bounded GPU sample frames pay these observations. No
// indirect argument readback, driver wait or resource lifetime change.
static void notePassWork(VkCommandBuffer cb,bool descriptor,uint64_t directItems=0,bool indirect=false){
 if(!gpuTimingActive)return;
 auto found=gpuActivePasses.find(cb);if(found==gpuActivePasses.end())return;
 auto& span=gpuSpans[found->second];
 if(descriptor)++span.descriptorBinds;
 else {++span.drawCalls;span.directWorkItems+=directItems;span.indirectCalls+=indirect;

 }
}
static void endGpuPass(VkCommandBuffer cb){auto found=gpuActivePasses.find(cb);if(found!=gpuActivePasses.end()){auto& span=gpuSpans[found->second];span.cpuNs=cpu::now()-span.cpuStart;endGpuSpan(found->second);gpuActivePasses.erase(found);}}
uint32_t beginOwnerGpuTiming(VkCommandBuffer cb){return beginGpuSpan(cb,"owner-copy-and-history",2);}
void endOwnerGpuTiming(uint32_t span){endGpuSpan(span);}
static void finishGpuTiming(){
 if(!gpuTimingActive)return;
 if(!gpuActivePasses.empty())fail("GPU timing render pass never ended");
 struct Result{uint64_t tick,available;};std::vector<Result> results(gpuSpans.size()*2);
 auto& v=kharvoxnative::vulkan_state();const auto status=results.empty()?VK_SUCCESS:gpuQueryResults(v.device,gpuTimingPool,0,uint32_t(results.size()),results.size()*sizeof(Result),results.data(),sizeof(Result),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
 std::ostringstream file,timeline,work;
 work<<"# frame="<<renderingPose.serial<<" pid="<<GetCurrentProcessId()<<" columns=frame,eye,width,height,job,cpuRecordingMs,drawCalls,directVertexOrIndexInstances,indirectCalls,descriptorBindCalls,gpuSpanMs,commandBuffer,cpuStartNs,cpuEndNs,spanIndex,framebuffer,depthOnly; CPU wall spans may overlap across open command buffers, do not sum; indirect workload unknown; sampled frame\n";
 timeline<<"# pid="<<GetCurrentProcessId()<<" frame="<<renderingPose.serial<<" bits="<<gpuTimingBits<<" periodNs="<<gpuTimingPeriod<<" status="<<status<<" skipped="<<gpuTimingSkipped<<" timestampsMayOverlap=true; columns=frame,commandBuffer,label,eye,startTick,endTick,valid,spanIndex,copyBytes,copyBuffers,cpuCopySubmitWaitMs\n";
 file<<"# pid="<<GetCurrentProcessId()<<" frame="<<renderingPose.serial<<" sample="<<++gpuTimingSamples<<" status="<<status<<" skipped="<<gpuTimingSkipped<<" ownerGpuComplete=true spansMayOverlap=true columns=frame,label,eye,width,height,job,gpuSpanMs,spanIndex\n";
 uint32_t available{};
 for(size_t i=0;i<gpuSpans.size();++i){const auto& s=gpuSpans[i];auto ms=timestampMilliseconds(results[2*i].tick,results[2*i+1].tick,(status==VK_SUCCESS||status==VK_NOT_READY)&&results[2*i].available,s.ended&&results[2*i+1].available,gpuTimingBits,gpuTimingPeriod);
  file<<renderingPose.serial<<'\t'<<s.name<<'\t'<<s.eye<<'\t'<<s.width<<'\t'<<s.height<<'\t'<<s.job<<'\t';if(ms){file<<*ms;++available;}else file<<"unavailable";file<<'\t'<<i<<'\n';
  timeline<<renderingPose.serial<<'\t'<<reinterpret_cast<uintptr_t>(s.cb)<<'\t'<<s.name<<'\t'<<s.eye<<'\t'<<results[2*i].tick<<'\t'<<results[2*i+1].tick<<'\t'<<bool(ms)<<'\t'<<i<<'\t'<<s.copyBytes<<'\t'<<s.copyBuffers<<'\t'<<double(s.copySubmitWaitNs)/1e6<<'\n';}
 for(size_t i=0;i<gpuSpans.size();++i){const auto& s=gpuSpans[i];if(!s.cpuStart)continue;
  auto ms=timestampMilliseconds(results[2*i].tick,results[2*i+1].tick,(status==VK_SUCCESS||status==VK_NOT_READY)&&results[2*i].available,s.ended&&results[2*i+1].available,gpuTimingBits,gpuTimingPeriod);
  work<<renderingPose.serial<<'\t'<<s.eye<<'\t'<<s.width<<'\t'<<s.height<<'\t'<<s.job<<'\t'<<double(s.cpuNs)/1e6<<'\t'<<s.drawCalls<<'\t'<<s.directWorkItems<<'\t'<<s.indirectCalls<<'\t'<<s.descriptorBinds<<'\t';if(ms)work<<*ms;else work<<"unavailable";work<<'\t'<<reinterpret_cast<uintptr_t>(s.cb)<<'\t'<<s.cpuStart<<'\t'<<s.cpuStart+s.cpuNs<<'\t'<<i<<'\t'<<s.framebuffer<<'\t'<<s.depthOnly<<'\n';}
 std::ostringstream audit;
 audit<<"# pid="<<GetCurrentProcessId()<<" frame="<<renderingPose.serial<<" dropped="<<gpuAuditDropped<<" columns=frame,order,spanIndex,eye,commandBuffer,event,sourceImage,targetImage,view,framebuffer,width,height,format,usage,loadOp,storeOp,fullArea,inputAttachment,preserveAttachment,descriptorType,stencilLoadOp,stencilStoreOp; order=CPU observation order not GPU execution; binds are potential reads; transfers/dispatch writes not proven; fullArea alone does not prove full overwrite\n";
 for(const auto& r:gpuResourceAudit)audit<<renderingPose.serial<<'\t'<<r.order<<'\t'<<r.span<<'\t'<<r.eye<<'\t'<<reinterpret_cast<uintptr_t>(r.cb)<<'\t'<<r.event<<'\t'<<r.image<<'\t'<<r.target<<'\t'<<r.view<<'\t'<<r.framebuffer<<'\t'<<r.width<<'\t'<<r.height<<'\t'<<r.format<<'\t'<<r.usage<<'\t'<<r.load<<'\t'<<r.store<<'\t'<<r.fullArea<<'\t'<<r.input<<'\t'<<r.preserved<<'\t'<<r.descriptorType<<'\t'<<r.stencilLoad<<'\t'<<r.stencilStore<<'\n';
 kharvoxnative::log::info(std::format("Native GPU resource audit frame={} pid={} rows={} dropped={} copiesUnchanged=true",renderingPose.serial,GetCurrentProcessId(),gpuResourceAudit.size(),gpuAuditDropped));
 std::array<uint64_t,2> passes{},depthPasses{},draws{},copyBatches{},copyBytes{};
 for(const auto& span:gpuSpans)if(span.eye<2){
  if(span.cpuStart){++passes[span.eye];depthPasses[span.eye]+=span.depthOnly;draws[span.eye]+=span.drawCalls;}
  if(span.name=="input-snapshot-copy"){++copyBatches[span.eye];copyBytes[span.eye]+=span.copyBytes;}
 }
 const auto summary=std::format("Native GPU frame frame={} pid={} ready={} leftOnly={} leftPasses={} rightPasses={} leftDepthPasses={} rightDepthPasses={} leftDraws={} rightDraws={} leftInputBatches={} rightInputBatches={} leftInputBytes={} rightInputBytes={} seededBuffers={} seededBytes={} originalCpuMs={:.3f} doubledCpuMs={:.3f} inputSubmitWaitMs={:.3f} ownerCompletionWaitMs={:.3f} rightRootCalls={} finalReplayCalls={} (CPU waits are not GPU busy time; depth-only does not imply shadow; timestamp spans may overlap)",renderingPose.serial,GetCurrentProcessId(),phase()==Phase::Ready,leftEyeOnlyDiagnostic(),passes[0],passes[1],depthPasses[0],depthPasses[1],draws[0],draws[1],copyBatches[0],copyBatches[1],copyBytes[0],copyBytes[1],cpu::current.seededBuffers,cpu::current.seededBytes,double(cpu::current.ns[cpu::Original])/1e6,double(cpu::current.ns[cpu::Doubled])/1e6,double(cpu::current.ns[cpu::GpuInputWait])/1e6,double(cpu::current.ns[cpu::XrCopyCompletion])/1e6,frame_root_double_attempts.load(),final_pass_doubles.load());
 trace::writeOrDefer("native_gpu_frames.log",summary+"\n");kharvoxnative::log::info(summary);
 trace::writeOrDefer("native_gpu_passes.tsv",file.str());trace::writeOrDefer("native_gpu_timeline.tsv",timeline.str());trace::writeOrDefer("native_pass_work.tsv",work.str());trace::writeOrDefer("native_gpu_resources.tsv",audit.str());
 kharvoxnative::log::info(std::format("Native GPU timing frame={} sample={}/96 availableSpans={}/{} omitted={} noExtraSubmitOrWait=true (timestamp spans may overlap; diagnostic frame)",renderingPose.serial,gpuTimingSamples,available,gpuSpans.size(),gpuTimingSkipped));

 gpuTimingActive=false;
}
static void releaseGpuTiming(VkDevice device){if(gpuTimingActive)fail("GPU timestamp queries still outstanding");if(gpuTimingPool){gpuDestroyQueries(device,gpuTimingPool,nullptr);gpuTimingPool=VK_NULL_HANDLE;}}

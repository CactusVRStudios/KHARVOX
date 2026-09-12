// Diagnostic only: capture small HDR tiles at actual producer boundaries.
// Every copy is outside a render pass, restores its image layout, and is read
// only after the owner's whole-device completion. No semaphore is consumed.
static constexpr VkDeviceSize stageTileBytes=64*64*4,stageCapacity=128;
static VkBuffer stageBuffer{};
static VkDeviceMemory stageMemory{};
static uint32_t stageCaptureCount{};
struct StageTile {VkImage source{},image{};uint32_t width{},height{};std::string label;};
static std::vector<StageTile> stageTiles;
struct StageUniform {uint32_t side{},binding{};VkBuffer buffer{};VkDeviceSize offset{};std::vector<unsigned char> bytes,endLeft;VkDescriptorType type{};};
static std::vector<StageUniform> stageUniforms;
static bool stageCaptureArmed{};
static bool stageCaptureOnRequest(){static const bool enabled=kharvox::runtimeFileExists(L"capture_native_stereo_stages_on_request");return enabled;}
static bool stageDiagnostic(){static const bool enabled=kharvox::runtimeFileExists(L"capture_native_stereo_stages");return (enabled||stageCaptureOnRequest())&&stageCaptureCount<120&&(!stageCaptureOnRequest()||stageCaptureArmed);}
void resetStageCapture(){
 stageTiles.clear();stageUniforms.clear();stageCaptureArmed=false;
 // Arm at a frame boundary only after the pair warm-up. In this mode no
 // stage copies run while waiting for post-load console settings or after
 // the bounded pair capture has completed, even if no HDR tiles were found.
 if(stageCaptureOnRequest()&&nativePairCaptureEligible>=119)
  stageCaptureArmed=nativeCaptureRequested(nativePairCaptureCount,kharvox::runtimeFileExists(L"capture_native_stereo_sequence"),kharvox::runtimeFileExists(L"capture_native_stereo_once"));
}
static const unsigned char* stageMappedBytes(VkBuffer buffer,VkDeviceSize offset,size_t bytes){
 auto bi=buffers.find(buffer);auto bind=buffer_bindings.find(buffer);if(bi==buffers.end()||bind==buffer_bindings.end()||offset>bi->second.size||bytes>bi->second.size-offset)return nullptr;
 auto memory=memory_mappings.find(bind->second.memory);if(memory==memory_mappings.end()||offset>UINT64_MAX-bind->second.offset)return nullptr;
 auto absolute=bind->second.offset+offset;if(absolute<memory->second.offset)return nullptr;auto relative=absolute-memory->second.offset;
 if(memory->second.size!=VK_WHOLE_SIZE&&(relative>memory->second.size||bytes>memory->second.size-relative))return nullptr;
 return memory->second.host+relative;
}
static void captureStageUniforms(VkCommandBuffer cb,uint32_t count,const VkDescriptorSet* sets,uint32_t nd,const uint32_t* dynamic){
 if(!stageDiagnostic()||phase()!=Phase::Ready||(!inside_real_frame_root&&!inside_frame_root_double)||stageUniforms.size()>=256)return;
 auto& r=replayStates[cb];if(!r.inRenderPass)return;
 {std::scoped_lock fl(framebuffer_depth_view_mutex);auto fb=framebuffer_infos.find(r.activeBegin.framebuffer);if(fb==framebuffer_infos.end()||fb->second.attachments.size()!=4)return;}
 std::scoped_lock sl(swapchains_mutex);uint32_t cursor=0;
 for(uint32_t i=0;i<count;++i){auto di=descriptor_sets.find(sets[i]);if(di==descriptor_sets.end())continue;std::map<uint32_t,DescriptorBinding> ordered(di->second.begin(),di->second.end());
  for(auto&[binding,d]:ordered){uint32_t extra=0;if(d.type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC||d.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC){if(cursor>=nd)return;extra=dynamic[cursor++];}
   const bool storage=d.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER||d.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
   if(d.type!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER&&d.type!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC&&!storage)continue;
   if(d.offset>UINT64_MAX-extra)continue;auto offset=d.offset+extra;auto bi=buffers.find(d.buffer);if(bi==buffers.end()||offset>=bi->second.size)continue;
   size_t bytes=size_t(std::min(VkDeviceSize(storage?4*1024*1024:65536),std::min(d.range,bi->second.size-offset)));const uint32_t side=inside_frame_root_double?1:0;
   if(storage){size_t capturedBytes{};for(const auto& record:stageUniforms)if(record.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER||record.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)capturedBytes+=record.bytes.size();if(capturedBytes+bytes>8*1024*1024)continue;}
   size_t sideCount=0;for(auto& record:stageUniforms)sideCount+=record.side==side;if(sideCount>=32)return;
   bool seen=false;for(auto& record:stageUniforms)if(record.side==side&&record.buffer==d.buffer&&record.offset==offset&&record.bytes.size()==bytes){seen=true;break;}if(seen)continue;
   auto data=stageMappedBytes(d.buffer,offset,bytes);if(!data)continue;if(stageUniforms.size()>=256)return;
   StageUniform record{side,binding,d.buffer,offset,{}};record.type=d.type;record.bytes.assign(data,data+bytes);stageUniforms.push_back(std::move(record));
  }
 }
}
static void captureStageLeftBoundary(){
 if(!stageDiagnostic())return;
 std::scoped_lock sl(swapchains_mutex);
 for(auto& record:stageUniforms){
  if(record.side!=0)continue;
  if(auto data=stageMappedBytes(record.buffer,record.offset,record.bytes.size()))record.endLeft.assign(data,data+record.bytes.size());
 }
}
static void ensureStageBuffer(){
 if(stageBuffer)return;
 auto& v=kharvoxnative::vulkan_state();
 auto create=load_device_proc<PFN_vkCreateBuffer>(v.device,"vkCreateBuffer");
 auto requirements=load_device_proc<PFN_vkGetBufferMemoryRequirements>(v.device,"vkGetBufferMemoryRequirements");
 auto allocate=load_device_proc<PFN_vkAllocateMemory>(v.device,"vkAllocateMemory");
 auto bind=load_device_proc<PFN_vkBindBufferMemory>(v.device,"vkBindBufferMemory");
 auto memoryProperties=reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(instanceProc("vkGetPhysicalDeviceMemoryProperties"));
 if(!create||!requirements||!allocate||!bind||!memoryProperties)fail("stage capture dispatch unavailable");
 VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};ci.size=stageTileBytes*stageCapacity;ci.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
 if(create(v.device,&ci,nullptr,&stageBuffer)!=VK_SUCCESS)fail("stage capture buffer creation failed");
 VkMemoryRequirements req{};requirements(v.device,stageBuffer,&req);
 VkPhysicalDeviceMemoryProperties props{};memoryProperties(v.physical_device,&props);uint32_t type=UINT32_MAX;
 for(uint32_t i=0;i<props.memoryTypeCount;++i)if((req.memoryTypeBits&(1u<<i))&&(props.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)){type=i;break;}
 if(type==UINT32_MAX)fail("stage capture host memory unavailable");
 VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=type;
 if(allocate(v.device,&ai,nullptr,&stageMemory)!=VK_SUCCESS||bind(v.device,stageBuffer,stageMemory,0)!=VK_SUCCESS)fail("stage capture allocation failed");
}
static void captureStageView(VkCommandBuffer cb,VkImageView view,const std::string& label){
 if(!stageDiagnostic()||phase()!=Phase::Ready||(!inside_real_frame_root&&!inside_frame_root_double))return;
 validateSeedCommand(cb);
 std::scoped_lock ml(mirror_mutex);std::scoped_lock fl(framebuffer_depth_view_mutex);
 auto vi=image_view_infos.find(view);if(vi==image_view_infos.end())return;
 auto ii=created_image_infos.find(vi->second.image);if(ii==created_image_infos.end()||ii->second.format!=VK_FORMAT_B10G11R11_UFLOAT_PACK32||ii->second.mips!=1||ii->second.layers!=1)return;
 if(stageTiles.size()>=stageCapacity)fail("stage capture capacity exceeded");
 VkImage image=vi->second.image;VkImageLayout layout=VK_IMAGE_LAYOUT_UNDEFINED;
 if(inside_frame_root_double){auto mi=mirrored_views.find(view);if(mi==mirrored_views.end())return;image=mi->second.image;layout=mirror_layout_of(image);}
 else if(auto known=sourceLayouts.whole(image))layout=*known;
 if(layout==VK_IMAGE_LAYOUT_UNDEFINED)return;
 ensureStageBuffer();
 VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=image;b.oldLayout=layout;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
 real_cmd_pipeline_barrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
 VkBufferImageCopy copy{};copy.bufferOffset=stageTileBytes*stageTiles.size();copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={std::min(64u,ii->second.extent.width),std::min(64u,ii->second.extent.height),1};copy.imageOffset={int32_t((ii->second.extent.width-copy.imageExtent.width)/2),int32_t((ii->second.extent.height-copy.imageExtent.height)/2),0};
 auto transfer=load_device_proc<PFN_vkCmdCopyImageToBuffer>(kharvoxnative::vulkan_state().device,"vkCmdCopyImageToBuffer");if(!transfer)fail("stage capture copy unavailable");transfer(cb,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,stageBuffer,1,&copy);
 std::swap(b.oldLayout,b.newLayout);b.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
 VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
 real_cmd_pipeline_barrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,1,&b);
 stageTiles.push_back({vi->second.image,image,copy.imageExtent.width,copy.imageExtent.height,std::format("{}:{}",inside_frame_root_double?"right":"left",label)});
}
static void captureStagePass(VkCommandBuffer cb){
 if(!stageDiagnostic())return;
 auto& r=replayStates[cb];std::vector<VkImageView> views;
 {std::scoped_lock fl(framebuffer_depth_view_mutex);auto fb=framebuffer_infos.find(r.activeBegin.framebuffer);if(fb!=framebuffer_infos.end())views=fb->second.attachments;}
 for(auto view:views)captureStageView(cb,view,std::format("pass:{}",reinterpret_cast<uintptr_t>(r.activeBegin.framebuffer)));
}
static void captureStageCompute(VkCommandBuffer cb){
 if(!stageDiagnostic())return;
 std::vector<VkImageView> views;
 {std::scoped_lock sl(swapchains_mutex);for(auto set:last_bound_compute_sets){auto di=descriptor_image_sets.find(set);if(di==descriptor_image_sets.end())continue;for(auto&[binding,image]:di->second)if(image.type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)views.push_back(image.image_view);}}
 for(auto view:views)captureStageView(cb,view,"compute");
}
static void VKAPI_CALL stageDispatch(VkCommandBuffer cb,uint32_t x,uint32_t y,uint32_t z){std::scoped_lock l(replayMutex);const auto timing=trace::active()&&GetCurrentThreadId()==renderThread&&(inside_real_frame_root||inside_frame_root_double)?beginGpuSpan(cb,"compute-dispatch",inside_frame_root_double?1:0,x,y,0):UINT32_MAX;hook_cmd_dispatch(cb,x,y,z);endGpuSpan(timing);captureStageCompute(cb);}
static void VKAPI_CALL stageDispatchIndirect(VkCommandBuffer cb,VkBuffer buffer,VkDeviceSize offset){std::scoped_lock l(replayMutex);const auto timing=trace::active()&&GetCurrentThreadId()==renderThread&&(inside_real_frame_root||inside_frame_root_double)?beginGpuSpan(cb,"compute-indirect",inside_frame_root_double?1:0,0,0,0):UINT32_MAX;hook_cmd_dispatch_indirect(cb,buffer,offset);endGpuSpan(timing);captureStageCompute(cb);}
void dumpStageCapture(const std::string& prefix){
 if(!stageBuffer||stageTiles.empty())return;
 {std::scoped_lock sl(swapchains_mutex);std::ofstream index(kharvox::logPathA((prefix+"uniforms.tsv").c_str()));index<<"side\tbinding\tbuffer\toffset\tbytes\tchangedAfterBind\tfile\tchangedBeforeDouble\tchangedAfterDouble\tendLeftFile\tdescriptorType\n";
  for(size_t i=0;i<stageUniforms.size();++i){auto& uniform=stageUniforms[i];auto now=stageMappedBytes(uniform.buffer,uniform.offset,uniform.bytes.size());auto name=prefix+std::format("uniform_{:03}.raw",i);std::ofstream bytes(kharvox::logPathA(name.c_str()),std::ios::binary);bytes.write(reinterpret_cast<const char*>(uniform.bytes.data()),uniform.bytes.size());
   std::string boundaryName;
   if(!uniform.endLeft.empty()){boundaryName=prefix+std::format("uniform_{:03}_end_left.raw",i);std::ofstream boundary(kharvox::logPathA(boundaryName.c_str()),std::ios::binary);boundary.write(reinterpret_cast<const char*>(uniform.endLeft.data()),uniform.endLeft.size());}
   index<<uniform.side<<'\t'<<uniform.binding<<'\t'<<reinterpret_cast<uintptr_t>(uniform.buffer)<<'\t'<<uniform.offset<<'\t'<<uniform.bytes.size()<<'\t'<<(now?(std::memcmp(now,uniform.bytes.data(),uniform.bytes.size())!=0):-1)<<'\t'<<name
    <<'\t'<<(uniform.endLeft.empty()?-1:uniform.endLeft!=uniform.bytes)<<'\t'<<((now&&!uniform.endLeft.empty())?(std::memcmp(now,uniform.endLeft.data(),uniform.endLeft.size())!=0):-1)<<'\t'<<boundaryName<<'\t'<<int(uniform.type)<<'\n';
  }
 }
 auto& v=kharvoxnative::vulkan_state();auto map=load_device_proc<PFN_vkMapMemory>(v.device,"vkMapMemory");auto unmap=load_device_proc<PFN_vkUnmapMemory>(v.device,"vkUnmapMemory");auto invalidate=load_device_proc<PFN_vkInvalidateMappedMemoryRanges>(v.device,"vkInvalidateMappedMemoryRanges");
 void* data{};if(!map||!unmap||!invalidate||map(v.device,stageMemory,0,VK_WHOLE_SIZE,0,&data)!=VK_SUCCESS)fail("stage capture map failed");
 VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};range.memory=stageMemory;range.size=VK_WHOLE_SIZE;if(invalidate(v.device,1,&range)!=VK_SUCCESS)fail("stage capture invalidate failed");
 std::ofstream index(kharvox::logPathA((prefix+"stages.tsv").c_str()));index<<"label\tsource\timage\twidth\theight\toffset\n";
 for(size_t i=0;i<stageTiles.size();++i){auto& tile=stageTiles[i];index<<tile.label<<'\t'<<reinterpret_cast<uintptr_t>(tile.source)<<'\t'<<reinterpret_cast<uintptr_t>(tile.image)<<'\t'<<tile.width<<'\t'<<tile.height<<'\t'<<i*stageTileBytes<<'\n';}
 std::ofstream bytes(kharvox::logPathA((prefix+"stages.raw").c_str()),std::ios::binary);bytes.write(static_cast<const char*>(data),stageTiles.size()*stageTileBytes);
 if(!index||!bytes)fail("stage capture write failed");unmap(v.device,stageMemory);++stageCaptureCount;
}

// Native-only, bounded source history. The existing owner submission and
// completion carry the readback; no second submit or Present semaphore wait.
namespace {
constexpr uint32_t watchWidth=128,watchHeight=72;
constexpr size_t watchPixels=watchWidth*watchHeight,watchEyeBytes=watchPixels*4;
struct WatchSample {
 StereoFrame frame{};PairWatchMetrics metrics{};
 std::array<unsigned char,watchPixels*6> rgb{};
 bool zeroIpd{},sharedStatic{},privateCompute{};
};
struct PairWatch {
 VkDevice device{};VkFormat format{};VkBuffer buffer{};VkDeviceMemory memory{};void* mapped{};
 std::array<VkImage,2> images{};std::array<VkDeviceMemory,2> imageMemory{};
 bool initialized{},pending{},manualWasPresent{};uint64_t generation{};
 WatchSample sample{};std::deque<WatchSample> history;PairWatchTrigger trigger{};
 std::string prefix,reason;
#define WATCH_FN(api) PFN_##api api{};
 WATCH_FN(vkCreateBuffer) WATCH_FN(vkDestroyBuffer) WATCH_FN(vkGetBufferMemoryRequirements)
 WATCH_FN(vkCreateImage) WATCH_FN(vkDestroyImage) WATCH_FN(vkGetImageMemoryRequirements)
 WATCH_FN(vkAllocateMemory) WATCH_FN(vkFreeMemory) WATCH_FN(vkBindBufferMemory) WATCH_FN(vkBindImageMemory)
 WATCH_FN(vkMapMemory) WATCH_FN(vkUnmapMemory) WATCH_FN(vkCmdBlitImage)
 WATCH_FN(vkCmdCopyImageToBuffer) WATCH_FN(vkCmdPipelineBarrier)
#undef WATCH_FN
} pairWatch;
void watchCheck(VkResult result){if(result!=VK_SUCCESS)fail("native pair watch allocation failed");}
void releasePairWatch(){
 auto& r=pairWatch;if(r.pending)fail("native pair watch destruction before owner completion");
 if(!r.device)return;
 if(r.mapped)r.vkUnmapMemory(r.device,r.memory);
 if(r.buffer)r.vkDestroyBuffer(r.device,r.buffer,nullptr);
 if(r.memory)r.vkFreeMemory(r.device,r.memory,nullptr);
 for(size_t e=0;e<2;++e){if(r.images[e])r.vkDestroyImage(r.device,r.images[e],nullptr);if(r.imageMemory[e])r.vkFreeMemory(r.device,r.imageMemory[e],nullptr);}
 r.buffer={};r.memory={};r.mapped=nullptr;r.images={};r.imageMemory={};r.initialized=false;r.device={};
}
void preparePairWatch(VkDevice device,VkFormat format){
 auto& r=pairWatch;if(r.device==device&&r.format==format)return;
 releasePairWatch();r.device=device;r.format=format;
#define WATCH_LOAD(api) r.api=load_device_proc<PFN_##api>(device,#api);if(!r.api)fail("native pair watch dispatch missing: " #api);
 WATCH_LOAD(vkCreateBuffer) WATCH_LOAD(vkDestroyBuffer) WATCH_LOAD(vkGetBufferMemoryRequirements)
 WATCH_LOAD(vkCreateImage) WATCH_LOAD(vkDestroyImage) WATCH_LOAD(vkGetImageMemoryRequirements)
 WATCH_LOAD(vkAllocateMemory) WATCH_LOAD(vkFreeMemory) WATCH_LOAD(vkBindBufferMemory) WATCH_LOAD(vkBindImageMemory)
 WATCH_LOAD(vkMapMemory) WATCH_LOAD(vkUnmapMemory) WATCH_LOAD(vkCmdBlitImage)
 WATCH_LOAD(vkCmdCopyImageToBuffer) WATCH_LOAD(vkCmdPipelineBarrier)
#undef WATCH_LOAD
 auto& v=kharvoxnative::vulkan_state();
 auto getProps=reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(instanceProc("vkGetPhysicalDeviceMemoryProperties"));
 auto getFormat=reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(instanceProc("vkGetPhysicalDeviceFormatProperties"));
 if(!getProps||!getFormat)fail("native pair watch physical dispatch missing");
 VkFormatProperties fp{};getFormat(v.physical_device,format,&fp);
 constexpr auto blit=VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT;
 if((fp.optimalTilingFeatures&blit)!=blit)fail("native pair watch format does not support blits");
 VkPhysicalDeviceMemoryProperties memory{};getProps(v.physical_device,&memory);
 auto allocate=[&](VkMemoryRequirements req,VkMemoryPropertyFlags required,VkMemoryPropertyFlags preferred,VkDeviceMemory& out){
  uint32_t type=UINT32_MAX;for(uint32_t i=0;i<memory.memoryTypeCount;++i)if((req.memoryTypeBits&(1u<<i))&&(memory.memoryTypes[i].propertyFlags&required)==required){type=i;if((memory.memoryTypes[i].propertyFlags&preferred)==preferred)break;}
  if(type==UINT32_MAX)fail("native pair watch memory unavailable");
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=type;watchCheck(r.vkAllocateMemory(device,&ai,nullptr,&out));
 };
 VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=watchEyeBytes*2;bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
 watchCheck(r.vkCreateBuffer(device,&bi,nullptr,&r.buffer));VkMemoryRequirements req{};r.vkGetBufferMemoryRequirements(device,r.buffer,&req);
 allocate(req,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,VK_MEMORY_PROPERTY_HOST_CACHED_BIT,r.memory);
 watchCheck(r.vkBindBufferMemory(device,r.buffer,r.memory,0));watchCheck(r.vkMapMemory(device,r.memory,0,VK_WHOLE_SIZE,0,&r.mapped));
 for(size_t e=0;e<2;++e){
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=format;ci.extent={watchWidth,watchHeight,1};ci.mipLevels=ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  watchCheck(r.vkCreateImage(device,&ci,nullptr,&r.images[e]));r.vkGetImageMemoryRequirements(device,r.images[e],&req);allocate(req,0,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,r.imageMemory[e]);watchCheck(r.vkBindImageMemory(device,r.images[e],r.imageMemory[e],0));
 }
 if(r.prefix.empty())r.prefix=std::format("native_watch_{}_{}_",GetCurrentProcessId(),GetTickCount64());
 kharvoxnative::log::info("Native pair watch active: 128x72 each source eye; 16-frame history; max32 candidate events; no extra submit/wait; not compositor output or an artifact classifier");
}
void dumpPairWatch(){
 auto& r=pairWatch;const auto prefix=r.prefix+std::format("{:02}_",r.trigger.events);
 std::ofstream meta(kharvox::logPathA((prefix+"capture.json").c_str()));
 meta<<"{\"kind\":\"native-source-pair-watch\",\"candidateOnly\":true,\"reason\":\""<<r.reason<<"\",\"ownerCompletionVerified\":true,\"layoutsRestored\":true,\"extraSubmit\":false,\"compositorOutput\":false,\"headsetValidated\":false,\"width\":128,\"height\":72,\"triggerSample\":"<<r.trigger.lastTrigger<<",\"frames\":[";
 size_t n{};for(const auto& sample:r.history){const auto& frame=sample.frame;
  if(n)meta<<',';meta<<"{\"frame\":"<<frame.pose.serial<<",\"generation\":"<<frame.generation<<",\"displayTime\":"<<frame.pose.displayTime<<",\"zeroIpd\":"<<(sample.zeroIpd?"true":"false")<<",\"sharedStatic\":"<<(sample.sharedStatic?"true":"false")<<",\"headQuaternion\":["<<frame.pose.head.orientation.x<<','<<frame.pose.head.orientation.y<<','<<frame.pose.head.orientation.z<<','<<frame.pose.head.orientation.w<<"],\"eyes\":[";
  for(size_t e=0;e<2;++e){auto name=prefix+std::format("{:02}_{}.ppm",n,e?"right":"left");std::ofstream file(kharvox::logPathA(name.c_str()),std::ios::binary);file<<"P6\n128 72\n255\n";file.write(reinterpret_cast<const char*>(sample.rgb.data()+e*watchPixels*3),watchPixels*3);if(!file)fail("native pair watch image write failed");
   const auto& eye=frame.eyes[e];const auto& q=eye.pose.orientation;const auto& p=eye.pose.position;
   if(e)meta<<',';meta<<"{\"eye\":"<<eye.eye<<",\"frame\":"<<eye.frame<<",\"image\":"<<reinterpret_cast<uintptr_t>(eye.image)<<",\"file\":\""<<name<<"\",\"sourceExtent\":["<<eye.extent.width<<','<<eye.extent.height<<"],\"format\":"<<int(eye.format)<<",\"layout\":"<<int(eye.layout)<<",\"pose\":["<<p.x<<','<<p.y<<','<<p.z<<','<<q.x<<','<<q.y<<','<<q.z<<','<<q.w<<"],\"fov\":["<<eye.fov.angleLeft<<','<<eye.fov.angleRight<<','<<eye.fov.angleUp<<','<<eye.fov.angleDown<<"],\"whiteFraction\":"<<sample.metrics.whiteFraction[e]<<'}';
  }meta<<"],\"privateCompute\":"<<(sample.privateCompute?"true":"false")<<"}";++n;
 }meta<<"]}\n";meta.close();if(!meta)fail("native pair watch metadata write failed");
 kharvoxnative::log::info(std::format("Native pair watch event={}/32 reason={} lastFrame={} frames={} ownerCompletionVerified=true candidateOnly=true (exclude disk-write frame from performance comparison)",r.trigger.events,r.reason,r.sample.frame.pose.serial,r.history.size()));
}
}
bool pairWatchEnabled(){static const bool enabled=kharvox::extendedDiagnosticsEnabled()&&kharvox::runtimeFileExists(L"capture_native_pair_watch");return installed()&&enabled;}
bool recordPairWatch(VkDevice device,VkCommandBuffer command,const StereoFrame& frame){
 if(!pairWatchEnabled()||pairWatch.trigger.events>=32)return false;
 auto& r=pairWatch;if(r.pending||device!=kharvoxnative::vulkan_state().device)fail("native pair watch owner/lifetime mismatch");
 const auto format=frame.eyes[0].format;
 if(format!=VK_FORMAT_R8G8B8A8_UNORM&&format!=VK_FORMAT_R8G8B8A8_SRGB&&format!=VK_FORMAT_B8G8R8A8_UNORM&&format!=VK_FORMAT_B8G8R8A8_SRGB)fail("native pair watch unsupported format");
 for(size_t e=0;e<2;++e)if(!frame.eyes[e].image||frame.eyes[e].format!=format||frame.eyes[e].eye!=e||frame.eyes[e].frame!=frame.pose.serial||!frame.eyes[e].extent.width||!frame.eyes[e].extent.height||frame.eyes[e].layout==VK_IMAGE_LAYOUT_UNDEFINED)fail("native pair watch invalid pair");
 preparePairWatch(device,format);
 if(r.generation!=frame.generation||(!r.history.empty()&&r.history.back().frame.pose.serial+1!=frame.pose.serial)){
  r.history.clear();r.trigger.pending=false;r.trigger.postFrames=0;r.generation=frame.generation;
 }
 r.sample={};r.sample.frame=frame;r.sample.zeroIpd=frame_root_zero_offset.load();r.sample.sharedStatic=reuseLeftStaticInputs;r.sample.privateCompute=!suppress_doubled_light_culling.load();
 auto barrier=[&](VkImage image,VkImageLayout oldLayout,VkImageLayout newLayout,VkAccessFlags src,VkAccessFlags dst){VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=image;b.oldLayout=oldLayout;b.newLayout=newLayout;b.srcAccessMask=src;b.dstAccessMask=dst;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};r.vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);};
 for(size_t e=0;e<2;++e){const auto& eye=frame.eyes[e];
  barrier(eye.image,eye.layout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT,VK_ACCESS_TRANSFER_READ_BIT);
  barrier(r.images[e],r.initialized?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,r.initialized?VK_ACCESS_TRANSFER_READ_BIT:0,VK_ACCESS_TRANSFER_WRITE_BIT);
  VkImageBlit blit{};blit.srcSubresource=blit.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};blit.srcOffsets[1]={int32_t(eye.extent.width),int32_t(eye.extent.height),1};blit.dstOffsets[1]={watchWidth,watchHeight,1};
  r.vkCmdBlitImage(command,eye.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,r.images[e],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_NEAREST);
  barrier(r.images[e],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
  VkBufferImageCopy copy{};copy.bufferOffset=e*watchEyeBytes;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={watchWidth,watchHeight,1};r.vkCmdCopyImageToBuffer(command,r.images[e],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,r.buffer,1,&copy);
  barrier(eye.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,eye.layout,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT);
 }
 VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;r.vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
 r.pending=true;r.initialized=true;return true;
}
void finishPairWatch(){
 auto& r=pairWatch;if(!r.pending)fail("native pair watch completion without recording");
 const bool bgra=r.format==VK_FORMAT_B8G8R8A8_UNORM||r.format==VK_FORMAT_B8G8R8A8_SRGB;
 std::array<unsigned char,watchEyeBytes*2> bytes{};copyMappedInput(bytes.data(),r.mapped,bytes.size());
 for(size_t e=0;e<2;++e)for(uint32_t y=0;y<watchHeight;++y)for(uint32_t x=0;x<watchWidth;++x){const size_t i=size_t(y)*watchWidth+x;const auto* p=bytes.data()+e*watchEyeBytes+i*4;auto* rgb=r.sample.rgb.data()+(e*watchPixels+i)*3;rgb[0]=p[bgra?2:0];rgb[1]=p[1];rgb[2]=p[bgra?0:2];
  const float brightness=(float(rgb[0])+rgb[1]+rgb[2])/3.0f;
  r.sample.metrics.eyeGap[(y/18)*8+x/16]+=(e?1.0f:-1.0f)*brightness/(18*16);
  if(rgb[0]>245&&rgb[1]>245&&rgb[2]>245)r.sample.metrics.whiteFraction[e]+=1.0f/watchPixels;
 }
 bool candidate=r.history.size()>=8&&pairWatchChange(r.history.back().metrics,r.sample.metrics),manual=false;
 if(r.trigger.samples%30==0){bool present=kharvox::extendedDiagnosticsEnabled()&&kharvox::runtimeFileExists(L"capture_native_watch_now");manual=present&&!r.manualWasPresent;r.manualWasPresent=present;}
 r.history.push_back(r.sample);if(r.history.size()>16)r.history.pop_front();
 const bool wasPending=r.trigger.pending;bool dump=r.trigger.advance(candidate,manual);
 if(!wasPending&&r.trigger.pending)r.reason=manual?"manual":candidate?"brightness-change-candidate":"baseline";
 r.pending=false;
 if(dump)dumpPairWatch();
}

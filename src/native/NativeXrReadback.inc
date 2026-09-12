// Diagnostic only. Record sources and final XR targets in the owner's existing
// copy command buffer. Read/free only after its proven completion, before XR
// releases either image. TRANSFER_SRC usage is requested only for this marker.
bool xrTargetCaptureEnabled(){return installed()&&[](){static const bool value=kharvox::runtimeFileExists(L"capture_native_xr_targets");return value;}();}
namespace {
struct XrReadTarget {VkImage image{};VkExtent2D extent{};VkFormat format{};VkImageLayout layout{};VkDeviceSize offset{};};
struct XrReadback {
 VkBuffer buffer{};VkDeviceMemory memory{};void* mapped{};VkDeviceSize size{};
 std::array<XrReadTarget,4> targets{};std::array<XrCaptureEye,2> eyes{};StereoFrame frame{};
 uint32_t captured{},eligible{};bool pending{};
} xrReadback;
}
bool recordXrTargetCapture(VkDevice device,VkCommandBuffer command,const StereoFrame& frame,const std::array<XrCaptureEye,2>& eyes){
 if(!xrTargetCaptureEnabled()||xrReadback.captured>=8||++xrReadback.eligible<120)return false;
 static const bool motionOnly=kharvox::runtimeFileExists(L"capture_native_xr_motion");
 static std::array<float,4> anchor{};static bool anchored{},triggered{};
 if(motionOnly&&!triggered){auto q=frame.pose.head.orientation;std::array<float,4> pose{q.x,q.y,q.z,q.w};if(!anchored){anchor=pose;anchored=true;return false;}if(!capturePoseMoved(anchor,pose))return false;triggered=true;kharvoxnative::log::info("Native XR target motion capture triggered after two degrees from anchor");}
 auto& v=kharvoxnative::vulkan_state();if(device!=v.device||xrReadback.pending)fail("XR target diagnostic device or lifetime mismatch");
 auto check=[](VkResult r){if(r!=VK_SUCCESS)fail("XR target diagnostic allocation failed");};
#define XR_READ_PROC(api) auto api=load_device_proc<PFN_##api>(device,#api);if(!api)fail("XR target diagnostic dispatch missing: " #api)
 XR_READ_PROC(vkCreateBuffer);XR_READ_PROC(vkGetBufferMemoryRequirements);XR_READ_PROC(vkAllocateMemory);XR_READ_PROC(vkBindBufferMemory);XR_READ_PROC(vkMapMemory);XR_READ_PROC(vkCmdCopyImageToBuffer);XR_READ_PROC(vkCmdPipelineBarrier);
#undef XR_READ_PROC
 auto& r=xrReadback;r.size=0;r.frame=frame;r.eyes=eyes;
 for(size_t i=0;i<4;++i){
  auto& t=r.targets[i];size_t e=i%2;
  t=i<2?XrReadTarget{frame.eyes[e].image,frame.eyes[e].extent,frame.eyes[e].format,frame.eyes[e].layout,r.size}
       :XrReadTarget{eyes[e].image,eyes[e].extent,eyes[e].format,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,r.size};
  if(!t.image||!t.extent.width||!t.extent.height||t.extent.width>16384||t.extent.height>16384)fail("XR target diagnostic invalid image extent");
  if(t.format!=VK_FORMAT_B8G8R8A8_SRGB&&t.format!=VK_FORMAT_B8G8R8A8_UNORM&&t.format!=VK_FORMAT_R8G8B8A8_SRGB&&t.format!=VK_FORMAT_R8G8B8A8_UNORM)fail("XR target diagnostic unsupported format");
  r.size+=VkDeviceSize(t.extent.width)*t.extent.height*4;
 }
 if(r.size>1024ull*1024*1024)fail("XR target diagnostic capacity exceeded");
 VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=r.size;bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;check(vkCreateBuffer(device,&bi,nullptr,&r.buffer));
 VkMemoryRequirements requirements{};vkGetBufferMemoryRequirements(device,r.buffer,&requirements);
 auto properties=reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(instanceProc("vkGetPhysicalDeviceMemoryProperties"));if(!properties)fail("XR target diagnostic memory dispatch missing");
 VkPhysicalDeviceMemoryProperties memory{};properties(v.physical_device,&memory);uint32_t type=UINT32_MAX;
 constexpr auto coherent=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
 for(uint32_t i=0;i<memory.memoryTypeCount;++i)if((requirements.memoryTypeBits&(1u<<i))&&(memory.memoryTypes[i].propertyFlags&coherent)==coherent){type=i;if(memory.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_CACHED_BIT)break;}
 if(type==UINT32_MAX)fail("XR target diagnostic coherent memory unavailable");
 VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=requirements.size;ai.memoryTypeIndex=type;check(vkAllocateMemory(device,&ai,nullptr,&r.memory));check(vkBindBufferMemory(device,r.buffer,r.memory,0));check(vkMapMemory(device,r.memory,0,VK_WHOLE_SIZE,0,&r.mapped));
 std::array<VkImageMemoryBarrier,4> barriers{};
 for(size_t i=0;i<4;++i){auto& b=barriers[i];b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;b.image=r.targets[i].image;b.oldLayout=r.targets[i].layout;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};}
 vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,4,barriers.data());
 for(auto& t:r.targets){VkBufferImageCopy copy{};copy.bufferOffset=t.offset;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={t.extent.width,t.extent.height,1};vkCmdCopyImageToBuffer(command,t.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,r.buffer,1,&copy);}
 for(auto& b:barriers){std::swap(b.oldLayout,b.newLayout);b.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;}
 VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
 vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,4,barriers.data());
 r.pending=true;return true;
}
void finishXrTargetCapture(){
 auto& r=xrReadback;if(!r.pending)fail("XR target diagnostic completed without recording");
 const auto prefix=std::format("native_xr_{:04}_",r.captured);auto path=[&](const std::string& name){return kharvox::logPathA((prefix+name).c_str());};
 std::vector<unsigned char> bytes(size_t(r.size));copyMappedInput(bytes.data(),r.mapped,bytes.size());
 const char* names[]{"source_left","source_right","xr_left","xr_right"};
 std::ofstream metadata(path("capture.json"));metadata<<"{\n\"source\":\"owner-command-after-overlay-before-xr-release\",\n\"copyCompletionVerified\":true,\n\"compositorOutput\":false,\n\"frame\":"<<r.frame.pose.serial<<",\n\"generation\":"<<r.frame.generation<<",\n\"targets\":[";
 for(size_t i=0;i<4;++i){const auto& t=r.targets[i];const bool bgra=t.format==VK_FORMAT_B8G8R8A8_SRGB||t.format==VK_FORMAT_B8G8R8A8_UNORM;constexpr uint32_t stride=4;const uint32_t width=(t.extent.width+stride-1)/stride,height=(t.extent.height+stride-1)/stride;
  std::vector<unsigned char> rgb(size_t(width)*height*3);uint64_t sum{};
  for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x){auto p=bytes.data()+t.offset+(VkDeviceSize(y*stride)*t.extent.width+x*stride)*4;auto n=(size_t(y)*width+x)*3;rgb[n]=p[bgra?2:0];rgb[n+1]=p[1];rgb[n+2]=p[bgra?0:2];sum+=rgb[n]+rgb[n+1]+rgb[n+2];}
  std::ofstream file(path(std::string(names[i])+".ppm"),std::ios::binary);file<<"P6\n"<<width<<' '<<height<<"\n255\n";file.write(reinterpret_cast<const char*>(rgb.data()),rgb.size());if(!file)fail("XR target diagnostic image write failed");
  if(i)metadata<<',';metadata<<"{\"label\":\""<<names[i]<<"\",\"image\":"<<reinterpret_cast<uintptr_t>(t.image)<<",\"width\":"<<t.extent.width<<",\"height\":"<<t.extent.height<<",\"format\":"<<int(t.format)<<",\"restoredLayout\":"<<int(t.layout)<<",\"sampleStride\":4,\"meanRgb\":"<<double(sum)/rgb.size();
  if(i>=2){auto& eye=r.eyes[i-2];metadata<<",\"acquiredIndex\":"<<eye.acquiredIndex<<",\"rect\":["<<eye.rect.offset.x<<','<<eye.rect.offset.y<<','<<eye.rect.extent.width<<','<<eye.rect.extent.height<<']';}
  metadata<<'}';
 }
 metadata<<"],\n\"headQuaternion\":["<<r.frame.pose.head.orientation.x<<','<<r.frame.pose.head.orientation.y<<','<<r.frame.pose.head.orientation.z<<','<<r.frame.pose.head.orientation.w<<"],\n\"publishedCenterValid\":"<<(diagnosticPublishedValid?"true":"false")<<",\n\"publishedCenterAtRoot\":[";
 for(size_t i=0;i<12;++i){if(i)metadata<<',';metadata<<diagnosticPublishedCenter[i];}
 metadata<<"],\n\"expectedCenterValid\":"<<(diagnosticExpectedValid?"true":"false")<<",\n\"expectedCenterAtRoot\":[";
 for(size_t i=0;i<12;++i){if(i)metadata<<',';metadata<<diagnosticExpectedCenter[i];}
 metadata<<"],\n\"centerNote\":\"Published camera API permits a one-Present boundary; this is a timing diagnostic, not a render-pose guarantee\",\n\"headsetValidated\":false\n}\n";metadata.close();if(!metadata)fail("XR target diagnostic metadata write failed");
 auto& v=kharvoxnative::vulkan_state();auto unmap=load_device_proc<PFN_vkUnmapMemory>(v.device,"vkUnmapMemory");auto free=load_device_proc<PFN_vkFreeMemory>(v.device,"vkFreeMemory");if(!unmap||!free)fail("XR target diagnostic retirement dispatch missing");
 unmap(v.device,r.memory);real_destroy_buffer(v.device,r.buffer,nullptr);free(v.device,r.memory,nullptr);r.buffer={};r.memory={};r.mapped=nullptr;r.pending=false;++r.captured;
 kharvoxnative::log::info(std::format("Native XR target capture frame={} sample={}/8 ownerCopyComplete=true beforeRelease=true layoutsRestored=true noExtraSubmit=true (diagnostic timings excluded)",r.frame.pose.serial,r.captured));
}

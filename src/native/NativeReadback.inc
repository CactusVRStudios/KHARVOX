// Opt-in, bounded technical evidence. Called after owner Present and proven
// device completion, before downstream Present releases the source image.
// No extra Present semaphore consumption and no second OpenXR session.
static uint64_t nativePairCaptureEligible{};
static uint32_t nativePairCaptureCount{};
static void captureNativePair(const StereoFrame& pair){
 if(!kharvox::extendedDiagnosticsEnabled())return;
 static HandCaptureTrigger handTrigger;
 const bool handMode=kharvox::runtimeFileExists(L"capture_native_hand_depth");
 const bool handCapture=handMode&&handTrigger.update((GetAsyncKeyState(VK_F8)&0x8000)!=0,pair.pose.gameplay,GetTickCount64());
 auto& eligible=nativePairCaptureEligible;auto& captured=nativePairCaptureCount;
 const bool sequence=kharvox::runtimeFileExists(L"capture_native_stereo_sequence");
 const bool normalCapture=captured<(sequence?120u:1u)&&++eligible>=120&&(sequence||kharvox::runtimeFileExists(L"capture_native_stereo_once"));
 if(!handCapture&&!normalCapture)return;
 const auto prefix=handCapture?std::format("native_hands_{:02}_",handTrigger.count):sequence?std::format("native_sequence_{:04}_",captured):std::string("native_");
 dumpStageCapture(prefix);
 dumpNativeFrozenInputs(prefix);
 auto path=[&](const std::string& name){return kharvox::logPathA((prefix+name).c_str());};
 const auto& v=kharvoxnative::vulkan_state();const auto extent=pair.eyes[0].extent;
 const auto format=pair.eyes[0].format;
 if(format!=VK_FORMAT_B8G8R8A8_UNORM&&format!=VK_FORMAT_B8G8R8A8_SRGB&&format!=VK_FORMAT_R8G8B8A8_UNORM&&format!=VK_FORMAT_R8G8B8A8_SRGB)fail("native capture format unsupported");
#define CAPTURE_PROC(api) auto api=load_device_proc<PFN_##api>(v.device,#api);if(!api)fail("native capture dispatch unavailable: " #api)
 CAPTURE_PROC(vkCreateBuffer);CAPTURE_PROC(vkDestroyBuffer);CAPTURE_PROC(vkGetBufferMemoryRequirements);CAPTURE_PROC(vkAllocateMemory);CAPTURE_PROC(vkFreeMemory);CAPTURE_PROC(vkBindBufferMemory);CAPTURE_PROC(vkMapMemory);CAPTURE_PROC(vkUnmapMemory);CAPTURE_PROC(vkInvalidateMappedMemoryRanges);
 CAPTURE_PROC(vkCreateCommandPool);CAPTURE_PROC(vkDestroyCommandPool);CAPTURE_PROC(vkAllocateCommandBuffers);CAPTURE_PROC(vkBeginCommandBuffer);CAPTURE_PROC(vkEndCommandBuffer);CAPTURE_PROC(vkCmdPipelineBarrier);CAPTURE_PROC(vkCmdCopyImageToBuffer);
 CAPTURE_PROC(vkCreateFence);CAPTURE_PROC(vkDestroyFence);CAPTURE_PROC(vkWaitForFences);CAPTURE_PROC(vkQueueSubmit);
#undef CAPTURE_PROC
 auto check=[](VkResult result,const char* operation){if(result!=VK_SUCCESS)fail(std::format("native readback {} failed: {}",operation,int(result)).c_str());};
 VkDeviceSize eyeBytes=VkDeviceSize(extent.width)*extent.height*4;
 struct Target{VkImage image;VkExtent2D extent;VkFormat format;VkImageLayout layout;VkDeviceSize offset,bytes;std::string label;VkOffset3D imageOffset{};VkExtent2D sourceExtent{};};
 std::vector<Target> targets;VkDeviceSize totalBytes=eyeBytes*2;
 for(int e=0;e<2;++e)targets.push_back({pair.eyes[e].image,extent,format,pair.eyes[e].layout,eyeBytes*e,eyeBytes,e?"right":"left"});
 hands::HandSceneTarget handTarget{};
 if(handCapture)hands::handSceneTargetForColor(pair.eyes[0].image,extent,handTarget);
 {std::scoped_lock ml(mirror_mutex);std::scoped_lock fl(framebuffer_depth_view_mutex);std::set<VkImage> seen;
  for(const auto&[view,mv]:mirrored_views){
   auto vi=image_view_infos.find(view);if(vi==image_view_infos.end()||!seen.insert(vi->second.image).second)continue;
   auto ii=created_image_infos.find(vi->second.image);if(ii==created_image_infos.end()||ii->second.mips!=1||ii->second.layers!=1)continue;
   const bool handDepth=handCapture&&vi->second.image==handTarget.depthImage&&mv.format==VK_FORMAT_D24_UNORM_S8_UINT;
   if(handCapture&&!handDepth)continue;
   // created_image_infos retains requested usage. Native's create hook adds
   // transfer usage when parity is enabled and fails if that creation fails.
   if(handDepth&&!renderdoc_parity_usage_enabled()&&!(ii->second.usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT)){
    kharvoxnative::log::warn("Native hand capture: game depth lacks TRANSFER_SRC usage; depth capture omitted");continue;
   }
   uint32_t bpp{};switch(mv.format){case VK_FORMAT_D24_UNORM_S8_UINT:if(!handDepth)continue;bpp=4;break;case VK_FORMAT_R16_SFLOAT:bpp=2;break;case VK_FORMAT_R16G16_SFLOAT:bpp=4;break;case VK_FORMAT_R16G16B16A16_SFLOAT:bpp=8;break;case VK_FORMAT_R8G8B8A8_UNORM:case VK_FORMAT_R32_SFLOAT:case VK_FORMAT_B10G11R11_UFLOAT_PACK32:bpp=4;break;case VK_FORMAT_R32G32_SFLOAT:bpp=8;break;case VK_FORMAT_R32G32B32A32_SFLOAT:bpp=16;break;default:continue;}
   auto sourceLayout=doom_image_layouts.find(vi->second.image);const auto mirrorLayout=mirror_layout_of(mv.image);
   if(sourceLayout==doom_image_layouts.end()||sourceLayout->second==VK_IMAGE_LAYOUT_UNDEFINED||mirrorLayout==VK_IMAGE_LAYOUT_UNDEFINED)continue;
   const bool crop=!handDepth&&(mv.extent.width>128||mv.extent.height>128);
   VkExtent2D sample=crop?VkExtent2D{std::min(64u,mv.extent.width),std::min(64u,mv.extent.height)}:mv.extent;
   VkOffset3D offset{int32_t((mv.extent.width-sample.width)/2),int32_t((mv.extent.height-sample.height)/2),0};
   const auto size=VkDeviceSize(sample.width)*sample.height*bpp;
   const auto label=handDepth?std::string("scene_depth"):std::to_string(reinterpret_cast<uintptr_t>(vi->second.image));
   totalBytes=(totalBytes+15)&~VkDeviceSize(15);targets.push_back({vi->second.image,sample,mv.format,sourceLayout->second,totalBytes,size,label+"_real",offset,mv.extent});totalBytes+=size;
   totalBytes=(totalBytes+15)&~VkDeviceSize(15);targets.push_back({mv.image,sample,mv.format,mirrorLayout,totalBytes,size,label+"_mirror",offset,mv.extent});totalBytes+=size;
  }
 }
 VkBuffer buffer{};VkDeviceMemory memory{};VkCommandPool pool{};VkFence fence{};
 VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=totalBytes;bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;check(vkCreateBuffer(v.device,&bi,nullptr,&buffer),"create buffer");
 VkMemoryRequirements req{};vkGetBufferMemoryRequirements(v.device,buffer,&req);
 VkPhysicalDeviceMemoryProperties props{};auto getMemory=reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(instanceProc("vkGetPhysicalDeviceMemoryProperties"));if(!getMemory)fail("native readback physical dispatch unavailable");getMemory(v.physical_device,&props);
 uint32_t type=UINT32_MAX;for(uint32_t i=0;i<props.memoryTypeCount;++i)if((req.memoryTypeBits&(1u<<i))&&(props.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)){type=i;if(props.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)break;}
 if(type==UINT32_MAX)fail("native readback host memory unavailable");
 VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=type;check(vkAllocateMemory(v.device,&ai,nullptr,&memory),"allocate memory");check(vkBindBufferMemory(v.device,buffer,memory,0),"bind memory");
 VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pi.flags=VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;pi.queueFamilyIndex=v.queue_family;check(vkCreateCommandPool(v.device,&pi,nullptr,&pool),"create pool");
 VkCommandBuffer cb{};VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;check(vkAllocateCommandBuffers(v.device,&ca,&cb),"allocate command buffer");
 VkCommandBufferBeginInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};ci.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;check(vkBeginCommandBuffer(cb,&ci),"begin command buffer");
 std::vector<VkImageMemoryBarrier> transitions(targets.size());
 for(size_t e=0;e<targets.size();++e){auto& b=transitions[e];b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;b.image=targets[e].image;b.oldLayout=targets[e].layout;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={targets[e].format==VK_FORMAT_D24_UNORM_S8_UINT?VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT):VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),0,1,0,1};}
 vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,uint32_t(transitions.size()),transitions.data());
 for(const auto& target:targets){VkBufferImageCopy copy{};copy.bufferOffset=target.offset;copy.imageSubresource={target.format==VK_FORMAT_D24_UNORM_S8_UINT?VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT):VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),0,0,1};copy.imageOffset=target.imageOffset;copy.imageExtent={target.extent.width,target.extent.height,1};vkCmdCopyImageToBuffer(cb,target.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&copy);}
 for(auto& b:transitions){std::swap(b.oldLayout,b.newLayout);b.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;}
 VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
 vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,uint32_t(transitions.size()),transitions.data());check(vkEndCommandBuffer(cb),"end command buffer");
 VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};check(vkCreateFence(v.device,&fi,nullptr,&fence),"create fence");VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&cb;check(vkQueueSubmit(v.queue,1,&submit,fence),"submit");check(vkWaitForFences(v.device,1,&fence,VK_TRUE,5'000'000'000ull),"wait capture fence");
 void* mapped{};check(vkMapMemory(v.device,memory,0,VK_WHOLE_SIZE,0,&mapped),"map memory");if(!(props.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};range.memory=memory;range.size=VK_WHOLE_SIZE;check(vkInvalidateMappedMemoryRanges(v.device,1,&range),"invalidate host cache");}
 auto bytes=static_cast<const unsigned char*>(mapped);uint64_t differing{},absoluteDelta{};const bool bgra=format==VK_FORMAT_B8G8R8A8_UNORM||format==VK_FORMAT_B8G8R8A8_SRGB;
 for(VkDeviceSize i=0;i<eyeBytes;i+=4){bool different=false;for(int c=0;c<3;++c){int delta=int(bytes[i+c])-int(bytes[eyeBytes+i+c]);absoluteDelta+=std::abs(delta);different|=delta!=0;}differing+=different;}
 const uint32_t stride=sequence&&!handCapture?4u:1u,captureWidth=(extent.width+stride-1)/stride,captureHeight=(extent.height+stride-1)/stride;
 for(int e=0;e<2;++e){std::ofstream file(path(e?"capture_right.ppm":"capture_left.ppm"),std::ios::binary);file<<"P6\n"<<captureWidth<<' '<<captureHeight<<"\n255\n";std::vector<char> rgb(size_t(captureWidth)*captureHeight*3);for(uint32_t y=0;y<captureHeight;++y)for(uint32_t x=0;x<captureWidth;++x){const auto p=bytes+eyeBytes*e+(VkDeviceSize(y*stride)*extent.width+x*stride)*4;auto index=(size_t(y)*captureWidth+x)*3;rgb[index]=char(p[bgra?2:0]);rgb[index+1]=char(p[1]);rgb[index+2]=char(p[bgra?0:2]);}file.write(rgb.data(),rgb.size());if(!file)fail("native capture file write failed");}
 std::ofstream auxiliary(path("aux.tsv"));auxiliary<<"label\tformat\twidth\theight\tlayout\tbytes\tsourceWidth\tsourceHeight\n";
 for(size_t i=2;i<targets.size();++i){const auto& target=targets[i];std::ofstream file(path("aux_"+target.label+".raw"),std::ios::binary);file.write(reinterpret_cast<const char*>(bytes+target.offset),size_t(target.bytes));if(!file)fail("native auxiliary capture write failed");auxiliary<<target.label<<'\t'<<int(target.format)<<'\t'<<target.extent.width<<'\t'<<target.extent.height<<'\t'<<int(target.layout)<<'\t'<<target.bytes<<'\t'<<target.sourceExtent.width<<'\t'<<target.sourceExtent.height<<'\n';}
 const double meanDelta=double(absoluteDelta)/(double(extent.width)*extent.height*3);
 {std::ofstream trace(path("capture_dataflow.txt"));trace<<"frame="<<pair.pose.serial<<'\n';for(const auto& line:nativeFrameTrace)trace<<line<<'\n';}
 std::ofstream evidence(path("capture.json"));
 evidence<<"{\n  \"source\": \"native-producer-pair-used-for-openxr-copy\",\n  \"xrProjectionAccepted\": true,\n  \"gpuCompletionVerified\": true,\n  \"frame\": "<<pair.pose.serial
  <<",\n  \"generation\": "<<pair.generation<<",\n  \"width\": "<<extent.width<<",\n  \"height\": "<<extent.height
  <<",\n  \"differentRgbPixels\": "<<differing<<",\n  \"meanAbsoluteChannelDifference\": "<<meanDelta
  <<",\n  \"leftImage\": "<<reinterpret_cast<uintptr_t>(pair.eyes[0].image)<<",\n  \"rightImage\": "<<reinterpret_cast<uintptr_t>(pair.eyes[1].image)
  <<",\n  \"headPosition\": ["<<pair.pose.head.position.x<<','<<pair.pose.head.position.y<<','<<pair.pose.head.position.z
  <<"],\n  \"headOrientation\": ["<<pair.pose.head.orientation.x<<','<<pair.pose.head.orientation.y<<','<<pair.pose.head.orientation.z<<','<<pair.pose.head.orientation.w
  <<"],\n  \"bodyOrigin\": ["<<renderingBodyOrigin[0]<<','<<renderingBodyOrigin[1]<<','<<renderingBodyOrigin[2]
  <<"],\n  \"worldScale\": "<<pair.pose.worldScale
  <<",\n  \"renderSeparationGameUnits\": "<<(g_ipd_override.load()>=0?g_ipd_override.load():frame_root_double_ipd.load())
  <<",\n  \"diagnosticZeroIpd\": "<<(kharvox::runtimeFileExists(L"native_test_zero_ipd")?"true":"false")<<",\n  \"eyes\": [";
 for(uint32_t e=0;e<2;++e){const auto& eye=pair.eyes[e];if(e)evidence<<',';evidence<<"{\"index\":"<<e<<",\"frame\":"<<eye.frame<<",\"layout\":"<<int(eye.layout)
   <<",\"position\":["<<eye.pose.position.x<<','<<eye.pose.position.y<<','<<eye.pose.position.z<<"],\"fov\":["<<eye.fov.angleLeft<<','<<eye.fov.angleRight<<','<<eye.fov.angleUp<<','<<eye.fov.angleDown<<"]}";}
 evidence<<"],\n  \"headsetValidated\": false\n}\n";
 if(handCapture){
  std::ofstream inputs(path("hand_poses.tsv"));inputs<<"role\tvalid\tx\ty\tz\tqx\tqy\tqz\tqw\n";
  auto poseRow=[&](const char* role,bool valid,const XrPosef& pose){inputs<<role<<'\t'<<valid<<'\t'<<pose.position.x<<'\t'<<pose.position.y<<'\t'<<pose.position.z<<'\t'<<pose.orientation.x<<'\t'<<pose.orientation.y<<'\t'<<pose.orientation.z<<'\t'<<pose.orientation.w<<'\n';};
  poseRow("eye_left",true,pair.eyes[0].pose);poseRow("eye_right",true,pair.eyes[1].pose);
  poseRow("grip_left",pair.pose.controllersValid[0],pair.pose.controllers[0]);poseRow("grip_right",pair.pose.controllersValid[1],pair.pose.controllers[1]);
 }
 vkUnmapMemory(v.device,memory);vkDestroyFence(v.device,fence,nullptr);vkDestroyCommandPool(v.device,pool,nullptr);vkDestroyBuffer(v.device,buffer,nullptr);vkFreeMemory(v.device,memory,nullptr);if(!handCapture)++captured;
 if(handCapture)kharvoxnative::log::info(std::format("Native hand capture saved prefix={} depthTargets={} leftDepthIncludesIntegratedHands=true rightDepthIsEngineOnly=true",prefix,targets.size()-2));
 kharvoxnative::log::info(std::format("Native GPU readback frame={} distinctImages=true differentRgbPixels={} meanChannelDelta={} (technical evidence, not headset acceptance)",pair.pose.serial,differing,meanDelta));
}

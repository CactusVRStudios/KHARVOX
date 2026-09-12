// Optional causal comparison. DOOM's CPU shadow cache can survive both root
// calls, but the last call writes a private atlas. Publish that terminal atlas
// only AFTER both eye consumers, so next frame starts from the same history.
// This hypothesis is not a proven fix; no quality setting/shader is changed.
static bool shadowHistoryCompare(){static const bool enabled=kharvox::runtimeFileExists(L"native_compare_shadow_history");return enabled;}
static bool shadowHistoryEnabled{};
static ShadowHistoryFrame shadowHistoryFrame;
static std::set<VkImageView> shadowHistoryViews;
static uint32_t shadowHistoryCopies{};
static void beginShadowHistory(){
 if(!shadowHistoryCompare())return;
 if(!shadowHistoryFrame.begin(renderingPose.serial))fail("shadow history previous owner submission uncompleted");
 shadowHistoryViews.clear();shadowHistoryCopies=0;
 shadowHistoryEnabled=kharvox::runtimeFileExists(L"native_test_shadow_history");
 static int previous=-1;if(previous!=int(shadowHistoryEnabled)){previous=int(shadowHistoryEnabled);
  kharvoxnative::log::warn(std::format("Native shadow history comparison frame={} enabled={} commitAfterEyeConsumers=true qualityUnchanged=true (diagnostic hypothesis)",renderingPose.serial,shadowHistoryEnabled));}
}
static void observeShadowHistorySeed(VkImageView view){
 if(!shadowHistoryCompare())return;
 std::scoped_lock fl(framebuffer_depth_view_mutex);
 auto vi=image_view_infos.find(view);if(vi==image_view_infos.end())fail("shadow history view disappeared");
 auto ii=sourceImageDefinitions.find(vi->second.image);if(ii==sourceImageDefinitions.end())fail("shadow history source definition missing");
 const auto& info=ii->second;
 const auto transfers=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
 if(shadowHistoryShape(info.width,info.height,info.format==VK_FORMAT_X8_D24_UNORM_PACK32,
      info.mips,info.layers,info.samples==VK_SAMPLE_COUNT_1_BIT,
      (info.usage&(VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))==(VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT),
      (info.usage&transfers)==transfers)&&info.type==VK_IMAGE_TYPE_2D&&info.depth==1)
  shadowHistoryViews.insert(view);
}
void recordShadowHistory(VkCommandBuffer command,const StereoFrame& frame){
 if(!shadowHistoryCompare()||!shadowHistoryEnabled)return;
 std::scoped_lock l(replayMutex);
 bool submitted=true;{std::scoped_lock sl(submissionMutex);for(auto cb:recordedCommands)submitted&=submittedCommands.count(cb)!=0;}
 // pair() consumes finalDone to prevent a second handoff. The immutable
 // handedPair, not that consumed readiness token, owns subsequent work.
 const bool matchingPair=pairHanded&&handedPair.pose.serial==frame.pose.serial
     &&rootDone==frame.pose.serial&&handedPair.generation==frame.generation;
 if(!outstanding||!shadowHistoryFrame.record(frame.pose.serial,matchingPair,submitted))fail("shadow history attempted before both eye producers or more than once");
 std::scoped_lock ml(mirror_mutex);std::scoped_lock fl(framebuffer_depth_view_mutex);
 std::set<VkImage> copied;
 for(auto view:shadowHistoryViews){
  auto vi=image_view_infos.find(view);auto mirror=mirrored_views.find(view);
  if(vi==image_view_infos.end()||mirror==mirrored_views.end())fail("shadow history source or mirror retired");
  const auto source=vi->second.image,target=mirror->second.image;
  if(!copied.insert(source).second)continue;
  if(source==target||!storageSeeded.count(target)||!mirror_images_written.count(target))fail("shadow history private atlas was not initialized this frame");
  auto info=sourceImageDefinitions.find(source);if(info==sourceImageDefinitions.end())fail("shadow history definition retired");
  const auto originalLayout=sourceLayouts.whole(source);const auto mirrorLayout=mirror_layout_of(target);
  if(!originalLayout||*originalLayout==VK_IMAGE_LAYOUT_UNDEFINED||mirrorLayout==VK_IMAGE_LAYOUT_UNDEFINED)fail("shadow history atlas layout unknown");
  const VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
  VkImageMemoryBarrier b[2]{};
  for(auto& x:b){x.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;x.srcQueueFamilyIndex=x.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;x.subresourceRange=range;x.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;}
  b[0].image=target;b[0].oldLayout=mirrorLayout;b[0].newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b[0].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
  b[1].image=source;b[1].oldLayout=*originalLayout;b[1].newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b[1].dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
  real_cmd_pipeline_barrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,2,b);
  VkImageCopy copy{};copy.srcSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};copy.dstSubresource=copy.srcSubresource;copy.extent={info->second.width,info->second.height,1};
  real_cmd_copy_image(command,target,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,source,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
  for(auto& x:b){std::swap(x.oldLayout,x.newLayout);x.srcAccessMask=x.dstAccessMask;x.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;}
  real_cmd_pipeline_barrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,2,b);
  ++shadowHistoryCopies;
 }
 // Runs in the owner's existing copy command buffer. It has the existing
 // Present waits exactly once and must complete before another root begins.
}
static void finishShadowHistory(){
 if(!shadowHistoryFrame.recorded)return;
 if(!shadowHistoryFrame.finish(true))fail("shadow history completion state invalid");
 static uint32_t count{};if(++count<=6||count%120==0)kharvoxnative::log::info(std::format("Native shadow history frame={} committedAtlases={} ownerGpuComplete=true layoutsRestored=true extraSubmits=0 extraPresentWaits=0",shadowHistoryFrame.frame,shadowHistoryCopies));
}

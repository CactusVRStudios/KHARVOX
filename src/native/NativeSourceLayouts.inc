// Included after pass metadata, inside kharvox::native. All source transitions
// are observed, but repair is enabled only while a Native frame is outstanding.
static SourceLayoutState sourceLayouts;
struct SourceImageDefinition {uint32_t width,height,depth,mips,layers;VkFormat format;VkImageUsageFlags usage;VkImageType type;VkSampleCountFlagBits samples;};
static std::map<VkImage,SourceImageDefinition> sourceImageDefinitions;
static bool sequenceDiagnostic(){static const bool enabled=kharvox::runtimeFileExists(L"capture_native_stereo_sequence");return enabled;}
void recordSourceImage(VkImage image,const VkImageCreateInfo& info){std::scoped_lock l(replayMutex);sourceLayouts.define(image,info.mipLevels,info.arrayLayers,info.initialLayout);sourceImageDefinitions[image]={info.extent.width,info.extent.height,info.extent.depth,info.mipLevels,info.arrayLayers,info.format,info.usage,info.imageType,info.samples};}
void forgetSourceImage(VkImage image){
 std::scoped_lock l(replayMutex);sourceLayouts.forget(image);
 sourceImageDefinitions.erase(image);
 std::scoped_lock ml(mirror_mutex);doom_image_layouts.erase(image);
 std::scoped_lock fl(framebuffer_depth_view_mutex);created_image_infos.erase(image);
}
static void sourcePassState(const VkRenderPassBeginInfo& begin,bool active){
 if(inside_frame_root_double||inside_final_pass_double)return; // the attachment was redirected
 auto p=passDefinitions.find(begin.renderPass);if(p==passDefinitions.end()||!p->second.supported)return;
 std::scoped_lock ml(mirror_mutex);std::scoped_lock fl(framebuffer_depth_view_mutex);
 auto f=framebuffer_infos.find(begin.framebuffer);if(f==framebuffer_infos.end())return;
 for(size_t i=0;i<f->second.attachments.size()&&i<p->second.attachments.size();++i){
  auto v=image_view_infos.find(f->second.attachments[i]);if(v==image_view_infos.end())continue;
  auto layout=active?p->second.active[i]:p->second.attachments[i].finalLayout;if(layout==VK_IMAGE_LAYOUT_UNDEFINED)continue;
  auto range=v->second.range;range.levelCount=1;range.layerCount=f->second.layers;
  sourceLayouts.observe(v->second.image,range,layout);
  if(auto whole=sourceLayouts.whole(v->second.image))doom_image_layouts[v->second.image]=*whole;
 }
}
static void VKAPI_CALL sourceBarrier(VkCommandBuffer cb,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags flags,uint32_t nm,const VkMemoryBarrier* m,uint32_t nb,const VkBufferMemoryBarrier* b,uint32_t ni,const VkImageMemoryBarrier* images){
 std::scoped_lock l(replayMutex);std::vector<VkImageMemoryBarrier> corrected;uint32_t changes=0;
 for(uint32_t i=0;i<ni;++i)changes+=sourceLayouts.transition(images[i],outstanding,corrected);
 if(changes){
  // A real source can still contain compute output when the second root asks
  // for the transition that followed its redirected color attachment. Cover
  // those writes as well. In-render-pass layout repair is never silently used.
  if(replayStates[cb].inRenderPass)fail("original image layout repair inside a render pass is unsupported");
  src=dst=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  for(auto& image:corrected){image.srcAccessMask|=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;image.dstAccessMask|=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;}
  static uint64_t count{};count+=changes;
  if(count<=12||count%300==0)kharvoxnative::log::info(std::format("Native source layout repair frame={} corrected={} total={} side={} originalsAndMirrorBarriersPreserved=true",renderingPose.serial,changes,count,inside_frame_root_double?"right":"left"));
 }
 // The donor's barrier augmentation and its mirror subresource ledger remain
 // in force. No original barrier is stolen to perform a mirror transition.
 hook_cmd_pipeline_barrier(cb,src,dst,flags,nm,m,nb,b,uint32_t(corrected.size()),corrected.data());
}

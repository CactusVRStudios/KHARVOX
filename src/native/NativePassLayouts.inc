// Included inside kharvox::native, after ReplayState. Native-only render-pass
// metadata and a STORE/LOAD continuation for descriptor layout transitions.
// Vulkan render-pass compatibility ignores attachment load/store operations
// and initial/final layouts (spec, Render Pass Compatibility).
struct PassDefinition {
 bool supported{},hasDepth{};
 VkRenderPassCreateFlags flags{};
 VkSubpassDescription subpass{};
 std::vector<VkAttachmentDescription> attachments;
 std::vector<VkAttachmentReference> inputs,colors,resolves;
 VkAttachmentReference depth{};
 std::vector<uint32_t> preserves;
 std::vector<VkSubpassDependency> dependencies;
 std::vector<VkImageLayout> active;
 VkRenderPass resume{};
};
static std::map<VkRenderPass,PassDefinition> passDefinitions;
static VkResult VKAPI_CALL captureRenderPass(VkDevice device,const VkRenderPassCreateInfo* info,const VkAllocationCallbacks* allocator,VkRenderPass* pass){
 const auto result=hook_create_render_pass(device,info,allocator,pass);
 if(result!=VK_SUCCESS||!info||!pass)return result;
 std::scoped_lock l(replayMutex);auto& d=passDefinitions[*pass];d={};
 if(info->pNext||info->subpassCount!=1)return result;
 d.supported=true;d.flags=info->flags;d.subpass=info->pSubpasses[0];
 if(info->attachmentCount)d.attachments.assign(info->pAttachments,info->pAttachments+info->attachmentCount);
 if(d.subpass.inputAttachmentCount)d.inputs.assign(d.subpass.pInputAttachments,d.subpass.pInputAttachments+d.subpass.inputAttachmentCount);
 if(d.subpass.colorAttachmentCount)d.colors.assign(d.subpass.pColorAttachments,d.subpass.pColorAttachments+d.subpass.colorAttachmentCount);
 if(d.subpass.pResolveAttachments)d.resolves.assign(d.subpass.pResolveAttachments,d.subpass.pResolveAttachments+d.subpass.colorAttachmentCount);
 if(d.subpass.pDepthStencilAttachment){d.hasDepth=true;d.depth=*d.subpass.pDepthStencilAttachment;}
 if(d.subpass.preserveAttachmentCount)d.preserves.assign(d.subpass.pPreserveAttachments,d.subpass.pPreserveAttachments+d.subpass.preserveAttachmentCount);
 if(info->dependencyCount)d.dependencies.assign(info->pDependencies,info->pDependencies+info->dependencyCount);
 d.active.resize(d.attachments.size(),VK_IMAGE_LAYOUT_UNDEFINED);
 auto use=[&](const VkAttachmentReference& ref){if(ref.attachment==VK_ATTACHMENT_UNUSED)return;if(ref.attachment>=d.active.size()){d.supported=false;return;}auto& layout=d.active[ref.attachment];if(layout!=VK_IMAGE_LAYOUT_UNDEFINED&&layout!=ref.layout)d.supported=false;layout=ref.layout;};
 for(auto& ref:d.inputs)use(ref);for(auto& ref:d.colors)use(ref);for(auto& ref:d.resolves)use(ref);if(d.hasDepth)use(d.depth);
 return result;
}
static PassDefinition& passDefinition(VkRenderPass pass){
 auto it=passDefinitions.find(pass);if(it==passDefinitions.end()||!it->second.supported)fail("native render pass metadata incomplete or multiple subpasses unsupported");return it->second;
}
static void attachmentState(const VkRenderPassBeginInfo& begin,bool active){
 auto& d=passDefinition(begin.renderPass);
 std::scoped_lock fl(framebuffer_depth_view_mutex);
 auto f=framebuffer_infos.find(begin.framebuffer);if(f==framebuffer_infos.end()||f->second.attachments.size()!=d.attachments.size())fail("native attachment count mismatch");
 for(size_t i=0;i<d.attachments.size();++i){auto mirror=mirrored_views.find(f->second.attachments[i]);if(mirror==mirrored_views.end())fail("native attachment mirror absent");auto layout=active?d.active[i]:d.attachments[i].finalLayout;if(layout!=VK_IMAGE_LAYOUT_UNDEFINED)set_mirror_range_layout(mirror->second.image,{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,f->second.layers},layout);}
}
static VkRenderPass continuationPass(VkRenderPass original){
 auto& d=passDefinition(original);if(d.resume)return d.resume;
 auto attachments=d.attachments;
 for(size_t i=0;i<attachments.size();++i){auto& a=attachments[i];if(d.active[i]==VK_IMAGE_LAYOUT_UNDEFINED)continue;
  // A continuation cannot recover contents discarded when ending the first
  // pass. Reject that combination before recording its end.
  const bool stencil=a.format==VK_FORMAT_S8_UINT||a.format==VK_FORMAT_D16_UNORM_S8_UINT||a.format==VK_FORMAT_D24_UNORM_S8_UINT||a.format==VK_FORMAT_D32_SFLOAT_S8_UINT;
  if((a.format!=VK_FORMAT_S8_UINT&&a.storeOp!=VK_ATTACHMENT_STORE_OP_STORE)||(stencil&&a.stencilStoreOp!=VK_ATTACHMENT_STORE_OP_STORE))fail("native layout continuation requires stored attachments");
  a.initialLayout=a.finalLayout;a.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;a.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_LOAD;
 }
 auto sub=d.subpass;sub.pInputAttachments=d.inputs.data();sub.pColorAttachments=d.colors.data();sub.pResolveAttachments=d.resolves.empty()?nullptr:d.resolves.data();sub.pDepthStencilAttachment=d.hasDepth?&d.depth:nullptr;sub.pPreserveAttachments=d.preserves.data();
 VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};ci.flags=d.flags;ci.attachmentCount=uint32_t(attachments.size());ci.pAttachments=attachments.data();ci.subpassCount=1;ci.pSubpasses=&sub;ci.dependencyCount=uint32_t(d.dependencies.size());ci.pDependencies=d.dependencies.data();
 if(real_create_render_pass(kharvoxnative::vulkan_state().device,&ci,nullptr,&d.resume)!=VK_SUCCESS)fail("native continuation render pass creation failed");
 return d.resume;
}
// Called with replayMutex and mirror_mutex held. These are raw dispatches:
// the donor's logical pass state continues unchanged across STORE/LOAD.
static VkRenderPass pauseForLayouts(VkCommandBuffer cb){
 auto& r=replayStates[cb];if(!r.inRenderPass)return VK_NULL_HANDLE;

 const auto resume=continuationPass(r.activeBegin.renderPass);
 real_cmd_end_render_pass(cb);r.inRenderPass=false;attachmentState(r.activeBegin,false);
 return resume;
}
static void resumeAfterLayouts(VkCommandBuffer cb,VkRenderPass resume){
 if(!resume)return;auto& r=replayStates[cb];auto begin=r.activeBegin;
 auto mirror=mirror_targets.find(begin.framebuffer);if(mirror==mirror_targets.end()||!mirror->second.ok)fail("native continuation target missing");
 begin.framebuffer=mirror->second.framebuffer;begin.renderPass=resume;begin.clearValueCount=0;begin.pClearValues=nullptr;
 real_cmd_begin_render_pass(cb,&begin,VK_SUBPASS_CONTENTS_INLINE);r.inRenderPass=true;attachmentState(r.activeBegin,true);
 static uint64_t count{};if(++count<=6||count%300==0)kharvoxnative::log::info(std::format("Native descriptor layout continuation={} frame={} pass={}",count,renderingPose.serial,doubled_pass_seq));
}
static void VKAPI_CALL destroyCapturedRenderPass(VkDevice device,VkRenderPass pass,const VkAllocationCallbacks* allocator){
 beforeDestroy("vkDestroyRenderPass",reinterpret_cast<uintptr_t>(pass));std::scoped_lock l(replayMutex);auto it=passDefinitions.find(pass);if(it!=passDefinitions.end()){if(it->second.resume)real_destroy_render_pass(device,it->second.resume,nullptr);passDefinitions.erase(it);}hook_destroy_render_pass(device,pass,allocator);
}

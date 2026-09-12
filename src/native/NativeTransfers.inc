// Native-only image transfer routing. Copies outside FrameRoot after it has
// completed preserve DOOM's command and append the corresponding mirror work.
// Copies inside the second root target only the private images.
static bool transferMirrors(VkImage src,VkImage dst,VkImage& mirrorSrc,VkImage& mirrorDst){
 if(!outstanding)return false;
 if(!inside_frame_root_double&&!inside_final_pass_double&&(inside_real_frame_root||!rootDone))return false;
 auto source=mirrored_images.find(src),target=mirrored_images.find(dst);
 if(target==mirrored_images.end()){if(source!=mirrored_images.end())fail("second-eye image transfer destination is not mirrored");return false;}
 mirrorSrc=source==mirrored_images.end()?src:source->second;mirrorDst=target->second;return true;
}
static void transferLayout(std::vector<VkImageMemoryBarrier>& barriers,VkImage image,VkImageLayout layout,const VkImageSubresourceLayers& sub){
 if(!native_image_layouts.count(image))return;
 VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=image;b.newLayout=layout;b.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
 b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
 for(uint32_t layer=sub.baseArrayLayer;layer<sub.baseArrayLayer+sub.layerCount;++layer){
  b.subresourceRange={sub.aspectMask,sub.mipLevel,1,layer,1};bool found=false;
  for(const auto& previous:barriers)if(previous.image==image&&previous.subresourceRange.baseMipLevel==sub.mipLevel&&previous.subresourceRange.baseArrayLayer==layer){if(previous.newLayout!=layout)fail("overlapping native transfer layouts");found=true;break;}
  if(!found)append_mirror_transitions(barriers,b);
 }
}
static void transferBarriers(VkCommandBuffer cb,const std::vector<VkImageMemoryBarrier>& barriers){
 validateSeedCommand(cb);
 {std::scoped_lock l(submissionMutex);if(submittedCommands.count(cb))fail("native transfer appended after submission");recordedCommands.insert(cb);}
 real_cmd_pipeline_barrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,uint32_t(barriers.size()),barriers.data());
}
static void VKAPI_CALL nativeCopyImage(VkCommandBuffer cb,VkImage src,VkImageLayout srcLayout,VkImage dst,VkImageLayout dstLayout,uint32_t count,const VkImageCopy* regions){
 std::scoped_lock l(replayMutex);std::scoped_lock ml(mirror_mutex);VkImage s{},d{};
 checkFinalCommand(cb,"nativeCopyImage");
 bool mirror=transferMirrors(src,dst,s,d);
 if(!mirror||(!inside_frame_root_double&&!inside_final_pass_double))real_cmd_copy_image(cb,src,srcLayout,dst,dstLayout,count,regions);
 if(!mirror)return;
 std::vector<VkImageMemoryBarrier> barriers;for(uint32_t i=0;i<count;++i){transferLayout(barriers,s,srcLayout,regions[i].srcSubresource);transferLayout(barriers,d,dstLayout,regions[i].dstSubresource);}transferBarriers(cb,barriers);
 real_cmd_copy_image(cb,s,srcLayout,d,dstLayout,count,regions);mirror_images_written.insert(d);
}
static void VKAPI_CALL nativeBlitImage(VkCommandBuffer cb,VkImage src,VkImageLayout srcLayout,VkImage dst,VkImageLayout dstLayout,uint32_t count,const VkImageBlit* regions,VkFilter filter){
 std::scoped_lock l(replayMutex);std::scoped_lock ml(mirror_mutex);VkImage s{},d{};
 checkFinalCommand(cb,"nativeBlitImage");
 bool mirror=transferMirrors(src,dst,s,d);
 if(!mirror||(!inside_frame_root_double&&!inside_final_pass_double))real_cmd_blit_image(cb,src,srcLayout,dst,dstLayout,count,regions,filter);
 if(!mirror)return;
 std::vector<VkImageMemoryBarrier> barriers;for(uint32_t i=0;i<count;++i){transferLayout(barriers,s,srcLayout,regions[i].srcSubresource);transferLayout(barriers,d,dstLayout,regions[i].dstSubresource);}transferBarriers(cb,barriers);
 real_cmd_blit_image(cb,s,srcLayout,d,dstLayout,count,regions,filter);mirror_images_written.insert(d);
 static uint32_t copies{};if(++copies<=4)kharvoxnative::log::info(std::format("Native mirrored blit={} frame={} mip {} -> {}",copies,renderingPose.serial,regions[0].srcSubresource.mipLevel,regions[0].dstSubresource.mipLevel));
}

template<class... A> static void decode(const char* name,uint64_t link,A...){
 if(std::strstr(name,"Template")||std::strstr(name,"Rendering")||std::strstr(name,"ClearAttachments")||std::strstr(name,"RenderPass2")||std::strstr(name,"WaitEvents")||std::strstr(name,"ResolveImage")||std::strstr(name,"PushConstants"))row("coverage-gap-structured-arguments",link);
}
template<class R,class... A> static void registry(const char*,R,A...){ }
static void registry(const char*,VkResult result,VkDevice,const VkCommandPoolCreateInfo* info,const VkAllocationCallbacks*,VkCommandPool* out){
 if(result!=VK_SUCCESS)return;{std::scoped_lock l(mutex);if(pools.size()<1024)pools[*out]=info->queueFamilyIndex;else ++registryDropped;}
 row("pool-created",parent,{handle(*out),info->queueFamilyIndex,info->flags});
}
static void registry(const char*,VkResult result,VkDevice,const VkCommandBufferAllocateInfo* info,VkCommandBuffer* out){
 if(result!=VK_SUCCESS)return;for(uint32_t i=0;i<info->commandBufferCount;++i){
  {std::scoped_lock l(mutex);if(commands.size()<8192)commands[out[i]]={info->commandPool,uint32_t(info->level)};else ++registryDropped;}
  row("command-allocated",parent,{handle(out[i]),handle(info->commandPool),uint64_t(info->level)});
 }
}
static void registry(const char*,VkResult result,VkCommandBuffer cb,const VkCommandBufferBeginInfo* info){
 if(result!=VK_SUCCESS)return;{std::scoped_lock l(mutex);auto it=commands.find(cb);if(it!=commands.end()){auto& c=it->second;c.flags=info->flags;c.recording=true;c.beginNs=timestamp();++c.generation;}}
 row("command-begin",parent,{handle(cb),info->flags,uint64_t(info->pInheritanceInfo!=nullptr)});
 if(active()){extensionRows(info->pNext,parent);if(info->pInheritanceInfo){const auto& p=*info->pInheritanceInfo;row("command-inheritance",parent,{handle(cb),handle(p.renderPass),p.subpass,handle(p.framebuffer),p.occlusionQueryEnable,p.queryFlags,p.pipelineStatistics});extensionRows(p.pNext,parent);}}
}
static void registry(const char* name,VkResult result,VkCommandBuffer cb){
 if(!std::strstr(name,"vkEndCommandBuffer"))return;
 if(result!=VK_SUCCESS)return;{std::scoped_lock l(mutex);auto it=commands.find(cb);if(it!=commands.end()){it->second.recording=false;it->second.endNs=timestamp();}}
 row("command-end",parent,{handle(cb)});
}
static void registry(const char*,VkResult result,VkCommandBuffer cb,VkCommandBufferResetFlags flags){
 if(result!=VK_SUCCESS)return;{std::scoped_lock l(mutex);auto it=commands.find(cb);if(it!=commands.end()){it->second.recording=false;++it->second.generation;}}
 row("command-reset",parent,{handle(cb),flags});
}
static void registry(const char*,VkResult result,VkDevice,VkCommandPool pool,VkCommandPoolResetFlags flags){
 if(result!=VK_SUCCESS)return;{std::scoped_lock l(mutex);for(auto& [cb,c]:commands)if(c.pool==pool){c.recording=false;++c.generation;}}
 row("pool-reset",parent,{handle(pool),flags});
}
static void registry(const char*,VkResult,VkDevice,VkCommandPool pool,uint32_t count,const VkCommandBuffer* cb){
 for(uint32_t i=0;i<count;++i){row("command-freed",parent,{handle(cb[i]),handle(pool)});std::scoped_lock l(mutex);commands.erase(cb[i]);}
}
static void registry(const char*,VkResult,VkDevice,VkCommandPool pool,const VkAllocationCallbacks*){
 row("pool-destroyed",parent,{handle(pool)});std::scoped_lock l(mutex);pools.erase(pool);for(auto it=commands.begin();it!=commands.end();)if(it->second.pool==pool)it=commands.erase(it);else ++it;
}
static void registry(const char*,VkResult,VkDevice,uint32_t family,uint32_t index,VkQueue* q){if(*q){std::scoped_lock l(mutex);if(queues.size()<64)queues[*q]={family,index,0};else ++registryDropped;}row("queue",parent,{handle(*q),family,index});}
static void registry(const char*,VkResult,VkDevice,const VkDeviceQueueInfo2* info,VkQueue* q){if(*q){std::scoped_lock l(mutex);if(queues.size()<64)queues[*q]={info->queueFamilyIndex,info->queueIndex,info->flags};else ++registryDropped;}row("queue",parent,{handle(*q),info->queueFamilyIndex,info->queueIndex,info->flags});}
static void registry(const char*,VkResult result,VkDevice,const VkSemaphoreCreateInfo* info,const VkAllocationCallbacks*,VkSemaphore* out){if(result==VK_SUCCESS){
 uint32_t type=VK_SEMAPHORE_TYPE_BINARY;uint64_t value{};
 for(auto p=static_cast<const VkBaseInStructure*>(info->pNext);p;p=p->pNext)if(p->sType==VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO){auto t=reinterpret_cast<const VkSemaphoreTypeCreateInfo*>(p);type=uint32_t(t->semaphoreType);value=t->initialValue;}
 {std::scoped_lock l(mutex);if(semaphores.size()<8192)semaphores[*out]={type,value};else ++registryDropped;}
 row("semaphore-created",parent,{handle(*out),info->flags,type,value});if(active())extensionRows(info->pNext,parent);
}}
static void registry(const char*,VkResult,VkDevice,VkSemaphore semaphore,const VkAllocationCallbacks*){std::scoped_lock l(mutex);semaphores.erase(semaphore);}
static void registry(const char*,VkResult result,VkDevice,const VkFenceCreateInfo* info,const VkAllocationCallbacks*,VkFence* out){if(result==VK_SUCCESS)row("fence-created",parent,{handle(*out),info->flags});}
static void registry(const char*,VkResult result,VkDevice,const VkBufferCreateInfo* info,const VkAllocationCallbacks*,VkBuffer* out){if(result==VK_SUCCESS){row("buffer-created",parent,{handle(*out),info->size,info->usage,info->flags,uint64_t(info->sharingMode)});if(active())extensionRows(info->pNext,parent);}}
static void registry(const char*,VkResult result,VkDevice,const VkImageCreateInfo* info,const VkAllocationCallbacks*,VkImage* out){if(result==VK_SUCCESS){row("image-created",parent,{handle(*out),info->extent.width,info->extent.height,info->extent.depth,uint64_t(info->format),info->usage,info->mipLevels,info->arrayLayers});if(active())extensionRows(info->pNext,parent);}}
static void registry(const char*,VkResult result,VkDevice,const VkImageViewCreateInfo* info,const VkAllocationCallbacks*,VkImageView* out){if(result==VK_SUCCESS){const auto& r=info->subresourceRange;row("view-created",parent,{handle(*out),handle(info->image),uint64_t(info->format),r.aspectMask,r.baseMipLevel,r.levelCount,r.baseArrayLayer,r.layerCount});if(active())extensionRows(info->pNext,parent);}}
static void registry(const char*,VkResult result,VkDevice,const VkMemoryAllocateInfo* info,const VkAllocationCallbacks*,VkDeviceMemory* out){if(result==VK_SUCCESS){row("memory-allocated",parent,{handle(*out),info->allocationSize,info->memoryTypeIndex});if(active())extensionRows(info->pNext,parent);}}
static void registry(const char*,VkResult result,VkDevice,VkDeviceMemory memory,VkDeviceSize offset,VkDeviceSize size,VkMemoryMapFlags,void** out){if(result==VK_SUCCESS)row("memory-mapped",parent,{handle(memory),handle(*out),offset,size});}
static void decode(const char*,uint64_t link,VkDevice,uint32_t count,const VkFence* fences,VkBool32 all,uint64_t timeout){row("fence-wait-options",link,{count,all,timeout});for(uint32_t i=0;i<count;++i)row("fence-wait",link,{i,handle(fences[i])});}
static void decode(const char*,uint64_t link,VkDevice,uint32_t count,const VkFence* fences){for(uint32_t i=0;i<count;++i)row("fence-reset",link,{i,handle(fences[i])});}
static void decode(const char*,uint64_t link,VkDevice,const VkSemaphoreWaitInfo* info,uint64_t timeout){row("semaphore-wait-options",link,{info->flags,timeout});for(uint32_t i=0;i<info->semaphoreCount;++i)row("semaphore-cpu-wait",link,{handle(info->pSemaphores[i]),info->pValues[i]});extensionRows(info->pNext,link);}
static void decode(const char*,uint64_t link,VkDevice,const VkSemaphoreSignalInfo* info){row("semaphore-cpu-signal",link,{handle(info->semaphore),info->value});extensionRows(info->pNext,link);}
static void registry(const char*,VkResult result,VkDevice,VkSwapchainKHR sc,uint64_t timeout,VkSemaphore semaphore,VkFence fence,uint32_t* index){if(result==VK_SUCCESS||result==VK_SUBOPTIMAL_KHR)row("acquire-image",parent,{handle(sc),*index,handle(semaphore),handle(fence),timeout});}
static void registry(const char*,VkResult result,VkDevice,const VkAcquireNextImageInfoKHR* info,uint32_t* index){if(result==VK_SUCCESS||result==VK_SUBOPTIMAL_KHR)row("acquire-image",parent,{handle(info->swapchain),*index,handle(info->semaphore),handle(info->fence),info->timeout,info->deviceMask});}
static void decode(const char*,uint64_t link,VkCommandBuffer cb,VkImage image,VkImageLayout layout,const VkClearDepthStencilValue* value,uint32_t count,const VkImageSubresourceRange* ranges){uint32_t depth{};std::memcpy(&depth,&value->depth,4);row("clear-depth-stencil",link,{handle(cb),handle(image),uint64_t(layout),depth,value->stencil,count});for(uint32_t i=0;i<count;++i){const auto& r=ranges[i];row("clear-range",link,{i,r.aspectMask,r.baseMipLevel,r.levelCount,r.baseArrayLayer,r.layerCount});}}
static void decode(const char*,uint64_t link,VkCommandBuffer cb,VkImage image,VkImageLayout layout,const VkClearColorValue* value,uint32_t count,const VkImageSubresourceRange* ranges){row("clear-color",link,{handle(cb),handle(image),uint64_t(layout),value->uint32[0],value->uint32[1],value->uint32[2],value->uint32[3],count});for(uint32_t i=0;i<count;++i){const auto& r=ranges[i];row("clear-range",link,{i,r.aspectMask,r.baseMipLevel,r.levelCount,r.baseArrayLayer,r.layerCount});}}
static void bufferImageRegions(uint64_t link,uint32_t count,const VkBufferImageCopy* copies){for(uint32_t i=0;i<count;++i){const auto& c=copies[i];row("buffer-image-range",link,{i,c.bufferOffset,c.bufferRowLength,c.bufferImageHeight,c.imageSubresource.aspectMask,c.imageSubresource.mipLevel,c.imageSubresource.baseArrayLayer,c.imageSubresource.layerCount});row("buffer-image-region",link,{i,uint64_t(c.imageOffset.x),uint64_t(c.imageOffset.y),uint64_t(c.imageOffset.z),c.imageExtent.width,c.imageExtent.height,c.imageExtent.depth});}}
static void decode(const char*,uint64_t link,VkCommandBuffer cb,VkBuffer buffer,VkImage image,VkImageLayout layout,uint32_t count,const VkBufferImageCopy* copies){row("buffer-to-image",link,{handle(cb),handle(buffer),handle(image),uint64_t(layout)});bufferImageRegions(link,count,copies);}
static void decode(const char*,uint64_t link,VkCommandBuffer cb,VkImage image,VkImageLayout layout,VkBuffer buffer,uint32_t count,const VkBufferImageCopy* copies){row("image-to-buffer",link,{handle(cb),handle(image),handle(buffer),uint64_t(layout)});bufferImageRegions(link,count,copies);}
static void decode(const char*,uint64_t link,VkCommandBuffer cb,uint32_t count,const VkCommandBuffer* secondary){for(uint32_t i=0;i<count;++i)row("execute-secondary",link,{handle(cb),i,handle(secondary[i])});}
static void decode(const char*,uint64_t link,VkCommandBuffer cb,uint32_t first,uint32_t count,const VkBuffer* buffers,const VkDeviceSize* offsets){for(uint32_t i=0;i<count;++i)row("vertex-buffer",link,{handle(cb),first+i,handle(buffers[i]),offsets[i]});}
static void decode(const char*,uint64_t link,VkDevice,uint32_t count,const VkMappedMemoryRange* ranges){for(uint32_t i=0;i<count;++i)row("mapped-range",link,{handle(ranges[i].memory),ranges[i].offset,ranges[i].size});}
static void decode(const char*,uint64_t link,VkCommandBuffer cb,const VkRenderPassBeginInfo* info,VkSubpassContents contents){row("render-pass",link,{handle(cb),handle(info->renderPass),handle(info->framebuffer),uint64_t(contents),uint64_t(info->renderArea.offset.x),uint64_t(info->renderArea.offset.y),info->renderArea.extent.width,info->renderArea.extent.height});extensionRows(info->pNext,link);}
static void decode(const char*,uint64_t link,VkCommandBuffer cb,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags flags,uint32_t nm,const VkMemoryBarrier* memory,uint32_t nb,const VkBufferMemoryBarrier* buffers,uint32_t ni,const VkImageMemoryBarrier* images){
 row("barrier-stages",link,{handle(cb),src,dst,flags,nm,nb,ni});
 for(uint32_t i=0;i<nm;++i)row("barrier-memory",link,{i,memory[i].srcAccessMask,memory[i].dstAccessMask});
 for(uint32_t i=0;i<nb;++i){const auto& b=buffers[i];row("barrier-buffer",link,{handle(b.buffer),b.offset,b.size,b.srcAccessMask,b.dstAccessMask,b.srcQueueFamilyIndex,b.dstQueueFamilyIndex});}
 for(uint32_t i=0;i<ni;++i){const auto& b=images[i];row("barrier-image",link,{i,handle(b.image),uint64_t(b.oldLayout),uint64_t(b.newLayout),b.srcAccessMask,b.dstAccessMask,b.srcQueueFamilyIndex,b.dstQueueFamilyIndex});row("barrier-image-range",link,{i,b.subresourceRange.aspectMask,b.subresourceRange.baseMipLevel,b.subresourceRange.levelCount,b.subresourceRange.baseArrayLayer,b.subresourceRange.layerCount});}
}
static void decode(const char*,uint64_t link,VkCommandBuffer cb,const VkDependencyInfo* info){
 row("barrier2",link,{handle(cb),info->dependencyFlags,info->memoryBarrierCount,info->bufferMemoryBarrierCount,info->imageMemoryBarrierCount});extensionRows(info->pNext,link);
 for(uint32_t i=0;i<info->memoryBarrierCount;++i){const auto& b=info->pMemoryBarriers[i];row("barrier2-memory",link,{i,b.srcStageMask,b.srcAccessMask,b.dstStageMask,b.dstAccessMask});extensionRows(b.pNext,link);}
 for(uint32_t i=0;i<info->bufferMemoryBarrierCount;++i){const auto& b=info->pBufferMemoryBarriers[i];row("barrier2-buffer",link,{i,handle(b.buffer),b.offset,b.size,b.srcQueueFamilyIndex,b.dstQueueFamilyIndex});row("barrier2-buffer-masks",link,{i,b.srcStageMask,b.srcAccessMask,b.dstStageMask,b.dstAccessMask});extensionRows(b.pNext,link);}
 for(uint32_t i=0;i<info->imageMemoryBarrierCount;++i){const auto& b=info->pImageMemoryBarriers[i];row("barrier2-image",link,{i,handle(b.image),uint64_t(b.oldLayout),uint64_t(b.newLayout),b.srcQueueFamilyIndex,b.dstQueueFamilyIndex});row("barrier2-image-masks",link,{i,b.srcStageMask,b.srcAccessMask,b.dstStageMask,b.dstAccessMask});row("barrier-image-range",link,{i,b.subresourceRange.aspectMask,b.subresourceRange.baseMipLevel,b.subresourceRange.levelCount,b.subresourceRange.baseArrayLayer,b.subresourceRange.layerCount});extensionRows(b.pNext,link);}
}
static void decode(const char*,uint64_t link,VkCommandBuffer cb,VkBuffer src,VkBuffer dst,uint32_t count,const VkBufferCopy* copies){for(uint32_t i=0;i<count;++i)row("copy-buffer",link,{handle(cb),handle(src),handle(dst),copies[i].srcOffset,copies[i].dstOffset,copies[i].size});}
static void decode(const char*,uint64_t link,VkCommandBuffer cb,VkImage src,VkImageLayout sl,VkImage dst,VkImageLayout dl,uint32_t count,const VkImageCopy* copies){
 row("copy-image",link,{handle(cb),handle(src),handle(dst),uint64_t(sl),uint64_t(dl),count});
 for(uint32_t i=0;i<count;++i){const auto& c=copies[i];row("copy-image-source",link,{i,c.srcSubresource.aspectMask,c.srcSubresource.mipLevel,c.srcSubresource.baseArrayLayer,c.srcSubresource.layerCount,uint64_t(c.srcOffset.x),uint64_t(c.srcOffset.y),uint64_t(c.srcOffset.z)});row("copy-image-target",link,{i,c.dstSubresource.aspectMask,c.dstSubresource.mipLevel,c.dstSubresource.baseArrayLayer,c.dstSubresource.layerCount,uint64_t(c.dstOffset.x),uint64_t(c.dstOffset.y),uint64_t(c.dstOffset.z)});row("copy-image-extent",link,{i,c.extent.width,c.extent.height,c.extent.depth});}
}
static void decode(const char*,uint64_t link,VkCommandBuffer cb,VkImage src,VkImageLayout sl,VkImage dst,VkImageLayout dl,uint32_t count,const VkImageBlit* copies,VkFilter filter){
 row("blit-image",link,{handle(cb),handle(src),handle(dst),uint64_t(sl),uint64_t(dl),count,uint64_t(filter)});
 for(uint32_t i=0;i<count;++i){const auto& c=copies[i];row("blit-source-range",link,{i,c.srcSubresource.aspectMask,c.srcSubresource.mipLevel,c.srcSubresource.baseArrayLayer,c.srcSubresource.layerCount});row("blit-target-range",link,{i,c.dstSubresource.aspectMask,c.dstSubresource.mipLevel,c.dstSubresource.baseArrayLayer,c.dstSubresource.layerCount});for(uint32_t k=0;k<2;++k)row("blit-offsets",link,{i,k,uint64_t(c.srcOffsets[k].x),uint64_t(c.srcOffsets[k].y),uint64_t(c.srcOffsets[k].z),uint64_t(c.dstOffsets[k].x),uint64_t(c.dstOffsets[k].y),uint64_t(c.dstOffsets[k].z)});}
}
static void decode(const char*,uint64_t link,VkCommandBuffer cb,VkPipelineBindPoint point,VkPipelineLayout layout,uint32_t first,uint32_t count,const VkDescriptorSet* sets,uint32_t dynamicCount,const uint32_t* dynamic){row("descriptor-bind",link,{handle(cb),uint64_t(point),handle(layout),first,count,dynamicCount});for(uint32_t i=0;i<count;++i)row("descriptor-set",link,{first+i,handle(sets[i])});for(uint32_t i=0;i<dynamicCount;++i)row("descriptor-dynamic",link,{i,dynamic[i]});}
static void decode(const char*,uint64_t link,VkDevice,uint32_t count,const VkWriteDescriptorSet* writes,uint32_t copies,const VkCopyDescriptorSet* copy){
 for(uint32_t i=0;i<count;++i){const auto& w=writes[i];row("descriptor-write",link,{i,handle(w.dstSet),w.dstBinding,w.dstArrayElement,w.descriptorCount,uint64_t(w.descriptorType)});extensionRows(w.pNext,link);
  if(w.pBufferInfo&&(w.descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER||w.descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER||w.descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC||w.descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC))for(uint32_t j=0;j<w.descriptorCount;++j){const auto& b=w.pBufferInfo[j];row("descriptor-write-buffer",link,{i,j,handle(b.buffer),b.offset,b.range});}
  if(w.pImageInfo&&(w.descriptorType==VK_DESCRIPTOR_TYPE_SAMPLER||w.descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER||w.descriptorType==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE||w.descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE||w.descriptorType==VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT))for(uint32_t j=0;j<w.descriptorCount;++j){const auto& b=w.pImageInfo[j];row("descriptor-write-image",link,{i,j,handle(b.sampler),handle(b.imageView),uint64_t(b.imageLayout)});}
 }
 for(uint32_t i=0;i<copies;++i){const auto& c=copy[i];row("descriptor-copy",link,{handle(c.srcSet),c.srcBinding,c.srcArrayElement,handle(c.dstSet),c.dstBinding,c.dstArrayElement,c.descriptorCount});}
}

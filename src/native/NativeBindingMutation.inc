// Wrap application metadata mutations around the existing complete handler.
// Keep its locks and Vulkan behavior; do not add a cross-thread renderer lock.
static BindingMutationClock bindingMutationClock;
static std::atomic<uint64_t> snapshotLayoutEpoch{1};
template<class T,int Tag,bool Layout=false,bool Resource=false> struct BindingMutationCall;
template<int Tag,bool Layout,bool Resource,class R,class... A> struct BindingMutationCall<R(VKAPI_PTR*)(A...),Tag,Layout,Resource> {
 using Fn=R(VKAPI_PTR*)(A...);static inline Fn next{};
 static R VKAPI_PTR call(A... args){BindingMutationScope scope(bindingMutationClock);
  struct LayoutChange {LayoutChange(){if constexpr(Layout)++snapshotLayoutEpoch;}~LayoutChange(){if constexpr(Layout)++snapshotLayoutEpoch;}} change;
  struct ResourceChange {ResourceChange(){if constexpr(Resource)qualityResourceMutation(true);}~ResourceChange(){if constexpr(Resource)qualityResourceMutation(false);}} resourceChange;
  return next(args...);}
};
static PFN_vkVoidFunction wrapBindingMutation(const char* name,PFN_vkVoidFunction next){
#define MUTATION(api) if(!std::strcmp(name,#api)){using Wrapper=BindingMutationCall<PFN_##api,__COUNTER__>;auto fn=reinterpret_cast<PFN_vkVoidFunction>(&Wrapper::call);if(next!=fn)Wrapper::next=reinterpret_cast<PFN_##api>(next);return fn;}
 MUTATION(vkUpdateDescriptorSets)
 MUTATION(vkUpdateDescriptorSetWithTemplate) MUTATION(vkUpdateDescriptorSetWithTemplateKHR)
 MUTATION(vkAllocateDescriptorSets) MUTATION(vkFreeDescriptorSets)
 MUTATION(vkResetDescriptorPool) MUTATION(vkDestroyDescriptorPool)
#define LAYOUT_MUTATION(api) if(!std::strcmp(name,#api)){using Wrapper=BindingMutationCall<PFN_##api,__COUNTER__,true>;auto fn=reinterpret_cast<PFN_vkVoidFunction>(&Wrapper::call);if(next!=fn)Wrapper::next=reinterpret_cast<PFN_##api>(next);return fn;}
 LAYOUT_MUTATION(vkCreateDescriptorSetLayout) LAYOUT_MUTATION(vkDestroyDescriptorSetLayout)
#undef LAYOUT_MUTATION
 MUTATION(vkCreateBuffer) MUTATION(vkDestroyBuffer)
 MUTATION(vkBindBufferMemory) MUTATION(vkBindBufferMemory2) MUTATION(vkBindBufferMemory2KHR)
 MUTATION(vkMapMemory) MUTATION(vkUnmapMemory) MUTATION(vkFreeMemory)
#define RESOURCE_MUTATION(api) if(!std::strcmp(name,#api)){using Wrapper=BindingMutationCall<PFN_##api,__COUNTER__,false,true>;auto fn=reinterpret_cast<PFN_vkVoidFunction>(&Wrapper::call);if(next!=fn)Wrapper::next=reinterpret_cast<PFN_##api>(next);return fn;}
 RESOURCE_MUTATION(vkCreateImage) RESOURCE_MUTATION(vkDestroyImage)
 RESOURCE_MUTATION(vkCreateImageView) RESOURCE_MUTATION(vkDestroyImageView)
 RESOURCE_MUTATION(vkCreateFramebuffer) RESOURCE_MUTATION(vkDestroyFramebuffer)
 RESOURCE_MUTATION(vkBindImageMemory) RESOURCE_MUTATION(vkBindImageMemory2) RESOURCE_MUTATION(vkBindImageMemory2KHR)
#undef RESOURCE_MUTATION
 MUTATION(vkCreateSampler) MUTATION(vkDestroySampler)
 MUTATION(vkCreateGraphicsPipelines) MUTATION(vkDestroyPipeline)
 MUTATION(vkCreatePipelineLayout) MUTATION(vkDestroyPipelineLayout)
#undef MUTATION
 return next;
}

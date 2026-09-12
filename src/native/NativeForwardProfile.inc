// Includes the existing KHARVOX layer beneath Native and the driver. It is
// CPU call time, NOT GPU execution time. Only outermost forwarding is charged.
static thread_local uint32_t forwardProfileDepth{};
template<class T,int Tag,cpu::Slot Slot> struct NativeForwardCall;
template<int Tag,cpu::Slot Slot,class R,class... A> struct NativeForwardCall<R(VKAPI_PTR*)(A...),Tag,Slot> {
 using Fn=R(VKAPI_PTR*)(A...);static inline Fn next{};
 static R VKAPI_PTR call(A... args){
  if(!cpu::current.active||!cpu::current.detailed)return next(args...);
  struct Timer {uint64_t start{};Timer(){if(forwardProfileDepth++==0&&cpu::current.active)start=cpu::now();}~Timer(){if(start)cpu::elapsed(Slot,start);--forwardProfileDepth;}} timer;
  return next(args...);
 }
};
static PFN_vkVoidFunction profileNativeForward(const char* name,PFN_vkVoidFunction next){
 if(!next||(!cpu::detailedEnabled()&&!cpu::sparseEnabled()&&!trace::enabled()))return next;
#define FORWARD(api,slot) if(!std::strcmp(name,#api)){using Wrapper=NativeForwardCall<PFN_##api,__COUNTER__,cpu::slot>;auto fn=reinterpret_cast<PFN_vkVoidFunction>(&Wrapper::call);if(next!=fn)Wrapper::next=reinterpret_cast<PFN_##api>(next);return fn;}
 FORWARD(vkCmdBindDescriptorSets,ForwardDescriptors)
 FORWARD(vkCmdDraw,ForwardDraw) FORWARD(vkCmdDrawIndexed,ForwardDraw)
 FORWARD(vkCmdDrawIndirect,ForwardDraw) FORWARD(vkCmdDrawIndexedIndirect,ForwardDraw)
 FORWARD(vkCmdBindPipeline,ForwardOther) FORWARD(vkCmdBindVertexBuffers,ForwardOther)
 FORWARD(vkCmdBindIndexBuffer,ForwardOther) FORWARD(vkCmdBeginRenderPass,ForwardOther)
 FORWARD(vkCmdEndRenderPass,ForwardOther) FORWARD(vkCmdPipelineBarrier,ForwardOther)
 FORWARD(vkCmdSetViewport,ForwardOther) FORWARD(vkCmdSetScissor,ForwardOther)
 FORWARD(vkUpdateDescriptorSets,ForwardOther) FORWARD(vkAllocateDescriptorSets,ForwardOther)
#undef FORWARD
 return next;
}

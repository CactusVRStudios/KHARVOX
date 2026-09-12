#pragma once
#include "NativeFrameTrace.h"
#include <openxr/openxr.h>
#include <cstring>
namespace kharvox::native::trace {
template<class... A> inline void xrTraceResult(uint64_t,XrResult,A...){}
inline void xrTraceResult(uint64_t link,XrResult result,XrSession,const XrFrameWaitInfo*,XrFrameState* state){if(XR_SUCCEEDED(result))row("xr-frame-state",link,{uint64_t(state->predictedDisplayTime),uint64_t(state->predictedDisplayPeriod),state->shouldRender});}
inline void xrTraceResult(uint64_t link,XrResult result,XrSwapchain swapchain,const XrSwapchainImageAcquireInfo*,uint32_t* index){if(XR_SUCCEEDED(result))row("xr-image-acquired",link,{reinterpret_cast<uintptr_t>(swapchain),*index});}
template<class... A> inline void xrTraceArguments(uint64_t,A...){}
inline void xrTraceArguments(uint64_t link,XrSwapchain swapchain,const XrSwapchainImageWaitInfo* info){row("xr-image-wait",link,{reinterpret_cast<uintptr_t>(swapchain),uint64_t(info->timeout)});}
inline void xrTraceArguments(uint64_t link,XrSwapchain swapchain,const XrSwapchainImageReleaseInfo*){row("xr-image-release",link,{reinterpret_cast<uintptr_t>(swapchain)});}
inline void xrTraceArguments(uint64_t link,XrSession,const XrFrameEndInfo* info){row("xr-frame-end",link,{uint64_t(info->displayTime),info->layerCount,uint64_t(info->environmentBlendMode)});}
template<class T> struct XrTraceCall;
template<class... A> struct XrTraceCall<XrResult(XRAPI_PTR*)(A...)> {
 using Fn=XrResult(XRAPI_PTR*)(A...);
 static inline Fn next{};static inline const char* name{};
 static XrResult XRAPI_PTR call(A... args){Scope scope(name);if(scope.id)xrTraceArguments(scope.id,args...);const auto result=next(args...);scope.result=result;if(scope.id)xrTraceResult(scope.id,result,args...);return result;}
};
template<class T> inline void wrapXrFrameTrace(const char* name,T& next){
 if(!enabled()||!next)return;
 if(std::strcmp(name,"xrWaitFrame")&&std::strcmp(name,"xrBeginFrame")&&std::strcmp(name,"xrEndFrame")&&std::strcmp(name,"xrAcquireSwapchainImage")&&std::strcmp(name,"xrWaitSwapchainImage")&&std::strcmp(name,"xrReleaseSwapchainImage"))return;
 using W=XrTraceCall<T>;if(next!=&W::call)W::next=next;W::name=name;next=&W::call;
}
}

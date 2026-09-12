#pragma once
#include "OpenXRRuntimePolicy.h"
namespace kharvox {
inline bool nativeXrReleaseRuntime(OpenXRRuntimeKind runtime){
 return isSteamBackedOpenXRRuntime(runtime)||runtime==OpenXRRuntimeKind::VirtualDesktop;
}
inline bool nativeEarlyXrRelease(bool requested,OpenXRRuntimeKind runtime,bool nativePair,bool projection,
 bool copyFence,bool queueSynchronized,bool readback){
 return requested&&nativeXrReleaseRuntime(runtime)&&nativePair&&projection&&copyFence&&queueSynchronized&&!readback;
}
inline bool nativeXrCopyFenceRequested(OpenXRRuntimeKind runtime,bool requested,bool nativePair,
 bool projection,bool queueSynchronized,bool readback){
 // Steam already used a private copy fence before this experiment. VDXR's
 // baseline/readback/loading paths retain queue-idle completion.
 return isSteamBackedOpenXRRuntime(runtime)
     ||nativeEarlyXrRelease(requested,runtime,nativePair,projection,true,queueSynchronized,readback);
}
// Image ownership may pass to the runtime after queue submission. CPU readers
// and application resources still require a verified completion before reuse.
class NativeXrCopyLifetime {
 bool early_{},submitted_{},completed_{};
public:
 explicit NativeXrCopyLifetime(bool early):early_(early){}
 void submitted(bool success){submitted_=success;}
 void completed(bool success){completed_=submitted_&&success;}
 bool canReleaseImages()const{return submitted_&&(early_||completed_);}
 bool canRetireResources()const{return submitted_&&completed_;}
};
}

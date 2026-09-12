#include "../src/openxr/NativeXrReleasePolicy.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <initializer_list>
using namespace kharvox;
int main(){
 // Runtime selection is independent of the six per-frame safety gates.
 // In particular VDXR is VirtualDesktop, not the Steam-backed VDXR4Steam.
 for(auto runtime:{OpenXRRuntimeKind::VirtualDesktop,OpenXRRuntimeKind::SteamVR,
     OpenXRRuntimeKind::VDXR4Steam,OpenXRRuntimeKind::MetaOculus,OpenXRRuntimeKind::Unknown}){
  const bool supported=runtime==OpenXRRuntimeKind::VirtualDesktop||runtime==OpenXRRuntimeKind::SteamVR||runtime==OpenXRRuntimeKind::VDXR4Steam;
  const bool steam=runtime==OpenXRRuntimeKind::SteamVR||runtime==OpenXRRuntimeKind::VDXR4Steam;
  for(unsigned bits=0;bits<64;++bits){
   const bool requested=bits&1,pair=bits&2,projection=bits&4,fence=bits&8,synchronized=bits&16,readback=bits&32;
   assert(nativeEarlyXrRelease(requested,runtime,pair,projection,fence,synchronized,readback)==(supported&&bits==31));
   // Steam preserves its earlier always-fenced behavior. VDXR only replaces
   // queueIdle on eligible test frames; opting out/readback/AER keep baseline.
   const bool expectedFence=steam||(runtime==OpenXRRuntimeKind::VirtualDesktop&&requested&&pair&&projection&&synchronized&&!readback);
   assert(nativeXrCopyFenceRequested(runtime,requested,pair,projection,synchronized,readback)==expectedFence);
  }
 }
 for(bool early:{false,true}){
  NativeXrCopyLifetime lifetime(early);
  assert(!lifetime.canReleaseImages()&&!lifetime.canRetireResources());
  // Even a spurious completion cannot authorize an unsubmitted image.
  lifetime.completed(true);
  assert(!lifetime.canReleaseImages()&&!lifetime.canRetireResources());
  lifetime.submitted(false);
  lifetime.completed(true);
  assert(!lifetime.canReleaseImages()&&!lifetime.canRetireResources());
  lifetime.submitted(true);
  assert(lifetime.canReleaseImages()==early);
  assert(!lifetime.canRetireResources());
  // Failed completion must retain CBs and framebuffers even when the runtime
  // has already accepted the released image.
  lifetime.completed(false);
  assert(!lifetime.canRetireResources());
  lifetime.completed(true);
  assert(lifetime.canReleaseImages()&&lifetime.canRetireResources());
 }
}

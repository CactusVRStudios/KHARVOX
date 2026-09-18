#include "../common/DiagnosticLogging.h"
#include "../common/PoseTrace.h"
#include "../weapon/WeaponHook.h"
#include "../weapon/AerDrawModel.h"
#include "../weapon/AerWeaponAttachments.h"
#include "NativeStereo.h"
#include "../sfs/NativeSfs.h"
#include "../hands/HandSceneDepthTracker.h"
#include "NativeHandSceneTarget.h"
#include <filesystem>
#include "NativeBindScratch.h"
#include "NativeOrderedWorkMap.h"
#include "NativeReplayPayload.h"
#include "NativeBindingMemo.h"
#include "NativeResourceSubset.h"
#include "NativeSourceLayoutState.h"
#include "NativeSpirvReadOnly.h"
#include "NativeCpuProfile.h"
#include "NativeFrameTrace.h"
#include "NativeMappedCopy.h"
#include "NativeCaptureTrigger.h"
#include "NativeLoadingDrain.h"
#include "NativeInlineStartup.h"
#include "NativeStartupControls.h"
#include "NativeStorageBindingSet.h"
#include "NativeResourceRetirement.h"
#include "NativeStorageMirrorBudget.h"
#include "NativeFramebufferRetirement.h"
#include "NativeQualityTransition.h"
#include "NativePairWatchPolicy.h"
#include "NativeDescriptorReuse.h"
#include "NativeDescriptorLifetime.h"
#include "NativeDescriptorArena.h"
#include "NativeShadowHistoryPolicy.h"
#include "NativeSharedShadowPolicy.h"
#include "NativeFreshShadowPolicy.h"
#include "NativeLightCullingPolicy.h"
#include "NativeGpuTimingPolicy.h"
#include <deque>
#include <span>
#include "NativeSnapshotMemoryPolicy.h"
#include "../common/RuntimePaths.h"
#include "../camera/CameraHook.h"
#include <fstream>
namespace kharvox::native {void recordDescriptorPoolAllocation(VkDescriptorPool,uint32_t,const VkDescriptorSet*);void forgetSourceDescriptors(uint32_t,const VkDescriptorSet*);void forgetDescriptorPool(VkDescriptorPool);}
namespace kharvox::native {VkResult allocateRedirectedDescriptor(VkDevice,VkDescriptorSetLayout,VkDescriptorSet*);}
namespace kharvox::native {void logDescriptorArenaStats();}
namespace kharvox::native {void waitForMirrorRetirement(VkDevice);}
namespace kharvox::native {void indexSourceBuffer(VkBuffer,bool);void unindexSourceBuffer(VkBuffer);}
namespace kharvox::native {static void finishShadowHistory();}
namespace kharvox::native {static void finishGpuTiming();}
namespace kharvox::native {static void reportImagePlans();}
namespace kharvox::native {static bool loadingDrainCommandClosed();}
namespace kharvox::native {void releaseStorageBindingBookkeeping();}
namespace kharvox::native {static uint32_t beginGpuSpan(VkCommandBuffer,const char*,uint32_t,uint32_t,uint32_t,uintptr_t);static void endGpuSpan(uint32_t);static void noteGpuCopy(uint32_t,uint64_t,uint64_t);static void noteGpuCopyWait(uint32_t,uint64_t);static void noteGpuImageSeed(uint32_t,VkImage,VkImage,VkImageView,uint32_t,uint32_t,const char*);}
namespace kharvox::native { bool prepareDoubleBoundary(VkCommandBuffer); bool isKnownEmptySet(VkDescriptorSet); std::string describeSetLayout(VkDescriptorSet); void observeStorageBindings(uint32_t,const VkDescriptorSet*); void seedFrameImages(VkCommandBuffer); void seedDepthTargets(VkCommandBuffer); void resetPassInputs(); }
namespace kharvox::native { void recordSourceImage(VkImage,const VkImageCreateInfo&); void forgetSourceImage(VkImage); }
namespace kharvox::native { void resetStageCapture(); void dumpStageCapture(const std::string&); }
namespace kharvox::native { static void dumpNativeFrozenInputs(const std::string&); }
namespace kharvox::native { void beginUniformSnapshots(); void finishUniformSnapshots(uint32_t); void retireUniformSnapshots(); void forgetUniformSnapshots(VkBuffer); void bindNativeDescriptors(VkCommandBuffer,VkPipelineBindPoint,VkPipelineLayout,uint32_t,uint32_t,const VkDescriptorSet*,const VkDescriptorSet*,uint32_t,const uint32_t*); }
namespace kharvox::native { PFN_vkVoidFunction instanceProc(const char*); bool beginRoot(); void endRoot(); void endFinal(); void seedBuffers(VkCommandBuffer); VkBuffer bufferForEye(VkBuffer); void seedStorageImage(VkCommandBuffer,VkImageView); void forgetBuffer(VkBuffer); void beforeDestroy(const char*,uintptr_t); void noteCommand(VkCommandBuffer); void checkFinalCommand(VkCommandBuffer,const char*); void recordLayout(VkDescriptorSetLayout,const VkDescriptorSetLayoutCreateInfo*); void validateLayout(VkDescriptorSetLayout); void validateSeedCommand(VkCommandBuffer); }
namespace kharvox::native {void freeNativeMemory(VkDevice,VkDeviceMemory,const VkAllocationCallbacks*,PFN_vkFreeMemory);}
namespace kharvox::native {void destroyNativeImage(VkDevice,VkImage,const VkAllocationCallbacks*,PFN_vkDestroyImage);}
#include "upstream/src/hooks/vulkan_hooks.cpp"
namespace kharvox::native {
using namespace kharvoxnative::vk_hooks;
static std::mutex qualityMutex;
static std::recursive_mutex qualityPresentationMutex;
void lockQualityPresentation(){qualityPresentationMutex.lock();}
void unlockQualityPresentation(){qualityPresentationMutex.unlock();}
static QualityTransition qualityTransition;
uint64_t qualityTransitionEpoch(){std::lock_guard lock(qualityMutex);return qualityTransition.epoch();}
void qualityResourceMutation(bool entering){std::lock_guard lock(qualityMutex);qualityTransition.mutation(entering);}
void qualityPresentCompleted(uint64_t epoch,bool success){
 std::lock_guard lock(qualityMutex);
 if(qualityTransition.presented(epoch,success))kharvoxnative::log::info("Native quality rebuild: 3 successful resource-stable game Presents completed; XR may resume with a fresh source");
}
#include "NativeCpuProfile.inc"
#include "NativeBindingMutation.inc"
#include "NativeForwardProfile.inc"
static PFN_vkGetInstanceProcAddr nativeInstanceProc{};
static PFN_vkGetDeviceProcAddr nativeRawDeviceProc{};
PFN_vkVoidFunction instanceProc(const char* name){auto&v=kharvoxnative::vulkan_state();return nativeInstanceProc?nativeInstanceProc(v.instance,name):nullptr;}
static std::atomic<Phase> state{Phase::Off};
static std::mutex poseMutex;
static FramePose nextPose{}, renderingPose{};
static uint64_t rootTrackingPose{};
static bool rootTrackingMatched{};
static float renderingBodyOrigin[3]{},renderingBodyAxis[9]{};
static float diagnosticPublishedCenter[12]{},diagnosticExpectedCenter[12]{};
static bool diagnosticPublishedValid{},diagnosticExpectedValid{};
static StereoFrame handedPair{};
static bool pairHanded{},xrProjectionAccepted{};
static std::vector<std::string> nativeFrameTrace;
static std::atomic<uint64_t> rootDone{}, finalDone{}, lastReadyFrame{};
static VkImage diagnosticFinalImage{};
static bool diagnosticPreparationCompleted{},diagnosticPreparationStable{};
static bool diagnosticCommandsClosed();
static std::atomic<DWORD> renderThread{};
static bool armed{};
static bool rootDeferred{};
static LoadingDrain loadingDrain;
static EmptyLoadingDrain emptyLoadingDrain;
static bool stereoWorkStarted{};
static bool preservedAttachmentInputs{};
#include "NativeDescriptorLifetime.inc"
#include "NativeDescriptorArena.inc"
static void(*nativeQueueLock)(){};
static void(*nativeQueueUnlock)(){};
void setQueueAccessCallbacks(void(*lock)(),void(*unlock)()){nativeQueueLock=lock;nativeQueueUnlock=unlock;}
static uint32_t warmFrames{};
static std::atomic_bool outstanding{};
static NativeRetirementQueue resourceRetirements;
static void physicallyRetireNativeResource(const NativeRetirementRequest& request){
 retireNativeResource(request,[](VkDeviceMemory memory){
 // Keep bindings and mapped pointers alive for CPU replay until retirement too.
 {std::unique_lock lock(swapchains_mutex);
  memory_mappings.erase(memory);
  for(auto it=buffer_bindings.begin();it!=buffer_bindings.end();){
   if(it->second.memory==memory)it=buffer_bindings.erase(it);else ++it;
  }
 }
 },[](VkImage image){forgetSourceImage(image);});
}
void freeNativeMemory(VkDevice device,VkDeviceMemory memory,const VkAllocationCallbacks* allocator,PFN_vkFreeMemory function){
 if(!memory){function(device,memory,allocator);return;}
 // Custom callback/user-data lifetimes cannot be extended by copying the struct.
 const bool deferrable=!allocator&&device==kharvoxnative::vulkan_state().device;
 const auto result=resourceRetirements.release(NativeMemoryFree{device,memory,allocator,function},deferrable,physicallyRetireNativeResource);
 if(result==NativeRetirementQueue::Result::Refused){
  beforeDestroy("vkFreeMemory (deferred queue refused)",reinterpret_cast<uintptr_t>(memory));
  // Fail closed even if completion raced the diagnostic's outstanding check.
  fail("native memory retirement queue refused a free");
 }
}
void destroyNativeImage(VkDevice device,VkImage image,const VkAllocationCallbacks* allocator,PFN_vkDestroyImage function){
 if(!image){function(device,image,allocator);return;}
 const bool deferrable=!allocator&&device==kharvoxnative::vulkan_state().device;
 const auto result=resourceRetirements.release(NativeImageDestroy{device,image,allocator,function},deferrable,physicallyRetireNativeResource);
 if(result==NativeRetirementQueue::Result::Refused){
  beforeDestroy("vkDestroyImage (deferred queue refused)",reinterpret_cast<uintptr_t>(image));
  fail("native resource retirement queue refused an image destruction");
 }
}
static std::mutex submissionMutex;
static std::set<VkCommandBuffer> recordedCommands,submittedCommands;
static std::set<VkImageView> realStorageViews;
static std::set<VkImage> storageSeeded;
bool requested(){static const bool value=kharvox::runtimeFileExists(L"enable_native_stereo_backend");return value;}
bool leftEyeOnlyDiagnostic(){return false;}
bool stereoPreparationEnabled(){static const bool value=eyeWorkPlan(leftEyeOnlyDiagnostic(),kharvox::runtimeFileExists(L"native_test_left_eye_stereo_prep")).prepareStereo;return value;}
bool mirrorPreparationEnabled(){static const bool value=eyeWorkPlan(leftEyeOnlyDiagnostic(),kharvox::runtimeFileExists(L"native_test_left_eye_stereo_prep"),kharvox::runtimeFileExists(L"native_test_left_eye_inputs_only")).prepareMirrors;return value;}
bool replayPreparationEnabled(){static const bool value=eyeWorkPlan(leftEyeOnlyDiagnostic(),kharvox::runtimeFileExists(L"native_test_left_eye_stereo_prep"),kharvox::runtimeFileExists(L"native_test_left_eye_inputs_only"),kharvox::runtimeFileExists(L"native_test_left_eye_no_replay_capture")).captureReplay;return value;}
bool inputPlanCaptureEnabled(){static const bool value=eyeWorkPlan(leftEyeOnlyDiagnostic(),kharvox::runtimeFileExists(L"native_test_left_eye_stereo_prep"),kharvox::runtimeFileExists(L"native_test_left_eye_inputs_only"),kharvox::runtimeFileExists(L"native_test_left_eye_no_replay_capture"),kharvox::runtimeFileExists(L"native_test_left_eye_no_input_plans")).captureInputPlans;return value;}
bool installed(){return state.load()!=Phase::Off;}
Phase phase(){return state.load();}
[[noreturn]] void fail(const char* why){
 state.store(Phase::RestartAer); frame_root_double_enabled=false;final_pass_double_armed=false;
 kharvoxnative::log::error(std::string("NATIVE STEREO RESTART AER REQUIRED: ")+why);
 {std::ofstream f(kharvox::runtimePathA("native_stereo_restart_aer.txt"));f<<why;}
 // A partially doubled command stream cannot safely become AER in-process.
 TerminateProcess(GetCurrentProcess(),0x4e535452);
 std::abort();
}
static void enterPhase(Phase next){auto current=state.load();do{if(!canTransition(current,next))fail("invalid native backend state transition");}while(!state.compare_exchange_weak(current,next));}
#include "NativeFreshShadows.inc"
#include "NativeLightCulling.inc"
#include "NativeInlineStartup.inc"
#include "NativeShadowCacheAudit.inc"
#include "NativeStartupControls.inc"
#include "NativeAerWorldProducer.inc"
static bool installEngine(bool visualOnly=false){
 auto base=reinterpret_cast<unsigned char*>(GetModuleHandleW(L"DOOMx64vk.exe"));if(!base)return false;
 auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);if(dos->e_magic!=IMAGE_DOS_SIGNATURE)return false;
 auto nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
 if(nt->Signature!=IMAGE_NT_SIGNATURE||nt->FileHeader.TimeDateStamp!=1711036533||nt->OptionalHeader.SizeOfImage!=336990208)return false;
 // The Native-only light visibility correction uses the engine's real CVar
 // setter. Validate its entire body and unwind identity before installing
 // any hook or changing any CVar. A mismatch retains the untouched AER path.
 DWORD64 cvarImage{};auto cvarEntry=RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(base+0x295780),&cvarImage,nullptr);
 if(!cvarEntry||cvarImage+cvarEntry->BeginAddress!=reinterpret_cast<DWORD64>(base+0x295780)||cvarEntry->EndAddress-cvarEntry->BeginAddress!=0x1bc||((base[cvarEntry->UnwindData]>>3)&UNW_FLAG_CHAININFO))return false;
 uint64_t cvarHash=14695981039346656037ull;for(size_t i=0;i<0x1bc;++i)cvarHash=(cvarHash^base[0x295780+i])*1099511628211ull;
 if(cvarHash!=0x25c4a7c50cfd75bcull)return false;
 struct Target {uintptr_t rva;uint64_t hash;void* hook;void** original;};
 std::vector<Target> targets;
  if(visualOnly){
   if(validateAerModelMatrix(base))
    targets.push_back({0x1ade630,0x53b78f0a1683547cull,reinterpret_cast<void*>(&hookAerModelMatrix),reinterpret_cast<void**>(&realAerModelMatrix)});
   else kharvoxnative::log::error("[AER-WEAPON-DRAW] fingerprint mismatch; draw correction unavailable");
  if(validateAerWorldProducer(base))
   targets.push_back({0x1830b80,0x409177e92a78d45eull,reinterpret_cast<void*>(&hookAerBuildViewMatrices),reinterpret_cast<void**>(&realAerBuildViewMatrices)});
  else kharvoxnative::log::error("[AER-WORLD-SOURCE] fingerprint mismatch; source observer unavailable, visual controls retained");
 }
 if(!visualOnly)targets={
#include "EngineProfile.inc"
 };
 { // Visual settings apply to both renderers; other controls remain Native-only.
  // Validate the entire bool setter, including its original permission checks,
  // not only an entry prefix. This hook joins the same transaction.
  freshShadowModule=reinterpret_cast<uintptr_t>(base);
  targets.push_back({0x295780,0xf2a2ed770e7ab47full,reinterpret_cast<void*>(&hookFreshShadowSet),reinterpret_cast<void**>(&realFreshShadowSet)});
 }
 for(const auto& t:targets){
  DWORD64 imageBase{};auto entry=RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(base+t.rva),&imageBase,nullptr);
  if(!entry||imageBase+entry->BeginAddress!=reinterpret_cast<DWORD64>(base+t.rva))return false;
  if((base[entry->UnwindData]>>3)&UNW_FLAG_CHAININFO)return false;
  uint64_t hash=14695981039346656037ull;for(int j=0;j<24;++j)hash=(hash^base[t.rva+j])*1099511628211ull;
  if(hash!=t.hash)return false;
 }
 auto result=MH_Initialize();if(result!=MH_OK&&result!=MH_ERROR_ALREADY_INITIALIZED)return false;
 std::vector<void*> created;
 for(const auto& t:targets){void* target=base+t.rva;if(MH_CreateHook(target,t.hook,t.original)!=MH_OK){for(void* p:created)MH_RemoveHook(p);return false;}created.push_back(target);}
 for(void* p:created)if(MH_QueueEnableHook(p)!=MH_OK)fail("engine hook activation could not be staged");
 if(MH_ApplyQueued()!=MH_OK){for(void* p:created){auto disabled=MH_DisableHook(p);if(disabled!=MH_OK&&disabled!=MH_ERROR_DISABLED)fail("engine hook rollback failed");if(MH_RemoveHook(p)!=MH_OK)fail("engine hook removal failed");}return false;}
 nativeCvarModule=reinterpret_cast<uintptr_t>(base);
 if(visualOnly&&realAerBuildViewMatrices)
  kharvoxnative::log::info("[AER-WORLD-SOURCE] r262 validated read-only producer observer ACTIVE; no camera writes or inline override");
 nativeCvarSet=realFreshShadowSet?realFreshShadowSet:reinterpret_cast<decltype(nativeCvarSet)>(base+0x295780);
 final_stage_hook_installed=false; // Completion is replayed at the Vulkan pass boundary.
 return true;
}
static PFN_vkGetDeviceProcAddr traceDeviceNext{};
static PFN_vkVoidFunction VKAPI_PTR traceDeviceProc(VkDevice device,const char* name){return trace::wrap(name,traceDeviceNext(device,name),true);}
void setDevice(VkInstance instance,VkPhysicalDevice physical,VkDevice device,PFN_vkGetDeviceProcAddr proc,PFN_vkGetInstanceProcAddr instanceProc){
 if(!requested()){
  // Before game queues: install the visual CVar setter and the guarded AER
  // matrix-producer hook. Native stereo replay/quality hooks stay disabled.
  static const bool visualReady=[](){
   if(!installEngine(true)){kharvoxnative::log::error("[RENDER-CVAR] AER setter profile/installation failed; visual settings preservation unavailable");return false;}
   const auto controls=rendererStartupControls(false,false,false,false,false,debugDisableAa(),kharvox::sfs::vrEnabled());
   const bool ok=initializeStartupControls(true,controls,[](const StartupControl& c,std::string& value){
    value=shadowAuditValue(c.rva,c.name);return value!="unavailable";
   },[](const StartupControl& c){return nativeCvarSet(reinterpret_cast<void*>(nativeCvarModule+c.rva),c.value,false);
   },[](const StartupControl& c,const std::string& before,const std::string& after){
    kharvoxnative::log::info(std::format("[RENDER-CVAR] renderer=AER startup name={} enforced={} before={} actual={} verified={} beforeGameQueue=true",c.name,c.value,before,after,after==c.value));
   });
   if(!ok)kharvoxnative::log::error("[RENDER-CVAR] AER startup visual settings readback failed");
   return ok;
  }();
  (void)visualReady;return;
 }
 auto& v=kharvoxnative::vulkan_state();v.instance=instance;v.physical_device=physical;v.device=device;
 trace::initialize();traceDeviceNext=proc;
 nativeRawDeviceProc=proc;g_get_device_proc_addr=trace::enabled()?traceDeviceProc:proc;nativeInstanceProc=instanceProc;
 // Initialize every trampoline before resource observation can start.
#define NATIVE_PROC(api,hook,original) original=reinterpret_cast<decltype(original)>(profileNativeForward(#api,trace::wrap(#api,proc(device,#api),true)));
#include "DispatchMap.inc"
#undef NATIVE_PROC
 real_queue_submit=reinterpret_cast<PFN_vkQueueSubmit>(proc(device,"vkQueueSubmit"));
 real_queue_wait_idle=reinterpret_cast<PFN_vkQueueWaitIdle>(trace::wrap("vkQueueWaitIdle",proc(device,"vkQueueWaitIdle"),true));
 real_device_wait_idle=reinterpret_cast<PFN_vkDeviceWaitIdle>(trace::wrap("vkDeviceWaitIdle",proc(device,"vkDeviceWaitIdle"),true));
 if(!installEngine()){kharvoxnative::log::error("Native engine profile mismatch or atomic hook installation failure: AER retained; no native hooks enabled");return;}
 initializeNativeInlineRenderer();
 initializeNativeRenderControls();
 enterPhase(Phase::Observing);
 kharvoxnative::log::info("Native Stereo r205 installed; experimental. KHARVOX owns tracking, weapons, input, HUD and OpenXR.");
}
void setQueue(VkQueue queue,uint32_t family,uint32_t index){if(!installed())return;auto&v=kharvoxnative::vulkan_state();if(v.queue)return;v.queue=queue;v.queue_family=family;v.queue_index=index;}
PFN_vkVoidFunction wrapRecorded(const char*,PFN_vkVoidFunction);
static void reportSnapshotBatches();
PFN_vkVoidFunction wrapProc(const char* name,PFN_vkVoidFunction next){
 if(!installed()||!next)return next;
 next=profileNativeForward(name,trace::wrap(name,next));
 if(auto recorded=wrapRecorded(name,next))return wrapBindingMutation(name,recorded);
#define NATIVE_PROC(api,hook,original) if(!strcmp(name,#api)){original=reinterpret_cast<decltype(original)>(next);return wrapBindingMutation(name,reinterpret_cast<PFN_vkVoidFunction>(&hook));}
#include "DispatchMap.inc"
#undef NATIVE_PROC
 return wrapBindingMutation(name,next);
}
void prepare(const FramePose& p){if(!installed())return;std::lock_guard<std::mutex> l(poseMutex);nextPose=p;}
static void traceResourceMetadata(){
 trace::Scope scope("resource-metadata-snapshot");
 const auto& v=kharvoxnative::vulkan_state();trace::row("owner-queue",scope.id,{reinterpret_cast<uintptr_t>(v.queue),v.queue_family,v.queue_index});
 {std::scoped_lock l(swapchains_mutex);
  for(const auto& [buffer,b]:buffers)trace::row("buffer-existing",scope.id,{reinterpret_cast<uintptr_t>(buffer),b.size,b.usage});
  for(const auto& [buffer,b]:buffer_bindings)trace::row("buffer-memory-existing",scope.id,{reinterpret_cast<uintptr_t>(buffer),reinterpret_cast<uintptr_t>(b.memory),b.offset});
  for(const auto& [memory,m]:memory_mappings)trace::row("mapping-existing",scope.id,{reinterpret_cast<uintptr_t>(memory),reinterpret_cast<uintptr_t>(m.host),m.offset,m.size});
 }
 {std::scoped_lock l(framebuffer_depth_view_mutex);
  for(const auto& [image,i]:created_image_infos)trace::row("image-existing",scope.id,{reinterpret_cast<uintptr_t>(image),i.extent.width,i.extent.height,i.extent.depth,uint64_t(i.format),i.usage,i.mips,i.layers});
  for(const auto& [view,v]:image_view_infos)trace::row("view-existing",scope.id,{reinterpret_cast<uintptr_t>(view),reinterpret_cast<uintptr_t>(v.image),uint64_t(v.format),v.range.aspectMask,v.range.baseMipLevel,v.range.levelCount,v.range.baseArrayLayer,v.range.layerCount});
  for(const auto& [fb,f]:framebuffer_infos){trace::row("framebuffer-existing",scope.id,{reinterpret_cast<uintptr_t>(fb),reinterpret_cast<uintptr_t>(f.render_pass),f.width,f.height,f.layers});for(size_t i=0;i<f.attachments.size();++i)trace::row("framebuffer-attachment",scope.id,{reinterpret_cast<uintptr_t>(fb),i,reinterpret_cast<uintptr_t>(f.attachments[i])});}
 }
}
bool beginRoot(){
 trace::poll();
 if(!installed()||phase()==Phase::RestartAer)return false;
 std::lock_guard<std::mutex> l(poseMutex);
 if(outstanding){fail("more than one FrameRoot before owner completion");return false;}
 if(loadingDrain.active()||emptyLoadingDrain.active())fail("loading drain survived owner completion");
 renderingPose=nextPose;rootTrackingPose=KharvoxCameraDiagnosticPoseId();rootTrackingMatched=false;rootDone=0;finalDone=0;rootDeferred=false;pairHanded=false;xrProjectionAccepted=false;stereoWorkStarted=false;
 diagnosticFinalImage=VK_NULL_HANDLE;diagnosticPreparationCompleted=false;diagnosticPreparationStable=false;
 KharvoxCameraGetBodyPose(renderingBodyOrigin,renderingBodyAxis);
 updateLightCulling(renderingPose.serial&&(renderingPose.gameplay||renderingPose.cinematic||renderingPose.scripted));
 auditShadowConfiguration(renderingPose.serial&&(renderingPose.gameplay||renderingPose.cinematic||renderingPose.scripted));
 {std::scoped_lock sl(submissionMutex);recordedCommands.clear();submittedCommands.clear();}
 if(qualityTransitionEpoch()||!renderingPose.serial||!(renderingPose.gameplay||renderingPose.cinematic||renderingPose.scripted)){frame_root_double_enabled=false;final_pass_double_armed=false;return false;}
 if(!resourceRetirements.begin([]{outstanding=true;}))fail("resource retirement survived owner completion");
 trace::beginFrame(renderingPose.serial);if(trace::consumeResourceSnapshot())traceResourceMetadata();cpu::begin(renderingPose.serial);
 if(cpu::detailedEnabled()||xrTargetCaptureEnabled()){
  diagnosticPublishedValid=KharvoxCameraGetHudCenterRenderPose(diagnosticPublishedCenter,diagnosticPublishedCenter+3);
  diagnosticExpectedValid=KharvoxCameraGetHeadRenderPose(diagnosticExpectedCenter,diagnosticExpectedCenter+3);
 }
 realStorageViews.clear();
 storageSeeded.clear();
 preservedAttachmentInputs=false;
 nativeFrameTrace.clear();
 resetPassInputs();
 resetStageCapture();
 beginUniformSnapshots();
 auto tid=GetCurrentThreadId();if(renderThread&&renderThread!=tid){fail("FrameRoot thread changed; restart required");return false;}renderThread=tid;
 uint8_t asynchronous=1;auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(L"DOOMx64vk.exe"));
 const bool inlineStateRead=safe_read_bytes(base+0x7061830,&asynchronous,1);
 if(!inlineStateRead||asynchronous){
  int32_t useSmp{};const bool cvarRead=safe_read_bytes(base+kUseSmpCvarRva+0x30,&useSmp,sizeof(useSmp));
  kharvoxnative::log::info(std::format("Native inline preflight refused stateRead={} asynchronousByte={} useSmpRead={} useSmpValue={} tid={} pid={}",inlineStateRead,uint32_t(asynchronous),cvarRead,useSmp,tid,GetCurrentProcessId()));
  fail("renderer not inline: launch requires r_useSMP=1");return false;
 }
 if(++warmFrames>600&&phase()!=Phase::Ready){fail("mirrors or final pass did not become ready within 600 rendered frames");return false;}
 if(!armed){g_barfix_on=true;g_rpfix_on=true;arm_frame_root_double(false);mirror_enabled=true;mirror_unfiltered=true;storage_mirror_enabled=true;frame_root_full_frame=true;frame_root_double_cap=UINT32_MAX;armed=true;enterPhase(Phase::Warming);}
 if(leftEyeOnlyDiagnostic()){
  mirror_enabled=mirrorPreparationEnabled();storage_mirror_enabled=mirrorPreparationEnabled();
  static bool reported{};if(!reported){reported=true;kharvoxnative::log::warn(replayPreparationEnabled()&&!inputPlanCaptureEnabled()?"Native LEFT + COMMAND CAPTURE DIAGNOSTIC: replay commands and immutable snapshots retained; image input plans disabled; right black; NOT full stereo":stereoPreparationEnabled()&&!replayPreparationEnabled()?"Native LEFT + SNAPSHOTS DIAGNOSTIC: left descriptor clones and immutable inputs retained; CPU replay capture/plans disabled; mirrors/right scene disabled; active pipeline and lifetime checks retained; NOT full stereo":stereoPreparationEnabled()&&!mirrorPreparationEnabled()?"Native LEFT + INPUT PREP DIAGNOSTIC: immutable inputs and descriptor/final command recording retained; mirror/holding/seed work disabled; right scene disabled; NOT full stereo":stereoPreparationEnabled()?"Native LEFT + STEREO PREP DIAGNOSTIC: left input snapshots, descriptor recording, holding copy and mirror seeds retained; right root and final replay disabled; right XR black; NOT full stereo":"Native LEFT-ONLY DIAGNOSTIC: one original scene root; right root, input snapshots, seeds, holding copy and final replay disabled; right XR clear black; NOT full stereo");}
 }
 static const bool testSetupWorld=kharvox::runtimeFileExists(L"native_test_setup_world");
 if(testSetupWorld){doubled_floor_level.store(0);static bool reported{};if(!reported){reported=true;kharvoxnative::log::warn("Native diagnostic: doubled SetupView_world enabled; isolated comparison, not an accepted fix");}}
 static const bool testZeroIpd=kharvox::runtimeFileExists(L"native_test_zero_ipd");
 static const bool compareZeroIpd=kharvox::runtimeFileExists(L"native_compare_zero_ipd");
 const bool zeroIpd=compareZeroIpd?kharvox::runtimeFileExists(L"native_test_zero_ipd"):testZeroIpd;
 if(zeroIpd||compareZeroIpd){
  g_ipd_override.store(zeroIpd?0.0f:-1.0f);frame_root_zero_offset.store(zeroIpd);
  // Describe the actual diagnostic centre cameras to XR; retaining physical
  // eye translations here would add an unrelated reprojection mismatch.
  if(zeroIpd)for(auto& view:renderingPose.views)view.pose=renderingPose.head;
  static int last=-1;if(last!=int(zeroIpd)){last=int(zeroIpd);kharvoxnative::log::warn(std::format("Native IPD comparison frame={} zeroSeparation={} twoProducerPasses=true xrPosesMatchDiagnosticCamera=true (ZERO is not stereo acceptance)",renderingPose.serial,zeroIpd));}
 }
 static const bool testPrivateCompute=kharvox::runtimeFileExists(L"native_test_private_compute");
 static const bool comparePrivateCompute=kharvox::runtimeFileExists(L"native_compare_private_compute");
 const bool privateCompute=comparePrivateCompute?kharvox::runtimeFileExists(L"native_test_private_compute"):testPrivateCompute;
 if(privateCompute||comparePrivateCompute){
  suppress_doubled_light_culling.store(!privateCompute);
  static int last=-1;if(last!=int(privateCompute)){last=int(privateCompute);kharvoxnative::log::warn(std::format("Native compute comparison frame={} privateDispatches={} twoProducerPasses=true (diagnostic, not an accepted lighting fix)",renderingPose.serial,privateCompute));}
 }
 const auto&a=renderingPose.views[0].pose.position;const auto&b=renderingPose.views[1].pose.position;
 frame_root_double_ipd=std::sqrt((a.x-b.x)*(a.x-b.x)+(a.y-b.y)*(a.y-b.y)+(a.z-b.z)*(a.z-b.z))*renderingPose.worldScale;
 frame_root_double_enabled=true;read_redirect_enabled=mirrorPreparationEnabled();
 final_pass_double_armed=!leftEyeOnlyDiagnostic()&&mirrors_converged();
 return true;
}
void endRoot(){if(rootDeferred){if(loadingDrain.active()&&!loadingDrain.rootReturned())fail("loading drain root return mismatch");if(emptyLoadingDrain.active()&&!emptyLoadingDrain.rootReturned())fail("empty loading drain root return mismatch");return;}if(!frame_root_double_enabled)fail("FrameRoot replay was disarmed by a recording failure");if(!leftEyeOnlyDiagnostic())finishUniformSnapshots(1);rootTrackingMatched=sourcePoseStable(renderingPose.source,rootTrackingPose,
    KharvoxCameraDiagnosticPoseId(),KharvoxCameraLevelTransitionGeneration());
 renderingPose.weaponKind=static_cast<uint32_t>(KharvoxWeaponCurrentKind());
 if(!rootTrackingMatched){
    static uint64_t rejected{};
    if(++rejected<=4||rejected%120==0)kharvoxnative::log::info(std::format(
        "[NATIVE-SOURCE] pair withheld preparedPose={} rootPose={} endPose={} sourceLevel={} currentLevel={} count={}",
        renderingPose.source.poseId,rootTrackingPose,KharvoxCameraDiagnosticPoseId(),
        renderingPose.source.level,KharvoxCameraLevelTransitionGeneration(),rejected));
 }
 rootDone=renderingPose.serial;}
void endFinal(){if(leftEyeOnlyDiagnostic()?(diagnosticFinalImage&&rootDone!=0):canCompleteFrame(final_pass_mirror_color!=VK_NULL_HANDLE,rootDone!=0,mirrors_converged(),preservedAttachmentInputs)){finalDone=rootDone.load();lastReadyFrame=finalDone.load();warmFrames=0;enterPhase(Phase::Ready);}}
bool pair(VkImage left,VkExtent2D extent,VkFormat format,StereoFrame& out){
 if(phase()!=Phase::Ready||finalDone==0||finalDone!=renderingPose.serial||rootDone!=finalDone||!rootTrackingMatched)return false;

 std::lock_guard<std::mutex> poseLock(poseMutex);
 if(leftEyeOnlyDiagnostic()){
  if(stereoPreparationEnabled()&&!diagnosticPreparationCompleted)fail("left preparation diagnostic published without its preparation boundary");
  if(frame_root_double_attempts.load()!=0||final_pass_doubles.load()!=0)fail("right render work executed in left-only diagnostic");
  const bool closed=diagnosticCommandsClosed();
  bool submittedAll{};{std::scoped_lock sl(submissionMutex);submittedAll=!recordedCommands.empty();for(auto cb:recordedCommands)submittedAll=submittedAll&&submittedCommands.count(cb)!=0;}
  if(!validLeftDiagnostic(true,reinterpret_cast<uintptr_t>(left),reinterpret_cast<uintptr_t>(diagnosticFinalImage),renderingPose.serial,rootDone.load(),finalDone.load(),submittedAll,closed))fail("left diagnostic producer identity/recording/submission mismatch");
  if(!extent.width||!extent.height||format==VK_FORMAT_UNDEFINED)fail("left diagnostic source extent/format invalid");
  out={};out.pose=renderingPose;out.generation=mirror_generation;out.producerQueue=kharvoxnative::vulkan_state().queue;out.rightEyeBlackDiagnostic=true;
  out.eyes[0]={left,extent,format,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,renderingPose.views[0].pose,renderingPose.views[0].fov,0,renderingPose.serial};
  out.eyes[1]={VK_NULL_HANDLE,extent,format,VK_IMAGE_LAYOUT_UNDEFINED,renderingPose.views[1].pose,renderingPose.views[1].fov,1,renderingPose.serial};
  finalDone=0;pairHanded=true;handedPair=out;return true;
 }
 std::lock_guard<std::mutex> lock(mirror_mutex);
 if(!mirrors_converged()||!canReadMirror(final_pass_mirror_color!=VK_NULL_HANDLE,mirror_images_written.count(final_pass_mirror_color)!=0,mirror_layout_of(final_pass_mirror_color)!=VK_IMAGE_LAYOUT_UNDEFINED)||left==final_pass_mirror_color)return false;
 {std::scoped_lock sl(submissionMutex);if(recordedCommands.empty())fail("native pair has no recorded commands");for(auto cb:recordedCommands)if(!submittedCommands.count(cb))fail("native pair producer commands were not submitted");}
 out.pose=renderingPose;out.generation=mirror_generation;out.producerQueue=kharvoxnative::vulkan_state().queue;
 out.eyes[0]={left,extent,format,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,renderingPose.views[0].pose,renderingPose.views[0].fov,0,renderingPose.serial};
 out.eyes[1]={final_pass_mirror_color,final_pass_mirror_extent,final_pass_mirror_format,mirror_layout_of(final_pass_mirror_color),renderingPose.views[1].pose,renderingPose.views[1].fov,1,renderingPose.serial};
 const PairIdentity identity{uint64_t(reinterpret_cast<uintptr_t>(left)),uint64_t(reinterpret_cast<uintptr_t>(final_pass_mirror_color)),renderingPose.serial,renderingPose.serial,rootDone.load(),out.generation,mirror_generation.load(),0,1,true,true,out.eyes[1].layout!=VK_IMAGE_LAYOUT_UNDEFINED};
 finalDone=0;pairHanded=validPair(identity)&&out.eyes[1].extent.width==extent.width&&out.eyes[1].extent.height==extent.height&&out.eyes[1].format==format;
 if(pairHanded)handedPair=out;return pairHanded;
}
bool handSceneTarget(const StereoFrame& frame,const hands::HandSceneTarget& left,hands::HandSceneTarget& right){
 right={};
 // Mirror resources deliberately bypass the game tracker. Resolve the exact
 // source attachment views through the owner's maps, never by size or format.
 // The caller leases the source views as well as these borrowed mirrors until
 // the Present GPU work completes, so source retirement cannot remove them.
 std::lock_guard lock(mirror_mutex);
 if(!pairHanded||frame.pose.serial!=handedPair.pose.serial||frame.generation!=mirror_generation.load()
    ||left.colorImage!=frame.eyes[0].image||frame.rightEyeBlackDiagnostic)return false;
 const bool resolved=resolveHandSceneMirror(left,frame.eyes[1].image,
     mirrored_views,mirror_image_layouts,mirror_images_written,right);
 static uint64_t checks{};
 if(++checks<=4||checks%300==0)kharvoxnative::log::info(std::format(
     "Native hand depth frame={} resolved={} leftColor={} leftDepth={} rightColor={} rightDepth={} reverseZ={} layout={}",
     frame.pose.serial,resolved,reinterpret_cast<void*>(left.colorImage),reinterpret_cast<void*>(left.depthImage),
     reinterpret_cast<void*>(right.colorImage),reinterpret_cast<void*>(right.depthImage),left.reverseDepth,int(right.depthLayout)));
 return resolved;
}
void xrPresented(uint64_t serial,XrResult result,bool projection){
 if(!pairHanded||serial!=handedPair.pose.serial)fail("XR result does not belong to the handed native pair");
 xrProjectionAccepted=projection&&result==XR_SUCCESS;
 static uint64_t count{};if(++count<=4||count%300==0||result!=XR_SUCCESS)kharvoxnative::log::info(std::format("Native XR submit frame={} projection={} views=2 leftOnly={} stereoPrep={} mirrorPrep={} replayCapture={} inputPlans={} prepStable={} rightRootCalls={} finalReplayCalls={} xrEndFrame={} leftImage={} rightImage={}",serial,projection,handedPair.rightEyeBlackDiagnostic,leftEyeOnlyDiagnostic()&&stereoPreparationEnabled(),mirrorPreparationEnabled(),replayPreparationEnabled(),inputPlanCaptureEnabled(),diagnosticPreparationStable,frame_root_double_attempts.load(),final_pass_doubles.load(),int(result),reinterpret_cast<void*>(handedPair.eyes[0].image),reinterpret_cast<void*>(handedPair.eyes[1].image)));
}
#include "NativeReadback.inc"
#include "NativeXrReadback.inc"
void completed(){
 if(!installed()||!outstanding)return;
 const bool emptyDraining=emptyLoadingDrain.active();
 bool noNativeWork{};
 {std::scoped_lock sl(submissionMutex);
  for(auto cb:recordedCommands)if(!submittedCommands.count(cb))fail("native command buffer not submitted at owner Present");
  if(emptyDraining)noNativeWork=recordedCommands.empty()&&submittedCommands.empty()&&!stereoWorkStarted&&rootDone.load()==0&&finalDone.load()==0;
 }
 const bool draining=loadingDrain.active();
 const bool drainedCommandClosed=draining&&loadingDrainCommandClosed();
 const bool recordingOutstanding=emptyDraining?!emptyLoadingDrain.recordingFinished(noNativeWork):draining?!loadingDrain.recordingFinished(drainedCommandClosed):rootDone.load()!=renderingPose.serial;
 if(recordingOutstanding)fail("native recording still open at owner Present");
 // Call only at the owner Present after a successful copy fence. Conservative
 // whole-device completion also covers engine submissions on other queues.
 auto&v=kharvoxnative::vulkan_state();{cpu::Scope profile(cpu::DeviceIdle);if(!real_device_wait_idle||real_device_wait_idle(v.device)!=VK_SUCCESS){fail("GPU completion failed");return;}}
 if(!canRetire(true,true,recordingOutstanding))fail("descriptor retirement before recording finished");
 if(lastReadyFrame!=renderingPose.serial&&phase()==Phase::Ready)enterPhase(Phase::Warming);
 {std::scoped_lock lock(read_redirect_mutex);
 freeRetiredRedirectedDescriptors();
 if(cpu::detailedEnabled()){static uint32_t counts{};if(++counts<=4||counts%60==0){std::scoped_lock dl(descriptorLifetimeMutex);kharvoxnative::log::info(std::format("Native descriptor lifetime frame={} liveClones={} retired={} trackedSources={} forgottenSources={} completionVerified=true",renderingPose.serial,redirected_sets.size(),retired_clones.size(),descriptorPoolLedger.size(),forgottenSourceDescriptors));}}
 if(phase()==Phase::Ready&&pairHanded&&xrProjectionAccepted){if(!leftEyeOnlyDiagnostic())captureNativePair(handedPair);static uint64_t count{};if(++count<=4||count%300==0){kharvoxnative::log::info(std::format("KHARVOX native frame={} generation={} submitted command buffers={} leftOnly={} GPU completion and XR projection verified; headset unvalidated",renderingPose.serial,mirror_generation.load(),submittedCommands.size(),leftEyeOnlyDiagnostic()));std::ofstream(kharvox::runtimePathA("renderer_status.txt"))<<(leftEyeOnlyDiagnostic()?(stereoPreparationEnabled()?(replayPreparationEnabled()&&!inputPlanCaptureEnabled()?"Renderer: LEFT + COMMAND CAPTURE DIAGNOSTIC; no image input plans; right black; NOT full stereo":!replayPreparationEnabled()?"Renderer: LEFT + SNAPSHOTS DIAGNOSTIC; no CPU replay capture; right black":!mirrorPreparationEnabled()?"Renderer: LEFT + INPUT PREP DIAGNOSTIC; right black; NOT full stereo":diagnosticPreparationStable?"Renderer: LEFT + STEREO PREP DIAGNOSTIC; right black; NOT full stereo":"Renderer: LEFT + STEREO PREP warming; right black; NOT full stereo"):"Renderer: LEFT-ONLY DIAGNOSTIC; left scene + black right XR; NOT full stereo"):"Renderer: Native Stereo EXP GPU pair + XR projection; headset unvalidated");}}
 finishGpuTiming();reportImagePlans();reportSnapshotBatches();
 finishShadowHistory();
 retireUniformSnapshots();
 if(draining){
  if(!loadingDrain.complete(drainedCommandClosed,true,true))fail("loading drain completion mismatch");
  kharvoxnative::log::info(std::format("Native loading drain completed frame={} originalCommandsSubmitted=true deviceCompletionVerified=true stereoPairPublished=false",renderingPose.serial));
 }
 if(emptyDraining){
  if(!emptyLoadingDrain.complete(noNativeWork,true))fail("empty loading drain completion mismatch");
  kharvoxnative::log::info(std::format("Native empty loading drain completed frame={} originalRootReturned=true noInjectedCommands=true deviceCompletionVerified=true stereoPairPublished=false",renderingPose.serial));
 }
 }
 // Image metadata retirement acquires replayMutex. Do not hold the descriptor
 // bookkeeping lock across this dispatch (replay can take those in reverse).
 uint32_t memories{},images{};
 const auto freed=resourceRetirements.complete([&](const NativeRetirementRequest& request){
  if(std::holds_alternative<NativeMemoryFree>(request))++memories;else ++images;
  physicallyRetireNativeResource(request);
 },[]{outstanding=false;});
 if(freed)kharvoxnative::log::info(std::format("Native deferred resources retired frame={} count={} memory={} images={} cpuReplayRetired=true deviceCompletionVerified=true",renderingPose.serial,freed,memories,images));
 final_pass_double_armed=false;
 presented_frame.fetch_add(1);
 cpu::finish(phase()==Phase::Ready);
 trace::endFrame(renderingPose.serial,phase()==Phase::Ready);
}
}

namespace kharvox::native {
struct BufferMirror { VkBuffer image{}; VkDeviceMemory memory{}; VkDeviceSize size{}; };
static std::map<VkBuffer,BufferMirror> storageBuffers;
static std::mutex bufferMutex;
// Protected by the donor metadata's swapchains_mutex, including lifecycle hooks.
static ResourceSubset<VkBuffer> storageSourceIndex;
void indexSourceBuffer(VkBuffer buffer,bool storage){storageSourceIndex.record(buffer,storage);}
void unindexSourceBuffer(VkBuffer buffer){storageSourceIndex.erase(buffer);}
void seedBuffers(VkCommandBuffer command){
 cpu::Scope profile(cpu::StorageSeed);
 if(!command){fail("no recording command buffer at FrameRoot boundary");return;}
 validateSeedCommand(command);
 {std::scoped_lock l(submissionMutex);if(submittedCommands.count(command))fail("storage seed after command submission");recordedCommands.insert(command);}
 auto&v=kharvoxnative::vulkan_state();
 std::vector<std::pair<VkBuffer,BufferInfo>> inputs;
 {cpu::Scope selectionProfile(cpu::StorageSelection);
  std::scoped_lock lock(swapchains_mutex);
  inputs.reserve(storageSourceIndex.selected().size());
  if(cpu::current.active){cpu::current.trackedSourceBuffers=buffers.size();cpu::current.indexedStorageBuffers=storageSourceIndex.selected().size();}
  for(auto source:storageSourceIndex.selected()){
   const auto found=buffers.find(source);
   if(found==buffers.end()||!(found->second.usage&VK_BUFFER_USAGE_STORAGE_BUFFER_BIT))fail("storage source index disagrees with live buffer metadata");
   inputs.push_back(*found);
  }
 }
 // Existing mirrors only need their per-frame copy. Resolve allocation
 // entry points and query memory types once, and only if this frame creates
 // a mirror. Keep this context local so device changes cannot reuse it.
 PFN_vkGetBufferMemoryRequirements getReq{};
 PFN_vkAllocateMemory allocate{};
 PFN_vkBindBufferMemory bind{};
 VkPhysicalDeviceMemoryProperties properties{};
 VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
 const auto seedTiming=beginGpuSpan(command,"storage-buffer-seed",1,0,0,0);
 real_cmd_pipeline_barrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
 std::scoped_lock lock(bufferMutex);
 for(auto&[source,info]:inputs){
  auto& target=storageBuffers[source];
  if(!target.image){
   if(!getReq){
    getReq=load_device_proc<PFN_vkGetBufferMemoryRequirements>(v.device,"vkGetBufferMemoryRequirements");
    allocate=load_device_proc<PFN_vkAllocateMemory>(v.device,"vkAllocateMemory");
    bind=load_device_proc<PFN_vkBindBufferMemory>(v.device,"vkBindBufferMemory");
    auto getMem=reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(kharvox::native::instanceProc("vkGetPhysicalDeviceMemoryProperties"));
    if(!getReq||!allocate||!bind||!getMem)fail("storage mirror dispatch unavailable");
    getMem(v.physical_device,&properties);
   }
   VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};ci.size=info.size;ci.usage=info.usage|VK_BUFFER_USAGE_TRANSFER_DST_BIT;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
   if(real_create_buffer(v.device,&ci,nullptr,&target.image)!=VK_SUCCESS){fail("storage buffer mirror creation failed");return;}
   VkMemoryRequirements req{};getReq(v.device,target.image,&req);uint32_t type=UINT32_MAX;
   for(uint32_t i=0;i<properties.memoryTypeCount;i++)if(req.memoryTypeBits&(1u<<i)){type=i;if(properties.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)break;}
   if(type==UINT32_MAX){fail("storage mirror has no memory type");return;}
   VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=type;
   if(allocate(v.device,&ai,nullptr,&target.memory)!=VK_SUCCESS||bind(v.device,target.image,target.memory,0)!=VK_SUCCESS){fail("storage mirror allocation failed");return;}
   target.size=info.size;mirror_generation.fetch_add(1);
  }
  if(target.size!=info.size){fail("storage buffer handle generation mismatch");return;}
  if(cpu::current.active){cpu::current.seededBytes+=info.size;++cpu::current.seededBuffers;}
  VkBufferCopy copy{0,0,info.size};real_cmd_copy_buffer(command,source,target.image,1,&copy);
 }
 barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
 real_cmd_pipeline_barrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&barrier,0,nullptr,0,nullptr);
 endGpuSpan(seedTiming);
}
VkBuffer bufferForEye(VkBuffer source){
 if(!inside_frame_root_double&&!inside_final_pass_double)return source;
 std::scoped_lock lock(bufferMutex);auto found=storageBuffers.find(source);return found==storageBuffers.end()?source:found->second.image;
}
void forgetBuffer(VkBuffer source){
 forgetUniformSnapshots(source);
 // Destruction while recording would invalidate an already recorded clone.
 // The owner requires a restart rather than allowing such a lifetime overlap.
 std::scoped_lock lock(bufferMutex);
 if(outstanding)fail("storage buffer retired while native frame is recording");
 auto it=storageBuffers.find(source);if(it!=storageBuffers.end()){
 auto& v=kharvoxnative::vulkan_state();real_destroy_buffer(v.device,it->second.image,nullptr);
 auto freeMemory=load_device_proc<PFN_vkFreeMemory>(v.device,"vkFreeMemory");freeMemory(v.device,it->second.memory,nullptr);
 storageBuffers.erase(it);mirror_generation.fetch_add(1);
 }
}
static void seedMirrorImage(VkCommandBuffer command,VkImageView sourceView,bool storage,const char* reason){
 // Called under mirror_mutex and outside a render pass. Storage images may
 // READ before writing. Initialize every mip/layer from the original image.
 auto mv=mirrored_views.find(sourceView);if(mv==mirrored_views.end()){fail("storage image mirror unavailable");return;}
 if(storageSeeded.count(mv->second.image))return;
 ImageViewInfo view{};CreatedImageInfo info{};
 {std::scoped_lock lock(framebuffer_depth_view_mutex);auto vi=image_view_infos.find(sourceView);if(vi==image_view_infos.end()){fail("untracked storage image view");return;}view=vi->second;auto im=created_image_infos.find(view.image);if(im==created_image_infos.end()){fail("untracked storage image");return;}info=im->second;}
 if(info.mips!=1||info.layers!=1)fail("readable storage image subresource layout coverage unsupported");
 auto sourceLayout=doom_image_layouts.find(view.image);
 if(sourceLayout==doom_image_layouts.end()||sourceLayout->second==VK_IMAGE_LAYOUT_UNDEFINED){fail("storage source layout unknown");return;}
 const auto oldSource=sourceLayout->second,oldMirror=mirror_layout_of(mv->second.image);
 VkImageAspectFlags aspects=VK_IMAGE_ASPECT_COLOR_BIT;
 if(info.format>=VK_FORMAT_D16_UNORM&&info.format<=VK_FORMAT_D32_SFLOAT_S8_UINT){
  aspects=info.format==VK_FORMAT_S8_UINT?0:VK_IMAGE_ASPECT_DEPTH_BIT;
  if(info.format>=VK_FORMAT_S8_UINT)aspects|=VK_IMAGE_ASPECT_STENCIL_BIT;
 }
 const auto targetLayout=storage?VK_IMAGE_LAYOUT_GENERAL:oldSource;
 const auto imageSeedTiming=beginGpuSpan(command,"mirror-image-seed",1,info.extent.width,info.extent.height,0);
 noteGpuImageSeed(imageSeedTiming,view.image,mv->second.image,sourceView,uint32_t(info.format),info.usage,reason);
 VkImageMemoryBarrier bars[2]{};
 for(auto&b:bars){b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;b.subresourceRange={aspects,0,info.mips,0,info.layers};}
 bars[0].image=view.image;bars[0].oldLayout=oldSource;bars[0].newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;bars[0].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
 bars[1].image=mv->second.image;bars[1].oldLayout=oldMirror;bars[1].newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;bars[1].dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
 real_cmd_pipeline_barrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,2,bars);
 std::vector<VkImageCopy> copies;
 for(uint32_t mip=0;mip<info.mips;mip++)for(auto aspect:{VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_ASPECT_DEPTH_BIT,VK_IMAGE_ASPECT_STENCIL_BIT})if(aspects&aspect){VkImageCopy c{};c.srcSubresource={VkImageAspectFlags(aspect),mip,0,info.layers};c.dstSubresource=c.srcSubresource;c.extent={std::max(1u,info.extent.width>>mip),std::max(1u,info.extent.height>>mip),std::max(1u,info.extent.depth>>mip)};copies.push_back(c);}
 real_cmd_copy_image(command,view.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,mv->second.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,uint32_t(copies.size()),copies.data());
 bars[0].oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;bars[0].newLayout=oldSource;bars[0].srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;bars[0].dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
 bars[1].oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;bars[1].newLayout=targetLayout;bars[1].srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;bars[1].dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
 real_cmd_pipeline_barrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,2,bars);
 set_mirror_range_layout(mv->second.image,bars[1].subresourceRange,targetLayout);mirror_images_written.insert(mv->second.image);storageSeeded.insert(mv->second.image);
 endGpuSpan(imageSeedTiming);
}
void seedStorageImage(VkCommandBuffer command,VkImageView sourceView){seedMirrorImage(command,sourceView,true,"seed-storage-image");}
void observeStorageBindings(uint32_t count,const VkDescriptorSet* sets){
 if(!mirrorPreparationEnabled()||!inside_real_frame_root||!sets)return;
 std::scoped_lock lock(swapchains_mutex);
 for(uint32_t i=0;i<count;i++){
  auto it=descriptor_image_sets.find(sets[i]);if(it==descriptor_image_sets.end())continue;
  for(auto&[binding,record]:it->second)if(record.type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE&&record.image_view)realStorageViews.insert(record.image_view);
 }
}
void seedFrameImages(VkCommandBuffer command){
 validateSeedCommand(command);
 seedDepthTargets(command);
 std::scoped_lock lock(mirror_mutex);
 for(auto view:realStorageViews){
  if(!ensure_mirror_storage_image(view))fail("cannot create storage image observed by the real pass");
  seedStorageImage(command,view);
 }
 static uint64_t calls{};
 if(++calls<=4)kharvoxnative::log::info(std::format("Native preseed frame={} storageViews={} outsideRenderPass=true",renderingPose.serial,realStorageViews.size()));
}
}

namespace kharvox::native {
#include "NativeResourceGuard.inc"
// Outside bookkeeping locks, only for destruction of Native mirrors. Menus
// have no outstanding stereo root and therefore no completed() device drain.
void waitForMirrorRetirement(VkDevice device){
 beforeDestroy("mirror retirement",reinterpret_cast<uintptr_t>(device));
 if(nativeQueueLock)nativeQueueLock();
 const auto result=real_device_wait_idle?real_device_wait_idle(device):VK_ERROR_INITIALIZATION_FAILED;
 if(nativeQueueUnlock)nativeQueueUnlock();
 if(result!=VK_SUCCESS)fail("GPU completion failed before mirror resource retirement");
}
void beginQualityChange(){
 auto& v=kharvoxnative::vulkan_state();
 if(!installed()||!v.queue)return;
 std::lock_guard presentationLock(qualityPresentationMutex);
 bool first{};{std::lock_guard lock(qualityMutex);first=qualityTransition.begin();}
 if(!first)return;
 // Arm before the engine setter can trigger any allocation/rebuild. This is
 // earlier than view destruction: r215 loses the device before that hook.
 waitForMirrorRetirement(v.device);
 prepare(FramePose{});
 kharvoxnative::log::info("Native quality rebuild: device drained before preset write; XR copies suspended until resource-stable game Presents");
}
}

namespace kharvox::native {void presentStarting(){if(!installed())return;std::lock_guard<std::mutex> l(poseMutex);nextPose.gameplay=false;nextPose.cinematic=false;nextPose.scripted=false;nextPose.source.domain=SceneDomain::Inactive;}}

#include "NativeFinalReplay.inc"
namespace kharvox::native { static bool diagnosticCommandsClosed(){std::scoped_lock lock(replayMutex,submissionMutex);for(auto cb:recordedCommands){auto it=replayStates.find(cb);if(it==replayStates.end()||it->second.recording||it->second.inRenderPass)return false;}return true;} }

namespace kharvox::native {
#include "NativePairWatch.inc"
void beforeDeviceDestroy(VkDevice device){if(!installed()||device!=kharvoxnative::vulkan_state().device)return;beforeDestroy("vkDestroyDevice",reinterpret_cast<uintptr_t>(device));cpu::flushPacingSequence();releaseStorageBindingBookkeeping();updateLightCulling(false);releaseGpuInputCopies(device);if(xrReadback.pending)fail("XR target readback outstanding at device destruction");releasePairWatch();releaseGpuTiming(device);destroyRedirectedArena(device);{std::scoped_lock lock(swapchains_mutex);storageSourceIndex.clear();}}
void noteCommand(VkCommandBuffer cb){if(!inside_frame_root_double&&!inside_final_pass_double)return;if(GetCurrentThreadId()!=renderThread)fail("native render commands escaped the inline render thread");stereoWorkStarted=true;std::scoped_lock l(submissionMutex);if(submittedCommands.count(cb))fail("native command buffer rewritten after submission");recordedCommands.insert(cb);}
void submitted(VkQueue queue,uint32_t count,const VkSubmitInfo* infos,VkResult result){
 if(!installed())return;
 if(result!=VK_SUCCESS){
  const auto report=std::format("Native process engine submit failed: VkResult={} outstanding={} phase={} qualityEpoch={} nativeFrame={} queue={} batches={} pid={}\n",
   int(result),outstanding.load(),int(phase()),qualityTransitionEpoch(),renderingPose.serial,reinterpret_cast<void*>(queue),count,GetCurrentProcessId());
  const auto name=std::format("native_submit_failure-{}.log",GetCurrentProcessId());
  {std::ofstream file(kharvox::logPathA(name.c_str()));file<<report;}
  kharvoxnative::log::error(report);
  fail("engine queue submission failed; see native_submit_failure and native_validation logs");
 }
 // Reused engine command buffers in menus are not outstanding Native work.
 if(!outstanding)return;
 std::scoped_lock l(submissionMutex);
 for(uint32_t i=0;i<count;i++)for(uint32_t j=0;j<infos[i].commandBufferCount;j++){
  auto cb=infos[i].pCommandBuffers[j];if(recordedCommands.count(cb)){
   if(queue!=kharvoxnative::vulkan_state().queue)fail("native commands submitted on an unexpected queue");
   submittedCommands.insert(cb);
  }
 }
}
void submitted2(VkQueue queue,uint32_t count,const VkSubmitInfo2* infos,VkResult result){if(!installed())return;for(uint32_t i=0;i<count;i++)for(uint32_t j=0;j<infos[i].commandBufferInfoCount;j++){auto cb=infos[i].pCommandBufferInfos[j].commandBuffer;VkSubmitInfo one{VK_STRUCTURE_TYPE_SUBMIT_INFO};one.commandBufferCount=1;one.pCommandBuffers=&cb;submitted(queue,1,&one,result);}}
}

namespace kharvox::native {
static std::mutex layoutMutex;
static std::map<VkDescriptorSetLayout,bool> completeLayouts;
static std::map<VkDescriptorSetLayout,std::string> layoutDescriptions;
static std::set<VkDescriptorSetLayout> emptyLayouts;
void recordLayout(VkDescriptorSetLayout layout,const VkDescriptorSetLayoutCreateInfo* info){
 bool complete=info&&!info->pNext&&info->flags==0;
 bool empty=complete;
 std::string description;
 if(info)for(uint32_t i=0;i<info->bindingCount;i++){
  const auto& b=info->pBindings[i];if(b.descriptorCount>1)complete=false;
  if(b.descriptorCount)empty=false;
  description+=std::format(" b{}:type{}x{} immutable={}",b.binding,int(b.descriptorType),b.descriptorCount,b.pImmutableSamplers!=nullptr);
 }
 std::scoped_lock lock(layoutMutex);completeLayouts[layout]=complete;
 emptyLayouts.erase(layout);if(empty)emptyLayouts.insert(layout);
 layoutDescriptions[layout]=description.empty()?"empty":description;
}
void validateLayout(VkDescriptorSetLayout layout){std::scoped_lock lock(layoutMutex);auto it=completeLayouts.find(layout);if(it==completeLayouts.end()||!it->second)fail("descriptor layout contains arrays/extensions or was not captured");}
bool isKnownEmptySet(VkDescriptorSet set){
 VkDescriptorSetLayout layout{};{std::scoped_lock l(read_redirect_mutex);auto i=descriptor_set_layouts.find(set);if(i==descriptor_set_layouts.end())return false;layout=i->second;}
 std::scoped_lock l(layoutMutex);return emptyLayouts.count(layout)!=0;
}
std::string describeSetLayout(VkDescriptorSet set){
 VkDescriptorSetLayout layout{};{std::scoped_lock l(read_redirect_mutex);auto i=descriptor_set_layouts.find(set);if(i==descriptor_set_layouts.end())return "allocation not captured";layout=i->second;}
 std::scoped_lock l(layoutMutex);auto i=layoutDescriptions.find(layout);return i==layoutDescriptions.end()?"layout not captured":i->second;
}
}

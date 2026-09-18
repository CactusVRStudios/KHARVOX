#pragma once
#include "NativeStereoPolicy.h"
#include "NativeFrameSource.h"
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <array>
#include <cstdint>
namespace kharvox::hands { struct HandSceneTarget; }
namespace kharvox::native {
struct FramePose { FrameSourceIdentity source{}; uint32_t weaponKind{}; bool leftHanded{},twoHanded{},weaponControlActive{}; uint64_t serial{}; XrTime displayTime{}; XrPosef head{}; XrPosef bodyTracking{}; std::array<XrPosef,2> controllers{}; std::array<XrView,2> views{}; std::array<XrFovf,2> submitFov{}; bool exactProjectionCrop{}; std::array<bool,2> controllersValid{}; float worldScale{39.37f}; bool gameplay{}; bool cinematic{}; bool scripted{}; bool viewSpace{}; };
struct EyeImage { VkImage image{}; VkExtent2D extent{}; VkFormat format{}; VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED}; XrPosef pose{}; XrFovf fov{}; uint32_t eye{}; uint64_t frame{}; };
// Borrowed until the next owner Present. Both images are GPU writes from the
// same FrameRoot, except the explicit left-only diagnostic (right is null).
// Present waits are owned/consumed once by KHARVOX's copy submit.
struct StereoFrame { std::array<EyeImage,2> eyes{}; FramePose pose{}; uint64_t generation{}; VkQueue producerQueue{}; bool requiresPresentWaits{true}; bool rightEyeBlackDiagnostic{}; };
// Latched at process startup. This mode has no right scene image or replay.
bool leftEyeOnlyDiagnostic();
bool stereoPreparationEnabled();
bool mirrorPreparationEnabled();
bool replayPreparationEnabled();
bool inputPlanCaptureEnabled();
void prepareLeftDiagnosticTargets();
void finishLeftDiagnosticPreparation();
bool requested(); bool installed(); Phase phase();
void setDevice(VkInstance,VkPhysicalDevice,VkDevice,PFN_vkGetDeviceProcAddr,PFN_vkGetInstanceProcAddr);
void setQueue(VkQueue,uint32_t,uint32_t);
void setQueueAccessCallbacks(void(*)(),void(*)());
void beforeDeviceDestroy(VkDevice);
PFN_vkVoidFunction wrapProc(const char*,PFN_vkVoidFunction);
void prepare(const FramePose&);
bool pair(VkImage,VkExtent2D,VkFormat,StereoFrame&);
bool handSceneTarget(const StereoFrame&,const hands::HandSceneTarget&,hands::HandSceneTarget&);
void xrPresented(uint64_t,XrResult,bool);
void completed(); void presentStarting();
void beginQualityChange();
void lockQualityPresentation();
void unlockQualityPresentation();
void qualityResourceMutation(bool entering);
uint64_t qualityTransitionEpoch();
void qualityPresentCompleted(uint64_t epoch,bool success);
// Opt-in evidence from the actual acquired OpenXR images, after overlays and
// before release. No additional submit, semaphore consumption or XR session.
struct XrCaptureEye {VkImage image{};VkExtent2D extent{};VkFormat format{};XrRect2Di rect{};uint32_t acquiredIndex{};};
bool xrTargetCaptureEnabled();
bool recordXrTargetCapture(VkDevice,VkCommandBuffer,const StereoFrame&,const std::array<XrCaptureEye,2>&);
void finishXrTargetCapture();
bool pairWatchEnabled();
bool recordPairWatch(VkDevice,VkCommandBuffer,const StereoFrame&);
void finishPairWatch();
void recordShadowHistory(VkCommandBuffer,const StereoFrame&);
uint32_t beginOwnerGpuTiming(VkCommandBuffer);
void endOwnerGpuTiming(uint32_t);
[[noreturn]] void fail(const char*);
void submitted(VkQueue,uint32_t,const VkSubmitInfo*,VkResult);
void submitted2(VkQueue,uint32_t,const VkSubmitInfo2*,VkResult);
}

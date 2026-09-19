#include "../common/RuntimeLog.h"
#include "../common/VulkanSfs.h"
#include "../sfs/NativeSfs.h"
#include "CopyGpuTiming.h"
#include "../common/AerRenderOrder.h"
#include "../common/AerEyeBasis.h"
#include "../common/AerCinematicProjection.h"
#include "ControllerFrameYaw.h"
#include "../common/AerSourceTracking.h"
#include "../common/DiagnosticLogging.h"
#include "../common/PoseTrace.h"
#include "ProjectionSourceCrop.h"
#include "FreshAerHandsPolicy.h"
#include "../native/NativeStereo.h"
#include "../native/NativeFrameTrace.h"
#include "../native/NativeXrFrameTrace.h"
#include "../native/NativeCpuProfile.h"
#include <windows.h>
#include <intrin.h>
#include <Unknwn.h>
#include "EyeCapturePng.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <filesystem>
#include <stdexcept>
#include <Xinput.h>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_VULKAN
#include "OpenXRBootstrap.h"
#include "GameImageLifetime.h"
#include "../camera/CameraHook.h"
#include "../debug/CameraScanner.h"
#include "../hud/HudGpuDiagnostic.h"
#include "../hud/HudHook.h"
#include "../hud/HudLayoutPolicy.h"
#include "../hud/HudMenuPolicy.h"
#include "../weapon/WeaponHook.h"
#include "../weapon/BackWeaponPolicy.h"
#include "../fsr/Fsr1Policy.h"
#include "../native/NativePresentationPolicy.h"
#include "../fsr/Fsr1Upscaler.h"
#include "../hands/HandRenderer.h"
#include "../hands/HandDepthPolicy.h"
#include "../common/RuntimePaths.h"
#include "OpenXRRuntimePolicy.h"
#include "RuntimeVulkanDispatch.h"
#include "NativeXrReleasePolicy.h"
#include "CommandRecordingState.h"
#include "SessionReadiness.h"
#include "SwapchainImageState.h"
#include "CinematicRefreshPolicy.h"
#include "CinewindowPosePolicy.h"
#include "HandsJumpPolicy.h"
#include "GripThresholdPolicy.h"
#include "EquipmentGripPolicy.h"
#include "MovementDirectionPolicy.h"
#include "SnapTurnStereoPolicy.h"
#include "PostCinematicYawPolicy.h"
#include "XInputHapticsPolicy.h"
#include "MotionWeaponWheelPolicy.h"
#include "../bhaptics/BhapticsIpcClient.h"
#include "../psvr2/Psvr2IpcClient.h"
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

static DWORD portableGetFileAttributesW(LPCWSTR path) {
    const auto name = wcsrchr(path, L'\\');
    return GetFileAttributesW(kharvox::runtimePath(name ? name + 1 : path).c_str());
}
#define GetFileAttributesW portableGetFileAttributesW

namespace {
struct DoomSwapchain { VkExtent2D extent{}; VkFormat format{}; std::vector<VkImage> images; uint32_t stablePresents{}; bool copyRecoveryPending{}; };
static uint64_t nativeQualityPresentEpoch{}; // Protected by the XR state mutex.
struct EyeSwapchain { XrSwapchain handle{XR_NULL_HANDLE}; uint32_t width{},height{}; std::vector<XrSwapchainImageVulkanKHR> images; std::vector<bool> initialized; };
EyeSwapchain pauseBindings;
bool pauseBindingsReady{};
struct HeadPose { XrQuaternionf orientation{0,0,0,1}; XrVector3f position{}; float yaw{},pitch{},roll{}; bool valid{}; };
struct ControllerPose {
    bool valid{},linearVelocityValid{},positionTracked{};
    XrVector3f position{},linearVelocity{};
    XrQuaternionf orientation{0,0,0,1};
};
struct AerCapturedInput {
    XrPosef pose{};XrFovf fov{};
    ControllerPose left{},right{};
    float artificialTurn{};uint64_t snapGeneration{};
    XrPosef center{};
    XrPosef laserBodyTracking{};
};
struct EyeRenderProjection { XrFovf symmetricFov{}; };
enum class TurnMode { Off, Smooth, Snap };
enum class FaceButtonRoute { None, Gameplay, Ui };
enum class PhysicalGlorykillHands { Left, Right, Both };
struct WeaponPose { XrVector3f position{}; XrQuaternionf orientation{0,0,0,1}; };
enum class TwoHandAimMode { Barrel, SideGrip };
struct TwoHandCalibration {
    XrVector3f supportPosition{-0.08f,0.0f,-0.38f};
    float captureRadius{0.20f};
    float maximumHandLineWeight{1.0f};
    TwoHandAimMode aimMode{TwoHandAimMode::Barrel};
    bool valid{};
};
bool immersiveCinematicsRequested() {
    return kharvox::runtimeFileExists(L"enable_immersive_cinematics_and_glory_kills");
}
bool immersiveCinematicFreelookRequested() {
    return kharvox::runtimeFileExists(L"enable_immersive_cinematic_freelook");
}
bool otherCinematicsInQuadRequested() {
    return kharvox::runtimeFileExists(L"enable_other_cinematics_in_quad");
}
bool cinewindowFollowsHeadsetRequested() {
    // r262 diagnostic: force a fixed tracking-space screen after anchor readiness.
    return false;
}
bool showHandsRequested() {
    char value[8]{};
    return GetEnvironmentVariableA("KHARVOX_SHOW_HANDS",value,sizeof(value))>0
        &&!strcmp(value,"1");
}
constexpr unsigned long long levelTransitionMonoGuardMilliseconds = 3000;
constexpr unsigned long long nativeAdaptiveParticipantReleaseGraceMilliseconds = 500;
struct CinematicAdaptiveTickHold {
    bool layoutChecked{},layoutAvailable{},active{};
    LONG savedAdaptiveTick{},savedMinHz{},savedMaxHz{},savedImmediateMode{},savedSyncNoAdaptiveTick{},savedFixedTic{};
    int targetHz{};
    LARGE_INTEGER startedAt{};
    uint64_t presents{};
};
struct State {
    kharvox::ControllerFrameYaw controllerFrameYaw;
    kharvox::AerInputHistory<AerCapturedInput> aerInputHistory;
    ControllerPose aerPairInputLeft{},aerPairInputRight{};
    uint64_t aerPairInputPoseId{},integratedPairSourcePoseId{},aerPublishedSourcePoseId{};
    XrPosef aerPairInputCenter{};unsigned aerPublishedDomain{};
    kharvox::AerInputHistory<XrVector3f> aerCinematicPairAnchors;
    bool aerSourceModeActive{};
    std::array<bool,2> aerSourceCacheValid{};
    std::array<kharvox::AerSourceKey,2> aerSourceCacheKeys{};
    HMODULE loader{}; XrInstance instance{XR_NULL_HANDLE}; XrSystemId system{XR_NULL_SYSTEM_ID}; XrSession session{XR_NULL_HANDLE}; XrSpace space{XR_NULL_HANDLE}; XrSpace viewSpace{XR_NULL_HANDLE};
    XrSessionState sessionState{XR_SESSION_STATE_UNKNOWN}; kharvox::SessionReadiness sessionReadiness{}; bool running{}, enable2{},useEnable2Bridge{},useEnable2RuntimeManaged{}; int64_t format{}; uint64_t frame{},submittedLayerFrames{};
    kharvox::OpenXRRuntimeKind runtimeKind{kharvox::OpenXRRuntimeKind::Unknown};
    kharvox::OpenXRVulkanPath vulkanPath{kharvox::OpenXRVulkanPath::None};
    std::string runtimeManifest;
    bool simulatorRuntime{};
    bool physicalIdentityQueryEnabled{};
    VkInstance vkInstance{}; VkPhysicalDevice physical{},xrPhysical{}; VkDevice device{}; VkQueue queue{}; uint32_t queueFamily{},queueIndex{},runtimeMaxVulkanApiVersion{}; KharvoxVulkanDispatch vk{};
    VkCommandPool commandPool{}; VkCommandBuffer commandBuffer{}; VkFence copyFence{};
    std::array<kharvox::SwapchainImageState,2> eyeImageStates{}; kharvox::SwapchainImageState hudImageState{};
    kharvox::CopyGpuTiming sfsCopyTiming;
    std::unordered_map<VkSwapchainKHR,DoomSwapchain> doomSwapchains; DoomSwapchain retiredCompatibleDoomSwapchain{}; bool retiredCompatibleDoomSwapchainValid{}; ULONGLONG retiredCompatibleDoomSwapchainAt{}; VkSwapchainKHR startupActiveDoomSwapchain{}; bool vdxrSessionDeferralLogged{}; std::array<EyeSwapchain,2> eyes; EyeSwapchain hudQuad; uint32_t hudSurfaceWidth{},hudSurfaceHeight{}; float hudSafeTanHalfHorizontal{},hudSafeTanHalfVertical{}; int64_t hudQuadFormat{}; uint64_t hudQuadCopiedFrames{}; bool hudQuadRuntimeFailureLogged{}; std::array<XrView,2> views{{{XR_TYPE_VIEW},{XR_TYPE_VIEW}}};
    HeadPose head{}; XrVector3f trackingHeadPosition{}; bool trackingHeadPositionValid{}; XrQuaternionf headZero{0,0,0,1}; XrVector3f headZeroPosition{}; bool headZeroValid{}; bool headZeroPositionValid{}; bool headCameraArmed{true}; bool nativeMenuCameraPoseHeld{}; bool quadMode{true}; bool centeredQuadTransitionPending{}; unsigned centeredQuadFramesRemaining{}; bool cinewindowFollowsHeadset{cinewindowFollowsHeadsetRequested()}; bool cinewindowPresentationActive{}; bool cinewindowFixedPoseFallbackLogged{}; kharvox::CinewindowAnchor cinewindowAnchor{}; kharvox::CinewindowCaptureReadiness cinewindowCaptureReadiness{}; bool hudEverythingQuad{},hudEverythingQuadAvailable{}; bool hudEverythingQuadKeyDown{}; bool steamQuadOnly{}; bool steamMetaCompatibilityMode{}; bool presentationKeyDown{}; bool presentationManualOverride{}; XrTime lastSteamDisplayTime{}; uint64_t steamDuplicateFrames{},steamLinkReprojectedFrames{};
    uint64_t steamWaitCalls{},steamBeginCalls{},steamEndCalls{},steamDiscardedBegins{},steamEndFailures{},steamLocalOrderViolations{};bool steamFrameBegun{};
    bool steamFramePrepared{}; XrFrameState steamPreparedFrame{XR_TYPE_FRAME_STATE}; XrResult steamPreparedWaitResult{XR_SUCCESS}; XrResult steamPreparedBeginResult{XR_SUCCESS}; double steamPreparedWaitMs{}; LARGE_INTEGER steamPreparedAt{}; DWORD steamPreparedThread{},steamPreparedAcquireCallerThread{}; VkSwapchainKHR steamPreparedDoomSwapchain{}; uint64_t steamPreparedFrames{},steamConsumedPreparedFrames{},steamRepeatedAcquires{}; bool steamFsrStartupHandshakeComplete{};
    int renderEye{kharvox::aerFirstRenderEye}; std::array<VkImage,2> stereoCache{}; std::array<VkDeviceMemory,2> stereoCacheMemory{}; std::array<bool,2> stereoCacheInitialized{}; std::array<std::uint64_t,2> stereoCacheRevision{}; VkExtent2D stereoCacheExtent{}; bool alternatingStereo{}; bool alternatingStereoSkipInitialCapture{}; bool alternatingStereoWarmupActive{}; int alternatingStereoWarmupEye{}; unsigned alternatingStereoWarmupFramesRemaining{}; bool stereoStartupControllerMissing{}; unsigned long long stereoLevelGeneration{}; bool stereoLevelGenerationValid{}; unsigned long long stereoLevelGuardUntilTick{}; float worldScale{39.3701f}; float renderScale{1.f};
    Fsr1Upscaler fsr1{}; bool fsr1Requested{}; bool fsr1InitializationAttempted{};
    kharvox::hands::HandRenderer handRenderer{}; bool showHands{showHandsRequested()};
    kharvox::hands::HandPose integratedPairLeftHand{},integratedPairRightHand{};
    kharvox::hands::HandVisibilityOutput integratedPairHandVisibility{};
    kharvox::hands::HandGameplayState integratedPairHandGameplay{};
    bool integratedPairHandsValid{};std::array<bool,2> stereoCacheHasIntegratedHands{};
    std::array<VkImage,2> freshHandsWorld{};
    std::array<VkDeviceMemory,2> freshHandsMemory{};
    VkExtent2D freshHandsExtent{};
    bool freshHandsInitialized{},freshHandsWorldValid{};
    uint64_t freshHandsWorldRevision{};
    bool handSceneDepthFallbackLogged{};
    std::array<XrPosef,2> programmedEyePose{}; std::array<XrFovf,2> programmedEyeFov{}; std::array<bool,2> programmedEyeViewValid{}; std::array<float,2> programmedEyeArtificialTurn{}; std::array<std::uint64_t,2> programmedEyeSnapGeneration{};
    XrPosef programmedMonoPose{}; XrFovf programmedMonoFov{}; bool programmedMonoPoseValid{};
    std::array<XrPosef,2> programmedMonoEyePose{}; std::array<XrFovf,2> programmedMonoEyeFov{}; bool programmedMonoEyeViewsValid{}; bool steamLinkMonoCameraActive{};
    XrVector3f immersiveCinematicLayerPosition{}; bool immersiveCinematicLayerPositionHeld{};
    std::array<XrPosef,2> cachedEyePose{}; std::array<XrFovf,2> cachedEyeFov{}; std::array<bool,2> cachedEyeViewValid{}; std::array<float,2> cachedEyeArtificialTurn{}; std::array<std::uint64_t,2> cachedEyeSnapGeneration{};
    std::array<XrPosef,2> previousCachedEyePose{}; std::array<XrFovf,2> previousCachedEyeFov{}; std::array<bool,2> previousCachedEyeViewValid{};
    std::array<XrPosef,2> steamXrAerCapturePose{}; std::array<XrFovf,2> steamXrAerCaptureFov{}; std::array<bool,2> steamXrAerCaptureEyesReady{}; bool steamXrAerCaptureValid{}; float steamXrAerCaptureArtificialTurn{};
    std::array<XrPosef,2> steamXrAerSubmittedPose{}; std::array<XrFovf,2> steamXrAerSubmittedFov{}; std::array<XrRect2Di,2> steamXrAerSubmittedRects{};
    bool steamXrAerPairReady{}; bool steamXrAerPairActive{}; float steamXrAerSubmittedArtificialTurn{}; XrDuration steamXrAerDisplayPeriod{}; uint64_t steamXrAerPublishedPairs{},steamXrAerReusedPairs{},steamXrAerTurnCompensations{};
    std::array<VkImage,2> steamLinkFixedPairImages{}; std::array<XrPosef,2> steamLinkFixedPairPose{}; std::array<XrFovf,2> steamLinkFixedPairFov{}; bool steamLinkFixedPairValid{}; uint64_t steamLinkFixedPairSubmissions{};
    std::array<XrQuaternionf,4> steamLinkHeadOrientationHistory{}; std::array<XrTime,4> steamLinkHeadOrientationTimes{};
    uint32_t steamLinkHeadHistoryIndex{},steamLinkHeadHistoryCount{},steamLinkMotionEnterSamples{},steamLinkMotionExitSamples{};
    bool steamLinkMotionFixedPairActive{}; uint64_t steamLinkMotionFilterSamples{};
    PFN_xrGetInstanceProcAddr getProc{}; PFN_xrEnumerateInstanceExtensionProperties enumerateExtensions{}; PFN_xrCreateInstance createInstance{}; PFN_xrDestroyInstance destroyInstance{}; PFN_xrGetSystem getSystem{};
    PFN_xrCreateSession createSession{}; PFN_xrDestroySession destroySession{}; PFN_xrPollEvent pollEvent{}; PFN_xrBeginSession beginSession{}; PFN_xrEndSession endSession{};
    PFN_xrCreateReferenceSpace createSpace{}; PFN_xrDestroySpace destroySpace{}; PFN_xrEnumerateViewConfigurationViews enumerateViews{}; PFN_xrEnumerateSwapchainFormats enumerateFormats{};
    PFN_xrCreateSwapchain createSwapchain{}; PFN_xrDestroySwapchain destroySwapchain{}; PFN_xrEnumerateSwapchainImages enumerateImages{}; PFN_xrAcquireSwapchainImage acquireImage{}; PFN_xrWaitSwapchainImage waitImage{}; PFN_xrReleaseSwapchainImage releaseImage{};
    PFN_xrWaitFrame waitFrame{}; PFN_xrBeginFrame beginFrame{}; PFN_xrLocateViews locateViews{}; PFN_xrEndFrame endFrame{};
    PFN_xrCreateActionSet createActionSet{}; PFN_xrDestroyActionSet destroyActionSet{}; PFN_xrCreateAction createAction{}; PFN_xrDestroyAction destroyAction{}; PFN_xrSuggestInteractionProfileBindings suggestBindings{}; PFN_xrAttachSessionActionSets attachActionSets{}; PFN_xrSyncActions syncActionsFn{}; PFN_xrGetCurrentInteractionProfile getCurrentInteractionProfile{}; PFN_xrCreateActionSpace createActionSpace{}; PFN_xrLocateSpace locateSpace{}; PFN_xrGetActionStatePose getActionStatePose{}; PFN_xrGetActionStateVector2f getActionStateVector2f{}; PFN_xrGetActionStateFloat getActionStateFloat{}; PFN_xrGetActionStateBoolean getActionStateBoolean{}; PFN_xrApplyHapticFeedback applyHapticFeedback{}; PFN_xrStopHapticFeedback stopHapticFeedback{};
    XrActionSet gameplayActionSet{XR_NULL_HANDLE}; XrAction rightAimPose{XR_NULL_HANDLE}; XrAction leftAimPose{XR_NULL_HANDLE}; XrAction rightGripPose{XR_NULL_HANDLE}; XrAction leftGripPose{XR_NULL_HANDLE};
    XrAction doomMove{XR_NULL_HANDLE}; XrAction doomTurn{XR_NULL_HANDLE}; XrAction doomFire{XR_NULL_HANDLE};
    XrAction doomJump{XR_NULL_HANDLE}; XrAction doomCrouch{XR_NULL_HANDLE};
    XrAction doomMelee{XR_NULL_HANDLE}; XrAction doomEquipment{XR_NULL_HANDLE};
    kharvox::EquipmentGripPolicy equipmentGrip;
    XrAction doomSecondaryFireGrip{XR_NULL_HANDLE};
    XrAction doomSupportGrip{XR_NULL_HANDLE};
    XrAction doomPause{XR_NULL_HANDLE}; XrAction doomMissionInfo{XR_NULL_HANDLE};
    XrAction doomSwitchWeaponMod{XR_NULL_HANDLE};
    XrAction leftHaptic{XR_NULL_HANDLE}; XrAction rightHaptic{XR_NULL_HANDLE};
    XrSpace rightAimSpace{XR_NULL_HANDLE}; XrSpace leftAimSpace{XR_NULL_HANDLE}; XrSpace rightGripSpace{XR_NULL_HANDLE}; XrSpace leftGripSpace{XR_NULL_HANDLE}; ControllerPose rightController{}; ControllerPose leftController{}; ControllerPose rightGripController{}; ControllerPose leftGripController{}; XrVector2f leftStick{}; XrVector2f rightStick{};
    XrPath leftHandUserPath{XR_NULL_PATH}; XrPath rightHandUserPath{XR_NULL_PATH}; XrPath valveIndexProfilePath{XR_NULL_PATH};
    bool interactionProfilesDirty{true}; bool interactionProfilesKnown{}; bool interactionProfileQueryFailureLogged{}; bool primaryGripUsesValveIndex{}; bool supportGripUsesValveIndex{};
    bool leftHanded{},leftHandSwapSticks{};
    bool actionsReady{}; bool hapticActionsReady{}; bool hapticBindingsSuggested{}; bool hapticFrequencyUnspecified{}; bool firstControllerHapticAppliedLogged{}; bool firePressed{},primaryFireDown{},jumpPressed{},crouchPressed{};
    unsigned long long psvr2FireStartedTick{}; KharvoxWeaponKind psvr2FireWeapon{KharvoxWeaponKind::Unknown};
    std::array<kharvox::XInputHapticOutputState,2> hapticOutputStates{};
    kharvox::XInputRumbleFrameAccumulator hapticFrameAccumulator{};
    unsigned long long weaponFireHapticFallbackUntilTick{};
    unsigned long long nextHapticErrorLogTick{}; uint64_t hapticErrorCount{};
    FaceButtonRoute jumpButtonRoute{FaceButtonRoute::None};
    FaceButtonRoute crouchButtonRoute{FaceButtonRoute::None};
    bool faceButtonGameplayContext{},faceButtonContextInitialized{};
    bool nativeUiInputContext{};
    bool jumpSuppressedUntilRelease{},crouchSuppressedUntilRelease{};
    bool meleePressed{},missionInfoPressed{};
    bool handsJumpEnabled{}; kharvox::HandsJumpState handsJump{};
    bool physicalGlorykillEnabled{};
    float physicalGlorykillSpeed{2.8f};
    PhysicalGlorykillHands physicalGlorykillHands{PhysicalGlorykillHands::Both};
    std::array<bool,2> physicalPunchArmed{};
    XrTime physicalPunchCooldownUntil{};
    bool weaponSelectPressed{},weaponSelectNativeStarted{},weaponWheelOpened{},secondaryFireGripPressed{};
    std::array<kharvox::ControllerClickState,2> wheelClicks{};
    kharvox::WeaponWheelStickHapticState wheelStickHaptics{};
    bool motionWheelEnabled{true}; kharvox::MotionWeaponWheelState motionWheelState{};
    bool chainsawArmed{true},pausePressed{};
    XrTime weaponSelectPressedTime{},weaponSwitchPulseUntil{},chainsawPulseUntil{};
    XrTime usePulseUntil{},meleePulseUntil{};
    bool immersiveCinematics{immersiveCinematicsRequested()};
    bool immersiveCinematicFreelook{immersiveCinematicFreelookRequested()};
    bool otherCinematicsInQuad{otherCinematicsInQuadRequested()};
    bool immersiveCinematicActive{},immersiveCinematicFreelookActive{};
    XrDuration fastestDisplayPeriod{};
    CinematicAdaptiveTickHold cinematicAdaptiveTick{};
    unsigned long long nativeAdaptiveParticipantGuardUntilTick{};
    bool nativeAdaptiveParticipantGuardActive{};
    bool comfortInteractiveParticipantActive{};
    bool immersiveCinematicCameraRequested{},firstPersonCinematicAssistHeld{};
    bool tutorialCinematicProjectionHeld{};
    bool ordinaryCinematicQuadHeld{};
    TurnMode turnMode{TurnMode::Smooth}; float turnDeadzone{.35f}; float smoothTurnDegreesPerSecond{230.f}; float snapTurnDegrees{60.f}; bool snapTurnArmed{true};
    kharvox::MovementDirectionMode movementDirectionMode{kharvox::MovementDirectionMode::Head}; bool offHandMovementDirectionStateKnown{},offHandMovementDirectionActive{};
    bool snapTurnActivationPending{}; float snapTurnPendingDegrees{}; std::uint64_t snapTurnGeneration{},snapTurnStereoCompensations{}; bool snapTurnStereoTransitionActive{};
    bool artificialTurnActive{}; float artificialTurnRemainingDegrees{}; float artificialTurnPublishedDegrees{}; float artificialTurnAccumulatedDegrees{}; float artificialTurnTotalDegrees{}; XrTime previousTurnDisplayTime{};
    float acceptedPhysicalYaw{}; float previousBodyYaw{}; bool previousBodyYawValid{};
    float bodyFollowTurnX{}; bool previousManualTurnActive{}; float bodyYawPerPositiveStick{-1.f}; bool bodyYawStickSignConfirmed{};
    kharvox::PostCinematicYawGuard postCinematicYawGuard{};
    bool gameplayRecenterPending{true};
    bool weapon6Dof{},weaponCalibrated{},weaponGameplayActivated{},weaponPoseSubmitted{},weaponTrackingHeld{};WeaponPose weaponBaseline{};
    bool laserSightEnabled{};
    bool twoHandEnabled{},twoHandCalibrationMode{},twoHandLatched{},supportGripPressed{},virtualGunstockEnabled{};
    bool bfgGripTriggered{},bfgGripSuppressedUntilRelease{};
    XrTime bfgGripHoldStart{},bfgPulseUntil{};
    kharvox::BackWeaponState backWeaponState{};
    kharvox::BackWeaponKind favoriteBackWeapon{kharvox::BackWeaponKind::CombatShotgun};
    kharvox::BackWeaponPulse backWeaponPulse{kharvox::BackWeaponPulse::None};
    XrTime backWeaponPulseUntil{};
    WORD backWeaponKeyboardKey{};
    XrTime backWeaponKeyboardReleaseTime{};
    bool backWeaponKeyboardKeyDown{};
    std::array<TwoHandCalibration,static_cast<size_t>(KharvoxWeaponKind::Count)> twoHandCalibrations{};
    KharvoxWeaponKind twoHandCalibrationTarget{KharvoxWeaponKind::Shotgun};
    KharvoxWeaponKind twoHandLatchedWeapon{KharvoxWeaponKind::Unknown};
    TwoHandAimMode twoHandCalibrationAimMode{TwoHandAimMode::Barrel};
    XrVector3f bodyFollowPosition{}; XrVector2f roomscaleStick{};
    std::array<float,3> previousPhysicsOrigin{}; bool previousPhysicsOriginValid{};
    std::array<float,3> previousRoomscaleWorldDirection{}; XrVector3f previousRoomscaleTrackingDirection{};
    bool cinematicBodyPoseHeld{};
    bool previousRoomscaleCommand{};
    PFN_xrGetVulkanGraphicsRequirements2KHR requirements2{}; PFN_xrGetVulkanGraphicsDevice2KHR graphicsDevice2{}; PFN_xrCreateVulkanInstanceKHR createVulkanInstance{}; PFN_xrCreateVulkanDeviceKHR createVulkanDevice{}; PFN_xrGetVulkanGraphicsRequirementsKHR requirements1{}; PFN_xrGetVulkanGraphicsDeviceKHR graphicsDevice1{};
    PFN_xrGetVulkanInstanceExtensionsKHR instanceExtensions{}; PFN_xrGetVulkanDeviceExtensionsKHR deviceExtensions{};
};
State s; std::mutex mutex; bool steamSessionCreationInProgress=false; thread_local bool mediationReentry=false; thread_local VkPhysicalDevice reentryPhysical=VK_NULL_HANDLE; thread_local PFN_vkCreateDevice reentryCreateDevice=nullptr;
alignas(4) volatile LONG immersiveAdaptiveParticipantBypass{};
alignas(4) volatile LONG nativeAdaptiveParticipantObserved{};
using XInputGetStateFn=DWORD(WINAPI*)(DWORD,XINPUT_STATE*);
using XInputSetStateFn=DWORD(WINAPI*)(DWORD,XINPUT_VIBRATION*);
XInputGetStateFn originalXInputGetState{};XInputSetStateFn originalXInputSetState{};std::atomic<SHORT> virtualLeftX{},virtualLeftY{},virtualRightX{},virtualRightY{};std::atomic<BYTE> virtualLeftTrigger{},virtualRightTrigger{};std::atomic<WORD> virtualButtons{};std::atomic<DWORD> virtualPacket{1};std::atomic<uint32_t> nativeXInputRumble{};std::atomic<uint32_t> pendingXInputRumblePeaks{};bool xinputHookReady{};bool xinputSetStateHookReady{};
PFN_vkGetInstanceProcAddr bridgeNextGipa=nullptr;
thread_local bool bridgeSessionTrace=false;
// Meta may invoke the XR_KHR_vulkan_enable2 callbacks from one of its own
// worker threads. Keep this short-lived state visible across threads.
std::atomic<PFN_vkGetInstanceProcAddr> runtimeManagedNextGipa{};
std::atomic<VkPhysicalDevice> runtimeManagedReentryPhysical{VK_NULL_HANDLE};
std::atomic<PFN_vkCreateDevice> runtimeManagedReentryCreateDevice{};
std::atomic<const VkDeviceCreateInfo*> runtimeManagedDownstreamCreateInfo{};
std::atomic<bool> runtimeManagedSessionLoaderRoute{};
PFN_vkGetPhysicalDeviceProperties2 bridgeGetPhysicalDeviceProperties2Downstream=nullptr;
PFN_vkGetPhysicalDeviceMemoryProperties bridgeGetPhysicalDeviceMemoryPropertiesDownstream=nullptr;
void log(const std::string& x, bool operational = false);
bool eyeCaptureEnabled();
void destroySessionResources();
void requestSessionRestart(const std::string& failure){s.running=false;s.sessionReadiness.restartRequired();log(failure+"; session restart required");}
KharvoxQueueAccessCallback queueAccessLockCallback{},queueAccessUnlockCallback{};
struct QueueAccessScope {
    QueueAccessScope(){if(queueAccessLockCallback)queueAccessLockCallback();}
    ~QueueAccessScope(){if(queueAccessUnlockCallback)queueAccessUnlockCallback();}
};
XrResult acquireSwapchainImage(XrSwapchain swapchain,const XrSwapchainImageAcquireInfo*info,uint32_t*index){QueueAccessScope queueAccess;return s.acquireImage(swapchain,info,index);}
XrResult releaseSwapchainImage(XrSwapchain swapchain,const XrSwapchainImageReleaseInfo*info){QueueAccessScope queueAccess;return s.releaseImage(swapchain,info);}
XrResult beginFrame(const XrFrameBeginInfo*info){QueueAccessScope queueAccess;return s.beginFrame(s.session,info);}
XrResult endFrame(const XrFrameEndInfo*info){QueueAccessScope queueAccess;return s.endFrame(s.session,info);}
class SteamXrFrameThread {
public:
    ~SteamXrFrameThread(){
        {
            std::lock_guard<std::mutex> lock(taskMutex);
            stopping=true;
        }
        wake.notify_one();
        if(worker.joinable())worker.join();
    }
    DWORD invoke(std::function<void()> work){
        std::unique_lock<std::mutex> lock(taskMutex);
        if(!worker.joinable()){
            worker=std::thread([this]{run();});
            ready.wait(lock,[this]{return workerThreadId!=0;});
            log("[STEAM-XR-THREAD] dedicated frame thread started id="+std::to_string(workerThreadId));
        }
        if(GetCurrentThreadId()==workerThreadId){
            lock.unlock();
            work();
            return workerThreadId;
        }
        idle.wait(lock,[this]{return !taskPending;});
        task=std::move(work);
        taskPending=true;
        taskComplete=false;
        wake.notify_one();
        complete.wait(lock,[this]{return taskComplete;});
        return workerThreadId;
    }
private:
    void run(){
        std::unique_lock<std::mutex> lock(taskMutex);
        workerThreadId=GetCurrentThreadId();
        ready.notify_all();
        for(;;){
            if(s.simulatorRuntime){
                // The simulator creates its preview window on this lifecycle
                // thread. WM_ACTIVATE sent by DOOM can otherwise block forever
                // when no new XR task arrives because DOOM is inside that send.
                wake.wait_for(lock,std::chrono::milliseconds(8),[this]{return stopping||taskPending;});
                if(!stopping&&!taskPending){
                    lock.unlock();
                    MSG message{};
                    for(unsigned n=0;n<64&&PeekMessageW(&message,nullptr,0,0,PM_REMOVE);++n){TranslateMessage(&message);DispatchMessageW(&message);}
                    lock.lock();
                    continue;
                }
            }else wake.wait(lock,[this]{return stopping||taskPending;});
            if(stopping&&!taskPending)break;
            auto current=std::move(task);
            lock.unlock();
            current();
            lock.lock();
            taskPending=false;
            taskComplete=true;
            complete.notify_all();
            idle.notify_all();
        }
    }
    std::mutex taskMutex;
    std::condition_variable wake,ready,complete,idle;
    std::thread worker;
    std::function<void()> task;
    DWORD workerThreadId{};
    bool taskPending{},taskComplete{},stopping{};
};
SteamXrFrameThread steamXrFrameThread;
bool isPhysicalCommand(const char*n);
VKAPI_ATTR VkResult VKAPI_CALL bridgeCreateDevice(VkPhysicalDevice,const VkDeviceCreateInfo*ci,const VkAllocationCallbacks*a,VkDevice*out){return reentryCreateDevice?reentryCreateDevice(reentryPhysical,ci,a,out):VK_ERROR_INITIALIZATION_FAILED;}
VKAPI_ATTR VkResult VKAPI_CALL runtimeManagedCreateDevice(VkPhysicalDevice,const VkDeviceCreateInfo*ci,const VkAllocationCallbacks*a,VkDevice*out){const auto createDevice=runtimeManagedReentryCreateDevice.load(std::memory_order_acquire);const auto physical=runtimeManagedReentryPhysical.load(std::memory_order_acquire);const auto downstreamCi=runtimeManagedDownstreamCreateInfo.load(std::memory_order_acquire);if(!createDevice||!physical||!ci)return VK_ERROR_INITIALIZATION_FAILED;VkDeviceCreateInfo merged=*ci;if(downstreamCi)merged.pNext=downstreamCi->pNext;log("[RUNTIME-ENABLE2] vkCreateDevice adapter reattached Vulkan loader chain thread="+std::to_string(GetCurrentThreadId()));return createDevice(physical,&merged,a,out);}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL runtimeManagedGipa(VkInstance instance,const char* name) {
    const auto loader = GetModuleHandleW(L"vulkan-1.dll");
    const auto loaderGipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        loader ? GetProcAddress(loader,"vkGetInstanceProcAddr") : nullptr);
    const auto createDevice = runtimeManagedReentryCreateDevice.load(std::memory_order_acquire);
    const auto resolved = kharvox::resolveRuntimeVulkanProc(instance, name,
        runtimeManagedSessionLoaderRoute.load(std::memory_order_acquire), s.simulatorRuntime,
        runtimeManagedNextGipa.load(std::memory_order_acquire), loaderGipa,
        createDevice ? reinterpret_cast<PFN_vkVoidFunction>(runtimeManagedCreateDevice) : nullptr,
        kharvox::sfs::nativeProbeEnabled()?s.device:VK_NULL_HANDLE,
        kharvox::sfs::nativeProbeEnabled()?s.vk.getDeviceProcAddr:nullptr);
    const char* route = "passthrough";
    switch (resolved.route) {
    case kharvox::RuntimeDispatchRoute::CreateDevice: route="create-device"; break;
    case kharvox::RuntimeDispatchRoute::SessionLoader: route="session-loader-gipa"; break;
    case kharvox::RuntimeDispatchRoute::PhysicalLoader: route="physical-loader"; break;
    case kharvox::RuntimeDispatchRoute::RuntimeDevice: route="runtime-device-downstream"; break;
    default: break;
    }
    log(std::string("[RUNTIME-ENABLE2] GIPA thread=")+std::to_string(GetCurrentThreadId())+
        " name="+(name?name:"NULL")+" returned="+
        std::to_string(reinterpret_cast<uintptr_t>(resolved.function))+" adapter="+route);
    return resolved.function;
}
void log(const std::string& x, bool operational){
    kharvox::writeRuntimeLog("[KHARVOX][XR]", x, operational);
}

double performanceMilliseconds(LARGE_INTEGER begin,LARGE_INTEGER end){static const double ticksPerMillisecond=[](){LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);return double(frequency.QuadPart)/1000.0;}();return ticksPerMillisecond>0.0?double(end.QuadPart-begin.QuadPart)/ticksPerMillisecond:0.0;}
namespace doomAdaptiveTick {
constexpr uintptr_t adaptiveTickNameRva=0x2835570;
constexpr uintptr_t adaptiveTickCurrentRva=0x5DA7670;
constexpr uintptr_t minHzNameRva=0x2835598;
constexpr uintptr_t minHzCurrentRva=0x5DA7710;
constexpr uintptr_t maxHzNameRva=0x28355C0;
constexpr uintptr_t maxHzCurrentRva=0x5DA77B0;
constexpr uintptr_t immediateModeNameRva=0x2835618;
constexpr uintptr_t immediateModeCurrentRva=0x5DA7850;
constexpr uintptr_t syncNoAdaptiveTickNameRva=0x21AC280;
constexpr uintptr_t syncNoAdaptiveTickCurrentRva=0x5B56F70;
constexpr uintptr_t fixedTicNameRva=0x282AE00;
constexpr uintptr_t fixedTicCurrentRva=0x5D4F320;
// idCommonLocal's adaptive timing object. The participant branch clears byte
// +0x24 before the XR observer can arm its bypass on the following Present.
// That stale zero keeps the later game-frame path on its fixed fallback even
// when every public CVar has already been corrected.
constexpr uintptr_t adaptiveRuntimeStatePointerRva=0x35EB8C0;
constexpr uintptr_t adaptiveRuntimeEnabledOffset=0x24;
constexpr uintptr_t participantBranchRva=0x3BED77;
constexpr uintptr_t participantBlockedRva=0x3BEE33;
constexpr uintptr_t participantContinueRva=0x3BED86;
}
bool reenableDoomAdaptiveRuntimeState(unsigned char* image){
    if(!image)return false;
    using namespace doomAdaptiveTick;
    auto state=*reinterpret_cast<unsigned char* volatile*>(
        image+adaptiveRuntimeStatePointerRva);
    if(!state)return false;
    MEMORY_BASIC_INFORMATION memory{};
    if(!VirtualQuery(state+adaptiveRuntimeEnabledOffset,&memory,sizeof(memory))
        ||memory.State!=MEM_COMMIT||(memory.Protect&PAGE_NOACCESS)
        ||(memory.Protect&PAGE_GUARD))return false;
    auto enabled=reinterpret_cast<volatile char*>(
        state+adaptiveRuntimeEnabledOffset);
    const char previous=_InterlockedCompareExchange8(enabled,0,0);
    if(previous!=0&&previous!=1)return false;
    _InterlockedExchange8(enabled,1);
    if(previous==0){
        static std::atomic<bool> repairedLogged{};
        if(!repairedLogged.exchange(true,std::memory_order_acq_rel))
            log("[CINEMATIC-HZ] stale participant block cleared from native adaptive runtime state");
    }
    return true;
}
bool installImmersiveAdaptiveParticipantBypass(unsigned char* image){
    static bool attempted=false;
    static bool installed=false;
    if(attempted)return installed;
    attempted=true;
    if(!image)return false;
    using namespace doomAdaptiveTick;
    auto target=image+participantBranchRva;
    constexpr unsigned char signature[]{
        0x85,0xC9,0x0F,0x8F,0xB4,0x00,0x00,0x00,
        0x80,0xBF,0x90,0xD4,0x49,0x00,0x00};
    if(std::memcmp(target,signature,sizeof(signature))!=0){
        log("[CINEMATIC-HZ] adaptive-participant branch signature mismatch; bypass disabled safely");
        return false;
    }
    auto stub=reinterpret_cast<unsigned char*>(VirtualAlloc(
        nullptr,128,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    if(!stub){log("[CINEMATIC-HZ] adaptive-participant stub allocation failed");return false;}
    std::vector<unsigned char> code;
    auto byte=[&](unsigned char value){code.push_back(value);};
    auto bytes=[&](std::initializer_list<unsigned char> values){
        code.insert(code.end(),values.begin(),values.end());
    };
    auto qword=[&](uintptr_t value){
        for(unsigned shift=0;shift<64;shift+=8)byte(static_cast<unsigned char>(value>>shift));
    };
    auto absoluteJump=[&](uintptr_t destination){
        bytes({0xFF,0x25,0x00,0x00,0x00,0x00});
        qword(destination);
    };
    bytes({0x85,0xC9});
    const size_t noParticipantsJump=code.size();bytes({0x7E,0x00});
    // Observe the exact native condition which blocks adaptive ticking. This
    // survives checkpoint entries which render through the ordinary gameplay
    // camera and therefore never trip DOOM's generic cutscene flag. Preserve
    // RAX because this is a mid-function hook and the overwritten instructions
    // did not modify any general-purpose register.
    byte(0x50);
    bytes({0x48,0xB8});qword(reinterpret_cast<uintptr_t>(&nativeAdaptiveParticipantObserved));
    bytes({0xC7,0x00,0x01,0x00,0x00,0x00});
    bytes({0x48,0xB8});qword(reinterpret_cast<uintptr_t>(&immersiveAdaptiveParticipantBypass));
    bytes({0x83,0x38,0x00});
    byte(0x58);
    const size_t bypassJump=code.size();bytes({0x75,0x00});
    absoluteJump(reinterpret_cast<uintptr_t>(image+participantBlockedRva));
    const size_t noParticipants=code.size();
    bytes({0x80,0xBF,0x90,0xD4,0x49,0x00,0x00});
    absoluteJump(reinterpret_cast<uintptr_t>(image+participantContinueRva));
    const size_t ignoreCounts=code.size();
    bytes({0x80,0xBF,0x90,0xD4,0x49,0x00,0x00});
    absoluteJump(reinterpret_cast<uintptr_t>(image+participantContinueRva));
    const auto bypassDistance=ignoreCounts-(bypassJump+2);
    const auto noParticipantsDistance=noParticipants-(noParticipantsJump+2);
    if(bypassDistance>127||noParticipantsDistance>127){VirtualFree(stub,0,MEM_RELEASE);return false;}
    code[bypassJump+1]=static_cast<unsigned char>(bypassDistance);
    code[noParticipantsJump+1]=static_cast<unsigned char>(noParticipantsDistance);
    std::memcpy(stub,code.data(),code.size());
    DWORD stubProtection{};
    if(!VirtualProtect(stub,128,PAGE_EXECUTE_READ,&stubProtection)){
        VirtualFree(stub,0,MEM_RELEASE);
        log("[CINEMATIC-HZ] adaptive-participant stub protection failed");
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(),stub,code.size());
    unsigned char patch[sizeof(signature)]{
        0xFF,0x25,0x00,0x00,0x00,0x00};
    const auto stubAddress=reinterpret_cast<uintptr_t>(stub);
    std::memcpy(patch+6,&stubAddress,sizeof(stubAddress));
    patch[14]=0x90;
    DWORD oldProtection{};
    if(!VirtualProtect(target,sizeof(patch),PAGE_EXECUTE_READWRITE,&oldProtection)){
        VirtualFree(stub,0,MEM_RELEASE);
        log("[CINEMATIC-HZ] adaptive-participant branch protection failed");
        return false;
    }
    std::memcpy(target,patch,sizeof(patch));
    FlushInstructionCache(GetCurrentProcess(),target,sizeof(patch));
    DWORD ignored{};VirtualProtect(target,sizeof(patch),oldProtection,&ignored);
    installed=true;
    log("[CINEMATIC-HZ] native adaptive-participant observation/bypass hook installed at RVA 0x3BED77");
    return true;
}
bool validateDoomAdaptiveTickLayout(unsigned char* image){
    if(!image)return false;
    const auto dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE)return false;
    const auto nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(image+dos->e_lfanew);
    if(nt->Signature!=IMAGE_NT_SIGNATURE)return false;
    const auto imageSize=static_cast<uintptr_t>(nt->OptionalHeader.SizeOfImage);
    auto matches=[&](uintptr_t rva,const char* expected){
        const size_t length=std::strlen(expected)+1;
        return rva<imageSize&&length<=imageSize-rva
            &&std::memcmp(image+rva,expected,length)==0;
    };
    using namespace doomAdaptiveTick;
    return matches(adaptiveTickNameRva,"com_adaptiveTick")
        &&matches(minHzNameRva,"com_adaptiveTickMinHz")
        &&matches(maxHzNameRva,"com_adaptiveTickMaxHz")
        &&matches(immediateModeNameRva,"com_adaptiveTickImmediateMode")
        &&matches(syncNoAdaptiveTickNameRva,"sync_noAdaptiveTick")
        &&matches(fixedTicNameRva,"com_fixedTic")
        &&adaptiveRuntimeStatePointerRva+sizeof(void*)<=imageSize
        &&syncNoAdaptiveTickCurrentRva+sizeof(LONG)<=imageSize;
}
bool ensureImmersiveAdaptiveTickSupport(){
    auto& hold=s.cinematicAdaptiveTick;
    if(hold.layoutChecked)return hold.layoutAvailable;
    hold.layoutChecked=true;
    auto image=reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    hold.layoutAvailable=validateDoomAdaptiveTickLayout(image)
        &&installImmersiveAdaptiveParticipantBypass(image);
    if(!hold.layoutAvailable)
        log("[CINEMATIC-HZ] DOOM 6.66 adaptive-tick layout mismatch; refresh hold disabled safely");
    return hold.layoutAvailable;
}
bool consumeNativeAdaptiveParticipantObservation(){
    if(!ensureImmersiveAdaptiveTickSupport())return false;
    return InterlockedExchange(&nativeAdaptiveParticipantObserved,0)!=0;
}
void updateImmersiveCinematicRefresh(bool active,XrDuration displayPeriod){
    s.fastestDisplayPeriod=kharvox::selectFastestPlausibleDisplayPeriod(
        s.fastestDisplayPeriod,displayPeriod);
    auto& hold=s.cinematicAdaptiveTick;
    auto image=reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    using namespace doomAdaptiveTick;
    auto value=[&](uintptr_t rva){return reinterpret_cast<volatile LONG*>(image+rva);};
    auto read=[&](uintptr_t rva){return InterlockedCompareExchange(value(rva),0,0);};
    auto write=[&](uintptr_t rva,LONG replacement){InterlockedExchange(value(rva),replacement);};
    auto restore=[&]{
        if(!hold.active||!image)return;
        write(minHzCurrentRva,hold.savedMinHz);
        write(maxHzCurrentRva,hold.savedMaxHz);
        write(immediateModeCurrentRva,hold.savedImmediateMode);
        write(syncNoAdaptiveTickCurrentRva,hold.savedSyncNoAdaptiveTick);
        write(fixedTicCurrentRva,hold.savedFixedTic);
        write(adaptiveTickCurrentRva,hold.savedAdaptiveTick);
        InterlockedExchange(&immersiveAdaptiveParticipantBypass,0);
        LARGE_INTEGER endedAt{};QueryPerformanceCounter(&endedAt);
        const double elapsedMs=performanceMilliseconds(hold.startedAt,endedAt);
        const double measuredHz=elapsedMs>0.0?double(hold.presents)*1000.0/elapsedMs:0.0;
        std::ostringstream out;
        out<<"[CINEMATIC-HZ] immersive hold released; presents="<<hold.presents
            <<" elapsedMs="<<std::fixed<<std::setprecision(1)<<elapsedMs
            <<" measuredPresentHz="<<std::setprecision(1)<<measuredHz
            <<" restored adaptive/min/max/immediate/syncNoAdaptive/fixedTic="
            <<hold.savedAdaptiveTick<<'/'<<hold.savedMinHz<<'/'<<hold.savedMaxHz<<'/'
            <<hold.savedImmediateMode<<'/'<<hold.savedSyncNoAdaptiveTick<<'/'<<hold.savedFixedTic;
        log(out.str());
        hold.active=false;
        hold.presents=0;
    };
    if(!active){restore();return;}
    if(!ensureImmersiveAdaptiveTickSupport()||!image)return;
    const int detectedTarget=kharvox::targetGameHzForDisplayPeriod(s.fastestDisplayPeriod);
    if(detectedTarget<=0)return;
    if(!hold.active){
        hold.savedAdaptiveTick=read(adaptiveTickCurrentRva);
        hold.savedMinHz=read(minHzCurrentRva);
        hold.savedMaxHz=read(maxHzCurrentRva);
        hold.savedImmediateMode=read(immediateModeCurrentRva);
        hold.savedSyncNoAdaptiveTick=read(syncNoAdaptiveTickCurrentRva);
        hold.savedFixedTic=read(fixedTicCurrentRva);
        const bool valuesValid=(hold.savedAdaptiveTick==0||hold.savedAdaptiveTick==1)
            &&hold.savedMinHz>=1&&hold.savedMinHz<=240
            &&hold.savedMaxHz>=hold.savedMinHz&&hold.savedMaxHz<=1000
            &&(hold.savedImmediateMode==0||hold.savedImmediateMode==1)
            &&(hold.savedSyncNoAdaptiveTick==0||hold.savedSyncNoAdaptiveTick==1)
            &&(hold.savedFixedTic==0||hold.savedFixedTic==1);
        if(!valuesValid){
            hold.layoutAvailable=false;
            log("[CINEMATIC-HZ] adaptive-tick values outside expected ranges; refresh hold disabled safely");
            return;
        }
        hold.targetHz=std::max(detectedTarget,static_cast<int>(hold.savedMinHz));
        hold.presents=0;
        QueryPerformanceCounter(&hold.startedAt);
        hold.active=true;
        log("[CINEMATIC-HZ] immersive hold armed target="+std::to_string(hold.targetHz)
            +" Hz displayPeriodNs="+std::to_string(s.fastestDisplayPeriod)
            +" saved adaptive/min/max/immediate/syncNoAdaptive/fixedTic="
            +std::to_string(hold.savedAdaptiveTick)+"/"+std::to_string(hold.savedMinHz)
            +"/"+std::to_string(hold.savedMaxHz)+"/"+std::to_string(hold.savedImmediateMode)
            +"/"+std::to_string(hold.savedSyncNoAdaptiveTick)+"/"+std::to_string(hold.savedFixedTic));
    }else if(detectedTarget>hold.targetHz){
        hold.targetHz=detectedTarget;
        log("[CINEMATIC-HZ] faster headset cadence detected; target raised to "+std::to_string(hold.targetHz)+" Hz");
    }
    // Reassert on every Present because a sync participant or config refresh
    // may rewrite these current values while the cinematic is running.
    write(maxHzCurrentRva,std::max<LONG>(hold.savedMaxHz,hold.targetHz));
    write(minHzCurrentRva,hold.targetHz);
    write(immediateModeCurrentRva,1);
    write(syncNoAdaptiveTickCurrentRva,0);
    // com_fixedTic is the outer gate for DOOM's adaptive-rate branch. r32 set
    // it to zero, which bypassed the very code that consumes Min/Max Hz. Keep
    // that gate enabled and repair the runtime byte cleared one frame before
    // the participant observer could arm its bypass.
    write(fixedTicCurrentRva,1);
    write(adaptiveTickCurrentRva,1);
    InterlockedExchange(&immersiveAdaptiveParticipantBypass,1);
    if(!reenableDoomAdaptiveRuntimeState(image)){
        static std::atomic<bool> stateFailureLogged{};
        if(!stateFailureLogged.exchange(true,std::memory_order_acq_rel))
            log("[CINEMATIC-HZ] native adaptive runtime state unavailable; CVar-only hold retained");
    }
    ++hold.presents;
}
DWORD WINAPI kharvoxXInputGetState(DWORD userIndex,XINPUT_STATE* state){DWORD result=originalXInputGetState?originalXInputGetState(userIndex,state):ERROR_DEVICE_NOT_CONNECTED;if(userIndex!=0||!state)return result;if(result!=ERROR_SUCCESS)ZeroMemory(state,sizeof(*state));state->dwPacketNumber=virtualPacket.load(std::memory_order_relaxed);state->Gamepad.sThumbLX=virtualLeftX.load(std::memory_order_relaxed);state->Gamepad.sThumbLY=virtualLeftY.load(std::memory_order_relaxed);state->Gamepad.sThumbRX=virtualRightX.load(std::memory_order_relaxed);state->Gamepad.sThumbRY=virtualRightY.load(std::memory_order_relaxed);state->Gamepad.bLeftTrigger=std::max(state->Gamepad.bLeftTrigger,virtualLeftTrigger.load(std::memory_order_relaxed));state->Gamepad.bRightTrigger=std::max(state->Gamepad.bRightTrigger,virtualRightTrigger.load(std::memory_order_relaxed));state->Gamepad.wButtons|=virtualButtons.load(std::memory_order_relaxed);return ERROR_SUCCESS;}
void retainPendingXInputRumblePeak(uint32_t packed){
    if(!packed)return;
    auto observed=pendingXInputRumblePeaks.load(std::memory_order_relaxed);
    for(;;){
        const auto merged=kharvox::mergeXInputRumblePeaks(observed,packed);
        if(merged==observed)return;
        if(pendingXInputRumblePeaks.compare_exchange_weak(
            observed,merged,std::memory_order_release,std::memory_order_relaxed))return;
    }
}
DWORD WINAPI kharvoxXInputSetState(DWORD userIndex,XINPUT_VIBRATION* vibration){
    // Preserve the original XInput side effect first. Everything below is an
    // additive mirror/capture path and must never replace controller rumble.
    const DWORD originalResult=originalXInputSetState
        ?originalXInputSetState(userIndex,vibration):ERROR_DEVICE_NOT_CONNECTED;
    if(userIndex!=0)return originalResult;
    const uint32_t packed=vibration?kharvox::packXInputRumble(
        vibration->wLeftMotorSpeed,vibration->wRightMotorSpeed):0;
    nativeXInputRumble.store(packed,std::memory_order_release);
    retainPendingXInputRumblePeak(packed);
    KharvoxBhapticsSubmitRumble(
        kharvox::xinputRumbleLowMotor(packed),
        kharvox::xinputRumbleHighMotor(packed));
    return originalResult==ERROR_DEVICE_NOT_CONNECTED?ERROR_SUCCESS:originalResult;
}
bool installXInputHook(){
    KharvoxBhapticsIpcStart();
    KharvoxPsvr2IpcStart();
    if(xinputHookReady&&xinputSetStateHookReady)return true;
    auto module=reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    if(!module)return false;
    auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(module);
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE)return false;
    auto nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(module+dos->e_lfanew);
    if(nt->Signature!=IMAGE_NT_SIGNATURE)return false;
    const auto directory=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if(!directory.VirtualAddress)return false;
    auto patchImport=[](IMAGE_THUNK_DATA64*address,ULONGLONG replacement){
        if(address->u1.Function==replacement)return true;
        DWORD oldProtect{};
        if(!VirtualProtect(&address->u1.Function,sizeof(address->u1.Function),PAGE_READWRITE,&oldProtect))return false;
        address->u1.Function=replacement;
        DWORD ignored{};
        VirtualProtect(&address->u1.Function,sizeof(address->u1.Function),oldProtect,&ignored);
        FlushInstructionCache(GetCurrentProcess(),&address->u1.Function,sizeof(address->u1.Function));
        return true;
    };
    for(auto descriptor=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(module+directory.VirtualAddress);descriptor->Name;descriptor++){
        const char*library=reinterpret_cast<const char*>(module+descriptor->Name);
        if(_stricmp(library,"XINPUT1_4.dll"))continue;
        auto names=reinterpret_cast<IMAGE_THUNK_DATA64*>(module+(descriptor->OriginalFirstThunk?descriptor->OriginalFirstThunk:descriptor->FirstThunk));
        auto addresses=reinterpret_cast<IMAGE_THUNK_DATA64*>(module+descriptor->FirstThunk);
        for(;names->u1.AddressOfData;names++,addresses++){
            if(!IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal))continue;
            const auto ordinal=IMAGE_ORDINAL64(names->u1.Ordinal);
            if(ordinal==2&&!xinputHookReady){
                originalXInputGetState=reinterpret_cast<XInputGetStateFn>(addresses->u1.Function);
                if(patchImport(addresses,reinterpret_cast<ULONGLONG>(kharvoxXInputGetState))){
                    xinputHookReady=true;
                }else originalXInputGetState=nullptr;
            }else if(ordinal==3&&!xinputSetStateHookReady){
                originalXInputSetState=reinterpret_cast<XInputSetStateFn>(addresses->u1.Function);
                if(patchImport(addresses,reinterpret_cast<ULONGLONG>(kharvoxXInputSetState))){
                    xinputSetStateHookReady=true;
                }else originalXInputSetState=nullptr;
            }
        }
        break;
    }
    if(xinputHookReady)log("[INPUT] XInputGetState IAT hook installed; analog OpenXR gamepad active");
    else log("[INPUT] XInputGetState IAT hook unavailable; keyboard movement fallback active");
    if(xinputSetStateHookReady)log("[HAPTICS] XInputSetState IAT hook installed; native DOOM rumble capture active");
    else log("[HAPTICS] XInputSetState IAT hook unavailable; OpenXR rumble disabled");
    return xinputHookReady;
}
std::string pointerOwner(PFN_vkVoidFunction p){if(!p)return "NULL";HMODULE module{};if(!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCSTR>(p),&module))return "unknown";char path[MAX_PATH]{};if(!GetModuleFileNameA(module,path,MAX_PATH))return "unknown";const char* base=strrchr(path,'\\');return base?base+1:path;}
VKAPI_ATTR void VKAPI_CALL bridgeGetPhysicalDeviceProperties2(VkPhysicalDevice p,VkPhysicalDeviceProperties2*out){log("[VD] ENTER vkGetPhysicalDeviceProperties2 physical="+std::to_string(reinterpret_cast<uintptr_t>(p)));bridgeGetPhysicalDeviceProperties2Downstream(p,out);log("[VD] EXIT vkGetPhysicalDeviceProperties2");}
VKAPI_ATTR void VKAPI_CALL bridgeGetPhysicalDeviceMemoryProperties(VkPhysicalDevice p,VkPhysicalDeviceMemoryProperties*out){log("[VD] ENTER vkGetPhysicalDeviceMemoryProperties physical="+std::to_string(reinterpret_cast<uintptr_t>(p)));bridgeGetPhysicalDeviceMemoryPropertiesDownstream(p,out);log("[VD] EXIT vkGetPhysicalDeviceMemoryProperties heaps="+std::to_string(out?out->memoryHeapCount:0));}
bool isDeviceCommand(const char*n){static const char* names[]={"vkGetImageMemoryRequirements2KHR","vkGetDeviceQueue","vkQueueSubmit","vkCreateImage","vkDestroyImage","vkAllocateMemory","vkFreeMemory","vkCreateCommandPool","vkDestroyCommandPool","vkAllocateCommandBuffers","vkFreeCommandBuffers","vkResetCommandBuffer","vkBeginCommandBuffer","vkCmdPipelineBarrier","vkCmdResetQueryPool","vkCmdWriteTimestamp","vkEndCommandBuffer","vkGetMemoryWin32HandlePropertiesKHR","vkBindImageMemory","vkCreateSemaphore","vkDestroySemaphore","vkImportSemaphoreWin32HandleKHR","vkCreateFence","vkDestroyFence","vkResetFences","vkWaitForFences","vkDeviceWaitIdle","vkCreateQueryPool","vkDestroyQueryPool","vkGetQueryPoolResults"};if(!n)return false;for(auto name:names)if(!strcmp(n,name))return true;return false;}
bool isPhysicalCommand(const char*n){return n&&(!strncmp(n,"vkGetPhysicalDevice",19)||!strcmp(n,"vkEnumerateDeviceExtensionProperties")||!strcmp(n,"vkEnumerateDeviceLayerProperties"));}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL bridgeGipa(VkInstance i,const char*n){if(bridgeSessionTrace)log(std::string("[VD-BRIDGE] ENTER GIPA(instance=")+std::to_string(reinterpret_cast<uintptr_t>(i))+", name="+(n?n:"NULL")+")");PFN_vkVoidFunction p{};if(n&&!strcmp(n,"vkCreateDevice")&&reentryCreateDevice)p=reinterpret_cast<PFN_vkVoidFunction>(bridgeCreateDevice);else if(isDeviceCommand(n)&&s.vk.getDeviceProcAddr&&s.device)p=s.vk.getDeviceProcAddr(s.device,n);else if(isPhysicalCommand(n)){auto loader=GetModuleHandleW(L"vulkan-1.dll");auto loaderGipa=reinterpret_cast<PFN_vkGetInstanceProcAddr>(loader?GetProcAddress(loader,"vkGetInstanceProcAddr"):nullptr);if(loaderGipa)p=loaderGipa(i,n);}else if(bridgeNextGipa)p=bridgeNextGipa(i,n);PFN_vkVoidFunction returned=p;if(bridgeSessionTrace&&n&&!strcmp(n,"vkGetPhysicalDeviceProperties2")){bridgeGetPhysicalDeviceProperties2Downstream=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(p);returned=reinterpret_cast<PFN_vkVoidFunction>(bridgeGetPhysicalDeviceProperties2);}else if(bridgeSessionTrace&&n&&!strcmp(n,"vkGetPhysicalDeviceMemoryProperties")){bridgeGetPhysicalDeviceMemoryPropertiesDownstream=reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(p);returned=reinterpret_cast<PFN_vkVoidFunction>(bridgeGetPhysicalDeviceMemoryProperties);}if(bridgeSessionTrace)log(std::string("[VD-BRIDGE] EXIT GIPA name=")+(n?n:"NULL")+" downstream="+std::to_string(reinterpret_cast<uintptr_t>(p))+" module="+pointerOwner(p)+" returned="+std::to_string(reinterpret_cast<uintptr_t>(returned)));return returned;}
std::string result(XrResult r){char b[XR_MAX_RESULT_STRING_SIZE]{};using Fn=XrResult(XRAPI_PTR*)(XrInstance,XrResult,char[XR_MAX_RESULT_STRING_SIZE]);Fn f=nullptr;if(s.instance&&s.getProc)s.getProc(s.instance,"xrResultToString",reinterpret_cast<PFN_xrVoidFunction*>(&f));if(f&&XR_SUCCEEDED(f(s.instance,r,b)))return b;return std::to_string(r);}
void clearCapturedXInputRumble(){
    nativeXInputRumble.store(0,std::memory_order_release);
    pendingXInputRumblePeaks.store(0,std::memory_order_release);
    kharvox::resetXInputRumbleFrameAccumulator(s.hapticFrameAccumulator);
    s.weaponFireHapticFallbackUntilTick=0;
    s.wheelClicks={};
}
void clearXInputHapticState(){
    clearCapturedXInputRumble();
    for(auto&state:s.hapticOutputStates)
        kharvox::resetXInputHapticOutputState(state);
}
bool psvr2ToolkitRequested(){
    char value[8]{};
    return GetEnvironmentVariableA("KHARVOX_USE_PSVR2_TOOLKIT",
        value,sizeof(value))>0&&!strcmp(value,"1");
}
kharvox::psvr2::TriggerOffReason psvr2InactiveReason(){
    using kharvox::psvr2::TriggerOffReason;
    if(KharvoxHudDeathMenuActive())return TriggerOffReason::Death;
    if(KharvoxHudPauseMenuActive()||KharvoxHudFullscreenMenuActive()
        ||KharvoxHudEndOfLevelMenuActive())return TriggerOffReason::Menu;
    if(KharvoxCameraCutsceneActive()
        &&!KharvoxCameraPlayerWeaponControlActive())
        return TriggerOffReason::Cinematic;
    return TriggerOffReason::Loading;
}
void updatePsvr2TriggerPolicy(){
    const bool menu=KharvoxHudPauseMenuActive()
        ||KharvoxHudFullscreenMenuActive()||KharvoxHudDeathMenuActive()
        ||KharvoxHudEndOfLevelMenuActive();
    const bool passiveCinematic=KharvoxCameraCutsceneActive()
        &&!KharvoxCameraPlayerWeaponControlActive();
    const bool gameplay=KharvoxCameraGameplayActive()
        &&KharvoxCameraWorldActive()&&!menu&&!passiveCinematic;
    const auto weapon=KharvoxWeaponCurrentKind();
    const auto ammoState=KharvoxWeaponGetAmmoState(weapon);
    const bool fireSequenceValid=psvr2ToolkitRequested()
        &&kharvox::isSteamBackedOpenXRRuntime(s.runtimeKind)
        &&s.sessionState==XR_SESSION_STATE_FOCUSED
        &&gameplay&&s.primaryFireDown
        &&ammoState!=KharvoxWeaponAmmoState::Empty
        &&ammoState!=KharvoxWeaponAmmoState::Unavailable;
    const auto now=GetTickCount64();
    if(fireSequenceValid){
        if(!s.psvr2FireStartedTick||s.psvr2FireWeapon!=weapon){
            s.psvr2FireStartedTick=now;
            s.psvr2FireWeapon=weapon;
        }
    }else{
        s.psvr2FireStartedTick=0;
        s.psvr2FireWeapon=KharvoxWeaponKind::Unknown;
    }
    const std::uint64_t fireHeldMilliseconds=s.psvr2FireStartedTick
        ?now-s.psvr2FireStartedTick:0;
    const kharvox::psvr2::TriggerPolicyInput input{
        psvr2ToolkitRequested(),
        kharvox::isSteamBackedOpenXRRuntime(s.runtimeKind),
        s.sessionState==XR_SESSION_STATE_FOCUSED,
        gameplay,
        weapon,
        s.primaryFireDown,
        fireHeldMilliseconds,
        ammoState,
        s.leftHanded,
        psvr2InactiveReason()};
    KharvoxPsvr2SubmitTrigger(kharvox::psvr2::selectTriggerCommand(input));
}
void logHapticError(const std::string&message){
    ++s.hapticErrorCount;
    const auto now=GetTickCount64();
    if(s.hapticErrorCount<=3||now>=s.nextHapticErrorLogTick){
        log("[HAPTICS] "+message+" count="+std::to_string(s.hapticErrorCount));
        s.nextHapticErrorLogTick=now+5000;
    }
}
bool hapticCallApplied(XrResult callResult){
    return callResult==XR_SUCCESS;
}
void updateXInputHaptics(){
    if(!s.session)return;
    if(s.sessionState!=XR_SESSION_STATE_FOCUSED){
        s.wheelClicks={};
        s.motionWheelState={};
        s.wheelStickHaptics={};
        pendingXInputRumblePeaks.store(0,std::memory_order_release);
        kharvox::resetXInputRumbleFrameAccumulator(s.hapticFrameAccumulator);
        KharvoxBhapticsSubmitRumble(0,0);
        return;
    }

    const auto now=GetTickCount64();
    const uint32_t current=nativeXInputRumble.load(std::memory_order_acquire);
    const uint32_t pending=pendingXInputRumblePeaks.exchange(
        0,std::memory_order_acq_rel);
    const uint32_t packed=kharvox::selectXInputRumbleForFrame(
        s.hapticFrameAccumulator,current,pending,now);
    const auto nativeSignal=kharvox::mapXInputRumble(
        kharvox::xinputRumbleLowMotor(packed),
        kharvox::xinputRumbleHighMotor(packed));
    const bool weaponFireFallbackActive=
        now<s.weaponFireHapticFallbackUntilTick;
    const auto fanout=kharvox::selectAdditiveHapticFanout(
        packed,nativeSignal,weaponFireFallbackActive);
    KharvoxBhapticsSubmitRumble(
        kharvox::xinputRumbleLowMotor(fanout.bhapticsRumble),
        kharvox::xinputRumbleHighMotor(fanout.bhapticsRumble));

    if(!s.actionsReady||!s.hapticActionsReady||!s.hapticBindingsSuggested
        ||!s.applyHapticFeedback||!s.stopHapticFeedback)return;

    const bool weaponRumble=fanout.controllerSignal.active
        &&weaponFireFallbackActive;
    // Explicit fire input identifies weapon rumble, unlike XInput's motor
    // channels themselves. One-hand fire targets the configured weapon hand;
    // an acquired two-hand grip deliberately keeps the mirrored pair.
    const auto nativeDesired=kharvox::routeXInputHapticSignal(
        fanout.controllerSignal,weaponRumble,s.leftHanded,s.twoHandLatched);

    const std::array<XrAction,2> actions{s.leftHaptic,s.rightHaptic};
    for(size_t hand=0;hand<actions.size();++hand){
        const auto desired=kharvox::mixControllerClick(nativeDesired[hand],s.wheelClicks[hand],now);
        auto&outputState=s.hapticOutputStates[hand];
        const auto command=kharvox::selectXInputHapticCommand(
            outputState,desired,now);
        if(command==kharvox::XInputHapticCommand::None)continue;
        XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
        info.action=actions[hand];
        const char*handName=hand==0?"left":"right";
        if(command==kharvox::XInputHapticCommand::Stop){
            const XrResult stopResult=s.stopHapticFeedback(s.session,&info);
            if(stopResult!=XR_SUCCESS)
                logHapticError(std::string("stop ")+handName+'='+result(stopResult));
            kharvox::noteXInputHapticStopAttempted(outputState);
            continue;
        }

        XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
        const auto durationMilliseconds=kharvox::controllerHapticDurationMilliseconds(
            nativeDesired[hand],s.wheelClicks[hand],now);
        vibration.duration=static_cast<XrDuration>(
            durationMilliseconds)*1000000;
        vibration.frequency=s.hapticFrequencyUnspecified
            ?XR_FREQUENCY_UNSPECIFIED:desired.frequencyHz;
        vibration.amplitude=desired.amplitude;
        auto applyResult=s.applyHapticFeedback(
            s.session,&info,
            reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
        if(!hapticCallApplied(applyResult)&&!s.hapticFrequencyUnspecified){
            s.hapticFrequencyUnspecified=true;
            log("[HAPTICS] first frequency-bearing apply failed; using OpenXR unspecified-frequency fallback");
            vibration.frequency=XR_FREQUENCY_UNSPECIFIED;
            applyResult=s.applyHapticFeedback(
                s.session,&info,
                reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
        }
        if(hapticCallApplied(applyResult)){
            kharvox::noteXInputHapticApplySucceeded(
                outputState,desired,now,durationMilliseconds);
            if(!s.firstControllerHapticAppliedLogged){
                log("[HAPTICS] first OpenXR controller rumble applied; bHaptics mirror is additive");
                s.firstControllerHapticAppliedLogged=true;
            }
        }else{
            logHapticError(std::string("apply ")+handName+'='+result(applyResult));
            kharvox::noteXInputHapticApplyFailed(outputState,now);
        }
    }
}
XrQuaternionf conjugate(XrQuaternionf q){return {-q.x,-q.y,-q.z,q.w};}
XrQuaternionf multiply(XrQuaternionf a,XrQuaternionf b){return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};}
XrQuaternionf normalizeQuaternion(XrQuaternionf q){const float length=std::sqrt(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w);if(length<=.000001f)return {0,0,0,1};const float inverse=1.f/length;return {q.x*inverse,q.y*inverse,q.z*inverse,q.w*inverse};}
float quaternionAngularDistanceDegrees(XrQuaternionf a,XrQuaternionf b){a=normalizeQuaternion(a);b=normalizeQuaternion(b);const float dot=std::clamp(std::abs(a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w),0.0f,1.0f);return 2.0f*std::acos(dot)*57.2957795131f;}
XrVector3f rotateVector(XrQuaternionf q,XrVector3f v){XrQuaternionf p{v.x,v.y,v.z,0};auto r=multiply(multiply(q,p),conjugate(q));return {r.x,r.y,r.z};}
XrVector3f addVector(XrVector3f a,XrVector3f b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
XrVector3f subtractVector(XrVector3f a,XrVector3f b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
XrPosef poseRelativeTo(XrPosef parent,XrPosef child){const XrQuaternionf inverse=conjugate(normalizeQuaternion(parent.orientation));XrPosef relative{};relative.orientation=normalizeQuaternion(multiply(inverse,child.orientation));relative.position=rotateVector(inverse,subtractVector(child.position,parent.position));return relative;}
XrVector3f scaleVector(XrVector3f value,float scale){return {value.x*scale,value.y*scale,value.z*scale};}
float dotVector(XrVector3f a,XrVector3f b){return a.x*b.x+a.y*b.y+a.z*b.z;}
XrVector3f crossVector(XrVector3f a,XrVector3f b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
float vectorLength(XrVector3f value){return std::sqrt(dotVector(value,value));}
XrVector3f normalizeVector(XrVector3f value,XrVector3f fallback={0,0,-1}){const float length=vectorLength(value);return length>.00001f?scaleVector(value,1.f/length):fallback;}
XrQuaternionf quaternionFromForwardUp(XrVector3f forward,XrVector3f upReference){
    forward=normalizeVector(forward);
    XrVector3f right=normalizeVector(crossVector(forward,upReference),{1,0,0});
    XrVector3f up=normalizeVector(crossVector(right,forward),{0,1,0});
    const XrVector3f back=scaleVector(forward,-1.f);
    const float m00=right.x,m01=up.x,m02=back.x;
    const float m10=right.y,m11=up.y,m12=back.y;
    const float m20=right.z,m21=up.z,m22=back.z;
    XrQuaternionf result{};
    const float trace=m00+m11+m22;
    if(trace>0.f){const float scale=std::sqrt(trace+1.f)*2.f;result.w=.25f*scale;result.x=(m21-m12)/scale;result.y=(m02-m20)/scale;result.z=(m10-m01)/scale;}
    else if(m00>m11&&m00>m22){const float scale=std::sqrt(1.f+m00-m11-m22)*2.f;result.w=(m21-m12)/scale;result.x=.25f*scale;result.y=(m01+m10)/scale;result.z=(m02+m20)/scale;}
    else if(m11>m22){const float scale=std::sqrt(1.f+m11-m00-m22)*2.f;result.w=(m02-m20)/scale;result.x=(m01+m10)/scale;result.y=.25f*scale;result.z=(m12+m21)/scale;}
    else{const float scale=std::sqrt(1.f+m22-m00-m11)*2.f;result.w=(m10-m01)/scale;result.x=(m02+m20)/scale;result.y=(m12+m21)/scale;result.z=.25f*scale;}
    return normalizeQuaternion(result);
}
float wrapDegrees(float value){while(value>180.f)value-=360.f;while(value<-180.f)value+=360.f;return value;}
XrQuaternionf yawQuaternion(float degrees){constexpr float degreesToRadians=.01745329251994329577f;const float half=.5f*degrees*degreesToRadians;return {0.f,std::sin(half),0.f,std::cos(half)};}
float trackingYawDegrees(XrQuaternionf q){constexpr float radiansToDegrees=57.2957795131f;return std::atan2(2.f*(q.w*q.y+q.x*q.z),1.f-2.f*(q.x*q.x+q.y*q.y))*radiansToDegrees;}
XrQuaternionf yawOnlyTrackingBaseline(XrQuaternionf q){return yawQuaternion(trackingYawDegrees(q));}
float bodyYawDegrees(const float axis[9]){constexpr float radiansToDegrees=57.2957795131f;return std::atan2(axis[1],axis[0])*radiansToDegrees;}
float physicalResidualHeadYaw(){return wrapDegrees(s.head.yaw-s.acceptedPhysicalYaw);}
float artificialTurnVisualYaw(){return s.artificialTurnActive?s.artificialTurnRemainingDegrees:0.f;}
float renderResidualHeadYaw(){return wrapDegrees(physicalResidualHeadYaw()+artificialTurnVisualYaw());}
void publishArtificialTurnYaw(){s.artificialTurnPublishedDegrees=artificialTurnVisualYaw();KharvoxCameraSetArtificialTurnYaw(s.artificialTurnPublishedDegrees);}
void updateAcceptedPhysicalYaw(bool gameplay,bool bodyPoseValid,const float bodyAxis[9],XrTime displayTime){
    if(!bodyPoseValid){s.previousBodyYawValid=false;s.bodyFollowTurnX=0.f;s.previousManualTurnActive=false;return;}
    const float currentBodyYaw=bodyYawDegrees(bodyAxis);
    if(s.previousBodyYawValid){
        const float delta=wrapDegrees(currentBodyYaw-s.previousBodyYaw);
        const auto cinematicGuard=kharvox::updatePostCinematicYawGuard(
            s.postCinematicYawGuard,displayTime,delta,true);
        if(gameplay&&cinematicGuard.absorbBodyDelta){
            // DOOM may return to its gameplay camera caller several seconds
            // before a scripted animation has finished blending player yaw.
            // Treat those native body-yaw samples exactly like hidden body
            // catch-up: move the accepted tracking-space yaw by the same delta
            // so the rendered world does not inherit the scripted spin.
            s.acceptedPhysicalYaw=wrapDegrees(s.acceptedPhysicalYaw+delta);
        }else if(gameplay&&s.previousManualTurnActive&&s.artificialTurnActive){
            s.artificialTurnAccumulatedDegrees+=delta;
            s.artificialTurnRemainingDegrees=wrapDegrees(
                s.artificialTurnRemainingDegrees-delta);
            if(std::abs(s.artificialTurnRemainingDegrees)<.15f){
                s.artificialTurnActive=false;
                s.artificialTurnRemainingDegrees=0.f;
            }
        }else if(gameplay&&!s.previousManualTurnActive&&std::abs(s.bodyFollowTurnX)>.001f){
            s.acceptedPhysicalYaw=wrapDegrees(s.acceptedPhysicalYaw+delta);
            if(std::abs(delta)>.01f&&!s.bodyYawStickSignConfirmed){
                const float measuredSign=delta*s.bodyFollowTurnX>=0.f?1.f:-1.f;
                s.bodyYawPerPositiveStick=measuredSign;s.bodyYawStickSignConfirmed=true;
                log("[BODY-YAW] confirmed native right-stick/body-yaw sign="+std::to_string(measuredSign));
            }
        }
        if(cinematicGuard.exit!=kharvox::PostCinematicYawGuardExit::None){
            std::ostringstream o;
            o<<"[BODY-YAW] post-cinematic native yaw guard released reason="
             <<(cinematicGuard.exit==kharvox::PostCinematicYawGuardExit::Stable?"stable":"timeout")
             <<" absorbed="<<s.postCinematicYawGuard.absorbedDegrees<<"deg";
            log(o.str());
        }
    }
    s.previousBodyYaw=currentBodyYaw;s.previousBodyYawValid=true;
    if(!gameplay){s.bodyFollowTurnX=0.f;s.previousManualTurnActive=false;}
}
XrFovf enclosingSymmetricFov(const XrFovf& fov){const float halfX=kharvox::centeredProjectionHalfAngle(fov.angleLeft,fov.angleRight),halfY=kharvox::centeredProjectionHalfAngle(fov.angleDown,fov.angleUp);return {-halfX,halfX,halfY,-halfY};}
XrFovf submittedEyeFov(const XrFovf& fov){return kharvox::useCenteredProjectionFov(s.runtimeKind)?enclosingSymmetricFov(fov):fov;}
EyeRenderProjection eyeRenderProjection(const XrFovf& fov){EyeRenderProjection projection{};projection.symmetricFov=enclosingSymmetricFov(fov);return projection;}
HeadPose ConvertOpenXRToDoom(const XrPosef& pose){HeadPose h{};h.orientation=pose.orientation;h.position=pose.position;auto q=s.headZeroValid?multiply(conjugate(s.headZero),pose.orientation):pose.orientation;const float sinp=2.f*(q.w*q.x-q.z*q.y);h.pitch=std::asin(std::clamp(sinp,-1.f,1.f));h.yaw=std::atan2(2.f*(q.w*q.y+q.x*q.z),1.f-2.f*(q.x*q.x+q.y*q.y));h.roll=std::atan2(2.f*(q.w*q.z+q.x*q.y),1.f-2.f*(q.x*q.x+q.z*q.z));constexpr float radToDeg=57.2957795131f;h.yaw*=radToDeg;h.pitch*=radToDeg;h.roll*=radToDeg;h.valid=true;return h;}
void resetBodyFollow(){s.bodyFollowPosition={};s.roomscaleStick={};s.previousPhysicsOriginValid=false;s.cinematicBodyPoseHeld=false;s.previousRoomscaleCommand=false;kharvox::resetPostCinematicYawGuard(s.postCinematicYawGuard);}
void publishGameCameraHeadPose(
    float yaw, float pitch, float roll,
    float forward, float lateral, float up,
    bool valid, bool centeredNativeMenuActive) {
    // Keep OpenXR head tracking and the Cinewindow Quad live, but publish one
    // neutral 6DoF pose while a native station dialog is open. Field Drone,
    // Suit/Player Upgrade and Rune Select author their menu camera around a
    // centered view; removing activation-time HMD rotation/translation
    // prevents edge crop.
    if(valid&&!kharvox::shouldPublishGameCameraHeadPose(centeredNativeMenuActive)){
        if(kharvox::shouldCenterNativeMenuGameCamera(
                centeredNativeMenuActive,s.nativeMenuCameraPoseHeld)){
            KharvoxCameraSetHeadPose(0,0,0,0,0,0,true);
            log("[NATIVE-MENU] game-camera centered at neutral 6DoF pose and locked for menu");
        }
        s.nativeMenuCameraPoseHeld=true;
        return;
    }
    if(s.nativeMenuCameraPoseHeld){
        s.nativeMenuCameraPoseHeld=false;
        log(valid
            ? "[NATIVE-MENU] game-camera pose released after menu end"
            : "[NATIVE-MENU] game-camera pose cleared after tracking loss");
    }
    // previousBodyYaw is the exact body sample used by updateAcceptedPhysicalYaw
    // to form this residual. Carry that reference with the head snapshot.
    KharvoxCameraSetHeadPose(yaw,pitch,roll,forward,lateral,up,valid,
        s.previousBodyYaw,s.previousBodyYawValid&&KharvoxCameraGameplayActive()
            &&!KharvoxCameraCutsceneActive());
}
void updateHeadPose(const XrViewState& viewState,XrTime displayTime,bool nativePackedStereoAtFrame){
    const bool fieldDroneMenuActive=KharvoxHudFieldDroneMenuActive();
    const bool suitUpgradeMenuActive=KharvoxHudSuitUpgradeMenuActive();
    const bool playerUpgradeMenuActive=KharvoxHudPlayerUpgradeMenuActive();
    const bool runeSelectMenuActive=KharvoxHudRuneSelectMenuActive();
    // Full-screen native upgrade and Rune selection menus use the same neutral
    // camera lock as Field Drone. Tutorial overlays deliberately retain the
    // normal tracked camera and never enter this centered Quad path.
    const bool centeredNativeMenuActive=fieldDroneMenuActive
        ||suitUpgradeMenuActive||playerUpgradeMenuActive||runeSelectMenuActive;
    const bool orientationValid=(viewState.viewStateFlags&XR_VIEW_STATE_ORIENTATION_VALID_BIT)!=0;
    const bool positionValid=(viewState.viewStateFlags&XR_VIEW_STATE_POSITION_VALID_BIT)!=0;
    if(!orientationValid){resetBodyFollow();s.head.valid=false;s.trackingHeadPositionValid=false;s.previousBodyYawValid=false;s.bodyFollowTurnX=0.f;s.previousManualTurnActive=false;s.snapTurnActivationPending=false;s.artificialTurnActive=false;s.artificialTurnRemainingDegrees=0.f;s.artificialTurnTotalDegrees=0.f;publishArtificialTurnYaw();publishGameCameraHeadPose(0,0,0,0,0,0,false,centeredNativeMenuActive);return;}
    const auto raw=s.views[0].pose.orientation;
    const XrVector3f midpoint{(s.views[0].pose.position.x+s.views[1].pose.position.x)*.5f,(s.views[0].pose.position.y+s.views[1].pose.position.y)*.5f,(s.views[0].pose.position.z+s.views[1].pose.position.z)*.5f};
    s.trackingHeadPositionValid=positionValid;
    if(positionValid)s.trackingHeadPosition=midpoint;
    if(!s.headZeroValid){s.headZero=yawOnlyTrackingBaseline(raw);s.headZeroValid=true;s.headZeroPositionValid=positionValid;if(positionValid)s.headZeroPosition=midpoint;log("[HEAD] yaw-only tracking baseline acquired; pitch/roll remain gravity-level; F8 recenter disabled");}
    const bool nativeGameplayActive=KharvoxCameraGameplayActive();
    if(!nativeGameplayActive)s.gameplayRecenterPending=true;
    if(nativeGameplayActive&&s.gameplayRecenterPending){
        s.headZero=yawOnlyTrackingBaseline(raw);s.headZeroValid=true;
        if(positionValid){s.headZeroPosition=midpoint;s.headZeroPositionValid=true;}
        s.acceptedPhysicalYaw=0.f;s.previousBodyYawValid=false;s.bodyFollowTurnX=0.f;s.previousManualTurnActive=false;s.snapTurnActivationPending=false;s.artificialTurnActive=false;s.artificialTurnRemainingDegrees=0.f;s.artificialTurnTotalDegrees=0.f;s.previousTurnDisplayTime=0;
        publishArtificialTurnYaw();
        resetBodyFollow();s.weaponCalibrated=false;s.weaponTrackingHeld=false;
        if(s.weapon6Dof)KharvoxWeaponResetCalibration();
        s.gameplayRecenterPending=false;
        log("[HEAD] gameplay/load entry rebased tracking yaw only; pitch/roll remain aligned to the OpenXR horizon");
    }
    const bool worldActive=nativeGameplayActive
        &&!KharvoxHudPauseMenuActive()&&!KharvoxHudDeathMenuActive()
        &&!centeredNativeMenuActive;
    if(positionValid&&(!s.headZeroPositionValid||!worldActive)){s.headZeroPosition=midpoint;s.headZeroPositionValid=true;}
    s.head=ConvertOpenXRToDoom(s.views[0].pose);
    // HUD17 steep-pitch control: do not alter the pose. Record the Euler
    // branch and presentation state at the reported down-look boundary so a
    // genuine HUD11 run can distinguish camera singularity from an old HUD
    // transform or a Projection/Quad transition.
    static bool steepPitchActive{};
    const float absolutePitch=std::abs(s.head.pitch);
    if(worldActive&&!steepPitchActive&&absolutePitch>=65.0f){
        steepPitchActive=true;
        log("[HUD17-PITCH] ENTER steep head pitch pitch="+std::to_string(s.head.pitch)
            +" yaw="+std::to_string(s.head.yaw)+" roll="+std::to_string(s.head.roll)
            +" presentation="+(s.quadMode?"QUAD":"PROJECTION"));
    }else if(steepPitchActive&&(!worldActive||absolutePitch<=55.0f)){
        log("[HUD17-PITCH] EXIT steep head pitch pitch="+std::to_string(s.head.pitch)
            +" yaw="+std::to_string(s.head.yaw)+" roll="+std::to_string(s.head.roll)
            +" presentation="+(s.quadMode?"QUAD":"PROJECTION"));
        steepPitchActive=false;
    }
    XrVector3f rawLocalPosition{};
    if(worldActive&&positionValid&&s.headZeroPositionValid){const XrVector3f delta{midpoint.x-s.headZeroPosition.x,midpoint.y-s.headZeroPosition.y,midpoint.z-s.headZeroPosition.z};rawLocalPosition=rotateVector(conjugate(s.headZero),delta);}
    if(!worldActive||!positionValid){
        resetBodyFollow();s.previousBodyYawValid=false;s.bodyFollowTurnX=0.f;s.previousManualTurnActive=false;s.snapTurnActivationPending=false;s.artificialTurnActive=false;s.artificialTurnRemainingDegrees=0.f;
        publishArtificialTurnYaw();
        if(!nativeGameplayActive)s.acceptedPhysicalYaw=0.f;
        const float yaw=nativeGameplayActive?physicalResidualHeadYaw():s.head.yaw;
        publishGameCameraHeadPose(yaw,s.head.pitch,s.head.roll,0,0,0,s.headCameraArmed,centeredNativeMenuActive);return;
    }
    float bodyOrigin[3]{},bodyAxis[9]{};
    const bool bodyPoseValid=KharvoxCameraGetBodyPose(bodyOrigin,bodyAxis);
    const bool cutsceneActive=KharvoxCameraCutsceneActive();
    const bool yawFollowGameplay=bodyPoseValid&&!cutsceneActive;
    if(cutsceneActive&&!s.cinematicBodyPoseHeld){
        s.cinematicBodyPoseHeld=true;
        kharvox::resetPostCinematicYawGuard(s.postCinematicYawGuard);
        log("[BODY] cinematic entry; accepted physical yaw retained");
    }
    if(yawFollowGameplay&&s.cinematicBodyPoseHeld){
        s.cinematicBodyPoseHeld=false;
        kharvox::armPostCinematicYawGuard(s.postCinematicYawGuard,displayTime);
        s.bodyFollowTurnX=0.f;
        s.previousManualTurnActive=false;
        log("[BODY] cinematic reentry; native yaw guard armed before physical/body reconciliation");
    }
    updateAcceptedPhysicalYaw(yawFollowGameplay,bodyPoseValid,bodyAxis,displayTime);
    const bool snapTurnAtStereoBoundary=
        kharvox::snapTurnMayActivateAtStereoBoundary(
            s.alternatingStereo,nativePackedStereoAtFrame,
            s.alternatingStereoWarmupActive,s.renderEye);
    if(yawFollowGameplay&&s.snapTurnActivationPending&&snapTurnAtStereoBoundary){
        s.snapTurnActivationPending=false;
        const auto snapYaw=kharvox::applySnapTurnToPhysicalResidual(
            s.acceptedPhysicalYaw,s.artificialTurnTotalDegrees,
            s.snapTurnPendingDegrees);
        s.acceptedPhysicalYaw=snapYaw.acceptedPhysicalYaw;
        s.artificialTurnTotalDegrees=snapYaw.accumulatedArtificialTurn;
        s.artificialTurnActive=false;
        s.artificialTurnRemainingDegrees=0.f;
        s.artificialTurnAccumulatedDegrees=0.f;
        ++s.snapTurnGeneration;
        s.snapTurnStereoTransitionActive=s.alternatingStereo
            &&!nativePackedStereoAtFrame;
        log("[TURN] pair-boundary snap applied as physical body-follow residual="+std::to_string(s.snapTurnPendingDegrees)+" generation="+std::to_string(s.snapTurnGeneration)+" aerGuard="+(s.snapTurnStereoTransitionActive?"active":"not-needed"));
    }
    publishArtificialTurnYaw();
    const float physicalHeadYaw=physicalResidualHeadYaw();
    if(cutsceneActive){
        // Preserve the accepted room-scale body pose across glory-kill and
        // cinematic cameras. Scripted player/camera yaw must never become a
        // counter-rotation of the physical HMD tracking space.
        s.roomscaleStick={};s.previousPhysicsOriginValid=false;
        s.previousRoomscaleCommand=false;
        const XrVector3f relativeTrackingWorld{rawLocalPosition.x-s.bodyFollowPosition.x,rawLocalPosition.y-s.bodyFollowPosition.y,rawLocalPosition.z-s.bodyFollowPosition.z};
        const XrVector3f relativeTracking=rotateVector(yawQuaternion(artificialTurnVisualYaw()),rotateVector(conjugate(yawQuaternion(s.acceptedPhysicalYaw)),relativeTrackingWorld));
        publishGameCameraHeadPose(physicalHeadYaw,s.head.pitch,s.head.roll,-relativeTracking.z*s.worldScale,-relativeTracking.x*s.worldScale,relativeTracking.y*s.worldScale,s.headCameraArmed,centeredNativeMenuActive);
        return;
    }
    float physicsOrigin[3]{};
    const bool physicsValid=KharvoxCameraGetPlayerPhysicsOrigin(physicsOrigin);
    if(physicsValid){
        if(s.previousPhysicsOriginValid&&s.previousRoomscaleCommand){
            const std::array<float,3> deltaWorld{physicsOrigin[0]-s.previousPhysicsOrigin[0],physicsOrigin[1]-s.previousPhysicsOrigin[1],physicsOrigin[2]-s.previousPhysicsOrigin[2]};
            float acceptedUnits=deltaWorld[0]*s.previousRoomscaleWorldDirection[0]+deltaWorld[1]*s.previousRoomscaleWorldDirection[1]+deltaWorld[2]*s.previousRoomscaleWorldDirection[2];
            if(acceptedUnits>0.f&&acceptedUnits<s.worldScale*.12f){
                const float acceptedMeters=acceptedUnits/s.worldScale;
                s.bodyFollowPosition.x+=s.previousRoomscaleTrackingDirection.x*acceptedMeters;
                s.bodyFollowPosition.z+=s.previousRoomscaleTrackingDirection.z*acceptedMeters;
            }
        }
        for(int axis=0;axis<3;++axis)s.previousPhysicsOrigin[axis]=physicsOrigin[axis];
        s.previousPhysicsOriginValid=true;
    }else{s.previousPhysicsOriginValid=false;}

    const XrVector3f relativeTrackingWorld{rawLocalPosition.x-s.bodyFollowPosition.x,rawLocalPosition.y-s.bodyFollowPosition.y,rawLocalPosition.z-s.bodyFollowPosition.z};
    const XrVector3f relativeTracking=rotateVector(yawQuaternion(artificialTurnVisualYaw()),rotateVector(conjugate(yawQuaternion(s.acceptedPhysicalYaw)),relativeTrackingWorld));
    const float forwardMeters=-relativeTracking.z,lateralMeters=-relativeTracking.x;
    const float horizontalError=std::hypot(forwardMeters,lateralMeters);
    s.roomscaleStick={};s.previousRoomscaleCommand=false;
    const bool manualMove=std::abs(s.leftStick.x)>.15f||std::abs(s.leftStick.y)>.15f;
    if(physicsValid&&bodyPoseValid&&!manualMove&&horizontalError>.0025f){
        const float forwardDirection=forwardMeters/horizontalError,lateralDirection=lateralMeters/horizontalError;
        const float desiredSpeed=std::min(horizontalError*40.f,5.5f);
        constexpr float nativeMaxSpeed=5.5f;
        constexpr float nativeDeadzone=float(XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE)/32767.f;
        const float magnitude=std::clamp(nativeDeadzone+(desiredSpeed/nativeMaxSpeed)*(1.f-nativeDeadzone),nativeDeadzone+.01f,1.f);
        // CameraHook's local lateral row points left, while positive XInput X
        // means strafe right. Invert once at the native input boundary.
        s.roomscaleStick={-lateralDirection*magnitude,forwardDirection*magnitude};
        for(int axis=0;axis<3;++axis)s.previousRoomscaleWorldDirection[axis]=bodyAxis[axis]*forwardDirection+bodyAxis[3+axis]*lateralDirection;
        s.previousRoomscaleTrackingDirection=rotateVector(yawQuaternion(s.acceptedPhysicalYaw),rotateVector(conjugate(yawQuaternion(artificialTurnVisualYaw())),{-lateralDirection,0.f,-forwardDirection}));
        s.previousRoomscaleCommand=true;
    }
    const float forward=forwardMeters*s.worldScale,lateral=lateralMeters*s.worldScale,up=relativeTracking.y*s.worldScale;
    publishGameCameraHeadPose(physicalHeadYaw,s.head.pitch,s.head.roll,forward,lateral,up,s.headCameraArmed,centeredNativeMenuActive);
}
void releaseBackWeaponKeyboardPulse();
void releaseMovement(){s.handsJump={};const SHORT oldX=virtualLeftX.exchange(0,std::memory_order_relaxed),oldY=virtualLeftY.exchange(0,std::memory_order_relaxed);const SHORT oldRightX=virtualRightX.exchange(0,std::memory_order_relaxed),oldRightY=virtualRightY.exchange(0,std::memory_order_relaxed);if(oldX||oldY||oldRightX||oldRightY)virtualPacket.fetch_add(1,std::memory_order_relaxed);releaseBackWeaponKeyboardPulse();}
SHORT stickToXInput(float value){return static_cast<SHORT>(std::lround(std::clamp(value,-1.f,1.f)*(value<0.f?32768.f:32767.f)));}
void updateVirtualLeftStick(XrVector2f stick,bool active){const SHORT x=active?stickToXInput(stick.x):0,y=active?stickToXInput(stick.y):0;const SHORT oldX=virtualLeftX.exchange(x,std::memory_order_relaxed),oldY=virtualLeftY.exchange(y,std::memory_order_relaxed);if(oldX!=x||oldY!=y)virtualPacket.fetch_add(1,std::memory_order_relaxed);}
void updateVirtualRightStick(XrVector2f stick,bool active){const SHORT x=active?stickToXInput(stick.x):0,y=active?stickToXInput(stick.y):0;const SHORT oldX=virtualRightX.exchange(x,std::memory_order_relaxed),oldY=virtualRightY.exchange(y,std::memory_order_relaxed);if(oldX!=x||oldY!=y)virtualPacket.fetch_add(1,std::memory_order_relaxed);}
XrVector2f centeredNativeUiStick(XrVector2f stick){
    // DOOM's Dossier/SWF path can consume XInput axes before the gameplay
    // look deadzone is applied. Center the physical stick here and rescale the
    // remaining travel so hardware drift can never become a permanent map
    // rotation or scroll command.
    constexpr float deadzone=.20f;
    const float magnitude=std::hypot(stick.x,stick.y);
    if(!std::isfinite(magnitude)||magnitude<=deadzone)return {};
    const float clamped=std::min(magnitude,1.f);
    const float scaled=(clamped-deadzone)/(1.f-deadzone);
    const float factor=scaled/magnitude;
    return {stick.x*factor,stick.y*factor};
}
XrVector2f movementDirectionRelativeStick(XrVector2f stick,bool gameplay){
    if(!gameplay||!s.head.valid)return stick;
    bool offHandTrackingValid=false;
    XrVector3f offHandForward{};
    if(s.movementDirectionMode==kharvox::MovementDirectionMode::OffHand
        &&s.headZeroValid){
        const bool useRightHand=kharvox::offHandForMovement(s.leftHanded)
            ==kharvox::MovementDirectionHand::Right;
        const auto& offHand=useRightHand?s.rightController:s.leftController;
        offHandTrackingValid=offHand.valid;
        if(offHandTrackingValid){
            const auto trackingOrientation=multiply(
                conjugate(s.headZero),offHand.orientation);
            offHandForward=rotateVector(trackingOrientation,{0,0,-1});
        }
    }
    const auto direction=kharvox::resolveMovementDirectionYaw(
        s.movementDirectionMode,renderResidualHeadYaw(),
        offHandTrackingValid,offHandForward.x,offHandForward.z,
        s.acceptedPhysicalYaw,artificialTurnVisualYaw());
    if(s.movementDirectionMode==kharvox::MovementDirectionMode::OffHand
        &&(!s.offHandMovementDirectionStateKnown
            ||s.offHandMovementDirectionActive!=direction.usedOffHand)){
        s.offHandMovementDirectionStateKnown=true;
        s.offHandMovementDirectionActive=direction.usedOffHand;
        const bool right=kharvox::offHandForMovement(s.leftHanded)
            ==kharvox::MovementDirectionHand::Right;
        log(std::string("[MOVE] Off hand direction ")
            +(direction.usedOffHand?"ACTIVE hand=":"fallback to Head direction hand=")
            +(right?"right":"left")
            +(direction.usedOffHand?"":" reason=tracking-invalid-or-near-vertical"));
    }
    const auto rotated=kharvox::rotateMovementStickForDirection(
        {stick.x,stick.y},direction.degrees);
    return {rotated.x,rotated.y};
}
void updateVirtualButtons(bool fire,bool gamepadA,bool gamepadB,bool weaponMod,bool melee,bool use,
    bool gamepadX,bool equipment,bool weaponSelect,bool missionInfo,
    bool switchWeaponMod,bool bfg,bool pause,bool cycleEquipment=false){
    const BYTE leftTrigger=weaponMod?255:0;
    const BYTE rightTrigger=fire?255:0;
    WORD buttons{};
    if(gamepadA)buttons|=XINPUT_GAMEPAD_A;
    if(gamepadB)buttons|=XINPUT_GAMEPAD_B;
    // Keep USE and melee on distinct native XInput channels. The physical
    // right-stick click sequences D-pad Right first and JOY8 second so DOOM
    // cannot let _attack2 consume the interaction event from the same poll.
    // Do not synthesize Xbox-Y here: DOOM can treat that physical channel as
    // BFG independently of the visible campaign binding.
    if(melee)buttons|=XINPUT_GAMEPAD_RIGHT_THUMB;
    if(use)buttons|=XINPUT_GAMEPAD_DPAD_RIGHT;
    if(gamepadX)buttons|=XINPUT_GAMEPAD_X;
    if(equipment)buttons|=XINPUT_GAMEPAD_LEFT_SHOULDER;
    if(weaponSelect)buttons|=XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if(missionInfo)buttons|=XINPUT_GAMEPAD_BACK;
    if(switchWeaponMod)buttons|=XINPUT_GAMEPAD_DPAD_UP;
    if(cycleEquipment)buttons|=XINPUT_GAMEPAD_DPAD_LEFT;
    // DOOM treats native Xbox-Y as its BFG channel. Only left-grip hold may
    // synthesize it; right-stick-click USE deliberately uses D-pad Right.
    if(bfg)buttons|=XINPUT_GAMEPAD_Y;
    if(pause)buttons|=XINPUT_GAMEPAD_START;
    const BYTE oldLeftTrigger=virtualLeftTrigger.exchange(leftTrigger,std::memory_order_relaxed);
    const BYTE oldRightTrigger=virtualRightTrigger.exchange(rightTrigger,std::memory_order_relaxed);
    const WORD oldButtons=virtualButtons.exchange(buttons,std::memory_order_relaxed);
    if(oldLeftTrigger!=leftTrigger||oldRightTrigger!=rightTrigger||oldButtons!=buttons)
        virtualPacket.fetch_add(1,std::memory_order_relaxed);
}
BOOL CALLBACK findCurrentProcessWindow(HWND window,LPARAM context){
    DWORD processId{};
    GetWindowThreadProcessId(window,&processId);
    if(processId==GetCurrentProcessId()&&IsWindowVisible(window)){
        *reinterpret_cast<HWND*>(context)=window;
        return FALSE;
    }
    return TRUE;
}
HWND findDoomWindow(){
    if(HWND doom=FindWindowW(nullptr,L"DOOMx64vk"))return doom;
    HWND doom{};
    EnumWindows(findCurrentProcessWindow,reinterpret_cast<LPARAM>(&doom));
    return doom;
}
void focusDoomWindow(){
    // Window activation can synchronously message DOOM's UI thread while that
    // thread waits for loading/render jobs. Never do this from the SFS frame
    // path; the launcher/user owns foreground activation for this prototype.
    if(kharvox::sfs::vrEnabled())return;
    if(!kharvox::shouldAutomaticallyFocusDoom(s.submittedLayerFrames))return;
    if(HWND doom=findDoomWindow()){
        if(GetForegroundWindow()==doom)return;
        static ULONGLONG lastAttempt{};
        static bool failureLogged{};
        const auto now=GetTickCount64();
        if(now-lastAttempt<1000)return;
        lastAttempt=now;
        ShowWindowAsync(doom,SW_RESTORE);
        BringWindowToTop(doom);
        const bool restored=SetForegroundWindow(doom)!=FALSE
            ||GetForegroundWindow()==doom;
        if(restored){
            log("[WINDOW-FOCUS] DOOM foreground restored from active XR input");
            failureLogged=false;
        }else if(!failureLogged){
            log("[WINDOW-FOCUS] DOOM foreground restore deferred to launcher/Windows");
            failureLogged=true;
        }
    }
}
bool sendBackWeaponKeyboardKey(WORD virtualKey,bool down){
    if(!virtualKey)return false;
    INPUT input{};
    input.type=INPUT_KEYBOARD;
    input.ki.wScan=static_cast<WORD>(MapVirtualKeyW(virtualKey,MAPVK_VK_TO_VSC));
    if(!input.ki.wScan)return false;
    input.ki.dwFlags=KEYEVENTF_SCANCODE|(down?0:KEYEVENTF_KEYUP);
    return SendInput(1,&input,sizeof(input))==1;
}
void releaseBackWeaponKeyboardPulse(){
    if(s.backWeaponKeyboardKeyDown)
        sendBackWeaponKeyboardKey(s.backWeaponKeyboardKey,false);
    s.backWeaponKeyboardKeyDown=false;
    s.backWeaponKeyboardKey=0;
    s.backWeaponKeyboardReleaseTime=0;
}
bool startBackWeaponKeyboardPulse(WORD virtualKey,XrTime displayTime){
    releaseBackWeaponKeyboardPulse();
    focusDoomWindow();
    if(!sendBackWeaponKeyboardKey(virtualKey,true))return false;
    s.backWeaponKeyboardKey=virtualKey;
    s.backWeaponKeyboardKeyDown=true;
    s.backWeaponKeyboardReleaseTime=displayTime+100000000;
    return true;
}
void serviceBackWeaponKeyboardPulse(XrTime displayTime,bool contextActive){
    if(s.backWeaponKeyboardKeyDown
        &&(!contextActive||displayTime>=s.backWeaponKeyboardReleaseTime))
        releaseBackWeaponKeyboardPulse();
}
float environmentFloat(const char* name,float fallback,float minimum,float maximum){char text[64]{};if(!GetEnvironmentVariableA(name,text,sizeof(text)))return fallback;char*end{};const float value=std::strtof(text,&end);return end!=text?std::clamp(value,minimum,maximum):fallback;}
bool environmentEnabled(const char* name){char text[16]{};if(!GetEnvironmentVariableA(name,text,sizeof(text)))return false;return !_stricmp(text,"1")||!_stricmp(text,"true")||!_stricmp(text,"yes")||!_stricmp(text,"on");}
float configuredWorldScale(){std::ifstream config(kharvox::runtimePathA("world_scale.cfg"));float value{};if(config>>value)return std::clamp(value,10.f,200.f);return environmentFloat("KHARVOX_WORLD_SCALE",39.3701f,10.f,200.f);}
float configuredRenderScale(){std::ifstream config(kharvox::runtimePathA("render_scale.cfg"));float value{};if(config>>value)return kharvox::effectiveRenderScale(value,false);char text[64]{};if(!GetEnvironmentVariableA("KHARVOX_RENDER_SCALE",text,sizeof(text)))return 1.f;char*end{};value=std::strtof(text,&end);return end!=text?kharvox::effectiveRenderScale(value,false):1.f;}
constexpr float nativeSmoothTurnCarrierSpeed=600.f;
constexpr float nativeDoomRightStickDeadzone=.15f;
float manualTurnCarrierSpeed(){return std::max(nativeSmoothTurnCarrierSpeed,s.smoothTurnDegreesPerSecond);}
void configureTurning(){char mode[32]{};GetEnvironmentVariableA("KHARVOX_TURN_MODE",mode,sizeof(mode));if(!_stricmp(mode,"snap"))s.turnMode=TurnMode::Snap;else if(!_stricmp(mode,"off"))s.turnMode=TurnMode::Off;else s.turnMode=TurnMode::Smooth;s.turnDeadzone=environmentFloat("KHARVOX_TURN_DEADZONE",.35f,.05f,.95f);s.smoothTurnDegreesPerSecond=environmentFloat("KHARVOX_SMOOTH_TURN_SPEED",230.f,10.f,600.f);s.snapTurnDegrees=environmentFloat("KHARVOX_SNAP_TURN_ANGLE",60.f,10.f,180.f);std::ostringstream o;o<<"[TURN] artificial mode="<<(s.turnMode==TurnMode::Smooth?"smooth":s.turnMode==TurnMode::Snap?"snap":"off")<<" configuredSmoothDegreesPerSecond="<<s.smoothTurnDegreesPerSecond<<" hiddenBodyCatchupSpeed="<<manualTurnCarrierSpeed()<<" deadzone="<<s.turnDeadzone<<" snapDegrees="<<s.snapTurnDegrees;log(o.str());}
void configureMovementDirection(){char mode[32]{};GetEnvironmentVariableA("KHARVOX_MOVEMENT_DIRECTION",mode,sizeof(mode));s.movementDirectionMode=!_stricmp(mode,"off-hand")?kharvox::MovementDirectionMode::OffHand:kharvox::MovementDirectionMode::Head;s.offHandMovementDirectionStateKnown=false;s.offHandMovementDirectionActive=false;log(std::string("[MOVE] direction=")+(s.movementDirectionMode==kharvox::MovementDirectionMode::OffHand?"off-hand":"head")+" turn pipeline unchanged");}
void configurePhysicalGlorykill(){
    s.handsJumpEnabled=environmentEnabled("KHARVOX_HANDS_JUMP");
    log(std::string("[INPUT] Hands Jump ")+(s.handsJumpEnabled?"ENABLED threshold=1.9m/s upward, both hands":"disabled"));
    s.physicalGlorykillEnabled=environmentEnabled("KHARVOX_PHYSICAL_GLORYKILL");
    s.physicalGlorykillSpeed=environmentFloat("KHARVOX_PHYSICAL_GLORYKILL_SPEED",2.8f,.5f,5.f);
    char hands[16]{};GetEnvironmentVariableA("KHARVOX_PHYSICAL_GLORYKILL_HANDS",hands,sizeof(hands));
    if(!_stricmp(hands,"left"))s.physicalGlorykillHands=PhysicalGlorykillHands::Left;
    else if(!_stricmp(hands,"right"))s.physicalGlorykillHands=PhysicalGlorykillHands::Right;
    else s.physicalGlorykillHands=PhysicalGlorykillHands::Both;
    s.physicalPunchArmed={false,false};
    s.physicalPunchCooldownUntil=0;
    std::ostringstream o;
    o<<"[INPUT] Physical Glorykill "<<(s.physicalGlorykillEnabled?"ENABLED":"disabled")
     <<" forwardSpeedThreshold="<<s.physicalGlorykillSpeed<<"m/s hands="
     <<(s.physicalGlorykillHands==PhysicalGlorykillHands::Left?"left":s.physicalGlorykillHands==PhysicalGlorykillHands::Right?"right":"both");
    log(o.str());
}
void configureLaserSight(){
    s.laserSightEnabled=environmentEnabled("KHARVOX_LASER_SIGHT")
        ||kharvox::runtimeFileExists(L"enable_laser_sight");

    log(std::string("[LASER] ")+(s.laserSightEnabled?"ENABLED":"disabled")
        +" diameter=0.004m; source-qualified scene-depth beam; no compositor overlay; Chainsaw and non-muzzle weapons disabled");
}
void configureMotionWeaponWheel(){
    char text[16]{};
    if(GetEnvironmentVariableA("KHARVOX_MOTION_WEAPON_WHEEL",text,sizeof(text))){
        s.motionWheelEnabled=(!_stricmp(text,"1")||!_stricmp(text,"true")||!_stricmp(text,"yes")||!_stricmp(text,"on"));
    }else{
        s.motionWheelEnabled=true;
    }
    log(std::string("[INPUT] Motion Weapon Wheel (Alyx Style) ")
        +(s.motionWheelEnabled?"ENABLED; physical hand displacement drives selection; haptic clicks on sector change":"disabled"));
}
float nativeManualTurnX(XrTime displayTime,bool active){
    const float x=active&&std::abs(s.rightStick.x)>=s.turnDeadzone?s.rightStick.x:0.f;
    float frameSeconds=0.f;
    if(s.previousTurnDisplayTime>0&&displayTime>s.previousTurnDisplayTime)frameSeconds=std::clamp(float(displayTime-s.previousTurnDisplayTime)*1e-9f,0.f,.05f);
    s.previousTurnDisplayTime=displayTime;
    if(s.turnMode==TurnMode::Off){s.snapTurnArmed=true;s.snapTurnActivationPending=false;s.artificialTurnActive=false;s.artificialTurnRemainingDegrees=0.f;publishArtificialTurnYaw();return 0.f;}
    if(s.turnMode==TurnMode::Smooth){
        if(x!=0.f&&frameSeconds>0.f){
            const float openXrMagnitude=(std::abs(x)-s.turnDeadzone)/(1.f-s.turnDeadzone);
            const float artificialDelta=s.bodyYawPerPositiveStick*std::copysign(openXrMagnitude*s.smoothTurnDegreesPerSecond*frameSeconds,x);
            s.artificialTurnRemainingDegrees=wrapDegrees(s.artificialTurnRemainingDegrees+artificialDelta);
            s.artificialTurnTotalDegrees=wrapDegrees(
                s.artificialTurnTotalDegrees+artificialDelta);
            s.artificialTurnActive=true;
        }
    }else{
        if(std::abs(s.rightStick.x)<s.turnDeadzone*.6f)s.snapTurnArmed=true;
        if(x!=0.f&&s.snapTurnArmed&&!s.artificialTurnActive&&!s.snapTurnActivationPending){
            s.snapTurnArmed=false;s.snapTurnActivationPending=true;
            s.snapTurnPendingDegrees=s.bodyYawPerPositiveStick*std::copysign(s.snapTurnDegrees,x);
            log("[TURN] snap queued artificialDelta="+std::to_string(s.snapTurnPendingDegrees));
        }
    }
    if(!s.artificialTurnActive||frameSeconds<=0.f)return 0.f;
    if(s.artificialTurnPublishedDegrees*s.artificialTurnRemainingDegrees<=0.f)return 0.f;
    const float publishedCatchupDegrees=std::copysign(std::min(std::abs(s.artificialTurnPublishedDegrees),std::abs(s.artificialTurnRemainingDegrees)),s.artificialTurnRemainingDegrees);
    const float stickDirection=publishedCatchupDegrees/s.bodyYawPerPositiveStick;
    const float desiredMagnitude=std::clamp(std::abs(publishedCatchupDegrees)/(manualTurnCarrierSpeed()*frameSeconds),.001f,1.f);
    const float nativeMagnitude=nativeDoomRightStickDeadzone+desiredMagnitude*(1.f-nativeDoomRightStickDeadzone);
    return std::copysign(nativeMagnitude,stickDirection);
}
float* doomFloat(uintptr_t rva){auto base=reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));return base?reinterpret_cast<float*>(base+rva):nullptr;}
constexpr uintptr_t joyYawSpeedCurrentRva=0x5FAA3A4;
constexpr uintptr_t joyDeadZoneCurrentRva=0x5FAA124;
constexpr uintptr_t joyDampenLookCurrentRva=0x5FAA580;
constexpr uintptr_t joyGammaLookCurrentRva=0x5FAA4E0;
constexpr uintptr_t joySmoothingEnabledCurrentRva=0x5FAA800;
constexpr uintptr_t joyEdgeAccelerationScalarCurrentRva=0x5FAAA84;
constexpr float bodyFollowYawSpeed=600.f;
void holdNativeTurnCvars(float degreesPerSecond){
    if(float* value=doomFloat(joyYawSpeedCurrentRva))*value=degreesPerSecond;
    if(float* value=doomFloat(joyDeadZoneCurrentRva))*value=nativeDoomRightStickDeadzone;
    if(int* value=reinterpret_cast<int*>(doomFloat(joyDampenLookCurrentRva)))*value=0;
    if(int* value=reinterpret_cast<int*>(doomFloat(joyGammaLookCurrentRva)))*value=0;
    if(int* value=reinterpret_cast<int*>(doomFloat(joySmoothingEnabledCurrentRva)))*value=0;
    if(float* value=doomFloat(joyEdgeAccelerationScalarCurrentRva))*value=0.f;
}
float nativePhysicalBodyTurnX(bool gameplay,bool manualTurnActive){
    if(!gameplay||manualTurnActive||!s.head.valid||s.postCinematicYawGuard.active){s.bodyFollowTurnX=0.f;return 0.f;}
    const float error=physicalResidualHeadYaw();
    if(std::abs(error)<.35f){s.bodyFollowTurnX=0.f;return 0.f;}
    const float magnitude=std::clamp(std::abs(error)/90.f,nativeDoomRightStickDeadzone+.02f,1.f);
    s.bodyFollowTurnX=std::copysign(magnitude,error)/s.bodyYawPerPositiveStick;
    return s.bodyFollowTurnX;
}
XrVector3f positionRelativeToBodyTrackingOrigin(XrVector3f position){
    const XrVector3f delta{position.x-s.headZeroPosition.x,
                           position.y-s.headZeroPosition.y,
                           position.z-s.headZeroPosition.z};
    const auto inverseBody=conjugate(s.headZero);
    const XrVector3f trackingPosition=rotateVector(inverseBody,delta);
    const XrVector3f relativePositionWorld{trackingPosition.x-s.bodyFollowPosition.x,trackingPosition.y-s.bodyFollowPosition.y,trackingPosition.z-s.bodyFollowPosition.z};
    const auto inverseAcceptedYaw=conjugate(yawQuaternion(s.controllerFrameYaw.acceptedForPose(s.acceptedPhysicalYaw)));
    const auto pendingSnapYaw=yawQuaternion(s.controllerFrameYaw.turnForPose(artificialTurnVisualYaw()));
    return rotateVector(pendingSnapYaw,rotateVector(inverseAcceptedYaw,relativePositionWorld));
}
WeaponPose controllerRelativeToBodyTrackingOrigin(const ControllerPose& controller){
    const auto inverseBody=conjugate(s.headZero);
    const XrQuaternionf trackingOrientation=multiply(inverseBody,controller.orientation);
    const auto inverseAcceptedYaw=conjugate(yawQuaternion(s.controllerFrameYaw.acceptedForPose(s.acceptedPhysicalYaw)));
    const auto pendingSnapYaw=yawQuaternion(s.controllerFrameYaw.turnForPose(artificialTurnVisualYaw()));
    return {positionRelativeToBodyTrackingOrigin(controller.position),multiply(pendingSnapYaw,multiply(inverseAcceptedYaw,trackingOrientation))};
}
const char* twoHandModeName(TwoHandAimMode mode){return mode==TwoHandAimMode::SideGrip?"side-grip":"barrel";}
const char* weaponKindName(KharvoxWeaponKind kind){return KharvoxWeaponKindDisplayName(kind);}
const char* backWeaponKindKey(kharvox::BackWeaponKind kind){
    switch(kind){
    case kharvox::BackWeaponKind::Pistol:return "pistol";
    case kharvox::BackWeaponKind::CombatShotgun:return "shotgun";
    case kharvox::BackWeaponKind::PlasmaRifle:return "plasma_rifle";
    case kharvox::BackWeaponKind::HeavyAssaultRifle:return "heavy_assault_rifle";
    case kharvox::BackWeaponKind::RocketLauncher:return "rocket_launcher";
    case kharvox::BackWeaponKind::SuperShotgun:return "super_shotgun";
    case kharvox::BackWeaponKind::GaussCannon:return "gauss_cannon";
    case kharvox::BackWeaponKind::Chaingun:return "chaingun";
    case kharvox::BackWeaponKind::Bfg:return "bfg";
    case kharvox::BackWeaponKind::Chainsaw:return "chainsaw";
    default:return "unknown";
    }
}
kharvox::BackWeaponKind backWeaponKindFromKey(std::string key){
    std::transform(key.begin(),key.end(),key.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    if(key=="pistol")return kharvox::BackWeaponKind::Pistol;
    if(key=="shotgun")return kharvox::BackWeaponKind::CombatShotgun;
    if(key=="plasma_rifle")return kharvox::BackWeaponKind::PlasmaRifle;
    if(key=="heavy_assault_rifle")return kharvox::BackWeaponKind::HeavyAssaultRifle;
    if(key=="rocket_launcher")return kharvox::BackWeaponKind::RocketLauncher;
    if(key=="super_shotgun")return kharvox::BackWeaponKind::SuperShotgun;
    if(key=="gauss_cannon")return kharvox::BackWeaponKind::GaussCannon;
    if(key=="chaingun")return kharvox::BackWeaponKind::Chaingun;
    if(key=="bfg")return kharvox::BackWeaponKind::Bfg;
    if(key=="chainsaw")return kharvox::BackWeaponKind::Chainsaw;
    return kharvox::BackWeaponKind::CombatShotgun;
}
kharvox::BackWeaponKind loadBackWeaponKind(){
    std::string key;
    std::ifstream configured(kharvox::runtimePathA("back_weapon.cfg"));
    if(!(configured>>key)){
        char environment[64]{};
        if(GetEnvironmentVariableA("KHARVOX_BACK_WEAPON",environment,sizeof(environment)))key=environment;
    }
    return backWeaponKindFromKey(key);
}
kharvox::BackWeaponKind backWeaponKindFromActive(KharvoxWeaponKind kind){
    switch(kind){
    case KharvoxWeaponKind::Pistol:return kharvox::BackWeaponKind::Pistol;
    case KharvoxWeaponKind::Shotgun:return kharvox::BackWeaponKind::CombatShotgun;
    case KharvoxWeaponKind::PlasmaRifle:return kharvox::BackWeaponKind::PlasmaRifle;
    case KharvoxWeaponKind::HeavyAssaultRifle:return kharvox::BackWeaponKind::HeavyAssaultRifle;
    case KharvoxWeaponKind::RocketLauncher:return kharvox::BackWeaponKind::RocketLauncher;
    case KharvoxWeaponKind::SuperShotgun:return kharvox::BackWeaponKind::SuperShotgun;
    case KharvoxWeaponKind::GaussCannon:return kharvox::BackWeaponKind::GaussCannon;
    case KharvoxWeaponKind::Chaingun:return kharvox::BackWeaponKind::Chaingun;
    case KharvoxWeaponKind::Bfg:return kharvox::BackWeaponKind::Bfg;
    case KharvoxWeaponKind::Chainsaw:return kharvox::BackWeaponKind::Chainsaw;
    default:return kharvox::BackWeaponKind::Unknown;
    }
}
KharvoxWeaponKind nativeWeaponKindFromBack(kharvox::BackWeaponKind kind){
    switch(kind){
    case kharvox::BackWeaponKind::Pistol:return KharvoxWeaponKind::Pistol;
    case kharvox::BackWeaponKind::CombatShotgun:return KharvoxWeaponKind::Shotgun;
    case kharvox::BackWeaponKind::PlasmaRifle:return KharvoxWeaponKind::PlasmaRifle;
    case kharvox::BackWeaponKind::HeavyAssaultRifle:return KharvoxWeaponKind::HeavyAssaultRifle;
    case kharvox::BackWeaponKind::RocketLauncher:return KharvoxWeaponKind::RocketLauncher;
    case kharvox::BackWeaponKind::SuperShotgun:return KharvoxWeaponKind::SuperShotgun;
    case kharvox::BackWeaponKind::GaussCannon:return KharvoxWeaponKind::GaussCannon;
    case kharvox::BackWeaponKind::Chaingun:return KharvoxWeaponKind::Chaingun;
    case kharvox::BackWeaponKind::Bfg:return KharvoxWeaponKind::Bfg;
    case kharvox::BackWeaponKind::Chainsaw:return KharvoxWeaponKind::Chainsaw;
    default:return KharvoxWeaponKind::Unknown;
    }
}
const char* weaponAmmoStateKey(KharvoxWeaponAmmoState state){
    switch(state){
    case KharvoxWeaponAmmoState::Unavailable:return "unavailable";
    case KharvoxWeaponAmmoState::Empty:return "empty";
    case KharvoxWeaponAmmoState::Usable:return "usable";
    default:return "unknown";
    }
}
bool laserWeaponAllowed(KharvoxWeaponKind kind){
    switch(kind){
    case KharvoxWeaponKind::Pistol:
    case KharvoxWeaponKind::Shotgun:
    case KharvoxWeaponKind::HeavyAssaultRifle:
    case KharvoxWeaponKind::PlasmaRifle:
    case KharvoxWeaponKind::RocketLauncher:
    case KharvoxWeaponKind::SuperShotgun:
    case KharvoxWeaponKind::GaussCannon:
    case KharvoxWeaponKind::Chaingun:
    case KharvoxWeaponKind::Bfg:
    case KharvoxWeaponKind::AssaultRifle:
    case KharvoxWeaponKind::ArcCannon:
    case KharvoxWeaponKind::MancubusGland:return true;
    case KharvoxWeaponKind::Chainsaw: return false;
    case KharvoxWeaponKind::Fists: return false;
    default:return false;
    }
}

XrPosef laserBodyTrackingTransform(){
    const auto accepted=yawQuaternion(s.controllerFrameYaw.acceptedForPose(s.acceptedPhysicalYaw));
    const auto pending=conjugate(yawQuaternion(s.controllerFrameYaw.turnForPose(artificialTurnVisualYaw())));
    return {multiply(s.headZero,multiply(accepted,pending)),
        addVector(s.headZeroPosition,rotateVector(s.headZero,s.bodyFollowPosition))};
}
bool renderedLaserPoseInTrackingSpace(XrVector3f& trackingOrigin,XrVector3f& trackingDirection,
    const XrPosef& tracking, uint64_t sourcePose=0,int sourceEye=-1){
    float muzzle[3]{},direction[3]{},body[3]{},axis[9]{};
    if(!s.headZeroValid||!s.headZeroPositionValid||!KharvoxWeaponGetLaserMuzzlePose(
        muzzle,direction,body,axis,sourcePose,sourceEye))return false;
    float local[3]{},forward[3]{};
    for(int row=0;row<3;++row)for(int c=0;c<3;++c){
        local[row]+=(muzzle[c]-body[c])*axis[row*3+c];
        forward[row]+=direction[c]*axis[row*3+c];
    }
    trackingOrigin=addVector(tracking.position,rotateVector(tracking.orientation,
        {-local[1]/s.worldScale,local[2]/s.worldScale,-local[0]/s.worldScale}));
    trackingDirection=normalizeVector(rotateVector(tracking.orientation,{-forward[1],forward[2],-forward[0]}));
    return std::isfinite(trackingOrigin.x)&&std::isfinite(trackingOrigin.y)&&std::isfinite(trackingOrigin.z)
        &&std::isfinite(trackingDirection.x)&&std::isfinite(trackingDirection.y)&&std::isfinite(trackingDirection.z);
}
TwoHandCalibration* twoHandCalibrationFor(KharvoxWeaponKind kind){
    const int index=static_cast<int>(kind);
    if(index<=static_cast<int>(KharvoxWeaponKind::Unknown)||index>=static_cast<int>(KharvoxWeaponKind::Count))return nullptr;
    return &s.twoHandCalibrations[static_cast<size_t>(index)];
}
void writeTwoHandStatus(const std::string& value){
    static std::string previous;
    if(value==previous)return;
    previous=value;
    std::ofstream out(kharvox::runtimePathA("two_hand_status.txt"),std::ios::trunc);
    if(out)out<<value<<'\n';
}
TwoHandAimMode loadTwoHandCalibrationAimMode(){
    std::ifstream alignment(kharvox::runtimePathA("two_hand_alignment.cfg"));
    std::string selectedMode;
    if(alignment>>selectedMode){
        std::transform(selectedMode.begin(),selectedMode.end(),selectedMode.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
        return selectedMode.find("side")!=std::string::npos?TwoHandAimMode::SideGrip:TwoHandAimMode::Barrel;
    }
    return TwoHandAimMode::Barrel;
}
KharvoxWeaponKind loadTwoHandCalibrationTarget(){
    std::ifstream targetFile(kharvox::runtimePathA("two_hand_calibration_target.cfg"));
    std::string key;
    if(targetFile>>key){
        const auto kind=KharvoxWeaponKindFromKey(key.c_str());
        if(kind!=KharvoxWeaponKind::Unknown)return kind;
    }
    return KharvoxWeaponKind::Shotgun;
}
bool validTwoHandCalibrationValues(XrVector3f support,float radius,float maximumWeight){
    return std::isfinite(support.x)&&std::isfinite(support.y)&&std::isfinite(support.z)
        &&std::isfinite(radius)&&std::isfinite(maximumWeight);
}
bool loadTwoHandCalibrationFile(const std::string& path){
    bool loadedAny=false;
    std::ifstream saved(path);
    int version{};std::string weaponKey,savedMode;XrVector3f support{};float radius{},maximumWeight{};
    while(saved>>version>>weaponKey>>savedMode>>support.x>>support.y>>support.z>>radius>>maximumWeight){
        const auto kind=KharvoxWeaponKindFromKey(weaponKey.c_str());
        if(version!=2||kind==KharvoxWeaponKind::Unknown||!validTwoHandCalibrationValues(support,radius,maximumWeight))continue;
        auto* calibration=twoHandCalibrationFor(kind);
        calibration->supportPosition=support;
        calibration->captureRadius=std::clamp(radius,.08f,.40f);
        calibration->maximumHandLineWeight=std::clamp(maximumWeight,0.f,1.f);
        calibration->aimMode=savedMode=="side-grip"?TwoHandAimMode::SideGrip:TwoHandAimMode::Barrel;
        calibration->valid=true;
        loadedAny=true;
    }
    return loadedAny;
}

bool loadTwoHandCalibrations(){
    // The reviewed project profiles establish a complete baseline. Legacy and
    // current user-saved files are then applied in increasing priority so a
    // personal recalibration overrides only the corresponding weapon.
    bool loadedAny=loadTwoHandCalibrationFile(
        kharvox::runtimePathA("two_hand_weapon_profiles_default.cfg"));

    // Preserve the already proven shotgun calibration from builds that stored
    // only one global profile. It overrides the project shotgun default.
    auto* shotgun=twoHandCalibrationFor(KharvoxWeaponKind::Shotgun);
    std::ifstream legacy(kharvox::runtimePathA("two_hand_shotgun_saved.cfg"));
    int version{};std::string savedMode;XrVector3f support{};float radius{},maximumWeight{};
    if(legacy>>version>>savedMode>>support.x>>support.y>>support.z>>radius>>maximumWeight&&version==1
        &&validTwoHandCalibrationValues(support,radius,maximumWeight)){
        shotgun->supportPosition=support;
        shotgun->captureRadius=std::clamp(radius,.08f,.40f);
        shotgun->maximumHandLineWeight=std::clamp(maximumWeight,0.f,1.f);
        shotgun->aimMode=savedMode=="side-grip"?TwoHandAimMode::SideGrip:TwoHandAimMode::Barrel;
        shotgun->valid=true;
        loadedAny=true;
        log("[TWO-HAND] loaded legacy shotgun calibration into the multi-weapon profile set");
    }
    loadedAny=loadTwoHandCalibrationFile(
        kharvox::runtimePathA("two_hand_weapon_profiles_saved.cfg"))||loadedAny;
    return loadedAny;
}
bool saveTwoHandCalibrations(){
    std::ofstream out(kharvox::runtimePathA("two_hand_weapon_profiles_saved.cfg"),std::ios::trunc);
    if(!out)return false;
    for(int index=static_cast<int>(KharvoxWeaponKind::Pistol);index<static_cast<int>(KharvoxWeaponKind::Count);++index){
        const auto kind=static_cast<KharvoxWeaponKind>(index);
        const auto* calibration=twoHandCalibrationFor(kind);
        if(!calibration||!calibration->valid)continue;
        out<<2<<' '<<KharvoxWeaponKindKey(kind)<<' '<<twoHandModeName(calibration->aimMode)<<' '
           <<calibration->supportPosition.x<<' '<<calibration->supportPosition.y<<' '<<calibration->supportPosition.z<<' '
           <<calibration->captureRadius<<' '<<calibration->maximumHandLineWeight<<'\n';
    }
    return static_cast<bool>(out);
}
size_t savedTwoHandCalibrationCount(){
    size_t count{};
    for(int index=static_cast<int>(KharvoxWeaponKind::Pistol);index<static_cast<int>(KharvoxWeaponKind::Count);++index){
        const auto* calibration=twoHandCalibrationFor(static_cast<KharvoxWeaponKind>(index));
        if(calibration&&calibration->valid)++count;
    }
    return count;
}
XrQuaternionf solveTwoHandOrientation(const WeaponPose& primary,const WeaponPose& supportGrip,const TwoHandCalibration& calibration,bool virtualGunstock,XrVector3f gunstockPosition){
    const auto primaryForward=normalizeVector(rotateVector(primary.orientation,{0,0,-1}));
    const auto primaryUp=normalizeVector(rotateVector(primary.orientation,{0,1,0}),{0,1,0});
    const auto handDelta=subtractVector(supportGrip.position,primary.position);

    if(calibration.aimMode==TwoHandAimMode::SideGrip){
        // A lateral handle must not become a false barrel. Keep aim controlled
        // by the primary hand and use the calibrated handle vector only for a
        // roll correction around that aim axis.
        const auto predictedSupport=rotateVector(primary.orientation,calibration.supportPosition);
        const auto predictedLateral=subtractVector(predictedSupport,scaleVector(primaryForward,dotVector(predictedSupport,primaryForward)));
        const auto actualLateral=subtractVector(handDelta,scaleVector(primaryForward,dotVector(handDelta,primaryForward)));
        if(vectorLength(predictedLateral)<.001f||vectorLength(actualLateral)<.001f)return primary.orientation;
        const auto from=normalizeVector(predictedLateral);
        const auto to=normalizeVector(actualLateral);
        const float signedAngle=std::atan2(dotVector(primaryForward,crossVector(from,to)),std::clamp(dotVector(from,to),-1.f,1.f));
        const float half=.5f*signedAngle*calibration.maximumHandLineWeight;
        const float sine=std::sin(half);
        const XrQuaternionf roll{primaryForward.x*sine,primaryForward.y*sine,primaryForward.z*sine,std::cos(half)};
        return normalizeQuaternion(multiply(roll,primary.orientation));
    }

    // The virtual stock places the aim origin 0.18 m below the HMD. Map the
    // calibrated local support vector, rather than an assumed local barrel
    // axis, onto the selected hand/stock line.
    const auto aimOrigin=virtualGunstock?gunstockPosition:primary.position;
    const auto handDirection=normalizeVector(subtractVector(supportGrip.position,aimOrigin),primaryForward);
    const auto localSupportDirection=normalizeVector(calibration.supportPosition,{0,0,-1});
    const auto localSupportFrame=quaternionFromForwardUp(localSupportDirection,{0,1,0});
    const auto handFrame=quaternionFromForwardUp(handDirection,primaryUp);
    return normalizeQuaternion(multiply(handFrame,conjugate(localSupportFrame)));
}
const ControllerPose& weaponController(){return s.leftHanded?s.leftController:s.rightController;}
const ControllerPose& supportGripController(){return s.leftHanded?s.rightGripController:s.leftGripController;}
const char* weaponHandName(){return s.leftHanded?"left":"right";}
const char* supportHandName(){return s.leftHanded?"right":"left";}
const char* weaponTriggerName(){return s.leftHanded?"LEFT TRIGGER":"RIGHT TRIGGER";}
const char* weaponGripName(){return s.leftHanded?"LEFT GRIP":"RIGHT GRIP";}
const char* supportGripName(){return s.leftHanded?"RIGHT GRIP":"LEFT GRIP";}
void refreshGripInteractionProfiles(){
    if(!s.interactionProfilesDirty||!s.session)return;
    if(!s.getCurrentInteractionProfile||s.leftHandUserPath==XR_NULL_PATH
        ||s.rightHandUserPath==XR_NULL_PATH||s.valveIndexProfilePath==XR_NULL_PATH){
        s.interactionProfilesDirty=false;
        return;
    }
    XrInteractionProfileState left{XR_TYPE_INTERACTION_PROFILE_STATE};
    XrInteractionProfileState right{XR_TYPE_INTERACTION_PROFILE_STATE};
    const auto leftResult=s.getCurrentInteractionProfile(
        s.session,s.leftHandUserPath,&left);
    const auto rightResult=s.getCurrentInteractionProfile(
        s.session,s.rightHandUserPath,&right);
    if(XR_FAILED(leftResult)||XR_FAILED(rightResult)){
        if(!s.interactionProfileQueryFailureLogged){
            s.interactionProfileQueryFailureLogged=true;
            log("[INPUT] active controller profile query deferred left="
                +result(leftResult)+" right="+result(rightResult));
        }
        return;
    }
    s.interactionProfilesDirty=false;
    s.interactionProfileQueryFailureLogged=false;
    const bool leftIndex=left.interactionProfile==s.valveIndexProfilePath;
    const bool rightIndex=right.interactionProfile==s.valveIndexProfilePath;
    const bool primaryIndex=s.leftHanded?leftIndex:rightIndex;
    const bool supportIndex=s.leftHanded?rightIndex:leftIndex;
    if(!s.interactionProfilesKnown
        ||primaryIndex!=s.primaryGripUsesValveIndex
        ||supportIndex!=s.supportGripUsesValveIndex){
        s.interactionProfilesKnown=true;
        s.primaryGripUsesValveIndex=primaryIndex;
        s.supportGripUsesValveIndex=supportIndex;
        log(std::string("[INPUT] grip profile weapon=")
            +(primaryIndex?"Valve Index (press>0.70 release<=0.55)":"default (press>0.55)")
            +" support="
            +(supportIndex?"Valve Index (press>0.70 release<=0.55)":"default (press>0.55)"));
    }
}
const char* moveStickName(){return s.leftHandSwapSticks?"right stick":"left stick";}
const char* turnStickName(){return s.leftHandSwapSticks?"left stick":"right stick";}
const char* rightFaceButtonHandName(){return s.leftHanded&&s.leftHandSwapSticks?"left":"right";}
const char* leftFaceButtonHandName(){return s.leftHanded&&s.leftHandSwapSticks?"right":"left";}
const char* pauseHandName(){return s.leftHanded?"right":"left";}
TwoHandCalibration activeHandCalibration(const TwoHandCalibration& saved){
    auto active=saved;
    // Profiles are stored in the established right-primary/left-support
    // convention. Mirroring only the controller-local lateral coordinate
    // makes the same profile usable with a left-primary/right-support hold.
    if(s.leftHanded)active.supportPosition.x=-active.supportPosition.x;
    return active;
}
bool captureTwoHandCalibration(bool triggerDown){
    if(!s.twoHandEnabled||!s.twoHandCalibrationMode)return false;
    if(triggerDown&&!s.firePressed){
        const auto kind=KharvoxWeaponCurrentKind();
        if(kind==KharvoxWeaponKind::Unknown){
            writeTwoHandStatus(std::string("CALIBRATION: equip ")+weaponKindName(s.twoHandCalibrationTarget)+"; native weapon identity is not ready");
            log("[TWO-HAND] calibration capture rejected: native weapon identity unknown");
        }else if(kind!=s.twoHandCalibrationTarget){
            writeTwoHandStatus(std::string("CALIBRATION: selected ")+weaponKindName(s.twoHandCalibrationTarget)
                +", but active weapon is "+weaponKindName(kind)+" — capture rejected");
            log(std::string("[TWO-HAND] calibration capture rejected: selected=")+KharvoxWeaponKindKey(s.twoHandCalibrationTarget)
                +" active="+KharvoxWeaponKindKey(kind));
        }else if(!s.headZeroValid||!weaponController().valid||!supportGripController().valid){
            writeTwoHandStatus(std::string("CALIBRATION: ")+weaponHandName()+" aim and "+supportHandName()+" grip poses must be tracked");
            log(std::string("[TWO-HAND] calibration capture rejected: ")+weaponHandName()+" aim/"+supportHandName()+" grip tracking incomplete");
        }else{
            const auto primary=controllerRelativeToBodyTrackingOrigin(weaponController());
            const auto support=controllerRelativeToBodyTrackingOrigin(supportGripController());
            auto* calibration=twoHandCalibrationFor(kind);
            calibration->supportPosition=rotateVector(conjugate(primary.orientation),subtractVector(support.position,primary.position));
            // Normalize a left-handed capture back into the canonical saved
            // convention so changing handedness never invalidates profiles.
            if(s.leftHanded)calibration->supportPosition.x=-calibration->supportPosition.x;
            calibration->aimMode=s.twoHandCalibrationAimMode;
            calibration->valid=true;
            const bool saved=saveTwoHandCalibrations();
            std::ostringstream details;
            details<<"[TWO-HAND] "<<KharvoxWeaponKindKey(kind)<<" calibration "<<(saved?"saved":"SAVE FAILED")
                   <<" mode="<<twoHandModeName(calibration->aimMode)
                   <<" localSupport="<<calibration->supportPosition.x<<','
                   <<calibration->supportPosition.y<<','<<calibration->supportPosition.z
                   <<" radius="<<calibration->captureRadius
                   <<" totalProfiles="<<savedTwoHandCalibrationCount();
            log(details.str());
            writeTwoHandStatus(saved
                ?std::string("CALIBRATION SAVED: ")+weaponKindName(kind)+"; hold "+supportGripName()+" near its support point to preview"
                :"CALIBRATION ERROR: two_hand_weapon_profiles_saved.cfg could not be written");
        }
    }
    // Calibration confirmation is deliberately never forwarded to DOOM as a shot.
    return triggerDown;
}
void locateGameplayControllerPoses(XrTime displayTime){
    auto locateController=[&](XrAction action,XrSpace space,ControllerPose& controller){
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};info.action=action;
        XrActionStatePose poseState{XR_TYPE_ACTION_STATE_POSE};
        if(XR_FAILED(s.getActionStatePose(s.session,&info,&poseState))||!poseState.isActive){controller.valid=false;controller.linearVelocityValid=false;return;}
        XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        location.next=&velocity;
        if(XR_FAILED(s.locateSpace(space,s.space,displayTime,&location))
            ||!(location.locationFlags&XR_SPACE_LOCATION_POSITION_VALID_BIT)
            ||!(location.locationFlags&XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)){controller.valid=false;controller.linearVelocityValid=false;return;}
        controller.positionTracked=(location.locationFlags&XR_SPACE_LOCATION_POSITION_TRACKED_BIT)!=0;
        controller.valid=true;controller.position=location.pose.position;controller.orientation=location.pose.orientation;
        controller.linearVelocityValid=(velocity.velocityFlags&XR_SPACE_VELOCITY_LINEAR_VALID_BIT)!=0;
        controller.linearVelocity=controller.linearVelocityValid?velocity.linearVelocity:XrVector3f{};
    };
    locateController(s.rightAimPose,s.rightAimSpace,s.rightController);
    locateController(s.leftAimPose,s.leftAimSpace,s.leftController);
    locateController(s.rightGripPose,s.rightGripSpace,s.rightGripController);
    locateController(s.leftGripPose,s.leftGripSpace,s.leftGripController);
}
void setWeaponCvars(float x,float y,float z,float pitch,float yaw,float roll){
    *reinterpret_cast<int*>(doomFloat(0x5B64540))=0;
    // r132's launch argument was overwritten here on every 6DoF update.
    // Keep identity depth while custom hands share the DOOM depth buffer;
    // preserve the accepted guns-only zero-depth behavior when disabled.
    *doomFloat(0x5BA9C44)=kharvox::hands::nativeWeaponDepthScale(s.showHands);
    *doomFloat(0x5BAAAA4)=1.f;
    *doomFloat(0x726C384)=1.f;
    *reinterpret_cast<int*>(doomFloat(0x5BAA3C0))=1;
    *reinterpret_cast<int*>(doomFloat(0x5BAA460))=1;
    *reinterpret_cast<int*>(doomFloat(0x5BAA5A0))=1;
    *reinterpret_cast<int*>(doomFloat(0x5BAA640))=1;
    *reinterpret_cast<int*>(doomFloat(0x5BAFAF0))=0;
    *reinterpret_cast<int*>(doomFloat(0x5BAE5F0))=0;
    *doomFloat(0x5BAA6E4)=x;*doomFloat(0x5BAA784)=y;*doomFloat(0x5BAA824)=z;
    *doomFloat(0x5BAA8C4)=pitch;*doomFloat(0x5BAA964)=yaw;*doomFloat(0x5BAAA04)=roll;
}
bool supportGripInCaptureRange(){
    if(!s.twoHandEnabled)return false;
    const auto* calibration=twoHandCalibrationFor(KharvoxWeaponCurrentKind());
    if(!calibration||!calibration->valid)return false;
    const auto& primary=weaponController();
    const auto& support=supportGripController();
    if(!primary.valid||!support.valid)return true;
    const auto current=controllerRelativeToBodyTrackingOrigin(primary);
    const auto grip=controllerRelativeToBodyTrackingOrigin(support);
    const auto active=activeHandCalibration(*calibration);
    const auto target=addVector(current.position,rotateVector(current.orientation,active.supportPosition));
    return vectorLength(subtractVector(grip.position,target))<=active.captureRadius;
}
void updateWeapon6Dof(){
    const bool gameplay=KharvoxCameraGameplayActive()
        &&!KharvoxWeaponCollectibleAnimationActive()
        &&!KharvoxCameraCutsceneActive()&&!KharvoxHudPauseMenuActive()
        &&!KharvoxHudDeathMenuActive()
        &&!KharvoxHudFullscreenMenuActive()
        &&!s.cinematicBodyPoseHeld;
    if(!s.weapon6Dof||!s.headZeroValid||!gameplay){
        KharvoxWeaponSetControllerPose(0,0,0,0,0,0,0,0,0,1,false);

        s.weaponTrackingHeld=false;s.twoHandLatched=false;s.twoHandLatchedWeapon=KharvoxWeaponKind::Unknown;
        if(s.twoHandEnabled)writeTwoHandStatus(s.twoHandCalibrationMode
            ?std::string("CALIBRATION: enter gameplay and equip ")+weaponKindName(s.twoHandCalibrationTarget)
            :"GAMEPLAY TEST: waiting for gameplay weapon data");
        return;
    }

    // OpenXR can briefly invalidate controller poses while the headset is
    // recentered. Keep the last submitted transform active during that gap;
    // disabling the hook for even one gameplay frame lets the native viewmodel
    // take over and its hit/pushback offset can become the new visible pose.
    const auto& primaryController=weaponController();
    const auto& supportController=supportGripController();
    if(!primaryController.valid){

        if(s.weaponPoseSubmitted){
            if(!s.weaponTrackingHeld)
                log("[WEAPON] controller tracking temporarily invalid; holding last valid 6DoF pose");
            s.weaponTrackingHeld=true;
            return;
        }
        KharvoxWeaponSetControllerPose(0,0,0,0,0,0,0,0,0,1,false);
        return;
    }
    if(s.weaponTrackingHeld){
        log("[WEAPON] controller tracking restored; resumed without native viewmodel takeover");
        s.weaponTrackingHeld=false;
    }

    const auto current=controllerRelativeToBodyTrackingOrigin(primaryController);
    if(!s.weaponCalibrated){
        s.weaponBaseline=current;
        s.weaponCalibrated=true;
        log("[WEAPON] initial controller pose calibrated in body-follow tracking space");
    }
    if(!s.weaponGameplayActivated){
        s.weaponGameplayActivated=true;
        KharvoxWeaponResetCalibration();
        setWeaponCvars(0,0,0,0,0,0);
        log("[WEAPON] gameplay activated with body-relative controller pose; no startup rotation baseline");
    }
    const auto kind=KharvoxWeaponCurrentKind();
    auto rotation=current.orientation;
    if(s.twoHandEnabled){
        if(s.twoHandLatched&&s.twoHandLatchedWeapon!=kind){
            log(std::string("[TWO-HAND] support released after weapon change from ")
                +KharvoxWeaponKindKey(s.twoHandLatchedWeapon)+" to "+KharvoxWeaponKindKey(kind));
            s.twoHandLatched=false;
            s.twoHandLatchedWeapon=KharvoxWeaponKind::Unknown;
        }
        auto* calibration=twoHandCalibrationFor(kind);
        if(kind==KharvoxWeaponKind::Unknown||!calibration){
            s.twoHandLatched=false;
            s.twoHandLatchedWeapon=KharvoxWeaponKind::Unknown;
            writeTwoHandStatus("Unknown weapon: safe one-handed fallback; waiting for native weapon identity");
        }else if(s.twoHandCalibrationMode&&kind!=s.twoHandCalibrationTarget){
            s.twoHandLatched=false;
            s.twoHandLatchedWeapon=KharvoxWeaponKind::Unknown;
            writeTwoHandStatus(s.twoHandCalibrationTarget==KharvoxWeaponKind::Bfg
                ?std::string("CALIBRATION: hold ")+supportGripName()+" for 650 ms to equip BFG 9000, then release it"
                :std::string("CALIBRATION: selected ")+weaponKindName(s.twoHandCalibrationTarget)
                    +"; switch from "+weaponKindName(kind)+" before capture");
        }else if(!calibration->valid){
            s.twoHandLatched=false;
            s.twoHandLatchedWeapon=KharvoxWeaponKind::Unknown;
            writeTwoHandStatus(s.twoHandCalibrationMode
                ?std::string("CALIBRATION: hold ")+weaponKindName(kind)+" with both hands and press "+weaponTriggerName()+" to capture"
                :std::string(weaponKindName(kind))+": calibration required — select it in Calibration mode in the launcher");
        }else if(!s.supportGripPressed){
            if(s.twoHandLatched)log(std::string("[TWO-HAND] ")+KharvoxWeaponKindKey(kind)+" support grip released");
            s.twoHandLatched=false;
            s.twoHandLatchedWeapon=KharvoxWeaponKind::Unknown;
            writeTwoHandStatus(s.twoHandCalibrationMode
                ?std::string("CALIBRATION READY: ")+weaponKindName(kind)+"; "+weaponTriggerName()+" recaptures, "+supportGripName()+" previews"
                :std::string(weaponKindName(kind))+": hold "+supportGripName()+" near the calibrated support point");
        }else if(!supportController.valid){
            writeTwoHandStatus(std::string(weaponKindName(kind))+": "+supportHandName()+" controller tracking unavailable; holding one-hand orientation");
        }else{
            const auto supportGrip=controllerRelativeToBodyTrackingOrigin(supportController);
            const auto activeCalibration=activeHandCalibration(*calibration);
            if(!s.twoHandLatched){
                const auto target=addVector(current.position,rotateVector(current.orientation,activeCalibration.supportPosition));
                const float distance=vectorLength(subtractVector(supportGrip.position,target));
                if(distance<=activeCalibration.captureRadius){
                    s.twoHandLatched=true;
                    s.twoHandLatchedWeapon=kind;
                    log(std::string("[TWO-HAND] ")+KharvoxWeaponKindKey(kind)+" support acquired mode="+twoHandModeName(activeCalibration.aimMode)
                        +" distance="+std::to_string(distance));
                }
            }
            if(s.twoHandLatched){
                const bool useVirtualGunstock=s.virtualGunstockEnabled
                    &&activeCalibration.aimMode==TwoHandAimMode::Barrel
                    &&s.trackingHeadPositionValid;
                auto gunstockPosition=useVirtualGunstock
                    ?positionRelativeToBodyTrackingOrigin(s.trackingHeadPosition)
                    :XrVector3f{};
                if(useVirtualGunstock)gunstockPosition.y-=.18f;
                rotation=solveTwoHandOrientation(current,supportGrip,activeCalibration,
                    useVirtualGunstock,gunstockPosition);
                writeTwoHandStatus(std::string(weaponKindName(kind))+": TWO-HAND ACTIVE — "
                    +(useVirtualGunstock?"virtual stock / ":"")
                    +twoHandModeName(activeCalibration.aimMode)+" alignment");
            }else{
                writeTwoHandStatus(std::string(weaponKindName(kind))+": "+supportHandName()+" grip held, move support hand onto its calibrated grab point");
            }
        }
    }
    const WeaponPose publishedWeaponPose{current.position,rotation};
    if(s.controllerFrameYaw.active&&kharvox::pose_trace::active.load(std::memory_order_relaxed)){
        kharvox::pose_trace::Event e{};e.kind=kharvox::pose_trace::ControllerTurnFrame;e.frame=s.frame;
        e.data[0]=s.controllerFrameYaw.turnForPose(artificialTurnVisualYaw());
        e.data[1]=artificialTurnVisualYaw();e.data[2]=s.controllerFrameYaw.accepted;
        e.data[3]=s.artificialTurnPublishedDegrees;kharvox::pose_trace::record(e);
    }
    const XrVector3f grip{-publishedWeaponPose.position.z*s.worldScale,
        -publishedWeaponPose.position.x*s.worldScale,
        publishedWeaponPose.position.y*s.worldScale};
    const XrVector3f baselineGrip{-s.weaponBaseline.position.z*s.worldScale,-s.weaponBaseline.position.x*s.worldScale,s.weaponBaseline.position.y*s.worldScale};
    setWeaponCvars(0,0,0,0,0,0);
    KharvoxWeaponSetControllerPose(grip.x,grip.y,grip.z,
        baselineGrip.x,baselineGrip.y,baselineGrip.z,
        publishedWeaponPose.orientation.x,publishedWeaponPose.orientation.y,
        publishedWeaponPose.orientation.z,publishedWeaponPose.orientation.w,true);
    s.weaponPoseSubmitted=true;


}
void updateGameplayActions(XrTime displayTime){
    if(!s.actionsReady){s.primaryFireDown=false;return;}
    *reinterpret_cast<int*>(doomFloat(0x5B64540))=0;
    holdNativeTurnCvars(manualTurnCarrierSpeed());
    static bool yawSpeedHoldLogged=false;
    if(!yawSpeedHoldLogged){yawSpeedHoldLogged=true;std::ostringstream o;o<<"[TURN] holding live joy_yawSpeed current-value RVA 0x"<<std::hex<<joyYawSpeedCurrentRva<<std::dec<<" at "<<manualTurnCarrierSpeed()<<" only for hidden body catch-up; visible Smooth Turn uses OpenXR frame time";log(o.str());}
    XrActiveActionSet active{s.gameplayActionSet,XR_NULL_PATH}; XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO}; sync.countActiveActionSets=1; sync.activeActionSets=&active;
    if(XR_FAILED(s.syncActionsFn(s.session,&sync))
        ||s.sessionState!=XR_SESSION_STATE_FOCUSED){
        s.motionWheelState={};
        s.wheelStickHaptics={};
        s.equipmentGrip.cancel();
        clearCapturedXInputRumble();
        releaseMovement();
        updateVirtualButtons(false,false,false,false,false,false,false,false,false,false,false,false,false);
        s.meleePressed=false;s.usePulseUntil=0;s.meleePulseUntil=0;
        s.physicalPunchArmed={false,false};s.physicalPunchCooldownUntil=0;
        s.weaponSelectPressed=false;s.weaponSelectNativeStarted=false;s.weaponWheelOpened=false;
        s.weaponSelectPressedTime=0;s.weaponSwitchPulseUntil=0;
        s.bfgGripHoldStart=0;s.bfgPulseUntil=0;s.bfgGripTriggered=false;
        s.bfgGripSuppressedUntilRelease=s.supportGripPressed;
        s.backWeaponState.zoneActive=false;
        s.backWeaponState.selectionPhase=kharvox::BackWeaponSelectionPhase::Idle;
        s.backWeaponState.pendingTarget=kharvox::BackWeaponKind::Unknown;
        s.backWeaponState.selectionDeadlineNanoseconds=0;
        s.backWeaponPulseUntil=0;
        s.bodyFollowTurnX=0.f;s.previousManualTurnActive=false;

        s.primaryFireDown=false;
        KharvoxCameraSetCrouchState(false);
        return;
    }
    refreshGripInteractionProfiles();
    locateGameplayControllerPoses(displayTime);
    auto readFloatAction=[](XrAction action){XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};info.action=action;XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};return XR_SUCCEEDED(s.getActionStateFloat(s.session,&info,&state))&&state.isActive?state.currentState:0.f;};
    auto readBooleanAction=[](XrAction action){XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};info.action=action;XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};return XR_SUCCEEDED(s.getActionStateBoolean(s.session,&info,&state))&&state.isActive&&state.currentState;};
    XrActionStateGetInfo moveInfo{XR_TYPE_ACTION_STATE_GET_INFO};moveInfo.action=s.doomMove;XrActionStateVector2f moveState{XR_TYPE_ACTION_STATE_VECTOR2F};
    const bool moveActionActive=XR_SUCCEEDED(s.getActionStateVector2f(s.session,&moveInfo,&moveState))&&moveState.isActive;
    s.leftStick=moveActionActive?moveState.currentState:XrVector2f{};
    const bool gameplayWorld=KharvoxCameraGameplayActive();
    const bool cutscene=KharvoxCameraCutsceneActive();
    const bool pauseMenu=KharvoxHudPauseMenuActive();
    const bool fullscreenMenu=KharvoxHudFullscreenMenuActive();
    const bool deathMenu=KharvoxHudDeathMenuActive();
    const bool nativeUiMenu=fullscreenMenu&&!pauseMenu&&!deathMenu;
    const bool gameplay=gameplayWorld&&!cutscene&&!pauseMenu&&!fullscreenMenu&&!deathMenu;
    serviceBackWeaponKeyboardPulse(displayTime,gameplay);
    if(nativeUiMenu!=s.nativeUiInputContext){
        s.nativeUiInputContext=nativeUiMenu;
        log(nativeUiMenu
            ?"[FULLSCREEN-UI] native UI routing active; centered logical sticks and handedness-aware controls"
            :"[FULLSCREEN-UI] native UI routing released");
    }
    // Ledge pulls, Glory Kills, and other first-person assists use DOOM's
    // cinematic camera while the gameplay world remains active. They must keep
    // the gameplay A/B mapping. Only a real UI/loading context may use native
    // label-matched A=confirm and B=back.
    const bool faceButtonGameplayContext=gameplayWorld&&!pauseMenu
        &&!fullscreenMenu&&!deathMenu;
    XrActionStateGetInfo turnInfo{XR_TYPE_ACTION_STATE_GET_INFO};turnInfo.action=s.doomTurn;XrActionStateVector2f turnState{XR_TYPE_ACTION_STATE_VECTOR2F};const bool turnActive=XR_SUCCEEDED(s.getActionStateVector2f(s.session,&turnInfo,&turnState))&&turnState.isActive;if(turnActive)s.rightStick=turnState.currentState;else s.rightStick={};
    // Independent vertical deadzone; never rescale X or alter turn sensitivity.
    constexpr float rightStickVerticalDeadzone=.4f;
    const float rightStickY=std::isfinite(s.rightStick.y)
        ?std::copysign(std::clamp((std::abs(s.rightStick.y)-rightStickVerticalDeadzone)
            /(1.f-rightStickVerticalDeadzone),0.f,1.f),s.rightStick.y):0.f;
    if(gameplay&&std::abs(s.rightStick.x)>=s.turnDeadzone
        &&kharvox::cancelPostCinematicYawGuard(s.postCinematicYawGuard)){
        log("[BODY-YAW] post-cinematic native yaw guard released reason=manual-turn absorbed="
            +std::to_string(s.postCinematicYawGuard.absorbedDegrees)+"deg");
    }
    const float fireValue=readFloatAction(s.doomFire);
    const bool triggerDown=fireValue>.55f;
    const bool weaponSelectDown=gameplay&&turnActive&&rightStickY<-.75f;
    const bool secondaryFireGripDown=kharvox::updateGripPressed(
        readFloatAction(s.doomSecondaryFireGrip),s.secondaryFireGripPressed,
        s.primaryGripUsesValveIndex
            ?kharvox::GripControllerProfile::ValveIndex
            :kharvox::GripControllerProfile::Default);
    const bool supportGripDown=kharvox::updateGripPressed(
        readFloatAction(s.doomSupportGrip),s.supportGripPressed,
        s.supportGripUsesValveIndex
            ?kharvox::GripControllerProfile::ValveIndex
            :kharvox::GripControllerProfile::Default);
    const bool cycleEquipment=s.equipmentGrip.update(supportGripDown,gameplay,
        supportGripController().valid&&weaponController().valid,
        s.twoHandLatched||s.twoHandCalibrationMode||supportGripInCaptureRange(),displayTime);
    if(supportGripDown!=s.supportGripPressed){
        log(std::string("[TWO-HAND] ")+supportGripName()+" support input "
            +(supportGripDown?"pressed":"released"));
        s.supportGripPressed=supportGripDown;
    }
    constexpr XrDuration bfgGripHoldDuration=650000000;
    const auto activeWeaponKind=KharvoxWeaponCurrentKind();
    const bool bfgCalibrationAccess=s.twoHandCalibrationMode
        &&s.twoHandCalibrationTarget==KharvoxWeaponKind::Bfg
        &&activeWeaponKind!=KharvoxWeaponKind::Bfg;
    const bool supportGripOwnsPress=s.twoHandLatched
        ||(s.twoHandCalibrationMode&&!bfgCalibrationAccess);
    if(!supportGripDown){
        s.bfgGripHoldStart=0;
        s.bfgGripTriggered=false;
        s.bfgGripSuppressedUntilRelease=false;
    }else if(!gameplay||supportGripOwnsPress){
        // An acquired support grip owns this press. Calibration owns it too,
        // except while BFG is the selected target and still needs the native
        // long-grip shortcut to become the active weapon.
        s.bfgGripHoldStart=0;
        s.bfgGripSuppressedUntilRelease=true;
    }else if(!s.bfgGripSuppressedUntilRelease&&!s.bfgGripTriggered){
        if(!s.bfgGripHoldStart)s.bfgGripHoldStart=displayTime;
        else if(displayTime-s.bfgGripHoldStart>=bfgGripHoldDuration){
            s.bfgGripTriggered=true;
            s.bfgPulseUntil=displayTime+100000000;
            log(std::string("[INPUT] ")+supportGripName()+" hold -> native Xbox-Y/JOY4 BFG pulse"
                +(bfgCalibrationAccess?" for calibration access":""));
        }
    }
    if(!gameplay)s.bfgPulseUntil=0;
    const bool bfgPulse=gameplay&&displayTime<s.bfgPulseUntil;
    if(weaponSelectDown&&!s.weaponSelectPressed){
        s.weaponSelectPressedTime=displayTime;
        s.weaponSelectNativeStarted=false;s.weaponWheelOpened=false;
        log(std::string("[INPUT] ")+turnStickName()+" down; tap/hold weapon selection started");
    }
    if(weaponSelectDown&&!s.weaponSelectNativeStarted
        &&s.weaponSelectPressedTime>0&&displayTime-s.weaponSelectPressedTime>=400000000){
        s.weaponSelectNativeStarted=true;
        log(std::string("[INPUT] ")+turnStickName()+" down hold -> native weapon wheel button started");
    }
    if(weaponSelectDown&&s.weaponSelectNativeStarted&&!s.weaponWheelOpened
        &&displayTime-s.weaponSelectPressedTime>=800000000){
        s.weaponWheelOpened=true;
        log(std::string("[INPUT] weapon wheel active; ")+moveStickName()+" drives selection while "+turnStickName()+" remains down");
    }
    if(s.weaponSelectPressed&&!weaponSelectDown){
        if(s.weaponSelectNativeStarted)log(std::string("[INPUT] weapon wheel/")+turnStickName()+" down released; selection confirmed");
        else{
            s.weaponSwitchPulseUntil=displayTime+100000000;
            log(std::string("[INPUT] ")+turnStickName()+" down tap -> switch weapon pulse");
        }
        s.weaponSelectNativeStarted=false;
        s.weaponWheelOpened=false;s.weaponSelectPressedTime=0;
    }
    if(!gameplay){
        s.weaponSelectNativeStarted=false;
        s.weaponWheelOpened=false;s.weaponSelectPressedTime=0;s.weaponSwitchPulseUntil=0;
    }
    s.weaponSelectPressed=weaponSelectDown;
    const bool nativeWeaponSelectDown=gameplay&&((weaponSelectDown&&s.weaponSelectNativeStarted)
        ||displayTime<s.weaponSwitchPulseUntil);
    const bool weaponWheelActive=weaponSelectDown&&s.weaponWheelOpened;
    const bool rawBackWeaponGripPress=secondaryFireGripDown
        &&!s.secondaryFireGripPressed;
    const auto& shoulderWeaponGripController=s.leftHanded
        ?s.leftGripController:s.rightGripController;
    const bool backWeaponTrackingValid=shoulderWeaponGripController.valid
        &&s.trackingHeadPositionValid&&s.headZeroValid&&s.headZeroPositionValid;
    kharvox::BackWeaponVec3 backWeaponGripRelativeToHead{};
    if(backWeaponTrackingValid){
        const auto grip=controllerRelativeToBodyTrackingOrigin(shoulderWeaponGripController).position;
        const auto head=positionRelativeToBodyTrackingOrigin(s.trackingHeadPosition);
        backWeaponGripRelativeToHead={
            kharvox::backWeaponNormalizeShoulderSide(grip.x-head.x,s.leftHanded),
            grip.y-head.y,grip.z-head.z};
    }
    const bool previousBackZone=s.backWeaponState.zoneActive;
    std::uint16_t backWeaponAmmoKnownMask{};
    std::uint16_t backWeaponAmmoUsableMask{};
    constexpr std::array<kharvox::BackWeaponKind,10> selectableBackWeapons{
        kharvox::BackWeaponKind::Pistol,kharvox::BackWeaponKind::CombatShotgun,
        kharvox::BackWeaponKind::PlasmaRifle,kharvox::BackWeaponKind::HeavyAssaultRifle,
        kharvox::BackWeaponKind::RocketLauncher,kharvox::BackWeaponKind::SuperShotgun,
        kharvox::BackWeaponKind::GaussCannon,kharvox::BackWeaponKind::Chaingun,
        kharvox::BackWeaponKind::Bfg,kharvox::BackWeaponKind::Chainsaw};
    for(const auto kind:selectableBackWeapons){
        const auto ammo=KharvoxWeaponGetAmmoState(nativeWeaponKindFromBack(kind));
        const auto mask=kharvox::backWeaponKindMask(kind);
        if(ammo!=KharvoxWeaponAmmoState::Unknown)backWeaponAmmoKnownMask|=mask;
        if(ammo==KharvoxWeaponAmmoState::Usable)backWeaponAmmoUsableMask|=mask;
    }
    kharvox::BackWeaponInput backWeaponInput{};
    backWeaponInput.shoulderGestureEnabled=true;
    backWeaponInput.gameplayActive=gameplay;
    backWeaponInput.weaponWheelActive=weaponWheelActive;
    backWeaponInput.calibrationMode=s.twoHandCalibrationMode;
    backWeaponInput.trackingValid=backWeaponTrackingValid;
    backWeaponInput.gripRelativeToHeadMeters=backWeaponGripRelativeToHead;
    backWeaponInput.gripDown=secondaryFireGripDown;
    backWeaponInput.triggerDown=triggerDown;
    backWeaponInput.favorite=s.favoriteBackWeapon;
    backWeaponInput.activeWeapon=backWeaponKindFromActive(activeWeaponKind);
    backWeaponInput.nowNanoseconds=static_cast<std::uint64_t>(std::max<XrTime>(0,displayTime));
    backWeaponInput.ammoKnownMask=backWeaponAmmoKnownMask;
    backWeaponInput.ammoUsableMask=backWeaponAmmoUsableMask;
    const auto backWeaponOutput=kharvox::updateBackWeapon(
        s.backWeaponState,backWeaponInput);
    if(rawBackWeaponGripPress){
        std::ostringstream diagnostic;
        diagnostic<<"[BACK-WEAPON] physical "<<weaponGripName()<<" edge ";
        if(backWeaponTrackingValid)
            diagnostic<<"bodyRelativeToHmd=(outward="<<backWeaponGripRelativeToHead.x
                <<"m, vertical="<<backWeaponGripRelativeToHead.y
                <<"m, behind="<<backWeaponGripRelativeToHead.z<<"m)";
        else diagnostic<<"tracking=invalid";
        diagnostic<<" zone="<<(backWeaponOutput.zoneActive?"inside":"outside")
            <<" gameplay="<<(gameplay?"yes":"no")
            <<" wheel="<<(weaponWheelActive?"yes":"no")
            <<" calibration="<<(s.twoHandCalibrationMode?"yes":"no")
            <<" shoulderAmmo="<<weaponAmmoStateKey(
                KharvoxWeaponGetAmmoState(nativeWeaponKindFromBack(s.favoriteBackWeapon)));
        log(diagnostic.str());
    }
    if(previousBackZone!=backWeaponOutput.zoneActive)
        log(std::string("[BACK-WEAPON] ")+(s.leftHanded?"left":"right")+"-back zone "
            +(backWeaponOutput.zoneActive?"ENTER":"EXIT"));
    if(backWeaponOutput.favoriteSkippedUnusable)
        log(std::string("[BACK-WEAPON] Shoulder Weapon skipped before selection: ")
            +backWeaponKindKey(s.favoriteBackWeapon)+" is unavailable or out of ammo");
    if(backWeaponOutput.shotgunSkippedUnusable)
        log("[BACK-WEAPON] Combat Shotgun skipped before selection: unavailable or out of ammo");
    if(backWeaponOutput.targetAlreadyActive)
        log(std::string("[BACK-WEAPON] selection target already active: ")
            +backWeaponKindKey(backWeaponOutput.activatedWeapon)+"; Grip consumed without pulse");
    if(backWeaponOutput.targetRequested)
        log(std::string("[BACK-WEAPON] fresh ")+weaponGripName()+" -> request Shoulder Weapon="
            +backWeaponKindKey(s.favoriteBackWeapon));
    if(backWeaponOutput.targetBecameActive)
        log(std::string("[BACK-WEAPON] shoulder selection became active: ")
            +backWeaponKindKey(backWeaponOutput.activatedWeapon));
    if(backWeaponOutput.shotgunFallbackRequested)
        log(backWeaponOutput.favoriteSkippedUnusable
            ?"[BACK-WEAPON] Combat Shotgun fallback requested immediately"
            :"[BACK-WEAPON] Shoulder Weapon did not become active within 1750 ms; Combat Shotgun fallback requested");
    if(backWeaponOutput.pistolFallbackRequested)
        log(backWeaponOutput.shotgunSkippedUnusable
            ?"[BACK-WEAPON] Pistol fallback requested immediately"
            :"[BACK-WEAPON] Combat Shotgun did not become active within 1750 ms; Pistol fallback requested");
    if(backWeaponOutput.selectionExhausted)
        log("[BACK-WEAPON] Pistol fallback did not become active; selection ended");
    if(backWeaponOutput.selectionAborted)
        log("[BACK-WEAPON] pending selection aborted because gameplay/tracking context ended");
    if(backWeaponOutput.pulse!=kharvox::BackWeaponPulse::None){
        if(backWeaponOutput.pulse==kharvox::BackWeaponPulse::DirectWeapon
            ||backWeaponOutput.pulse==kharvox::BackWeaponPulse::CombatShotgunFallback
            ||backWeaponOutput.pulse==kharvox::BackWeaponPulse::PistolFallback){
            const auto target=backWeaponOutput.pulse==kharvox::BackWeaponPulse::DirectWeapon
                ?s.favoriteBackWeapon
                :backWeaponOutput.pulse==kharvox::BackWeaponPulse::CombatShotgunFallback
                    ?kharvox::BackWeaponKind::CombatShotgun
                    :kharvox::BackWeaponKind::Pistol;
            const WORD key=kharvox::backWeaponKeyboardVirtualKey(target);
            const bool sent=startBackWeaponKeyboardPulse(key,displayTime);
            log(std::string("[BACK-WEAPON] native keyboard one-shot key=")
                +(key?std::string(1,static_cast<char>(key)):"none")
                +(sent?" pressed":" FAILED")+" duration=100ms");
        }else{
            s.backWeaponPulse=backWeaponOutput.pulse;
            s.backWeaponPulseUntil=displayTime+100000000;
            log(std::string("[BACK-WEAPON] native one-shot channel=")
                +(backWeaponOutput.pulse==kharvox::BackWeaponPulse::Bfg
                    ?"Xbox-Y/BFG":"Xbox-X/Chainsaw")+" duration=100ms");
        }
    }
    if(!gameplay||weaponWheelActive||s.twoHandCalibrationMode
        ||!backWeaponTrackingValid)s.backWeaponPulseUntil=0;
    if(secondaryFireGripDown!=s.secondaryFireGripPressed){
        if(backWeaponOutput.gripConsumed)
            log(std::string("[BACK-WEAPON] ")+weaponGripName()+" "
                +(secondaryFireGripDown?"consumed until release":"released"));
        else
            log(std::string("[INPUT] ")+weaponGripName()+" -> weapon mod/secondary fire "
                +(secondaryFireGripDown?"pressed":"released"));
        s.secondaryFireGripPressed=secondaryFireGripDown;
    }
    const bool manualMove=std::abs(s.leftStick.x)>.15f||std::abs(s.leftStick.y)>.15f;
    // Manual locomotion is body-relative by default. Native room-scale body
    // following remains a separate synthesized stick source.
    const XrVector2f nativeUiLeftStick=centeredNativeUiStick(s.leftStick);
    const XrVector2f requestedMovement=nativeUiMenu?nativeUiLeftStick
        :manualMove?movementDirectionRelativeStick(s.leftStick,gameplay)
        :(gameplay?s.roomscaleStick:s.leftStick);
    const XrVector2f movementStick=weaponWheelActive?XrVector2f{}:requestedMovement;
    const bool movementActive=!weaponWheelActive&&(moveActionActive||(gameplay&&(std::abs(movementStick.x)>.001f||std::abs(movementStick.y)>.001f)));
    if(std::abs(movementStick.x)>.15f||std::abs(movementStick.y)>.15f)focusDoomWindow();
    if(xinputHookReady)updateVirtualLeftStick(movementStick,movementActive);else{updateVirtualLeftStick({},false);updateVirtualRightStick({},false);}
    const bool meleeDown=readBooleanAction(s.doomMelee);
    const bool meleeUseDown=gameplay&&meleeDown;
    if(meleeUseDown!=s.meleePressed){
        if(meleeUseDown){
            s.chainsawArmed=false;
            s.chainsawPulseUntil=0;
            s.usePulseUntil=displayTime+100000000;
            s.meleePulseUntil=displayTime+200000000;
            log(std::string("[INPUT] ")+weaponHandName()+" thumbstick -> native JOY_DPAD_RIGHT USE phase, then JOY8 melee/glory phase");
        }else log(std::string("[INPUT] ")+weaponHandName()+" thumbstick released");
        s.meleePressed=meleeUseDown;
    }
    const bool physicalPunchContext=s.physicalGlorykillEnabled&&gameplay
        &&!fullscreenMenu&&!weaponWheelActive;
    const std::array<const ControllerPose*,2> punchControllers{{&s.rightController,&s.leftController}};
    const std::array<const char*,2> punchHandNames{{"right","left"}};
    if(!physicalPunchContext){
        s.physicalPunchArmed={false,false};
    }else{
        const float rearmSpeed=s.physicalGlorykillSpeed*.45f;
        for(size_t hand=0;hand<punchControllers.size();++hand){
            const bool handAllowed=hand==0
                ?s.physicalGlorykillHands!=PhysicalGlorykillHands::Left
                :s.physicalGlorykillHands!=PhysicalGlorykillHands::Right;
            if(!handAllowed){s.physicalPunchArmed[hand]=false;continue;}
            const auto& controller=*punchControllers[hand];
            if(!controller.valid||!controller.linearVelocityValid){s.physicalPunchArmed[hand]=false;continue;}
            const auto forward=normalizeVector(rotateVector(controller.orientation,{0,0,-1}));
            const float forwardSpeed=dotVector(controller.linearVelocity,forward);
            if(forwardSpeed<=rearmSpeed)s.physicalPunchArmed[hand]=true;
            if(!s.physicalPunchArmed[hand]||forwardSpeed<s.physicalGlorykillSpeed
                ||displayTime<s.physicalPunchCooldownUntil)continue;
            s.physicalPunchArmed={false,false};
            s.physicalPunchCooldownUntil=displayTime+350000000;
            s.meleePulseUntil=std::max(s.meleePulseUntil,displayTime+200000000);
            std::ostringstream o;
            o<<"[INPUT] Physical Glorykill "<<punchHandNames[hand]
             <<" punch -> native JOY8 melee/glory pulse forwardSpeed="
             <<std::fixed<<std::setprecision(2)<<forwardSpeed<<"m/s threshold="
             <<s.physicalGlorykillSpeed<<"m/s";
            log(o.str());
            break;
        }
    }
    if(!gameplay){s.usePulseUntil=0;s.meleePulseUntil=0;}
    if(!gameplay||weaponWheelActive){s.snapTurnActivationPending=false;s.artificialTurnActive=false;s.artificialTurnRemainingDegrees=0.f;publishArtificialTurnYaw();}
    const float manualTurnX=nativeManualTurnX(displayTime,turnActive&&gameplay&&!weaponSelectDown&&!weaponWheelActive);
    const bool manualTurnActive=manualTurnX!=0.f;
    const float physicalBodyTurnX=nativePhysicalBodyTurnX(gameplay&&!weaponWheelActive,manualTurnActive);
    const float gameplayTurnX=manualTurnActive?manualTurnX:physicalBodyTurnX;
    holdNativeTurnCvars(physicalBodyTurnX!=0.f?bodyFollowYawSpeed:manualTurnCarrierSpeed());
    auto nativeUiRightStick=centeredNativeUiStick(s.rightStick);
    nativeUiRightStick.y=rightStickY;
    XrVector2f wheelSelectionStick=s.leftStick;
    if(s.motionWheelEnabled){
        // Motion follows the weapon hand as in PR #1; the selection stick is independent.
        const auto& wCtrl=weaponController();
        kharvox::MotionWeaponWheelInput wheelInput{};
        wheelInput.wheelActive=weaponWheelActive;
        wheelInput.trackingValid=wCtrl.valid&&wCtrl.positionTracked&&s.head.valid;
        wheelInput.stickBypass=kharvox::motionWheelStickBypass(
            s.leftStick.x,s.leftStick.y);
        wheelInput.config.nativeStickDeadzone=nativeDoomRightStickDeadzone;
        wheelInput.handPosition={wCtrl.position.x,wCtrl.position.y,wCtrl.position.z};
        wheelInput.hmdOrientation={s.head.orientation.x,s.head.orientation.y,s.head.orientation.z,s.head.orientation.w};
        wheelInput.nowNanoseconds=static_cast<std::uint64_t>(std::max<XrTime>(0,displayTime));
        const bool stickOwned=s.motionWheelState.stickWasActive;
        const bool anchorValid=s.motionWheelState.anchorValid;
        const auto wheelOutput=kharvox::updateMotionWeaponWheel(s.motionWheelState,wheelInput);
        if(weaponWheelActive&&!wheelOutput.stickBypassActive)
            wheelSelectionStick={wheelOutput.stickX,wheelOutput.stickY};
        if(!stickOwned&&s.motionWheelState.stickWasActive)
            log("[MOTION-WHEEL] physical stick active (radial deadzone 0.30)");
        if(stickOwned&&!s.motionWheelState.stickWasActive&&weaponWheelActive)
            log("[MOTION-WHEEL] stick centered; hand motion resumed");
        if(weaponWheelActive&&anchorValid&&!s.motionWheelState.anchorValid)
            log("[MOTION-WHEEL] tracking lost; motion anchor cleared");
        if(weaponWheelActive&&!anchorValid&&s.motionWheelState.anchorValid)
            log("[MOTION-WHEEL] tracking anchor acquired");
        const bool stickOwnsSelection=weaponWheelActive&&wheelOutput.stickBypassActive;
        const bool stickClick=kharvox::updateWeaponWheelStickHaptics(
            s.wheelStickHaptics,s.leftStick.x,s.leftStick.y,stickOwnsSelection);
        const int hapticHand=kharvox::weaponWheelHapticHand(
            stickOwnsSelection,s.leftHanded,s.leftHandSwapSticks);
        // Cancel only the previous owner's UI click; native game rumble still
        // goes through the shared mixer unchanged. Stick clicks need no pose.
        s.wheelClicks[1-hapticHand]={};
        if(!weaponWheelActive||(!stickOwnsSelection&&!wheelInput.trackingValid))
            s.wheelClicks={};
        else if(stickOwnsSelection?stickClick:wheelOutput.triggerHapticPulse)
            kharvox::queueControllerClick(s.wheelClicks[hapticHand],GetTickCount64());
    }
    const XrVector2f nativeRightStick=nativeUiMenu?nativeUiRightStick
        :weaponWheelActive?wheelSelectionStick:XrVector2f{gameplayTurnX,0.f};
    const bool nativeRightStickActive=nativeUiMenu?turnActive
        :(weaponWheelActive||gameplayTurnX!=0.f);
    if(xinputHookReady)updateVirtualRightStick(nativeRightStick,nativeRightStickActive);
    s.previousManualTurnActive=manualTurnActive;
    const bool chainsawUp=gameplay&&turnActive&&!meleeDown&&rightStickY>.75f;
    if(!meleeDown&&rightStickY<.55f)s.chainsawArmed=true;
    if(chainsawUp&&s.chainsawArmed){s.chainsawArmed=false;s.chainsawPulseUntil=displayTime+100000000;log(std::string("[INPUT] doom_chainsaw edge from ")+turnStickName()+" up");}
    const bool backWeaponPulseActive=gameplay&&displayTime<s.backWeaponPulseUntil;
    const bool backWeaponBfgPulse=backWeaponPulseActive
        &&s.backWeaponPulse==kharvox::BackWeaponPulse::Bfg;
    const bool chainsawPulse=(gameplay&&displayTime<s.chainsawPulseUntil)
        ||(backWeaponPulseActive&&s.backWeaponPulse==kharvox::BackWeaponPulse::Chainsaw);
    const bool weaponModDown=gameplay&&secondaryFireGripDown
        &&!backWeaponOutput.gripConsumed;
    const bool calibrationFireBlocked=captureTwoHandCalibration(gameplay&&triggerDown);
    const bool fireDown=triggerDown&&!weaponWheelActive&&!calibrationFireBlocked;
    // This is the primary-fire state after KHARVOX has removed weapon-wheel
    // and calibration ownership. Adaptive triggers must never consume the raw
    // OpenXR analog trigger value.
    s.primaryFireDown=gameplay&&fireDown;
    if(gameplay&&(fireDown||weaponModDown)){
        const auto hapticNow=GetTickCount64();
        s.weaponFireHapticFallbackUntilTick=hapticNow
            +kharvox::weaponFireHapticFallbackHoldMilliseconds;
    }else if(!gameplay){
        s.weaponFireHapticFallbackUntilTick=0;
        s.wheelClicks={};
    }
    s.firePressed=triggerDown;
    const bool jumpDown=readBooleanAction(s.doomJump);
    const bool crouchDown=readBooleanAction(s.doomCrouch);
    if(!s.faceButtonContextInitialized){
        s.faceButtonGameplayContext=faceButtonGameplayContext;
        s.faceButtonContextInitialized=true;
    }else if(faceButtonGameplayContext!=s.faceButtonGameplayContext){
        // A held face button must never change meaning without a physical
        // release. On real gameplay/UI transitions release its native route
        // and require a fresh press in the new context.
        if(jumpDown&&s.jumpPressed){
            s.jumpSuppressedUntilRelease=true;
            s.jumpButtonRoute=FaceButtonRoute::None;
        }
        if(crouchDown&&s.crouchPressed){
            s.crouchSuppressedUntilRelease=true;
            s.crouchButtonRoute=FaceButtonRoute::None;
        }
        s.faceButtonGameplayContext=faceButtonGameplayContext;
        log(std::string("[INPUT] face-button context -> ")
            +(faceButtonGameplayContext?"GAMEPLAY":"UI")
            +((s.jumpSuppressedUntilRelease||s.crouchSuppressedUntilRelease)
                ?"; held button neutralized until release":""));
    }
    const auto previousCrouchRoute=s.crouchButtonRoute;
    auto updateFaceButtonRoute=[&](bool down,bool wasDown,bool& suppressed,FaceButtonRoute& route){
        if(!down){suppressed=false;route=FaceButtonRoute::None;return;}
        if(suppressed){route=FaceButtonRoute::None;return;}
        if(!wasDown||route==FaceButtonRoute::None)
            route=faceButtonGameplayContext?FaceButtonRoute::Gameplay:FaceButtonRoute::Ui;
    };
    updateFaceButtonRoute(jumpDown,s.jumpPressed,s.jumpSuppressedUntilRelease,s.jumpButtonRoute);
    updateFaceButtonRoute(crouchDown,s.crouchPressed,s.crouchSuppressedUntilRelease,s.crouchButtonRoute);
    const auto crouchEventRoute=crouchDown?s.crouchButtonRoute:previousCrouchRoute;
    if(crouchDown!=s.crouchPressed&&crouchEventRoute==FaceButtonRoute::Ui)
        log(std::string("[INPUT] ")+rightFaceButtonHandName()+" lower face button -> native gamepad A/menu confirm "
            +(crouchDown?"pressed":"released"));
    const bool jumpGameplayDown=jumpDown&&!s.jumpSuppressedUntilRelease
        &&s.jumpButtonRoute==FaceButtonRoute::Gameplay;
    const bool jumpUiDown=jumpDown&&!s.jumpSuppressedUntilRelease
        &&s.jumpButtonRoute==FaceButtonRoute::Ui;
    const bool crouchGameplayDown=crouchDown&&!s.crouchSuppressedUntilRelease
        &&s.crouchButtonRoute==FaceButtonRoute::Gameplay;
    const bool crouchUiDown=crouchDown&&!s.crouchSuppressedUntilRelease
        &&s.crouchButtonRoute==FaceButtonRoute::Ui;
    const auto previousJumpPulse=s.handsJump.pulseUntil;
    const bool handsJumpDown=s.handsJump.update(displayTime,
        s.handsJumpEnabled&&gameplay&&!weaponWheelActive&&!s.twoHandCalibrationMode
            &&s.sessionState==XR_SESSION_STATE_FOCUSED,
        s.leftGripController.valid&&s.rightGripController.valid
            &&s.leftGripController.positionTracked&&s.rightGripController.positionTracked
            &&s.leftGripController.linearVelocityValid&&s.rightGripController.linearVelocityValid,
        s.leftGripController.linearVelocity.y,s.rightGripController.linearVelocity.y);
    if(s.handsJump.pulseUntil>previousJumpPulse)
        log("[INPUT] Hands Jump -> native gamepad A jump pulse; leftUp="
            +std::to_string(s.leftGripController.linearVelocity.y)+" rightUp="
            +std::to_string(s.rightGripController.linearVelocity.y)+"m/s");
    const bool gamepadADown=jumpGameplayDown||handsJumpDown||crouchUiDown;
    const bool gamepadBDown=crouchGameplayDown||jumpUiDown;
    KharvoxCameraSetCrouchState(crouchGameplayDown);
    s.jumpPressed=jumpDown;
    s.crouchPressed=crouchDown;
    const bool nativeUseDown=gameplay&&displayTime<s.usePulseUntil;
    const bool nativeMeleeDown=gameplay&&displayTime>=s.usePulseUntil
        &&(meleeDown||displayTime<s.meleePulseUntil);
    const bool equipmentDown=readFloatAction(s.doomEquipment)>.55f;
    const bool missionInfoDown=readBooleanAction(s.doomMissionInfo);
    if(missionInfoDown!=s.missionInfoPressed)
        log(std::string(nativeUiMenu?"[FULLSCREEN-UI] ":"[INPUT] ")+leftFaceButtonHandName()
            +" lower face button -> "+(nativeUiMenu?"native BACK/close ":gameplay?"native JOY10 Dossier ":"native gamepad X/menu delete ")
            +(missionInfoDown?"pressed":"released"));
    s.missionInfoPressed=missionInfoDown;
    const bool switchWeaponModDown=readBooleanAction(s.doomSwitchWeaponMod);
    const bool pauseDown=readBooleanAction(s.doomPause);
    const bool restrictedLeftHandSwap=s.leftHanded&&!s.leftHandSwapSticks;
    if(pauseDown&&!s.pausePressed)log(std::string("[INPUT] ")+pauseHandName()+" thumbstick click -> native JOY9/start pause");
    s.pausePressed=pauseDown;
    if(xinputHookReady){
        // Full-screen game interfaces are native gamepad UIs, not gameplay.
        // Reconstruct their Xbox
        // controls from logical actions so the selected handedness mapping is
        // preserved. Both sticks remain centered/raw and every release is
        // explicit.
        const bool gamepadXDown=chainsawPulse||(!gameplay&&!nativeUiMenu&&missionInfoDown);
        const bool nativeUiRightTriggerDown=restrictedLeftHandSwap?equipmentDown:triggerDown;
        const bool nativeUiLeftTriggerDown=restrictedLeftHandSwap?triggerDown:equipmentDown;
        const bool nativeUiLeftShoulderDown=restrictedLeftHandSwap?secondaryFireGripDown:supportGripDown;
        const bool nativeUiRightShoulderDown=restrictedLeftHandSwap?supportGripDown
            :(secondaryFireGripDown&&!backWeaponOutput.gripConsumed);
        const bool rightTriggerDown=gameplay?fireDown:(nativeUiMenu&&nativeUiRightTriggerDown);
        const bool leftTriggerDown=weaponModDown||(nativeUiMenu&&nativeUiLeftTriggerDown);
        const bool rightThumbDown=nativeMeleeDown||(nativeUiMenu&&meleeDown);
        const bool leftShoulderDown=(gameplay&&equipmentDown)||(nativeUiMenu&&nativeUiLeftShoulderDown);
        const bool rightShoulderDown=nativeWeaponSelectDown
            ||(nativeUiMenu&&nativeUiRightShoulderDown);
        const bool backDown=(gameplay||nativeUiMenu)&&missionInfoDown;
        const bool yDown=bfgPulse||backWeaponBfgPulse||(nativeUiMenu&&switchWeaponModDown);
        updateVirtualButtons(rightTriggerDown,gamepadADown,gamepadBDown,
            leftTriggerDown,rightThumbDown,nativeUseDown,gamepadXDown,
            leftShoulderDown,rightShoulderDown,backDown,
            gameplay&&switchWeaponModDown,yDown,pauseDown,gameplay&&cycleEquipment);
    }else{
        updateVirtualButtons(false,false,false,false,false,false,false,false,false,false,false,false,false);
    }
    auto publishHudHandPose=[&](bool rightHand,const ControllerPose& controller){
        if(!controller.valid||!s.headZeroValid){KharvoxHudSetHandPose(rightHand,0,0,0,0,0,0,1,false);return;}
        const auto current=controllerRelativeToBodyTrackingOrigin(controller);
        const XrVector3f grip{-current.position.z*s.worldScale,-current.position.x*s.worldScale,current.position.y*s.worldScale};
        KharvoxHudSetHandPose(rightHand,grip.x,grip.y,grip.z,current.orientation.x,current.orientation.y,current.orientation.z,current.orientation.w,true);
    };
    publishHudHandPose(true,s.rightGripController);
    publishHudHandPose(false,s.leftGripController);
    updateWeapon6Dof();
}
bool createGameplayActions(){
    s.leftHanded=environmentEnabled("KHARVOX_LEFT_HANDED");
    s.favoriteBackWeapon=loadBackWeaponKind();
    char leftHandSwap[32]{};
    GetEnvironmentVariableA("KHARVOX_LEFT_HAND_SWAP",leftHandSwap,sizeof(leftHandSwap));
    s.leftHandSwapSticks=s.leftHanded&&!_stricmp(leftHandSwap,"buttons-and-sticks");
    log(std::string("[INPUT] handedness=")+(s.leftHanded?"LEFT":"right")
        +" swap="+(s.leftHandSwapSticks?"buttons-and-sticks":s.leftHanded?"buttons":"none"));
    log(std::string("[BACK-WEAPON] configured Shoulder Weapon=")
        +backWeaponKindKey(s.favoriteBackWeapon)
        +(s.leftHanded
            ?"; left Grip behind left shoulder enabled"
            :"; right Grip behind right shoulder enabled"));
    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(si.actionSetName,"kharvox_gameplay");strcpy_s(si.localizedActionSetName,"KHARVOX Gameplay");
    auto r=s.createActionSet(s.instance,&si,&s.gameplayActionSet);if(XR_FAILED(r)){log("[RIGHT] xrCreateActionSet "+result(r));return false;}
    auto createAction=[&](const char*name,const char*localized,XrActionType type,XrAction&action){XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};strcpy_s(ai.actionName,name);strcpy_s(ai.localizedActionName,localized);ai.actionType=type;const auto actionResult=s.createAction(s.gameplayActionSet,&ai,&action);if(XR_FAILED(actionResult))log(std::string("[INPUT] xrCreateAction ")+name+' '+result(actionResult));return XR_SUCCEEDED(actionResult);};
    if(!createAction("right_aim_pose","Right Aim Pose",XR_ACTION_TYPE_POSE_INPUT,s.rightAimPose)
        ||!createAction("left_aim_pose","Left Aim Pose",XR_ACTION_TYPE_POSE_INPUT,s.leftAimPose)
        ||!createAction("right_grip_pose","Right Grip Pose",XR_ACTION_TYPE_POSE_INPUT,s.rightGripPose)
        ||!createAction("left_grip_pose","Left Grip Pose",XR_ACTION_TYPE_POSE_INPUT,s.leftGripPose)
        ||!createAction("doom_move","Doom Move",XR_ACTION_TYPE_VECTOR2F_INPUT,s.doomMove)
        ||!createAction("doom_turn","Doom Turn",XR_ACTION_TYPE_VECTOR2F_INPUT,s.doomTurn)
        ||!createAction("doom_fire","Doom Fire",XR_ACTION_TYPE_FLOAT_INPUT,s.doomFire)
        ||!createAction("doom_jump","Doom Jump",XR_ACTION_TYPE_BOOLEAN_INPUT,s.doomJump)
        ||!createAction("doom_crouch","Doom Crouch",XR_ACTION_TYPE_BOOLEAN_INPUT,s.doomCrouch)
        ||!createAction("doom_melee","Doom Melee",XR_ACTION_TYPE_BOOLEAN_INPUT,s.doomMelee)
        ||!createAction("doom_equipment","Doom Equipment",XR_ACTION_TYPE_FLOAT_INPUT,s.doomEquipment)
        ||!createAction("doom_secondary_fire_grip","Doom Weapon Mod or Secondary Fire",XR_ACTION_TYPE_FLOAT_INPUT,s.doomSecondaryFireGrip)
        ||!createAction("doom_support_grip","Doom Two Hand Support Grip",XR_ACTION_TYPE_FLOAT_INPUT,s.doomSupportGrip)
        ||!createAction("doom_pause","Doom Pause",XR_ACTION_TYPE_BOOLEAN_INPUT,s.doomPause)
        ||!createAction("doom_mission_info","Doom Mission Information",XR_ACTION_TYPE_BOOLEAN_INPUT,s.doomMissionInfo)
        ||!createAction("doom_switch_weapon_mod","Doom Switch Weapon Mod",XR_ACTION_TYPE_BOOLEAN_INPUT,s.doomSwitchWeaponMod))return false;
    if(s.applyHapticFeedback&&s.stopHapticFeedback){
        const bool leftHapticCreated=createAction("left_haptic","Left Haptic",XR_ACTION_TYPE_VIBRATION_OUTPUT,s.leftHaptic);
        const bool rightHapticCreated=createAction("right_haptic","Right Haptic",XR_ACTION_TYPE_VIBRATION_OUTPUT,s.rightHaptic);
        if(leftHapticCreated&&rightHapticCreated){
            s.hapticActionsReady=true;
            log("[HAPTICS] OpenXR core vibration output actions created");
        }else{
            if(s.leftHaptic!=XR_NULL_HANDLE)s.destroyAction(s.leftHaptic);
            if(s.rightHaptic!=XR_NULL_HANDLE)s.destroyAction(s.rightHaptic);
            s.leftHaptic=XR_NULL_HANDLE;s.rightHaptic=XR_NULL_HANDLE;
            log("[HAPTICS] vibration actions unavailable; gameplay input remains active");
        }
    }
    PFN_xrStringToPath stringToPath{};r=s.getProc(s.instance,"xrStringToPath",reinterpret_cast<PFN_xrVoidFunction*>(&stringToPath));if(XR_FAILED(r)||!stringToPath){log("[RIGHT] xrStringToPath unavailable");return false;}
    const bool gripProfilePathsReady=
        XR_SUCCEEDED(stringToPath(s.instance,"/user/hand/left",&s.leftHandUserPath))
        &&XR_SUCCEEDED(stringToPath(s.instance,"/user/hand/right",&s.rightHandUserPath))
        &&XR_SUCCEEDED(stringToPath(s.instance,"/interaction_profiles/valve/index_controller",&s.valveIndexProfilePath));
    if(!gripProfilePathsReady){
        s.leftHandUserPath=XR_NULL_PATH;
        s.rightHandUserPath=XR_NULL_PATH;
        s.valveIndexProfilePath=XR_NULL_PATH;
        log("[INPUT] active grip profile paths unavailable; retaining default grip threshold");
    }
    auto suggestProfile=[&](const char*profileName,const char*label,bool questTouch,
            kharvox::GripControllerProfile gripProfile){
        XrPath profile{},rightPosePath{},leftPosePath{},rightGripPosePath{},leftGripPosePath{},movePath{},turnPath{},firePath{};
        XrPath jumpPath{},crouchPath{},meleePath{},equipmentPath{};
        XrPath secondaryFireGripPath{},supportGripPath{},missionInfoPath{},switchWeaponModPath{},pausePath{};
        XrPath leftHapticPath{},rightHapticPath{};
        auto resolvePath=[&](const std::string&path,XrPath&output){
            const auto pathResult=stringToPath(s.instance,path.c_str(),&output);
            if(XR_FAILED(pathResult))log(std::string("[INPUT] ")+label+" path failed "+path+' '+result(pathResult));
            return XR_SUCCEEDED(pathResult);
        };
        auto inputPath=[](const char*hand,const char*control){return std::string("/user/hand/")+hand+"/input/"+control;};
        auto faceButtonPath=[&](const char*hand,bool upper){
            const char*button=upper?"b":"a";
            if(questTouch&&!strcmp(hand,"left"))button=upper?"y":"x";
            return std::string("/user/hand/")+hand+"/input/"+button+"/click";
        };
        const char*primaryHand=s.leftHanded?"left":"right";
        const char*supportHand=s.leftHanded?"right":"left";
        // The first launcher mode is intentionally narrow: face buttons stay
        // on their original controls, while the two thumbstick-click actions
        // swap as a pair (left Melee/Use, right Pause). The second mode retains
        // the complete controller-side button swap.
        const bool fullButtonSwap=s.leftHanded&&s.leftHandSwapSticks;
        const char*rightButtonHand=fullButtonSwap?primaryHand:"right";
        const char*leftButtonHand=fullButtonSwap?supportHand:"left";
        const char*pauseHand=s.leftHanded?"right":"left";
        const char*moveHand=s.leftHandSwapSticks?"right":"left";
        const char*turnHand=s.leftHandSwapSticks?"left":"right";
        if(!resolvePath(profileName,profile)
            ||!resolvePath("/user/hand/right/input/aim/pose",rightPosePath)
            ||!resolvePath("/user/hand/left/input/aim/pose",leftPosePath)
            ||!resolvePath("/user/hand/right/input/grip/pose",rightGripPosePath)
            ||!resolvePath("/user/hand/left/input/grip/pose",leftGripPosePath)
            ||!resolvePath(inputPath(moveHand,"thumbstick"),movePath)
            ||!resolvePath(inputPath(turnHand,"thumbstick"),turnPath)
            ||!resolvePath(inputPath(primaryHand,"trigger/value"),firePath)
            ||!resolvePath(faceButtonPath(rightButtonHand,true),jumpPath)
            ||!resolvePath(faceButtonPath(rightButtonHand,false),crouchPath)
            ||!resolvePath(inputPath(primaryHand,"thumbstick/click"),meleePath)
            ||!resolvePath(inputPath(supportHand,"trigger/value"),equipmentPath)
            ||!resolvePath(inputPath(primaryHand,kharvox::gripInputComponent(gripProfile)),secondaryFireGripPath)
            ||!resolvePath(inputPath(supportHand,kharvox::gripInputComponent(gripProfile)),supportGripPath)
            ||!resolvePath(inputPath(pauseHand,"thumbstick/click"),pausePath)
            ||!resolvePath(faceButtonPath(leftButtonHand,false),missionInfoPath)
            ||!resolvePath(faceButtonPath(leftButtonHand,true),switchWeaponModPath))return false;
        std::vector<XrActionSuggestedBinding>bindings{
            {s.rightAimPose,rightPosePath},{s.leftAimPose,leftPosePath},{s.rightGripPose,rightGripPosePath},{s.leftGripPose,leftGripPosePath},
            {s.doomMove,movePath},{s.doomTurn,turnPath},{s.doomFire,firePath},
            {s.doomJump,jumpPath},{s.doomCrouch,crouchPath},{s.doomMelee,meleePath},
            {s.doomEquipment,equipmentPath},
            {s.doomSecondaryFireGrip,secondaryFireGripPath},{s.doomSupportGrip,supportGripPath},{s.doomPause,pausePath},
            {s.doomMissionInfo,missionInfoPath},{s.doomSwitchWeaponMod,switchWeaponModPath}
        };
        bool hapticBindingsIncluded=false;
        if(s.hapticActionsReady
            &&resolvePath("/user/hand/left/output/haptic",leftHapticPath)
            &&resolvePath("/user/hand/right/output/haptic",rightHapticPath)){
            bindings.push_back({s.leftHaptic,leftHapticPath});
            bindings.push_back({s.rightHaptic,rightHapticPath});
            hapticBindingsIncluded=true;
        }
        XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        sb.interactionProfile=profile;sb.suggestedBindings=bindings.data();
        sb.countSuggestedBindings=uint32_t(bindings.size());
        auto bindResult=s.suggestBindings(s.instance,&sb);
        if(XR_FAILED(bindResult)&&hapticBindingsIncluded){
            log(std::string("[HAPTICS] ")+label+" haptic bindings unavailable "+result(bindResult)+"; retrying input-only profile");
            bindings.resize(bindings.size()-2);
            sb.suggestedBindings=bindings.data();
            sb.countSuggestedBindings=uint32_t(bindings.size());
            bindResult=s.suggestBindings(s.instance,&sb);
            hapticBindingsIncluded=false;
        }
        if(XR_SUCCEEDED(bindResult)&&hapticBindingsIncluded){
            s.hapticBindingsSuggested=true;
            log(std::string("[HAPTICS] ")+label+" left/right output bindings loaded");
        }
        log(std::string("[INPUT] ")+label+" profile "+(XR_SUCCEEDED(bindResult)?"loaded":"FAILED "+result(bindResult)));
        return XR_SUCCEEDED(bindResult);
    };
    if(!suggestProfile("/interaction_profiles/oculus/touch_controller","Quest Touch",true,
            kharvox::GripControllerProfile::Default)
        ||!suggestProfile("/interaction_profiles/valve/index_controller","Valve Index",false,
            kharvox::GripControllerProfile::ValveIndex))return false;
    auto createPoseSpace=[&](XrAction action,XrSpace&space,const char*label){
        XrActionSpaceCreateInfo xi{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        xi.action=action;xi.poseInActionSpace.orientation.w=1;
        const auto spaceResult=s.createActionSpace(s.session,&xi,&space);
        if(XR_FAILED(spaceResult))log(std::string("[")+label+"] xrCreateActionSpace "+result(spaceResult));
        return XR_SUCCEEDED(spaceResult);
    };
    if(!createPoseSpace(s.rightAimPose,s.rightAimSpace,"RIGHT")
        ||!createPoseSpace(s.leftAimPose,s.leftAimSpace,"LEFT")
        ||!createPoseSpace(s.rightGripPose,s.rightGripSpace,"RIGHT-GRIP")
        ||!createPoseSpace(s.leftGripPose,s.leftGripSpace,"LEFT-GRIP"))return false;
    XrSessionActionSetsAttachInfo at{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};at.countActionSets=1;at.actionSets=&s.gameplayActionSet;r=s.attachActionSets(s.session,&at);if(XR_FAILED(r)){log("[RIGHT] xrAttachSessionActionSets "+result(r));return false;}
    s.interactionProfilesDirty=true;
    s.interactionProfilesKnown=false;
    configureTurning();configureMovementDirection();configurePhysicalGlorykill();configureLaserSight();configureMotionWeaponWheel();s.actionsReady=true;
    log(std::string("[HAPTICS] OpenXR core rumble ")+(s.hapticActionsReady&&s.hapticBindingsSuggested?"ready":"unavailable; continuing without VR rumble"));
    if(s.hapticActionsReady&&s.hapticBindingsSuggested)
        log("[HAPTICS] weapon-fire fallback armed amplitude="
            +std::to_string(kharvox::weaponFireHapticFallbackAmplitude)
            +" frequencyHz="+std::to_string(kharvox::weaponFireHapticFallbackFrequencyHz));
    log(std::string("[INPUT] KHARVOX gameplay layout ready: handedness=")+(s.leftHanded?"left":"right")
        +" swap="+(s.leftHandSwapSticks?"buttons-and-sticks":s.leftHanded?"buttons":"none")
        +" weaponHand="+weaponHandName()+" supportHand="+supportHandName());
    return true;
}
void updatePresentationMode(){
    static bool optionLogged=false;
    if(!optionLogged){
        log(std::string("[PRESENTATION] Immersive Mode ")+(s.immersiveCinematics?"ENABLED":"disabled")
            +"; cinematic/Glory Kill freelook "+(s.immersiveCinematicFreelook?"ENABLED":"disabled")
            +"; other cinematics in Quad "+(s.otherCinematicsInQuad?"ENABLED":"disabled")
            +"; Cinewindow "+(s.cinewindowFollowsHeadset
                ?"follows headset":"fixed at transition horizon"));
        optionLogged=true;
    }
    const bool hudEverythingQuadKeyDown=(GetAsyncKeyState(VK_F9)&0x8000)!=0;
    if(s.hudEverythingQuadAvailable&&hudEverythingQuadKeyDown&&!s.hudEverythingQuadKeyDown){
        s.hudEverythingQuad=!s.hudEverythingQuad;
        log(std::string("[HUD10-EVERYTHING-QUAD] F9 -> ")
            +(s.hudEverythingQuad
                ?"ACTIVE: complete composed DOOM image on Cinewindow Quad"
                :"disabled: normal automatic Projection/Quad presentation restored"));
    }
    s.hudEverythingQuadKeyDown=hudEverythingQuadKeyDown;
    const bool cutscene=KharvoxCameraCutsceneActive();
    const bool collectibleQuad=KharvoxWeaponCollectibleAnimationActive();
    const bool bossSequence=KharvoxCameraBossSequenceActive();
    static bool previousBossSequence=false;
    if(bossSequence&&!previousBossSequence){
        kharvox::resetCinewindowAnchor(s.cinewindowAnchor);
        s.cinewindowCaptureReadiness.trackedSince=0;
        s.cinewindowPresentationActive=false;
        log("[BOSS-QUAD] r313 centered native boss scene requested");
    }
    previousBossSequence=bossSequence;
    static bool previousCollectibleQuad=false;
    if(collectibleQuad!=previousCollectibleQuad){
        log(collectibleQuad ? "[COLLECTIBLE-QUAD] native toy animation active; centered Quad requested"
            : "[COLLECTIBLE-QUAD] native toy animation ended; automatic presentation restored");
        previousCollectibleQuad=collectibleQuad;
    }
    const bool deathMenuActive=KharvoxHudDeathMenuActive();
    const bool fullscreenMenuActive=KharvoxHudFullscreenMenuActive();
    const bool tutorialActive=KharvoxHudTutorialActive();
    const bool hudMovieActive=KharvoxHudMovieActive();
    static bool previousHudMovie=false;
    if(hudMovieActive && !previousHudMovie){
        kharvox::resetCinewindowAnchor(s.cinewindowAnchor);
        s.cinewindowCaptureReadiness.trackedSince=0;
        s.cinewindowPresentationActive=false;
        log("[HUD-MOVIE] recenter full-frame movie Quad at current headset pose");
    }
    previousHudMovie=hudMovieActive;
    const bool worldActive=KharvoxCameraWorldActive();
    const bool previousImmersiveCinematic=s.immersiveCinematicActive;
    const bool previousImmersiveCinematicFreelook=s.immersiveCinematicFreelookActive;
    if(!cutscene){
        s.firstPersonCinematicAssistHeld=false;
        s.tutorialCinematicProjectionHeld=false;
        s.ordinaryCinematicQuadHeld=false;
    }else if(s.otherCinematicsInQuad){
        if(tutorialActive&&!s.tutorialCinematicProjectionHeld){
            s.tutorialCinematicProjectionHeld=true;
            log("[PRESENTATION] selective Immersive exception latched: Tutorial overlay/video (Quad suppressed)");
        }
        if(!s.tutorialCinematicProjectionHeld&&!s.firstPersonCinematicAssistHeld){
            const bool syncAttack=KharvoxCameraSyncAttackActive();
            const bool ledgeTransition=KharvoxCameraLedgeTransitionActive();
            if(syncAttack||ledgeTransition){
                s.firstPersonCinematicAssistHeld=true;
                log(std::string("[PRESENTATION] selective Immersive exception latched: ")
                    +(syncAttack?"Glory Kill / native sync attack":"Jump / Ledge transition"));
            }else if(!s.ordinaryCinematicQuadHeld){
                s.ordinaryCinematicQuadHeld=true;
                log("[PRESENTATION] ordinary in-game cinematic routed to centered QUAD");
            }
        }
    }
    s.immersiveCinematicActive=!bossSequence&&!hudMovieActive&&!collectibleQuad&&kharvox::shouldKeepCinematicImmersive(
        s.immersiveCinematics,s.otherCinematicsInQuad,
        cutscene,worldActive,fullscreenMenuActive,deathMenuActive,
        s.firstPersonCinematicAssistHeld,s.tutorialCinematicProjectionHeld);
    // Keep the camera override armed during gameplay so a Glory Kill or ledge
    // pull receives the correct FOV and freelook on its very first rendered
    // cinematic frame. Once an ordinary cinematic is identified, disarm it
    // for the centered source frames that will feed the Quad.
    s.immersiveCinematicCameraRequested=s.immersiveCinematics&&!bossSequence&&!hudMovieActive&&!collectibleQuad
        &&(!s.otherCinematicsInQuad||!cutscene||s.firstPersonCinematicAssistHeld
            ||s.tutorialCinematicProjectionHeld);
    s.immersiveCinematicFreelookActive=s.immersiveCinematicActive&&s.immersiveCinematicFreelook&&!bossSequence;
    if(s.immersiveCinematicActive&&!previousImmersiveCinematic)
        log("[PRESENTATION] in-game cinematic remains in PROJECTION (Immersive Mode)");
    if(s.immersiveCinematicFreelookActive&&!previousImmersiveCinematicFreelook)
        log("[IMMERSIVE] HMD freelook active during cinematic / Glory Kill");
    const bool packedFrame=KharvoxCameraNativeStereoActive();
    const bool gameplayFrameActive=KharvoxCameraGameplayActive();
    const bool pauseMenuActive=KharvoxHudPauseMenuActive();
    const bool hudEverythingQuad=s.hudEverythingQuad;
    const bool steamQuadOnly=s.steamQuadOnly;
    // Full-screen overlays such as Dossier, PlayerUpgrade, EndOfLevel, Pause
    // and CampaignDeath are
    // independent of the world-camera lifetime. They use the same QUAD
    // presentation as the already-correct MainMenu without touching SWF scale
    // or mouse/controller hit-test coordinates.
    // Keep the tolerant player-capture signal for other non-input-driven
    // transitions. Pause itself is driven only by its native screen lifecycle.
    const bool gameplayPresentationInterrupted=worldActive&&!cutscene&&!gameplayFrameActive;
    // Classify once at the participant's native entry and hold that decision
    // through its release grace. A passive scene commonly restores controls a
    // frame before its participant disappears; reclassifying live would expose
    // that final authored frame in Projection.
    const bool comfortInteractiveParticipant=
        s.comfortInteractiveParticipantActive;
    const bool comfortParticipantQuad=
        kharvox::shouldRouteNativeParticipantToComfortQuad(
            s.immersiveCinematics,s.nativeAdaptiveParticipantGuardActive,
            tutorialActive,comfortInteractiveParticipant);
    const bool desiredQuad=bossSequence||hudMovieActive||collectibleQuad||comfortParticipantQuad||kharvox::selectQuadPresentation(
        hudEverythingQuad,steamQuadOnly,packedFrame,cutscene,
        s.immersiveCinematicActive,worldActive,
        gameplayPresentationInterrupted,fullscreenMenuActive,
        pauseMenuActive,deathMenuActive,tutorialActive);
    if(kharvox::native::installed()) {
        s.centeredQuadTransitionPending=false;
        s.centeredQuadFramesRemaining=0;
        s.quadMode=desiredQuad;
        return;
    }
    if(kharvox::shouldCancelCenteredQuadTransition(
            desiredQuad,s.centeredQuadTransitionPending)){
        s.centeredQuadTransitionPending=false;
        s.centeredQuadFramesRemaining=0;
        log("[QUAD-CENTER] pending centered Quad cancelled; Projection remains active");
    }
    if(kharvox::shouldBeginCenteredQuadTransition(
            desiredQuad,s.quadMode,s.centeredQuadTransitionPending)){
        // The image reaching Present was rendered before this frame selected
        // its presentation mode. Neutralize the eye camera immediately and
        // expose a black Cinewindow Quad while the known three-Present DOOM
        // queue retires. Replace black only when the source is centre-eye.
        s.centeredQuadTransitionPending=true;
        // The source submitted by this same Present predates the neutral
        // command, so do not count it as one of the three settled frames.
        s.centeredQuadFramesRemaining=
            kharvox::centeredQuadPipelineSettleFrames+1;
        KharvoxCameraSetStereoEye(0,0,0,0,0,false);
        // The first submitted image already belongs to the transition. The copy
        // below remains black until all delayed game-camera frames have retired.
        s.quadMode=s.vk.cmdClearColorImage!=nullptr;
        log(std::string("[QUAD-CENTER] central camera pipeline settle started")
            +(hudEverythingQuad?" (HUD10 complete-frame diagnostic)"
                :steamQuadOnly?" (SteamVR QUAD-only isolation)"
                :deathMenuActive?" (native CampaignDeath menu session)"
                :fullscreenMenuActive?" (full-screen in-game GUI)"
                :pauseMenuActive?" (native Pause menu session)"
                :hudMovieActive?" (native HUD movie full frame)"
                :collectibleQuad?" (native Doomguy collectible animation)"
                :comfortParticipantQuad?" (native Comfort cinematic participant)"
                :cutscene?" (cutscene caller)"
                :gameplayPresentationInterrupted?" (gameplay camera paused)":"")
            +(s.quadMode
                ?"; black Quad active until centered source is ready"
                :"; Vulkan clear unavailable, Projection retained during settle"));
    }else if(!desiredQuad&&s.quadMode){
        s.quadMode=false;
        log("Presentation automatic switch: PROJECTION");
    }
}
void advanceCenteredQuadTransition(){
    if(!s.centeredQuadTransitionPending)return;
    s.centeredQuadFramesRemaining=kharvox::advanceCenteredQuadTransition(
        s.centeredQuadFramesRemaining);
    if(s.centeredQuadFramesRemaining)return;
    s.centeredQuadTransitionPending=false;
    s.quadMode=true;
    log("[QUAD-CENTER] central camera pipeline settled; centered source released on QUAD");
}
void resetCinewindowAnchorState(){
    s.cinewindowCaptureReadiness.trackedSince=0;
    kharvox::resetCinewindowAnchor(s.cinewindowAnchor);
    s.cinewindowPresentationActive=false;
    s.cinewindowFixedPoseFallbackLogged=false;
}
void updateCinewindowAnchor(const XrViewState& viewState,XrTime displayTime){
    const bool cinewindowActive=s.quadMode||s.centeredQuadTransitionPending;
    if(!cinewindowActive){
        if(s.cinewindowPresentationActive&&!s.cinewindowFollowsHeadset
            &&s.cinewindowAnchor.valid)
            log("[CINEWINDOW] fixed tracking-space anchor released");
        resetCinewindowAnchorState();
        return;
    }
    if(!s.cinewindowPresentationActive){
        s.cinewindowPresentationActive=true;
        kharvox::resetCinewindowAnchor(s.cinewindowAnchor);
        s.cinewindowFixedPoseFallbackLogged=false;
    }
    if(s.cinewindowFollowsHeadset||s.cinewindowAnchor.valid)return;

    constexpr XrViewStateFlags requiredFlags=
        XR_VIEW_STATE_ORIENTATION_VALID_BIT|XR_VIEW_STATE_POSITION_VALID_BIT
        |XR_VIEW_STATE_ORIENTATION_TRACKED_BIT|XR_VIEW_STATE_POSITION_TRACKED_BIT;
    if(s.cinewindowCaptureReadiness.ready(displayTime,
            s.sessionState==XR_SESSION_STATE_FOCUSED,
            (viewState.viewStateFlags&requiredFlags)==requiredFlags)){
        const XrPosef& left=s.views[0].pose;
        const XrPosef& right=s.views[1].pose;
        const float centerX=(left.position.x+right.position.x)*0.5f;
        const float centerY=(left.position.y+right.position.y)*0.5f;
        const float centerZ=(left.position.z+right.position.z)*0.5f;
        if(kharvox::captureCinewindowAnchor(s.cinewindowAnchor,
                left.orientation.x,left.orientation.y,left.orientation.z,left.orientation.w,
                centerX,centerY,centerZ)){
            log("[CINEWINDOW] fixed anchor captured after focused tracking settled; eyeHeight="
                +std::to_string(centerY)+" displayTime="+std::to_string(displayTime));
            return;
        }
    }
    if(!s.cinewindowFixedPoseFallbackLogged){
        s.cinewindowFixedPoseFallbackLogged=true;
        log("[CINEWINDOW] fixed anchor waiting for focused, tracked head pose and settled LOCAL space; temporary headset-follow fallback active");
    }
}
template<class T> bool load(const char* n,T& out){bool ok=s.getProc&&XR_SUCCEEDED(s.getProc(s.instance,n,reinterpret_cast<PFN_xrVoidFunction*>(&out)))&&out;if(!ok)log(std::string("missing ")+n);else kharvox::native::trace::wrapXrFrameTrace(n,out);return ok;}
void pollEvents(){
    if(!s.sessionReadiness.usable()||!s.pollEvent)return;
    XrEventDataBuffer e{XR_TYPE_EVENT_DATA_BUFFER};
    while(s.pollEvent(s.instance,&e)==XR_SUCCESS){
        if(e.type==XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED){
            auto&c=*reinterpret_cast<XrEventDataSessionStateChanged*>(&e);
            s.sessionState=c.state;
            log("State -> "+std::to_string(c.state));
            if(c.state==XR_SESSION_STATE_FOCUSED){
                s.interactionProfilesDirty=true;
                focusDoomWindow();
            }
            if(c.state!=XR_SESSION_STATE_FOCUSED){
                // A headset put on later must not inherit the earlier menu height.
                resetCinewindowAnchorState();
                const auto reason=(c.state==XR_SESSION_STATE_STOPPING
                    ||c.state==XR_SESSION_STATE_EXITING
                    ||c.state==XR_SESSION_STATE_LOSS_PENDING)
                    ?kharvox::psvr2::TriggerOffReason::SessionEnd
                    :kharvox::psvr2::TriggerOffReason::Unfocused;
                KharvoxPsvr2SubmitTrigger(
                    kharvox::psvr2::offCommand(s.leftHanded,reason));
            }
            if(c.state==XR_SESSION_STATE_READY&&!s.running){
                XrSessionBeginInfo b{XR_TYPE_SESSION_BEGIN_INFO};
                b.primaryViewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                XrResult r{};if(s.simulatorRuntime&&kharvox::sfs::vrEnabled())steamXrFrameThread.invoke([&]{r=s.beginSession(s.session,&b);});else r=s.beginSession(s.session,&b);
                log("xrBeginSession "+result(r));
                s.running=XR_SUCCEEDED(r);
            }else if(c.state==XR_SESSION_STATE_STOPPING){
                updateImmersiveCinematicRefresh(false,0);
                clearXInputHapticState();
                if(s.running){
                    if(s.steamFramePrepared&&s.steamFrameBegun&&s.endFrame){
                        XrFrameEndInfo empty{XR_TYPE_FRAME_END_INFO};
                        empty.displayTime=s.steamPreparedFrame.predictedDisplayTime;
                        empty.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                        XrResult endResult{XR_ERROR_RUNTIME_FAILURE};
                        const DWORD frameThread=steamXrFrameThread.invoke([&]{endResult=endFrame(&empty);});
                        ++s.steamEndCalls;
                        if(XR_FAILED(endResult))++s.steamEndFailures;
                        log("[STEAM-XR-SPLIT] pending acquire frame ended empty before xrEndSession result="+result(endResult)+" lifecycleThread="+std::to_string(frameThread));
                    }
                    s.steamFramePrepared=false;
                    s.steamFrameBegun=false;
                    releaseMovement();
                    if(s.simulatorRuntime&&kharvox::sfs::vrEnabled())steamXrFrameThread.invoke([&]{s.endSession(s.session);});else s.endSession(s.session);
                    s.running=false;
                    resetCinewindowAnchorState();
                }
            }else if(c.state==XR_SESSION_STATE_EXITING||c.state==XR_SESSION_STATE_LOSS_PENDING){
                updateImmersiveCinematicRefresh(false,0);
                clearXInputHapticState();
                s.steamFramePrepared=false;
                s.steamFrameBegun=false;
                releaseMovement();
                s.running=false;
                resetCinewindowAnchorState();
                if(s.queue&&s.vk.queueWaitIdle){QueueAccessScope queueAccess;s.vk.queueWaitIdle(s.queue);}
                if(c.state==XR_SESSION_STATE_EXITING)s.sessionReadiness.exitRequested();
                else{s.sessionReadiness.systemRecoveryRequired();log("OpenXR system loss requires application restart");}
                destroySessionResources();
            }
        }else if(e.type==XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED){
            s.interactionProfilesDirty=true;
        }else if(e.type==XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING){
            clearXInputHapticState();
            KharvoxPsvr2SubmitTrigger(kharvox::psvr2::offCommand(
                s.leftHanded,kharvox::psvr2::TriggerOffReason::SessionEnd));
        }else if(e.type==XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING){
            auto&change=*reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(&e);
            if(change.referenceSpaceType==XR_REFERENCE_SPACE_TYPE_LOCAL){
                s.cinewindowCaptureReadiness.referenceChanged(change.changeTime);
                kharvox::resetCinewindowAnchor(s.cinewindowAnchor);
                s.cinewindowFixedPoseFallbackLogged=false;
                log("[CINEWINDOW] LOCAL reference-space change pending; recapture after changeTime="
                    +std::to_string(change.changeTime));
            }
        }
        e={XR_TYPE_EVENT_DATA_BUFFER};
    }
}
bool initializeSwapchainImages(EyeSwapchain& swapchain) {
    uint32_t count{};
    XrResult r = s.enumerateImages(swapchain.handle, 0, &count, nullptr);
    if (XR_FAILED(r) || !count) return false;
    swapchain.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    r = s.enumerateImages(
        swapchain.handle, count, &count,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchain.images.data()));
    if (XR_FAILED(r)) return false;
    swapchain.initialized.assign(count, false);
    return true;
}

#include "PauseBindings.inc"

bool createSwapchains() {
    uint32_t viewCount{};
    auto r = s.enumerateViews(
        s.instance, s.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        0, &viewCount, nullptr);
    if (XR_FAILED(r) || viewCount < 2) {
        log("stereo views failed " + result(r));
        return false;
    }
    std::vector<XrViewConfigurationView> config(
        viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    s.enumerateViews(
        s.instance, s.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        viewCount, &viewCount, config.data());
    const bool steamVrOwnsRenderScale =
        s.runtimeKind == kharvox::OpenXRRuntimeKind::SteamVR;
    s.renderScale = kharvox::effectiveRenderScale(
        configuredRenderScale(), steamVrOwnsRenderScale&&!kharvox::native::requested());
    const bool fsr1LauncherOption = GetFileAttributesW(
        kharvox::runtimePath(L"enable_fsr_upscaling").c_str()) != INVALID_FILE_ATTRIBUTES;
    const bool afwLauncherOption = false;
    const float fsrSourceScale = s.renderScale;
    s.fsr1Requested = kharvox::requestFsr1Upscaling(
        fsr1LauncherOption, !afwLauncherOption, fsrSourceScale);
    const float eyeTargetScale = kharvox::openXrEyeTargetScale(
        s.renderScale, s.fsr1Requested,
        s.runtimeKind == kharvox::OpenXRRuntimeKind::VirtualDesktop,
        kharvox::native::requested());
    log("Stereo views: " + std::to_string(viewCount)
        + " renderScale=" + std::to_string(s.renderScale)
        + " fsrSourceScale=" + std::to_string(fsrSourceScale)
        + " eyeTargetScale=" + std::to_string(eyeTargetScale)
        + (s.fsr1Requested ? " FSR1 native output target"
            : " scene and XR supersampling; no application ceiling"));
    for (uint32_t i = 0; i < 2; ++i) {
        log("Eye " + std::to_string(i)
            + " recommended=" + std::to_string(config[i].recommendedImageRectWidth)
            + "x" + std::to_string(config[i].recommendedImageRectHeight)
            + " max=" + std::to_string(config[i].maxImageRectWidth)
            + "x" + std::to_string(config[i].maxImageRectHeight)
            + " samples=" + std::to_string(config[i].recommendedSwapchainSampleCount));
    }

    uint32_t formatCount{};
    s.enumerateFormats(s.session, 0, &formatCount, nullptr);
    std::vector<int64_t> formats(formatCount);
    s.enumerateFormats(s.session, formatCount, &formatCount, formats.data());
    std::ostringstream formatLog;
    formatLog << "Formats:";
    for (auto format : formats) formatLog << ' ' << format;
    log(formatLog.str());
    auto sceneFormat = std::find(
        formats.begin(), formats.end(), int64_t(VK_FORMAT_B8G8R8A8_SRGB));
    if (sceneFormat == formats.end()) sceneFormat = std::find(
        formats.begin(), formats.end(), int64_t(VK_FORMAT_B8G8R8A8_UNORM));
    s.format = sceneFormat != formats.end()
        ? *sceneFormat : (formats.empty() ? 0 : formats[0]);
    log("Swapchain format=" + std::to_string(s.format)
        + (s.format == VK_FORMAT_B8G8R8A8_SRGB ? " SRGB" : " fallback"));

    for (uint32_t i = 0; i < 2; ++i) {
        auto& eye = s.eyes[i];
        if (!kharvox::scaledRenderDimension(config[i].recommendedImageRectWidth,
                eyeTargetScale, config[i].maxImageRectWidth, eye.width)
            || !kharvox::scaledRenderDimension(config[i].recommendedImageRectHeight,
                eyeTargetScale, config[i].maxImageRectHeight, eye.height)) {
            log("Requested RenderScale exceeds OpenXR eye " + std::to_string(i)
                + " maximum " + std::to_string(config[i].maxImageRectWidth) + "x"
                + std::to_string(config[i].maxImageRectHeight)
                + "; swapchain creation rejected (no clamping). Reduce RenderScale.");
            return false;
        }
        log("Eye " + std::to_string(i) + " swapchain="
            + std::to_string(eye.width) + "x" + std::to_string(eye.height));
        XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        createInfo.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT
            | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        if(kharvox::native::xrTargetCaptureEnabled()||eyeCaptureEnabled()){
            createInfo.usageFlags|=XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
            log("Native XR target diagnostic: transfer-source usage requested; timings are not a performance test");
        }
        createInfo.format = s.format;
        createInfo.sampleCount = 1;
        createInfo.width = eye.width;
        createInfo.height = eye.height;
        createInfo.faceCount = 1;
        createInfo.arraySize = 1;
        createInfo.mipCount = 1;
        r = s.createSwapchain(s.session, &createInfo, &eye.handle);
        if (XR_FAILED(r)) {
            log("xrCreateSwapchain eye=" + std::to_string(i) + " " + result(r));
            return false;
        }
        if (!initializeSwapchainImages(eye)) {
            log("xrEnumerateSwapchainImages eye=" + std::to_string(i) + " failed");
            return false;
        }
    }

    // The compositor HUD is a 16:9 game surface, but its pixel density comes
    // from the active OpenXR system. Using the recommended per-eye width keeps
    // text density proportional across Quest, Index, Pimax, and future HMDs
    // instead of baking HUD8's Quest-only 960x540 destination into the layer.
    auto& hud = s.hudQuad;
    const uint32_t recommendedHudWidth = std::max(
        config[0].recommendedImageRectWidth, config[1].recommendedImageRectWidth);
    const uint32_t maximumHudWidth = std::min(
        config[0].maxImageRectWidth, config[1].maxImageRectWidth);
    const uint32_t maximumHudHeight = std::min(
        config[0].maxImageRectHeight, config[1].maxImageRectHeight);
    hud.width = std::max(2u, std::min(recommendedHudWidth, maximumHudWidth) & ~1u);
    hud.height = std::max(2u, static_cast<uint32_t>(std::lround(
        static_cast<float>(hud.width) * 9.0f / 16.0f)) & ~1u);
    if (hud.height > maximumHudHeight) {
        hud.height = std::max(2u, maximumHudHeight & ~1u);
        hud.width = std::max(2u, std::min(maximumHudWidth,
            static_cast<uint32_t>(std::lround(
                static_cast<float>(hud.height) * 16.0f / 9.0f))) & ~1u);
    }
    s.hudSurfaceWidth=hud.width;
    s.hudSurfaceHeight=hud.height;
    s.hudSafeTanHalfHorizontal=0.0f;
    s.hudSafeTanHalfVertical=0.0f;
    log("[HUD9-ADAPT] OpenXR recommended HUD surface="
        + std::to_string(hud.width) + "x" + std::to_string(hud.height)
        + " (16:9, independent of scene RenderScale)");

    constexpr int64_t hudFormat = VK_FORMAT_R8G8B8A8_UNORM;
    if (!kharvox::hudgpu::enabled()) {
        log("[HUD16-AER] legacy HUD9 GPU capture and unused HUD swapchain disabled");
    } else if (std::find(formats.begin(), formats.end(), hudFormat) == formats.end()) {
        log("[HUD9-QUAD] runtime does not expose VK_FORMAT_R8G8B8A8_UNORM; quad preview disabled");
    } else {
        XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        createInfo.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT
            | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        createInfo.format = hudFormat;
        createInfo.sampleCount = 1;
        createInfo.width = hud.width;
        createInfo.height = hud.height;
        createInfo.faceCount = 1;
        createInfo.arraySize = 1;
        createInfo.mipCount = 1;
        r = s.createSwapchain(s.session, &createInfo, &hud.handle);
        if (XR_SUCCEEDED(r) && initializeSwapchainImages(hud)) {
            s.hudQuadFormat = hudFormat;
            log("[HUD9-QUAD] headset-resolution RGBA8 swapchain ready; waiting for isolated HUD source");
        } else {
            log("[HUD9-QUAD] HUD swapchain unavailable " + result(r)
                + "; native HUD fallback remains active");
            if (hud.handle != XR_NULL_HANDLE) s.destroySwapchain(hud.handle);
            hud = {};
        }
    }

    // A compositor-owned world-space ribbon avoids AER/AFW cached-eye trails:
    // OpenXR expands this tiny transparent texture into a physical 4 mm beam
    // independently for both eyes. Two crossed quads are submitted later to
    // approximate a narrow cylindrical sight without a custom game shader.

    log(initializePauseBindings(formats)
        ? "[PAUSE-BINDINGS] static controller image uploaded; pause root only"
        : "[PAUSE-BINDINGS] image unavailable; normal pause menu retained");
    log("Swapchains ready");
    return true;
}
// The standalone intro keeps submitting black until this process is ready
// to create its session. Wait for destruction, not just an exit request.
bool releaseStandaloneIntro(){
    wchar_t token[64]{};
    if(!GetEnvironmentVariableW(L"KHARVOX_VR_INTRO_HANDOFF",token,64))return true;
    const std::wstring id(token);
    if(id.size()!=32||id.find_first_not_of(L"0123456789abcdef")!=std::wstring::npos)return false;
    const auto prefix=L"Local\\KHARVOX-VR-INTRO-"+id;
    HANDLE release=OpenEventW(EVENT_MODIFY_STATE,FALSE,(prefix+L"-release").c_str());
    HANDLE released=OpenEventW(SYNCHRONIZE,FALSE,(prefix+L"-released").c_str());
    if(!release||!released){
        if(release)CloseHandle(release);if(released)CloseHandle(released);
        log("[VR-INTRO] handoff helper already closed; creating game session");
        return true;
    }
    SetEvent(release);
    const DWORD wait=WaitForSingleObject(released,10000);
    CloseHandle(release);CloseHandle(released);
    if(wait!=WAIT_OBJECT_0){log("[VR-INTRO] session release timed out; game session deferred");return false;}
    SetEnvironmentVariableW(L"KHARVOX_VR_INTRO_HANDOFF",nullptr);
    log("[VR-INTRO] standalone black session released; creating DOOM session");
    return true;
}
void destroySessionResources(){
    s.running=false;
    s.steamFramePrepared=false;
    s.steamFrameBegun=false;
    s.handRenderer.shutdown();
    if(s.copyFence&&s.vk.destroyFence)s.vk.destroyFence(s.device,s.copyFence,nullptr);
    s.copyFence=VK_NULL_HANDLE;
    if(s.commandPool&&s.vk.destroyCommandPool)s.vk.destroyCommandPool(s.device,s.commandPool,nullptr);
    s.commandPool=VK_NULL_HANDLE;
    s.commandBuffer=VK_NULL_HANDLE;
    auto destroySwapchain=[&](EyeSwapchain& swapchain){
        if(swapchain.handle&&s.destroySwapchain)s.destroySwapchain(swapchain.handle);
        swapchain={};
    };
    destroySwapchain(pauseBindings);
    pauseBindingsReady=false;
    destroySwapchain(s.hudQuad);
    for(auto&eye:s.eyes)destroySwapchain(eye);
    auto destroySpace=[&](XrSpace& space){
        if(space&&s.destroySpace)s.destroySpace(space);
        space=XR_NULL_HANDLE;
    };
    destroySpace(s.rightAimSpace);
    destroySpace(s.leftAimSpace);
    destroySpace(s.rightGripSpace);
    destroySpace(s.leftGripSpace);
    destroySpace(s.viewSpace);
    destroySpace(s.space);
    if(s.session&&s.destroySession)s.destroySession(s.session);
    s.session=XR_NULL_HANDLE;
    if(s.gameplayActionSet&&s.destroyActionSet)s.destroyActionSet(s.gameplayActionSet);
    s.gameplayActionSet=XR_NULL_HANDLE;
    s.actionsReady=false;
    s.hapticActionsReady=false;
    s.sessionState=XR_SESSION_STATE_UNKNOWN;
    for(auto&state:s.eyeImageStates)state.reset();
    s.hudImageState.reset();
    s.sessionReadiness.reset();
}
bool createSession(){
    if(s.session||!s.instance||!s.vkInstance||!s.physical||!s.device||!s.queue)return false;
    // The launcher removes its temporary marker after the startup health check.
    // Latch this diagnostic for the whole XR session before the first frame so
    // gameplay cannot silently fall back to PROJECTION later.
    s.hudEverythingQuadAvailable=kharvox::runtimeFileExists(L"enable_hud_everything_quad");
    s.hudEverythingQuad=s.hudEverythingQuadAvailable;
    s.hudEverythingQuadKeyDown=false;
    s.steamQuadOnly=kharvox::isSteamBackedOpenXRRuntime(s.runtimeKind)
        &&kharvox::runtimeFileExists(L"enable_steam_quad_only");
    XrGraphicsRequirementsVulkanKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    XrResult r=s.enable2?s.requirements2(s.instance,s.system,&req):s.requirements1(s.instance,s.system,&req);
    log("Graphics requirements "+result(r)+" min="+std::to_string(req.minApiVersionSupported)+" max="+std::to_string(req.maxApiVersionSupported));
    if(XR_FAILED(r))return false;
    VkPhysicalDevice required{};
    if(s.enable2){
        if(kharvox::isSteamBackedOpenXRRuntime(s.runtimeKind)&&s.xrPhysical){
            required=s.xrPhysical;
            r=XR_SUCCESS;
            log("[STEAM-XR] Reusing cached runtime physical="+std::to_string(reinterpret_cast<uintptr_t>(required))+"; post-device xrGetVulkanGraphicsDevice2KHR skipped");
        }else{
            XrVulkanGraphicsDeviceGetInfoKHR gi{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
            gi.systemId=s.system;
            gi.vulkanInstance=s.vkInstance;
            r=s.graphicsDevice2(s.instance,&gi,&required);
        }
    }else r=s.graphicsDevice1(s.instance,s.system,s.vkInstance,&required);
    log("Runtime physical="+std::to_string(reinterpret_cast<uintptr_t>(required))+" created physical="+std::to_string(reinterpret_cast<uintptr_t>(s.physical)));
    if(XR_FAILED(r)){log("graphics device query failed "+result(r));return false;}if(required!=s.physical){log("runtime physical changed after device creation; refusing mismatched OpenXR binding");return false;}XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};binding.instance=s.vkInstance;binding.physicalDevice=required;binding.device=s.device;binding.queueFamilyIndex=s.queueFamily;binding.queueIndex=s.queueIndex;XrSessionCreateInfo ci{XR_TYPE_SESSION_CREATE_INFO};ci.next=&binding;ci.systemId=s.system;if(!releaseStandaloneIntro())return false;bridgeSessionTrace=s.useEnable2Bridge;log("[XR] ENTER xrCreateSession runtime="+std::string(kharvox::openXRRuntimeKindName(s.runtimeKind)));r=s.createSession(s.instance,&ci,&s.session);log("[XR] EXIT xrCreateSession "+result(r));bridgeSessionTrace=false;log("xrCreateSession "+result(r));if(XR_FAILED(r)){s.session=XR_NULL_HANDLE;return false;}s.sessionReadiness.created();auto fail=[](){destroySessionResources();return false;};
    XrReferenceSpaceCreateInfo si{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};si.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_LOCAL;si.poseInReferenceSpace.orientation.w=1;r=s.createSpace(s.session,&si,&s.space);log("xrCreateReferenceSpace LOCAL "+result(r));if(XR_FAILED(r))return fail();si.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_VIEW;r=s.createSpace(s.session,&si,&s.viewSpace);log("xrCreateReferenceSpace VIEW "+result(r));if(XR_FAILED(r))return fail();s.worldScale=configuredWorldScale();log("World scale="+std::to_string(s.worldScale)+" DOOM units/meter");log("Immersive cinematic FOV uses exact frame-matched OpenXR projection; cinematic zoom disabled");log("Presentation mode: QUAD");if(s.hudEverythingQuad)log("[HUD10-EVERYTHING-QUAD] LATCHED FOR SESSION: complete final DOOM image including world, weapon, and every UI element is shown on one Cinewindow Quad; F9 toggles normal 6DoF Projection");if(s.steamQuadOnly)log("[STEAM-QUAD-ONLY] LATCHED FOR SESSION: gameplay copies the normal DOOM frame into one Cinewindow QUAD layer; no PROJECTION layer will be submitted");
    if(!createGameplayActions()||!createSwapchains())return fail();VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;pi.queueFamilyIndex=s.queueFamily;if(s.vk.createCommandPool(s.device,&pi,nullptr,&s.commandPool)!=VK_SUCCESS)return fail();VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ai.commandPool=s.commandPool;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=1;if(s.vk.allocateCommandBuffers(s.device,&ai,&s.commandBuffer)!=VK_SUCCESS)return fail();
    {
        std::array<VkExtent2D,2> extents{{{s.eyes[0].width,s.eyes[0].height},{s.eyes[1].width,s.eyes[1].height}}};
        std::array<std::vector<VkImage>,2> images{};
        for(size_t eye=0;eye<2;eye++)for(const auto&image:s.eyes[eye].images)images[eye].push_back(image.image);
        if(s.showHands||s.laserSightEnabled)
            s.handRenderer.initialize(s.physical,s.device,s.queue,s.queueFamily,s.vk,
                static_cast<VkFormat>(s.format),extents,images,kharvox::runtimeDirectory(),
                [](const std::string&message){log(message);});
        else log("[HANDS] Show Hands disabled; no assets or Vulkan resources loaded");
        const auto handAssets=s.handRenderer.availability();
        if(handAssets.leftFist||handAssets.rightFist)
            log("[HANDS] Berserk visibility input is safely inactive: no reliable native Berserk-state probe exists yet");
    }
    const bool createCopyFence=kharvox::isSteamBackedOpenXRRuntime(s.runtimeKind)
        ||(s.runtimeKind==kharvox::OpenXRRuntimeKind::VirtualDesktop&&kharvox::rendererDefaults::earlyXrRelease);
    if(createCopyFence&&s.vk.createFence&&s.vk.resetFences&&s.vk.waitForFences){
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence createdFence{};
        const auto created=s.vk.createFence(s.device,&fi,nullptr,&createdFence);
        if(created==VK_SUCCESS)s.copyFence=createdFence;
        log("[XR-COPY-FENCE] runtime="+std::string(kharvox::openXRRuntimeKindName(s.runtimeKind))
            +(created==VK_SUCCESS?" private copy fence available; use selected per frame":" copy fence unavailable; queueWaitIdle fallback retained"));
    }
    s.sessionReadiness.completed();log("Session created");return true;}
void barrierAspect(VkCommandBuffer cb,VkImage img,VkImageLayout oldL,VkImageLayout newL,VkAccessFlags src,VkAccessFlags dst,VkImageAspectFlags aspect,uint32_t baseLayer=0){VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.srcAccessMask=src;b.dstAccessMask=dst;b.oldLayout=kharvox::sfs::sourceLayout(s.device,img,oldL);b.newLayout=kharvox::sfs::sourceLayout(s.device,img,newL);b.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=img;b.subresourceRange.aspectMask=aspect;b.subresourceRange.baseArrayLayer=baseLayer;b.subresourceRange.levelCount=1;b.subresourceRange.layerCount=1;s.vk.cmdPipelineBarrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);}
void barrier(VkCommandBuffer cb,VkImage img,VkImageLayout oldL,VkImageLayout newL,VkAccessFlags src,VkAccessFlags dst){barrierAspect(cb,img,oldL,newL,src,dst,VK_IMAGE_ASPECT_COLOR_BIT);}
void invalidateAlternatingStereoHistory(bool clearProgrammedViews){
    s.aerSourceCacheValid={};s.aerSourceCacheKeys={};s.aerSourceModeActive=false;s.aerPublishedSourcePoseId=0;
    s.aerInputHistory={};s.aerPairInputPoseId=0;s.integratedPairSourcePoseId=0;
    s.stereoCacheInitialized={};
    s.stereoCacheHasIntegratedHands={};
    s.integratedPairHandsValid=false;
    s.freshHandsWorldValid=false;
    s.cachedEyeViewValid={};
    s.previousCachedEyeViewValid={};
    s.steamXrAerCaptureValid=false;
    s.steamXrAerCaptureEyesReady={};
    s.steamXrAerPairReady=false;
    s.steamXrAerPairActive=false;
    s.steamXrAerDisplayPeriod=0;
    s.steamLinkFixedPairValid=false;
    s.steamLinkFixedPairImages={};
    if(clearProgrammedViews)s.programmedEyeViewValid={};
}
#include "EyeCapture.inc"
#include "FreshAerHandsCache.inc"

std::array<uint64_t,2> poseTracePublishedRevisions{};
std::array<uint64_t,2> poseTraceProgrammedIds{},poseTraceCachedIds{},poseTracePublishedIds{};
unsigned poseTraceFlags(){return (kharvox::native::requested()?1u:0u)|(s.quadMode?2u:0u)
    |(s.steamXrAerPairActive?4u:0u)|(s.alternatingStereoWarmupActive?8u:0u)
    |(s.sessionState==XR_SESSION_STATE_FOCUSED?16u:0u);}
void traceEye(unsigned kind,int eye,const XrPosef& pose,const XrFovf& fov,
    XrTime time,uint64_t source=0,uint64_t revision=0,unsigned extraFlags=0,uint64_t poseId=0){
    if(!kharvox::pose_trace::active.load(std::memory_order_relaxed))return;
    kharvox::pose_trace::Event e{};e.kind=kind;e.frame=s.frame;e.eye=eye;
    e.poseId=poseId;
    e.flags=poseTraceFlags()|extraFlags;e.displayTime=time;e.source=source;e.revision=revision;
    e.data[0]=pose.position.x;e.data[1]=pose.position.y;e.data[2]=pose.position.z;
    e.data[3]=pose.orientation.x;e.data[4]=pose.orientation.y;e.data[5]=pose.orientation.z;e.data[6]=pose.orientation.w;
    e.data[7]=fov.angleLeft;e.data[8]=fov.angleRight;e.data[9]=fov.angleUp;e.data[10]=fov.angleDown;
    e.data[11]=s.artificialTurnTotalDegrees;e.data[12]=s.worldScale;kharvox::pose_trace::record(e);
}

bool ensureStereoCache(VkExtent2D extent){if(s.stereoCache[0]&&s.stereoCacheExtent.width==extent.width&&s.stereoCacheExtent.height==extent.height)return true;if(!s.vk.createImage||!s.vk.getImageMemoryRequirements||!s.vk.allocateMemory||!s.vk.bindImageMemory||!s.vk.getPhysicalDeviceMemoryProperties)return false;if(!s.vk.queueWaitIdle||!s.queue||s.vk.queueWaitIdle(s.queue)!=VK_SUCCESS)return false;s.fsr1.releaseAfterCompletion();s.fsr1InitializationAttempted=false;invalidateAlternatingStereoHistory(true);for(int e=0;e<2;e++){if(s.stereoCache[e])s.vk.destroyImage(s.device,s.stereoCache[e],nullptr);if(s.stereoCacheMemory[e])s.vk.freeMemory(s.device,s.stereoCacheMemory[e],nullptr);s.stereoCache[e]=VK_NULL_HANDLE;s.stereoCacheMemory[e]=VK_NULL_HANDLE;}VkPhysicalDeviceMemoryProperties properties{};s.vk.getPhysicalDeviceMemoryProperties(s.physical,&properties);for(int e=0;e<2;e++){VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=static_cast<VkFormat>(s.format);ci.extent={extent.width,extent.height,1};ci.mipLevels=1;ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;ci.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;if(s.vk.createImage(s.device,&ci,nullptr,&s.stereoCache[e])!=VK_SUCCESS)return false;VkMemoryRequirements requirements{};s.vk.getImageMemoryRequirements(s.device,s.stereoCache[e],&requirements);uint32_t memoryType=UINT32_MAX;for(uint32_t i=0;i<properties.memoryTypeCount;i++)if((requirements.memoryTypeBits&(1u<<i))&&(properties.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){memoryType=i;break;}if(memoryType==UINT32_MAX)return false;VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=requirements.size;ai.memoryTypeIndex=memoryType;if(s.vk.allocateMemory(s.device,&ai,nullptr,&s.stereoCacheMemory[e])!=VK_SUCCESS||s.vk.bindImageMemory(s.device,s.stereoCache[e],s.stereoCacheMemory[e],0)!=VK_SUCCESS)return false;}s.stereoCacheExtent=extent;log("Stereo cache ready "+std::to_string(extent.width)+"x"+std::to_string(extent.height)+" format="+std::to_string(s.format));return true;}
}

static void initializeOpenXR(VkInstance instance){std::lock_guard<std::mutex>l(mutex);if(s.instance){if(instance)s.vkInstance=instance;return;}char weapon[8]{};const bool weaponEnvironment=GetEnvironmentVariableA("KHARVOX_WEAPON_6DOF",weapon,sizeof(weapon))>0&&strcmp(weapon,"0");const bool weaponMarker=GetFileAttributesW(kharvox::runtimePath(L"enable_6dof_weapon").c_str())!=INVALID_FILE_ATTRIBUTES;s.weapon6Dof=weaponEnvironment||weaponMarker;const bool twoHandMarker=GetFileAttributesW(kharvox::runtimePath(L"enable_two_hand_weapon").c_str())!=INVALID_FILE_ATTRIBUTES;const bool calibrationMarker=GetFileAttributesW(kharvox::runtimePath(L"enable_two_hand_calibration").c_str())!=INVALID_FILE_ATTRIBUTES;const bool virtualGunstockMarker=GetFileAttributesW(kharvox::runtimePath(L"enable_virtual_gunstock").c_str())!=INVALID_FILE_ATTRIBUTES;s.twoHandEnabled=s.weapon6Dof&&twoHandMarker;s.twoHandCalibrationMode=s.twoHandEnabled&&calibrationMarker;s.virtualGunstockEnabled=s.twoHandEnabled&&virtualGunstockMarker;s.twoHandCalibrationTarget=loadTwoHandCalibrationTarget();s.twoHandCalibrationAimMode=loadTwoHandCalibrationAimMode();loadTwoHandCalibrations();if(s.twoHandCalibrationMode){if(auto* calibration=twoHandCalibrationFor(s.twoHandCalibrationTarget))calibration->aimMode=s.twoHandCalibrationAimMode;}log(std::string("[WEAPON] native 6DOF ")+(s.weapon6Dof?"ENABLED":"disabled"));log(std::string("[TWO-HAND] ")+(s.twoHandEnabled?"ENABLED":"disabled")+" mode="+(s.twoHandCalibrationMode?"calibration":"gameplay-test")+" target="+KharvoxWeaponKindKey(s.twoHandCalibrationTarget)+" alignment="+twoHandModeName(s.twoHandCalibrationAimMode)+" virtualGunstock="+(s.virtualGunstockEnabled?"enabled":"disabled")+" savedProfiles="+std::to_string(savedTwoHandCalibrationCount()));if(s.twoHandEnabled)writeTwoHandStatus(s.twoHandCalibrationMode?std::string("CALIBRATION: enter gameplay and equip ")+weaponKindName(s.twoHandCalibrationTarget):"GAMEPLAY TEST: waiting for gameplay weapon data");installXInputHook();s.vkInstance=instance;const auto bundledLoader=kharvox::runtimePath(L"openxr_loader.dll");s.loader=LoadLibraryW(bundledLoader.c_str());if(!s.loader)s.loader=LoadLibraryW(L"openxr_loader.dll");if(!s.loader){log("OpenXR loader not found beside KharvoxLayer.dll or on the system DLL path");return;}s.getProc=reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(s.loader,"xrGetInstanceProcAddr"));s.enumerateExtensions=reinterpret_cast<PFN_xrEnumerateInstanceExtensionProperties>(GetProcAddress(s.loader,"xrEnumerateInstanceExtensionProperties"));s.createInstance=reinterpret_cast<PFN_xrCreateInstance>(GetProcAddress(s.loader,"xrCreateInstance"));if(!s.getProc||!s.enumerateExtensions||!s.createInstance){log("loader exports missing");return;}uint32_t n=0;s.enumerateExtensions(nullptr,0,&n,nullptr);std::vector<XrExtensionProperties> ex(n,{XR_TYPE_EXTENSION_PROPERTIES});s.enumerateExtensions(nullptr,n,&n,ex.data());bool e2=false,e1=false;for(auto&e:ex){e2|=!strcmp(e.extensionName,XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);e1|=!strcmp(e.extensionName,XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);}const bool med=GetFileAttributesW(kharvox::runtimePath(L"enable_xr_mediated").c_str())!=INVALID_FILE_ATTRIBUTES;s.runtimeManifest=kharvox::activeOpenXRRuntimeManifest();s.runtimeKind=kharvox::classifyOpenXRRuntime(s.runtimeManifest);s.vulkanPath=kharvox::selectOpenXRVulkanPath(s.runtimeKind,med,e1,e2);s.useEnable2Bridge=s.vulkanPath==kharvox::OpenXRVulkanPath::VulkanEnable2VirtualDesktopBridge;s.useEnable2RuntimeManaged=s.vulkanPath==kharvox::OpenXRVulkanPath::VulkanEnable2RuntimeManaged;s.enable2=s.useEnable2Bridge||s.useEnable2RuntimeManaged;const char* ext=s.vulkanPath==kharvox::OpenXRVulkanPath::VulkanEnable1Direct?XR_KHR_VULKAN_ENABLE_EXTENSION_NAME:s.enable2?XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME:nullptr;log("OpenXR runtime policy: runtime="+std::string(kharvox::openXRRuntimeKindName(s.runtimeKind))+" manifest="+(s.runtimeManifest.empty()?std::string("<unavailable>"):s.runtimeManifest)+" selected="+kharvox::openXRVulkanPathName(s.vulkanPath)+" supportsEnable1="+(e1?"yes":"no")+" supportsEnable2="+(e2?"yes":"no"));if(!ext){log("No safe Vulkan path exposed by the active OpenXR runtime");return;}XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};strcpy_s(ci.applicationInfo.applicationName,"KHARVOX");strcpy_s(ci.applicationInfo.engineName,"KHARVOX");ci.applicationInfo.apiVersion=XR_MAKE_VERSION(1,0,0);ci.enabledExtensionCount=1;ci.enabledExtensionNames=&ext;auto r=s.createInstance(&ci,&s.instance);log("xrCreateInstance "+std::to_string(r)+" extension="+ext);if(XR_FAILED(r))return;PFN_xrGetInstanceProperties getInstanceProperties{};if(XR_SUCCEEDED(s.getProc(s.instance,"xrGetInstanceProperties",reinterpret_cast<PFN_xrVoidFunction*>(&getInstanceProperties)))&&getInstanceProperties){XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};if(XR_SUCCEEDED(getInstanceProperties(s.instance,&properties))){const std::string runtimeName(properties.runtimeName);s.simulatorRuntime=runtimeName=="OpenXR Simulator Runtime";s.steamMetaCompatibilityMode=kharvox::isSteamBackedOpenXRRuntime(s.runtimeKind)&&runtimeName.find("Meta compatibility mode")!=std::string::npos;log("OpenXR runtime reported name="+runtimeName+" version="+std::to_string(XR_VERSION_MAJOR(properties.runtimeVersion))+"."+std::to_string(XR_VERSION_MINOR(properties.runtimeVersion))+"."+std::to_string(XR_VERSION_PATCH(properties.runtimeVersion)));if(s.steamMetaCompatibilityMode)log("[STEAMLINK-AER] Meta compatibility runtime detected; stereoscopic alternating-eye path armed");}}
    bool core=true;
    core=load("xrDestroyInstance",s.destroyInstance)&&core;core=load("xrGetSystem",s.getSystem)&&core;core=load("xrCreateSession",s.createSession)&&core;core=load("xrDestroySession",s.destroySession)&&core;core=load("xrPollEvent",s.pollEvent)&&core;core=load("xrBeginSession",s.beginSession)&&core;core=load("xrEndSession",s.endSession)&&core;core=load("xrDestroySpace",s.destroySpace)&&core;core=load("xrCreateSwapchain",s.createSwapchain)&&core;core=load("xrDestroySwapchain",s.destroySwapchain)&&core;core=load("xrWaitFrame",s.waitFrame)&&core;core=load("xrBeginFrame",s.beginFrame)&&core;core=load("xrLocateViews",s.locateViews)&&core;core=load("xrEndFrame",s.endFrame)&&core;
    core=load("xrCreateReferenceSpace",s.createSpace)&&core;core=load("xrEnumerateViewConfigurationViews",s.enumerateViews)&&core;core=load("xrEnumerateSwapchainFormats",s.enumerateFormats)&&core;core=load("xrEnumerateSwapchainImages",s.enumerateImages)&&core;core=load("xrAcquireSwapchainImage",s.acquireImage)&&core;core=load("xrWaitSwapchainImage",s.waitImage)&&core;core=load("xrReleaseSwapchainImage",s.releaseImage)&&core;
    core=load("xrCreateActionSet",s.createActionSet)&&core;core=load("xrDestroyActionSet",s.destroyActionSet)&&core;core=load("xrCreateAction",s.createAction)&&core;core=load("xrDestroyAction",s.destroyAction)&&core;core=load("xrSuggestInteractionProfileBindings",s.suggestBindings)&&core;core=load("xrAttachSessionActionSets",s.attachActionSets)&&core;core=load("xrSyncActions",s.syncActionsFn)&&core;core=load("xrCreateActionSpace",s.createActionSpace)&&core;core=load("xrLocateSpace",s.locateSpace)&&core;core=load("xrGetActionStatePose",s.getActionStatePose)&&core;core=load("xrGetActionStateVector2f",s.getActionStateVector2f)&&core;core=load("xrGetActionStateFloat",s.getActionStateFloat)&&core;core=load("xrGetActionStateBoolean",s.getActionStateBoolean)&&core;
    const bool interactionProfileFunction=
        XR_SUCCEEDED(s.getProc(s.instance,"xrGetCurrentInteractionProfile",reinterpret_cast<PFN_xrVoidFunction*>(&s.getCurrentInteractionProfile)))
        &&s.getCurrentInteractionProfile;
    if(!interactionProfileFunction){
        s.getCurrentInteractionProfile=nullptr;
        log("[INPUT] xrGetCurrentInteractionProfile unavailable; retaining default grip threshold");
    }
    const bool hapticFunctions=
        XR_SUCCEEDED(s.getProc(s.instance,"xrApplyHapticFeedback",reinterpret_cast<PFN_xrVoidFunction*>(&s.applyHapticFeedback)))&&s.applyHapticFeedback
        &&XR_SUCCEEDED(s.getProc(s.instance,"xrStopHapticFeedback",reinterpret_cast<PFN_xrVoidFunction*>(&s.stopHapticFeedback)))&&s.stopHapticFeedback;
    if(!hapticFunctions){s.applyHapticFeedback=nullptr;s.stopHapticFeedback=nullptr;}
    log(std::string("[HAPTICS] OpenXR core function table ")+(hapticFunctions?"ready":"unavailable; continuing without VR rumble"));
    log(std::string("core function table ")+(core?"ready":"INCOMPLETE"));if(!core)return;
    bool graphics=false;if(s.enable2)graphics=load("xrGetVulkanGraphicsRequirements2KHR",s.requirements2)&&load("xrGetVulkanGraphicsDevice2KHR",s.graphicsDevice2)&&load("xrCreateVulkanInstanceKHR",s.createVulkanInstance)&&load("xrCreateVulkanDeviceKHR",s.createVulkanDevice);else graphics=load("xrGetVulkanGraphicsRequirementsKHR",s.requirements1)&&load("xrGetVulkanGraphicsDeviceKHR",s.graphicsDevice1)&&load("xrGetVulkanInstanceExtensionsKHR",s.instanceExtensions)&&load("xrGetVulkanDeviceExtensionsKHR",s.deviceExtensions);log(std::string("Vulkan XR functions ")+(graphics?"ready":"INCOMPLETE"));if(s.enable2){log("XR_KHR_vulkan_enable2 active");log(std::string("xrCreateVulkanInstanceKHR ")+(s.createVulkanInstance?"available":"MISSING"));log(std::string("xrCreateVulkanDeviceKHR ")+(s.createVulkanDevice?"available":"MISSING"));log(std::string("xrGetVulkanGraphicsDevice2KHR ")+(s.graphicsDevice2?"available":"MISSING"));}if(!graphics)return;XrSystemGetInfo gi{XR_TYPE_SYSTEM_GET_INFO};gi.formFactor=XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;log("calling xrGetSystem");r=s.getSystem(s.instance,&gi,&s.system);log("xrGetSystem result="+std::to_string(r)+" system="+std::to_string(s.system));if(XR_SUCCEEDED(r)){XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};const XrResult requirementsResult=s.enable2?s.requirements2(s.instance,s.system,&requirements):s.requirements1(s.instance,s.system,&requirements);if(XR_SUCCEEDED(requirementsResult)){s.runtimeMaxVulkanApiVersion=VK_MAKE_API_VERSION(0,XR_VERSION_MAJOR(requirements.maxApiVersionSupported),XR_VERSION_MINOR(requirements.maxApiVersionSupported),XR_VERSION_PATCH(requirements.maxApiVersionSupported));log("Runtime Vulkan API range min="+std::to_string(XR_VERSION_MAJOR(requirements.minApiVersionSupported))+"."+std::to_string(XR_VERSION_MINOR(requirements.minApiVersionSupported))+" max="+std::to_string(XR_VERSION_MAJOR(requirements.maxApiVersionSupported))+"."+std::to_string(XR_VERSION_MINOR(requirements.maxApiVersionSupported)));}else log("Runtime Vulkan requirements query failed "+result(requirementsResult));}if(!s.enable2&&XR_SUCCEEDED(r)){uint32_t size=0;s.instanceExtensions(s.instance,s.system,0,&size,nullptr);std::vector<char>b(size);s.instanceExtensions(s.instance,s.system,size,&size,b.data());log(std::string("Required instance extensions: ")+b.data());size=0;s.deviceExtensions(s.instance,s.system,0,&size,nullptr);b.assign(size,0);s.deviceExtensions(s.instance,s.system,size,&size,b.data());log(std::string("Required device extensions: ")+b.data());}}

void KharvoxXRShutdownHaptics(){clearXInputHapticState();KharvoxPsvr2SubmitTrigger(kharvox::psvr2::offCommand(s.leftHanded,kharvox::psvr2::TriggerOffReason::SessionEnd));KharvoxPsvr2IpcRequestStop();KharvoxBhapticsIpcRequestStop();}
void KharvoxXRDeviceDestroyed(){
    std::lock_guard<std::mutex>l(mutex);
    s.running=false;
    s.sessionReadiness.reset();
    VkResult idleResult=VK_SUCCESS;
    if(s.queue&&s.vk.queueWaitIdle){QueueAccessScope queueAccess;idleResult=s.vk.queueWaitIdle(s.queue);}
    if(idleResult!=VK_SUCCESS)log("XR device teardown queue idle failed "+std::to_string(idleResult));
    releaseEyeCapture();
    for(auto&source:eyeSourceCapture){source.completed=true;releaseEyeSource(source);}
    s.fsr1.releaseAfterCompletion();
    s.fsr1InitializationAttempted=false;
    if(s.sfsCopyTiming.pool)s.sfsCopyTiming.shutdownAfterCompletion();
    for(int e=0;e<2;++e){
        if(s.freshHandsWorld[e]&&s.vk.destroyImage)s.vk.destroyImage(s.device,s.freshHandsWorld[e],nullptr);
        if(s.freshHandsMemory[e]&&s.vk.freeMemory)s.vk.freeMemory(s.device,s.freshHandsMemory[e],nullptr);
        if(s.stereoCache[e]&&s.vk.destroyImage)s.vk.destroyImage(s.device,s.stereoCache[e],nullptr);
        if(s.stereoCacheMemory[e]&&s.vk.freeMemory)s.vk.freeMemory(s.device,s.stereoCacheMemory[e],nullptr);
    }
    s.freshHandsWorld={};s.freshHandsMemory={};s.freshHandsExtent={};s.freshHandsInitialized=false;s.freshHandsWorldValid=false;
    s.stereoCache={};s.stereoCacheMemory={};s.stereoCacheExtent={};s.stereoCacheInitialized={};s.stereoCacheRevision={};
    destroySessionResources();
    s.doomSwapchains.clear();
    s.retiredCompatibleDoomSwapchain={};
    s.retiredCompatibleDoomSwapchainValid=false;
    s.startupActiveDoomSwapchain=VK_NULL_HANDLE;
    s.queue=VK_NULL_HANDLE;
    s.device=VK_NULL_HANDLE;
    s.physical=VK_NULL_HANDLE;
    s.vk={};
}

bool KharvoxXRMediationEnabled(){return GetFileAttributesW(kharvox::runtimePath(L"enable_xr_mediated").c_str())!=INVALID_FILE_ATTRIBUTES;}
bool KharvoxXRMediationReentry(){return mediationReentry;}
VkPhysicalDevice KharvoxXRMappedPhysicalForReentry(){return reentryPhysical;}
PFN_vkCreateDevice KharvoxXRCreateDeviceForReentry(){return reentryCreateDevice;}
bool KharvoxXRCreateVulkanInstance(PFN_vkGetInstanceProcAddr g,const VkInstanceCreateInfo* ci,
    const VkAllocationCallbacks* a,VkInstance* out,VkResult* vr) {
    std::lock_guard<std::mutex> l(mutex);
    if(!s.enable2||!s.createVulkanInstance)return false;
    if(!out||!vr)return true;
    // The Vulkan layer chain can carry a loader-owned object in *out.
    // Only clear failed outputs AFTER runtime creation returns.
    *vr=VK_ERROR_INITIALIZATION_FAILED;
    if(!ci||!g||!s.requirements2)return true;
    XrGraphicsRequirementsVulkanKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    auto rr=s.requirements2(s.instance,s.system,&req);
    if(XR_FAILED(rr)){log("requirements2 "+result(rr));return true;}
    s.physicalIdentityQueryEnabled=ci->pApplicationInfo&&ci->pApplicationInfo->apiVersion>=VK_API_VERSION_1_1;
    for(uint32_t index=0;index<ci->enabledExtensionCount;++index)
        if(!strcmp(ci->ppEnabledExtensionNames[index],VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME))s.physicalIdentityQueryEnabled=true;
    XrVulkanInstanceCreateInfoKHR xi{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xi.systemId=s.system;xi.vulkanCreateInfo=ci;xi.vulkanAllocator=a;
    if(s.useEnable2Bridge){bridgeNextGipa=g;xi.pfnGetInstanceProcAddr=bridgeGipa;}
    else xi.pfnGetInstanceProcAddr=g;
    log(std::string("[XR-STARTUP] creating Vulkan instance via ")+(s.useEnable2Bridge?"VD bridge":"runtime-managed")+
        " nextGipaOwner="+pointerOwner(reinterpret_cast<PFN_vkVoidFunction>(g)),true);
    rr=s.createVulkanInstance(s.instance,&xi,out,vr);
    kharvox::finishRuntimeVulkanCreate(XR_SUCCEEDED(rr),vr,out);
    log("xrCreateVulkanInstanceKHR xr="+result(rr)+" vk="+std::to_string(*vr),true);
    if(*vr==VK_SUCCESS)s.vkInstance=*out;
    return true;
}
bool KharvoxXRCreateVulkanDevice(PFN_vkGetInstanceProcAddr g,VkPhysicalDevice doomPhysical,
    const VkDeviceCreateInfo* runtimeCi,const VkDeviceCreateInfo* downstreamCi,
    const VkAllocationCallbacks* a,VkDevice* out,VkResult* vr) {
    std::lock_guard<std::mutex> l(mutex);
    if(!s.enable2||!s.createVulkanDevice)return false;
    if(!out||!vr)return true;
    // The Vulkan layer chain can carry a loader-owned object in *out.
    // Only clear failed outputs AFTER runtime creation returns.
    *vr=VK_ERROR_INITIALIZATION_FAILED;
    if(!g||!s.vkInstance||!doomPhysical||!s.graphicsDevice2)return true;
    if(!s.xrPhysical){
        XrVulkanGraphicsDeviceGetInfoKHR gi{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
        gi.systemId=s.system;gi.vulkanInstance=s.vkInstance;
        const auto gr=s.graphicsDevice2(s.instance,&gi,&s.xrPhysical);
        log("xrGetVulkanGraphicsDevice2KHR "+result(gr)+" physical="+std::to_string(reinterpret_cast<uintptr_t>(s.xrPhysical)));
        if(XR_FAILED(gr)||!s.xrPhysical){
            s.xrPhysical=VK_NULL_HANDLE;
            log("[XR-STARTUP] physical-device selection failed; no direct-device fallback",true);
            return true;
        }
    }
    // Runtime physical handles come from the public loader except for the
    // simulator's downstream enumeration. Never compare the raw handle values.
    const auto loader=GetModuleHandleW(L"vulkan-1.dll");
    const auto publicGipa=reinterpret_cast<PFN_vkGetInstanceProcAddr>(loader?GetProcAddress(loader,"vkGetInstanceProcAddr"):nullptr);
    const auto xrGipa=s.simulatorRuntime?g:publicGipa;
    const auto doomProps=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(g(s.vkInstance,"vkGetPhysicalDeviceProperties"));
    const auto xrProps=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(xrGipa?xrGipa(s.vkInstance,"vkGetPhysicalDeviceProperties"):nullptr);
    const auto create=kharvox::resolveLayerCreateDevice(g,s.vkInstance);
    if(!doomProps||!xrProps||!create){log("[XR-STARTUP] missing instance-scoped device dispatch; refusing unbound device",true);return true;}
    VkPhysicalDeviceProperties dp{},xp{};doomProps(doomPhysical,&dp);xrProps(s.xrPhysical,&xp);
    log(std::string("[XR-STARTUP] DOOM GPU=")+dp.deviceName+" vendor="+std::to_string(dp.vendorID)+" device="+std::to_string(dp.deviceID)+
        " XR GPU="+xp.deviceName+" vendor="+std::to_string(xp.vendorID)+" device="+std::to_string(xp.deviceID)+
        " instance="+std::to_string(reinterpret_cast<uintptr_t>(s.vkInstance))+
        " layeredPhysical="+std::to_string(reinterpret_cast<uintptr_t>(doomPhysical))+
        " runtimePhysical="+std::to_string(reinterpret_cast<uintptr_t>(s.xrPhysical))+
        " createDeviceOwner="+pointerOwner(reinterpret_cast<PFN_vkVoidFunction>(create)),true);
    if(dp.vendorID!=xp.vendorID||dp.deviceID!=xp.deviceID){log("[XR-STARTUP] GPU identity mismatch; refusing direct-device fallback",true);return true;}
    auto props2=[&](PFN_vkGetInstanceProcAddr proc){
        auto fn=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(proc(s.vkInstance,"vkGetPhysicalDeviceProperties2"));
        return fn?fn:reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(proc(s.vkInstance,"vkGetPhysicalDeviceProperties2KHR"));
    };
    const auto doomProps2=props2(g),xrProps2=props2(xrGipa);
    if(s.physicalIdentityQueryEnabled&&doomProps2&&xrProps2){
        VkPhysicalDeviceIDProperties did{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES},xid{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 dprops{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2},xprops{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        dprops.pNext=&did;xprops.pNext=&xid;doomProps2(doomPhysical,&dprops);xrProps2(s.xrPhysical,&xprops);
        const bool hasD=std::any_of(std::begin(did.deviceUUID),std::end(did.deviceUUID),[](uint8_t v){return v!=0;});
        const bool hasX=std::any_of(std::begin(xid.deviceUUID),std::end(xid.deviceUUID),[](uint8_t v){return v!=0;});
        if(hasD&&hasX&&std::memcmp(did.deviceUUID,xid.deviceUUID,VK_UUID_SIZE)){
            log("[XR-STARTUP] GPU UUID mismatch despite equal vendor/device IDs; refusing device",true);return true;
        }
        log(std::string("[XR-STARTUP] GPU UUID comparison=")+(hasD&&hasX?"matched":"unavailable"),true);
    }else log("[XR-STARTUP] GPU UUID query unavailable; vendor/device comparison only",true);
    XrVulkanDeviceCreateInfoKHR di{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    di.systemId=s.system;di.vulkanPhysicalDevice=s.xrPhysical;di.vulkanAllocator=a;
    XrResult rr{};
    if(s.useEnable2RuntimeManaged){
        runtimeManagedSessionLoaderRoute.store(false,std::memory_order_release);
        runtimeManagedNextGipa.store(g,std::memory_order_release);
        runtimeManagedReentryPhysical.store(doomPhysical,std::memory_order_release);
        runtimeManagedReentryCreateDevice.store(create,std::memory_order_release);
        runtimeManagedDownstreamCreateInfo.store(downstreamCi,std::memory_order_release);
        di.pfnGetInstanceProcAddr=runtimeManagedGipa;di.vulkanCreateInfo=runtimeCi;
        log("xrCreateVulkanDeviceKHR cross-thread layer adapter armed",true);
        rr=s.createVulkanDevice(s.instance,&di,out,vr);
        kharvox::finishRuntimeVulkanCreate(XR_SUCCEEDED(rr),vr,out);
        runtimeManagedDownstreamCreateInfo.store(nullptr,std::memory_order_release);
        runtimeManagedReentryCreateDevice.store(nullptr,std::memory_order_release);
        runtimeManagedReentryPhysical.store(VK_NULL_HANDLE,std::memory_order_release);
        if(kharvox::isSteamBackedOpenXRRuntime(s.runtimeKind)&&*vr==VK_SUCCESS){
            runtimeManagedSessionLoaderRoute.store(true,std::memory_order_release);
            log("[STEAM-XR] Switching retained callback to public Vulkan Loader GIPA for runtime session dispatch");
        }else{
            runtimeManagedSessionLoaderRoute.store(false,std::memory_order_release);
            if(!s.simulatorRuntime)runtimeManagedNextGipa.store(nullptr,std::memory_order_release);
        }
    }else{
        di.vulkanCreateInfo=downstreamCi;bridgeNextGipa=g;
        reentryPhysical=doomPhysical;reentryCreateDevice=create;di.pfnGetInstanceProcAddr=bridgeGipa;
        rr=s.createVulkanDevice(s.instance,&di,out,vr);
        reentryPhysical=VK_NULL_HANDLE;reentryCreateDevice=nullptr;
        kharvox::finishRuntimeVulkanCreate(XR_SUCCEEDED(rr),vr,out);
    }
    log("xrCreateVulkanDeviceKHR xr="+result(rr)+" vk="+std::to_string(*vr),true);
    return true;
}
void KharvoxXRInitialize(VkInstance instance){
    // Simulator preview windows can be created while loading/initializing the
    // runtime. Keep their owner alive across DOOM's loading/render thread swap.
    if(kharvox::sfs::vrEnabled())steamXrFrameThread.invoke([&]{initializeOpenXR(instance);});
    else initializeOpenXR(instance);
}
void KharvoxXRPreparePhysicalDeviceBinding(VkPhysicalDevice doomPhysical){std::lock_guard<std::mutex>l(mutex);if(s.useEnable2Bridge||s.vulkanPath!=kharvox::OpenXRVulkanPath::VulkanEnable1Direct||!s.graphicsDevice1||!s.instance||!s.system||!s.vkInstance||s.xrPhysical)return;const XrResult r=s.graphicsDevice1(s.instance,s.system,s.vkInstance,&s.xrPhysical);log("xrGetVulkanGraphicsDeviceKHR before vkCreateDevice "+result(r)+" runtimePhysical="+std::to_string(reinterpret_cast<uintptr_t>(s.xrPhysical))+" layeredPhysical="+std::to_string(reinterpret_cast<uintptr_t>(doomPhysical)));if(XR_FAILED(r))s.xrPhysical=VK_NULL_HANDLE;}
std::vector<std::string> KharvoxXRRequiredDeviceExtensions(){std::lock_guard<std::mutex>l(mutex);std::vector<std::string> out;if(!s.deviceExtensions||!s.system)return out;uint32_t size=0;if(XR_FAILED(s.deviceExtensions(s.instance,s.system,0,&size,nullptr))||!size)return out;std::vector<char>b(size);if(XR_FAILED(s.deviceExtensions(s.instance,s.system,size,&size,b.data())))return out;char*ctx=nullptr;for(char*t=strtok_s(b.data()," ",&ctx);t;t=strtok_s(nullptr," ",&ctx))out.emplace_back(t);return out;}
std::vector<std::string> KharvoxXRRequiredInstanceExtensions(){std::lock_guard<std::mutex>l(mutex);std::vector<std::string> out;if(!s.instanceExtensions||!s.system)return out;uint32_t size=0;if(XR_FAILED(s.instanceExtensions(s.instance,s.system,0,&size,nullptr))||!size)return out;std::vector<char>b(size);if(XR_FAILED(s.instanceExtensions(s.instance,s.system,size,&size,b.data())))return out;char*ctx=nullptr;for(char*t=strtok_s(b.data()," ",&ctx);t;t=strtok_s(nullptr," ",&ctx))out.emplace_back(t);return out;}
uint32_t KharvoxXRClampVulkanApiVersion(uint32_t requestedVersion){std::lock_guard<std::mutex>l(mutex);return s.runtimeMaxVulkanApiVersion?std::min(requestedVersion,s.runtimeMaxVulkanApiVersion):requestedVersion;}
void KharvoxXRSetDevice(VkPhysicalDevice p,VkDevice d,const KharvoxVulkanDispatch& v){std::lock_guard<std::mutex>l(mutex);s.physical=s.xrPhysical?s.xrPhysical:p;s.device=d;s.vk=v;
if (s.simulatorRuntime) {
    // xrPhysical was enumerated through the retained downstream GIPA, so public
    // loader exports must never receive it (the loader rejects that handle).
    const auto next = runtimeManagedNextGipa.load(std::memory_order_acquire);
    s.vk.getPhysicalDeviceMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        next ? next(s.vkInstance,"vkGetPhysicalDeviceMemoryProperties") : nullptr);
    s.vk.getPhysicalDeviceProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        next ? next(s.vkInstance,"vkGetPhysicalDeviceProperties") : nullptr);
    s.vk.getPhysicalDeviceQueueFamilyProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        next ? next(s.vkInstance,"vkGetPhysicalDeviceQueueFamilyProperties") : nullptr);
    if (!s.vk.getPhysicalDeviceMemoryProperties || !s.vk.getPhysicalDeviceProperties ||
        !s.vk.getPhysicalDeviceQueueFamilyProperties) {
        s.physical = VK_NULL_HANDLE; // Session creation stays gated; no mixed dispatch.
        log("[SIM-XR] matching downstream physical-device dispatch unavailable; session refused");
    } else {
        log("[SIM-XR] retained matching downstream physical-device dispatch for simulator");
    }
}
log("DOOM VkInstance="+std::to_string(reinterpret_cast<uintptr_t>(s.vkInstance))+" Physical="+std::to_string(reinterpret_cast<uintptr_t>(s.physical))+" Device="+std::to_string(reinterpret_cast<uintptr_t>(d)));}
void KharvoxXRSetQueueAccessCallbacks(KharvoxQueueAccessCallback lockCallback,KharvoxQueueAccessCallback unlockCallback){std::lock_guard<std::mutex>l(mutex);queueAccessLockCallback=lockCallback;queueAccessUnlockCallback=unlockCallback;log("[STEAM-XR-THREAD] Vulkan queue lock ownership delegated to XR lifecycle/copy calls");}
bool KharvoxXRRecommendedSourceSize(uint32_t* width, uint32_t* height) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!width || !height || !s.instance || !s.system || !s.enumerateViews) return false;
    uint32_t count{};
    if (XR_FAILED(s.enumerateViews(s.instance,s.system,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,0,&count,nullptr)) || count!=2) return false;
    std::array<XrViewConfigurationView,2> views{{{XR_TYPE_VIEW_CONFIGURATION_VIEW},{XR_TYPE_VIEW_CONFIGURATION_VIEW}}};
    if (XR_FAILED(s.enumerateViews(s.instance,s.system,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,2,&count,views.data())) || count!=2) return false;
    *width=std::max(views[0].recommendedImageRectWidth,views[1].recommendedImageRectWidth);
    *height=std::max(views[0].recommendedImageRectHeight,views[1].recommendedImageRectHeight);
    return *width && *height;
}

void KharvoxXRSetQueue(VkQueue q,uint32_t f,uint32_t i) {
    std::unique_lock<std::mutex> l(mutex,std::try_to_lock);
    if(!l.owns_lock()){log("Ignoring reentrant/auxiliary Vulkan queue callback family="+std::to_string(f));return;}
    if(!q||!s.physical||!s.vk.getPhysicalDeviceQueueFamilyProperties){log("[XR-QUEUE] missing physical queue dispatch",true);return;}
    uint32_t count{};
    s.vk.getPhysicalDeviceQueueFamilyProperties(s.physical,&count,nullptr);
    std::vector<VkQueueFamilyProperties> properties(count);
    if(count)s.vk.getPhysicalDeviceQueueFamilyProperties(s.physical,&count,properties.data());
    if(f>=count){log("[XR-QUEUE] invalid family="+std::to_string(f),true);return;}
    const auto flags=properties[f].queueFlags;
    const bool selected=kharvox::selectXrGraphicsQueue(s.queue,q,flags);
    log("[XR-QUEUE] candidate="+std::to_string(reinterpret_cast<uintptr_t>(q))+
        " family="+std::to_string(f)+" index="+std::to_string(i)+" flags="+std::to_string(flags)+
        " graphics="+((flags&VK_QUEUE_GRAPHICS_BIT)?"yes":"no")+
        " selected="+(selected?"yes":"no"),true);
    if(!selected)return;
    // Keep the queue/family consistent with the XR binding and command pools.
    s.queue=q;s.queueFamily=f;s.queueIndex=i;
    log("DOOM VkQueue="+std::to_string(reinterpret_cast<uintptr_t>(q))+" family="+std::to_string(f)+" index="+std::to_string(i));
    if(GetFileAttributesW(kharvox::runtimePath(L"enable_xr_session").c_str())!=INVALID_FILE_ATTRIBUTES)
        log("[XR] Session start deferred until vkGetDeviceQueue has returned through the Vulkan Loader");
    else log("XR session gated off (place enable_xr_session beside the KHARVOX module to enable)");
}
bool KharvoxXRStartSessionIfReady(){
    // 0.96 provider bring-up only. The producer currently fails before a
    // present, and its two-layer swapchain is not a verified SBS receipt.
    // Never feed its stereo output into the ordinary AER path.
    if((kharvox::vulkanSfsEnabled()||kharvox::sfs::nativeProbeEnabled())&&!kharvox::sfs::vrEnabled())return false;
    std::unique_lock<std::mutex>l(mutex);
    const bool steamRuntime=kharvox::isSteamBackedOpenXRRuntime(s.runtimeKind);
    if(steamRuntime&&steamSessionCreationInProgress)return false;
    if(!s.sessionReadiness.canCreate())return false;
    if(s.sessionReadiness.usable())return true;
    if(s.session)destroySessionResources();
    if(GetFileAttributesW(kharvox::runtimePath(L"enable_xr_session").c_str())==INVALID_FILE_ATTRIBUTES)return false;
    if(!s.instance||!s.vkInstance||!s.physical||!s.device||!s.queue)return false;
    const bool directVirtualDesktop=
        s.runtimeKind==kharvox::OpenXRRuntimeKind::VirtualDesktop;
    uint32_t completedNativePresents{};
    const auto active=s.doomSwapchains.find(s.startupActiveDoomSwapchain);
    if(active!=s.doomSwapchains.end())
        completedNativePresents=active->second.stablePresents;
    if(kharvox::deferSharedDeviceSessionCreation(directVirtualDesktop,
            false,completedNativePresents,s.runtimeKind==kharvox::OpenXRRuntimeKind::MetaOculus)){
        if(!s.vdxrSessionDeferralLogged){
            s.vdxrSessionDeferralLogged=true;
            log("[XR-STARTUP] OpenXR session and eye resources deferred until one DOOM swapchain completes 3 native Presents");
        }
        return false;
    }
    if(directVirtualDesktop||s.runtimeKind==kharvox::OpenXRRuntimeKind::MetaOculus)
        log("[XR-STARTUP] native WSI stable; creating OpenXR session after completed Present count="
            +std::to_string(completedNativePresents));
    log("[XR] Starting deferred session from a post-queue Vulkan call");
    if(s.simulatorRuntime&&kharvox::sfs::vrEnabled()){bool created{};steamXrFrameThread.invoke([&]{created=createSession();});return created;}
    if(!steamRuntime)return createSession();
    steamSessionCreationInProgress=true;
    log("[STEAM-XR] Releasing KHARVOX state lock before xrCreateSession for runtime Vulkan reentry");
    l.unlock();const bool created=createSession();l.lock();
    steamSessionCreationInProgress=false;return created;
}
void KharvoxXRPrepareFrame(VkSwapchainKHR swapchain){
    std::lock_guard<std::mutex>l(mutex);
    pollEvents();
    // DOOM throttles its native Vulkan presentation when its desktop window
    // loses foreground ownership. Restore it only while the startup stream is
    // being validated; after that the launcher status provides a manual action.
    if(s.running)focusDoomWindow();
    if(kharvox::native::installed()&&kharvox::native::qualityTransitionEpoch())return;
    if(!kharvox::isSteamBackedOpenXRRuntime(s.runtimeKind)||!s.running||!swapchain||!s.waitFrame||!s.beginFrame)return;
    const auto doomSwapchain=s.doomSwapchains.find(swapchain);
    if(doomSwapchain==s.doomSwapchains.end())return;
    const uint32_t requiredSteamStartupPresents=kharvox::startupStablePresents(
        true,false,s.fsr1Requested);
    if(doomSwapchain->second.stablePresents<requiredSteamStartupPresents){
        if(doomSwapchain->second.stablePresents==0)
            log("[STEAM-STARTUP] deferring XR lifecycle until one DOOM swapchain survives "
                +std::to_string(requiredSteamStartupPresents)+" native present(s)"
                +(s.fsr1Requested?" (FSR1 startup path)":""));
        return;
    }
    if(s.steamFramePrepared||s.steamFrameBegun){
        const uint64_t repeated=++s.steamRepeatedAcquires;
        if(repeated<=8||repeated%120==0)log("[STEAM-XR-SPLIT] acquire arrived before prepared frame was presented; keeping one pending frame count="+std::to_string(repeated)+" thread="+std::to_string(GetCurrentThreadId()));
        return;
    }
    const DWORD acquireCallerThread=GetCurrentThreadId();
    LARGE_INTEGER waitStart{},waitEnd{};
    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frame{XR_TYPE_FRAME_STATE};
    XrResult waitResult{XR_ERROR_RUNTIME_FAILURE};
    XrResult beginResult{XR_ERROR_RUNTIME_FAILURE};
    bool beginAttempted=false;
    const DWORD lifecycleThread=steamXrFrameThread.invoke([&]{
        QueryPerformanceCounter(&waitStart);
        waitResult=s.waitFrame(s.session,&waitInfo,&frame);
        QueryPerformanceCounter(&waitEnd);
        if(XR_SUCCEEDED(waitResult)){
            XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
            beginAttempted=true;
            beginResult=beginFrame(&beginInfo);
        }
    });
    const double waitMs=performanceMilliseconds(waitStart,waitEnd);
    ++s.steamWaitCalls;
    if(XR_FAILED(waitResult)){
        log("[STEAM-XR-SPLIT] xrWaitFrame "+result(waitResult)+" lifecycleThread="+std::to_string(lifecycleThread)+" acquireCallerThread="+std::to_string(acquireCallerThread)+" wait/begin/end="+std::to_string(s.steamWaitCalls)+"/"+std::to_string(s.steamBeginCalls)+"/"+std::to_string(s.steamEndCalls));
        return;
    }
    if(beginAttempted)++s.steamBeginCalls;
    if(XR_FAILED(beginResult)){
        log("[STEAM-XR-SPLIT] xrBeginFrame "+result(beginResult)+" lifecycleThread="+std::to_string(lifecycleThread)+" acquireCallerThread="+std::to_string(acquireCallerThread)+" wait/begin/end="+std::to_string(s.steamWaitCalls)+"/"+std::to_string(s.steamBeginCalls)+"/"+std::to_string(s.steamEndCalls));
        return;
    }
    s.steamFrameBegun=true;
    s.steamFramePrepared=true;
    s.steamPreparedFrame=frame;
    s.steamPreparedWaitResult=waitResult;
    s.steamPreparedBeginResult=beginResult;
    s.steamPreparedWaitMs=waitMs;
    QueryPerformanceCounter(&s.steamPreparedAt);
    s.steamPreparedThread=lifecycleThread;
    s.steamPreparedAcquireCallerThread=acquireCallerThread;
    s.steamPreparedDoomSwapchain=swapchain;
    const uint64_t prepared=++s.steamPreparedFrames;
    if(prepared<=8||prepared%120==0)log("[STEAM-XR-SPLIT] prepared="+std::to_string(prepared)+" wait="+result(waitResult)+" begin="+result(beginResult)+" shouldRender="+std::to_string(frame.shouldRender)+" predicted="+std::to_string(frame.predictedDisplayTime)+" period="+std::to_string(frame.predictedDisplayPeriod)+" waitFrameMs="+std::to_string(waitMs)+" lifecycleThread="+std::to_string(s.steamPreparedThread)+" acquireCallerThread="+std::to_string(s.steamPreparedAcquireCallerThread));
}
void KharvoxXRSwapchainCreated(VkSwapchainKHR sc,const VkSwapchainCreateInfoKHR&i){
    std::lock_guard<std::mutex>l(mutex);
    s.startupActiveDoomSwapchain=sc;
    auto& created=s.doomSwapchains[sc];
    created.extent=i.imageExtent;
    created.format=i.imageFormat;
    const ULONGLONG now=GetTickCount64();
    const bool compatibleReplacement=s.retiredCompatibleDoomSwapchainValid
        &&now-s.retiredCompatibleDoomSwapchainAt<=2000
        &&s.retiredCompatibleDoomSwapchain.extent.width==i.imageExtent.width
        &&s.retiredCompatibleDoomSwapchain.extent.height==i.imageExtent.height
        &&s.retiredCompatibleDoomSwapchain.format==i.imageFormat;
    if(compatibleReplacement){
        const bool directVirtualDesktop=
            s.runtimeKind==kharvox::OpenXRRuntimeKind::VirtualDesktop;
        created.stablePresents=kharvox::inheritStartupPresentProgress(
            directVirtualDesktop,s.runtimeKind==kharvox::OpenXRRuntimeKind::MetaOculus)
            ?s.retiredCompatibleDoomSwapchain.stablePresents:0;
        if(created.stablePresents)
            log("[STARTUP] compatible DOOM swapchain replacement inherited present progress="+std::to_string(created.stablePresents));
        else if(directVirtualDesktop||s.runtimeKind==kharvox::OpenXRRuntimeKind::MetaOculus)
            log("[XR-STARTUP] compatible replacement must establish its own stable native Present history");
        created.copyRecoveryPending=kharvox::shouldArmEarlySwapchainCopyRecovery(
            compatibleReplacement,
            s.retiredCompatibleDoomSwapchain.stablePresents,
            s.submittedLayerFrames);
        if(created.copyRecoveryPending){
            // None of the cached image content or view metadata may be carried
            // across the native swapchain lifetime boundary. The first Present
            // remains owned by DOOM; KHARVOX ends its already-open XR frame
            // without touching the replacement image.
            invalidateAlternatingStereoHistory(true);
            log("[STARTUP-RECOVERY] early compatible swapchain replacement armed; source history invalidated and first XR copy will be skipped");
        }
    }
    s.retiredCompatibleDoomSwapchainValid=false;
}
void KharvoxXRSwapchainImages(VkSwapchainKHR sc,uint32_t n,const VkImage*i){if(!i)return;std::lock_guard<std::mutex>l(mutex);auto&d=s.doomSwapchains[sc];d.images.assign(i,i+n);}
void KharvoxXRSwapchainDestroyed(VkSwapchainKHR sc){
    std::lock_guard<std::mutex>l(mutex);
    const auto retiring=s.doomSwapchains.find(sc);
    if(retiring!=s.doomSwapchains.end()){
        if(s.session&&kharvox::quiesceFsrStartupSwapchain(
                s.runtimeKind!=kharvox::OpenXRRuntimeKind::Unknown,
                s.fsr1Requested,retiring->second.stablePresents,
                s.submittedLayerFrames)
            &&s.queue&&s.vk.queueWaitIdle){
            const VkResult idleResult=s.vk.queueWaitIdle(s.queue);
            log("[FSR-STARTUP] runtime="+std::string(kharvox::openXRRuntimeKindName(s.runtimeKind))
                +" native graphics queue quiesced before presented startup swapchain retirement result="
                +std::to_string(static_cast<int>(idleResult)));
        }
        s.retiredCompatibleDoomSwapchain=retiring->second;
        s.retiredCompatibleDoomSwapchainValid=true;
        s.retiredCompatibleDoomSwapchainAt=GetTickCount64();
    }
    if(s.steamFramePrepared&&s.steamFrameBegun&&s.steamPreparedDoomSwapchain==sc&&s.endFrame){
        XrFrameEndInfo empty{XR_TYPE_FRAME_END_INFO};
        empty.displayTime=s.steamPreparedFrame.predictedDisplayTime;
        empty.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        XrResult endResult{XR_ERROR_RUNTIME_FAILURE};
        const DWORD frameThread=steamXrFrameThread.invoke([&]{endResult=endFrame(&empty);});
        ++s.steamEndCalls;
        if(XR_FAILED(endResult))++s.steamEndFailures;
        s.steamFramePrepared=false;
        s.steamFrameBegun=false;
        s.steamPreparedDoomSwapchain=VK_NULL_HANDLE;
        log("[STEAM-XR-SPLIT] pending acquire frame ended empty before DOOM swapchain destruction result="+result(endResult)+" lifecycleThread="+std::to_string(frameThread));
    }
    s.doomSwapchains.erase(sc);
    if(s.startupActiveDoomSwapchain==sc)
        s.startupActiveDoomSwapchain=VK_NULL_HANDLE;
}
void KharvoxXRNativePresentCompleted(VkSwapchainKHR sc,VkResult presentResult){
    std::lock_guard<std::mutex>l(mutex);
    const auto found=s.doomSwapchains.find(sc);
    if(found==s.doomSwapchains.end())return;
    const bool success=presentResult==VK_SUCCESS||presentResult==VK_SUBOPTIMAL_KHR;
    if(nativeQualityPresentEpoch){
        kharvox::native::qualityPresentCompleted(nativeQualityPresentEpoch,success);
        nativeQualityPresentEpoch=0;
    }
    if(!success)return;
    const uint32_t completed=++found->second.stablePresents;
    if(!s.session&&(s.runtimeKind==kharvox::OpenXRRuntimeKind::VirtualDesktop||s.runtimeKind==kharvox::OpenXRRuntimeKind::MetaOculus)
        &&s.startupActiveDoomSwapchain==sc
        &&completed==kharvox::startupStablePresents(false,true,false))
        log("[XR-STARTUP] active DOOM swapchain completed 3 native Presents; OpenXR session is now eligible");
}
bool KharvoxXRIsDoomSwapchainImage(VkImage image){std::lock_guard<std::mutex>l(mutex);for(const auto&pair:s.doomSwapchains)for(auto candidate:pair.second.images)if(candidate==image)return true;return false;}
void KharvoxXRPresent(VkQueue q,const VkPresentInfoKHR*p,bool* consumedPresentWaits,kharvox::GameImageLifetime::Use& gameImages){
    // A quality setter must not race a copy that passed the epoch check but
    // has not submitted yet. Order: Native presentation -> XR state -> queues.
    struct NativeQualityLease {
        bool active{kharvox::native::installed()};
        NativeQualityLease(){if(active)kharvox::native::lockQualityPresentation();}
        ~NativeQualityLease(){if(active)kharvox::native::unlockQualityPresentation();}
    } qualityLease;
    if(consumedPresentWaits)*consumedPresentWaits=false;
    std::lock_guard<std::mutex>l(mutex);
    // Freeze completed producer observations before xrWaitFrame can let the
    // frontend advance. A mixed window must never be labelled with the next eye.
    const auto aerSourceObservation=KharvoxCameraTakeAerWorldSource();
    pollEvents();
    const auto poseTraceStatus=kharvox::pose_trace::poll(KharvoxCameraGameplayActive()&&!s.quadMode,
        s.quadMode&&s.cinewindowAnchor.valid&&s.sessionState==XR_SESSION_STATE_FOCUSED);
    if(!poseTraceStatus.empty())log(poseTraceStatus);
    {kharvox::pose_trace::Event e{};e.kind=kharvox::pose_trace::PresentBegin;e.frame=s.frame;e.flags=poseTraceFlags();kharvox::pose_trace::record(e);}
    nativeQualityPresentEpoch=0;
    KharvoxHudPollQuadControls();
    if(!s.running||!p||!p->swapchainCount)return;
    auto it=s.doomSwapchains.find(p->pSwapchains[0]);
    if(it==s.doomSwapchains.end()||it->second.images.empty())return;
    uint32_t srcIndex=p->pImageIndices[0];
    if(srcIndex>=it->second.images.size())return;
    if(kharvox::native::installed()&&
        (nativeQualityPresentEpoch=kharvox::native::qualityTransitionEpoch())!=0){
        // Leave all binary Present waits for DOOM's downstream Present. No XR
        // acquire/copy is allowed while engine render targets are rebuilding.
        invalidateAlternatingStereoHistory(true);
        if(s.steamFramePrepared&&s.steamFrameBegun&&s.endFrame){
            XrFrameEndInfo empty{XR_TYPE_FRAME_END_INFO};
            empty.displayTime=s.steamPreparedFrame.predictedDisplayTime;
            empty.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            XrResult ended{XR_ERROR_RUNTIME_FAILURE};
            steamXrFrameThread.invoke([&]{ended=endFrame(&empty);});
            ++s.steamEndCalls;
            if(XR_FAILED(ended))++s.steamEndFailures;
            s.steamFramePrepared=false;s.steamFrameBegun=false;
            s.steamPreparedDoomSwapchain=VK_NULL_HANDLE;
            log("[NATIVE-QUALITY] prepared Steam frame ended without images: "+result(ended));
        }
        return;
    }
    const bool steamBackedRuntime=kharvox::isSteamBackedOpenXRRuntime(s.runtimeKind);
    const uint32_t requiredStablePresents=kharvox::startupStablePresents(
        steamBackedRuntime,s.runtimeKind==kharvox::OpenXRRuntimeKind::VirtualDesktop,
        s.fsr1Requested);
    const uint32_t stablePresent=it->second.stablePresents;
    if(stablePresent<requiredStablePresents){
        if(stablePresent==0){
            const char* guardName=steamBackedRuntime?"Steam-backed startup guard"
                :s.runtimeKind==kharvox::OpenXRRuntimeKind::VirtualDesktop
                    ?"Virtual Desktop":"Meta runtime-managed startup guard";
            log(std::string("new DOOM swapchain: ")+guardName
                +" defers XR work for "+std::to_string(requiredStablePresents)
                +" native present(s)"+(s.fsr1Requested?" (FSR1 startup path)":""));
        }
        return;
    }
    if(stablePresent==0)log("runtime-managed OpenXR: first DOOM present is immediately eligible for XR submission");
    const bool steamRuntime=steamBackedRuntime||(s.simulatorRuntime&&kharvox::sfs::vrEnabled());
    XrFrameState frame{XR_TYPE_FRAME_STATE};
    XrResult waitResult{XR_SUCCESS};
    XrResult beginResult{XR_SUCCESS};
    double steamWaitFrameMs=0.0;
    if(steamRuntime&&s.steamFramePrepared){
        frame=s.steamPreparedFrame;
        waitResult=s.steamPreparedWaitResult;
        beginResult=s.steamPreparedBeginResult;
        steamWaitFrameMs=s.steamPreparedWaitMs;
        LARGE_INTEGER presentAt{};
        QueryPerformanceCounter(&presentAt);
        const double renderLeadMs=performanceMilliseconds(s.steamPreparedAt,presentAt);
        const DWORD lifecycleThread=s.steamPreparedThread;
        const DWORD acquireCallerThread=s.steamPreparedAcquireCallerThread;
        s.steamFramePrepared=false;
        s.steamPreparedDoomSwapchain=VK_NULL_HANDLE;
        const uint64_t consumed=++s.steamConsumedPreparedFrames;
        if(!s.steamFrameBegun){
            ++s.steamLocalOrderViolations;
            log("[STEAM-XR-SPLIT] prepared frame lost its begun state before present; violation="+std::to_string(s.steamLocalOrderViolations));
        }
        if(consumed<=8||consumed%120==0)log("[STEAM-XR-SPLIT] consumed="+std::to_string(consumed)+" renderLeadMs="+std::to_string(renderLeadMs)+" lifecycleThread="+std::to_string(lifecycleThread)+" acquireCallerThread="+std::to_string(acquireCallerThread)+" presentCallerThread="+std::to_string(GetCurrentThreadId())+" wait/begin/end="+std::to_string(s.steamWaitCalls)+"/"+std::to_string(s.steamBeginCalls)+"/"+std::to_string(s.steamEndCalls));
    }else{
        LARGE_INTEGER waitStart{},waitEnd{};
        if(steamRuntime||kharvox::native::cpu::enabled())QueryPerformanceCounter(&waitStart);
        XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
        if(steamRuntime&&s.steamFrameBegun){
            ++s.steamLocalOrderViolations;
            log("[STEAM-XR-ORDER] local frame still open before xrWaitFrame; violation="+std::to_string(s.steamLocalOrderViolations));
        }
        DWORD lifecycleThread=GetCurrentThreadId();
        if(steamRuntime){
            lifecycleThread=steamXrFrameThread.invoke([&]{
                waitResult=s.waitFrame(s.session,&wi,&frame);
                if(XR_SUCCEEDED(waitResult)){
                    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
                    beginResult=beginFrame(&bi);
                }
            });
        }else {kharvox::native::cpu::Scope profile(kharvox::native::cpu::XrWaitFrame);waitResult=s.waitFrame(s.session,&wi,&frame);}
        if(steamRuntime)++s.steamWaitCalls;
        if(steamRuntime||kharvox::native::cpu::enabled())QueryPerformanceCounter(&waitEnd);
        steamWaitFrameMs=steamRuntime?performanceMilliseconds(waitStart,waitEnd):0.0;
        if(kharvox::native::cpu::enabled()){
            static uint64_t pacingSamples{};
            if(++pacingSamples<=8||pacingSamples%120==0)log("[XR-PACING] sample="+std::to_string(pacingSamples)+" nativeRequested="+std::to_string(kharvox::native::requested())+" nativePhase="+std::to_string(int(kharvox::native::phase()))+" quad="+std::to_string(s.quadMode)+" waitFrameMs="+std::to_string(performanceMilliseconds(waitStart,waitEnd))+" predictedPeriodNs="+std::to_string(frame.predictedDisplayPeriod)+" shouldRender="+std::to_string(frame.shouldRender)+" waitResult="+std::to_string(waitResult));
        }
        if(XR_FAILED(waitResult)){
            log("[STEAM-XR-ORDER] xrWaitFrame "+result(waitResult)+" lifecycleThread="+std::to_string(lifecycleThread)+" callerThread="+std::to_string(GetCurrentThreadId())+" wait/begin/end="+std::to_string(s.steamWaitCalls)+"/"+std::to_string(s.steamBeginCalls)+"/"+std::to_string(s.steamEndCalls));
            return;
        }
        if(!steamRuntime){XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};beginResult=beginFrame(&bi);}
        if(steamRuntime)++s.steamBeginCalls;
        if(XR_FAILED(beginResult)){
            log("[STEAM-XR-ORDER] xrBeginFrame "+result(beginResult)+" after wait="+result(waitResult)+" lifecycleThread="+std::to_string(lifecycleThread)+" callerThread="+std::to_string(GetCurrentThreadId())+" wait/begin/end="+std::to_string(s.steamWaitCalls)+"/"+std::to_string(s.steamBeginCalls)+"/"+std::to_string(s.steamEndCalls));
            return;
        }
        if(steamRuntime)s.steamFrameBegun=true;
    }
    auto r=beginResult;
    auto endFrameChecked=[&](const XrFrameEndInfo& endInfo,const std::string& reason){
        if(steamRuntime&&!s.steamFrameBegun){
            ++s.steamLocalOrderViolations;
            log("[STEAM-XR-ORDER] local xrEndFrame without open frame reason="+reason+" violation="+std::to_string(s.steamLocalOrderViolations));
        }
        XrResult endResult{XR_ERROR_RUNTIME_FAILURE};
        kharvox::pose_trace::Event traceEnd{};traceEnd.kind=kharvox::pose_trace::EndBegin;traceEnd.frame=s.frame;traceEnd.displayTime=endInfo.displayTime;traceEnd.flags=poseTraceFlags();traceEnd.source=endInfo.layerCount;kharvox::pose_trace::record(traceEnd);
        DWORD lifecycleThread=GetCurrentThreadId();
        if(steamRuntime){kharvox::native::cpu::Scope profile(kharvox::native::cpu::XrEndFrame);lifecycleThread=steamXrFrameThread.invoke([&]{endResult=endFrame(&endInfo);});}
        else {
            kharvox::native::cpu::Scope profile(kharvox::native::cpu::XrEndFrame);
            endResult=endFrame(&endInfo);
        }
        traceEnd.kind=kharvox::pose_trace::EndReturn;traceEnd.status=endResult;kharvox::pose_trace::record(traceEnd);
        if(steamRuntime){
            ++s.steamEndCalls;
            s.steamFrameBegun=false;
            if(XR_FAILED(endResult))++s.steamEndFailures;
            const bool noteworthy=waitResult!=XR_SUCCESS||beginResult!=XR_SUCCESS||endResult!=XR_SUCCESS;
            if(s.steamEndCalls<=8||s.steamEndCalls%120==0||noteworthy){
                log("[STEAM-XR-ORDER] cycle="+std::to_string(s.steamEndCalls)
                    +" reason="+reason
                    +" layers="+std::to_string(endInfo.layerCount)
                    +" wait="+result(waitResult)
                    +" begin="+result(beginResult)
                    +" end="+result(endResult)
                    +" shouldRender="+std::to_string(frame.shouldRender)
                    +" predicted="+std::to_string(frame.predictedDisplayTime)
                    +" period="+std::to_string(frame.predictedDisplayPeriod)
                    +" lifecycleThread="+std::to_string(lifecycleThread)
                    +" callerThread="+std::to_string(GetCurrentThreadId())
                    +" wait/begin/end="+std::to_string(s.steamWaitCalls)+"/"+std::to_string(s.steamBeginCalls)+"/"+std::to_string(s.steamEndCalls)
                    +" discarded="+std::to_string(s.steamDiscardedBegins)
                    +" endFailures="+std::to_string(s.steamEndFailures)
                    +" localViolations="+std::to_string(s.steamLocalOrderViolations));
            }
        }
        return endResult;
    };
    auto endEmptyFrame=[&](const std::string& reason){XrFrameEndInfo empty{XR_TYPE_FRAME_END_INFO};empty.displayTime=frame.predictedDisplayTime;empty.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;return endFrameChecked(empty,reason);};
    if(beginResult==XR_FRAME_DISCARDED){
        if(steamRuntime)++s.steamDiscardedBegins;
        const XrResult discardedEnd=endEmptyFrame("begin-discarded");
        if(steamRuntime&&XR_SUCCEEDED(discardedEnd))s.lastSteamDisplayTime=frame.predictedDisplayTime;
        s.frame++;
        return;
    }
    if(steamRuntime&&s.lastSteamDisplayTime&&frame.predictedDisplayTime<=s.lastSteamDisplayTime){
        const XrResult emptyResult=endEmptyFrame("duplicate-time");
        const uint64_t duplicates=++s.steamDuplicateFrames;
        s.frame++;
        if(duplicates<=8||duplicates%120==0)
            log("[STEAM-PERF] duplicate compositor time skipped predicted="+std::to_string(frame.predictedDisplayTime)+" previous="+std::to_string(s.lastSteamDisplayTime)+" waitFrameMs="+std::to_string(steamWaitFrameMs)+" count="+std::to_string(duplicates)+" end="+result(emptyResult));
        return;
    }
    if(!frame.shouldRender){
        const XrResult emptyResult=endEmptyFrame("shouldRender-false");
        if(steamRuntime&&XR_SUCCEEDED(emptyResult))s.lastSteamDisplayTime=frame.predictedDisplayTime;
        s.frame++;
        if(steamRuntime&&(s.frame<=8||steamWaitFrameMs>40.0))
            log("[STEAM-PERF] no-render frame waitFrameMs="+std::to_string(steamWaitFrameMs)+" predicted="+std::to_string(frame.predictedDisplayTime)+" period="+std::to_string(frame.predictedDisplayPeriod)+" end="+result(emptyResult));
        return;
    }
    if(it->second.copyRecoveryPending){
        it->second.copyRecoveryPending=false;
        const XrResult recoveryResult=endEmptyFrame("early-swapchain-replacement");
        if(steamRuntime&&XR_SUCCEEDED(recoveryResult))
            s.lastSteamDisplayTime=frame.predictedDisplayTime;
        s.frame++;
        log("[STARTUP-RECOVERY] replacement swapchain survived its first native Present; XR frame ended empty without Vulkan copy result="+result(recoveryResult));
        return;
    }
    if(kharvox::useSteamFsrQuadStartupHandshake(steamRuntime,s.fsr1Requested,
            s.quadMode,s.steamFsrStartupHandshakeComplete)){
        auto& handshakeEye=s.eyes[0];
        auto& handshakeImageState=s.eyeImageStates[0];
        uint32_t handshakeImageIndex{};
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrResult handshakeResult=handshakeImageState.owned()?XR_ERROR_CALL_ORDER_INVALID:
            acquireSwapchainImage(handshakeEye.handle,&acquire,&handshakeImageIndex);
        if(XR_SUCCEEDED(handshakeResult))handshakeImageState.acquired();
        if(handshakeImageState.owned()){
            XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wait.timeout=XR_INFINITE_DURATION;
            handshakeResult=s.waitImage(handshakeEye.handle,&wait);
            handshakeImageState.waited(handshakeResult!=XR_TIMEOUT_EXPIRED&&XR_SUCCEEDED(handshakeResult));
        }
        if(handshakeImageState.releasable()){
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            const XrResult releaseResult=releaseSwapchainImage(handshakeEye.handle,&release);
            if(XR_SUCCEEDED(releaseResult))handshakeImageState.released();
            if(XR_SUCCEEDED(handshakeResult))handshakeResult=releaseResult;
        }
        if(handshakeImageState.owned())
            requestSessionRestart("startup handshake retained XR image ownership "+result(handshakeResult));
        if(XR_SUCCEEDED(handshakeResult)&&!handshakeImageState.owned()){
            const int32_t quadWidth=static_cast<int32_t>(handshakeEye.width);
            const int32_t quadHeight=std::min(static_cast<int32_t>(handshakeEye.height),
                std::max(1,static_cast<int32_t>(std::lround(
                    static_cast<double>(handshakeEye.width)*9.0/16.0))));
            XrCompositionLayerQuad handshakeQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
            handshakeQuad.space=s.viewSpace;
            handshakeQuad.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
            handshakeQuad.subImage.swapchain=handshakeEye.handle;
            handshakeQuad.subImage.imageRect.offset={0,
                (static_cast<int32_t>(handshakeEye.height)-quadHeight)/2};
            handshakeQuad.subImage.imageRect.extent={quadWidth,quadHeight};
            handshakeQuad.pose.orientation.w=1.0f;
            handshakeQuad.pose.position.z=-kharvox::fullFrameQuadDistanceMeters(false);
            const float width=kharvox::fullFrameQuadWidthMeters(false);
            handshakeQuad.size={width,width*9.0f/16.0f};
            const XrCompositionLayerBaseHeader* layer=
                reinterpret_cast<const XrCompositionLayerBaseHeader*>(&handshakeQuad);
            XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
            endInfo.displayTime=frame.predictedDisplayTime;
            endInfo.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            endInfo.layerCount=1;
            endInfo.layers=&layer;
            handshakeResult=endFrameChecked(endInfo,"fsr1-quad-startup-handshake");
        }else{
            const XrResult emptyResult=endEmptyFrame("fsr1-quad-handshake-image-failed");
            if(handshakeResult==XR_SUCCESS)handshakeResult=emptyResult;
        }
        if(handshakeResult!=XR_TIMEOUT_EXPIRED&&XR_SUCCEEDED(handshakeResult)){
            s.steamFsrStartupHandshakeComplete=true;
            s.lastSteamDisplayTime=frame.predictedDisplayTime;
        }
        s.frame++;
        log("[STEAM-FSR1-STARTUP] first compositor frame submitted as no-copy Quad result="
            +result(handshakeResult)+"; native DOOM present retained; FSR1 remains deferred until gameplay");
        return;
    }
    double steamCopyWaitMs=0.0;
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};li.viewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;li.displayTime=frame.predictedDisplayTime;li.space=s.space;
    XrViewState vs{XR_TYPE_VIEW_STATE};uint32_t vc=0;r=s.locateViews(s.session,&li,&vs,2,&vc,s.views.data());
    if(XR_FAILED(r)||vc<2){log("xrLocateViews "+result(r));endEmptyFrame("locate-views-failed");return;}
    // Use the centred portion of the asymmetric FOV that is visible to both
    // eyes. This is the stable coordinate system for a view-space HUD and is
    // independent of display resolution, render scale, and lens asymmetry.
    const float locatedHudSafeTanHalfHorizontal=std::max(0.10f,std::min({
        -std::tan(s.views[0].fov.angleLeft),std::tan(s.views[0].fov.angleRight),
        -std::tan(s.views[1].fov.angleLeft),std::tan(s.views[1].fov.angleRight)}));
    const float locatedHudSafeTanHalfVertical=std::max(0.10f,std::min({
        -std::tan(s.views[0].fov.angleDown),std::tan(s.views[0].fov.angleUp),
        -std::tan(s.views[1].fov.angleDown),std::tan(s.views[1].fov.angleUp)}));
    // The view FOV is a headset property. Latch its first valid value for the
    // session so runtime prediction noise cannot make the HUD breathe/jitter.
    if(s.hudSafeTanHalfHorizontal<=0.0f||s.hudSafeTanHalfVertical<=0.0f){
        s.hudSafeTanHalfHorizontal=locatedHudSafeTanHalfHorizontal;
        s.hudSafeTanHalfVertical=locatedHudSafeTanHalfVertical;
    }
    const float hudSafeTanHalfHorizontal=s.hudSafeTanHalfHorizontal;
    const float hudSafeTanHalfVertical=s.hudSafeTanHalfVertical;
    KharvoxHudSetHeadsetGeometry(s.hudSurfaceWidth,s.hudSurfaceHeight,
        hudSafeTanHalfHorizontal,hudSafeTanHalfVertical);
    constexpr float hudContentAspect=16.0f/9.0f;
    constexpr float hudReferenceDistanceMeters=0.50f;
    constexpr float hudSafeFovFraction=0.90f;
    constexpr float hudReferenceUserScale=3.0f;
    const float hudAppliedLayoutFit=kharvox::selectHudLayoutFit(
        hudSafeTanHalfHorizontal,hudSafeTanHalfVertical);
    const float hudAngularWidthMeters=2.0f*hudReferenceDistanceMeters
        *hudAppliedLayoutFit
        *hudSafeFovFraction*(KharvoxHudQuadScale()/hudReferenceUserScale);
    const float locatedFullHalfX=0.5f*std::min(
        s.views[0].fov.angleRight-s.views[0].fov.angleLeft,
        s.views[1].fov.angleRight-s.views[1].fov.angleLeft);
    const float locatedFullHalfY=0.5f*std::min(
        s.views[0].fov.angleUp-s.views[0].fov.angleDown,
        s.views[1].fov.angleUp-s.views[1].fov.angleDown);
    const XrFovf locatedCommonEyeFov{
        -locatedFullHalfX,locatedFullHalfX,locatedFullHalfY,-locatedFullHalfY};
    const bool sfsBackend=kharvox::sfs::vrEnabled();
    const bool nativeBackend=kharvox::native::installed()||sfsBackend;
    const float immersiveHalfX=kharvox::immersiveProjectionHalfAngle(
        s.runtimeKind,nativeBackend,s.views[0].fov.angleLeft,s.views[0].fov.angleRight,
        s.views[1].fov.angleLeft,s.views[1].fov.angleRight);
    const float immersiveHalfY=kharvox::immersiveProjectionHalfAngle(
        s.runtimeKind,nativeBackend,s.views[0].fov.angleDown,s.views[0].fov.angleUp,
        s.views[1].fov.angleDown,s.views[1].fov.angleUp);
    const XrFovf locatedImmersiveRenderFov{
        -immersiveHalfX,immersiveHalfX,immersiveHalfY,-immersiveHalfY};
    static bool nativeCoverageLogged=false;
    if(nativeBackend&&!nativeCoverageLogged){
        nativeCoverageLogged=true;
        log("[NATIVE-FOV] runtime-enclosing symmetric render/submit FOV degrees="
            +std::to_string(2.f*immersiveHalfX*57.2957795f)+"x"
            +std::to_string(2.f*immersiveHalfY*57.2957795f));
    }
    XrPosef locatedMonoPose=s.views[0].pose;
    locatedMonoPose.position.x=(s.views[0].pose.position.x+s.views[1].pose.position.x)*0.5f;
    locatedMonoPose.position.y=(s.views[0].pose.position.y+s.views[1].pose.position.y)*0.5f;
    locatedMonoPose.position.z=(s.views[0].pose.position.z+s.views[1].pose.position.z)*0.5f;
    const bool sourceMonoPoseValid=s.programmedMonoPoseValid;
    const XrPosef sourceMonoPose=sourceMonoPoseValid?s.programmedMonoPose:locatedMonoPose;
    const XrFovf sourceMonoFov=sourceMonoPoseValid?s.programmedMonoFov:locatedImmersiveRenderFov;
    const bool sourceMonoEyeViewsValid=s.programmedMonoEyeViewsValid;
    const std::array<XrPosef,2> sourceMonoEyePose=sourceMonoEyeViewsValid?s.programmedMonoEyePose:std::array<XrPosef,2>{s.views[0].pose,s.views[1].pose};
    const std::array<XrFovf,2> sourceMonoEyeFov=sourceMonoEyeViewsValid?s.programmedMonoEyeFov:std::array<XrFovf,2>{s.views[0].fov,s.views[1].fov};
    static bool runtimeFovLogged=false;
    if(!runtimeFovLogged){constexpr float degrees=57.2957795131f;std::ostringstream o;o<<"Runtime asymmetric eye FOV degrees eye0(L/R/U/D)="<<s.views[0].fov.angleLeft*degrees<<'/'<<s.views[0].fov.angleRight*degrees<<'/'<<s.views[0].fov.angleUp*degrees<<'/'<<s.views[0].fov.angleDown*degrees<<" eye1="<<s.views[1].fov.angleLeft*degrees<<'/'<<s.views[1].fov.angleRight*degrees<<'/'<<s.views[1].fov.angleUp*degrees<<'/'<<s.views[1].fov.angleDown*degrees;log(o.str());runtimeFovLogged=true;}
    static const bool nativeStereoEnabled=false;
    static const bool nativeTwoViewEnabled=false;
    static const bool cachedEyeReprojection=GetFileAttributesW(kharvox::runtimePath(L"enable_cached_eye_reprojection").c_str())!=INVALID_FILE_ATTRIBUTES;
    static const bool nvidiaAfwMarker=false;
    const bool nativePackedStereoAtFrame=nativeBackend||((nativeStereoEnabled||nativeTwoViewEnabled)
        &&KharvoxCameraNativeStereoActive());
    updateHeadPose(vs,frame.predictedDisplayTime,nativePackedStereoAtFrame);
    s.controllerFrameYaw={s.acceptedPhysicalYaw,s.artificialTurnPublishedDegrees,
        kharvox::usePublishedControllerYaw(nativePackedStereoAtFrame,sfsBackend)};
    for(int eye=0;eye<2;++eye)traceEye(kharvox::pose_trace::Located,eye,s.views[eye].pose,s.views[eye].fov,frame.predictedDisplayTime,0,frame.predictedDisplayPeriod,unsigned(vs.viewStateFlags)<<16);
    if((vs.viewStateFlags&XR_VIEW_STATE_ORIENTATION_VALID_BIT)!=0){s.programmedMonoPose=locatedMonoPose;s.programmedMonoFov=locatedImmersiveRenderFov;s.programmedMonoPoseValid=true;s.programmedMonoEyePose={s.views[0].pose,s.views[1].pose};s.programmedMonoEyeFov={s.views[0].fov,s.views[1].fov};s.programmedMonoEyeViewsValid=true;}
    else{s.programmedMonoPoseValid=false;s.programmedMonoEyeViewsValid=false;}
    // DOOM's current source already contains the weapon rendered from the
    // previously published controller input. Preserve that input before polling
    // the next prediction; compositing fresh hands onto it creates a time skew.
    const auto sourceLeftGripController=s.leftGripController;
    const auto sourceRightGripController=s.rightGripController;
    updateGameplayActions(frame.predictedDisplayTime);updateXInputHaptics();updatePresentationMode();updatePsvr2TriggerPolicy();
    kharvox::native::StereoFrame nativeFrame{};
    bool nativeFrameValid=false;
    if(nativeBackend){
        nativeFrameValid=sfsBackend?kharvox::sfs::pair(s.device,it->second.images[srcIndex],it->second.extent,it->second.format,nativeFrame,&aerSourceObservation):kharvox::native::pair(it->second.images[srcIndex],it->second.extent,it->second.format,nativeFrame);
    }

    std::array<XrView,2> lockedCinematicViewSpaceViews{{
        {XR_TYPE_VIEW},{XR_TYPE_VIEW}}};
    bool lockedCinematicViewSpaceViewsValid=false;
    if(s.immersiveCinematicActive&&!s.immersiveCinematicFreelookActive){
        XrViewLocateInfo viewSpaceLocate=li;
        viewSpaceLocate.space=s.viewSpace;
        XrViewState viewSpaceState{XR_TYPE_VIEW_STATE};
        uint32_t viewSpaceCount{};
        const XrResult viewSpaceResult=s.locateViews(
            s.session,&viewSpaceLocate,&viewSpaceState,2,&viewSpaceCount,
            lockedCinematicViewSpaceViews.data());
        lockedCinematicViewSpaceViewsValid=XR_SUCCEEDED(viewSpaceResult)
            &&viewSpaceCount>=2
            &&(viewSpaceState.viewStateFlags
                &XR_VIEW_STATE_ORIENTATION_VALID_BIT)!=0
            &&(viewSpaceState.viewStateFlags
                &XR_VIEW_STATE_POSITION_VALID_BIT)!=0;
        static bool exactViewSpaceLogged=false;
        static bool viewSpaceFailureLogged=false;
        if(lockedCinematicViewSpaceViewsValid&&!exactViewSpaceLogged){
            exactViewSpaceLogged=true;
            log("[IMMERSIVE] exact runtime VIEW-space eye poses ACTIVE; locked cinematic IPD/cant/centering are runtime-provided");
        }else if(!lockedCinematicViewSpaceViewsValid&&!viewSpaceFailureLogged){
            viewSpaceFailureLogged=true;
            log("[IMMERSIVE] runtime VIEW-space locate unavailable "
                +result(viewSpaceResult)+"; derived eye-to-head fallback retained");
        }
    }
    const bool upgradeCinematicRefreshGuard=
        KharvoxHudUpgradeCinematicRefreshGuardActive();
    const auto nativeAdaptiveGuardNow=GetTickCount64();
    const bool nativeAdaptiveParticipantObserved=
        consumeNativeAdaptiveParticipantObservation();
    const bool nativeAdaptiveParticipantEligible=KharvoxCameraWorldActive()
        &&!KharvoxHudPauseMenuActive()&&!KharvoxHudDeathMenuActive()
        &&!KharvoxHudFullscreenMenuActive()&&!KharvoxHudTutorialActive();
    s.nativeAdaptiveParticipantGuardUntilTick=
        kharvox::updateNativeAdaptiveParticipantGuardUntil(
            nativeAdaptiveParticipantEligible,
            nativeAdaptiveParticipantObserved,nativeAdaptiveGuardNow,
            s.nativeAdaptiveParticipantGuardUntilTick,
            nativeAdaptiveParticipantReleaseGraceMilliseconds);
    const bool nativeAdaptiveParticipantGuard=
        kharvox::nativeAdaptiveParticipantGuardIsActive(
            nativeAdaptiveGuardNow,
            s.nativeAdaptiveParticipantGuardUntilTick);
    if(nativeAdaptiveParticipantGuard
        !=s.nativeAdaptiveParticipantGuardActive){
        s.nativeAdaptiveParticipantGuardActive=nativeAdaptiveParticipantGuard;
        s.comfortInteractiveParticipantActive=
            kharvox::shouldKeepNativeParticipantInComfortProjection(
                s.immersiveCinematics,nativeAdaptiveParticipantGuard,
                KharvoxHudTutorialActive(),KharvoxCameraPlayerWeaponControlActive(),
                KharvoxWeaponIsTrackingActive(),
                KharvoxWeaponCurrentKind()!=KharvoxWeaponKind::Unknown);
        log(nativeAdaptiveParticipantGuard
            ?(s.immersiveCinematics
                ?"[AER-CINEMATIC] native sync participant observed; animated camera/weapon render pairing armed"
                :s.comfortInteractiveParticipantActive
                    ?"[COMFORT-CINEMATIC] native participant observed with live weapon input; Projection guard armed"
                    :"[COMFORT-CINEMATIC] native passive participant observed; centered Cinewindow guard armed")
            :(s.immersiveCinematics
                ?"[AER-CINEMATIC] native sync participant cleared; animated render pairing released"
                :"[COMFORT-CINEMATIC] native sync participant cleared; Cinewindow guard released"));
        if(s.comfortInteractiveParticipantActive)
            log("[COMFORT-CINEMATIC] live weapon input confirmed; participant remains stereoscopic PROJECTION");
    }
    const bool comfortInteractiveParticipant=
        s.comfortInteractiveParticipantActive;
    updateImmersiveCinematicRefresh(
        !KharvoxCameraBossSequenceActive()&&kharvox::shouldHoldImmersiveRefresh(
            s.immersiveCinematics||comfortInteractiveParticipant,s.quadMode,
            KharvoxCameraCutsceneActive(),upgradeCinematicRefreshGuard,
            nativeAdaptiveParticipantGuard),
        frame.predictedDisplayPeriod);
    updateCinewindowAnchor(vs,frame.predictedDisplayTime);
    kharvox::hudgpu::QuadCandidate hudSource{};
    const bool hudQuadRequested = !s.quadMode
        && s.hudQuad.handle != XR_NULL_HANDLE
        && KharvoxCameraGameplayActive()
        && !KharvoxCameraCutsceneActive()
        && !KharvoxHudPauseMenuActive()
        && !KharvoxHudDeathMenuActive()
        && !KharvoxHudFullscreenMenuActive()
        && gameImages.select([&]{return kharvox::hudgpu::getQuadCandidate(hudSource);},[&]{
            return std::array{kharvox::GameImageLifetime::key(hudSource.image)};
        })
        && hudSource.format == VK_FORMAT_R8G8B8A8_UNORM
        && hudSource.extent.width > 0
        && hudSource.extent.height > 0;
    uint32_t hudImageIndex{};
    bool hudImageCopied{};
    constexpr float radiansToDegrees=57.2957795131f;
    const bool immersiveCameraArmed=kharvox::shouldArmImmersiveCamera(
        s.immersiveCinematicCameraRequested,s.quadMode,s.centeredQuadTransitionPending);
    KharvoxCameraSetImmersiveCinematicFov(
        (locatedImmersiveRenderFov.angleRight-locatedImmersiveRenderFov.angleLeft)*radiansToDegrees,
        (locatedImmersiveRenderFov.angleUp-locatedImmersiveRenderFov.angleDown)*radiansToDegrees,
        immersiveCameraArmed,
        s.immersiveCinematicActive);
    KharvoxCameraSetImmersiveCinematicFreelook(
        immersiveCameraArmed&&s.immersiveCinematicFreelook&&!KharvoxCameraBossSequenceActive());
    if(s.immersiveCinematicFreelookActive&&!s.immersiveCinematicLayerPositionHeld){s.immersiveCinematicLayerPosition=sourceMonoPose.position;s.immersiveCinematicLayerPositionHeld=true;log("[IMMERSIVE] OpenXR layer position held at cinematic entry; rendered HMD rotation pose synchronized");}
    else if(!s.immersiveCinematicFreelookActive&&s.immersiveCinematicLayerPositionHeld){s.immersiveCinematicLayerPositionHeld=false;log("[IMMERSIVE] OpenXR layer position hold released");}
    if(nativeBackend){
        kharvox::native::FramePose next{};next.serial=s.frame+1;
        next.displayTime=frame.predictedDisplayTime;next.head=locatedMonoPose;
        next.views=s.views;next.worldScale=s.worldScale;
        next.bodyTracking=laserBodyTrackingTransform();
        next.exactProjectionCrop=s.runtimeKind==kharvox::OpenXRRuntimeKind::MetaOculus;
        next.cinematic=!s.quadMode&&s.immersiveCinematicActive&&KharvoxCameraWorldActive();
        next.scripted=!s.quadMode&&!next.cinematic&&s.nativeAdaptiveParticipantGuardActive&&KharvoxCameraWorldActive();
        next.gameplay=!s.quadMode&&!next.cinematic&&!next.scripted&&KharvoxCameraGameplayActive()&&!KharvoxCameraCutsceneActive();
        // Snapshot the inputs before recording the source ID into the SFS
        // frame. Refresh phase zero each frame; there is no second CPU eye.
        if(sfsBackend){
            KharvoxCameraSetAerRenderPair(kharvox::aerFirstRenderEye,next.gameplay);
            KharvoxWeaponSetAerRenderPair(kharvox::aerFirstRenderEye,next.gameplay,KharvoxCameraDiagnosticPoseId());
        }
        next.source={KharvoxCameraDiagnosticPoseId(),KharvoxCameraLevelTransitionGeneration(),
            kharvox::native::sceneDomain(s.quadMode,next.cinematic,next.scripted,next.gameplay)};
        next.weaponKind=static_cast<uint32_t>(KharvoxWeaponCurrentKind());
        next.leftHanded=s.leftHanded;next.twoHanded=s.twoHandLatched;
        next.weaponControlActive=KharvoxCameraPlayerWeaponControlActive();
        next.viewSpace=next.cinematic&&!s.immersiveCinematicFreelookActive;
        next.controllersValid={s.leftGripController.valid,s.rightGripController.valid};
        next.controllers[0]={s.leftGripController.orientation,s.leftGripController.position};
        next.controllers[1]={s.rightGripController.orientation,s.rightGripController.position};
        for(size_t e=0;e<2;++e){
            next.views[e].fov=locatedImmersiveRenderFov;
            next.submitFov[e]=next.exactProjectionCrop?s.views[e].fov:locatedImmersiveRenderFov;
            if(next.viewSpace)next.views[e].pose=lockedCinematicViewSpaceViewsValid
                ?lockedCinematicViewSpaceViews[e].pose:poseRelativeTo(locatedMonoPose,s.views[e].pose);
            else if(next.cinematic&&s.immersiveCinematicLayerPositionHeld){
                next.views[e].pose.position.x+=s.immersiveCinematicLayerPosition.x-locatedMonoPose.position.x;
                next.views[e].pose.position.y+=s.immersiveCinematicLayerPosition.y-locatedMonoPose.position.y;
                next.views[e].pose.position.z+=s.immersiveCinematicLayerPosition.z-locatedMonoPose.position.z;
            }
        }
        nativeFrameValid=nativeFrameValid&&kharvox::native::sameSceneContext(
            nativeFrame.pose.source,next.source,nativeFrame.pose.viewSpace,next.viewSpace);
        if(sfsBackend){
            next.exactProjectionCrop=true;
            // Owned sources have eye-shaped extents: project directly to each
            // eye instead of rendering a wide carrier and cropping it again.
            for(size_t e=0;e<2;++e){next.views[e].fov=kharvox::sfs::sourceRingActive(s.device)?s.views[e].fov:locatedImmersiveRenderFov;next.submitFov[e]=s.views[e].fov;}
            if(next.viewSpace)next.head={{0,0,0,1},{0,0,0}};
            else if(next.cinematic&&s.immersiveCinematicLayerPositionHeld)next.head.position=s.immersiveCinematicLayerPosition;
            kharvox::sfs::prepare(s.device,next,locatedImmersiveRenderFov);
        }else kharvox::native::prepare(next);
        if(!sfsBackend){KharvoxCameraSetAerRenderPair(0,false);KharvoxWeaponSetAerRenderPair(0,false);}
        KharvoxCameraSetStereoEye(0,(locatedImmersiveRenderFov.angleRight-locatedImmersiveRenderFov.angleLeft)*radiansToDegrees,
            (locatedImmersiveRenderFov.angleUp-locatedImmersiveRenderFov.angleDown)*radiansToDegrees,0,0,
            kharvox::native::renderScene(s.quadMode,next.gameplay,next.cinematic||next.scripted));
        // Keep the selected VR mode while a matching native pair warms up.
        // Withhold projection below instead of displaying an unrelated mono/Quad.
        static int lastNativeDomain=-1;
        if(lastNativeDomain!=int(next.source.domain)){
            lastNativeDomain=int(next.source.domain);
            log("[NATIVE-SOURCE] domain="+std::to_string(lastNativeDomain)
                +" level="+std::to_string(next.source.level)+" pose="+std::to_string(next.source.poseId));
        }
        static int lastNativeCinematic=-1;
        const int cinematicState=next.cinematic?(nativeFrameValid?2:1):0;
        if(cinematicState!=lastNativeCinematic){
            log("[NATIVE-CINEMATIC] state="+std::to_string(cinematicState)
                +" (0=inactive 1=warming 2=current-pair) AER=false viewSpace="+std::to_string(next.viewSpace));
            lastNativeCinematic=cinematicState;
        }
    }
    const bool fsr1Requested=s.fsr1Requested;
    // A Native cinematic uses two current producer images, just like gameplay.
    const bool nativePackedStereo=nativeBackend
        ? kharvox::native::useCurrentPair(nativeFrameValid,s.quadMode,s.immersiveCinematicActive,nativeFrame.pose.cinematic)
        : nativePackedStereoAtFrame;
    const bool nativeTwoViewPacked=nativeTwoViewEnabled&&KharvoxCameraNativeTwoViewActive();
    bool stereoNow=(nativeBackend?nativePackedStereo:
        (nativePackedStereo||(!s.quadMode&&KharvoxCameraWorldActive())))
        &&!kharvox::shouldSuppressStereoForCenteredQuadTransition(s.centeredQuadTransitionPending);
    const bool steamLinkProjectionGameplay=!s.quadMode
        &&!s.centeredQuadTransitionPending
        &&KharvoxCameraWorldActive()&&!s.immersiveCinematicActive;
    const bool coherentAerPairPresentationContext=
        kharvox::useCoherentAerPairPresentationContext(
            steamLinkProjectionGameplay,s.immersiveCinematicActive,
            s.quadMode,s.centeredQuadTransitionPending);
    // AER must remain stereoscopic under SteamVR's streamed-headset Meta
    // compatibility runtime. The old exact same-frame path is now used only
    // after an explicitly selected AFW renderer has failed to initialize.
    bool steamLinkSameFrameMono=kharvox::useSteamLinkSameFrameMonoFallback(
        s.steamMetaCompatibilityMode,steamLinkProjectionGameplay,
        nvidiaAfwMarker,false,false);
    static bool steamLinkSameFrameMonoLogged=false;
    static bool steamLinkAerStereoLogged=false;
    if(steamLinkSameFrameMono){
        stereoNow=false;
        if(!steamLinkSameFrameMonoLogged){
            log("[STEAMLINK-MONO] same-frame exact per-eye PROJECTION ACTIVE; SRGB cache and source-pose/FOV lock enabled");
            steamLinkSameFrameMonoLogged=true;
        }
    }
    // Immersive cinematics must remain real stereo. CameraHook now applies
    // per-eye IPD to the special cinematic caller and the AER pair guard keeps
    // its native animation pose coherent with the scripted weapon prop.
    const VkExtent2D eyeSourceExtent=nativeTwoViewPacked
        ?VkExtent2D{it->second.extent.width/2,it->second.extent.height}
        :it->second.extent;
    const auto transitionNow=GetTickCount64();
    const auto currentLevelGeneration=KharvoxCameraLevelTransitionGeneration();
    if(!s.stereoLevelGenerationValid){
        s.stereoLevelGeneration=currentLevelGeneration;
        s.stereoLevelGenerationValid=true;
    }else if(currentLevelGeneration!=s.stereoLevelGeneration){
        s.stereoLevelGeneration=currentLevelGeneration;
        // A checkpoint or map load may replace the world without first
        // entering Quad. Never combine one eye cached before that boundary
        // with the other eye rendered afterwards. Force the normal mono ->
        // settled-left -> settled-right startup before stereo is submitted.
        invalidateAlternatingStereoHistory(true);
        s.stereoLevelGuardUntilTick=
            transitionNow+levelTransitionMonoGuardMilliseconds;
        if(s.alternatingStereo){
            s.alternatingStereo=false;
            s.alternatingStereoSkipInitialCapture=false;
            s.alternatingStereoWarmupActive=false;
            s.alternatingStereoWarmupEye=kharvox::aerFirstRenderEye;
            s.alternatingStereoWarmupFramesRemaining=0;
            s.stereoStartupControllerMissing=false;
            // Native/SFS already prepared the next centered camera above.
            // Only AER needs to disarm its alternating camera while settling.
            if(!nativeBackend)KharvoxCameraSetStereoEye(0,0,0,0,0,false);
        }
        log("[HUD22-AER] level/checkpoint transition discarded both eye histories and armed central-mono settle guard generation="
            +std::to_string(currentLevelGeneration));
    }
    const bool levelTransitionMonoGuard=
        kharvox::shouldSuppressStereoForTimedTransition(
            transitionNow,s.stereoLevelGuardUntilTick);
    if(!levelTransitionMonoGuard&&s.stereoLevelGuardUntilTick){
        s.stereoLevelGuardUntilTick=0;
        log("[HUD22-AER] level/checkpoint central-mono settle guard released");
    }
    // The old r29 Argent timer is only a pacing trigger now. Source-qualified
    // AER handles both generic and ordinary-camera scripted cinematics.
    if(!nativeBackend&&levelTransitionMonoGuard)stereoNow=false;
    const bool restartingAlternatingStereo=stereoNow&&!s.alternatingStereo;
    const bool stereoExtentChanged=s.stereoCacheExtent.width!=eyeSourceExtent.width
        ||s.stereoCacheExtent.height!=eyeSourceExtent.height;
    if(stereoNow&&(restartingAlternatingStereo||stereoExtentChanged)){
        // Reusing GPU images must not imply reusing their old level/eye
        // contents. Otherwise the first eye cache refreshes while
        // the second eye is submitted from the previous loading/gameplay state.
        if(restartingAlternatingStereo&&!stereoExtentChanged)
            invalidateAlternatingStereoHistory(true);
        stereoNow=ensureStereoCache(eyeSourceExtent);
        if(nativeBackend&&!stereoNow)kharvox::native::fail("native stereo eye cache allocation failed");
        s.alternatingStereo=stereoNow;
        s.renderEye=kharvox::aerFirstRenderEye;
        // The image which reaches this hook was rendered before this XR frame
        // selected its next eye.  At a QUAD/loading -> PROJECTION transition it
        // is therefore still a mono/old-camera image and must not be labelled
        // as the left-eye cache.  Submit it to both eyes for this transition,
        // then capture a deliberately programmed right frame followed by left.
        s.alternatingStereoSkipInitialCapture=stereoNow&&!nativePackedStereo;
        s.alternatingStereoWarmupActive=stereoNow&&!nativePackedStereo;
        s.alternatingStereoWarmupEye=kharvox::aerFirstRenderEye;
        s.alternatingStereoWarmupFramesRemaining=0;
        s.stereoStartupControllerMissing=stereoNow&&!nativePackedStereo
            &&(!s.rightController.valid||!s.leftController.valid);
        if(stereoNow&&!nativePackedStereo){
            log(std::string("[HUD22-AER] pipeline-settled left/right cache warmup armed; controllerPair=")
                +(s.stereoStartupControllerMissing?"incomplete":"complete"));
        }
        log(stereoNow?(nativeTwoViewPacked?"DOOM genuine native two-view SBS ACTIVE":nativeBackend?(sfsBackend?"Vulkan SFS same-frame eye pair ACTIVE (headset validation pending)":nativeFrame.rightEyeBlackDiagnostic?"Native LEFT-ONLY DIAGNOSTIC active; black right; not full stereo":"Native Stereo frame-root pair ACTIVE (experimental, not live validated)"):nativePackedStereo?"DOOM native full-frame mono validation ACTIVE":cachedEyeReprojection?"Alternating-eye stereo; r262 uses cached poses without gameplay turn compensation":"Alternating-eye stereo capture ACTIVE"):"Stereo cache failed; M1B mono fallback");
    }else if(!stereoNow&&s.alternatingStereo){
        s.alternatingStereo=false;
        s.alternatingStereoSkipInitialCapture=false;
        s.alternatingStereoWarmupActive=false;
        s.alternatingStereoWarmupEye=kharvox::aerFirstRenderEye;
        s.alternatingStereoWarmupFramesRemaining=0;
        s.stereoStartupControllerMissing=false;
        if(!nativeBackend)KharvoxCameraSetStereoEye(0,0,0,0,0,false);
        log("Stereo capture INACTIVE");
    }
    stereoNow=stereoNow&&s.alternatingStereo;
    if(stereoNow&&!nativeBackend&&s.steamMetaCompatibilityMode&&steamLinkProjectionGameplay
        &&!nvidiaAfwMarker&&!steamLinkAerStereoLogged){
        log("[STEAMLINK-AER] stereoscopic alternating-eye PROJECTION ACTIVE; independent left/right camera offsets enabled");
        steamLinkAerStereoLogged=true;
    }
    if(stereoNow&&!nativePackedStereo&&s.stereoStartupControllerMissing
        &&s.rightController.valid&&s.leftController.valid){
        // A weapon change happens to invalidate the native Weapon HUD and was
        // masking this startup problem. Re-prime automatically at controller
        // recovery so both eye caches see one coherent HUD generation without
        // requiring a manual weapon selection.
        invalidateAlternatingStereoHistory(false);
        s.renderEye=kharvox::aerFirstRenderEye;
        s.alternatingStereoSkipInitialCapture=true;
        s.alternatingStereoWarmupActive=true;
        s.alternatingStereoWarmupEye=kharvox::aerFirstRenderEye;
        s.alternatingStereoWarmupFramesRemaining=0;
        s.stereoStartupControllerMissing=false;
        log("[HUD22-AER] controller pair recovered; pipeline-settled cache warmup re-armed");
    }
    const int scheduledRenderEye=s.renderEye;
    const char* coherentAerPairLogTag=steamRuntime
        ?"[STEAMXR-AER-PAIR]":s.runtimeKind==kharvox::OpenXRRuntimeKind::VirtualDesktop
            ?"[VD-AER-PAIR]":s.runtimeKind==kharvox::OpenXRRuntimeKind::MetaOculus
                ?"[META-AER-PAIR]":"[IMMERSIVE-AER-PAIR]";
    const bool steamXrAerPairPath=kharvox::useSteamXrCoherentAerPair(
        s.runtimeKind,coherentAerPairPresentationContext,stereoNow,
        nativePackedStereo,nvidiaAfwMarker,s.alternatingStereoWarmupActive,
        s.immersiveCinematicActive||s.nativeAdaptiveParticipantGuardActive);
    if(steamXrAerPairPath){
        if(!s.steamXrAerPairActive){
            s.steamXrAerPairActive=true;
            s.steamXrAerDisplayPeriod=frame.predictedDisplayPeriod;
            const double runtimeHz=frame.predictedDisplayPeriod>0
                ?1.0e9/double(frame.predictedDisplayPeriod):0.0;
            const double pairHz=kharvox::coherentAerFullPairHz(
                frame.predictedDisplayPeriod);
            std::ostringstream activation;
            activation<<coherentAerPairLogTag<<" coherent completed-pair mode ACTIVE; renderOrder=RIGHT-THEN-LEFT eyeMapping=UNCHANGED; runtimeHz="
                <<runtimeHz<<" fullPairHz="<<pairHz
                <<" predictedDisplayPeriodNs="<<frame.predictedDisplayPeriod
                <<" (sampled on mode entry; transient missed-frame multiples ignored)";
            log(activation.str());
        }
    }else if(s.steamXrAerPairActive){
        s.steamXrAerPairActive=false;
        s.steamXrAerDisplayPeriod=0;
        s.steamXrAerCaptureValid=false;
        s.steamXrAerCaptureEyesReady={};
        s.steamXrAerPairReady=false;
        log(std::string(coherentAerPairLogTag)
            +" coherent completed-pair mode released");
    }
    const bool steamXrAerPairEligible=steamXrAerPairPath
        &&s.steamXrAerCaptureValid;
    // Cinematics must bind completed producer eyes too. Scheduling the next eye
    // does not identify the image returned by the asynchronous engine.
    const bool aerSourceMode=kharvox::aerUseSourceQualification(steamXrAerPairPath,s.immersiveCinematicActive,
        s.nativeAdaptiveParticipantGuardActive,KharvoxCameraGameplayActive(),KharvoxCameraCutsceneActive());
    if(aerSourceMode!=s.aerSourceModeActive){
        s.aerSourceModeActive=aerSourceMode;
        s.aerSourceCacheValid={};s.aerSourceCacheKeys={};s.aerPublishedSourcePoseId=0;
        s.steamXrAerPairReady=false;s.integratedPairHandsValid=false;
        log(aerSourceMode?"[AER-SOURCE-PAIR] r262 ACTIVE; completed CPU producer sources bind cache eye/pose; waiting for a matching pair"
            :"[AER-SOURCE-PAIR] source-bound gameplay mode released");
    }
    AerCapturedInput aerCapturedInput{};
    const bool aerSourceQualified=aerSourceMode&&aerSourceObservation.valid()
        &&aerSourceObservation.key.level==KharvoxCameraLevelTransitionGeneration()
        &&s.aerInputHistory.find({aerSourceObservation.key.poseId,aerSourceObservation.key.level,aerSourceObservation.key.eye},aerCapturedInput);
    const int currentRenderEye=aerSourceQualified?aerSourceObservation.key.eye:scheduledRenderEye;
    const int staleEye=currentRenderEye^1;
    const bool sourcePairWillPublish=aerSourceQualified&&!s.alternatingStereoSkipInitialCapture
        &&kharvox::aerSourceCompletesPair(aerSourceObservation.key,s.aerSourceCacheValid[staleEye],
            s.aerSourceCacheKeys[staleEye],s.aerPublishedSourcePoseId);
    const bool captureViewValid=aerSourceMode?aerSourceQualified:s.programmedEyeViewValid[currentRenderEye];
    auto capturePose=aerSourceQualified?aerCapturedInput.pose:s.programmedEyePose[currentRenderEye];
    if(aerSourceQualified&&aerSourceObservation.key.domain==1){
        if(s.immersiveCinematicFreelook){
            const kharvox::AerSourceKey anchorKey{aerSourceObservation.key.poseId,aerSourceObservation.key.level,0,1};
            XrVector3f anchor{};
            if(!s.aerCinematicPairAnchors.find(anchorKey,anchor)){
                anchor=s.immersiveCinematicLayerPositionHeld?s.immersiveCinematicLayerPosition:aerCapturedInput.center.position;
                s.aerCinematicPairAnchors.remember(anchorKey,anchor);
            }
            capturePose.position.x+=anchor.x-aerCapturedInput.center.position.x;
            capturePose.position.y+=anchor.y-aerCapturedInput.center.position.y;
            capturePose.position.z+=anchor.z-aerCapturedInput.center.position.z;
        }else capturePose=poseRelativeTo(aerCapturedInput.center,capturePose);
    }
    const auto captureFov=aerSourceQualified?aerCapturedInput.fov:s.programmedEyeFov[currentRenderEye];
    const auto captureTurn=aerSourceQualified?aerCapturedInput.artificialTurn:s.programmedEyeArtificialTurn[currentRenderEye];
    const auto captureSnap=aerSourceQualified?aerCapturedInput.snapGeneration:s.programmedEyeSnapGeneration[currentRenderEye];
    if(aerSourceMode){
        kharvox::pose_trace::Event e{};e.kind=kharvox::pose_trace::AerSourceWindow;e.frame=s.frame;
        e.poseId=aerSourceObservation.key.poseId;e.eye=aerSourceObservation.key.eye;
        e.flags=aerSourceObservation.key.domain;e.revision=aerSourceObservation.count;e.status=aerSourceObservation.ambiguous?2:aerSourceObservation.valid()?1:0;
        kharvox::pose_trace::record(e);
        e.kind=kharvox::pose_trace::AerSourceResolved;e.status=aerSourceQualified?1:0;
        e.source=reinterpret_cast<uintptr_t>(it->second.images[srcIndex]);
        e.revision=poseTraceProgrammedIds[scheduledRenderEye];e.flags=unsigned(scheduledRenderEye);
        kharvox::pose_trace::record(e);
        static std::array<std::array<uint64_t,3>,3> counts{};
        const auto state=aerSourceQualified?0:aerSourceObservation.ambiguous?2:1;
        const auto domain=std::min(aerSourceObservation.key.domain,2u);
        const auto n=++counts[domain][state];
        if(kharvox::extendedDiagnosticsEnabled()&&(n<=4||n%1024==0))
            log("[AER-SOURCE-PAIR] qualified="+std::to_string(aerSourceQualified)+" ambiguous="+std::to_string(aerSourceObservation.ambiguous)
                +" sourceEye="+std::to_string(aerSourceObservation.key.eye)+" sourcePose="+std::to_string(aerSourceObservation.key.poseId)
                +" domain="+std::to_string(aerSourceObservation.key.domain)
                +" scheduledEye="+std::to_string(scheduledRenderEye)+" publish="+std::to_string(sourcePairWillPublish)+" count="+std::to_string(n));
    }
    // Retain the previous application-yaw bypass; runtime timewarp still uses
    // the stored rendering pose, now selected by the observed source.
    const bool bypassApplicationTurnCompensation=aerSourceMode||(!nativeBackend&&stereoNow&&!s.quadMode
        &&!s.immersiveCinematicActive&&KharvoxCameraGameplayActive()
        &&!KharvoxCameraCutsceneActive());
    static bool bypassLogged=false;
    if(bypassApplicationTurnCompensation&&!bypassLogged){
        log("[CACHED-POSES-NO-TURN] r262 diagnostic ACTIVE; cached render poses restored, application turn compensation bypassed; runtime timewarp unchanged");
        bypassLogged=true;
    }
    // Retain the published XR images until a complete new pair is available.
    const bool steamXrAerReusePublishedPair=aerSourceMode
        ?s.steamXrAerPairReady&&!sourcePairWillPublish
        :steamXrAerPairEligible&&s.steamXrAerPairReady&&currentRenderEye==kharvox::aerFirstRenderEye;
    // r262: restore source-integrated hands and scene-depth occlusion.
    const bool freshHandsRequested=false&&kharvox::freshAerHandsEnabled(steamXrAerPairPath,
        nativeBackend,s.showHands,KharvoxCameraGameplayActive(),
        KharvoxCameraCutsceneActive(),s.immersiveCinematicActive);
    const bool freshAerHands=freshHandsRequested&&ensureFreshHandsWorldCache(eyeSourceExtent);
    static int lastFreshHandsState=-1;
    const int freshHandsState=freshAerHands?1:freshHandsRequested?2:0;
    if(lastFreshHandsState!=freshHandsState){
        log(freshAerHands?"[FRESH-AER-HANDS] ACTIVE; fresh hands every XR output, world and weapon remain paired"
            :freshHandsRequested?"[FRESH-AER-HANDS] UNAVAILABLE; clean cache allocation failed, baseline retained"
            :"[DEPTH-HANDS] r262 fresh overlay disabled; scene-integrated depth-tested hands requested");
        lastFreshHandsState=freshHandsState;
    }
    if(!freshAerHands)s.freshHandsWorldValid=false;
    const bool updateEyeSwapchains=aerSourceMode?sourcePairWillPublish:kharvox::freshAerHandsUpdateTargets(
        steamXrAerReusePublishedPair,freshAerHands,s.freshHandsWorldValid);
    const bool steamLinkAdaptivePairEligible=s.steamMetaCompatibilityMode&&nvidiaAfwMarker&&!s.quadMode&&!s.centeredQuadTransitionPending&&KharvoxCameraWorldActive()&&!s.immersiveCinematicActive;
    const bool steamXrCommonSourcePose=steamRuntime&&nvidiaAfwMarker&&!s.quadMode&&!s.centeredQuadTransitionPending&&KharvoxCameraWorldActive()&&!s.immersiveCinematicActive;
    float steamLinkHeadAngularVelocity=0.0f;
    bool steamLinkMotionSampleReady=false;
    if((vs.viewStateFlags&XR_VIEW_STATE_ORIENTATION_VALID_BIT)!=0){
        constexpr uint32_t historySize=4;
        const auto currentOrientation=s.views[0].pose.orientation;
        if(s.steamLinkHeadHistoryCount==historySize){
            const auto oldestOrientation=s.steamLinkHeadOrientationHistory[s.steamLinkHeadHistoryIndex];
            const XrTime oldestTime=s.steamLinkHeadOrientationTimes[s.steamLinkHeadHistoryIndex];
            double windowSeconds=double(frame.predictedDisplayTime-oldestTime)*1.0e-9;
            if(windowSeconds<0.015||windowSeconds>0.25){
                const double periodSeconds=frame.predictedDisplayPeriod>0
                    ?std::clamp(double(frame.predictedDisplayPeriod)*1.0e-9,0.005,0.05)
                    :(1.0/90.0);
                windowSeconds=periodSeconds*double(historySize);
            }
            steamLinkHeadAngularVelocity=quaternionAngularDistanceDegrees(oldestOrientation,currentOrientation)/float(windowSeconds);
            steamLinkMotionSampleReady=true;
        }else ++s.steamLinkHeadHistoryCount;
        s.steamLinkHeadOrientationHistory[s.steamLinkHeadHistoryIndex]=currentOrientation;
        s.steamLinkHeadOrientationTimes[s.steamLinkHeadHistoryIndex]=frame.predictedDisplayTime;
        s.steamLinkHeadHistoryIndex=(s.steamLinkHeadHistoryIndex+1)%historySize;
    }else{
        s.steamLinkHeadHistoryIndex=0;s.steamLinkHeadHistoryCount=0;
        s.steamLinkMotionEnterSamples=0;s.steamLinkMotionExitSamples=0;
        s.steamLinkMotionFixedPairActive=false;
    }
    if(steamLinkAdaptivePairEligible&&steamLinkMotionSampleReady){
        constexpr float enterVelocityDegreesPerSecond=8.0f;
        constexpr float exitVelocityDegreesPerSecond=5.0f;
        constexpr uint32_t enterConfirmationSamples=2;
        constexpr uint32_t exitConfirmationSamples=6;
        const uint64_t filterSample=++s.steamLinkMotionFilterSamples;
        if(s.steamLinkMotionFixedPairActive){
            s.steamLinkMotionEnterSamples=0;
            s.steamLinkMotionExitSamples=steamLinkHeadAngularVelocity<=exitVelocityDegreesPerSecond?s.steamLinkMotionExitSamples+1:0;
            if(s.steamLinkMotionExitSamples>=exitConfirmationSamples){
                s.steamLinkMotionFixedPairActive=false;
                s.steamLinkMotionExitSamples=0;
                std::ostringstream transition;
                transition<<"[STEAMLINK-ADAPT] sustained head stability -> per-frame scene mode windowAngularVelocityDegPerSec="<<steamLinkHeadAngularVelocity;
                log(transition.str());
            }
        }else{
            s.steamLinkMotionExitSamples=0;
            s.steamLinkMotionEnterSamples=steamLinkHeadAngularVelocity>=enterVelocityDegreesPerSecond?s.steamLinkMotionEnterSamples+1:0;
            if(s.steamLinkMotionEnterSamples>=enterConfirmationSamples){
                s.steamLinkMotionFixedPairActive=true;
                s.steamLinkMotionEnterSamples=0;
                std::ostringstream transition;
                transition<<"[STEAMLINK-ADAPT] sustained physical head motion -> fixed-pair scene mode windowAngularVelocityDegPerSec="<<steamLinkHeadAngularVelocity;
                log(transition.str());
            }
        }
        if(filterSample<=4||filterSample%240==0){
            std::ostringstream sample;
            sample<<"[STEAMLINK-FILTER] sample="<<filterSample<<" windowAngularVelocityDegPerSec="<<steamLinkHeadAngularVelocity
                  <<" mode="<<(s.steamLinkMotionFixedPairActive?"fixed-pair":"per-frame")
                  <<" enterProgress="<<s.steamLinkMotionEnterSamples<<'/'<<enterConfirmationSamples
                  <<" exitProgress="<<s.steamLinkMotionExitSamples<<'/'<<exitConfirmationSamples;
            log(sample.str());
        }
    }
    if(!steamLinkAdaptivePairEligible){
        s.steamLinkMotionEnterSamples=0;s.steamLinkMotionExitSamples=0;s.steamLinkMotionFixedPairActive=false;
    }
    const bool steamLinkFixedPairMode=steamLinkAdaptivePairEligible&&s.steamLinkMotionFixedPairActive;
    const bool steamLinkPrepareWarp=!steamLinkFixedPairMode||currentRenderEye==kharvox::aerSecondRenderEye||!s.steamLinkFixedPairValid;
    if(!steamLinkFixedPairMode){s.steamLinkFixedPairValid=false;s.steamLinkFixedPairImages={};}
    const bool nvidiaAfwRequested=nvidiaAfwMarker;

    const VkExtent2D maximumEyeExtent{
        std::max(s.eyes[0].width,s.eyes[1].width),
        std::max(s.eyes[0].height,s.eyes[1].height)};
    if(stereoNow&&(!nativePackedStereo||nativeBackend)&&fsr1Requested&&!s.fsr1InitializationAttempted){
        s.fsr1InitializationAttempted=true;
        s.fsr1.initialize(s.physical,s.device,s.vk,static_cast<VkFormat>(s.format),
            eyeSourceExtent,maximumEyeExtent);
    }
    static bool steamLinkAfwReadyLogged=false;
    static bool steamLinkAfwFallbackLogged=false;
    static bool steamXrCommonSourcePoseLogged=false;


    // The first loading/cinematic Quad needs the same SRGB interpretation as
    // later Quads. Do not depend on a previous stereo frame having filled the
    // cache: the mono transfer below initializes it before it is consumed.
    const bool initialMonoColorCache=!stereoNow&&(s.quadMode||s.immersiveCinematicActive)
        &&ensureStereoCache(it->second.extent);
    const bool steamLinkMonoColorCache=steamLinkSameFrameMono&&ensureStereoCache(eyeSourceExtent);
    static bool steamLinkMonoCacheFailureLogged=false;
    if(steamLinkSameFrameMono&&!steamLinkMonoColorCache&&!steamLinkMonoCacheFailureLogged){log("[STEAMLINK-MONO] SRGB intermediate cache allocation failed; color-correct test invalid");steamLinkMonoCacheFailureLogged=true;}
    VkSemaphore afwWaitSemaphore{};uint64_t afwWaitValue{};const bool afwPrepared=false;
    std::array<uint32_t,2> xi{};
    bool imageReleaseFailed{};
    auto releaseImageChecked=[&](XrSwapchain swapchain,kharvox::SwapchainImageState& imageState){
        if(!imageState.releasable())return false;
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        const XrResult released=releaseSwapchainImage(swapchain,&ri);
        if(XR_FAILED(released)){
            imageReleaseFailed=true;
            requestSessionRestart("XR image release failed "+result(released));
            return false;
        }
        imageState.released();
        return true;
    };
    auto releaseAcquiredEyeImages=[&](){
        bool released=true;
        for(int e=0;e<2;e++)if(s.eyeImageStates[e].owned())
            released=releaseImageChecked(s.eyes[e].handle,s.eyeImageStates[e])&&released;
        return released;
    };
    if(updateEyeSwapchains){
        for(int e=0;e<2;e++){
            if(s.eyeImageStates[e].owned()){
                requestSessionRestart("eye image ownership retained after earlier failure");
                endEmptyFrame("eye-image-still-owned");return;
            }
            XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            r=acquireSwapchainImage(s.eyes[e].handle,&ai,&xi[e]);
            if(XR_FAILED(r)){
                releaseAcquiredEyeImages();
                log("acquire eye "+result(r));
                endEmptyFrame("acquire-eye-"+std::to_string(e)+"-failed");return;
            }
            s.eyeImageStates[e].acquired();
            XrSwapchainImageWaitInfo xw{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            xw.timeout=XR_INFINITE_DURATION;
            r=s.waitImage(s.eyes[e].handle,&xw);
            if(!s.eyeImageStates[e].waited(r!=XR_TIMEOUT_EXPIRED&&XR_SUCCEEDED(r))){
                requestSessionRestart("wait eye "+result(r));
                releaseAcquiredEyeImages();
                endEmptyFrame("wait-eye-"+std::to_string(e)+"-failed");return;
            }
        }
    }
    if(hudQuadRequested){
        if(s.hudImageState.owned()){
            requestSessionRestart("HUD image ownership retained after earlier failure");
            releaseAcquiredEyeImages();
            endEmptyFrame("hud-image-still-owned");return;
        }
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        r=acquireSwapchainImage(s.hudQuad.handle,&acquire,&hudImageIndex);
        if(XR_SUCCEEDED(r)){
            s.hudImageState.acquired();
            if(hudImageIndex>=s.hudQuad.images.size())r=XR_ERROR_RUNTIME_FAILURE;
        }
        if(XR_SUCCEEDED(r)){
            XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wait.timeout=XR_INFINITE_DURATION;
            r=s.waitImage(s.hudQuad.handle,&wait);
            s.hudImageState.waited(r!=XR_TIMEOUT_EXPIRED&&XR_SUCCEEDED(r));
        }
        if(!s.hudImageState.releasable()&&s.hudImageState.owned())requestSessionRestart("HUD image acquire/wait failed "+result(r));
        if(!s.hudImageState.releasable()&&!s.hudQuadRuntimeFailureLogged){
            log("[HUD9-QUAD] acquire/wait failed "+result(r)+"; native HUD fallback retained");
            s.hudQuadRuntimeFailureLogged=true;
        }
        if(!s.running){releaseAcquiredEyeImages();endEmptyFrame("hud-image-failed");return;}
    }

    const bool monoColorCache=steamLinkMonoColorCache||initialMonoColorCache;
    std::array<XrRect2Di,2> submittedRects{};
    if(steamXrAerReusePublishedPair)
        submittedRects=s.steamXrAerSubmittedRects;
    const bool handGameplayActive=(!nativeBackend||(nativeFrameValid&&(nativeFrame.pose.gameplay
        ||(nativeFrame.pose.scripted&&nativeFrame.pose.weaponControlActive))))
        &&!s.quadMode&&KharvoxCameraGameplayActive()
        &&KharvoxCameraWorldActive()&&!KharvoxCameraCutsceneActive()
        &&!s.immersiveCinematicActive;
    // Berserk is intentionally false until a reliable native state probe is
    // found. The pure policy and renderer are already prepared for it. Every
    // authored camera sequence is excluded explicitly: Glory Kills and ledge
    // assists can still report both gameplay and world-camera active.
    const auto handVisibility=kharvox::hands::selectHandVisibility({
        nativeFrameValid?nativeFrame.pose.leftHanded:s.leftHanded,
        nativeFrameValid?nativeFrame.pose.twoHanded:s.twoHandLatched,false,s.handRenderer.availability(),
        s.showHands,handGameplayActive});
    auto handPose=[](const ControllerPose&controller){
        kharvox::hands::HandPose pose{};
        pose.position[0]=controller.position.x;pose.position[1]=controller.position.y;
        pose.position[2]=controller.position.z;pose.orientation[0]=controller.orientation.x;
        pose.orientation[1]=controller.orientation.y;pose.orientation[2]=controller.orientation.z;
        pose.orientation[3]=controller.orientation.w;pose.valid=controller.valid;return pose;
    };
    auto nativeLeftController=s.leftGripController,nativeRightController=s.rightGripController;
    if(nativeFrameValid){nativeLeftController.valid=nativeFrame.pose.controllersValid[0];nativeRightController.valid=nativeFrame.pose.controllersValid[1];nativeLeftController.orientation=nativeFrame.pose.controllers[0].orientation;nativeLeftController.position=nativeFrame.pose.controllers[0].position;
        nativeRightController.orientation=nativeFrame.pose.controllers[1].orientation;nativeRightController.position=nativeFrame.pose.controllers[1].position;}
    const auto currentLeftHandPose=handPose(nativeLeftController);
    const auto currentRightHandPose=handPose(nativeRightController);
    const bool alignAerSourceHands=!nativeBackend&&s.alternatingStereo
        &&!nativePackedStereo&&handGameplayActive;
    const auto sourceLeftHandPose=alignAerSourceHands
        ?handPose(aerSourceQualified?aerCapturedInput.left:sourceLeftGripController):currentLeftHandPose;
    const auto sourceRightHandPose=alignAerSourceHands
        ?handPose(aerSourceQualified?aerCapturedInput.right:sourceRightGripController):currentRightHandPose;
    const kharvox::hands::HandGameplayState currentHandGameplay{
        nativeFrameValid?static_cast<KharvoxWeaponKind>(nativeFrame.pose.weaponKind):KharvoxWeaponCurrentKind(),
        nativeFrameValid?nativeFrame.pose.leftHanded:s.leftHanded};
    // AER completes a stereo pair over two native frames. Hold the same
    // controller sample for both eyes so scene integration cannot reintroduce
    // near-field stereo shimmer/double vision.
    if((aerSourceMode?aerSourceQualified&&s.integratedPairSourcePoseId!=aerSourceObservation.key.poseId
        :nativeBackend||currentRenderEye==kharvox::aerFirstRenderEye)||!s.integratedPairHandsValid){
        s.integratedPairSourcePoseId=aerSourceQualified?aerSourceObservation.key.poseId:0;
        s.integratedPairLeftHand=sourceLeftHandPose;
        s.integratedPairRightHand=sourceRightHandPose;
        s.integratedPairHandVisibility=handVisibility;
        s.integratedPairHandGameplay=currentHandGameplay;
        s.integratedPairHandsValid=true;
    }
    const auto&integratedLeftHandPose=s.integratedPairLeftHand;
    const auto&integratedRightHandPose=s.integratedPairRightHand;
    const auto&integratedHandVisibility=s.integratedPairHandVisibility;
    const auto&integratedHandGameplay=s.integratedPairHandGameplay;
    const bool skipInitialAlternatingCapture=stereoNow&&!nativePackedStereo
        &&s.alternatingStereoSkipInitialCapture;
    const bool settlingAlternatingPipeline=stereoNow&&!nativePackedStereo
        &&s.alternatingStereoWarmupActive
        &&s.alternatingStereoWarmupFramesRemaining>1;
    const bool skipAlternatingCapture=skipInitialAlternatingCapture
        ||settlingAlternatingPipeline||(aerSourceMode&&!aerSourceQualified);
    auto abortCommandRecording=[&](const char*operation,VkResult failure){
        s.fsr1.discardRecordedFrame();
        if(freshAerHands)s.freshHandsWorldValid=false;
        for(int e=0;e<2;++e)if(s.eyeImageStates[e].owned()&&xi[e]<s.eyes[e].initialized.size())
            s.eyes[e].initialized[xi[e]]=false;
        if(s.hudImageState.owned()&&hudImageIndex<s.hudQuad.initialized.size())
            s.hudQuad.initialized[hudImageIndex]=false;
        invalidateAlternatingStereoHistory(true);
        s.handRenderer.finishSceneIntegratedFrame();
        releaseAcquiredEyeImages();
        if(s.hudImageState.owned())releaseImageChecked(s.hudQuad.handle,s.hudImageState);
        log(std::string(operation)+" failed "+std::to_string(failure));
        endEmptyFrame(std::string(operation)+"-failed");
    };
    kharvox::CommandRecordingState commandRecording;
    const VkResult resetResult=s.vk.resetCommandBuffer(s.commandBuffer,0);
    if(!commandRecording.reset(resetResult==VK_SUCCESS)){abortCommandRecording("reset-command-buffer",resetResult);return;}
    VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    const VkResult beginCommandResult=s.vk.beginCommandBuffer(s.commandBuffer,&cbi);
    if(!commandRecording.begun(beginCommandResult==VK_SUCCESS)){abortCommandRecording("begin-command-buffer",beginCommandResult);return;}
    if(sfsBackend){
        if(!s.sfsCopyTiming.pool){
            VkPhysicalDeviceProperties properties{};s.vk.getPhysicalDeviceProperties(s.physical,&properties);
            uint32_t count{};s.vk.getPhysicalDeviceQueueFamilyProperties(s.physical,&count,nullptr);
            std::vector<VkQueueFamilyProperties> families(count);s.vk.getPhysicalDeviceQueueFamilyProperties(s.physical,&count,families.data());
            if(s.queueFamily<count)s.sfsCopyTiming.initialize(s.device,s.vk.getDeviceProcAddr,properties.limits.timestampPeriod,families[s.queueFamily].timestampValidBits);
        }
        s.sfsCopyTiming.begin(s.commandBuffer);
    }
    const auto nativeOwnerGpuSpan=nativeFrameValid&&!sfsBackend?kharvox::native::beginOwnerGpuTiming(s.commandBuffer):UINT32_MAX;
    VkImage src=it->second.images[srcIndex];
    pollEyeCapture();
    std::array<bool,2> rawEyeCaptureRecorded{};
    if(stereoNow&&!s.quadMode&&!nativePackedStereo&&!skipAlternatingCapture)
        rawEyeCaptureRecorded[currentRenderEye]=recordEyeSource(currentRenderEye,src,
            kharvox::sfs::sourceLayout(s.device,src,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR),it->second.extent,it->second.format,
            s.stereoCacheRevision[currentRenderEye]+1);
    kharvox::hands::HandPose sceneLaser{};
    XrVector3f laserOrigin{},laserDirection{};
    if(s.laserSightEnabled&&handGameplayActive&&laserWeaponAllowed(KharvoxWeaponCurrentKind())
        &&(!aerSourceMode||aerSourceQualified)
        &&renderedLaserPoseInTrackingSpace(laserOrigin,laserDirection,
            aerSourceQualified?aerCapturedInput.laserBodyTracking:sfsBackend&&nativeFrameValid?nativeFrame.pose.bodyTracking:laserBodyTrackingTransform(),
            aerSourceQualified?aerSourceObservation.key.poseId:sfsBackend&&nativeFrameValid?nativeFrame.pose.source.poseId:0,
            aerSourceQualified?aerSourceObservation.key.eye:sfsBackend&&nativeFrameValid?0:-1)){
        const auto up=std::fabs(laserDirection.y)>.9f?XrVector3f{1,0,0}:XrVector3f{0,1,0};
        const auto rotation=quaternionFromForwardUp(laserDirection,up);
        sceneLaser={{laserOrigin.x,laserOrigin.y,laserOrigin.z},
            {rotation.x,rotation.y,rotation.z,rotation.w},true};
    }
    bool sourceHasIntegratedHands=false;
    auto integrateSceneHands=[&](VkImage source,VkImageLayout oldLayout,const XrPosef& sourcePose,const XrFovf& sourceFov,uint32_t sourceLayer=0){
        kharvox::hands::HandSceneTarget handSceneTarget{};
        kharvox::hands::HandSceneTarget sourceTarget{};
        const bool nativeRight=!sfsBackend&&nativeBackend&&nativeFrameValid&&source==nativeFrame.eyes[1].image;
        const bool available=gameImages.select([&]{
            if(!kharvox::hands::handSceneTargetForColor(nativeRight?nativeFrame.eyes[0].image:source,
                it->second.extent,sourceTarget))return false;
            handSceneTarget=sourceTarget;
            if(sfsBackend){
                handSceneTarget.copyDepthForHands=true;
                handSceneTarget.depthArrayLayer=sourceLayer;
                return kharvox::sfs::eyeAttachmentView(s.device,sourceTarget.colorView,sourceLayer,handSceneTarget.colorView)
                    &&kharvox::sfs::eyeAttachmentView(s.device,sourceTarget.depthView,sourceLayer,handSceneTarget.depthView);
            }
            return !nativeRight||kharvox::native::handSceneTarget(nativeFrame,sourceTarget,handSceneTarget);
        },[&]{return std::array{
                kharvox::GameImageLifetime::key(sourceTarget.colorImage),
                kharvox::GameImageLifetime::key(sourceTarget.depthImage),
                kharvox::GameImageLifetime::key(sourceTarget.colorView),
                kharvox::GameImageLifetime::key(sourceTarget.depthView),
                kharvox::GameImageLifetime::key(handSceneTarget.colorImage),
                kharvox::GameImageLifetime::key(handSceneTarget.depthImage),
                kharvox::GameImageLifetime::key(handSceneTarget.colorView),
                kharvox::GameImageLifetime::key(handSceneTarget.depthView)};})
            &&handSceneTarget.colorFormat==it->second.format;
        if(!available){
            static bool laserDepthMissingLogged=false;
            if(sceneLaser.valid&&!laserDepthMissingLogged){
                log("[LASER] scene depth unavailable; beam withheld (no overlay fallback)");
                laserDepthMissingLogged=true;
            }
            barrierAspect(s.commandBuffer,source,oldLayout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_IMAGE_ASPECT_COLOR_BIT,sourceLayer);
            return false;
        }
        barrierAspect(s.commandBuffer,source,oldLayout,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_IMAGE_ASPECT_COLOR_BIT,sourceLayer);
        const VkImageAspectFlags depthAspect=
            kharvox::hands::handSceneDepthAspect(handSceneTarget.depthFormat);
        barrierAspect(s.commandBuffer,handSceneTarget.depthImage,
            handSceneTarget.depthLayout,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            depthAspect,handSceneTarget.depthArrayLayer);
        kharvox::hands::HandEyeView sourceHandView{};
        sourceHandView.pose.position[0]=sourcePose.position.x;
        sourceHandView.pose.position[1]=sourcePose.position.y;
        sourceHandView.pose.position[2]=sourcePose.position.z;
        sourceHandView.pose.orientation[0]=sourcePose.orientation.x;
        sourceHandView.pose.orientation[1]=sourcePose.orientation.y;
        sourceHandView.pose.orientation[2]=sourcePose.orientation.z;
        sourceHandView.pose.orientation[3]=sourcePose.orientation.w;
        sourceHandView.pose.valid=true;sourceHandView.angleLeft=sourceFov.angleLeft;
        sourceHandView.angleRight=sourceFov.angleRight;
        sourceHandView.angleUp=sourceFov.angleUp;
        sourceHandView.angleDown=sourceFov.angleDown;
        sourceHandView.imageRectWidth=it->second.extent.width;
        sourceHandView.imageRectHeight=it->second.extent.height;
        const auto depthRange=kharvox::hands::doomSceneHandDepthRange(
            s.worldScale);
        sourceHandView.nearZ=depthRange.nearMeters;
        sourceHandView.farZ=depthRange.farMeters;
        const bool integrated=s.handRenderer.recordSceneIntegrated(
            s.commandBuffer,handSceneTarget,sourceHandView,
            integratedLeftHandPose,integratedRightHandPose,
            freshAerHands?kharvox::hands::HandVisibilityOutput{}:integratedHandVisibility,integratedHandGameplay,sceneLaser);
        static bool laserSceneLogged=false;
        if(integrated&&sceneLaser.valid&&!laserSceneLogged){
            log("[LASER] scene-depth beam recorded with completed weapon source; compositor ribbons removed");
            laserSceneLogged=true;
        }
        barrierAspect(s.commandBuffer,handSceneTarget.depthImage,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            handSceneTarget.depthLayout,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,
            depthAspect,handSceneTarget.depthArrayLayer);
        barrierAspect(s.commandBuffer,source,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_IMAGE_ASPECT_COLOR_BIT,sourceLayer);
        return integrated&&!freshAerHands;
    };
    const bool sceneIntegrationEligible=(!freshAerHands||sceneLaser.valid)&&handGameplayActive&&stereoNow
        &&!nativePackedStereo&&!skipAlternatingCapture
        &&captureViewValid;
    if(sceneIntegrationEligible){
        sourceHasIntegratedHands=integrateSceneHands(src,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            capturePose,eyeRenderProjection(captureFov).symmetricFov);
    }else{
        barrier(s.commandBuffer,src,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    }
    std::array<bool,2> integratedNativeHands{};
    std::array<VkImage,2> copySources{src,src};
    if(stereoNow&&!skipAlternatingCapture){
        const int first=nativePackedStereo?0:currentRenderEye;
        const int last=nativePackedStereo?1:currentRenderEye;
        for(int e=first;e<=last;e++){
            if(nativeFrame.rightEyeBlackDiagnostic&&e==1)continue;
            VkImage cache=s.stereoCache[e];
            barrier(s.commandBuffer,cache,s.stereoCacheInitialized[e]?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,s.stereoCacheInitialized[e]?VK_ACCESS_TRANSFER_READ_BIT:0,VK_ACCESS_TRANSFER_WRITE_BIT);
            VkImageCopy cacheCopy{};
            cacheCopy.srcSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
            cacheCopy.srcSubresource.layerCount=1;
            if(sfsBackend)cacheCopy.srcSubresource.baseArrayLayer=e;
            cacheCopy.srcOffset={nativeTwoViewPacked?int32_t(e*eyeSourceExtent.width):0,0,0};
            cacheCopy.dstSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
            cacheCopy.dstSubresource.layerCount=1;
            cacheCopy.extent={eyeSourceExtent.width,eyeSourceExtent.height,1};
            VkImage eyeSource=src;
            if(nativeBackend&&nativeFrameValid){
                eyeSource=nativeFrame.eyes[e].image;
                if(!s.quadMode)rawEyeCaptureRecorded[e]=recordEyeSource(e,eyeSource,
                    e==0?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:nativeFrame.eyes[e].layout,
                    eyeSourceExtent,nativeFrame.eyes[e].format,s.stereoCacheRevision[e]+1,sfsBackend?uint32_t(e):0);
                if(handGameplayActive&&(s.showHands||sceneLaser.valid)){
                    integratedNativeHands[e]=integrateSceneHands(eyeSource,
                        e==0?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:nativeFrame.eyes[e].layout,
                        nativeFrame.eyes[e].pose,nativeFrame.eyes[e].fov,sfsBackend?uint32_t(e):0);
                }else if(e==1)barrierAspect(s.commandBuffer,eyeSource,nativeFrame.eyes[e].layout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_IMAGE_ASPECT_COLOR_BIT,sfsBackend?1:0);
            }
            s.vk.cmdCopyImage(s.commandBuffer,eyeSource,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,cache,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&cacheCopy);
            if(nativeBackend&&nativeFrameValid&&e==1)barrierAspect(s.commandBuffer,eyeSource,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,nativeFrame.eyes[e].layout,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_IMAGE_ASPECT_COLOR_BIT,sfsBackend?1:0);
            barrier(s.commandBuffer,cache,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
            s.stereoCacheInitialized[e]=true;
            s.stereoCacheHasIntegratedHands[e]=nativeBackend?integratedNativeHands[e]:sourceHasIntegratedHands;
            if(nativeBackend&&handGameplayActive&&s.showHands){
                static std::array<int,2> lastIntegration{-1,-1};
                if(lastIntegration[e]!=int(integratedNativeHands[e])){
                    lastIntegration[e]=int(integratedNativeHands[e]);
                    log("[NATIVE-HANDS] eye="+std::to_string(e)+" sceneDepth="+std::to_string(integratedNativeHands[e])
                        +" source="+std::to_string(reinterpret_cast<uintptr_t>(eyeSource))
                        +" frame="+std::to_string(nativeFrame.pose.serial)
                        +(integratedNativeHands[e]?"":" "+kharvox::hands::handSceneTargetDiagnostic(eyeSource,it->second.extent)));
                }
            }
            ++s.stereoCacheRevision[e];
            poseTraceCachedIds[e]=aerSourceQualified?aerSourceObservation.key.poseId:poseTraceProgrammedIds[e];
            if(!nativePackedStereo&&captureViewValid)traceEye(kharvox::pose_trace::CacheRecorded,e,capturePose,eyeRenderProjection(captureFov).symmetricFov,frame.predictedDisplayTime,reinterpret_cast<uintptr_t>(src),s.stereoCacheRevision[e],0,poseTraceCachedIds[e]);
            if(aerSourceQualified){s.aerSourceCacheKeys[e]=aerSourceObservation.key;s.aerSourceCacheValid[e]=true;}
            if(!nativePackedStereo&&captureViewValid){
                if(s.cachedEyeViewValid[e]){
                    s.previousCachedEyePose[e]=s.cachedEyePose[e];
                    s.previousCachedEyeFov[e]=s.cachedEyeFov[e];
                    s.previousCachedEyeViewValid[e]=true;
                }
                s.cachedEyePose[e]=capturePose;
                s.cachedEyeFov[e]=captureFov;
                s.cachedEyeViewValid[e]=true;
                s.cachedEyeArtificialTurn[e]=captureTurn;
                s.cachedEyeSnapGeneration[e]=captureSnap;
                if(steamXrAerPairEligible)
                    s.steamXrAerCaptureEyesReady[e]=true;
            }
        }
        for(int e=0;e<2;e++)
            copySources[e]=s.stereoCacheInitialized[e]?s.stereoCache[e]:s.stereoCache[first];
    }
    if(kharvox::snapTurnStereoTransitionComplete(
            s.snapTurnStereoTransitionActive,s.snapTurnGeneration,
            s.cachedEyeViewValid[0],s.cachedEyeSnapGeneration[0],
            s.cachedEyeViewValid[1],s.cachedEyeSnapGeneration[1])){
        s.snapTurnStereoTransitionActive=false;
        log("[TURN] AER snap transition complete generation="
            +std::to_string(s.snapTurnGeneration));
    }
    bool steamLinkFullMotionWarp=false;
    bool steamLinkUseFixedPair=false;

    if(steamLinkFixedPairMode&&s.steamLinkFixedPairValid){
        copySources=s.steamLinkFixedPairImages;
        steamLinkFullMotionWarp=true;
        steamLinkUseFixedPair=true;
        const uint64_t submissions=++s.steamLinkFixedPairSubmissions;
        if(submissions<=4||submissions%120==0)log("[STEAMLINK-PAIR] motion-gated coherent pair submitted; synthetic eye remains left count="+std::to_string(submissions));
    }
    if(!stereoNow&&monoColorCache){VkImage cache=s.stereoCache[0];barrier(s.commandBuffer,cache,s.stereoCacheInitialized[0]?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,s.stereoCacheInitialized[0]?VK_ACCESS_TRANSFER_READ_BIT:0,VK_ACCESS_TRANSFER_WRITE_BIT);VkImageCopy cacheCopy{};cacheCopy.srcSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;cacheCopy.srcSubresource.layerCount=1;cacheCopy.dstSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;cacheCopy.dstSubresource.layerCount=1;cacheCopy.extent={it->second.extent.width,it->second.extent.height,1};s.vk.cmdCopyImage(s.commandBuffer,src,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,cache,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&cacheCopy);barrier(s.commandBuffer,cache,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);s.stereoCacheInitialized[0]=true;copySources={cache,cache};}
    static const bool nativeXRResolution=GetFileAttributesW(kharvox::runtimePath(L"enable_native_xr_resolution").c_str())!=INVALID_FILE_ATTRIBUTES;
    static bool nativeXRResolutionLogged=false;
    // SFS renders a centered enclosing projection. Its asymmetric eye crop
    // must always fill the submitted surface, independently of legacy flags.
    const bool fillNativeEye=(nativeXRResolution||sfsBackend)&&!s.quadMode;
    if(fillNativeEye&&!nativeXRResolutionLogged){log("Native XR projection surface ACTIVE");nativeXRResolutionLogged=true;}
    // The overlay fallback must obey the same source-time pair contract as
    // scene-integrated hands, even if the source depth target is unavailable.
    const auto&leftHandPose=alignAerSourceHands&&!freshAerHands?integratedLeftHandPose:currentLeftHandPose;
    const auto&rightHandPose=alignAerSourceHands&&!freshAerHands?integratedRightHandPose:currentRightHandPose;

    // The hand is composited after the captured DOOM image, so its camera must
    // match the projection view that OpenXR will actually receive.  AER may
    // submit a cached/completed eye pair rather than this frame's raw located
    // views.  Using the raw views here makes a near controller object diverge
    // strongly between the two compositor images (visible as double vision).
    std::array<XrPosef,2> handSubmittedPose{};
    std::array<XrFovf,2> handSubmittedFov{};
    const bool handPairWillPublish=aerSourceMode?sourcePairWillPublish:steamXrAerPairEligible&&updateEyeSwapchains
        &&currentRenderEye==kharvox::aerSecondRenderEye&&s.steamXrAerCaptureEyesReady[0]
        &&s.steamXrAerCaptureEyesReady[1]&&s.cachedEyeViewValid[0]
        &&s.cachedEyeViewValid[1];
    const bool handUsesCompletedPair=steamXrAerPairEligible
        &&(s.steamXrAerPairReady||handPairWillPublish);
    for(int e=0;e<2;e++){
        const bool useSteamLinkExactMono=steamLinkSameFrameMono
            &&sourceMonoEyeViewsValid;
        const bool useSteamLinkSynchronizedViews=steamLinkFullMotionWarp
            &&sourceMonoEyeViewsValid;
        const bool useSteamLinkReprojectedView=steamLinkFullMotionWarp
            &&e==staleEye;
        const bool useCachedView=!useSteamLinkSynchronizedViews
            &&!useSteamLinkReprojectedView&&cachedEyeReprojection&&stereoNow
            &&!nativePackedStereo&&!s.alternatingStereoWarmupActive
            &&s.cachedEyeViewValid[e];
        if(handUsesCompletedPair){
            handSubmittedPose[e]=handPairWillPublish
                ?(aerSourceMode?s.cachedEyePose[e]:s.steamXrAerCapturePose[e]):s.steamXrAerSubmittedPose[e];
            handSubmittedFov[e]=handPairWillPublish
                ?(aerSourceMode?s.cachedEyeFov[e]:s.steamXrAerCaptureFov[e]):s.steamXrAerSubmittedFov[e];
        }else if(useSteamLinkExactMono||useSteamLinkSynchronizedViews){
            handSubmittedPose[e]=useSteamLinkSynchronizedViews
                &&steamLinkUseFixedPair?s.steamLinkFixedPairPose[e]
                :sourceMonoEyePose[e];
            handSubmittedFov[e]=useSteamLinkSynchronizedViews
                &&steamLinkUseFixedPair?s.steamLinkFixedPairFov[e]
                :sourceMonoEyeFov[e];
        }else{
            handSubmittedPose[e]=useSteamLinkReprojectedView?s.views[e].pose
                :(useCachedView?s.cachedEyePose[e]:s.views[e].pose);
            handSubmittedFov[e]=useSteamLinkReprojectedView
                ?submittedEyeFov(s.views[e].fov)
                :(useCachedView?s.cachedEyeFov[e]
                    :(nativePackedStereo?locatedCommonEyeFov
                        :submittedEyeFov(s.views[e].fov)));
        }
        if(nativeFrameValid){handSubmittedPose[e]=nativeFrame.eyes[e].pose;handSubmittedFov[e]=nativeFrame.pose.exactProjectionCrop?nativeFrame.pose.submitFov[e]:nativeFrame.eyes[e].fov;}
        if(!bypassApplicationTurnCompensation&&handUsesCompletedPair
            &&(steamRuntime||s.runtimeKind==kharvox::OpenXRRuntimeKind::VirtualDesktop
                ||s.runtimeKind==kharvox::OpenXRRuntimeKind::MetaOculus)
            &&steamLinkProjectionGameplay){
            const float pairTurn=handPairWillPublish
                ?s.steamXrAerCaptureArtificialTurn
                :s.steamXrAerSubmittedArtificialTurn;
            const float compensationDegrees=wrapDegrees(pairTurn
                -s.artificialTurnTotalDegrees);
            if(std::abs(compensationDegrees)>.001f)
                handSubmittedPose[e].orientation=normalizeQuaternion(multiply(
                    yawQuaternion(compensationDegrees),
                    handSubmittedPose[e].orientation));
        }
        if(!bypassApplicationTurnCompensation&&!handUsesCompletedPair&&stereoNow&&!nativePackedStereo){
            const auto snapCompensation=kharvox::snapTurnEyeCompensation(
                s.snapTurnStereoTransitionActive,nativePackedStereo,
                s.cachedEyeViewValid[e],s.snapTurnGeneration,
                s.cachedEyeSnapGeneration[e],s.artificialTurnTotalDegrees,
                s.cachedEyeArtificialTurn[e]);
            if(snapCompensation.apply)
                handSubmittedPose[e].orientation=normalizeQuaternion(multiply(
                    yawQuaternion(snapCompensation.degrees),
                    handSubmittedPose[e].orientation));
        }
    }
    static bool handStereoAlignmentLogged=false;
    if(handGameplayActive&&!handStereoAlignmentLogged){
        handStereoAlignmentLogged=true;
        log("[HANDS] projection aligned to submitted eye pose, FOV and image rectangle");
    }
    const bool freshWorldPairRecorded=freshAerHands&&handPairWillPublish;
    if(freshWorldPairRecorded){
        for(int e=0;e<2;++e){
            barrier(s.commandBuffer,s.freshHandsWorld[e],
                s.freshHandsInitialized?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,s.freshHandsInitialized?VK_ACCESS_TRANSFER_READ_BIT:0,VK_ACCESS_TRANSFER_WRITE_BIT);
            VkImageCopy copy{};copy.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
            copy.dstSubresource=copy.srcSubresource;copy.extent={eyeSourceExtent.width,eyeSourceExtent.height,1};
            s.vk.cmdCopyImage(s.commandBuffer,copySources[e],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                s.freshHandsWorld[e],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
            barrier(s.commandBuffer,s.freshHandsWorld[e],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
        }
    }
    const bool useFreshWorldPair=freshAerHands&&(freshWorldPairRecorded||s.freshHandsWorldValid);
    if(useFreshWorldPair)copySources=s.freshHandsWorld;
    if(updateEyeSwapchains)for(int e=0;e<2;e++){
        auto& eye=s.eyes[e];
        VkImage eyeSrc=copySources[e];
        VkImage dst=eye.images[xi[e]].image;
        barrier(s.commandBuffer,dst,eye.initialized[xi[e]]?VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,eye.initialized[xi[e]]?VK_ACCESS_MEMORY_READ_BIT:0,VK_ACCESS_TRANSFER_WRITE_BIT);
        if(nativeFrame.rightEyeBlackDiagnostic&&e==1){
            // No right scene source, cache copy, upscale or overlay. The XR
            // swapchain still receives initialized opaque black with its
            // normal acquisition, submission, release and completion contract.
            VkClearColorValue black{};black.float32[3]=1.0f;
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            s.vk.cmdClearColorImage(s.commandBuffer,dst,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&black,1,&range);
            submittedRects[e]={{0,0},{int32_t(eye.width),int32_t(eye.height)}};
            barrier(s.commandBuffer,dst,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_MEMORY_READ_BIT);
            eye.initialized[xi[e]]=true;
            continue;
        }
        uint32_t sourceWidth=stereoNow?eyeSourceExtent.width:it->second.extent.width;
        uint32_t sourceHeight=stereoNow?eyeSourceExtent.height:it->second.extent.height;
        uint32_t copyWidth=fillNativeEye?eye.width:std::min(sourceWidth,eye.width);
        uint32_t copyHeight=fillNativeEye?eye.height:std::min(sourceHeight,eye.height);
        if(s.quadMode){
            const float sourceAspect=float(sourceWidth)/float(sourceHeight),eyeAspect=float(eye.width)/float(eye.height);
            if(eyeAspect>sourceAspect){copyHeight=eye.height;copyWidth=uint32_t(float(copyHeight)*sourceAspect);}
            else{copyWidth=eye.width;copyHeight=uint32_t(float(copyWidth)/sourceAspect);}
        }
        const int32_t copyX=fillNativeEye?0:(int32_t(eye.width)-int32_t(copyWidth))/2;
        const int32_t copyY=fillNativeEye?0:(int32_t(eye.height)-int32_t(copyHeight))/2;
        submittedRects[e].offset={copyX,copyY};
        submittedRects[e].extent={int32_t(copyWidth),int32_t(copyHeight)};

        int32_t sourceX0=0,sourceY0=0,sourceX1=int32_t(sourceWidth),sourceY1=int32_t(sourceHeight);
        const bool nativeExactCrop=nativeFrameValid&&stereoNow&&nativeFrame.pose.exactProjectionCrop;
        const bool asymmetricProjectionCrop=fillNativeEye&&(stereoNow||s.immersiveCinematicActive||steamLinkSameFrameMono)&&(!nativePackedStereo||nativeExactCrop)&&(sfsBackend||s.runtimeKind!=kharvox::OpenXRRuntimeKind::VirtualDesktop);
        if(asymmetricProjectionCrop){
            const XrFovf submittedFov=useFreshWorldPair?handSubmittedFov[e]:nativeExactCrop?nativeFrame.pose.submitFov[e]:steamLinkSameFrameMono
                ?sourceMonoEyeFov[e]
                :(s.immersiveCinematicActive?s.views[e].fov:(s.cachedEyeViewValid[e]?s.cachedEyeFov[e]:submittedEyeFov(s.views[e].fov)));
            const XrFovf renderFov=nativeExactCrop?nativeFrame.eyes[e].fov:eyeRenderProjection(submittedFov).symmetricFov;
            const auto bounds=kharvox::projectionSourceCrop(renderFov,submittedFov);
            const auto [left,right,top,bottom]=bounds;
            sourceX0=std::clamp(int32_t(std::lround(left*float(sourceWidth))),0,int32_t(sourceWidth)-1);
            sourceX1=std::clamp(int32_t(std::lround(right*float(sourceWidth))),sourceX0+1,int32_t(sourceWidth));
            sourceY0=std::clamp(int32_t(std::lround(top*float(sourceHeight))),0,int32_t(sourceHeight)-1);
            sourceY1=std::clamp(int32_t(std::lround(bottom*float(sourceHeight))),sourceY0+1,int32_t(sourceHeight));
            static std::array<bool,4> cropLogged{};
            const size_t cropSlot=e+(nativeExactCrop?2:0);
            if(!cropLogged[cropSlot]){
                std::ostringstream crop;
                crop<<"[STEREO] exact asymmetric eye "<<e<<" source crop="<<sourceX0<<','<<sourceY0<<".."<<sourceX1<<','<<sourceY1
                    <<" nativeExact="<<nativeExactCrop<<" frame="<<(nativeExactCrop?nativeFrame.pose.serial:0)
                    <<" from="<<sourceWidth<<'x'<<sourceHeight<<" target="<<eye.width<<'x'<<eye.height;
                log(crop.str());
                cropLogged[cropSlot]=true;
            }
        }

        if(eyeCapture.requested)eyeCaptureCrop[e]={sourceX0,sourceY0,sourceX1,sourceY1};
        bool syntheticFsr1Source=false;
        if(kharvox::useFsr1ForFrame(s.fsr1.active(),stereoNow,
                nativePackedStereo,s.quadMode,nativeBackend&&nativeFrameValid)){
            const Fsr1SourceRect fsrSourceRect{sourceX0,sourceY0,
                static_cast<uint32_t>(sourceX1-sourceX0),
                static_cast<uint32_t>(sourceY1-sourceY0)};
            if(const VkImage upscaled=s.fsr1.record(s.commandBuffer,eyeSrc,e,
                    useFreshWorldPair?((uint64_t{1}<<63)|(s.freshHandsWorldRevision+uint64_t(freshWorldPairRecorded))):s.stereoCacheRevision[e],fsrSourceRect,{copyWidth,copyHeight})){
                eyeSrc=upscaled;
                sourceWidth=copyWidth;sourceHeight=copyHeight;
                sourceX0=0;sourceY0=0;
                sourceX1=static_cast<int32_t>(copyWidth);
                sourceY1=static_cast<int32_t>(copyHeight);
                syntheticFsr1Source=true;
            }
        }
        if(eyeCapture.requested)eyeCaptureFsr[e]=syntheticFsr1Source;
        const bool syntheticAfwSource=false&&eyeSrc!=s.stereoCache[e];
        const bool fullSource=sourceX0==0&&sourceY0==0&&sourceX1==int32_t(sourceWidth)&&sourceY1==int32_t(sourceHeight);
        if(eyeCapture.requested)eyeCaptureTransfer[e]=
            kharvox::shouldBlackoutCenteredQuadTransition(s.centeredQuadTransitionPending,s.quadMode)?"blackout":
            (s.vk.cmdCopyImage&&!syntheticAfwSource&&!syntheticFsr1Source&&fullSource&&copyWidth==sourceWidth&&copyHeight==sourceHeight)?"copy":"linear-blit";
        if(kharvox::shouldBlackoutCenteredQuadTransition(
                s.centeredQuadTransitionPending,s.quadMode)){
            VkClearColorValue black{};
            black.float32[3]=1.0f;
            VkImageSubresourceRange range{};
            range.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
            range.levelCount=1;
            range.layerCount=1;
            s.vk.cmdClearColorImage(s.commandBuffer,dst,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&black,1,&range);
        }else if(s.vk.cmdCopyImage&&!syntheticAfwSource&&!syntheticFsr1Source&&fullSource&&copyWidth==sourceWidth&&copyHeight==sourceHeight){
            VkImageCopy copy{};copy.srcSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;copy.srcSubresource.layerCount=1;copy.dstSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;copy.dstSubresource.layerCount=1;copy.dstOffset={copyX,copyY,0};copy.extent={copyWidth,copyHeight,1};
            s.vk.cmdCopyImage(s.commandBuffer,eyeSrc,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,dst,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
        }else{
            VkImageBlit blit{};blit.srcSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;blit.srcSubresource.layerCount=1;blit.srcOffsets[0]={sourceX0,sourceY0,0};blit.srcOffsets[1]={sourceX1,sourceY1,1};blit.dstSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;blit.dstSubresource.layerCount=1;blit.dstOffsets[0]={copyX,copyY,0};blit.dstOffsets[1]={copyX+int32_t(copyWidth),copyY+int32_t(copyHeight),1};
            s.vk.cmdBlitImage(s.commandBuffer,eyeSrc,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,dst,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_LINEAR);
        }
        barrier(s.commandBuffer,dst,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        kharvox::hands::HandEyeView handView{};
        handView.pose.position[0]=handSubmittedPose[e].position.x;
        handView.pose.position[1]=handSubmittedPose[e].position.y;
        handView.pose.position[2]=handSubmittedPose[e].position.z;
        handView.pose.orientation[0]=handSubmittedPose[e].orientation.x;
        handView.pose.orientation[1]=handSubmittedPose[e].orientation.y;
        handView.pose.orientation[2]=handSubmittedPose[e].orientation.z;
        handView.pose.orientation[3]=handSubmittedPose[e].orientation.w;
        handView.pose.valid=true;
        handView.angleLeft=handSubmittedFov[e].angleLeft;
        handView.angleRight=handSubmittedFov[e].angleRight;
        handView.angleUp=handSubmittedFov[e].angleUp;
        handView.angleDown=handSubmittedFov[e].angleDown;
        handView.imageRectX=submittedRects[e].offset.x;
        handView.imageRectY=submittedRects[e].offset.y;
        handView.imageRectWidth=uint32_t(submittedRects[e].extent.width);
        handView.imageRectHeight=uint32_t(submittedRects[e].extent.height);
        if(freshAerHands||!s.stereoCacheHasIntegratedHands[e])
            s.handRenderer.record(s.commandBuffer,e,xi[e],handView,leftHandPose,
                rightHandPose,handVisibility,currentHandGameplay);
        eye.initialized[xi[e]]=true;
    }
    if(s.hudImageState.releasable()){
        VkImage hudDestination=s.hudQuad.images[hudImageIndex].image;
        const VkImageLayout previousDestinationLayout=s.hudQuad.initialized[hudImageIndex]
            ?VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;
        barrier(s.commandBuffer,hudDestination,previousDestinationLayout,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            s.hudQuad.initialized[hudImageIndex]?VK_ACCESS_MEMORY_READ_BIT:0,
            VK_ACCESS_TRANSFER_WRITE_BIT);
        if(hudSource.layout!=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
            barrier(s.commandBuffer,hudSource.image,hudSource.layout,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT
                    |VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
        if(hudSource.extent.width==s.hudQuad.width
            &&hudSource.extent.height==s.hudQuad.height){
            VkImageCopy hudCopy{};
            hudCopy.srcSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
            hudCopy.srcSubresource.layerCount=1;
            hudCopy.dstSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
            hudCopy.dstSubresource.layerCount=1;
            hudCopy.extent={s.hudQuad.width,s.hudQuad.height,1};
            s.vk.cmdCopyImage(s.commandBuffer,hudSource.image,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,hudDestination,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&hudCopy);
        }else{
            VkImageBlit hudBlit{};
            hudBlit.srcSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
            hudBlit.srcSubresource.layerCount=1;
            hudBlit.srcOffsets[1]={static_cast<int32_t>(hudSource.extent.width),
                static_cast<int32_t>(hudSource.extent.height),1};
            hudBlit.dstSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
            hudBlit.dstSubresource.layerCount=1;
            hudBlit.dstOffsets[1]={static_cast<int32_t>(s.hudQuad.width),
                static_cast<int32_t>(s.hudQuad.height),1};
            s.vk.cmdBlitImage(s.commandBuffer,hudSource.image,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,hudDestination,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&hudBlit,VK_FILTER_LINEAR);
        }
        if(hudSource.layout!=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
            barrier(s.commandBuffer,hudSource.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                hudSource.layout,VK_ACCESS_TRANSFER_READ_BIT,
                VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT);
        barrier(s.commandBuffer,hudDestination,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_MEMORY_READ_BIT);
        s.hudQuad.initialized[hudImageIndex]=true;
        hudImageCopied=true;
    }

    // Alternating camera offsets are useful for proving stereo, but showing the
    // raw alternating render on the desktop produces extreme left/right flicker.
    // Keep the HMD eye caches untouched and mirror one stable eye to the game's
    // swapchain. The mirror then updates at half rate instead of changing eyes.
    if(stereoNow&&!nativePackedStereo&&s.stereoCacheInitialized[0]){barrier(s.commandBuffer,src,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);VkImageCopy mirrorCopy{};mirrorCopy.srcSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;mirrorCopy.srcSubresource.layerCount=1;mirrorCopy.dstSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;mirrorCopy.dstSubresource.layerCount=1;mirrorCopy.extent={it->second.extent.width,it->second.extent.height,1};s.vk.cmdCopyImage(s.commandBuffer,s.stereoCache[0],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,src,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&mirrorCopy);barrier(s.commandBuffer,src,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_MEMORY_READ_BIT);}else if(stereoNow&&nativePackedStereo&&!nativeBackend&&s.stereoCacheInitialized[0]){barrier(s.commandBuffer,src,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);VkImageBlit mirrorBlit{};mirrorBlit.srcSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;mirrorBlit.srcSubresource.layerCount=1;mirrorBlit.srcOffsets[1]={int32_t(eyeSourceExtent.width),int32_t(eyeSourceExtent.height),1};mirrorBlit.dstSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;mirrorBlit.dstSubresource.layerCount=1;mirrorBlit.dstOffsets[1]={int32_t(it->second.extent.width),int32_t(it->second.extent.height),1};s.vk.cmdBlitImage(s.commandBuffer,s.stereoCache[0],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,src,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&mirrorBlit,VK_FILTER_LINEAR);barrier(s.commandBuffer,src,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_MEMORY_READ_BIT);}else barrier(s.commandBuffer,src,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_MEMORY_READ_BIT);
    eyeCaptureSourcesMatched=true;
    for(int e=0;e<2;++e)eyeCaptureSourcesMatched=eyeCaptureSourcesMatched
        &&eyeSourceCapture[e].valid&&eyeSourceCapture[e].revision==s.stereoCacheRevision[e]
        &&copySources[e]==s.stereoCache[e]&&eyeSourceCapture[e].cache==copySources[e]
        &&eyeSourceCapture[e].generation==KharvoxCameraLevelTransitionGeneration();
    const bool sourceCaptureTimedOut=eyeCapture.requested&&GetTickCount64()>eyeCapture.due+10000;
    if(sourceCaptureTimedOut&&!eyeCaptureSourcesMatched)
        log("[EYE-CAPTURE] raw source pair unavailable; final-only capture explicitly marked sourceMatched=false");
    const bool eyeCaptureRecorded=(eyeCaptureSourcesMatched||sourceCaptureTimedOut)&&stereoNow&&!s.quadMode&&updateEyeSwapchains
        &&(aerSourceMode?sourcePairWillPublish:!steamXrAerPairPath||currentRenderEye==kharvox::aerSecondRenderEye)&&recordEyeCapture(xi);
    bool nativeXrCaptureRecorded=false;
    if(nativeFrameValid&&!nativeFrame.rightEyeBlackDiagnostic&&updateEyeSwapchains&&kharvox::native::xrTargetCaptureEnabled()){
        std::array<kharvox::native::XrCaptureEye,2> targets{};
        for(size_t e=0;e<2;++e)targets[e]={s.eyes[e].images[xi[e]].image,{s.eyes[e].width,s.eyes[e].height},VkFormat(s.format),submittedRects[e],xi[e]};
        nativeXrCaptureRecorded=kharvox::native::recordXrTargetCapture(s.device,s.commandBuffer,nativeFrame,targets);
    }
    const bool nativeWatchRecorded=!sfsBackend&&nativeFrameValid&&!nativeFrame.rightEyeBlackDiagnostic&&updateEyeSwapchains&&kharvox::native::recordPairWatch(s.device,s.commandBuffer,nativeFrame);
    if(!sfsBackend&&nativeFrameValid&&!nativeFrame.rightEyeBlackDiagnostic)kharvox::native::recordShadowHistory(s.commandBuffer,nativeFrame);
    kharvox::native::endOwnerGpuTiming(nativeOwnerGpuSpan);
    if(sfsBackend)s.sfsCopyTiming.end(s.commandBuffer);
    const VkResult copyRecordResult=s.vk.endCommandBuffer(s.commandBuffer);
    if(!commandRecording.ended(copyRecordResult==VK_SUCCESS)){abortCommandRecording("end-command-buffer",copyRecordResult);return;}
    VkSemaphore afwSignalSemaphore{};uint64_t afwSignalValue{};const bool afwSignalPrepared=false;
    std::vector<VkSemaphore> submitWaits;if(p->waitSemaphoreCount)submitWaits.assign(p->pWaitSemaphores,p->pWaitSemaphores+p->waitSemaphoreCount);if(afwPrepared)submitWaits.push_back(afwWaitSemaphore);
    std::vector<VkPipelineStageFlags> waitStages(submitWaits.size(),VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    std::vector<uint64_t> waitValues(submitWaits.size(),0);if(afwPrepared)waitValues.back()=afwWaitValue;
    VkTimelineSemaphoreSubmitInfo timelineSubmit{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};timelineSubmit.waitSemaphoreValueCount=uint32_t(waitValues.size());timelineSubmit.pWaitSemaphoreValues=waitValues.data();timelineSubmit.signalSemaphoreValueCount=afwSignalPrepared?1u:0u;timelineSubmit.pSignalSemaphoreValues=afwSignalPrepared?&afwSignalValue:nullptr;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.pNext=(afwPrepared||afwSignalPrepared)?&timelineSubmit:nullptr;submit.waitSemaphoreCount=uint32_t(submitWaits.size());submit.pWaitSemaphores=submitWaits.empty()?nullptr:submitWaits.data();submit.pWaitDstStageMask=waitStages.empty()?nullptr:waitStages.data();submit.commandBufferCount=1;submit.pCommandBuffers=&s.commandBuffer;submit.signalSemaphoreCount=afwSignalPrepared?1u:0u;submit.pSignalSemaphores=afwSignalPrepared?&afwSignalSemaphore:nullptr;
    VkFence copyCompletion=VK_NULL_HANDLE;
    const bool sourceRingBackend=sfsBackend&&kharvox::sfs::sourceRingActive(s.device);
    // Owned sources can use the existing ordered XR release contract: submit
    // before release/endFrame, then prove completion before reusing parameters,
    // command buffers or hand attachments. Readbacks keep synchronous retirement.
    const bool earlyReleaseRequested=(!sfsBackend||sourceRingBackend)&&kharvox::rendererDefaults::earlyXrRelease;
    const bool nativePairReady=nativeFrameValid&&updateEyeSwapchains&&s.eyeImageStates[0].releasable()&&s.eyeImageStates[1].releasable();
    const bool queueSynchronized=queueAccessLockCallback&&queueAccessUnlockCallback;
    const bool readbackRecorded=nativeXrCaptureRecorded||nativeWatchRecorded||eyeCaptureRecorded
        ||rawEyeCaptureRecorded[0]||rawEyeCaptureRecorded[1];
    if(kharvox::nativeXrCopyFenceRequested(s.runtimeKind,earlyReleaseRequested,nativePairReady,
        !s.quadMode,queueSynchronized,readbackRecorded)&&s.copyFence&&s.vk.resetFences&&s.vk.waitForFences){
        if(s.vk.resetFences(s.device,1,&s.copyFence)==VK_SUCCESS)copyCompletion=s.copyFence;
        else log("[XR-COPY-FENCE] copy fence reset failed; using queueWaitIdle fallback");
    }
    const bool earlyRelease=kharvox::nativeEarlyXrRelease(earlyReleaseRequested,s.runtimeKind,
        nativePairReady,!s.quadMode,copyCompletion!=VK_NULL_HANDLE,queueSynchronized,readbackRecorded);
    kharvox::NativeXrCopyLifetime copyLifetime(earlyRelease);
    kharvox::sfs::OwnerCopyCompletion sfsOwnerCopy;
    LARGE_INTEGER nativeSubmitAt{},nativeSubmitDone{},nativeReleaseAt{},nativeReleaseDone{},nativeEndAt{},nativeEndDone{},nativeCompleteAt{};
    LARGE_INTEGER copyWaitStart{},copyWaitEnd{};
    if(steamRuntime)QueryPerformanceCounter(&copyWaitStart);
    VkResult submitResult{VK_ERROR_DEVICE_LOST};
    VkResult completionResult{VK_ERROR_DEVICE_LOST};
    {
        // Keep the current copy and its completion boundary ordered before the
        // downstream Present. A different DOOM submit thread must not jump
        // between them; the isolated fence still avoids waiting for unrelated
        // work queued after this submission outside the layer.
        const auto nativeQueueLockStart=kharvox::native::cpu::current.active?kharvox::native::cpu::now():0;
        QueueAccessScope queueAccess;
        kharvox::native::cpu::elapsed(kharvox::native::cpu::XrQueueLock,nativeQueueLockStart);
        kharvox::native::cpu::Scope nativeCopyProfile(kharvox::native::cpu::XrCopyCompletion);
        if(sfsBackend&&nativePairReady&&!s.quadMode&&queueSynchronized)
            sfsOwnerCopy=kharvox::sfs::captureOwnerCopy(s.device,q,copyCompletion,nativeFrame.generation);
        if(nativeFrameValid)QueryPerformanceCounter(&nativeSubmitAt);
        {kharvox::native::cpu::Scope submitProfile(kharvox::native::cpu::XrQueueSubmit);kharvox::native::trace::SubmitScope capture("owner-submit",q,1,&submit,copyCompletion);submitResult=s.vk.queueSubmit(q,1,&submit,copyCompletion);capture.result(submitResult);}
        if(nativeFrameValid)QueryPerformanceCounter(&nativeSubmitDone);
        copyLifetime.submitted(submitResult==VK_SUCCESS);
        if(submitResult==VK_SUCCESS&&consumedPresentWaits)*consumedPresentWaits=true;
        if(submitResult==VK_SUCCESS&&!earlyRelease){
            kharvox::native::cpu::Scope waitProfile(kharvox::native::cpu::XrQueueWait);
            completionResult=copyCompletion
                ?s.vk.waitForFences(s.device,1,&copyCompletion,VK_TRUE,UINT64_MAX)
                :s.vk.queueWaitIdle(q);
        }
    }
    auto reportNativeDeviceLost=[&](const char* operation,VkResult failure){
        if(failure!=VK_ERROR_DEVICE_LOST||!kharvox::native::requested())return;
        const auto report=std::string("Native GPU device lost: operation=")+operation
            +" runtime="+kharvox::openXRRuntimeKindName(s.runtimeKind)
            +" ownerFrame="+std::to_string(s.frame)+" nativePair="+std::to_string(nativeFrameValid)
            +" nativePhase="+std::to_string(int(kharvox::native::phase()))
            +" quad="+std::to_string(s.quadMode)+" early="+std::to_string(earlyRelease)
            +" copyFence="+std::to_string(copyCompletion!=VK_NULL_HANDLE)
            +" pid="+std::to_string(GetCurrentProcessId())+" result=VK_ERROR_DEVICE_LOST";
        const auto filename="native_gpu_failure-"+std::to_string(GetCurrentProcessId())+".log";
        {std::ofstream file(kharvox::logPathA(filename.c_str()));file<<report<<'\n';}
        log(report);
        kharvox::native::fail("GPU device lost during owner copy; see native_gpu_failure log");
    };
    reportNativeDeviceLost("queueSubmit",submitResult);
    if(submitResult!=VK_SUCCESS){
        if(freshAerHands)s.freshHandsWorldValid=false;
        if(nativeFrameValid)kharvox::native::fail("owner native copy submission failed; restart required");
        s.handRenderer.finishSceneIntegratedFrame();
        releaseAcquiredEyeImages();
        if(s.hudImageState.owned())releaseImageChecked(s.hudQuad.handle,s.hudImageState);
        log("copy submit failed "+std::to_string(submitResult));endEmptyFrame("copy-submit-failed");return;
    }
    if(steamRuntime){QueryPerformanceCounter(&copyWaitEnd);steamCopyWaitMs=performanceMilliseconds(copyWaitStart,copyWaitEnd);}
    if(!earlyRelease)copyLifetime.completed(completionResult==VK_SUCCESS);
    if(!earlyRelease)reportNativeDeviceLost(copyCompletion?"waitForFences":"queueWaitIdle",completionResult);
    if(!earlyRelease&&completionResult!=VK_SUCCESS){
        if(freshAerHands)s.freshHandsWorldValid=false;
        if(nativeFrameValid)kharvox::native::fail("owner native copy completion failed; restart required");
        releaseAcquiredEyeImages();
        if(s.hudImageState.owned())releaseImageChecked(s.hudQuad.handle,s.hudImageState);
        log(std::string(copyCompletion?"copy fence wait failed ":"copy queue wait failed ")+std::to_string(completionResult));endEmptyFrame("copy-completion-failed");return;
    }
    if(!earlyRelease){
        if(sfsBackend)kharvox::sfs::copyCompleted(s.device,sfsOwnerCopy,submitResult,completionResult);
        for(int e=0;e<2;++e)if(rawEyeCaptureRecorded[e])eyeSourceCapture[e].completed=true;
        if(nativeFrameValid)QueryPerformanceCounter(&nativeCompleteAt);
        if(!copyLifetime.canRetireResources())kharvox::native::fail("owner resources retired before copy completion");
        if(nativeXrCaptureRecorded)kharvox::native::finishXrTargetCapture();
        if(nativeWatchRecorded)kharvox::native::finishPairWatch();
        s.handRenderer.finishSceneIntegratedFrame();
    }
    if(consumedPresentWaits)*consumedPresentWaits=true;
    if(!copyLifetime.canReleaseImages())kharvox::native::fail("XR images released before ordered copy submission");
    if(nativeFrameValid)QueryPerformanceCounter(&nativeReleaseAt);
    releaseAcquiredEyeImages();
    if(s.hudImageState.owned())releaseImageChecked(s.hudQuad.handle,s.hudImageState);

    if(nativeFrameValid)QueryPerformanceCounter(&nativeReleaseDone);
    if(freshWorldPairRecorded){
        s.freshHandsInitialized=true;s.freshHandsWorldValid=true;
        ++s.freshHandsWorldRevision;
    }
    if(freshAerHands&&updateEyeSwapchains&&(s.frame%240==0||s.freshHandsWorldRevision<=2))
        log("[FRESH-AER-HANDS] hands refreshed both eyes; worldPair="
            +std::to_string(s.freshHandsWorldRevision)+" reused="
            +std::to_string(steamXrAerReusePublishedPair)+" sourceHands=excluded");
    if(handPairWillPublish){
        if(aerSourceMode){
            if(s.aerPublishedDomain!=aerSourceObservation.key.domain)
                log("[AER-SOURCE-DOMAIN] completed pair published domain="+std::to_string(aerSourceObservation.key.domain)
                    +" pose="+std::to_string(aerSourceObservation.key.poseId));
            s.aerPublishedSourcePoseId=aerSourceObservation.key.poseId;
            s.aerPublishedDomain=aerSourceObservation.key.domain;
        }
        s.steamXrAerSubmittedPose=aerSourceMode?s.cachedEyePose:s.steamXrAerCapturePose;
        poseTracePublishedRevisions=s.stereoCacheRevision;
        poseTracePublishedIds=poseTraceCachedIds;
        s.steamXrAerSubmittedFov=aerSourceMode?s.cachedEyeFov:s.steamXrAerCaptureFov;
        s.steamXrAerSubmittedRects=submittedRects;
        s.steamXrAerSubmittedArtificialTurn=
            aerSourceMode?captureTurn:s.steamXrAerCaptureArtificialTurn;
        s.steamXrAerPairReady=true;
        const uint64_t published=++s.steamXrAerPublishedPairs;
        if(published<=4||published%256==0)
            log(std::string(coherentAerPairLogTag)
                +" complete left/right pair published count="
                +std::to_string(published));
    }else if(steamXrAerReusePublishedPair){
        const uint64_t reused=++s.steamXrAerReusedPairs;
        if(reused<=4||reused%256==0)
            log(std::string(coherentAerPairLogTag)
                +" last complete pair re-submitted while awaiting new complete pair count="
                +std::to_string(reused));
    }
    XrPosef monoPose=locatedMonoPose;
    if(steamLinkSameFrameMono&&sourceMonoPoseValid)monoPose=sourceMonoPose;
    if(s.immersiveCinematicFreelookActive){monoPose=sourceMonoPose;if(s.immersiveCinematicLayerPositionHeld)monoPose.position=s.immersiveCinematicLayerPosition;}
    else if(s.immersiveCinematicActive){monoPose={};monoPose.orientation.w=1.0f;}
    // DOOM's camera accepts only symmetric FOVs. Runtime-managed paths render
    // an enclosing symmetric frustum, crop it in tangent space during the
    // Vulkan blit, and submit the runtime's exact asymmetric FOV and pose.
    // Virtual Desktop retains its physically proven centered projection.
    const XrFovf commonEyeFov=locatedCommonEyeFov;
    float halfTanX=std::min({-std::tan(s.views[0].fov.angleLeft),std::tan(s.views[0].fov.angleRight),-std::tan(s.views[1].fov.angleLeft),std::tan(s.views[1].fov.angleRight)});float halfTanY=std::min({-std::tan(s.views[0].fov.angleDown),std::tan(s.views[0].fov.angleUp),-std::tan(s.views[1].fov.angleDown),std::tan(s.views[1].fov.angleUp)});constexpr float sourceAspect=16.0f/9.0f;halfTanY=std::min(halfTanY,halfTanX/sourceAspect);halfTanX=halfTanY*sourceAspect;XrFovf monoFov{-std::atan(halfTanX),std::atan(halfTanX),std::atan(halfTanY),-std::atan(halfTanY)};
    if((steamLinkSameFrameMono&&sourceMonoPoseValid)||(s.immersiveCinematicActive&&!s.quadMode))monoFov=sourceMonoFov;
    const bool exactImmersiveProjection=s.immersiveCinematicActive
        &&!nativePackedStereo
        &&(sfsBackend||s.runtimeKind!=kharvox::OpenXRRuntimeKind::VirtualDesktop);
    static bool exactImmersiveProjectionLogged=false;
    if(exactImmersiveProjection&&!exactImmersiveProjectionLogged){
        log("[IMMERSIVE] exact asymmetric per-eye crop ACTIVE; mono animation position and cached HMD rotation retained");
        exactImmersiveProjectionLogged=true;
    }
    std::array<XrCompositionLayerProjectionView,2> pv{{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}}};
    for(int e=0;e<2;e++){
        const bool useSteamXrCompletedAerPair=steamXrAerPairEligible
            &&s.steamXrAerPairReady;
        const bool useSteamLinkExactMono=steamLinkSameFrameMono&&sourceMonoEyeViewsValid;
        const bool useSteamLinkSynchronizedViews=steamLinkFullMotionWarp&&sourceMonoEyeViewsValid;
        const bool useSteamLinkReprojectedView=steamLinkFullMotionWarp&&e==staleEye;
        const bool useCachedView=!useSteamLinkSynchronizedViews
            &&!useSteamLinkReprojectedView&&cachedEyeReprojection&&stereoNow
            &&!nativePackedStereo&&!s.alternatingStereoWarmupActive
            &&s.cachedEyeViewValid[e];
        if(useSteamXrCompletedAerPair){
            pv[e].pose=s.steamXrAerSubmittedPose[e];
            pv[e].fov=s.steamXrAerSubmittedFov[e];
        }else if(useSteamLinkExactMono||useSteamLinkSynchronizedViews){
            pv[e].pose=useSteamLinkSynchronizedViews&&steamLinkUseFixedPair?s.steamLinkFixedPairPose[e]:sourceMonoEyePose[e];
            pv[e].fov=useSteamLinkSynchronizedViews&&steamLinkUseFixedPair?s.steamLinkFixedPairFov[e]:sourceMonoEyeFov[e];
        }else{
            pv[e].pose=useSteamLinkReprojectedView?s.views[e].pose:(useCachedView?s.cachedEyePose[e]:(stereoNow?s.views[e].pose:monoPose));
            pv[e].fov=exactImmersiveProjection?s.views[e].fov
                :(useSteamLinkReprojectedView?submittedEyeFov(s.views[e].fov):(useCachedView?s.cachedEyeFov[e]:(nativePackedStereo?commonEyeFov:(stereoNow?submittedEyeFov(s.views[e].fov):monoFov))));
        }
        if(nativeFrameValid&&stereoNow){pv[e].pose=nativeFrame.eyes[e].pose;pv[e].fov=nativeFrame.pose.exactProjectionCrop?nativeFrame.pose.submitFov[e]:nativeFrame.eyes[e].fov;}
        if(!bypassApplicationTurnCompensation&&useSteamXrCompletedAerPair
            &&(steamRuntime||s.runtimeKind==kharvox::OpenXRRuntimeKind::VirtualDesktop
                ||s.runtimeKind==kharvox::OpenXRRuntimeKind::MetaOculus)
            &&steamLinkProjectionGameplay){
            // A complete pair can be re-submitted once while the next right eye
            // is rendered. Compensate the deterministic stick-turn delta which
            // occurred since that pair was captured, so its final frame does
            // not trail after the stick returns to centre.
            const float compensationDegrees=wrapDegrees(
                s.steamXrAerSubmittedArtificialTurn
                -s.artificialTurnTotalDegrees);
            if(std::abs(compensationDegrees)>.001f){
                pv[e].pose.orientation=normalizeQuaternion(multiply(
                    yawQuaternion(compensationDegrees),
                    pv[e].pose.orientation));
                if(e==0){
                    const auto count=++s.steamXrAerTurnCompensations;
                    if(count<=4||count%512==0)
                        log("[STEAMXR-TURN] completed-pair yaw compensation="
                            +std::to_string(compensationDegrees)+"deg");
                }
            }
        }
        if(!bypassApplicationTurnCompensation&&!nativeBackend&&!useSteamXrCompletedAerPair&&stereoNow&&!nativePackedStereo){
            const auto snapCompensation=kharvox::snapTurnEyeCompensation(
                s.snapTurnStereoTransitionActive,nativePackedStereo,
                s.cachedEyeViewValid[e],s.snapTurnGeneration,
                s.cachedEyeSnapGeneration[e],s.artificialTurnTotalDegrees,
                s.cachedEyeArtificialTurn[e]);
            if(snapCompensation.apply){
                pv[e].pose.orientation=normalizeQuaternion(multiply(
                    yawQuaternion(snapCompensation.degrees),
                    pv[e].pose.orientation));
                if(e==0){
                    const auto count=++s.snapTurnStereoCompensations;
                    if(count<=8||count%128==0)
                        log("[TURN] AER stale snap eye compensated degrees="
                            +std::to_string(snapCompensation.degrees)
                            +" generation="+std::to_string(s.snapTurnGeneration));
                }
            }
        }
        if(!aerSourceMode&&s.immersiveCinematicActive&&!s.immersiveCinematicFreelookActive&&!(nativeFrameValid&&stereoNow)){
            // projection.space is VIEW below. xrLocateViews returned LOCAL-space
            // poses, so submitting them unchanged applies the HMD transform
            // twice. Ask the runtime for the exact VIEW-space eye pose. The
            // midpoint-derived transform remains only a fail-safe for runtimes
            // which reject that standard locate operation.
            pv[e].pose=lockedCinematicViewSpaceViewsValid
                ?lockedCinematicViewSpaceViews[e].pose
                :poseRelativeTo(locatedMonoPose,s.views[e].pose);
            const XrFovf lockedEyeFov=lockedCinematicViewSpaceViewsValid
                ?lockedCinematicViewSpaceViews[e].fov:s.views[e].fov;
            // Virtual Desktop renders AER into a centred symmetric frustum.
            // Keep that exact convention when the layer is locked as well;
            // submitting its asymmetric raw FOV here while the pixels were
            // rendered centred was the VDXR-only no-freelook double image.
            pv[e].fov=submittedEyeFov(lockedEyeFov);
            static bool virtualDesktopLockedFovLogged=false;
            if(e==0&&kharvox::useCenteredProjectionFov(s.runtimeKind)
                &&!virtualDesktopLockedFovLogged){
                virtualDesktopLockedFovLogged=true;
                log("[IMMERSIVE] Virtual Desktop locked cinematic uses matching centered render/submission FOV");
            }
        }
        pv[e].subImage.swapchain=s.eyes[e].handle;
        pv[e].subImage.imageRect=submittedRects[e];
    }
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projection.space=(nativeFrameValid&&stereoNow?nativeFrame.pose.viewSpace:
        (aerSourceMode?s.aerPublishedDomain==1&&!s.immersiveCinematicFreelook:
            s.immersiveCinematicActive&&!s.immersiveCinematicFreelookActive))
        ?s.viewSpace:s.space;
    projection.viewCount=2;
    projection.views=pv.data();

    auto& quadEye=s.eyes[0];
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain=quadEye.handle;
    quad.subImage.imageRect=submittedRects[0];
    const auto croppedHeight=kharvox::fullFrameQuadCroppedHeight(
        quad.subImage.imageRect.extent.width,quad.subImage.imageRect.extent.height);
    quad.subImage.imageRect.offset.y+=(quad.subImage.imageRect.extent.height-croppedHeight)/2;
    quad.subImage.imageRect.extent.height=croppedHeight;
    const float completeFrameDistance=s.hudEverythingQuad?KharvoxHudDistanceMeters()
        :kharvox::fullFrameQuadDistanceMeters(false);
    if(!s.cinewindowFollowsHeadset&&s.cinewindowAnchor.valid){
        const auto fixedPose=kharvox::fixedCinewindowLayerPose(
            s.cinewindowAnchor,completeFrameDistance);
        quad.space=s.space;
        quad.pose.orientation={fixedPose.orientationX,fixedPose.orientationY,
            fixedPose.orientationZ,fixedPose.orientationW};
        quad.pose.position={fixedPose.positionX,fixedPose.positionY,
            fixedPose.positionZ};
    }else{
        quad.space=s.viewSpace;
        quad.pose.orientation.w=1;
        quad.pose.position.z=-completeFrameDistance;
    }
    const float completeFrameWidth=s.hudEverythingQuad
        ?std::max(0.05f,hudAngularWidthMeters)
        :kharvox::fullFrameQuadWidthMeters(false);
    quad.size={completeFrameWidth,kharvox::fullFrameQuadHeightMeters(
        completeFrameWidth,quad.subImage.imageRect.extent.width,
        quad.subImage.imageRect.extent.height)};
    // Diagnostic distinguishes a moving layer from movement within its pixels.
    if(s.quadMode&&(s.frame%120==0)){
        log("[QUAD-POSE] r262 fixed="+std::to_string(s.cinewindowAnchor.valid)
            +" pixels="+std::to_string(quad.subImage.imageRect.extent.width)+"x"+std::to_string(quad.subImage.imageRect.extent.height)
            +" meters="+std::to_string(quad.size.width)+"x"+std::to_string(quad.size.height)
            +" headQ="+std::to_string(locatedMonoPose.orientation.x)+","
            +std::to_string(locatedMonoPose.orientation.y)+","
            +std::to_string(locatedMonoPose.orientation.z)+","
            +std::to_string(locatedMonoPose.orientation.w)
            +" layerQ="+std::to_string(quad.pose.orientation.x)+","
            +std::to_string(quad.pose.orientation.y)+","
            +std::to_string(quad.pose.orientation.z)+","
            +std::to_string(quad.pose.orientation.w)
            +" layerP="+std::to_string(quad.pose.position.x)+","
            +std::to_string(quad.pose.position.y)+","
            +std::to_string(quad.pose.position.z));
    }

    // A centered coplanar image below Pause extends the canvas without stretching
    // DOOM's menu, moving its hit targets, or following a different head pose.
    const bool showPauseBindings=s.quadMode&&!s.centeredQuadTransitionPending
        &&pauseBindingsReady&&KharvoxHudPauseRootVisible();
    XrCompositionLayerQuad bindingsLayer=quad;
    if(showPauseBindings){
        bindingsLayer.subImage.swapchain=pauseBindings.handle;
        bindingsLayer.subImage.imageRect={{0,0},{static_cast<int32_t>(pauseBindings.width),
            static_cast<int32_t>(pauseBindings.height)}};
        bindingsLayer.size={quad.size.width,quad.size.width*float(pauseBindings.height)/float(pauseBindings.width)};
        const auto offset=rotateVector(quad.pose.orientation,
            {0,-(quad.size.height+bindingsLayer.size.height)*.5f-0.025f,0});
        bindingsLayer.pose.position=addVector(quad.pose.position,offset);
    }

    XrCompositionLayerQuad hudQuadLayer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    hudQuadLayer.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    hudQuadLayer.space=s.viewSpace;
    hudQuadLayer.eyeVisibility=nativeFrame.rightEyeBlackDiagnostic?XR_EYE_VISIBILITY_LEFT:XR_EYE_VISIBILITY_BOTH;
    hudQuadLayer.subImage.swapchain=s.hudQuad.handle;
    hudQuadLayer.subImage.imageRect.offset={0,0};
    hudQuadLayer.subImage.imageRect.extent={
        static_cast<int32_t>(s.hudQuad.width),static_cast<int32_t>(s.hudQuad.height)};
    hudQuadLayer.pose.orientation.w=1;
    hudQuadLayer.pose.position.z=-KharvoxHudDistanceMeters();
    const float hudWidthMeters=std::max(0.05f,hudAngularWidthMeters);
    hudQuadLayer.size={hudWidthMeters,hudWidthMeters/hudContentAspect};

    std::array<const XrCompositionLayerBaseHeader*,5> layers{};
    uint32_t layerCount{};
    layers[layerCount++]=s.quadMode
        ?reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad)
        :reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
    if(showPauseBindings) layers[layerCount++]=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&bindingsLayer);

    if(hudImageCopied){
        layers[layerCount++]=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&hudQuadLayer);
        const uint64_t copied=++s.hudQuadCopiedFrames;
        if(copied==1){
            std::ostringstream activated;
            activated<<std::fixed<<std::setprecision(2)
                <<"[HUD9-QUAD] ACTIVE source="<<hudSource.extent.width<<'x'
                <<hudSource.extent.height<<" target="<<s.hudQuad.width<<'x'
                <<s.hudQuad.height<<" distance="<<KharvoxHudDistanceMeters()
                <<"m physicalWidth="<<hudWidthMeters<<"m size="
                <<KharvoxHudQuadScale()<<"x; Num +/- preview, Num * save";
            log(activated.str());
        }
    }
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime=frame.predictedDisplayTime;
    ei.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    // Never submit uninitialized/mislabelled projection images while priming.
    // Keep the frame lifecycle and future eye programming running.
    ei.layerCount=imageReleaseFailed||(aerSourceMode&&!s.steamXrAerPairReady)
        ||(nativeBackend&&!s.quadMode&&!nativeFrameValid)?0:layerCount;
    ei.layers=layers.data();
    const std::string layerReason=hudImageCopied?"projection+hud-quad":(s.quadMode?"quad-layer":"projection-layer");
    if(nativeFrameValid)QueryPerformanceCounter(&nativeEndAt);
    if(eyeCaptureRecorded)finishEyeCapture(pv,frame.predictedDisplayTime,nativeBackend);
    if(!s.quadMode&&ei.layerCount)for(int eye=0;eye<2;++eye)traceEye(kharvox::pose_trace::Submitted,eye,pv[eye].pose,pv[eye].fov,frame.predictedDisplayTime,0,steamXrAerPairPath?poseTracePublishedRevisions[eye]:s.stereoCacheRevision[eye],0,nativeBackend?0:(steamXrAerPairPath?poseTracePublishedIds[eye]:poseTraceCachedIds[eye]));
    if(s.quadMode){
        kharvox::pose_trace::Event e{};e.kind=kharvox::pose_trace::QuadSubmitted;
        e.frame=s.frame;e.displayTime=frame.predictedDisplayTime;e.flags=poseTraceFlags();
        e.source=reinterpret_cast<uintptr_t>(quad.space);e.status=s.cinewindowAnchor.valid;
        e.data[0]=quad.pose.position.x;e.data[1]=quad.pose.position.y;e.data[2]=quad.pose.position.z;
        e.data[3]=quad.pose.orientation.x;e.data[4]=quad.pose.orientation.y;
        e.data[5]=quad.pose.orientation.z;e.data[6]=quad.pose.orientation.w;
        e.data[7]=quad.size.width;e.data[8]=quad.size.height;kharvox::pose_trace::record(e);
    }
    r=endFrameChecked(ei,layerReason);
    if(nativeFrameValid)QueryPerformanceCounter(&nativeEndDone);
    if(earlyRelease){
        // The XR call's queue lock has been released before this wait. The
        // private copy fence remains live until this wait; no next frame,
        // readback or temporary framebuffer destruction can pass it.
        LARGE_INTEGER deferredWaitStart{},deferredWaitEnd{};
        QueryPerformanceCounter(&deferredWaitStart);
        {kharvox::native::cpu::Scope copyProfile(kharvox::native::cpu::XrCopyCompletion);
         kharvox::native::cpu::Scope waitProfile(kharvox::native::cpu::XrQueueWait);
         completionResult=s.vk.waitForFences(s.device,1,&copyCompletion,VK_TRUE,UINT64_MAX);}
        QueryPerformanceCounter(&deferredWaitEnd);nativeCompleteAt=deferredWaitEnd;
        steamCopyWaitMs+=performanceMilliseconds(deferredWaitStart,deferredWaitEnd);
        reportNativeDeviceLost("deferredWaitForFences",completionResult);
        copyLifetime.completed(completionResult==VK_SUCCESS);
        if(!copyLifetime.canRetireResources())kharvox::native::fail("early XR release copy completion failed; resources retained");
        if(sfsBackend)kharvox::sfs::copyCompleted(s.device,sfsOwnerCopy,submitResult,completionResult);
        s.handRenderer.finishSceneIntegratedFrame();
    }
    if(nativeFrameValid){
        static uint64_t releaseSamples{};
        static const auto qpcFrequency=[](){LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);return frequency.QuadPart;}();
        if(++releaseSamples<=8||releaseSamples%120==0||r!=XR_SUCCESS)
            log("[NATIVE-XR-RELEASE] frame="+std::to_string(nativeFrame.pose.serial)
                +" runtime="+kharvox::openXRRuntimeKindName(s.runtimeKind)
                +" early="+std::to_string(earlyRelease)+" focused="+std::to_string(s.sessionState==XR_SESSION_STATE_FOCUSED)
                +" submitQpc="+std::to_string(nativeSubmitAt.QuadPart)
                +" qpcFrequency="+std::to_string(qpcFrequency)
                +" submitMs="+std::to_string(performanceMilliseconds(nativeSubmitAt,nativeSubmitDone))
                +" submitToReleaseMs="+std::to_string(performanceMilliseconds(nativeSubmitDone,nativeReleaseAt))
                +" releaseMs="+std::to_string(performanceMilliseconds(nativeReleaseAt,nativeReleaseDone))
                +" submitToEndMs="+std::to_string(performanceMilliseconds(nativeSubmitDone,nativeEndAt))
                +" endFrameMs="+std::to_string(performanceMilliseconds(nativeEndAt,nativeEndDone))
                +" submitToCompletionMs="+std::to_string(performanceMilliseconds(nativeSubmitDone,nativeCompleteAt))
                +" completionVerified="+std::to_string(copyLifetime.canRetireResources())+" end="+result(r));
    }
    if(sfsBackend&&copyLifetime.canRetireResources()){
        double gpuMs{};
        if(s.sfsCopyTiming.completed(gpuMs)){
            static uint64_t samples{};static double total{},maximum{};
            total+=gpuMs;maximum=std::max(maximum,gpuMs);
            if(++samples==120){
                log("[SFS-GPU] xrCommandsMeanMs="+std::to_string(total/120.0)+" xrCommandsMaxMs="+std::to_string(maximum)+" samples=120 scope=copy+hands+hud excludes-prior-game-work");
                samples=0;total=maximum=0;
            }
        }
    }
    if(!sfsBackend&&nativeFrameValid)kharvox::native::xrPresented(nativeFrame.pose.serial,r,!s.quadMode);
    if(XR_SUCCEEDED(r)){
        ++s.submittedLayerFrames;
        if(steamRuntime)s.lastSteamDisplayTime=frame.predictedDisplayTime;
    }
    s.frame++;
    advanceCenteredQuadTransition();
    if(steamRuntime&&(s.frame<=8||s.frame%120==0||steamWaitFrameMs>40.0||steamCopyWaitMs>40.0||r!=XR_SUCCESS))
        log("[STEAM-PERF] frame="+std::to_string(s.frame)+" waitFrameMs="+std::to_string(steamWaitFrameMs)+" copyFenceMs="+std::to_string(steamCopyWaitMs)+" predicted="+std::to_string(frame.predictedDisplayTime)+" period="+std::to_string(frame.predictedDisplayPeriod)+" end="+result(r));
    if(stereoNow&&!nativeBackend){
        const float dx=s.views[1].pose.position.x-s.views[0].pose.position.x,dy=s.views[1].pose.position.y-s.views[0].pose.position.y,dz=s.views[1].pose.position.z-s.views[0].pose.position.z;
        const float halfEyeUnits=0.5f*std::sqrt(dx*dx+dy*dy+dz*dz)*s.worldScale;
        constexpr float radToDeg=57.2957795131f;
        std::array<EyeRenderProjection,2> renderProjections{};
        for(int e=0;e<2;e++){
            renderProjections[e]=eyeRenderProjection(s.views[e].fov);
            const auto& eyeFov=renderProjections[e].symmetricFov;
            const float eyeFovX=(eyeFov.angleRight-eyeFov.angleLeft)*radToDeg,eyeFovY=(eyeFov.angleUp-eyeFov.angleDown)*radToDeg;
            KharvoxCameraConfigureEye(e==0?VREye::Left:VREye::Right,e==0?-halfEyeUnits:halfEyeUnits,eyeFovX,eyeFovY,0.0f,0.0f);
        }
        static bool exactProjectionLogged=false;
        if(!exactProjectionLogged&&!nativePackedStereo&&s.runtimeKind!=kharvox::OpenXRRuntimeKind::VirtualDesktop){
            std::ostringstream projectionInfo;
            projectionInfo<<"[STEREO] exact asymmetric projection ACTIVE; symmetric render FOV left/right="
                <<(renderProjections[0].symmetricFov.angleRight-renderProjections[0].symmetricFov.angleLeft)*radToDeg<<'/'
                <<(renderProjections[1].symmetricFov.angleRight-renderProjections[1].symmetricFov.angleLeft)*radToDeg
                <<" vertical="<<(renderProjections[0].symmetricFov.angleUp-renderProjections[0].symmetricFov.angleDown)*radToDeg<<'/'
                <<(renderProjections[1].symmetricFov.angleUp-renderProjections[1].symmetricFov.angleDown)*radToDeg;
            log(projectionInfo.str());
            exactProjectionLogged=true;
        }
        if(nativePackedStereo){
            const float fovX=(commonEyeFov.angleRight-commonEyeFov.angleLeft)*radToDeg,fovY=(commonEyeFov.angleUp-commonEyeFov.angleDown)*radToDeg;
            KharvoxCameraSetNativeStereoParameters(halfEyeUnits,0.0f);
            KharvoxCameraSetStereoEye(0,fovX,fovY,0,0,true);
        }else{
            // DOOM's frontend/backend queue may retire more than one image
            // after a new eye camera is programmed. Hold each requested eye
            // for three Presents and cache only the third. The intervening
            // images feed both eyes equally, avoiding a falsely labelled HUD
            // or weapon frame while the render pipeline and native transforms
            // settle after loading.
            constexpr unsigned pipelineSettleFrames=3;
            if(s.alternatingStereoSkipInitialCapture){
                s.alternatingStereoSkipInitialCapture=false;
                s.alternatingStereoWarmupEye=kharvox::aerFirstRenderEye;
                s.alternatingStereoWarmupFramesRemaining=pipelineSettleFrames;
                s.renderEye=kharvox::aerFirstRenderEye;
                log("[HUD22-AER] mono transition submitted; right eye pipeline settle started");
            }else if(s.alternatingStereoWarmupActive){
                if(s.alternatingStereoWarmupFramesRemaining>1){
                    --s.alternatingStereoWarmupFramesRemaining;
                    s.renderEye=s.alternatingStereoWarmupEye;
                }else if(s.alternatingStereoWarmupEye==kharvox::aerFirstRenderEye){
                    s.alternatingStereoWarmupEye=kharvox::aerSecondRenderEye;
                    s.alternatingStereoWarmupFramesRemaining=pipelineSettleFrames;
                    s.renderEye=kharvox::aerSecondRenderEye;
                    log("[HUD22-AER] settled right cache captured; left eye pipeline settle started");
                }else{
                    s.alternatingStereoWarmupActive=false;
                    s.alternatingStereoWarmupFramesRemaining=0;
                    s.renderEye=scheduledRenderEye^1;
                    log("[HUD22-AER] pipeline-settled left/right cache pair complete");
                }
            }else{
                s.renderEye=scheduledRenderEye^1;
            }
            const bool steamXrAerNextPair=kharvox::useSteamXrCoherentAerPair(
                s.runtimeKind,coherentAerPairPresentationContext,stereoNow,
                nativePackedStereo,nvidiaAfwMarker,
                s.alternatingStereoWarmupActive,
                s.immersiveCinematicActive||s.nativeAdaptiveParticipantGuardActive);
            if(steamXrAerNextPair&&s.renderEye==kharvox::aerFirstRenderEye){
                for(int e=0;e<2;e++){
                    s.steamXrAerCapturePose[e]=s.views[e].pose;
                    s.steamXrAerCaptureFov[e]=submittedEyeFov(s.views[e].fov);
                }
                // Keep the raw located pair here. Source-qualified capture
                // applies the cinematic anchor once, using that source centre.
                s.steamXrAerCaptureEyesReady={};
                s.steamXrAerCaptureValid=true;
                s.steamXrAerCaptureArtificialTurn=
                    s.artificialTurnTotalDegrees;
            }else if(!steamXrAerNextPair){
                s.steamXrAerCaptureValid=false;
                s.steamXrAerCaptureEyesReady={};
            }
            const auto& nextProjection=renderProjections[s.renderEye];
            const auto& nextRenderFov=nextProjection.symmetricFov;
            s.programmedEyePose[s.renderEye]=steamXrAerNextPair
                    &&s.steamXrAerCaptureValid
                ?s.steamXrAerCapturePose[s.renderEye]
                :s.views[s.renderEye].pose;
            s.programmedEyeFov[s.renderEye]=steamXrAerNextPair
                    &&s.steamXrAerCaptureValid
                ?s.steamXrAerCaptureFov[s.renderEye]
                :submittedEyeFov(s.views[s.renderEye].fov);
            s.programmedEyeArtificialTurn[s.renderEye]=s.artificialTurnTotalDegrees;
            s.programmedEyeSnapGeneration[s.renderEye]=s.snapTurnGeneration;
            s.programmedEyeViewValid[s.renderEye]=true;
            const float fovX=(nextRenderFov.angleRight-nextRenderFov.angleLeft)*radToDeg,fovY=(nextRenderFov.angleUp-nextRenderFov.angleDown)*radToDeg;
            KharvoxCameraSetStereoEye(kharvox::aerDoomEyeOffset(s.renderEye,halfEyeUnits),fovX,fovY,0.0f,0.0f,true);
        }
    }
    const bool animatedSequenceActive=
        s.nativeAdaptiveParticipantGuardActive||s.immersiveCinematicActive;
    KharvoxCameraSetAnimatedSequenceActive(animatedSequenceActive);
    const bool synchronizeAnimatedAerPair=s.alternatingStereo&&!nativePackedStereo
        &&!s.quadMode&&animatedSequenceActive;
    const bool synchronizeSteamXrAerPair=kharvox::useSteamXrCoherentAerPair(
        s.runtimeKind,coherentAerPairPresentationContext,s.alternatingStereo,
        nativePackedStereo,nvidiaAfwMarker,s.alternatingStereoWarmupActive,
        s.immersiveCinematicActive||s.nativeAdaptiveParticipantGuardActive)
        &&s.steamXrAerCaptureValid;
    const bool synchronizeAerPair=synchronizeAnimatedAerPair
        ||synchronizeSteamXrAerPair;
    if(!sfsBackend){
        KharvoxCameraSetAerRenderPair(s.renderEye,synchronizeAerPair);
        KharvoxWeaponSetAerRenderPair(s.renderEye,synchronizeAerPair,KharvoxCameraDiagnosticPoseId());
    }
    if(s.alternatingStereo&&!nativePackedStereo&&s.programmedEyeViewValid[s.renderEye]){
        poseTraceProgrammedIds[s.renderEye]=KharvoxCameraDiagnosticPoseId();
        const auto inputPoseId=poseTraceProgrammedIds[s.renderEye];
        if(s.aerPairInputPoseId!=inputPoseId){
            s.aerPairInputPoseId=inputPoseId;s.aerPairInputCenter=locatedMonoPose;
            s.aerPairInputLeft=s.leftGripController;s.aerPairInputRight=s.rightGripController;
        }
        s.aerInputHistory.remember({inputPoseId,KharvoxCameraLevelTransitionGeneration(),s.renderEye},
            {s.programmedEyePose[s.renderEye],s.programmedEyeFov[s.renderEye],
             s.aerPairInputLeft,s.aerPairInputRight,s.programmedEyeArtificialTurn[s.renderEye],s.programmedEyeSnapGeneration[s.renderEye],s.aerPairInputCenter,laserBodyTrackingTransform()});
        traceEye(kharvox::pose_trace::Programmed,s.renderEye,s.programmedEyePose[s.renderEye],eyeRenderProjection(s.programmedEyeFov[s.renderEye]).symmetricFov,frame.predictedDisplayTime,0,0,0,poseTraceProgrammedIds[s.renderEye]);
    }
    if(XR_SUCCEEDED(r)&&s.submittedLayerFrames==1)log("Frame 1 submitted mode="+(s.quadMode?std::string("QUAD"):std::string("PROJECTION"))+" result="+result(r), true);
    if(XR_SUCCEEDED(r)&&s.submittedLayerFrames==120){
        log("Frame 120 stable mode="+(s.quadMode?std::string("QUAD"):std::string("PROJECTION"))+" result="+result(r), true);
        log("[WINDOW-FOCUS] automatic foreground restoration disabled after startup; launcher status remains available for manual focus");
    }
    if(steamLinkSameFrameMono){
        const float monoRenderFovX=(locatedImmersiveRenderFov.angleRight-locatedImmersiveRenderFov.angleLeft)*radiansToDegrees;
        const float monoRenderFovY=(locatedImmersiveRenderFov.angleUp-locatedImmersiveRenderFov.angleDown)*radiansToDegrees;
        KharvoxCameraSetStereoEye(0.0f,monoRenderFovX,monoRenderFovY,0.0f,0.0f,true);
        if(!s.steamLinkMonoCameraActive)log("[STEAMLINK-MONO] central DOOM camera FOV locked to OpenXR enclosing projection "+std::to_string(monoRenderFovX)+"x"+std::to_string(monoRenderFovY));
        s.steamLinkMonoCameraActive=true;
    }else if(s.steamLinkMonoCameraActive){
        KharvoxCameraSetStereoEye(0,0,0,0,0,false);
        s.steamLinkMonoCameraActive=false;
        log("[STEAMLINK-MONO] central camera projection lock released");
    }
}

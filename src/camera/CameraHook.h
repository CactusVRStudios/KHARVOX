#pragma once
#include "AerWorldViewHistory.h"
#include "../common/AerSourceTracking.h"

enum class VREye : int {
    Mono = 0,
    Left = 1,
    Right = 2
};

bool KharvoxCameraInstallDiagnosticHook();
void KharvoxCameraFinishNativeStereoBootstrap();
void KharvoxCameraCompleteNativeStereoBootstrapAfterRender();
void KharvoxCameraPollDiagnostic();
void KharvoxCameraSetHeadPose(
    float yawDegrees, float pitchDegrees, float rollDegrees,
    float forwardUnits, float lateralUnits, float upUnits,
    bool valid, float referenceBodyYaw = 0.0f, bool referenceBodyValid = false);
void KharvoxCameraSetArtificialTurnYaw(float yawDegrees);
unsigned long long KharvoxCameraDiagnosticPoseId();
// Copies only recognized, observed gameplay cameras; caller owns engine writes.
kharvox::AerWorldViewResult KharvoxCameraAlignAerWorldView(const float pose[12],const float fov[2],
    kharvox::AerWorldView& source,kharvox::AerWorldView& target);
kharvox::AerWorldViewResult KharvoxCameraRecognizeAerWorldView(const float pose[12],const float fov[2],kharvox::AerWorldView& source);
void KharvoxCameraRecordAerWorldSource(const kharvox::AerWorldView& source);
bool KharvoxCameraUsesAerGameplaySource();
kharvox::AerSourceObservation KharvoxCameraTakeAerWorldSource();
void KharvoxCameraSetCrouchState(bool active);
void KharvoxCameraSetImmersiveCinematicFov(
    float fovXDegrees, float fovYDegrees,
    bool requested, bool active);
void KharvoxCameraSetImmersiveCinematicFreelook(bool requested);
bool KharvoxCameraWorldActive();
bool KharvoxCameraGameplayActive();
bool KharvoxCameraNativeStereoActive();
bool KharvoxCameraNativeTwoViewActive();
bool KharvoxCameraCutsceneActive();
bool KharvoxCameraBossSequenceActive();
bool KharvoxCameraSyncAttackActive();
bool KharvoxCameraLedgeTransitionActive();
// True only when DOOM's native player command tracker currently permits both
// view control and weapon buttons. This distinguishes playable first-person
// scripted intervals from passive scenes which merely render a weapon model.
bool KharvoxCameraPlayerWeaponControlActive();
bool KharvoxCameraGetBodyPose(float origin[3], float axis[9]);
bool KharvoxCameraGetHeadRenderPose(float origin[3], float axis[9]);
bool KharvoxCameraGetHudCenterRenderPose(float origin[3], float axis[9]);
unsigned long long KharvoxCameraCurrentPresentSerial();
unsigned long long KharvoxCameraLevelTransitionGeneration();
bool KharvoxCameraGetPlayerPhysicsOrigin(float origin[3]);
void KharvoxCameraSetStereoEye(float localXUnits, float fovXDegrees, float fovYDegrees, float opticalCenterYawDegrees, float opticalCenterPitchDegrees, bool enabled);
// Separates an authored animated-camera interval from ordinary gameplay. The
// gameplay body is gravity-level at all other times; physical HMD pitch/roll is
// still applied independently after that body pose is published.
void KharvoxCameraSetAnimatedSequenceActive(bool active);
// Programs the eye which DOOM will render next and enables native camera-pose
// pairing for animated AER sequences. The base cinematic/gameplay camera pose
// is shared by the two eyes; IPD and the current HMD pose remain per-eye.
void KharvoxCameraSetAerRenderPair(int renderEye, bool enabled);
void KharvoxCameraSetNativeStereoParameters(float halfEyeSeparationUnits, float screenSeparation);
void KharvoxCameraConfigureEye(VREye eye, float localXUnits, float fovXDegrees, float fovYDegrees,
    float opticalCenterYawDegrees, float opticalCenterPitchDegrees);
bool KharvoxSameFrameStereoInstalled();
VREye KharvoxCameraCurrentEye();

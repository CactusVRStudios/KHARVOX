#include "../common/DiagnosticLogging.h"
#include "HudHook.h"
#include "OffhandHudPolicy.h"
#include "TutorialRenderPolicy.h"
#include "TutorialBindingText.h"
#include "HudLayoutPolicy.h"
#include "HudMenuPolicy.h"

#include "../camera/CameraHook.h"
#include "../common/RuntimePaths.h"

#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <map>
#include <sstream>
#include <vector>

bool KharvoxHudPrepareOriginTransform(void* entity, float* nativeOrigin);

namespace {
bool readableRange(const void* address, size_t bytes);

std::string calibrationPath() { return kharvox::runtimePathA("hud_role_calibration_saved.cfg"); }
std::string identityPath() { return kharvox::runtimePathA("hud_roles_saved.cfg"); }
std::string flatCalibrationPath() { return kharvox::runtimePathA("hud_flat_calibration_saved.cfg"); }
std::string flatCalibrationDefaultPath() { return kharvox::runtimePathA("hud_flat_calibration_default.cfg"); }
std::string profileCalibrationPath() { return kharvox::runtimePathA("hud_profile_calibration_saved.cfg"); }
std::string profileCalibrationDefaultPath() { return kharvox::runtimePathA("hud_profile_calibration_default.cfg"); }
constexpr int hudRoleCount = 6;
constexpr std::array<const char*, hudRoleCount> hudRoleNames{
    "WEAPON HUD", "HEALTH HUD", "COMPASS", "WEAPON WHEEL", "DOSSIER/MAP", "AUXILIARY UI"
};
constexpr std::array<std::array<float, 4>, hudRoleCount> defaultRoleCalibration{{
    {-6.0f, 0.0f, 5.0f, 1.0f},
    {-2.0f, 0.0f, 4.0f, 1.0f},
    {45.0f, 0.0f, 12.0f, 1.0f},
    {45.0f, 0.0f, 0.0f, 1.0f},
    {50.0f, 0.0f, 0.0f, 1.0f},
    {45.0f, 0.0f, 0.0f, 1.0f}
}};

struct RoleCalibration {
    std::array<std::atomic<float>, 3> offset{};
    std::atomic<float> scale{1.0f};
};

std::array<RoleCalibration, hudRoleCount> roleCalibrations{};
int selectedCalibrationRole{2};
std::atomic<bool> installed{};
constexpr float defaultWorldUnitsPerMeter = 39.3701f;
constexpr float defaultHudDistanceMeters = 1.0f;
constexpr float defaultHudQuadScale = 3.0f;
constexpr float defaultHudHorizontalOffsetTan = 0.0f;
constexpr float minimumHudDistanceMeters = 0.11f;
constexpr float maximumHudDistanceMeters = 10.0f;
constexpr float minimumHudScale = 0.10f;
constexpr float maximumHudScale = 30.0f;
constexpr float minimumHudHorizontalOffsetTan = -1.0f;
constexpr float maximumHudHorizontalOffsetTan = 1.0f;
constexpr float minimumHudLayoutScale = 0.005f;
constexpr float maximumHudLayoutScale = 20.0f;
// 3.0x is the physical Quest-tested user reference. HudLayoutPolicy keeps its
// angular layout stable between runtimes and display resolutions.
constexpr float hudReferenceUserScale = 3.0f;
// Independently measured HUD-depth limits. The supported game places its
// screen-space gameplay HUD origins inside this narrow camera-depth band;
// ordinary in-world GUI surfaces live outside it.
constexpr float nativeHudNearDepth = 5.9f;
constexpr float nativeHudFarDepth = 10.0f;
constexpr float nativeHudReferenceDepth = (nativeHudNearDepth + nativeHudFarDepth) * 0.5f;
std::atomic<float> hudDistanceMeters{defaultHudDistanceMeters};
std::atomic<float> hudQuadScale{defaultHudQuadScale};
std::atomic<float> hudHorizontalOffsetTan{defaultHudHorizontalOffsetTan};
std::atomic<float> hudHeadsetFitScale{kharvox::calibratedHudLayoutFit};
std::atomic<std::uint32_t> hudHeadsetSurfaceWidth{};
std::atomic<std::uint32_t> hudHeadsetSurfaceHeight{};
float hudWorldUnitsPerMeter{defaultWorldUnitsPerMeter};
float hudLauncherDistanceMeters{defaultHudDistanceMeters};
float hudLauncherScale{defaultHudQuadScale};
float hudLauncherHorizontalOffsetTan{defaultHudHorizontalOffsetTan};
alignas(8) volatile LONG64 capturedCrosshairContext{};
void* crosshairCaptureStub{};

struct HudCalibrationSnapshot {
    float distanceMeters{defaultHudDistanceMeters};
    float elementScale{defaultHudQuadScale};
    float horizontalOffsetTan{defaultHudHorizontalOffsetTan};
    float headsetFitScale{kharvox::calibratedHudLayoutFit};
};

std::mutex hudFrameCalibrationMutex;
unsigned long long hudFrameCalibrationPresent{~0ull};
HudCalibrationSnapshot hudFrameCalibration{};

struct PendingHudSubmission {
    bool active{};
    bool crosshair{};
    bool offhand{};
    float offhandWidth{};
    std::array<float,9> offhandAxis{};
    bool offscreen{};
    uintptr_t context{};
    void* expectedFinalEntity{};
    int profileIndex{-1};
    std::array<float, 3> contextOrigin{};
    float contextScale{};
    float profileYawDegrees{};
    std::array<float, 3> desiredOrigin{};
    std::array<float, 9> headAxis{};
};

constexpr size_t maximumPendingHudDepth = 16;
thread_local std::array<PendingHudSubmission, maximumPendingHudDepth> pendingHudStack{};
thread_local size_t pendingHudDepth{};

// Cache each native HUD surface profile's authored camera-local transform once
// and rebuild it from the live head pose thereafter. The key includes caller,
// dimensions and native scale, so Health, Ammo, Compass, subtitles, dialogue
// and notifications remain separate without requiring manual role marking.
// At the same time both AER eyes and any recreated render entity resolve to the
// same Flat-game placement instead of intermittently selecting another pose.
struct FlatHudProfilePose {
    unsigned long long lastSeenTick{};
    bool originValid{};
    std::array<float, 3> nativeLocalOrigin{};
    bool axisValid{};
    std::array<float, 9> localAxisDirections{};
};

constexpr size_t maximumGuiProfiles = 128;
std::array<FlatHudProfilePose, maximumGuiProfiles> flatHudProfilePoses{};
std::mutex flatHudProfilePoseMutex;

struct TrackedHandPose {
    std::array<std::atomic<float>, 3> grip{};
    std::array<std::atomic<float>, 4> quaternion{};
    std::atomic<bool> valid{};
};

std::array<TrackedHandPose, 2> handPoses{};
std::mutex handPoseMutex;
std::mutex offhandHudMutex;
bool offhandCanvasHookReady{};
struct OffhandCanvasSubmission {const void* entity{};float center[3]{},axis[9]{},width{};};
thread_local OffhandCanvasSubmission offhandCanvasSubmission;
thread_local const void* suppressedOffhandCanvas{};

kharvox::OffhandHudConfig offhandHudConfig;
std::atomic<bool> offhandCalibrationActive{};
std::atomic<int> offhandSelectedSurface{};
std::atomic<unsigned long long> offhandSelectionUntil{};
bool offhandHudLeftMode(){static const bool value=[](){char text[16]{};return GetEnvironmentVariableA("KHARVOX_LEFT_HANDED",text,sizeof(text))&&std::strcmp(text,"0");}();return value;}
void reloadOffhandHud(){
    static unsigned long long last{};const auto now=GetTickCount64();if(last&&now-last<500)return;last=now;
    const auto attributes=GetFileAttributesA(kharvox::runtimePathA("enable_live_ammo_calibration").c_str());
    offhandCalibrationActive.store(attributes!=INVALID_FILE_ATTRIBUTES&&!(attributes&FILE_ATTRIBUTE_DIRECTORY),std::memory_order_release);
    std::ifstream file(kharvox::runtimePathA("offhand_hud.cfg"));kharvox::OffhandHudConfig next;
    if(kharvox::readOffhandHudConfig(file,next)){std::lock_guard<std::mutex> lock(offhandHudMutex);offhandHudConfig=next;}
}


struct GuiProfile {
    uintptr_t callerRva{};
    int width{};
    int height{};
    int scaleMilli{};
    unsigned long long lastSeenTick{};
    float lateralOffsetTan{};
    float verticalOffsetTan{};
    float distanceOffsetMeters{};
    float yawDegrees{};
    bool builtInExcluded{};
    bool userExcluded{};
};

struct SavedGuiProfileCalibration {
    GuiProfile identity{};
    float lateralOffsetTan{};
    float verticalOffsetTan{};
    float distanceOffsetMeters{};
    float yawDegrees{};
    bool excluded{};
};

struct HudProfileAdjustment {
    float lateralOffsetTan{};
    float verticalOffsetTan{};
    float distanceOffsetMeters{};
    float yawDegrees{};
    bool centerPreview{};
};

std::array<GuiProfile, maximumGuiProfiles> guiProfiles{};
int guiProfileCount{};
int selectedGuiProfile{-1};
unsigned long long selectedGuiProfileCenterUntilTick{};
std::array<SavedGuiProfileCalibration, maximumGuiProfiles> savedGuiProfileCalibrations{};
int savedGuiProfileCalibrationCount{};
std::array<GuiProfile, hudRoleCount> savedHudRoles{};
std::array<bool, hudRoleCount> savedHudRoleValid{};
bool divideWasDown{}, excludeWasDown{};
bool hudDebuggingEnabled{};
std::mutex guiProfileMutex;
std::atomic<unsigned long long> fullscreenMenuLastSeenTick{};
std::atomic<unsigned long long> fieldDroneMenuSessionUntilTick{};
std::atomic<bool> fieldDroneNativeActivatorBound{};
std::atomic<unsigned long long> suitUpgradeMenuSessionUntilTick{};
std::atomic<bool> suitUpgradeNativeSessionActive{};
std::atomic<bool> pauseMenuActive{};
std::atomic<bool> pauseRootVisible{};
std::atomic<bool> deathMenuActive{};
std::atomic<unsigned long long> deathMenuLevelGenerationAtShow{};
std::atomic<unsigned int> endOfLevelScreenMask{};
std::atomic<unsigned long long> endOfLevelMenuGenerationAtShow{};
std::atomic<unsigned int> playerUpgradeScreenMask{};
std::atomic<unsigned long long> playerUpgradeMenuGenerationAtShow{};
std::atomic<bool> runeSelectMenuActive{};
std::atomic<unsigned long long> runeSelectMenuGenerationAtShow{};
std::atomic<bool> runeChallengeScreenActive{};
std::atomic<unsigned long long> runeChallengeScreenGenerationAtShow{};
std::atomic<unsigned long long> runeChallengeScreenSessionUntilTick{};
std::atomic<bool> runeChallengePauseRecoveryArmed{};
std::atomic<unsigned long long> runeChallengeGenerationAtPause{};
std::atomic<bool> runeChallengeMapLoadPending{};
std::atomic<unsigned long long> runeChallengeGenerationAtLoad{};
std::atomic<int> runeChallengePendingLoadCommand{-1};
std::atomic<bool> runeTrialNativeSessionActive{};
std::atomic<unsigned long long> runeTrialMenuSessionUntilTick{};
std::atomic<unsigned long long> runeTrialMenuGenerationAtActivation{};
std::atomic<bool> runeTrialChallengeLoadPending{};
std::atomic<unsigned long long> playerUpgradeManagerLastFrameTick{};
std::atomic<bool> playerUpgradePickupSessionActive{};
std::atomic<unsigned long long> playerUpgradePickupConfirmedTick{};
std::atomic<bool> playerUpgradePickupCinematicObserved{};
std::atomic<bool> playerUpgradePickupFallbackQuadLogged{};
std::atomic<unsigned long long> playerUpgradePickupSessionStartTick{};
std::atomic<unsigned long long> playerUpgradePickupSessionUntilTick{};
std::atomic<bool> upgradeCinematicRefreshGuardActive{};
std::atomic<unsigned long long> upgradeCinematicRefreshGuardStartTick{};
std::atomic<unsigned long long> upgradeCinematicRefreshGuardUntilTick{};
std::atomic<unsigned int> tutorialScreenMask{};
std::atomic<unsigned long long> tutorialManagerLastActiveFrameTick{};
std::atomic<unsigned long long> tutorialSessionUntilTick{};
std::atomic<unsigned long long> diagnosticMatchedSurfaces{};
std::atomic<unsigned long long> diagnosticCompletedSurfaces{};
std::atomic<unsigned long long> diagnosticCrosshairSurfaces{};
std::atomic<unsigned long long> diagnosticLastMatchQpc{};
std::atomic<unsigned int> diagnosticLastThreadId{};
std::atomic<uintptr_t> diagnosticLastCallerRva{};
std::atomic<int> diagnosticLastWidth{};
std::atomic<int> diagnosticLastHeight{};
std::atomic<int> diagnosticLastScaleMilli{};
std::atomic<bool> diagnosticLastWasCrosshair{};
struct DiagnosticEventSlot {
    std::atomic<unsigned long long> serial{};
    std::atomic<unsigned long long> qpc{};
    std::atomic<unsigned int> threadId{};
    std::atomic<uintptr_t> callerRva{};
    std::atomic<int> width{};
    std::atomic<int> height{};
    std::atomic<int> scaleMilli{};
    std::atomic<bool> crosshair{};
};
constexpr size_t diagnosticEventCapacity = 512;
std::array<DiagnosticEventSlot, diagnosticEventCapacity> diagnosticEvents{};

using PauseScreenTransitionFn = void(__fastcall*)(void*, int);
using FieldDroneActivatorFn = void(__fastcall*)(void*, void*);
using SuitUpgradeActivateFn = void(__fastcall*)(void*, void*);
using SuitUpgradeReleaseFn = void*(__fastcall*)(void*, void*, void*);
using PlayerUpgradeManagerFrameFn = void(__fastcall*)(void*, int);
using HudNotifyFn = void(__fastcall*)(void*, void*, void*, unsigned char);
using EliteGuardActivateFn = bool(__fastcall*)(void*, void*, unsigned int);
using VegaTrainingActivateFn = bool(__fastcall*)(void*, void*, unsigned int);
using VegaTrainingEventFn = void*(__fastcall*)(void*, void*, void*);
using VegaTrainingUseFn = void*(__fastcall*)(void*, void*);
using RunePopupHandleActionFn = bool(__fastcall*)(void*, void*);
using RuneChallengeHandleActionFn = bool(__fastcall*)(void*, void*);
PauseScreenTransitionFn originalPauseScreenShow{};
PauseScreenTransitionFn originalPauseScreenHide{};
PauseScreenTransitionFn originalCampaignDeathShow{};
PauseScreenTransitionFn originalCampaignDeathHide{};
PauseScreenTransitionFn originalEndOfLevelShow{};
PauseScreenTransitionFn originalEndOfLevelHide{};
PauseScreenTransitionFn originalDossierEndOfLevelShow{};
PauseScreenTransitionFn originalDossierEndOfLevelHide{};
PauseScreenTransitionFn originalEndOfLevelDossierShow{};
PauseScreenTransitionFn originalEndOfLevelDossierHide{};
PauseScreenTransitionFn originalPlayerUpgradeShow{};
PauseScreenTransitionFn originalPlayerUpgradeHide{};
PauseScreenTransitionFn originalArgentSelectionShow{};
PauseScreenTransitionFn originalArgentSelectionHide{};
PauseScreenTransitionFn originalDossierSuitShow{};
PauseScreenTransitionFn originalDossierSuitHide{};
PauseScreenTransitionFn originalDossierSuitDiagShow{};
PauseScreenTransitionFn originalDossierSuitDiagHide{};
PauseScreenTransitionFn originalUpgradeStationSelectionShow{};
PauseScreenTransitionFn originalUpgradeStationSelectionHide{};
PauseScreenTransitionFn originalUpgradeStationLeftShow{};
PauseScreenTransitionFn originalUpgradeStationLeftHide{};
PauseScreenTransitionFn originalUpgradeStationRightShow{};
PauseScreenTransitionFn originalUpgradeStationRightHide{};
PauseScreenTransitionFn originalPlayerUpgradeIdleShow{};
PauseScreenTransitionFn originalPlayerUpgradeIdleHide{};
PauseScreenTransitionFn originalPlayerUpgradeInUseShow{};
PauseScreenTransitionFn originalPlayerUpgradeInUseHide{};
PauseScreenTransitionFn originalPlayerUpgradeUsedShow{};
PauseScreenTransitionFn originalPlayerUpgradeUsedHide{};
PauseScreenTransitionFn originalRuneSelectShow{};
PauseScreenTransitionFn originalRuneSelectHide{};
PauseScreenTransitionFn originalRuneChallengeShow{};
PauseScreenTransitionFn originalRuneChallengeHide{};
PauseScreenTransitionFn originalTutorialSimpleShow{};
PauseScreenTransitionFn originalTutorialSimpleHide{};
PauseScreenTransitionFn originalTutorialVideoShow{};
PauseScreenTransitionFn originalTutorialVideoHide{};
PauseScreenTransitionFn originalTutorialTextShow{};
PauseScreenTransitionFn originalTutorialTextHide{};
FieldDroneActivatorFn originalFieldDroneBindActivator{};
FieldDroneActivatorFn originalFieldDroneReleaseActivator{};
SuitUpgradeActivateFn originalSuitUpgradeActivate{};
SuitUpgradeReleaseFn originalSuitUpgradeRelease{};
PlayerUpgradeManagerFrameFn originalPlayerUpgradeManagerFrame{};
PlayerUpgradeManagerFrameFn originalTutorialManagerFrame{};
HudNotifyFn originalHudNotify{};
EliteGuardActivateFn originalEliteGuardActivate{};
VegaTrainingActivateFn originalVegaTrainingActivate{};
VegaTrainingEventFn originalVegaTrainingShowRuneMenu{};
VegaTrainingEventFn originalVegaTrainingReleaseActivator{};
VegaTrainingEventFn originalVegaTrainingDeactivate{};
VegaTrainingUseFn originalVegaTrainingUse{};
RunePopupHandleActionFn originalRunePopupHandleAction{};
RuneChallengeHandleActionFn originalRuneChallengeHandleAction{};

constexpr unsigned long long fullscreenMenuHoldMilliseconds = 500;
constexpr unsigned long long fieldDroneMenuSessionMilliseconds = 60000;
constexpr unsigned long long suitUpgradeMenuSessionMilliseconds = 300000;
constexpr unsigned long long runeTrialMenuSessionMilliseconds = 300000;
constexpr unsigned long long playerUpgradeManagerFrameHoldMilliseconds = 250;
constexpr unsigned long long playerUpgradePickupEntryGraceMilliseconds = 1000;
constexpr unsigned long long playerUpgradePickupSessionMilliseconds = 300000;
constexpr unsigned long long upgradeCinematicRefreshGuardMinimumMilliseconds = 8000;
constexpr unsigned long long upgradeCinematicRefreshGuardSafetyMilliseconds = 30000;
constexpr unsigned long long tutorialManagerFrameHoldMilliseconds = 250;
constexpr unsigned long long tutorialSessionMilliseconds = 300000;

void log(const std::string& text) {
    if (!kharvox::extendedDiagnosticsEnabled()) return;
    char temp[MAX_PATH]{};
    GetTempPathA(MAX_PATH, temp);
    std::ofstream out(std::string(temp) + "KHARVOX.log", std::ios::app);
    out << "[KHARVOX][HUD] " << text << '\n';
}

void pulsePlayerUpgradeManagerQuadSession() {
    const auto now = GetTickCount64();
    const auto previous = playerUpgradeManagerLastFrameTick.exchange(
        now, std::memory_order_acq_rel);
    if (!kharvox::shouldKeepPulsedMenuSession(
            previous, now, playerUpgradeManagerFrameHoldMilliseconds)) {
        playerUpgradeMenuGenerationAtShow.store(
            KharvoxCameraLevelTransitionGeneration(), std::memory_order_release);
        log("native PlayerUpgrade manager frame; Drone-style centered full-frame Quad/UI session started");
    }
}

void pulseTutorialActiveSession() {
    const auto now = GetTickCount64();
    const auto previous = tutorialManagerLastActiveFrameTick.exchange(
        now, std::memory_order_acq_rel);
    tutorialSessionUntilTick.store(
        now + tutorialSessionMilliseconds, std::memory_order_release);
    if (!kharvox::shouldKeepPulsedMenuSession(
            previous, now, tutorialManagerFrameHoldMilliseconds)
        && !tutorialScreenMask.load(std::memory_order_acquire)) {
        log("native Tutorial manager active-screen frame; Projection exclusion active (no Quad/camera lock)");
    }
}

void beginPlayerUpgradePickupSession(const char* nativeSource) {
    const auto now = GetTickCount64();
    const bool alreadyActive = playerUpgradePickupSessionActive.exchange(
        true, std::memory_order_acq_rel);
    playerUpgradePickupSessionUntilTick.store(
        now + playerUpgradePickupSessionMilliseconds,
        std::memory_order_release);
    if (!alreadyActive) {
        playerUpgradePickupConfirmedTick.store(0, std::memory_order_release);
        playerUpgradePickupSessionStartTick.store(now, std::memory_order_release);
        playerUpgradePickupCinematicObserved.store(false, std::memory_order_release);
        playerUpgradePickupFallbackQuadLogged.store(false, std::memory_order_release);
        playerUpgradeMenuGenerationAtShow.store(
            KharvoxCameraLevelTransitionGeneration(), std::memory_order_release);
        log(std::string(nativeSource)
            + "; Elite Guard pickup armed (animation remains immersive; actual Praetor menu owns centered Quad)");
    }
}

void beginUpgradeCinematicRefreshGuard() {
    const auto now = GetTickCount64();
    upgradeCinematicRefreshGuardStartTick.store(now, std::memory_order_release);
    upgradeCinematicRefreshGuardUntilTick.store(
        now + upgradeCinematicRefreshGuardSafetyMilliseconds,
        std::memory_order_release);
    if (!upgradeCinematicRefreshGuardActive.exchange(
            true, std::memory_order_acq_rel)) {
        log("native VEGA/Argent upgrade accepted; refresh-only handoff guard armed; AER stereo remains enabled");
    }
}

void appendAbsoluteJump(unsigned char* destination, const void* address) {
    destination[0] = 0xFF;
    destination[1] = 0x25;
    std::memset(destination + 2, 0, 4);
    const auto absolute = reinterpret_cast<unsigned long long>(address);
    std::memcpy(destination + 6, &absolute, sizeof(absolute));
}

void appendAbsoluteCall(unsigned char* destination, const void* address) {
    destination[0] = 0x48;
    destination[1] = 0xB8;
    const auto absolute = reinterpret_cast<unsigned long long>(address);
    std::memcpy(destination + 2, &absolute, sizeof(absolute));
    destination[10] = 0xFF;
    destination[11] = 0xD0;
}

extern "C" void __fastcall setRenderEntityOriginHudHook(
    void* rawEntity, const float* nativeOrigin) {
    auto entity = static_cast<unsigned char*>(rawEntity);
    const auto image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto returnAddress = reinterpret_cast<uintptr_t>(_ReturnAddress());
    if (image && returnAddress - image == 0xF91F58)
        KharvoxHudPrepareOriginTransform(rawEntity, const_cast<float*>(nativeOrigin));

    // Complete DOOM's small native origin setter directly. HUD5 hooks this
    // function at entry so the modified origin is propagated by the engine
    // before its untouched axis and final-entity calculations run.
    const unsigned char lockMask = entity[0x71];
    if ((entity[0x70] & lockMask) == 0)
        std::memcpy(entity + 0xC8, nativeOrigin, sizeof(float) * 3);
    std::memcpy(entity + 0x78, nativeOrigin, sizeof(float) * 3);
}

bool installHudOriginHook() {
    auto image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!image) return false;
    auto target = image + 0x3B7500;
    constexpr std::array<unsigned char, 17> signature{
        0x0F,0xB6,0x41,0x71,0x84,0x41,0x70,0x75,0x1A,
        0x8B,0x02,0x89,0x81,0xC8,0x00,0x00,0x00
    };
    if (std::memcmp(target, signature.data(), signature.size())) {
        log("HUD5 origin-setter signature mismatch at RVA 0x3B7500; HUD transform disabled");
        return false;
    }
    DWORD oldProtect{};
    if (!VirtualProtect(target, signature.size(), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        log("HUD5 origin-setter patch protection failed");
        return false;
    }
    std::array<unsigned char, signature.size()> patch{};
    patch.fill(0x90);
    appendAbsoluteJump(patch.data(), reinterpret_cast<const void*>(&setRenderEntityOriginHudHook));
    std::memcpy(target, patch.data(), patch.size());
    FlushInstructionCache(GetCurrentProcess(), target, patch.size());
    DWORD ignored{};
    VirtualProtect(target, patch.size(), oldProtect, &ignored);
    log("HUD5 origin-only hook installed at idRenderEntity::SetOrigin RVA 0x3B7500 (DOOMHUDPush return RVA 0xF91F58)");
    return true;
}

bool installCrosshairCaptureHook() {
    auto image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!image) return false;
    auto target = image + 0xC317BE;
    constexpr std::array<unsigned char, 16> signature{
        0x41, 0x8B, 0x85, 0xBC, 0x00, 0x00, 0x00,
        0x89, 0x86, 0x90, 0x00, 0x00, 0x00,
        0x41, 0x8B, 0xD7
    };
    if (std::memcmp(target, signature.data(), signature.size())) {
        log("DOOMHUDCrosshair signature mismatch at RVA 0xC317BE; hard off-screen fallback unavailable");
        return false;
    }

    constexpr size_t stubSize = 2 + 10 + 3 + 2 + signature.size() + 14;
    auto stub = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, stubSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!stub) {
        log("DOOMHUDCrosshair capture stub allocation failed");
        return false;
    }
    size_t cursor{};
    stub[cursor++] = 0x41; // push r11
    stub[cursor++] = 0x53;
    stub[cursor++] = 0x49; // mov r11, &capturedCrosshairContext
    stub[cursor++] = 0xBB;
    const auto captureAddress = reinterpret_cast<unsigned long long>(&capturedCrosshairContext);
    std::memcpy(stub + cursor, &captureAddress, sizeof(captureAddress));
    cursor += sizeof(captureAddress);
    stub[cursor++] = 0x49; // mov [r11], rsi
    stub[cursor++] = 0x89;
    stub[cursor++] = 0x33;
    stub[cursor++] = 0x41; // pop r11
    stub[cursor++] = 0x5B;
    std::memcpy(stub + cursor, signature.data(), signature.size());
    cursor += signature.size();
    appendAbsoluteJump(stub + cursor, target + signature.size());
    FlushInstructionCache(GetCurrentProcess(), stub, stubSize);
    DWORD stubProtect{};
    if (!VirtualProtect(stub, stubSize, PAGE_EXECUTE_READ, &stubProtect)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        log("DOOMHUDCrosshair capture stub execute protection failed");
        return false;
    }

    DWORD oldProtect{};
    if (!VirtualProtect(target, signature.size(), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        log("DOOMHUDCrosshair patch protection failed");
        return false;
    }
    std::array<unsigned char, signature.size()> patch{};
    patch.fill(0x90);
    appendAbsoluteJump(patch.data(), stub);
    std::memcpy(target, patch.data(), patch.size());
    FlushInstructionCache(GetCurrentProcess(), target, patch.size());
    DWORD ignored{};
    VirtualProtect(target, signature.size(), oldProtect, &ignored);
    crosshairCaptureStub = stub;
    log("DOOMHUDCrosshair exact context capture installed at RVA 0xC317BE");
    return true;
}

template <size_t Size, typename Function>
bool installEntryHook(
    unsigned char* target, const std::array<unsigned char, Size>& signature,
    const void* hook, Function& original, const char* label) {
    auto trampoline = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, Size + 14, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        log(std::string(label) + " trampoline allocation failed");
        return false;
    }
    std::memcpy(trampoline, target, signature.size());
    appendAbsoluteJump(trampoline + signature.size(), target + signature.size());
    original = reinterpret_cast<Function>(trampoline);

    DWORD oldProtect{};
    if (!VirtualProtect(
            target, signature.size(), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        original = nullptr;
        VirtualFree(trampoline, 0, MEM_RELEASE);
        log(std::string(label) + " patch protection failed");
        return false;
    }
    std::array<unsigned char, Size> patch{};
    patch.fill(0x90);
    appendAbsoluteJump(patch.data(), hook);
    std::memcpy(target, patch.data(), patch.size());
    FlushInstructionCache(GetCurrentProcess(), target, patch.size());
    DWORD ignored{};
    VirtualProtect(target, signature.size(), oldProtect, &ignored);
    return true;
}

using HudCanvasSizeFn=void(__fastcall*)(void*,int,int,float,float);
HudCanvasSizeFn originalHudCanvasSize{};
void __fastcall offhandHudCanvasSize(void* entity,int width,int height,float extentX,float extentY){
    const auto image=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress())-image;
    const auto pending=offhandCanvasSubmission;
    offhandCanvasSubmission={};
    const bool suppressed=caller==0xF922A7&&suppressedOffhandCanvas==entity&&width==512&&height==300;
    suppressedOffhandCanvas=nullptr;
    if(suppressed){originalHudCanvasSize(entity,width,height,0,0);return;}
    float origin[3]{};
    const bool owned=caller==0xF922A7&&pending.entity==entity&&width==512&&height==300
        &&kharvox::centeredOffhandHud(pending.center,pending.axis,pending.width,
            float(width)/float(height),origin,extentX,extentY);
    originalHudCanvasSize(entity,width,height,extentX,extentY);
    if(!owned)return;
    auto bytes=static_cast<unsigned char*>(entity);
    if((bytes[0x70]&bytes[0x71])==0)std::memcpy(bytes+0xc8,origin,sizeof(origin));
    std::memcpy(bytes+0x78,origin,sizeof(origin));
    static std::atomic<bool> noted{};
    if(!noted.exchange(true))log("[OFFHAND-HUD] centered canvas pivot and metric panel size active");
}
bool installOffhandCanvasHook(){
    auto image=reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    // Two complete instructions, no RIP-relative operands.
    constexpr std::array<unsigned char,14> signature{0xf3,0x0f,0x10,0x54,0x24,0x28,0xf3,0x0f,0x11,0x99,0xcc,0x4e,0,0};
    if(!image||!readableRange(image+0x1590d60,signature.size())
        ||std::memcmp(image+0x1590d60,signature.data(),signature.size())){
        log("[OFFHAND-HUD] canvas signature mismatch; retaining standard HUD");return false;}
    return installEntryHook(image+0x1590d60,signature,reinterpret_cast<const void*>(&offhandHudCanvasSize),originalHudCanvasSize,"Offhand centered canvas");
}

using LocalizedTextLookup=const char*(__fastcall*)(const void*);
LocalizedTextLookup originalLocalizedTextLookup{};
const char* __fastcall localizedTextBindingHook(const void* key){
    const char* original=originalLocalizedTextLookup(key);
    if(!original)return original;
    const size_t length=strnlen_s(original,8192);
    if(length==8192||!std::memchr(original,'_',length))return original;
    try {
        static const bool left=[](){char b[32]{};return GetEnvironmentVariableA("KHARVOX_LEFT_HANDED",b,sizeof(b))&&std::strcmp(b,"0");}();
        static const bool swap=[](){char b[32]{};GetEnvironmentVariableA("KHARVOX_LEFT_HAND_SWAP",b,sizeof(b));return !_stricmp(b,"buttons-and-sticks");}();
        // Own returned strings for process lifetime: native widgets may retain
        // pointers. Never mutate or release DOOM's localization allocations.
        static std::mutex bindingMutex;
        static std::map<std::string,std::string,std::less<>> replacements;
        std::lock_guard<std::mutex> lock(bindingMutex);
        const auto found=replacements.find(std::string_view(original,length));
        if(found!=replacements.end())return found->second.c_str();
        const auto replaced=kharvox::replaceTutorialBindings({original,length},left,swap);
        if(replaced==original||replacements.size()>=4096)return original;
        const auto inserted=replacements.emplace(std::string(original,length),replaced);
        log("[TUTORIAL-BINDING] localized action labels replaced; entry="+std::to_string(replacements.size()));
        return inserted.first->second.c_str();
    }catch(...){return original;}
}

void installLocalizedBindingHook(){
    const auto image=reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    // Native lookup takes a string-ID pointer and returns localized UTF-8.
    // Tutorial_Text calls it at C5793A/C57984 before copying into its widgets.
    constexpr std::array<unsigned char,15> signature{
        0x4C,0x8B,0xDC,0x48,0x83,0xEC,0x58,0x49,0xC7,0x43,0xC8,0xFE,0xFF,0xFF,0xFF};
    if(!image||std::memcmp(image+0x280580,signature.data(),signature.size())){
        log("[TUTORIAL-BINDING] lookup signature mismatch; native labels retained");return;
    }
    if(installEntryHook(image+0x280580,signature,reinterpret_cast<const void*>(&localizedTextBindingHook),
        originalLocalizedTextLookup,"localized KHARVOX action labels"))
        log("[TUTORIAL-BINDING] localized lookup replacement active");
}

void __fastcall hudNotifyHook(
    void* hud, void* notification, void* context, unsigned char immediate) {
    // Keep the native notification as a late fallback for campaign variants
    // which bypass the concrete Elite Guard activation override.
    if (notification
        && *static_cast<const unsigned int*>(notification) == 0x8000u) {
        beginPlayerUpgradePickupSession(
            "native HUDNOTIFY_PICKUP_SUITUPGRADE fallback");
        // Activation precedes the authored animation; this native award event
        // is separate evidence. Some campaigns keep the cinematic camera and
        // never call the generic PlayerUpgrade manager/ShowScreen callbacks.
        playerUpgradePickupConfirmedTick.store(GetTickCount64(), std::memory_order_release);
        log("[PRAETOR-QUAD] native pickup notification confirmed; centered menu session armed");
    }
    originalHudNotify(hud, notification, context, immediate);
}

bool __fastcall eliteGuardActivateHook(
    void* eliteGuard, void* activator, unsigned int activationFlags) {
    // idInteractable_EliteGuard has its own vtable slot 194 implementation;
    // Glory Kills and ordinary idInteractable_GiveItems objects never enter
    // this function. Begin before the original so the introductory animation
    // remains immersive. The exact PlayerUpgrade manager/screen boundary will
    // center only the interactive menu which follows it.
    const bool sessionWasActive = playerUpgradePickupSessionActive.load(
        std::memory_order_acquire);
    beginPlayerUpgradePickupSession(
        "native idInteractable_EliteGuard activation RVA 0x925210");
    const bool activated = originalEliteGuardActivate(
        eliteGuard, activator, activationFlags);
    if (!activated && !sessionWasActive) {
        playerUpgradePickupSessionActive.store(false, std::memory_order_release);
        playerUpgradePickupCinematicObserved.store(false, std::memory_order_release);
        playerUpgradePickupFallbackQuadLogged.store(false, std::memory_order_release);
        playerUpgradePickupSessionStartTick.store(0, std::memory_order_release);
        playerUpgradePickupSessionUntilTick.store(0, std::memory_order_release);
        log("native idInteractable_EliteGuard activation rejected; pending pickup session cancelled");
    }
    return activated;
}

bool installEliteGuardActivationHook() {
    auto image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!image) return false;

    auto target = image + 0x925210;
    constexpr std::array<unsigned char, 16> signature{
        0x40,0x55,0x56,0x57,0x48,0x8D,0x6C,0x24,
        0xB9,0x48,0x81,0xEC,0x00,0x01,0x00,0x00
    };
    if (!installEntryHook(
            target, signature,
            reinterpret_cast<const void*>(&eliteGuardActivateHook),
            originalEliteGuardActivate,
            "idInteractable_EliteGuard activation")) return false;
    log("native Elite Guard activation hook installed: exact class vtable slot 194 RVA 0x925210");
    return true;
}

bool installPlayerUpgradePickupNotificationHook() {
    auto image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!image) return false;
    auto target = image + 0xC24920;
    constexpr std::array<unsigned char, 20> signature{
        0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,
        0x41,0x57,0x48,0x8D,0xAC,0x24,0xA0,0xBB,0xFF,0xFF
    };
    if (!installEntryHook(
            target, signature,
            reinterpret_cast<const void*>(&hudNotifyHook),
            originalHudNotify, "HUD notification dispatcher")) return false;
    log("native HUD notification hook installed: dispatcher RVA 0xC24920, PICKUP_SUITUPGRADE=0x8000");
    return true;
}

void __fastcall playerUpgradeManagerFrameHook(
    void* playerUpgradeManager, int frameTime) {
    // The picked-up Praetor Suit menu is rendered through the manager itself.
    // In the observed campaign flow DOOM bypasses every outer/child
    // ShowScreen callback, so this per-frame native boundary is the first
    // exact signal that the interactive menu image actually exists.
    pulsePlayerUpgradeManagerQuadSession();
    originalPlayerUpgradeManagerFrame(playerUpgradeManager, frameTime);
}

bool installPlayerUpgradeManagerFrameHook() {
    auto image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!image) return false;

    auto target = image + 0xC92DE0;
    constexpr std::array<unsigned char, 14> signature{
        0x40,0x53,0x48,0x83,0xEC,0x20,0x48,
        0x8B,0xD9,0xE8,0x12,0xEF,0x2F,0x00
    };
    if (std::memcmp(target, signature.data(), signature.size())) {
        log("idMenuManager_PlayerUpgrade frame signature mismatch; Praetor pickup Quad disabled");
        return false;
    }

    // The overwritten prefix contains the manager's direct call to the common
    // render/update path. Re-encode that call absolutely inside the trampoline
    // so its destination remains correct after relocation.
    constexpr size_t copiedPrefixSize = 9;
    constexpr size_t absoluteCallSize = 12;
    constexpr size_t absoluteJumpSize = 14;
    constexpr size_t trampolineSize = copiedPrefixSize
        + absoluteCallSize + absoluteJumpSize;
    auto trampoline = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, trampolineSize, MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        log("PlayerUpgrade manager frame trampoline allocation failed");
        return false;
    }
    std::memcpy(trampoline, target, copiedPrefixSize);
    appendAbsoluteCall(trampoline + copiedPrefixSize, image + 0xF91D00);
    appendAbsoluteJump(
        trampoline + copiedPrefixSize + absoluteCallSize,
        target + signature.size());
    originalPlayerUpgradeManagerFrame =
        reinterpret_cast<PlayerUpgradeManagerFrameFn>(trampoline);

    DWORD oldProtect{};
    if (!VirtualProtect(
            target, signature.size(), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        originalPlayerUpgradeManagerFrame = nullptr;
        VirtualFree(trampoline, 0, MEM_RELEASE);
        log("PlayerUpgrade manager frame patch protection failed");
        return false;
    }
    std::array<unsigned char, signature.size()> patch{};
    patch.fill(0x90);
    appendAbsoluteJump(
        patch.data(),
        reinterpret_cast<const void*>(&playerUpgradeManagerFrameHook));
    std::memcpy(target, patch.data(), patch.size());
    FlushInstructionCache(GetCurrentProcess(), target, patch.size());
    DWORD ignored{};
    VirtualProtect(target, patch.size(), oldProtect, &ignored);
    log("native PlayerUpgrade manager frame hook installed: RVA 0xC92DE0");
    return true;
}

void __fastcall fieldDroneBindActivatorHook(void* fieldDrone, void* activator) {
    // BindActivator is the concrete native transition into the Weapon Mod Bot
    // dialog. It is independent of the continuously rendered proximity SWF.
    KharvoxHudBeginFieldDroneMenuSession();
    originalFieldDroneBindActivator(fieldDrone, activator);
}

void __fastcall fieldDroneReleaseActivatorHook(void* fieldDrone, void* activator) {
    originalFieldDroneReleaseActivator(fieldDrone, activator);
    // ReleaseActivator is shared by decline/back and successful acceptance, so
    // both legitimate dialog exits restore Projection without interpreting A,
    // B, Dossier, or any other controller button as a lifecycle event.
    KharvoxHudEndFieldDroneMenuSession();
}

bool installFieldDroneLifecycleHooks() {
    auto image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!image) return false;

    auto bindTarget = image + 0x939940;
    auto releaseTarget = image + 0x93A4B0;
    constexpr std::array<unsigned char, 16> bindSignature{
        0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,
        0xEC,0x30,0x48,0x8B,0xF9,0x48,0x8B,0xCA
    };
    constexpr std::array<unsigned char, 16> releaseSignature{
        0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,
        0xEC,0x20,0x48,0x8B,0xF9,0x48,0x8B,0xCA
    };
    if (std::memcmp(bindTarget, bindSignature.data(), bindSignature.size())
        || std::memcmp(releaseTarget, releaseSignature.data(), releaseSignature.size())) {
        log("idInteractable_WeaponModBot lifecycle signature mismatch; Field Drone Quad disabled");
        return false;
    }
    // Install the exit observer first so a partial installation can never
    // create a session that lacks its native release path.
    if (!installEntryHook(
            releaseTarget, releaseSignature,
            reinterpret_cast<const void*>(&fieldDroneReleaseActivatorHook),
            originalFieldDroneReleaseActivator, "WeaponModBot ReleaseActivator")) return false;
    if (!installEntryHook(
            bindTarget, bindSignature,
            reinterpret_cast<const void*>(&fieldDroneBindActivatorHook),
            originalFieldDroneBindActivator, "WeaponModBot BindActivator")) return false;
    log("native Field Drone lifecycle hooks installed: BindActivator RVA 0x939940, ReleaseActivator RVA 0x93A4B0");
    return true;
}

void beginRuneTrialQuadSession(const char* nativeSource) {
    const auto now = GetTickCount64();
    const bool alreadyActive = runeTrialNativeSessionActive.exchange(
        true, std::memory_order_acq_rel);
    if (!alreadyActive) {
        runeTrialMenuGenerationAtActivation.store(
            KharvoxCameraLevelTransitionGeneration(),
            std::memory_order_release);
        runeTrialChallengeLoadPending.store(false, std::memory_order_release);
    }
    runeTrialMenuSessionUntilTick.store(
        now + runeTrialMenuSessionMilliseconds, std::memory_order_release);
    if (!alreadyActive)
        log(std::string(nativeSource)
            + "; Vega Training animation/Rune menu centered Quad/UI session started");
}

void cancelRejectedRuneTrialQuadSession() {
    runeTrialNativeSessionActive.store(false, std::memory_order_release);
    runeTrialChallengeLoadPending.store(false, std::memory_order_release);
    runeTrialMenuGenerationAtActivation.store(0, std::memory_order_release);
    if (runeTrialMenuSessionUntilTick.exchange(0, std::memory_order_acq_rel))
        log("native idInteractable_VegaTraining activation rejected; pre-animation Quad session cancelled");
}

void endRuneTrialQuadSession(const char* nativeSource) {
    runeTrialNativeSessionActive.store(false, std::memory_order_release);
    runeTrialChallengeLoadPending.store(false, std::memory_order_release);
    runeTrialMenuGenerationAtActivation.store(0, std::memory_order_release);
    if (runeTrialMenuSessionUntilTick.exchange(0, std::memory_order_acq_rel))
        log(std::string(nativeSource) + "; Rune Quad/UI session released");
}

void beginRuneChallengeScreenSession(const char* nativeSource) {
    const bool alreadyActive = runeChallengeScreenActive.exchange(
        true, std::memory_order_acq_rel);
    const bool mapLoadDestinationScreen =
        kharvox::shouldTransferRuneChallengeMapLoadToDestinationScreen(
            alreadyActive,
            runeChallengeMapLoadPending.load(std::memory_order_acquire));
    if (!alreadyActive || mapLoadDestinationScreen) {
        runeChallengePauseRecoveryArmed.store(false, std::memory_order_release);
        runeChallengeGenerationAtPause.store(0, std::memory_order_release);
        runeChallengeMapLoadPending.store(false, std::memory_order_release);
        runeChallengeGenerationAtLoad.store(0, std::memory_order_release);
        runeChallengePendingLoadCommand.store(-1, std::memory_order_release);
        runeChallengeScreenGenerationAtShow.store(
            KharvoxCameraLevelTransitionGeneration(), std::memory_order_release);
        runeChallengeScreenSessionUntilTick.store(
            GetTickCount64() + runeTrialMenuSessionMilliseconds,
            std::memory_order_release);
        if (mapLoadDestinationScreen) {
            log(std::string(nativeSource)
                + "; map-load destination Start/Exit screen took ownership; centered Quad/UI retained");
        } else {
            log(std::string(nativeSource)
                + "; Rune Trial Start/Exit centered Quad/UI session started");
        }
    }

    // The accepted terminal session only bridges the map load. Once the exact
    // destination screen owns the UI, transfer ownership to its Show/Hide
    // lifecycle so ordinary gameplay cannot remain latched in Quad.
    if (runeTrialNativeSessionActive.load(std::memory_order_acquire)
        || runeTrialMenuSessionUntilTick.load(std::memory_order_acquire))
        endRuneTrialQuadSession("Rune Trial destination screen took ownership");
}

void endRuneChallengeScreenSession(const std::string& nativeSource) {
    runeChallengePauseRecoveryArmed.store(false, std::memory_order_release);
    runeChallengeGenerationAtPause.store(0, std::memory_order_release);
    runeChallengeMapLoadPending.store(false, std::memory_order_release);
    runeChallengeGenerationAtLoad.store(0, std::memory_order_release);
    runeChallengePendingLoadCommand.store(-1, std::memory_order_release);
    runeChallengeScreenGenerationAtShow.store(0, std::memory_order_release);
    runeChallengeScreenSessionUntilTick.store(0, std::memory_order_release);
    if (runeChallengeScreenActive.exchange(false, std::memory_order_acq_rel))
        log(nativeSource
            + "; Rune Trial Start/Exit Quad/UI session released");
}

bool runeChallengeStartFlag(void* screen) {
    if (!screen || !readableRange(screen, 0x249)) return false;
    return static_cast<const unsigned char*>(screen)[0x248] != 0;
}

bool runeChallengeActionReady(void* screen) {
    if (!screen || !readableRange(screen, 0x24A)) return false;
    const auto bytes = static_cast<const unsigned char*>(screen);
    if (*reinterpret_cast<void* const*>(bytes + 8) && bytes[0x249]) return false;
    const auto image = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
    const auto manager = *reinterpret_cast<const unsigned char* const*>(image + 0x5B0F6D0);
    return manager && readableRange(manager + 0x300AD0, sizeof(int))
        && *reinterpret_cast<const int*>(manager + 0x300AD0) != 2;
}

bool __fastcall runeChallengeHandleActionHook(void* screen, void* action) {
    int actionType = -1;
    int commandIndex = -1;
    if (action && readableRange(action, 16)) {
        actionType = *static_cast<const int*>(action);
        if (actionType == 1) {
            auto scriptValue = *reinterpret_cast<void**>(
                static_cast<unsigned char*>(action) + 8);
            const auto image = reinterpret_cast<unsigned char*>(
                GetModuleHandleW(nullptr));
            if (image && scriptValue) {
                using ScriptValueToIntegerFn = int(__fastcall*)(void*);
                const auto toInteger = reinterpret_cast<ScriptValueToIntegerFn>(
                    image + 0x17438B0);
                commandIndex = toInteger(scriptValue);
            }
        }
    }

    const bool challengeActiveBeforeAction = runeChallengeScreenActive.load(
        std::memory_order_acquire);
    const bool nativeReady = runeChallengeActionReady(screen);
    const bool armMapLoad = nativeReady && kharvox::shouldArmRuneChallengeMapLoadAction(
        challengeActiveBeforeAction, actionType, commandIndex);
    const auto generation = armMapLoad
        ? KharvoxCameraLevelTransitionGeneration() : 0;
    if (armMapLoad) {
        runeChallengeGenerationAtLoad.store(generation, std::memory_order_release);
        runeChallengePendingLoadCommand.store(commandIndex, std::memory_order_release);
        runeChallengeMapLoadPending.store(true, std::memory_order_release);
        runeChallengeScreenSessionUntilTick.store(
            GetTickCount64() + runeTrialMenuSessionMilliseconds,
            std::memory_order_release);
        runeChallengePauseRecoveryArmed.store(false, std::memory_order_release);
        runeChallengeGenerationAtPause.store(0, std::memory_order_release);
    }

    // Pre-arm before calling DOOM: Exit may synchronously hide/destroy the
    // outgoing screen and begin the map teardown inside HandleAction_Impl.
    const bool isStart = actionType == 1 && commandIndex == 0;
    const bool startFlagBefore = isStart && runeChallengeStartFlag(screen);
    const bool handled = originalRuneChallengeHandleAction(screen, action);
    // HandleAction returns true for inputs swallowed during the native opening
    // transition / challenge load. Only the actual Start branch sets +0x248.
    // Do not confuse "consumed" with a completed menu command.
    const bool startCommitted = isStart && !startFlagBefore && runeChallengeStartFlag(screen);
    const bool challengeActive = runeChallengeScreenActive.load(
        std::memory_order_acquire);
    if (kharvox::shouldRetainRuneChallengeMapLoadAction(
            challengeActiveBeforeAction, handled && nativeReady, actionType, commandIndex)) {
        // A synchronous destination ShowScreen can already have transferred
        // ownership and cleared the pending flag. Never re-arm it afterwards.
        if (!runeChallengeMapLoadPending.load(std::memory_order_acquire))
            return handled;
        log("native Gui_EndOfChallenge map-load command="
            + std::to_string(commandIndex)
            + " handled; retaining Quad/UI through world replacement generation="
            + std::to_string(generation));
    } else if (armMapLoad) {
        runeChallengeMapLoadPending.store(false, std::memory_order_release);
        runeChallengeGenerationAtLoad.store(0, std::memory_order_release);
        runeChallengePendingLoadCommand.store(-1, std::memory_order_release);
        log("native Gui_EndOfChallenge map-load command="
            + std::to_string(commandIndex)
            + " was not handled; pre-armed load guard cancelled");
    } else if (kharvox::shouldReleaseRuneChallengeAction(
            challengeActive, handled && nativeReady, actionType, commandIndex, startCommitted)) {
        endRuneChallengeScreenSession(
            "native Gui_EndOfChallenge handled closing command="
            + std::to_string(commandIndex));
    } else if (challengeActive && handled && actionType == 1 && commandIndex == 0) {
        log("[RUNE-START] input consumed without native Start commit; Quad/UI retained");
    }
    return handled;
}

bool __fastcall runePopupHandleActionHook(void* popup, void* action) {
    int selectionIndex = -1;
    if (action && readableRange(action, 16)
        && *static_cast<const int*>(action) == 1) {
        auto scriptValue = *reinterpret_cast<void**>(
            static_cast<unsigned char*>(action) + 8);
        const auto image = reinterpret_cast<unsigned char*>(
            GetModuleHandleW(nullptr));
        if (image && scriptValue) {
            using ScriptValueToIntegerFn = int(__fastcall*)(void*);
            const auto toInteger = reinterpret_cast<ScriptValueToIntegerFn>(
                image + 0x17438B0);
            selectionIndex = toInteger(scriptValue);
        }
    }

    const bool handled = originalRunePopupHandleAction(popup, action);
    const bool challengeLoadPending = runeTrialChallengeLoadPending.load(
        std::memory_order_acquire);
    if (kharvox::shouldReleaseRunePopupSelection(
            runeTrialNativeSessionActive.load(std::memory_order_acquire),
            challengeLoadPending, selectionIndex)) {
        endRuneTrialQuadSession(
            "native idMenuWidget_RunePopup Exit selection index=1");
    } else if (selectionIndex == 0
        && runeTrialNativeSessionActive.load(std::memory_order_acquire)) {
        log("native idMenuWidget_RunePopup Accept selection index=0; retaining Quad for VegaTraining::Use");
    }
    return handled;
}

bool __fastcall vegaTrainingActivateHook(
    void* vegaTraining, void* activator, unsigned int activationFlags) {
    // The exact VegaTraining class override runs when USE commits to the Rune
    // Stone. Arm before the original so the complete authored interaction
    // animation is already submitted through the centered Cinewindow.
    const bool sessionWasActive = runeTrialNativeSessionActive.load(
        std::memory_order_acquire);
    beginRuneTrialQuadSession(
        "native idInteractable_VegaTraining activation RVA 0x939200");
    const bool activated = originalVegaTrainingActivate(
        vegaTraining, activator, activationFlags);
    if (!activated && !sessionWasActive)
        cancelRejectedRuneTrialQuadSession();
    return activated;
}

void* __fastcall vegaTrainingShowRuneMenuHook(
    void* vegaTraining, void* eventReturn, void* eventArguments) {
    // ae_showRuneMenu is embedded in the Rune Stone animation and owns the
    // actually observed full-screen image. DOOM also replays this event while
    // restoring some saves without opening the interactive menu. It may only
    // refresh a session already owned by the exact activation hook.
    if (kharvox::shouldAcceptRuneMenuAnimationEvent(
            runeTrialNativeSessionActive.load(std::memory_order_acquire))) {
        beginRuneTrialQuadSession(
            "native idInteractable_VegaTraining ae_showRuneMenu RVA 0x937680");
    } else {
        log("unowned ae_showRuneMenu during save/load ignored; no Rune Quad session created");
    }
    return originalVegaTrainingShowRuneMenu(
        vegaTraining, eventReturn, eventArguments);
}

void* __fastcall vegaTrainingReleaseActivatorHook(
    void* vegaTraining, void* eventReturn, void* activator) {
    auto result = originalVegaTrainingReleaseActivator(
        vegaTraining, eventReturn, activator);
    // Retain this as a fallback for campaign variants which do use the common
    // activator release. The observed Rune Exit and accepted nextmap paths
    // bypass it and are handled by their exact callbacks below.
    endRuneTrialQuadSession(
        "native idInteractable_VegaTraining Event_ReleaseActivator");
    return result;
}

void* __fastcall vegaTrainingUseHook(
    void* vegaTraining, void* eventReturn) {
    // This exact function formats and dispatches DOOM's nextmap command for
    // an accepted Rune Trial. Mark the load before the original so the Quad
    // survives every generation change and the destination confirmation UI.
    runeTrialChallengeLoadPending.store(true, std::memory_order_release);
    log("native idInteractable_VegaTraining::Use accepted Rune Trial; retaining Quad through nextmap and destination confirmation");
    return originalVegaTrainingUse(vegaTraining, eventReturn);
}

void* __fastcall vegaTrainingDeactivateHook(
    void* vegaTraining, void* eventReturn, void* eventArguments) {
    auto result = originalVegaTrainingDeactivate(
        vegaTraining, eventReturn, eventArguments);
    if (runeTrialChallengeLoadPending.load(std::memory_order_acquire)) {
        log("native vegaTrainingDeactivate during accepted Rune Trial; Quad retained for destination confirmation");
        return result;
    }

    // Exit/cancel returns to the campaign without loading a new world. This
    // event is the real native boundary; ReleaseActivator is not called here.
    endRuneTrialQuadSession("native vegaTrainingDeactivate Exit/cancel");
    return result;
}

bool installVegaTrainingUseEntryHook(
    unsigned char* image, unsigned char* target,
    const std::array<unsigned char, 15>& signature) {
    if (std::memcmp(target, signature.data(), signature.size())) {
        log("idInteractable_VegaTraining::Use signature mismatch; Rune Trial load retention disabled");
        return false;
    }

    // The overwritten prologue calls __chkstk with the requested allocation
    // size in RAX. Re-encode the relative call through R11 so RAX is preserved
    // and the relocated trampoline remains equivalent to DOOM's original.
    constexpr size_t copiedPrefixSize = 7;
    constexpr size_t absoluteCallThroughR11Size = 13;
    constexpr size_t copiedSuffixSize = 3;
    constexpr size_t absoluteJumpSize = 14;
    constexpr size_t trampolineSize = copiedPrefixSize
        + absoluteCallThroughR11Size + copiedSuffixSize + absoluteJumpSize;
    auto trampoline = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, trampolineSize, MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        log("idInteractable_VegaTraining::Use trampoline allocation failed");
        return false;
    }

    size_t cursor{};
    std::memcpy(trampoline + cursor, signature.data(), copiedPrefixSize);
    cursor += copiedPrefixSize;
    trampoline[cursor++] = 0x49; // mov r11, absolute __chkstk
    trampoline[cursor++] = 0xBB;
    const auto checkStackAddress = reinterpret_cast<unsigned long long>(
        image + 0x1E31170);
    std::memcpy(
        trampoline + cursor, &checkStackAddress, sizeof(checkStackAddress));
    cursor += sizeof(checkStackAddress);
    trampoline[cursor++] = 0x41; // call r11; preserves allocation size in RAX
    trampoline[cursor++] = 0xFF;
    trampoline[cursor++] = 0xD3;
    std::memcpy(
        trampoline + cursor,
        signature.data() + copiedPrefixSize + 5,
        copiedSuffixSize);
    cursor += copiedSuffixSize;
    appendAbsoluteJump(trampoline + cursor, target + signature.size());
    originalVegaTrainingUse = reinterpret_cast<VegaTrainingUseFn>(trampoline);

    DWORD oldProtect{};
    if (!VirtualProtect(
            target, signature.size(), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        originalVegaTrainingUse = nullptr;
        VirtualFree(trampoline, 0, MEM_RELEASE);
        log("idInteractable_VegaTraining::Use patch protection failed");
        return false;
    }
    std::vector<unsigned char> patch(signature.size(), 0x90);
    appendAbsoluteJump(
        patch.data(), reinterpret_cast<const void*>(&vegaTrainingUseHook));
    std::memcpy(target, patch.data(), patch.size());
    FlushInstructionCache(GetCurrentProcess(), target, patch.size());
    DWORD ignored{};
    VirtualProtect(target, patch.size(), oldProtect, &ignored);
    return true;
}

bool installVegaTrainingLifecycleHooks() {
    auto image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!image) return false;

    auto activateTarget = image + 0x939200;
    auto useTarget = image + 0x937090;
    auto deactivateTarget = image + 0x937540;
    auto showRuneMenuTarget = image + 0x937680;
    auto releaseTarget = image + 0x937250;
    auto runePopupActionTarget = image + 0xC7E810;
    constexpr std::array<unsigned char, 14> activateSignature{
        0x40,0x53,0x55,0x56,0x57,0x41,0x56,
        0x48,0x81,0xEC,0x50,0x01,0x00,0x00
    };
    constexpr std::array<unsigned char, 16> showRuneMenuSignature{
        0x48,0x8B,0xC4,0x55,0x41,0x54,0x41,0x55,
        0x41,0x56,0x41,0x57,0x48,0x8D,0x68,0x98
    };
    constexpr std::array<unsigned char, 15> useSignature{
        0x40,0x57,0xB8,0x80,0x80,0x00,0x00,
        0xE8,0xD4,0xA0,0x4F,0x01,0x48,0x2B,0xE0
    };
    constexpr std::array<unsigned char, 14> deactivateSignature{
        0x40,0x55,0x56,0x57,0x48,0x83,0xEC,
        0x20,0x48,0x8B,0xF9,0x49,0x8B,0xE8
    };
    constexpr std::array<unsigned char, 15> releaseSignature{
        0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,
        0x24,0x10,0x57,0x48,0x83,0xEC,0x20
    };
    constexpr std::array<unsigned char, 19> runePopupActionSignature{
        0x55,0x56,0x57,0x48,0x81,0xEC,0x80,0x01,0x00,0x00,
        0x48,0xC7,0x44,0x24,0x30,0xFE,0xFF,0xFF,0xFF
    };
    auto vtableActivation = *reinterpret_cast<void**>(
        image + 0x21508F8 + 194 * sizeof(void*));
    if (vtableActivation != activateTarget
        || std::memcmp(
            activateTarget, activateSignature.data(), activateSignature.size())
        || std::memcmp(
            showRuneMenuTarget, showRuneMenuSignature.data(),
            showRuneMenuSignature.size())
        || std::memcmp(
            useTarget, useSignature.data(), useSignature.size())
        || std::memcmp(
            deactivateTarget, deactivateSignature.data(),
            deactivateSignature.size())
        || std::memcmp(
            releaseTarget, releaseSignature.data(), releaseSignature.size())
        || *reinterpret_cast<void**>(image + 0x224D3E8 + 8 * sizeof(void*))
            != runePopupActionTarget
        || std::memcmp(
            runePopupActionTarget, runePopupActionSignature.data(),
            runePopupActionSignature.size())) {
        log("idInteractable_VegaTraining lifecycle signature/vtable mismatch; Rune Quad disabled");
        return false;
    }

    // Validate every endpoint above, then install the accepted-load marker and
    // both exit observers before any hook which can create the session.
    if (!installVegaTrainingUseEntryHook(
            image, useTarget, useSignature)) return false;
    if (!installEntryHook(
            runePopupActionTarget, runePopupActionSignature,
            reinterpret_cast<const void*>(&runePopupHandleActionHook),
            originalRunePopupHandleAction,
            "idMenuWidget_RunePopup HandleAction")) return false;
    if (!installEntryHook(
            deactivateTarget, deactivateSignature,
            reinterpret_cast<const void*>(&vegaTrainingDeactivateHook),
            originalVegaTrainingDeactivate,
            "VegaTraining vegaTrainingDeactivate")) return false;
    if (!installEntryHook(
            releaseTarget, releaseSignature,
            reinterpret_cast<const void*>(&vegaTrainingReleaseActivatorHook),
            originalVegaTrainingReleaseActivator,
            "VegaTraining Event_ReleaseActivator")) return false;
    if (!installEntryHook(
            showRuneMenuTarget, showRuneMenuSignature,
            reinterpret_cast<const void*>(&vegaTrainingShowRuneMenuHook),
            originalVegaTrainingShowRuneMenu,
            "VegaTraining ae_showRuneMenu")) return false;
    if (!installEntryHook(
            activateTarget, activateSignature,
            reinterpret_cast<const void*>(&vegaTrainingActivateHook),
            originalVegaTrainingActivate,
            "idInteractable_VegaTraining activation")) return false;
    log("native VegaTraining/Rune lifecycle hooks installed: activation RVA 0x939200, RunePopup HandleAction RVA 0xC7E810, Use/nextmap RVA 0x937090, vegaTrainingDeactivate RVA 0x937540, ae_showRuneMenu RVA 0x937680, Event_ReleaseActivator RVA 0x937250");
    return true;
}

void __fastcall suitUpgradeActivateHook(void* upgradeStation, void* player);
void* __fastcall suitUpgradeReleaseHook(
    void* upgradeStation, void* eventReturn, void* eventArgument);

bool installSuitUpgradeActivateEntryHook(
    unsigned char* target, const std::array<unsigned char, 14>& signature) {
    // ActivateStation starts with a nullable-player branch. A copied relative
    // jump would target the wrong address from a normal trampoline, so encode
    // the equivalent null return locally and jump back only for a real player.
    constexpr size_t trampolineSize = 3 + 2 + 1 + 5 + 14;
    auto trampoline = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, trampolineSize, MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        log("UpgradeStation ActivateStation trampoline allocation failed");
        return false;
    }
    size_t cursor{};
    trampoline[cursor++] = 0x48; // test rdx,rdx
    trampoline[cursor++] = 0x85;
    trampoline[cursor++] = 0xD2;
    trampoline[cursor++] = 0x75; // jne nonNullPlayer
    trampoline[cursor++] = 0x01;
    trampoline[cursor++] = 0xC3; // null player: original target is a bare ret
    std::memcpy(trampoline + cursor, signature.data() + 9, 5);
    cursor += 5; // mov [rsp+10h],rsi
    appendAbsoluteJump(trampoline + cursor, target + signature.size());
    originalSuitUpgradeActivate =
        reinterpret_cast<SuitUpgradeActivateFn>(trampoline);

    DWORD oldProtect{};
    if (!VirtualProtect(
            target, signature.size(), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        originalSuitUpgradeActivate = nullptr;
        VirtualFree(trampoline, 0, MEM_RELEASE);
        log("UpgradeStation ActivateStation patch protection failed");
        return false;
    }
    std::array<unsigned char, 14> patch{};
    patch.fill(0x90);
    appendAbsoluteJump(
        patch.data(), reinterpret_cast<const void*>(&suitUpgradeActivateHook));
    std::memcpy(target, patch.data(), patch.size());
    FlushInstructionCache(GetCurrentProcess(), target, patch.size());
    DWORD ignored{};
    VirtualProtect(target, patch.size(), oldProtect, &ignored);
    return true;
}

void __fastcall suitUpgradeActivateHook(void* upgradeStation, void* player) {
    originalSuitUpgradeActivate(upgradeStation, player);
    // The native function sets this byte only after all station/player/UI
    // validation succeeds and directly before entering the UpgradeStation
    // dialog. Failed interactions therefore never open a speculative Quad.
    if (upgradeStation && player
        && *(static_cast<unsigned char*>(upgradeStation) + 0x5408) != 0)
        KharvoxHudBeginSuitUpgradeMenuSession();
}

void* __fastcall suitUpgradeReleaseHook(
    void* upgradeStation, void* eventReturn, void* eventArgument) {
    auto result = originalSuitUpgradeRelease(
        upgradeStation, eventReturn, eventArgument);
    // Event_ReleasePlayer is the shared native exit after cancel/back and a
    // completed upgrade. End the Quad only after DOOM has restored the player.
    KharvoxHudEndSuitUpgradeMenuSession();
    return result;
}

bool installSuitUpgradeLifecycleHooks() {
    auto image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!image) return false;

    auto activateTarget = image + 0x7E7DB0;
    auto releaseTarget = image + 0x7E8530;
    constexpr std::array<unsigned char, 14> activateSignature{
        0x48,0x85,0xD2,0x0F,0x84,0xBD,0x00,
        0x00,0x00,0x48,0x89,0x74,0x24,0x10
    };
    constexpr std::array<unsigned char, 18> releaseSignature{
        0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x74,0x24,
        0x20,0x57,0x48,0x81,0xEC,0x80,0x00,0x00,0x00
    };
    if (std::memcmp(
            activateTarget, activateSignature.data(), activateSignature.size())
        || std::memcmp(
            releaseTarget, releaseSignature.data(), releaseSignature.size())) {
        log("idDesignSystems_UpgradeStation lifecycle signature mismatch; Suit Upgrade Quad disabled");
        return false;
    }
    // Install the exit observer first so a partial install cannot leave a
    // successfully opened Suit Upgrade session without a native release path.
    if (!installEntryHook(
            releaseTarget, releaseSignature,
            reinterpret_cast<const void*>(&suitUpgradeReleaseHook),
            originalSuitUpgradeRelease,
            "UpgradeStation Event_ReleasePlayer")) return false;
    if (!installSuitUpgradeActivateEntryHook(
            activateTarget, activateSignature)) return false;
    log("native Suit Upgrade lifecycle hooks installed: ActivateStation RVA 0x7E7DB0, Event_ReleasePlayer RVA 0x7E8530");
    return true;
}

void __fastcall pauseScreenShowHook(void* pauseScreen, int transitionType) {
    pauseRootVisible.store(true, std::memory_order_release);
    if (runeChallengeScreenActive.load(std::memory_order_acquire)) {
        runeChallengeGenerationAtPause.store(
            KharvoxCameraLevelTransitionGeneration(), std::memory_order_release);
        if (!runeChallengePauseRecoveryArmed.exchange(
                true, std::memory_order_acq_rel))
            log("native Pause opened over Gui_EndOfChallenge; checkpoint recovery armed");
    }
    if (!pauseMenuActive.exchange(true, std::memory_order_acq_rel))
        log("native Pause screen shown; Pause QUAD active (transition="
            + std::to_string(transitionType) + ")");
    originalPauseScreenShow(pauseScreen, transitionType);
}

void __fastcall pauseScreenHideHook(void* pauseScreen, int transitionType) {
    pauseRootVisible.store(false, std::memory_order_release);
    originalPauseScreenHide(pauseScreen, transitionType);
    // Live transition observations identify 0 as the forward transition into
    // SETTINGS (and its descendants), 1 as the return transition, and 2 as
    // closing the complete Pause session back to gameplay. Keep the session
    // latched while the root Pause screen is merely hidden by a child screen.
    if (transitionType != 2) {
        log("native Pause screen hidden for child menu; Pause QUAD retained (transition="
            + std::to_string(transitionType) + ")");
        return;
    }
    if (pauseMenuActive.exchange(false, std::memory_order_acq_rel))
        log("native Pause session closed; Pause QUAD inactive (transition=2)");
}

bool installPauseMenuLifecycleHooks() {
    auto image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!image) return false;

    auto showTarget = image + 0x106A8D0;
    auto hideTarget = image + 0x1068DD0;
    constexpr std::array<unsigned char, 20> showSignature{
        0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
        0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x70, 0xFF, 0xFF, 0xFF
    };
    constexpr std::array<unsigned char, 20> hideSignature{
        0x48, 0x8B, 0xC4, 0x57, 0x48, 0x81, 0xEC, 0x00, 0x01, 0x00,
        0x00, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF
    };
    if (std::memcmp(showTarget, showSignature.data(), showSignature.size())
        || std::memcmp(hideTarget, hideSignature.data(), hideSignature.size())) {
        log("idMenuScreen_Shell_Pause lifecycle signature mismatch; Pause QUAD disabled");
        return false;
    }
    if (!installEntryHook(
            showTarget, showSignature,
            reinterpret_cast<const void*>(&pauseScreenShowHook),
            originalPauseScreenShow, "Pause ShowScreen")) return false;
    if (!installEntryHook(
            hideTarget, hideSignature,
            reinterpret_cast<const void*>(&pauseScreenHideHook),
            originalPauseScreenHide, "Pause HideScreen")) return false;
    log("native Pause lifecycle hooks installed: ShowScreen RVA 0x106A8D0, HideScreen RVA 0x1068DD0");
    return true;
}

template <typename Function>
bool installMenuVtableHook(
    unsigned char* image, uintptr_t vtableRva, size_t slotIndex,
    uintptr_t expectedFunctionRva, const void* hook, Function& original,
    const char* label) {
    auto entry = reinterpret_cast<void* volatile*>(
        image + vtableRva + slotIndex * sizeof(void*));
    auto expected = static_cast<void*>(image + expectedFunctionRva);
    auto current = *entry;
    if (current != expected) {
        log(std::string(label)
            + " vtable target mismatch; native menu lifecycle hook disabled");
        return false;
    }
    DWORD oldProtect{};
    if (!VirtualProtect(
            const_cast<void**>(entry), sizeof(void*), PAGE_READWRITE,
            &oldProtect)) {
        log(std::string(label) + " vtable protection failed");
        return false;
    }
    original = reinterpret_cast<Function>(current);
    InterlockedExchangePointer(entry, const_cast<void*>(hook));
    DWORD ignored{};
    VirtualProtect(const_cast<void**>(entry), sizeof(void*), oldProtect, &ignored);
    return true;
}

void __fastcall campaignDeathShowHook(void* deathScreen, int transitionType) {
    deathMenuLevelGenerationAtShow.store(
        KharvoxCameraLevelTransitionGeneration(), std::memory_order_release);
    if (!deathMenuActive.exchange(true, std::memory_order_acq_rel))
        log("native CampaignDeath screen shown; Death-menu QUAD active (transition="
            + std::to_string(transitionType) + ")");
    originalCampaignDeathShow(deathScreen, transitionType);
}

void __fastcall campaignDeathHideHook(void* deathScreen, int transitionType) {
    originalCampaignDeathHide(deathScreen, transitionType);
    // GUI screen transition observations use 0/1 while traversing child screens
    // and 2 when the complete screen session closes. Keep a confirmation or
    // nested screen in QUAD just as the native Pause lifecycle already does.
    if (transitionType != 2) {
        log("native CampaignDeath screen hidden for child/transition; Death-menu QUAD retained (transition="
            + std::to_string(transitionType) + ")");
        return;
    }
    if (deathMenuActive.exchange(false, std::memory_order_acq_rel))
        log("native CampaignDeath session closed; Death-menu QUAD inactive (transition=2)");
}

bool installCampaignDeathMenuLifecycleHooks() {
    auto image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!image) return false;

    // MSVC RTTI identifies idMenuScreen_Gui_CampaignDeath at vtable RVA
    // 0x224A9F0. Slot 21 is its ShowScreen override; slot 22 is the inherited
    // GUI HideScreen implementation. Patching only this class' two vtable
    // entries avoids intercepting every unrelated screen sharing HideScreen.
    constexpr uintptr_t vtableRva = 0x224A9F0;
    constexpr size_t showSlot = 21;
    constexpr size_t hideSlot = 22;
    constexpr uintptr_t showFunctionRva = 0xC12430;
    constexpr uintptr_t hideFunctionRva = 0xFB2C10;
    if (!installMenuVtableHook(
            image, vtableRva, hideSlot, hideFunctionRva,
            reinterpret_cast<const void*>(&campaignDeathHideHook),
            originalCampaignDeathHide, "CampaignDeath HideScreen")) return false;
    if (!installMenuVtableHook(
            image, vtableRva, showSlot, showFunctionRva,
            reinterpret_cast<const void*>(&campaignDeathShowHook),
            originalCampaignDeathShow, "CampaignDeath ShowScreen")) return false;
    log("native CampaignDeath lifecycle hooks installed: vtable RVA 0x224A9F0 slots 21/22");
    return true;
}

constexpr unsigned int endOfLevelGuiBit = 1u << 0;
constexpr unsigned int dossierEndOfLevelBit = 1u << 1;
constexpr unsigned int endOfLevelDossierBit = 1u << 2;
constexpr unsigned int playerUpgradeGuiBit = 1u << 0;
constexpr unsigned int argentSelectionBit = 1u << 1;
constexpr unsigned int dossierSuitBit = 1u << 2;
constexpr unsigned int dossierSuitDiagBit = 1u << 3;
constexpr unsigned int upgradeStationSelectionBit = 1u << 4;
constexpr unsigned int upgradeStationLeftBit = 1u << 5;
constexpr unsigned int upgradeStationRightBit = 1u << 6;
constexpr unsigned int playerUpgradeIdleBit = 1u << 7;
constexpr unsigned int playerUpgradeInUseBit = 1u << 8;
constexpr unsigned int playerUpgradeUsedBit = 1u << 9;

void beginDeathStyleScreenSession(
    std::atomic<unsigned int>& screenMask,
    std::atomic<unsigned long long>& generationAtShow,
    unsigned int screenBit, const char* screenName, int transitionType) {
    const auto previous = screenMask.fetch_or(screenBit, std::memory_order_acq_rel);
    if (!previous)
        generationAtShow.store(
            KharvoxCameraLevelTransitionGeneration(), std::memory_order_release);
    if (!(previous & screenBit))
        log(std::string("native ") + screenName
            + " shown; CampaignDeath-style full-frame Quad/UI active (transition="
            + std::to_string(transitionType) + ")");
}

void endDeathStyleScreenSession(
    std::atomic<unsigned int>& screenMask,
    unsigned int screenBit, const char* screenName, int transitionType,
    bool transitionOneCloses = false) {
    // 0/1 traverse internal pages; 2 closes this concrete screen. A bitmask
    // keeps the full session alive when DOOM overlaps an outer screen and a
    // Dossier/selection child during the same menu. Gui_ArgentSelection and
    // Dossier_SuitDiag are observed exceptions where transition 1 closes the
    // concrete screen rather than opening a child.
    const auto active = screenMask.load(std::memory_order_acquire);
    if (!(active & screenBit)) return;
    const auto remaining = kharvox::screenMaskAfterHide(
        active, screenBit, transitionType, transitionOneCloses);
    if (remaining == active) {
        log(std::string("native ") + screenName
            + " hidden for child/transition; CampaignDeath-style Quad/UI retained (transition="
            + std::to_string(transitionType) + ")");
        return;
    }
    const auto previous = screenMask.fetch_and(~screenBit, std::memory_order_acq_rel);
    if (!(previous & screenBit)) return;
    const auto actualRemaining = kharvox::screenMaskAfterHide(
        previous, screenBit, transitionType, transitionOneCloses);
    log(std::string("native ") + screenName
        + (actualRemaining
            ? " closed; overlapping menu screen retains CampaignDeath-style Quad/UI"
            : " session closed; CampaignDeath-style Quad/UI inactive")
        + " (transition=" + std::to_string(transitionType) + ")");
}

bool installScreenLifecyclePair(
    unsigned char* image, uintptr_t vtableRva,
    uintptr_t showFunctionRva, uintptr_t hideFunctionRva,
    const void* showHook, const void* hideHook,
    PauseScreenTransitionFn& originalShow,
    PauseScreenTransitionFn& originalHide,
    const char* screenName) {
    constexpr size_t showSlot = 21;
    constexpr size_t hideSlot = 22;
    const auto hideLabel = std::string(screenName) + " HideScreen";
    const auto showLabel = std::string(screenName) + " ShowScreen";
    if (!installMenuVtableHook(
            image, vtableRva, hideSlot, hideFunctionRva, hideHook,
            originalHide, hideLabel.c_str())) return false;
    if (!installMenuVtableHook(
            image, vtableRva, showSlot, showFunctionRva, showHook,
            originalShow, showLabel.c_str())) return false;
    log(std::string("native ") + screenName + " lifecycle hooks installed: vtable RVA 0x"
        + [&] { std::ostringstream out; out << std::hex << std::uppercase << vtableRva; return out.str(); }()
        + " slots 21/22");
    return true;
}

#define KHARVOX_DEFINE_MASKED_SCREEN_HOOKS(prefix, originalPrefix, mask, generation, bit, label) \
    void __fastcall prefix##ShowHook(void* screen, int transitionType) { \
        beginDeathStyleScreenSession(mask, generation, bit, label, transitionType); \
        originalPrefix##Show(screen, transitionType); \
    } \
    void __fastcall prefix##HideHook(void* screen, int transitionType) { \
        originalPrefix##Hide(screen, transitionType); \
        endDeathStyleScreenSession(mask, bit, label, transitionType); \
    }

KHARVOX_DEFINE_MASKED_SCREEN_HOOKS(
    endOfLevel, originalEndOfLevel, endOfLevelScreenMask,
    endOfLevelMenuGenerationAtShow, endOfLevelGuiBit, "Gui_EndOfLevel")
KHARVOX_DEFINE_MASKED_SCREEN_HOOKS(
    dossierEndOfLevel, originalDossierEndOfLevel, endOfLevelScreenMask,
    endOfLevelMenuGenerationAtShow, dossierEndOfLevelBit, "Dossier_EndOfLevel")
KHARVOX_DEFINE_MASKED_SCREEN_HOOKS(
    endOfLevelDossier, originalEndOfLevelDossier, endOfLevelScreenMask,
    endOfLevelMenuGenerationAtShow, endOfLevelDossierBit, "EndOfLevelDossier")
KHARVOX_DEFINE_MASKED_SCREEN_HOOKS(
    playerUpgrade, originalPlayerUpgrade, playerUpgradeScreenMask,
    playerUpgradeMenuGenerationAtShow, playerUpgradeGuiBit, "Gui_PlayerUpgrade")
void __fastcall argentSelectionShowHook(void* screen, int transitionType) {
    beginDeathStyleScreenSession(
        playerUpgradeScreenMask, playerUpgradeMenuGenerationAtShow,
        argentSelectionBit, "Gui_ArgentSelection", transitionType);
    originalArgentSelectionShow(screen, transitionType);
}

void __fastcall argentSelectionHideHook(void* screen, int transitionType) {
    originalArgentSelectionHide(screen, transitionType);
    endDeathStyleScreenSession(
        playerUpgradeScreenMask, argentSelectionBit,
        "Gui_ArgentSelection", transitionType, true);
    // VEGA closes an accepted Argent upgrade with transition 1, then keeps
    // rendering a visible cinematic/loading handoff after its generic camera
    // flag can already report gameplay. Keep the refresh trigger across this
    // boundary; completed-source AER now handles stereo image ownership.
    if (transitionType == 1) beginUpgradeCinematicRefreshGuard();
}
KHARVOX_DEFINE_MASKED_SCREEN_HOOKS(
    dossierSuit, originalDossierSuit, playerUpgradeScreenMask,
    playerUpgradeMenuGenerationAtShow, dossierSuitBit, "Dossier_Suit")
void __fastcall dossierSuitDiagShowHook(void* screen, int transitionType) {
    beginDeathStyleScreenSession(
        playerUpgradeScreenMask, playerUpgradeMenuGenerationAtShow,
        dossierSuitDiagBit, "Dossier_SuitDiag", transitionType);
    originalDossierSuitDiagShow(screen, transitionType);
}

void __fastcall dossierSuitDiagHideHook(void* screen, int transitionType) {
    originalDossierSuitDiagHide(screen, transitionType);
    // The post-Rune assignment Dossier closes with transition 1. Releasing
    // only this exact screen bit returns ownership to the normal presentation
    // selector, so Immersive, Comfort, and cinematics-only Quad settings keep
    // their own established behavior after the menu.
    endDeathStyleScreenSession(
        playerUpgradeScreenMask, dossierSuitDiagBit,
        "Dossier_SuitDiag", transitionType, true);
}
KHARVOX_DEFINE_MASKED_SCREEN_HOOKS(
    upgradeStationSelection, originalUpgradeStationSelection, playerUpgradeScreenMask,
    playerUpgradeMenuGenerationAtShow, upgradeStationSelectionBit, "UpgradeStation_Selection")
KHARVOX_DEFINE_MASKED_SCREEN_HOOKS(
    upgradeStationLeft, originalUpgradeStationLeft, playerUpgradeScreenMask,
    playerUpgradeMenuGenerationAtShow, upgradeStationLeftBit, "UpgradeStation_Left")
KHARVOX_DEFINE_MASKED_SCREEN_HOOKS(
    upgradeStationRight, originalUpgradeStationRight, playerUpgradeScreenMask,
    playerUpgradeMenuGenerationAtShow, upgradeStationRightBit, "UpgradeStation_Right")
KHARVOX_DEFINE_MASKED_SCREEN_HOOKS(
    playerUpgradeIdle, originalPlayerUpgradeIdle, playerUpgradeScreenMask,
    playerUpgradeMenuGenerationAtShow, playerUpgradeIdleBit, "PlayerUpgrade_Idle")
KHARVOX_DEFINE_MASKED_SCREEN_HOOKS(
    playerUpgradeInUse, originalPlayerUpgradeInUse, playerUpgradeScreenMask,
    playerUpgradeMenuGenerationAtShow, playerUpgradeInUseBit, "PlayerUpgrade_InUse")
KHARVOX_DEFINE_MASKED_SCREEN_HOOKS(
    playerUpgradeUsed, originalPlayerUpgradeUsed, playerUpgradeScreenMask,
    playerUpgradeMenuGenerationAtShow, playerUpgradeUsedBit, "PlayerUpgrade_Used")

#undef KHARVOX_DEFINE_MASKED_SCREEN_HOOKS

bool installEndOfLevelMenuLifecycleHooks() {
    auto image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!image) return false;
    bool installedAll = true;
    installedAll = installScreenLifecyclePair(
        image, 0x224B730, 0xC174E0, 0xC155F0,
        reinterpret_cast<const void*>(&endOfLevelShowHook),
        reinterpret_cast<const void*>(&endOfLevelHideHook),
        originalEndOfLevelShow, originalEndOfLevelHide, "Gui_EndOfLevel") && installedAll;
    installedAll = installScreenLifecyclePair(
        image, 0x2234300, 0xBA73C0, 0xFB2C10,
        reinterpret_cast<const void*>(&dossierEndOfLevelShowHook),
        reinterpret_cast<const void*>(&dossierEndOfLevelHideHook),
        originalDossierEndOfLevelShow, originalDossierEndOfLevelHide,
        "Dossier_EndOfLevel") && installedAll;
    installedAll = installScreenLifecyclePair(
        image, 0x2235890, 0xBAE500, 0xFB2C10,
        reinterpret_cast<const void*>(&endOfLevelDossierShowHook),
        reinterpret_cast<const void*>(&endOfLevelDossierHideHook),
        originalEndOfLevelDossierShow, originalEndOfLevelDossierHide,
        "EndOfLevelDossier") && installedAll;
    return installedAll;
}

bool installPlayerUpgradeMenuLifecycleHooks() {
    auto image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!image) return false;
    bool installedAll = true;
#define KHARVOX_INSTALL_UPGRADE_SCREEN(vtable, showRva, hideRva, prefix, originalPrefix, label) \
    installedAll = installScreenLifecyclePair( \
        image, vtable, showRva, hideRva, \
        reinterpret_cast<const void*>(&prefix##ShowHook), \
        reinterpret_cast<const void*>(&prefix##HideHook), \
        originalPrefix##Show, originalPrefix##Hide, label) && installedAll
    KHARVOX_INSTALL_UPGRADE_SCREEN(
        0x224D0A0, 0xC1E960, 0xFB2C10,
        playerUpgrade, originalPlayerUpgrade, "Gui_PlayerUpgrade");
    KHARVOX_INSTALL_UPGRADE_SCREEN(
        0x224A888, 0xC113B0, 0x1009F60,
        argentSelection, originalArgentSelection, "Gui_ArgentSelection");
    KHARVOX_INSTALL_UPGRADE_SCREEN(
        0x2247008, 0xBF7040, 0xBF5A50,
        dossierSuit, originalDossierSuit, "Dossier_Suit");
    KHARVOX_INSTALL_UPGRADE_SCREEN(
        0x2237F00, 0xBB97E0, 0xFB2C10,
        dossierSuitDiag, originalDossierSuitDiag, "Dossier_SuitDiag");
    KHARVOX_INSTALL_UPGRADE_SCREEN(
        0x22585A0, 0xC58D10, 0xFB2C10,
        upgradeStationSelection, originalUpgradeStationSelection,
        "UpgradeStation_Selection");
    KHARVOX_INSTALL_UPGRADE_SCREEN(
        0x22583C0, 0xC58BE0, 0xFB2C10,
        upgradeStationLeft, originalUpgradeStationLeft, "UpgradeStation_Left");
    KHARVOX_INSTALL_UPGRADE_SCREEN(
        0x22584B0, 0xC58C10, 0xFB2C10,
        upgradeStationRight, originalUpgradeStationRight, "UpgradeStation_Right");
    KHARVOX_INSTALL_UPGRADE_SCREEN(
        0x2263430, 0xC93A30, 0xFB2C10,
        playerUpgradeIdle, originalPlayerUpgradeIdle, "PlayerUpgrade_Idle");
    KHARVOX_INSTALL_UPGRADE_SCREEN(
        0x2263610, 0xC93840, 0xFB2C10,
        playerUpgradeInUse, originalPlayerUpgradeInUse, "PlayerUpgrade_InUse");
    KHARVOX_INSTALL_UPGRADE_SCREEN(
        0x2263520, 0xC93A30, 0xFB2C10,
        playerUpgradeUsed, originalPlayerUpgradeUsed, "PlayerUpgrade_Used");
#undef KHARVOX_INSTALL_UPGRADE_SCREEN
    return installedAll;
}

void __fastcall runeSelectShowHook(void* screen, int transitionType) {
    runeSelectMenuGenerationAtShow.store(
        KharvoxCameraLevelTransitionGeneration(), std::memory_order_release);
    if (!runeSelectMenuActive.exchange(true, std::memory_order_acq_rel))
        log("native Gui_RuneSelect shown; centered full-frame Quad/UI active (transition="
            + std::to_string(transitionType) + ")");
    originalRuneSelectShow(screen, transitionType);
}

void __fastcall runeSelectHideHook(void* screen, int transitionType) {
    originalRuneSelectHide(screen, transitionType);
    // RuneSelect owns one selection screen rather than an overlapping Dossier
    // hierarchy. Any native HideScreen therefore ends its input and camera
    // ownership immediately, including a confirmed Rune Trial selection.
    if (runeSelectMenuActive.exchange(false, std::memory_order_acq_rel))
        log("native Gui_RuneSelect hidden; Quad/UI session released (transition="
            + std::to_string(transitionType) + ")");
}

bool installRuneSelectMenuLifecycleHooks() {
    auto image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!image) return false;

    // MSVC RTTI from the configured DOOM 6.66 executable identifies the exact
    // idMenuScreen_Gui_RuneSelect class. Hooking its Show/Hide slots prevents
    // the nearby Rune Stone hint or ordinary gameplay from opening the Quad.
    return installScreenLifecyclePair(
        image, 0x223F470, 0xC01C20, 0xBFFF80,
        reinterpret_cast<const void*>(&runeSelectShowHook),
        reinterpret_cast<const void*>(&runeSelectHideHook),
        originalRuneSelectShow, originalRuneSelectHide, "Gui_RuneSelect");
}

void __fastcall runeChallengeShowHook(void* screen, int transitionType) {
    beginRuneChallengeScreenSession(
        "native Gui_EndOfChallenge ShowScreen");
    originalRuneChallengeShow(screen, transitionType);
}

void __fastcall runeChallengeHideHook(void* screen, int transitionType) {
    originalRuneChallengeHide(screen, transitionType);
    if (kharvox::shouldRetainRuneChallengeMapLoadAfterHide(
            runeChallengeScreenActive.load(std::memory_order_acquire),
            runeChallengeMapLoadPending.load(std::memory_order_acquire),
            runeSelectMenuActive.load(std::memory_order_acquire))) {
        log("native Gui_EndOfChallenge HideScreen occurred during pending map load; Quad/UI ownership retained");
        return;
    }
    endRuneChallengeScreenSession(
        "native Gui_EndOfChallenge HideScreen (transition="
        + std::to_string(transitionType) + ")");
}

bool installRuneChallengeMenuLifecycleHooks() {
    auto image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!image) return false;

    // Exact accepted-Start flag write; validate before reading the screen field.
    constexpr unsigned char startCommitSignature[]{0xC6,0x83,0x48,0x02,0x00,0x00,0x01};
    constexpr unsigned char screenGate[]{0x80,0xB9,0x49,0x02,0,0,0};
    constexpr unsigned char managerLoad[]{0x48,0x8B,0x05,0xB9,0xA4,0xEF,0x04};
    constexpr unsigned char managerGate[]{0x83,0x7E,0x08,0x02};
    if (std::memcmp(image + 0xC154C4, startCommitSignature, sizeof(startCommitSignature))
        || std::memcmp(image + 0xC15200, screenGate, sizeof(screenGate))
        || std::memcmp(image + 0xC15210, managerLoad, sizeof(managerLoad))
        || std::memcmp(image + 0xC15226, managerGate, sizeof(managerGate))) {
        log("[RUNE-START] native Start signature mismatch; challenge hooks disabled");
        return false;
    }

    // idMenuScreen_Gui_EndOfChallenge owns the Start / Exit screen at the
    // challenge spawn as well as Retry / Exit after a result. It is a native
    // game menu even though a playable world camera is already active behind
    // it and no cinematic announces its ShowScreen call.
    // Slot 8 is this class' exact HandleAction_Impl. DOOM does not call the
    // inherited HideScreen after Start/Exit/Retry, so the handled command is
    // required to release both Cine Window and menu-control ownership.
    if (!installMenuVtableHook(
            image, 0x224B820, 8, 0xC151D0,
            reinterpret_cast<const void*>(&runeChallengeHandleActionHook),
            originalRuneChallengeHandleAction,
            "Gui_EndOfChallenge HandleAction_Impl")) return false;
    if (!installScreenLifecyclePair(
        image, 0x224B820, 0xC17380, 0xFB2C10,
        reinterpret_cast<const void*>(&runeChallengeShowHook),
        reinterpret_cast<const void*>(&runeChallengeHideHook),
        originalRuneChallengeShow, originalRuneChallengeHide,
        "Gui_EndOfChallenge/RuneTrial")) return false;
    log("native Gui_EndOfChallenge closing-action hook installed: vtable RVA 0x224B820 slot 8");
    return true;
}

constexpr unsigned int tutorialSimpleBit = 1u << 0;
constexpr unsigned int tutorialVideoBit = 1u << 1;
constexpr unsigned int tutorialTextBit = 1u << 2;

// The tutorial manager owns the SWF; rendering may be deferred to a job.
// Resolve the selected screen at draw time: base Frame may transition screens.
std::atomic<void*> tutorialTextSwf{};
using TutorialSpriteRender = void(__fastcall*)(void*, void*, void*, void*, int, bool);
TutorialSpriteRender originalTutorialSpriteRender{};
std::atomic<unsigned long long> tutorialRenderGeneration{0};
std::atomic<unsigned long long> tutorialRenderLogged{0};

bool isTutorialTextManager(void* owner) {
    const auto manager = static_cast<const unsigned char*>(owner);
    if (!manager || !readableRange(manager, 0x4E0)) return false;
    const int id = *reinterpret_cast<const int*>(manager + 8);
    if (id < 0 || id >= 96) return false;
    const int index = *reinterpret_cast<const int*>(manager + 0x350 + id * 4);
    const int count = *reinterpret_cast<const int*>(manager + 0x4D8);
    auto screens = *reinterpret_cast<void* const* const*>(manager + 0x4D0);
    if (count <= 0 || count > 128 || index < 0 || index >= count
        || !screens || !readableRange(screens, count * sizeof(void*))) return false;
    const auto screen = screens[index];
    return screen && readableRange(screen, sizeof(void*))
        && *reinterpret_cast<void* const*>(screen)
            == reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr)) + 0x2257D20;
}

// SWF jobs can execute after manager Frame returns. Carry exact SWF identity
// across that boundary, rather than relying on thread-local call-stack ownership.
void publishTutorialTextSwf(void* manager) {
    if (!manager || !readableRange(manager, 0x28)) return;
    void* swf = *reinterpret_cast<void**>(static_cast<unsigned char*>(manager) + 0x20);
    if (isTutorialTextManager(manager)) {
        if (tutorialTextSwf.exchange(swf) != swf)
            log("[TUTORIAL-RENDER] Text SWF registered for deferred render");
    } else {
        tutorialTextSwf.compare_exchange_strong(swf, nullptr);
    }
}

bool isTutorialTextRoot(void* swf, void* sprite) {
    return swf && swf == tutorialTextSwf.load(std::memory_order_acquire)
        && readableRange(swf, 0x1C4)
        && *reinterpret_cast<void* const*>(static_cast<unsigned char*>(swf) + 0x1A0) == sprite;
}

std::atomic<void*> objectiveSprite{};
std::atomic<void*> voiceCommOwner{},voiceCommSprite{};
std::atomic<unsigned long long> voiceCommLevel{};
using VoiceCommUpdate=void(__fastcall*)(void*);
VoiceCommUpdate originalVoiceCommUpdate{};
thread_local bool voiceCommTransformActive{};
void __fastcall voiceCommUpdateHook(void* widget){
    originalVoiceCommUpdate(widget);
    if(!widget||!readableRange(widget,16))return;
    auto sprite=*reinterpret_cast<void**>(static_cast<unsigned char*>(widget)+8);
    voiceCommOwner.store(widget,std::memory_order_relaxed);
    voiceCommLevel.store(KharvoxCameraLevelTransitionGeneration(),std::memory_order_relaxed);
    if(voiceCommSprite.exchange(sprite,std::memory_order_release)!=sprite&&sprite)
        log("[VOICE-COMM] native VoiceCommunication Flash element bound");
}
using ObjectiveUpdate = void(__fastcall*)(void*);
ObjectiveUpdate originalObjectiveUpdate{};
void __fastcall objectiveUpdateHook(void* widget) {
    originalObjectiveUpdate(widget);
    // Native BindSprite (FAE870) stores the resolved Flash element at +8.
    if (widget && readableRange(widget,16)) {
        void* sprite=*reinterpret_cast<void**>(static_cast<unsigned char*>(widget)+8);
        if (objectiveSprite.exchange(sprite)!=sprite && sprite)
            log("[MISSION-RENDER] Hud_Objective Flash element bound");
    }
}

std::array<std::atomic<void*>,3> runeCounterSprites{};
std::atomic<void*> bossVitalsOwner{},bossVitalsSprite{};
std::atomic<unsigned long long> bossVitalsLevel{},bossVitalsBoundAt{};
using BossVitalsUpdate=void(__fastcall*)(void*);
BossVitalsUpdate originalBossVitalsUpdate{};
thread_local bool bossVitalsTransformActive{};
void __fastcall bossVitalsUpdateHook(void* widget){
    originalBossVitalsUpdate(widget);
    if(!widget||!readableRange(widget,0x1B4))return;
    auto bytes=static_cast<unsigned char*>(widget);
    const int type=*reinterpret_cast<int*>(bytes+0x1B0);
    void* sprite=*reinterpret_cast<void**>(bytes+8);
    if(type>=0&&type<=2&&sprite){
        bossVitalsOwner.store(widget,std::memory_order_relaxed);
        bossVitalsLevel.store(KharvoxCameraLevelTransitionGeneration(),std::memory_order_relaxed);
        bossVitalsBoundAt.store(GetTickCount64(),std::memory_order_relaxed);
        if(bossVitalsSprite.exchange(sprite,std::memory_order_acq_rel)!=sprite)
            log("[BOSS-HUD] r309 BossDemonVitals element bound type="+std::to_string(type));
    }else if(bossVitalsOwner.load(std::memory_order_acquire)==widget){
        bossVitalsSprite.store(nullptr,std::memory_order_release);
    }
}
using RuneCounterUpdate=void(__fastcall*)(void*);
RuneCounterUpdate originalRuneCounterUpdate{};
thread_local bool runeCounterTransformActive{};
thread_local void* runeCanvasSwf{};
thread_local std::array<float,6> runeCanvasMatrix{};
void __fastcall runeCounterUpdateHook(void* screen) {
    originalRuneCounterUpdate(screen);
    if(!screen || !readableRange(screen,0x1E0))return;
    // Gui_EndOfChallenge initialization binds _timer_runes, movement timer,
    // and middle to these three widgets. The left objective and menus are separate.
    for(int i=0;i<3;++i){
        auto widget=*reinterpret_cast<void**>(static_cast<unsigned char*>(screen)+0x1C8+i*8);
        void* sprite=widget && readableRange(widget,16)
            ?*reinterpret_cast<void**>(static_cast<unsigned char*>(widget)+8):nullptr;
        if(runeCounterSprites[i].exchange(sprite)!=sprite && sprite)
            log("[RUNE-COUNTER] bound native gameplay widget="+std::to_string(i));
    }
}

void __fastcall tutorialSpriteRenderHook(
    void* swf, void* view, void* sprite, void* state, int time, bool flag) {
    struct CanvasScope {void* swf;std::array<float,6> matrix;
        ~CanvasScope(){runeCanvasSwf=swf;runeCanvasMatrix=matrix;}
    } canvasScope{runeCanvasSwf,runeCanvasMatrix};
    // Native renderer arguments are live SWF objects; only copy state on the root.
    if(swf && state && sprite==*reinterpret_cast<void**>(static_cast<unsigned char*>(swf)+0x1A0)){
        runeCanvasSwf=swf;
        std::copy_n(static_cast<float*>(state),6,runeCanvasMatrix.begin());
    }
    const auto voiceOwner=voiceCommOwner.load(std::memory_order_relaxed);
    if(sprite&&!voiceCommTransformActive&&sprite==voiceCommSprite.load(std::memory_order_acquire)
        &&voiceCommLevel.load(std::memory_order_relaxed)==KharvoxCameraLevelTransitionGeneration()
        &&voiceOwner&&readableRange(voiceOwner,16)
        &&*reinterpret_cast<uintptr_t*>(voiceOwner)==reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))+0x225CA20
        &&*reinterpret_cast<void**>(static_cast<unsigned char*>(voiceOwner)+8)==sprite
        &&swf&&swf==runeCanvasSwf&&state&&readableRange(state,24)&&readableRange(swf,0x1C4)){
        auto matrix=static_cast<float*>(state);
        std::array<float,6> before{};std::copy_n(matrix,6,before.begin());
        const float width=*reinterpret_cast<float*>(static_cast<unsigned char*>(swf)+0x1BC);
        const float height=*reinterpret_cast<float*>(static_cast<unsigned char*>(swf)+0x1C0);
        const auto after=kharvox::voiceCommMatrix(before,runeCanvasMatrix,width,height);
        struct Restore {float* matrix;std::array<float,6> value;bool active;
            ~Restore(){std::copy(value.begin(),value.end(),matrix);voiceCommTransformActive=active;}
        } restore{matrix,before,voiceCommTransformActive};
        voiceCommTransformActive=true;
        std::copy(after.begin(),after.end(),matrix);
        static std::atomic<bool> logged{};
        if(!logged.exchange(true))log("[VOICE-COMM] size=54%; canvas anchor=55%,72%; native plane depth retained");
        originalTutorialSpriteRender(swf,view,sprite,state,time,flag);
        return;
    }
    if(sprite&&!bossVitalsTransformActive
        &&sprite==bossVitalsSprite.load(std::memory_order_acquire)
        &&bossVitalsLevel.load(std::memory_order_relaxed)==KharvoxCameraLevelTransitionGeneration()
        &&GetTickCount64()<=bossVitalsBoundAt.load(std::memory_order_relaxed)+500
        &&swf&&swf==runeCanvasSwf&&state&&readableRange(state,24)&&readableRange(swf,0x1C4)){
        auto matrix=static_cast<float*>(state);
        std::array<float,6> before{};std::copy_n(matrix,6,before.begin());
        const float height=*reinterpret_cast<float*>(static_cast<unsigned char*>(swf)+0x1C0);
        const float width=*reinterpret_cast<float*>(static_cast<unsigned char*>(swf)+0x1BC);
        const auto after=kharvox::bossHealthBarMatrix(before,runeCanvasMatrix,width,height);
        struct Restore {float* matrix;std::array<float,6> value;bool active;
            ~Restore(){std::copy(value.begin(),value.end(),matrix);bossVitalsTransformActive=active;}
        } restore{matrix,before,bossVitalsTransformActive};
        bossVitalsTransformActive=true;
        std::copy(after.begin(),after.end(),matrix);
        static std::atomic<bool> logged{};
        if(!logged.exchange(true))log("[BOSS-HUD] r310 health bar size=40%; lowered by 25% canvas height");
        originalTutorialSpriteRender(swf,view,sprite,state,time,flag);
        return;
    }
    int runeCounterIndex=-1;
    if(sprite && !runeCounterTransformActive)
        for(int i=0;i<3;++i)
            if(sprite==runeCounterSprites[i].load(std::memory_order_acquire))runeCounterIndex=i;
    if(runeCounterIndex>=0 && swf==runeCanvasSwf && state && readableRange(state,24) && swf && readableRange(swf,0x1C4)){
        auto matrix=static_cast<float*>(state);
        std::array<float,6> before{};std::copy_n(matrix,6,before.begin());
        const float height=*reinterpret_cast<float*>(static_cast<unsigned char*>(swf)+0x1C0);
        const float width=*reinterpret_cast<float*>(static_cast<unsigned char*>(swf)+0x1BC);
        const auto after=kharvox::runeCounterMatrix(before,runeCanvasMatrix,width,height,runeCounterIndex==2);
        struct Restore {float* p;std::array<float,6> m;bool previous;
            ~Restore(){std::copy(m.begin(),m.end(),p);runeCounterTransformActive=previous;}
        } restore{matrix,before,runeCounterTransformActive};
        runeCounterTransformActive=true;
        std::copy(after.begin(),after.end(),matrix);
        static std::atomic<bool> logged{false};
        if(!logged.exchange(true))log("[RUNE-COUNTER] native timer/counter size=42.5%; timer height compensated; kill counter additionally lowered");
        originalTutorialSpriteRender(swf,view,sprite,state,time,flag);
        return;
    }
    if (sprite && sprite == objectiveSprite.load(std::memory_order_acquire)
        && state && readableRange(state,24) && swf && readableRange(swf,0x1C4)) {
        auto matrix=static_cast<float*>(state);
        std::array<float,6> before{}; std::copy_n(matrix,6,before.begin());
        const auto canvas=reinterpret_cast<const float*>(static_cast<unsigned char*>(swf)+0x1BC);
        const auto after=kharvox::missionTextMatrix(before,canvas[0],canvas[1]);
        struct Restore { float* p; std::array<float,6> m;
            ~Restore(){std::copy(m.begin(),m.end(),p);} } restore{matrix,before};
        std::copy(after.begin(),after.end(),matrix);
        static std::atomic<bool> logged{false};
        if(!logged.exchange(true))log("[MISSION-RENDER] exact Objective element shifted right=20% down=18%; scale unchanged");
        originalTutorialSpriteRender(swf,view,sprite,state,time,flag);
        return;
    }
    if (!isTutorialTextRoot(swf, sprite) || !state || !readableRange(state, 24)) {
        originalTutorialSpriteRender(swf, view, sprite, state, time, flag);
        return;
    }
    auto matrix = static_cast<float*>(state);
    const float height = *reinterpret_cast<const float*>(
        static_cast<const unsigned char*>(swf) + 0x1C0);
    const float width = *reinterpret_cast<const float*>(
        static_cast<const unsigned char*>(swf) + 0x1BC);
    std::array<float, 6> before{};
    std::copy_n(matrix, 6, before.begin());
    const auto after = kharvox::tutorialTextMatrix(before, width, height);
    // Native render state is caller-stack storage, not persistent Flash state.
    // Root scale/translation change; depth and animated child transforms stay native.
    struct Restore { float* value; std::array<float,6> before;
        ~Restore() { std::copy(before.begin(), before.end(), value); } } restore{matrix, before};
    std::copy(after.begin(), after.end(), matrix);
    const auto generation = tutorialRenderGeneration.load(std::memory_order_relaxed);
    if (tutorialRenderLogged.exchange(generation + 1) != generation + 1)
        log("[TUTORIAL-RENDER] Text owned root session=" + std::to_string(generation)
            + " height=" + std::to_string(height) + " scaleY=" + std::to_string(matrix[1])
            + " yBefore=" + std::to_string(before[5]) + " yAfter=" + std::to_string(after[5]) + " sizeFactor=0.55");
    originalTutorialSpriteRender(swf, view, sprite, state, time, flag);
}

bool installTutorialRenderHook() {
    auto image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    constexpr std::array<unsigned char, 15> signature{
        0x48,0x8B,0xC4,0x4C,0x89,0x48,0x20,0x4C,0x89,0x40,0x18,0x48,0x89,0x50,0x10};
    if (!image || !readableRange(image + 0x1621000, signature.size())
        || std::memcmp(image + 0x1621000, signature.data(), signature.size())) {
        log("[TUTORIAL-RENDER] signature mismatch; native placement retained");
        return false;
    }
    return installEntryHook(image + 0x1621000, signature,
        reinterpret_cast<const void*>(&tutorialSpriteRenderHook),
        originalTutorialSpriteRender, "Tutorial owned SWF root translation");
}

void beginTutorialScreenActivity(
    unsigned int screenBit, const char* screenName, int transitionType) {
    if (screenBit == tutorialTextBit) {
        tutorialRenderGeneration.fetch_add(1);
        log("[TUTORIAL-RENDER] Text shown; awaiting owned SWF root");
    }
    const auto now = GetTickCount64();
    tutorialSessionUntilTick.store(
        now + tutorialSessionMilliseconds, std::memory_order_release);
    const auto previous = tutorialScreenMask.fetch_or(
        screenBit, std::memory_order_acq_rel);
    if (!(previous & screenBit))
        log(std::string("native ") + screenName
            + " shown; Projection exclusion active, Quad/camera lock suppressed (transition="
            + std::to_string(transitionType) + ")");
}

void endTutorialScreenActivity(
    unsigned int screenBit, const char* screenName, int transitionType) {
    const auto previous = tutorialScreenMask.fetch_and(
        ~screenBit, std::memory_order_acq_rel);
    if (!(previous & screenBit)) return;
    if (screenBit == tutorialTextBit)
        log(std::string("[TUTORIAL-RENDER] Text hidden; ownedRootObserved=")
            + (tutorialRenderLogged.load() == tutorialRenderGeneration.load() + 1 ? "true" : "false"));
    const auto remaining = previous & ~screenBit;
    log(std::string("native ") + screenName
        + (remaining
            ? " hidden; overlapping Tutorial keeps Projection exclusion active"
            : " hidden; Tutorial activity lifecycle closed")
        + " (transition=" + std::to_string(transitionType) + ")");
}

#define KHARVOX_DEFINE_TUTORIAL_SCREEN_HOOKS(prefix, originalPrefix, bit, label) \
    void __fastcall prefix##ShowHook(void* screen, int transitionType) { \
        beginTutorialScreenActivity(bit, label, transitionType); \
        originalPrefix##Show(screen, transitionType); \
    } \
    void __fastcall prefix##HideHook(void* screen, int transitionType) { \
        originalPrefix##Hide(screen, transitionType); \
        endTutorialScreenActivity(bit, label, transitionType); \
    }

KHARVOX_DEFINE_TUTORIAL_SCREEN_HOOKS(
    tutorialSimple, originalTutorialSimple,
    tutorialSimpleBit, "Tutorial_Simple")
KHARVOX_DEFINE_TUTORIAL_SCREEN_HOOKS(
    tutorialVideo, originalTutorialVideo,
    tutorialVideoBit, "Tutorial_Video")
KHARVOX_DEFINE_TUTORIAL_SCREEN_HOOKS(
    tutorialText, originalTutorialText,
    tutorialTextBit, "Tutorial_Text")

#undef KHARVOX_DEFINE_TUTORIAL_SCREEN_HOOKS

void __fastcall tutorialManagerFrameHook(
    void* tutorialManager, int frameTime) {
    publishTutorialTextSwf(tutorialManager);
    originalTutorialManagerFrame(tutorialManager, frameTime);
    publishTutorialTextSwf(tutorialManager);

    // idMenuManager_Tutorial keeps the owner of the currently rendered
    // tutorial at +0x8E8. The manager itself exists outside tutorials, so its
    // frame callback alone is not a lifecycle signal; the non-null active
    // screen owner is. This is the path used by the Elite Guard/Praetor-token
    // pickup after its short cinematic.
    const auto bytes = static_cast<const unsigned char*>(tutorialManager);
    if (*reinterpret_cast<void* const*>(bytes + 0x8E8))
        pulseTutorialActiveSession();
}

std::atomic<unsigned long long> hudMovieUntil{};
using HudMovieFrame=void(__fastcall*)(void*,int);
HudMovieFrame originalHudMovieFrame{};
using HudMoviePlaying=bool(__fastcall*)(void*);
HudMoviePlaying nativeHudMoviePlaying{};
std::atomic<uintptr_t> hudMovieOwner{};
void observeHudMovie(void* manager,const char* source){
    const auto bytes=static_cast<unsigned char*>(manager);
    auto image=reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if(!bytes || !nativeHudMoviePlaying || !readableRange(bytes,sizeof(void*))
        || *reinterpret_cast<void**>(bytes)!=image+0x2241010 || !readableRange(bytes,0x9B8))return;
    auto swf=*reinterpret_cast<unsigned char**>(bytes+0x20);
    auto material=*reinterpret_cast<unsigned char**>(bytes+0x9B0);
    auto movie=material && readableRange(material,0x1A0)
        ?*reinterpret_cast<void**>(material+0x198):nullptr;
    const bool visible=swf && readableRange(swf,0x1B5) && swf[0x1B4];
    const bool playing=movie && nativeHudMoviePlaying(manager);
    const bool active=visible && playing;
    const int state=(visible?1:0)|(material?2:0)|(movie?4:0)|(playing?8:0);
    static std::atomic<int> previousState{-1};
    if(previousState.exchange(state)!=state)
        log(std::string("[HUD-MOVIE] observer=")+source+" state="+std::to_string(state)
            +" visible="+std::to_string(visible)+" material="+std::to_string(material!=nullptr)
            +" movie="+std::to_string(movie!=nullptr)+" playing="+std::to_string(playing));
    if(active){
        hudMovieOwner.store(reinterpret_cast<uintptr_t>(manager));
        const auto before=hudMovieUntil.exchange(GetTickCount64()+250);
        if(!before)log("[HUD-MOVIE] visible native HUD video; full-frame Quad requested");
    }else if(hudMovieOwner.load()==reinterpret_cast<uintptr_t>(manager)){
        if(hudMovieUntil.exchange(0))log("[HUD-MOVIE] native HUD video ended; automatic presentation restored");
    }
}
void __fastcall hudMovieFrameHook(void* manager,int frameTime){
    originalHudMovieFrame(manager,frameTime);
    observeHudMovie(manager,"manager-frame");
}
bool installHudMovieHook(){
    auto image=reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    constexpr unsigned char playingSignature[]{0x48,0x89,0x5C,0x24,0x10,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0x99,0xB0,0x09,0,0};
    if(!image || std::memcmp(image+0xBDED30,playingSignature,sizeof(playingSignature))){
        log("[HUD-MOVIE] native playback signature mismatch; hook disabled");return false;
    }
    nativeHudMoviePlaying=reinterpret_cast<HudMoviePlaying>(image+0xBDED30);
    return installMenuVtableHook(image,0x2241010,9,0xBE0E40,
        reinterpret_cast<const void*>(&hudMovieFrameHook),originalHudMovieFrame,
        "Hud_Cinematic movie frame");
}

bool installTutorialLifecycleHooks() {
    auto image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!image) return false;
    bool installedAll = true;
#define KHARVOX_INSTALL_TUTORIAL_SCREEN(vtable, showRva, prefix, originalPrefix, label) \
    installedAll = installScreenLifecyclePair( \
        image, vtable, showRva, 0xFB2C10, \
        reinterpret_cast<const void*>(&prefix##ShowHook), \
        reinterpret_cast<const void*>(&prefix##HideHook), \
        originalPrefix##Show, originalPrefix##Hide, label) && installedAll
    KHARVOX_INSTALL_TUTORIAL_SCREEN(
        0x2257B40, 0xC57640,
        tutorialSimple, originalTutorialSimple, "Tutorial_Simple");
    KHARVOX_INSTALL_TUTORIAL_SCREEN(
        0x2257C30, 0xC576B0,
        tutorialVideo, originalTutorialVideo, "Tutorial_Video");
    KHARVOX_INSTALL_TUTORIAL_SCREEN(
        0x2257D20, 0xC57670,
        tutorialText, originalTutorialText, "Tutorial_Text");
#undef KHARVOX_INSTALL_TUTORIAL_SCREEN

    const bool managerInstalled = installMenuVtableHook(
        image, 0x2243F78, 9, 0xBE6C80,
        reinterpret_cast<const void*>(&tutorialManagerFrameHook),
        originalTutorialManagerFrame, "Tutorial manager active frame");
    if (managerInstalled)
        log("native Tutorial manager active-frame hook installed: vtable RVA 0x2243F78 slot 9");
    return managerInstalled && installedAll;
}

bool finiteValues(const float* values, int count) {
    if (!values) return false;
    for (int index = 0; index < count; ++index)
        if (!std::isfinite(values[index])) return false;
    return true;
}

std::array<float, 3> rotateXrQuaternionInDoomCoordinates(
    const std::array<float, 4>& quaternion,
    const std::array<float, 3>& doomVector) {
    const std::array<float, 3> xr{-doomVector[1], doomVector[2], -doomVector[0]};
    const std::array<float, 3> u{quaternion[0], quaternion[1], quaternion[2]};
    const float dotUV = u[0] * xr[0] + u[1] * xr[1] + u[2] * xr[2];
    const float dotUU = u[0] * u[0] + u[1] * u[1] + u[2] * u[2];
    const std::array<float, 3> cross{
        u[1] * xr[2] - u[2] * xr[1],
        u[2] * xr[0] - u[0] * xr[2],
        u[0] * xr[1] - u[1] * xr[0]
    };
    const std::array<float, 3> rotated{
        2.0f * dotUV * u[0] + (quaternion[3] * quaternion[3] - dotUU) * xr[0]
            + 2.0f * quaternion[3] * cross[0],
        2.0f * dotUV * u[1] + (quaternion[3] * quaternion[3] - dotUU) * xr[1]
            + 2.0f * quaternion[3] * cross[1],
        2.0f * dotUV * u[2] + (quaternion[3] * quaternion[3] - dotUU) * xr[2]
            + 2.0f * quaternion[3] * cross[2]
    };
    return {-rotated[2], -rotated[0], rotated[1]};
}

std::array<float, 3> bodyVectorToWorld(
    const float bodyAxis[9], const std::array<float, 3>& local) {
    return {
        local[0] * bodyAxis[0] + local[1] * bodyAxis[3] + local[2] * bodyAxis[6],
        local[0] * bodyAxis[1] + local[1] * bodyAxis[4] + local[2] * bodyAxis[7],
        local[0] * bodyAxis[2] + local[1] * bodyAxis[5] + local[2] * bodyAxis[8]
    };
}

bool getHandWorldPose(bool rightHand, float origin[3], float axis[9]) {
    std::lock_guard<std::mutex> poseGuard(handPoseMutex);
    auto& hand = handPoses[rightHand ? 0 : 1];
    if (!hand.valid.load(std::memory_order_acquire)) return false;
    float bodyOrigin[3]{}, bodyAxis[9]{};
    if (!KharvoxCameraGetBodyPose(bodyOrigin, bodyAxis)) return false;

    std::array<float, 3> grip{};
    std::array<float, 4> quaternion{};
    for (int index = 0; index < 3; ++index)
        grip[index] = hand.grip[index].load(std::memory_order_relaxed);
    for (int index = 0; index < 4; ++index)
        quaternion[index] = hand.quaternion[index].load(std::memory_order_relaxed);
    const auto worldGrip = bodyVectorToWorld(bodyAxis, grip);
    for (int component = 0; component < 3; ++component)
        origin[component] = bodyOrigin[component] + worldGrip[component];

    constexpr std::array<std::array<float, 3>, 3> basis{{
        {1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f}
    }};
    for (int row = 0; row < 3; ++row) {
        const auto bodyRow = rotateXrQuaternionInDoomCoordinates(quaternion, basis[row]);
        const auto worldRow = bodyVectorToWorld(bodyAxis, bodyRow);
        for (int component = 0; component < 3; ++component)
            axis[row * 3 + component] = worldRow[component];
    }
    return finiteValues(origin, 3) && finiteValues(axis, 9);
}

bool getOffhandHudFrame(float origin[3],float axis[9]) {
    static std::mutex mutex;std::lock_guard<std::mutex> guard(mutex);
    static unsigned long long present=~0ull;static bool valid{};static float position[3]{},basis[9]{};
    const auto now=KharvoxCameraCurrentPresentSerial();
    if(now!=present){present=now;valid=getHandWorldPose(offhandHudLeftMode(),position,basis);}
    if(!valid)return false;std::memcpy(origin,position,sizeof(position));std::memcpy(axis,basis,sizeof(basis));return true;
}

void pollOffhandHudHotkeys(){
    static bool rotate=true,plusWasDown=false,resetWasDown=false,toggleWasDown=false;
    static unsigned long long nextStep{};
    DWORD process{};GetWindowThreadProcessId(GetForegroundWindow(),&process);
    const bool active=process==GetCurrentProcessId()&&KharvoxCameraWorldActive()
        &&offhandCalibrationActive.load(std::memory_order_acquire)
        &&!(GetAsyncKeyState(VK_MENU)&0x8000)&&!(GetAsyncKeyState(VK_CONTROL)&0x8000);
    const bool plus=(GetAsyncKeyState(VK_ADD)&0x8000)!=0;
    const bool reset=(GetAsyncKeyState(VK_NUMPAD5)&0x8000)!=0;
    const bool toggle=(GetAsyncKeyState(VK_NUMPAD0)&0x8000)!=0;
    const bool switchMode=active&&plus&&!plusWasDown;
    const bool resetNow=active&&reset&&!resetWasDown;
    const bool toggleNow=active&&toggle&&!toggleWasDown;
    plusWasDown=plus;resetWasDown=reset;toggleWasDown=toggle;
    if(!active){nextStep=0;return;}
    if(toggleNow){const int selected=1-offhandSelectedSurface.load();offhandSelectedSurface.store(selected);
        offhandSelectionUntil.store(GetTickCount64()+1200);
        log(std::string("[OFFHAND-HUD] selected=")+(selected?"Ammo":"Life"));}
    if(switchMode){rotate=!rotate;log(std::string("[OFFHAND-HUD] calibration mode=")+(rotate?"rotation":"position"));}
    const auto now=GetTickCount64();if(now<nextStep&&!resetNow&&!toggleNow)return;
    auto down=[](int key){return (GetAsyncKeyState(key)&0x8000)!=0;};
    const int x=int(down(VK_NUMPAD6))-int(down(VK_NUMPAD4));
    const int y=int(down(VK_NUMPAD8))-int(down(VK_NUMPAD2));
    const int z=int(down(VK_NUMPAD9))-int(down(VK_NUMPAD7));
    const int size=int(down(VK_MULTIPLY))-int(down(VK_DIVIDE));
    if(!x&&!y&&!z&&!size&&!resetNow){nextStep=0;return;}
    const bool fine=down(VK_SHIFT);kharvox::OffhandHudConfig config;
    {std::lock_guard<std::mutex> lock(offhandHudMutex);config=offhandHudConfig;}
    auto& c=config.modes[offhandSelectedSurface.load()*2+(offhandHudLeftMode()?1:0)];
    config.enabled=true;
    if(resetNow){
        kharvox::OffhandHudConfig defaults;
        std::ifstream input(kharvox::runtimePathA("offhand_hud_default.cfg"));
        c=kharvox::readOffhandHudConfig(input,defaults)
            ?defaults.modes[offhandSelectedSurface.load()*2+(offhandHudLeftMode()?1:0)]:kharvox::OffhandHudCalibration{};
    }
    else {
        if(rotate){const float step=fine?2.5f:5.f;
            c.degrees[0]=std::remainder(c.degrees[0]+y*step,360.f);
            c.degrees[1]=std::remainder(c.degrees[1]+x*step,360.f);
            c.degrees[2]=std::remainder(c.degrees[2]+z*step,360.f);
        }else{const float step=fine?.25f:.5f;
            c.centimeters[0]=std::clamp(c.centimeters[0]-z*step,-100.f,100.f);
            c.centimeters[1]=std::clamp(c.centimeters[1]-x*step,-100.f,100.f);
            c.centimeters[2]=std::clamp(c.centimeters[2]+y*step,-100.f,100.f);
        }
        c.scale=std::clamp(c.scale+size*(fine?.005f:.01f),.02f,2.f);
    }
    const auto path=kharvox::runtimePathA("offhand_hud.cfg"),temp=path+".hotkeys.tmp";
    std::ofstream output(temp,std::ios::trunc);output.imbue(std::locale::classic());
    output<<"2 "<<int(config.enabled)<<'\n';
    for(const auto& mode:config.modes){for(float v:mode.centimeters)output<<v<<' ';for(float v:mode.degrees)output<<v<<' ';output<<mode.scale<<'\n';}
    output.close();
    if(!output||!MoveFileExA(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){
        log("[OFFHAND-HUD] calibration save failed; keeping previous values");nextStep=now+1000;return;
    }
    {std::lock_guard<std::mutex> lock(offhandHudMutex);offhandHudConfig=config;}
    nextStep=now+100;
    std::ostringstream status;status<<"[OFFHAND-HUD] SAVED "<<(offhandHudLeftMode()?"left-mode/right-hand":"normal/left-hand")
        <<" target="<<(offhandSelectedSurface.load()?"Ammo":"Life")<<" mode="<<(rotate?"rotation":"position")<<" enabled="<<config.enabled;
    for(float v:c.centimeters)status<<" pos="<<v;for(float v:c.degrees)status<<" deg="<<v;
    status<<" size="<<c.scale;log(status.str());
}

bool getAnchorPose(KharvoxHudAnchor anchor, float origin[3], float axis[9]) {
    if (anchor == KharvoxHudAnchor::RightHand) return getHandWorldPose(true, origin, axis);
    if (anchor == KharvoxHudAnchor::LeftHand) return getHandWorldPose(false, origin, axis);
    return anchor >= KharvoxHudAnchor::Head && anchor <= KharvoxHudAnchor::Auxiliary
        && KharvoxCameraGetHeadRenderPose(origin, axis);
}

int roleIndex(KharvoxHudAnchor anchor) {
    const int index = static_cast<int>(anchor) - 1;
    return index >= 0 && index < hudRoleCount ? index : -1;
}

bool readableRange(const void* address, size_t bytes) {
    if (!address || !bytes) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT
        || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    const auto first = reinterpret_cast<uintptr_t>(address);
    const auto last = first + bytes;
    const auto regionLast = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    return last >= first && last <= regionLast;
}

bool writableRange(const void* address, size_t bytes) {
    if (!readableRange(address, bytes)) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof(info))) return false;
    const DWORD protection = info.Protect & 0xFF;
    return protection == PAGE_READWRITE || protection == PAGE_WRITECOPY
        || protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

void loadHeadlockedHudSettings() {
    float worldUnitsPerMeter = defaultWorldUnitsPerMeter;
    {
        std::ifstream input(kharvox::runtimePathA("world_scale.cfg"));
        float configured{};
        if (input >> configured && std::isfinite(configured))
            worldUnitsPerMeter = std::clamp(configured, 1.0f, 200.0f);
    }

    float distanceMeters = defaultHudDistanceMeters;
    float legacyScale = defaultHudQuadScale;
    {
        std::ifstream input(kharvox::runtimePathA("hud_headlock.cfg"));
        float configuredDistance{};
        if (input >> configuredDistance && std::isfinite(configuredDistance)) {
            distanceMeters = std::clamp(
                configuredDistance, minimumHudDistanceMeters, maximumHudDistanceMeters);
            float configuredScale{};
            if (input >> configuredScale && std::isfinite(configuredScale))
                legacyScale = std::clamp(configuredScale, minimumHudScale, maximumHudScale);
        }
    }
    float scale = legacyScale;
    {
        std::ifstream input(kharvox::runtimePathA("hud_quad_scale_default.cfg"));
        float defaultScale{};
        if (input >> defaultScale && std::isfinite(defaultScale))
            scale = std::clamp(defaultScale, minimumHudScale, maximumHudScale);
    }
    {
        std::ifstream input(kharvox::runtimePathA("hud_quad_scale_saved.cfg"));
        float savedScale{};
        if (input >> savedScale && std::isfinite(savedScale))
            scale = std::clamp(savedScale, minimumHudScale, maximumHudScale);
    }
    float horizontalOffsetTan = defaultHudHorizontalOffsetTan;
    bool flatCalibrationLoaded{};
    bool flatCalibrationUsesSavedOverride{};
    auto loadFlatCalibration = [&](const std::string& path) {
        std::ifstream input(path);
        float savedDistance{}, savedScale{}, savedHorizontalOffsetTan{};
        if (input >> savedDistance >> savedScale
            && std::isfinite(savedDistance) && std::isfinite(savedScale)) {
            distanceMeters = std::clamp(
                savedDistance, minimumHudDistanceMeters, maximumHudDistanceMeters);
            scale = std::clamp(savedScale, minimumHudScale, maximumHudScale);
            if (input >> savedHorizontalOffsetTan
                && std::isfinite(savedHorizontalOffsetTan)) {
                horizontalOffsetTan = std::clamp(savedHorizontalOffsetTan,
                    minimumHudHorizontalOffsetTan, maximumHudHorizontalOffsetTan);
            }
            flatCalibrationLoaded = true;
            return true;
        }
        return false;
    };
    flatCalibrationUsesSavedOverride = loadFlatCalibration(flatCalibrationPath());
    if (!flatCalibrationUsesSavedOverride)
        loadFlatCalibration(flatCalibrationDefaultPath());
    // The repository calibration is now the launch baseline. Debugging can
    // still create a saved override, and Num 5 resets to whichever complete
    // calibration was active when this session started.
    hudLauncherDistanceMeters = distanceMeters;
    hudLauncherScale = scale;
    hudLauncherHorizontalOffsetTan = horizontalOffsetTan;
    hudWorldUnitsPerMeter = worldUnitsPerMeter;
    hudDistanceMeters.store(distanceMeters, std::memory_order_release);
    hudQuadScale.store(scale, std::memory_order_release);
    hudHorizontalOffsetTan.store(horizontalOffsetTan, std::memory_order_release);
    std::ostringstream out;
    out << std::fixed << std::setprecision(2)
        << "headlocked HUD settings: distance=" << distanceMeters
        << "m quadSize=" << scale
        << "x horizontalTan=" << std::setprecision(4) << horizontalOffsetTan
        << " source=" << (flatCalibrationUsesSavedOverride
            ? "saved debug override"
            : flatCalibrationLoaded ? "repository default" : "legacy fallback")
        << " HUD-debugging=" << (hudDebuggingEnabled ? "enabled" : "disabled");
    log(out.str());
}

void restorePendingHudSubmission(const PendingHudSubmission& pending) {
    if (!pending.active) return;
    if (writableRange(reinterpret_cast<void*>(pending.context + 0x60), 0x34)) {
        std::memcpy(reinterpret_cast<void*>(pending.context + 0x60),
            pending.contextOrigin.data(), sizeof(float) * pending.contextOrigin.size());
        std::memcpy(reinterpret_cast<void*>(pending.context + 0x90),
            &pending.contextScale, sizeof(pending.contextScale));
    }
}

void restoreAllPendingHudSubmissions() {
    while (pendingHudDepth) {
        const auto pending = pendingHudStack[--pendingHudDepth];
        pendingHudStack[pendingHudDepth] = {};
        restorePendingHudSubmission(pending);
    }
}

HudCalibrationSnapshot calibrationForCurrentRenderFrame() {
    // The controls are polled from vkQueuePresentKHR and may run on a different
    // thread from DOOM's HUD front end. Latch one complete set per Present
    // generation so every element drawn into a frame sees identical values.
    const auto present = KharvoxCameraCurrentPresentSerial();
    std::lock_guard<std::mutex> guard(hudFrameCalibrationMutex);
    if (hudFrameCalibrationPresent != present) {
        hudFrameCalibration.distanceMeters =
            hudDistanceMeters.load(std::memory_order_acquire);
        hudFrameCalibration.elementScale =
            hudQuadScale.load(std::memory_order_acquire);
        hudFrameCalibration.horizontalOffsetTan =
            hudHorizontalOffsetTan.load(std::memory_order_acquire);
        hudFrameCalibration.headsetFitScale =
            hudHeadsetFitScale.load(std::memory_order_acquire);
        hudFrameCalibrationPresent = present;
    }
    return hudFrameCalibration;
}

bool buildHeadlockedOriginTransform(
    const float* nativeOrigin, bool crosshair, bool offscreen,
    const void* finalEntity,
    int profileIndex, float desiredOrigin[3], float headAxisOut[9],
    float* nativeDepthOut, const HudCalibrationSnapshot& calibration,
    const HudProfileAdjustment& profileAdjustment) {
    float centerOrigin[3]{}, centerAxis[9]{};
    if (!finiteValues(nativeOrigin, 3)
        || (!KharvoxCameraGetHudCenterRenderPose(centerOrigin, centerAxis)
            && !KharvoxCameraGetHeadRenderPose(centerOrigin, centerAxis))) return false;
    std::memcpy(headAxisOut, centerAxis, sizeof(centerAxis));

    float localOrigin[3]{};
    for (int centerRow = 0; centerRow < 3; ++centerRow)
        for (int world = 0; world < 3; ++world)
            localOrigin[centerRow] += (nativeOrigin[world] - centerOrigin[world])
                * centerAxis[centerRow * 3 + world];
    if (nativeDepthOut) *nativeDepthOut = localOrigin[0];

    std::array<float, 3> stableLocalOrigin{};
    if (!crosshair) {
        if (!finalEntity || profileIndex < 0) return false;

        {
            std::lock_guard<std::mutex> guard(flatHudProfilePoseMutex);
            if (profileIndex >= static_cast<int>(flatHudProfilePoses.size())) return false;
            auto& profilePose = flatHudProfilePoses[profileIndex];
            profilePose.lastSeenTick = GetTickCount64();
            if (!profilePose.originValid) {
                // The measured native HUD-depth band remains the fail-closed
                // boundary against an unrecognized world GUI. Once accepted,
                // this profile's original flat-screen position becomes its
                // stable layout reference for the session.
                if (localOrigin[0] < nativeHudNearDepth || localOrigin[0] > nativeHudFarDepth)
                    return false;
                std::copy(std::begin(localOrigin), std::end(localOrigin),
                    profilePose.nativeLocalOrigin.begin());
                profilePose.originValid = true;
            }
            stableLocalOrigin = profilePose.nativeLocalOrigin;
        }
        if (nativeDepthOut) *nativeDepthOut = stableLocalOrigin[0];
    }

    if (offscreen) {
        // Crosshair stays rigidly behind the current HMD pose. It therefore
        // remains outside the view even after arbitrary 6DoF head movement.
        // During a native Ledge transition every validated gameplay-HUD
        // surface uses this same placement and returns automatically when the
        // ledge state ends.
        const float backward = -10.0f * hudWorldUnitsPerMeter;
        const float downward = -10.0f * hudWorldUnitsPerMeter;
        for (int world = 0; world < 3; ++world)
            desiredOrigin[world] = centerOrigin[world]
                + centerAxis[world] * backward
                + centerAxis[6 + world] * downward;
    } else {
        // Apply a constant depth adjustment to the native plane. The
        // the launcher setting names the target centre explicitly, while the
        // small native depth differences are retained to preserve layering.
        const float profileDistanceMeters = std::clamp(
            calibration.distanceMeters + profileAdjustment.distanceOffsetMeters,
            minimumHudDistanceMeters, maximumHudDistanceMeters);
        const float targetDepth = profileDistanceMeters * hudWorldUnitsPerMeter
            + (stableLocalOrigin[0] - nativeHudReferenceDepth);
        if (targetDepth < 0.05f * hudWorldUnitsPerMeter) return false;
        // Preserve the authored Flat-game screen coordinates. Headset FOV fit
        // and target depth determine only where each element sits. User zoom
        // is deliberately absent here and affects each element's own geometry
        // later, rather than spreading the complete HUD layout apart.
        const float positionFitScale = std::clamp(
            calibration.headsetFitScale, 0.35f, 2.0f);
        const float angularPositionScale =
            targetDepth / stableLocalOrigin[0] * positionFitScale;
        // Keep the global centering correction angular and independent of the
        // authored Flat-HUD layout. Distance and size can then be calibrated
        // without changing this offset or the relative placement of elements.
        const float authoredLateral = profileAdjustment.centerPreview
            ? 0.0f : stableLocalOrigin[1] * angularPositionScale;
        const float authoredVertical = profileAdjustment.centerPreview
            ? 0.0f : stableLocalOrigin[2] * angularPositionScale;
        const float horizontalOffset = targetDepth * (
            calibration.horizontalOffsetTan
            + (profileAdjustment.centerPreview ? 0.0f : profileAdjustment.lateralOffsetTan));
        const float verticalOffset = profileAdjustment.centerPreview
            ? 0.0f : targetDepth * profileAdjustment.verticalOffsetTan;
        for (int world = 0; world < 3; ++world)
            desiredOrigin[world] = centerOrigin[world]
                + centerAxis[world] * targetDepth
                + centerAxis[3 + world] * (authoredLateral + horizontalOffset)
                + centerAxis[6 + world] * (authoredVertical + verticalOffset);
    }
    return finiteValues(desiredOrigin, 3);
}

bool buildStableHeadlockedAxis(
    const PendingHudSubmission& pending, const float* nativeAxis,
    float desiredAxis[9]) {
    if (pending.offscreen) {
        if (!finiteValues(nativeAxis, 9)) return false;
        std::memcpy(desiredAxis, nativeAxis, sizeof(float) * 9);
        return true;
    }
    if (!pending.expectedFinalEntity || pending.profileIndex < 0
        || !finiteValues(nativeAxis, 9) || !finiteValues(pending.headAxis.data(), 9))
        return false;

    std::array<float, 9> localDirections{};
    {
        std::lock_guard<std::mutex> guard(flatHudProfilePoseMutex);
        if (pending.profileIndex >= static_cast<int>(flatHudProfilePoses.size())) return false;
        auto& profilePose = flatHudProfilePoses[pending.profileIndex];
        if (!profilePose.originValid) return false;
        profilePose.lastSeenTick = GetTickCount64();
        if (!profilePose.axisValid) {
            // Capture each authored HUD row relative to the same head basis
            // used for its origin. Only direction is cached; live row length
            // remains available for Num +/- scaling and native GUI sizing.
            for (int hudRow = 0; hudRow < 3; ++hudRow) {
                const float length = std::sqrt(
                    nativeAxis[hudRow * 3] * nativeAxis[hudRow * 3]
                    + nativeAxis[hudRow * 3 + 1] * nativeAxis[hudRow * 3 + 1]
                    + nativeAxis[hudRow * 3 + 2] * nativeAxis[hudRow * 3 + 2]);
                if (!std::isfinite(length) || length < 0.00001f) return false;
                for (int headRow = 0; headRow < 3; ++headRow) {
                    float component{};
                    for (int world = 0; world < 3; ++world)
                        component += nativeAxis[hudRow * 3 + world]
                            * pending.headAxis[headRow * 3 + world];
                    profilePose.localAxisDirections[hudRow * 3 + headRow] = component / length;
                }
            }
            profilePose.axisValid = true;
        }
        localDirections = profilePose.localAxisDirections;
    }

    const float yawRadians = pending.profileYawDegrees * 0.01745329251994329577f;
    const float yawCosine = std::cos(yawRadians);
    const float yawSine = std::sin(yawRadians);
    for (int hudRow = 0; hudRow < 3; ++hudRow) {
        const float length = std::sqrt(
            nativeAxis[hudRow * 3] * nativeAxis[hudRow * 3]
            + nativeAxis[hudRow * 3 + 1] * nativeAxis[hudRow * 3 + 1]
            + nativeAxis[hudRow * 3 + 2] * nativeAxis[hudRow * 3 + 2]);
        if (!std::isfinite(length) || length < 0.00001f) return false;
        // Rotate the complete authored surface around the HMD-local up axis.
        // Positive profile yaw turns it to the player's right. The origin is
        // intentionally unchanged, so yaw cannot disturb screen placement or
        // the calibrated stereo depth.
        const float localForward = localDirections[hudRow * 3];
        const float localLateral = localDirections[hudRow * 3 + 1];
        const float rotatedForward = localForward * yawCosine
            - localLateral * yawSine;
        const float rotatedLateral = localForward * yawSine
            + localLateral * yawCosine;
        for (int world = 0; world < 3; ++world)
            desiredAxis[hudRow * 3 + world] = length * (
                pending.headAxis[world] * rotatedForward
                + pending.headAxis[3 + world] * rotatedLateral
                + pending.headAxis[6 + world] * localDirections[hudRow * 3 + 2]);
    }
    return finiteValues(desiredAxis, 9);
}

bool sameProfile(const GuiProfile& left, const GuiProfile& right) {
    return left.callerRva == right.callerRva
        && left.width == right.width
        && left.height == right.height
        && left.scaleMilli == right.scaleMilli;
}

std::string profileText(const GuiProfile& profile) {
    std::ostringstream out;
    out << "caller=0x" << std::hex << profile.callerRva << std::dec
        << " size=" << profile.width << 'x' << profile.height
        << " nativeScale=" << std::fixed << std::setprecision(3)
        << (profile.scaleMilli / 1000.0f);
    return out.str();
}

bool isFullscreenMenuProfile(const GuiProfile& profile) {
    return kharvox::isFullscreenMenuSurface(kharvox::classifyHudGuiSurface(
        profile.callerRva, profile.width, profile.height, profile.scaleMilli));
}

bool isFieldDronePromptProfile(const GuiProfile& profile) {
    return kharvox::classifyHudGuiSurface(
        profile.callerRva, profile.width, profile.height, profile.scaleMilli)
        == kharvox::HudGuiSurfaceKind::FieldDronePrompt;
}

void loadHudIdentity() {
    std::ifstream input(identityPath());
    int role{};
    unsigned long long caller{};
    GuiProfile profile{};
    while (input >> role >> std::hex >> caller >> std::dec
        >> profile.width >> profile.height >> profile.scaleMilli) {
        if (role < 1 || role > hudRoleCount) continue;
        profile.callerRva = static_cast<uintptr_t>(caller);
        savedHudRoles[role - 1] = profile;
        savedHudRoleValid[role - 1] = true;
        log(std::string("saved ") + hudRoleNames[role - 1]
            + " identity loaded: " + profileText(profile));
    }
}

void saveHudIdentities() {
    std::ofstream output(identityPath(), std::ios::trunc);
    for (int role = 0; role < hudRoleCount; ++role) {
        if (!savedHudRoleValid[role]) continue;
        const auto& profile = savedHudRoles[role];
        output << (role + 1) << ' ' << std::hex << profile.callerRva << std::dec << ' '
            << profile.width << ' ' << profile.height << ' ' << profile.scaleMilli << '\n';
    }
}

void loadGuiProfileCalibrations() {
    std::ifstream input(profileCalibrationPath());
    bool savedOverride = input.is_open();
    if (!savedOverride) {
        input.clear();
        input.open(profileCalibrationDefaultPath());
    }
    std::string line;
    while (savedGuiProfileCalibrationCount < static_cast<int>(savedGuiProfileCalibrations.size())
        && std::getline(input, line)) {
        std::istringstream row(line);
        unsigned long long caller{};
        int excluded{};
        float yawOrLegacyExcluded{};
        SavedGuiProfileCalibration saved{};
        if (!(row >> std::hex >> caller >> std::dec
            >> saved.identity.width >> saved.identity.height >> saved.identity.scaleMilli
            >> saved.lateralOffsetTan >> saved.verticalOffsetTan
            >> saved.distanceOffsetMeters >> yawOrLegacyExcluded)) continue;
        // HUD17/HUD18 files ended after the exclusion flag. HUD19 appends yaw
        // immediately before that flag. Accept both so an existing developer
        // override can be opened and re-saved without losing calibration.
        if (row >> excluded) saved.yawDegrees = yawOrLegacyExcluded;
        else excluded = static_cast<int>(std::lround(yawOrLegacyExcluded));
        if (!std::isfinite(saved.lateralOffsetTan)
            || !std::isfinite(saved.verticalOffsetTan)
            || !std::isfinite(saved.distanceOffsetMeters)
            || !std::isfinite(saved.yawDegrees)) continue;
        saved.identity.callerRva = static_cast<uintptr_t>(caller);
        saved.lateralOffsetTan = std::clamp(saved.lateralOffsetTan, -2.0f, 2.0f);
        saved.verticalOffsetTan = std::clamp(saved.verticalOffsetTan, -2.0f, 2.0f);
        saved.distanceOffsetMeters = std::clamp(saved.distanceOffsetMeters, -5.0f, 5.0f);
        saved.yawDegrees = std::clamp(saved.yawDegrees, -180.0f, 180.0f);
        saved.excluded = excluded != 0;
        savedGuiProfileCalibrations[savedGuiProfileCalibrationCount++] = saved;
    }
    if (savedGuiProfileCalibrationCount)
        log("HUD19 per-profile calibrations loaded="
            + std::to_string(savedGuiProfileCalibrationCount)
            + " source=" + (savedOverride ? "saved debug override" : "repository default"));
}

void writeGuiProfileCalibrations() {
    std::ofstream output(profileCalibrationPath(), std::ios::trunc);
    output << std::fixed << std::setprecision(4);
    for (int index = 0; index < savedGuiProfileCalibrationCount; ++index) {
        const auto& saved = savedGuiProfileCalibrations[index];
        output << std::hex << saved.identity.callerRva << std::dec << ' '
            << saved.identity.width << ' ' << saved.identity.height << ' '
            << saved.identity.scaleMilli << ' ' << saved.lateralOffsetTan << ' '
            << saved.verticalOffsetTan << ' ' << saved.distanceOffsetMeters << ' '
            << saved.yawDegrees << ' '
            << (saved.excluded ? 1 : 0) << '\n';
    }
}

void persistGuiProfileCalibration(int profileIndex) {
    if (profileIndex < 0 || profileIndex >= guiProfileCount) return;
    const auto& profile = guiProfiles[profileIndex];
    int savedIndex{-1};
    for (int index = 0; index < savedGuiProfileCalibrationCount; ++index) {
        if (sameProfile(savedGuiProfileCalibrations[index].identity, profile)) {
            savedIndex = index;
            break;
        }
    }
    if (savedIndex < 0) {
        if (savedGuiProfileCalibrationCount
            >= static_cast<int>(savedGuiProfileCalibrations.size())) return;
        savedIndex = savedGuiProfileCalibrationCount++;
    }
    auto& saved = savedGuiProfileCalibrations[savedIndex];
    saved.identity = profile;
    saved.lateralOffsetTan = profile.lateralOffsetTan;
    saved.verticalOffsetTan = profile.verticalOffsetTan;
    saved.distanceOffsetMeters = profile.distanceOffsetMeters;
    saved.yawDegrees = profile.yawDegrees;
    saved.excluded = profile.userExcluded;
    writeGuiProfileCalibrations();
}

int registerGuiProfile(const GuiProfile& profile) {
    const auto now = GetTickCount64();
    for (int index = 0; index < guiProfileCount; ++index) {
        if (sameProfile(guiProfiles[index], profile)) {
            guiProfiles[index].lastSeenTick = now;
            return index;
        }
    }
    if (guiProfileCount >= static_cast<int>(guiProfiles.size())) return -1;
    const int index = guiProfileCount++;
    guiProfiles[index] = profile;
    guiProfiles[index].lastSeenTick = now;
    // Both callers were physically identified as wall terminals in live tests.
    guiProfiles[index].builtInExcluded = profile.callerRva == 0x6B6F75
        || profile.callerRva == 0x907FA0;
    for (int savedIndex = 0; savedIndex < savedGuiProfileCalibrationCount; ++savedIndex) {
        const auto& saved = savedGuiProfileCalibrations[savedIndex];
        if (!sameProfile(saved.identity, profile)) continue;
        guiProfiles[index].lateralOffsetTan = saved.lateralOffsetTan;
        guiProfiles[index].verticalOffsetTan = saved.verticalOffsetTan;
        guiProfiles[index].distanceOffsetMeters = saved.distanceOffsetMeters;
        guiProfiles[index].yawDegrees = saved.yawDegrees;
        guiProfiles[index].userExcluded = saved.excluded;
        break;
    }
    log("discovered GUI profile #" + std::to_string(index + 1) + ": "
        + profileText(guiProfiles[index])
        + (guiProfiles[index].builtInExcluded ? " [known wall terminal; excluded]" : "")
        + (isFieldDronePromptProfile(guiProfiles[index])
            ? " [Field Drone proximity prompt; excluded]" : "")
        + (guiProfiles[index].userExcluded ? " [HUD19 user-excluded]" : ""));
    for (int role = 0; role < hudRoleCount; ++role)
        if (savedHudRoleValid[role] && sameProfile(savedHudRoles[role], guiProfiles[index]))
            log(std::string("saved ") + hudRoleNames[role] + " profile matched in current session");
    return index;
}

void selectNextGuiProfile(int direction) {
    if (!guiProfileCount) {
        log("Num /: no gameplay GUI profiles discovered yet");
        return;
    }
    const auto now = GetTickCount64();
    constexpr unsigned long long visibleProfileWindowMilliseconds = 1500;
    if (selectedGuiProfile < 0) selectedGuiProfile = direction > 0 ? -1 : 0;
    for (int attempt = 0; attempt < guiProfileCount; ++attempt) {
        selectedGuiProfile = (selectedGuiProfile + direction + guiProfileCount)
            % guiProfileCount;
        const auto& profile = guiProfiles[selectedGuiProfile];
        const bool recentlyVisible = profile.lastSeenTick && now >= profile.lastSeenTick
            && now - profile.lastSeenTick <= visibleProfileWindowMilliseconds;
        if (profile.builtInExcluded || isFullscreenMenuProfile(profile)
            || isFieldDronePromptProfile(profile)
            || !recentlyVisible) continue;
        selectedGuiProfileCenterUntilTick = now + 2000;
        std::ostringstream out;
        out << std::fixed << std::setprecision(4)
            << "Num /: HUD19 SELECTED profile #" << (selectedGuiProfile + 1)
            << " -> centered for 2 seconds: " << profileText(profile)
            << " lateralTan=" << profile.lateralOffsetTan
            << " verticalTan=" << profile.verticalOffsetTan
            << " depthOffset=" << profile.distanceOffsetMeters << "m"
            << " yaw=" << profile.yawDegrees << "deg"
            << (profile.userExcluded ? " [EXCLUDED after preview]" : "");
        log(out.str());
        return;
    }
    selectedGuiProfile = -1;
    selectedGuiProfileCenterUntilTick = 0;
    log("Num /: no recently visible gameplay HUD profile available");
}

void pollIdentityKeys() {
    if(offhandCalibrationActive.load())return;
    static bool leftWasDown{}, rightWasDown{}, upWasDown{}, downWasDown{};
    static bool fartherWasDown{}, closerWasDown{}, yawLeftWasDown{}, yawRightWasDown{};
    static bool resetWasDown{};
    static unsigned long long nextHorizontalRepeatTick{};
    static unsigned long long nextVerticalRepeatTick{};
    static unsigned long long nextDepthRepeatTick{};
    static unsigned long long nextYawRepeatTick{};
    static int repeatingHorizontalDirection{};
    static int repeatingVerticalDirection{};
    static int repeatingDepthDirection{};
    static int repeatingYawDirection{};

    if (!hudDebuggingEnabled) return;

    const bool controlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shiftDown = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    const bool divideDown = !controlDown && !altDown
        && (GetAsyncKeyState(VK_DIVIDE) & 0x8000) != 0;
    const bool subtractDown = controlDown && !altDown
        && (GetAsyncKeyState(VK_SUBTRACT) & 0x8000) != 0;
    const bool leftDown = controlDown && !altDown
        && (GetAsyncKeyState(VK_NUMPAD4) & 0x8000) != 0;
    const bool rightDown = controlDown && !altDown
        && (GetAsyncKeyState(VK_NUMPAD6) & 0x8000) != 0;
    const bool upDown = controlDown && !altDown
        && (GetAsyncKeyState(VK_NUMPAD8) & 0x8000) != 0;
    const bool downDown = controlDown && !altDown
        && (GetAsyncKeyState(VK_NUMPAD2) & 0x8000) != 0;
    const bool fartherDown = controlDown && !altDown
        && (GetAsyncKeyState(VK_NUMPAD7) & 0x8000) != 0;
    const bool closerDown = controlDown && !altDown
        && (GetAsyncKeyState(VK_NUMPAD9) & 0x8000) != 0;
    const bool yawLeftDown = controlDown && !altDown
        && (GetAsyncKeyState(VK_NUMPAD1) & 0x8000) != 0;
    const bool yawRightDown = controlDown && !altDown
        && (GetAsyncKeyState(VK_NUMPAD3) & 0x8000) != 0;
    const bool resetDown = controlDown && !altDown
        && (GetAsyncKeyState(VK_NUMPAD5) & 0x8000) != 0;
    const auto now = GetTickCount64();

    if (divideDown && !divideWasDown) {
        selectNextGuiProfile(shiftDown ? -1 : 1);
    }
    if (subtractDown && !excludeWasDown && selectedGuiProfile >= 0) {
        auto& profile = guiProfiles[selectedGuiProfile];
        profile.userExcluded = !profile.userExcluded;
        selectedGuiProfileCenterUntilTick = now + 2000;
        persistGuiProfileCalibration(selectedGuiProfile);
        log(std::string("Ctrl+Num -: HUD19 selected profile ")
            + (profile.userExcluded ? "EXCLUDED after 2-second preview: " : "INCLUDED: ")
            + profileText(profile));
    }

    auto applyAdjustment = [&](int horizontalDirection, int verticalDirection,
                               int depthDirection, int yawDirection, bool initialPress) {
        if (selectedGuiProfile < 0) {
            if (initialPress) log("HUD19 adjustment ignored: select a visible profile with Num /");
            return;
        }
        auto& profile = guiProfiles[selectedGuiProfile];
        const float angularStep = shiftDown ? 0.002f : 0.01f;
        const float depthStep = shiftDown ? 0.01f : 0.05f;
        const float yawStepDegrees = shiftDown ? 0.5f : 2.5f;
        profile.lateralOffsetTan = std::clamp(
            profile.lateralOffsetTan + angularStep * horizontalDirection, -2.0f, 2.0f);
        profile.verticalOffsetTan = std::clamp(
            profile.verticalOffsetTan + angularStep * verticalDirection, -2.0f, 2.0f);
        profile.distanceOffsetMeters = std::clamp(
            profile.distanceOffsetMeters + depthStep * depthDirection, -5.0f, 5.0f);
        profile.yawDegrees = std::clamp(
            profile.yawDegrees + yawStepDegrees * yawDirection, -180.0f, 180.0f);
        persistGuiProfileCalibration(selectedGuiProfile);
        std::ostringstream out;
        out << std::fixed << std::setprecision(4)
            << "HUD19 profile #" << (selectedGuiProfile + 1)
            << " AUTO-SAVED lateralTan=" << profile.lateralOffsetTan
            << " verticalTan=" << profile.verticalOffsetTan
            << " depthOffset=" << profile.distanceOffsetMeters << "m"
            << " yaw=" << profile.yawDegrees << "deg"
            << (shiftDown ? " (fine)" : "");
        log(out.str());
    };

    int horizontalDirection{};
    if (rightDown && !leftDown) horizontalDirection = 1;
    else if (leftDown && !rightDown) horizontalDirection = -1;
    const bool initialHorizontalPress = (horizontalDirection > 0 && !rightWasDown)
        || (horizontalDirection < 0 && !leftWasDown);
    const bool horizontalRepeat = horizontalDirection
        && horizontalDirection == repeatingHorizontalDirection
        && nextHorizontalRepeatTick && now >= nextHorizontalRepeatTick;
    if (horizontalDirection && (initialHorizontalPress || horizontalRepeat)) {
        applyAdjustment(horizontalDirection, 0, 0, 0, initialHorizontalPress);
        nextHorizontalRepeatTick = now + (initialHorizontalPress ? 350 : 90);
        repeatingHorizontalDirection = horizontalDirection;
    } else if (!horizontalDirection) {
        repeatingHorizontalDirection = 0;
        nextHorizontalRepeatTick = 0;
    }

    int verticalDirection{};
    if (upDown && !downDown) verticalDirection = 1;
    else if (downDown && !upDown) verticalDirection = -1;
    const bool initialVerticalPress = (verticalDirection > 0 && !upWasDown)
        || (verticalDirection < 0 && !downWasDown);
    const bool verticalRepeat = verticalDirection
        && verticalDirection == repeatingVerticalDirection
        && nextVerticalRepeatTick && now >= nextVerticalRepeatTick;
    if (verticalDirection && (initialVerticalPress || verticalRepeat)) {
        applyAdjustment(0, verticalDirection, 0, 0, initialVerticalPress);
        nextVerticalRepeatTick = now + (initialVerticalPress ? 350 : 90);
        repeatingVerticalDirection = verticalDirection;
    } else if (!verticalDirection) {
        repeatingVerticalDirection = 0;
        nextVerticalRepeatTick = 0;
    }

    int depthDirection{};
    if (fartherDown && !closerDown) depthDirection = 1;
    else if (closerDown && !fartherDown) depthDirection = -1;
    const bool initialDepthPress = (depthDirection > 0 && !fartherWasDown)
        || (depthDirection < 0 && !closerWasDown);
    const bool depthRepeat = depthDirection && depthDirection == repeatingDepthDirection
        && nextDepthRepeatTick && now >= nextDepthRepeatTick;
    if (depthDirection && (initialDepthPress || depthRepeat)) {
        applyAdjustment(0, 0, depthDirection, 0, initialDepthPress);
        nextDepthRepeatTick = now + (initialDepthPress ? 350 : 90);
        repeatingDepthDirection = depthDirection;
    } else if (!depthDirection) {
        repeatingDepthDirection = 0;
        nextDepthRepeatTick = 0;
    }

    int yawDirection{};
    if (yawRightDown && !yawLeftDown) yawDirection = 1;
    else if (yawLeftDown && !yawRightDown) yawDirection = -1;
    const bool initialYawPress = (yawDirection > 0 && !yawRightWasDown)
        || (yawDirection < 0 && !yawLeftWasDown);
    const bool yawRepeat = yawDirection && yawDirection == repeatingYawDirection
        && nextYawRepeatTick && now >= nextYawRepeatTick;
    if (yawDirection && (initialYawPress || yawRepeat)) {
        applyAdjustment(0, 0, 0, yawDirection, initialYawPress);
        nextYawRepeatTick = now + (initialYawPress ? 350 : 90);
        repeatingYawDirection = yawDirection;
    } else if (!yawDirection) {
        repeatingYawDirection = 0;
        nextYawRepeatTick = 0;
    }

    if (resetDown && !resetWasDown && selectedGuiProfile >= 0) {
        auto& profile = guiProfiles[selectedGuiProfile];
        profile.lateralOffsetTan = 0.0f;
        profile.verticalOffsetTan = 0.0f;
        profile.distanceOffsetMeters = 0.0f;
        profile.yawDegrees = 0.0f;
        persistGuiProfileCalibration(selectedGuiProfile);
        selectedGuiProfileCenterUntilTick = now + 2000;
        log("Ctrl+Num 5: HUD19 selected profile offsets RESET and auto-saved: "
            + profileText(profile));
    }

    divideWasDown = divideDown;
    excludeWasDown = subtractDown;
    leftWasDown = leftDown;
    rightWasDown = rightDown;
    upWasDown = upDown;
    downWasDown = downDown;
    fartherWasDown = fartherDown;
    closerWasDown = closerDown;
    yawLeftWasDown = yawLeftDown;
    yawRightWasDown = yawRightDown;
    resetWasDown = resetDown;
}

void loadCalibration() {
    for (int role = 0; role < hudRoleCount; ++role) {
        for (int component = 0; component < 3; ++component)
            roleCalibrations[role].offset[component].store(
                defaultRoleCalibration[role][component], std::memory_order_relaxed);
        roleCalibrations[role].scale.store(
            defaultRoleCalibration[role][3], std::memory_order_relaxed);
    }
    std::ifstream input(calibrationPath());
    int role{};
    float forward{}, lateral{}, up{}, scale{};
    while (input >> role >> forward >> lateral >> up >> scale) {
        if (role < 1 || role > hudRoleCount) continue;
        auto& values = roleCalibrations[role - 1];
        values.offset[0].store(std::clamp(forward, -200.0f, 200.0f));
        values.offset[1].store(std::clamp(lateral, -200.0f, 200.0f));
        values.offset[2].store(std::clamp(up, -200.0f, 200.0f));
        values.scale.store(std::clamp(scale, 0.10f, 4.0f));
    }
    log(std::string("per-role calibration loaded; active target=")
        + hudRoleNames[selectedCalibrationRole]);
}

void saveCalibration() {
    std::ofstream output(calibrationPath(), std::ios::trunc);
    output << std::fixed << std::setprecision(4);
    for (int role = 0; role < hudRoleCount; ++role) {
        output << (role + 1);
        for (int component = 0; component < 3; ++component)
            output << ' ' << roleCalibrations[role].offset[component].load();
        output << ' ' << roleCalibrations[role].scale.load() << '\n';
    }
    log("all HUD role calibrations SAVED");
}

void pollCalibrationKeys() {
    if(offhandCalibrationActive.load())return;
    static std::array<bool, 10> wasDown{};
    const std::array<int, 10> keys{
        VK_NUMPAD0, VK_NUMPAD4, VK_NUMPAD6, VK_NUMPAD8, VK_NUMPAD2,
        VK_NUMPAD7, VK_NUMPAD9, VK_ADD, VK_MULTIPLY, VK_NUMPAD5
    };
    std::array<bool, 10> down{};
    for (int index = 0; index < static_cast<int>(keys.size()); ++index)
        down[index] = (GetAsyncKeyState(keys[index]) & 0x8000) != 0;
    const bool decimalDown = (GetAsyncKeyState(VK_DECIMAL) & 0x8000) != 0;
    static bool decimalWasDown{};
    const bool modifierDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0
        || (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0
        || (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

    if (!modifierDown && down[0] && !wasDown[0]) {
        selectedCalibrationRole = (selectedCalibrationRole + 1) % hudRoleCount;
        log(std::string("Num 0: calibration target=")
            + hudRoleNames[selectedCalibrationRole]);
    }
    auto& selected = roleCalibrations[selectedCalibrationRole];
    float forward = selected.offset[0].load(std::memory_order_relaxed);
    float lateral = selected.offset[1].load(std::memory_order_relaxed);
    float up = selected.offset[2].load(std::memory_order_relaxed);
    float scale = selected.scale.load(std::memory_order_relaxed);
    bool changed{};
    if (!modifierDown && down[1] && !wasDown[1]) { lateral -= 1.0f; changed = true; }
    if (!modifierDown && down[2] && !wasDown[2]) { lateral += 1.0f; changed = true; }
    if (!modifierDown && down[3] && !wasDown[3]) { up += 1.0f; changed = true; }
    if (!modifierDown && down[4] && !wasDown[4]) { up -= 1.0f; changed = true; }
    if (!modifierDown && down[5] && !wasDown[5]) { forward += 1.0f; changed = true; }
    if (!modifierDown && down[6] && !wasDown[6]) { forward -= 1.0f; changed = true; }
    if (!modifierDown && down[7] && !wasDown[7]) { scale = std::min(scale + 0.05f, 4.0f); changed = true; }
    if (!modifierDown && down[8] && !wasDown[8]) { scale = std::max(scale - 0.05f, 0.10f); changed = true; }
    if (!modifierDown && down[9] && !wasDown[9]) {
        forward = defaultRoleCalibration[selectedCalibrationRole][0];
        lateral = defaultRoleCalibration[selectedCalibrationRole][1];
        up = defaultRoleCalibration[selectedCalibrationRole][2];
        scale = defaultRoleCalibration[selectedCalibrationRole][3];
        changed = true;
    }
    if (changed) {
        selected.offset[0].store(std::clamp(forward, -200.0f, 200.0f));
        selected.offset[1].store(std::clamp(lateral, -200.0f, 200.0f));
        selected.offset[2].store(std::clamp(up, -200.0f, 200.0f));
        selected.scale.store(scale);
        std::ostringstream out;
        out << std::fixed << std::setprecision(2)
            << hudRoleNames[selectedCalibrationRole] << " preview forward=" << forward
            << " lateral=" << lateral << " up=" << up << " scale=" << scale;
        log(out.str());
    }
    if (!modifierDown && decimalDown && !decimalWasDown) saveCalibration();
    wasDown = down;
    decimalWasDown = decimalDown;
}

bool gameplayHudActive() {
    return KharvoxCameraGameplayActive()
        && !KharvoxCameraCutsceneActive()
        && !pauseMenuActive.load(std::memory_order_acquire)
        && !deathMenuActive.load(std::memory_order_acquire)
        && !KharvoxHudFullscreenMenuActive();
}

}

bool KharvoxHudPrepareOriginTransform(void* intermediateEntity, float* nativeOrigin) {
    suppressedOffhandCanvas=nullptr;
    if (!intermediateEntity || !nativeOrigin || !KharvoxCameraWorldActive()) return false;

    const auto current = reinterpret_cast<uintptr_t>(nativeOrigin) - 0x60;
    // Hud_Cinematic can be submitted by the shared HUD renderer (BDCF54),
    // bypassing its derived Frame vtable slot. Observe this exact draw owner
    // before excluding native movie layout from our HUD transforms.
    observeHudMovie(reinterpret_cast<void*>(current),"HUD-draw");
    if(KharvoxHudMovieActive())return false;
    if (!writableRange(reinterpret_cast<void*>(current + 0x60), 0x34)
        || !readableRange(reinterpret_cast<void*>(current + 0x20), 0x28)) return false;
    const auto capturedCrosshair = static_cast<uintptr_t>(InterlockedCompareExchange64(
        &capturedCrosshairContext, 0, 0));
    const bool crosshair = capturedCrosshair && current == capturedCrosshair;
    const bool ledgeTransitionActive = KharvoxCameraLedgeTransitionActive();
    const bool offscreen = kharvox::shouldPlaceHudOffscreen(
        crosshair, ledgeTransitionActive);
    const bool cinematicSurface=KharvoxCameraCutsceneActive() && !crosshair && !ledgeTransitionActive;
    const bool observeCinematicMenu=kharvox::shouldObserveCinematicMenuSurface(
        cinematicSurface,crosshair,ledgeTransitionActive,
        playerUpgradePickupSessionActive.load(std::memory_order_acquire)
            ||suitUpgradeNativeSessionActive.load(std::memory_order_acquire)
            ||runeTrialNativeSessionActive.load(std::memory_order_acquire));
    // Menu detection must precede the exclusion from gameplay HUD transforms.
    // Ordinary cinematics retain the cheap early exit.
    // Classify owned Life/Ammo before cinematic early-out to suppress native fallback.
    uintptr_t diagnosticCallerRva{};
    int diagnosticWidth{};
    int diagnosticHeight{};
    int diagnosticScaleMilli{};
    int flatProfileIndex{-1};
    HudProfileAdjustment profileAdjustment{};

    if (!crosshair) {
        void* frames[8]{};
        const auto frameCount = CaptureStackBackTrace(0, 8, frames, nullptr);
        const auto image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        uintptr_t callerRva{};
        for (USHORT index = 0; index < frameCount; ++index) {
            const auto address = reinterpret_cast<uintptr_t>(frames[index]);
            if (address >= image && address - image < 0x8000000) {
                const auto rva = address - image;
                if (rva >= 0xF91E00 && rva <= 0xF92450) continue;
                callerRva = rva;
                break;
            }
        }
        if (!callerRva) return false;

        const auto screen = *reinterpret_cast<const uintptr_t*>(current + 0x20);
        if (!readableRange(reinterpret_cast<const void*>(screen + 0x1BC), sizeof(float) * 2)) return false;
        const float width = *reinterpret_cast<const float*>(screen + 0x1BC);
        const float height = *reinterpret_cast<const float*>(screen + 0x1C0);
        const float nativeScale = *reinterpret_cast<const float*>(current + 0x90);
        if (!std::isfinite(width) || !std::isfinite(height) || !std::isfinite(nativeScale)) return false;

        GuiProfile profile{
            callerRva,
            static_cast<int>(std::lround(width)),
            static_cast<int>(std::lround(height)),
            static_cast<int>(std::lround(nativeScale * 1000.0f))
        };
        diagnosticCallerRva = profile.callerRva;
        diagnosticWidth = profile.width;
        diagnosticHeight = profile.height;
        diagnosticScaleMilli = profile.scaleMilli;
        bool managedOffhand=false;
        if(offhandCanvasHookReady&&kharvox::offhandHudSurface(profile.callerRva,profile.width,profile.height,profile.scaleMilli)>=0){
            std::lock_guard<std::mutex> guard(offhandHudMutex);
            managedOffhand=offhandHudConfig.enabled||offhandCalibrationActive.load();
        }
        float handOrigin[3]{},handAxis[9]{};
        const bool tracked=!managedOffhand||getOffhandHudFrame(handOrigin,handAxis);
        if(kharvox::suppressOffhandHudFallback(managedOffhand,gameplayHudActive(),cinematicSurface,
            ledgeTransitionActive,KharvoxCameraSyncAttackActive(),tracked)){
            suppressedOffhandCanvas=*reinterpret_cast<void**>(current+0x30);
            static std::atomic<unsigned> count{};const auto n=++count;
            if(n<=4||n%240==0)log("[OFFHAND-HUD] native fallback suppressed during sequence/tracking gap count="+std::to_string(n));
            return false;
        }
        if(cinematicSurface&&!observeCinematicMenu)return false;
        const auto surfaceKind = kharvox::classifyHudGuiSurface(
            profile.callerRva, profile.width, profile.height, profile.scaleMilli);
        const bool runeTrialChallengeMenu = surfaceKind
            == kharvox::HudGuiSurfaceKind::RuneTrialChallengeMenu;
        if (runeTrialChallengeMenu
            && runeTrialChallengeLoadPending.load(std::memory_order_acquire))
            beginRuneChallengeScreenSession(
                "exact 1024x1024 EndOfChallenge render fallback");
        const bool fullscreen = kharvox::isFullscreenMenuSurface(surfaceKind);
        const bool fieldDronePrompt = surfaceKind
            == kharvox::HudGuiSurfaceKind::FieldDronePrompt;
        if (fullscreen) {
            const auto now = GetTickCount64();
            const auto previous = fullscreenMenuLastSeenTick.exchange(
                now, std::memory_order_acq_rel);
            if (!previous || now - previous > fullscreenMenuHoldMilliseconds)
                log(std::string(kharvox::fullscreenGuiKindName(surfaceKind))
                    + " full-screen in-game GUI active: " + profileText(profile));
        }
        if(observeCinematicMenu){
            // Bounded render-boundary evidence even if native Show/manager and
            // award-notification callbacks are bypassed in this campaign.
            static std::mutex observationMutex;
            static std::array<GuiProfile,64> observed{};
            static size_t observedCount{};
            static unsigned long long observedSession{};
            const auto session=playerUpgradePickupSessionStartTick.load(std::memory_order_acquire);
            std::lock_guard<std::mutex> lock(observationMutex);
            if(session!=observedSession){observedSession=session;observedCount=0;}
            bool known=false;
            for(size_t i=0;i<observedCount;++i)
                known=known||(observed[i].callerRva==profile.callerRva
                    &&observed[i].width==profile.width&&observed[i].height==profile.height);
            if(!known&&observedCount<observed.size()){
                observed[observedCount++]=profile;
                log("[MENU-RENDER] cinematic native menu surface: "+profileText(profile)
                    +" ageMs="+std::to_string(session?GetTickCount64()-session:0));
            }
            return false; // Observe menu ownership; never transform cinematic GUI.
        }
        std::lock_guard<std::mutex> guard(guiProfileMutex);
        const int profileIndex = registerGuiProfile(profile);
        flatProfileIndex = profileIndex;
        pollIdentityKeys();
        bool excludedWorldGui = callerRva == 0x6B6F75 || callerRva == 0x907FA0;
        if (profileIndex >= 0) {
            const auto& registered = guiProfiles[profileIndex];
            profileAdjustment.lateralOffsetTan = registered.lateralOffsetTan;
            profileAdjustment.verticalOffsetTan = registered.verticalOffsetTan;
            profileAdjustment.distanceOffsetMeters = registered.distanceOffsetMeters;
            profileAdjustment.yawDegrees = registered.yawDegrees;
            profileAdjustment.centerPreview = profileIndex == selectedGuiProfile
                && GetTickCount64() < selectedGuiProfileCenterUntilTick;
            excludedWorldGui = excludedWorldGui || registered.builtInExcluded
                || (registered.userExcluded && !profileAdjustment.centerPreview);
        }
        if (fullscreen || fieldDronePrompt || excludedWorldGui) return false;
    }

    if (!kharvox::shouldApplyHudTransform(
            gameplayHudActive(), crosshair, ledgeTransitionActive))
        return false;
    const auto expectedFinalEntity = *reinterpret_cast<void**>(current + 0x30);
    const auto contextIntermediateEntity = *reinterpret_cast<void**>(current + 0x40);
    if (!expectedFinalEntity || contextIntermediateEntity != intermediateEntity
        || !writableRange(static_cast<unsigned char*>(intermediateEntity) + 0x78, 0x5C)) return false;

    const float nativeScale = *reinterpret_cast<const float*>(current + 0x90);
    const auto calibration = calibrationForCurrentRenderFrame();
    float desiredOrigin[3]{};
    float headAxis[9]{};
    float nativeDepth{};
    if (!std::isfinite(nativeScale)
        || !buildHeadlockedOriginTransform(
            nativeOrigin, crosshair, offscreen,
            expectedFinalEntity, flatProfileIndex,
            desiredOrigin, headAxis, &nativeDepth, calibration,
            profileAdjustment)) return false;

    bool offhand=false;float offhandAxis[9]{};float offhandScale=1;
    const int handSurface=kharvox::offhandHudSurface(diagnosticCallerRva,diagnosticWidth,diagnosticHeight,diagnosticScaleMilli);
    if(offhandCanvasHookReady&&!crosshair&&!offscreen&&handSurface>=0){
        kharvox::OffhandHudConfig config;{std::lock_guard<std::mutex> lock(offhandHudMutex);config=offhandHudConfig;}
        float grip[3]{},handAxis[9]{};
        if((config.enabled||offhandCalibrationActive.load())&&getOffhandHudFrame(grip,handAxis)){
            const auto& values=config.modes[handSurface*2+(offhandHudLeftMode()?1:0)];
            kharvox::offhandHudBasis(handAxis,values,offhandAxis);
            kharvox::offhandHudOrigin(grip,handAxis,offhandAxis,values,handSurface,hudWorldUnitsPerMeter,desiredOrigin);
            offhand=true;offhandScale=values.scale;
            if(offhandCalibrationActive.load()&&offhandSelectedSurface.load()==handSurface&&GetTickCount64()<offhandSelectionUntil.load())offhandScale*=1.15f;
            profileAdjustment.yawDegrees=0;
            static std::atomic<unsigned> seen{};const auto bit=1u<<handSurface;
            if(!(seen.fetch_or(bit)&bit))log("[OFFHAND-HUD] surface="+std::to_string(handSurface)+" hand="+(offhandHudLeftMode()?"right":"left"));
        }
    }

    if (pendingHudDepth == maximumPendingHudDepth) {
        log("transient HUD stack overflow; restoring all contexts before continuing");
        restoreAllPendingHudSubmissions();
    }

    PendingHudSubmission pending{};
    pending.active = true;
    pending.crosshair = crosshair;
    pending.offhand=offhand;
    pending.offhandWidth=.25f*hudWorldUnitsPerMeter*(offhandScale/.40f);
    std::memcpy(pending.offhandAxis.data(),offhandAxis,sizeof(offhandAxis));
    pending.offscreen = offscreen;
    pending.context = current;
    pending.expectedFinalEntity = expectedFinalEntity;
    pending.profileIndex = flatProfileIndex;
    pending.profileYawDegrees = profileAdjustment.yawDegrees;
    std::memcpy(pending.contextOrigin.data(), reinterpret_cast<void*>(current + 0x60), sizeof(float) * 3);
    pending.contextScale = nativeScale;
    std::memcpy(pending.desiredOrigin.data(), desiredOrigin, sizeof(desiredOrigin));
    std::memcpy(pending.headAxis.data(), headAxis, sizeof(headAxis));

    std::memcpy(reinterpret_cast<void*>(current + 0x60), desiredOrigin, sizeof(desiredOrigin));
    const float desiredScale = offscreen ? nativeScale : offhand ? nativeScale*offhandScale : nativeScale * std::clamp(
        calibration.elementScale / hudReferenceUserScale
            * calibration.headsetFitScale,
        minimumHudLayoutScale, maximumHudLayoutScale);
    std::memcpy(reinterpret_cast<void*>(current + 0x90), &desiredScale, sizeof(desiredScale));
    pendingHudStack[pendingHudDepth++] = pending;

    LARGE_INTEGER matchedAt{};
    QueryPerformanceCounter(&matchedAt);
    diagnosticLastCallerRva.store(diagnosticCallerRva, std::memory_order_relaxed);
    diagnosticLastWidth.store(diagnosticWidth, std::memory_order_relaxed);
    diagnosticLastHeight.store(diagnosticHeight, std::memory_order_relaxed);
    diagnosticLastScaleMilli.store(diagnosticScaleMilli, std::memory_order_relaxed);
    diagnosticLastThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
    diagnosticLastWasCrosshair.store(crosshair, std::memory_order_relaxed);
    diagnosticLastMatchQpc.store(
        static_cast<unsigned long long>(matchedAt.QuadPart), std::memory_order_release);
    const auto diagnosticSerial = diagnosticMatchedSurfaces.fetch_add(
        1, std::memory_order_acq_rel) + 1;
    auto& diagnosticEvent = diagnosticEvents[diagnosticSerial % diagnosticEventCapacity];
    diagnosticEvent.qpc.store(
        static_cast<unsigned long long>(matchedAt.QuadPart), std::memory_order_relaxed);
    diagnosticEvent.threadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
    diagnosticEvent.callerRva.store(diagnosticCallerRva, std::memory_order_relaxed);
    diagnosticEvent.width.store(diagnosticWidth, std::memory_order_relaxed);
    diagnosticEvent.height.store(diagnosticHeight, std::memory_order_relaxed);
    diagnosticEvent.scaleMilli.store(diagnosticScaleMilli, std::memory_order_relaxed);
    diagnosticEvent.crosshair.store(crosshair, std::memory_order_relaxed);
    diagnosticEvent.serial.store(diagnosticSerial, std::memory_order_release);
    if (crosshair)
        diagnosticCrosshairSurfaces.fetch_add(1, std::memory_order_release);

    if (!crosshair) {
        static std::atomic<bool> genericHudLogged{};
        if (!genericHudLogged.exchange(true, std::memory_order_relaxed)) {
            std::ostringstream out;
            out << std::fixed << std::setprecision(3)
                << "generic DOOMHUDPush depth match=" << nativeDepth
                << " in native HUD band [" << nativeHudNearDepth << ',' << nativeHudFarDepth
                << "]; [HUD11-FLAT] stable per-profile camera-local origin/axis path active";
            log(out.str());
        }
    }
    return true;
}

bool KharvoxHudCompleteFinalEntity(
    const void* entity, const float* nativeAxis,
    float desiredOrigin[3], float desiredAxis[9]) {
    if(entity&&entity==suppressedOffhandCanvas)return false;
    if (!pendingHudDepth) return false;
    size_t match = pendingHudDepth;
    if (entity) {
        for (size_t index = pendingHudDepth; index-- > 0;) {
            if (pendingHudStack[index].expectedFinalEntity == entity) {
                match = index;
                break;
            }
        }
    }
    if (match == pendingHudDepth) {
        static std::atomic<unsigned int> mismatchLogs{};
        if (mismatchLogs.fetch_add(1, std::memory_order_relaxed) < 8)
            log("final HUD entity did not match the transient stack; all pending contexts restored");
        restoreAllPendingHudSubmissions();
        return false;
    }

    const auto matchedPending = pendingHudStack[match];
    const bool transformed = desiredOrigin && desiredAxis
        && buildStableHeadlockedAxis(matchedPending, nativeAxis, desiredAxis);
    offhandCanvasSubmission={};
    if(transformed&&matchedPending.offhand){
        float rotated[9]{};
        for(int row=0;row<3;++row)for(int local=0;local<3;++local){float value=0;
            for(int world=0;world<3;++world)value+=desiredAxis[row*3+world]*matchedPending.headAxis[local*3+world];
            for(int world=0;world<3;++world)rotated[row*3+world]+=value*matchedPending.offhandAxis[local*3+world];}
        std::memcpy(desiredAxis,rotated,sizeof(rotated));
        offhandCanvasSubmission.entity=entity;
        offhandCanvasSubmission.width=matchedPending.offhandWidth;
        std::memcpy(offhandCanvasSubmission.center,matchedPending.desiredOrigin.data(),sizeof(offhandCanvasSubmission.center));
        std::memcpy(offhandCanvasSubmission.axis,desiredAxis,sizeof(offhandCanvasSubmission.axis));
    }
    if (transformed)
        std::memcpy(desiredOrigin, matchedPending.desiredOrigin.data(), sizeof(float) * 3);

    const bool perfectlyNested = match + 1 == pendingHudDepth;
    while (pendingHudDepth > match) {
        const auto pending = pendingHudStack[--pendingHudDepth];
        pendingHudStack[pendingHudDepth] = {};
        restorePendingHudSubmission(pending);
    }
    if (!perfectlyNested) {
        static std::atomic<unsigned int> unwindLogs{};
        if (unwindLogs.fetch_add(1, std::memory_order_relaxed) < 8)
            log("out-of-order HUD final entity matched; nested contexts safely unwound");
    }
    diagnosticCompletedSurfaces.fetch_add(1, std::memory_order_release);
    return transformed;
}

bool KharvoxHudInstallHook() {
    if (installed.load(std::memory_order_acquire)) return true;
    char hudDebugging[16]{};
    const auto hudDebuggingLength = GetEnvironmentVariableA(
        "KHARVOX_HUD_DEBUG", hudDebugging, sizeof(hudDebugging));
    hudDebuggingEnabled = hudDebuggingLength > 0
        && std::strcmp(hudDebugging, "0") != 0
        && _stricmp(hudDebugging, "false") != 0
        && _stricmp(hudDebugging, "off") != 0;
    loadHeadlockedHudSettings();
    loadHudIdentity();
    loadGuiProfileCalibrations();
    offhandCanvasHookReady=installOffhandCanvasHook();
    installHudOriginHook();
    installCrosshairCaptureHook();
    installPauseMenuLifecycleHooks();
    installCampaignDeathMenuLifecycleHooks();
    installEndOfLevelMenuLifecycleHooks();
    installPlayerUpgradeMenuLifecycleHooks();
    installRuneSelectMenuLifecycleHooks();
    installRuneChallengeMenuLifecycleHooks();
    if (installTutorialRenderHook()) {
        auto image=reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
        installMenuVtableHook(image,0x225CA20,3,0xC75A00,
            reinterpret_cast<const void*>(&voiceCommUpdateHook),originalVoiceCommUpdate,
            "Hud_VoiceCommunication / bound Flash element");
        installMenuVtableHook(image,0x224E830,7,0xC722F0,
            reinterpret_cast<const void*>(&objectiveUpdateHook),originalObjectiveUpdate,
            "Hud_Objective Update / bound Flash element");
        installMenuVtableHook(image,0x225C828,7,0xC6EAC0,
            reinterpret_cast<const void*>(&bossVitalsUpdateHook),originalBossVitalsUpdate,
            "Hud_BossDemonVitals Update / boss health bar");
        installMenuVtableHook(image,0x224B820,7,0xC17880,
            reinterpret_cast<const void*>(&runeCounterUpdateHook),originalRuneCounterUpdate,
            "Gui_EndOfChallenge Update / gameplay counter elements");
    }
    installTutorialLifecycleHooks();
    installLocalizedBindingHook();
    installHudMovieHook();
    installPlayerUpgradeManagerFrameHook();
    installPlayerUpgradePickupNotificationHook();
    installEliteGuardActivationHook();
    installFieldDroneLifecycleHooks();
    installVegaTrainingLifecycleHooks();
    installSuitUpgradeLifecycleHooks();
    installed.store(true, std::memory_order_release);
    log(std::string("[HUD30-ELITEGUARD-SPLIT] exact idInteractable_EliteGuard activation arms an immersive pickup cinematic; only the actual PlayerUpgrade manager/screen (or a non-gameplay fallback) starts centered compact Quad; FOV inference removed; native notification fallback retained; baked six-profile calibration and per-profile yaw active; interactive HUD debugging ")
        + (hudDebuggingEnabled ? "ENABLED" : "disabled"));
    return true;
}

void KharvoxHudPollQuadControls() {
    reloadOffhandHud();
    pollOffhandHudHotkeys();
    if(offhandCalibrationActive.load())return;
    static bool addWasDown{};
    static bool subtractWasDown{};
    static bool fartherWasDown{};
    static bool closerWasDown{};
    static bool leftWasDown{};
    static bool rightWasDown{};
    static bool multiplyWasDown{};
    static bool resetWasDown{};
    static unsigned long long nextScaleRepeatTick{};
    static unsigned long long nextDistanceRepeatTick{};
    static unsigned long long nextHorizontalRepeatTick{};
    static int repeatingScaleDirection{};
    static int repeatingDistanceDirection{};
    static int repeatingHorizontalDirection{};

    if (!hudDebuggingEnabled) return;

    const bool blocked = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0
        || (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    const bool fine = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool addDown = !blocked && (GetAsyncKeyState(VK_ADD) & 0x8000) != 0;
    const bool subtractDown = !blocked
        && (GetAsyncKeyState(VK_SUBTRACT) & 0x8000) != 0;
    const bool fartherDown = !blocked && (GetAsyncKeyState(VK_NUMPAD7) & 0x8000) != 0;
    const bool closerDown = !blocked && (GetAsyncKeyState(VK_NUMPAD9) & 0x8000) != 0;
    const bool leftDown = !blocked && (GetAsyncKeyState(VK_NUMPAD4) & 0x8000) != 0;
    const bool rightDown = !blocked && (GetAsyncKeyState(VK_NUMPAD6) & 0x8000) != 0;
    const bool multiplyDown = !blocked
        && (GetAsyncKeyState(VK_MULTIPLY) & 0x8000) != 0;
    const bool resetDown = !blocked && !fine
        && (GetAsyncKeyState(VK_NUMPAD5) & 0x8000) != 0;
    const auto now = GetTickCount64();

    int scaleDirection{};
    if (addDown && !subtractDown) scaleDirection = 1;
    else if (subtractDown && !addDown) scaleDirection = -1;
    const bool initialScalePress = (scaleDirection > 0 && !addWasDown)
        || (scaleDirection < 0 && !subtractWasDown);
    const bool scaleRepeat = scaleDirection && scaleDirection == repeatingScaleDirection
        && nextScaleRepeatTick && now >= nextScaleRepeatTick;
    if (scaleDirection && (initialScalePress || scaleRepeat)) {
        const float oldScale = hudQuadScale.load(std::memory_order_acquire);
        const float step = fine ? 0.05f : 0.25f;
        const float newScale = std::clamp(
            oldScale + step * static_cast<float>(scaleDirection),
            minimumHudScale, maximumHudScale);
        hudQuadScale.store(newScale, std::memory_order_release);
        nextScaleRepeatTick = now + (initialScalePress ? 350 : 90);
        repeatingScaleDirection = scaleDirection;
        std::ostringstream out;
        out << std::fixed << std::setprecision(2)
            << (scaleDirection > 0 ? "Num +" : "Num -")
            << ": HUD19 global element size=" << newScale << "x"
            << (fine ? " (fine preview)" : " (preview)");
        log(out.str());
    } else if (!scaleDirection) {
        repeatingScaleDirection = 0;
        nextScaleRepeatTick = 0;
    }

    int distanceDirection{};
    if (fartherDown && !closerDown) distanceDirection = 1;
    else if (closerDown && !fartherDown) distanceDirection = -1;
    const bool initialDistancePress = (distanceDirection > 0 && !fartherWasDown)
        || (distanceDirection < 0 && !closerWasDown);
    const bool distanceRepeat = distanceDirection
        && distanceDirection == repeatingDistanceDirection
        && nextDistanceRepeatTick && now >= nextDistanceRepeatTick;
    if (distanceDirection && (initialDistancePress || distanceRepeat)) {
        const float oldDistance = hudDistanceMeters.load(std::memory_order_acquire);
        const float step = fine ? 0.01f : 0.05f;
        const float newDistance = std::clamp(
            oldDistance + step * static_cast<float>(distanceDirection),
            minimumHudDistanceMeters, maximumHudDistanceMeters);
        hudDistanceMeters.store(newDistance, std::memory_order_release);
        nextDistanceRepeatTick = now + (initialDistancePress ? 350 : 90);
        repeatingDistanceDirection = distanceDirection;
        std::ostringstream out;
        out << std::fixed << std::setprecision(2)
            << (distanceDirection > 0 ? "Num 7" : "Num 9")
            << ": HUD19 global Flat-HUD distance=" << newDistance << "m"
            << (fine ? " (fine preview)" : " (preview)");
        log(out.str());
    } else if (!distanceDirection) {
        repeatingDistanceDirection = 0;
        nextDistanceRepeatTick = 0;
    }

    int horizontalDirection{};
    if (rightDown && !leftDown) horizontalDirection = 1;
    else if (leftDown && !rightDown) horizontalDirection = -1;
    const bool initialHorizontalPress = (horizontalDirection > 0 && !rightWasDown)
        || (horizontalDirection < 0 && !leftWasDown);
    const bool horizontalRepeat = horizontalDirection
        && horizontalDirection == repeatingHorizontalDirection
        && nextHorizontalRepeatTick && now >= nextHorizontalRepeatTick;
    if (horizontalDirection && (initialHorizontalPress || horizontalRepeat)) {
        const float oldOffset = hudHorizontalOffsetTan.load(std::memory_order_acquire);
        const float step = fine ? 0.002f : 0.01f;
        const float newOffset = std::clamp(
            oldOffset + step * static_cast<float>(horizontalDirection),
            minimumHudHorizontalOffsetTan, maximumHudHorizontalOffsetTan);
        hudHorizontalOffsetTan.store(newOffset, std::memory_order_release);
        nextHorizontalRepeatTick = now + (initialHorizontalPress ? 350 : 90);
        repeatingHorizontalDirection = horizontalDirection;
        constexpr float radiansToDegrees = 57.2957795131f;
        std::ostringstream out;
        out << std::fixed << std::setprecision(4)
            << (horizontalDirection > 0 ? "Num 6" : "Num 4")
            << ": HUD19 global horizontalTan=" << newOffset
            << std::setprecision(2) << " (" << std::atan(newOffset) * radiansToDegrees
            << " deg, " << (fine ? "fine preview)" : "preview)");
        log(out.str());
    } else if (!horizontalDirection) {
        repeatingHorizontalDirection = 0;
        nextHorizontalRepeatTick = 0;
    }

    if (resetDown && !resetWasDown) {
        hudDistanceMeters.store(hudLauncherDistanceMeters, std::memory_order_release);
        hudQuadScale.store(hudLauncherScale, std::memory_order_release);
        hudHorizontalOffsetTan.store(
            hudLauncherHorizontalOffsetTan, std::memory_order_release);
        std::ostringstream out;
        out << std::fixed << std::setprecision(2)
            << "Num 5: HUD19 global preview reset to launch values distance="
            << hudLauncherDistanceMeters << "m size=" << hudLauncherScale << 'x'
            << " horizontalTan=" << std::setprecision(4)
            << hudLauncherHorizontalOffsetTan;
        log(out.str());
    }

    if (multiplyDown && !multiplyWasDown) {
        const float distance = hudDistanceMeters.load(std::memory_order_acquire);
        const float scale = hudQuadScale.load(std::memory_order_acquire);
        const float horizontalOffsetTan =
            hudHorizontalOffsetTan.load(std::memory_order_acquire);
        std::ofstream calibration(flatCalibrationPath(), std::ios::trunc);
        calibration << std::fixed << std::setprecision(4)
            << distance << ' ' << scale << ' ' << horizontalOffsetTan << '\n';
        std::ofstream legacyScale(
            kharvox::runtimePathA("hud_quad_scale_saved.cfg"), std::ios::trunc);
        legacyScale << std::fixed << std::setprecision(2) << scale << '\n';
        std::ostringstream out;
        out << std::fixed << std::setprecision(2)
            << "Num *: HUD19 global Flat-HUD distance=" << distance
            << "m size=" << scale << "x horizontalTan="
            << std::setprecision(4) << horizontalOffsetTan << " SAVED";
        log(out.str());
    }

    addWasDown = addDown;
    subtractWasDown = subtractDown;
    fartherWasDown = fartherDown;
    closerWasDown = closerDown;
    leftWasDown = leftDown;
    rightWasDown = rightDown;
    multiplyWasDown = multiplyDown;
    resetWasDown = resetDown;
}

float KharvoxHudQuadScale() {
    return hudQuadScale.load(std::memory_order_acquire);
}

float KharvoxHudDistanceMeters() {
    return hudDistanceMeters.load(std::memory_order_acquire);
}

void KharvoxHudSetHeadsetGeometry(
    std::uint32_t surfaceWidth, std::uint32_t surfaceHeight,
    float safeTanHalfHorizontal, float safeTanHalfVertical) {
    if (!surfaceWidth || !surfaceHeight
        || !std::isfinite(safeTanHalfHorizontal)
        || !std::isfinite(safeTanHalfVertical)
        || safeTanHalfHorizontal <= 0.0f || safeTanHalfVertical <= 0.0f) return;

    // Pixel density and angular layout are deliberately independent: the
    // OpenXR surface owns sharpness, while the fixed angular reference owns
    // placement and size. Only a narrower common-eye FOV may shrink the HUD to
    // prevent clipping; a wider SteamXR FOV must never spread it apart.
    const float availableFit = std::min(
        safeTanHalfHorizontal / kharvox::nativeHudTanHalfHorizontal,
        safeTanHalfVertical / kharvox::nativeHudTanHalfVertical);
    const float fitScale = kharvox::selectHudLayoutFit(
        safeTanHalfHorizontal, safeTanHalfVertical);
    hudHeadsetFitScale.store(fitScale, std::memory_order_release);

    const auto oldWidth = hudHeadsetSurfaceWidth.exchange(
        surfaceWidth, std::memory_order_acq_rel);
    const auto oldHeight = hudHeadsetSurfaceHeight.exchange(
        surfaceHeight, std::memory_order_acq_rel);
    if (oldWidth != surfaceWidth || oldHeight != surfaceHeight) {
        std::ostringstream out;
        out << std::fixed << std::setprecision(3)
            << "[HUD9-ADAPT] runtime surface=" << surfaceWidth << 'x' << surfaceHeight
            << " safeTan=" << safeTanHalfHorizontal << 'x' << safeTanHalfVertical
            << " available-fit=" << availableFit
            << " applied-fit=" << fitScale
            << " policy=fixed-angular-reference"
            << " user-reference=" << hudReferenceUserScale << 'x';
        log(out.str());
    }
}

void KharvoxHudGetDiagnosticSnapshot(KharvoxHudDiagnosticSnapshot& snapshot) {
    snapshot.matchedSurfaces = diagnosticMatchedSurfaces.load(std::memory_order_acquire);
    snapshot.completedSurfaces = diagnosticCompletedSurfaces.load(std::memory_order_acquire);
    snapshot.crosshairSurfaces = diagnosticCrosshairSurfaces.load(std::memory_order_acquire);
    snapshot.lastMatchQpc = diagnosticLastMatchQpc.load(std::memory_order_acquire);
    snapshot.lastThreadId = diagnosticLastThreadId.load(std::memory_order_relaxed);
    snapshot.lastCallerRva = diagnosticLastCallerRva.load(std::memory_order_relaxed);
    snapshot.lastWidth = diagnosticLastWidth.load(std::memory_order_relaxed);
    snapshot.lastHeight = diagnosticLastHeight.load(std::memory_order_relaxed);
    snapshot.lastScaleMilli = diagnosticLastScaleMilli.load(std::memory_order_relaxed);
    snapshot.lastWasCrosshair = diagnosticLastWasCrosshair.load(std::memory_order_relaxed);
}

std::size_t KharvoxHudCopyDiagnosticEvents(
    std::uint64_t firstSerial, KharvoxHudDiagnosticEvent* events, std::size_t capacity) {
    if (!events || !capacity || !firstSerial) return 0;
    const auto lastSerial = diagnosticMatchedSurfaces.load(std::memory_order_acquire);
    std::size_t copied{};
    for (auto serial = firstSerial; serial <= lastSerial && copied < capacity; ++serial) {
        auto& slot = diagnosticEvents[serial % diagnosticEventCapacity];
        if (slot.serial.load(std::memory_order_acquire) != serial) continue;
        KharvoxHudDiagnosticEvent event{};
        event.serial = serial;
        event.qpc = slot.qpc.load(std::memory_order_relaxed);
        event.threadId = slot.threadId.load(std::memory_order_relaxed);
        event.callerRva = slot.callerRva.load(std::memory_order_relaxed);
        event.width = slot.width.load(std::memory_order_relaxed);
        event.height = slot.height.load(std::memory_order_relaxed);
        event.scaleMilli = slot.scaleMilli.load(std::memory_order_relaxed);
        event.crosshair = slot.crosshair.load(std::memory_order_relaxed);
        if (slot.serial.load(std::memory_order_acquire) != serial) continue;
        events[copied++] = event;
    }
    return copied;
}

bool KharvoxHudFullscreenMenuActive() {
    const auto lastSeen = fullscreenMenuLastSeenTick.load(std::memory_order_acquire);
    const auto now = GetTickCount64();
    const bool detectedMenu = lastSeen && now >= lastSeen
        && now - lastSeen <= fullscreenMenuHoldMilliseconds;
    return detectedMenu || KharvoxHudFieldDroneMenuActive()
        || KharvoxHudSuitUpgradeMenuActive()
        || KharvoxHudPlayerUpgradeMenuActive()
        || KharvoxHudRuneSelectMenuActive()
        || KharvoxHudEndOfLevelMenuActive();
}

bool KharvoxHudFieldDroneMenuActive() {
    const auto until = fieldDroneMenuSessionUntilTick.load(std::memory_order_acquire);
    if (!until) return false;
    const auto now = GetTickCount64();
    if (kharvox::shouldKeepNativeMenuSession(
            fieldDroneNativeActivatorBound.load(std::memory_order_acquire),
            now, until)) return true;

    auto expected = until;
    if (fieldDroneMenuSessionUntilTick.compare_exchange_strong(
            expected, 0, std::memory_order_acq_rel)) {
        fieldDroneNativeActivatorBound.store(false, std::memory_order_release);
        log("Field Drone menu safety timeout; Quad session released");
    }
    return false;
}

void KharvoxHudBeginFieldDroneMenuSession() {
    const auto now = GetTickCount64();
    fieldDroneNativeActivatorBound.store(true, std::memory_order_release);
    fieldDroneMenuSessionUntilTick.store(
        now + fieldDroneMenuSessionMilliseconds, std::memory_order_release);
    log("native Field Drone BindActivator; full-screen menu Quad session started");
}

void KharvoxHudEndFieldDroneMenuSession() {
    fieldDroneNativeActivatorBound.store(false, std::memory_order_release);
    if (fieldDroneMenuSessionUntilTick.exchange(0, std::memory_order_acq_rel))
        log("native Field Drone ReleaseActivator; Quad session released");
}

bool KharvoxHudSuitUpgradeMenuActive() {
    const auto until = suitUpgradeMenuSessionUntilTick.load(
        std::memory_order_acquire);
    if (!until) return false;
    const auto now = GetTickCount64();
    if (kharvox::shouldKeepNativeMenuSession(
            suitUpgradeNativeSessionActive.load(std::memory_order_acquire),
            now, until)) return true;

    auto expected = until;
    if (suitUpgradeMenuSessionUntilTick.compare_exchange_strong(
            expected, 0, std::memory_order_acq_rel)) {
        suitUpgradeNativeSessionActive.store(false, std::memory_order_release);
        log("Suit Upgrade menu safety timeout; Quad session released");
    }
    return false;
}

void KharvoxHudBeginSuitUpgradeMenuSession() {
    const auto now = GetTickCount64();
    const bool alreadyActive = suitUpgradeNativeSessionActive.exchange(
        true, std::memory_order_acq_rel);
    suitUpgradeMenuSessionUntilTick.store(
        now + suitUpgradeMenuSessionMilliseconds, std::memory_order_release);
    if (!alreadyActive)
        log("native Suit Upgrade ActivateStation; full-screen menu Quad session started");
}

void KharvoxHudEndSuitUpgradeMenuSession() {
    suitUpgradeNativeSessionActive.store(false, std::memory_order_release);
    if (suitUpgradeMenuSessionUntilTick.exchange(0, std::memory_order_acq_rel))
        log("native Suit Upgrade Event_ReleasePlayer; Quad session released");
}

bool KharvoxHudPauseMenuActive() {
    return pauseMenuActive.load(std::memory_order_acquire);
}

bool KharvoxHudDeathMenuActive() {
    if (!deathMenuActive.load(std::memory_order_acquire)) return false;
    const auto shownGeneration = deathMenuLevelGenerationAtShow.load(
        std::memory_order_acquire);
    const auto currentGeneration = KharvoxCameraLevelTransitionGeneration();
    if (currentGeneration != shownGeneration
        && deathMenuActive.exchange(false, std::memory_order_acq_rel)) {
        log("native CampaignDeath latch released by confirmed level/menu transition generation="
            + std::to_string(currentGeneration));
        return false;
    }
    return deathMenuActive.load(std::memory_order_acquire);
}

bool KharvoxHudEndOfLevelMenuActive() {
    const auto activeMask = endOfLevelScreenMask.load(std::memory_order_acquire);
    if (!activeMask) return false;
    const auto shownGeneration = endOfLevelMenuGenerationAtShow.load(
        std::memory_order_acquire);
    const auto currentGeneration = KharvoxCameraLevelTransitionGeneration();
    if (kharvox::shouldKeepEndOfLevelMenuSession(
            true, shownGeneration, currentGeneration,
            KharvoxCameraGameplayActive())) return true;

    if (endOfLevelScreenMask.exchange(0, std::memory_order_acq_rel))
        log("native EndOfLevel latch released after new playable level generation="
            + std::to_string(currentGeneration));
    return false;
}

bool KharvoxHudPlayerUpgradeMenuActive() {
    const auto now = GetTickCount64();
    if (playerUpgradePickupSessionActive.load(std::memory_order_acquire)) {
        const auto sessionStart = playerUpgradePickupSessionStartTick.load(
            std::memory_order_acquire);
        const auto until = playerUpgradePickupSessionUntilTick.load(
            std::memory_order_acquire);
        const bool cinematicActive = KharvoxCameraCutsceneActive();
        if (cinematicActive
            && !playerUpgradePickupCinematicObserved.exchange(
                true, std::memory_order_acq_rel)) {
            log("Elite Guard pickup cinematic detected; keeping immersive Projection/head tracking until the actual menu");
        }
        const auto pickupPhase = kharvox::selectPlayerUpgradePickupPhase(
            true,
            playerUpgradePickupCinematicObserved.load(std::memory_order_acquire),
            cinematicActive, KharvoxCameraGameplayActive(), now, sessionStart,
            playerUpgradePickupEntryGraceMilliseconds, until,
            playerUpgradePickupConfirmedTick.load(std::memory_order_acquire));
        if (pickupPhase
            == kharvox::PlayerUpgradePickupPhase::CenteredMenuFallback) {
            if (!playerUpgradePickupFallbackQuadLogged.exchange(
                    true, std::memory_order_acq_rel)) {
                log("[PRAETOR-QUAD] confirmed pickup or non-gameplay fallback; centered Quad/UI session active");
            }
            return true;
        }

        if (pickupPhase == kharvox::PlayerUpgradePickupPhase::Complete
            && playerUpgradePickupSessionActive.exchange(
                false, std::memory_order_acq_rel)) {
            playerUpgradePickupCinematicObserved.store(
                false, std::memory_order_release);
            playerUpgradePickupFallbackQuadLogged.store(
                false, std::memory_order_release);
            playerUpgradePickupSessionStartTick.store(
                0, std::memory_order_release);
            playerUpgradePickupSessionUntilTick.store(
                0, std::memory_order_release);
            if (until && now > until)
                log("Praetor pickup transition safety timeout; pending session released");
            else
                log("Elite Guard pickup cinematic ended without an active menu; immersive gameplay retained");
        }
    }

    auto lastManagerFrame = playerUpgradeManagerLastFrameTick.load(
        std::memory_order_acquire);
    if (kharvox::shouldKeepPulsedMenuSession(
            lastManagerFrame, now,
            playerUpgradeManagerFrameHoldMilliseconds)) return true;
    if (lastManagerFrame
        && playerUpgradeManagerLastFrameTick.compare_exchange_strong(
            lastManagerFrame, 0, std::memory_order_acq_rel))
        log("native PlayerUpgrade manager frames ended; Drone-style Quad heartbeat released");

    const auto activeMask = playerUpgradeScreenMask.load(std::memory_order_acquire);
    if (!activeMask) return false;
    const auto shownGeneration = playerUpgradeMenuGenerationAtShow.load(
        std::memory_order_acquire);
    const auto currentGeneration = KharvoxCameraLevelTransitionGeneration();
    if (kharvox::shouldKeepEndOfLevelMenuSession(
            true, shownGeneration, currentGeneration,
            KharvoxCameraGameplayActive())) return true;

    if (playerUpgradeScreenMask.exchange(0, std::memory_order_acq_rel))
        log("native PlayerUpgrade/Suit latch released after new playable level generation="
            + std::to_string(currentGeneration));
    return false;
}

bool KharvoxHudPauseRootVisible() {
    return pauseMenuActive.load(std::memory_order_acquire)
        && pauseRootVisible.load(std::memory_order_acquire);
}

bool KharvoxHudRuneSelectMenuActive() {
    bool challengeScreenActive{};
    if (runeChallengeScreenActive.load(std::memory_order_acquire)) {
        const auto now = GetTickCount64();
        const auto deadline = runeChallengeScreenSessionUntilTick.load(
            std::memory_order_acquire);
        const auto currentGeneration = KharvoxCameraLevelTransitionGeneration();
        const int pendingLoadCommand = runeChallengePendingLoadCommand.load(
            std::memory_order_acquire);
        const bool mapLoadComplete =
            kharvox::shouldReleaseRuneChallengeMapLoad(
                true,
                runeChallengeMapLoadPending.load(std::memory_order_acquire),
                runeChallengeGenerationAtLoad.load(std::memory_order_acquire),
                currentGeneration, KharvoxCameraGameplayActive());
        const bool withinSafetyDeadline = kharvox::shouldKeepNativeMenuSession(
            true, now, deadline);
        const bool checkpointRecovery =
            kharvox::shouldReleaseRuneChallengeCheckpointRecovery(
                true,
                runeChallengePauseRecoveryArmed.load(std::memory_order_acquire),
                runeChallengeGenerationAtPause.load(std::memory_order_acquire),
                currentGeneration, KharvoxCameraGameplayActive());
        challengeScreenActive = withinSafetyDeadline
            && !checkpointRecovery && !mapLoadComplete;
        if (!challengeScreenActive
            && runeChallengeScreenActive.exchange(false, std::memory_order_acq_rel)) {
            runeChallengePauseRecoveryArmed.store(
                false, std::memory_order_release);
            runeChallengeGenerationAtPause.store(
                0, std::memory_order_release);
            runeChallengeMapLoadPending.store(
                false, std::memory_order_release);
            runeChallengeGenerationAtLoad.store(
                0, std::memory_order_release);
            runeChallengePendingLoadCommand.store(
                -1, std::memory_order_release);
            runeChallengeScreenGenerationAtShow.store(
                0, std::memory_order_release);
            runeChallengeScreenSessionUntilTick.store(
                0, std::memory_order_release);
            if (!withinSafetyDeadline) {
                log("native Gui_EndOfChallenge Rune Trial safety timeout; Quad/UI session released");
            } else if (mapLoadComplete) {
                log("native Gui_EndOfChallenge command="
                    + std::to_string(pendingLoadCommand)
                    + " map load reached stable gameplay generation="
                    + std::to_string(currentGeneration)
                    + "; Quad/UI session released");
            } else if (checkpointRecovery) {
                log("native Gui_EndOfChallenge latch released after Pause/Load Checkpoint reached playable generation="
                    + std::to_string(currentGeneration));
            }
        }
    }

    bool guiScreenActive{};
    if (runeSelectMenuActive.load(std::memory_order_acquire)) {
        const auto shownGeneration = runeSelectMenuGenerationAtShow.load(
            std::memory_order_acquire);
        const auto currentGeneration = KharvoxCameraLevelTransitionGeneration();
        guiScreenActive = kharvox::shouldKeepEndOfLevelMenuSession(
            true, shownGeneration, currentGeneration,
            KharvoxCameraGameplayActive());
        if (!guiScreenActive
            && runeSelectMenuActive.exchange(false, std::memory_order_acq_rel))
            log("native Gui_RuneSelect latch released after Rune Trial/load transition generation="
                + std::to_string(currentGeneration));
    }

    const auto until = runeTrialMenuSessionUntilTick.load(
        std::memory_order_acquire);
    if (!until) return challengeScreenActive || guiScreenActive;
    const auto now = GetTickCount64();
    const auto generationAtActivation =
        runeTrialMenuGenerationAtActivation.load(std::memory_order_acquire);
    const auto currentGeneration = KharvoxCameraLevelTransitionGeneration();
    const bool challengeLoadPending =
        runeTrialChallengeLoadPending.load(std::memory_order_acquire);
    if (kharvox::shouldKeepRuneTrialMenuSession(
            runeTrialNativeSessionActive.load(std::memory_order_acquire),
            challengeLoadPending, generationAtActivation, currentGeneration,
            KharvoxCameraGameplayActive(), now, until)) return true;

    auto expected = until;
    if (runeTrialMenuSessionUntilTick.compare_exchange_strong(
            expected, 0, std::memory_order_acq_rel)) {
        runeTrialNativeSessionActive.store(false, std::memory_order_release);
        runeTrialChallengeLoadPending.store(false, std::memory_order_release);
        runeTrialMenuGenerationAtActivation.store(0, std::memory_order_release);
        if (challengeLoadPending && currentGeneration != generationAtActivation
            && KharvoxCameraGameplayActive()) {
            log("Rune Trial safety deadline reached after accepted load generation="
                + std::to_string(currentGeneration)
                + "; Quad/UI session released without destination ownership");
        } else {
            log("VegaTraining/Rune menu safety timeout; Quad/UI session released");
        }
    }
    return challengeScreenActive || guiScreenActive;
}

bool KharvoxHudUpgradeCinematicRefreshGuardActive() {
    if (!upgradeCinematicRefreshGuardActive.load(std::memory_order_acquire))
        return false;

    const auto now = GetTickCount64();
    const auto start = upgradeCinematicRefreshGuardStartTick.load(
        std::memory_order_acquire);
    const auto until = upgradeCinematicRefreshGuardUntilTick.load(
        std::memory_order_acquire);
    if (kharvox::shouldKeepCinematicMenuSession(
            true, KharvoxCameraCutsceneActive(),
            KharvoxCameraGameplayActive(), now, start,
            upgradeCinematicRefreshGuardMinimumMilliseconds, until)) {
        return true;
    }

    if (upgradeCinematicRefreshGuardActive.exchange(
            false, std::memory_order_acq_rel)) {
        upgradeCinematicRefreshGuardStartTick.store(0, std::memory_order_release);
        upgradeCinematicRefreshGuardUntilTick.store(0, std::memory_order_release);
        log(until && now > until
            ? "VEGA/Argent refresh guard safety timeout"
            : "VEGA/Argent refresh handoff settled");
    }
    return false;
}

bool KharvoxHudTutorialActive() {
    const auto now = GetTickCount64();
    const auto until = tutorialSessionUntilTick.load(
        std::memory_order_acquire);
    if (until && now > until) {
        tutorialManagerLastActiveFrameTick.store(0, std::memory_order_release);
        tutorialScreenMask.store(0, std::memory_order_release);
        if (tutorialSessionUntilTick.exchange(0, std::memory_order_acq_rel))
            log("Tutorial activity safety timeout; Projection exclusion released");
        return false;
    }

    auto lastManagerFrame = tutorialManagerLastActiveFrameTick.load(
        std::memory_order_acquire);
    if (kharvox::shouldKeepPulsedMenuSession(
            lastManagerFrame, now,
            tutorialManagerFrameHoldMilliseconds)) return true;

    // The manager's active-owner pointer is authoritative. If native
    // HideScreen was skipped, retire any stale screen bit immediately after
    // the last confirmed active manager frame instead of trapping gameplay in
    // Quad until the long safety deadline.
    if (lastManagerFrame) {
        tutorialManagerLastActiveFrameTick.store(0, std::memory_order_release);
        tutorialSessionUntilTick.store(0, std::memory_order_release);
        tutorialScreenMask.store(0, std::memory_order_release);
        log("native Tutorial manager active screen ended; Projection exclusion released");
        return false;
    }
    return tutorialScreenMask.load(std::memory_order_acquire) != 0;
}

void KharvoxHudSetHandPose(
    bool rightHand,
    float gripForward, float gripLateral, float gripUp,
    float quaternionX, float quaternionY, float quaternionZ, float quaternionW,
    bool valid) {
    std::lock_guard<std::mutex> poseGuard(handPoseMutex);
    auto& hand = handPoses[rightHand ? 0 : 1];
    const float grip[]{gripForward, gripLateral, gripUp};
    const float quaternion[]{quaternionX, quaternionY, quaternionZ, quaternionW};
    for (int index = 0; index < 3; ++index)
        hand.grip[index].store(grip[index], std::memory_order_relaxed);
    for (int index = 0; index < 4; ++index)
        hand.quaternion[index].store(quaternion[index], std::memory_order_relaxed);
    hand.valid.store(valid, std::memory_order_release);
}

bool KharvoxHudMovieActive(){
    const auto until=hudMovieUntil.load(std::memory_order_acquire);
    return until && GetTickCount64()<until;
}

bool KharvoxHudOffhandCalibrationActive(){return offhandCalibrationActive.load(std::memory_order_acquire);}

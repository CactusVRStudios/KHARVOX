#include "LaserWorldPose.h"
#include "LaserSourcePolicy.h"
#include "../common/AerRenderOrder.h"
#include "../common/DiagnosticLogging.h"
#include "../common/PoseTrace.h"
#include "WeaponHook.h"
#include "AerWeaponPairPolicy.h"
#include "AerWeaponPoseCache.h"
#include "CollectiblePresentation.h"
#include "../sfs/NativeSfs.h"

#include "../camera/CameraHook.h"
#include "../hud/HudHook.h"
#include "../hands/HandDepthPolicy.h"

#include <windows.h>
#include <intrin.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include "../common/RuntimePaths.h"

static DWORD portableGetFileAttributesW(LPCWSTR path) {
    const auto name = wcsrchr(path, L'\\');
    return GetFileAttributesW(kharvox::runtimePath(name ? name + 1 : path).c_str());
}
#define GetFileAttributesW portableGetFileAttributesW
#include <sstream>

namespace {
struct PoseState {
    std::array<std::atomic<float>, 3> grip{};
    std::array<std::atomic<float>, 3> baselineGrip{};
    std::array<std::atomic<float>, 4> deltaQuaternion{};
    std::atomic<bool> valid{};
    std::atomic<unsigned> resetGeneration{1};
    uint64_t sampleQpc{}; // Protected by controllerInputMutex.
};

PoseState pose;
std::mutex controllerInputMutex;
std::atomic<uint64_t> weaponSourceEpoch{1};
kharvox::AerWeaponSourceHistory weaponSourceHistory;
kharvox::AerWeaponSourceTransforms weaponSourceTransforms;
thread_local kharvox::AerWeaponFrame weaponRootSource;
thread_local bool weaponRootSourceValid{};
thread_local uint64_t weaponRootSourcePresent{};

kharvox::AerWeaponInput currentControllerInput(){
    std::lock_guard lock(controllerInputMutex);
    kharvox::AerWeaponInput input;
    for(int i=0;i<3;++i){input.grip[i]=pose.grip[i].load();input.baseline[i]=pose.baselineGrip[i].load();}
    for(int i=0;i<4;++i)input.orientation[i]=pose.deltaQuaternion[i].load();
    input.valid=pose.valid.load();input.generation=pose.resetGeneration.load();input.epoch=weaponSourceEpoch.load();
    input.sampleQpc=pose.sampleQpc;
    return input;
}
bool resolveWeaponSource(kharvox::AerWeaponFrame& frame,
    kharvox::AerWeaponResolveDiagnostic* details=nullptr){
    kharvox::AerWeaponResolveDiagnostic diagnostic;
    const bool resolved=weaponSourceHistory.resolveCurrent([]{return KharvoxCameraCurrentPresentSerial();},
        KharvoxCameraLevelTransitionGeneration(),weaponSourceEpoch.load(std::memory_order_acquire),
        pose.resetGeneration.load(std::memory_order_acquire),frame,diagnostic);
    if(details)*details=diagnostic;
    return resolved;
}
bool controllerPlacementActive(){
    if(pose.valid.load(std::memory_order_acquire))return true;
    kharvox::AerWeaponFrame source;
    return KharvoxCameraUsesAerGameplaySource()&&resolveWeaponSource(source);
}
void traceWeaponSource(unsigned kind,uintptr_t entity,const kharvox::AerWeaponFrame& frame,
    const float* origin,const float* axis,int status){
    if(!kharvox::pose_trace::active.load(std::memory_order_relaxed))return;
    kharvox::pose_trace::Event e{};e.kind=kind;e.source=entity;e.status=status;
    e.frame=KharvoxCameraCurrentPresentSerial();e.poseId=frame.camera.key.poseId;e.eye=frame.camera.key.eye;
    e.revision=frame.input.epoch;e.flags=frame.input.generation;
    if(origin)std::memcpy(e.data,origin,3*sizeof(float));
    if(axis)std::memcpy(e.data+3,axis,9*sizeof(float));
    e.data[12]=frame.camera.bodyYawDelta;
    kharvox::pose_trace::record(e);
}
bool customHandsRequested() {
    static const bool enabled = [] {
        char value[8]{};
        return GetEnvironmentVariableA("KHARVOX_SHOW_HANDS", value, sizeof(value)) > 0
            && !strcmp(value, "1");
    }();
    return enabled;
}
std::atomic<bool> installed{};
std::atomic<bool> fovHookInstalled{};
std::atomic<bool> muzzleFireAxisOverride{};
volatile LONG* handsHitReactionsEnable{};
SRWLOCK handsHitReactionsLock = SRWLOCK_INIT;
bool handsHitReactionsSuppressed{};
LONG handsHitReactionsPreviousValue{};
volatile LONG* weaponKickEnable{};
SRWLOCK weaponKickLock = SRWLOCK_INIT;
bool weaponKickSuppressed{};
LONG weaponKickPreviousValue{};
std::atomic<KharvoxWeaponKind> activeWeaponKind{KharvoxWeaponKind::Unknown};
std::atomic<uintptr_t> activeWeaponData{};
using InventoryCountFn = int(__fastcall*)(void*);
using InventoryItemFn = void*(__fastcall*)(void*, int);
using WeaponAmmoTotalFn = int(__fastcall*)(void*, void*);
InventoryCountFn inventoryCountNative{};
InventoryItemFn inventoryItemNative{};
WeaponAmmoTotalFn weaponAmmoTotalNative{};
std::atomic<bool> weaponAmmoSnapshotSupported{};
std::array<std::atomic<KharvoxWeaponAmmoState>,
    static_cast<size_t>(KharvoxWeaponKind::Count)> weaponAmmoStates{};
std::atomic<uintptr_t> weaponAmmoSnapshotPlayer{};
std::atomic<unsigned long long> weaponAmmoSnapshotTick{};
std::atomic<unsigned> unknownWeaponRescanCalls{};
unsigned char* muzzleFireAxisBranch{};
using HandsFovScaleFn = float(__fastcall*)(void*);
HandsFovScaleFn originalHandsFovScale{};
unsigned calibratedGeneration{};
uintptr_t calibratedEntity{};
std::array<float, 3> modelOriginLocal{};
std::array<float, 9> modelAxisLocal{};
thread_local bool controllerRootProbeValid{};
thread_local void* activeHands{};
std::atomic<bool> collectibleClassifierSupported{};
std::atomic<unsigned long long> collectibleSeenPresent{};
std::atomic<unsigned long long> collectibleSeenLevel{};
using UpdateHandsTransformFn = void(__fastcall*)(void*);
UpdateHandsTransformFn originalUpdateHandsTransform{};
using ResolveHandsResourceFn = short*(__fastcall*)(void*, short*, void*, const char*);
ResolveHandsResourceFn originalResolveHandsResource{};
using GetHandsModelFn = void*(__fastcall*)(void*);
using GetJointTransformFn = bool(__fastcall*)(void*, int, unsigned, float*, float*);
using HideRenderModelMeshFn = void(__fastcall*)(void*, int);
using WeaponRenderUpdateFn = void(__fastcall*)(
    void*, const float*, const float*, const float*, int, int, int, int, float, float);
GetHandsModelFn getHandsModel{};
GetJointTransformFn getJointTransform{};
using GetNamedJointTransformFn=bool(__fastcall*)(void*,int,const char*,float*,float*);
GetNamedJointTransformFn getNamedJointTransform{};
HideRenderModelMeshFn hideRenderModelMesh{};
WeaponRenderUpdateFn originalWeaponRenderUpdate{};
std::array<std::atomic<float>, 3> animatedGripPivot{};
std::atomic<bool> animatedGripPivotValid{};
std::array<std::atomic<float>, 3> neutralGripPivot{};
std::atomic<bool> neutralGripPivotValid{};
std::atomic<unsigned> neutralGripPivotGeneration{};
unsigned neutralGripPivotWarmup{};
unsigned neutralGripPivotSamples{};
std::array<float, 3> neutralGripPivotSum{};
std::atomic<uintptr_t> lastControllerRootEntity{};
std::array<std::atomic<float>, 3> lastNativeControllerRootOrigin{};
std::array<std::atomic<float>, 9> lastNativeControllerRootAxis{};
std::array<std::atomic<float>, 3> lastControllerRootOrigin{};
std::array<std::atomic<float>, 9> lastControllerRootAxis{};
std::array<std::atomic<float>, 3> lastWeaponBodyAnchorOrigin{};
std::atomic<uintptr_t> lastWeaponPropEntity{};
std::atomic<uintptr_t> lastWeaponRenderObject{};
std::array<std::atomic<float>, 3> lastWeaponPresentOrigin{};
std::array<std::atomic<float>, 9> lastWeaponPresentAxis{};
std::array<std::atomic<float>, 3> lastWeaponPresentViewOffset{};
std::array<std::atomic<int>, 4> lastWeaponPresentIntegers{};
std::atomic<float> lastWeaponPresentModelScale{};
std::atomic<float> lastWeaponPresentDepthHack{};
struct LaserSourceSnapshot {
    std::array<float,3> origin{}, direction{}, bodyOrigin{};
    std::array<float,9> bodyAxis{};
    KharvoxWeaponKind kind{KharvoxWeaponKind::Unknown};
    uint64_t tick{}, epoch{};
};
std::mutex laserSourceMutex;
kharvox::AerInputHistory<LaserSourceSnapshot> laserSources;
LaserSourceSnapshot latestLaserSource;
std::atomic<unsigned> laserMuzzleKindsLogged{};
std::atomic<unsigned> laserMuzzleFailureKindsLogged{};
constexpr size_t weaponObjectSnapshotBytes = 0x2000;
std::array<unsigned char, weaponObjectSnapshotBytes> baselineWeaponObjectBytes{};
bool baselineWeaponObjectValid{};
uintptr_t baselineWeaponObjectAddress{};
std::array<std::atomic<float>, 3> lastWeaponPropOrigin{};
std::array<std::atomic<float>, 9> lastWeaponPropAxis{};
std::atomic<unsigned> boneSnapshotCount{};
bool boneSnapshotKeyWasDown{};
std::atomic<bool> jointScanLogged{};
std::atomic<unsigned long long> jointScanCalls{};
std::atomic<bool> playerRootHiddenLogged{};
std::atomic<std::uint64_t> aerRenderPairState{
    kharvox::packAerWeaponPairState({1, -1, false})};

kharvox::AerWeaponPoseCache aerWeaponPoseCache;

bool readableRange(const void* address, size_t bytes);
void log(const std::string& text);
void setHandsHitReactionsSuppressed(bool suppress);
void setWeaponKickSuppressed(bool suppress);
void invalidateAerWeaponPairCache();
bool synchronizeAerAnimatedWeaponProp(
    uintptr_t entity, const float* nativeOrigin, const float* nativeAxis,
    float synchronizedOrigin[3], float synchronizedAxis[9]);

bool installMuzzleFireAxisOverride(unsigned char* image) {
    // idHands::FireWeapon normally redirects the muzzle toward the 2D
    // crosshair. The weapon-decl flag useMuzzleAsFireAxis bypasses that path.
    // Preserve the native implementation and only force its existing branch
    // while a valid VR weapon pose is active.
    auto branch = image + 0xD5EFF3;
    constexpr unsigned char signature[]{0x0F,0x85,0x6A,0x06,0x00,0x00};
    if (std::memcmp(branch, signature, sizeof(signature))) {
        log("RVA 0xD5EFF3 useMuzzleAsFireAxis branch signature mismatch; VR shot direction disabled");
        return false;
    }
    muzzleFireAxisBranch = branch;
    log("native idHands useMuzzleAsFireAxis branch armed at RVA 0xD5EFF3");
    return true;
}

bool installFrontPushbackSuppression(unsigned char* image) {
    // Two independent near-contact traces can set the same hands pushback flag
    // at state+0x175. The old patch changed only the later trace branch and
    // jumped to this common block, so a flag set by the earlier/behind-target
    // path still played "hands_moving_front_pushback_into" and could remain
    // latched. Clear the common flag and skip only that local hands-animation
    // block. Player collision, damage, physical knockback and world movement
    // remain untouched.
    if (GetFileAttributesW(kharvox::runtimePath(L"enable_6dof_weapon").c_str()) ==
        INVALID_FILE_ATTRIBUTES) {
        return false;
    }

    auto target = image + 0x6305B4;
    constexpr unsigned char signature[]{
        0x41,0x80,0xBD,0x75,0x01,0x00,0x00,0x00,
        0x0F,0x84,0xF2,0x00,0x00,0x00
    };
    constexpr unsigned char replacement[]{
        // mov byte ptr [r13+0x175],0
        0x41,0xC6,0x85,0x75,0x01,0x00,0x00,0x00,
        // jmp 0x6306B4; nop
        0xE9,0xF3,0x00,0x00,0x00,0x90
    };
    if (std::memcmp(target, signature, sizeof(signature))) {
        log("hands collision-pushback signature mismatch at RVA 0x6305B4; suppression disabled");
        return false;
    }

    DWORD oldProtect{};
    if (!VirtualProtect(target, sizeof(signature), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        log("front-pushback target protection failed");
        return false;
    }
    std::memcpy(target, replacement, sizeof(replacement));
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(replacement));
    DWORD ignored{};
    VirtualProtect(target, sizeof(signature), oldProtect, &ignored);
    log("6DoF hands collision-pushback flag cleared and common animation block skipped at RVA 0x6305B4");
    return true;
}

bool armHandsHitReactionSuppression(unsigned char* image) {
    if (GetFileAttributesW(kharvox::runtimePath(L"enable_6dof_weapon").c_str()) ==
        INVALID_FILE_ATTRIBUTES) {
        return false;
    }

    // Both reaction start and the running animation update check this CVar.
    // Suppress it before idHandsHitReactions::Init and keep it suppressed for
    // the complete 6DoF process lifetime. Toggling it around pause/cinematics
    // can otherwise resume a reaction and then freeze its additive pose on the
    // next gameplay entry. Damage, player knockback and enemy behavior remain
    // on their native paths.
    auto triggerGate = image + 0xD88998;
    auto updateGate = image + 0xD892E4;
    constexpr unsigned char triggerSignature[]{0x83,0x3D,0x11,0x6E,0xE2,0x04,0x00};
    constexpr unsigned char updateSignature[]{0x83,0x3D,0xC5,0x64,0xE2,0x04,0x00};
    if (std::memcmp(triggerGate, triggerSignature, sizeof(triggerSignature)) ||
        std::memcmp(updateGate, updateSignature, sizeof(updateSignature))) {
        log("hands hit-reaction gate signature mismatch at RVA 0xD88998; suppression disabled");
        return false;
    }

    auto value = reinterpret_cast<volatile LONG*>(image + 0x5BAF7B0);
    const LONG current = InterlockedCompareExchange(value, 0, 0);
    if (current != 0 && current != 1) {
        log("hands_hitReactionsEnable has an unexpected initial value; suppression disabled");
        return false;
    }
    handsHitReactionsEnable = value;
    setHandsHitReactionsSuppressed(true);
    log("6DoF idHands hit-reaction suppression armed session-wide; trigger RVA 0xD88998, update RVA 0xD892E4, CVar RVA 0x5BAF7B0");
    return true;
}

void setHandsHitReactionsSuppressed(bool suppress) {
    if (!handsHitReactionsEnable) return;

    bool changed = false;
    LONG restoredValue = 0;
    AcquireSRWLockExclusive(&handsHitReactionsLock);
    if (suppress) {
        if (!handsHitReactionsSuppressed) {
            handsHitReactionsPreviousValue =
                InterlockedExchange(handsHitReactionsEnable, 0);
            handsHitReactionsSuppressed = true;
            changed = true;
        } else if (InterlockedCompareExchange(handsHitReactionsEnable, 0, 0) != 0) {
            // Reassert the isolation if a config refresh rewrites the CVar.
            InterlockedExchange(handsHitReactionsEnable, 0);
        }
    } else if (handsHitReactionsSuppressed) {
        restoredValue = handsHitReactionsPreviousValue;
        InterlockedExchange(handsHitReactionsEnable, restoredValue);
        handsHitReactionsSuppressed = false;
        changed = true;
    }
    ReleaseSRWLockExclusive(&handsHitReactionsLock);

    if (changed) {
        if (suppress) {
            log("6DoF first-person hands hit reactions SUPPRESSED SESSION-WIDE; damage and player knockback preserved");
        } else {
            log("first-person hands hit reactions restored to previous value=" +
                std::to_string(restoredValue));
        }
    }
}

bool armWeaponKickSuppression(unsigned char* image) {
    if (GetFileAttributesW(kharvox::runtimePath(L"enable_6dof_weapon").c_str()) ==
        INVALID_FILE_ATTRIBUTES) {
        return false;
    }

    // DOOM VFR explicitly runs with g_weaponKick disabled. DOOM 2016's player
    // view path checks the same native CVar before applying its independent
    // first-person kick transform, so isolate that remaining VR-incompatible
    // motion without touching damage or physical knockback.
    auto gate = image + 0xE489B7;
    constexpr unsigned char signature[]{0x83,0x3D,0xC2,0x62,0xD1,0x04,0x00};
    if (std::memcmp(gate, signature, sizeof(signature))) {
        log("g_weaponKick gate signature mismatch at RVA 0xE489B7; suppression disabled");
        return false;
    }

    auto value = reinterpret_cast<volatile LONG*>(image + 0x5B5EC80);
    const LONG current = InterlockedCompareExchange(value, 0, 0);
    if (current != 0 && current != 1) {
        log("g_weaponKick has an unexpected initial value; suppression disabled");
        return false;
    }
    weaponKickEnable = value;
    setWeaponKickSuppressed(true);
    log("6DoF native weapon-kick suppression armed session-wide; gate RVA 0xE489B7, CVar RVA 0x5B5EC80");
    return true;
}

void setWeaponKickSuppressed(bool suppress) {
    if (!weaponKickEnable) return;

    bool changed = false;
    LONG restoredValue = 0;
    AcquireSRWLockExclusive(&weaponKickLock);
    if (suppress) {
        if (!weaponKickSuppressed) {
            weaponKickPreviousValue = InterlockedExchange(weaponKickEnable, 0);
            weaponKickSuppressed = true;
            changed = true;
        } else if (InterlockedCompareExchange(weaponKickEnable, 0, 0) != 0) {
            InterlockedExchange(weaponKickEnable, 0);
        }
    } else if (weaponKickSuppressed) {
        restoredValue = weaponKickPreviousValue;
        InterlockedExchange(weaponKickEnable, restoredValue);
        weaponKickSuppressed = false;
        changed = true;
    }
    ReleaseSRWLockExclusive(&weaponKickLock);

    if (changed) {
        if (suppress) {
            log("6DoF native g_weaponKick SUPPRESSED SESSION-WIDE");
        } else {
            log("native g_weaponKick restored to previous value=" +
                std::to_string(restoredValue));
        }
    }
}

void setMuzzleFireAxisOverride(bool enabled) {
    if (!muzzleFireAxisBranch || muzzleFireAxisOverride.load(std::memory_order_acquire) == enabled) return;
    constexpr unsigned char original[]{0x0F,0x85,0x6A,0x06,0x00,0x00};
    // NOP + JMP preserves the original six-byte footprint and displacement.
    constexpr unsigned char forced[]{0x90,0xE9,0x6A,0x06,0x00,0x00};
    DWORD oldProtect{};
    if (!VirtualProtect(muzzleFireAxisBranch, sizeof(original), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        log("useMuzzleAsFireAxis branch protection failed");
        return;
    }
    std::memcpy(muzzleFireAxisBranch, enabled ? forced : original, sizeof(original));
    FlushInstructionCache(GetCurrentProcess(), muzzleFireAxisBranch, sizeof(original));
    DWORD ignored{};
    VirtualProtect(muzzleFireAxisBranch, sizeof(original), oldProtect, &ignored);
    muzzleFireAxisOverride.store(enabled, std::memory_order_release);
    log(std::string("VR shot direction -> native muzzle origin/axis ") + (enabled ? "ENABLED" : "disabled"));
}

float configuredWeaponScale() {
    static const float scale = [] {
        // Steam may already be running, in which case environment variables
        // set by the launcher are not inherited by the game process.  Read the
        // short-lived launch config first; keep the environment variable as a
        // fallback for direct launches and development tools.
        std::ifstream config(kharvox::runtimePathA("weapon_scale.cfg"));
        float configuredValue{};
        if (config >> configuredValue) return std::clamp(configuredValue, 0.2f, 1.5f);

        char text[64]{};
        if (GetEnvironmentVariableA("KHARVOX_WEAPON_SCALE", text, sizeof(text)) > 0) {
            char* end{};
            const float value = std::strtof(text, &end);
            if (end != text) return std::clamp(value, 0.2f, 1.5f);
        }
        return 1.0f;
    }();
    return scale;
}

std::array<float, 3> configuredWeaponPivotAdjustment() {
    static const std::array<float, 3> pivot = [] {
        std::array<float, 3> value{};
        std::ifstream config(kharvox::runtimePathA("weapon_pivot.cfg"));
        if (config >> value[0] >> value[1] >> value[2]) return value;

        return std::array<float, 3>{0.0f, 0.0f, 0.0f};
    }();
    return pivot;
}

float loadConfiguredWeaponYawOffsetDegrees() {
    static const float yaw = [] {
        std::ifstream config(kharvox::runtimePathA("weapon_yaw.cfg"));
        float value{};
        if (config >> value) return std::clamp(value, -180.0f, 180.0f);
        return 25.0f;
    }();
    return yaw;
}

std::atomic<float> weaponYawOffsetDegrees{loadConfiguredWeaponYawOffsetDegrees()};

float currentWeaponYawOffsetDegrees() {
    return weaponYawOffsetDegrees.load(std::memory_order_relaxed);
}

float loadConfiguredWeaponRollOffsetDegrees() {
    static const float roll = [] {
        std::ifstream config(kharvox::runtimePathA("weapon_roll.cfg"));
        float value{};
        if (config >> value) return std::clamp(value, -180.0f, 180.0f);
        return 0.0f;
    }();
    return roll;
}

std::atomic<float> weaponRollOffsetDegrees{loadConfiguredWeaponRollOffsetDegrees()};

float currentWeaponRollOffsetDegrees() {
    return weaponRollOffsetDegrees.load(std::memory_order_relaxed);
}

float loadConfiguredWeaponPitchOffsetDegrees() {
    static const float pitch = [] {
        std::ifstream config(kharvox::runtimePathA("weapon_pitch.cfg"));
        float value{};
        if (config >> value) return std::clamp(value, -180.0f, 180.0f);
        return 0.0f;
    }();
    return pitch;
}

std::atomic<float> weaponPitchOffsetDegrees{loadConfiguredWeaponPitchOffsetDegrees()};

float currentWeaponPitchOffsetDegrees() {
    return weaponPitchOffsetDegrees.load(std::memory_order_relaxed);
}

void pollWeaponRotationCalibration() {
    static bool f5WasDown{}, f6WasDown{}, f7WasDown{};
    static bool f9WasDown{}, f10WasDown{}, f11WasDown{}, f12WasDown{};
    const bool f5Down = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
    const bool f6Down = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
    const bool f9Down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    const bool f10Down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    const bool f11Down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    const bool f12Down = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
    const bool f7Down = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    if (f5Down && !f5WasDown) {
        const float value = std::clamp(currentWeaponPitchOffsetDegrees() - 1.0f, -180.0f, 180.0f);
        weaponPitchOffsetDegrees.store(value, std::memory_order_relaxed);
        log("weapon pitch calibration F5 -> " + std::to_string(value) + " degrees");
    }
    if (f6Down && !f6WasDown) {
        const float value = std::clamp(currentWeaponPitchOffsetDegrees() + 1.0f, -180.0f, 180.0f);
        weaponPitchOffsetDegrees.store(value, std::memory_order_relaxed);
        log("weapon pitch calibration F6 -> " + std::to_string(value) + " degrees");
    }
    if (f9Down && !f9WasDown) {
        const float value = std::clamp(currentWeaponYawOffsetDegrees() - 1.0f, -180.0f, 180.0f);
        weaponYawOffsetDegrees.store(value, std::memory_order_relaxed);
        log("weapon yaw calibration F9 -> " + std::to_string(value) + " degrees");
    }
    if (f10Down && !f10WasDown) {
        const float value = std::clamp(currentWeaponYawOffsetDegrees() + 1.0f, -180.0f, 180.0f);
        weaponYawOffsetDegrees.store(value, std::memory_order_relaxed);
        log("weapon yaw calibration F10 -> " + std::to_string(value) + " degrees");
    }
    if (f11Down && !f11WasDown) {
        const float value = std::clamp(currentWeaponRollOffsetDegrees() - 1.0f, -180.0f, 180.0f);
        weaponRollOffsetDegrees.store(value, std::memory_order_relaxed);
        log("weapon roll calibration F11 -> " + std::to_string(value) + " degrees");
    }
    if (f12Down && !f12WasDown) {
        const float value = std::clamp(currentWeaponRollOffsetDegrees() + 1.0f, -180.0f, 180.0f);
        weaponRollOffsetDegrees.store(value, std::memory_order_relaxed);
        log("weapon roll calibration F12 -> " + std::to_string(value) + " degrees");
    }
    if (f7Down && !f7WasDown) {
        const float yaw = currentWeaponYawOffsetDegrees();
        const float roll = currentWeaponRollOffsetDegrees();
        const float pitch = currentWeaponPitchOffsetDegrees();
        std::ofstream(kharvox::runtimePathA("weapon_yaw_saved.cfg"), std::ios::trunc) << yaw << '\n';
        std::ofstream(kharvox::runtimePathA("weapon_roll_saved.cfg"), std::ios::trunc) << roll << '\n';
        std::ofstream(kharvox::runtimePathA("weapon_pitch_saved.cfg"), std::ios::trunc) << pitch << '\n';
        log("weapon rotation calibration CONFIRMED with F7 -> yaw=" + std::to_string(yaw) +
            " roll=" + std::to_string(roll) + " pitch=" + std::to_string(pitch) + " degrees");
    }
    f5WasDown = f5Down;
    f6WasDown = f6Down;
    f9WasDown = f9Down;
    f10WasDown = f10Down;
    f11WasDown = f11Down;
    f12WasDown = f12Down;
    f7WasDown = f7Down;
}

void captureAnimatedGripPivot(void* hands) {
    if (!controllerPlacementActive()) return;
    if (!hands || !getHandsModel || !getJointTransform || !readableRange(static_cast<unsigned char*>(hands) + 0x5B1A, sizeof(uint16_t))) return;
    uint16_t joint{};
    std::memcpy(&joint, static_cast<unsigned char*>(hands) + 0x5B1A, sizeof(joint));
    if (joint == 0xFFFF) return;
    void* model = getHandsModel(hands);
    if (!model) return;
    float origin[3]{}, axis[9]{};
    if (!getJointTransform(model, 1, joint, origin, axis)) return;
    for (float component : origin)
        if (!std::isfinite(component) || std::fabs(component) > 500.0f) return;

    const unsigned generation = pose.resetGeneration.load(std::memory_order_acquire);
    if (neutralGripPivotGeneration.load(std::memory_order_acquire) != generation) {
        neutralGripPivotValid.store(false, std::memory_order_release);
        neutralGripPivotGeneration.store(generation, std::memory_order_release);
        neutralGripPivotWarmup = 0;
        neutralGripPivotSamples = 0;
        neutralGripPivotSum.fill(0.0f);
    }
    // Capture a short settled neutral window after gameplay/cutscene entry.
    // This becomes the LateUpdate reference used to remove only global
    // first-person pose translation from attached weapon render entities.
    if (!neutralGripPivotValid.load(std::memory_order_acquire)) {
        if (neutralGripPivotWarmup < 45) {
            ++neutralGripPivotWarmup;
        } else if (neutralGripPivotSamples < 30) {
            for (int index = 0; index < 3; ++index)
                neutralGripPivotSum[index] += origin[index];
            ++neutralGripPivotSamples;
            if (neutralGripPivotSamples == 30) {
                for (int index = 0; index < 3; ++index)
                    neutralGripPivot[index].store(neutralGripPivotSum[index] / 30.0f,
                                                  std::memory_order_relaxed);
                neutralGripPivotValid.store(true, std::memory_order_release);
                std::ostringstream neutral;
                neutral << "weapon LateUpdate neutral pivot generation=" << generation << " pivot="
                        << neutralGripPivot[0].load(std::memory_order_relaxed) << ','
                        << neutralGripPivot[1].load(std::memory_order_relaxed) << ','
                        << neutralGripPivot[2].load(std::memory_order_relaxed);
                log(neutral.str());
            }
        }
    }
    for (int index = 0; index < 3; ++index)
        animatedGripPivot[index].store(origin[index], std::memory_order_relaxed);
    animatedGripPivotValid.store(true, std::memory_order_release);
    jointScanLogged.store(true, std::memory_order_release);
}

void hideAnimatedPlayerRootModel(void* hands) {
    if (!controllerPlacementActive() || !hands || !getHandsModel ||
        !hideRenderModelMesh) return;
    void* model = getHandsModel(hands);
    if (!model) return;

    // fp_hands contains the arms plus three remaining player-body surfaces.
    // Weapons are independent anim-web models, so clearing every visibility
    // bit on this root preserves the weapon, animator, joints and the
    // righthandattach pivot used by the 6DoF transform.
    unsigned long long visibilityBefore{};
    auto visibilityAddress = static_cast<unsigned char*>(model) + 0xEE8;
    if (!readableRange(visibilityAddress, sizeof(visibilityBefore))) return;
    std::memcpy(&visibilityBefore, visibilityAddress, sizeof(visibilityBefore));
    auto remaining = visibilityBefore;
    while (remaining) {
        unsigned long surfaceIndex{};
        _BitScanForward64(&surfaceIndex, remaining);
        hideRenderModelMesh(model, static_cast<int>(surfaceIndex));
        remaining &= remaining - 1;
    }
    if (!playerRootHiddenLogged.exchange(true, std::memory_order_acq_rel)) {
        unsigned long long visibilityAfter{};
        std::memcpy(&visibilityAfter, visibilityAddress, sizeof(visibilityAfter));
        std::ostringstream out;
        out << "weapon-only fp_hands root fully hidden model=0x" << std::hex
            << reinterpret_cast<uintptr_t>(model)
            << " visibilityMask=0x" << std::hex << visibilityBefore
            << "->0x" << visibilityAfter
            << " using native per-surface mask clears; weapon props remain visible";
        log(out.str());
    }
}

float configuredHandsProjectionScale() {
    static const float scale = [] {
        std::ifstream config(kharvox::runtimePathA("hands_projection_scale.cfg"));
        float configuredValue{};
        if (config >> configuredValue) return std::clamp(configuredValue, 0.2f, 1.5f);

        char text[64]{};
        if (GetEnvironmentVariableA("KHARVOX_HANDS_PROJECTION_SCALE", text, sizeof(text)) > 0) {
            char* end{};
            const float value = std::strtof(text, &end);
            if (end != text) return std::clamp(value, 0.2f, 1.5f);
        }
        return 1.0f;
    }();
    return scale;
}

void log(const std::string& text) {
    if (!kharvox::extendedDiagnosticsEnabled()) return;
    char temp[MAX_PATH]{};
    GetTempPathA(MAX_PATH, temp);
    std::ofstream out(std::string(temp) + "KHARVOX.log", std::ios::app);
    out << "[KHARVOX][WEAPON] " << text << '\n';
}

void emit8(unsigned char*& cursor, unsigned char value) { *cursor++ = value; }

void emit64(unsigned char*& cursor, unsigned long long value) {
    std::memcpy(cursor, &value, sizeof(value));
    cursor += sizeof(value);
}

bool readableRange(const void* address, size_t bytes) {
    if (!address || !bytes) return false;
    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQuery(address, &memory, sizeof(memory)) || memory.State != MEM_COMMIT) return false;
    if ((memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    const auto begin = reinterpret_cast<uintptr_t>(address);
    const auto regionEnd = reinterpret_cast<uintptr_t>(memory.BaseAddress) + memory.RegionSize;
    return begin <= regionEnd && bytes <= regionEnd - begin;
}

bool writableRange(void* address, size_t bytes) {
    if (!address || !bytes) return false;
    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQuery(address, &memory, sizeof(memory)) || memory.State != MEM_COMMIT) return false;
    if ((memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    constexpr DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY
        | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((memory.Protect & writable) == 0) return false;
    const auto begin = reinterpret_cast<uintptr_t>(address);
    const auto regionEnd = reinterpret_cast<uintptr_t>(memory.BaseAddress) + memory.RegionSize;
    return begin <= regionEnd && bytes <= regionEnd - begin;
}

struct WeaponIdentityMatch {
    KharvoxWeaponKind kind{KharvoxWeaponKind::Unknown};
    int score{};
};

WeaponIdentityMatch weaponIdentityMatch(const std::string& value) {
    std::string lower;
    lower.reserve(value.size());
    for (const unsigned char character : value)
        lower.push_back(static_cast<char>(std::tolower(character)));

    const auto contains = [&](const char* text) { return lower.find(text) != std::string::npos; };
    if (contains("weapon/zion/player/sp/pistol")) return {KharvoxWeaponKind::Pistol, 100};
    if (contains("weapon/zion/player/sp/shotgun")) return {KharvoxWeaponKind::Shotgun, 100};
    if (contains("weapon/zion/player/sp/heavy_rifle_heavy_zoom")) return {KharvoxWeaponKind::HeavyAssaultRifle, 100};
    if (contains("weapon/zion/player/sp/heavy_rifle_heavy_ar")) return {KharvoxWeaponKind::HeavyAssaultRifle, 100};
    if (contains("weapon/zion/player/sp/plasma_rifle")) return {KharvoxWeaponKind::PlasmaRifle, 100};
    if (contains("weapon/zion/player/sp/rocket_launcher")) return {KharvoxWeaponKind::RocketLauncher, 100};
    if (contains("weapon/zion/player/sp/double_barrel")) return {KharvoxWeaponKind::SuperShotgun, 100};
    if (contains("weapon/zion/player/sp/gauss_rifle")) return {KharvoxWeaponKind::GaussCannon, 100};
    if (contains("weapon/zion/player/sp/chaingun")) return {KharvoxWeaponKind::Chaingun, 100};
    if (contains("weapon/zion/player/sp/bfg")) return {KharvoxWeaponKind::Bfg, 100};
    if (contains("weapon/zion/player/sp/chainsaw")) return {KharvoxWeaponKind::Chainsaw, 100};
    if (contains("weapon/zion/player/sp/fists")) return {KharvoxWeaponKind::Fists, 100};
    if (contains("weapon/zion/player/sp/assault_rifle_assaultrifle")) return {KharvoxWeaponKind::AssaultRifle, 100};
    if (contains("weapon/zion/player/sp/arc_cannon")) return {KharvoxWeaponKind::ArcCannon, 100};
    if (contains("weapon/zion/player/sp/mancubus_gland")) return {KharvoxWeaponKind::MancubusGland, 100};

    if (lower.find("weapons/pistols/pistol") != std::string::npos ||
        lower.find("zion_weapon_pistol") != std::string::npos) return {KharvoxWeaponKind::Pistol, 50};
    if (lower.find("weapons/shotgun") != std::string::npos ||
        lower.find("zion_weapon_shotgun") != std::string::npos) return {KharvoxWeaponKind::Shotgun, 50};
    if (contains("zion_weapon_heavy") || contains("weapons/heavy_rifle")) return {KharvoxWeaponKind::HeavyAssaultRifle, 50};
    if (contains("zion_weapon_plasma") || contains("weapons/plasma")) return {KharvoxWeaponKind::PlasmaRifle, 50};
    if (contains("zion_weapon_rocket") || contains("weapons/rocket_launcher")) return {KharvoxWeaponKind::RocketLauncher, 50};
    if (contains("zion_weapon_double_barrel") || contains("weapons/double_barrel")) return {KharvoxWeaponKind::SuperShotgun, 50};
    if (contains("zion_weapon_gauss") || contains("weapons/gauss")) return {KharvoxWeaponKind::GaussCannon, 50};
    if (contains("zion_weapon_chaingun") || contains("weapons/chaingun")) return {KharvoxWeaponKind::Chaingun, 50};
    if (contains("zion_weapon_bfg") || contains("weapons/bfg")) return {KharvoxWeaponKind::Bfg, 50};
    if (contains("zion_weapon_chainsaw") || contains("weapons/chainsaw")) return {KharvoxWeaponKind::Chainsaw, 50};
    if (contains("zion_weapon_fists") || contains("weapons/fists")) return {KharvoxWeaponKind::Fists, 50};
    if (contains("zion_weapon_assault") || contains("weapons/assault_rifle")) return {KharvoxWeaponKind::AssaultRifle, 50};
    if (contains("zion_weapon_arc_cannon") || contains("weapons/arc_cannon")) return {KharvoxWeaponKind::ArcCannon, 50};
    if (contains("zion_weapon_mancubus") || contains("weapons/mancubus_gland")) return {KharvoxWeaponKind::MancubusGland, 50};
    return {};
}

bool readableAsciiString(uintptr_t address, std::string& value) {
    value.clear();
    if (address < 0x10000) return false;
    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory)) ||
        memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    const auto regionEnd = reinterpret_cast<uintptr_t>(memory.BaseAddress) + memory.RegionSize;
    const size_t available = static_cast<size_t>(regionEnd - address);
    const size_t limit = std::min<size_t>(available, 240);
    const auto text = reinterpret_cast<const unsigned char*>(address);
    for (size_t index = 0; index < limit; ++index) {
        const unsigned char character = text[index];
        if (!character) return value.size() >= 6;
        if (character < 0x20 || character > 0x7e) return false;
        value.push_back(static_cast<char>(character));
    }
    return false;
}

bool queryPlayerInventorySafely(void* player, void*& inventory, int& count) {
    inventory = nullptr;
    count = 0;
    if (!player || !inventoryCountNative || !readableRange(player, sizeof(void*))) return false;
    __try {
        auto vtable = *reinterpret_cast<void***>(player);
        if (!vtable || !readableRange(vtable, 0x668)) return false;
        auto getInventory = reinterpret_cast<void*(__fastcall*)(void*)>(
            vtable[0x660 / sizeof(void*)]);
        if (!getInventory) return false;
        inventory = getInventory(player);
        if (!inventory || !readableRange(inventory, 0x10)) return false;
        count = inventoryCountNative(inventory);
        return count >= 0 && count <= 256;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        inventory = nullptr;
        count = 0;
        return false;
    }
}

bool queryInventoryItemSafely(void* inventory, int index, void*& item) {
    item = nullptr;
    if (!inventory || !inventoryItemNative) return false;
    __try {
        item = inventoryItemNative(inventory, index);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        item = nullptr;
        return false;
    }
}

bool queryWeaponAmmoTotalSafely(void* item, void* inventory, int& total) {
    total = 0;
    if (!item || !inventory || !weaponAmmoTotalNative) return false;
    __try {
        total = weaponAmmoTotalNative(item, inventory);
        return total >= 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        total = 0;
        return false;
    }
}

bool shoulderSelectableAmmoKind(KharvoxWeaponKind kind) {
    switch (kind) {
    case KharvoxWeaponKind::Pistol:
    case KharvoxWeaponKind::Shotgun:
    case KharvoxWeaponKind::HeavyAssaultRifle:
    case KharvoxWeaponKind::PlasmaRifle:
    case KharvoxWeaponKind::RocketLauncher:
    case KharvoxWeaponKind::SuperShotgun:
    case KharvoxWeaponKind::GaussCannon:
    case KharvoxWeaponKind::Chaingun:
    case KharvoxWeaponKind::Bfg:
    case KharvoxWeaponKind::Chainsaw:
        return true;
    default:
        return false;
    }
}

const char* weaponAmmoStateName(KharvoxWeaponAmmoState state) {
    switch (state) {
    case KharvoxWeaponAmmoState::Unavailable: return "unavailable";
    case KharvoxWeaponAmmoState::Empty: return "empty";
    case KharvoxWeaponAmmoState::Usable: return "usable";
    default: return "unknown";
    }
}

bool installWeaponAmmoSnapshot(unsigned char* image) {
    auto countTarget = image + 0x2E0CD0;
    auto itemTarget = image + 0xEEDC50;
    auto totalTarget = image + 0xF24BD0;
    constexpr unsigned char countSignature[]{0x8B,0x41,0x08,0xC3};
    constexpr unsigned char itemSignature[]{
        0x48,0x83,0xEC,0x28,0x85,0xD2,0x78,0x4F,
        0x3B,0x51,0x08,0x7D,0x4A,0x48,0x89,0x5C,0x24,0x20
    };
    constexpr unsigned char totalSignature[]{
        0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,
        0x48,0x8B,0x01,0x48,0x8B,0xDA,0x8B,0x91,0xD4,0x08,0x00,0x00,
        0xFF,0x90,0xF0,0x03,0x00,0x00
    };
    if (std::memcmp(countTarget, countSignature, sizeof(countSignature))
        || std::memcmp(itemTarget, itemSignature, sizeof(itemSignature))
        || std::memcmp(totalTarget, totalSignature, sizeof(totalSignature))) {
        log("native inventory/ammo signature mismatch; Shoulder Weapon keeps timeout-only fallback");
        return false;
    }
    inventoryCountNative = reinterpret_cast<InventoryCountFn>(countTarget);
    inventoryItemNative = reinterpret_cast<InventoryItemFn>(itemTarget);
    weaponAmmoTotalNative = reinterpret_cast<WeaponAmmoTotalFn>(totalTarget);
    weaponAmmoSnapshotSupported.store(true, std::memory_order_release);
    log("read-only native Shoulder Weapon ammo snapshot armed at RVAs 0x2E0CD0/0xEEDC50/0xF24BD0");
    return true;
}

void scoreInlineWeaponStrings(const unsigned char* bytes, size_t byteCount,
                              WeaponIdentityMatch& bestMatch, std::string& bestText) {
    for (size_t start = 0; start < byteCount;) {
        while (start < byteCount && (bytes[start] < 0x20 || bytes[start] > 0x7e)) ++start;
        size_t end = start;
        while (end < byteCount && bytes[end] >= 0x20 && bytes[end] <= 0x7e) ++end;
        if (end - start >= 6) {
            const std::string candidate(reinterpret_cast<const char*>(bytes + start), end - start);
            const auto match = weaponIdentityMatch(candidate);
            if (match.score > bestMatch.score) {
                bestMatch = match;
                bestText = candidate;
            }
        }
        start = end + 1;
    }
}

void scoreWeaponObject(uintptr_t object, size_t requestedBytes, bool followPointers,
                       WeaponIdentityMatch& bestMatch, std::string& bestText) {
    if (!object || !readableRange(reinterpret_cast<void*>(object), requestedBytes)) return;
    const auto bytes = reinterpret_cast<const unsigned char*>(object);
    scoreInlineWeaponStrings(bytes, requestedBytes, bestMatch, bestText);
    if (bestMatch.score >= 100) return;

    unsigned followed = 0;
    for (size_t offset = 0; offset + sizeof(uintptr_t) <= requestedBytes; offset += sizeof(uintptr_t)) {
        uintptr_t pointer{};
        std::memcpy(&pointer, bytes + offset, sizeof(pointer));
        std::string candidate;
        if (readableAsciiString(pointer, candidate)) {
            const auto match = weaponIdentityMatch(candidate);
            if (match.score > bestMatch.score) {
                bestMatch = match;
                bestText = candidate;
                if (bestMatch.score >= 100) return;
            }
        }
        if (!followPointers || followed >= 64 || pointer < 0x10000 || (pointer & 0x7) ||
            !readableRange(reinterpret_cast<void*>(pointer), 0x400)) continue;
        ++followed;
        scoreWeaponObject(pointer, 0x400, false, bestMatch, bestText);
        if (bestMatch.score >= 100) return;
    }
}

KharvoxWeaponKind classifyWeaponData(void* weaponData, std::string& matchedText) {
    WeaponIdentityMatch match;
    scoreWeaponObject(reinterpret_cast<uintptr_t>(weaponData), 0xD00, true, match, matchedText);
    return match.kind;
}

enum class SniperPresentationKind {
    None,
    HeavyAssaultRifle,
    GaussCannon,
};

bool asciiEqualsInsensitive(const std::string& value, const char* token) {
    const auto tokenLength = std::strlen(token);
    if (!tokenLength || value.size() != tokenLength) return false;
    return std::equal(value.begin(), value.end(), token,
        [](unsigned char left, unsigned char right) {
            return std::tolower(left) == std::tolower(right);
        });
}

bool weaponObjectDirectlyNames(void* weaponData, const char* declName) {
    constexpr size_t weaponDeclScanBytes = 0x2000;
    if (!weaponData || !readableRange(weaponData, weaponDeclScanBytes)) return false;
    const auto bytes = static_cast<const unsigned char*>(weaponData);

    for (size_t start = 0; start < weaponDeclScanBytes;) {
        while (start < weaponDeclScanBytes && (bytes[start] < 0x20 || bytes[start] > 0x7e)) ++start;
        size_t end = start;
        while (end < weaponDeclScanBytes && bytes[end] >= 0x20 && bytes[end] <= 0x7e) ++end;
        if (end > start && asciiEqualsInsensitive(
                std::string(reinterpret_cast<const char*>(bytes + start), end - start), declName))
            return true;
        start = end + 1;
    }

    // Only inspect direct character pointers. Following idDecl pointers here
    // would make the base weapon look like its referenced secondary mod.
    for (size_t offset = 0; offset + sizeof(uintptr_t) <= weaponDeclScanBytes;
         offset += sizeof(uintptr_t)) {
        uintptr_t pointer{};
        std::memcpy(&pointer, bytes + offset, sizeof(pointer));
        std::string candidate;
        if (readableAsciiString(pointer, candidate) && asciiEqualsInsensitive(candidate, declName))
            return true;
    }
    return false;
}

SniperPresentationKind sniperPresentationKind(void* weaponData) {
    if (weaponObjectDirectlyNames(
            weaponData, "weapon/zion/player/sp/heavy_rifle_heavy_zoom"))
        return SniperPresentationKind::HeavyAssaultRifle;
    if (weaponObjectDirectlyNames(
            weaponData, "weapon/zion/player/sp/gauss_rifle_charged_sniper"))
        return SniperPresentationKind::GaussCannon;
    return SniperPresentationKind::None;
}

bool patchVrSniperPresentation(void* weaponData) {
    const auto kind = sniperPresentationKind(weaponData);
    if (kind == SniperPresentationKind::None) return false;

    // These offsets come from DOOMx64vk's idDeclWeapon reflection metadata.
    // The replacement values mirror the game's own co-op Gauss sniper setup:
    // retain the zoom/fire state, but do not hide/animate hands and do not
    // select a scope GUI or zoom reticle.
    constexpr size_t ironSightZoomOffset = 0xDE8;
    constexpr size_t zoomedFovOffset = ironSightZoomOffset + 0x00;
    constexpr size_t hideHandsOnZoomOffset = ironSightZoomOffset + 0x24;
    constexpr size_t hideHandsOnZoomDelayOffset = ironSightZoomOffset + 0x28;
    constexpr size_t scopeGuiNumOffset = ironSightZoomOffset + 0x30;
    constexpr size_t zoomModeOffset = ironSightZoomOffset + 0x48;
    constexpr size_t reticleWhenZoomedOffset = 0x12B8;
    constexpr int zoomNone = 0;
    constexpr int zoomWeapon = 1;
    constexpr int zoomWeaponNoHandAnim = 2;
    constexpr int scopeNone = 0;

    auto bytes = static_cast<unsigned char*>(weaponData);
    if (!readableRange(bytes, reticleWhenZoomedOffset + sizeof(uintptr_t))) {
        log("VR sniper presentation skipped: idDeclWeapon range is unreadable");
        return true;
    }

    float zoomedFov{};
    unsigned char hideHandsOnZoom{};
    int hideHandsOnZoomDelay{};
    int scopeGuiNum{};
    int zoomMode{};
    uintptr_t reticleWhenZoomed{};
    std::memcpy(&zoomedFov, bytes + zoomedFovOffset, sizeof(zoomedFov));
    std::memcpy(&hideHandsOnZoom, bytes + hideHandsOnZoomOffset, sizeof(hideHandsOnZoom));
    std::memcpy(&hideHandsOnZoomDelay, bytes + hideHandsOnZoomDelayOffset,
                sizeof(hideHandsOnZoomDelay));
    std::memcpy(&scopeGuiNum, bytes + scopeGuiNumOffset, sizeof(scopeGuiNum));
    std::memcpy(&zoomMode, bytes + zoomModeOffset, sizeof(zoomMode));
    std::memcpy(&reticleWhenZoomed, bytes + reticleWhenZoomedOffset, sizeof(reticleWhenZoomed));

    const float expectedFov = kind == SniperPresentationKind::HeavyAssaultRifle ? 45.0f : 50.0f;
    if (!std::isfinite(zoomedFov) || std::fabs(zoomedFov - expectedFov) > 0.01f ||
        (zoomMode != zoomNone && zoomMode != zoomWeapon &&
         zoomMode != zoomWeaponNoHandAnim) ||
        hideHandsOnZoom > 1 || hideHandsOnZoomDelay < 0 || hideHandsOnZoomDelay > 10000 ||
        scopeGuiNum < 0 || scopeGuiNum > 8) {
        std::ostringstream out;
        out << "VR sniper presentation skipped: idDeclWeapon invariants failed"
            << " fov=" << zoomedFov << " mode=" << zoomMode
            << " hide=" << static_cast<unsigned>(hideHandsOnZoom)
            << " hideDelay=" << hideHandsOnZoomDelay << " scope=" << scopeGuiNum
            << " zoomReticle=0x" << std::hex << reticleWhenZoomed
            << " data=0x" << reinterpret_cast<uintptr_t>(weaponData);
        log(out.str());
        return true;
    }

    // A mode-0 object is the engine's cached/runtime copy seen on the first
    // activation after a load. Preserve that state: changing it to a weapon
    // zoom mode would alter gameplay. Its copied presentation fields still
    // need clearing before the HUD consumes the object.
    const int presentationZoomMode =
        zoomMode == zoomNone ? zoomNone : zoomWeaponNoHandAnim;
    const bool weaponChanged = zoomMode != presentationZoomMode || hideHandsOnZoom != 0 ||
        hideHandsOnZoomDelay != 0 || scopeGuiNum != scopeNone || reticleWhenZoomed != 0;
    if (weaponChanged) {
        DWORD oldProtect{};
        if (!VirtualProtect(bytes + hideHandsOnZoomOffset,
                            reticleWhenZoomedOffset + sizeof(uintptr_t) - hideHandsOnZoomOffset,
                            PAGE_READWRITE, &oldProtect)) {
            log("VR sniper presentation skipped: idDeclWeapon protection change failed");
            return true;
        }
        const unsigned char keepHandsVisible = 0;
        const int noHideDelay = 0;
        const uintptr_t noZoomReticle{};
        std::memcpy(bytes + hideHandsOnZoomOffset, &keepHandsVisible, sizeof(keepHandsVisible));
        std::memcpy(bytes + hideHandsOnZoomDelayOffset, &noHideDelay, sizeof(noHideDelay));
        std::memcpy(bytes + scopeGuiNumOffset, &scopeNone, sizeof(scopeNone));
        std::memcpy(bytes + zoomModeOffset, &presentationZoomMode,
                    sizeof(presentationZoomMode));
        std::memcpy(bytes + reticleWhenZoomedOffset, &noZoomReticle, sizeof(noZoomReticle));
        DWORD ignored{};
        VirtualProtect(bytes + hideHandsOnZoomOffset,
                       reticleWhenZoomedOffset + sizeof(uintptr_t) - hideHandsOnZoomOffset,
                       oldProtect, &ignored);
    }

    if (weaponChanged) {
        std::ostringstream out;
        out << "VR sniper presentation prepared for "
            << (kind == SniperPresentationKind::HeavyAssaultRifle
                    ? "Heavy Assault Rifle Precision Bolt" : "Gauss Cannon Precision Bolt")
            << ": zoomMode " << zoomMode << "->" << presentationZoomMode
            << ", hideHands " << static_cast<unsigned>(hideHandsOnZoom) << "->0"
            << ", hideDelay " << hideHandsOnZoomDelay << "->0"
            << ", scopeGui " << scopeGuiNum << "->" << scopeNone
            << ", zoomReticle 0x" << std::hex << reticleWhenZoomed << "->0"
            << ", data=0x" << reinterpret_cast<uintptr_t>(weaponData);
        log(out.str());
    }
    return true;
}

void observeActiveWeaponData(void* weaponData) {
    const auto address = reinterpret_cast<uintptr_t>(weaponData);
    const bool changed = activeWeaponData.exchange(address, std::memory_order_acq_rel) != address;
    if (!changed) {
        if (activeWeaponKind.load(std::memory_order_acquire) != KharvoxWeaponKind::Unknown) return;
        if ((unknownWeaponRescanCalls.fetch_add(1, std::memory_order_relaxed) % 120) != 119) return;
    } else {
        unknownWeaponRescanCalls.store(0, std::memory_order_relaxed);
        invalidateAerWeaponPairCache();
    }
    if (changed) patchVrSniperPresentation(weaponData);
    std::string matchedText;
    const auto kind = classifyWeaponData(weaponData, matchedText);
    activeWeaponKind.store(kind, std::memory_order_release);
    if (!changed && kind == KharvoxWeaponKind::Unknown) return;
    std::ostringstream out;
    out << "active native weapon decl -> " << KharvoxWeaponKindDisplayName(kind)
        << " [" << KharvoxWeaponKindKey(kind) << "] data=0x" << std::hex << address;
    if (!matchedText.empty()) out << " identity=\"" << matchedText << '"';
    log(out.str());
}

void* resolveActiveWeaponDataNative(void* owner, int fireMode) {
    auto bytes = static_cast<unsigned char*>(owner);
    int resolvedMode = fireMode;
    if (resolvedMode == -1) std::memcpy(&resolvedMode, bytes + 0x8D4, sizeof(resolvedMode));
    void* defaultWeapon{};
    std::memcpy(&defaultWeapon, bytes + 0x30, sizeof(defaultWeapon));
    void* selected{};
    const auto selectedOffset = static_cast<ptrdiff_t>(resolvedMode) * 0x190 + 0x1898;
    std::memcpy(&selected, bytes + selectedOffset, sizeof(selected));
    if (selected) return selected;
    if (resolvedMode == 1 && defaultWeapon) {
        std::memcpy(&selected, static_cast<unsigned char*>(defaultWeapon) + 0x480, sizeof(selected));
        return selected;
    }
    return defaultWeapon;
}

void prepareReachableVrSniperDecls(void* owner, void* activeDecl) {
    if (!owner || !readableRange(owner, 0x1BC0)) return;
    auto ownerBytes = static_cast<unsigned char*>(owner);
    std::array<void*, 6> candidates{};
    size_t candidateCount = 0;
    candidates[candidateCount++] = activeDecl;

    void* defaultWeapon{};
    std::memcpy(&defaultWeapon, ownerBytes + 0x30, sizeof(defaultWeapon));
    candidates[candidateCount++] = defaultWeapon;
    if (defaultWeapon && readableRange(defaultWeapon, 0x488)) {
        void* secondaryWeapon{};
        std::memcpy(&secondaryWeapon, static_cast<unsigned char*>(defaultWeapon) + 0x480,
                    sizeof(secondaryWeapon));
        candidates[candidateCount++] = secondaryWeapon;
    }

    // idHands caches the currently resolved primary/secondary weapon decls in
    // 0x190-byte fire-mode slots. Preparing all populated slots on equip keeps
    // the flat scope state from being consumed by the first mod-button edge.
    for (int fireMode = 0; fireMode < 2; ++fireMode) {
        void* selected{};
        const auto offset = static_cast<ptrdiff_t>(fireMode) * 0x190 + 0x1898;
        std::memcpy(&selected, ownerBytes + offset, sizeof(selected));
        candidates[candidateCount++] = selected;
    }

    for (size_t index = 0; index < candidateCount; ++index) {
        if (!candidates[index]) continue;
        bool duplicate = false;
        for (size_t previous = 0; previous < index; ++previous)
            duplicate |= candidates[previous] == candidates[index];
        if (!duplicate) patchVrSniperPresentation(candidates[index]);
    }
}

extern "C" void* __fastcall activeWeaponDataHook(void* owner, int fireMode) {
    void* result = resolveActiveWeaponDataNative(owner, fireMode);
    const auto image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto returnAddress = reinterpret_cast<uintptr_t>(_ReturnAddress());
    if (image && returnAddress >= image && returnAddress - image == 0xD7D6C2) {
        if (activeWeaponData.load(std::memory_order_acquire) != reinterpret_cast<uintptr_t>(result))
            prepareReachableVrSniperDecls(owner, result);
        observeActiveWeaponData(result);
    }
    return result;
}

bool installActiveWeaponIdentityHook(unsigned char* image) {
    auto target = image + 0xF137B0;
    constexpr unsigned char signature[] = {
        0x44,0x8B,0xCA,0x83,0xFA,0xFF,0x75,0x07,
        0x44,0x8B,0x89,0xD4,0x08,0x00,0x00
    };
    if (std::memcmp(target, signature, sizeof(signature))) {
        log("RVA 0xF137B0 active weapon-data signature mismatch; two-hand identity stays UNKNOWN/einhändig");
        return false;
    }
    DWORD oldProtect{};
    if (!VirtualProtect(target, sizeof(signature), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        log("active weapon-data target protection failed; two-hand identity stays UNKNOWN/einhändig");
        return false;
    }
    unsigned char jump[sizeof(signature)]{0xFF,0x25,0,0,0,0};
    const auto wrapper = reinterpret_cast<unsigned long long>(activeWeaponDataHook);
    std::memcpy(jump + 6, &wrapper, sizeof(wrapper));
    jump[14] = 0x90;
    std::memcpy(target, jump, sizeof(jump));
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(jump));
    DWORD ignored{};
    VirtualProtect(target, sizeof(jump), oldProtect, &ignored);
    log("active weapon identity and VR sniper-presentation hook installed at RVA 0xF137B0; decl paths are caller-gated to idHands");
    return true;
}

float jointMatrixScore(const float* matrix) {
    float score = 0.0f;
    for (int row = 0; row < 3; ++row) {
        float lengthSquared = 0.0f;
        for (int column = 0; column < 3; ++column) {
            const float value = matrix[row * 4 + column];
            if (!std::isfinite(value) || std::fabs(value) > 16.0f) return 0.0f;
            lengthSquared += value * value;
        }
        const float translation = matrix[row * 4 + 3];
        if (!std::isfinite(translation) || std::fabs(translation) > 10000.0f) return 0.0f;
        if (lengthSquared > 0.2f && lengthSquared < 4.0f) score += 1.0f;
    }
    return score;
}

bool laserAllowedForWeapon(KharvoxWeaponKind kind) {
    return kind != KharvoxWeaponKind::Unknown
        && kind != KharvoxWeaponKind::Chainsaw
        && kind != KharvoxWeaponKind::Fists;
}

void invalidateLaserMuzzlePose() {
    std::lock_guard lock(laserSourceMutex);
    latestLaserSource={};
    laserSources={};
}

bool validPropJoint(const float origin[3], const float axis[9]) {
    for (int component = 0; component < 3; ++component)
        if (!std::isfinite(origin[component]) || std::fabs(origin[component]) > 300.0f)
            return false;
    for (int row = 0; row < 3; ++row) {
        float lengthSquared{};
        for (int column = 0; column < 3; ++column) {
            const float value = axis[row * 3 + column];
            if (!std::isfinite(value) || std::fabs(value) > 4.0f) return false;
            lengthSquared += value * value;
        }
        if (lengthSquared < 0.20f || lengthSquared > 4.0f) return false;
    }
    return true;
}

void publishPropLaserMuzzlePose(void* propEntity, const float* propOrigin,
                                const float* propAxis) {
    const auto kind = activeWeaponKind.load(std::memory_order_acquire);
    if (!laserAllowedForWeapon(kind) || !propEntity || !propOrigin || !propAxis
        || !getJointTransform) {
        invalidateLaserMuzzlePose();
        return;
    }

    const unsigned kindBit=1u<<static_cast<unsigned>(kind);
    std::array<float,3> localOrigin{},localDirection{};
    std::array<float,9> localAxis{};
    // The named accessor returns WORLD space using the entity's cached
    // +C8/+D4 transform. Undo that exact transform before applying the final
    // source-qualified prop transform below (the hook has not copied D4 yet).
    std::array<float,3> locatorWorld{},cachedOrigin{};
    std::array<float,9> locatorWorldAxis{},cachedAxis{};
    const auto bytes=static_cast<const unsigned char*>(propEntity);
    bool queried=false,nativeLocator=false;
    if(getNamedJointTransform&&readableRange(bytes+0xC8,12+36)){
        std::memcpy(cachedOrigin.data(),bytes+0xC8,12);
        std::memcpy(cachedAxis.data(),bytes+0xD4,36);
        queried=getNamedJointTransform(propEntity,1,"muzzle",locatorWorld.data(),locatorWorldAxis.data());
        nativeLocator=queried&&kharvox::laserWorldToLocal(locatorWorld.data(),locatorWorldAxis.data(),
            cachedOrigin.data(),cachedAxis.data(),localOrigin.data(),localAxis.data())
            &&validPropJoint(localOrigin.data(),localAxis.data());
    }
    if(nativeLocator){for(int c=0;c<3;++c)localDirection[c]=localAxis[c];}
    else{
        const auto seen=laserMuzzleFailureKindsLogged.fetch_or(kindBit,std::memory_order_relaxed);
        if(!(seen&kindBit))log(std::string("[LASER] native locator fallback weapon=")
            +KharvoxWeaponKindDisplayName(kind)+(queried?" reason=invalid-transform":" reason=lookup-unavailable"));
    thread_local uintptr_t jointEntity{};
    thread_local KharvoxWeaponKind jointKind{};
    thread_local unsigned stableJoint{};
    thread_local bool stableJointValid{};
    thread_local int stableConvention{},stableBasis{};
    thread_local float stableSign{1.f};
    const bool sameJoint=stableJointValid&&jointEntity==reinterpret_cast<uintptr_t>(propEntity)&&jointKind==kind;
    if (jointEntity != reinterpret_cast<uintptr_t>(propEntity) || jointKind != kind) {
        jointEntity=reinterpret_cast<uintptr_t>(propEntity);jointKind=kind;stableJointValid=false;
    }
    unsigned selectedJoint{};
    unsigned validJoints{};
    unsigned consecutiveInvalid{};
    float farthestDistanceSquared = 4.0f;
    for (unsigned joint = stableJointValid ? stableJoint : 0; joint < (stableJointValid ? stableJoint+1 : 64); ++joint) {
        float candidateOrigin[3]{}, candidateAxis[9]{};
        const bool valid = getJointTransform(
            propEntity, 1, joint, candidateOrigin, candidateAxis)
            && validPropJoint(candidateOrigin, candidateAxis);
        if (!valid) {
            if (validJoints && ++consecutiveInvalid >= 4) break;
            continue;
        }
        consecutiveInvalid = 0;
        ++validJoints;
        const float distanceSquared = candidateOrigin[0] * candidateOrigin[0]
            + candidateOrigin[1] * candidateOrigin[1]
            + candidateOrigin[2] * candidateOrigin[2];
        if (!std::isfinite(distanceSquared) || distanceSquared <= farthestDistanceSquared
            || distanceSquared >= 90000.0f) continue;
        farthestDistanceSquared = distanceSquared;
        selectedJoint = joint;
        std::copy(candidateOrigin, candidateOrigin + 3, localOrigin.begin());
        std::copy(candidateAxis, candidateAxis + 9, localAxis.begin());
    }

    if (farthestDistanceSquared <= 4.0f) {
        const unsigned failures = laserMuzzleFailureKindsLogged.fetch_or(kindBit, std::memory_order_relaxed);
        if ((failures & kindBit) == 0) {
            std::ostringstream out;
            out << "[LASER] weapon prop has no usable outer joint weapon="
                << KharvoxWeaponKindDisplayName(kind) << " validJoints=" << validJoints;
            log(out.str());
        }
        invalidateLaserMuzzlePose();
        return;
    }

    stableJoint=selectedJoint;stableJointValid=true;
    const float distance = std::sqrt(farthestDistanceSquared);
    const std::array<float, 3> radial{
        localOrigin[0] / distance, localOrigin[1] / distance, localOrigin[2] / distance};
    localDirection = radial;
    float bestAlignment{};
    // idMat3 convention differs between the public joint accessor and the
    // renderer's packed idJointMat. Test both row and column bases and keep the
    // one that points most closely from the weapon origin toward the selected
    // outer joint.
    for (int convention = 0; convention < 2; ++convention) {
        for (int basisIndex = 0; basisIndex < 3; ++basisIndex) {
            std::array<float, 3> basis{};
            for (int component = 0; component < 3; ++component)
                basis[component] = convention == 0
                    ? localAxis[basisIndex * 3 + component]
                    : localAxis[component * 3 + basisIndex];
            const float length = std::sqrt(basis[0] * basis[0]
                + basis[1] * basis[1] + basis[2] * basis[2]);
            if (!std::isfinite(length) || length < 0.25f) continue;
            for (float& value : basis) value /= length;
            const float signedAlignment = basis[0] * radial[0]
                + basis[1] * radial[1] + basis[2] * radial[2];
            const float alignment = std::fabs(signedAlignment);
            if (alignment > bestAlignment) {
                bestAlignment = alignment;
                if(!sameJoint){stableConvention=convention;stableBasis=basisIndex;stableSign=signedAlignment<0.f?-1.f:1.f;}
                if (signedAlignment < 0.0f)
                    for (float& value : basis) value = -value;
                localDirection = basis;
            }
        }
    }

    for(int c=0;c<3;++c)localDirection[c]=stableSign*localAxis[
        stableConvention==0?stableBasis*3+c:c*3+stableBasis];

    }

    std::array<float, 3> worldOrigin{};
    std::array<float, 3> worldDirection{};
    for (int component = 0; component < 3; ++component) {
        worldOrigin[component] = propOrigin[component]
            + localOrigin[0] * propAxis[component]
            + localOrigin[1] * propAxis[3 + component]
            + localOrigin[2] * propAxis[6 + component];
        worldDirection[component] = localDirection[0] * propAxis[component]
            + localDirection[1] * propAxis[3 + component]
            + localDirection[2] * propAxis[6 + component];
    }
    const float worldLength = std::sqrt(worldDirection[0] * worldDirection[0]
        + worldDirection[1] * worldDirection[1]
        + worldDirection[2] * worldDirection[2]);
    if (!std::isfinite(worldLength) || worldLength < 0.001f) {
        invalidateLaserMuzzlePose();
        return;
    }
    for (float& value : worldDirection) value /= worldLength;

    LaserSourceSnapshot snapshot{};
    snapshot.origin=worldOrigin;snapshot.direction=worldDirection;snapshot.kind=kind;
    snapshot.tick=GetTickCount64();snapshot.epoch=weaponSourceEpoch.load(std::memory_order_acquire);
    kharvox::AerSourceKey key{};
    if(KharvoxCameraUsesAerGameplaySource()) {
        if(!weaponRootSourceValid || !kharvox::aerWeaponPropSourceMatches(weaponRootSource,
            weaponRootSourcePresent,KharvoxCameraCurrentPresentSerial(),KharvoxCameraLevelTransitionGeneration(),
            snapshot.epoch,pose.resetGeneration.load(std::memory_order_acquire))) return;
        snapshot.bodyOrigin=weaponRootSource.camera.bodyOrigin;
        snapshot.bodyAxis=weaponRootSource.camera.bodyAxis;
        key=weaponRootSource.camera.key;
    } else if(!KharvoxCameraGetBodyPose(snapshot.bodyOrigin.data(),snapshot.bodyAxis.data())) return;
    {
        std::lock_guard lock(laserSourceMutex);
        latestLaserSource=snapshot;
        laserSources.remember(key,snapshot);
    }

    static std::atomic<unsigned> nativeLocatorKindsLogged{};
    if(nativeLocator&&!(nativeLocatorKindsLogged.fetch_or(kindBit,std::memory_order_relaxed)&kindBit))
        log(std::string("[LASER] native WORLD locator accepted weapon=")+KharvoxWeaponKindDisplayName(kind)
            +"; cached owner transform removed before source prop transform");
    const unsigned alreadyLogged = laserMuzzleKindsLogged.fetch_or(kindBit, std::memory_order_relaxed);
    if ((alreadyLogged & kindBit) == 0) {
        std::ostringstream out;
        out << "[LASER] weapon-prop muzzle acquired weapon="
            << KharvoxWeaponKindDisplayName(kind) << " source=" << (nativeLocator?"native-muzzle-world-corrected":"stable-joint-fallback")
            << " localOrigin=" << localOrigin[0] << ',' << localOrigin[1] << ',' << localOrigin[2]
            << " localDirection=" << localDirection[0] << ',' << localDirection[1] << ',' << localDirection[2]
            << " worldOrigin=" << worldOrigin[0] << ',' << worldOrigin[1] << ',' << worldOrigin[2]
            << " worldDirection=" << worldDirection[0] << ',' << worldDirection[1] << ',' << worldDirection[2];
        log(out.str());
    }
}

unsigned scanJointArraysInObject(const unsigned char* bytes, size_t objectBytes,
                                 const char* owner, uintptr_t ownerAddress) {
    if (!bytes || objectBytes < sizeof(uintptr_t)) return 0;
    unsigned candidates = 0;
    for (size_t countOffset = 0; countOffset + sizeof(uint32_t) <= objectBytes; countOffset += 4) {
        uint32_t count{};
        std::memcpy(&count, bytes + countOffset, sizeof(count));
        if (count != 91 && count != 96) continue;
        const size_t start = countOffset >= 0x30 ? countOffset - 0x30 : 0;
        const size_t end = std::min(objectBytes - sizeof(uintptr_t), countOffset + 0x30);
        for (size_t pointerOffset = start; pointerOffset <= end; pointerOffset += 8) {
            uintptr_t pointer{};
            std::memcpy(&pointer, bytes + pointerOffset, sizeof(pointer));
            if ((pointer & 0x7) != 0 || !readableRange(reinterpret_cast<void*>(pointer), size_t(count) * 48)) continue;
            auto matrices = reinterpret_cast<const float*>(pointer);
            float score = 0.0f;
            for (uint32_t joint = 0; joint < std::min<uint32_t>(count, 8); ++joint)
                score += jointMatrixScore(matrices + joint * 12);
            if (score < 12.0f) continue;
            const float* grip = matrices + 2 * 12;
            std::ostringstream out;
            out << "joint-array candidate #" << ++candidates
                << " owner=" << owner << "@0x" << std::hex << ownerAddress
                << " countOffset=0x" << countOffset
                << " pointerOffset=0x" << pointerOffset
                << " ptr=0x" << pointer << std::dec
                << " count=" << count << " score=" << score
                << " joint2.translation=" << grip[3] << ',' << grip[7] << ',' << grip[11]
                << " joint2.row0=" << grip[0] << ',' << grip[1] << ',' << grip[2];
            log(out.str());
        }
    }
    return candidates;
}

void dumpGenericJointMatrixCandidates(std::ofstream& out, const unsigned char* bytes,
                                      size_t objectBytes, const char* owner,
                                      uintptr_t ownerAddress) {
    if (!bytes || objectBytes < sizeof(uintptr_t)) return;
    for (size_t countOffset = 0; countOffset + sizeof(uint32_t) <= objectBytes; countOffset += 4) {
        uint32_t count{};
        std::memcpy(&count, bytes + countOffset, sizeof(count));
        if (count < 2 || count > 256) continue;
        const size_t start = countOffset >= 0x40 ? countOffset - 0x40 : 0;
        const size_t end = std::min(objectBytes - sizeof(uintptr_t), countOffset + 0x40);
        for (size_t pointerOffset = start; pointerOffset <= end; pointerOffset += 8) {
            uintptr_t pointer{};
            std::memcpy(&pointer, bytes + pointerOffset, sizeof(pointer));
            const size_t matrixBytes = size_t(count) * 48;
            if ((pointer & 0xF) != 0 || !readableRange(reinterpret_cast<void*>(pointer), matrixBytes)) continue;
            const auto matrices = reinterpret_cast<const float*>(pointer);
            const uint32_t tested = std::min<uint32_t>(count, 8);
            float score = 0.0f;
            for (uint32_t joint = 0; joint < tested; ++joint)
                score += jointMatrixScore(matrices + joint * 12);
            if (score < float(tested) * 2.7f) continue;
            out << "MATRIX_CANDIDATE owner=" << owner << "@0x" << std::hex << ownerAddress
                << " countOffset=0x" << countOffset << " pointerOffset=0x" << pointerOffset
                << " ptr=0x" << pointer << std::dec << " count=" << count << " score=" << score;
            const uint32_t logged = std::min<uint32_t>(count, 4);
            for (uint32_t joint = 0; joint < logged; ++joint) {
                const float* matrix = matrices + joint * 12;
                out << " j" << joint << '=' << matrix[3] << ',' << matrix[7] << ',' << matrix[11];
            }
            out << '\n';
        }
    }
}

void dumpDirectJointMatrixRuns(std::ofstream& out, const unsigned char* bytes,
                               size_t objectBytes, const char* owner,
                               uintptr_t ownerAddress) {
    if (!bytes || objectBytes < 48 * 4) return;
    for (size_t offset = 0; offset + 48 * 4 <= objectBytes; offset += 4) {
        const auto first = reinterpret_cast<const float*>(bytes + offset);
        unsigned run = 0;
        while (offset + size_t(run + 1) * 48 <= objectBytes && run < 256 &&
               jointMatrixScore(first + run * 12) >= 2.9f) {
            ++run;
        }
        if (run < 4) continue;
        out << "MATRIX_RUN owner=" << owner << "@0x" << std::hex << ownerAddress
            << " offset=0x" << offset << std::dec << " count=" << run << '\n';
        for (unsigned joint = 0; joint < run; ++joint) {
            const float* matrix = first + joint * 12;
            out << "MATRIX_RUN_JOINT " << joint << " origin="
                << matrix[3] << ',' << matrix[7] << ',' << matrix[11] << " axis=";
            for (int row = 0; row < 3; ++row)
                for (int column = 0; column < 3; ++column)
                    out << (row || column ? "," : "") << matrix[row * 4 + column];
            out << '\n';
        }
        offset += size_t(run) * 48 - 4;
    }
}

void dumpEntityPointerGraph(std::ofstream& out, uintptr_t entity, const char* owner,
                            size_t directBytes = 0x200) {
    if (!entity || !readableRange(reinterpret_cast<void*>(entity), directBytes)) return;
    const auto bytes = reinterpret_cast<const unsigned char*>(entity);
    out << "POINTER_GRAPH " << owner << " entity=0x" << std::hex << entity << std::dec << '\n';
    dumpGenericJointMatrixCandidates(out, bytes, directBytes, owner, entity);
    dumpDirectJointMatrixRuns(out, bytes, directBytes, owner, entity);

    std::array<uintptr_t, 512> visited{};
    size_t visitedCount = 0;
    for (size_t offset = 0; offset + sizeof(uintptr_t) <= directBytes && visitedCount < visited.size(); offset += 8) {
        uintptr_t pointer{};
        std::memcpy(&pointer, bytes + offset, sizeof(pointer));
        if ((pointer & 0x7) != 0 || pointer < 0x10000 ||
            !readableRange(reinterpret_cast<void*>(pointer), 0x1000)) continue;
        bool duplicate = false;
        for (size_t index = 0; index < visitedCount; ++index) duplicate |= visited[index] == pointer;
        if (duplicate) continue;
        visited[visitedCount++] = pointer;
        out << "PTRLINK owner=" << owner << " offset=0x" << std::hex << offset
            << " target=0x" << pointer << std::dec << '\n';
        dumpGenericJointMatrixCandidates(out, reinterpret_cast<const unsigned char*>(pointer),
                                         0x1000, owner, pointer);
        dumpDirectJointMatrixRuns(out, reinterpret_cast<const unsigned char*>(pointer),
                                  0x1000, owner, pointer);
    }
    out << "POINTER_GRAPH_END " << owner << " links=" << visitedCount << '\n';
}

void dumpWeaponObjectFloatDeltas(std::ofstream& out, uintptr_t weaponObject,
                                 unsigned snapshot) {
    if (!weaponObject ||
        !readableRange(reinterpret_cast<void*>(weaponObject), weaponObjectSnapshotBytes)) {
        out << "WEAPON_OBJECT_SNAPSHOT unreadable\n";
        return;
    }
    const auto bytes = reinterpret_cast<const unsigned char*>(weaponObject);
    if (snapshot == 1 || !baselineWeaponObjectValid) {
        std::memcpy(baselineWeaponObjectBytes.data(), bytes, weaponObjectSnapshotBytes);
        baselineWeaponObjectAddress = weaponObject;
        baselineWeaponObjectValid = true;
        out << "WEAPON_OBJECT_BASELINE address=0x" << std::hex << weaponObject
            << std::dec << " bytes=" << weaponObjectSnapshotBytes << '\n';
        return;
    }

    out << "WEAPON_OBJECT_COMPARE baselineAddress=0x" << std::hex
        << baselineWeaponObjectAddress << " currentAddress=0x" << weaponObject
        << std::dec << " sameObject=" << (baselineWeaponObjectAddress == weaponObject) << '\n';
    unsigned logged = 0;
    for (size_t offset = 0; offset + sizeof(float) * 3 <= weaponObjectSnapshotBytes;
         offset += sizeof(float)) {
        float baseline[3]{}, current[3]{};
        std::memcpy(baseline, baselineWeaponObjectBytes.data() + offset, sizeof(baseline));
        std::memcpy(current, bytes + offset, sizeof(current));
        bool plausible = true;
        float deltaSquared = 0.0f;
        for (int component = 0; component < 3; ++component) {
            plausible = plausible && std::isfinite(baseline[component]) &&
                        std::isfinite(current[component]) &&
                        std::abs(baseline[component]) <= 4096.0f &&
                        std::abs(current[component]) <= 4096.0f;
            const float delta = current[component] - baseline[component];
            deltaSquared += delta * delta;
        }
        if (!plausible || deltaSquared < 0.0625f || deltaSquared > 4000000.0f) continue;
        out << "WEAPON_FLOAT3_DELTA offset=0x" << std::hex << offset << std::dec
            << " baseline=" << baseline[0] << ',' << baseline[1] << ',' << baseline[2]
            << " current=" << current[0] << ',' << current[1] << ',' << current[2]
            << " delta=" << current[0] - baseline[0] << ','
            << current[1] - baseline[1] << ',' << current[2] - baseline[2] << '\n';
        if (++logged >= 1024) break;
    }
    out << "WEAPON_OBJECT_COMPARE_END float3Deltas=" << logged << '\n';
}

void scanHandsJointArrays(void* hands) {
    if (!hands || jointScanLogged.load(std::memory_order_acquire)) return;
    const auto call = jointScanCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (call != 1 && call % 120 != 0) return;

    constexpr size_t handsBytes = 0x10400;
    if (!readableRange(hands, handsBytes)) {
        if (call == 1) log("joint scan skipped: idHands range is not readable");
        return;
    }
    const auto bytes = static_cast<const unsigned char*>(hands);
    unsigned candidates = scanJointArraysInObject(bytes, handsBytes, "idHands",
                                                   reinterpret_cast<uintptr_t>(hands));

    // Animator/render-model state is normally owned by idHands through one
    // level of pointers. Search those compact objects too; the original scan
    // only inspected inline idHands storage and therefore missed loaded joints.
    std::array<uintptr_t, 512> visited{};
    size_t visitedCount = 0;
    for (size_t pointerOffset = 0; pointerOffset + sizeof(uintptr_t) <= handsBytes && visitedCount < visited.size(); pointerOffset += 8) {
        uintptr_t pointer{};
        std::memcpy(&pointer, bytes + pointerOffset, sizeof(pointer));
        if ((pointer & 0x7) != 0 || pointer < 0x10000 || !readableRange(reinterpret_cast<void*>(pointer), 0x800)) continue;
        bool duplicate = false;
        for (size_t index = 0; index < visitedCount; ++index) duplicate |= visited[index] == pointer;
        if (duplicate) continue;
        visited[visitedCount++] = pointer;
        candidates += scanJointArraysInObject(reinterpret_cast<const unsigned char*>(pointer), 0x800,
                                               "idHands-ref", pointer);
    }
    if (candidates) jointScanLogged.store(true, std::memory_order_release);
    if (call <= 240 || candidates || call % 1200 == 0) {
        log("joint scan call=" + std::to_string(call) +
            " referencedObjects=" + std::to_string(visitedCount) +
            " candidates=" + std::to_string(candidates));
    }
}

void dumpBoneSnapshot(void* hands) {
    if (!hands || !getHandsModel || !getJointTransform) return;
    void* model = getHandsModel(hands);
    if (!model) return;

    const unsigned snapshot = boneSnapshotCount.fetch_add(1, std::memory_order_acq_rel) + 1;
    const char* label = snapshot == 1 ? "BASELINE" : snapshot == 2 ? "DISPLACED" : "EXTRA";
    const auto path = kharvox::logPathA("weapon_bone_snapshots.log");
    std::ofstream out(path, snapshot == 1 ? std::ios::trunc : std::ios::app);
    if (!out) {
        log("F4 bone snapshot could not open " + path);
        return;
    }

    out << "=== SNAPSHOT " << snapshot << ' ' << label
        << " tick=" << GetTickCount64()
        << " generation=" << pose.resetGeneration.load(std::memory_order_acquire)
        << " hands=0x" << std::hex << reinterpret_cast<uintptr_t>(hands)
        << " model=0x" << reinterpret_cast<uintptr_t>(model) << std::dec << " ===\n";

    float bodyOrigin[3]{}, bodyAxis[9]{};
    if (KharvoxCameraGetBodyPose(bodyOrigin, bodyAxis)) {
        out << "BODY origin=" << bodyOrigin[0] << ',' << bodyOrigin[1] << ',' << bodyOrigin[2] << " axis=";
        for (int index = 0; index < 9; ++index) out << (index ? "," : "") << bodyAxis[index];
        out << '\n';
    }
    out << "CONTROLLER grip=";
    for (int index = 0; index < 3; ++index)
        out << (index ? "," : "") << pose.grip[index].load(std::memory_order_relaxed);
    out << " baseline=";
    for (int index = 0; index < 3; ++index)
        out << (index ? "," : "") << pose.baselineGrip[index].load(std::memory_order_relaxed);
    out << " quaternion=";
    for (int index = 0; index < 4; ++index)
        out << (index ? "," : "") << pose.deltaQuaternion[index].load(std::memory_order_relaxed);
    out << '\n';

    out << "PIVOT current=";
    for (int index = 0; index < 3; ++index)
        out << (index ? "," : "") << animatedGripPivot[index].load(std::memory_order_relaxed);
    out << " neutral=";
    for (int index = 0; index < 3; ++index)
        out << (index ? "," : "") << neutralGripPivot[index].load(std::memory_order_relaxed);
    out << " neutralValid=" << neutralGripPivotValid.load(std::memory_order_acquire) << '\n';

    const auto dumpSubmittedTransform = [&](const char* name, uintptr_t entity,
                                            const auto& origin, const auto& axis) {
        out << name << " entity=0x" << std::hex << entity << std::dec << " origin=";
        for (int index = 0; index < 3; ++index)
            out << (index ? "," : "") << origin[index].load(std::memory_order_relaxed);
        out << " axis=";
        for (int index = 0; index < 9; ++index)
            out << (index ? "," : "") << axis[index].load(std::memory_order_relaxed);
        out << '\n';
    };
    dumpSubmittedTransform("ROOT_NATIVE", lastControllerRootEntity.load(std::memory_order_acquire),
                           lastNativeControllerRootOrigin, lastNativeControllerRootAxis);
    out << "WEAPON_ANCHOR origin=";
    for (int index = 0; index < 3; ++index)
        out << (index ? "," : "") << lastWeaponBodyAnchorOrigin[index].load(std::memory_order_relaxed);
    out << '\n';
    dumpSubmittedTransform("ROOT", lastControllerRootEntity.load(std::memory_order_acquire),
                           lastControllerRootOrigin, lastControllerRootAxis);
    dumpSubmittedTransform("PROP", lastWeaponPropEntity.load(std::memory_order_acquire),
                           lastWeaponPropOrigin, lastWeaponPropAxis);
    out << "WEAPON_PRESENT object=0x" << std::hex
        << lastWeaponRenderObject.load(std::memory_order_acquire) << std::dec
        << " origin=";
    for (int index = 0; index < 3; ++index)
        out << (index ? "," : "") << lastWeaponPresentOrigin[index].load(std::memory_order_relaxed);
    out << " axis=";
    for (int index = 0; index < 9; ++index)
        out << (index ? "," : "") << lastWeaponPresentAxis[index].load(std::memory_order_relaxed);
    out << " viewOffset=";
    for (int index = 0; index < 3; ++index)
        out << (index ? "," : "") << lastWeaponPresentViewOffset[index].load(std::memory_order_relaxed);
    out << " integers=";
    for (int index = 0; index < 4; ++index)
        out << (index ? "," : "") << lastWeaponPresentIntegers[index].load(std::memory_order_relaxed);
    out << " modelScale=" << lastWeaponPresentModelScale.load(std::memory_order_relaxed)
        << " depthHack=" << lastWeaponPresentDepthHack.load(std::memory_order_relaxed) << '\n';

    const auto dumpNativeEntityDepth = [&](const char* role, uintptr_t entity) {
        const auto bytes = reinterpret_cast<const unsigned char*>(entity);
        if (!entity || !readableRange(bytes, 0x154)) return;
        float depth{};
        std::memcpy(&depth, bytes + 0x14C, sizeof(depth));
        out << "NATIVE_ENTITY_DEPTH role=" << role << " stored=" << depth
            << " flags151=" << unsigned(bytes[0x151]) << '\n';
    };
    dumpNativeEntityDepth("root", lastControllerRootEntity.load(std::memory_order_acquire));
    dumpNativeEntityDepth("weapon-prop", lastWeaponPropEntity.load(std::memory_order_acquire));

    const auto dumpModelBones = [&](const char* prefix, void* jointModel,
                                    unsigned jointLimit) {
        unsigned validBones = 0;
        if (!jointModel || !readableRange(jointModel, 0xEA0)) {
            out << prefix << "_MODEL unreadable\n";
            return validBones;
        }
        uintptr_t animator{};
        std::memcpy(&animator, static_cast<const unsigned char*>(jointModel) + 0xE98,
                    sizeof(animator));
        out << prefix << "_MODEL entity=0x" << std::hex
            << reinterpret_cast<uintptr_t>(jointModel) << " animator=0x" << animator
            << std::dec << '\n';
        if (!animator || !readableRange(reinterpret_cast<void*>(animator), sizeof(uintptr_t)))
            return validBones;
        for (unsigned joint = 0; joint < jointLimit; ++joint) {
            float origin[3]{}, axis[9]{};
            if (!getJointTransform(jointModel, 1, joint, origin, axis)) {
                out << prefix << ' ' << joint << " invalid\n";
                continue;
            }
            bool finite = true;
            for (float value : origin) finite = finite && std::isfinite(value);
            for (float value : axis) finite = finite && std::isfinite(value);
            if (!finite) {
                out << prefix << ' ' << joint << " nonfinite\n";
                continue;
            }
            ++validBones;
            out << prefix << ' ' << joint << " origin=" << origin[0] << ',' << origin[1] << ',' << origin[2]
                << " axis=";
            for (int index = 0; index < 9; ++index) out << (index ? "," : "") << axis[index];
            out << '\n';
        }
        return validBones;
    };

    const unsigned validBones = dumpModelBones("BONE", model, 96);
    const auto propEntity = lastWeaponPropEntity.load(std::memory_order_acquire);
    const unsigned validPropBones = dumpModelBones(
        "PROP_BONE", reinterpret_cast<void*>(propEntity), 128);
    dumpWeaponObjectFloatDeltas(
        out, lastWeaponRenderObject.load(std::memory_order_acquire), snapshot);
    dumpEntityPointerGraph(out, lastControllerRootEntity.load(std::memory_order_acquire), "ROOT", 0x1000);
    dumpEntityPointerGraph(out, propEntity, "PROP", 0x1000);
    dumpEntityPointerGraph(out, lastWeaponRenderObject.load(std::memory_order_acquire),
                           "WEAPON_RENDER", 0x2000);
    out << "=== END SNAPSHOT " << snapshot << " validBones=" << validBones
        << " validPropBones=" << validPropBones << " ===\n";
    out.close();

    // Also force the existing memory-layout scanner once per snapshot. Its
    // candidate addresses are written to KHARVOX.log beside this marker.
    jointScanLogged.store(false, std::memory_order_release);
    jointScanCalls.store(0, std::memory_order_release);
    scanHandsJointArrays(hands);
    log(std::string("F4 bone snapshot #") + std::to_string(snapshot) + " " + label +
        " captured to " + path + " validBones=" +
        std::to_string(validBones));
}

void pollBoneSnapshot(void* hands) {
    const bool down = (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
    if (down && !boneSnapshotKeyWasDown) dumpBoneSnapshot(hands);
    boneSnapshotKeyWasDown = down;
}

bool validateCollectibleClassifier(unsigned char* image) {
    // Reflection: idHands::webAnimator (+0x3248), its customAnimSelect
    // (+0x878), and idHands::handsFlags (+0x10374). The reflected getter
    // proves customAnimPlaying is bit 7 of handsFlags byte 5.
    constexpr uint64_t webField = 0x8d800003248ULL;
    constexpr uint64_t selectField = 0x400000878ULL;
    constexpr uint64_t flagsField = 0x700010374ULL;
    constexpr unsigned char getter[]{0x0f,0xb6,0x41,0x05,0xc1,0xe8,0x07,0xc3};
    const bool supported = !std::memcmp(image + 0x31e3048, &webField, 8)
        && !std::memcmp(image + 0x3206230, &selectField, 8)
        && !std::memcmp(image + 0x31e4638, &flagsField, 8)
        && !std::memcmp(image + 0x1508030, getter, sizeof(getter))
        && !std::strcmp(reinterpret_cast<const char*>(image + 0x27aa228),
            "HANDS_CUSTOM_ANIM_COLLECTABLE_FIRST")
        && *reinterpret_cast<const uint64_t*>(image + 0x3519988) == 6
        && *reinterpret_cast<const uint64_t*>(image + 0x3519998) == 7
        && *reinterpret_cast<const uint64_t*>(image + 0x35199a8) == 8;
    collectibleClassifierSupported.store(supported, std::memory_order_release);
    log(supported ? "[COLLECTIBLE-QUAD] native animation classifier validated (first/standard/fist bump)"
        : "[COLLECTIBLE-QUAD] native metadata mismatch; classifier disabled");
    return supported;
}

bool readCollectibleAnimation(void* hands) {
    if (!hands || !collectibleClassifierSupported.load(std::memory_order_acquire)) return false;
    const auto bytes = static_cast<const unsigned char*>(hands);
#if defined(_MSC_VER)
    __try {
#endif
        return kharvox::collectibleAnimationPlaying(
            *reinterpret_cast<const float*>(bytes + 0x3248 + 0x878), bytes[0x10374 + 5]);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#endif
}

extern "C" void __fastcall updateHandsTransformHook(void* hands) {
    activeHands = hands;
    if (originalUpdateHandsTransform) originalUpdateHandsTransform(hands);

    // Observe the live native animation even in Quad/cinematics, before the
    // gameplay-only model helper guard. Never retain a game object pointer.
    const bool collectible = readCollectibleAnimation(hands);
    collectibleSeenLevel.store(KharvoxCameraLevelTransitionGeneration(), std::memory_order_relaxed);
    collectibleSeenPresent.store(collectible ? KharvoxCameraCurrentPresentSerial() : 0,
        std::memory_order_release);

    // idHands continues to receive update calls while a level is being torn
    // down and while the replacement player/hands graph is only partially
    // constructed.  The native update itself owns that transition, but our
    // model/joint helper calls require a fully active gameplay camera.
    if (collectible || !KharvoxCameraGameplayActive()) {
        animatedGripPivotValid.store(false, std::memory_order_release);
        neutralGripPivotValid.store(false, std::memory_order_release);
        controllerRootProbeValid = false;
        activeHands = nullptr;
        return;
    }
    pollBoneSnapshot(hands);
    captureAnimatedGripPivot(hands);
    hideAnimatedPlayerRootModel(hands);
    activeHands = nullptr;
}

extern "C" short* __fastcall resolveHandsResourceHook(
    void* owner, short* result, void* candidate, const char* name) {
    if (!owner) {
        // The original routine already uses -1 as its normal "not found"
        // result when candidate is null.  During restart/menu re-entry DOOM
        // can also call it with a null owner; mirror the same benign outcome
        // instead of dereferencing owner+0xA8.
        if (result) *result = -1;
        return result;
    }
    return originalResolveHandsResource
        ? originalResolveHandsResource(owner, result, candidate, name)
        : result;
}

bool installHandsResourceResolverNullGuard(unsigned char* image) {
    auto target = image + 0x16E18D0;
    constexpr unsigned char signature[] = {
        0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,
        0x57,0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x20
    };
    if (std::memcmp(target, signature, sizeof(signature))) {
        log("RVA 0x16E18D0 idHands resource signature mismatch; restart null guard disabled");
        return false;
    }

    auto trampoline = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        log("idHands resource null-guard trampoline allocation failed");
        return false;
    }
    auto cursor = trampoline;
    std::memcpy(cursor, signature, sizeof(signature));
    cursor += sizeof(signature);
    emit8(cursor, 0xFF); emit8(cursor, 0x25);
    emit8(cursor, 0); emit8(cursor, 0); emit8(cursor, 0); emit8(cursor, 0);
    emit64(cursor, reinterpret_cast<unsigned long long>(target + sizeof(signature)));
    originalResolveHandsResource = reinterpret_cast<ResolveHandsResourceFn>(trampoline);

    DWORD oldProtect{};
    if (!VirtualProtect(target, sizeof(signature), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        log("idHands resource null-guard target protection failed");
        return false;
    }
    unsigned char jump[sizeof(signature)]{0xFF,0x25,0,0,0,0};
    const auto wrapper = reinterpret_cast<unsigned long long>(resolveHandsResourceHook);
    std::memcpy(jump + 6, &wrapper, sizeof(wrapper));
    for (size_t index = 14; index < sizeof(jump); ++index) jump[index] = 0x90;
    std::memcpy(target, jump, sizeof(jump));
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(jump));
    DWORD ignored{};
    VirtualProtect(target, sizeof(jump), oldProtect, &ignored);
    log("idHands restart null guard installed at RVA 0x16E18D0");
    return true;
}

bool installUpdateHandsTransformHook(unsigned char* image) {
    auto target = image + 0xD7D5D0;
    constexpr unsigned char signature[] = {
        0x48,0x8B,0xC4,0x48,0x89,0x58,0x18,0x55,0x56,0x57,
        0x48,0x8D,0xA8,0x98,0xFD,0xFF,0xFF
    };
    if (std::memcmp(target, signature, sizeof(signature))) {
        log("RVA 0xD7D5D0 idHands update signature mismatch; joint scanner disabled");
        return false;
    }
    auto trampoline = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) return false;
    auto cursor = trampoline;
    std::memcpy(cursor, signature, sizeof(signature));
    cursor += sizeof(signature);
    emit8(cursor, 0xFF); emit8(cursor, 0x25);
    emit8(cursor, 0); emit8(cursor, 0); emit8(cursor, 0); emit8(cursor, 0);
    emit64(cursor, reinterpret_cast<unsigned long long>(target + sizeof(signature)));
    originalUpdateHandsTransform = reinterpret_cast<UpdateHandsTransformFn>(trampoline);
    getHandsModel = reinterpret_cast<GetHandsModelFn>(image + 0xD60D10);
    getJointTransform = reinterpret_cast<GetJointTransformFn>(image + 0x15EE6C0);
    // Signature checked against the game's named-locator entry, not a guessed vtable slot.
    constexpr unsigned char namedJointSignature[]{0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xEC,0x60};
    if(!std::memcmp(image+0x15EFB60,namedJointSignature,sizeof(namedJointSignature)))
        getNamedJointTransform=reinterpret_cast<GetNamedJointTransformFn>(image+0x15EFB60);
    else log("[LASER] named muzzle locator signature mismatch; affected lasers withheld");
    // 0x15EFEC0 clears the surface visibility bit (BTR). 0x15F0C40 is
    // the inverse ShowMesh operation (BTS), despite their adjacent layout.
    hideRenderModelMesh = reinterpret_cast<HideRenderModelMeshFn>(image + 0x15EFEC0);

    DWORD oldProtect{};
    if (!VirtualProtect(target, sizeof(signature), PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    unsigned char jump[sizeof(signature)]{0xFF,0x25,0,0,0,0};
    const auto wrapper = reinterpret_cast<unsigned long long>(updateHandsTransformHook);
    std::memcpy(jump + 6, &wrapper, sizeof(wrapper));
    for (size_t index = 14; index < sizeof(jump); ++index) jump[index] = 0x90;
    std::memcpy(target, jump, sizeof(jump));
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(jump));
    VirtualProtect(target, sizeof(jump), oldProtect, &oldProtect);
    log("idHands update wrapper installed at RVA 0xD7D5D0 for joint discovery");
    return true;
}

extern "C" void __fastcall weaponRenderUpdateHook(
    void* renderObject, const float* origin, const float* axis, const float* viewOffset,
    int renderView, int frame, int time, int flags, float modelScale, float depthHack) {
    const auto image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto returnAddress = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const bool weaponPresentation = image && returnAddress - image == 0xD8B0D3
        && renderObject && KharvoxCameraGameplayActive();
    const float* submittedOrigin = origin;
    const float* submittedAxis = axis;
    float pairedOrigin[3]{}, pairedAxis[9]{};
    if (weaponPresentation && synchronizeAerAnimatedWeaponProp(
            reinterpret_cast<uintptr_t>(renderObject)
                ^ (uintptr_t{1} << (sizeof(uintptr_t) * 8 - 1)),
            origin, axis, pairedOrigin, pairedAxis)) {
        submittedOrigin = pairedOrigin;
        submittedAxis = pairedAxis;
    }
    if (weaponPresentation) {
        lastWeaponRenderObject.store(reinterpret_cast<uintptr_t>(renderObject),
                                     std::memory_order_release);
        if (submittedOrigin) {
            for (int index = 0; index < 3; ++index)
                lastWeaponPresentOrigin[index].store(submittedOrigin[index], std::memory_order_relaxed);
        }
        if (submittedAxis) {
            for (int index = 0; index < 9; ++index)
                lastWeaponPresentAxis[index].store(submittedAxis[index], std::memory_order_relaxed);
        }
        if (viewOffset) {
            for (int index = 0; index < 3; ++index)
                lastWeaponPresentViewOffset[index].store(viewOffset[index], std::memory_order_relaxed);
        }
        lastWeaponPresentIntegers[0].store(renderView, std::memory_order_relaxed);
        lastWeaponPresentIntegers[1].store(frame, std::memory_order_relaxed);
        lastWeaponPresentIntegers[2].store(time, std::memory_order_relaxed);
        lastWeaponPresentIntegers[3].store(flags, std::memory_order_relaxed);
        lastWeaponPresentModelScale.store(modelScale, std::memory_order_relaxed);
        // D8B04A -> stack+48 -> F275A0 forwards hands_depthHack to child/FX
        // render updates. It does NOT prove the root/prop entity's cached
        // +0x14C value changed; the entity-axis hook logs that independently.
        lastWeaponPresentDepthHack.store(depthHack, std::memory_order_relaxed);
        static std::atomic<unsigned> depthWeaponsLogged{};
        const auto kind=KharvoxWeaponCurrentKind();
        const auto index=static_cast<unsigned>(kind);
        if(customHandsRequested()&&index>0&&index<static_cast<unsigned>(KharvoxWeaponKind::Count)){
            const unsigned bit=1u<<index;
            if(!(depthWeaponsLogged.fetch_or(bit,std::memory_order_relaxed)&bit))
                log(std::string("[HANDS] native weapon depth weapon=")
                    +KharvoxWeaponKindKey(kind)+" submitted="+std::to_string(depthHack)
                    +" expected=1 source=child-render-update");
        }
    }
    if (originalWeaponRenderUpdate) {
        originalWeaponRenderUpdate(renderObject, submittedOrigin, submittedAxis, viewOffset,
                                   renderView, frame, time, flags, modelScale, depthHack);
    }
}

bool installWeaponRenderUpdateProbe(unsigned char* image) {
    auto target = image + 0xF275A0;
    constexpr unsigned char signature[] = {
        0x48,0x8B,0xC4,0x48,0x89,0x58,0x08,0x48,0x89,0x68,0x10,
        0x48,0x89,0x70,0x18
    };
    if (std::memcmp(target, signature, sizeof(signature))) {
        log("RVA 0xF275A0 weapon-render signature mismatch; child-model probe disabled");
        return false;
    }

    auto trampoline = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        log("weapon-render probe trampoline allocation failed");
        return false;
    }
    auto cursor = trampoline;
    std::memcpy(cursor, signature, sizeof(signature));
    cursor += sizeof(signature);
    emit8(cursor, 0xFF); emit8(cursor, 0x25);
    emit8(cursor, 0); emit8(cursor, 0); emit8(cursor, 0); emit8(cursor, 0);
    emit64(cursor, reinterpret_cast<unsigned long long>(target + sizeof(signature)));
    originalWeaponRenderUpdate = reinterpret_cast<WeaponRenderUpdateFn>(trampoline);

    DWORD oldProtect{};
    if (!VirtualProtect(target, sizeof(signature), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        originalWeaponRenderUpdate = nullptr;
        VirtualFree(trampoline, 0, MEM_RELEASE);
        log("weapon-render probe target protection failed");
        return false;
    }
    unsigned char jump[sizeof(signature)]{0xFF,0x25,0,0,0,0};
    const auto wrapper = reinterpret_cast<unsigned long long>(weaponRenderUpdateHook);
    std::memcpy(jump + 6, &wrapper, sizeof(wrapper));
    for (size_t index = 14; index < sizeof(jump); ++index) jump[index] = 0x90;
    std::memcpy(target, jump, sizeof(jump));
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(jump));
    DWORD ignored{};
    VirtualProtect(target, sizeof(jump), oldProtect, &ignored);
    log("weapon child-render probe installed at RVA 0xF275A0; caller gated to RVA 0xD8B0D3");
    return true;
}

extern "C" float __fastcall handsFovScaleHook(void* hands) {
    // A value of exactly one means that the first-person model uses the same
    // projection scale as world geometry.  DOOM's original routine combines
    // per-weapon animation and aspect-ratio compensation, which makes a
    // world/controller-space weapon slide when the HMD view rotates.
    if (controllerPlacementActive()) return configuredHandsProjectionScale();
    return originalHandsFovScale ? originalHandsFovScale(hands) : 1.0f;
}

bool installHandsFovScaleHook(unsigned char* image) {
    auto target = image + 0xD60760;
    constexpr unsigned char signature[] = {
        0x40, 0x53, 0x48, 0x83, 0xEC, 0x60, 0x48, 0x8B, 0xD9,
        0x48, 0x8B, 0x89, 0xB0, 0x02, 0x00, 0x00
    };
    if (std::memcmp(target, signature, sizeof(signature))) {
        log("RVA 0xD60760 hands-FOV signature mismatch; projection neutralizer disabled");
        return false;
    }

    auto trampoline = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        log("hands-FOV trampoline allocation failed");
        return false;
    }
    auto cursor = trampoline;
    std::memcpy(cursor, signature, sizeof(signature));
    cursor += sizeof(signature);
    emit8(cursor, 0xFF); emit8(cursor, 0x25);
    emit8(cursor, 0); emit8(cursor, 0); emit8(cursor, 0); emit8(cursor, 0);
    emit64(cursor, reinterpret_cast<unsigned long long>(target + sizeof(signature)));
    originalHandsFovScale = reinterpret_cast<HandsFovScaleFn>(trampoline);

    DWORD oldProtect{};
    if (!VirtualProtect(target, sizeof(signature), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        originalHandsFovScale = nullptr;
        VirtualFree(trampoline, 0, MEM_RELEASE);
        log("hands-FOV target protection failed");
        return false;
    }
    unsigned char jump[sizeof(signature)]{0xFF, 0x25, 0, 0, 0, 0};
    const auto wrapper = reinterpret_cast<unsigned long long>(handsFovScaleHook);
    std::memcpy(jump + 6, &wrapper, sizeof(wrapper));
    for (size_t index = 14; index < sizeof(jump); ++index) jump[index] = 0x90;
    std::memcpy(target, jump, sizeof(jump));
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(jump));
    VirtualProtect(target, sizeof(jump), oldProtect, &oldProtect);
    fovHookInstalled.store(true, std::memory_order_release);
    log("6DoF hands projection override installed at RVA 0xD60760 (constant scale=" +
        std::to_string(configuredHandsProjectionScale()) + " while tracked)");
    return true;
}

std::array<float, 3> rotateXrQuaternionInDoomCoordinates(
    const std::array<float, 4>& q,
    const std::array<float, 3>& doomVector) {
    // DOOM local (forward, lateral, up) = OpenXR local (-z, -x, y).
    const std::array<float, 3> xr{-doomVector[1], doomVector[2], -doomVector[0]};
    const std::array<float, 3> u{q[0], q[1], q[2]};
    const float dotUV = u[0] * xr[0] + u[1] * xr[1] + u[2] * xr[2];
    const float dotUU = u[0] * u[0] + u[1] * u[1] + u[2] * u[2];
    const std::array<float, 3> cross{
        u[1] * xr[2] - u[2] * xr[1],
        u[2] * xr[0] - u[0] * xr[2],
        u[0] * xr[1] - u[1] * xr[0]
    };
    const std::array<float, 3> rotated{
        2.0f * dotUV * u[0] + (q[3] * q[3] - dotUU) * xr[0] + 2.0f * q[3] * cross[0],
        2.0f * dotUV * u[1] + (q[3] * q[3] - dotUU) * xr[1] + 2.0f * q[3] * cross[1],
        2.0f * dotUV * u[2] + (q[3] * q[3] - dotUU) * xr[2] + 2.0f * q[3] * cross[2]
    };
    return {-rotated[2], -rotated[0], rotated[1]};
}

std::array<float, 3> worldVectorToBody(const float* bodyAxis, const std::array<float, 3>& world) {
    return {
        world[0] * bodyAxis[0] + world[1] * bodyAxis[1] + world[2] * bodyAxis[2],
        world[0] * bodyAxis[3] + world[1] * bodyAxis[4] + world[2] * bodyAxis[5],
        world[0] * bodyAxis[6] + world[1] * bodyAxis[7] + world[2] * bodyAxis[8]
    };
}

std::array<float, 3> bodyVectorToWorld(const float* bodyAxis, const std::array<float, 3>& local) {
    return {
        local[0] * bodyAxis[0] + local[1] * bodyAxis[3] + local[2] * bodyAxis[6],
        local[0] * bodyAxis[1] + local[1] * bodyAxis[4] + local[2] * bodyAxis[7],
        local[0] * bodyAxis[2] + local[1] * bodyAxis[5] + local[2] * bodyAxis[8]
    };
}


bool stabilizeWeaponBodyOrigin(float bodyOrigin[3], const float bodyAxis[9]) {
    float physicsOrigin[3]{};
    if (!KharvoxCameraGetPlayerPhysicsOrigin(physicsOrigin)) return false;

    const std::array<float, 3> nativeWorldOffset{
        bodyOrigin[0] - physicsOrigin[0],
        bodyOrigin[1] - physicsOrigin[1],
        bodyOrigin[2] - physicsOrigin[2]
    };
    const auto localOffset = worldVectorToBody(bodyAxis, nativeWorldOffset);
    const float horizontalLength = std::hypot(bodyAxis[0], bodyAxis[1]);
    if (!std::isfinite(horizontalLength) || horizontalLength < 0.001f) return false;

    const float forwardX = bodyAxis[0] / horizontalLength;
    const float forwardY = bodyAxis[1] / horizontalLength;
    const float levelForward[3]{forwardX, forwardY, 0.0f};
    const float levelLateral[3]{-forwardY, forwardX, 0.0f};
    for (int world = 0; world < 3; ++world) {
        bodyOrigin[world] = physicsOrigin[world]
            + levelForward[world] * localOffset[0]
            + levelLateral[world] * localOffset[1];
    }
    bodyOrigin[2] += localOffset[2];
    return std::isfinite(bodyOrigin[0]) && std::isfinite(bodyOrigin[1])
        && std::isfinite(bodyOrigin[2]);
}

bool buildControllerTransform(uintptr_t entity, const float* nativeOrigin, const float* nativeAxis,
                              float* desiredOrigin, float* desiredAxis) {
    weaponRootSourceValid=false;
    const bool sourceMode=KharvoxCameraUsesAerGameplaySource();
    if (!sourceMode&&!pose.valid.load(std::memory_order_acquire)) return false;
    pollWeaponRotationCalibration();
    kharvox::AerWeaponFrame sourceFrame;
    kharvox::AerWeaponResolveDiagnostic sourceDiagnostic;
    const bool sourceBound=sourceMode&&resolveWeaponSource(sourceFrame,&sourceDiagnostic);
    if(sourceMode&&!sourceBound){
        traceWeaponSource(kharvox::pose_trace::WeaponSourceRoot,entity,sourceFrame,nullptr,nullptr,0);
        static std::atomic<unsigned long long> misses[9]{};
        const auto reason=static_cast<unsigned>(sourceDiagnostic.failure);
        const auto count=misses[reason].fetch_add(1,std::memory_order_relaxed)+1;
        if(count<=4||count%120==0)
            log("[AER-WEAPON] unresolved source root count="+std::to_string(count)
                +" reason="+std::to_string(reason)
                +" pose="+std::to_string(sourceDiagnostic.poseId)
                +" cameraPresent="+std::to_string(sourceDiagnostic.cameraPresent)
                +" present="+std::to_string(sourceDiagnostic.now)
                +" level="+std::to_string(KharvoxCameraLevelTransitionGeneration()));
        return false;
    }
    const auto input=sourceBound?kharvox::rebaseAerWeaponInput(sourceFrame.input,sourceFrame.camera.bodyYawDelta)
        :currentControllerInput();
    if(!input.valid)return false;
    float bodyOrigin[3]{}, bodyAxis[9]{};
    if(sourceBound){
        std::memcpy(bodyOrigin,sourceFrame.camera.bodyOrigin.data(),sizeof(bodyOrigin));
        std::memcpy(bodyAxis,sourceFrame.camera.bodyAxis.data(),sizeof(bodyAxis));
    }else if (!KharvoxCameraGetBodyPose(bodyOrigin, bodyAxis)) return false;
    // A native damage impulse can pitch/roll the camera basis. CameraHook's
    // stable eye-height is intentionally stored in that native basis for the
    // rest of the game, but rotating 87 units of height also introduces a
    // large false horizontal offset into a tracked weapon. Reconstruct only
    // the weapon's translation anchor in a gravity-level yaw basis. Do not
    // publish this correction back to camera, HMD, HUD, interaction or turn.
    if(!sourceBound)stabilizeWeaponBodyOrigin(bodyOrigin, bodyAxis);
    for (int index = 0; index < 3; ++index)
        lastWeaponBodyAnchorOrigin[index].store(bodyOrigin[index], std::memory_order_relaxed);
    const std::array<float, 3> nativeOffset{
        nativeOrigin[0] - bodyOrigin[0], nativeOrigin[1] - bodyOrigin[1], nativeOrigin[2] - bodyOrigin[2]
    };
    const auto nativeOriginLocal = worldVectorToBody(bodyAxis, nativeOffset);

    const unsigned generation = input.generation;
    if (calibratedGeneration != generation || calibratedEntity != entity) {
        modelOriginLocal = nativeOriginLocal;
        float headOrigin[3]{}, headAxis[9]{};
        // Never lock in a body-relative fallback: that would reintroduce an
        // entry-pose-dependent mount if the HMD basis arrives one frame late.
        // Leave calibration pending until the final render basis is valid.
        if(sourceBound)std::memcpy(headAxis,sourceFrame.camera.headAxis.data(),sizeof(headAxis));
        else if (!KharvoxCameraGetHeadRenderPose(headOrigin, headAxis)) return false;
        for (int row = 0; row < 3; ++row) {
            const std::array<float, 3> worldRow{
                nativeAxis[row * 3], nativeAxis[row * 3 + 1], nativeAxis[row * 3 + 2]
            };
            // DOOM's final native idHands axis has already inherited the
            // rendered camera pose. Read the weapon-specific holder/model
            // basis relative to that final view, not relative to the player
            // body. The tracked controller can then supply the absolute
            // tracking-level rotation without baking entry-time HMD pitch or
            // roll into the weapon mount.
            const auto localRow = worldVectorToBody(headAxis, worldRow);
            for (int column = 0; column < 3; ++column) modelAxisLocal[row * 3 + column] = localRow[column];
        }
        calibratedGeneration = generation;
        calibratedEntity = entity;
        std::ostringstream out;
        out << "final idHands transform calibrated entity=0x" << std::hex << entity
            << std::dec << " nativeLocal=" << modelOriginLocal[0] << ',' << modelOriginLocal[1] << ',' << modelOriginLocal[2]
            << " mountBasis=HMD_DECOUPLED";
        log(out.str());
    }

    const auto grip=input.grip,baselineGrip=input.baseline;
    const auto deltaQuaternion=input.orientation;

    const float weaponScale = configuredWeaponScale();
    const float yawHalfRadians = currentWeaponYawOffsetDegrees() * 0.00872664626f;
    const std::array<float, 4> yawCorrection{
        0.0f, std::sin(yawHalfRadians), 0.0f, std::cos(yawHalfRadians)
    };
    const float pitchHalfRadians = currentWeaponPitchOffsetDegrees() * 0.00872664626f;
    const std::array<float, 4> pitchCorrection{
        std::sin(pitchHalfRadians), 0.0f, 0.0f, std::cos(pitchHalfRadians)
    };
    const float rollHalfRadians = currentWeaponRollOffsetDegrees() * 0.00872664626f;
    const std::array<float, 4> rollCorrection{
        0.0f, 0.0f, std::sin(rollHalfRadians), std::cos(rollHalfRadians)
    };
    std::array<std::array<float, 3>, 3> rotatedModelAxisLocal{};
    for (int row = 0; row < 3; ++row) {
        const std::array<float, 3> localRow{
            modelAxisLocal[row * 3], modelAxisLocal[row * 3 + 1], modelAxisLocal[row * 3 + 2]
        };
        const auto yawCorrectedLocalRow = rotateXrQuaternionInDoomCoordinates(yawCorrection, localRow);
        const auto pitchCorrectedLocalRow = rotateXrQuaternionInDoomCoordinates(pitchCorrection, yawCorrectedLocalRow);
        const auto correctedLocalRow = rotateXrQuaternionInDoomCoordinates(rollCorrection, pitchCorrectedLocalRow);
        rotatedModelAxisLocal[row] = rotateXrQuaternionInDoomCoordinates(deltaQuaternion, correctedLocalRow);
    }

    std::array<float, 3> desiredLocalOrigin{};
    if (animatedGripPivotValid.load(std::memory_order_acquire)) {
        const auto adjustment = configuredWeaponPivotAdjustment();
        std::array<float, 3> gripPivot{};
        for (int index = 0; index < 3; ++index)
            gripPivot[index] = animatedGripPivot[index].load(std::memory_order_relaxed) + adjustment[index];
        std::array<float, 3> pivotOffsetBody{};
        for (int component = 0; component < 3; ++component) {
            pivotOffsetBody[component] = weaponScale * (
                gripPivot[0] * rotatedModelAxisLocal[0][component] +
                gripPivot[1] * rotatedModelAxisLocal[1][component] +
                gripPivot[2] * rotatedModelAxisLocal[2][component]);
        }
        for (int component = 0; component < 3; ++component)
            desiredLocalOrigin[component] = grip[component] - pivotOffsetBody[component];
    } else {
        const std::array<float, 3> gripToModel{
            modelOriginLocal[0] - baselineGrip[0], modelOriginLocal[1] - baselineGrip[1], modelOriginLocal[2] - baselineGrip[2]
        };
        auto rotatedGripToModel = rotateXrQuaternionInDoomCoordinates(deltaQuaternion, gripToModel);
        for (int component = 0; component < 3; ++component)
            desiredLocalOrigin[component] = grip[component] + rotatedGripToModel[component] * weaponScale;
    }
    const auto desiredWorldOffset = bodyVectorToWorld(bodyAxis, desiredLocalOrigin);
    for (int axis = 0; axis < 3; ++axis) desiredOrigin[axis] = bodyOrigin[axis] + desiredWorldOffset[axis];

    for (int row = 0; row < 3; ++row) {
        const auto worldRow = bodyVectorToWorld(bodyAxis, rotatedModelAxisLocal[row]);
        for (int column = 0; column < 3; ++column)
            desiredAxis[row * 3 + column] = worldRow[column] * weaponScale;
    }

    if(sourceBound){
        if(sourceFrame.input.epoch!=weaponSourceEpoch.load(std::memory_order_acquire)
            ||sourceFrame.input.generation!=pose.resetGeneration.load(std::memory_order_acquire))return false;
        const bool reused=weaponSourceTransforms.hold(sourceFrame,entity,0,desiredOrigin,desiredAxis,&sourceFrame);
        weaponRootSource=sourceFrame;weaponRootSourceValid=true;
        weaponRootSourcePresent=KharvoxCameraCurrentPresentSerial();
        traceWeaponSource(kharvox::pose_trace::WeaponSourceRoot,entity,sourceFrame,desiredOrigin,desiredAxis,reused?2:1);
    }
    return true;
}

bool synchronizeAerAnimatedWeaponProp(
    uintptr_t entity, const float* nativeOrigin, const float* nativeAxis,
    float synchronizedOrigin[3], float synchronizedAxis[9]) {
    if (!entity || !nativeOrigin || !nativeAxis) return false;
    if(KharvoxCameraUsesAerGameplaySource()){
        // Carry the source selected by this worker's actual root calculation
        // through to its child prop. Never substitute the newer scheduled eye.
        const auto now=KharvoxCameraCurrentPresentSerial();
        const bool bound=weaponRootSourceValid&&kharvox::aerWeaponPropSourceMatches(weaponRootSource,
            weaponRootSourcePresent,now,KharvoxCameraLevelTransitionGeneration(),
            weaponSourceEpoch.load(std::memory_order_acquire),pose.resetGeneration.load(std::memory_order_acquire));
        if(!bound){
            traceWeaponSource(kharvox::pose_trace::WeaponSourceProp,entity,{},nativeOrigin,nativeAxis,0);
            return false;
        }
        std::memcpy(synchronizedOrigin,nativeOrigin,3*sizeof(float));
        std::memcpy(synchronizedAxis,nativeAxis,9*sizeof(float));
        kharvox::AerWeaponFrame propSource;
        const bool reused=weaponSourceTransforms.hold(weaponRootSource,entity,1,synchronizedOrigin,synchronizedAxis,&propSource);
        traceWeaponSource(kharvox::pose_trace::WeaponSourceProp,entity,propSource,synchronizedOrigin,synchronizedAxis,reused?2:1);
        static std::atomic<uint64_t> counts{};const auto n=counts.fetch_add(1,std::memory_order_relaxed)+1;
        if(kharvox::extendedDiagnosticsEnabled()&&(n<=4||n%1024==0)) {
            LARGE_INTEGER now{},frequency{};QueryPerformanceCounter(&now);QueryPerformanceFrequency(&frequency);
            const auto sample=weaponRootSource.input.sampleQpc;
            const double ageMs=sample&&frequency.QuadPart>0&&uint64_t(now.QuadPart)>=sample
                ?1000.0*double(uint64_t(now.QuadPart)-sample)/double(frequency.QuadPart):-1.0;
            log("[AER-WEAPON-SOURCE] r263 pose="+std::to_string(weaponRootSource.camera.key.poseId)
                +" eye="+std::to_string(weaponRootSource.camera.key.eye)+" reused="+std::to_string(reused)+" count="+std::to_string(n)
                +" inputToPropMs="+std::to_string(ageMs)+" sourcePresent="+std::to_string(weaponRootSource.camera.present)
                +" currentPresent="+std::to_string(KharvoxCameraCurrentPresentSerial()));
        }
        return reused;
    }
    const auto pairState = kharvox::unpackAerWeaponPairState(
        aerRenderPairState.load(std::memory_order_acquire));
    if (!pairState.enabled) return false;
    if (!aerWeaponPoseCache.resolve(pairState, entity, nativeOrigin, nativeAxis,
            synchronizedOrigin, synchronizedAxis)) return false;
    static std::atomic<unsigned long long> synchronizedProps{};
    const auto count = synchronizedProps.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 4 || count % 120 == 0)
        log("[AER-CINEMATIC] final animated weapon prop shared across eye pair #"
            + std::to_string(count));
    return true;
}

void updateCustomHandsWeaponDepth(unsigned char* entity, bool prop) {
    if (!customHandsRequested()) return; // no change at all to guns-only
    // Verified in the supported DOOM executable: D6AA6D writes the root
    // entity's +0x14C; D8AB52 writes the weapon prop's +0x14C. r133 live
    // snapshots showed both still at 0.1 while the CVar/child argument was 1.
    // Touch only the entity supplied by the two verified axis-hook callers,
    // before DOOM submits its updated transform. No global renderer patch,
    // render-category flag change, cached-pointer writes or extra GPU work.
    constexpr std::size_t depthOffset = 0x14C;
    if (!writableRange(entity + depthOffset, sizeof(float))) return;
    static SRWLOCK lock = SRWLOCK_INIT;
    static kharvox::hands::NativeWeaponDepthOverrides<> overrides;
    if (!TryAcquireSRWLockExclusive(&lock)) return;
    float before{}, after{};
    std::memcpy(&before, entity + depthOffset, sizeof(before));
    const bool active = controllerPlacementActive()
        && !readCollectibleAnimation(activeHands)
        && KharvoxCameraGameplayActive() && !KharvoxCameraCutsceneActive();
    const bool changed = overrides.update(reinterpret_cast<uintptr_t>(entity),
                                           before, active, after);
    if (changed) std::memcpy(entity + depthOffset, &after, sizeof(after));
    ReleaseSRWLockExclusive(&lock);

    // Bounded diagnostic: one entry per weapon and role, plus one restoration
    // entry per role. The field readback distinguishes cached depth from CVar.
    static std::atomic<unsigned> logged[2]{};
    static std::atomic<unsigned> restored{};
    const auto kind = KharvoxWeaponCurrentKind();
    const unsigned index = static_cast<unsigned>(kind);
    bool report = false;
    if (active && index > 0 && index < static_cast<unsigned>(KharvoxWeaponKind::Count)) {
        const unsigned bit = 1u << index;
        report = !(logged[prop ? 1 : 0].fetch_or(bit, std::memory_order_relaxed) & bit);
    } else if (changed) {
        const unsigned bit = prop ? 2u : 1u;
        report = !(restored.fetch_or(bit, std::memory_order_relaxed) & bit);
    }
    if (report) {
        float stored{};
        std::memcpy(&stored, entity + depthOffset, sizeof(stored));
        log(std::string("[HANDS] native entity depth role=") + (prop ? "weapon-prop" : "root")
            + " weapon=" + KharvoxWeaponKindKey(kind) + " before=" + std::to_string(before)
            + " stored=" + std::to_string(stored) + " mode=" + (active ? "scene" : "restored"));
    }
}

extern "C" void __fastcall setRenderEntityAxisHook(void* rawEntity, const float* nativeAxis) {
    auto entity = static_cast<unsigned char*>(rawEntity);
    const auto returnSlot = static_cast<unsigned char*>(_AddressOfReturnAddress());
    const auto image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto returnAddress = *reinterpret_cast<const uintptr_t*>(returnSlot);
    const bool weaponCaller = returnAddress - image == 0xD7E518;
    const bool weaponPropCaller = returnAddress - image == 0xD8AE66;
    const bool hudFinalCaller = returnAddress - image == 0xF9227F;
    if (weaponCaller || weaponPropCaller)
        updateCustomHandsWeaponDepth(entity, weaponPropCaller);
    const float* axisToCopy = nativeAxis;
    float desiredOrigin[3]{}, desiredAxis[9]{};
    if (hudFinalCaller && KharvoxHudCompleteFinalEntity(
            rawEntity, nativeAxis, desiredOrigin, desiredAxis)) {
        static std::atomic<bool> hudMatchLogged{};
        if (!hudMatchLogged.exchange(true, std::memory_order_relaxed))
            log("[HUD11-FLAT] final HUD origin and axis locked to the same current head pose at RVA 0xF9227F");
        const unsigned char lockMask = entity[0x71];
        if ((entity[0x70] & lockMask) == 0)
            std::memcpy(entity + 0xC8, desiredOrigin, sizeof(desiredOrigin));
        std::memcpy(entity + 0x78, desiredOrigin, sizeof(desiredOrigin));
        axisToCopy = desiredAxis;
    }

    if (weaponCaller) {
        // CALL pushes eight bytes. The caller's [rsp+48] origin is 0x50 bytes
        // above this callee's return-address slot.
        auto callerOrigin = reinterpret_cast<float*>(returnSlot + 0x50);
        for (int index = 0; index < 3; ++index)
            lastNativeControllerRootOrigin[index].store(callerOrigin[index], std::memory_order_relaxed);
        for (int index = 0; index < 9; ++index)
            lastNativeControllerRootAxis[index].store(nativeAxis[index], std::memory_order_relaxed);
        if (buildControllerTransform(reinterpret_cast<uintptr_t>(rawEntity), callerOrigin, nativeAxis,
                                     desiredOrigin, desiredAxis)) {
            std::memcpy(callerOrigin, desiredOrigin, sizeof(desiredOrigin));
            axisToCopy = desiredAxis;
            lastControllerRootEntity.store(reinterpret_cast<uintptr_t>(rawEntity), std::memory_order_release);
            for (int index = 0; index < 3; ++index)
                lastControllerRootOrigin[index].store(desiredOrigin[index], std::memory_order_relaxed);
            for (int index = 0; index < 9; ++index)
                lastControllerRootAxis[index].store(desiredAxis[index], std::memory_order_relaxed);
            controllerRootProbeValid = true;
        }
    } else if (weaponPropCaller && readableRange(entity + 0x78, sizeof(float) * 3)) {
        auto propOrigin = reinterpret_cast<float*>(entity + 0x78);
        if (synchronizeAerAnimatedWeaponProp(
                reinterpret_cast<uintptr_t>(rawEntity), propOrigin, nativeAxis,
                desiredOrigin, desiredAxis)) {
            const unsigned char lockMask = entity[0x71];
            if ((entity[0x70] & lockMask) == 0)
                std::memcpy(entity + 0xC8, desiredOrigin, sizeof(desiredOrigin));
            std::memcpy(propOrigin, desiredOrigin, sizeof(desiredOrigin));
            axisToCopy = desiredAxis;
        }
        if (controllerRootProbeValid && controllerPlacementActive()
            && !readCollectibleAnimation(activeHands)
            && KharvoxCameraGameplayActive() && !KharvoxCameraCutsceneActive()) {
            lastWeaponPropEntity.store(reinterpret_cast<uintptr_t>(rawEntity), std::memory_order_release);
            for (int index = 0; index < 3; ++index)
                lastWeaponPropOrigin[index].store(propOrigin[index], std::memory_order_relaxed);
            for (int index = 0; index < 9; ++index)
                lastWeaponPropAxis[index].store(axisToCopy[index], std::memory_order_relaxed);
            // This is the actual weapon-prop entity and its final model transform.
            // Query its native animated joints while the entity is valid; unlike
            // the transient child-render scratch object, this exposes the barrel
            // joints consistently for every recognized weapon.
            publishPropLaserMuzzlePose(rawEntity, propOrigin, axisToCopy);
        }
    }

    const unsigned char lockMask = entity[0x71];
    if ((entity[0x70] & lockMask) == 0) std::memcpy(entity + 0xD4, axisToCopy, sizeof(float) * 9);
    std::memcpy(entity + 0x84, axisToCopy, sizeof(float) * 9);
}
}

bool KharvoxWeaponInstallHook() {
    if (installed.load(std::memory_order_acquire)) return true;
    auto image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!image) return false;
    installWeaponAmmoSnapshot(image);
    installHandsResourceResolverNullGuard(image);
    installFrontPushbackSuppression(image);
    const bool hitReactionOverrideArmed = armHandsHitReactionSuppression(image);
    // The optional validated CVar profile now owns g_weaponkick. Do not
    // arm the old unconditional integer override when the checkbox is off.
    const bool weaponKickOverrideArmed = false;
    installActiveWeaponIdentityHook(image);
    validateCollectibleClassifier(image);
    installUpdateHandsTransformHook(image);
    installWeaponRenderUpdateProbe(image);
    installHandsFovScaleHook(image);
    installMuzzleFireAxisOverride(image);
    auto target = image + 0x3B5400;
    constexpr unsigned char signature[] = {
        0x0F,0xB6,0x41,0x71,0x84,0x41,0x70,0x75,0x50,
        0x8B,0x02,0x89,0x81,0xD4,0x00,0x00,0x00
    };
    if (std::memcmp(target, signature, sizeof(signature))) {
        log("RVA 0x3B5400 axis-copy signature mismatch; final transform hook disabled");
        if (weaponKickOverrideArmed) setWeaponKickSuppressed(false);
        if (hitReactionOverrideArmed) setHandsHitReactionsSuppressed(false);
        return false;
    }
    DWORD oldProtect{};
    if (!VirtualProtect(target, sizeof(signature), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        log("axis-copy target protection failed");
        if (weaponKickOverrideArmed) setWeaponKickSuppressed(false);
        if (hitReactionOverrideArmed) setHandsHitReactionsSuppressed(false);
        return false;
    }
    unsigned char jump[sizeof(signature)]{0xFF,0x25,0,0,0,0};
    const auto wrapper = reinterpret_cast<unsigned long long>(setRenderEntityAxisHook);
    std::memcpy(jump + 6, &wrapper, sizeof(wrapper));
    for (size_t index = 14; index < sizeof(jump); ++index) jump[index] = 0x90;
    std::memcpy(target, jump, sizeof(jump));
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(jump));
    VirtualProtect(target, sizeof(jump), oldProtect, &oldProtect);
    installed.store(true, std::memory_order_release);
    const auto pivot = configuredWeaponPivotAdjustment();
    std::ostringstream installedMessage;
    installedMessage << "final idHands origin/axis hook installed at RVA 0x3B5400; caller gated to RVA 0xD7E518"
        << "; animated righthandattach adjustment=" << pivot[0] << ',' << pivot[1] << ',' << pivot[2]
        << " yawOffset=" << currentWeaponYawOffsetDegrees()
        << " rollOffset=" << currentWeaponRollOffsetDegrees()
        << " pitchOffset=" << currentWeaponPitchOffsetDegrees()
        << " scale=" << configuredWeaponScale();
    log(installedMessage.str());
    return true;
}

bool KharvoxWeaponIsTrackingActive() {
    return installed.load(std::memory_order_acquire) && pose.valid.load(std::memory_order_acquire);
}

bool KharvoxWeaponCollectibleAnimationActive() {
    const auto seen = collectibleSeenPresent.load(std::memory_order_acquire);
    return kharvox::collectibleObservationCurrent(seen, KharvoxCameraCurrentPresentSerial(),
        collectibleSeenLevel.load(std::memory_order_relaxed), KharvoxCameraLevelTransitionGeneration());
}

void KharvoxWeaponObserveAerCamera(const kharvox::AerWeaponCamera& camera){weaponSourceHistory.camera(camera);}
int KharvoxWeaponResolveAerDraw(kharvox::AerSourceKey source,const float* origin,const float* axis,
    float* targetOrigin,float* targetAxis,uint64_t& matchedPoseId,uintptr_t model,uintptr_t asset,bool* recovered,const float* drawCameraOrigin){
    kharvox::AerWeaponFrame frame;
    const bool found=weaponSourceHistory.frame(source,KharvoxCameraCurrentPresentSerial(),frame);
    const bool followBody=found&&kharvox::sfs::vrEnabled()&&alignAerWeaponDrawCamera(frame,drawCameraOrigin);
    return weaponSourceTransforms.forDraw(source,origin,axis,targetOrigin,targetAxis,matchedPoseId,found?&frame:nullptr,model,asset,recovered,followBody);
}

void KharvoxWeaponRememberAerInput(unsigned long long poseId) {
    if(poseId){
        const auto input=weaponSourceHistory.remember(poseId,KharvoxCameraLevelTransitionGeneration(),currentControllerInput());
        if(kharvox::pose_trace::active.load(std::memory_order_relaxed)){
            kharvox::pose_trace::Event e{};e.kind=kharvox::pose_trace::WeaponSourceInput;
            e.frame=KharvoxCameraCurrentPresentSerial();e.poseId=poseId;e.eye=-1; // input shared by the pair
            e.revision=input.epoch;e.flags=input.generation;e.status=input.valid?1:0;
            std::memcpy(e.data,input.grip.data(),sizeof(input.grip));
            std::memcpy(e.data+3,input.orientation.data(),sizeof(input.orientation));kharvox::pose_trace::record(e);
        }
    }
}

void KharvoxWeaponSetAerRenderPair(int renderEye, bool enabled, unsigned long long poseId) {
    if(enabled)KharvoxWeaponRememberAerInput(poseId);
    // Only the pair-cache phase is remapped; the physical camera eye is not.
    renderEye = kharvox::aerRenderPairPhase(renderEye);
    auto previous = aerRenderPairState.load(std::memory_order_acquire);
    for (;;) {
        const auto next = kharvox::nextAerWeaponPairState(
            previous, renderEye, enabled);
        if (aerRenderPairState.compare_exchange_weak(previous, next,
                std::memory_order_release, std::memory_order_acquire))
            break;
    }
}

namespace {
void invalidateAerWeaponPairCache() {
    weaponSourceEpoch.fetch_add(1,std::memory_order_acq_rel);
    auto previous = aerRenderPairState.load(std::memory_order_acquire);
    while (!aerRenderPairState.compare_exchange_weak(previous,
        kharvox::invalidateAerWeaponPairState(previous),
        std::memory_order_release, std::memory_order_acquire)) { }
}
}

KharvoxWeaponKind KharvoxWeaponCurrentKind() {
    return activeWeaponKind.load(std::memory_order_acquire);
}

void KharvoxWeaponCaptureAmmoSnapshot(void* player) {
    if (!player || !weaponAmmoSnapshotSupported.load(std::memory_order_acquire)) return;
    const auto now = GetTickCount64();
    const auto previousPlayer = weaponAmmoSnapshotPlayer.load(std::memory_order_acquire);
    const auto previousTick = weaponAmmoSnapshotTick.load(std::memory_order_acquire);
    if (previousPlayer == reinterpret_cast<uintptr_t>(player)
        && previousTick && now - previousTick < 100) return;

    void* inventory{};
    int itemCount{};
    if (!queryPlayerInventorySafely(player, inventory, itemCount)) return;

    std::array<KharvoxWeaponAmmoState,
        static_cast<size_t>(KharvoxWeaponKind::Count)> snapshot{};
    snapshot.fill(KharvoxWeaponAmmoState::Unknown);
    for (size_t index = 0; index < snapshot.size(); ++index) {
        const auto kind = static_cast<KharvoxWeaponKind>(index);
        if (shoulderSelectableAmmoKind(kind))
            snapshot[index] = KharvoxWeaponAmmoState::Unavailable;
    }

    for (int index = 0; index < itemCount; ++index) {
        void* item{};
        if (!queryInventoryItemSafely(inventory, index, item)) return;
        if (!item || !readableRange(item, 0x38)) continue;
        uintptr_t declaration{};
        std::memcpy(&declaration, static_cast<unsigned char*>(item) + 0x30,
            sizeof(declaration));
        if (!declaration || !readableRange(reinterpret_cast<void*>(declaration), 0x63E))
            continue;
        if (!*reinterpret_cast<const unsigned char*>(declaration + 0x63D)) continue;

        uintptr_t nameAddress{};
        std::memcpy(&nameAddress, reinterpret_cast<const void*>(declaration + 0x08),
            sizeof(nameAddress));
        std::string declarationName;
        if (!readableAsciiString(nameAddress, declarationName)) continue;
        const auto match = weaponIdentityMatch(declarationName);
        if (!shoulderSelectableAmmoKind(match.kind)) continue;

        const auto kindIndex = static_cast<size_t>(match.kind);
        if (snapshot[kindIndex] == KharvoxWeaponAmmoState::Usable) continue;
        // The campaign pistol has infinite ammunition. Its native ammo item can
        // report zero, so ownership/selectability is sufficient for this final
        // fallback. Every other weapon uses DOOM's loaded + reserve query.
        if (match.kind == KharvoxWeaponKind::Pistol) {
            snapshot[kindIndex] = KharvoxWeaponAmmoState::Usable;
            continue;
        }
        int total{};
        snapshot[kindIndex] = queryWeaponAmmoTotalSafely(item, inventory, total)
            ? (total > 0 ? KharvoxWeaponAmmoState::Usable : KharvoxWeaponAmmoState::Empty)
            : KharvoxWeaponAmmoState::Unknown;
    }

    bool changed = previousPlayer != reinterpret_cast<uintptr_t>(player);
    for (size_t index = 0; index < snapshot.size(); ++index) {
        const auto kind = static_cast<KharvoxWeaponKind>(index);
        if (!shoulderSelectableAmmoKind(kind)) continue;
        const auto previous = weaponAmmoStates[index].exchange(
            snapshot[index], std::memory_order_acq_rel);
        changed |= previous != snapshot[index];
    }
    weaponAmmoSnapshotPlayer.store(reinterpret_cast<uintptr_t>(player), std::memory_order_release);
    weaponAmmoSnapshotTick.store(now, std::memory_order_release);

    if (changed) {
        std::ostringstream message;
        message << "[BACK-WEAPON] ammo snapshot";
        for (size_t index = 0; index < snapshot.size(); ++index) {
            const auto kind = static_cast<KharvoxWeaponKind>(index);
            if (!shoulderSelectableAmmoKind(kind)) continue;
            message << ' ' << KharvoxWeaponKindKey(kind) << '='
                << weaponAmmoStateName(snapshot[index]);
        }
        log(message.str());
    }
}

KharvoxWeaponAmmoState KharvoxWeaponGetAmmoState(KharvoxWeaponKind kind) {
    const auto index = static_cast<size_t>(kind);
    if (!weaponAmmoSnapshotSupported.load(std::memory_order_acquire)
        || index >= weaponAmmoStates.size()) return KharvoxWeaponAmmoState::Unknown;
    const auto tick = weaponAmmoSnapshotTick.load(std::memory_order_acquire);
    if (!tick || GetTickCount64() - tick > 500) return KharvoxWeaponAmmoState::Unknown;
    return weaponAmmoStates[index].load(std::memory_order_acquire);
}

bool KharvoxWeaponGetLaserMuzzlePose(float origin[3], float direction[3],
    float bodyOrigin[3], float bodyAxis[9], unsigned long long sourcePose, int sourceEye) {
    if(!origin||!direction||!bodyOrigin||!bodyAxis||!KharvoxWeaponIsTrackingActive())return false;
    LaserSourceSnapshot sample{};
    {
        std::lock_guard lock(laserSourceMutex);
        if(sourcePose) {
            if(!laserSources.find({sourcePose,KharvoxCameraLevelTransitionGeneration(),sourceEye},sample))return false;
        }else sample=latestLaserSource;
    }
    if(!kharvox::laserSourceUsable(sample.tick,GetTickCount64(),sample.epoch,
        weaponSourceEpoch.load(std::memory_order_acquire),unsigned(sample.kind),unsigned(KharvoxWeaponCurrentKind()))
        ||!laserAllowedForWeapon(sample.kind))return false;
    std::copy(sample.origin.begin(),sample.origin.end(),origin);
    std::copy(sample.direction.begin(),sample.direction.end(),direction);
    std::copy(sample.bodyOrigin.begin(),sample.bodyOrigin.end(),bodyOrigin);
    std::copy(sample.bodyAxis.begin(),sample.bodyAxis.end(),bodyAxis);
    return true;
}

const char* KharvoxWeaponKindKey(KharvoxWeaponKind kind) {
    switch (kind) {
    case KharvoxWeaponKind::Pistol: return "pistol";
    case KharvoxWeaponKind::Shotgun: return "shotgun";
    case KharvoxWeaponKind::HeavyAssaultRifle: return "heavy_assault_rifle";
    case KharvoxWeaponKind::PlasmaRifle: return "plasma_rifle";
    case KharvoxWeaponKind::RocketLauncher: return "rocket_launcher";
    case KharvoxWeaponKind::SuperShotgun: return "super_shotgun";
    case KharvoxWeaponKind::GaussCannon: return "gauss_cannon";
    case KharvoxWeaponKind::Chaingun: return "chaingun";
    case KharvoxWeaponKind::Bfg: return "bfg";
    case KharvoxWeaponKind::Chainsaw: return "chainsaw";
    case KharvoxWeaponKind::Fists: return "fists";
    case KharvoxWeaponKind::AssaultRifle: return "assault_rifle";
    case KharvoxWeaponKind::ArcCannon: return "arc_cannon";
    case KharvoxWeaponKind::MancubusGland: return "mancubus_gland";
    default: return "unknown";
    }
}

const char* KharvoxWeaponKindDisplayName(KharvoxWeaponKind kind) {
    switch (kind) {
    case KharvoxWeaponKind::Pistol: return "Pistol";
    case KharvoxWeaponKind::Shotgun: return "Combat Shotgun";
    case KharvoxWeaponKind::HeavyAssaultRifle: return "Heavy Assault Rifle";
    case KharvoxWeaponKind::PlasmaRifle: return "Plasma Rifle";
    case KharvoxWeaponKind::RocketLauncher: return "Rocket Launcher";
    case KharvoxWeaponKind::SuperShotgun: return "Super Shotgun";
    case KharvoxWeaponKind::GaussCannon: return "Gauss Cannon";
    case KharvoxWeaponKind::Chaingun: return "Chaingun";
    case KharvoxWeaponKind::Bfg: return "BFG 9000";
    case KharvoxWeaponKind::Chainsaw: return "Chainsaw";
    case KharvoxWeaponKind::Fists: return "Fists";
    case KharvoxWeaponKind::AssaultRifle: return "Assault Rifle";
    case KharvoxWeaponKind::ArcCannon: return "Arc Cannon";
    case KharvoxWeaponKind::MancubusGland: return "Mancubus Gland";
    default: return "Unknown weapon";
    }
}

KharvoxWeaponKind KharvoxWeaponKindFromKey(const char* key) {
    if (!key || !*key) return KharvoxWeaponKind::Unknown;
    for (int value = static_cast<int>(KharvoxWeaponKind::Pistol);
         value < static_cast<int>(KharvoxWeaponKind::Count); ++value) {
        const auto kind = static_cast<KharvoxWeaponKind>(value);
        if (_stricmp(key, KharvoxWeaponKindKey(kind)) == 0) return kind;
    }
    return KharvoxWeaponKind::Unknown;
}

void KharvoxWeaponResetCalibration() {
    std::lock_guard lock(controllerInputMutex);
    weaponSourceEpoch.fetch_add(1,std::memory_order_acq_rel);
    pose.valid.store(false, std::memory_order_release);
    if (installed.load(std::memory_order_acquire)) {
        setHandsHitReactionsSuppressed(true);
        setWeaponKickSuppressed(true);
    }
    pose.resetGeneration.fetch_add(1, std::memory_order_acq_rel);
}

void KharvoxWeaponSetControllerPose(
    float gripForward, float gripLateral, float gripUp,
    float baselineForward, float baselineLateral, float baselineUp,
    float controllerQuaternionX, float controllerQuaternionY,
    float controllerQuaternionZ, float controllerQuaternionW,
    bool valid) {
    // An invalid pose disables the override, but must not erase the last good
    // transform. OpenXR may report a short invalid interval during recentering,
    // and zeroing here made later diagnostics and recovery depend on native
    // viewmodel state from that interval.
    if (installed.load(std::memory_order_acquire)) {
        // These are 6DoF session policies, not tracking-validity policies. Keep
        // them enforced through pause, cinematic and controller-loss windows.
        setHandsHitReactionsSuppressed(true);
        setWeaponKickSuppressed(true);
    }
    std::lock_guard lock(controllerInputMutex);
    if (!valid) {
        pose.valid.store(false, std::memory_order_release);
        setMuzzleFireAxisOverride(false);
        return;
    }
    const float gripValues[]{gripForward, gripLateral, gripUp};
    LARGE_INTEGER published{};QueryPerformanceCounter(&published);pose.sampleQpc=uint64_t(published.QuadPart);
    const float baselineValues[]{baselineForward, baselineLateral, baselineUp};
    const float quaternionValues[]{controllerQuaternionX, controllerQuaternionY, controllerQuaternionZ, controllerQuaternionW};
    for (int index = 0; index < 3; ++index) {
        pose.grip[index].store(gripValues[index], std::memory_order_relaxed);
        pose.baselineGrip[index].store(baselineValues[index], std::memory_order_relaxed);
    }
    for (int index = 0; index < 4; ++index)
        pose.deltaQuaternion[index].store(quaternionValues[index], std::memory_order_relaxed);
    pose.valid.store(valid, std::memory_order_release);
    setMuzzleFireAxisOverride(valid && installed.load(std::memory_order_acquire));
}

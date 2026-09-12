#pragma once
#include "AerWeaponSource.h"
int KharvoxWeaponResolveAerDraw(kharvox::AerSourceKey source,const float* origin,const float* axis,
    float* targetOrigin,float* targetAxis,uint64_t& matchedPoseId,
    uintptr_t model=0,uintptr_t asset=0,bool* recovered=nullptr);

enum class KharvoxWeaponKind {
    Unknown = 0,
    Pistol = 1,
    Shotgun = 2,
    HeavyAssaultRifle = 3,
    PlasmaRifle = 4,
    RocketLauncher = 5,
    SuperShotgun = 6,
    GaussCannon = 7,
    Chaingun = 8,
    Bfg = 9,
    Chainsaw = 10,
    Fists = 11,
    AssaultRifle = 12,
    ArcCannon = 13,
    MancubusGland = 14,
    Count = 15
};

// Read-only state captured from DOOM's native inventory/ammo implementation.
// Unknown is deliberately distinct from Empty so unsupported game builds keep
// the proven activation-timeout fallback instead of skipping valid weapons.
enum class KharvoxWeaponAmmoState {
    Unknown = 0,
    Unavailable,
    Empty,
    Usable
};

bool KharvoxWeaponInstallHook();
bool KharvoxWeaponIsTrackingActive();
bool KharvoxWeaponCollectibleAnimationActive();
KharvoxWeaponKind KharvoxWeaponCurrentKind();
// Called from the proven idPlayer gameplay-camera path. Internally throttled;
// it never changes inventory or ammo and only publishes a short-lived cache.
void KharvoxWeaponCaptureAmmoSnapshot(void* player);
KharvoxWeaponAmmoState KharvoxWeaponGetAmmoState(KharvoxWeaponKind kind);
// Returns the current rendered weapon muzzle in DOOM world coordinates. The
// pose is taken from the weapon model's animated joint matrices, not inferred
// from the controller. Chainsaw/fists and stale weapon frames return false.
bool KharvoxWeaponGetLaserMuzzlePose(float origin[3], float direction[3]);
const char* KharvoxWeaponKindKey(KharvoxWeaponKind kind);
const char* KharvoxWeaponKindDisplayName(KharvoxWeaponKind kind);
KharvoxWeaponKind KharvoxWeaponKindFromKey(const char* key);
void KharvoxWeaponResetCalibration();
void KharvoxWeaponSetControllerPose(
    float gripForward, float gripLateral, float gripUp,
    float baselineForward, float baselineLateral, float baselineUp,
    float controllerQuaternionX, float controllerQuaternionY,
    float controllerQuaternionZ, float controllerQuaternionW,
    bool valid);
// Shares DOOM's final animated weapon-prop transform across an AER eye pair.
// This is deliberately downstream of the controller root so scripted weapon
// poses used by cinematics are synchronized as well.
void KharvoxWeaponSetAerRenderPair(int renderEye, bool enabled, unsigned long long poseId = 0);
// Must precede exposing a new paired head pose to camera/weapon workers.
void KharvoxWeaponRememberAerInput(unsigned long long poseId);
void KharvoxWeaponObserveAerCamera(const kharvox::AerWeaponCamera& camera);

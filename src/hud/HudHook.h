#pragma once
bool KharvoxHudPauseRootVisible();

#include <cstdint>
#include <cstddef>

enum class KharvoxHudAnchor : int {
    None = 0,
    RightHand = 1,
    LeftHand = 2,
    Head = 3,
    WeaponWheel = 4,
    DossierMap = 5,
    Auxiliary = 6
};

bool KharvoxHudInstallHook();
bool KharvoxHudFullscreenMenuActive();
bool KharvoxHudFieldDroneMenuActive();
void KharvoxHudBeginFieldDroneMenuSession();
void KharvoxHudEndFieldDroneMenuSession();
bool KharvoxHudSuitUpgradeMenuActive();
bool KharvoxHudPlayerUpgradeMenuActive();
bool KharvoxHudRuneSelectMenuActive();
bool KharvoxHudUpgradeCinematicRefreshGuardActive();
bool KharvoxHudTutorialActive();
void KharvoxHudBeginSuitUpgradeMenuSession();
void KharvoxHudEndSuitUpgradeMenuSession();
bool KharvoxHudPauseMenuActive();
bool KharvoxHudDeathMenuActive();
bool KharvoxHudEndOfLevelMenuActive();
void KharvoxHudPollQuadControls();
float KharvoxHudQuadScale();
float KharvoxHudDistanceMeters();
void KharvoxHudSetHeadsetGeometry(
    std::uint32_t surfaceWidth, std::uint32_t surfaceHeight,
    float safeTanHalfHorizontal, float safeTanHalfVertical);
void KharvoxHudSetHandPose(
    bool rightHand,
    float gripForward, float gripLateral, float gripUp,
    float quaternionX, float quaternionY, float quaternionZ, float quaternionW,
    bool valid);
bool KharvoxHudCompleteFinalEntity(
    const void* entity, const float* nativeAxis,
    float desiredOrigin[3], float desiredAxis[9]);

struct KharvoxHudDiagnosticSnapshot {
    std::uint64_t matchedSurfaces{};
    std::uint64_t completedSurfaces{};
    std::uint64_t crosshairSurfaces{};
    std::uint64_t lastMatchQpc{};
    std::uint32_t lastThreadId{};
    std::uintptr_t lastCallerRva{};
    int lastWidth{};
    int lastHeight{};
    int lastScaleMilli{};
    bool lastWasCrosshair{};
};

struct KharvoxHudDiagnosticEvent {
    std::uint64_t serial{};
    std::uint64_t qpc{};
    std::uint32_t threadId{};
    std::uintptr_t callerRva{};
    int width{};
    int height{};
    int scaleMilli{};
    bool crosshair{};
};

// HUD9 exposes a lock-free snapshot to the Vulkan diagnostic/capture layer. It is
// observational only: no render state or native HUD data is changed here.
void KharvoxHudGetDiagnosticSnapshot(KharvoxHudDiagnosticSnapshot& snapshot);
std::size_t KharvoxHudCopyDiagnosticEvents(
    std::uint64_t firstSerial, KharvoxHudDiagnosticEvent* events, std::size_t capacity);

// Native HUD movie playback, independent of immersive 3D cinematics.
bool KharvoxHudMovieActive();

bool KharvoxHudOffhandCalibrationActive();

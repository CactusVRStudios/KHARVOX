#include "AerCameraPairCache.h"
#include "CyberdemonQuadPolicy.h"
#include "DoomViewEffects.h"
#include "../common/AerRenderOrder.h"
#include "../common/AerEyeBasis.h"
#include "../common/AerCinematicProjection.h"
#include "../common/DiagnosticLogging.h"
#include "BodyPoseRebase.h"
#include "BodyStanceHeight.h"
#include "BodyAnchorPolicy.h"
#include "../common/PoseTrace.h"
#include "../native/NativeStereo.h"
#include "CameraHook.h"
#include "CameraBasisPolicy.h"
#include "PlayerControlPolicy.h"
#include "../common/RuntimePaths.h"
#include "../hud/HudHook.h"
#include "../weapon/WeaponHook.h"

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
#include <sstream>
#include <vector>

namespace {
struct HookSample {
    volatile uintptr_t context{};
    volatile unsigned long long hits{};
    volatile float yaw{};
    volatile float pitch{};
    volatile float positionX{};
    volatile float positionY{};
    volatile float positionZ{};
    volatile float fovX{};
    volatile float fovY{};
    volatile uintptr_t returnAddress{};
};

HookSample sample;
volatile long manualPositionApplied{};
std::mutex headPoseMutex;
unsigned long long headPoseId{}, aerHeadPairPoseId{};
std::atomic<float> headReferenceBodyYaw{};
std::atomic<bool> headReferenceBodyValid{};
std::atomic<float> headYaw{};
std::atomic<float> artificialTurnYaw{};
std::atomic<float> headPitch{};
std::atomic<float> headRoll{};
std::atomic<float> headForward{};
std::atomic<float> headLateral{};
std::atomic<float> headUp{};
std::atomic<bool> headValid{};
std::atomic<bool> worldCameraActive{};
std::atomic<bool> animatedSequenceActive{};
std::array<std::atomic<float>, 3> bodyCameraOrigin{};
std::array<std::atomic<float>, 9> bodyCameraAxis{};
std::atomic<bool> bodyCameraValid{};
std::array<std::atomic<float>, 9> preCinematicGameplayAxis{};
std::atomic<bool> preCinematicGameplayAxisValid{};
std::array<std::atomic<float>, 3> playerPhysicsOrigin{};
std::atomic<uintptr_t> playerPhysicsOwner{};
std::atomic<bool> playerPhysicsOriginValid{};
std::atomic<uintptr_t> playerLedgeTransitionOwner{};
std::atomic<bool> playerLedgeTransitionActive{};
std::atomic<bool> syncAttackClassifierSupported{};
std::atomic<bool> playerControlClassifierSupported{};
std::atomic<unsigned long long> cameraPresentSerial{};
std::atomic<unsigned long long> playerPhysicsCapturePresent{};
std::atomic<unsigned long long> levelTransitionGeneration{};
std::array<std::atomic<float>, 3> hudRenderAnchorOrigin{};
std::array<std::atomic<float>, 9> hudRenderAnchorAxis{};
std::atomic<unsigned long long> hudRenderPoseSequence{};
std::atomic<unsigned long long> hudRenderPosePresent{};
std::atomic<bool> hudRenderPoseValid{};
std::array<std::atomic<float>, 3> stableBodyViewOffsetLocal{};
std::atomic<uintptr_t> stableBodyViewOffsetOwner{};
std::atomic<bool> stableBodyViewOffsetValid{};
std::atomic<bool> crouchRequested{};
std::mutex bodyStanceMutex;
kharvox::BodyStanceHeight bodyStanceHeight;
volatile unsigned long long cutsceneFovHits{};
volatile float cutsceneFovValue{};
std::atomic<bool> cutsceneActive{};
alignas(4) volatile LONG immersiveCinematicFovXBits{};
alignas(4) volatile LONG immersiveCinematicFovYBits{};
volatile LONG immersiveCinematicFovRequested{};
volatile LONG immersiveCinematicFovHookActive{};
volatile LONG immersiveCinematicFreelookRequested{};
std::atomic<float> stereoEyeOffset{};
std::atomic<float> stereoFovX{};
std::atomic<float> stereoFovY{};
std::atomic<float> stereoOpticalCenterYaw{};
std::atomic<float> stereoOpticalCenterPitch{};
std::atomic<bool> stereoEyeEnabled{};
std::atomic<bool> aerRenderPairEnabled{};
// SteamXR's compositor consumes one render pose for the complete stereo
// projection layer.  When AER builds that layer over two Presents, preserve
// the exact HMD/artificial-turn input used by the first eye until the second
// eye has been rendered.  The runtime still receives a fresh xrEndFrame at its
// negotiated cadence (90/120/144 Hz or otherwise) and performs late reprojection.
std::array<std::atomic<float>, 7> aerHeadPairPose{};
float aerHeadPairReferenceYaw{};
bool aerHeadPairReferenceValid{};
std::atomic<bool> aerHeadPairTracked{};
std::atomic<bool> aerHeadPairSnapshotValid{};
std::array<std::atomic<float>, 2> configuredEyeOffset{};
std::array<std::atomic<float>, 2> configuredEyeFovX{};
std::array<std::atomic<float>, 2> configuredEyeFovY{};
std::array<std::atomic<float>, 2> configuredEyeOpticalCenterYaw{};
std::array<std::atomic<float>, 2> configuredEyeOpticalCenterPitch{};
std::atomic<bool> configuredStereo{};
std::atomic<bool> sameFrameInstalled{};
std::atomic<bool> nativeSameFrameInstalled{};
std::atomic<bool> nativeTwoViewInstalled{};
std::atomic<int> nativeStereoBootstrapStage{};
std::atomic<bool> nativeStereoLayoutEnabled{};
std::atomic<bool> nativeStereoFrameReady{};
alignas(8) std::array<unsigned char, 0x50> nativeLeftSingleViewLayout{};
alignas(8) std::array<unsigned char, 0x50> nativeRightSingleViewLayout{};
using RenderFrontendFn = bool(__fastcall*)(void*, void*, void*);
RenderFrontendFn originalRenderFrontend{};
using ShaderParseContextFn = void*(__fastcall*)(void*, void*, unsigned int);
ShaderParseContextFn originalShaderParseContext{};
using VulkanBackendResizeFn = void(__fastcall*)(void*);
VulkanBackendResizeFn originalVulkanBackendResize{};
using VulkanBackendOwnerInitFn = void(__fastcall*)(void*);
VulkanBackendOwnerInitFn originalVulkanBackendOwnerInit{};
std::atomic<void*> lastValidVulkanBackendContext{};
std::atomic<void*> lastValidVulkanBackendTimingContext{};
struct ShaderArenaExpansion {
    void* pool{};
    unsigned char* memory{};
    size_t bytes{};
};
std::array<ShaderArenaExpansion, 128> shaderArenaExpansions{};
std::mutex shaderArenaMutex;
std::atomic<VREye> currentRenderEye{VREye::Mono};

kharvox::AerCameraPairCache aerCameraPairPoses;
kharvox::AerWorldViewHistory aerWorldViewHistory;
kharvox::AerSourceWindow aerSourceWindow;

struct RenderHeadPose {
    float yaw{};
    float artificialYaw{};
    float pitch{};
    float roll{};
    float forward{};
    float lateral{};
    float up{};
    bool valid{};
    float referenceBodyYaw{};
    bool referenceBodyValid{};
    unsigned long long poseId{};
};

RenderHeadPose renderHeadPose() {
    std::lock_guard<std::mutex> guard(headPoseMutex);
    const bool paired = aerRenderPairEnabled.load(std::memory_order_acquire)
        && aerHeadPairSnapshotValid.load(std::memory_order_acquire);
    if (paired) {
        return {
            aerHeadPairPose[0].load(std::memory_order_relaxed),
            aerHeadPairPose[1].load(std::memory_order_relaxed),
            aerHeadPairPose[2].load(std::memory_order_relaxed),
            aerHeadPairPose[3].load(std::memory_order_relaxed),
            aerHeadPairPose[4].load(std::memory_order_relaxed),
            aerHeadPairPose[5].load(std::memory_order_relaxed),
            aerHeadPairPose[6].load(std::memory_order_relaxed),
            aerHeadPairTracked.load(std::memory_order_relaxed),
            aerHeadPairReferenceYaw, aerHeadPairReferenceValid, aerHeadPairPoseId
        };
    }
    return {
        headYaw.load(std::memory_order_relaxed),
        artificialTurnYaw.load(std::memory_order_relaxed),
        headPitch.load(std::memory_order_relaxed),
        headRoll.load(std::memory_order_relaxed),
        headForward.load(std::memory_order_relaxed),
        headLateral.load(std::memory_order_relaxed),
        headUp.load(std::memory_order_relaxed),
        headValid.load(std::memory_order_acquire),
        headReferenceBodyYaw.load(),headReferenceBodyValid.load(),headPoseId
    };
}

void log(const std::string& text) {
    if (!kharvox::extendedDiagnosticsEnabled()) return;
    char temp[MAX_PATH]{};
    GetTempPathA(MAX_PATH, temp);
    std::ofstream out(std::string(temp) + "KHARVOX.log", std::ios::app);
    out << "[KHARVOX][CAMERA] " << text << '\n';
}

void rotateCameraBasis(float* basis, float yaw, float pitch, float roll) {
    if (!basis || (yaw == 0.0f && pitch == 0.0f && roll == 0.0f)) return;
    constexpr float degreesToRadians = 0.01745329251994329577f;
    const float cy = std::cos(yaw * degreesToRadians), sy = std::sin(yaw * degreesToRadians);
    const float cp = std::cos(pitch * degreesToRadians), sp = std::sin(pitch * degreesToRadians);
    const float cr = std::cos(roll * degreesToRadians), sr = std::sin(roll * degreesToRadians);
    for (int column = 0; column < 3; ++column) {
        if (yaw != 0.0f) {
            const float row0 = basis[column];
            const float row1 = basis[3 + column];
            basis[column] = cy * row0 - sy * row1;
            basis[3 + column] = sy * row0 + cy * row1;
        }
        if (pitch != 0.0f) {
            const float row0 = basis[column];
            const float row2 = basis[6 + column];
            basis[column] = cp * row0 + sp * row2;
            basis[6 + column] = -sp * row0 + cp * row2;
        }
        if (roll != 0.0f) {
            const float row1 = basis[3 + column];
            const float row2 = basis[6 + column];
            basis[3 + column] = cr * row1 - sr * row2;
            basis[6 + column] = sr * row1 + cr * row2;
        }
    }
}

void synchronizeAerAnimatedCameraPose(
    uintptr_t callerRva, float* origin, float* axis) {
    if (!aerCameraPairPoses.resolve(callerRva, origin, axis)) return;
    static std::atomic<unsigned long long> synchronizedPairs{};
    const auto pair = synchronizedPairs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (pair <= 4 || pair % 120 == 0) {
        std::ostringstream out;
        out << "[AER-WORLD-PAIR] r260 camera shared across workers within this eye pair #"
            << pair << " callerRva=0x" << std::hex << callerRva;
        log(out.str());
    }
}

unsigned char* findTextSignature(const unsigned char* signature, size_t bytes) {
    auto image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!image) return nullptr;
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    auto section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (std::memcmp(section->Name, ".text", 5)) continue;
        auto begin = image + section->VirtualAddress;
        const size_t size = section->Misc.VirtualSize;
        for (size_t offset = 0; offset + bytes <= size; ++offset) {
            if (!std::memcmp(begin + offset, signature, bytes)) return begin + offset;
        }
    }
    return nullptr;
}

void emit8(unsigned char*& p, unsigned char value) { *p++ = value; }
void emit64(unsigned char*& p, unsigned long long value) {
    std::memcpy(p, &value, sizeof(value));
    p += sizeof(value);
}

extern "C" void __fastcall patchPlayerFocusTrace(void* rawFocusTrace) {
    if (!rawFocusTrace || !KharvoxCameraGameplayActive()
        || KharvoxCameraCutsceneActive() || KharvoxCameraBossSequenceActive()) return;

    float hmdOrigin[3]{};
    float hmdAxis[9]{};
    if (!KharvoxCameraGetHeadRenderPose(hmdOrigin, hmdAxis)) return;

    auto bytes = static_cast<unsigned char*>(rawFocusTrace);
    auto start = reinterpret_cast<float*>(bytes + 0xB0);
    auto nearEnd = reinterpret_cast<float*>(bytes + 0xBC);
    auto farEnd = reinterpret_cast<float*>(bytes + 0xC8);
    const float originalStart[3]{start[0], start[1], start[2]};
    auto distanceFromStart = [&](const float* end) {
        const float x = end[0] - originalStart[0];
        const float y = end[1] - originalStart[1];
        const float z = end[2] - originalStart[2];
        return std::sqrt(x * x + y * y + z * z);
    };
    const float nearDistance = distanceFromStart(nearEnd);
    const float farDistance = distanceFromStart(farEnd);
    if (!std::isfinite(nearDistance) || !std::isfinite(farDistance)
        || nearDistance < 0.01f || farDistance < 0.01f
        || nearDistance > 10000.0f || farDistance > 10000.0f) return;

    // Interaction focus traces from the last view origin along
    // lastViewAxis[0]. Do the equivalent at DOOM 2016's FocusTracker seam:
    // retain both native trace lengths, but replace the body-view origin and
    // direction with the HMD render pose. This never changes player/view yaw.
    for (int axis = 0; axis < 3; ++axis) {
        start[axis] = hmdOrigin[axis];
        nearEnd[axis] = hmdOrigin[axis] + hmdAxis[axis] * nearDistance;
        farEnd[axis] = hmdOrigin[axis] + hmdAxis[axis] * farDistance;
    }

}

bool installPlayerFocusTraceHook(unsigned char* image) {
    if (!image) return false;
    // DOOM 2016 Vulkan FocusTracker.cpp trace construction. The displaced
    // instructions finish the two native endpoints at +0xBC and +0xC8.
    constexpr uintptr_t focusTraceRva = 0xD48938;
    constexpr unsigned char signature[] = {
        0xF3,0x0F,0x11,0xBE,0xD4,0x00,0x00,0x00,
        0x41,0x0F,0x11,0x07,
        0x0F,0x28,0x45,0x80
    };
    auto target = image + focusTraceRva;
    if (std::memcmp(target, signature, sizeof(signature))) {
        log("[FOCUS] FocusTracker trace signature mismatch; HMD USE ray disabled");
        return false;
    }

    auto stub = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!stub) {
        log("[FOCUS] FocusTracker trace stub allocation failed");
        return false;
    }
    auto p = stub;
    // Preserve flags, all volatile GPRs and XMM0-XMM5 around the helper call.
    // Sixty-four bytes of pushes keep the existing 16-byte stack alignment.
    emit8(p,0x9C); emit8(p,0x50); emit8(p,0x51); emit8(p,0x52);
    emit8(p,0x41); emit8(p,0x50); emit8(p,0x41); emit8(p,0x51);
    emit8(p,0x41); emit8(p,0x52); emit8(p,0x41); emit8(p,0x53);
    emit8(p,0x48); emit8(p,0x81); emit8(p,0xEC); emit8(p,0x80); emit8(p,0); emit8(p,0); emit8(p,0);
    constexpr unsigned char saveXmm[] = {
        0xF3,0x0F,0x7F,0x44,0x24,0x20, 0xF3,0x0F,0x7F,0x4C,0x24,0x30,
        0xF3,0x0F,0x7F,0x54,0x24,0x40, 0xF3,0x0F,0x7F,0x5C,0x24,0x50,
        0xF3,0x0F,0x7F,0x64,0x24,0x60, 0xF3,0x0F,0x7F,0x6C,0x24,0x70
    };
    std::memcpy(p, saveXmm, sizeof(saveXmm)); p += sizeof(saveXmm);
    emit8(p,0x48); emit8(p,0x89); emit8(p,0xF1); // rcx = rsi focus trace
    emit8(p,0x48); emit8(p,0xB8); emit64(p,reinterpret_cast<unsigned long long>(patchPlayerFocusTrace));
    emit8(p,0xFF); emit8(p,0xD0);
    constexpr unsigned char loadXmm[] = {
        0xF3,0x0F,0x6F,0x44,0x24,0x20, 0xF3,0x0F,0x6F,0x4C,0x24,0x30,
        0xF3,0x0F,0x6F,0x54,0x24,0x40, 0xF3,0x0F,0x6F,0x5C,0x24,0x50,
        0xF3,0x0F,0x6F,0x64,0x24,0x60, 0xF3,0x0F,0x6F,0x6C,0x24,0x70
    };
    std::memcpy(p, loadXmm, sizeof(loadXmm)); p += sizeof(loadXmm);
    emit8(p,0x48); emit8(p,0x81); emit8(p,0xC4); emit8(p,0x80); emit8(p,0); emit8(p,0); emit8(p,0);
    emit8(p,0x41); emit8(p,0x5B); emit8(p,0x41); emit8(p,0x5A);
    emit8(p,0x41); emit8(p,0x59); emit8(p,0x41); emit8(p,0x58);
    emit8(p,0x5A); emit8(p,0x59); emit8(p,0x58); emit8(p,0x9D);
    std::memcpy(p, signature, sizeof(signature)); p += sizeof(signature);
    emit8(p,0xFF); emit8(p,0x25); emit8(p,0); emit8(p,0); emit8(p,0); emit8(p,0);
    emit64(p,reinterpret_cast<unsigned long long>(target + sizeof(signature)));

    DWORD oldProtect{};
    if (!VirtualProtect(target, sizeof(signature), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        log("[FOCUS] FocusTracker trace target protection failed");
        return false;
    }
    unsigned char jump[sizeof(signature)]{0xFF,0x25,0,0,0,0};
    const auto address = reinterpret_cast<unsigned long long>(stub);
    std::memcpy(jump + 6, &address, sizeof(address));
    for (size_t index = 14; index < sizeof(jump); ++index) jump[index] = 0x90;
    std::memcpy(target, jump, sizeof(jump));
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(jump));
    VirtualProtect(target, sizeof(jump), oldProtect, &oldProtect);
    log("[FOCUS] native FocusTracker HMD view-axis hook installed RVA=0xD48938");
    return true;
}

extern "C" void __fastcall patchPlayerLedgeTraceAxis(
    void* rawLedgeJobData, void* rawLedgeMechanic) {
    if (!rawLedgeJobData || !rawLedgeMechanic
        || !KharvoxCameraGameplayActive() || KharvoxCameraCutsceneActive()) return;

    // idPlayerMechanicLedgeGrab::ledgeGrabState_t is -1 while merely checking
    // the surroundings. Once a grab starts, DOOM deliberately aligns this
    // axis to the detected ledge; never disturb that native animation state.
    const auto mechanicBytes = static_cast<unsigned char*>(rawLedgeMechanic);
    if (*reinterpret_cast<const int*>(mechanicBytes + 0x5258) != -1) return;

    float hmdOrigin[3]{};
    float hmdAxis[9]{};
    if (!KharvoxCameraGetHeadRenderPose(hmdOrigin, hmdAxis)) return;
    const float horizontalLength = std::hypot(hmdAxis[0], hmdAxis[1]);
    if (!std::isfinite(horizontalLength) || horizontalLength < 0.001f) return;

    // At this seam +0x14 is DOOM's already flattened/normalized forward test
    // axis. Replace only that axis. The untouched native code immediately
    // derives +0x20/+0x2C right/up axes and constructs every original ledge
    // trace, so collision, reach, height and surface validation remain native.
    auto forward = reinterpret_cast<float*>(
        static_cast<unsigned char*>(rawLedgeJobData) + 0x14);
    forward[0] = hmdAxis[0] / horizontalLength;
    forward[1] = hmdAxis[1] / horizontalLength;
    forward[2] = 0.0f;

}

bool installPlayerLedgeTraceAxisHook(unsigned char* image) {
    if (!image) return false;
    // idPlayerMechanicLedgeGrab prepares its normalized forward axis before
    // deriving the perpendicular trace basis at this instruction boundary.
    constexpr uintptr_t ledgeAxisRva = 0xDA1870;
    constexpr unsigned char signature[] = {
        0x48,0x8B,0x43,0x10,
        0x48,0x8B,0x88,0x08,0x08,0x00,0x00,
        0x48,0x8B,0x01
    };
    auto target = image + ledgeAxisRva;
    if (std::memcmp(target, signature, sizeof(signature))) {
        log("[LEDGE] player ledge trace-axis signature mismatch; HMD ledge direction disabled");
        return false;
    }

    auto stub = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!stub) {
        log("[LEDGE] player ledge trace-axis stub allocation failed");
        return false;
    }
    auto p = stub;
    emit8(p,0x9C); emit8(p,0x50); emit8(p,0x51); emit8(p,0x52);
    emit8(p,0x41); emit8(p,0x50); emit8(p,0x41); emit8(p,0x51);
    emit8(p,0x41); emit8(p,0x52); emit8(p,0x41); emit8(p,0x53);
    emit8(p,0x48); emit8(p,0x81); emit8(p,0xEC); emit8(p,0x80); emit8(p,0); emit8(p,0); emit8(p,0);
    constexpr unsigned char saveXmm[] = {
        0xF3,0x0F,0x7F,0x44,0x24,0x20, 0xF3,0x0F,0x7F,0x4C,0x24,0x30,
        0xF3,0x0F,0x7F,0x54,0x24,0x40, 0xF3,0x0F,0x7F,0x5C,0x24,0x50,
        0xF3,0x0F,0x7F,0x64,0x24,0x60, 0xF3,0x0F,0x7F,0x6C,0x24,0x70
    };
    std::memcpy(p, saveXmm, sizeof(saveXmm)); p += sizeof(saveXmm);
    emit8(p,0x48); emit8(p,0x89); emit8(p,0xC1); // rcx = rax ledge job data
    emit8(p,0x48); emit8(p,0x89); emit8(p,0xDA); // rdx = rbx ledge mechanic
    emit8(p,0x48); emit8(p,0xB8); emit64(p,reinterpret_cast<unsigned long long>(patchPlayerLedgeTraceAxis));
    emit8(p,0xFF); emit8(p,0xD0);
    constexpr unsigned char loadXmm[] = {
        0xF3,0x0F,0x6F,0x44,0x24,0x20, 0xF3,0x0F,0x6F,0x4C,0x24,0x30,
        0xF3,0x0F,0x6F,0x54,0x24,0x40, 0xF3,0x0F,0x6F,0x5C,0x24,0x50,
        0xF3,0x0F,0x6F,0x64,0x24,0x60, 0xF3,0x0F,0x6F,0x6C,0x24,0x70
    };
    std::memcpy(p, loadXmm, sizeof(loadXmm)); p += sizeof(loadXmm);
    emit8(p,0x48); emit8(p,0x81); emit8(p,0xC4); emit8(p,0x80); emit8(p,0); emit8(p,0); emit8(p,0);
    emit8(p,0x41); emit8(p,0x5B); emit8(p,0x41); emit8(p,0x5A);
    emit8(p,0x41); emit8(p,0x59); emit8(p,0x41); emit8(p,0x58);
    emit8(p,0x5A); emit8(p,0x59); emit8(p,0x58); emit8(p,0x9D);
    std::memcpy(p, signature, sizeof(signature)); p += sizeof(signature);
    emit8(p,0xFF); emit8(p,0x25); emit8(p,0); emit8(p,0); emit8(p,0); emit8(p,0);
    emit64(p,reinterpret_cast<unsigned long long>(target + sizeof(signature)));

    DWORD oldProtect{};
    if (!VirtualProtect(target, sizeof(signature), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        log("[LEDGE] player ledge trace-axis target protection failed");
        return false;
    }
    unsigned char jump[sizeof(signature)]{0xFF,0x25,0,0,0,0};
    const auto address = reinterpret_cast<unsigned long long>(stub);
    std::memcpy(jump + 6, &address, sizeof(address));
    std::memcpy(target, jump, sizeof(jump));
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(jump));
    VirtualProtect(target, sizeof(jump), oldProtect, &oldProtect);
    log("[LEDGE] native HMD horizontal trace-axis hook installed RVA=0xDA1870");
    return true;
}

extern "C" void __fastcall capturePlayerLedgeTransition(
    void* rawLedgeMechanic, int newState) {
    if (!rawLedgeMechanic) return;
    const auto mechanic = static_cast<unsigned char*>(rawLedgeMechanic);
    const uintptr_t owner = *reinterpret_cast<const uintptr_t*>(mechanic + 0x10);
    playerLedgeTransitionOwner.store(owner, std::memory_order_relaxed);
    playerLedgeTransitionActive.store(newState != -1, std::memory_order_release);
}

bool installPlayerLedgeStateCapture(unsigned char* image) {
    if (!image) return false;
    // Tail of idPlayerMechanicLedgeGrab::SetState. The first two writes retain
    // the old state and the third commits EBP as the new native state.
    constexpr uintptr_t stateCommitRva = 0xDA24A3;
    constexpr unsigned char signature[] = {
        0x8B,0x86,0x58,0x52,0x00,0x00,
        0x89,0x86,0x5C,0x52,0x00,0x00,
        0x89,0xAE,0x58,0x52,0x00,0x00
    };
    auto target = image + stateCommitRva;
    if (std::memcmp(target, signature, sizeof(signature))) {
        log("[CINEMATIC-CLASS] Ledge state signature mismatch; Jump/Ledge exception disabled");
        return false;
    }

    auto stub = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!stub) {
        log("[CINEMATIC-CLASS] Ledge state capture stub allocation failed");
        return false;
    }
    auto p = stub;
    std::memcpy(p, signature, sizeof(signature)); p += sizeof(signature);
    emit8(p,0x9C); emit8(p,0x50); emit8(p,0x51); emit8(p,0x52);
    emit8(p,0x41); emit8(p,0x50); emit8(p,0x41); emit8(p,0x51);
    emit8(p,0x41); emit8(p,0x52); emit8(p,0x41); emit8(p,0x53);
    emit8(p,0x48); emit8(p,0x81); emit8(p,0xEC); emit8(p,0x80); emit8(p,0); emit8(p,0); emit8(p,0);
    constexpr unsigned char saveXmm[] = {
        0xF3,0x0F,0x7F,0x44,0x24,0x20, 0xF3,0x0F,0x7F,0x4C,0x24,0x30,
        0xF3,0x0F,0x7F,0x54,0x24,0x40, 0xF3,0x0F,0x7F,0x5C,0x24,0x50,
        0xF3,0x0F,0x7F,0x64,0x24,0x60, 0xF3,0x0F,0x7F,0x6C,0x24,0x70
    };
    std::memcpy(p, saveXmm, sizeof(saveXmm)); p += sizeof(saveXmm);
    emit8(p,0x48); emit8(p,0x89); emit8(p,0xF1); // rcx = rsi mechanic
    emit8(p,0x8B); emit8(p,0xD5);               // edx = ebp new state
    emit8(p,0x48); emit8(p,0xB8); emit64(p,reinterpret_cast<unsigned long long>(capturePlayerLedgeTransition));
    emit8(p,0xFF); emit8(p,0xD0);
    constexpr unsigned char loadXmm[] = {
        0xF3,0x0F,0x6F,0x44,0x24,0x20, 0xF3,0x0F,0x6F,0x4C,0x24,0x30,
        0xF3,0x0F,0x6F,0x54,0x24,0x40, 0xF3,0x0F,0x6F,0x5C,0x24,0x50,
        0xF3,0x0F,0x6F,0x64,0x24,0x60, 0xF3,0x0F,0x6F,0x6C,0x24,0x70
    };
    std::memcpy(p, loadXmm, sizeof(loadXmm)); p += sizeof(loadXmm);
    emit8(p,0x48); emit8(p,0x81); emit8(p,0xC4); emit8(p,0x80); emit8(p,0); emit8(p,0); emit8(p,0);
    emit8(p,0x41); emit8(p,0x5B); emit8(p,0x41); emit8(p,0x5A);
    emit8(p,0x41); emit8(p,0x59); emit8(p,0x41); emit8(p,0x58);
    emit8(p,0x5A); emit8(p,0x59); emit8(p,0x58); emit8(p,0x9D);
    emit8(p,0xFF); emit8(p,0x25); emit8(p,0); emit8(p,0); emit8(p,0); emit8(p,0);
    emit64(p,reinterpret_cast<unsigned long long>(target + sizeof(signature)));

    DWORD oldProtect{};
    if (!VirtualProtect(target, sizeof(signature), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        log("[CINEMATIC-CLASS] Ledge state target protection failed");
        return false;
    }
    unsigned char jump[sizeof(signature)]{0xFF,0x25,0,0,0,0};
    const auto address = reinterpret_cast<unsigned long long>(stub);
    std::memcpy(jump + 6, &address, sizeof(address));
    for (size_t index = 14; index < sizeof(jump); ++index) jump[index] = 0x90;
    std::memcpy(target, jump, sizeof(jump));
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(jump));
    VirtualProtect(target, sizeof(jump), oldProtect, &oldProtect);
    log("[CINEMATIC-CLASS] native Jump/Ledge state capture installed RVA=0xDA24A3");
    return true;
}

bool validateSyncAttackClassifier(unsigned char* image) {
    if (!image) return false;
    constexpr uintptr_t startSyncAttackRva = 0xDA7980;
    constexpr unsigned char startSignature[] = {
        0x48,0x8B,0xC4,0x57,0x41,0x54,0x41,0x55,
        0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x60
    };
    // The native usability path reads idPlayer+0x3DC9 immediately before its
    // explicit "instigating a sync attack" diagnostic.
    constexpr uintptr_t instigatorFlagReadRva = 0x9B2908;
    constexpr unsigned char flagReadSignature[] = {
        0x80,0xB8,0xC9,0x3D,0x00,0x00,0x00,0x74,0x26
    };
    const bool supported = !std::memcmp(
            image + startSyncAttackRva, startSignature, sizeof(startSignature))
        && !std::memcmp(image + instigatorFlagReadRva,
            flagReadSignature, sizeof(flagReadSignature));
    syncAttackClassifierSupported.store(supported, std::memory_order_release);
    log(supported
        ? "[CINEMATIC-CLASS] native Glory Kill sync-state classifier validated RVA=0xDA7980"
        : "[CINEMATIC-CLASS] sync-state signature mismatch; Glory Kill exception disabled");
    return supported;
}

bool validatePlayerControlClassifier(unsigned char* image) {
    if (!image) return false;
    // DOOM 6.66 reflection record for idPlayer::inhibitFlags. The low dword is
    // the idPlayer field offset; the high dword identifies a four-byte value.
    // Validate the record before reading the live player object so an unknown
    // executable revision fails closed to the Comfort Cinewindow.
    constexpr uintptr_t inhibitFlagsRecordRva = 0x30AACD8;
    constexpr unsigned char reflectedFieldSignature[]{
        0x4C,0x4C,0x01,0x00, 0x04,0x00,0x00,0x00
    };
    const bool supported = !std::memcmp(
        image + inhibitFlagsRecordRva + 0x18,
        reflectedFieldSignature, sizeof(reflectedFieldSignature));
    playerControlClassifierSupported.store(supported, std::memory_order_release);
    log(supported
        ? "[COMFORT-CINEMATIC] native idPlayer inhibitFlags classifier validated offset=0x14C4C"
        : "[COMFORT-CINEMATIC] player-control metadata mismatch; interactive Projection exception disabled");
    return supported;
}

float interlockedFloatLoad(volatile LONG* bits) {
    const LONG value = InterlockedCompareExchange(bits, 0, 0);
    float result{};
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

void interlockedFloatStore(volatile LONG* bits, float value) {
    LONG encoded{};
    std::memcpy(&encoded, &value, sizeof(encoded));
    InterlockedExchange(bits, encoded);
}

bool immersiveCinematicFov(float& fovX, float& fovY) {
    if (InterlockedCompareExchange(&immersiveCinematicFovRequested, 0, 0) == 0)
        return false;
    fovX = interlockedFloatLoad(&immersiveCinematicFovXBits);
    fovY = interlockedFloatLoad(&immersiveCinematicFovYBits);
    return std::isfinite(fovX) && std::isfinite(fovY)
        && fovX > 1.0f && fovY > 1.0f;
}

using PlayerViewOriginFn = const float*(__fastcall*)(void*);
using PlayerViewAxisFn = const float*(__fastcall*)(void*);
using PhysicsGetOriginFn = const float*(__fastcall*)(void*, int);
std::atomic<PlayerViewAxisFn> originalPlayerViewAxis{};
std::atomic<bool> playerViewAxisHookInstalled{};

bool readableMemory(const void* address, size_t bytes) {
    if (!address || !bytes) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT) return false;
    if ((info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    const auto begin = reinterpret_cast<uintptr_t>(address);
    const auto end = begin + bytes;
    const auto regionEnd = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    return end >= begin && end <= regionEnd;
}

bool executableMemory(const void* address) {
    if (!address) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT) return false;
    if ((info.Protect & PAGE_GUARD) != 0) return false;
    const DWORD protection = info.Protect & 0xFF;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ
        || protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

bool invokePhysicsGetOriginSafely(PhysicsGetOriginFn getOrigin, void* physics, float origin[3]) {
    if (!getOrigin || !physics || !origin) return false;
    const float* current{};
#if defined(_MSC_VER)
    __try {
        current = getOrigin(physics, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    current = getOrigin(physics, 0);
#endif
    if (!readableMemory(current, 3 * sizeof(float))
        || !std::isfinite(current[0]) || !std::isfinite(current[1]) || !std::isfinite(current[2]))
        return false;
    for (int axis = 0; axis < 3; ++axis) origin[axis] = current[axis];
    return true;
}

void invalidateLevelReferences() {
    const bool hadPlayer = playerPhysicsOriginValid.exchange(false, std::memory_order_acq_rel);
    playerPhysicsOwner.store(0, std::memory_order_release);
    playerLedgeTransitionOwner.store(0, std::memory_order_release);
    playerLedgeTransitionActive.store(false, std::memory_order_release);
    playerPhysicsCapturePresent.store(0, std::memory_order_release);
    stableBodyViewOffsetValid.store(false, std::memory_order_release);
    stableBodyViewOffsetOwner.store(0, std::memory_order_release);
    bodyCameraValid.store(false, std::memory_order_release);
    preCinematicGameplayAxisValid.store(false, std::memory_order_release);
    if (hadPlayer) {
        const auto generation = levelTransitionGeneration.fetch_add(
            1, std::memory_order_acq_rel) + 1;
        log("level transition: stale player/body references invalidated generation="
            + std::to_string(generation));
    }
}

bool readLivePlayerPhysicsOrigin(float origin[3], uintptr_t* ownerOut = nullptr) {
    if (!origin || !worldCameraActive.load(std::memory_order_acquire)
        || !playerPhysicsOriginValid.load(std::memory_order_acquire)) return false;
    const auto present = cameraPresentSerial.load(std::memory_order_acquire);
    const auto captured = playerPhysicsCapturePresent.load(std::memory_order_acquire);
    if (!captured || present < captured || present - captured > 2) return false;
    const uintptr_t owner = playerPhysicsOwner.load(std::memory_order_acquire);
    if (!owner || owner > UINTPTR_MAX - 0x14E58) return false;
    auto physics = reinterpret_cast<unsigned char*>(owner + 0x14E58);
    if (!readableMemory(physics, sizeof(void*))) return false;
    auto physicsVtable = *reinterpret_cast<void***>(physics);
    constexpr size_t getOriginSlot = 0x80 / sizeof(void*);
    if (!readableMemory(physicsVtable, (getOriginSlot + 1) * sizeof(void*))) return false;
    auto getOrigin = reinterpret_cast<PhysicsGetOriginFn>(physicsVtable[getOriginSlot]);
    if (!executableMemory(reinterpret_cast<const void*>(getOrigin))) return false;
    if (!invokePhysicsGetOriginSafely(getOrigin, physics, origin)) return false;
    if (!playerPhysicsOriginValid.load(std::memory_order_acquire)
        || playerPhysicsOwner.load(std::memory_order_acquire) != owner) return false;
    if (ownerOut) *ownerOut = owner;
    return true;
}

RenderHeadPose renderHeadPoseForBody(const float axis[9]) {
    auto head=renderHeadPose();
    if(head.valid&&head.referenceBodyValid&&KharvoxCameraGameplayActive()
        &&!KharvoxCameraCutsceneActive()&&!animatedSequenceActive.load()){
        const float bodyYaw=std::atan2(axis[1],axis[0])*57.2957795131f;
        kharvox::rebaseHeadToBody(head.referenceBodyYaw,bodyYaw,head.yaw,head.forward,head.lateral);
    }
    return head;
}
void applyResidualHeadOrientation(float axis[9],const RenderHeadPose& head);
void applyResidualHeadOrientation(float axis[9]) {
    const auto head=renderHeadPoseForBody(axis);applyResidualHeadOrientation(axis,head);
}
void applyResidualHeadOrientation(float axis[9],const RenderHeadPose& head) {
    constexpr float degreesToRadians = 0.01745329251994329577f;
    const float yaw = -(head.yaw + head.artificialYaw) * degreesToRadians;
    const float pitch = head.pitch * degreesToRadians;
    const float roll = head.roll * degreesToRadians;
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float cr = std::cos(roll), sr = std::sin(roll);
    for (int column = 0; column < 3; ++column) {
        const float yawRow0 = axis[column];
        const float yawRow1 = axis[3 + column];
        axis[column] = cy * yawRow0 - sy * yawRow1;
        axis[3 + column] = sy * yawRow0 + cy * yawRow1;

        const float pitchRow0 = axis[column];
        const float pitchRow2 = axis[6 + column];
        axis[column] = cp * pitchRow0 + sp * pitchRow2;
        axis[6 + column] = -sp * pitchRow0 + cp * pitchRow2;

        const float rollRow1 = axis[3 + column];
        const float rollRow2 = axis[6 + column];
        axis[3 + column] = cr * rollRow1 - sr * rollRow2;
        axis[6 + column] = sr * rollRow1 + cr * rollRow2;
    }
}

void publishHudCenterRenderPose(
    const float anchorOrigin[3], const float anchorAxis[9]) {
    // One sequence-locked snapshot ties the HUD to the exact camera struct
    // consumed by this render pass. Reconstructing it later from live player
    // physics and tracking atomics mixes different update loops.
    hudRenderPoseSequence.fetch_add(1, std::memory_order_acq_rel);
    for (int index = 0; index < 3; ++index) {
        hudRenderAnchorOrigin[index].store(anchorOrigin[index], std::memory_order_relaxed);
    }
    for (int index = 0; index < 9; ++index) {
        hudRenderAnchorAxis[index].store(anchorAxis[index], std::memory_order_relaxed);
    }
    hudRenderPosePresent.store(
        cameraPresentSerial.load(std::memory_order_relaxed), std::memory_order_relaxed);
    hudRenderPoseValid.store(true, std::memory_order_relaxed);
    hudRenderPoseSequence.fetch_add(1, std::memory_order_release);
}

extern "C" const float* __fastcall getVRGameplayViewAxis(void* player) {
    auto original = originalPlayerViewAxis.load(std::memory_order_acquire);
    if (!original) return nullptr;
    const float* nativeAxis = original(player);
    if (!nativeAxis || !readableMemory(nativeAxis, 9 * sizeof(float))
        || !headValid.load(std::memory_order_acquire)
        || !worldCameraActive.load(std::memory_order_acquire)
        || cutsceneActive.load(std::memory_order_acquire)
        || reinterpret_cast<uintptr_t>(player) != playerPhysicsOwner.load(std::memory_order_acquire)
        || KharvoxCameraBossSequenceActive())
        return nativeAxis;

    const auto imageBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const uintptr_t callerRva = imageBase && caller >= imageBase ? caller - imageBase : 0;
    // The normal gameplay render camera obtains this axis at RVA 0xE3F8F6
    // (return address 0xE3F8FC). CameraHook composes OpenXR orientation there
    // after publishing the untouched native body pose. Returning the combined
    // axis to that one caller would apply HMD orientation twice.
    if (callerRva == 0xE3F8FC) return nativeAxis;

    thread_local std::array<float, 9> combinedAxis{};
    std::memcpy(combinedAxis.data(), nativeAxis, sizeof(combinedAxis));
    applyResidualHeadOrientation(combinedAxis.data());

    return combinedAxis.data();
}

bool installPlayerViewAxisAdapter(void* player) {
    if (!player) return false;
    auto vtable = *reinterpret_cast<void***>(player);
    constexpr size_t viewAxisSlot = 0x368 / sizeof(void*);
    if (!readableMemory(vtable, (viewAxisSlot + 1) * sizeof(void*))) return false;
    auto slot = vtable + viewAxisSlot;
    const auto adapter = reinterpret_cast<void*>(getVRGameplayViewAxis);
    if (*slot == adapter) return true;
    if (playerViewAxisHookInstalled.load(std::memory_order_acquire)) return false;
    auto original = reinterpret_cast<PlayerViewAxisFn>(*slot);
    if (!executableMemory(reinterpret_cast<const void*>(original))) return false;
    originalPlayerViewAxis.store(original, std::memory_order_release);

    DWORD oldProtect{};
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        originalPlayerViewAxis.store(nullptr, std::memory_order_release);
        log("[VIEW] idPlayer ViewAxis vtable protection failed");
        return false;
    }
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(slot), adapter);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    DWORD ignored{};
    VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
    playerViewAxisHookInstalled.store(true, std::memory_order_release);
    log("[VIEW] idPlayer gameplay ViewAxis adapter installed at vtable slot 0x368");
    return true;
}

extern "C" const float* __fastcall capturePlayerPhysicsOrigin(void* player) {
    if (!player) return nullptr;
    auto playerVtable = *reinterpret_cast<void***>(player);
    if (!playerVtable) return nullptr;
    installPlayerViewAxisAdapter(player);
    auto originalViewOrigin = reinterpret_cast<PlayerViewOriginFn>(playerVtable[0x370 / sizeof(void*)]);
    if (!originalViewOrigin) return nullptr;

    auto physics = static_cast<unsigned char*>(player) + 0x14E58;
    auto physicsVtable = *reinterpret_cast<void***>(physics);
    if (physicsVtable) {
        auto getOrigin = reinterpret_cast<PhysicsGetOriginFn>(physicsVtable[0x80 / sizeof(void*)]);
        if (getOrigin) {
            float origin[3]{};
            if (invokePhysicsGetOriginSafely(getOrigin, physics, origin)) {
                for (int axis = 0; axis < 3; ++axis)
                    playerPhysicsOrigin[axis].store(origin[axis], std::memory_order_relaxed);
                playerPhysicsOwner.store(reinterpret_cast<uintptr_t>(player), std::memory_order_relaxed);
                playerPhysicsCapturePresent.store(
                    cameraPresentSerial.load(std::memory_order_relaxed), std::memory_order_relaxed);
                playerPhysicsOriginValid.store(true, std::memory_order_release);
            }
        }
    }
    KharvoxWeaponCaptureAmmoSnapshot(player);
    return originalViewOrigin(player);
}

bool installPlayerPhysicsOriginCapture(unsigned char* image) {
    constexpr uintptr_t callsiteRva = 0xE3F8D3;
    auto target = image + callsiteRva;
    constexpr unsigned char signature[] = {
        0x48,0x8B,0x03,
        0x48,0x8B,0xCB,
        0xFF,0x90,0x70,0x03,0x00,0x00,
        0x48,0x8B,0xCB
    };
    if (std::memcmp(target, signature, sizeof(signature))) {
        log("player physics-origin capture signature mismatch; stable weapon anchor disabled");
        return false;
    }

    auto stub = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, 96, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!stub) {
        log("player physics-origin capture stub allocation failed");
        return false;
    }
    auto p = stub;
    emit8(p, 0x48); emit8(p, 0x83); emit8(p, 0xEC); emit8(p, 0x20);
    emit8(p, 0x48); emit8(p, 0x8B); emit8(p, 0xCB);
    emit8(p, 0x48); emit8(p, 0xB8);
    emit64(p, reinterpret_cast<unsigned long long>(capturePlayerPhysicsOrigin));
    emit8(p, 0xFF); emit8(p, 0xD0);
    emit8(p, 0x48); emit8(p, 0x83); emit8(p, 0xC4); emit8(p, 0x20);
    emit8(p, 0x48); emit8(p, 0x8B); emit8(p, 0xCB);
    emit8(p, 0xFF); emit8(p, 0x25); emit8(p, 0); emit8(p, 0); emit8(p, 0); emit8(p, 0);
    emit64(p, reinterpret_cast<unsigned long long>(target + sizeof(signature)));

    DWORD oldProtect{};
    if (!VirtualProtect(target, sizeof(signature), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        log("player physics-origin capture target protection failed");
        return false;
    }
    unsigned char jump[sizeof(signature)]{0xFF,0x25,0,0,0,0};
    const auto stubAddress = reinterpret_cast<unsigned long long>(stub);
    std::memcpy(jump + 6, &stubAddress, sizeof(stubAddress));
    jump[14] = 0x90;
    std::memcpy(target, jump, sizeof(jump));
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(jump));
    DWORD ignored{};
    VirtualProtect(target, sizeof(jump), oldProtect, &ignored);
    log("stable idPhysics_Player origin capture installed at gameplay RVA 0xE3F8D3");
    return true;
}

void selectConfiguredEye(VREye eye) {
    if (!configuredStereo.load(std::memory_order_acquire) || eye == VREye::Mono) return;
    currentRenderEye.store(eye, std::memory_order_release);
    const size_t index = eye == VREye::Left ? 0 : 1;
    stereoEyeOffset.store(configuredEyeOffset[index].load(std::memory_order_relaxed), std::memory_order_relaxed);
    stereoFovX.store(configuredEyeFovX[index].load(std::memory_order_relaxed), std::memory_order_relaxed);
    stereoFovY.store(configuredEyeFovY[index].load(std::memory_order_relaxed), std::memory_order_relaxed);
    stereoOpticalCenterYaw.store(configuredEyeOpticalCenterYaw[index].load(std::memory_order_relaxed), std::memory_order_relaxed);
    stereoOpticalCenterPitch.store(configuredEyeOpticalCenterPitch[index].load(std::memory_order_relaxed), std::memory_order_relaxed);
    stereoEyeEnabled.store(true, std::memory_order_release);
}

extern "C" void* __fastcall nativeShaderParseContextHook(void* destination, void* source, unsigned int mode) {
    if (nativeStereoLayoutEnabled.load(std::memory_order_acquire)) {
        // The parser takes almost all remaining bytes from its shared arena.
        // Native two-view startup can construct contexts for the same pool on
        // multiple worker threads, so checking/growing the arena and running
        // the constructor must be one operation. Otherwise another thread can
        // observe the freshly grown arena before the first constructor advances
        // its cursor and both contexts receive the same storage.
        std::lock_guard<std::mutex> lock(shaderArenaMutex);
        if (source) {
        auto pool = *reinterpret_cast<unsigned char**>(static_cast<unsigned char*>(source) + 0x98);
        if (pool) {
            auto end = *reinterpret_cast<unsigned char**>(pool + 0x10);
            auto current = *reinterpret_cast<unsigned char**>(pool + 0x18);
            const size_t remaining = end && current && end >= current ? size_t(end - current) : 0;
            if (remaining < 0x50) {
                ShaderArenaExpansion* expansion{};
                for (auto& candidate : shaderArenaExpansions) {
                    if (!candidate.pool) {
                        expansion = &candidate;
                        break;
                    }
                }
                if (expansion) {
                    constexpr size_t expansionBytes = 8ull * 1024ull * 1024ull;
                    expansion->pool = pool;
                    expansion->bytes = expansionBytes;
                    expansion->memory = static_cast<unsigned char*>(VirtualAlloc(
                        nullptr, expansion->bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
                    if (expansion->memory) {
                        // This third-party shader parser receives external arena
                        // storage and never owns/frees it. Keep every emergency
                        // block alive for the process: a previously returned
                        // parser context can still reference its generation.
                        *reinterpret_cast<unsigned char**>(pool + 0x08) = expansion->memory;
                        *reinterpret_cast<unsigned char**>(pool + 0x10) = expansion->memory + expansion->bytes;
                        *reinterpret_cast<unsigned char**>(pool + 0x18) = expansion->memory;
                        *reinterpret_cast<size_t*>(pool + 0x20) = expansion->bytes;
                        std::ostringstream out;
                        out << "[Stereo] expanded exhausted Vulkan shader arena pool=0x" << std::hex
                            << reinterpret_cast<uintptr_t>(pool) << std::dec
                            << " oldRemaining=" << remaining << " newBytes=" << expansion->bytes
                            << " mode=" << mode;
                        log(out.str());
                    } else {
                        log("[Stereo] Vulkan shader arena expansion allocation failed");
                    }
                } else {
                    log("[Stereo] Vulkan shader arena expansion slots exhausted");
                }
            }
        }
        }
        return originalShaderParseContext
            ? originalShaderParseContext(destination, source, mode)
            : nullptr;
    }
    return originalShaderParseContext
        ? originalShaderParseContext(destination, source, mode)
        : nullptr;
}

bool installNativeShaderArenaGuard(unsigned char* image) {
    constexpr uintptr_t shaderParseContextRva = 0x1CCD5A0;
    auto target = image + shaderParseContextRva;
    constexpr unsigned char signature[] = {
        0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x89,0x11,0x48,
        0x8B,0xD9,0x4C,0x8B,0x9A,0x98,0x00,0x00,0x00
    };
    if (std::memcmp(target, signature, sizeof(signature))) {
        log("[Stereo] Vulkan shader parse-context signature mismatch; native stereo disabled");
        return false;
    }
    auto trampoline = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        log("[Stereo] Vulkan shader arena trampoline allocation failed");
        return false;
    }
    auto cursor = trampoline;
    std::memcpy(cursor, signature, sizeof(signature));
    cursor += sizeof(signature);
    emit8(cursor, 0xFF); emit8(cursor, 0x25);
    emit8(cursor, 0); emit8(cursor, 0); emit8(cursor, 0); emit8(cursor, 0);
    emit64(cursor, reinterpret_cast<unsigned long long>(target + sizeof(signature)));
    originalShaderParseContext = reinterpret_cast<ShaderParseContextFn>(trampoline);

    DWORD oldProtect{};
    if (!VirtualProtect(target, sizeof(signature), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        originalShaderParseContext = nullptr;
        VirtualFree(trampoline, 0, MEM_RELEASE);
        log("[Stereo] Vulkan shader arena target protection failed");
        return false;
    }
    unsigned char jump[sizeof(signature)]{0xFF,0x25,0,0,0,0};
    const auto wrapper = reinterpret_cast<unsigned long long>(nativeShaderParseContextHook);
    std::memcpy(jump + 6, &wrapper, sizeof(wrapper));
    for (size_t index = 14; index < sizeof(jump); ++index) jump[index] = 0x90;
    std::memcpy(target, jump, sizeof(jump));
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(jump));
    VirtualProtect(target, sizeof(jump), oldProtect, &oldProtect);
    log("[Stereo] Vulkan shader arena exhaustion guard installed RVA=0x1CCD5A0");
    return true;
}

extern "C" void __fastcall nativeVulkanBackendResizeHook(void* context) {
    const auto packed = reinterpret_cast<uintptr_t>(context);
    const auto width = static_cast<unsigned>(packed & 0xFFFFFFFFu);
    const auto height = static_cast<unsigned>(packed >> 32);
    const bool looksLikePackedExtent = width > 0 && width <= 16384
        && height > 0 && height <= 16384;
    if (nativeStereoLayoutEnabled.load(std::memory_order_acquire) && looksLikePackedExtent) {
        auto replacement = lastValidVulkanBackendContext.load(std::memory_order_acquire);
        std::ostringstream out;
        out << "[Stereo] Vulkan backend context repaired packedExtent="
            << width << 'x' << height << " replacement=0x" << std::hex
            << reinterpret_cast<uintptr_t>(replacement);
        log(out.str());
        if (replacement && originalVulkanBackendResize) originalVulkanBackendResize(replacement);
        return;
    }
    if (context && !looksLikePackedExtent)
        lastValidVulkanBackendContext.store(context, std::memory_order_release);
    if (originalVulkanBackendResize) originalVulkanBackendResize(context);
}

bool installNativeVulkanBackendGuard(unsigned char* image) {
    constexpr uintptr_t backendResizeRva = 0x17B9330;
    auto target = image + backendResizeRva;
    constexpr unsigned char signature[] = {
        0x40,0x56,0x57,0x41,0x56,0x48,0x83,0xEC,0x40,
        0x48,0xC7,0x44,0x24,0x30,0xFE,0xFF,0xFF,0xFF
    };
    if (std::memcmp(target, signature, sizeof(signature))) {
        log("[Stereo] Vulkan backend-resize signature mismatch; native stereo disabled");
        return false;
    }
    auto trampoline = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        log("[Stereo] Vulkan backend-resize trampoline allocation failed");
        return false;
    }
    auto cursor = trampoline;
    std::memcpy(cursor, signature, sizeof(signature));
    cursor += sizeof(signature);
    emit8(cursor, 0xFF); emit8(cursor, 0x25);
    emit8(cursor, 0); emit8(cursor, 0); emit8(cursor, 0); emit8(cursor, 0);
    emit64(cursor, reinterpret_cast<unsigned long long>(target + sizeof(signature)));
    originalVulkanBackendResize = reinterpret_cast<VulkanBackendResizeFn>(trampoline);

    DWORD oldProtect{};
    if (!VirtualProtect(target, sizeof(signature), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        originalVulkanBackendResize = nullptr;
        VirtualFree(trampoline, 0, MEM_RELEASE);
        log("[Stereo] Vulkan backend-resize target protection failed");
        return false;
    }
    unsigned char jump[sizeof(signature)]{0xFF,0x25,0,0,0,0};
    const auto wrapper = reinterpret_cast<unsigned long long>(nativeVulkanBackendResizeHook);
    std::memcpy(jump + 6, &wrapper, sizeof(wrapper));
    for (size_t index = 14; index < sizeof(jump); ++index) jump[index] = 0x90;
    std::memcpy(target, jump, sizeof(jump));
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(jump));
    VirtualProtect(target, sizeof(jump), oldProtect, &oldProtect);
    log("[Stereo] Vulkan packed-extent backend guard installed RVA=0x17B9330");
    return true;
}

extern "C" void __fastcall nativeVulkanBackendOwnerInitHook(void* owner) {
    if (owner) {
        auto contextSlot = reinterpret_cast<void* volatile*>(static_cast<unsigned char*>(owner) + 0x178);
        auto timingContextSlot = reinterpret_cast<void* volatile*>(static_cast<unsigned char*>(owner) + 0x240);
        auto context = *contextSlot;
        auto timingContext = *timingContextSlot;
        const auto packed = reinterpret_cast<uintptr_t>(context);
        const auto width = static_cast<unsigned>(packed & 0xFFFFFFFFu);
        const auto height = static_cast<unsigned>(packed >> 32);
        const bool looksLikePackedExtent = width > 0 && width <= 16384
            && height > 0 && height <= 16384;
        const bool nativeStereo = nativeStereoLayoutEnabled.load(std::memory_order_acquire);
        if (!nativeStereo) {
            if (!looksLikePackedExtent && context)
                lastValidVulkanBackendContext.store(context, std::memory_order_release);
            if (timingContext)
                lastValidVulkanBackendTimingContext.store(timingContext, std::memory_order_release);
        } else if (!looksLikePackedExtent && context) {
            lastValidVulkanBackendContext.store(context, std::memory_order_release);
        } else if (looksLikePackedExtent) {
            auto replacement = lastValidVulkanBackendContext.load(std::memory_order_acquire);
            if (replacement) {
                InterlockedExchangePointer(contextSlot, replacement);
                std::ostringstream out;
                out << "[Stereo] repaired Vulkan backend owner +0x178 packedExtent="
                    << width << 'x' << height << " replacement=0x" << std::hex
                    << reinterpret_cast<uintptr_t>(replacement);
                log(out.str());
            }
        }
        if (nativeStereo) {
            auto timingReplacement = lastValidVulkanBackendTimingContext.load(std::memory_order_acquire);
            if (timingReplacement && timingContext != timingReplacement) {
                InterlockedExchangePointer(timingContextSlot, timingReplacement);
                std::ostringstream out;
                out << "[Stereo] repaired Vulkan backend owner +0x240 old=0x" << std::hex
                    << reinterpret_cast<uintptr_t>(timingContext) << " replacement=0x"
                    << reinterpret_cast<uintptr_t>(timingReplacement);
                log(out.str());
            }
        }
    }
    if (originalVulkanBackendOwnerInit) originalVulkanBackendOwnerInit(owner);
}

bool installNativeVulkanBackendOwnerGuard(unsigned char* image) {
    constexpr uintptr_t backendOwnerInitRva = 0x186E000;
    auto target = image + backendOwnerInitRva;
    constexpr unsigned char signature[] = {
        0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,
        0x57,0x48,0x83,0xEC,0x30,0x48,0x8B,0xF9
    };
    if (std::memcmp(target, signature, sizeof(signature))) {
        log("[Stereo] Vulkan backend-owner signature mismatch; native stereo disabled");
        return false;
    }
    auto trampoline = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        log("[Stereo] Vulkan backend-owner trampoline allocation failed");
        return false;
    }
    auto cursor = trampoline;
    std::memcpy(cursor, signature, sizeof(signature));
    cursor += sizeof(signature);
    emit8(cursor, 0xFF); emit8(cursor, 0x25);
    emit8(cursor, 0); emit8(cursor, 0); emit8(cursor, 0); emit8(cursor, 0);
    emit64(cursor, reinterpret_cast<unsigned long long>(target + sizeof(signature)));
    originalVulkanBackendOwnerInit = reinterpret_cast<VulkanBackendOwnerInitFn>(trampoline);

    DWORD oldProtect{};
    if (!VirtualProtect(target, sizeof(signature), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        originalVulkanBackendOwnerInit = nullptr;
        VirtualFree(trampoline, 0, MEM_RELEASE);
        log("[Stereo] Vulkan backend-owner target protection failed");
        return false;
    }
    unsigned char jump[sizeof(signature)]{0xFF,0x25,0,0,0,0};
    const auto wrapper = reinterpret_cast<unsigned long long>(nativeVulkanBackendOwnerInitHook);
    std::memcpy(jump + 6, &wrapper, sizeof(wrapper));
    for (size_t index = 14; index < sizeof(jump); ++index) jump[index] = 0x90;
    std::memcpy(target, jump, sizeof(jump));
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(jump));
    VirtualProtect(target, sizeof(jump), oldProtect, &oldProtect);
    log("[Stereo] Vulkan backend owner-field guard installed RVA=0x186E000");
    return true;
}

bool installNativeSameFrameStereo() {
    const bool splitFrontend = false;
    const bool nativeTwoView = false;
    if (!splitFrontend && !nativeTwoView) {
        return false;
    }
    if (nativeTwoView) {
        // Physical gameplay validation on 2026-08-22 proved that DOOM's
        // dormant two-view list does not construct valid Vulkan backend owner
        // state for its second view. Fail closed even if a stale research
        // marker is created manually; the normal split-frontend experiment is
        // a separate path and remains available through enable_native_stereo.
        log("[Stereo] genuine native two-view BLOCKED after invalid Vulkan-view crash");
        return false;
    }

    // DOOM 6.66 already contains a native multi-view renderer. Its global
    // render-view object defaults to the one-view "single" layout. Select the
    // built-in two-view leftRightStereo layout and keep both views enabled in
    // every game frame. These writes are gated and signature-checked so the
    // physically accepted alternating-eye path remains untouched by default.
    auto image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!image) return false;
    constexpr uintptr_t viewLayoutSlotRva = 0x36111E8;
    constexpr uintptr_t primaryViewListRva = 0x360F610;
    constexpr uintptr_t primaryViewStorageRva = 0x360F630;
    constexpr uintptr_t singleLayoutRva = 0x284FFC8;
    constexpr uintptr_t leftRightNameRecordRva = 0x2850068;
    constexpr uintptr_t leftRightNameRva = 0x2850230;
    constexpr uintptr_t leftRightLayoutRva = 0x2850090;
    constexpr uintptr_t nextModeNameRecordRva = 0x28500E0;
    constexpr uintptr_t multiViewCurrentRva = 0x5FB8B20;

    auto layoutSlot = reinterpret_cast<void* volatile*>(image + viewLayoutSlotRva);
    struct ViewListHeader {
        void* data;
        LONG count;
        LONG capacity;
        unsigned long long flags;
    };
    auto primaryViews = reinterpret_cast<ViewListHeader*>(image + primaryViewListRva);
    auto currentLayout = *layoutSlot;
    auto singleLayout = static_cast<void*>(image + singleLayoutRva);
    auto leftRightLayout = static_cast<void*>(image + leftRightLayoutRva);
    const auto leftRightName = *reinterpret_cast<const char* const*>(image + leftRightNameRecordRva);
    const auto expectedLeftRightName = reinterpret_cast<const char*>(image + leftRightNameRva);
    const auto nextModeName = *reinterpret_cast<const char* const*>(image + nextModeNameRecordRva);
    const bool tableMatches = leftRightName == expectedLeftRightName
        && std::strcmp(leftRightName, "leftRightStereo") == 0
        && nextModeName && std::strcmp(nextModeName, "HDMI3D") == 0;
    const bool viewStorageMatches = primaryViews->data == image + primaryViewStorageRva
        && primaryViews->capacity == 1
        && *(image + primaryViewListRva + 0x13) == 1;
    if (!tableMatches || !viewStorageMatches
        || (currentLayout != singleLayout && currentLayout != leftRightLayout)) {
        std::ostringstream out;
        out << "[Stereo] native renderer signature mismatch currentLayout=0x" << std::hex
            << reinterpret_cast<uintptr_t>(currentLayout)
            << " primaryStorage=0x" << reinterpret_cast<uintptr_t>(primaryViews->data)
            << std::dec << " capacity=" << primaryViews->capacity
            << " mode=" << unsigned(*(image + primaryViewListRva + 0x13));
        log(out.str());
        return false;
    }

    // Build two one-entry layouts from DOOM's native leftRightStereo table.
    // The old proof retained the half-width SBS viewport, even though each
    // frontend pass ultimately produces a complete frame. That reduced a
    // 1920x1080 surface to roughly 960x540 before OpenXR upscaling. Each
    // sequential pass now owns the complete target. The following 0x28-byte
    // record is the mode terminator.
    std::memcpy(nativeLeftSingleViewLayout.data(), image + leftRightLayoutRva, 0x28);
    std::memcpy(nativeLeftSingleViewLayout.data() + 0x28, image + nextModeNameRecordRva, 0x28);
    std::memcpy(nativeRightSingleViewLayout.data(), image + leftRightLayoutRva + 0x28, 0x28);
    std::memcpy(nativeRightSingleViewLayout.data() + 0x28, image + nextModeNameRecordRva, 0x28);
    const float fullOrigin = 0.0f;
    const float fullExtent = 1.0f;
    for (auto* layout : {nativeLeftSingleViewLayout.data(), nativeRightSingleViewLayout.data()}) {
        std::memcpy(layout + 0x0C, &fullOrigin, sizeof(fullOrigin));
        std::memcpy(layout + 0x14, &fullExtent, sizeof(fullExtent));
        std::memcpy(layout + 0x18, &fullExtent, sizeof(fullExtent));
    }
    // Diagnostic: remove the per-view -1/+1 stereo multiplier from both
    // records. This isolates viewport/projection behavior from DOOM's world
    // and screen eye-separation calculations.
    const float zeroEyeFactor = 0.0f;
    std::memcpy(nativeLeftSingleViewLayout.data() + 0x20, &zeroEyeFactor, sizeof(zeroEyeFactor));
    std::memcpy(nativeRightSingleViewLayout.data() + 0x20, &zeroEyeFactor, sizeof(zeroEyeFactor));
    InterlockedExchangePointer(layoutSlot, singleLayout);
    nativeStereoLayoutEnabled.store(false, std::memory_order_release);
    nativeStereoFrameReady.store(false, std::memory_order_release);
    nativeStereoBootstrapStage.store(0, std::memory_order_release);
    nativeSameFrameInstalled.store(true, std::memory_order_release);
    std::ostringstream out;
    out << "[Stereo] same-frame split-frontend PREPARED leftLayout=0x" << std::hex
        << reinterpret_cast<uintptr_t>(nativeLeftSingleViewLayout.data())
        << " rightLayout=0x" << reinterpret_cast<uintptr_t>(nativeRightSingleViewLayout.data())
        << " sourceLayoutRVA=0x" << leftRightLayoutRva
        << " viewport=FULL eyeFactor=0 DIAGNOSTIC";
    log(out.str());
    return true;
}

void finishNativeStereoBootstrap() {
    int expectedStage = 1;
    if (!nativeStereoBootstrapStage.compare_exchange_strong(
            expectedStage, 2, std::memory_order_acq_rel)) return;
    auto image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!image) {
        nativeStereoBootstrapStage.store(0, std::memory_order_release);
        return;
    }
    constexpr uintptr_t viewLayoutSlotRva = 0x36111E8;
    constexpr uintptr_t primaryViewListRva = 0x360F610;
    constexpr uintptr_t primaryViewStorageRva = 0x360F630;
    constexpr uintptr_t reserveViewsRva = 0x1188C70;
    constexpr uintptr_t constructViewRva = 0x117AF50;
    constexpr uintptr_t leftRightLayoutRva = 0x2850090;
    constexpr size_t viewBytes = 0x920;
    struct ViewListHeader {
        void* data;
        LONG count;
        LONG capacity;
        unsigned long long flags;
    };
    auto views = reinterpret_cast<ViewListHeader*>(image + primaryViewListRva);
    using ReserveViewsFn = bool(__fastcall*)(ViewListHeader*, int);
    using ConstructViewFn = void*(__fastcall*)(void*);
    auto reserveViews = reinterpret_cast<ReserveViewsFn>(image + reserveViewsRva);
    auto constructView = reinterpret_cast<ConstructViewFn>(image + constructViewRva);
    const LONG constructedViews = std::max<LONG>(0, views->count);
    auto mode = image + primaryViewListRva + 0x13;
    if (*mode == 1 && views->data == image + primaryViewStorageRva && views->capacity == 1)
        *mode = 2;
    if (!reserveViews(views, 2) || !views->data || views->capacity < 2) {
        log("[Stereo] first-acquire native two-view idList conversion failed");
        nativeStereoBootstrapStage.store(0, std::memory_order_release);
        return;
    }
    for (LONG index = constructedViews; index < 2; ++index)
        constructView(static_cast<unsigned char*>(views->data) + size_t(index) * viewBytes);
    InterlockedExchange(&views->count, 0);
    auto layoutSlot = reinterpret_cast<void* volatile*>(image + viewLayoutSlotRva);
    InterlockedExchangePointer(layoutSlot, image + leftRightLayoutRva);
    std::ostringstream out;
    out << "[Stereo] first-acquire bootstrap frame ARMED viewList count=" << views->count
        << " capacity=" << views->capacity << " data=0x" << std::hex
        << reinterpret_cast<uintptr_t>(views->data) << std::dec
        << " mode=" << unsigned(*mode);
    log(out.str());
}

void completeNativeStereoBootstrapAfterRender() {
    int expectedStage = 2;
    if (!nativeStereoBootstrapStage.compare_exchange_strong(
            expectedStage, 0, std::memory_order_acq_rel)) return;
    auto image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!image) return;
    constexpr uintptr_t viewLayoutSlotRva = 0x36111E8;
    constexpr uintptr_t primaryViewListRva = 0x360F610;
    constexpr uintptr_t singleLayoutRva = 0x284FFC8;
    constexpr size_t viewBytes = 0x920;
    struct ViewListHeader { void* data; LONG count; LONG capacity; };
    auto views = reinterpret_cast<ViewListHeader*>(image + primaryViewListRva);
    std::ostringstream out;
    out << "[Stereo] bootstrap frame rendered viewList count=" << views->count
        << " capacity=" << views->capacity << " data=0x" << std::hex
        << reinterpret_cast<uintptr_t>(views->data);
    if (views->data && views->capacity >= 2) {
        for (int index = 0; index < 2; ++index) {
            auto view = static_cast<unsigned char*>(views->data) + size_t(index) * viewBytes;
            out << " view" << index << "(+178)=0x"
                << reinterpret_cast<uintptr_t>(*reinterpret_cast<void**>(view + 0x178))
                << " view" << index << "(+240)=0x"
                << reinterpret_cast<uintptr_t>(*reinterpret_cast<void**>(view + 0x240));
        }
    }
    log(out.str());
    if (nativeTwoViewInstalled.load(std::memory_order_acquire)
        && nativeStereoLayoutEnabled.load(std::memory_order_acquire)) {
        nativeStereoFrameReady.store(true, std::memory_order_release);
        log("[Stereo] genuine native two-view ACTIVE; packed SBS frame ready");
        return;
    }
    InterlockedExchangePointer(
        reinterpret_cast<void* volatile*>(image + viewLayoutSlotRva), image + singleLayoutRva);
    nativeStereoLayoutEnabled.store(false, std::memory_order_release);
    nativeStereoFrameReady.store(false, std::memory_order_release);
    log("[Stereo] bootstrap complete; single layout selected after first rendered frame");
}

extern "C" bool __fastcall renderFrontendStereoProof(void* context, void* arg2, void* arg3) {
    if (!originalRenderFrontend) return false;
    if (nativeSameFrameInstalled.load(std::memory_order_acquire)) {
        if (!worldCameraActive.load(std::memory_order_acquire)
            || cutsceneActive.load(std::memory_order_acquire)) {
            nativeStereoFrameReady.store(false, std::memory_order_release);
            return originalRenderFrontend(context, arg2, arg3);
        }
        auto image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
        if (!image) return originalRenderFrontend(context, arg2, arg3);
        constexpr uintptr_t viewLayoutSlotRva = 0x36111E8;
        auto layoutSlot = reinterpret_cast<void* volatile*>(image + viewLayoutSlotRva);
        auto savedLayout = *layoutSlot;
        // Native layout eye signs provide separation; suppress the alternating
        // camera offset that was programmed for the preceding fallback frame.
        stereoEyeOffset.store(0.0f, std::memory_order_relaxed);
        const auto hitsBefore = InterlockedCompareExchange64(
            reinterpret_cast<volatile LONG64*>(&sample.hits), 0, 0);
        InterlockedExchangePointer(layoutSlot, nativeLeftSingleViewLayout.data());
        currentRenderEye.store(VREye::Left, std::memory_order_release);
        const bool leftResult = originalRenderFrontend(context, arg2, arg3);
        const auto hitsAfterLeft = InterlockedCompareExchange64(
            reinterpret_cast<volatile LONG64*>(&sample.hits), 0, 0);
        InterlockedExchangePointer(layoutSlot, nativeRightSingleViewLayout.data());
        currentRenderEye.store(VREye::Right, std::memory_order_release);
        const bool rightResult = originalRenderFrontend(context, arg2, arg3);
        const auto hitsAfterRight = InterlockedCompareExchange64(
            reinterpret_cast<volatile LONG64*>(&sample.hits), 0, 0);
        InterlockedExchangePointer(layoutSlot, savedLayout);
        currentRenderEye.store(VREye::Mono, std::memory_order_release);
        nativeStereoFrameReady.store(true, std::memory_order_release);
        static std::atomic<unsigned long long> nativeFrame{};
        const auto frame = nativeFrame.fetch_add(1, std::memory_order_relaxed) + 1;
        if (frame <= 10 || frame % 300 == 0) {
            std::ostringstream out;
            out << "[Stereo] split-frontend gameFrame=" << frame
                << " left=" << (leftResult ? "OK" : "false")
                << " right=" << (rightResult ? "OK" : "false")
                << " cameraHits=" << (hitsAfterLeft - hitsBefore)
                << '+' << (hitsAfterRight - hitsAfterLeft)
                << " packedSameFrame=YES";
            log(out.str());
        }
        return leftResult || rightResult;
    }
    if (!worldCameraActive.load(std::memory_order_acquire) || !configuredStereo.load(std::memory_order_acquire)) {
        return originalRenderFrontend(context, arg2, arg3);
    }
    static std::atomic<unsigned long long> proofFrame{};
    const auto frame = proofFrame.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto before = InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(&sample.hits), 0, 0);
    selectConfiguredEye(VREye::Left);
    const bool leftResult = originalRenderFrontend(context, arg2, arg3);
    const auto afterLeft = InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(&sample.hits), 0, 0);
    selectConfiguredEye(VREye::Right);
    const bool rightResult = originalRenderFrontend(context, arg2, arg3);
    const auto afterRight = InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(&sample.hits), 0, 0);
    currentRenderEye.store(VREye::Mono, std::memory_order_release);
    if (frame <= 10 || frame % 300 == 0) {
        std::ostringstream out;
        out << "[Stereo] gameFrame=" << frame
            << " leftCameraHook=" << (afterLeft > before ? "YES" : "NO")
            << " rightCameraHook=" << (afterRight > afterLeft ? "YES" : "NO")
            << " sameFrame=" << ((afterLeft > before && afterRight > afterLeft) ? "YES" : "NO");
        log(out.str());
    }
    return leftResult || rightResult;
}

bool installSameFrameProofHook() {
    const bool nativeSplitFrontend = false;
    if (!nativeSplitFrontend
        && !kharvox::runtimeFileExists(L"enable_same_frame_stereo")) {
        log("[Stereo] same-frame proof gated off; M1C alternating fallback");
        return false;
    }
    auto image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    auto target = image ? image + 0xDC80D0 : nullptr;
    constexpr unsigned char prefix[] = {0x48,0x89,0x5C,0x24,0x10,0x56,0x48,0x83,0xEC,0x30,0x48,0x8B,0x01,0x49,0x8B,0xF0};
    if (!target || std::memcmp(target, prefix, sizeof(prefix))) {
        log("[Stereo] DC80D0 render-front-end signature mismatch; proof hook not installed");
        return false;
    }
    constexpr size_t displacedBytes = sizeof(prefix);
    auto trampoline = static_cast<unsigned char*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        log("[Stereo] render-view trampoline allocation failed");
        return false;
    }
    auto p = trampoline;
    std::memcpy(p, prefix, displacedBytes); p += displacedBytes;
    emit8(p, 0xFF); emit8(p, 0x25); emit8(p, 0); emit8(p, 0); emit8(p, 0); emit8(p, 0);
    emit64(p, reinterpret_cast<unsigned long long>(target + displacedBytes));
    originalRenderFrontend = reinterpret_cast<RenderFrontendFn>(trampoline);
    DWORD old{};
    if (!VirtualProtect(target, displacedBytes, PAGE_EXECUTE_READWRITE, &old)) {
        log("[Stereo] render-front-end target protection failed");
        originalRenderFrontend = nullptr;
        return false;
    }
    unsigned char jump[displacedBytes] = {0xFF,0x25,0,0,0,0};
    const auto wrapper = reinterpret_cast<unsigned long long>(renderFrontendStereoProof);
    std::memcpy(jump + 6, &wrapper, sizeof(wrapper));
    std::memcpy(target, jump, sizeof(jump));
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(jump));
    VirtualProtect(target, sizeof(jump), old, &old);
    sameFrameInstalled.store(true, std::memory_order_release);
    std::ostringstream out;
    out << (nativeSplitFrontend
        ? "[Stereo] native split-frontend hook installed RVA=0x"
        : "[Stereo] same-frame render-front-end proof installed RVA=0x") << std::hex
        << (target - reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr)));
    log(out.str());
    return true;
}

bool installCutsceneFovHook() {
    // Independently measured cinematic FOV signature. Besides identifying the
    // cinematic path, the adjacent output values expose its
    // current VR projection FOV. Do the same only while the optional immersive
    // cinematic presentation is active. The target is an exact frame-matched
    // OpenXR FOV; any wider render/narrower layer pairing makes rotational
    // reprojection geometrically incorrect and causes zoom-dependent warping.
    constexpr unsigned char signature[] = {
        0xF3,0x0F,0x58,0xC0,0xF3,0x0F,0x11,0x00,
        // The supported DOOM 6.66 path uses a 0x48-byte frame here. Fail closed
        // instead of accepting alternate frame layouts.
        0x48,0x83,0xC4,0x48,0xC3,0xCC
    };
    auto target=findTextSignature(signature,sizeof(signature));
    if(!target){log("DOOMCutsceneFOV signature mismatch; cutscene QUAD unavailable");return false;}
    auto stub=static_cast<unsigned char*>(VirtualAlloc(nullptr,160,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));
    if(!stub)return false;
    auto p=stub;
    std::memcpy(p,signature,4);p+=4;                 // addss xmm0,xmm0
    emit8(p,0x49);emit8(p,0xBB);                    // mov r11, &cutsceneFovHits
    emit64(p,reinterpret_cast<unsigned long long>(&cutsceneFovHits));
    emit8(p,0xF0);emit8(p,0x49);emit8(p,0xFF);emit8(p,0x03); // lock inc qword [r11]
    emit8(p,0x49);emit8(p,0xBA);
    emit64(p,reinterpret_cast<unsigned long long>(&cutsceneFovValue));
    emit8(p,0xF3);emit8(p,0x41);emit8(p,0x0F);emit8(p,0x11);emit8(p,0x02); // movss [r10],xmm0

    emit8(p,0x49);emit8(p,0xBB);                    // mov r11, &immersiveCinematicFovHookActive
    emit64(p,reinterpret_cast<unsigned long long>(&immersiveCinematicFovHookActive));
    emit8(p,0x41);emit8(p,0x83);emit8(p,0x3B);emit8(p,0x00); // cmp dword ptr [r11],0
    emit8(p,0x0F);emit8(p,0x84);                    // je originalWrite
    auto disabledJump=p;p+=4;

    emit8(p,0x49);emit8(p,0xBA);                    // mov r10, &immersiveCinematicFovXBits
    emit64(p,reinterpret_cast<unsigned long long>(&immersiveCinematicFovXBits));
    emit8(p,0xF3);emit8(p,0x41);emit8(p,0x0F);emit8(p,0x10);emit8(p,0x02); // movss xmm0,[r10]
    emit8(p,0xF3);emit8(p,0x0F);emit8(p,0x11);emit8(p,0x00); // movss [rax],xmm0
    emit8(p,0x49);emit8(p,0xBA);                    // mov r10, &immersiveCinematicFovYBits
    emit64(p,reinterpret_cast<unsigned long long>(&immersiveCinematicFovYBits));
    emit8(p,0xF3);emit8(p,0x41);emit8(p,0x0F);emit8(p,0x10);emit8(p,0x02); // movss xmm0,[r10]
    emit8(p,0xF3);emit8(p,0x0F);emit8(p,0x11);emit8(p,0x40);emit8(p,0x04); // movss [rax+4],xmm0
    emit8(p,0xE9);                                  // jmp epilogue
    auto epilogueJump=p;p+=4;

    auto originalWrite=p;
    emit8(p,0xF3);emit8(p,0x0F);emit8(p,0x11);emit8(p,0x00); // movss [rax],xmm0
    auto epilogue=p;
    std::memcpy(p,signature+8,6);p+=6;              // stack restore + return
    const auto disabledRelative=static_cast<unsigned int>(originalWrite-(disabledJump+4));
    std::memcpy(disabledJump,&disabledRelative,sizeof(disabledRelative));
    const auto epilogueRelative=static_cast<unsigned int>(epilogue-(epilogueJump+4));
    std::memcpy(epilogueJump,&epilogueRelative,sizeof(epilogueRelative));
    DWORD old{};
    if(!VirtualProtect(target,sizeof(signature),PAGE_EXECUTE_READWRITE,&old))return false;
    unsigned char jump[sizeof(signature)]={0xFF,0x25,0,0,0,0};
    const auto address=reinterpret_cast<unsigned long long>(stub);
    std::memcpy(jump+6,&address,sizeof(address));
    std::memcpy(target,jump,sizeof(jump));
    FlushInstructionCache(GetCurrentProcess(),target,sizeof(jump));
    VirtualProtect(target,sizeof(jump),old,&old);
    std::ostringstream out;out<<"DOOMCutsceneFOV hook installed RVA=0x"<<std::hex<<(target-reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr)));
    log(out.str());return true;
}

extern "C" void __fastcall patchCamera(void* rawContext, void* rawReturnAddress) {
    if (!rawContext) return;
    auto camera = static_cast<float*>(rawContext);
    const uintptr_t context = reinterpret_cast<uintptr_t>(rawContext);
    const uintptr_t returnAddress = reinterpret_cast<uintptr_t>(rawReturnAddress);
    const auto imageBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const uintptr_t callerRva = imageBase ? returnAddress - imageBase : 0;
    const auto unmodifiedCameraBytes = reinterpret_cast<unsigned char*>(rawContext);
    const auto unmodifiedPosition = reinterpret_cast<float*>(unmodifiedCameraBytes + 0xC0);
    const auto unmodifiedBasis = reinterpret_cast<float*>(unmodifiedCameraBytes + 0xCC);
    if(callerRva==0xA30CFF&&KharvoxCameraBossSequenceActive()){
        cutsceneActive.store(true,std::memory_order_release);
        InterlockedExchange(&immersiveCinematicFovHookActive,0);
        sample.context=context;sample.returnAddress=returnAddress;
        sample.positionX=unmodifiedPosition[0];sample.positionY=unmodifiedPosition[1];
        sample.positionZ=unmodifiedPosition[2];
        sample.fovX=*reinterpret_cast<float*>(unmodifiedCameraBytes+0x70);
        sample.fovY=*reinterpret_cast<float*>(unmodifiedCameraBytes+0x74);
        InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&sample.hits));
        return; // Native Quad source: no HMD, IPD, paired pose or VR FOV writes.
    }
    synchronizeAerAnimatedCameraPose(
        callerRva, unmodifiedPosition, unmodifiedBasis);
    if (callerRva == 0xA30CFF) {
        // Glory kills and other cinematics use a different camera basis. They
        // must never overwrite the persistent gameplay body pose. In the
        // optional immersive mode, keep the native animated position but
        // replace the animated rotation with the current HMD orientation on
        // top of the gameplay body basis held at cinematic entry. HMD
        // translation remains disabled so the animation still owns position.
        const bool wasCutscene = cutsceneActive.exchange(true, std::memory_order_acq_rel);
        if (!wasCutscene && bodyCameraValid.load(std::memory_order_acquire)) {
            preCinematicGameplayAxisValid.store(false, std::memory_order_release);
            float entryBasis[9]{};
            float levelEntryBasis[9]{};
            for (int index = 0; index < 9; ++index)
                entryBasis[index] = bodyCameraAxis[index].load(std::memory_order_relaxed);
            const float* heldBasis = entryBasis;
            if (kharvox::makeGravityLevelBodyBasis(
                    entryBasis, nullptr, levelEntryBasis))
                heldBasis = levelEntryBasis;
            for (int index = 0; index < 9; ++index)
                preCinematicGameplayAxis[index].store(heldBasis[index], std::memory_order_relaxed);
            preCinematicGameplayAxisValid.store(true, std::memory_order_release);
            log("cinematic entry: gravity-level gameplay body basis held");
        }
        const bool immersiveHmdRotation =
            InterlockedCompareExchange(&immersiveCinematicFreelookRequested, 0, 0) != 0
            && worldCameraActive.load(std::memory_order_acquire)
            && !KharvoxCameraBossSequenceActive()
            && preCinematicGameplayAxisValid.load(std::memory_order_acquire)
            && headValid.load(std::memory_order_acquire);
        if (immersiveHmdRotation) {
            float hmdBasis[9]{};
            for (int index = 0; index < 9; ++index)
                hmdBasis[index] = preCinematicGameplayAxis[index].load(
                    std::memory_order_relaxed);
            applyResidualHeadOrientation(hmdBasis);
            std::memcpy(unmodifiedBasis, hmdBasis, sizeof(hmdBasis));
            if (!wasCutscene)
                log("[IMMERSIVE] animation position + HMD-only rotation ACTIVE; HMD translation disabled");
        }
        const float nativeFovX = *reinterpret_cast<float*>(unmodifiedCameraBytes + 0x70);
        const float nativeFovY = *reinterpret_cast<float*>(unmodifiedCameraBytes + 0x74);
        float targetFovX{}, targetFovY{};
        if (immersiveCinematicFov(targetFovX, targetFovY)) {
            *reinterpret_cast<float*>(unmodifiedCameraBytes + 0x70) = targetFovX;
            *reinterpret_cast<float*>(unmodifiedCameraBytes + 0x74) = targetFovY;
            *(unmodifiedCameraBytes + 0x6F) = 1;
        }
        const bool stereoEnabled = stereoEyeEnabled.load(std::memory_order_acquire);
        // A cinematic can start on an engine worker before XR sees its mode
        // transition. Arm the existing per-eye FOV writer before that first
        // producer; otherwise the native 90-degree FOV overwrites this eye.
        if(stereoEnabled
            &&InterlockedCompareExchange(&immersiveCinematicFovRequested,0,0)!=0){
            InterlockedExchange(&immersiveCinematicFovHookActive,1);
            if(!wasCutscene)log(std::string(kharvox::native::requested()
                ?"[NATIVE-CINEMATIC] first-source FOV override armed from runtime projection "
                :"[AER-CINEMATIC] r268 first-eye FOV override armed from runtime projection ")
                +std::to_string(stereoFovX.load())+"x"+std::to_string(stereoFovY.load()));
        }
        float tracedEyeOffset=0.0f;
        if (stereoEnabled) {
            // This caller used to return before the normal camera path applied
            // IPD and optical projection. Glory Kills, ledge transitions and
            // other immersive cinematics were therefore deliberately mono.
            // Preserve the native animated centre camera, then derive the
            // requested eye exactly as gameplay does.
            const float eyeOffset = stereoEyeOffset.load(std::memory_order_relaxed);
            tracedEyeOffset=eyeOffset;
            for (int axis = 0; axis < 3; ++axis)
                unmodifiedPosition[axis] += unmodifiedBasis[3 + axis] * eyeOffset;
            rotateCameraBasis(
                unmodifiedBasis,
                -stereoOpticalCenterYaw.load(std::memory_order_relaxed),
                stereoOpticalCenterPitch.load(std::memory_order_relaxed), 0.0f);
            // Stereo must use the same per-eye render FOV as its cached image.
            // The enclosing mono cinematic FOV is not the per-eye projection.
            if(!kharvox::native::requested()||!immersiveCinematicFov(targetFovX,targetFovY)){
                *reinterpret_cast<float*>(unmodifiedCameraBytes + 0x70) =
                    stereoFovX.load(std::memory_order_relaxed);
                *reinterpret_cast<float*>(unmodifiedCameraBytes + 0x74) =
                    stereoFovY.load(std::memory_order_relaxed);
                *(unmodifiedCameraBytes + 0x6F) = 1;
            }
            static std::atomic<bool> stereoCinematicLogged{};
            if (!stereoCinematicLogged.exchange(true, std::memory_order_acq_rel))
                log(kharvox::native::requested()
                    ?"[NATIVE-CINEMATIC] animated center camera projection active; eye separation is applied by the two Native scene roots"
                    :"[AER-CINEMATIC] cinematic camera now receives per-eye IPD/projection; native animation retained");
        }
        sample.context = context;
        sample.yaw = camera[0];
        sample.pitch = camera[1];
        sample.positionX = unmodifiedPosition[0];
        sample.positionY = unmodifiedPosition[1];
        sample.positionZ = unmodifiedPosition[2];
        sample.fovX = nativeFovX;
        sample.fovY = nativeFovY;
        sample.returnAddress = returnAddress;
        const auto cinematicHead=renderHeadPose();
        if(!kharvox::native::requested()&&stereoEnabled&&tracedEyeOffset!=0
            &&aerRenderPairEnabled.load(std::memory_order_acquire)){
            kharvox::AerWorldView observed;
            std::memcpy(observed.pose.data(),unmodifiedCameraBytes+0xC0,sizeof(observed.pose));
            std::memcpy(observed.fov.data(),unmodifiedCameraBytes+0x70,sizeof(observed.fov));
            observed.context=callerRva;observed.poseId=cinematicHead.poseId;
            observed.present=KharvoxCameraCurrentPresentSerial();
            observed.level=KharvoxCameraLevelTransitionGeneration();
            observed.eye=kharvox::aerEyeFromDoomOffset(tracedEyeOffset);observed.domain=1;
            aerWorldViewHistory.remember(observed);
        }
        kharvox::pose_trace::camera(KharvoxCameraCurrentPresentSerial(),context,unmodifiedCameraBytes,
            tracedEyeOffset,kharvox::native::requested()?2u:262u,callerRva,cinematicHead.poseId,
            kharvox::native::requested()?-2:kharvox::aerEyeFromDoomOffset(tracedEyeOffset));
        InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&sample.hits));
        return;
    }
    const bool gameplayCamera = callerRva == 0xE3F96A;
    const bool cinematicEnded = gameplayCamera
        && cutsceneActive.exchange(false, std::memory_order_acq_rel);
    if (cinematicEnded && preCinematicGameplayAxisValid.load(std::memory_order_acquire))
        log("[IMMERSIVE] cinematic exit: native pitch/roll discarded; gravity-level gameplay horizon restored");
    // F5-F12 are reserved for weapon calibration. The former F9/F10 camera
    // probe could still fire during the one frame in which weapon tracking
    // transitions from invalid to valid and corrupt an otherwise saved pose.
    const bool moveLeft = false;
    const bool moveRight = false;
    if (nativeSameFrameInstalled.load(std::memory_order_acquire)) {
        const auto eye = currentRenderEye.load(std::memory_order_acquire);
        if (eye != VREye::Mono) {
            static std::atomic<unsigned long long> nativeProbeCount{};
            const auto probe = nativeProbeCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (probe <= 80) {
                std::ostringstream out;
                out << "[StereoProbe] #" << probe
                    << " eye=" << (eye == VREye::Left ? "LEFT" : "RIGHT")
                    << " context=0x" << std::hex << context << std::dec
                    << std::fixed << std::setprecision(4)
                    << " fov=" << *reinterpret_cast<float*>(unmodifiedCameraBytes + 0x70)
                    << 'x' << *reinterpret_cast<float*>(unmodifiedCameraBytes + 0x74)
                    << " pos=" << unmodifiedPosition[0] << ','
                    << unmodifiedPosition[1] << ',' << unmodifiedPosition[2]
                    << " axis0=" << unmodifiedBasis[0] << ','
                    << unmodifiedBasis[1] << ',' << unmodifiedBasis[2]
                    << " axis1=" << unmodifiedBasis[3] << ','
                    << unmodifiedBasis[4] << ',' << unmodifiedBasis[5];
                log(out.str());
            }
        }
    }
    float bodyBasis[9]{};
    std::memcpy(bodyBasis, unmodifiedBasis, sizeof(bodyBasis));
    // The old exit repair only ran after a generic cinematic had set
    // preCinematicGameplayAxisValid. Ordinary melee/recoil animation uses this
    // same gameplay caller and could therefore leave native pitch/roll in the
    // persistent body pose. Make Z-up leveling the normal gameplay invariant.
    // Authored animated sequences are classified separately and retain their
    // native motion; HMD pitch/roll is applied below after this body basis.
    float fallbackBasis[9]{};
    const float* fallback = nullptr;
    if (bodyCameraValid.load(std::memory_order_acquire)) {
        for (int index = 0; index < 9; ++index)
            fallbackBasis[index] = bodyCameraAxis[index].load(std::memory_order_relaxed);
        fallback = fallbackBasis;
    } else if (preCinematicGameplayAxisValid.load(std::memory_order_acquire)) {
        for (int index = 0; index < 9; ++index)
            fallbackBasis[index] = preCinematicGameplayAxis[index].load(std::memory_order_relaxed);
        fallback = fallbackBasis;
    }
    float levelBasis[9]{};
    if (kharvox::makeStableGameplayBodyBasis(
            gameplayCamera,
            animatedSequenceActive.load(std::memory_order_acquire),
            bodyBasis, fallback, levelBasis)) {
        std::memcpy(bodyBasis, levelBasis, sizeof(bodyBasis));
        std::memcpy(unmodifiedBasis, bodyBasis, sizeof(bodyBasis));
    }
    float stableBodyOrigin[3]{unmodifiedPosition[0], unmodifiedPosition[1], unmodifiedPosition[2]};
    const bool mayCalibrateAnchor=kharvox::mayCalibrateBodyAnchor(gameplayCamera,
        animatedSequenceActive.load(std::memory_order_acquire),
        KharvoxCameraPlayerWeaponControlActive(),KharvoxCameraBossSequenceActive());
    const bool existingBodyAnchor=stableBodyViewOffsetValid.load(std::memory_order_acquire)
        &&stableBodyViewOffsetOwner.load(std::memory_order_relaxed)
            ==playerPhysicsOwner.load(std::memory_order_acquire);
    if (playerPhysicsOriginValid.load(std::memory_order_acquire)
        &&(existingBodyAnchor||mayCalibrateAnchor)) {
        std::lock_guard<std::mutex> stanceGuard(bodyStanceMutex);
        const uintptr_t owner = playerPhysicsOwner.load(std::memory_order_relaxed);
        float physicsOrigin[3]{};
        for (int index = 0; index < 3; ++index)
            physicsOrigin[index] = playerPhysicsOrigin[index].load(std::memory_order_relaxed);
        std::array<float, 3> viewOffsetLocal{};
        const float offset[3]{
            unmodifiedPosition[0] - physicsOrigin[0],
            unmodifiedPosition[1] - physicsOrigin[1],
            unmodifiedPosition[2] - physicsOrigin[2]
        };
        std::array<float, 3> measuredViewOffsetLocal{};
        for (int row = 0; row < 3; ++row)
            measuredViewOffsetLocal[row] = offset[0] * bodyBasis[row * 3]
                + offset[1] * bodyBasis[row * 3 + 1]
                + offset[2] * bodyBasis[row * 3 + 2];
        const bool offsetValid = stableBodyViewOffsetValid.load(std::memory_order_acquire)
            && stableBodyViewOffsetOwner.load(std::memory_order_relaxed) == owner;
        if (!offsetValid&&mayCalibrateAnchor) {
            viewOffsetLocal = measuredViewOffsetLocal;
            stableBodyViewOffsetValid.store(false, std::memory_order_release);
            for (int row = 0; row < 3; ++row)
                stableBodyViewOffsetLocal[row].store(viewOffsetLocal[row], std::memory_order_relaxed);
            stableBodyViewOffsetOwner.store(owner, std::memory_order_relaxed);
            stableBodyViewOffsetValid.store(true, std::memory_order_release);
            std::ostringstream out;
            out << "stable weapon/body anchor calibrated player=0x" << std::hex << owner
                << std::dec << " localViewOffset=" << viewOffsetLocal[0] << ','
                << viewOffsetLocal[1] << ',' << viewOffsetLocal[2];
            log(out.str());
        } else if(offsetValid) {
            for (int row = 0; row < 3; ++row)
                viewOffsetLocal[row] = stableBodyViewOffsetLocal[row].load(std::memory_order_relaxed);
        } else {
            viewOffsetLocal=measuredViewOffsetLocal;
        }
        // Follow the native stance animation beyond button release. A button
        // level cannot represent toggle crouch or a geometry-blocked stand.
        // Reset on every anchor calibration, even if the player address repeats.
        const float previousUp = viewOffsetLocal[2];
        if(mayCalibrateAnchor)viewOffsetLocal[2] = bodyStanceHeight.update(measuredViewOffsetLocal[2],
            previousUp, !offsetValid, crouchRequested.load(std::memory_order_acquire),
            cameraPresentSerial.load(std::memory_order_acquire),
            physicsOrigin[0]*bodyBasis[6]+physicsOrigin[1]*bodyBasis[7]+physicsOrigin[2]*bodyBasis[8]);
        if (viewOffsetLocal[2] != previousUp)
            stableBodyViewOffsetLocal[2].store(viewOffsetLocal[2], std::memory_order_relaxed);
        for (int axis = 0; axis < 3; ++axis)
            stableBodyOrigin[axis] = physicsOrigin[axis]
                + bodyBasis[axis] * viewOffsetLocal[0]
                + bodyBasis[3 + axis] * viewOffsetLocal[1]
                + bodyBasis[6 + axis] * viewOffsetLocal[2];
    }
    // Ordinary-camera scripted sequences (including checkpoint entry into the
    // VEGA scene) never use the generic cutscene caller. Pair the final native
    // body camera after the physics/view-height reconstruction so that branch
    // receives the same protection as the explicit cinematic camera.
    synchronizeAerAnimatedCameraPose(
        callerRva ^ (uintptr_t{1} << (sizeof(uintptr_t) * 8 - 1)),
        stableBodyOrigin, bodyBasis);
    for (int index = 0; index < 3; ++index)
        bodyCameraOrigin[index].store(stableBodyOrigin[index], std::memory_order_relaxed);
    for (int index = 0; index < 9; ++index)
        bodyCameraAxis[index].store(bodyBasis[index], std::memory_order_relaxed);
    bodyCameraValid.store(true, std::memory_order_release);
    // OpenXR yaw has the opposite sign from this DOOM render-basis axis.
    // Apply physical HMD rotation first, then translate to the eye along that
    // head-relative lateral axis, and apply the static optical-center rotation
    // last. This prevents an asymmetric frustum from rotating the IPD vector.
    const bool stereoEnabled = stereoEyeEnabled.load(std::memory_order_acquire);
    const float opticalYaw = stereoEnabled ? stereoOpticalCenterYaw.load(std::memory_order_relaxed) : 0.0f;
    const float opticalPitch = stereoEnabled ? stereoOpticalCenterPitch.load(std::memory_order_relaxed) : 0.0f;
    auto renderHead = renderHeadPose();
    const float originalHeadYaw=renderHead.yaw;
    const float currentBodyYaw=std::atan2(unmodifiedBasis[1],unmodifiedBasis[0])*57.2957795131f;
    float bodyRebaseDelta=0;
    if(gameplayCamera&&!animatedSequenceActive.load(std::memory_order_acquire)
        &&renderHead.valid&&renderHead.referenceBodyValid){
        bodyRebaseDelta=kharvox::rebaseHeadToBody(renderHead.referenceBodyYaw,currentBodyYaw,
            renderHead.yaw,renderHead.forward,renderHead.lateral);
    }
    if(kharvox::pose_trace::active.load(std::memory_order_relaxed)){
        kharvox::pose_trace::Event e{};e.kind=kharvox::pose_trace::HeadApplied;
        e.frame=KharvoxCameraCurrentPresentSerial();e.source=context;e.revision=callerRva;
        e.poseId=renderHead.poseId;
        e.flags=(renderHead.valid?1u:0u)|(aerRenderPairEnabled.load()?2u:0u);
        e.data[0]=renderHead.yaw;e.data[1]=renderHead.pitch;e.data[2]=renderHead.roll;
        e.data[3]=renderHead.forward;e.data[4]=renderHead.lateral;e.data[5]=renderHead.up;
        e.data[6]=renderHead.artificialYaw;e.data[7]=renderHead.referenceBodyYaw;
        e.data[8]=currentBodyYaw;e.data[9]=bodyRebaseDelta;e.data[10]=originalHeadYaw;
        e.flags|=renderHead.referenceBodyValid?4u:0u;kharvox::pose_trace::record(e);
        e.kind=kharvox::pose_trace::CameraBase;
        for(int i=0;i<3;++i)e.data[i]=unmodifiedPosition[i];
        for(int i=0;i<9;++i)e.data[i+3]=unmodifiedBasis[i];
        e.data[12]=opticalYaw;e.data[13]=opticalPitch;
        e.data[14]=stereoEnabled?stereoEyeOffset.load(std::memory_order_relaxed):0.f;
        kharvox::pose_trace::record(e);
        e.kind=kharvox::pose_trace::BodyAnchor;
        for(int i=0;i<3;++i)e.data[i]=stableBodyOrigin[i];
        for(int i=0;i<9;++i)e.data[i+3]=bodyBasis[i];
        kharvox::pose_trace::record(e);
    }

    const float hmdYaw = renderHead.valid
        ? -(renderHead.yaw + renderHead.artificialYaw) : 0.0f;
    const float hmdPitch = renderHead.valid ? renderHead.pitch : 0.0f;
    const float hmdRoll = renderHead.valid ? renderHead.roll : 0.0f;

    // Room-scale translation is relative to the automatic initial tracking origin
    // and expressed in the unmodified DOOM body-camera basis. Applying it
    // before the HMD rotation keeps leaning independent of head orientation.
    if (renderHead.valid) {
        const float forward = renderHead.forward;
        const float lateral = renderHead.lateral;
        const float up = renderHead.up;
        auto position = reinterpret_cast<float*>(unmodifiedCameraBytes + 0xC0);
        for (int axis = 0; axis < 3; ++axis) {
            position[axis] += bodyBasis[axis] * forward
                + bodyBasis[3 + axis] * lateral
                + bodyBasis[6 + axis] * up;
        }
    }

    auto basis = reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(rawContext) + 0xCC);
    // Position is camera+0xC0 and the freshly copied 3x3 orientation is
    // camera+0xCC. Rotate without feeding modified gameplay angles back into
    // DOOM's camera controller.
    rotateCameraBasis(basis, hmdYaw, hmdPitch, hmdRoll);

    // Capture the centre-eye pose from the same mutable camera object before
    // per-eye IPD and optical-centre adjustments. It contains DOOM's exact
    // current movement, room-scale translation, HMD pose and artificial turn
    // as consumed by this render pass.
    float hudAnchorOrigin[3]{};
    float hudAnchorAxis[9]{};
    auto renderPosition = reinterpret_cast<float*>(
        reinterpret_cast<unsigned char*>(rawContext) + 0xC0);
    std::memcpy(hudAnchorOrigin, renderPosition, sizeof(hudAnchorOrigin));
    std::memcpy(hudAnchorAxis, basis, sizeof(hudAnchorAxis));
    if (gameplayCamera)
        publishHudCenterRenderPose(hudAnchorOrigin, hudAnchorAxis);

    const float eyeOffset = stereoEnabled ? stereoEyeOffset.load(std::memory_order_relaxed) : 0.0f;
    if (moveLeft || moveRight || eyeOffset != 0.0f) {
        auto position = reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(rawContext) + 0xC0);
        const float offset = (moveLeft ? -12.0f : (moveRight ? 12.0f : 0.0f)) + eyeOffset;
        // Basis row 1 is now head-relative but does not yet contain the
        // per-eye optical-center rotation.
        for (int axis = 0; axis < 3; ++axis) position[axis] += basis[3 + axis] * offset;
        InterlockedCompareExchange(&manualPositionApplied, moveLeft ? -1 : 1, 0);
    } else InterlockedExchange(&manualPositionApplied, 0);

    rotateCameraBasis(basis, -opticalYaw, opticalPitch, 0.0f);

    // The same copy function places world position at camera+0xC0 and the
    // local orientation basis at +0xCC. Use an exaggerated 12-unit offset
    // for the M1C eye-translation proof; this does not move the player body.
    const auto cameraBytes = reinterpret_cast<unsigned char*>(rawContext);
    const float sourceFovX = *reinterpret_cast<float*>(cameraBytes + 0x70);
    const float sourceFovY = *reinterpret_cast<float*>(cameraBytes + 0x74);
    if (stereoEnabled) {
        auto bytes = reinterpret_cast<unsigned char*>(rawContext);
        // DOOM only consumes camera+0x70/+0x74 as an explicit projection when
        // the adjacent custom-FOV flag is set. The immersive cinematic path
        // already sets this byte, which explains why weapon and attached HUD
        // projection became rigid only after the first immersive Glory Kill.
        // Set the same native flag for normal VR gameplay from its first map
        // frame instead of waiting for a cinematic to initialize it.
        *(bytes + 0x6F) = 1;
        *reinterpret_cast<float*>(bytes + 0x70) = stereoFovX.load(std::memory_order_relaxed);
        *reinterpret_cast<float*>(bytes + 0x74) = stereoFovY.load(std::memory_order_relaxed);
    }
    sample.context = context;
    sample.yaw = camera[0];
    sample.pitch = camera[1];
    auto position = reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(rawContext) + 0xC0);
    sample.positionX = position[0];
    sample.positionY = position[1];
    sample.positionZ = position[2];
    sample.fovX = sourceFovX;
    sample.fovY = sourceFovY;
    sample.returnAddress = returnAddress;
    if(gameplayCamera&&stereoEnabled&&eyeOffset!=0&&!kharvox::native::requested()){
        kharvox::AerWorldView observed;
        std::memcpy(observed.pose.data(),cameraBytes+0xC0,sizeof(observed.pose));
        std::memcpy(observed.fov.data(),cameraBytes+0x70,sizeof(observed.fov));
        // The engine can copy a player's main camera through pooled objects.
        // Keep the validated player/level identity, not a transient copy address.
        observed.context=playerPhysicsOwner.load(std::memory_order_acquire);observed.poseId=renderHead.poseId;
        observed.present=KharvoxCameraCurrentPresentSerial();
        observed.domain=animatedSequenceActive.load(std::memory_order_acquire)?2u:0u;
        observed.level=KharvoxCameraLevelTransitionGeneration();observed.eye=kharvox::aerEyeFromDoomOffset(eyeOffset);
        aerWorldViewHistory.remember(observed);
        if(!observed.domain&&aerRenderPairEnabled.load(std::memory_order_acquire)){
            kharvox::AerWeaponCamera weaponCamera;
            weaponCamera.key={observed.poseId,observed.level,observed.eye};weaponCamera.present=observed.present;
            std::memcpy(weaponCamera.bodyOrigin.data(),stableBodyOrigin,sizeof(stableBodyOrigin));
            std::memcpy(weaponCamera.bodyAxis.data(),bodyBasis,sizeof(bodyBasis));
            std::memcpy(weaponCamera.headAxis.data(),hudAnchorAxis,sizeof(hudAnchorAxis));
            weaponCamera.bodyYawDelta=bodyRebaseDelta;
            KharvoxWeaponObserveAerCamera(weaponCamera);
        }
    }
    kharvox::pose_trace::camera(KharvoxCameraCurrentPresentSerial(),context,cameraBytes,eyeOffset,(gameplayCamera?1u:0u)|(kharvox::native::requested()?0u:4u),callerRva,renderHead.poseId,
        kharvox::native::requested()?-2:kharvox::aerEyeFromDoomOffset(eyeOffset));
    InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&sample.hits));
}
}

void KharvoxCameraFinishNativeStereoBootstrap() {
    finishNativeStereoBootstrap();
}

kharvox::AerWorldViewResult KharvoxCameraAlignAerWorldView(const float pose[12],const float fov[2],
    kharvox::AerWorldView& source,kharvox::AerWorldView& target){
    using R=kharvox::AerWorldViewResult;
    if(kharvox::native::requested()||!KharvoxCameraGameplayActive()||KharvoxCameraCutsceneActive()
        ||animatedSequenceActive.load(std::memory_order_acquire)||!stereoEyeEnabled.load())return R::Invalid;
    const auto head=renderHeadPose();const auto offset=stereoEyeOffset.load(std::memory_order_acquire);
    if(!head.valid||offset==0||!aerRenderPairEnabled.load(std::memory_order_acquire))return R::Invalid;
    const auto result=aerWorldViewHistory.align(pose,fov,head.poseId,kharvox::aerEyeFromDoomOffset(offset),
        KharvoxCameraCurrentPresentSerial(),KharvoxCameraLevelTransitionGeneration(),source,target);
    // A phase change during lookup invalidates the target; never mix the two
    // publications to manufacture a replacement camera.
    if(renderHeadPose().poseId!=head.poseId||stereoEyeOffset.load()!=offset)return R::MissingTarget;
    return result;
}

kharvox::AerWorldViewResult KharvoxCameraRecognizeAerWorldView(const float pose[12],const float fov[2],kharvox::AerWorldView& source){
    // A queued draw may belong to the prior cinematic/gameplay camera. Its
    // exact source identity, not the latest global mode, decides recognition.
    if(kharvox::native::requested())return kharvox::AerWorldViewResult::Invalid;
    return aerWorldViewHistory.recognize(pose,fov,KharvoxCameraCurrentPresentSerial(),KharvoxCameraLevelTransitionGeneration(),source);
}
void KharvoxCameraRecordAerWorldSource(const kharvox::AerWorldView& source){aerSourceWindow.observe({source.poseId,source.level,source.eye,source.domain});}
kharvox::AerSourceObservation KharvoxCameraTakeAerWorldSource(){return aerSourceWindow.take();}
bool KharvoxCameraUsesAerGameplaySource(){
    return aerRenderPairEnabled.load(std::memory_order_acquire)&&!kharvox::native::requested()
        &&KharvoxCameraGameplayActive()&&!KharvoxCameraCutsceneActive()
        &&!animatedSequenceActive.load(std::memory_order_acquire);
}

void KharvoxCameraCompleteNativeStereoBootstrapAfterRender() {
    completeNativeStereoBootstrapAfterRender();
}

void KharvoxCameraSetHeadPose(
    float yawDegrees, float pitchDegrees, float rollDegrees,
    float forwardUnits, float lateralUnits, float upUnits,
    bool valid, float referenceBodyYaw, bool referenceBodyValid) {
    std::lock_guard<std::mutex> guard(headPoseMutex);
    ++headPoseId;
    if(kharvox::pose_trace::active.load(std::memory_order_relaxed)){
        kharvox::pose_trace::Event e{};e.kind=kharvox::pose_trace::HeadPublished;
        e.poseId=headPoseId;e.frame=KharvoxCameraCurrentPresentSerial();e.flags=valid?1u:0u;
        e.data[0]=yawDegrees;e.data[1]=pitchDegrees;e.data[2]=rollDegrees;
        e.data[3]=forwardUnits;e.data[4]=lateralUnits;e.data[5]=upUnits;
        e.data[6]=referenceBodyYaw;kharvox::pose_trace::record(e);
    }
    headReferenceBodyYaw.store(referenceBodyYaw);
    headReferenceBodyValid.store(referenceBodyValid&&valid);
    headYaw.store(yawDegrees, std::memory_order_relaxed);
    headPitch.store(pitchDegrees, std::memory_order_relaxed);
    headRoll.store(rollDegrees, std::memory_order_relaxed);
    headForward.store(forwardUnits, std::memory_order_relaxed);
    headLateral.store(lateralUnits, std::memory_order_relaxed);
    headUp.store(upUnits, std::memory_order_relaxed);
    headValid.store(valid, std::memory_order_release);
}

void KharvoxCameraSetArtificialTurnYaw(float yawDegrees) {
    std::lock_guard<std::mutex> guard(headPoseMutex);
    const float previous = artificialTurnYaw.exchange(yawDegrees, std::memory_order_acq_rel);
    const bool activated = std::abs(previous) < 0.001f && std::abs(yawDegrees) >= 0.001f;
    const bool cleared = std::abs(previous) >= 0.001f && std::abs(yawDegrees) < 0.001f;
    if (activated || cleared) {
        log("[TURN] central artificial render/gameplay yaw=" + std::to_string(yawDegrees));
    }
}

unsigned long long KharvoxCameraDiagnosticPoseId() {
    return renderHeadPose().poseId;
}

void KharvoxCameraSetCrouchState(bool active) {
    crouchRequested.store(active, std::memory_order_release);
}

void KharvoxCameraSetImmersiveCinematicFov(
    float fovXDegrees, float fovYDegrees,
    bool requested, bool active) {
    const auto overrideFov=kharvox::cinematicOverrideFov(stereoEyeEnabled.load(std::memory_order_acquire),
        kharvox::native::requested(),fovXDegrees,fovYDegrees,stereoFovX.load(),stereoFovY.load());
    fovXDegrees=overrideFov[0];fovYDegrees=overrideFov[1];
    const bool valid = std::isfinite(fovXDegrees) && std::isfinite(fovYDegrees)
        && fovXDegrees > 1.0f && fovYDegrees > 1.0f;
    if (valid) {
        interlockedFloatStore(&immersiveCinematicFovXBits, fovXDegrees);
        interlockedFloatStore(&immersiveCinematicFovYBits, fovYDegrees);
    }
    InterlockedExchange(&immersiveCinematicFovRequested, requested && valid ? 1 : 0);
    const LONG hookEnabled = requested && active && valid ? 1 : 0;
    const LONG previous = InterlockedExchange(&immersiveCinematicFovHookActive, hookEnabled);
    if (previous != hookEnabled) {
        if (hookEnabled) {
            log("[FOV] immersive exact OpenXR match ACTIVE; cinematic zoom disabled target="
                + std::to_string(fovXDegrees) + "x" + std::to_string(fovYDegrees));
        } else if (requested) {
            log("[FOV] DOOMCutsceneFOV override inactive outside cinematic");
        }
    }
}

void KharvoxCameraSetImmersiveCinematicFreelook(bool requested) {
    const LONG enabled = requested ? 1 : 0;
    const LONG previous = InterlockedExchange(
        &immersiveCinematicFreelookRequested, enabled);
    if (previous != enabled)
        log(std::string("[IMMERSIVE] cinematic HMD freelook ")
            + (requested ? "ENABLED" : "disabled"));
}

bool KharvoxCameraInstallDiagnosticHook() {
    static bool attempted = false;
    if (attempted) return sample.hits != 0;
    attempted = true;
    KharvoxWeaponInstallHook();
    KharvoxHudInstallHook();
    auto image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (image) {
        installPlayerPhysicsOriginCapture(image);
        installPlayerFocusTraceHook(image);
        installPlayerLedgeStateCapture(image);
        validateSyncAttackClassifier(image);
        validatePlayerControlClassifier(image);
    }

    // Independently measured camera-update signature. This is the tail of the
    // function that copies camera position and a 3x3 basis into rcx+0xC0.
    constexpr unsigned char signature[] = {
        0x89,0x81,0xE8,0x00,0x00,0x00,
        0x41,0x8B,0x40,0x20,
        0x89,0x81,0xEC,0x00,0x00,0x00,
        0xC3
    };
    auto target = findTextSignature(signature, sizeof(signature));
    if (!target) {
        log("DOOMPatchCamera signature mismatch; native hook not installed");
        return false;
    }

    auto stub = static_cast<unsigned char*>(VirtualAlloc(nullptr, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!stub) {
        log("DOOMPatchCamera stub allocation failed");
        return false;
    }

    auto p = stub;
    // Execute the two displaced matrix copies, then process the fully
    // populated camera object in rcx, matching the reference hook timing.
    std::memcpy(p, signature, sizeof(signature) - 1);
    p += sizeof(signature) - 1;
    emit8(p, 0x48); emit8(p, 0x8B); emit8(p, 0x14); emit8(p, 0x24); // rdx = return address
    emit8(p, 0x50);                                      // push rax
    emit8(p, 0x48); emit8(p, 0x83); emit8(p, 0xEC); emit8(p, 0x20); // shadow space
    emit8(p, 0x48); emit8(p, 0xB8);
    emit64(p, reinterpret_cast<unsigned long long>(patchCamera));
    emit8(p, 0xFF); emit8(p, 0xD0);                      // call rax
    emit8(p, 0x48); emit8(p, 0x83); emit8(p, 0xC4); emit8(p, 0x20);
    emit8(p, 0x58);                                      // pop rax
    emit8(p, 0xC3);                                      // original return

    DWORD old{};
    if (!VirtualProtect(target, sizeof(signature), PAGE_EXECUTE_READWRITE, &old)) {
        log("DOOMPatchCamera target protection failed");
        return false;
    }
    unsigned char jump[sizeof(signature)] = {0xFF,0x25,0,0,0,0};
    const auto address = reinterpret_cast<unsigned long long>(stub);
    std::memcpy(jump + 6, &address, sizeof(address));
    for (size_t i = 14; i < sizeof(jump); ++i) jump[i] = 0x90;
    std::memcpy(target, jump, sizeof(jump));
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(jump));
    VirtualProtect(target, sizeof(jump), old, &old);

    std::ostringstream installed;
    installed << "DOOMPatchCamera hook installed RVA=0x" << std::hex
              << (target - reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr)));
    log(installed.str());
    installCutsceneFovHook();
    if(!kharvox::native::requested()){
        installNativeSameFrameStereo();
        installSameFrameProofHook();
    }
    return true;
}

void KharvoxCameraPollDiagnostic() {
    // Runs for active VR presentation, independently of Extended Logging and
    // runtime/backend. Keep engine-side shake/kick rotations out of VR views.
    const auto image=reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    const auto dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    const auto nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(image+dos->e_lfanew);
    const auto viewEffects=kharvox::enforceViewEffects(image,nt->OptionalHeader.SizeOfImage);
    static bool viewEffectsLogged=false;
    if(!viewEffectsLogged||viewEffects==1||viewEffects==2||viewEffects==3){
        viewEffectsLogged=true;
        log(viewEffects==4?"[VR-VIEW-EFFECTS] engine layout/value mismatch; enforcement skipped":
            "[VR-VIEW-EFFECTS] effective view_skipShakes=1 view_skipKicks=1 repairedMask="+std::to_string(viewEffects));
    }
    cameraPresentSerial.fetch_add(1, std::memory_order_acq_rel);
    const auto cutsceneHits=InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(&cutsceneFovHits),0,0);
    static unsigned long long previousCutsceneHits{};
    if(cutsceneHits!=previousCutsceneHits){
        static float previousFov{};
        const float value=cutsceneFovValue;
        if(std::fabs(value-80.0f)>0.1f&&std::fabs(value-90.0f)>0.1f&&std::fabs(value-previousFov)>0.05f){log("DOOMCutsceneFOV unusual value="+std::to_string(value));previousFov=value;}
    }
    previousCutsceneHits=cutsceneHits;
    const auto positionDirection = InterlockedExchangeAdd(&manualPositionApplied, 0);
    if (positionDirection == -1 && InterlockedCompareExchange(&manualPositionApplied, -2, -1) == -1)
        log("F9 DOOMPatchCamera local-X position -12 units applied");
    if (positionDirection == 1 && InterlockedCompareExchange(&manualPositionApplied, 2, 1) == 1)
        log("F10 DOOMPatchCamera local-X position +12 units applied");

    const auto hits = sample.hits;
    static unsigned long long previousHits{};
    static unsigned stableFrames{}, missingFrames{};
    const bool hitThisFrame = hits != previousHits;
    previousHits = hits;
    static float previousCameraFovX{},previousCameraFovY{};
    if(hitThisFrame&&(std::fabs(sample.fovX-previousCameraFovX)>1.0f||std::fabs(sample.fovY-previousCameraFovY)>1.0f)){
        log("DOOM camera source FOV="+std::to_string(sample.fovX)+"x"+std::to_string(sample.fovY));
        previousCameraFovX=sample.fovX;previousCameraFovY=sample.fovY;
    }
    if (hitThisFrame) {
        missingFrames = 0;
        // Presentation also requires a fresh player-physics capture, so the
        // former 20-consecutive-hit debounce added only latency. During a
        // low-FPS/loading handoff any missed diagnostic frame restarted the
        // count and could leave a ready level in QUAD for several seconds.
        constexpr unsigned activationFrames = 3;
        if (stableFrames < activationFrames) ++stableFrames;
        if (stableFrames == activationFrames && !worldCameraActive.exchange(true))
            log("world camera ACTIVE after 3 stable present frames");
    } else {
        stableFrames = 0;
        if (missingFrames < 90) ++missingFrames;
        if (missingFrames == 3) invalidateLevelReferences();
        if (missingFrames == 90 && worldCameraActive.exchange(false))
            log("world camera INACTIVE after 90 present frames without a hit");
    }

    if (nativeSameFrameInstalled.load(std::memory_order_acquire)) {
        if (!worldCameraActive.load(std::memory_order_acquire)
            || cutsceneActive.load(std::memory_order_acquire))
            nativeStereoFrameReady.store(false, std::memory_order_release);
    }
    if (nativeTwoViewInstalled.load(std::memory_order_acquire)) {
        const bool shouldRenderStereo = worldCameraActive.load(std::memory_order_acquire)
            && !cutsceneActive.load(std::memory_order_acquire);
        const bool layoutEnabled = nativeStereoLayoutEnabled.load(std::memory_order_acquire);
        if (shouldRenderStereo && !layoutEnabled
            && nativeStereoBootstrapStage.load(std::memory_order_acquire) == 0) {
            nativeStereoLayoutEnabled.store(true, std::memory_order_release);
            nativeStereoFrameReady.store(false, std::memory_order_release);
            nativeStereoBootstrapStage.store(1, std::memory_order_release);
            log("[Stereo] stable gameplay reached; genuine native two-view bootstrap scheduled");
        } else if (!shouldRenderStereo && (layoutEnabled
                   || nativeStereoBootstrapStage.load(std::memory_order_acquire) != 0)) {
            auto image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
            if (image) {
                constexpr uintptr_t viewLayoutSlotRva = 0x36111E8;
                constexpr uintptr_t singleLayoutRva = 0x284FFC8;
                InterlockedExchangePointer(
                    reinterpret_cast<void* volatile*>(image + viewLayoutSlotRva),
                    image + singleLayoutRva);
            }
            nativeStereoBootstrapStage.store(0, std::memory_order_release);
            nativeStereoLayoutEnabled.store(false, std::memory_order_release);
            nativeStereoFrameReady.store(false, std::memory_order_release);
            log("[Stereo] genuine native two-view suspended outside gameplay");
        }
    }
}

bool KharvoxCameraWorldActive() {
    return worldCameraActive.load(std::memory_order_acquire);
}

bool KharvoxCameraGameplayActive() {
    if (!worldCameraActive.load(std::memory_order_acquire)) return false;
    // Scripted first-person cameras intentionally stop refreshing the native
    // player hook. Their held body pose remains valid until gameplay returns.
    if (cutsceneActive.load(std::memory_order_acquire)) return true;
    if (!playerPhysicsOriginValid.load(std::memory_order_acquire)) return false;
    const auto present = cameraPresentSerial.load(std::memory_order_acquire);
    const auto captured = playerPhysicsCapturePresent.load(std::memory_order_acquire);
    return captured && present >= captured && present - captured <= 2;
}

bool KharvoxCameraNativeStereoActive() {
    return nativeStereoFrameReady.load(std::memory_order_acquire);
}

bool KharvoxCameraNativeTwoViewActive() {
    return nativeTwoViewInstalled.load(std::memory_order_acquire)
        && nativeStereoFrameReady.load(std::memory_order_acquire);
}

bool KharvoxCameraCutsceneActive() { return cutsceneActive.load(std::memory_order_acquire); }

bool KharvoxCameraSyncAttackActive() {
    // idPlayer+0x3DC9 is the exact flag DOOM's native usability path checks
    // before reporting that the local activator is instigating a sync attack.
    // Campaign Glory Kills hold it for their native sync-attack lifetime.
    constexpr uintptr_t syncAttackInstigatorOffset = 0x3DC9;
    const uintptr_t owner = playerPhysicsOwner.load(std::memory_order_acquire);
    if (!syncAttackClassifierSupported.load(std::memory_order_acquire)
        || !owner || owner > UINTPTR_MAX - syncAttackInstigatorOffset - 1
        || !readableMemory(reinterpret_cast<const void*>(owner),
            syncAttackInstigatorOffset + 1))
        return false;
#if defined(_MSC_VER)
    __try {
        return *reinterpret_cast<const unsigned char*>(
            owner + syncAttackInstigatorOffset) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    return *reinterpret_cast<const unsigned char*>(
        owner + syncAttackInstigatorOffset) != 0;
#endif
}

namespace {
bool readBossSequenceBytes(uintptr_t address,void* output,size_t size){
    if(!readableMemory(reinterpret_cast<const void*>(address),size))return false;
#if defined(_MSC_VER)
    __try {
        std::memcpy(output,reinterpret_cast<const void*>(address),size);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
#else
    std::memcpy(output,reinterpret_cast<const void*>(address),size);return true;
#endif
}
using BossObservation=kharvox::CyberdemonSequenceObservation;
bool readNativeSequencePlayer(uintptr_t& owner){
    const auto image=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    static const bool supported=[&]{
        uint64_t listField{};
        constexpr unsigned char globalRead[]{0x48,0x8b,0x05,0x2c,0x50,0x7b,0x05};
        unsigned char actual[sizeof(globalRead)]{};
        return readBossSequenceBytes(image+0x3035f10,&listField,8)
            &&listField==0x20000a5d48ull
            &&readBossSequenceBytes(image+0x35a69d,actual,sizeof(actual))
            &&!std::memcmp(actual,globalRead,sizeof(actual));
    }();
    uintptr_t game{},gameAgain{},vtable{},entries{},entriesAgain{};
    int count{};
    struct Managed {uint32_t id,check;uintptr_t entity;};
    Managed player{},again{};
    if(!supported||!readBossSequenceBytes(image+0x5b0f6d0,&game,8)||!game
        ||!readBossSequenceBytes(game,&vtable,8)||vtable!=image+0x201e3d8
        ||!readBossSequenceBytes(game+0xa5d50,&entries,8)||!entries
        ||!readBossSequenceBytes(game+0xa5d58,&count,4)||count<1||count>4
        ||!readBossSequenceBytes(entries,&player,sizeof(player))||!player.entity
        ||player.id!=player.check||player.id==0x1fffffe
        ||!readBossSequenceBytes(image+0x5b0f6d0,&gameAgain,8)||gameAgain!=game
        ||!readBossSequenceBytes(game+0xa5d50,&entriesAgain,8)||entriesAgain!=entries
        ||!readBossSequenceBytes(entries,&again,sizeof(again))
        ||player.id!=again.id||player.entity!=again.entity||player.check!=again.check)return false;
    owner=player.entity;return true;
}
// The map's camera-only twins transition need not bind idPlayer.syncMaster.
// Discover its pre-spawned native sequence entities only when a known Hell Guards
// player sequence first primes this owner. Do not scan the level every frame.
BossObservation readHellGuardsMapCamera(uintptr_t owner,bool prime,char (&name)[128]){
    const auto image=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    static const bool supported=[&]{
        uint64_t spawnedField{},workingField{};
        return readBossSequenceBytes(image+0x3035ad8,&spawnedField,8)
            &&spawnedField==0x20000a54d0ull
            &&readBossSequenceBytes(image+0x300f600,&workingField,8)
            &&workingField==0x100004e98ull;
    }();
    if(!supported)return BossObservation::Inactive;
    uintptr_t game{},vtable{};
    const bool validGame=readBossSequenceBytes(image+0x5b0f6d0,&game,8)&&game
        &&readBossSequenceBytes(game,&vtable,8)&&vtable==image+0x201e3d8;
    // A null global game pointer is normal between DOOM update phases. It does
    // not end a map cinematic whose validated native entity is still active.
    struct Candidate {uintptr_t entity{},decl{};std::array<char,128> name{};};
    static std::mutex mutex;
    static uintptr_t cachedOwner{},cachedGame{};
    static std::array<Candidate,8> candidates{};
    static size_t candidateCount{};
    static bool scanned{};
    static ULONGLONG nextScan{};
    std::lock_guard lock(mutex);
    if(kharvox::shouldResetBossMapCameraScope(cachedOwner,cachedGame,owner,game,validGame)){
        cachedOwner=owner;cachedGame=game;candidateCount=0;scanned=false;nextScan=0;
    }
    if(prime&&validGame&&owner&&!scanned&&GetTickCount64()>=nextScan){
        nextScan=GetTickCount64()+500;
        struct Managed {uint32_t id,check;uintptr_t entity;uint64_t remaining[2];};
        uintptr_t entries{},again{};int count{},countAgain{};
        if(!readBossSequenceBytes(game+0xa54d8,&entries,8)||!entries
            ||!readBossSequenceBytes(game+0xa54e0,&count,4)||count<1||count>32768)
            return BossObservation::Unknown;
        std::vector<Managed> snapshot(static_cast<size_t>(count));
        if(!readBossSequenceBytes(entries,snapshot.data(),snapshot.size()*sizeof(Managed))
            ||!readBossSequenceBytes(game+0xa54d8,&again,8)||again!=entries
            ||!readBossSequenceBytes(game+0xa54e0,&countAgain,4)||countAgain!=count)
            return BossObservation::Unknown;
        candidateCount=0;
        for(const auto& ref:snapshot){
            Candidate candidate{};uintptr_t text{};
            if(!ref.entity||ref.id!=ref.check||ref.id==0x1fffffe
                ||!readBossSequenceBytes(ref.entity,&vtable,8)||vtable!=image+0x21ab128
                ||!readBossSequenceBytes(ref.entity+0x6d0,&candidate.decl,8)||!candidate.decl
                ||!readBossSequenceBytes(candidate.decl+8,&text,8)||!text
                ||!readBossSequenceBytes(text,candidate.name.data(),candidate.name.size()))continue;
            const auto end=static_cast<const char*>(std::memchr(candidate.name.data(),0,candidate.name.size()));
            if(!end||!kharvox::isHellGuardsCameraSequence(std::string_view(candidate.name.data(),end-candidate.name.data())))continue;
            candidate.entity=ref.entity;
            if(candidateCount<candidates.size())candidates[candidateCount++]=candidate;
        }
        scanned=std::any_of(candidates.begin(),candidates.begin()+candidateCount,[](const auto& candidate){
            return std::string_view(candidate.name.data()).find("_twins_cine_")!=std::string_view::npos;
        });
        log("[BOSS-QUAD] r313 Hell Guards map camera candidates="+std::to_string(candidateCount));
    }
    // Re-read the native flag even within the same present: a scene can start
    // after an earlier presentation query but before its first camera write.
    bool unavailable=false;
    for(size_t i=0;i<candidateCount;++i){
        const auto& candidate=candidates[i];uintptr_t decl{};unsigned char working{};
        if(!readBossSequenceBytes(candidate.entity,&vtable,8)||vtable!=image+0x21ab128
            ||!readBossSequenceBytes(candidate.entity+0x6d0,&decl,8)||decl!=candidate.decl
            ||!readBossSequenceBytes(candidate.entity+0x4e98,&working,1)){
            unavailable=true;continue;
        }
        if(!kharvox::isActiveHellGuardsCameraSequence(candidate.name.data(),working))continue;
        // Revalidate identity after the flag read; cached names cannot authorize a reused object.
        if(!readBossSequenceBytes(candidate.entity+0x6d0,&decl,8)||decl!=candidate.decl){
            unavailable=true;continue;
        }
        if(!validGame){
            static std::atomic<bool> retainedLogged{};
            if(!retainedLogged.exchange(true))log("[BOSS-QUAD] r313 active map camera retained across unavailable global game context");
        }
        std::memcpy(name,candidate.name.data(),sizeof(name));
        return BossObservation::Active;
    }
    return unavailable?BossObservation::Unknown:BossObservation::Inactive;
}
BossObservation readCurrentBossSequenceAt(uintptr_t owner,uintptr_t referenceOffset,char (&name)[128]){
    const auto image=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    static const bool supported=[&]{
        uint64_t masterField{},savedField{},declField{};
        return readBossSequenceBytes(image+0x30aa120,&masterField,8)
            &&readBossSequenceBytes(image+0x30aa1f8,&savedField,8)
            &&readBossSequenceBytes(image+0x307b400,&declField,8)
            &&masterField==0x200000d198ull&&savedField==0x200000da18ull
            &&declField==0x8000006d0ull;
    }();
    if(!supported||!owner)return BossObservation::Unknown;
    struct Managed {uint32_t id,check;uintptr_t entity;};
    Managed master{},again{};
    uintptr_t vtable{},decl{},text{};
    if(!readBossSequenceBytes(owner+referenceOffset,&master,sizeof(master)))return BossObservation::Unknown;
    if(!master.entity)return BossObservation::Inactive;
    if(master.id!=master.check||master.id==0x1fffffe
        ||!readBossSequenceBytes(master.entity,&vtable,8)||vtable!=image+0x21ab128
        ||!readBossSequenceBytes(master.entity+0x6d0,&decl,8)||!decl
        ||!readBossSequenceBytes(decl+8,&text,8)||!text)return BossObservation::Unknown;
    if(!readBossSequenceBytes(text,name,sizeof(name))){
        for(size_t i=0;i<sizeof(name);++i){
            if(!readBossSequenceBytes(text+i,name+i,1))return BossObservation::Unknown;
            if(!name[i])break;
        }
    }
    const auto end=static_cast<const char*>(std::memchr(name,0,sizeof(name)));
    if(!end||!readBossSequenceBytes(owner+referenceOffset,&again,sizeof(again))
        ||master.id!=again.id||master.check!=again.check||master.entity!=again.entity)return BossObservation::Unknown;
    return kharvox::isBossQuadSequence(std::string_view(name,end-name))
        ?BossObservation::Active:BossObservation::Inactive;
}
}
bool KharvoxCameraBossSequenceActive(){
    char name[128]{};
    uintptr_t owner{},master{};
    // Read DOOM's native player list independently of the gameplay physics
    // capture, which is deliberately invalidated during checkpoint loading.
    const bool readable=readNativeSequencePlayer(owner)
        &&readBossSequenceBytes(owner+0xd1a0,&master,sizeof(master));
    auto observation=readable?readCurrentBossSequenceAt(owner,master?0xd198:0xda18,name)
        :BossObservation::Unknown;
    const bool prime=observation==BossObservation::Active&&kharvox::isHellGuardsQuadSequence(name);
    char mapName[128]{};
    const auto mapObservation=readHellGuardsMapCamera(owner,prime,mapName);
    observation=kharvox::combineBossSequenceObservations(observation,mapObservation);
    if(mapObservation==BossObservation::Active)std::memcpy(name,mapName,sizeof(name));
    static std::mutex holdMutex;
    static kharvox::CyberdemonSequenceHold hold;
    bool active{};
    {
        std::lock_guard lock(holdMutex);
        active=hold.update(observation,KharvoxCameraCurrentPresentSerial(),
            KharvoxCameraLevelTransitionGeneration(),cutsceneActive.load(std::memory_order_acquire));
    }
    static std::atomic<bool> previous{};
    if(previous.exchange(active,std::memory_order_acq_rel)!=active)
        log(active?std::string("[BOSS-QUAD] r313 native boss sequence: ")+name
            :"[BOSS-QUAD] r313 sequence ended/unavailable; automatic presentation restored");
    return active;
}

bool KharvoxCameraLedgeTransitionActive() {
    const uintptr_t owner = playerPhysicsOwner.load(std::memory_order_acquire);
    return owner && playerLedgeTransitionActive.load(std::memory_order_acquire)
        && playerLedgeTransitionOwner.load(std::memory_order_acquire) == owner;
}

bool KharvoxCameraPlayerWeaponControlActive() {
    constexpr uintptr_t inhibitFlagsOffset = 0x14C4C;
    const uintptr_t owner = playerPhysicsOwner.load(std::memory_order_acquire);
    if (!playerControlClassifierSupported.load(std::memory_order_acquire)
        || !worldCameraActive.load(std::memory_order_acquire)
        || !playerPhysicsOriginValid.load(std::memory_order_acquire)
        || !owner || owner > UINTPTR_MAX - inhibitFlagsOffset - sizeof(uint32_t))
        return false;

    const auto flagsAddress = reinterpret_cast<const uint32_t*>(
        owner + inhibitFlagsOffset);
    if (!readableMemory(flagsAddress, sizeof(*flagsAddress))) return false;
#if defined(_MSC_VER)
    __try {
        return kharvox::playerWeaponControlAvailable(*flagsAddress);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    return kharvox::playerWeaponControlAvailable(*flagsAddress);
#endif
}

bool KharvoxCameraGetBodyPose(float origin[3], float axis[9]) {
    if (!origin || !axis || !bodyCameraValid.load(std::memory_order_acquire)) return false;
    for (int index = 0; index < 9; ++index)
        axis[index] = bodyCameraAxis[index].load(std::memory_order_relaxed);
    uintptr_t liveOwner{};
    float livePhysicsOrigin[3]{};
    const bool liveOffsetValid = stableBodyViewOffsetValid.load(std::memory_order_acquire);
    const uintptr_t offsetOwner = stableBodyViewOffsetOwner.load(std::memory_order_relaxed);
    if (liveOffsetValid && readLivePlayerPhysicsOrigin(livePhysicsOrigin, &liveOwner)
        && liveOwner == offsetOwner) {
        float viewOffsetLocal[3]{};
        for (int index = 0; index < 3; ++index)
            viewOffsetLocal[index] = stableBodyViewOffsetLocal[index].load(std::memory_order_relaxed);
        for (int worldAxis = 0; worldAxis < 3; ++worldAxis)
            origin[worldAxis] = livePhysicsOrigin[worldAxis]
                + axis[worldAxis] * viewOffsetLocal[0]
                + axis[3 + worldAxis] * viewOffsetLocal[1]
                + axis[6 + worldAxis] * viewOffsetLocal[2];
        return true;
    }
    for (int index = 0; index < 3; ++index)
        origin[index] = bodyCameraOrigin[index].load(std::memory_order_relaxed);
    return true;
}

bool KharvoxCameraGetHeadRenderPose(float origin[3], float axis[9]) {
    if (!origin || !axis || !KharvoxCameraGetBodyPose(origin, axis)) return false;
    const auto head = renderHeadPoseForBody(axis);
    if (!head.valid) return true;

    const float forward = head.forward;
    const float lateral = head.lateral;
    const float up = head.up;
    for (int world = 0; world < 3; ++world)
        origin[world] += axis[world] * forward
            + axis[3 + world] * lateral
            + axis[6 + world] * up;

    applyResidualHeadOrientation(axis,head);
    if(kharvox::pose_trace::active.load(std::memory_order_relaxed)){
        kharvox::pose_trace::Event e{};e.kind=kharvox::pose_trace::HeadRenderCenter;
        e.poseId=head.poseId;e.frame=KharvoxCameraCurrentPresentSerial();
        for(int i=0;i<3;++i)e.data[i]=origin[i];
        for(int i=0;i<9;++i)e.data[i+3]=axis[i];
        kharvox::pose_trace::record(e);
    }
    return true;
}

bool KharvoxCameraGetHudCenterRenderPose(float origin[3], float axis[9]) {
    if (!origin || !axis
        || !hudRenderPoseValid.load(std::memory_order_acquire)) return false;

    for (int attempt = 0; attempt < 4; ++attempt) {
        const auto before = hudRenderPoseSequence.load(std::memory_order_acquire);
        if (before & 1) continue;
        for (int index = 0; index < 3; ++index) {
            origin[index] = hudRenderAnchorOrigin[index].load(std::memory_order_relaxed);
        }
        for (int index = 0; index < 9; ++index) {
            axis[index] = hudRenderAnchorAxis[index].load(std::memory_order_relaxed);
        }
        const auto snapshotPresent = hudRenderPosePresent.load(std::memory_order_relaxed);
        const auto after = hudRenderPoseSequence.load(std::memory_order_acquire);
        if (before != after || (after & 1)) continue;

        const auto currentPresent = cameraPresentSerial.load(std::memory_order_acquire);
        if (snapshotPresent == currentPresent) {
            static std::atomic<bool> exactLogged{};
            if (!exactLogged.exchange(true, std::memory_order_relaxed))
                log("[HUD11-FLAT] exact same-render central camera snapshot active");
            return true;
        }

        // vkQueuePresentKHR advances cameraPresentSerial before all late HUD
        // front-end work has necessarily retired. In that short boundary the
        // most recently published central camera is still the camera which
        // authored this HUD work. Rejecting it switched one frame to the live
        // body/head reconstruction, whose timing and body offset differ. Keep
        // only this precisely bounded one-Present snapshot; anything genuinely
        // older remains fail-closed and may use the normal fallback path.
        const bool onePresentBoundary = currentPresent > snapshotPresent
            && currentPresent - snapshotPresent == 1;
        if (onePresentBoundary) {
            static std::atomic<bool> boundaryLogged{};
            if (!boundaryLogged.exchange(true, std::memory_order_relaxed))
                log("[HUD15-POSE] one-Present camera boundary retained; live-pose source switch suppressed");
            return true;
        }

        static std::atomic<bool> staleLogged{};
        if (!staleLogged.exchange(true, std::memory_order_relaxed)) {
            log("[HUD15-POSE] central render-camera snapshot older than one Present; rejected");
        }
        return false;
    }
    return false;
}

unsigned long long KharvoxCameraCurrentPresentSerial() {
    return cameraPresentSerial.load(std::memory_order_acquire);
}

unsigned long long KharvoxCameraLevelTransitionGeneration() {
    return levelTransitionGeneration.load(std::memory_order_acquire);
}

bool KharvoxCameraGetPlayerPhysicsOrigin(float origin[3]) {
    if (!origin || !playerPhysicsOriginValid.load(std::memory_order_acquire)) return false;
    for (int index = 0; index < 3; ++index)
        origin[index] = playerPhysicsOrigin[index].load(std::memory_order_relaxed);
    return true;
}

void KharvoxCameraSetStereoEye(float localXUnits, float fovXDegrees, float fovYDegrees, float opticalCenterYawDegrees, float opticalCenterPitchDegrees, bool enabled) {
    stereoEyeOffset.store(localXUnits, std::memory_order_relaxed);
    stereoFovX.store(fovXDegrees, std::memory_order_relaxed);
    stereoFovY.store(fovYDegrees, std::memory_order_relaxed);
    stereoOpticalCenterYaw.store(opticalCenterYawDegrees, std::memory_order_relaxed);
    stereoOpticalCenterPitch.store(opticalCenterPitchDegrees, std::memory_order_relaxed);
    stereoEyeEnabled.store(enabled, std::memory_order_release);
    // The downstream cutscene FOV writer must use the same programmed eye,
    // not restore an enclosing mono frustum after the camera hook.
    if(enabled&&!kharvox::native::requested()){
        interlockedFloatStore(&immersiveCinematicFovXBits,fovXDegrees);
        interlockedFloatStore(&immersiveCinematicFovYBits,fovYDegrees);
    }
}

void KharvoxCameraSetAnimatedSequenceActive(bool active) {
    animatedSequenceActive.store(active, std::memory_order_release);
}

void KharvoxCameraSetAerRenderPair(int renderEye, bool enabled) {
    // Only the pair-cache phase is remapped; the physical camera eye is not.
    const int physicalEye = renderEye;
    renderEye = kharvox::aerRenderPairPhase(renderEye);
    std::lock_guard<std::mutex> guard(headPoseMutex);
    const int normalizedEye = enabled && renderEye == 1 ? 1 : enabled ? 0 : -1;
    const bool previousEnabled = aerRenderPairEnabled.load(std::memory_order_acquire);
    if (enabled && (normalizedEye == 0 || !previousEnabled
            || !aerHeadPairSnapshotValid.load(std::memory_order_acquire))) {
        aerHeadPairSnapshotValid.store(false, std::memory_order_release);
        aerHeadPairPose[0].store(headYaw.load(std::memory_order_relaxed), std::memory_order_relaxed);
        aerHeadPairPose[1].store(artificialTurnYaw.load(std::memory_order_relaxed), std::memory_order_relaxed);
        aerHeadPairPose[2].store(headPitch.load(std::memory_order_relaxed), std::memory_order_relaxed);
        aerHeadPairPose[3].store(headRoll.load(std::memory_order_relaxed), std::memory_order_relaxed);
        aerHeadPairPose[4].store(headForward.load(std::memory_order_relaxed), std::memory_order_relaxed);
        aerHeadPairPose[5].store(headLateral.load(std::memory_order_relaxed), std::memory_order_relaxed);
        aerHeadPairPose[6].store(headUp.load(std::memory_order_relaxed), std::memory_order_relaxed);
        aerHeadPairReferenceYaw=headReferenceBodyYaw.load();
        aerHeadPairPoseId=headPoseId;
        // The render worker can consume this ID as soon as headPoseMutex is
        // released. Register its controller sample BEFORE publishing the pair,
        // not in the later independent WeaponSetAerRenderPair call.
        KharvoxWeaponRememberAerInput(aerHeadPairPoseId);
        aerHeadPairReferenceValid=headReferenceBodyValid.load();
        aerHeadPairTracked.store(headValid.load(std::memory_order_acquire), std::memory_order_relaxed);
        aerHeadPairSnapshotValid.store(true, std::memory_order_release);
    } else if (!enabled) {
        aerHeadPairSnapshotValid.store(false, std::memory_order_release);
    }
    aerCameraPairPoses.begin(physicalEye, enabled);
    aerRenderPairEnabled.store(enabled, std::memory_order_release);
}

void KharvoxCameraSetNativeStereoParameters(float halfEyeSeparationUnits, float screenSeparation) {
    if (!nativeSameFrameInstalled.load(std::memory_order_acquire)
        && !nativeTwoViewInstalled.load(std::memory_order_acquire)) return;
    auto image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!image) return;
    // idCVar float current values live at object + 0x34 in this executable.
    constexpr uintptr_t worldSeparationCurrentRva = 0x5FB88A4;
    constexpr uintptr_t screenSeparationCurrentRva = 0x5FB8944;
    // Keep DOOM's native left/right target order. For an HMD the projection
    // screen separation is zero; only desktop/3D-TV convergence uses a
    // non-zero screen-space offset.
    constexpr uintptr_t swapEyesCurrentRva = 0x5FB8A80;
    LONG worldBits{}, screenBits{};
    static_assert(sizeof(worldBits) == sizeof(halfEyeSeparationUnits));
    std::memcpy(&worldBits, &halfEyeSeparationUnits, sizeof(worldBits));
    std::memcpy(&screenBits, &screenSeparation, sizeof(screenBits));
    InterlockedExchange(reinterpret_cast<volatile LONG*>(image + worldSeparationCurrentRva), worldBits);
    InterlockedExchange(reinterpret_cast<volatile LONG*>(image + screenSeparationCurrentRva), screenBits);
    InterlockedExchange(reinterpret_cast<volatile LONG*>(image + swapEyesCurrentRva), 0);
    static std::atomic<bool> swapLogged{};
    if (!swapLogged.exchange(true, std::memory_order_relaxed))
        log("[Stereo] DOOM HMD projection screenSeparation=0 swapEyes=0");
}

void KharvoxCameraConfigureEye(VREye eye, float localXUnits, float fovXDegrees, float fovYDegrees,
    float opticalCenterYawDegrees, float opticalCenterPitchDegrees) {
    if (eye == VREye::Mono) return;
    const size_t index = eye == VREye::Left ? 0 : 1;
    configuredEyeOffset[index].store(localXUnits, std::memory_order_relaxed);
    configuredEyeFovX[index].store(fovXDegrees, std::memory_order_relaxed);
    configuredEyeFovY[index].store(fovYDegrees, std::memory_order_relaxed);
    configuredEyeOpticalCenterYaw[index].store(opticalCenterYawDegrees, std::memory_order_relaxed);
    configuredEyeOpticalCenterPitch[index].store(opticalCenterPitchDegrees, std::memory_order_relaxed);
    configuredStereo.store(true, std::memory_order_release);
}

bool KharvoxSameFrameStereoInstalled() {
    return sameFrameInstalled.load(std::memory_order_acquire);
}

VREye KharvoxCameraCurrentEye() {
    return currentRenderEye.load(std::memory_order_acquire);
}

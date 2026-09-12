#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace kharvox {

struct MotionWeaponWheelVec3 {
    float x{};
    float y{};
    float z{};
};

struct MotionWeaponWheelQuat {
    float x{};
    float y{};
    float z{};
    float w{1.0f};
};

struct MotionWeaponWheelConfig {
    bool enabled{true};
    bool hapticsEnabled{true};
    float deadzoneMeters{0.02f};    // 2.0 cm deadzone from origin before activating stick
    float maxRadiusMeters{0.065f};  // 6.5 cm reach for 100% stick deflection
    int sectorCount{8};             // 8 radial weapon slots (45 degrees per slot)
};

struct MotionWeaponWheelInput {
    MotionWeaponWheelConfig config{};
    bool wheelActive{};
    bool stickBypass{};
    MotionWeaponWheelVec3 handPosition{};
    MotionWeaponWheelQuat hmdOrientation{};
    std::uint64_t nowNanoseconds{};
};

struct MotionWeaponWheelState {
    bool wasActive{};
    MotionWeaponWheelVec3 originHandPosition{};
    int lastSelectedSector{-1};
};

struct MotionWeaponWheelOutput {
    float stickX{};
    float stickY{};
    bool stickActive{};
    bool stickBypassActive{};
    bool triggerHapticPulse{};
    int selectedSector{-1};
};

inline MotionWeaponWheelVec3 rotateVectorByQuat(const MotionWeaponWheelQuat& q, const MotionWeaponWheelVec3& v) {
    // Rodriguez quaternion rotation formula: v' = v + w * t + (q_xyz x t), where t = 2 * (q_xyz x v)
    const float tx = 2.0f * (q.y * v.z - q.z * v.y);
    const float ty = 2.0f * (q.z * v.x - q.x * v.z);
    const float tz = 2.0f * (q.x * v.y - q.y * v.x);

    const float cx = q.y * tz - q.z * ty;
    const float cy = q.z * tx - q.x * tz;
    const float cz = q.x * ty - q.y * tx;

    return {
        v.x + q.w * tx + cx,
        v.y + q.w * ty + cy,
        v.z + q.w * tz + cz
    };
}

inline MotionWeaponWheelOutput updateMotionWeaponWheel(
    MotionWeaponWheelState& state,
    const MotionWeaponWheelInput& input) {

    MotionWeaponWheelOutput output{};

    if (!input.config.enabled || !input.wheelActive) {
        state.wasActive = false;
        state.originHandPosition = {};
        state.lastSelectedSector = -1;
        return output;
    }

    // Edge trigger on wheel open: capture hand anchor origin
    if (!state.wasActive) {
        state.wasActive = true;
        state.originHandPosition = input.handPosition;
        state.lastSelectedSector = -1;
        return output;
    }

    // Physical thumbstick bypass: user is actively using physical analog stick
    if (input.stickBypass) {
        output.stickBypassActive = true;
        return output;
    }

    // OpenXR coordinate system: +X Right, +Y Up, -Z Forward
    const MotionWeaponWheelVec3 hmdRight = rotateVectorByQuat(input.hmdOrientation, {1.0f, 0.0f, 0.0f});
    const MotionWeaponWheelVec3 hmdUp = rotateVectorByQuat(input.hmdOrientation, {0.0f, 1.0f, 0.0f});

    const MotionWeaponWheelVec3 delta{
        input.handPosition.x - state.originHandPosition.x,
        input.handPosition.y - state.originHandPosition.y,
        input.handPosition.z - state.originHandPosition.z
    };

    // Project 3D displacement onto 2D HMD View Plane
    const float screenX = delta.x * hmdRight.x + delta.y * hmdRight.y + delta.z * hmdRight.z;
    const float screenY = delta.x * hmdUp.x + delta.y * hmdUp.y + delta.z * hmdUp.z;

    const float distance = std::sqrt(screenX * screenX + screenY * screenY);

    if (distance < input.config.deadzoneMeters) {
        state.lastSelectedSector = -1;
        output.stickX = 0.0f;
        output.stickY = 0.0f;
        output.stickActive = false;
        output.selectedSector = -1;
        output.triggerHapticPulse = false;
        return output;
    }

    const float span = std::max(0.001f, input.config.maxRadiusMeters - input.config.deadzoneMeters);
    const float intensity = std::clamp((distance - input.config.deadzoneMeters) / span, 0.0f, 1.0f);
    const float dirX = screenX / distance;
    const float dirY = screenY / distance;

    output.stickX = dirX * intensity;
    output.stickY = dirY * intensity;
    output.stickActive = true;

    // Sector calculation for DOOM radial weapon wheel (8 sectors)
    // Angle in range [0, 2*PI)
    constexpr float twoPi = 6.28318530717958647692f;
    float angle = std::atan2(dirY, dirX);
    if (angle < 0.0f) {
        angle += twoPi;
    }

    const int sectorCount = std::max(1, input.config.sectorCount);
    const float sectorSpan = twoPi / static_cast<float>(sectorCount);
    const float halfSector = sectorSpan * 0.5f;

    float centeredAngle = angle + halfSector;
    while (centeredAngle >= twoPi) centeredAngle -= twoPi;

    const int sector = static_cast<int>(centeredAngle / sectorSpan) % sectorCount;
    output.selectedSector = sector;

    if (input.config.hapticsEnabled && sector != state.lastSelectedSector) {
        output.triggerHapticPulse = true;
    }
    state.lastSelectedSector = sector;

    return output;
}

} // namespace kharvox

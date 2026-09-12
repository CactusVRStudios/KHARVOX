#include "../src/openxr/MotionWeaponWheelPolicy.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "Assertion failed: " << message << std::endl;
        std::abort();
    }
}

bool near(float a, float b, float eps = 0.001f) {
    return std::fabs(a - b) <= eps;
}

} // namespace

int main() {
    kharvox::MotionWeaponWheelState state{};
    kharvox::MotionWeaponWheelInput input{};

    // 1. When wheel is inactive, output must be completely inert
    input.wheelActive = false;
    input.handPosition = {1.0f, 1.0f, 1.0f};
    auto out = kharvox::updateMotionWeaponWheel(state, input);
    check(!out.stickActive, "inactive wheel must not activate stick");
    check(!out.triggerHapticPulse, "inactive wheel must not trigger haptic");
    check(out.selectedSector == -1, "inactive wheel sector must be -1");

    // 2. First frame wheel opens: captures origin anchor and returns zero stick
    input.wheelActive = true;
    input.handPosition = {0.2f, 1.2f, -0.4f};
    input.hmdOrientation = {0.0f, 0.0f, 0.0f, 1.0f}; // Identity orientation
    out = kharvox::updateMotionWeaponWheel(state, input);
    check(!out.stickActive, "first frame anchor must not deflect stick");
    check(!out.triggerHapticPulse, "first frame must not trigger haptic");
    check(near(state.originHandPosition.x, 0.2f), "origin x must match anchor");
    check(near(state.originHandPosition.y, 1.2f), "origin y must match anchor");
    check(near(state.originHandPosition.z, -0.4f), "origin z must match anchor");

    // 3. Movement within deadzone (1.5 cm < 2.0 cm threshold)
    input.handPosition = {0.215f, 1.2f, -0.4f}; // +1.5 cm in X (Right)
    out = kharvox::updateMotionWeaponWheel(state, input);
    check(!out.stickActive, "deadzone displacement must not activate stick");
    check(out.selectedSector == -1, "deadzone sector must remain -1");
    check(!out.triggerHapticPulse, "deadzone must not trigger haptic pulse");

    // 4. Movement beyond deadzone to Sector 0 (Right: +X)
    // Distance = 4.25 cm (midway between 2.0 cm deadzone and 6.5 cm max reach)
    input.handPosition = {0.2425f, 1.2f, -0.4f}; // +4.25 cm
    out = kharvox::updateMotionWeaponWheel(state, input);
    check(out.stickActive, "movement beyond deadzone must activate stick");
    check(out.selectedSector == 0, "positive X displacement must map to Sector 0 (Right)");
    check(out.triggerHapticPulse, "entering Sector 0 from deadzone must trigger haptic pulse");
    check(out.stickX > 0.4f && out.stickX < 0.6f, "stickX should be scaled approximately to 0.5");
    check(near(out.stickY, 0.0f), "pure horizontal movement must have zero stickY");

    // Sub-frame in same sector: must NOT repeat haptic pulse
    out = kharvox::updateMotionWeaponWheel(state, input);
    check(!out.triggerHapticPulse, "staying in same sector must not re-trigger haptic pulse");

    // 5. Max radius reach and clamping (> 6.5 cm)
    input.handPosition = {0.30f, 1.2f, -0.4f}; // +10 cm (well beyond 6.5 cm)
    out = kharvox::updateMotionWeaponWheel(state, input);
    check(near(out.stickX, 1.0f), "full reach displacement must clamp stickX to 1.0");
    check(out.selectedSector == 0, "must remain in Sector 0");

    // 6. Sector transitions: test all 8 radial sectors
    // Sector 1: Up-Right (+X, +Y)
    input.handPosition = {0.25f, 1.25f, -0.4f};
    out = kharvox::updateMotionWeaponWheel(state, input);
    check(out.selectedSector == 1, "Up-Right displacement must map to Sector 1");
    check(out.triggerHapticPulse, "crossing to Sector 1 must trigger haptic pulse");

    // Sector 2: Up (+Y)
    input.handPosition = {0.2f, 1.25f, -0.4f};
    out = kharvox::updateMotionWeaponWheel(state, input);
    check(out.selectedSector == 2, "Up displacement must map to Sector 2");
    check(out.triggerHapticPulse, "crossing to Sector 2 must trigger haptic pulse");

    // Sector 3: Up-Left (-X, +Y)
    input.handPosition = {0.15f, 1.25f, -0.4f};
    out = kharvox::updateMotionWeaponWheel(state, input);
    check(out.selectedSector == 3, "Up-Left displacement must map to Sector 3");

    // Sector 4: Left (-X)
    input.handPosition = {0.15f, 1.2f, -0.4f};
    out = kharvox::updateMotionWeaponWheel(state, input);
    check(out.selectedSector == 4, "Left displacement must map to Sector 4");

    // Sector 5: Down-Left (-X, -Y)
    input.handPosition = {0.15f, 1.15f, -0.4f};
    out = kharvox::updateMotionWeaponWheel(state, input);
    check(out.selectedSector == 5, "Down-Left displacement must map to Sector 5");

    // Sector 6: Down (-Y)
    input.handPosition = {0.2f, 1.15f, -0.4f};
    out = kharvox::updateMotionWeaponWheel(state, input);
    check(out.selectedSector == 6, "Down displacement must map to Sector 6");

    // Sector 7: Down-Right (+X, -Y)
    input.handPosition = {0.25f, 1.15f, -0.4f};
    out = kharvox::updateMotionWeaponWheel(state, input);
    check(out.selectedSector == 7, "Down-Right displacement must map to Sector 7");

    // 7. Physical stick bypass
    input.stickBypass = true;
    out = kharvox::updateMotionWeaponWheel(state, input);
    check(out.stickBypassActive, "stick bypass flag must activate stickBypassActive output");
    check(!out.stickActive, "stick bypass must not output motion stick values");
    check(!out.triggerHapticPulse, "stick bypass must suppress haptics");
    input.stickBypass = false;

    // 8. HMD Yaw rotation invariance
    // Reset wheel with HMD rotated 90 degrees to the right (Yaw = -90 deg around +Y)
    input.wheelActive = false;
    kharvox::updateMotionWeaponWheel(state, input);

    input.wheelActive = true;
    // Quaternion for -90 degrees around Y: y = -sin(45 deg) = -0.7071f, w = cos(45 deg) = 0.7071f
    input.hmdOrientation = {0.0f, -0.70710678f, 0.0f, 0.70710678f};
    input.handPosition = {0.0f, 1.0f, 0.0f};
    // Frame 1: capture anchor
    out = kharvox::updateMotionWeaponWheel(state, input);

    // When facing -90 degrees (facing towards +X world), the user's "screen right" is +Z world
    // Moving hand +5cm along world +Z should be perceived as moving to screen right (Sector 0)
    input.handPosition = {0.0f, 1.0f, 0.05f};
    out = kharvox::updateMotionWeaponWheel(state, input);
    check(out.selectedSector == 0, "rotated HMD view must project screen right to Sector 0");
    check(out.stickX > 0.0f, "screen right displacement must result in positive stickX");

    // 9. Wheel close and state cleanup
    input.wheelActive = false;
    out = kharvox::updateMotionWeaponWheel(state, input);
    check(!out.stickActive, "closing wheel must clear output");
    check(!state.wasActive, "closing wheel must reset state.wasActive");
    check(state.lastSelectedSector == -1, "closing wheel must reset lastSelectedSector");

    std::cout << "All MotionWeaponWheelPolicy tests passed successfully!" << std::endl;
    return 0;
}

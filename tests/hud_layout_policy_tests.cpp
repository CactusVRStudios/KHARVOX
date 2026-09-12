#include "../src/hud/HudLayoutPolicy.h"

#include <cmath>

namespace {

bool near(float left, float right) {
    return std::fabs(left - right) < 0.0001f;
}

}

int main() {
    using namespace kharvox;

    // The physically reviewed Meta/VDXR projection remains the reference.
    if (!near(selectHudLayoutFit(0.83909965f, 0.966f), calibratedHudLayoutFit)) return 1;

    // SteamXR's wider projection must no longer enlarge or spread the HUD.
    if (!near(selectHudLayoutFit(0.947f, 1.329f), calibratedHudLayoutFit)) return 2;

    // Pixel resolution is deliberately absent from the policy: only angular
    // visibility matters, and a wider surface retains the same calibration.
    if (!near(selectHudLayoutFit(1.20f, 1.20f), calibratedHudLayoutFit)) return 3;

    // A narrower common eye area may shrink the layout to prevent clipping.
    if (!near(selectHudLayoutFit(0.70f, 0.90f), 0.70f)) return 4;
    if (!near(selectHudLayoutFit(0.10f, 0.10f), minimumHudLayoutFit)) return 5;

    // Invalid runtime geometry fails stable at the calibrated reference.
    if (!near(selectHudLayoutFit(NAN, 1.0f), calibratedHudLayoutFit)) return 6;

    return 0;
}

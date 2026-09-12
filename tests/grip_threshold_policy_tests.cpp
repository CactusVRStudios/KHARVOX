#include "../src/openxr/GripThresholdPolicy.h"

#include <cmath>
#include <cstring>

int main() {
    using namespace kharvox;

    if (std::strcmp(gripInputComponent(GripControllerProfile::Default),
            "squeeze/value") != 0) return 1;
    if (std::strcmp(gripInputComponent(GripControllerProfile::ValveIndex),
            "squeeze/force") != 0) return 2;

    // Existing controller profiles retain the previous > 0.55 behavior.
    if (updateGripPressed(0.55f, false, GripControllerProfile::Default)) return 3;
    if (!updateGripPressed(0.56f, false, GripControllerProfile::Default)) return 4;
    if (updateGripPressed(0.55f, true, GripControllerProfile::Default)) return 5;

    // Valve Index needs a deliberate squeeze to engage.
    if (updateGripPressed(0.70f, false, GripControllerProfile::ValveIndex)) return 6;
    if (!updateGripPressed(0.71f, false, GripControllerProfile::ValveIndex)) return 7;

    // Once engaged, hysteresis prevents chatter while the grip is held.
    if (!updateGripPressed(0.60f, true, GripControllerProfile::ValveIndex)) return 8;
    if (updateGripPressed(0.55f, true, GripControllerProfile::ValveIndex)) return 9;

    if (updateGripPressed(NAN, false, GripControllerProfile::ValveIndex)) return 10;
    if (updateGripPressed(NAN, true, GripControllerProfile::ValveIndex)) return 11;

    return 0;
}

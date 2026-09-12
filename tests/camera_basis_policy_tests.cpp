#include "../src/camera/CameraBasisPolicy.h"
#include "../src/camera/PlayerControlPolicy.h"

#include <cmath>

namespace {

bool near(float actual, float expected, float tolerance = 0.0001f) {
    return std::fabs(actual - expected) <= tolerance;
}

bool matches(const float actual[9], const float expected[9]) {
    for (int index = 0; index < 9; ++index) {
        if (!near(actual[index], expected[index])) return false;
    }
    return true;
}

} // namespace

int main() {
    const float identity[9]{1,0,0, 0,1,0, 0,0,1};
    float output[9]{};
    if (!kharvox::makeGravityLevelBodyBasis(identity, nullptr, output)
        || !matches(output, identity)) return 1;

    constexpr float sine = 0.17364818f;
    constexpr float cosine = 0.98480775f;
    const float rolled[9]{1,0,0, 0,cosine,sine, 0,-sine,cosine};
    if (!kharvox::makeGravityLevelBodyBasis(rolled, nullptr, output)
        || !matches(output, identity)) return 2;

    const float pitchedYawRight[9]{0,cosine,sine, -1,0,0, 0,-sine,cosine};
    const float yawRight[9]{0,1,0, -1,0,0, 0,0,1};
    if (!kharvox::makeGravityLevelBodyBasis(pitchedYawRight, nullptr, output)
        || !matches(output, yawRight)) return 3;

    const float vertical[9]{0,0,1, 1,0,0, 0,1,0};
    if (!kharvox::makeGravityLevelBodyBasis(vertical, yawRight, output)
        || !matches(output, yawRight)) return 4;

    const float degenerate[9]{};
    if (kharvox::makeGravityLevelBodyBasis(degenerate, nullptr, output)) return 5;

    // Ordinary gameplay leveling is not conditional on a preceding cinematic:
    // melee/recoil roll must be discarded on every gameplay frame.
    if (!kharvox::makeStableGameplayBodyBasis(
            true, false, rolled, identity, output)
        || !matches(output, identity)) return 6;
    // Authored cinematic motion and non-gameplay camera callers remain native.
    if (kharvox::makeStableGameplayBodyBasis(
            true, true, rolled, identity, output)) return 7;
    if (kharvox::makeStableGameplayBodyBasis(
            false, false, rolled, identity, output)) return 8;

    // Movement-only holds can still be playable weapon sequences. View,
    // buttons and profile-driven view locks identify passive authored scenes.
    if (!kharvox::playerWeaponControlAvailable(0)) return 9;
    if (!kharvox::playerWeaponControlAvailable(0x07)) return 10;
    if (kharvox::playerWeaponControlAvailable(0x08)) return 11;
    if (kharvox::playerWeaponControlAvailable(0x10)) return 12;
    if (kharvox::playerWeaponControlAvailable(0x100)) return 13;

    return 0;
}

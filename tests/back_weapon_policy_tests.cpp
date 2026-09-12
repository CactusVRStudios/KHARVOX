#include "../src/weapon/BackWeaponPolicy.h"

#include <cassert>

namespace {
constexpr std::uint64_t second = 1000000000ULL;

kharvox::BackWeaponInput baseInput() {
    kharvox::BackWeaponInput input{};
    input.shoulderGestureEnabled = true;
    input.gameplayActive = true;
    input.trackingValid = true;
    input.gripRelativeToHeadMeters = {0.28f, -0.28f, 0.28f};
    input.favorite = kharvox::BackWeaponKind::PlasmaRifle;
    input.activeWeapon = kharvox::BackWeaponKind::Pistol;
    return input;
}

void setAmmoKnown(kharvox::BackWeaponInput& input,
                  kharvox::BackWeaponKind kind, bool usable) {
    const auto mask = kharvox::backWeaponKindMask(kind);
    input.ammoKnownMask |= mask;
    if (usable) input.ammoUsableMask |= mask;
    else input.ammoUsableMask &= static_cast<std::uint16_t>(~mask);
}
}

int main() {
    using namespace kharvox;

    assert(backWeaponKeyboardVirtualKey(BackWeaponKind::Pistol) == '1');
    assert(backWeaponKeyboardVirtualKey(BackWeaponKind::CombatShotgun) == '2');
    assert(backWeaponKeyboardVirtualKey(BackWeaponKind::PlasmaRifle) == '3');
    assert(backWeaponKeyboardVirtualKey(BackWeaponKind::HeavyAssaultRifle) == '4');
    assert(backWeaponKeyboardVirtualKey(BackWeaponKind::RocketLauncher) == '5');
    assert(backWeaponKeyboardVirtualKey(BackWeaponKind::SuperShotgun) == '6');
    assert(backWeaponKeyboardVirtualKey(BackWeaponKind::GaussCannon) == '7');
    assert(backWeaponKeyboardVirtualKey(BackWeaponKind::Chaingun) == '8');
    assert(backWeaponKeyboardVirtualKey(BackWeaponKind::Bfg) == 0);
    assert(backWeaponKeyboardVirtualKey(BackWeaponKind::Chainsaw) == 0);
    assert(backWeaponNormalizeShoulderSide(0.28f, false) == 0.28f);
    assert(backWeaponNormalizeShoulderSide(-0.28f, true) == 0.28f);

    // Outside the zone, right Grip remains ordinary secondary fire.
    BackWeaponState outside{};
    auto input = baseInput();
    input.gripRelativeToHeadMeters = {0.0f, 0.0f, -0.2f};
    input.gripDown = true;
    auto output = updateBackWeapon(outside, input);
    assert(!output.zoneActive && !output.gripConsumed);
    assert(output.pulse == BackWeaponPulse::None);
    assert(!output.triggerDown);

    // Entry/exit uses hysteresis: this point is outside entry but inside exit.
    BackWeaponState hysteresis{};
    input = baseInput();
    updateBackWeapon(hysteresis, input);
    assert(hysteresis.zoneActive);
    input.gripRelativeToHeadMeters = {0.02f, -0.28f, 0.08f};
    output = updateBackWeapon(hysteresis, input);
    assert(output.zoneActive);
    input.gripRelativeToHeadMeters = {-0.03f, -0.28f, 0.08f};
    output = updateBackWeapon(hysteresis, input);
    assert(!output.zoneActive);

    // Different arm lengths remain usable below the shoulder, while a Grip
    // in front of the body can never enter the activation zone.
    BackWeaponState lowerShoulder{};
    input = baseInput();
    input.gripRelativeToHeadMeters = {0.18f, -0.70f, 0.18f};
    output = updateBackWeapon(lowerShoulder, input);
    assert(output.zoneActive);
    BackWeaponState inFront{};
    input.gripRelativeToHeadMeters = {0.18f, -0.30f, -0.02f};
    output = updateBackWeapon(inFront, input);
    assert(!output.zoneActive);

    // Fresh press inside requests once and consumes Grip through release.
    BackWeaponState fresh{};
    input = baseInput();
    updateBackWeapon(fresh, input);
    input.gripDown = true;
    input.triggerDown = true;
    output = updateBackWeapon(fresh, input);
    assert(output.targetRequested);
    assert(output.pulse == BackWeaponPulse::DirectWeapon);
    assert(output.gripConsumed);
    assert(output.triggerDown); // Trigger/fire is passed through untouched.
    input.nowNanoseconds += 10000000ULL;
    output = updateBackWeapon(fresh, input);
    assert(output.pulse == BackWeaponPulse::None);
    assert(output.gripConsumed && output.triggerDown);
    input.gripDown = false;
    output = updateBackWeapon(fresh, input);
    assert(!output.gripConsumed);

    // A Grip held before zone entry cannot activate it.
    BackWeaponState heldBeforeEntry{};
    input = baseInput();
    input.gripRelativeToHeadMeters = {0.0f, 0.0f, -0.2f};
    input.gripDown = true;
    updateBackWeapon(heldBeforeEntry, input);
    input.gripRelativeToHeadMeters = {0.28f, -0.28f, 0.28f};
    output = updateBackWeapon(heldBeforeEntry, input);
    assert(output.zoneActive && !output.targetRequested && !output.gripConsumed);

    // A controller already held on the first observed frame is not a fresh
    // edge; one complete release is required before the gesture is armed.
    BackWeaponState initiallyHeld{};
    input = baseInput();
    input.gripDown = true;
    output = updateBackWeapon(initiallyHeld, input);
    assert(!output.targetRequested && !output.gripConsumed);

    // Tracking and all non-gameplay contexts fail closed.
    for (int context = 0; context < 6; ++context) {
        BackWeaponState blocked{};
        input = baseInput();
        input.gripDown = true;
        if (context == 0) input.trackingValid = false;
        if (context == 1) input.gameplayActive = false; // pause/fullscreen menu
        if (context == 2) input.gameplayActive = false; // cutscene
        if (context == 3) input.gameplayActive = false; // death screen
        if (context == 4) input.weaponWheelActive = true;
        if (context == 5) input.calibrationMode = true;
        output = updateBackWeapon(blocked, input);
        assert(!output.targetRequested && !output.gripConsumed);
    }

    // Already active succeeds without any native pulse.
    BackWeaponState alreadyActive{};
    input = baseInput();
    input.activeWeapon = input.favorite;
    updateBackWeapon(alreadyActive, input);
    input.gripDown = true;
    output = updateBackWeapon(alreadyActive, input);
    assert(output.targetAlreadyActive && output.gripConsumed);
    assert(output.pulse == BackWeaponPulse::None);

    // Observing the requested target completes without fallback.
    BackWeaponState success{};
    input = baseInput();
    updateBackWeapon(success, input);
    input.gripDown = true;
    input.nowNanoseconds = second;
    updateBackWeapon(success, input);
    input.activeWeapon = input.favorite;
    input.nowNanoseconds += second;
    output = updateBackWeapon(success, input);
    assert(output.targetBecameActive && !output.fallbackRequested);
    assert(output.activatedWeapon == input.favorite);

    // A missing Shoulder Weapon gets a delayed Combat Shotgun fallback. The
    // selection remains pending so an unavailable/empty Shotgun can fall back
    // once more to the Pistol.
    BackWeaponState fallback{};
    input = baseInput();
    updateBackWeapon(fallback, input);
    input.gripDown = true;
    input.nowNanoseconds = second;
    output = updateBackWeapon(fallback, input);
    assert(output.targetRequested);
    input.nowNanoseconds += 2 * second;
    output = updateBackWeapon(fallback, input);
    assert(output.fallbackRequested);
    assert(output.shotgunFallbackRequested);
    assert(output.pulse == BackWeaponPulse::CombatShotgunFallback);
    input.nowNanoseconds += second;
    output = updateBackWeapon(fallback, input);
    assert(!output.fallbackRequested && output.pulse == BackWeaponPulse::None);
    input.activeWeapon = BackWeaponKind::CombatShotgun;
    output = updateBackWeapon(fallback, input);
    assert(output.targetBecameActive);
    assert(output.activatedWeapon == BackWeaponKind::CombatShotgun);

    // If both Shoulder Weapon and Shotgun fail, Pistol is selected exactly
    // once, then the state machine stops rather than cycling forever.
    BackWeaponState fullFallback{};
    input = baseInput();
    input.activeWeapon = BackWeaponKind::Unknown;
    updateBackWeapon(fullFallback, input);
    input.gripDown = true;
    input.nowNanoseconds = second;
    updateBackWeapon(fullFallback, input);
    input.nowNanoseconds += 2 * second;
    output = updateBackWeapon(fullFallback, input);
    assert(output.pulse == BackWeaponPulse::CombatShotgunFallback);
    input.nowNanoseconds += 2 * second;
    output = updateBackWeapon(fullFallback, input);
    assert(output.fallbackRequested && output.pistolFallbackRequested);
    assert(output.pulse == BackWeaponPulse::PistolFallback);
    input.nowNanoseconds += 2 * second;
    output = updateBackWeapon(fullFallback, input);
    assert(output.selectionExhausted && output.pulse == BackWeaponPulse::None);
    input.nowNanoseconds += 2 * second;
    output = updateBackWeapon(fullFallback, input);
    assert(!output.fallbackRequested && !output.selectionExhausted);

    // Shotgun as the selected Shoulder Weapon falls directly to Pistol when
    // DOOM cannot activate it (unavailable or empty).
    BackWeaponState shotgun{};
    input = baseInput();
    input.favorite = BackWeaponKind::CombatShotgun;
    updateBackWeapon(shotgun, input);
    input.gripDown = true;
    input.nowNanoseconds = second;
    updateBackWeapon(shotgun, input);
    input.nowNanoseconds += 2 * second;
    output = updateBackWeapon(shotgun, input);
    assert(output.fallbackRequested && output.pistolFallbackRequested);
    assert(!output.shotgunFallbackRequested);
    assert(output.targetAlreadyActive);
    assert(output.activatedWeapon == BackWeaponKind::Pistol);
    assert(output.pulse == BackWeaponPulse::None);

    // A known-empty Shoulder Weapon is never put into the player's hands.
    // The usable Shotgun fallback is requested on the same update.
    BackWeaponState emptyFavorite{};
    input = baseInput();
    setAmmoKnown(input, input.favorite, false);
    setAmmoKnown(input, BackWeaponKind::CombatShotgun, true);
    updateBackWeapon(emptyFavorite, input);
    input.gripDown = true;
    input.nowNanoseconds = second;
    output = updateBackWeapon(emptyFavorite, input);
    assert(output.favoriteSkippedUnusable && output.shotgunFallbackRequested);
    assert(!output.targetRequested && output.pulse == BackWeaponPulse::CombatShotgunFallback);
    assert(emptyFavorite.pendingTarget == BackWeaponKind::CombatShotgun);

    // If both the favorite and Shotgun are known empty/unavailable, selection
    // skips both immediately and requests the infinite-ammo Pistol once.
    BackWeaponState bothEmpty{};
    input = baseInput();
    input.activeWeapon = BackWeaponKind::Unknown;
    setAmmoKnown(input, input.favorite, false);
    setAmmoKnown(input, BackWeaponKind::CombatShotgun, false);
    updateBackWeapon(bothEmpty, input);
    input.gripDown = true;
    input.nowNanoseconds = second;
    output = updateBackWeapon(bothEmpty, input);
    assert(output.favoriteSkippedUnusable && output.shotgunSkippedUnusable);
    assert(output.pistolFallbackRequested && !output.shotgunFallbackRequested);
    assert(output.pulse == BackWeaponPulse::PistolFallback);

    // An empty favorite is not accepted merely because it happens to be the
    // currently equipped weapon.
    BackWeaponState activeButEmpty{};
    input = baseInput();
    input.activeWeapon = input.favorite;
    setAmmoKnown(input, input.favorite, false);
    setAmmoKnown(input, BackWeaponKind::CombatShotgun, true);
    updateBackWeapon(activeButEmpty, input);
    input.gripDown = true;
    output = updateBackWeapon(activeButEmpty, input);
    assert(!output.targetAlreadyActive && output.favoriteSkippedUnusable);
    assert(output.pulse == BackWeaponPulse::CombatShotgunFallback);

    // A snapshot that becomes known while a request is pending advances the
    // fallback immediately rather than waiting 1750 ms or accepting emptiness.
    BackWeaponState lateSnapshot{};
    input = baseInput();
    updateBackWeapon(lateSnapshot, input);
    input.gripDown = true;
    input.nowNanoseconds = second;
    output = updateBackWeapon(lateSnapshot, input);
    assert(output.targetRequested && output.pulse == BackWeaponPulse::DirectWeapon);
    setAmmoKnown(input, input.favorite, false);
    setAmmoKnown(input, BackWeaponKind::CombatShotgun, true);
    input.nowNanoseconds += second / 10;
    output = updateBackWeapon(lateSnapshot, input);
    assert(output.favoriteSkippedUnusable && !output.targetBecameActive);
    assert(output.pulse == BackWeaponPulse::CombatShotgunFallback);

    // A configured Shotgun known to be empty goes straight to Pistol without
    // briefly selecting the empty Shotgun.
    BackWeaponState knownEmptyShotgun{};
    input = baseInput();
    input.favorite = BackWeaponKind::CombatShotgun;
    input.activeWeapon = BackWeaponKind::Unknown;
    setAmmoKnown(input, BackWeaponKind::CombatShotgun, false);
    updateBackWeapon(knownEmptyShotgun, input);
    input.gripDown = true;
    output = updateBackWeapon(knownEmptyShotgun, input);
    assert(output.favoriteSkippedUnusable && output.shotgunSkippedUnusable);
    assert(output.pulse == BackWeaponPulse::PistolFallback);

    // Leaving gameplay aborts a live attempt and prevents a later fallback.
    BackWeaponState aborted{};
    input = baseInput();
    updateBackWeapon(aborted, input);
    input.gripDown = true;
    input.nowNanoseconds = second;
    updateBackWeapon(aborted, input);
    input.gameplayActive = false;
    input.nowNanoseconds += second;
    output = updateBackWeapon(aborted, input);
    assert(output.selectionAborted);
    input.nowNanoseconds += 2 * second;
    output = updateBackWeapon(aborted, input);
    assert(!output.fallbackRequested);

    // The side-neutral policy accepts the mirrored left-shoulder input. Only
    // an explicitly disabled gesture fails closed.
    BackWeaponState leftHanded{};
    input = baseInput();
    input.gripRelativeToHeadMeters.x = backWeaponNormalizeShoulderSide(-0.28f, true);
    updateBackWeapon(leftHanded, input);
    input.gripDown = true;
    input.triggerDown = true;
    output = updateBackWeapon(leftHanded, input);
    assert(output.gripConsumed && output.pulse == BackWeaponPulse::DirectWeapon);
    assert(output.triggerDown);

    BackWeaponState disabled{};
    input = baseInput();
    input.shoulderGestureEnabled = false;
    input.gripDown = true;
    output = updateBackWeapon(disabled, input);
    assert(!output.gripConsumed && output.pulse == BackWeaponPulse::None);

    return 0;
}

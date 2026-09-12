#pragma once

#include <cstdint>

namespace kharvox {

enum class BackWeaponKind {
    Unknown = -1,
    Pistol,
    CombatShotgun,
    PlasmaRifle,
    HeavyAssaultRifle,
    RocketLauncher,
    SuperShotgun,
    GaussCannon,
    Chaingun,
    Bfg,
    Chainsaw
};

enum class BackWeaponPulse {
    None,
    DirectWeapon,
    Bfg,
    Chainsaw,
    CombatShotgunFallback,
    PistolFallback
};

enum class BackWeaponSelectionPhase {
    Idle,
    WaitingForFavorite,
    WaitingForShotgun,
    WaitingForPistol
};

struct BackWeaponVec3 {
    // OpenXR body-relative metres after handedness normalization: +x points
    // outward toward the weapon shoulder (right in right-handed mode, left in
    // left-handed mode), +y up, and +z behind.
    float x{};
    float y{};
    float z{};
};

struct BackWeaponZoneBounds {
    float minimumOutward{};
    float maximumOutward{};
    float minimumVertical{};
    float maximumVertical{};
    float minimumBehind{};
    float maximumBehind{};
};

struct BackWeaponZonePolicy {
    // The entry box tolerates different shoulder widths, arm lengths and a
    // controller held somewhat below the shoulder, but still requires the
    // physical Grip pose to be behind the HMD/body plane. Exit is wider on
    // every side so minor tracking noise cannot chatter the zone.
    BackWeaponZoneBounds enter{0.06f, 0.68f, -0.78f, 0.12f, 0.06f, 0.72f};
    BackWeaponZoneBounds exit{-0.02f, 0.82f, -0.92f, 0.26f, -0.01f, 0.86f};
    std::uint64_t selectionTimeoutNanoseconds{1750000000ULL};
};

struct BackWeaponState {
    bool zoneActive{};
    bool previousGripDown{};
    bool gripArmed{};
    bool gripConsumedUntilRelease{};
    BackWeaponSelectionPhase selectionPhase{BackWeaponSelectionPhase::Idle};
    BackWeaponKind pendingTarget{BackWeaponKind::Unknown};
    std::uint64_t selectionDeadlineNanoseconds{};
};

struct BackWeaponInput {
    bool shoulderGestureEnabled{true};
    bool gameplayActive{};
    bool weaponWheelActive{};
    bool calibrationMode{};
    bool trackingValid{};
    BackWeaponVec3 gripRelativeToHeadMeters{};
    bool gripDown{};
    bool triggerDown{};
    BackWeaponKind favorite{BackWeaponKind::CombatShotgun};
    BackWeaponKind activeWeapon{BackWeaponKind::Unknown};
    std::uint64_t nowNanoseconds{};
    // A zero known-mask means no trustworthy live snapshot and preserves the
    // original activation-timeout behavior. A known bit without a usable bit
    // means the weapon is unavailable or has no loaded/reserve ammunition.
    std::uint16_t ammoKnownMask{};
    std::uint16_t ammoUsableMask{};
};

struct BackWeaponOutput {
    BackWeaponPulse pulse{BackWeaponPulse::None};
    bool zoneActive{};
    bool gripConsumed{};
    // Back-weapon logic is intentionally incapable of changing fire input.
    bool triggerDown{};
    bool targetRequested{};
    bool targetAlreadyActive{};
    bool targetBecameActive{};
    bool fallbackRequested{};
    bool shotgunFallbackRequested{};
    bool pistolFallbackRequested{};
    bool favoriteSkippedUnusable{};
    bool shotgunSkippedUnusable{};
    bool selectionExhausted{};
    bool selectionAborted{};
    BackWeaponKind activatedWeapon{BackWeaponKind::Unknown};
};

inline bool backWeaponPointInside(const BackWeaponVec3& point,
                                  const BackWeaponZoneBounds& bounds) {
    return point.x >= bounds.minimumOutward && point.x <= bounds.maximumOutward
        && point.y >= bounds.minimumVertical && point.y <= bounds.maximumVertical
        && point.z >= bounds.minimumBehind && point.z <= bounds.maximumBehind;
}

inline float backWeaponNormalizeShoulderSide(float bodyRight, bool leftHanded) {
    return leftHanded ? -bodyRight : bodyRight;
}

inline BackWeaponPulse backWeaponTargetPulse(BackWeaponKind kind) {
    switch (kind) {
    case BackWeaponKind::Bfg: return BackWeaponPulse::Bfg;
    case BackWeaponKind::Chainsaw: return BackWeaponPulse::Chainsaw;
    case BackWeaponKind::Pistol:
    case BackWeaponKind::CombatShotgun:
    case BackWeaponKind::PlasmaRifle:
    case BackWeaponKind::HeavyAssaultRifle:
    case BackWeaponKind::RocketLauncher:
    case BackWeaponKind::SuperShotgun:
    case BackWeaponKind::GaussCannon:
    case BackWeaponKind::Chaingun:
        return BackWeaponPulse::DirectWeapon;
    default:
        return BackWeaponPulse::None;
    }
}

inline std::uint16_t backWeaponKindMask(BackWeaponKind kind) {
    const int index = static_cast<int>(kind);
    return index >= 0 && index < 16
        ? static_cast<std::uint16_t>(1u << index) : 0;
}

inline bool backWeaponAmmoKnown(const BackWeaponInput& input, BackWeaponKind kind) {
    const auto mask = backWeaponKindMask(kind);
    return mask && (input.ammoKnownMask & mask) != 0;
}

inline bool backWeaponAmmoUsable(const BackWeaponInput& input, BackWeaponKind kind) {
    const auto mask = backWeaponKindMask(kind);
    return mask && (input.ammoUsableMask & mask) != 0;
}

inline bool backWeaponKnownUnusable(const BackWeaponInput& input, BackWeaponKind kind) {
    return backWeaponAmmoKnown(input, kind) && !backWeaponAmmoUsable(input, kind);
}

// DOOM's campaign profile has stable native number-row weapon bindings:
// 1..8 select _weap0.._weap7. Playtests showed that the same commands are
// ignored when synthesized through JOY7 or D-pad XInput channels, so normal
// shoulder favorites use these proven keyboard bindings instead.
inline unsigned short backWeaponKeyboardVirtualKey(BackWeaponKind kind) {
    switch (kind) {
    case BackWeaponKind::Pistol: return 0x31;             // '1'
    case BackWeaponKind::CombatShotgun: return 0x32;     // '2'
    case BackWeaponKind::PlasmaRifle: return 0x33;       // '3'
    case BackWeaponKind::HeavyAssaultRifle: return 0x34; // '4'
    case BackWeaponKind::RocketLauncher: return 0x35;    // '5'
    case BackWeaponKind::SuperShotgun: return 0x36;      // '6'
    case BackWeaponKind::GaussCannon: return 0x37;       // '7'
    case BackWeaponKind::Chaingun: return 0x38;          // '8'
    default: return 0;
    }
}

inline BackWeaponOutput updateBackWeapon(
    BackWeaponState& state, const BackWeaponInput& input,
    const BackWeaponZonePolicy& policy = {}) {
    BackWeaponOutput output{};
    output.triggerDown = input.triggerDown;

    if (!input.gripDown) {
        state.gripConsumedUntilRelease = false;
        state.gripArmed = true;
    }
    const bool contextActive = input.shoulderGestureEnabled && input.gameplayActive
        && !input.weaponWheelActive && !input.calibrationMode
        && input.trackingValid;

    if (!contextActive) {
        state.zoneActive = false;
        if (state.selectionPhase != BackWeaponSelectionPhase::Idle) {
            state.selectionPhase = BackWeaponSelectionPhase::Idle;
            state.pendingTarget = BackWeaponKind::Unknown;
            state.selectionDeadlineNanoseconds = 0;
            output.selectionAborted = true;
        }
    } else {
        state.zoneActive = backWeaponPointInside(
            input.gripRelativeToHeadMeters,
            state.zoneActive ? policy.exit : policy.enter);
    }

    const auto clearSelection = [&]() {
        state.selectionPhase = BackWeaponSelectionPhase::Idle;
        state.pendingTarget = BackWeaponKind::Unknown;
        state.selectionDeadlineNanoseconds = 0;
    };
    const auto beginSelection = [&](BackWeaponKind target,
                                    BackWeaponSelectionPhase phase,
                                    BackWeaponPulse pulse) {
        output.pulse = pulse;
        state.selectionPhase = phase;
        state.pendingTarget = target;
        state.selectionDeadlineNanoseconds = input.nowNanoseconds
            + policy.selectionTimeoutNanoseconds;
    };
    const auto finishAlreadyActive = [&](BackWeaponKind target) {
        output.targetAlreadyActive = true;
        output.activatedWeapon = target;
        clearSelection();
    };
    const auto requestPistol = [&]() {
        output.fallbackRequested = true;
        output.pistolFallbackRequested = true;
        if (input.activeWeapon == BackWeaponKind::Pistol)
            finishAlreadyActive(BackWeaponKind::Pistol);
        else
            beginSelection(BackWeaponKind::Pistol,
                BackWeaponSelectionPhase::WaitingForPistol,
                BackWeaponPulse::PistolFallback);
    };
    const auto requestShotgunOrPistol = [&]() {
        output.fallbackRequested = true;
        if (backWeaponKnownUnusable(input, BackWeaponKind::CombatShotgun)) {
            output.shotgunSkippedUnusable = true;
            requestPistol();
        } else {
            output.shotgunFallbackRequested = true;
            if (input.activeWeapon == BackWeaponKind::CombatShotgun)
                finishAlreadyActive(BackWeaponKind::CombatShotgun);
            else
                beginSelection(BackWeaponKind::CombatShotgun,
                    BackWeaponSelectionPhase::WaitingForShotgun,
                    BackWeaponPulse::CombatShotgunFallback);
        }
    };

    if (state.selectionPhase != BackWeaponSelectionPhase::Idle) {
        const auto phase = state.selectionPhase;
        const auto pending = state.pendingTarget;
        if (phase != BackWeaponSelectionPhase::WaitingForPistol
            && backWeaponKnownUnusable(input, pending)) {
            if (phase == BackWeaponSelectionPhase::WaitingForFavorite)
                output.favoriteSkippedUnusable = true;
            if (phase == BackWeaponSelectionPhase::WaitingForShotgun
                || pending == BackWeaponKind::CombatShotgun) {
                output.shotgunSkippedUnusable = true;
                requestPistol();
            } else {
                requestShotgunOrPistol();
            }
        } else if (input.activeWeapon == pending) {
            output.activatedWeapon = pending;
            clearSelection();
            output.targetBecameActive = true;
        } else if (input.nowNanoseconds >= state.selectionDeadlineNanoseconds) {
            if (phase == BackWeaponSelectionPhase::WaitingForFavorite
                && pending != BackWeaponKind::CombatShotgun) {
                requestShotgunOrPistol();
            } else if (phase == BackWeaponSelectionPhase::WaitingForFavorite
                       || phase == BackWeaponSelectionPhase::WaitingForShotgun) {
                requestPistol();
            } else {
                output.selectionExhausted = true;
                clearSelection();
            }
        }
    }

    const bool freshGripPress = state.gripArmed
        && input.gripDown && !state.previousGripDown;
    if (contextActive && state.zoneActive && freshGripPress) {
        state.gripArmed = false;
        state.gripConsumedUntilRelease = true;
        if (state.selectionPhase == BackWeaponSelectionPhase::Idle) {
            if (backWeaponKnownUnusable(input, input.favorite)) {
                output.favoriteSkippedUnusable = true;
                if (input.favorite == BackWeaponKind::CombatShotgun)
                    output.shotgunSkippedUnusable = true;
                requestShotgunOrPistol();
            } else if (input.activeWeapon == input.favorite) {
                finishAlreadyActive(input.favorite);
            } else {
                output.pulse = backWeaponTargetPulse(input.favorite);
                output.targetRequested = output.pulse != BackWeaponPulse::None;
                if (output.targetRequested) {
                    state.selectionPhase = BackWeaponSelectionPhase::WaitingForFavorite;
                    state.pendingTarget = input.favorite;
                    state.selectionDeadlineNanoseconds = input.nowNanoseconds
                        + policy.selectionTimeoutNanoseconds;
                }
            }
        }
    }

    state.previousGripDown = input.gripDown;
    output.zoneActive = state.zoneActive;
    output.gripConsumed = state.gripConsumedUntilRelease && input.gripDown;
    return output;
}

} // namespace kharvox

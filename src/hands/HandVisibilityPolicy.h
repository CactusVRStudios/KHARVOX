#pragma once

namespace kharvox::hands {

enum class HandModelKind { None, Fist, GunHolding };

struct HandAssetAvailability {
    bool leftFist{};
    bool rightFist{};
    bool leftGun{};
    bool rightGun{};
};

struct HandVisibilityInput {
    bool leftHanded{};
    bool twoHandGrab{};
    bool berserk{};
    HandAssetAvailability assets{};
    bool showHands{};
    // False for every authored camera sequence, including Glory Kills, ledge
    // transitions and ordinary cinematics. Presentation context has priority
    // over Berserk and all handedness rules.
    bool presentationAllowsHands{true};
};

struct HandVisibilityOutput {
    HandModelKind left{HandModelKind::None};
    HandModelKind right{HandModelKind::None};
    // True means KHARVOX deliberately supplies no replacement for the support
    // hand and must leave DOOM's existing native representation untouched.
    bool preserveNativeOffHand{};
};

inline HandVisibilityOutput selectHandVisibility(
    const HandVisibilityInput& input) {
    HandVisibilityOutput output{};
    if (!input.showHands || !input.presentationAllowsHands)
        return output;
    if (input.berserk) {
        output.left = input.assets.leftFist
            ? HandModelKind::Fist : HandModelKind::None;
        output.right = input.assets.rightFist
            ? HandModelKind::Fist : HandModelKind::None;
        return output;
    }

    const bool leftIsWeaponHand = input.leftHanded;
    // Both physical hands use their existing tracked Grip transform. The
    // support grip selects a closed gun pose only after acquisition; releasing
    // it restores the free-hand fist without changing the weapon hand.
    const auto desiredLeft = leftIsWeaponHand
        ? HandModelKind::GunHolding
        : (input.twoHandGrab ? HandModelKind::GunHolding : HandModelKind::Fist);
    const auto desiredRight = !leftIsWeaponHand
        ? HandModelKind::GunHolding
        : (input.twoHandGrab ? HandModelKind::GunHolding : HandModelKind::Fist);
    const auto available = [&](bool left, HandModelKind kind) {
        if (kind == HandModelKind::Fist)
            return left ? input.assets.leftFist : input.assets.rightFist;
        if (kind == HandModelKind::GunHolding)
            return left ? input.assets.leftGun : input.assets.rightGun;
        return kind == HandModelKind::None;
    };
    if (available(true, desiredLeft)) output.left = desiredLeft;
    if (available(false, desiredRight)) output.right = desiredRight;

    const bool offHandIsRight = input.leftHanded;
    const auto offHandKind = offHandIsRight ? desiredRight : desiredLeft;
    if (!available(!offHandIsRight, offHandKind))
        output.preserveNativeOffHand = true;
    return output;
}

} // namespace kharvox::hands

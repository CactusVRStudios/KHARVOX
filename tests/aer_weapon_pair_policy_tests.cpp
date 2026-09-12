#include <iostream>

#include "../src/weapon/AerWeaponPairPolicy.h"

namespace {
int failures{};
void require(bool condition, const char* name) {
    if (!condition) {
        std::cerr << "FAILED: " << name << '\n';
        ++failures;
    }
}
}

int main() {
    using namespace kharvox;
    auto packed = packAerWeaponPairState({7, -1, false});
    packed = nextAerWeaponPairState(packed, 0, true);
    auto state = unpackAerWeaponPairState(packed);
    require(state.enabled && state.eye == 0 && state.serial == 8,
        "enable starts fresh left/right pair");

    packed = nextAerWeaponPairState(packed, 1, true);
    state = unpackAerWeaponPairState(packed);
    require(state.eye == 1 && state.serial == 8,
        "right eye retains current pair serial");

    packed = nextAerWeaponPairState(packed, 0, true);
    state = unpackAerWeaponPairState(packed);
    require(state.eye == 0 && state.serial == 9,
        "next left eye starts new pair serial");

    const auto repeatedLeft = nextAerWeaponPairState(packed, 0, true);
    require(unpackAerWeaponPairState(repeatedLeft).serial == 9,
        "repeated same-eye render stays in pair");

    const auto invalidated = invalidateAerWeaponPairState(repeatedLeft);
    require(unpackAerWeaponPairState(invalidated).serial == 10,
        "weapon change invalidates pair data");

    const auto disabled = nextAerWeaponPairState(invalidated, 0, false);
    state = unpackAerWeaponPairState(disabled);
    require(!state.enabled && state.eye == -1 && state.serial == 11,
        "disable invalidates incomplete pair");

    const auto alignedA = aerWeaponPairHash(0x1000, 128);
    const auto alignedB = aerWeaponPairHash(0x1100, 128);
    const auto alignedC = aerWeaponPairHash(0x1200, 128);
    require(alignedA != alignedB || alignedB != alignedC,
        "aligned render pointers do not all collide");
    require(aerWeaponPairHash(0x1000, 0) == 0,
        "zero-capacity hash is defensive");

    return failures == 0 ? 0 : 1;
}

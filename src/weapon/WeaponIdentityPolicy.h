#pragma once
#include <cstdint>

namespace kharvox {
struct WeaponIdentity {
    uintptr_t owner{}, baseDecl{}, activeDecl{};
    int kind{};
    bool known{};
};
inline bool weaponIdentityNeedsReset(const WeaponIdentity& before, const WeaponIdentity& after) {
    if (before.owner != after.owner || before.baseDecl != after.baseDecl) return true;
    if (before.activeDecl == after.activeDecl) return false;
    // A fire-mode decl is not a new equipped weapon. Require the same live
    // owner, base weapon and recognized family before retaining queued poses.
    return !(after.owner && after.baseDecl && before.known && after.known
        && before.kind == after.kind);
}
}

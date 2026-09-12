#pragma once
#include <cstdint>

namespace kharvox {
// DOOM 6.66 idHandsCustomAnimSelect. The separate customWeaponAnimSelect
// starts at zero and is NOT a collectible discriminator.
inline bool collectibleAnimationPlaying(float customAnim, unsigned char flags) {
    return (flags & 0x80) != 0
        && (customAnim == 6.0f || customAnim == 7.0f || customAnim == 8.0f);
}

inline bool collectibleObservationCurrent(std::uint64_t seen, std::uint64_t now,
    std::uint64_t sourceLevel, std::uint64_t currentLevel) {
    // Expire only missing observations; a native false observation clears
    // immediately. Present age does not turn a long GPU frame into an exit.
    return seen != 0 && now >= seen && now - seen <= 8
        && sourceLevel == currentLevel;
}
}

#pragma once

#include <cstdint>

namespace kharvox {

// idUCmdTracker::inhibitFlags_t values reflected by DOOM 6.66. A scripted
// sequence is still genuinely first-person when the player can aim and use the
// weapon. Movement-only locks (for example a short authored positional hold)
// do not turn that interaction into a passive cinematic.
constexpr std::uint32_t playerViewInhibit = 0x08;
constexpr std::uint32_t playerButtonsInhibit = 0x10;
constexpr std::uint32_t playerProfileViewInhibit = 0x100;
constexpr std::uint32_t playerWeaponControlInhibitMask =
    playerViewInhibit | playerButtonsInhibit | playerProfileViewInhibit;

constexpr bool playerWeaponControlAvailable(std::uint32_t inhibitFlags) {
    return (inhibitFlags & playerWeaponControlInhibitMask) == 0;
}

} // namespace kharvox

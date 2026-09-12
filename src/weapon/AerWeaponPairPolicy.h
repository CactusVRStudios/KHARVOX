#pragma once

#include <cstddef>
#include <cstdint>

namespace kharvox {

struct AerWeaponPairState {
    std::uint64_t serial{1};
    int eye{-1};
    bool enabled{};
};

inline std::uint64_t packAerWeaponPairState(const AerWeaponPairState& state) {
    const auto eye = state.eye == 1 ? std::uint64_t{1} : std::uint64_t{0};
    return (state.serial << 2) | (eye << 1)
        | (state.enabled ? std::uint64_t{1} : std::uint64_t{0});
}

inline AerWeaponPairState unpackAerWeaponPairState(std::uint64_t packed) {
    const bool enabled = (packed & 1u) != 0;
    return {packed >> 2, enabled ? static_cast<int>((packed >> 1) & 1u) : -1,
        enabled};
}

inline std::uint64_t nextAerWeaponPairState(
    std::uint64_t packed, int renderEye, bool enabled) {
    const auto previous = unpackAerWeaponPairState(packed);
    const int eye = enabled && renderEye == 1 ? 1 : enabled ? 0 : -1;
    auto serial = previous.serial;
    // A coherent pair begins with the left eye. A new serial makes every
    // render-prop entry valid for exactly one left/right pair instead of the
    // entire AER session. Enabling/disabling also invalidates incomplete data.
    if (previous.enabled != enabled
        || (enabled && previous.enabled && eye == 0 && previous.eye == 1))
        ++serial;
    return packAerWeaponPairState({serial, eye, enabled});
}

inline std::uint64_t invalidateAerWeaponPairState(std::uint64_t packed) {
    auto state = unpackAerWeaponPairState(packed);
    ++state.serial;
    return packAerWeaponPairState(state);
}

inline std::size_t aerWeaponPairHash(
    std::uintptr_t entity, std::size_t capacity) {
    if (!capacity) return 0;
    // Render-object pointers are strongly aligned, so the old entity % N
    // replacement sent nearly every overflow to the same slot. Mix the useful
    // upper bits before selecting the first open-addressing slot.
    std::uint64_t value = static_cast<std::uint64_t>(entity) >> 4;
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return static_cast<std::size_t>(value % capacity);
}

} // namespace kharvox

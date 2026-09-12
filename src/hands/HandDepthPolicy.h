#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace kharvox::hands {

// Preserve the established guns-only CVar value. This CVar alone is NOT
// sufficient: native render entities cache their own depth scale at +0x14C.
inline float nativeWeaponDepthScale(bool showHands) {
    return showHands ? 1.0f : 0.0f;
}

// Own only overrides applied at a recognized native weapon update. Never
// dereference saved entity keys: restoration happens when that same entity is
// handed to us again by DOOM. A native rewrite/address reuse replaces the
// baseline; an inactive context must not overwrite a new native value.
template<std::size_t Capacity = 128>
class NativeWeaponDepthOverrides {
    struct Entry { std::uintptr_t entity{}; float original{}; };
    std::array<Entry, Capacity> entries_{};
public:
    bool update(std::uintptr_t entity, float current, bool active, float& desired) {
        desired = current;
        if (!entity || !std::isfinite(current) || current < 0.f || current > 1.f)
            return false;
        Entry* found{};
        Entry* free{};
        for (auto& entry : entries_) {
            if (entry.entity == entity) { found = &entry; break; }
            if (!entry.entity && !free) free = &entry;
        }
        if (!active) {
            if (!found) return false;
            if (current == 1.f) desired = found->original;
            *found = {};
            return desired != current;
        }
        if (current == 1.f) return false;
        if (!found) found = free;
        if (!found) return false; // bounded storage: do not make an unowned edit
        *found = {entity, current};
        desired = 1.f;
        return true;
    }
};

struct HandDepthRange {
    float nearMeters{0.02f};
    float farMeters{100.0f};
};

// DOOM's native scene projection uses a three-unit near clip plane. Hand
// geometry is expressed in OpenXR metres, so the shared-depth render path must
// use the same near plane after applying KHARVOX's configured world scale.
// A zero native far plane means infinity; a very distant finite plane gives
// the same stable Vulkan depth curve without producing infinities in the CPU
// matrix. The isolated OpenXR overlay deliberately retains its old range.
inline HandDepthRange doomSceneHandDepthRange(float worldUnitsPerMeter) {
    constexpr float nativeNearUnits = 3.0f;
    constexpr float effectivelyInfiniteFarUnits = 1000000.0f;
    const float scale = std::clamp(worldUnitsPerMeter, 10.0f, 200.0f);
    return {nativeNearUnits / scale, effectivelyInfiniteFarUnits / scale};
}

} // namespace kharvox::hands

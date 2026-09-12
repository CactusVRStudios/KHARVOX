#pragma once

#include <algorithm>
#include <cmath>

namespace kharvox {

// The reviewed HUD calibration was performed with a common inner-eye half-FOV
// of 40 degrees under Meta OpenXR and VDXR. Express that reference in tangent
// space so pixel resolution, supersampling, and runtime swapchain size cannot
// change the apparent HUD size or spacing.
inline constexpr float calibratedHudLayoutFit = 0.83909965f; // tan(40 degrees)
inline constexpr float minimumHudLayoutFit = 0.35f;
inline constexpr float nativeHudTanHalfHorizontal = 1.0f;
inline constexpr float nativeHudTanHalfVertical = 9.0f / 16.0f;

inline float selectHudLayoutFit(
    float safeTanHalfHorizontal, float safeTanHalfVertical) {
    if (!std::isfinite(safeTanHalfHorizontal)
        || !std::isfinite(safeTanHalfVertical)
        || safeTanHalfHorizontal <= 0.0f
        || safeTanHalfVertical <= 0.0f)
        return calibratedHudLayoutFit;

    const float availableFit = std::min(
        safeTanHalfHorizontal / nativeHudTanHalfHorizontal,
        safeTanHalfVertical / nativeHudTanHalfVertical);

    // A wider runtime FOV must not enlarge or spread the calibrated HUD. A
    // genuinely narrower common eye area may shrink it only as much as needed
    // to keep the authored 16:9 layout visible in both eyes.
    return std::clamp(
        std::min(availableFit, calibratedHudLayoutFit),
        minimumHudLayoutFit, calibratedHudLayoutFit);
}

} // namespace kharvox

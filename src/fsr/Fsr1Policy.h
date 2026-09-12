#pragma once

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <limits>

namespace kharvox {

// No application supersampling ceiling, including direct SteamVR.
inline float effectiveRenderScale(float selectedRenderScale, bool) {
    return std::isfinite(selectedRenderScale) ? std::max(selectedRenderScale, 0.5f)
                                             : selectedRenderScale;
}
inline float steamNativeResolutionScale(float, bool) { return 1.0f; }
inline float safeHighResolutionCarrierScale(float selectedRenderScale) {
    return effectiveRenderScale(selectedRenderScale, false);
}
inline float sourceCarrierScale(float selectedRenderScale, bool, bool) {
    return effectiveRenderScale(selectedRenderScale, false);
}
// Validate before conversion. A hardware/API limit is an error, never a
// silent reduction of the user's requested resolution. Output stays unchanged.
inline bool scaledRenderDimension(uint32_t base, double scale, uint32_t maximum,
                                  uint32_t& output) {
    const double rounded = std::round(double(base) * scale);
    if (!std::isfinite(scale) || scale <= 0 || !std::isfinite(rounded)
        || rounded < 1 || rounded > maximum) return false;
    output = static_cast<uint32_t>(rounded);
    return true;
}
inline bool requestFsr1Upscaling(bool launcherOption, bool aerSelected,
                                 float renderScale) {
    return launcherOption && aerSelected && renderScale >= 0.5f
        && renderScale < 0.999f;
}

inline bool useFsr1ForFrame(bool requested, bool alternatingStereo,
                            bool nativePackedStereo, bool quadMode, bool currentNativePair = false) {
    return requested && alternatingStereo && (!nativePackedStereo || currentNativePair) && !quadMode;
}

inline float openXrEyeTargetScale(float renderScale, bool fsr1Requested,
                                  bool directVirtualDesktop, bool nativeStereo = false) {
    if (fsr1Requested) return 1.0f;
    return effectiveRenderScale(renderScale, false);
}

inline std::uint32_t startupStablePresents(bool steamBackedRuntime,
                                           bool directVirtualDesktop,
                                           bool fsr1Requested) {
    // Meta's runtime-managed Vulkan path can expose a short chain of compatible
    // DOOM startup swapchains. Starting XR GPU work on the first member has
    // produced an intermittent VK_ERROR_DEVICE_LOST with full-size FSR targets.
    // Meta session creation is separately gated on three Presents from one
    // current swapchain. Steam-backed paths retain their
    // validated immediate submission behavior.
    // VDXR has now demonstrated the same failure even after queue-idle and an
    // empty recovery frame: copying from a replacement WSI image on its first
    // few Presents can still lose the Vulkan device. Require each VDXR
    // swapchain handle to survive three native Presents before any XR GPU
    // work. This short delay applies with and without FSR.
    if (directVirtualDesktop) return 3u;
    if (!steamBackedRuntime && fsr1Requested)
        return 2u;
    return 0u;
}

inline bool inheritStartupPresentProgress(bool directVirtualDesktop, bool directMeta = false) {
    // Meta and VDXR replacements must prove their own lifetime; progress from
    // a retired handle defeats startupStablePresents() and caused the r126
    // DEVICE_LOST sequence.
    return !directVirtualDesktop && !directMeta;
}

inline bool deferSharedDeviceSessionCreation(bool directVirtualDesktop,
                                               bool sessionExists,
                                               std::uint32_t completedPresents, bool directMeta = false) {
    // Meta and VDXR may share DOOM's Vulkan device with its compositor. Creating the
    // OpenXR eye swapchains while DOOM is still replacing its startup WSI
    // swapchains can lose that device before KHARVOX submits a single frame.
    // Count completed downstream Presents, not intercepted Present attempts.
    return (directVirtualDesktop || directMeta) && !sessionExists
        && completedPresents < startupStablePresents(false, true, false);
}

inline bool quiesceFsrStartupSwapchain(bool supportedRuntime,
                                       bool fsr1Requested,
                                       std::uint32_t nativePresents,
                                       std::uint64_t submittedLayerFrames) {
    // A presented WSI image can still be owned by the presentation engine when
    // DOOM replaces one of its short-lived startup swapchains.  This was first
    // observed on Meta, but the same WSI lifetime race can exist behind VDXR
    // and SteamVR. Restrict the expensive idle wait to that startup boundary.
    // The same ownership hazard remains when DOOM replaces its first WSI
    // swapchain just after KHARVOX has submitted a handful of XR layers. VDXR
    // can otherwise accept the replacement and then report DEVICE_LOST on the
    // next XR copy. Quiesce only inside the tightly bounded startup window;
    // ordinary level and resolution changes remain non-blocking.
    return supportedRuntime && fsr1Requested && nativePresents > 0u
        && submittedLayerFrames <= 8u;
}

inline bool useSteamFsrQuadStartupHandshake(bool steamBackedRuntime,
                                            bool fsr1Requested,
                                            bool quadMode,
                                            bool handshakeComplete) {
    return steamBackedRuntime && fsr1Requested && quadMode
        && !handshakeComplete;
}

} // namespace kharvox

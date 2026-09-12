#pragma once

namespace kharvox {
// r260 diagnostic: temporal order only. OpenXR eye indices, image targets,
// FOVs retain 0=left, 1=right. DOOM lateral offsets are defined in AerEyeBasis.h.
inline constexpr int aerFirstRenderEye = 1;
inline constexpr int aerSecondRenderEye = aerFirstRenderEye ^ 1;
// Pair caches use phase 0 to capture and phase 1 to reuse, independently of
// the anatomical eye. Never use this phase as an image/pose array index.
constexpr int aerRenderPairPhase(int eye) { return eye ^ aerFirstRenderEye; }
}

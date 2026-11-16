#pragma once

#include "HandPoseMath.h"
#include "HandVisibilityPolicy.h"

namespace kharvox::hands {

// Decide before borrowing scene attachments: a requested pose without valid
// controller tracking produces no hand geometry. Laser-only passes remain valid.
inline constexpr bool handSceneHasGeometry(const HandVisibilityOutput&visibility,
    const HandPose&leftGrip,const HandPose&rightGrip,const HandPose&laser){
    return (visibility.left!=HandModelKind::None&&leftGrip.valid)
        ||(visibility.right!=HandModelKind::None&&rightGrip.valid)||laser.valid;
}

} // namespace kharvox::hands

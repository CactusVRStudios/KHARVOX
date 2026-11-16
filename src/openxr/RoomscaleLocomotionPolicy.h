#pragma once
#include <cmath>

namespace kharvox {
// Head pose is always used for the view. Converting it into player movement
// is a separate, explicit opt-in; ordinary neck motion must not press a stick.
inline bool allowRoomscaleMovement(bool enabled,bool physicsValid,
    bool bodyValid,bool manualMovement,float horizontalError) {
    return enabled&&physicsValid&&bodyValid&&!manualMovement
        &&std::isfinite(horizontalError)&&horizontalError>.10f;
}
}

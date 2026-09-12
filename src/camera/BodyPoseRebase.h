#pragma once
#include <cmath>
namespace kharvox {
// Express the same world-space head pose in a newer game-body yaw basis.
// No filtering: cancellation is exact, including crossing +/-180 degrees.
inline float rebaseHeadToBody(float referenceYaw,float currentYaw,
    float& headYaw,float& forward,float& left){
    if(!std::isfinite(referenceYaw)||!std::isfinite(currentYaw)
        ||!std::isfinite(headYaw)||!std::isfinite(forward)||!std::isfinite(left))return 0;
    const float delta=std::remainder(currentYaw-referenceYaw,360.0f);
    const float a=delta*0.01745329251994329577f;
    const float c=std::cos(a),s=std::sin(a),f=forward,l=left;
    headYaw=std::remainder(headYaw-delta,360.0f);
    forward=c*f+s*l;left=-s*f+c*l;
    return delta;
}
}

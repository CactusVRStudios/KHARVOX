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
// Owed stick turn at render time: published remaining yaw + rate x age of
// the publication. The render pass can read a publication late (or the pair
// snapshot's), so without the age term a stale read is a zero step, then a
// double. No body term: rebaseHeadToBody already cancels the carrier's body
// motion. Age clamped to 100 ms.
inline float extrapolateArtificialTurn(float artificialYaw,
    float rateDegreesPerSecond=0.f,float elapsedSeconds=0.f){
    if(!std::isfinite(artificialYaw)||artificialYaw==0.f)return artificialYaw;
    float extrapolated=0.f;
    if(std::isfinite(rateDegreesPerSecond)&&std::isfinite(elapsedSeconds)&&elapsedSeconds>0.f)
        extrapolated=rateDegreesPerSecond*(elapsedSeconds<0.1f?elapsedSeconds:0.1f);
    return std::remainder(artificialYaw+extrapolated,360.0f);
}
}

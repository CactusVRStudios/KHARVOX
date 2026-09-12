#pragma once
#include <array>
namespace kharvox {
inline std::array<float,2> cinematicOverrideFov(bool stereo,bool native,float monoX,float monoY,float eyeX,float eyeY){
    return stereo&&!native?std::array<float,2>{eyeX,eyeY}:std::array<float,2>{monoX,monoY};
}
// Preserve eye separation/orientation while removing translation the cinematic
// camera does not render. The centre and both eyes must be from the same sample.
template<class Pose,class Vector>
inline void anchorCinematicEyePair(std::array<Pose,2>& eyes,const Vector& anchor){
    const auto a=eyes[0].position,b=eyes[1].position;
    const float dx=anchor.x-(a.x+b.x)*.5f,dy=anchor.y-(a.y+b.y)*.5f,dz=anchor.z-(a.z+b.z)*.5f;
    for(auto& eye:eyes){eye.position.x+=dx;eye.position.y+=dy;eye.position.z+=dz;}
}
}

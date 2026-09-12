#pragma once
namespace kharvox {
// OpenXR +X points right; DOOM's second camera basis row points LEFT.
// Camera translation therefore has the opposite sign to OpenXR eye-local X.
constexpr float aerDoomEyeOffset(int eye,float halfIpdUnits){return eye==0?halfIpdUnits:eye==1?-halfIpdUnits:0.f;}
constexpr int aerEyeFromDoomOffset(float offset){return offset>0?0:offset<0?1:-1;}
}

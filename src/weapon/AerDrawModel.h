#pragma once
#include <array>
#include <cmath>
namespace kharvox {
inline bool aerDrawSourceEligible(unsigned flags,unsigned billboard){return !(flags&7)&&billboard<0x80;}
// DOOM 0x2C3F70: row-major matrix with scaled axis vectors as columns.
inline bool aerDrawModelMatrix(const float* origin,const float* axis,const float* scale,float* out){
    for(int i=0;i<3;++i)if(!std::isfinite(origin[i])||!std::isfinite(scale[i])||std::abs(scale[i])>100)return false;
    for(int i=0;i<9;++i)if(!std::isfinite(axis[i]))return false;
    for(int r=0;r<3;++r){for(int c=0;c<3;++c)out[4*r+c]=axis[3*c+r]*scale[c];out[4*r+3]=origin[r];}
    out[12]=out[13]=out[14]=0;out[15]=1;return true;
}
}

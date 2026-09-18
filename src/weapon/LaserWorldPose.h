#pragma once
#include <cmath>

namespace kharvox {
// DOOM stores each local basis vector as a row; world = origin + local * axis.
inline bool laserWorldToLocal(const float* world,const float* worldAxis,
    const float* origin,const float* axis,float* local,float* localAxis){
    for(int i=0;i<9;++i)if(!std::isfinite(axis[i])||!std::isfinite(worldAxis[i]))return false;
    for(int i=0;i<3;++i)if(!std::isfinite(world[i])||!std::isfinite(origin[i]))return false;
    float dual[9]{};
    for(int row=0;row<3;++row){
        const int a=((row+1)%3)*3,b=((row+2)%3)*3;
        dual[row*3]=axis[a+1]*axis[b+2]-axis[a+2]*axis[b+1];
        dual[row*3+1]=axis[a+2]*axis[b]-axis[a]*axis[b+2];
        dual[row*3+2]=axis[a]*axis[b+1]-axis[a+1]*axis[b];
    }
    const float determinant=axis[0]*dual[0]+axis[1]*dual[1]+axis[2]*dual[2];
    if(!std::isfinite(determinant)||std::abs(determinant)<1e-6f)return false;
    for(int row=0;row<3;++row){
        local[row]=0;
        for(int c=0;c<3;++c)local[row]+=(world[c]-origin[c])*dual[row*3+c]/determinant;
        if(!std::isfinite(local[row]))return false;
        for(int basis=0;basis<3;++basis){
            localAxis[basis*3+row]=0;
            for(int c=0;c<3;++c)localAxis[basis*3+row]+=worldAxis[basis*3+c]*dual[row*3+c]/determinant;
            if(!std::isfinite(localAxis[basis*3+row]))return false;
        }
    }
    return true;
}
// Carry the observed animated muzzle through the exact model correction used
// by the draw. Includes translation, rotation and model scale; no new tracking.
inline bool laserFollowDraw(const float* sourceOrigin,const float* sourceAxis,
    const float* targetOrigin,const float* targetAxis,float* origin,float* direction){
    float rayAxes[9]{},local[3]{},localAxes[9]{},out[3]{},forward[3]{};
    for(int i=0;i<3;++i)rayAxes[i]=direction[i];
    if(!laserWorldToLocal(origin,rayAxes,sourceOrigin,sourceAxis,local,localAxes))return false;
    float length2=0;
    for(int i=0;i<3;++i){out[i]=targetOrigin[i];
        for(int j=0;j<3;++j){out[i]+=local[j]*targetAxis[j*3+i];forward[i]+=localAxes[j]*targetAxis[j*3+i];}
        if(!std::isfinite(out[i])||!std::isfinite(forward[i]))return false;
        length2+=forward[i]*forward[i];
    }
    if(!std::isfinite(length2)||length2<1e-12f)return false;
    for(int i=0;i<3;++i){origin[i]=out[i];direction[i]=forward[i]/std::sqrt(length2);}
    return true;
}
}

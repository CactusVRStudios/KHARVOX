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
}

#pragma once
#include <cstdint>
#include <cmath>
namespace kharvox {
// Steam DOOMx64vk RTTI: idMenuManager_WeaponSelect and its Framing manager.
inline bool ownedWeaponWheel(uintptr_t vtable) {
    return vtable==0x22456f8 || vtable==0x2245608;
}
inline constexpr float weaponWheelDistanceMeters=3.0f;
inline constexpr float weaponWheelWidthMeters=3.0f;
inline bool weaponWheelPose(const float* eye,const float* head,float units,float* center) {
    if(!std::isfinite(units)||units<=0)return false;
    for(int i=0;i<3;++i){
        center[i]=eye[i]+head[i]*weaponWheelDistanceMeters*units;
        if(!std::isfinite(center[i]))return false;
    }
    return true;
}
// Preserve the native canvas handedness and row lengths, remove authored tilt.
inline bool flatWeaponWheelAxis(const float* native,const float* head,float* out) {
    int used=0;
    for(int row=0;row<3;++row){
        float local[3]{},length=0;
        for(int i=0;i<3;++i){
            if(!std::isfinite(native[row*3+i]))return false;
            length+=native[row*3+i]*native[row*3+i];
            for(int j=0;j<3;++j)local[j]+=native[row*3+i]*head[j*3+i];
        }
        length=std::sqrt(length);if(length<.00001f)return false;
        int best=0;for(int j=1;j<3;++j)if(std::abs(local[j])>std::abs(local[best]))best=j;
        if((used&(1<<best)) || (row<2&&best==0))return false;
        used|=1<<best;
        for(int i=0;i<3;++i)out[row*3+i]=head[best*3+i]*(local[best]<0?-length:length);
    }
    return true;
}
}

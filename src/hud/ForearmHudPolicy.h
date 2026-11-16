#pragma once
#include <array>
#include <cmath>
#include <istream>

namespace kharvox {
struct ForearmHudPanel {
    float widthCm{},heightCm{};
    // Three rows of a rigid panel -> virtual DOOM Grip matrix. Translation
    // is centimetres; columns are Blender right/up/front, not native GUI rows.
    std::array<float,12> matrix{};
};
struct ForearmHudConfig {
    bool enabled{};
    // Native owner slots: WeaponInfo/ammo, BottomLeft/health, ProgMeter.
    std::array<ForearmHudPanel,3> panels{};
};
inline bool forearmHudRigid(const float* rows){
    for(int i=0;i<9;++i)if(!std::isfinite(rows[i]))return false;
    for(int i=0;i<3;++i)for(int j=i;j<3;++j){
        float dot{};for(int k=0;k<3;++k)dot+=rows[i*3+k]*rows[j*3+k];
        if(std::abs(dot-(i==j?1.f:0.f))>.001f)return false;
    }
    const float det=rows[0]*(rows[4]*rows[8]-rows[5]*rows[7])
        -rows[1]*(rows[3]*rows[8]-rows[5]*rows[6])+rows[2]*(rows[3]*rows[7]-rows[4]*rows[6]);
    return det>.999f;
}
inline bool readForearmHudConfig(std::istream& input,ForearmHudConfig& output){
    int version{},enabled{};ForearmHudConfig next;
    if(!(input>>version>>enabled)||version!=1||(enabled!=0&&enabled!=1))return false;
    for(auto& panel:next.panels){
        if(!(input>>panel.widthCm>>panel.heightCm)||!std::isfinite(panel.widthCm)
            ||!std::isfinite(panel.heightCm)||panel.widthCm<1||panel.widthCm>100
            ||panel.heightCm<0||panel.heightCm>100)return false;
        for(float& value:panel.matrix)if(!(input>>value)||!std::isfinite(value)||std::abs(value)>200)return false;
        float rotation[9]{};
        for(int row=0;row<3;++row)for(int column=0;column<3;++column)
            rotation[row*3+column]=panel.matrix[row*4+column];
        if(!forearmHudRigid(rotation))return false;
    }
    // Life/ammo retain the native canvas; ProgMeter height remains dynamic.
    for(int i=0;i<2;++i)if(std::abs(next.panels[i].heightCm/next.panels[i].widthCm-300.f/512.f)>.001f)return false;
    if(next.panels[2].heightCm!=0)return false;
    input>>std::ws;if(!input.eof())return false;
    next.enabled=enabled!=0;output=next;return true;
}
inline bool forearmHudActive(const ForearmHudConfig& config,bool leftHanded){
    return config.enabled&&!leftHanded;
}
inline bool forearmHudTransform(const ForearmHudConfig& config,int surface,bool leftHanded,
    const float* grip,const float* hand,float unitsPerMeter,float* center,float* pixels){
    if(!forearmHudActive(config,leftHanded)||surface<0||surface>2
        ||!std::isfinite(unitsPerMeter)||unitsPerMeter<=0||!forearmHudRigid(hand))return false;
    const auto& matrix=config.panels[surface].matrix;
    float position[3]{},axis[9]{};
    for(int world=0;world<3;++world){
        if(!std::isfinite(grip[world]))return false;
        position[world]=grip[world];
        for(int local=0;local<3;++local){
            position[world]+=hand[local*3+world]*matrix[local*4+3]*unitsPerMeter*.01f;
            // Native GUI pixels increase right/down. Blender panel Y is up;
            // reversing Y and normal is a basis conversion, not a pose offset.
            for(int pixel=0;pixel<3;++pixel)
                axis[pixel*3+world]+=hand[local*3+world]*matrix[local*4+pixel]*(pixel==0?1.f:-1.f);
        }
        if(!std::isfinite(position[world]))return false;
    }
    if(!forearmHudRigid(axis))return false;
    for(int i=0;i<3;++i)center[i]=position[i];
    for(int i=0;i<9;++i)pixels[i]=axis[i];
    return true;
}
inline bool forearmHudPixelAxis(const float* orientation,const float* nativeAxis,float* output){
    if(!forearmHudRigid(orientation))return false;
    float result[9]{};
    for(int row=0;row<3;++row){
        float squared{};
        for(int i=0;i<3;++i){const float value=nativeAxis[row*3+i];if(!std::isfinite(value))return false;squared+=value*value;}
        const float length=std::sqrt(squared);
        if(!std::isfinite(length)||length<.00001f)return false;
        for(int i=0;i<3;++i)result[row*3+i]=length*orientation[row*3+i];
    }
    for(int i=0;i<9;++i)output[i]=result[i];
    return true;
}
}

#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <istream>
namespace kharvox {
struct OffhandHudCalibration {
    std::array<float,3> centimeters{8,0,8};
    std::array<float,3> degrees{}; // pitch, yaw, roll
    float scale{0.10f};
};
struct OffhandHudConfig { bool enabled{true};std::array<OffhandHudCalibration,2> modes{}; };
inline int offhandHudSurface(uintptr_t caller,int width,int height,int scale){
    if(caller!=0xbdcf54||width!=512||height!=300)return -1;
    return scale==83?0:scale==100?1:-1;
}
inline bool readOffhandHudConfig(std::istream& stream,OffhandHudConfig& out){
    int version{},enabled{};OffhandHudConfig value;
    if(!(stream>>version>>enabled)||version!=1||(enabled!=0&&enabled!=1))return false;
    for(auto& mode:value.modes){
        for(auto& v:mode.centimeters)if(!(stream>>v)||!std::isfinite(v)||std::abs(v)>100)return false;
        for(auto& v:mode.degrees)if(!(stream>>v)||!std::isfinite(v)||std::abs(v)>180)return false;
        if(!(stream>>mode.scale)||!std::isfinite(mode.scale)||mode.scale<.02f||mode.scale>2)return false;
    }
    value.enabled=enabled!=0;out=value;return true;
}
inline void offhandHudBasis(const float* hand,const OffhandHudCalibration& c,float* out){
    std::array<float,9> local{1,0,0,0,1,0,0,0,1};
    // Intrinsic pitch (forward/up), yaw (forward/left), roll (left/up).
    const int pairs[3][2]{{0,2},{0,1},{1,2}};
    for(int r=0;r<3;++r){const float a=c.degrees[r]*.01745329251994329577f;
        const float cs=std::cos(a),sn=std::sin(a);const int x=pairs[r][0],y=pairs[r][1];
        for(int j=0;j<3;++j){const float u=local[x*3+j],v=local[y*3+j];local[x*3+j]=cs*u+sn*v;local[y*3+j]=-sn*u+cs*v;}}
    for(int i=0;i<3;++i)for(int j=0;j<3;++j){out[i*3+j]=0;for(int k=0;k<3;++k)out[i*3+j]+=local[i*3+k]*hand[k*3+j];}
}
inline void offhandHudOrigin(const float* grip,const float* hand,const float* panel,
    const OffhandHudCalibration& c,int surface,float unitsPerMeter,float* out){
    for(int i=0;i<3;++i){out[i]=grip[i];
        for(int j=0;j<3;++j)out[i]+=hand[j*3+i]*c.centimeters[j]*unitsPerMeter*.01f;
        out[i]+=panel[3+i]*(surface==0?1.f:-1.f)*7.f*(c.scale/.10f)*unitsPerMeter*.01f;}
}
}

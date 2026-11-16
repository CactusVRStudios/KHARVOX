#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <istream>

namespace kharvox {
struct WristHudGestureConfig {
    bool enabled{};
    float faceOn{40},faceOff{55},fovOn{25},fovOff{35},nearCm{15},farCm{85};
    float onMs{120},offMs{180},fadeInMs{160},fadeOutMs{220};
};
inline bool readWristHudGestureConfig(std::istream& input,WristHudGestureConfig& output){
    int version{},enabled{};WristHudGestureConfig next;
    if(!(input>>version>>enabled)||version!=1||(enabled!=0&&enabled!=1))return false;
    for(float* value:{&next.faceOn,&next.faceOff,&next.fovOn,&next.fovOff,&next.nearCm,
        &next.farCm,&next.onMs,&next.offMs,&next.fadeInMs,&next.fadeOutMs})
        if(!(input>>*value)||!std::isfinite(*value))return false;
    if(next.faceOn<0||next.faceOff<=next.faceOn||next.faceOff>=90||next.fovOn<=0
        ||next.fovOff<=next.fovOn||next.fovOff>=90||next.nearCm<1||next.farCm<=next.nearCm
        ||next.farCm>200||next.onMs<0||next.offMs<0||next.onMs>2000||next.offMs>2000
        ||next.fadeInMs<1||next.fadeOutMs<1||next.fadeInMs>2000||next.fadeOutMs>2000)return false;
    input>>std::ws;if(!input.eof())return false;
    next.enabled=enabled!=0;output=next;return true;
}
struct WristHudMetrics {float distanceCm{},facing{},fov{};bool valid{};};
inline WristHudMetrics wristHudMetrics(const float* center,const float* front,
    const float* eye,const float* forward,float unitsPerMeter){
    WristHudMetrics result;
    if(!std::isfinite(unitsPerMeter)||unitsPerMeter<=0)return result;
    float squared{},normalLength{},forwardLength{};
    for(int i=0;i<3;++i){
        if(!std::isfinite(center[i])||!std::isfinite(front[i])||!std::isfinite(eye[i])||!std::isfinite(forward[i]))return result;
        const float delta=eye[i]-center[i];squared+=delta*delta;
        normalLength+=front[i]*front[i];forwardLength+=forward[i]*forward[i];
    }
    const float distance=std::sqrt(squared);
    if(distance<1e-6f||std::abs(normalLength-1.f)>.001f||std::abs(forwardLength-1.f)>.001f)return result;
    for(int i=0;i<3;++i){const float toward=(eye[i]-center[i])/distance;
        result.facing+=front[i]*toward;result.fov-=forward[i]*toward;}
    result.distanceCm=distance/unitsPerMeter*100.f;
    result.valid=std::isfinite(result.distanceCm);return result;
}
struct WristHudGestureState {
    bool active{},pending{},initialized{};
    double since{},last{};
    float alpha{};
    void reset(){*this={};}
    float update(const WristHudGestureConfig& config,const WristHudMetrics& metrics,double now){
        if(!config.enabled){reset();return 1.f;}
        if(!metrics.valid||!std::isfinite(metrics.distanceCm)||!std::isfinite(metrics.facing)
            ||!std::isfinite(metrics.fov)||!std::isfinite(now)||(initialized&&now<last)){
            reset();return 0.f;
        }
        constexpr float radians=.01745329251994329577f;
        const bool wanted=metrics.distanceCm>=config.nearCm-(active?3.f:0.f)
            &&metrics.distanceCm<=config.farCm+(active?5.f:0.f)
            &&metrics.facing>=std::cos((active?config.faceOff:config.faceOn)*radians)
            &&metrics.fov>=std::cos((active?config.fovOff:config.fovOn)*radians);
        if(wanted!=active){
            if(!pending){since=now;pending=true;}
            if(now-since>=(active?config.offMs:config.onMs)*.001){active=wanted;pending=false;}
        }else pending=false;
        const double dt=initialized?std::clamp(now-last,0.,.1):0.;last=now;initialized=true;
        const double duration=(active?config.fadeInMs:config.fadeOutMs)*.001;
        alpha=std::clamp(alpha+static_cast<float>((active?dt:-dt)/duration),0.f,1.f);
        return alpha;
    }
};
// Native SWF render-state color transform: matrix[6], multiplicative RGBA,
// additive RGBA, followed by native mask state. Only alpha channels change.
inline bool fadeWristHudNativeState(std::array<float,22>& state,float opacity){
    if(!std::isfinite(opacity)||opacity<0||opacity>1
        ||!std::isfinite(state[9])||!std::isfinite(state[13]))return false;
    state[9]*=opacity;state[13]*=opacity;return true;
}
}

#include "../src/hud/WristHudGesturePolicy.h"
#include "../src/hands/HandPoseMath.h"
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

namespace {bool near(float a,float b){return std::abs(a-b)<.0001f;}}
int main(int argc,char** argv){
    using namespace kharvox;
    WristHudGestureConfig config;
    std::istringstream input("1 1 40 55 25 35 15 85 120 180 160 220");
    if(!readWristHudGestureConfig(input,config)||!config.enabled)return 1;
    for(const char* bad:{"2 1","1 1","1 1 55 40 25 35 15 85 120 180 160 220", "1 1 40 55 25 35 15 85 120 180 0 220"}){
        std::istringstream stream(bad);auto previous=config;
        if(readWristHudGestureConfig(stream,previous)||!near(previous.faceOn,40))return 2;
    }
    if(argc>2)return 3;
    if(argc==2){std::ifstream file(argv[1]);if(!readWristHudGestureConfig(file,config))return 4;}
    const float center[3]{1,2,3},front[3]{0,0,1},eye[3]{1,2,63},forward[3]{0,0,-1};
    const auto metrics=wristHudMetrics(center,front,eye,forward,100);
    if(!metrics.valid||!near(metrics.distanceCm,60)||!near(metrics.facing,1)||!near(metrics.fov,1))return 5;
    WristHudGestureState state;
    state.update(config,metrics,1.);state.update(config,metrics,1.119);
    if(state.active||state.alpha!=0)return 6;
    state.update(config,metrics,1.121);
    if(!state.active||state.alpha<=0||state.alpha>=1)return 7;
    state.update(config,metrics,1.3);state.update(config,metrics,1.4);
    if(!near(state.alpha,1))return 8;
    auto border=metrics;border.facing=std::cos(50.f*.01745329252f);
    state.update(config,border,1.5);if(!state.active)return 9;
    auto away=metrics;away.facing=-1;
    state.update(config,away,2.);state.update(config,away,2.179);if(!state.active)return 10;
    state.update(config,away,2.181);if(state.active||state.alpha<=0||state.alpha>=1)return 11;
    for(double t:{2.4,2.5,2.6})state.update(config,away,t);
    if(state.alpha!=0)return 12;
    state.update(config,border,3.);if(state.active)return 13;
    auto outside=metrics;outside.fov=0;
    state.update(config,outside,4.);state.update(config,outside,5.);if(state.active)return 14;
    auto far=metrics;far.distanceCm=100;
    state.update(config,far,6.);state.update(config,far,7.);if(state.active)return 15;
    state.update(config,metrics,8.);state.update(config,metrics,8.2);
    state.update(config,WristHudMetrics{},8.3);if(state.active||state.alpha!=0)return 16;
    const auto disabled=WristHudGestureConfig{};
    if(!near(state.update(disabled,metrics,9.),1))return 17;
    // A shared world rotation/translation cannot change head-relative metrics.
    using namespace kharvox::hands::math;
    const float degrees[]{23,71,-35};const auto rotate=rotationDegrees(degrees);
    auto transform=[&](const float* point,bool position,std::array<float,3>& out){
        for(int row=0;row<3;++row){out[row]=position?float((row+1)*17):0;
            for(int col=0;col<3;++col)out[row]+=rotate.m[col*4+row]*point[col];}
    };
    std::array<float,3> c{},n{},e{},f{};
    transform(center,true,c);transform(front,false,n);transform(eye,true,e);transform(forward,false,f);
    const auto rotated=wristHudMetrics(c.data(),n.data(),e.data(),f.data(),100);
    if(!rotated.valid||!near(rotated.distanceCm,60)||!near(rotated.facing,1)||!near(rotated.fov,1))return 18;
    // State opacity preserves matrix, RGB transforms and native mask bits.
    std::array<float,22> native{};
    for(int i=0;i<22;++i)native[i]=float(i)+.25f;
    native[9]=1;native[13]=.2f;const auto before=native;
    if(!fadeWristHudNativeState(native,.5f)||!near(native[9],.5f)||!near(native[13],.1f))return 19;
    for(int i=0;i<22;++i)if(i!=9&&i!=13&&std::memcmp(&native[i],&before[i],sizeof(float)))return 20;
    if(fadeWristHudNativeState(native,-1))return 21;
    std::cout<<"Wrist HUD gesture: head-relative metrics, hysteresis, delays, fades, tracking reset and native state preservation passed\n";
    return 0;
}

#include "../src/sfs/FrameProjection.h"
#include <iostream>
#include <stdexcept>
using namespace kharvox::sfs;
static void check(bool value){if(!value)throw std::runtime_error("SFS projection mismatch");}
int main(){try{
    XrPosef center{{0,0,0,1},{0,1.6f,0}};
    XrFovf source{-.85f,.85f,.8f,-.8f};
    std::array<XrView,2> eyes{{{XR_TYPE_VIEW},{XR_TYPE_VIEW}}};
    for(unsigned e=0;e<2;++e){eyes[e].pose=center;eyes[e].pose.position.x=e?.032f:-.032f;eyes[e].fov={e?-.7f:-.9f,e?.9f:.7f,.75f,-.8f};}
    FrameUniforms data;check(frameProjection(center,source,eyes,39.37f,true,data));
    auto centeredEyes=eyes;for(auto& eye:centeredEyes)eye.fov=source;
    FrameUniforms centered;check(frameProjection(center,source,centeredEyes,39.37f,true,centered));
    check(centered.legacy[0].stereo[0]==0&&centered.legacy[1].stereo[0]==0);
    check(centered.legacy[0].stereo[2]>0&&centered.legacy[1].stereo[2]<0);
    check(centered.legacy[0].stereo[2]==centered.translation[0][0]);
    for(unsigned e=0;e<2;++e)for(float depth:{10.f,100.f,1000.f})for(float x:{-5.f,0.f,7.f}){
        const float y=2.f,l=std::tan(source.angleLeft),r=std::tan(source.angleRight),b=std::tan(source.angleDown),t=std::tan(source.angleUp);
        const float originalX=2*x/(r-l)-(r+l)*depth/(r-l);
        const float originalY=-2*y/(t-b)+(t+b)*depth/(t-b);
        const float el=std::tan(eyes[e].fov.angleLeft),er=std::tan(eyes[e].fov.angleRight),eb=std::tan(eyes[e].fov.angleDown),et=std::tan(eyes[e].fov.angleUp);
        const float expectedX=2*(x-eyes[e].pose.position.x*39.37f)/(er-el)-(er+el)*depth/(er-el);
        const float expectedY=-2*y/(et-eb)+(et+eb)*depth/(et-eb);
        check(std::abs((data.clip[e][0]*originalX+data.clip[e][12]*depth+data.translation[e][0]-expectedX)/depth)<1e-5f);
        check(std::abs((data.clip[e][5]*originalY+data.clip[e][13]*depth+data.translation[e][1]-expectedY)/depth)<1e-5f);
    }
    eyes[0].pose.position.z=.01f;check(!frameProjection(center,source,eyes,39.37f,true,data));
    check(frameProjection(center,source,eyes,39.37f,false,data));check(data.translation[0][0]==0&&data.clip[0][0]==1);
    eyes[0].pose.position.z=0;eyes[0].pose.orientation={0,.1f,0,.994987f};check(!frameProjection(center,source,eyes,39.37f,true,data));
    std::cout<<"SFS asymmetric FOV/IPD projection and unsupported-pose rejection passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

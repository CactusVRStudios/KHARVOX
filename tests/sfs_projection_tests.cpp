#include "../src/sfs/FrameProjection.h"
#include "../src/sfs/SourcePoseHistory.h"
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
        // The glass lookup's homogeneous window transform must land on the
        // same eye pixel as the independently projected world point above.
        const float windowX=(originalX+depth)*.5f,windowY=(originalY+depth)*.5f;
        const float refrX=((windowX*2-depth)*data.clip[e][0]+data.clip[e][12]*depth+data.translation[e][0]+depth)*.5f/depth;
        const float refrY=((windowY*2-depth)*data.clip[e][5]+data.clip[e][13]*depth+data.translation[e][1]+depth)*.5f/depth;
        check(std::abs(refrX-(expectedX/depth+1)*.5f)<1e-5f);
        check(std::abs(refrY-(expectedY/depth+1)*.5f)<1e-5f);
    }
    // A screen overlay must describe the same ray in asymmetric eye frusta.
    // Leaving center-camera NDC unchanged (test7) fails this round trip. The
    // screen/world split affects IPD only, never the FOV conversion.
    for(float w:{1.f,6.f,8.f,8.01f,40.f})for(float nx:{-.7f,0.f,.6f}){
        const float ny=.3f;
        const float sl=std::tan(source.angleLeft),sr=std::tan(source.angleRight);
        const float sb=std::tan(source.angleDown),st=std::tan(source.angleUp);
        for(unsigned e=0;e<2;++e){
            const float x=data.clip[e][0]*nx+data.clip[e][12]+(w>8?data.translation[e][0]/w:0);
            const float y=data.clip[e][5]*ny+data.clip[e][13]+(w>8?data.translation[e][1]/w:0);
            const float l=std::tan(eyes[e].fov.angleLeft),r=std::tan(eyes[e].fov.angleRight);
            const float b=std::tan(eyes[e].fov.angleDown),t=std::tan(eyes[e].fov.angleUp);
            const float rayX=(x*(r-l)+r+l)*.5f;
            const float rayY=(-y*(t-b)+t+b)*.5f;
            check(std::abs(rayX-((nx*(sr-sl)+sr+sl)*.5f-(w>8?eyes[e].pose.position.x*39.37f/w:0)))<1e-5f);
            check(std::abs(rayY-((-ny*(st-sb)+st+sb)*.5f))<1e-5f);
        }
    }
    SourcePoseHistory history;
    kharvox::native::FramePose producer{};producer.gameplay=true;producer.serial=10;
    producer.source={91,2,kharvox::native::SceneDomain::Gameplay};producer.head=center;producer.views=eyes;
    history.remember(producer,data);
    auto acquired=producer;acquired.serial=12;acquired.source.poseId=93;
    acquired.head.orientation={0,.1f,0,.994987f};
    kharvox::native::FramePose resolved;
    kharvox::AerSourceObservation observation{{91,2,0,0},1,false};
    check(history.resolve(observation,acquired,data,resolved));
    check(resolved.source.poseId==91&&resolved.head.orientation.w==1&&resolved.serial==10);
    observation.ambiguous=true;check(!history.resolve(observation,acquired,data,resolved));observation.ambiguous=false;
    observation.key.level=3;check(!history.resolve(observation,acquired,data,resolved));observation.key.level=2;
    auto changed=data;changed.clip[0][0]+=.1f;check(!history.resolve(observation,acquired,changed,resolved));
    acquired.serial=20;check(!history.resolve(observation,acquired,data,resolved));
    eyes[0].pose.position.z=.01f;check(!frameProjection(center,source,eyes,39.37f,true,data));
    check(frameProjection(center,source,eyes,39.37f,false,data));check(data.translation[0][0]==0&&data.clip[0][0]==1);
    eyes[0].pose.position.z=0;eyes[0].pose.orientation={0,.1f,0,.994987f};check(!frameProjection(center,source,eyes,39.37f,true,data));
    std::cout<<"SFS asymmetric FOV/IPD projection and unsupported-pose rejection passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

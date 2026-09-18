#include "../src/camera/AerCameraPairCache.h"
#include "../src/weapon/AerWeaponSource.h"
#include "../src/native/NativeFrameSource.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace kharvox;
static void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
static bool near(float a,float b){return std::abs(a-b)<.002f;}
int main(){try{
    AerCameraPairCache cameraCache;
    AerWeaponSourceHistory history;
    AerWeaponSourceTransforms attachments;
    // SFS advances one centered CPU camera per frame, never an AER second
    // phase. Exercise history wraparound while walking and body yaw catches up.
    for(uint64_t tick=1;tick<=600;++tick){
        const uint64_t level=1+tick/200,epoch=1+tick/100;
        const unsigned generation=1+unsigned(tick/150);
        const uint64_t pose=tick*2;
        AerWeaponInput input;
        input.valid=true;input.epoch=epoch;input.generation=generation;
        input.grip={12.f+std::sin(float(tick)*.03f),-6.f,3.f};
        history.remember(pose,level,input);
        // A later prediction may arrive before this frame's weapon is built.
        auto future=input;future.grip={90,80,70};
        history.remember(pose+1,level,future);
        const float degrees=float(int(tick%120)-60),a=degrees*.01745329252f;
        const float c=std::cos(a),s=std::sin(a);
        AerWeaponCamera camera;camera.key={pose,level,0};camera.present=tick;
        camera.bodyOrigin={float(tick)*.7f,float(tick)*-.2f,64.f};
        camera.bodyAxis={c,s,0,-s,c,0,0,0,1};camera.headAxis=camera.bodyAxis;
        camera.bodyYawDelta=degrees;
        cameraCache.begin(aerFirstRenderEye,true);
        float origin[3],axis[9];
        std::copy(camera.bodyOrigin.begin(),camera.bodyOrigin.end(),origin);
        std::copy(camera.bodyAxis.begin(),camera.bodyAxis.end(),axis);
        check(!cameraCache.resolve(0x1000,origin,axis),"SFS reused an AER camera phase");
        check(near(origin[0],camera.bodyOrigin[0]),"Walking camera held an older frame");
        history.camera(camera);
        AerWeaponFrame frame;
        check(history.resolve(tick,level,epoch,generation,frame),"Centered SFS weapon source missing");
        check(frame.input.grip==input.grip,"Future controller leaked into rendered source");
        check(aerControllerWorldFrame(frame,origin,axis),"Controller world frame invalid");
        for(int i=0;i<3;++i)check(near(origin[i],camera.bodyOrigin[i]+input.grip[i]),"Body yaw catch-up moved controller relative to player");
        check(!attachments.hold(frame,0x2000,0,origin,axis),"New SFS frame held old weapon transform");
        const float expected=origin[0];origin[0]+=20;
        check(attachments.hold(frame,0x2000,0,origin,axis)&&near(origin[0],expected),"Same-frame attachment lost source transform");
        check(!history.resolve(tick,level+1,epoch,generation,frame),"Old level accepted");
        check(!history.resolve(tick,level,epoch+1,generation,frame),"Reset weapon accepted");
        check(!history.resolve(tick,level,epoch,generation+1,frame),"Old calibration accepted");
        auto missing=input;missing.valid=false;
        history.remember(pose+10000,level,missing);camera.key.poseId=pose+10000;history.camera(camera);
        check(!history.resolve(tick,level,epoch,generation,frame),"Missing tracking invented a weapon pose");
        history.remember(pose+10000,level,input);
        check(history.resolve(tick,level,epoch,generation,frame),"Tracking recovery did not repair source");
        using namespace kharvox::native;
        const FrameSourceIdentity source{pose,level,SceneDomain::Gameplay};
        check(sameSceneContext(source,{pose+1,level,SceneDomain::Gameplay},false,false),"Next prediction rejected current pair");
        check(!sameSceneContext(source,{pose+1,level,SceneDomain::Inactive},false,false),"Menu accepted gameplay pair");
        check(!sameSceneContext(source,{pose+1,level,SceneDomain::Cinematic},false,false),"Cinematic accepted gameplay pair");
        check(!sameSceneContext(source,{pose+1,level+1,SceneDomain::Gameplay},false,false),"Checkpoint accepted prior world");
    }
    std::cout<<"SFS 600-frame gameplay/source contract passed (synthetic inputs, no hardware claim)\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

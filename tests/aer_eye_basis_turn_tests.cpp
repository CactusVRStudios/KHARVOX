#include "../src/common/AerEyeBasis.h"
#include "../src/common/AerRenderOrder.h"
#include "../src/camera/CameraBasisPolicy.h"
#include "../src/openxr/ControllerFrameYaw.h"
#include <array>
#include <cmath>
#include <cstdlib>

using Vec = std::array<float,3>;
void check(bool ok){if(!ok)std::abort();}
bool near(float a,float b){return std::abs(a-b)<.00001f;}
float dot(Vec a,Vec b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
Vec sub(Vec a,Vec b){return {a[0]-b[0],a[1]-b[1],a[2]-b[2]};}
Vec scale(Vec a,float s){return {a[0]*s,a[1]*s,a[2]*s};}
// Rigid world rotations; pitch and roll cannot change anatomical eye identity.
Vec tilt(Vec a,float pitch,float roll){
    const float c=std::cos(pitch),s=std::sin(pitch),cr=std::cos(roll),sr=std::sin(roll);
    const Vec p{c*a[0]+s*a[2],a[1],-s*a[0]+c*a[2]};
    return {p[0],cr*p[1]-sr*p[2],sr*p[1]+cr*p[2]};
}
// OpenXR yaw rotates both controller position and its forward vector about +Y.
Vec yaw(Vec a,float degrees){
    const float angle=degrees*.0174532925199433f,c=std::cos(angle),s=std::sin(angle);
    return {c*a[0]+s*a[2],a[1],-s*a[0]+c*a[2]};
}
int main(){
    using namespace kharvox;
    constexpr float half=.032f;
    for(float heading:{0.f,.8f,2.f,-2.7f}){
        const float source[9]{std::cos(heading),std::sin(heading),0,0,0,0,0,0,1};
        float body[9]{};check(makeGravityLevelBodyBasis(source,nullptr,body));
        for(float pitch:{0.f,.6f,-.9f})for(float roll:{0.f,.4f,-.7f}){
            const Vec forward=tilt({body[0],body[1],body[2]},pitch,roll);
            const Vec left=tilt({body[3],body[4],body[5]},pitch,roll);
            const Vec originL=scale(left,aerDoomEyeOffset(0,half));
            const Vec originR=scale(left,aerDoomEyeOffset(1,half));
            check(near(dot(sub(originL,originR),left),2*half));
            for(float distance:{.25f,.7f,10.f}){
                const Vec point=scale(forward,distance);
                // Screen +X points right, opposite to DOOM's row 1.
                const float xL=-dot(sub(point,originL),left)/dot(sub(point,originL),forward);
                const float xR=-dot(sub(point,originR),left)/dot(sub(point,originR),forward);
                check(xL>0&&xR<0); // near centered point has crossed disparity
                check(near(xL-xR,2*half/distance));
            }
        }
    }
    for(int eye:{aerFirstRenderEye,aerSecondRenderEye})
        check(aerEyeFromDoomOffset(aerDoomEyeOffset(eye,half))==eye);
    check(aerFirstRenderEye==1&&aerSecondRenderEye==0);
    check(aerEyeFromDoomOffset(0)==-1&&aerDoomEyeOffset(-1,half)==0);
    // Action processing advances pending turn after head publication, in either
    // direction and across reversal. A later body catch-up must not change it.
    check(usePublishedControllerYaw(false,false)); // AER
    check(usePublishedControllerYaw(true,true)); // SFS
    check(!usePublishedControllerYaw(true,false)); // native replay unchanged
    for(bool sfs:{false,true})
    for(float accepted:{-60.f,0.f,75.f})for(float published:{-12.f,0.f,12.f})
    for(float advance:{-20.f,-3.f,3.f,20.f}){
        ControllerFrameYaw frame{accepted,published,usePublishedControllerYaw(sfs,sfs)};
        const float pending=published+advance,liveAccepted=accepted+8.f;
        for(Vec v:{Vec{.2f,-.3f,-.6f},Vec{0,0,-1}}){
            const Vec headSpace=yaw(yaw(v,-accepted),published);
            const Vec controller=yaw(yaw(v,-frame.acceptedForPose(liveAccepted)),frame.turnForPose(pending));
            for(int i=0;i<3;++i)check(near(controller[i],headSpace[i]));
            // The laser's inverse uses exactly the same frame convention.
            const Vec roundTrip=yaw(yaw(controller,-frame.turnForPose(pending)),frame.acceptedForPose(liveAccepted));
            for(int i=0;i<3;++i)check(near(roundTrip[i],v[i]));
            const Vec old=yaw(yaw(v,-accepted),pending);
            check(std::hypot(old[0]-headSpace[0],old[2]-headSpace[2])>.001f);
        }
        frame.active=false;
        check(frame.turnForPose(pending)==pending&&frame.acceptedForPose(liveAccepted)==liveAccepted);
    }
}

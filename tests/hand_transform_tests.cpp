#include "../src/hands/HandPoseMath.h"
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

using namespace kharvox::hands;
using namespace kharvox::hands::math;

namespace {
void check(bool value) { if(!value) std::abort(); }
bool near(float a,float b) { return std::abs(a-b)<0.0001f; }
std::array<float,4> point(const Mat4&m,float x,float y,float z) {
    std::array<float,4> result{};
    for(int row=0;row<4;++row)
        result[row]=m.m[row]*x+m.m[4+row]*y+m.m[8+row]*z+m.m[12+row];
    return result;
}
}

int main(int argc,char**argv) {
    check(argc==2);
    // A rotated controller must rotate the wrist translation, but mesh scale
    // and left-asset mirroring must never scale/mirror that translation.
    constexpr float halfSqrt=0.7071067811865475f;
    HandPose grip{{1,2,3},{0,halfSqrt,0,halfSqrt},true};
    HandCalibration wrist{{.1f,.2f,.3f},{0,0,90},.4f};
    for(bool mirrored:{false,true}) {
        const auto model=calibratedHandMatrix(grip,wrist,mirrored);
        const auto origin=point(model,0,0,0);
        check(near(origin[0],1.3f)&&near(origin[1],2.2f)&&near(origin[2],2.9f));
        const auto finger=point(model,1,0,0);
        check(near(finger[0],1.3f)&&near(finger[2],2.9f));
        check(near(finger[1],mirrored?1.8f:2.6f));
    }
    const auto inverse=multiply(inversePose(grip),poseMatrix(grip));
    for(int i=0;i<16;++i)check(near(inverse.m[i],i%5==0?1.f:0.f));

    // Exercise every shipped two-hand support point, including side grips.
    // The same metre-space model/projection is used by SFS and AER. These
    // synthetic poses check geometry, not live controller-to-weapon contact.
    std::ifstream profiles(argv[1]);check(bool(profiles));
    int version{},count{};std::string weapon,mode;float x{},y{},z{},radius{},weight{};
    while(profiles>>version>>weapon>>mode>>x>>y>>z>>radius>>weight) {
        check(version==2&&radius>0&&weight>=0&&weight<=1);
        for(bool leftHanded:{false,true}) {
            const float lateral=leftHanded?-x:x;
            // Known +90 degree yaw maps (x,y,z) to (z,y,-x).
            const HandPose primary{{0,-.1f,-1.5f},{0,halfSqrt,0,halfSqrt},true};
            const auto contact=point(poseMatrix(primary),lateral,y,z);
            check(near(contact[0],z)&&near(contact[1],y-.1f)
                &&near(contact[2],-1.5f-lateral));
            HandPose support{{contact[0],contact[1],contact[2]},{0,0,0,1},true};
            const auto model=calibratedHandMatrix(support,{},leftHanded);
            std::array<float,2> projectedX{};
            for(int eye=0;eye<2;++eye) {
                HandEyeView view{};
                view.pose.position[0]=eye==0?-.032f:.032f;
                view.pose.orientation[3]=1;view.pose.valid=true;
                view.angleLeft=-.8f;view.angleRight=.8f;
                view.angleUp=.9f;view.angleDown=-.7f;
                const auto clip=point(multiply(projection(view),
                    multiply(inversePose(view.pose),model)),0,0,0);
                check(clip[3]>0&&std::abs(clip[0])<clip[3]
                    &&std::abs(clip[1])<clip[3]&&clip[2]>0&&clip[2]<clip[3]);
                projectedX[eye]=clip[0]/clip[3];
                const float expectedY=(std::tan(.9f)+std::tan(-.7f)
                    -2.f*contact[1]/-contact[2])
                    /(std::tan(.9f)-std::tan(-.7f));
                check(near(clip[1]/clip[3],expectedY));
            }
            check(projectedX[0]>projectedX[1]); // Correct stereo parallax.
        }
        ++count;
    }
    check(profiles.eof()&&count==9);
    std::cout<<"Hand wrist transforms and nine support profiles passed\n";
}

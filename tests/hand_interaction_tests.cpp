#include "../src/hands/HandInteractionPolicy.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

using namespace kharvox::hands;
using namespace kharvox::hands::math;
namespace {
void check(bool value){if(!value)std::abort();}
bool near(float a,float b){return std::abs(a-b)<.0001f;}
std::vector<std::uint8_t> bytes(const std::filesystem::path&path){
    std::ifstream in(path,std::ios::binary);check(bool(in));
    return {std::istreambuf_iterator<char>(in),{}};
}
std::array<float,3> point(const Mat4&model,const std::array<float,3>&v){
    std::array<float,3> result{};
    for(int i=0;i<3;++i)result[i]=model.m[12+i]+model.m[i]*v[0]+model.m[4+i]*v[1]+model.m[8+i]*v[2];
    return result;
}
}
int main(int argc,char**argv){
    check(argc==3);
    const auto open=controllerFingerPose(0,0),index=controllerFingerPose(1,0),grip=controllerFingerPose(0,1);
    check(near(open.index,0)&&near(open.grip,0)&&near(open.thumb,0));
    check(index.index==1&&near(index.grip,open.grip)&&near(grip.index,open.index)&&grip.grip==1&&grip.thumb==1);
    const auto invalid=controllerFingerPose(std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity());
    check(near(invalid.index,open.index)&&near(invalid.grip,open.grip));
    const auto clipped=controllerFingerPose(2,-1);check(clipped.index==1&&near(clipped.grip,open.grip));
    for(const auto fingers:{open,index,grip,FingerPose{1,1,1}}){
        const auto locked=handDrawPush(identity(),identity(),fingers,false);
        for(float value:locked.openness)check(value==0); // Gun pose must never animate.
    }
    const auto openPush=handDrawPush(identity(),identity(),FingerPose{},true);
    check(openPush.openness[0]==1&&openPush.openness[1]==1&&openPush.openness[2]==1);
    const auto model=multiply(translation(1,2,3),rotationDegrees(std::array<float,3>{35,20,70}.data()));
    const auto packed=handDrawPush(model,identity());const auto transformed=point(model,{.2f,.3f,.4f});
    for(int row=0;row<3;++row)check(near(transformed[row],packed.modelRows[row][0]*.2f+packed.modelRows[row][1]*.3f+packed.modelRows[row][2]*.4f+packed.modelRows[row][3]));

    std::array<FingerMorphHeader,2> header{};
    const std::filesystem::path assets(argv[2]);
    for(int hand=0;hand<2;++hand){
        const std::string prefix=hand==0?"DOOM_LEFT_HAND_":"DOOM_RIGHT_HAND_";
        const auto fist=bytes(assets/(prefix+"FIST.glb")),gun=bytes(assets/(prefix+"GUN.glb"));
        auto morphs=bytes(assets/(prefix+"FINGERS.bin"));
        std::uint32_t vertices{};std::memcpy(&vertices,morphs.data()+8,4);
        const auto hash=handAssetHash(fist.data(),fist.size());
        check(readFingerMorphHeader(morphs.data(),morphs.size(),vertices,hash,header[hand]));
        check(header[hand].gunHash==handAssetHash(gun.data(),gun.size()));
        Mat4 inverse{};check(inverseAffine(header[hand].gunWrist,inverse));
        const auto product=multiply(inverse,header[hand].gunWrist);
        for(int i=0;i<16;++i)check(near(product.m[i],i%5==0?1.f:0.f));
        check(!readFingerMorphHeader(morphs.data(),morphs.size(),vertices,hash+1,header[hand]));
        check(!readFingerMorphHeader(morphs.data(),morphs.size()-1,vertices,hash,header[hand]));
        check(!readFingerMorphHeader(morphs.data(),morphs.size(),vertices+1,hash,header[hand]));
    }

    std::ifstream profiles(argv[1]);check(bool(profiles));int version{},count{};
    std::string name,mode;float x{},y{},z{},radius{},weight{};
    while(profiles>>version>>name>>mode>>x>>y>>z>>radius>>weight){
        check(version==2);
        for(bool leftHanded:{false,true})for(int rotation=0;rotation<4;++rotation){
            const int supportHand=leftHanded?1:0;
            const float h=.7071067811865475f;
            const std::array<std::array<float,4>,4> rotations{{{0,0,0,1},{0,h,0,h},{h,0,0,h},{0,0,h,h}}};
            HandPose weapon{{.2f,1.1f,-1.5f},{0,0,0,1},true};
            std::copy(rotations[rotation].begin(),rotations[rotation].end(),weapon.orientation);
            const auto anchor=supportAnchorPose(weapon,{leftHanded?-x:x,y,z},{0,0,0,1});check(anchor.valid);
            const auto target=point(poseMatrix(weapon),{leftHanded?-x:x,y,z});
            HandCalibration calibration{{.35f,-.2f,.4f},{155,-105,25},.2464f};
            const auto attached=attachedHandMatrix(anchor,calibration,false,
                header[supportHand].fistWrist,header[supportHand].gunWrist,header[supportHand].graspCentre);
            const auto contact=point(attached,header[supportHand].graspCentre);
            for(int component=0;component<3;++component)check(near(contact[component],target[component]));
            weapon.position[0]+=.25f;weapon.position[1]-=.1f;weapon.position[2]+=.3f;
            const auto moved=supportAnchorPose(weapon,{leftHanded?-x:x,y,z},{0,0,0,1});
            const auto after=attachedHandMatrix(moved,calibration,false,header[supportHand].fistWrist,
                header[supportHand].gunWrist,header[supportHand].graspCentre);
            check(near(after.m[12]-attached.m[12],.25f)&&near(after.m[13]-attached.m[13],-.1f)
                &&near(after.m[14]-attached.m[14],.3f));
            for(int i=0;i<12;++i)check(near(attached.m[i],after.m[i]));
        }
        ++count;
    }
    check(count==9);
    check(!supportAnchorPose({}, {0,0,0},{0,0,0,1}).valid);
    HandPose tracked{{},{0,0,0,1},true};
    check(!supportAnchorPose(tracked,{std::numeric_limits<float>::infinity(),0,0},{0,0,0,1}).valid);
    std::cout<<"Nine support profiles: fixed grasp centres, handedness, weapon rotations and frozen grip pose; finger inputs and 128-byte shader ABI passed\n";
}

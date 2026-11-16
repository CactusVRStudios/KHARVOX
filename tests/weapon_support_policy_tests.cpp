#include "../src/hands/WeaponSupportPolicy.h"
#include <cstdlib>
#include <limits>

using namespace kharvox::hands;
using namespace kharvox::hands::math;
namespace {
void check(bool value){if(!value)std::abort();}
bool near(float a,float b){return std::abs(a-b)<.0001f;}
std::array<float,3> point(const Mat4& matrix,const std::array<float,3>& p) {
    std::array<float,3> result{};
    for(int row=0;row<3;++row)result[row]=matrix.m[12+row]
        +matrix.m[row]*p[0]+matrix.m[4+row]*p[1]+matrix.m[8+row]*p[2];
    return result;
}
void expectPoint(const Mat4& matrix,const std::array<float,3>& p,
    const std::array<float,3>& expected) {
    const auto actual=point(matrix,p);
    for(int i=0;i<3;++i)check(near(actual[i],expected[i]));
}
WeaponSupportFrame initialFrame() {
    WeaponSupportFrame result;
    result.axis=result.bodyAxis={1,0,0,0,1,0,0,0,1};
    result.tick=100;result.epoch=4;result.level=8;result.present=12;result.weapon=3;
    return result;
}
void rejected(const WeaponSupportFrame& frame,const Mat4& tracking,float units) {
    Mat4 output=translation(42,43,44);
    const auto before=output;
    check(!weaponSupportMatrix(frame,tracking,units,output));
    for(int i=0;i<16;++i)check(output.m[i]==before.m[i]);
}
}

int main() {
    {
        // Regression fixture: delivered heavy-rifle authored FIST frame and
        // existing global Touch calibration. The bind pivot is NOT the grip.
        Mat4 mapping{{-9.65984454511f,-.35918134997f,-.814718673874f,0.f,
            .442875976582f,-9.63878225075f,-1.00158762806f,0.f,
            -.772427206508f,-1.0345562838f,9.61449096048f,0.f,
            8.65068335545f,-2.19059131353f,-3.41011932286f,1.f}};
        const HandCalibration calibration{{-.005f,-.04f,0.f},{155.f,-105.f,25.f},.2464f};
        const std::array<float,3> anchor{-6.08008241653f,0.f,-3.42441439629f};
        constexpr float units=.01955799254745f,assetUnits=.025399986284f;
        const auto model=scale(units);std::array<float,3> grip{};
        check(authoredSupportGripPoint(model,anchor,assetUnits,mapping,calibration,false,grip));
        check(near(grip[0],.25861427222f)&&near(grip[1],-.01646060601f)&&near(grip[2],-.05774815337f));
        const auto wrong=point(model,anchor);
        float gap{};for(int i=0;i<3;++i)gap+=(grip[i]-wrong[i])*(grip[i]-wrong[i]);
        check(std::sqrt(gap)>.37f);
        check(!weaponSupportWithinCapture(true,std::sqrt(gap),0,.2f));
        check(weaponSupportWithinCapture(true,0,0,.2f));
        // Replay archive frame 7. With the pivot target the local aim vector
        // points backwards; the authored grip produces the forward hand line.
        WeaponSupportFrame frame=initialFrame();
        frame.origin={1530.15f,4113.29f,1205.19f};frame.bodyOrigin={1539.89f,4116.7f,1241.64f};
        frame.axis={-.57818f,-.302771f,-.40858f,.329076f,-.694418f,.0489129f,-.387708f,-.137887f,.650822f};
        frame.bodyAxis={-.882192f,-.47089f,0.f,.47089f,-.882192f,0.f,0.f,0.f,1.f};
        Mat4 body{};check(weaponSupportMatrix(frame,identity(),39.3701f,body));
        std::array<float,3> authored{};check(authoredSupportGripPoint(body,anchor,assetUnits,mapping,calibration,false,authored));
        const HandPose primary{{.03853254f,-.86510829f,-.25878268f},{-.256267f,.0300153f,-.0449546f,.965093f},true};
        const auto local=inversePose(primary);
        check(point(local,point(body,anchor))[2]>.05f);
        check(point(local,authored)[2]<-.1f);
        auto unchanged=grip;auto invalid=mapping;invalid.m[12]=std::numeric_limits<float>::quiet_NaN();
        check(!authoredSupportGripPoint(model,anchor,assetUnits,invalid,calibration,false,unchanged));
        check(unchanged==grip);
    }
    {
        const auto weapon=multiply(translation(1,2,3),scale(.02f));
        const auto bind=translation(11.3106f,0,0);Mat4 inverse{},out{};
        check(inverseSupportSimilarity(bind,inverse));
        check(weaponJointModel(weapon,bind,inverse,out));
        for(int i=0;i<16;++i)check(near(out.m[i],weapon.m[i]));
        auto live=bind;live.m[12]+=3;
        check(weaponJointModel(weapon,live,inverse,out));
        check(near(out.m[12],weapon.m[12]+.06f));
        const auto turn=multiply(translation(11.3106f,0,0),rotationDegrees(std::array<float,3>{0,0,90}.data()));
        check(weaponJointModel(weapon,turn,inverse,out));
        expectPoint(out,{12.3106f,0,0},{1.f+.02f*11.3106f,2.02f,3.f});
    }
    for(bool physicalLeft:{false,true})for(bool leftHanded:{false,true})
        for(bool gun:{false,true})for(bool acquired:{false,true}){
            check(weaponPrimaryEligible(physicalLeft,leftHanded,gun,acquired)
                ==(!physicalLeft&&!leftHanded&&gun&&acquired));
            check(weaponSupportEligible(physicalLeft,leftHanded,gun,acquired)
                ==(physicalLeft!=leftHanded&&gun&&acquired));
            check(!(weaponPrimaryEligible(physicalLeft,leftHanded,gun,acquired)
                &&weaponSupportEligible(physicalLeft,leftHanded,gun,acquired)));
            for(bool oneHandedWeapon:{false,true}){
                check(weaponPrimaryEligible(physicalLeft,leftHanded,gun,acquired,oneHandedWeapon)
                    ==(!physicalLeft&&!leftHanded&&gun&&(acquired||oneHandedWeapon)));
                check(weaponPoseFrameRequired(leftHanded,acquired,oneHandedWeapon)
                    ==(acquired||(!leftHanded&&oneHandedWeapon)));
            }
        }
    {
        SupportHudGeometry geometry;geometry.valid=true;
        geometry.metersPerAssetUnit=.02f;geometry.anchor={22,-2,3};
        geometry.gripToAsset=multiply(translation(2,3,4),scale(50));
        for(float weaponScale:{.5f,1.f,2.f}){
            const auto weapon=multiply(translation(100,200,300),scale(weaponScale));
            float origin[3]{},axis[9]{};
            check(supportHudFrame(weapon,geometry,50,origin,axis));
            Mat4 fixed{};check(fixedSizeSupportMatrix(weapon,geometry.anchor,1,fixed));
            const auto expected=multiply(fixed,geometry.gripToAsset);
            for(int i=0;i<3;++i)check(near(origin[i],expected.m[12+i]));
            for(int i=0;i<9;++i)check(near(axis[i],i%4==0?1.f:0.f));
        }
        auto matrix=multiply(translation(1,2,3),scaleAxes(-.25f,.25f,.25f));
        Mat4 inverse{};check(inverseSupportSimilarity(matrix,inverse));
        const auto product=multiply(matrix,inverse);
        for(int i=0;i<16;++i)check(near(product.m[i],i%5==0?1.f:0.f));
        matrix.m[4]=.1f;check(!inverseSupportSimilarity(matrix,inverse));
        float origin[3]{42,43,44},axis[9]{};geometry.gripToAsset=scale(25);
        check(!supportHudFrame(identity(),geometry,50,origin,axis)&&origin[0]==42&&axis[0]==0);
    }
    // Regression: Grip must not inherit native weapon scale. Both hands and
    // every source-eye orientation use the same physical glove dimensions.
    for(float x:{.005f,.0254f,.05f})for(float y:{.01f,.03f})for(float sign:{-1.f,1.f}){
        auto weapon=multiply(translation(2,3,4),rotationDegrees(std::array<float,3>{23,47,-19}.data()));
        weapon=multiply(weapon,scaleAxes(x*sign,y,.04f));
        for(float physical:{.012f,.0254f,.03f}){
            const std::array<float,3> anchor{22,-3,4};
            Mat4 fixed{};check(fixedSizeSupportMatrix(weapon,anchor,physical,fixed));
            expectPoint(fixed,anchor,point(weapon,anchor));
            const std::array<float,3> a{1,3,-2},b{-4,7,6};
            const auto pa=point(fixed,a),pb=point(fixed,b);
            double before{},after{};
            for(int i=0;i<3;++i){before+=double(a[i]-b[i])*(a[i]-b[i]);after+=double(pa[i]-pb[i])*(pa[i]-pb[i]);}
            check(near(static_cast<float>(std::sqrt(after)),static_cast<float>(std::sqrt(before))*physical));
        }
    }
    {
        auto weapon=identity(),sentinel=translation(42,43,44);
        for(int invalid=0;invalid<5;++invalid){
            weapon=identity();float physical=.02f;std::array<float,3> anchor{};
            if(invalid==0)weapon.m[0]=0;
            if(invalid==1)weapon.m[4]=.2f;
            if(invalid==2)weapon.m[3]=1;
            if(invalid==3)physical=-1;
            if(invalid==4)anchor[0]=std::numeric_limits<float>::quiet_NaN();
            auto result=sentinel;check(!fixedSizeSupportMatrix(weapon,anchor,physical,result));
            for(int i=0;i<16;++i)check(result.m[i]==sentinel.m[i]);
        }
    }
    auto frame=initialFrame();Mat4 output{};
    check(supportHudSourceMatches(frame,12,8,3,frame.bodyOrigin.data(),frame.bodyAxis.data()));
    check(!supportHudSourceMatches(frame,13,8,3,frame.bodyOrigin.data(),frame.bodyAxis.data()));
    check(!supportHudSourceMatches(frame,12,9,3,frame.bodyOrigin.data(),frame.bodyAxis.data()));
    check(!supportHudSourceMatches(frame,12,8,6,frame.bodyOrigin.data(),frame.bodyAxis.data()));
    const float differentBody[3]{0,.1f,0};
    check(!supportHudSourceMatches(frame,12,8,3,differentBody,frame.bodyAxis.data()));
    check(weaponSupportMatrix(frame,identity(),10,output));
    expectPoint(output,{10,20,30},{-2,3,-1});
    frame.origin={110,220,330};frame.bodyOrigin={100,200,300};
    const auto tracking=translation(1,2,3);
    check(weaponSupportMatrix(frame,tracking,10,output));
    expectPoint(output,{0,0,0},{-1,5,2});
    expectPoint(output,{10,20,30},{-3,8,1});

    // A rotated native body and weapon plus a rotated XR tracking origin must
    // retain each weapon scale; applying only a controller pose loses these.
    frame.origin={110,200,300};
    frame.bodyAxis={0,1,0,-1,0,0,0,0,1};
    frame.axis={0,2,0,-3,0,0,0,0,4};
    constexpr float halfSqrt=.7071067811865475f;
    const HandPose tracked{{1,2,3},{0,halfSqrt,0,halfSqrt},true};
    check(weaponSupportMatrix(frame,poseMatrix(tracked),10,output));
    expectPoint(output,{0,0,0},{1,2,2});
    expectPoint(output,{2,3,4},{.6f,3.6f,2.9f});
    for(int column=0;column<3;++column){
        float squared{};for(int row=0;row<3;++row)squared+=output.m[column*4+row]*output.m[column*4+row];
        check(near(std::sqrt(squared),.2f+.1f*column));
    }
    const auto unreflected=output;
    for(int i=0;i<3;++i)frame.axis[i]=-frame.axis[i];
    check(weaponSupportMatrix(frame,poseMatrix(tracked),10,output));
    expectPoint(output,{2,3,4},{1.4f,3.6f,2.9f});
    // The right-hand asset is baked in its own mirrored weapon coordinates.
    // The runtime matrix preserves reflection rather than adding a second one.
    const auto originalPoint=point(unreflected,{2,3,4});
    expectPoint(output,{-2,3,4},originalPoint);

    for(unsigned weapon=1;weapon<=9;++weapon)for(bool leftHanded:{false,true}){
        check(weaponSupportProfileIndex(weapon,leftHanded)==weapon*2+(leftHanded?1:0));
        check(weaponSupportProfileIndex(weapon,leftHanded)!=weaponSupportProfileIndex(weapon,!leftHanded));
        check(weaponSupportEligible(!leftHanded,leftHanded,true,true));
        check(!weaponSupportEligible(leftHanded,leftHanded,true,true));
        check(!weaponSupportEligible(!leftHanded,leftHanded,false,true));
        check(!weaponSupportEligible(!leftHanded,leftHanded,true,false)); // grip released/gameplay ended
    }
    frame=initialFrame();
    check(weaponSupportFrameCurrent(frame,100,4,8,3));
    check(weaponSupportFrameCurrent(frame,350,4,8,3));
    check(!weaponSupportFrameCurrent(frame,351,4,8,3));
    check(!weaponSupportFrameCurrent(frame,99,4,8,3));
    check(!weaponSupportFrameCurrent(frame,100,5,8,3));
    check(!weaponSupportFrameCurrent(frame,100,4,9,3));
    check(!weaponSupportFrameCurrent(frame,100,4,8,2)); // weapon changed
    frame.tick=0;check(!weaponSupportFrameCurrent(frame,0,4,8,3));
    check(weaponSupportWithinCapture(true,.1f,9.f,.2f));
    check(!weaponSupportWithinCapture(true,.3f,.1f,.2f)); // old wrong grab zone must not override native geometry
    check(weaponSupportWithinCapture(false,9.f,.1f,.2f));
    check(!weaponSupportWithinCapture(false,.1f,.3f,.2f));
    check(!weaponSupportWithinCapture(true,-.1f,.1f,.2f));
    check(!weaponSupportWithinCapture(true,.1f,.1f,0));

    frame=initialFrame();
    rejected(frame,identity(),0);rejected(frame,identity(),-1);
    const auto nan=std::numeric_limits<float>::quiet_NaN();
    check(!weaponSupportWithinCapture(true,nan,.1f,.2f));
    check(!weaponSupportWithinCapture(false,.1f,nan,.2f));
    rejected(frame,identity(),nan);
    auto invalid=frame;invalid.origin[0]=nan;rejected(invalid,identity(),10);
    invalid=frame;invalid.bodyOrigin[1]=nan;rejected(invalid,identity(),10);
    invalid=frame;invalid.axis[2]=nan;rejected(invalid,identity(),10);
    invalid=frame;invalid.axis={};rejected(invalid,identity(),10);
    invalid=frame;invalid.axis={1,0,0,2,0,0,0,0,1};rejected(invalid,identity(),10);
    invalid=frame;invalid.bodyAxis[0]=2;rejected(invalid,identity(),10);
    invalid=frame;invalid.bodyAxis[0]=-1;rejected(invalid,identity(),10);
    invalid=frame;invalid.bodyAxis[3]=.2f;rejected(invalid,identity(),10);
    auto invalidTracking=identity();invalidTracking.m[12]=nan;rejected(frame,invalidTracking,10);
    invalidTracking=identity();invalidTracking.m[3]=1;rejected(frame,invalidTracking,10);
    invalidTracking=identity();invalidTracking.m[0]=0;rejected(frame,invalidTracking,10);
}

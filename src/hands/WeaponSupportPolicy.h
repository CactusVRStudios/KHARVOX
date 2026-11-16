#pragma once
#include "HandPoseMath.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace kharvox::hands {

struct WeaponSupportFrame {
    std::array<float,3> origin{},bodyOrigin{};
    // Consecutive vectors are local X/Y/Z in DOOM world coordinates. Weapon
    // axes retain their native scale and possible reflection; body axes do not.
    std::array<float,9> axis{},bodyAxis{};
    std::uint64_t tick{},epoch{},level{},present{};
    unsigned weapon{};
    std::array<std::array<float,3>,2> jointOrigin{}; // shotgun: pump, base
    std::array<std::array<float,9>,2> jointAxis{};
    unsigned jointMask{};
};

struct SupportHudGeometry {
    bool valid{};
    float metersPerAssetUnit{};
    std::array<float,3> anchor{},minimumCm{},maximumCm{};
    std::array<std::array<float,6>,8> hudSections{};
    unsigned hudSectionMask{};
    // Raw DOOM Grip-local metres -> baked support asset units.
    math::Mat4 gripToAsset{};
    int jointSlot{-1};
    math::Mat4 jointBindInverse=math::identity();
};

inline std::size_t weaponSupportProfileIndex(unsigned weapon,bool leftHanded) noexcept {
    return static_cast<std::size_t>(weapon)*2+(leftHanded?1:0);
}
inline bool weaponSupportEligible(bool physicalLeft,bool leftHanded,
    bool gunHolding,bool acquired) noexcept {
    return physicalLeft!=leftHanded&&gunHolding&&acquired;
}
// The initial manually authored primary pose is physical-right only. Keep
// the calibrated left-handed path until a physical-left primary is authored.
inline bool weaponPoseFrameRequired(bool leftHanded,bool acquired,
    bool oneHandedWeapon=false) noexcept {
    return acquired||(!leftHanded&&oneHandedWeapon);
}
inline bool weaponPrimaryEligible(bool physicalLeft,bool leftHanded,
    bool gunHolding,bool acquired,bool oneHandedWeapon=false) noexcept {
    return !physicalLeft&&!leftHanded&&gunHolding
        &&weaponPoseFrameRequired(leftHanded,acquired,oneHandedWeapon);
}
inline bool weaponSupportWithinCapture(bool nativePoint,float nativeDistance,
    float previousDistance,float radius) noexcept {
    const float distance=nativePoint?nativeDistance:previousDistance;
    return std::isfinite(radius)&&radius>0&&std::isfinite(distance)
        &&distance>=0&&distance<=radius;
}
inline bool weaponSupportFrameCurrent(const WeaponSupportFrame& frame,
    std::uint64_t nowTick,std::uint64_t epoch,std::uint64_t level,unsigned weapon) noexcept {
    return frame.tick&&nowTick>=frame.tick&&nowTick-frame.tick<=250
        &&frame.epoch==epoch&&frame.level==level&&frame.weapon==weapon;
}

namespace support_detail {
inline double determinant(const float* a) noexcept {
    return double(a[0])*(double(a[4])*a[8]-double(a[5])*a[7])
        -double(a[1])*(double(a[3])*a[8]-double(a[5])*a[6])
        +double(a[2])*(double(a[3])*a[7]-double(a[4])*a[6]);
}
inline bool finite(const float* values,std::size_t count) noexcept {
    for(std::size_t i=0;i<count;++i)if(!std::isfinite(values[i]))return false;
    return true;
}
inline bool rigidBasis(const float* basis) noexcept {
    if(!finite(basis,9))return false;
    for(int row=0;row<3;++row)for(int other=row;other<3;++other){
        double dot{};
        for(int i=0;i<3;++i)dot+=double(basis[row*3+i])*basis[other*3+i];
        if(std::abs(dot-(row==other?1.0:0.0))>.01)return false;
    }
    return determinant(basis)>.99;
}
inline bool nonsingular(const float* basis) noexcept {
    if(!finite(basis,9))return false;
    double squaredProduct=1;
    for(int row=0;row<3;++row){
        double squared{};
        for(int i=0;i<3;++i)squared+=double(basis[row*3+i])*basis[row*3+i];
        squaredProduct*=squared;
    }
    return squaredProduct>0
        &&std::abs(determinant(basis))>1e-6*std::sqrt(squaredProduct);
}
} // namespace support_detail

inline bool inverseSupportSimilarity(const math::Mat4& matrix,math::Mat4& out) noexcept {
    if(!support_detail::finite(matrix.m,16)||std::abs(matrix.m[3])>1e-6f
        ||std::abs(matrix.m[7])>1e-6f||std::abs(matrix.m[11])>1e-6f
        ||std::abs(matrix.m[15]-1.f)>1e-6f)return false;
    double squared{};for(int row=0;row<3;++row)squared+=double(matrix.m[row])*matrix.m[row];
    if(squared<1e-16)return false;
    for(int a=0;a<3;++a)for(int b=a;b<3;++b){
        double dot{};for(int row=0;row<3;++row)dot+=double(matrix.m[a*4+row])*matrix.m[b*4+row];
        if(std::abs(dot-(a==b?squared:0))>squared*.001)return false;
    }
    auto result=math::identity();
    for(int column=0;column<3;++column)for(int row=0;row<3;++row)
        result.m[column*4+row]=static_cast<float>(matrix.m[row*4+column]/squared);
    for(int row=0;row<3;++row)for(int k=0;k<3;++k)
        result.m[12+row]-=result.m[k*4+row]*matrix.m[12+k];
    if(!support_detail::finite(result.m,16))return false;
    out=result;return true;
}

inline bool weaponJointModel(const math::Mat4& weapon,const math::Mat4& joint,
    const math::Mat4& bindInverse,math::Mat4& out) noexcept {
    math::Mat4 checked{};
    if(!inverseSupportSimilarity(joint,checked)
        ||!inverseSupportSimilarity(bindInverse,checked))return false;
    const auto result=math::multiply(weapon,math::multiply(joint,bindInverse));
    if(!support_detail::finite(result.m,16))return false;
    out=result;return true;
}

inline bool supportHudSourceMatches(const WeaponSupportFrame& frame,std::uint64_t present,
    std::uint64_t level,unsigned weapon,const float* bodyOrigin,const float* bodyAxis) noexcept {
    if(frame.present!=present||frame.level!=level||frame.weapon!=weapon
        ||!support_detail::finite(bodyOrigin,3)||!support_detail::finite(bodyAxis,9))return false;
    for(int i=0;i<3;++i)if(!std::isfinite(frame.bodyOrigin[i])
        ||std::abs(frame.bodyOrigin[i]-bodyOrigin[i])>=.05f)return false;
    for(int i=0;i<9;++i)if(!std::isfinite(frame.bodyAxis[i])
        ||std::abs(frame.bodyAxis[i]-bodyAxis[i])>=.001f)return false;
    return true;
}

// Keep a baked support glove at its calibrated physical size. The native
// anchor still follows the complete weapon affine transform. Normalizing
// around the origin instead would detach the palm when weapon scale changes.
// Reflections are preserved; shear/perspective fail without changing out.
inline bool fixedSizeSupportMatrix(const math::Mat4& weapon,
    const std::array<float,3>& anchor,float metersPerAssetUnit,math::Mat4& out) noexcept {
    if(!support_detail::finite(weapon.m,16)||!support_detail::finite(anchor.data(),3)
        ||!std::isfinite(metersPerAssetUnit)||metersPerAssetUnit<=0
        ||std::abs(weapon.m[3])>1e-6f||std::abs(weapon.m[7])>1e-6f
        ||std::abs(weapon.m[11])>1e-6f||std::abs(weapon.m[15]-1.f)>1e-6f)return false;
    auto result=math::identity();
    for(int column=0;column<3;++column){
        double squared{};
        for(int row=0;row<3;++row)squared+=double(weapon.m[column*4+row])*weapon.m[column*4+row];
        if(squared<1e-16)return false;
        const double length=std::sqrt(squared);
        for(int row=0;row<3;++row)result.m[column*4+row]=static_cast<float>(weapon.m[column*4+row]/length);
    }
    for(int a=0;a<3;++a)for(int b=a+1;b<3;++b){
        double dot{};for(int row=0;row<3;++row)dot+=double(result.m[a*4+row])*result.m[b*4+row];
        if(std::abs(dot)>.001)return false;
    }
    for(int column=0;column<3;++column)for(int row=0;row<3;++row)
        result.m[column*4+row]*=metersPerAssetUnit;
    for(int row=0;row<3;++row){
        double origin=weapon.m[12+row];
        for(int column=0;column<3;++column)
            origin+=(double(weapon.m[column*4+row])-result.m[column*4+row])*anchor[column];
        result.m[12+row]=static_cast<float>(origin);
    }
    if(!support_detail::finite(result.m,16))return false;
    out=result;return true;
}

// The native bind anchor preserves rendered hand size. It is not necessarily
// where a tracked controller belongs inside the authored hand. Recover that
// grip origin through the existing FIST-to-authored mapping and calibration.
inline bool authoredSupportGripPoint(const math::Mat4& jointModel,
    const std::array<float,3>& anchor,float metersPerAssetUnit,
    const math::Mat4& fistToAsset,const HandCalibration& calibration,
    bool mirrorX,std::array<float,3>& output) noexcept {
    HandPose origin{};origin.valid=true;
    math::Mat4 inverseCalibration{},fixed{};
    if(!inverseSupportSimilarity(math::calibratedHandMatrix(origin,calibration,mirrorX),inverseCalibration)
        ||!support_detail::finite(fistToAsset.m,16)
        ||!fixedSizeSupportMatrix(jointModel,anchor,metersPerAssetUnit,fixed))return false;
    const auto gripToAsset=math::multiply(fistToAsset,inverseCalibration);
    std::array<float,3> point{};
    for(int row=0;row<3;++row){
        point[row]=fixed.m[12+row];
        for(int column=0;column<3;++column)
            point[row]+=fixed.m[column*4+row]*gripToAsset.m[12+column];
        if(!std::isfinite(point[row]))return false;
    }
    output=point;return true;
}

// Native world coordinates, before any eye transform. Thus the HUD uses the
// same support wrist and armor for both eyes rather than following a nearby
// raw controller independently from the visible locked glove.
inline bool supportHudFrame(const math::Mat4& weapon,const SupportHudGeometry& geometry,
    float unitsPerMeter,float* origin,float* axis) noexcept {
    math::Mat4 model{};
    if(!geometry.valid||!std::isfinite(unitsPerMeter)||unitsPerMeter<=0
        ||!fixedSizeSupportMatrix(weapon,geometry.anchor,
            geometry.metersPerAssetUnit*unitsPerMeter,model))return false;
    const auto grip=math::multiply(model,geometry.gripToAsset);
    if(!support_detail::finite(grip.m,16))return false;
    float basis[9]{};
    for(int column=0;column<3;++column){
        float squared{};for(int row=0;row<3;++row)squared+=grip.m[column*4+row]*grip.m[column*4+row];
        const float length=std::sqrt(squared);
        if(!std::isfinite(length)||std::abs(length/unitsPerMeter-1.f)>.003f)return false;
        for(int row=0;row<3;++row)basis[column*3+row]=grip.m[column*4+row]/length;
    }
    if(!support_detail::rigidBasis(basis))return false;
    for(int i=0;i<3;++i)origin[i]=grip.m[12+i];
    for(int i=0;i<9;++i)axis[i]=basis[i];
    return true;
}

// Exactly the attachment path used for the rendered authored glove. Using
// the bare weapon matrix loses the animated joint and its inverse bind.
inline bool supportHudFrame(const WeaponSupportFrame& frame,const SupportHudGeometry& geometry,
    float unitsPerMeter,float* origin,float* axis) noexcept {
    if(!geometry.valid)return false;
    auto model=math::identity();
    for(int column=0;column<3;++column)for(int row=0;row<3;++row)
        model.m[column*4+row]=frame.axis[column*3+row];
    for(int i=0;i<3;++i)model.m[12+i]=frame.origin[i];
    if(geometry.jointSlot>=0){
        const auto slot=static_cast<unsigned>(geometry.jointSlot);
        if(slot>=2||!(frame.jointMask&(1u<<slot)))return false;
        auto joint=math::identity();
        for(int column=0;column<3;++column)for(int row=0;row<3;++row)
            joint.m[column*4+row]=frame.jointAxis[slot][column*3+row];
        for(int i=0;i<3;++i)joint.m[12+i]=frame.jointOrigin[slot][i];
        if(!weaponJointModel(model,joint,geometry.jointBindInverse,model))return false;
    }
    return supportHudFrame(model,geometry,unitsPerMeter,origin,axis);
}

// Assets are already baked in native weapon-local DOOM coordinates. This is
// their complete affine transform into XR LOCAL, shared by both physical hands.
// Failure leaves out unchanged; callers retain their existing hand fallback.
inline bool weaponSupportMatrix(const WeaponSupportFrame& frame,
    const math::Mat4& bodyTracking,float unitsPerMeter,math::Mat4& out) noexcept {
    if(!std::isfinite(unitsPerMeter)||unitsPerMeter<=0
        ||!support_detail::finite(frame.origin.data(),3)
        ||!support_detail::finite(frame.bodyOrigin.data(),3)
        ||!support_detail::nonsingular(frame.axis.data())
        ||!support_detail::rigidBasis(frame.bodyAxis.data())
        ||!support_detail::finite(bodyTracking.m,16))return false;
    const float trackingBasis[]{bodyTracking.m[0],bodyTracking.m[1],bodyTracking.m[2],
        bodyTracking.m[4],bodyTracking.m[5],bodyTracking.m[6],
        bodyTracking.m[8],bodyTracking.m[9],bodyTracking.m[10]};
    if(!support_detail::rigidBasis(trackingBasis)
        ||std::abs(bodyTracking.m[3])>1e-6f||std::abs(bodyTracking.m[7])>1e-6f
        ||std::abs(bodyTracking.m[11])>1e-6f||std::abs(bodyTracking.m[15]-1.f)>1e-6f)return false;
    auto local=math::identity();
    for(int column=0;column<4;++column){
        double body[3]{};
        for(int row=0;row<3;++row)for(int i=0;i<3;++i){
            const double world=column==3?double(frame.origin[i])-frame.bodyOrigin[i]
                :frame.axis[column*3+i];
            body[row]+=world*frame.bodyAxis[row*3+i]/unitsPerMeter;
        }
        local.m[column*4]=-static_cast<float>(body[1]);
        local.m[column*4+1]=static_cast<float>(body[2]);
        local.m[column*4+2]=-static_cast<float>(body[0]);
    }
    const auto result=math::multiply(bodyTracking,local);
    if(!support_detail::finite(result.m,16))return false;
    out=result;return true;
}

} // namespace kharvox::hands

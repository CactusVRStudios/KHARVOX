#pragma once

#include "HandPoseMath.h"
#include <array>
#include <cstddef>
#include <cstring>

namespace kharvox::hands {

// Physical hands, independent of which controller owns the weapon actions.
struct FingerPose { float index{},grip{},thumb{}; };
struct HandInteractionState {
    HandPose support{}; // Grasp centre and captured wrist orientation, XR LOCAL.
    std::array<FingerPose,2> fingers{};
    bool grenadeHeld{};
    // Completed native weapon model transform, XR LOCAL. Reviewed support
    // meshes are already baked in weapon-local coordinates, not wrist space.
    math::Mat4 supportModel{};
    bool supportModelValid{};
};

inline FingerPose controllerFingerPose(float trigger,float squeeze) {
    auto unit=[](float v){return std::isfinite(v)?std::clamp(v,0.f,1.f):0.f;};
    // The mesh endpoint is now an authored relaxed pose, with individually
    // bent phalanges. Zero input selects that pose; full input closes the fist.
    const float index=unit(trigger),grip=unit(squeeze);
    // Touch controllers report trigger/squeeze, not five independent finger
    // angles. Thumb closure follows the grip; never claim optical tracking.
    return {index,grip,grip};
}

inline HandPose supportAnchorPose(const HandPose&weapon,
    const std::array<float,3>&offset,const std::array<float,4>&relativeWrist) {
    if(!weapon.valid)return {};
    const auto model=math::poseMatrix(weapon);HandPose result{};
    for(int row=0;row<3;++row)result.position[row]=model.m[12+row]
        +model.m[row]*offset[0]+model.m[4+row]*offset[1]+model.m[8+row]*offset[2];
    const auto&a=weapon.orientation;const auto&b=relativeWrist;
    result.orientation[0]=a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1];
    result.orientation[1]=a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0];
    result.orientation[2]=a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3];
    result.orientation[3]=a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2];
    float squared{};for(float value:result.orientation)squared+=value*value;
    if(!std::isfinite(squared)||squared<1e-8f)return {};
    for(float&value:result.orientation)value/=std::sqrt(squared);
    for(float value:result.position)if(!std::isfinite(value))return {};
    result.valid=true;return result;
}

inline bool inverseAffine(const math::Mat4& value,math::Mat4& result) {
    const auto&m=value.m;
    const float det=m[0]*(m[5]*m[10]-m[9]*m[6])
        -m[4]*(m[1]*m[10]-m[9]*m[2])+m[8]*(m[1]*m[6]-m[5]*m[2]);
    if(!std::isfinite(det)||std::abs(det)<1e-8f)return false;
    result=math::identity();auto&r=result.m;const float d=1.f/det;
    r[0]=(m[5]*m[10]-m[9]*m[6])*d;r[4]=(m[8]*m[6]-m[4]*m[10])*d;r[8]=(m[4]*m[9]-m[8]*m[5])*d;
    r[1]=(m[9]*m[2]-m[1]*m[10])*d;r[5]=(m[0]*m[10]-m[8]*m[2])*d;r[9]=(m[8]*m[1]-m[0]*m[9])*d;
    r[2]=(m[1]*m[6]-m[5]*m[2])*d;r[6]=(m[4]*m[2]-m[0]*m[6])*d;r[10]=(m[0]*m[5]-m[4]*m[1])*d;
    for(int row=0;row<3;++row)r[12+row]=-(r[row]*m[12]+r[4+row]*m[13]+r[8+row]*m[14]);
    return true;
}

// Transfer the accepted free-hand wrist frame to the authored gun-pose wrist,
// then put the actual grasp centre on the weapon's calibrated support point.
// The static GLBs have different export origins; their origins are not grips.
inline math::Mat4 attachedHandMatrix(const HandPose& anchor,
    const HandCalibration& calibration,bool mirrorX,
    const math::Mat4& fistWrist,const math::Mat4& gunWrist,
    const std::array<float,3>& graspCentre) {
    math::Mat4 inverseGun{};
    if(!inverseAffine(gunWrist,inverseGun))return math::calibratedHandMatrix(anchor,calibration,mirrorX);
    const auto shape=math::multiply(math::rotationDegrees(calibration.rotationDegrees),
        math::multiply(math::scaleAxes(mirrorX?-calibration.scale:calibration.scale,
            calibration.scale,calibration.scale),math::multiply(fistWrist,inverseGun)));
    auto result=math::multiply(math::poseMatrix(anchor),shape);
    for(int row=0;row<3;++row)
        result.m[12+row]=anchor.position[row]-(result.m[row]*graspCentre[0]
            +result.m[4+row]*graspCentre[1]+result.m[8+row]*graspCentre[2]);
    return result;
}

inline std::uint64_t handAssetHash(const std::uint8_t*data,std::size_t size) {
    std::uint64_t hash=14695981039346656037ull;
    for(std::size_t i=0;i<size;++i)hash=(hash^data[i])*1099511628211ull;
    return hash;
}
struct FingerMorphHeader {
    std::uint64_t gunHash{};
    math::Mat4 fistWrist{},gunWrist{};
    std::array<float,3> graspCentre{};
    std::array<float,3> fistGraspCentre{};
    std::array<std::array<float,3>,3> graspDeltas{};
};
inline constexpr std::size_t fingerMorphHeaderBytes=216;
inline bool readFingerMorphHeader(const std::uint8_t*data,std::size_t size,
    std::size_t vertices,std::uint64_t fistHash,FingerMorphHeader&header) {
    if(!data||size<fingerMorphHeaderBytes||vertices==0||vertices>1000000
        ||size!=fingerMorphHeaderBytes+vertices*18*sizeof(float)
        ||std::memcmp(data,"KHFING02",8))return false;
    std::uint32_t count{};std::uint64_t sourceHash{};
    std::memcpy(&count,data+8,4);std::memcpy(&sourceHash,data+12,8);
    if(count!=vertices||sourceHash!=fistHash)return false;
    std::memcpy(&header.gunHash,data+20,8);
    std::memcpy(header.fistWrist.m,data+28,64);std::memcpy(header.gunWrist.m,data+92,64);
    std::memcpy(header.graspCentre.data(),data+156,12);
    std::memcpy(header.fistGraspCentre.data(),data+168,12);
    std::memcpy(header.graspDeltas.data(),data+180,36);
    for(float value:header.fistWrist.m)if(!std::isfinite(value))return false;
    for(float value:header.gunWrist.m)if(!std::isfinite(value))return false;
    for(float value:header.graspCentre)if(!std::isfinite(value))return false;
    for(float value:header.fistGraspCentre)if(!std::isfinite(value))return false;
    for(const auto&delta:header.graspDeltas)for(float value:delta)if(!std::isfinite(value))return false;
    math::Mat4 inverse{};
    return inverseAffine(header.fistWrist,inverse)&&inverseAffine(header.gunWrist,inverse);
}

// Keep the Vulkan-guaranteed 128-byte push limit: three affine rows replace
// a redundant fourth model row, leaving four floats for finger morph weights.
struct HandDrawPush {
    float modelRows[3][4]{};
    math::Mat4 viewProjection{};
    float openness[4]{};
};
static_assert(sizeof(HandDrawPush)==128);
static_assert(offsetof(HandDrawPush,viewProjection)==48&&offsetof(HandDrawPush,openness)==112);
inline HandDrawPush handDrawPush(const math::Mat4&model,
    const math::Mat4&viewProjection,FingerPose fingers={},bool animate=false) {
    HandDrawPush result{};result.viewProjection=viewProjection;
    for(int row=0;row<3;++row)for(int col=0;col<4;++col)
        result.modelRows[row][col]=model.m[col*4+row];
    if(animate){result.openness[0]=1.f-fingers.index;
        result.openness[1]=1.f-fingers.grip;result.openness[2]=1.f-fingers.thumb;}
    return result;
}
} // namespace kharvox::hands

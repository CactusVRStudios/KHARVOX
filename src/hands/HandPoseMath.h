#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace kharvox::hands {

struct HandPose {
    float position[3]{};
    float orientation[4]{0.f, 0.f, 0.f, 1.f};
    bool valid{};
};

struct HandEyeView {
    HandPose pose{};
    float angleLeft{};
    float angleRight{};
    float angleUp{};
    float angleDown{};
    // Match the sub-rectangle submitted to OpenXR in both renderers.
    std::int32_t imageRectX{};
    std::int32_t imageRectY{};
    std::uint32_t imageRectWidth{};
    std::uint32_t imageRectHeight{};
    // Metres; the scene-depth path supplies DOOM's native range.
    float nearZ{0.02f};
    float farZ{100.0f};
};

struct HandCalibration {
    float position[3]{};
    float rotationDegrees[3]{};
    float scale{0.4f};
};

namespace math {
struct Mat4 { float m[16]{}; };

inline Mat4 identity(){Mat4 r{};r.m[0]=r.m[5]=r.m[10]=r.m[15]=1.f;return r;}
inline Mat4 multiply(const Mat4&a,const Mat4&b){Mat4 r{};for(int c=0;c<4;c++)for(int row=0;row<4;row++)for(int k=0;k<4;k++)r.m[c*4+row]+=a.m[k*4+row]*b.m[c*4+k];return r;}
inline Mat4 translation(float x,float y,float z){auto r=identity();r.m[12]=x;r.m[13]=y;r.m[14]=z;return r;}
inline Mat4 scale(float value){auto r=identity();r.m[0]=r.m[5]=r.m[10]=value;return r;}
inline Mat4 scaleAxes(float x,float y,float z){auto r=identity();r.m[0]=x;r.m[5]=y;r.m[10]=z;return r;}
inline Mat4 quaternion(float x,float y,float z,float w){
    Mat4 r=identity();const float xx=x*x,yy=y*y,zz=z*z,xy=x*y,xz=x*z,yz=y*z,wx=w*x,wy=w*y,wz=w*z;
    r.m[0]=1-2*(yy+zz);r.m[1]=2*(xy+wz);r.m[2]=2*(xz-wy);
    r.m[4]=2*(xy-wz);r.m[5]=1-2*(xx+zz);r.m[6]=2*(yz+wx);
    r.m[8]=2*(xz+wy);r.m[9]=2*(yz-wx);r.m[10]=1-2*(xx+yy);return r;
}
inline Mat4 rotationDegrees(const float degrees[3]){
    constexpr float d=0.01745329251994329577f;
    const float hx=degrees[0]*d*.5f,hy=degrees[1]*d*.5f,hz=degrees[2]*d*.5f;
    const float sx=std::sin(hx),cx=std::cos(hx),sy=std::sin(hy),cy=std::cos(hy),sz=std::sin(hz),cz=std::cos(hz);
    return quaternion(sx*cy*cz-cx*sy*sz,cx*sy*cz+sx*cy*sz,cx*cy*sz-sx*sy*cz,cx*cy*cz+sx*sy*sz);
}
inline Mat4 poseMatrix(const HandPose&pose){return multiply(translation(pose.position[0],pose.position[1],pose.position[2]),quaternion(pose.orientation[0],pose.orientation[1],pose.orientation[2],pose.orientation[3]));}
inline Mat4 inversePose(const HandPose&pose){
    const Mat4 inverseRotation=quaternion(-pose.orientation[0],-pose.orientation[1],-pose.orientation[2],pose.orientation[3]);
    return multiply(inverseRotation,translation(-pose.position[0],-pose.position[1],-pose.position[2]));
}
inline Mat4 projection(const HandEyeView&view){
    const float left=std::tan(view.angleLeft),right=std::tan(view.angleRight),down=std::tan(view.angleDown),up=std::tan(view.angleUp);
    const float nearZ=std::clamp(view.nearZ,.001f,10.f);
    const float farZ=std::max(view.farZ,nearZ+1.f);Mat4 r{};
    r.m[0]=2.f/(right-left);r.m[5]=2.f/(down-up);
    // Vulkan's positive viewport uses the OpenXR (down-up) vertical span.
    r.m[8]=(right+left)/(right-left);r.m[9]=(up+down)/(down-up);
    r.m[10]=-farZ/(farZ-nearZ);r.m[11]=-1.f;r.m[14]=-(farZ*nearZ)/(farZ-nearZ);return r;
}
inline Mat4 calibratedHandMatrix(const HandPose&pose,
    const HandCalibration&calibration,bool mirrorX){
    const float xScale=mirrorX?-calibration.scale:calibration.scale;
    const Mat4 local=multiply(translation(calibration.position[0],calibration.position[1],calibration.position[2]),
        multiply(rotationDegrees(calibration.rotationDegrees),scaleAxes(xScale,calibration.scale,calibration.scale)));
    return multiply(poseMatrix(pose),local);
}
} // namespace math
} // namespace kharvox::hands

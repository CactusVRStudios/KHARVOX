#version 450
#extension GL_EXT_multiview : require
layout(set=0,binding=0) uniform sampler2D stereoImage;
layout(set=0,binding=1) uniform sampler2D monoImage;
struct StereoEye {vec4 stereo;vec4 custom;};
layout(set=0,binding=30,std140) uniform Parameters {StereoEye vk3d_params[2];} profile;
void main() {
    float z=7.0;
    float offset=profile.vk3d_params[gl_ViewIndex].stereo.x * (z - profile.vk3d_params[gl_ViewIndex].stereo.y);
    gl_FragDepth=texelFetch(stereoImage,textureSize(stereoImage,0)/2,0).r+texture(monoImage,vec2(0.5)).r+offset;
}

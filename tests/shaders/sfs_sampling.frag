#version 450
layout(set=0,binding=0) uniform sampler2D stereoImage;
layout(set=0,binding=1) uniform sampler2D monoImage;
void main() {
    ivec2 size = textureSize(stereoImage, 0);
    gl_FragDepth = texelFetch(stereoImage, size / 2, 0).r + texture(monoImage, vec2(0.5)).r;
}

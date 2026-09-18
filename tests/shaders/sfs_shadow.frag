#version 450
layout(set=0,binding=0) uniform sampler2DShadow shadowAtlas;
layout(set=0,binding=1) uniform sampler2D monoImage;
void main() {
    gl_FragDepth=texture(shadowAtlas,vec3(.5,.5,.5))*.5+texture(monoImage,vec2(.5)).r;
}

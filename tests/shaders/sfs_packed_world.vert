#version 450
layout(set=0,binding=0,std140) uniform Camera {
    vec4 mvpmatrixw;
    vec4 vertexxyzscale;
} camera;
layout(location=0) in vec4 in_Position;
layout(location=12) in vec2 in_VmtrTC;
layout(location=0) out vec2 materialTc;
void main() {
    materialTc=in_VmtrTC;
    vec4 p=in_Position*camera.vertexxyzscale;
    gl_Position=vec4(p.xyz,dot(p,camera.mvpmatrixw));
}

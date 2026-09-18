#version 450
layout(set=0,binding=2,std140) uniform Camera {vec4 mvpmatrixw;} camera;
layout(location=0) in vec2 in_VmtrTC;
layout(location=0) out float cameraDepth;
void main() {
    vec2 atlasTilePos=vec2(in_VmtrTC.x,in_VmtrTC.y);
    gl_Position=vec4((atlasTilePos*2.0)-vec2(1.0),0.0,1.0);
    cameraDepth=dot(camera.mvpmatrixw,gl_Position);
}

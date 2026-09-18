#version 450
layout(set=0,binding=2,std140) uniform Camera { vec4 mvpmatrixw; } camera;
void main() {
    vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);
    float w=camera.mvpmatrixw.w;
    gl_Position=vec4(vec2(p.x*4.0-.5,p.y*4.0-1.0)*w,.5*w,w);
}

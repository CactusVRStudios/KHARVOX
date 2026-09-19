#version 450
layout(set=0,binding=0,std140) uniform Camera {
    vec4 mvpmatrixx; vec4 mvpmatrixy; vec4 mvpmatrixz; vec4 mvpmatrixw;
} variantCamera;
layout(location=0) in vec4 position;
layout(location=1) in vec2 texcoord;
layout(location=3) in vec4 color;
layout(location=0) out vec4 tint;
layout(location=2) out vec2 uv;
void main() {
    gl_Position=vec4(dot(position,variantCamera.mvpmatrixx),dot(position,variantCamera.mvpmatrixy),
        dot(position,variantCamera.mvpmatrixz),dot(position,variantCamera.mvpmatrixw));
    tint=color; uv=texcoord;
}

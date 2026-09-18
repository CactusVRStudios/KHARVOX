#version 450
layout(set=0,binding=0,std140) uniform Camera {
    vec4 mvpmatrixx;
    vec4 mvpmatrixy;
    vec4 mvpmatrixz;
    vec4 mvpmatrixw;
} camera;
// Disabled profile effects can retain unused camera declarations.
void main() {}

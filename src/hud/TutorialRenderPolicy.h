#pragma once
#include <array>
#include <cmath>
namespace kharvox {
inline std::array<float,6> voiceCommMatrix(std::array<float,6> m,
    const std::array<float,6>& root,float w,float h){
    for(float x:m)if(!std::isfinite(x))return m;
    for(float x:root)if(!std::isfinite(x))return m;
    if(!std::isfinite(w)||w<=0||w>8192||!std::isfinite(h)||h<=0||h>8192)return m;
    // Lower-center anchor in canvas space, independent of render pixels.
    // Keep native animated scale/rotation, but replace the off-screen position.
    m[4]=root[4]+root[0]*w*.55f+root[2]*h*.72f;
    m[5]=root[5]+root[3]*w*.55f+root[1]*h*.72f;
    for(int i=0;i<4;++i)m[i]*=.54f;
    return m;
}
inline std::array<float,6> bossHealthBarMatrix(std::array<float,6> m,
    const std::array<float,6>& root,float w,float h){
    for(float x:m)if(!std::isfinite(x))return m;
    for(float x:root)if(!std::isfinite(x))return m;
    if(!std::isfinite(w)||w<=0||w>8192||!std::isfinite(h)||h<=0||h>8192)return m;
    const float determinant=root[0]*root[1]-root[2]*root[3];
    if(!std::isfinite(determinant)||std::abs(determinant)<1e-8f)return m;
    // 60% smaller, centered horizontally on the canvas. Keep the authored
    // vertical anchor and r309's downward translation rather than scaling it.
    constexpr float scale=.4f;
    const float localX=(root[1]*(m[4]-root[4])-root[2]*(m[5]-root[5]))/determinant;
    const float centerOffset=(w*.5f-localX)*(1-scale);
    m[4]+=root[0]*centerOffset+root[2]*h*.25f;
    m[5]+=root[3]*centerOffset+root[1]*h*.25f;
    for(int i=0;i<4;++i)m[i]*=scale;
    return m;
}
inline std::array<float,6> runeCounterMatrix(std::array<float,6> m,
    const std::array<float,6>& root,float w,float h,bool killCounter=false){
    for(float x:m)if(!std::isfinite(x))return m;
    for(float x:root)if(!std::isfinite(x))return m;
    if(!std::isfinite(w)||!std::isfinite(h)||w<=0||h<=0||w>8192||h>8192)return m;
    const float cx=root[4]+root[0]*w*.5f+root[2]*h*.5f;
    const float cy=root[5]+root[3]*w*.5f+root[1]*h*.5f;
    // Scale the rendered group about the common canvas center. Using its
    // parent's local origin would pull centered children sideways.
    // 15% smaller than r298 (0.50 * 0.85). Compensate the scale change
    // at the authored top-timer center (~10% canvas height), retaining its height.
    constexpr float scale=.425f;
    const float down=killCounter?.07f:.03f;
    for(int i=0;i<4;++i)m[i]*=scale;
    m[4]=m[4]*scale+cx*(1-scale)+root[2]*h*down;
    m[5]=m[5]*scale+cy*(1-scale)+root[1]*h*down;
    return m;
}
inline std::array<float,6> missionTextMatrix(std::array<float,6> m,float w,float h) {
    for(float x:m)if(!std::isfinite(x))return m;
    if(!std::isfinite(w)||!std::isfinite(h)||w<=0||h<=0||w>8192||h>8192)return m;
    m[4]+=m[0]*w*.20f+m[2]*h*.18f;
    m[5]+=m[3]*w*.20f+m[1]*h*.18f;
    return m;
}
inline std::array<float,6> tutorialTextMatrix(std::array<float,6> m, float w, float h) {
    for (float x:m) if (!std::isfinite(x)) return m;
    if (!std::isfinite(w)||!std::isfinite(h)||w<=0||h<=0||w>8192||h>8192) return m;
    // Scale around canvas center, then raise the result by 10% canvas height.
    // Native tutorial occupies the lower fifth: its center moves near 62% height.
    constexpr float scale=0.55f;
    const float dx=(1-scale)*w*0.5f;
    const float dy=((1-scale)*0.5f-0.10f)*h;
    m[4]+=m[0]*dx+m[2]*dy;
    m[5]+=m[3]*dx+m[1]*dy;
    for(int i=0;i<4;++i)m[i]*=scale;
    return m;
}
}

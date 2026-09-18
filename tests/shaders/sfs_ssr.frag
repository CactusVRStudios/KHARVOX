#version 450
layout(set=0,binding=0) uniform sampler2D samp_viewdepthmap;
layout(set=0,binding=1) uniform sampler2D monoImage;
struct Low {vec4 projectionmatrixz;vec4 globalvieworigin;vec4 resolutionscale;vec4 frustumvectl;vec4 frustumvectr;vec4 frustumvecbl;vec4 windowpostoglobalx;};
struct Inputs {vec4 fragCoord;};
struct High {vec4 prevviewprojectionmatrixx;vec4 prevviewprojectionmatrixy;vec4 ssrparms;vec4 prevglobalpostowindowx;};
// SSR packed projection plus hit-to-world/history round trips. The readback
// checks independent eye-local coordinates and zero static reprojection error.
vec3 GetViewPos(vec3 winPos, vec4 inverseProjection0, vec4 inverseProjection1)
{
    return vec3((inverseProjection0.xy * winPos.xy) + inverseProjection0.zw, inverseProjection1.z) / vec3((inverseProjection1.x * winPos.z) + inverseProjection1.y);
}
vec3 GetWindowPosZ(vec3 viewPos, vec4 projection)
{
    return vec3(vec2(0.5) + (projection.xy * (viewPos.xy / vec2(viewPos.z))), (projection.w / viewPos.z) + projection.z);
}
void main() {
    Low freqLow_fragmentUniforms=Low(vec4(0,0,0,2),vec4(0),vec4(1),vec4(-1,-1,1,0),vec4(1,-1,1,0),vec4(-1,1,1,0),vec4(1));
    Inputs lightInput=Inputs(vec4(0,0,1,1));
    vec2 tc=vec2(.5);
    vec3 clusterCoordinate=vec3(tc,0);
    clusterCoordinate.y = 1.0 - clusterCoordinate.y;
    clusterCoordinate.xy=floor(clusterCoordinate.xy*vec2(16,8));
    vec3 frustumVecX0=vec3(0,-1,1),frustumVecX1=vec3(0,1,1);
    vec3 frustumVec = mix(frustumVecX1, frustumVecX0, vec3(1.0 - (tc.y * freqLow_fragmentUniforms.resolutionscale.w)));
    float zLinear=freqLow_fragmentUniforms.projectionmatrixz.w/(lightInput.fragCoord.z+freqLow_fragmentUniforms.projectionmatrixz.z);
    vec3 world_pos = freqLow_fragmentUniforms.globalvieworigin.xyz + (frustumVec * zLinear);
    High high=High(vec4(.5,0,.5,0),vec4(0,.5,.5,0),vec4(.1),vec4(1));
    float rcpHW=1.0/zLinear;
    vec2 winPosPrev = vec2(dot(world_pos, high.prevviewprojectionmatrixx.xyz),dot(world_pos, high.prevviewprojectionmatrixy.xyz)) * rcpHW;
    // Static geometry must produce zero motion after per-eye reprojection.
    vec2 velocity=tc-winPosPrev;
    vec3 aoPosition=GetViewPos(vec3(tc,1),vec4(2,2,-1,-1),vec4(0,.5,1,0));
    vec2 aoWindow=GetWindowPosZ(vec3(0,0,2),vec4(.5)).xy;
    vec3 best_hit=vec3(tc,1);
    vec3 hitPoint = vec3(best_hit.xy * freqLow_fragmentUniforms.resolutionscale.zw, best_hit.z);
    vec4 tc_reproj=vec4(hitPoint.xy*2.0-1.0,0,2);
    vec2 historyUv = (tc_reproj.xy * 0.5) + vec2(0.5);
    gl_FragDepth=texelFetch(samp_viewdepthmap,ivec2(2),0).r+texture(monoImage,vec2(.5)).r+clusterCoordinate.x*.001+world_pos.x*.1+dot(velocity,vec2(.1))
        +aoPosition.x*high.ssrparms.x+(aoWindow.x-.5)*.2 + dot(abs(historyUv-tc),vec2(.1))*high.prevglobalpostowindowx.x + dot(abs(hitPoint.xy-(world_pos.xy/(2.0*zLinear)+vec2(.5))),vec2(.1))*freqLow_fragmentUniforms.windowpostoglobalx.x;
}

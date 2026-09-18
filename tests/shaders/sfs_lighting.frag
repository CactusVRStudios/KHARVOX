#version 450
layout(set=0,binding=0) uniform sampler2D stereoImage;
layout(set=0,binding=1) uniform sampler2D monoImage;
struct Low {vec4 projectionmatrixz;vec4 globalvieworigin;vec4 resolutionscale;vec4 frustumvectl;vec4 frustumvectr;vec4 frustumvecbl;};
struct Inputs {vec4 fragCoord;};
void main() {
    Low freqLow_fragmentUniforms=Low(vec4(0,0,0,2),vec4(0),vec4(1),vec4(-1,-1,1,0),vec4(1,-1,1,0),vec4(-1,1,1,0));
    Inputs inputs=Inputs(vec4(0,0,1,1));
    vec2 tc=vec2(.5);
    vec3 clusterCoordinate=vec3(tc,0);
    clusterCoordinate.y = 1.0 - clusterCoordinate.y;
    clusterCoordinate.xy=floor(clusterCoordinate.xy*vec2(16,8));
    vec3 frustumVecX0=vec3(0,-1,1),frustumVecX1=vec3(0,1,1);
    vec3 frustumVec = mix(frustumVecX1, frustumVecX0, vec3(1.0 - (tc.y * freqLow_fragmentUniforms.resolutionscale.w)));
    float zLinear=freqLow_fragmentUniforms.projectionmatrixz.w/(inputs.fragCoord.z+freqLow_fragmentUniforms.projectionmatrixz.z);
    vec3 world_pos = freqLow_fragmentUniforms.globalvieworigin.xyz + (frustumVec * zLinear);
    gl_FragDepth=texelFetch(stereoImage,ivec2(2),0).r+texture(monoImage,vec2(.5)).r+clusterCoordinate.x*.001+world_pos.x*.1;
}

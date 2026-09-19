#include "../src/sfs/DoomLighting.h"
#include "../src/sfs/ShaderCompiler.h"
#include "../src/sfs/ShaderProfile.h"
#include <fstream>
#include <stdexcept>
#include <iostream>
using namespace kharvox::sfs;
static void check(bool b){if(!b)throw std::runtime_error("Lighting rewrite regression");}
int main(int argc,char** argv){try{
    check(argc==5);
    for(int i=1;i<5;++i){
        std::ifstream file(argv[i],std::ios::binary|std::ios::ate);check(bool(file));const auto size=file.tellg();check(size>=20&&size%4==0);
        std::vector<uint32_t> words(size_t(size)/4);file.seekg(0);check(bool(file.read(reinterpret_cast<char*>(words.data()),size)));
        if(i==4){
            check(!needsStereoProjection(words,false));
            check(!needsHeadsetProjection(words,true,false));
            check(!needsHeadsetProjection(words,false,false));
            continue;
        }
        ShaderCompileOptions options;options.vertexProjection=needsStereoProjection(words,false);check(options.vertexProjection);
        // Unshifted TV replacement still has a centered camera MVP. Camera
        // passes need the headset FOV; the identical shadow variant does not.
        check(!needsStereoProjection(words,true));
        check(needsHeadsetProjection(words,true,false));
        check(!needsHeadsetProjection(words,true,true));
        check(needsHeadsetProjection(words,false,false));
        check(!needsHeadsetProjection(words,false,true));
        const auto compiled=compileStereoShader(words,options);
        check(compiled.vertexProjectionApplied==(i!=1)); // Atlas vs camera, same MVP member.
        options.monoscopicView=true;
        const auto mono=compileStereoShader(words,options);
        check(!mono.vertexProjectionApplied&&!mono.screenSpaceUiApplied);
        check(mono.glsl.find("khSfsProjection")==std::string::npos);
        options.monoscopicView=false;
        options.screenSpaceUi=true;
        const auto ui=compileStereoShader(words,options);
        check(ui.screenSpaceUiApplied==(i==2));
        if(i==3){
            check(ui.vertexProjectionApplied);
            check(ui.glsl.find("if (gl_Position.w > 8.0)")==std::string::npos);
            check(ui.glsl.find("gl_Position += khSfsProjection.eyeTranslation[gl_ViewIndex];")!=std::string::npos);
        }
        if(i==2){
            const auto fov=ui.glsl.find("gl_Position = khSfsProjection.clipFromCenter[gl_ViewIndex] * gl_Position;");
            const auto ipd=ui.glsl.find(".xyz) > 0.00000001 || gl_Position.w > 8.0) gl_Position += khSfsProjection.eyeTranslation[gl_ViewIndex];");
            check(fov!=std::string::npos&&ipd!=std::string::npos&&fov<ipd);
        }
    }
    std::string unrelated="void main() { /* shared light data */ }";
    check(doomUiShader(0xd7790e0979cc584full)&&doomUiShader(0xc757868ee21edb47ull));
    check(!doomUiShader(0x6fb890e92d8bf507ull));
    const auto untouched=unrelated;auto none=correctDoomLighting(unrelated);
    check(unrelated==untouched&&!none.clusters&&!none.worldPositions);
    std::string shader=R"(
        clusterCoordinate.y = 1.0 - clusterCoordinate.y;
        clusterCoordinate.x -= (_123.vk3d_params[gl_ViewIndex].stereo.x * 0.5);
        vec4 projection = freqLow_fragmentUniforms.projectionmatrixz;
        float z = inputs.fragCoord.z;
        vec3 frustumVec = mix(frustumVecX1, frustumVecX0, vec3(1.0 - (tc.y * freqLow_fragmentUniforms.resolutionscale.w)));
        vec3 world_pos = freqLow_fragmentUniforms.globalvieworigin.xyz + (frustumVec * zLinear);
        world_pos -= (camera_horizontal_world_normalized * adjustment_magnitude);
        vec2 winPosPrev = vec2(dot(world_pos, high.prevviewprojectionmatrixx.xyz),dot(world_pos, high.prevviewprojectionmatrixy.xyz)) * rcpHW;
    )";
    const auto result=correctDoomLighting(shader);
    check(result.clusters==1&&result.worldPositions==1);
    check(result.temporal==1&&shader.find("winPosPrev = khSfsPreviousUv(winPosPrev, rcpHW);")!=std::string::npos);
    check(shader.find("clusterCoordinate.x -=")==std::string::npos);
    check(shader.find("world_pos -=")==std::string::npos);
    check(shader.find("khSfsCenterUv(clusterCoordinate.xy, freqLow_fragmentUniforms.projectionmatrixz.w")!=std::string::npos);
    check(shader.find("khSfsCenterUv(khSfsUv, zLinear, false)")!=std::string::npos);
    for(const std::string input:{"inputs","lightInput"}){
        std::string clustered="clusterCoordinate.y = 1.0 - clusterCoordinate.y;\nfloat z = "+input+".fragCoord.z;\nvec4 p = low.projectionmatrixz;";
        check(correctDoomLighting(clustered).clusters==1);
        check(clustered.find("low.projectionmatrixz.w / ("+input+".fragCoord.z + low.projectionmatrixz.z)")!=std::string::npos);
    }
    std::string worldClusters="vec3 ndcPos = clipPos.xyz / vec3(clipPos.w);\nvec3 clusterCoordinate = vec3(texCoord,0);";
    const auto worldClustersBefore=worldClusters;
    check(!correctDoomLighting(worldClusters).clusters&&worldClusters==worldClustersBefore);
    std::string particle="// low.gpuparticlephysicsparms samp_viewdepthmap samp_viewnormalmap low.viewprojectionmatrixw\n"
        "screenPosition.w = dot4(p, m);\nbool visible = screenPosition.x < screenPosition.w;\nDoCollisionTest(screenPosition);";
    check(correctDoomParticleCollision(particle));
    check(particle.find("khSfsProjection.clipFromCenter[0]")<particle.find("bool visible"));
    check(particle.find("screenPosition.y = -screenPosition.y;")<particle.find("khSfsProjection.clipFromCenter[0]"));
    std::string unrelatedParticle="screenPosition.w = dot4(p, m);";
    check(!correctDoomParticleCollision(unrelatedParticle));
    // A partial/unknown reconstruction must not erase its profile correction.
    std::string partial="world_pos -= (camera_horizontal_world_normalized * adjustment_magnitude);";
    const auto before=partial;correctDoomLighting(partial);check(partial==before);
    for(bool profile:{false,true}){
        std::string glass="mat4 m = mat4(low.globalpostowindowx);\nvec4 refr_tc = MatrixMul(p, m);\n";
        if(profile)glass+="refr_tc.x += ((_1360.vk3d_params[gl_ViewIndex].stereo.x * (refr_tc.w - _1360.vk3d_params[gl_ViewIndex].stereo.y)) * 0.5);\n";
        glass+="vec2 uv = refr_tc.xy / vec2(refr_tc.w);\nvec4 color = tex2Dlod(samp_scenemip0, uv);";
        auto fixed=correctDoomLighting(glass);check(fixed.refractions==1);
        check(glass.find("refr_tc.x +=")==std::string::npos);
        check(glass.find("khSfsEyeWindow(refr_tc)")<glass.find("vec2 uv"));
    }
    std::string unknown="vec4 refr_tc = MatrixMul(p, m);";
    const auto unknownBefore=unknown;check(correctDoomLighting(unknown).refractions==0&&unknown==unknownBefore);
    const std::string ao=R"(
        // low.ssdoparms samp_viewdepthmap
        vec3 GetViewPos(vec3 winPos, vec4 inverseProjection0, vec4 inverseProjection1) {
            return vec3((inverseProjection0.xy * winPos.xy) + inverseProjection0.zw, inverseProjection1.z) / vec3((inverseProjection1.x * winPos.z) + inverseProjection1.y);
        }
        vec2 GetWindowPos(vec3 viewPos, vec4 projection) {
            return vec2(0.5) + (projection.xy * (viewPos.xy / vec2(viewPos.z)));
        }
    )";
    auto correctedAo=ao;check(correctDoomLighting(correctedAo).ssdo==1);
    check(correctedAo.find("winPos.xy = khSfsCenterRayUv(winPos.xy)")!=std::string::npos);
    check(correctedAo.find("return khSfsEyeRayUv(")!=std::string::npos);
    // Never change a partial or unrelated packed-projection shader.
    auto partialAo=ao.substr(0,ao.find("vec2 GetWindowPos"));const auto partialAoBefore=partialAo;
    check(correctDoomLighting(partialAo).ssdo==0&&partialAo==partialAoBefore);
    const std::string ssr=R"(
        // low.ssrparms low.windowpostoglobalx high.prevglobalpostowindowx
        vec3 GetViewPos(vec3 winPos, vec4 inverseProjection0, vec4 inverseProjection1) {
            return vec3((inverseProjection0.xy * winPos.xy) + inverseProjection0.zw, inverseProjection1.z) / vec3((inverseProjection1.x * winPos.z) + inverseProjection1.y);
        }
        vec3 GetWindowPosZ(vec3 viewPos, vec4 projection) {
            return vec3(vec2(0.5) + (projection.xy * (viewPos.xy / vec2(viewPos.z))), (projection.w / viewPos.z) + projection.z);
        }
        scene.world_pos = vec3(0.0);
        vec3 p = vec3(best_hit.xy * low.resolutionscale.zw, best_hit.z);
        vec2 uv = (tc_reproj.xy * 0.5) + vec2(0.5);
    )";
    auto fixedSsr=ssr;check(correctDoomLighting(fixedSsr).ssr==1);
    check(fixedSsr.find("winPos.xy = khSfsCenterRayUv(winPos.xy)")!=std::string::npos);
    check(fixedSsr.find("return vec3(khSfsEyeRayUv(")!=std::string::npos);
    check(fixedSsr.find("p.xy = khSfsCenterUv(p.xy,")!=std::string::npos);
    check(fixedSsr.find("uv = khSfsPreviousUv(uv, 1.0 / tc_reproj.w)")!=std::string::npos);
    check(fixedSsr.find("scene.world_pos = vec3(0.0)")==std::string::npos);
    auto partialSsr=ssr.substr(0,ssr.find("vec2 uv"));const auto originalSsr=partialSsr;
    check(!correctDoomLighting(partialSsr).ssr&&partialSsr==originalSsr);
    std::cout<<"Generic/profile lighting anchors and replacement without double correction passed\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

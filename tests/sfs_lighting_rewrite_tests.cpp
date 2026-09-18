#include "../src/sfs/DoomLighting.h"
#include <stdexcept>
#include <iostream>
using namespace kharvox::sfs;
static void check(bool b){if(!b)throw std::runtime_error("Lighting rewrite regression");}
int main(){try{
    std::string unrelated="void main() { /* shared light data */ }";
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
    )";
    const auto result=correctDoomLighting(shader);
    check(result.clusters==1&&result.worldPositions==1);
    check(shader.find("clusterCoordinate.x -=")==std::string::npos);
    check(shader.find("world_pos -=")==std::string::npos);
    check(shader.find("khSfsCenterUv(clusterCoordinate.xy, freqLow_fragmentUniforms.projectionmatrixz.w")!=std::string::npos);
    check(shader.find("khSfsCenterUv(khSfsUv, zLinear, false)")!=std::string::npos);
    // A partial/unknown reconstruction must not erase its profile correction.
    std::string partial="world_pos -= (camera_horizontal_world_normalized * adjustment_magnitude);";
    const auto before=partial;correctDoomLighting(partial);check(partial==before);
    std::cout<<"Generic/profile lighting anchors and replacement without double correction passed\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

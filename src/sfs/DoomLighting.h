#pragma once
#include <regex>
#include <string>

namespace kharvox::sfs {
struct LightingCorrections { unsigned clusters{}, worldPositions{}; };

// These are DOOM semantic anchors, also retained by its AMD modules, rather
// than GPU-specific shader hashes. Unknown shader layouts remain untouched.
// Buffers containing clustered light lists are built for the centered camera.
inline LightingCorrections correctDoomLighting(std::string& source) {
    LightingCorrections result;
    const std::string anchor="clusterCoordinate.y = 1.0 - clusterCoordinate.y;";
    std::smatch uniform;
    const std::regex projection(R"((\w+)\.projectionmatrixz)");
    if(source.find(anchor)!=std::string::npos && std::regex_search(source,uniform,projection)
       && source.find("inputs.fragCoord.z")!=std::string::npos) {
        // Remove the fixed-display profile adjustment before installing the
        // inverse headset projection, including the depth-dependent IPD term.
        const auto matrix=uniform[1].str()+".projectionmatrixz";
        source=std::regex_replace(source,std::regex(R"(clusterCoordinate\.x -= [^;\n]*\.stereo\.x \* 0\.5\)?;)"),"");
        const auto replacement=anchor+"\n    clusterCoordinate.xy = clamp(khSfsCenterUv(clusterCoordinate.xy, "
            +matrix+".w / (inputs.fragCoord.z + "+matrix+".z), true), vec2(0.0), vec2(0.99999994));";
        size_t pos=0;
        while((pos=source.find(anchor,pos))!=std::string::npos){source.replace(pos,anchor.size(),replacement);pos+=replacement.size();++result.clusters;}
    }
    // Deferred lighting/fog reconstruct position using the centered frustum.
    // Restore its UV before reconstruction; sampling the eye's G-buffer itself
    // remains at the original UV. Existing profile world corrections are
    // superseded only when this complete reconstruction pattern is recognized.
    const std::regex world(R"(vec3 world_pos = (\w+)\.globalvieworigin\.xyz \+ \(frustumVec \* (\w+)\);)");
    const std::regex uv(R"(vec3 frustumVec = mix\(frustumVecX1, frustumVecX0, vec3\(1\.0 - \((\w+)\.y \* (\w+)\.resolutionscale\.w\)\)\);)");
    std::smatch w,u;
    if(std::regex_search(source,w,world)&&std::regex_search(source,u,uv)&&w[1]==u[2]){
        const auto low=w[1].str(),depth=w[2].str(),tc=u[1].str();
        const auto correction="\n    vec2 khSfsUv = "+tc+".xy * "+low+".resolutionscale.zw;\n"
            "    vec2 khSfsUvDelta = khSfsCenterUv(khSfsUv, "+depth+", false) - khSfsUv;\n"
            "    world_pos += (("+low+".frustumvectr.xyz - "+low+".frustumvectl.xyz) * khSfsUvDelta.x + ("
            +low+".frustumvecbl.xyz - "+low+".frustumvectl.xyz) * khSfsUvDelta.y) * "+depth+";";
        source.insert(size_t(w.position()+w.length()),correction);
        source=std::regex_replace(source,std::regex(R"(world_pos -= \(camera_horizontal_world_normalized \* adjustment_magnitude\);)"),"");
        source=std::regex_replace(source,std::regex(R"(world_pos\.xyz -= adjustment_magnitude \* camera_horizontal_world_normalized;)"),"");
        ++result.worldPositions;
    }
    return result;
}
inline std::string lightingProjectionHelper(const std::string& eye){
    return "vec2 khSfsCenterUv(vec2 uv, float clipW, bool flipY) {\n"
        "    int eye = int("+eye+");\n"
        "    mat4 m = khSfsProjection.clipFromCenter[eye];\n"
        "    vec2 ndc = uv * 2.0 - 1.0; if (flipY) ndc.y = -ndc.y;\n"
        "    ndc = (ndc - m[3].xy - khSfsProjection.eyeTranslation[eye].xy / max(clipW, 0.000001)) / vec2(m[0][0], m[1][1]);\n"
        "    if (flipY) ndc.y = -ndc.y; return ndc * 0.5 + 0.5;\n}\n";
}
}

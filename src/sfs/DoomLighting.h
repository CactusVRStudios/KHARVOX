#pragma once
#include <regex>
#include <string>

namespace kharvox::sfs {
struct LightingCorrections { unsigned clusters{}, worldPositions{}, refractions{}, temporal{}, ssdo{}, ssr{}; };

// Shared particle state must run once. Its depth/normal sampling already selects
// array layer zero: project collision coordinates into that same eye before
// bounds checks, perspective division and the engine's final Y flip.
inline bool correctDoomParticleCollision(std::string& source) {
    const std::regex clipW(R"(screenPosition\.w = dot4\([^;\n]+\);)");
    std::smatch w;
    if(source.find(".gpuparticlephysicsparms")==std::string::npos ||
       source.find("samp_viewdepthmap")==std::string::npos || source.find("samp_viewnormalmap")==std::string::npos ||
       source.find(".viewprojectionmatrixw")==std::string::npos || source.find("DoCollisionTest(")==std::string::npos ||
       !std::regex_search(source,w,clipW))return false;
    source.insert(size_t(w.position()+w.length()),
        "\n    screenPosition.y = -screenPosition.y;\n"
        "    screenPosition = khSfsProjection.clipFromCenter[0] * screenPosition + khSfsProjection.eyeTranslation[0];\n"
        "    screenPosition.y = -screenPosition.y;\n");
    return true;
}

// These are DOOM semantic anchors, also retained by its AMD modules, rather
// than GPU-specific shader hashes. Unknown shader layouts remain untouched.
// Buffers containing clustered light lists are built for the centered camera.
inline LightingCorrections correctDoomLighting(std::string& source) {
    LightingCorrections result;
    // SSDO traces a hemisphere in eye-local view space. Its packed projection
    // is still the centered camera's: reconstructing from eye UVs and sampling
    // with centered UVs makes occlusion slide across thin geometry. Convert
    // both directions; no IPD translation is needed for eye-local differences.
    const std::string viewReturn="return vec3((inverseProjection0.xy * winPos.xy) + inverseProjection0.zw, inverseProjection1.z) / vec3((inverseProjection1.x * winPos.z) + inverseProjection1.y);";
    const std::string windowReturn="return vec2(0.5) + (projection.xy * (viewPos.xy / vec2(viewPos.z)));";
    if(source.find(".ssdoparms")!=std::string::npos&&source.find("samp_viewdepthmap")!=std::string::npos
       &&source.find("vec3 GetViewPos(vec3 winPos, vec4 inverseProjection0, vec4 inverseProjection1)")!=std::string::npos
       &&source.find("vec2 GetWindowPos(vec3 viewPos, vec4 projection)")!=std::string::npos
       &&source.find(viewReturn)!=std::string::npos&&source.find(windowReturn)!=std::string::npos){
        source.replace(source.find(viewReturn),viewReturn.size(),
            "winPos.xy = khSfsCenterRayUv(winPos.xy);\n    "+viewReturn);
        source.replace(source.find(windowReturn),windowReturn.size(),
            "return khSfsEyeRayUv(vec2(0.5) + (projection.xy * (viewPos.xy / vec2(viewPos.z))));");
        ++result.ssdo;
    }
    // Window-space refraction must sample the same eye projection that produced
    // scenemip. A horizontal TV separation alone misses the HMD's FOV scale/y.
    const std::regex refract(R"(vec4 refr_tc = MatrixMul\([^;\n]+\);)");
    std::smatch refr;
    if(source.find(".globalpostowindowx")!=std::string::npos
       &&source.find("samp_scenemip0")!=std::string::npos
       &&std::regex_search(source,refr,refract)){
        source=std::regex_replace(source,std::regex(R"(refr_tc\.x \+= [^;\n]*\.stereo\.[^;\n]*;)"),"");
        if(std::regex_search(source,refr,refract)){
            source.insert(size_t(refr.position()+refr.length()),"\n    refr_tc = khSfsEyeWindow(refr_tc);");
            ++result.refractions;
        }
    }
    const std::string anchor="clusterCoordinate.y = 1.0 - clusterCoordinate.y;";
    std::smatch uniform,depthInput;
    const std::regex projection(R"((\w+)\.projectionmatrixz)");
    const std::regex fragDepth(R"((\w+\.fragCoord)\.z)");
    if(source.find(anchor)!=std::string::npos && std::regex_search(source,uniform,projection)
       && std::regex_search(source,depthInput,fragDepth)) {
        // Remove the fixed-display profile adjustment before installing the
        // inverse headset projection, including the depth-dependent IPD term.
        const auto matrix=uniform[1].str()+".projectionmatrixz";
        const auto depth=depthInput[1].str()+".z";
        source=std::regex_replace(source,std::regex(R"(clusterCoordinate\.x -= [^;\n]*\.stereo\.x \* 0\.5\)?;)"),"");
        const auto replacement=anchor+"\n    clusterCoordinate.xy = clamp(khSfsCenterUv(clusterCoordinate.xy, "
            +matrix+".w / ("+depth+" + "+matrix+".z), true), vec2(0.0), vec2(0.99999994));";
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
        // The previous world-to-window matrix is also centered. Current-eye
        // reconstruction alone leaves spurious motion even for a static scene.
        const std::regex previous(R"(vec2 winPosPrev = [^;\n]*\.prevviewprojectionmatrixx[^;\n]*\.prevviewprojectionmatrixy[^;\n]*\* rcpHW;)");
        std::smatch p;
        if(std::regex_search(source,p,previous)){
            source.insert(size_t(p.position()+p.length()),"\n    winPosPrev = khSfsPreviousUv(winPosPrev, rcpHW);");
            ++result.temporal;
        }
    }
    // SSR rays live in eye-local view space, but hit reconstruction and history
    // matrices are centered. Convert all four boundaries together; never apply
    // a partial rewrite or retain the profile's zero-position workaround.
    const std::string windowZ="return vec3(vec2(0.5) + (projection.xy * (viewPos.xy / vec2(viewPos.z))), (projection.w / viewPos.z) + projection.z);";
    const std::regex hitWindow(R"(vec3 (\w+) = vec3\(best_hit\.xy \* (\w+)\.resolutionscale\.zw, best_hit\.z\);)");
    const std::regex previousWindow(R"(vec2 (\w+) = \(tc_reproj\.xy \* 0\.5\) \+ vec2\(0\.5\);)");
    std::smatch hit,previousHit;
    if(source.find(".ssrparms")!=std::string::npos && source.find(".windowpostoglobalx")!=std::string::npos &&
       source.find(".prevglobalpostowindowx")!=std::string::npos && source.find(viewReturn)!=std::string::npos &&
       source.find(windowZ)!=std::string::npos && std::regex_search(source,hit,hitWindow) &&
       std::regex_search(source,previousHit,previousWindow)){
        source.replace(source.find(viewReturn),viewReturn.size(),"winPos.xy = khSfsCenterRayUv(winPos.xy);\n    "+viewReturn);
        source.replace(source.find(windowZ),windowZ.size(),
            "return vec3(khSfsEyeRayUv(vec2(0.5) + (projection.xy * (viewPos.xy / vec2(viewPos.z)))), (projection.w / viewPos.z) + projection.z);");
        std::regex_search(source,hit,hitWindow);
        const auto p=hit[1].str(),low=hit[2].str();
        source.insert(size_t(hit.position()+hit.length()),"\n    "+p+".xy = khSfsCenterUv("+p+".xy, "+low+".projectionmatrixz.w / ("+p+".z + "+low+".projectionmatrixz.z), false);");
        std::regex_search(source,previousHit,previousWindow);
        const auto prev=previousHit[1].str();
        source.insert(size_t(previousHit.position()+previousHit.length()),"\n    "+prev+" = khSfsPreviousUv("+prev+", 1.0 / tc_reproj.w);");
        source=std::regex_replace(source,std::regex(R"(scene\.world_pos = vec3\(0\.0\);)"),"");
        ++result.ssr;
    }
    return result;
}
inline std::string lightingProjectionHelper(const std::string& eye){
    return "vec2 khSfsCenterRayUv(vec2 uv) {\n"
        "    mat4 m = khSfsProjection.clipFromCenter[int("+eye+")];\n"
        "    return ((uv * 2.0 - 1.0 - m[3].xy) / vec2(m[0][0], m[1][1]) + 1.0) * 0.5;\n}\n"
        "vec2 khSfsEyeRayUv(vec2 uv) {\n"
        "    mat4 m = khSfsProjection.clipFromCenter[int("+eye+")];\n"
        "    return ((uv * 2.0 - 1.0) * vec2(m[0][0], m[1][1]) + m[3].xy + 1.0) * 0.5;\n}\n"
        "vec2 khSfsPreviousUv(vec2 uv, float inverseW) {\n"
        "    int eye = int("+eye+");\n"
        "    mat4 m = khSfsProjection.previousClipFromCenter[eye];\n"
        "    return ((uv * 2.0 - 1.0) * vec2(m[0][0], m[1][1]) + m[3].xy + khSfsProjection.previousEyeTranslation[eye].xy * inverseW + 1.0) * 0.5;\n}\n"
        "vec4 khSfsEyeWindow(vec4 window) {\n"
        "    int eye = int("+eye+");\n"
        "    mat4 m = khSfsProjection.clipFromCenter[eye];\n"
        "    vec2 clipXY = (window.xy * 2.0 - window.ww) * vec2(m[0][0], m[1][1]);\n"
        "    window.xy = (clipXY + m[3].xy * window.w + khSfsProjection.eyeTranslation[eye].xy + window.ww) * 0.5;\n"
        "    return window;\n}\n"
        "vec2 khSfsCenterUv(vec2 uv, float clipW, bool flipY) {\n"
        "    int eye = int("+eye+");\n"
        "    mat4 m = khSfsProjection.clipFromCenter[eye];\n"
        "    vec2 ndc = uv * 2.0 - 1.0; if (flipY) ndc.y = -ndc.y;\n"
        "    ndc = (ndc - m[3].xy - khSfsProjection.eyeTranslation[eye].xy / max(clipW, 0.000001)) / vec2(m[0][0], m[1][1]);\n"
        "    if (flipY) ndc.y = -ndc.y; return ndc * 0.5 + 0.5;\n}\n";
}
}

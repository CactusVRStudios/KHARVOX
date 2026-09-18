#pragma once
#include "FrameProjection.h"
#include "../native/NativeStereo.h"
#include "../common/AerSourceTracking.h"

namespace kharvox::sfs {
// The CPU camera can precede Acquire by more than one frame. Projection
// uniforms contain eye-local offsets, not world rotation; qualify the latter
// against the observed camera source before handing it to the compositor.
class SourcePoseHistory {
    struct Sample {native::FramePose pose;FrameUniforms uniforms;};
    AerInputHistory<Sample> history_;
public:
    void remember(const native::FramePose& pose,const FrameUniforms& uniforms){
        if(pose.gameplay)history_.remember({pose.source.poseId,pose.source.level,0,0},{pose,uniforms});
    }
    bool resolve(const AerSourceObservation& observed,const native::FramePose& acquired,
                 const FrameUniforms& installed,native::FramePose& result)const{
        if(!observed.valid()||observed.key.eye!=0||observed.key.domain!=0
            ||!acquired.gameplay||acquired.viewSpace)return false;
        Sample sample;
        if(!history_.find(observed.key,sample)
            ||!native::sameSceneContext(sample.pose.source,acquired.source,sample.pose.viewSpace,acquired.viewSpace)
            ||sample.pose.serial>acquired.serial||acquired.serial-sample.pose.serial>8)return false;
        // Relabelling cannot repair a changed FOV, IPD or world scale. Refuse
        // such transitions rather than attach a pose to incompatible pixels.
        for(unsigned e=0;e<2;++e){
            for(unsigned i=0;i<16;++i)if(!std::isfinite(installed.clip[e][i])
                ||!std::isfinite(sample.uniforms.clip[e][i])
                ||std::abs(installed.clip[e][i]-sample.uniforms.clip[e][i])>0.0001f)return false;
            for(unsigned i=0;i<4;++i)if(!std::isfinite(installed.translation[e][i])
                ||!std::isfinite(sample.uniforms.translation[e][i])
                ||std::abs(installed.translation[e][i]-sample.uniforms.translation[e][i])>0.0001f)return false;
        }
        result=sample.pose;return true;
    }
};
}

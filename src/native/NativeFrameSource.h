#pragma once
#include <cstdint>
namespace kharvox::native {
enum class SceneDomain : unsigned { Gameplay, Cinematic, Scripted, Inactive };
struct FrameSourceIdentity { uint64_t poseId{},level{}; SceneDomain domain{SceneDomain::Inactive}; };
constexpr SceneDomain sceneDomain(bool quad,bool cinematic,bool scripted,bool gameplay){
    return quad?SceneDomain::Inactive:cinematic?SceneDomain::Cinematic:
        scripted?SceneDomain::Scripted:gameplay?SceneDomain::Gameplay:SceneDomain::Inactive;
}
// The prepared tracking sample must be the one used throughout both native roots.
constexpr bool sourcePoseStable(FrameSourceIdentity source,uint64_t firstPose,
                               uint64_t lastPose,uint64_t level){
    return source.poseId&&source.domain!=SceneDomain::Inactive&&source.level==level
        &&source.poseId==firstPose&&firstPose==lastPose;
}
// A completed frame may use an older prediction, but never another scene domain
// or level. Compare pose identity to its producer, not the next XR prediction.
constexpr bool sameSceneContext(FrameSourceIdentity source,FrameSourceIdentity desired,
                                bool sourceViewSpace,bool desiredViewSpace){
    return source.poseId&&source.domain!=SceneDomain::Inactive
        &&source.domain==desired.domain&&source.level==desired.level
        &&sourceViewSpace==desiredViewSpace;
}
// A display-only hold never makes an old producer pair valid for rendering.
constexpr bool canHoldSfsTransition(FrameSourceIdentity previous,FrameSourceIdentity desired,
                                    bool sameSpace,int64_t ageNanoseconds){
    const bool cameraTransition=
        (previous.domain==SceneDomain::Gameplay&&desired.domain==SceneDomain::Cinematic)
        ||(previous.domain==SceneDomain::Cinematic&&desired.domain==SceneDomain::Gameplay);
    return previous.poseId&&desired.poseId&&cameraTransition&&previous.level==desired.level
        &&sameSpace&&ageNanoseconds>=0&&ageNanoseconds<=100000000;
}
}

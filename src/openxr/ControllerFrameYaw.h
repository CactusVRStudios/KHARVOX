#pragma once
namespace kharvox {
// SFS shares AER's centered CPU camera/input publication order even though
// its two GPU eyes are produced together. Pending turn belongs to the next
// camera publication, not to the controller used by this frame.
constexpr bool usePublishedControllerYaw(bool nativePackedStereo,bool sfs){
    return !nativePackedStereo||sfs;
}
// Captured after head publication, before action processing advances pending
// smooth turn. Position and orientation must use this same frame convention.
struct ControllerFrameYaw {
    float accepted{},published{};bool active{};
    float acceptedForPose(float live)const{return active?accepted:live;}
    float turnForPose(float pending)const{return active?published:pending;}
};
}

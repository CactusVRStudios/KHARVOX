#pragma once
namespace kharvox {
// Captured after head publication, before action processing advances pending
// smooth turn. Position and orientation must use this same frame convention.
struct ControllerFrameYaw {
    float accepted{},published{};bool active{};
    float acceptedForPose(float live)const{return active?accepted:live;}
    float turnForPose(float pending)const{return active?published:pending;}
};
}

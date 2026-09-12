#pragma once
#include <cmath>
#include <cstdint>

namespace kharvox {
// Follow measured stance after a short confirmation, independently of button
// release. World-space vertical movement must not become a local stance change.
class BodyStanceHeight {
    bool valid_{};
    float height_{}, previousMeasured_{}, previousPhysicsUp_{};
    unsigned confirmations_{}, stationary_{};
    std::uint64_t present_{};
public:
    float update(float measured,float calibrated,bool reset,bool requested,
        std::uint64_t present,float physicsUp) {
        (void)requested; // Button level cannot distinguish toggle/blocked crouch.
        if(!std::isfinite(measured)||!std::isfinite(calibrated)||!std::isfinite(physicsUp))
            return valid_?height_:calibrated;
        if(reset||!valid_){
            valid_=true;height_=calibrated;previousMeasured_=measured;
            previousPhysicsUp_=physicsUp;confirmations_=0;stationary_=2;present_=present;
            return height_;
        }
        if(present==present_)return height_;
        present_=present;
        const bool verticalMotion=std::abs(physicsUp-previousPhysicsUp_)>.05f;
        previousPhysicsUp_=physicsUp;
        if(verticalMotion){stationary_=0;confirmations_=0;previousMeasured_=measured;return height_;}
        if(stationary_<2){++stationary_;confirmations_=0;previousMeasured_=measured;return height_;}
        const float delta=measured-height_;
        const float previousDelta=previousMeasured_-height_;
        if(std::abs(delta)>2.f){
            // Confirm direction across distinct Presents; isolated spikes cannot
            // move the anchor even if the physics update arrives one frame later.
            confirmations_=(delta*previousDelta>0 && std::abs(previousDelta)>2.f)
                ?confirmations_+1:1;
            if(confirmations_>=2){height_=measured;confirmations_=0;}
        }else{
            confirmations_=0;
            // Finish a confirmed stance smoothly; sub-unit idle bob stays rejected.
            if(std::abs(delta)>.05f && std::abs(measured-previousMeasured_)<.05f
                && std::abs(previousDelta)>1.f)height_=measured;
        }
        previousMeasured_=measured;
        return height_;
    }
};
}

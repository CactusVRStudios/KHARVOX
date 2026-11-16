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
    float standingHeight_{};
    bool physicalCameraCompensation_{};
    bool physicalStandingValid_{}, physicalCrouched_{};
    float physicalStandingHeight_{};
    unsigned physicalConfirmations_{};
    std::uint64_t physicalSample_{};
public:
    // Metres in OpenXR LOCAL space, never world/camera height. Keep this
    // calibration across menus, loading and tracking loss. A reference-space
    // change explicitly resets it; menu-time head movement cannot rebase it.
    void resetPhysicalCrouch() {
        physicalStandingValid_=false;physicalCrouched_=false;
        physicalConfirmations_=0;physicalSample_=0;
    }
    bool updatePhysicalCrouch(float trackedHeight,bool enabled,bool gameplay,
        bool tracked,std::uint64_t sample) {
        if(!enabled){resetPhysicalCrouch();return false;}
        if(!gameplay||!tracked||!std::isfinite(trackedHeight)){
            // Suspend output without losing the latched posture. Resuming
            // tracking/menu gameplay must not briefly toggle a crouched body
            // upright while the next two samples are being confirmed.
            physicalConfirmations_=0;
            physicalSample_=sample;return false;
        }
        if(!physicalStandingValid_){
            physicalStandingHeight_=trackedHeight;physicalStandingValid_=true;
            physicalSample_=sample;return false;
        }
        if(sample==physicalSample_)return physicalCrouched_;
        physicalSample_=sample;
        const float drop=physicalStandingHeight_-trackedHeight;
        const bool next=physicalCrouched_?drop>.20f:drop>=.35f;
        if(next==physicalCrouched_)physicalConfirmations_=0;
        else if(++physicalConfirmations_>=2){
            physicalCrouched_=next;physicalConfirmations_=0;
        }
        return physicalCrouched_;
    }
    bool physicalStandingHeight(float& height) const {
        if(!physicalStandingValid_)return false;
        height=physicalStandingHeight_;return true;
    }
    bool stanceHeights(float& measured,float& standing) const {
        if(!valid_)return false;
        measured=height_;standing=standingHeight_;return true;
    }
    std::uint64_t stancePresent() const { return present_; }
    // The real engine stance still follows update(). During a physical crouch
    // the HMD supplies the visual descent, so do not add the native descent a
    // second time. Retain compensation through the native stand animation or
    // a blocked stand; an explicit manual crouch takes camera ownership back.
    float cameraHeight(bool physical,bool manual) {
        if(manual)physicalCameraCompensation_=false;
        else if(physical)physicalCameraCompensation_=true;
        else if(height_>=standingHeight_-.05f)physicalCameraCompensation_=false;
        return physicalCameraCompensation_?standingHeight_:height_;
    }
    float update(float measured,float calibrated,bool reset,bool requested,
        std::uint64_t present,float physicsUp) {
        (void)requested; // Button level cannot distinguish toggle/blocked crouch.
        if(!std::isfinite(measured)||!std::isfinite(calibrated)||!std::isfinite(physicsUp))
            return valid_?height_:calibrated;
        if(reset||!valid_){
            valid_=true;height_=calibrated;previousMeasured_=measured;
            standingHeight_=calibrated;physicalCameraCompensation_=false;
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
        if(height_>standingHeight_)standingHeight_=height_;
        return height_;
    }
};
}

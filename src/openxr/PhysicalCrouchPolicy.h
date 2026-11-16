#pragma once
#include <cstdint>

namespace kharvox {

// DOOM's _crouch command toggles native stance on a new press. Physical
// posture owns only the toggle it introduced; manual input/jump takes over.
// Native camera-height feedback comes from BodyStanceHeight, not HMD height.
class PhysicalCrouchCommand {
    bool owned_{}, requireStanding_{};
    std::int64_t pulseUntil_{}, settleUntil_{};
public:
    bool ownsCrouch() const { return owned_; }
    // With no physical request/ownership, native height cannot affect the
    // command. Still run update() for manual input and pending release pulses.
    bool needsStanceFeedback(bool requested) const { return requested||owned_; }
    bool update(std::int64_t now,bool requested,bool gameplay,bool manual,
        bool jump,bool stanceValid,bool nativeCrouched) {
        if(!requested)requireStanding_=false;
        if(manual||jump){
            owned_=false;requireStanding_=requested;
            pulseUntil_=0;settleUntil_=0;return false;
        }
        // Retain ownership across an interruption so that standing up in a
        // menu can be reconciled on gameplay reentry. Never send UI cancel.
        if(!gameplay){pulseUntil_=0;return false;}
        if(now<pulseUntil_)return true;
        if(!stanceValid||now<settleUntil_)return false;
        bool toggle=false;
        if(owned_){
            if(!requested){toggle=nativeCrouched;owned_=false;}
            else if(!nativeCrouched){
                // Jump/script/native input already stood up. Do not fight it
                // with repeated toggles while the headset is still lowered.
                owned_=false;requireStanding_=true;
            }
        }else if(requested&&!requireStanding_&&!nativeCrouched){
            owned_=true;toggle=true;
        }
        if(toggle){
            pulseUntil_=now+100000000; // Allow native input polls to see it.
            settleUntil_=now+400000000; // Native stance animation/confirmation.
        }
        return toggle;
    }
};
} // namespace kharvox

#pragma once
#include <cstdint>

namespace kharvox {
struct StickRecenterOutput {
    bool primary{},support{},recenter{};
};
// Inputs are the weapon-hand and support-hand stick clicks, so handedness
// and stick-axis swapping do not change the physical two-stick chord.
class StickRecenterPolicy {
    bool captured_{},forwarding_{},pendingPrimary_{},pendingSupport_{};
    std::int64_t readyAt_{},primaryPulseUntil_{},supportPulseUntil_{};
public:
    void cancel() { *this={};captured_=true; }
    StickRecenterOutput update(std::int64_t now,bool primary,bool support) {
        if(captured_){if(!primary&&!support)*this={};return {};}
        if(!forwarding_){
            if(!readyAt_){
                if(!primary&&!support)return {};
                readyAt_=now+150000000;
            }
            pendingPrimary_|=primary;pendingSupport_|=support;
            if(now<=readyAt_&&primary&&support){
                cancel();return {false,false,true};
            }
            if(now<readyAt_)return {};
            forwarding_=true;
            // A quick single tap must survive the chord detection window.
            if(pendingPrimary_&&!primary)primaryPulseUntil_=now+100000000;
            if(pendingSupport_&&!support)supportPulseUntil_=now+100000000;
        }
        const StickRecenterOutput result{primary||now<primaryPulseUntil_,
            support||now<supportPulseUntil_,false};
        if(!primary&&!support&&!result.primary&&!result.support)*this={};
        return result;
    }
};
} // namespace kharvox

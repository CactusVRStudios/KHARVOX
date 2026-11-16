#pragma once
#include <array>
#include <cmath>
#include <cstdint>

namespace kharvox {
struct GrenadeThrowOutput {
    bool equipment{},physical{};
    std::array<float,3> velocity{}; // OpenXR LOCAL metres/second.
    bool released{};
};
class GrenadeThrowPolicy {
    struct Sample { std::int64_t time{};std::array<float,3> velocity{}; };
    std::array<Sample,8> samples_{};
    unsigned count_{},next_{};
    bool previousTrigger_{},armed_{},physicalAllowed_{};
    std::int64_t pressedAt_{},pulseUntil_{};
public:
    bool holding() const { return armed_&&previousTrigger_&&physicalAllowed_; }
    void cancel(bool held) { *this={};previousTrigger_=held; }
    GrenadeThrowOutput update(std::int64_t now,bool enabled,bool gameplay,
        bool trigger,bool tracked,bool handFree,const std::array<float,3>& velocity) {
        if(!gameplay){cancel(trigger);return {};}
        if(!enabled){cancel(trigger);return {trigger,false,{}};}
        const bool validVelocity=tracked&&std::isfinite(velocity[0])
            &&std::isfinite(velocity[1])&&std::isfinite(velocity[2])
            &&velocity[0]*velocity[0]+velocity[1]*velocity[1]+velocity[2]*velocity[2]<=400.f;
        if(trigger&&!previousTrigger_){
            armed_=true;pressedAt_=now;count_=next_=0;
            physicalAllowed_=validVelocity&&handFree;
        }
        if(armed_){
            physicalAllowed_&=validVelocity&&handFree;
            if(validVelocity){
                samples_[next_]={now,velocity};next_=(next_+1)%samples_.size();
                if(count_<samples_.size())++count_;
            }
        }
        GrenadeThrowOutput output{};
        if(!trigger&&previousTrigger_&&armed_){
            output.released=true;
            armed_=false;pulseUntil_=now+100000000;
            unsigned recent{};
            for(unsigned i=0;i<count_;++i)if(now>=samples_[i].time
                &&now-samples_[i].time<=100000000){
                for(int axis=0;axis<3;++axis)output.velocity[axis]+=samples_[i].velocity[axis];
                ++recent;
            }
            if(recent)for(auto& value:output.velocity)value/=float(recent);
            const auto& v=output.velocity;
            output.physical=physicalAllowed_&&recent&&now-pressedAt_>=120000000
                &&v[0]*v[0]+v[1]*v[1]+v[2]*v[2]>=1.2f*1.2f;
        }
        previousTrigger_=trigger;output.equipment=now<pulseUntil_;
        return output;
    }
};
} // namespace kharvox

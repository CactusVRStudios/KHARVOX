#pragma once
#include <array>
#include <cmath>
#include <cstdint>

namespace kharvox {
// A one-shot, expiring aim request. The hook changes only the axis copied
// into a native player HandsThrow message; origin/speed stay engine-owned.
class GrenadeThrowAim {
    std::array<float,9> axis_{};
    std::uint64_t expires_{};
    bool pending_{};
public:
    void cancel() { pending_=false; }
    bool queue(const float direction[3],std::uint64_t nowMs) {
        pending_=false;
        const float length=std::sqrt(direction[0]*direction[0]
            +direction[1]*direction[1]+direction[2]*direction[2]);
        if(!std::isfinite(length)||length<.001f)return false;
        for(int i=0;i<3;++i)axis_[i]=direction[i]/length;
        const std::array<float,3> reference=std::abs(axis_[2])<.95f
            ?std::array<float,3>{0,0,1}:std::array<float,3>{0,1,0};
        axis_[3]=reference[1]*axis_[2]-reference[2]*axis_[1];
        axis_[4]=reference[2]*axis_[0]-reference[0]*axis_[2];
        axis_[5]=reference[0]*axis_[1]-reference[1]*axis_[0];
        const float leftLength=std::sqrt(axis_[3]*axis_[3]+axis_[4]*axis_[4]+axis_[5]*axis_[5]);
        for(int i=3;i<6;++i)axis_[i]/=leftLength;
        axis_[6]=axis_[1]*axis_[5]-axis_[2]*axis_[4];
        axis_[7]=axis_[2]*axis_[3]-axis_[0]*axis_[5];
        axis_[8]=axis_[0]*axis_[4]-axis_[1]*axis_[3];
        expires_=nowMs+1000;pending_=true;return true;
    }
    bool take(std::uint64_t nowMs,bool localPlayerRelease,std::array<float,9>& axis) {
        if(!pending_)return false;
        if(nowMs>expires_){pending_=false;return false;}
        if(!localPlayerRelease)return false;
        axis=axis_;pending_=false;return true;
    }
};
} // namespace kharvox

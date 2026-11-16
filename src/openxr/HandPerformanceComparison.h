#pragma once
#include <array>
#include <cmath>
#include <cstdint>

namespace kharvox {

struct PerformanceSamples {
    std::uint64_t count{};
    double totalMs{},maximumMs{};
    void add(double ms) {
        if(!std::isfinite(ms)||ms<0)return;
        ++count;totalMs+=ms;if(ms>maximumMs)maximumMs=ms;
    }
    double meanMs()const{return count?totalMs/double(count):0;}
};

// Compare the same resident assets in three gameplay windows. Do not unload
// textures: this isolates drawing/depth integration, NOT VRAM pressure. Menus,
// cinematics and the first two seconds after each interruption are excluded.
class HandPerformanceComparison {
    int phase_{-1};
    bool enabled_{},eligible_{},sampleCurrent_{};
    std::uint64_t previousMs_{},windowMs_{};
public:
    static constexpr std::uint64_t warmupMs=2000,phaseMs=14000;
    struct Window {PerformanceSamples frames,xrGpu,xrWall;};
    std::array<Window,3> windows{};
    explicit HandPerformanceComparison(bool enabled=false):enabled_(enabled){}
    int phase()const{return phase_;}
    bool finished()const{return phase_==3;}
    bool hideHands()const{return enabled_&&phase_==1&&eligible_;}
    bool enabled()const{return enabled_;}
    bool update(std::uint64_t now,bool gameplay,bool handsConfigured) {
        sampleCurrent_=false;
        if(!enabled_||finished())return false;
        if(!gameplay||!handsConfigured){
            // Retain completed samples, but demand a fresh continuous warmup.
            eligible_=false;windowMs_=0;previousMs_=now;return false;
        }
        if(!eligible_){
            eligible_=true;previousMs_=now;windowMs_=0;
            if(phase_<0){phase_=0;return true;}
            return false;
        }
        if(now<previousMs_){eligible_=false;return false;}
        const auto delta=now-previousMs_;previousMs_=now;
        const auto before=windowMs_;windowMs_+=delta;
        if(windowMs_>=phaseMs){
            ++phase_;windowMs_=0;
            if(finished())eligible_=false;
            return true;
        }
        sampleCurrent_=before>=warmupMs;
        if(sampleCurrent_)windows[phase_].frames.add(double(delta));
        return false;
    }
    void gpu(double ms){if(sampleCurrent_&&phase_>=0&&phase_<3)windows[phase_].xrGpu.add(ms);}
    void xr(double ms){if(sampleCurrent_&&phase_>=0&&phase_<3)windows[phase_].xrWall.add(ms);}
};
} // namespace kharvox

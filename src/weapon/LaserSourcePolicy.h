#pragma once
#include <cstdint>
namespace kharvox {
inline bool laserSourceUsable(uint64_t tick,uint64_t now,uint64_t epoch,uint64_t currentEpoch,
    unsigned kind,unsigned currentKind){
    return tick&&now>=tick&&now-tick<=100&&epoch==currentEpoch&&kind==currentKind;
}
}

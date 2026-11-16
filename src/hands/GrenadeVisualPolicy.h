#pragma once

#include <cstdint>
#include <istream>
#include <string>

namespace kharvox::hands {

// Visual-only prototype. No equipment selection, charges or throw ownership.
struct GrenadeVisualConfig {
    bool enabled{};
    std::uint64_t handHash{}, grenadeHash{};
};

inline bool readGrenadeVisualConfig(std::istream& input,GrenadeVisualConfig& output){
    int version{},enabled{};GrenadeVisualConfig next;
    if(!(input>>version>>enabled>>next.handHash>>next.grenadeHash)
        ||version!=1||(enabled!=0&&enabled!=1)||!next.handHash||!next.grenadeHash)return false;
    std::string extra;if(input>>extra)return false;
    next.enabled=enabled!=0;output=next;return true;
}

inline constexpr bool grenadeVisualEligible(bool ready,bool leftHanded,
    bool held,bool twoHanded,bool physicalLeft,bool freeHand){
    return ready&&!leftHanded&&held&&!twoHanded&&physicalLeft&&freeHand;
}

} // namespace kharvox::hands

#pragma once

#include <cstdint>

namespace kharvox::hands {

// Values are observations, never dereferenced by the sampling policy.
struct NativeHandsDiagnosticKey {
    std::uintptr_t owner{},model{},animator{};
    std::uint64_t level{},maskBefore{},maskAfter{};
    std::uint32_t context{},weapon{};
};

class NativeHandsDiagnosticPolicy {
public:
    static constexpr unsigned maximumSamples=512;
    bool sample(bool enabled,bool worldActive,std::uint64_t tick,
        const NativeHandsDiagnosticKey& key) {
        if(!enabled||!worldActive||count_>=maximumSamples)return false;
        // Do not lose a brief cinematic/model/level transition to mask chatter.
        const bool transition=!count_||key.owner!=last_.owner||key.model!=last_.model
            ||key.animator!=last_.animator||key.level!=last_.level
            ||key.context!=last_.context||key.weapon!=last_.weapon;
        const auto elapsed=tick>=lastTick_?tick-lastTick_:0;
        const bool maskChanged=key.maskBefore!=last_.maskBefore||key.maskAfter!=last_.maskAfter;
        if(!transition&&elapsed<1000&&(!maskChanged||elapsed<100))return false;
        last_=key;lastTick_=tick;++count_;return true;
    }
    unsigned count()const{return count_;}
private:
    NativeHandsDiagnosticKey last_{};
    std::uint64_t lastTick_{};
    unsigned count_{};
};

} // namespace kharvox::hands

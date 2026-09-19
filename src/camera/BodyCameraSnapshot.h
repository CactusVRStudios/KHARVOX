#pragma once
#include <array>
#include <cstdint>
#include <mutex>

namespace kharvox {
struct BodyCameraSnapshot {
    std::array<float,3> origin{},viewOffset{};
    std::array<float,9> axis{};
    uintptr_t anchorOwner{};
    uint64_t generation{};
    bool valid{};
};
class BodyCameraState {
    std::mutex mutex_;
    BodyCameraSnapshot pose_;
public:
    BodyCameraSnapshot read(){
        std::lock_guard lock(mutex_);
        return pose_;
    }
    bool publish(const BodyCameraSnapshot& pose){
        std::lock_guard lock(mutex_);
        if(pose.generation!=pose_.generation)return false;
        pose_=pose;
        return true;
    }
    void invalidate(){
        std::lock_guard lock(mutex_);
        const auto generation=pose_.generation+1;
        pose_={};pose_.generation=generation;
    }
};
}

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
struct PlayerPhysicsSnapshot {
    std::array<float,3> origin{};
    uintptr_t owner{};
    uint64_t present{},generation{};
    bool valid{};
};
class CameraPoseState {
    std::mutex mutex_;
    BodyCameraSnapshot pose_;
    PlayerPhysicsSnapshot physics_;
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
    PlayerPhysicsSnapshot physics(){
        std::lock_guard lock(mutex_);
        return physics_;
    }
    bool publishPhysics(const PlayerPhysicsSnapshot& physics){
        std::lock_guard lock(mutex_);
        if(physics.generation!=pose_.generation)return false;
        physics_=physics;
        return true;
    }
    bool invalidate(){
        std::lock_guard lock(mutex_);
        const bool hadPlayer=physics_.valid;
        const auto generation=pose_.generation+1;
        pose_={};pose_.generation=generation;
        physics_={};physics_.generation=generation;
        return hadPlayer;
    }
};
}

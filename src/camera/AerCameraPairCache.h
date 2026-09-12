#pragma once
#include "../common/AerRenderOrder.h"
#include "../weapon/AerWeaponPoseCache.h"
#include <atomic>

namespace kharvox {
// Camera work can migrate between engine workers. Share the bounded,
// synchronized pose storage already used for weapon pairs. Keys are camera
// caller/processing-stage identities instead of weapon entity pointers.
class AerCameraPairCache {
    AerWeaponPoseCache poses_;
    std::atomic<uint64_t> state_{packAerWeaponPairState({1,-1,false})};
public:
    void begin(int physicalEye,bool enabled) {
        auto previous=state_.load(std::memory_order_acquire);
        for(;;) {
            const auto next=nextAerWeaponPairState(previous,
                aerRenderPairPhase(physicalEye),enabled);
            if(state_.compare_exchange_weak(previous,next,
                    std::memory_order_release,std::memory_order_acquire))return;
        }
    }
    bool resolve(uintptr_t cameraStage,float* origin,float* axis) {
        if(!origin||!axis)return false;
        const auto state=unpackAerWeaponPairState(state_.load(std::memory_order_acquire));
        float heldOrigin[3]{},heldAxis[9]{};
        if(!poses_.resolve(state,cameraStage,origin,axis,heldOrigin,heldAxis))return false;
        std::memcpy(origin,heldOrigin,sizeof(heldOrigin));
        std::memcpy(axis,heldAxis,sizeof(heldAxis));
        return true;
    }
};
}

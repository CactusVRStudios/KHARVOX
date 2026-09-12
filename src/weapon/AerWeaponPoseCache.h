#pragma once
#include "AerWeaponPairPolicy.h"
#include <array>
#include <cstring>
#include <mutex>

namespace kharvox {
// Render work for the two eyes can run on different DOOM worker threads.
// Store final entity transforms per pair, not per worker.
class AerWeaponPoseCache {
    struct Entry {
        uintptr_t entity{};
        uint64_t serial{};
        bool valid{};
        float origin[3]{},axis[9]{};
    };
    std::array<Entry,128> entries_{};
    std::mutex mutex_;
public:
    bool resolve(AerWeaponPairState state,uintptr_t entity,const float* origin,
        const float* axis,float* heldOrigin,float* heldAxis) {
        if(!state.enabled||!entity||(state.eye!=0&&state.eye!=1))return false;
        std::lock_guard<std::mutex> lock(mutex_);
        Entry* found{}; Entry* reusable{};
        const auto first=aerWeaponPairHash(entity,entries_.size());
        for(size_t i=0;i<entries_.size();++i){
            auto& candidate=entries_[(first+i)%entries_.size()];
            if(candidate.valid&&candidate.serial==state.serial&&candidate.entity==entity){found=&candidate;break;}
            if(!reusable&&(!candidate.valid||candidate.serial!=state.serial))reusable=&candidate;
        }
        if(state.eye==0){
            auto* entry=found?found:reusable;
            // On overflow preserve existing entities rather than substituting
            // an unrelated pose. A missing left pose leaves the right unchanged.
            if(!entry)return false;
            entry->entity=entity;entry->serial=state.serial;entry->valid=true;
            std::memcpy(entry->origin,origin,sizeof(entry->origin));
            std::memcpy(entry->axis,axis,sizeof(entry->axis));
            return false;
        }
        if(!found)return false;
        std::memcpy(heldOrigin,found->origin,sizeof(found->origin));
        std::memcpy(heldAxis,found->axis,sizeof(found->axis));
        return true;
    }
};
}

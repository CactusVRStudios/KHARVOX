#pragma once
#include <memory>
#include <cstdint>
#include <algorithm>
namespace kharvox::native {
// Compare argument values, never descriptor contents. Even a match must still
// execute the bind so updated descriptors and snapshot validation are observed.
template<class T,class Point,class Layout,class Sets,class Offsets>
bool sameReplayArguments(const T& payload,Point point,Layout layout,uint32_t first,
                         const Sets& sets,const Offsets& offsets){
    return payload.point==point&&payload.layout==layout&&payload.first==first&&
        std::equal(payload.sets.begin(),payload.sets.end(),sets.begin(),sets.end())&&
        std::equal(payload.offsets.begin(),payload.offsets.end(),offsets.begin(),offsets.end());
}
// The slot owns one reference; the current state callback may own one more.
// Any captured initial state, final command or executing callback owns an
// additional reference and forces a new immutable payload for the next bind.
// currentStateOwnsSlot may only be true for the callback at this slot's key.
template<class T>
std::shared_ptr<T> prepareReplayPayload(std::shared_ptr<T>& slot,
                                      bool currentStateOwnsSlot,bool& reused){
    reused=slot&&(slot.use_count()==1||(currentStateOwnsSlot&&slot.use_count()==2));
    if(!reused)slot=std::make_shared<T>();
    return slot;
}
}

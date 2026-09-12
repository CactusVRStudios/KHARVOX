#pragma once
#include <cstdint>

namespace kharvox::native {
// Reuse only stored state, never suppress execution or a final-pass command.
// Copy the callback into history before executing, since execution may re-enter
// and replace its state node. Do not retain an iterator across execution.
template<class Replay,class Execute>
bool reuseReplayCommand(Replay& replay,uint64_t key,bool identical,const Execute& execute){
    if(!identical)return false;
    auto it=replay.state.find(key);if(it==replay.state.end())return false;
    if(replay.final&&!replay.replaying)replay.commands.emplace_back(it->second);
    execute();return true;
}

template<class Replay,class Cache,class Values,class StateFactory,class BatchFactory,class Execute>
uint64_t recordReplayBatchCached(Replay& replay,Cache& cache,uint64_t base,uint32_t first,uint32_t count,
                                const Values& values,const StateFactory& state,const BatchFactory& batch,
                                const Execute& execute){
    uint64_t reused=0;
    for(uint32_t i=0;i<count;++i){
        const auto key=base+uint64_t(first)+i;const auto value=values(i);
        const auto it=cache.find(key);
        if(it!=cache.end()&&it->second==value&&replay.state.count(key)!=0){++reused;continue;}
        replay.state[key]=state(i);cache[key]=value;
    }
    if(replay.final&&!replay.replaying)replay.commands.emplace_back(batch());
    execute();return reused;
}
// The tracked graphics pipeline is consumed by descriptor reflection and
// snapshot validation. It must follow execution, including deferred replay.
template<class Replay,class Pipeline,class Bind>
void executeReplayPipeline(Replay& replay,Pipeline pipeline,const Bind& bind){
    replay.pipeline=pipeline;
    bind();
}
// Preserve an existing exact range's state node. Other fully covered ranges
// are superseded. Validate partial overlaps before mutating either container.
template<class Replay>
bool prepareDescriptorRange(Replay& replay, uint32_t first, uint32_t count) {
    const uint64_t last = uint64_t(first) + count;
    for (const auto& range : replay.descriptorRanges) {
        const uint64_t begin = range.first, end = begin + range.second;
        if (first < end && last > begin && !(first <= begin && last >= end))
            return false;
    }
    for (auto it = replay.descriptorRanges.begin(); it != replay.descriptorRanges.end();) {
        const uint64_t begin = it->first, end = begin + it->second;
        if (first <= begin && last >= end && begin != first) {
            replay.state.erase(0x10000 + begin);
            it = replay.descriptorRanges.erase(it);
        } else ++it;
    }
    replay.descriptorRanges[first] = count;
    return true;
}
// Persist only the copies needed by the final-pass stream and state restore.
// Accept the concrete callable by reference: ordinary draws need no temporary
// std::function allocation. Store before invoking, as re-entry sees this state.
template<class Replay, class Command>
void recordReplayCommand(Replay& replay, uint64_t key, const Command& command,
                         bool saveState = true) {
    if (replay.final && !replay.replaying) replay.commands.emplace_back(command);
    if (saveState) replay.state[key] = command;
    command();
}
// Keep Vulkan array commands batched during recording. Individual entries
// are retained only for partial-overwrite state restoration. Deferred final
// replay owns one batch copy; ordinary recording borrows the caller's array.
template<class Replay,class StateFactory,class BatchFactory,class Execute>
void recordReplayBatch(Replay& replay,uint64_t base,uint32_t first,uint32_t count,
                       const StateFactory& state,const BatchFactory& batch,
                       const Execute& execute){
    for(uint32_t i=0;i<count;++i)replay.state[base+uint64_t(first)+i]=state(i);
    if(replay.final&&!replay.replaying)replay.commands.emplace_back(batch());
    execute();
}
}

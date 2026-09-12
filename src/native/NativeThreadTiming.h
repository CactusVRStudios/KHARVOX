#pragma once
#include <cstdint>
namespace kharvox::native {
struct ThreadTimingSample {uint64_t cpu100ns{},cycles{};uint32_t thread{};bool valid{};};
struct ThreadTimingDelta {uint64_t cpuNs{},cycles{};bool valid{};};
inline ThreadTimingDelta threadTimingDelta(ThreadTimingSample a,ThreadTimingSample b){
 if(!a.valid||!b.valid||a.thread!=b.thread||b.cpu100ns<a.cpu100ns||b.cycles<a.cycles)return {};
 return {(b.cpu100ns-a.cpu100ns)*100,b.cycles-a.cycles,true};
}
}

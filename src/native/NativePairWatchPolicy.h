#pragma once
#include <array>
#include <cmath>
#include <cstdint>
namespace kharvox::native {
// These are evidence triggers, not a classifier for rendering correctness.
// Motion, occlusion, particles and legitimate lighting can trigger them too.
struct PairWatchMetrics {
 std::array<float,32> eyeGap{};
 std::array<float,2> whiteFraction{};
};
inline bool pairWatchChange(const PairWatchMetrics& previous,const PairWatchMetrics& current){
 for(size_t i=0;i<2;++i)if(current.whiteFraction[i]-previous.whiteFraction[i]>0.12f)return true;
 for(size_t i=0;i<32;++i)if(std::abs(current.eyeGap[i]-previous.eyeGap[i])>18.0f)return true;
 return false;
}
struct PairWatchTrigger {
 uint64_t samples{},lastTrigger{};
 uint32_t events{},postFrames{};
 bool baseline{},pending{};
 // 8 preceding frames + trigger + 7 following frames. At most32 events.
 bool advance(bool candidate,bool manual){
  ++samples;
  if(pending){if(--postFrames==0){pending=false;++events;return true;}return false;}
  if(events>=32||samples<9)return false;
  if((!baseline&&samples>=120)||manual||(candidate&&samples-lastTrigger>=120)){
   baseline=true;pending=true;postFrames=7;lastTrigger=samples;
  }
  return false;
 }
};
}

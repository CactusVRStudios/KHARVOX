#pragma once
#include <cstdint>
namespace kharvox::native {
// Deliberately narrow diagnostic: the two atlas shapes observed in this
// DOOM build. Main-view depth and other formats are never committed back.
constexpr bool shadowHistoryShape(uint32_t width,uint32_t height,bool depth24,
                                 uint32_t mips,uint32_t layers,bool singleSample,
                                 bool sampledDepthOnly,bool transferable){
 return width==8192&&(height==8192||height==16384)&&depth24&&mips==1&&layers==1
     &&singleSample&&sampledDepthOnly&&transferable;
}
struct ShadowHistoryFrame {
 uint64_t frame{};bool recorded{};
 bool begin(uint64_t serial){if(recorded||!serial)return false;frame=serial;return true;}
 bool record(uint64_t serial,bool pairHanded,bool producersSubmitted){
  if(recorded||!frame||serial!=frame||!pairHanded||!producersSubmitted)return false;
  recorded=true;return true;
 }
 bool finish(bool gpuComplete){if(!recorded||!gpuComplete)return false;recorded=false;return true;}
};
}

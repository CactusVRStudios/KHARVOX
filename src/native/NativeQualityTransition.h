#pragma once
#include <cstdint>

namespace kharvox::native {
// Caller serializes access. Only completed downstream Presents count, and a
// resource mutation between Present entry and completion invalidates that frame.
class QualityTransition {
 uint64_t epoch_{};
 uint32_t mutations_{},stable_{};
 bool active_{};
public:
 bool begin(){const bool first=!active_;active_=true;++epoch_;stable_=0;return first;}
 void mutation(bool entering){
  if(entering)++mutations_;else if(mutations_)--mutations_;
  if(active_){++epoch_;stable_=0;}
 }
 uint64_t epoch()const{return active_?epoch_:0;}
 bool presented(uint64_t observed,bool success){
  if(!active_||!observed)return false;
  if(!success||mutations_||observed!=epoch_){stable_=0;return false;}
  if(++stable_<3)return false;
  active_=false;return true;
 }
};
}

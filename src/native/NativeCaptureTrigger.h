#pragma once
#include <array>
#include <cmath>
#include <cstdint>
namespace kharvox::native {
struct HandCaptureTrigger {
 uint64_t due{};uint32_t count{};bool down{};
 bool update(bool pressed,bool gameplay,uint64_t now){
  const bool edge=pressed&&!down;down=pressed;
  if(!gameplay){due=0;return false;}
  if(edge&&count<3&&!due)due=now+2000;
  if(!due||now<due)return false;
  due=0;++count;return true;
 }
};
inline bool nativeCaptureRequested(uint32_t captured,bool sequence,bool once){
 return (sequence||once)&&captured<(sequence?120u:1u);
}
// Quaternion signs represent the same rotation. Accept scaled finite inputs,
// reject missing/invalid poses, and trigger after two degrees from the anchor.
inline bool capturePoseMoved(const std::array<float,4>& anchor,const std::array<float,4>& pose){
 double dot{},a{},b{};for(size_t i=0;i<4;++i){if(!std::isfinite(anchor[i])||!std::isfinite(pose[i]))return false;dot+=double(anchor[i])*pose[i];a+=double(anchor[i])*anchor[i];b+=double(pose[i])*pose[i];}
 return a>1e-12&&b>1e-12&&std::abs(dot)/std::sqrt(a*b)<0.9998476951563913;
}
}

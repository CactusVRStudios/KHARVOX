#include "../src/native/NativeCaptureTrigger.h"
#include <limits>
#include <iostream>
int main(){using namespace kharvox::native;int errors{};auto check=[&](bool b){errors+=!b;};
 HandCaptureTrigger hand;
 check(!hand.update(true,false,100));check(!hand.update(false,true,101));
 check(!hand.update(true,true,200));check(!hand.update(true,true,2199));
 check(hand.update(true,true,2200));check(!hand.update(true,true,5000));
 check(!hand.update(false,true,5001));check(!hand.update(true,true,5002));
 check(!hand.update(false,false,5003));check(!hand.update(false,true,8000));
 check(hand.count==1);
 for(uint64_t now:{10000ull,20000ull}){
  check(!hand.update(true,true,now));check(hand.update(false,true,now+2000));
 }
 check(hand.count==3);check(!hand.update(true,true,30000));check(!hand.update(false,true,40000));
 const std::array<float,4> identity{0,0,0,1};
 check(!nativeCaptureRequested(0,false,false));check(nativeCaptureRequested(0,false,true));
 check(!nativeCaptureRequested(1,false,true));check(nativeCaptureRequested(1,true,false));
 check(nativeCaptureRequested(119,true,true));check(!nativeCaptureRequested(120,true,true));
 check(!capturePoseMoved(identity,identity));check(!capturePoseMoved(identity,{0,0,0,-1}));check(!capturePoseMoved(identity,{0,0,0,2}));
 check(!capturePoseMoved(identity,{0,0,0,0}));check(!capturePoseMoved(identity,{0,std::numeric_limits<float>::quiet_NaN(),0,1}));
 check(!capturePoseMoved(identity,{0,0.008726535f,0,0.999961923f}));
 check(capturePoseMoved(identity,{0,0.026176948f,0,0.999657325f}));
 check(capturePoseMoved(identity,{0,-0.052353896f,0,-1.999314650f}));
 std::cout<<"Motion capture trigger errors="<<errors<<'\n';return errors?1:0;}

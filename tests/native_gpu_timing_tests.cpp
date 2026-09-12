#include "../src/native/NativeGpuTimingPolicy.h"
#include <cstdlib>
#include <iostream>
using namespace kharvox::native;
static void require(bool ok){if(!ok)std::abort();}
int main(){
 require(timestampMilliseconds(100,1100,true,true,64,1)==0.001);
 require(timestampMilliseconds(250,5,true,true,8,1000)==0.011);
 require(timestampMilliseconds(UINT64_MAX-4,5,true,true,64,1000)==0.01);
 require(!timestampMilliseconds(1,2,false,true,64,1));
 require(!timestampMilliseconds(1,2,true,false,64,1));
 require(!timestampMilliseconds(1,2,true,true,0,1));
 require(!timestampMilliseconds(1,2,true,true,65,1));
 require(!timestampMilliseconds(1,2,true,true,64,0));
 require(!gpuTimingSampleDue(false,120,0));require(!gpuTimingSampleDue(true,0,0));
 require(!gpuTimingSampleDue(true,119,0));require(gpuTimingSampleDue(true,120,0));
 require(gpuTimingSampleDue(true,240,95));require(!gpuTimingSampleDue(true,240,96));
 uint32_t samples=0;for(uint64_t frame=1;frame<=20000;++frame)if(gpuTimingSampleDue(true,frame,samples))++samples;
 require(samples==96);
 std::cout<<"GPU timestamp availability, width, rollover and period checks passed\n";
}

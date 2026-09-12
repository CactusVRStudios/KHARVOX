#include "../src/common/PoseTrace.h"
#include <sstream>
#include <thread>
#include <iostream>
int main(){
 SetEnvironmentVariableA("KHARVOX_EXTENDED_LOGGING","0");
 using namespace kharvox::pose_trace;
 int failures=0;auto check=[&](bool ok){if(!ok)++failures;};
 check(!kharvox::extendedDiagnosticsEnabled() && poll(true,true).empty() && !active.load());
 Event e{};e.kind=Camera;e.eye=1;e.frame=123;e.data[0]=1.25f;e.poseId=9007199254740993ULL;
 check(!record(e));begin();
 // Concurrent producers must never overwrite records or exceed the bound.
 auto producer=[&]{for(unsigned i=0;i<2000;++i)record(e);};
 std::thread a(producer),b(producer);a.join();b.join();stop();
 check(count+dropped.load()==4000);check(count>0);
 for(size_t i=0;i<count;++i){check(events[i].frame==123&&events[i].data[0]==1.25f&&events[i].poseId==e.poseId);if(i)check(events[i].qpc>=events[i-1].qpc);}
 const auto held=count;check(!record(e)&&count==held);
 std::ostringstream csv;writeCsv(csv);check(csv.str().find("source,revision,status,poseId,d0")!=std::string::npos);
 check(csv.str().find(",9007199254740993,1.25")!=std::string::npos);
 begin();for(size_t i=0;i<capacity+5;++i)record(e);stop();
 check(count==capacity&&dropped==5);
 begin();check(count==0&&dropped==0);
 unsigned char cameraBytes[0x100]{};
 camera(1,2,cameraBytes,.032f,5,3,4,0); // AER left has positive DOOM-left offset
 camera(1,2,cameraBytes,-.032f,5,3,4,1);
 camera(1,2,cameraBytes,-.032f,1,3,4); // Native legacy trace convention unchanged
 stop();check(count==3&&events[0].eye==0&&events[1].eye==1&&events[2].eye==0);
 std::cout<<"pose trace failures="<<failures<<'\n';return failures?1:0;
}

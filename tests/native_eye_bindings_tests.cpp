#include "../src/native/NativeEyeBindings.h"
#include <array>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <iostream>
static void check(bool v,const char* message){if(!v)throw std::runtime_error(message);}
int main(){try{
 using States=std::map<int,std::function<void()>>;
 std::set<int> entered;
 int eye=0,boundCamera=-1,boundVertex=-1,binds=0;
 const std::array<int,2> cameraSnapshots{100,101},vertexMirrors{200,201};
 States commands{{1,[&]{boundVertex=vertexMirrors[eye];++binds;}},
                 {2,[&]{boundCamera=cameraSnapshots[eye];++binds;}}};
 // Left frame binds private snapshots; the engine remembers only the same
 // original handles, so it emits no bind when it enters the right depth pass.
 for(auto& [key,fn]:commands)fn();
 eye=1;
 check(boundCamera==100,"regression setup must reproduce a stale left binding");
 check(kharvox::native::restoreEyeBindingsOnce(7,entered,commands),"first right pass did not restore bindings");
 check(boundCamera==101&&boundVertex==201,"right depth draws still consume left resources");
 check(!kharvox::native::restoreEyeBindingsOnce(7,entered,commands)&&binds==4,"later pass replays obsolete state");
 States other{{1,[&]{++binds;}}};
 check(kharvox::native::restoreEyeBindingsOnce(8,entered,other)&&binds==5,"each command buffer has independent Vulkan state");
 entered.clear();boundCamera=100;
 check(kharvox::native::restoreEyeBindingsOnce(7,entered,commands)&&boundCamera==101,"new frame retained old command-buffer admission");
 // A binding explicitly changed before the first pass replaces the inherited
 // command. Restore the current logical state, never a frozen left command list.
 entered.clear();commands[2]=[&]{boundCamera=999;};
 kharvox::native::restoreEyeBindingsOnce(7,entered,commands);
 check(boundCamera==999,"explicit right binding was overwritten with left state");
 entered.clear();int executed=0;
 States recursive;
 recursive[1]=[&]{recursive.clear();++executed;};
 recursive[2]=[&]{++executed;};
 kharvox::native::restoreEyeBindingsOnce(9,entered,recursive);
 check(executed==2,"reentrant bookkeeping invalidated pending state callbacks");
 std::cout<<"Native inherited eye bindings passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

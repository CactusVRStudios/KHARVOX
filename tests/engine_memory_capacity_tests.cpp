#include "../src/vulkan/EngineMemoryCapacity.h"
#include <cstdlib>
#include <initializer_list>

void require(bool condition){if(!condition)std::abort();}
int main(){
    constexpr uint64_t mib=1024*1024;
    // Captured failing simulator/VDXR allocation versus a successful smaller one.
    require(kharvox::oversizedEnginePoolImage(186122240,4,true,128*mib));
    require(!kharvox::oversizedEnginePoolImage(56229888,4,true,128*mib));
    require(!kharvox::oversizedEnginePoolImage(128*mib,4,true,128*mib));
    require(kharvox::oversizedEnginePoolImage(128*mib+1,4,true,128*mib));
    require(!kharvox::oversizedEnginePoolImage(186122240,5,true,128*mib));
    require(!kharvox::oversizedEnginePoolImage(186122240,4,false,128*mib));
    require(!kharvox::oversizedEnginePoolImage(186122240,4,true,0));
    require(!kharvox::oversizedEnginePoolImage(186122240,4,true,256*mib));
    require(kharvox::enginePoolCapacity(4,128*mib,64*mib)==128*mib);
    for(auto flags:{2u,8u,16u,6u,12u,20u})
        require(kharvox::enginePoolCapacity(flags,128*mib,64*mib)==64*mib);
    require(kharvox::enginePoolCapacity(4,512*mib,256*mib)==512*mib);
}

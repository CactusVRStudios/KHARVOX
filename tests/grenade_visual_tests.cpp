#include "../src/hands/GrenadeVisualPolicy.h"
#include <sstream>

int main(){
    using namespace kharvox::hands;
    GrenadeVisualConfig config;
    std::istringstream valid("1 1 42 99\n");
    if(!readGrenadeVisualConfig(valid,config)||!config.enabled||config.handHash!=42||config.grenadeHash!=99)return 1;
    for(const char* text:{"", "2 1 42 99", "1 2 42 99", "1 1 0 99", "1 1 42 0", "1 1 42", "1 1 42 99 extra"}){
        GrenadeVisualConfig unchanged;unchanged.handHash=7;
        std::istringstream invalid(text);
        if(readGrenadeVisualConfig(invalid,unchanged)||unchanged.enabled||unchanged.handHash!=7)return 2;
    }
    std::istringstream disabled("1 0 42 99");
    if(!readGrenadeVisualConfig(disabled,config)||config.enabled)return 3;
    for(int mask=0;mask<64;++mask){
        const bool ready=mask&1,leftHanded=mask&2,held=mask&4,twoHanded=mask&8,left=mask&16,free=mask&32;
        const bool result=grenadeVisualEligible(ready,leftHanded,held,twoHanded,left,free);
        if(result&&(!ready||leftHanded||!held||twoHanded||!left||!free))return 4;
    }
    if(!grenadeVisualEligible(true,false,true,false,true,true))return 5;
    return 0;
}

#include "../src/openxr/FreshAerHandsPolicy.h"
#include <cstdlib>
void require(bool value){if(!value)std::abort();}
int main(){
    using namespace kharvox;
    require(freshAerHandsEnabled(true,false,true,true,false,false));
    require(!freshAerHandsEnabled(true,true,true,true,false,false));
    require(!freshAerHandsEnabled(false,false,true,true,false,false));
    require(!freshAerHandsEnabled(true,false,false,true,false,false));
    require(!freshAerHandsEnabled(true,false,true,false,false,false));
    require(!freshAerHandsEnabled(true,false,true,true,true,false));
    require(!freshAerHandsEnabled(true,false,true,true,false,true));
    // First capture cannot expose a mixed eye pair. The completed right pair
    // is copied; on the following left capture only the clean published world
    // is reused while both XR targets receive new hands.
    require(!freshAerHandsUpdateTargets(true,true,false));
    require(freshAerHandsUpdateTargets(false,true,false));
    require(freshAerHandsUpdateTargets(true,true,true));
    require(freshAerHandsUpdateTargets(false,true,true));
    // Baseline retains its original acquire/reuse cadence.
    require(!freshAerHandsUpdateTargets(true,false,true));
    require(freshAerHandsUpdateTargets(false,false,true));
}

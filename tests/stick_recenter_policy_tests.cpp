#include "../src/openxr/StickRecenterPolicy.h"
#include <cstdlib>
#include <initializer_list>
namespace { void check(bool value){if(!value)std::abort();} }
int main() {
    constexpr std::int64_t ms=1000000;
    kharvox::StickRecenterPolicy sticks;
    auto out=sticks.update(0,true,false);
    check(!out.primary&&!out.support&&!out.recenter);
    out=sticks.update(50*ms,true,true);
    check(out.recenter&&!out.primary&&!out.support);
    for(int i=60;i<1000;i+=10){
        out=sticks.update(i*ms,true,true);
        check(!out.recenter&&!out.primary&&!out.support);
    }
    check(!sticks.update(1000*ms,false,true).support); // require both releases
    sticks.update(1100*ms,false,false);
    check(sticks.update(1200*ms,true,true).recenter);

    for(bool primary:{false,true}){ // single pause/melee in both handed layouts
        sticks={};
        sticks.update(0,primary,!primary);
        out=sticks.update(151*ms,primary,!primary);
        check(out.primary==primary&&out.support!=primary&&!out.recenter);
        out=sticks.update(200*ms,false,false);
        check(!out.primary&&!out.support);
        // Quick taps are replayed once, even if already released at 150 ms.
        sticks.update(300*ms,primary,!primary);
        sticks.update(320*ms,false,false);
        out=sticks.update(451*ms,false,false);
        check(out.primary==primary&&out.support!=primary);
        out=sticks.update(552*ms,false,false);
        check(!out.primary&&!out.support);
    }
    sticks={};sticks.update(0,true,false);
    out=sticks.update(151*ms,true,false);check(out.primary);
    out=sticks.update(200*ms,true,true);
    check(!out.recenter&&out.primary&&out.support); // late second click is classical
    sticks.cancel();out=sticks.update(300*ms,true,true);
    check(!out.recenter&&!out.primary&&!out.support); // focus loss
    sticks.update(400*ms,false,false);
    check(sticks.update(500*ms,true,true).recenter);
}

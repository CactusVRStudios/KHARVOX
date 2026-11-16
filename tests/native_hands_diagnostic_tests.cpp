#include "../src/hands/NativeHandsDiagnosticPolicy.h"
#include <cstdlib>

using namespace kharvox::hands;
int main(){
    auto check=[](bool condition){if(!condition)std::abort();};
    NativeHandsDiagnosticPolicy policy;
    NativeHandsDiagnosticKey gameplay{1,2,3,1,15,0,1,2};
    check(!policy.sample(false,true,100,gameplay));
    check(!policy.sample(true,false,100,gameplay));
    check(policy.count()==0);
    check(policy.sample(true,true,100,gameplay));
    // Eye/present count is deliberately absent from the key: an unchanged
    // second eye or a fast render worker must not flood the trace.
    for(unsigned tick=101;tick<1100;++tick)check(!policy.sample(true,true,tick,gameplay));
    check(policy.sample(true,true,1100,gameplay));
    auto changed=gameplay;changed.maskBefore=3;
    check(!policy.sample(true,true,1101,changed));
    check(!policy.sample(true,true,1199,changed));
    check(policy.sample(true,true,1200,changed));
    // Entry and exit of a short interaction are retained immediately.
    changed.context=3;check(policy.sample(true,true,1201,changed));
    changed.context=1;check(policy.sample(true,true,1202,changed));
    changed.model=4;check(policy.sample(true,true,1203,changed));
    changed.animator=5;check(policy.sample(true,true,1204,changed));
    changed.owner=6;check(policy.sample(true,true,1205,changed));
    changed.level=2;check(policy.sample(true,true,1206,changed));
    changed.weapon=3;check(policy.sample(true,true,1207,changed));
    // A changed AFTER mask matters even if the native mask is unchanged.
    changed.maskAfter=1;check(!policy.sample(true,true,1208,changed));
    check(policy.sample(true,true,1307,changed));
    check(!policy.sample(true,true,1,changed)); // Clock rollback cannot underflow.
    const auto beforeDisabled=policy.count();
    changed.level=3;check(!policy.sample(false,true,1400,changed));
    check(policy.count()==beforeDisabled);
    check(policy.sample(true,true,1400,changed));
    // A pathological stream of owner/model/context changes remains bounded.
    for(unsigned i=0;i<1000;++i){changed.level=4+i;policy.sample(true,true,1401+i,changed);}
    check(policy.count()==NativeHandsDiagnosticPolicy::maximumSamples);
    check(!policy.sample(true,true,100000,gameplay));
}

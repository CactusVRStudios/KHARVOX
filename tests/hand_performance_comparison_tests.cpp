#include "../src/openxr/HandPerformanceComparison.h"
#include <cstdlib>
#include <limits>
namespace {void check(bool value){if(!value)std::abort();}}
int main(){
    using namespace kharvox;
    HandPerformanceComparison disabled;
    check(!disabled.update(100,true,true)&&!disabled.hideHands()&&disabled.phase()==-1);
    HandPerformanceComparison c(true);
    check(!c.update(0,false,true)&&c.phase()==-1);
    check(!c.update(10,true,false)&&c.phase()==-1);
    check(c.update(100,true,true)&&c.phase()==0&&!c.hideHands());
    c.update(2100,true,true);c.gpu(100);check(c.windows[0].xrGpu.count==0);
    c.update(2110,true,true);c.gpu(1);c.xr(2);
    check(c.windows[0].frames.count==1&&c.windows[0].frames.meanMs()==10);
    check(c.windows[0].xrGpu.meanMs()==1&&c.windows[0].xrWall.meanMs()==2);
    check(c.update(14100,true,true)&&c.phase()==1&&c.hideHands());
    c.gpu(100);check(c.windows[1].xrGpu.count==0); // no mixed transition sample
    c.update(16100,true,true);c.update(16110,true,true);c.gpu(.5);
    check(c.windows[1].frames.count==1);
    c.update(16200,false,true);check(!c.hideHands());
    c.update(30000,true,true);check(c.hideHands());
    c.update(31000,true,true);c.gpu(100);
    check(c.windows[1].frames.count==1&&c.windows[1].xrGpu.count==1);
    c.update(32000,true,true);c.update(32010,true,true);
    check(c.windows[1].frames.count==2);
    check(c.update(44000,true,true)&&c.phase()==2&&!c.hideHands());
    check(c.update(58000,true,true)&&c.finished()&&!c.hideHands());
    c.update(60000,true,true);c.gpu(100);check(c.windows[2].xrGpu.count==0);
    PerformanceSamples s;s.add(-1);s.add(std::numeric_limits<double>::infinity());
    check(s.count==0);s.add(0);s.add(2);check(s.count==2&&s.meanMs()==1&&s.maximumMs==2);
}

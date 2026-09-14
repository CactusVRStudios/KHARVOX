#include "../src/weapon/LaserSourcePolicy.h"
#include "../src/common/AerSourceTracking.h"
#include <stdexcept>
static void check(bool v){if(!v)throw std::runtime_error("laser source invariant");}
int main(){
    using namespace kharvox;
    check(laserSourceUsable(100,116,3,3,2,2));
    check(!laserSourceUsable(100,116,3,4,2,2)); // reload/controller reset
    check(!laserSourceUsable(100,116,3,3,2,3)); // weapon switch
    check(!laserSourceUsable(100,201,3,3,2,2)); // stopped producer
    check(!laserSourceUsable(0,16,3,3,2,2));
    check(!laserSourceUsable(100,99,3,3,2,2));
    AerInputHistory<int> history;
    history.remember({10,1,0},100);
    history.remember({11,1,1},111);
    int sample=0;
    check(history.find({10,1,0},sample)&&sample==100); // do not use newest eye
    check(!history.find({10,1,1},sample));
    check(!history.find({10,2,0},sample)); // level generation
    for(unsigned i=20;i<160;++i)history.remember({i,1,0},int(i));
    check(!history.find({10,1,0},sample)); // bounded history
}

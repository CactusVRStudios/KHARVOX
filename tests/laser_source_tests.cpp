#include "../src/weapon/LaserSourcePolicy.h"
#include "../src/common/AerSourceTracking.h"
#include <stdexcept>
#include "../src/weapon/LaserWorldPose.h"
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
    // World origins must not be rejected by a model-local +/-300 bound.
    // Round-trip rotated/scaled owner bases, including the 0.77 weapon scale.
    for(float scale : {.77f,1.f,1.1f})for(float angle : {-1.f,0.f,1.f}){
        const float c=std::cos(angle),s=std::sin(angle);
        const float owner[9]{c*scale,s*scale,0,-s*scale,c*scale,0,0,0,scale};
        const float origin[3]{2951,-1513,-350};
        const float expected[3]{22,3,-2};
        float world[3]{},axis[9]{},local[3]{},localAxis[9]{};
        for(int i=0;i<3;++i){world[i]=origin[i];for(int j=0;j<3;++j)world[i]+=expected[j]*owner[j*3+i];}
        for(int i=0;i<9;++i)axis[i]=owner[i];
        check(laserWorldToLocal(world,axis,origin,owner,local,localAxis));
        for(int i=0;i<3;++i)check(std::abs(local[i]-expected[i])<.001f);
        for(int i=0;i<9;++i)check(std::abs(localAxis[i]-(i%4==0?1.f:0.f))<.0001f);
    }
    float zero[9]{},result[9]{};
    check(!laserWorldToLocal(zero,zero,zero,zero,result,result));
    {
        float source[3]{10,20,30},scaled[9]{2,0,0,0,2,0,0,0,2};
        float target[3]{100,200,300},turned[9]{0,2,0,-2,0,0,0,0,2};
        float muzzle[3]{14,20,30},direction[3]{1,0,0};
        check(laserFollowDraw(source,scaled,target,turned,muzzle,direction));
        check(std::abs(muzzle[0]-100)<.001f&&std::abs(muzzle[1]-204)<.001f&&std::abs(muzzle[2]-300)<.001f);
        check(std::abs(direction[0])<.001f&&std::abs(direction[1]-1)<.001f&&std::abs(direction[2])<.001f);
        check(!laserFollowDraw(source,zero,target,turned,muzzle,direction));
        check(muzzle[1]==204&&direction[1]==1); // rejection must not modify output
    }
}

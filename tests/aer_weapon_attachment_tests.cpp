#include "../src/weapon/AerWeaponAttachments.h"
#include <cstdlib>
#include <cstdio>
#include <limits>
#define check(v) do { if(!(v)){ std::fprintf(stderr,"attachment check failed at line %d\n",__LINE__); std::abort(); } } while(false)
int main(){
    using namespace kharvox;
    const std::array<float,12> identity{0,0,0,1,0,0,0,1,0,0,0,1};
    AerWeaponAttachments h;auto original=identity,target=identity,child=identity,camera=identity;
    std::array<float,12> out{};AerSourceKey key{1,1,1};
    for(unsigned i=0;i<6;++i){
        key.poseId=1+i*2;original[0]=float(i);target[0]=original[0]+100;child[0]=original[0]+2;
        h.parent(10,11,key,i,original.data(),target.data(),camera.data());
        check(h.resolve(20,21,key,i,child.data(),out.data())==(i==5));
    }
    check(std::abs(out[0]-107)<.0001f);
    // Both eyes use their matching source; no previous-eye parent is allowed.
    key.eye=0;check(!h.resolve(20,21,key,6,child.data(),out.data()));
    h.parent(10,11,key,6,original.data(),target.data(),camera.data());
    check(h.resolve(20,21,key,6,child.data(),out.data())&&std::abs(out[0]-107)<.0001f);
    child[0]+=.01f;check(h.resolve(20,21,key,6,child.data(),out.data()));
    check(std::abs(out[0]-107.01f)<.0001f); // preserve current local animation
    check(!h.resolve(20,21,key,7,child.data(),out.data())); // stale parent
    check(!h.resolve(20,22,key,6,child.data(),out.data())); // replaced GUI
    // Preserve a scaled child axis while rotating it with the corrected parent.
    AerWeaponAttachments rotated;
    for(unsigned i=0;i<6;++i){
        key={1+i*2,1,1};original=identity;original[0]=float(i);
        target={100,0,0,0,1,0,-1,0,0,0,0,1};
        child=original;child[0]+=2;child[3]=2;
        rotated.parent(10,11,key,i,original.data(),target.data(),camera.data());
        check(rotated.resolve(20,21,key,i,child.data(),out.data())==(i==5));
    }
    check(std::abs(out[0]-100)<.0001f&&std::abs(out[1]-2)<.0001f);
    check(std::abs(out[3])<.0001f&&std::abs(out[4]-2)<.0001f);
    // Scaled weapon parents must retain their independent HUD correction.
    for(float size:{.7f,.77f,1.f,1.1f}){
        AerWeaponAttachments scaled;
        for(unsigned i=0;i<6;++i){
            key={1+i*2,1,int(i%2)};original=identity;original[0]=float(i);
            target={100,0,0,0,1,0,-1,0,0,0,0,1};
            for(int j=3;j<12;++j){original[j]*=size;target[j]*=size;}
            child=identity;child[0]=original[0]+2*size;
            scaled.parent(10,11,key,i,original.data(),target.data(),camera.data());
            check(scaled.resolve(20,21,key,i,child.data(),out.data())==(i==5));
        }
        check(std::abs(out[0]-100)<.0001f&&std::abs(out[1]-2*size)<.0001f);
        check(std::abs(out[4]-1)<.0001f); // no double-scaling of HUD
    }
    original=target=child=identity;
    AerWeaponAttachments headlocked,wall,ambiguous;
    for(unsigned i=0;i<10;++i){
        key={1+i*2,1,1};original[0]=float(i);target[0]=100+float(i);camera[0]=float(i);child[0]=float(i)+2;
        headlocked.parent(10,11,key,i,original.data(),target.data(),camera.data());
        check(!headlocked.resolve(20,21,key,i,child.data(),out.data()));
        camera[0]=0;wall.parent(10,11,key,i,original.data(),target.data(),camera.data());
        auto stationary=identity;stationary[0]=2;
        check(!wall.resolve(20,21,key,i,stationary.data(),out.data()));
        ambiguous.parent(10,11,key,i,original.data(),target.data(),camera.data());
        auto other=target;other[0]+=10;
        ambiguous.parent(12,13,key,i,original.data(),other.data(),camera.data());
        check(!ambiguous.resolve(20,21,key,i,child.data(),out.data()));
    }
    key.domain=1;check(!h.resolve(20,21,key,6,child.data(),out.data()));
    key.domain=0;key.level=2;check(!h.resolve(20,21,key,6,child.data(),out.data()));
    child[0]=std::numeric_limits<float>::quiet_NaN();check(!h.resolve(20,21,key,6,child.data(),out.data()));
}

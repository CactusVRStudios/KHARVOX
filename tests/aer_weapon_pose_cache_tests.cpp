#include "../src/weapon/AerWeaponPoseCache.h"
#include "../src/common/AerRenderOrder.h"
#include <thread>
#include <cstdlib>

void require(bool value){if(!value)std::abort();}
int main(){
    // Physical right/left images keep their eye indices. Only pair lifetime
    // and capture/reuse phase change when the render order is reversed.
    {
        using namespace kharvox;
        AerWeaponPoseCache ordered;
        uint64_t packed=packAerWeaponPairState({1,-1,false});
        const float axis[9]{1,0,0,0,1,0,0,0,1};
        float held[3]{},heldAxis[9]{};
        for(int pair=0;pair<3;++pair){
            const float first[3]{float(pair+10),0,0},second[3]{-1,0,0};
            packed=nextAerWeaponPairState(packed,aerRenderPairPhase(aerFirstRenderEye),true);
            const auto begin=unpackAerWeaponPairState(packed);
            require(begin.eye==0);
            require(!ordered.resolve(begin,42,first,axis,held,heldAxis));
            packed=nextAerWeaponPairState(packed,aerRenderPairPhase(aerSecondRenderEye),true);
            const auto end=unpackAerWeaponPairState(packed);
            require(end.eye==1&&end.serial==begin.serial);
            require(ordered.resolve(end,42,second,axis,held,heldAxis));
            require(held[0]==first[0]);
        }
        require(aerFirstRenderEye==1&&aerSecondRenderEye==0);
    }
    kharvox::AerWeaponPoseCache cache;
    const float left[3]{1,2,3},right[3]{4,5,6};
    const float axis[9]{1,0,0,0,1,0,0,0,1};
    float out[3]{},outAxis[9]{};
    std::thread first([&]{require(!cache.resolve({1,0,true},42,left,axis,out,outAxis));});
    first.join();
    std::thread second([&]{require(cache.resolve({1,1,true},42,right,axis,out,outAxis));});
    second.join();
    require(out[0]==1&&out[1]==2&&out[2]==3&&outAxis[8]==1);
    // New pair, another object, disabled pairing and missing-left must not
    // reuse a transform from the previous pair or an unrelated render object.
    require(!cache.resolve({2,1,true},42,right,axis,out,outAxis));
    require(!cache.resolve({1,1,true},43,right,axis,out,outAxis));
    require(!cache.resolve({1,1,false},42,right,axis,out,outAxis));
    require(!cache.resolve({2,0,true},42,right,axis,out,outAxis));
    require(cache.resolve({2,1,true},42,left,axis,out,outAxis));
    require(out[0]==4);
    // Repeated left updates publish the final transform; overflow does not
    // replace a valid entity with another object's pose.
    require(!cache.resolve({2,0,true},42,left,axis,out,outAxis));
    require(cache.resolve({2,1,true},42,right,axis,out,outAxis));
    require(out[0]==1);
    for(uintptr_t key=100;key<300;++key)cache.resolve({2,0,true},key,right,axis,out,outAxis);
    require(cache.resolve({2,1,true},42,right,axis,out,outAxis));
    require(out[0]==1);
}

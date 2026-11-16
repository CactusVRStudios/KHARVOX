#include "../src/openxr/RoomscaleLocomotionPolicy.h"
#include <cstdlib>
#include <limits>
#include <initializer_list>
int main(){
    auto check=[](bool condition){if(!condition)std::abort();};
    for(float displacement:{0.f,.0025f,.03f,.09f,.2f,1.f})
        check(!kharvox::allowRoomscaleMovement(false,true,true,false,displacement));
    check(!kharvox::allowRoomscaleMovement(true,true,true,false,.09f));
    check(kharvox::allowRoomscaleMovement(true,true,true,false,.2f));
    check(!kharvox::allowRoomscaleMovement(true,true,true,true,.2f));
    check(!kharvox::allowRoomscaleMovement(true,false,true,false,.2f));
    check(!kharvox::allowRoomscaleMovement(true,true,false,false,.2f));
    check(!kharvox::allowRoomscaleMovement(true,true,true,false,std::numeric_limits<float>::quiet_NaN()));
}

#include "../src/openxr/GrenadeThrowPolicy.h"
#include "../src/weapon/GrenadeThrowAim.h"
#include <cstdlib>
#include <limits>
namespace { void check(bool value){if(!value)std::abort();} }
int main() {
    constexpr std::int64_t ms=1000000;
    kharvox::GrenadeThrowPolicy gesture;
    check(gesture.update(0,false,true,true,false,true,{}).equipment); // classic held button
    check(!gesture.update(10*ms,false,true,false,false,true,{}).equipment);
    check(!gesture.update(20*ms,true,true,true,true,true,{0,0,-2}).equipment);
    check(gesture.holding());
    gesture.update(150*ms,true,true,true,true,true,{0,0,-3});
    auto out=gesture.update(180*ms,true,true,false,true,true,{0,0,-3});
    check(out.equipment&&out.physical&&out.velocity[2]==-3);
    check(!gesture.holding());
    out=gesture.update(200*ms,true,true,false,true,true,{});
    check(out.equipment&&!out.physical); // one aim event per release
    check(!gesture.update(281*ms,true,true,false,true,true,{}).equipment);
    gesture={};gesture.update(0,true,true,true,true,true,{});
    out=gesture.update(20*ms,true,true,false,true,true,{});
    check(out.equipment&&!out.physical); // quick tap retains native throw
    gesture={};gesture.update(0,true,true,true,true,true,{0,0,-3});
    out=gesture.update(150*ms,true,true,false,false,true,{0,0,-3});
    check(out.equipment&&!out.physical); // tracking loss falls back to button
    gesture={};gesture.update(0,true,true,true,true,false,{0,0,-3});
    check(!gesture.holding()); // no held visual while supporting a weapon
    out=gesture.update(150*ms,true,true,false,true,false,{0,0,-3});
    check(out.equipment&&!out.physical); // two-hand support cannot steer grenade
    gesture={};gesture.update(0,true,true,true,true,true,{0,0,-3});
    gesture.update(150*ms,true,false,true,true,true,{0,0,-3});
    check(!gesture.holding()); // menus remove the held visual without throwing
    out=gesture.update(200*ms,true,true,false,true,true,{0,0,-3});
    check(!out.equipment&&!out.physical); // menus cancel without a delayed throw
    gesture={};gesture.update(0,true,true,true,true,true,{0,0,-100});
    out=gesture.update(150*ms,true,true,false,true,true,{0,0,-3});
    check(out.equipment&&!out.physical); // tracking spike cannot steer

    kharvox::GrenadeThrowAim aim;std::array<float,9> axis{};
    const float forward[]{1,0,0};check(aim.queue(forward,100));
    check(!aim.take(200,false,axis)); // other player/caller/message is untouched
    check(aim.take(200,true,axis));
    for(int i=0;i<9;++i)check(std::abs(axis[i]-(i%4==0?1.f:0.f))<.0001f);
    check(!aim.take(201,true,axis));
    check(aim.queue(forward,300));check(!aim.take(1301,true,axis));
    const float invalid[]{std::numeric_limits<float>::quiet_NaN(),0,1};
    check(!aim.queue(invalid,0));
    for(const auto direction:{std::array<float,3>{0,0,1},std::array<float,3>{0,0,-1},
            std::array<float,3>{0,-1,0},std::array<float,3>{1,2,3}}){
        check(aim.queue(direction.data(),0)&&aim.take(1,true,axis));
        for(int row=0;row<3;++row)for(int other=0;other<3;++other){
            float dot{};for(int i=0;i<3;++i)dot+=axis[row*3+i]*axis[other*3+i];
            check(std::abs(dot-(row==other?1.f:0.f))<.0001f);
        }
    }
}

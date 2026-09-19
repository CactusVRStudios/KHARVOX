#include "../src/camera/CameraPoseState.h"
#include <atomic>
#include <cassert>
#include <thread>
#include <vector>

int main(){
    kharvox::CameraPoseState state;
    assert(!state.read().valid);
    auto stale=state.read();stale.valid=true;
    state.invalidate();assert(!state.publish(stale));assert(!state.read().valid);
    auto current=state.read();current.valid=true;current.anchorOwner=1;
    assert(state.publish(current));assert(state.read().valid);
    std::atomic<bool> start{},done{};
    std::vector<std::thread> readers;
    for(unsigned n=0;n<4;++n)readers.emplace_back([&]{
        while(!start.load(std::memory_order_acquire))std::this_thread::yield();
        do {
            const auto pose=state.read();
            if(!pose.valid)continue;
            const auto value=pose.origin[0];
            for(auto coordinate:pose.origin)assert(coordinate==value);
            for(auto coordinate:pose.axis)assert(coordinate==value);
            for(auto coordinate:pose.viewOffset)assert(coordinate==value);
            assert(pose.anchorOwner==uintptr_t(value)+1);
        }while(!done.load(std::memory_order_acquire));
    });
    start.store(true,std::memory_order_release);
    for(unsigned value=1;value<=100000;++value){
        if(value%100==0)state.invalidate();
        auto pose=state.read();pose.valid=true;pose.anchorOwner=value+1;
        pose.origin.fill(float(value));pose.axis.fill(float(value));pose.viewOffset.fill(float(value));
        assert(state.publish(pose));
    }
    done.store(true,std::memory_order_release);
    for(auto& reader:readers)reader.join();
    stale=state.read();state.invalidate();assert(!state.publish(stale));
    const auto invalid=state.read();assert(!invalid.valid&&!invalid.anchorOwner);

    assert(!state.physics().valid);
    auto stalePhysics=state.physics();stalePhysics.valid=true;
    state.invalidate();assert(!state.publishPhysics(stalePhysics));
    auto physics=state.physics();physics.valid=true;physics.owner=1;physics.present=2;
    assert(state.publishPhysics(physics));
    start.store(false);done.store(false);readers.clear();
    for(unsigned n=0;n<4;++n)readers.emplace_back([&]{
        while(!start.load(std::memory_order_acquire))std::this_thread::yield();
        do {
            const auto pose=state.physics();
            if(!pose.valid)continue;
            const auto value=pose.origin[0];
            for(auto coordinate:pose.origin)assert(coordinate==value);
            assert(pose.owner==uintptr_t(value)+1);
            assert(pose.present==uint64_t(value)+2);
        }while(!done.load(std::memory_order_acquire));
    });
    start.store(true,std::memory_order_release);
    for(unsigned value=1;value<=100000;++value){
        if(value%100==0)state.invalidate();
        auto pose=state.physics();pose.valid=true;pose.owner=value+1;pose.present=value+2;
        pose.origin.fill(float(value));
        assert(state.publishPhysics(pose));
    }
    done.store(true,std::memory_order_release);
    for(auto& reader:readers)reader.join();
    stalePhysics=state.physics();stale=state.read();stale.valid=true;
    assert(state.invalidate());assert(!state.invalidate());
    assert(!state.publishPhysics(stalePhysics));assert(!state.publish(stale));
    assert(!state.physics().valid&&!state.physics().owner&&!state.physics().present);
    assert(state.physics().generation==state.read().generation);
}

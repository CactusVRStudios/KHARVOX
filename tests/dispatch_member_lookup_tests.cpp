#include "../src/vulkan/DispatchMemberLookup.h"
#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <unordered_map>

static void require(bool ok){if(!ok)std::abort();}
static int primary(){return 11;}
static int auxiliary(){return 22;}
struct Dispatch {
    using Fn=int(*)();
    struct Nested { Fn barrier{}; } xr;
    Fn draw{},bind{};
    std::array<uintptr_t,128> unrelated{};
    static inline int copies{};
    Dispatch()=default;
    Dispatch(const Dispatch& other):draw(other.draw),bind(other.bind),unrelated(other.unrelated){++copies;}
};
int main(){
 std::mutex mutex;std::unordered_map<uintptr_t,Dispatch> registry;
 registry[1].draw=primary;registry[1].bind=auxiliary;
 registry[2].draw=auxiliary;
 auto lookup=[&](uintptr_t key,Dispatch::Fn Dispatch::* member){return kharvox::lookupDispatchMember(mutex,registry,key,member);};
 require(lookup(1,&Dispatch::draw)()==11&&lookup(1,&Dispatch::bind)()==22);
 require(lookup(2,&Dispatch::draw)()==22&&!lookup(2,&Dispatch::bind));
 require(!lookup(0,&Dispatch::draw)&&!lookup(3,&Dispatch::draw));
 // Destruction, same-key replacement, and initially absent dispatch must be
 // observed immediately without a cache generation or a per-thread reset.
 {std::lock_guard<std::mutex> lock(mutex);registry.erase(1);}
 require(!lookup(1,&Dispatch::draw));
 {std::lock_guard<std::mutex> lock(mutex);registry[1].draw=auxiliary;}
 require(lookup(1,&Dispatch::draw)()==22);
 // The copied callback can re-enter the registry; no lock/reference escapes.
 const auto callback=lookup(1,&Dispatch::draw);
 {std::lock_guard<std::mutex> lock(mutex);registry[1].draw=primary;}
 require(callback()==22&&lookup(1,&Dispatch::draw)()==11);
 std::atomic<bool> start{};
 std::thread reader([&]{while(!start.load()){};for(int i=0;i<10000;++i){auto fn=lookup(1,&Dispatch::draw);require(!fn||fn==primary||fn==auxiliary);}});
 start=true;
 for(int i=0;i<10000;++i){std::lock_guard<std::mutex> lock(mutex);if(i%3==0)registry.erase(1);else registry[1].draw=i%2?primary:auxiliary;}
 reader.join();require(Dispatch::copies==0);
 auto nested=[&](uintptr_t key){return kharvox::lookupDispatchMember(mutex,registry,key,&Dispatch::xr,&Dispatch::Nested::barrier);};
 registry[1].xr.barrier=primary;registry[2].xr.barrier=auxiliary;
 require(nested(1)()==11&&nested(2)()==22&&!nested(3));
 const auto saved=nested(1);
 {std::lock_guard<std::mutex> lock(mutex);registry.erase(1);}
 require(!nested(1)&&saved()==11);
 {std::lock_guard<std::mutex> lock(mutex);registry[1].xr.barrier=auxiliary;}
 require(nested(1)()==22&&Dispatch::copies==0);

 std::cout<<"Dispatch member lookup: primary/auxiliary separation, missing/erased/reused keys, concurrent publication and zero whole-record copies passed\n";
}

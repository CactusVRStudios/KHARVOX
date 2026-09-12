#include "../src/native/NativeBindScratch.h"
#include "../src/native/NativeReplayRecording.h"
#include "../src/native/NativeOrderedWorkMap.h"
#include "../src/native/NativeThreadTiming.h"
#include "../src/native/NativeReplayPayload.h"
#include "../src/native/NativeStorageBindingSet.h"
#include <set>
#include <functional>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <map>
#include <new>
#include <thread>
#include <vector>

static std::atomic<size_t> allocations{};
void* operator new(size_t n) {
    if (auto p=std::malloc(n ? n : 1)) { ++allocations; return p; }
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
static void check(bool value) { if(!value) std::abort(); }
using namespace kharvox::native;
struct Scratch { std::vector<uint64_t> values; };
struct Replay {
    std::map<uint32_t,uint32_t> descriptorRanges;
    std::map<uint64_t,uint64_t> state;
};
int main() {
    StorageBindingSet bindings;std::set<uint32_t> bindingReference;
    uint32_t rng=17;
    for(int i=0;i<10000;++i){
        rng=rng*1664525u+1013904223u;
        if((rng&15)==0){bindings.clear();bindingReference.clear();}
        else {bindings.insert((rng>>8)%37);bindingReference.insert((rng>>8)%37);}
        check(bindings.size()==bindingReference.size());
        check(std::equal(bindings.begin(),bindings.end(),bindingReference.begin(),bindingReference.end()));
    }
    // A saved memo payload owns its values even after the live binding set
    // is cleared/repopulated by another bind, replay or command-buffer reset.
    bindings.clear();bindings.insert(3);bindings.insert(7);
    std::vector<uint32_t> saved(bindings.begin(),bindings.end());
    bindings.clear();check(bindings.empty());bindings.insert(11);
    check(saved==std::vector<uint32_t>({3,7}));
    std::map<uint32_t,StorageBindingSet> retainedBindings;
    for(auto value:{1u,5u,9u})retainedBindings[0].insert(value);
    const auto bindWarm=allocations.load();
    for(int i=0;i<10000;++i){auto& set=retainedBindings[0];set.clear();for(auto value:{9u,1u,5u,1u})set.insert(value);}
    check(allocations.load()==bindWarm&&retainedBindings[0].size()==3);
    std::map<int,std::map<uint32_t,StorageBindingSet>> commands;
    const std::vector<uint32_t> empty,sorted{1,5,9},changed{2,5};
    check(!restoreStorageBindings(commands,1,0,empty)&&commands.empty());
    check(restoreStorageBindings(commands,1,0,sorted));
    check(restoreStorageBindings(commands,1,1,changed));
    check(restoreStorageBindings(commands,2,0,changed));
    const auto restoreWarm=allocations.load();
    for(int i=0;i<10000;++i)check(!restoreStorageBindings(commands,1,0,sorted));
    check(allocations.load()==restoreWarm);
    check(restoreStorageBindings(commands,1,0,empty)&&commands[1][0].empty());
    check(commands[1][1].size()==2&&commands[2][0].size()==2); // partial bind isolation
    check(!restoreStorageBindings(commands,1,0,empty));
    check(restoreStorageBindings(commands,1,0,changed));
    check(sorted==std::vector<uint32_t>({1,5,9})); // memo payload remains owned
    commands.clear();check(restoreStorageBindings(commands,1,0,sorted));
    // Compare replacement semantics with the original clear+insert path.
    StorageBindingSet oldSet,newSet;
    for(int i=0;i<1000;++i){const auto& source=i%3==0?empty:i%3==1?sorted:changed;
     oldSet.clear();for(auto value:source)oldSet.insert(value);newSet.replaceSorted(source);
     check(std::equal(oldSet.begin(),oldSet.end(),newSet.begin(),newSet.end()));
    }
    std::map<uint32_t,std::set<uint32_t>> oldBindings;
    const auto oldStart=allocations.load();
    for(int i=0;i<10000;++i){oldBindings.erase(0);for(auto value:{9u,1u,5u,1u})oldBindings[0].insert(value);}
    check(allocations.load()-oldStart>=40000);
    std::cout<<"10000 storage binding replacements: retained set zero allocations after warmup; old map/set path at least 40000; sorted/unique semantics and saved payload ownership match\n";
    Scratch* retained{};
    {
        BindScratchLease<Scratch> outer; retained=&outer.get();
        outer.get().values.assign(128,42);
        const auto data=outer.get().values.data();
        try {
            BindScratchLease<Scratch> inner;
            check(&inner.get()!=retained); inner.get().values.assign(512,9);
            throw 1;
        } catch(int) {}
        check(data==outer.get().values.data() && outer.get().values[0]==42);
        std::thread worker([&] {
            BindScratchLease<Scratch> other;
            check(&other.get()!=retained); other.get().values.assign(128,7);
        });
        worker.join(); check(outer.get().values[0]==42);
    }
    Replay r;
    check(prepareDescriptorRange(r,2,1)); r.state[0x10002]=17;
    auto stateNode=&r.state.at(0x10002);
    const auto rangeNode=&r.descriptorRanges.at(2);
    const auto before=allocations.load();
    for(int i=0;i<10000;++i) {
        BindScratchLease<Scratch> lease;
        check(&lease.get()==retained);
        lease.get().values.clear(); lease.get().values.assign(128,i);
        check(prepareDescriptorRange(r,2,1)); r.state[0x10002]=i;
        check(&r.state.at(0x10002)==stateNode && &r.descriptorRanges.at(2)==rangeNode);
    }
    check(allocations.load()==before);
    check(prepareDescriptorRange(r,4,2)); r.state[0x10004]=19;
    const auto ranges=r.descriptorRanges;
    const auto state=r.state;
    check(!prepareDescriptorRange(r,5,2));
    check(r.descriptorRanges==ranges && r.state==state);
    check(prepareDescriptorRange(r,2,4));
    check(r.descriptorRanges.size()==1 && r.descriptorRanges.at(2)==4);
    check(r.state.size()==1 && &r.state.at(0x10002)==stateNode);
    check(prepareDescriptorRange(r,0,6));
    check(r.descriptorRanges.size()==1 && r.state.empty());
    // 64-bit arithmetic preserves valid comparison at the uint32 boundary.
    Replay high; check(prepareDescriptorRange(high,UINT32_MAX-1,2));
    check(!prepareDescriptorRange(high,UINT32_MAX,1));
    OrderedWorkMap<uint64_t,uint64_t> work;
    std::map<uint64_t,uint64_t> reference;
    uint32_t seed=23;
    for(int i=0;i<5000;++i){
        seed=seed*1664525u+1013904223u;
        const auto key=seed%97, value=seed%251;
        auto a=work.emplace(key,value);auto b=reference.emplace(key,value);
        check(a.second==b.second && a.first->second==b.first->second);
        check(work.size()==reference.size());
        check(std::equal(work.begin(),work.end(),reference.begin(),[](auto a,auto b){return a.first==b.first&&a.second==b.second;}));
    }
    const auto warm=allocations.load();
    for(int n=0;n<1000;++n){work.clear();for(uint64_t i=0;i<97;++i)work.emplace(96-i,i);}
    check(allocations.load()==warm);
    auto timing=threadTimingDelta({100,1000,7,true},{130,1400,7,true});
    check(timing.valid&&timing.cpuNs==3000&&timing.cycles==400);
    check(!threadTimingDelta({100,1000,7,true},{130,1400,8,true}).valid);
    check(!threadTimingDelta({100,1000,7,true},{99,1400,7,true}).valid);
    check(!threadTimingDelta({100,1000,7,true},{130,999,7,true}).valid);
    check(!threadTimingDelta({100,1000,7,false},{130,1400,7,true}).valid);
    std::shared_ptr<Scratch> payloadSlot;
    std::function<void()> stateCallback;
    uint64_t observed{};
    auto recordPayload=[&](uint64_t value){
        bool reused{};auto payload=prepareReplayPayload(payloadSlot,bool(stateCallback),reused);
        payload->values.assign(128,value);
        stateCallback=[payload,&observed]{auto executing=payload;observed=executing->values[0];};
        stateCallback();return reused;
    };
    check(!recordPayload(1));
    const auto payloadWarm=allocations.load();
    for(uint64_t i=0;i<10000;++i){check(recordPayload(i));check(observed==i);}
    check(allocations.load()==payloadWarm);
    auto initial=stateCallback;
    check(!recordPayload(42));initial();check(observed==9999);
    auto finalCommand=stateCallback;
    check(!recordPayload(43));finalCommand();check(observed==42);
    stateCallback();check(observed==43);
    auto orphanedHistory=stateCallback;stateCallback={};
    // Slot + history (two refs) must not be confused with slot + live state.
    check(!recordPayload(44));orphanedHistory();check(observed==43);
    {
        auto executing=payloadSlot; // callback-local pin during re-entry
        check(!recordPayload(45));check(executing->values[0]==44);
    }
    struct Arguments {int point{},layout{};uint32_t first{};std::vector<int> sets;std::vector<uint32_t> offsets;};
    Arguments args{1,2,3,{11,12},{20,24}};
    std::vector<int> sets{11,12};std::vector<uint32_t> offsets{20,24};
    auto matches=[&](int point,int layout,uint32_t first){return sameReplayArguments(args,point,layout,first,sets,offsets);};
    check(matches(1,2,3));check(!matches(0,2,3));check(!matches(1,9,3));check(!matches(1,2,0));
    offsets[1]=25;check(!matches(1,2,3));offsets[1]=24;
    sets[1]=13;check(!matches(1,2,3));sets[1]=12;
    sets.pop_back();check(!matches(1,2,3));sets.push_back(12);
    offsets.push_back(0);check(!matches(1,2,3));
    args.sets.clear();args.offsets.clear();sets.clear();offsets.clear();check(matches(1,2,3));
    std::cout<<"10000 repeated CPU work-list/range updates: zero allocations after warmup; nested/thread isolation and overlap rejection passed\n";
    std::cout<<"Ordered work lists match std::map across 5000 inserts/duplicates; repeated clear/rebuild retains capacity; thread accounting rejects invalid counters\n";
    std::cout<<"10000 descriptor payload updates allocate nothing after warmup; initial/final/orphaned/executing callbacks preserve old bytes\n";
}

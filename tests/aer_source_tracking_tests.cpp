#include "../src/common/AerSourceTracking.h"
#include <cstdlib>
#include <thread>
void check(bool ok){if(!ok)std::abort();}
int main(){
    using namespace kharvox;
    AerSourceWindow window;AerSourceKey right{771,2,1},left{771,2,0},next{773,2,1};
    check(!window.take().valid());
    std::thread a([&]{window.observe(right);});std::thread b([&]{window.observe(right);});a.join();b.join();
    auto observed=window.take();check(observed.valid()&&observed.key==right&&observed.count==2);
    check(!window.take().valid());
    window.observe(right);window.observe(left);check(window.take().ambiguous);
    window.observe(right);window.observe(next);check(!window.take().valid());
    AerInputHistory<int,4> history;history.remember(right,10);history.remember(left,20);
    int value=-1;check(history.find(right,value)&&value==10);check(history.find(left,value)&&value==20);
    check(!history.find(next,value));check(!history.find({771,3,1},value));
    check(aerSourceCompletesPair(right,true,left));
    check(!aerSourceCompletesPair(next,true,left));check(!aerSourceCompletesPair(right,false,left));
    check(!aerSourceCompletesPair(right,true,right));check(!aerSourceCompletesPair(right,true,{771,3,0}));
    check(!aerSourceCompletesPair(right,true,left,771)); // duplicate
    check(!aerSourceCompletesPair(right,true,left,773)); // late old pair cannot rewind
    check(aerSourceCompletesPair(right,true,left,769));
    // Pipeline lag: planned right/new must NOT relabel observed left/previous.
    AerSourceKey actual{769,2,0};history.remember(actual,30);window.observe(actual);
    const auto source=window.take();check(source.valid()&&history.find(source.key,value)&&value==30);
    check(source.key.eye==0&&source.key.poseId!=next.poseId);
    for(int i=0;i<5;++i)history.remember({uint64_t(800+i),2,0},i);
    check(!history.find(actual,value)); // bounded history, no stale fallback
    // Replay the measured right-first schedule with a delayed source. Publish
    // only source-matched pairs; keep programming cadence independent of it.
    std::array<AerSourceKey,2> cache{};std::array<bool,2> ready{};
    uint64_t published=0;int scheduled=1;unsigned publications=0;
    for(const auto key:std::array{AerSourceKey{769,2,0},AerSourceKey{771,2,1},
            AerSourceKey{771,2,0},AerSourceKey{},AerSourceKey{773,2,0},
            AerSourceKey{775,2,1},AerSourceKey{775,2,0}}){
        if(key.valid()){
            const bool publish=aerSourceCompletesPair(key,ready[key.eye^1],cache[key.eye^1]);
            cache[key.eye]=key;ready[key.eye]=true;
            if(publish){published=key.poseId;++publications;check(cache[0].poseId==cache[1].poseId);}
        }
        scheduled^=1;
        if(key.poseId==773)check(published==771); // missing right: retain complete pair
    }
    check(published==775&&publications==2&&scheduled==0);
    ready={};check(!aerSourceCompletesPair({775,3,1},ready[0],cache[0]));
}

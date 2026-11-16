#include "../src/common/PerformanceCpuCounters.h"
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {
void check(bool value){if(!value)std::abort();}

void nestedScopes() {
    using namespace kharvox;
    const PerformanceCpuTimer native(PerformanceCpuScope::NativeHandsUpdate);
    const PerformanceCpuTimer custom(PerformanceCpuScope::CustomHandsUpdate);
    const PerformanceCpuTimer stance(PerformanceCpuScope::StanceRead);
    const PerformanceCpuTimer actions(PerformanceCpuScope::GameplayActions);
    const PerformanceCpuTimer weapon(PerformanceCpuScope::WeaponUpdate);
}
}

int main(int argc,char** argv) {
    check(argc==2);
    const bool enabled=std::strcmp(argv[1],"--enabled")==0;
    check(enabled||std::strcmp(argv[1],"--disabled")==0);
    check(SetEnvironmentVariableA("KHARVOX_PERFORMANCE_COMPARISON",enabled?"1":"0")!=0);
    using namespace kharvox;
    check(performanceCpuEnabled()==enabled);
    // The opt-in is sampled once; later environment edits cannot turn on
    // measurement halfway through a gameplay session.
    check(SetEnvironmentVariableA("KHARVOX_PERFORMANCE_COMPARISON",enabled?"0":"1")!=0);
    check(performanceCpuEnabled()==enabled);
    for(unsigned i=0;i<static_cast<unsigned>(PerformanceCpuScope::Count);++i){
        const auto snapshot=performanceCpuSnapshot(static_cast<PerformanceCpuScope>(i));
        check(snapshot.count==0&&snapshot.totalMs==0&&snapshot.maxMs==0);
    }
    nestedScopes();
    constexpr unsigned workers=4,repetitions=200;
    std::array<std::thread,workers> threads;
    for(auto& thread:threads)thread=std::thread([repetitions]{
        for(unsigned iteration=0;iteration<repetitions;++iteration)nestedScopes();
    });
    for(auto& thread:threads)thread.join();
    for(unsigned i=0;i<static_cast<unsigned>(PerformanceCpuScope::Count);++i){
        const auto scope=static_cast<PerformanceCpuScope>(i);
        const auto snapshot=performanceCpuSnapshot(scope);
        check(snapshot.count==(enabled?1+workers*repetitions:0));
        check(std::isfinite(snapshot.totalMs)&&std::isfinite(snapshot.maxMs)
            &&snapshot.totalMs>=0&&snapshot.maxMs>=0&&snapshot.maxMs<=snapshot.totalMs);
        if(!enabled)check(snapshot.totalMs==0&&snapshot.maxMs==0);
        // Snapshot reads must neither reset nor increment cumulative counters.
        const auto repeated=performanceCpuSnapshot(scope);
        check(repeated.count==snapshot.count&&repeated.totalMs==snapshot.totalMs
            &&repeated.maxMs==snapshot.maxMs);
    }
    check(performanceCpuSnapshot(PerformanceCpuScope::Count).count==0);
}

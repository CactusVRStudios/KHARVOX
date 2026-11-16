#pragma once
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdint>

namespace kharvox {

enum class PerformanceCpuScope {
    NativeHandsUpdate,CustomHandsUpdate,StanceRead,GameplayActions,WeaponUpdate,Count
};
struct PerformanceCpuSnapshot {
    std::uint64_t count{};
    double totalMs{},maxMs{};
};

inline bool performanceCpuEnabled() noexcept {
    static const bool enabled=[]{
        char value[8]{};
        return GetEnvironmentVariableA("KHARVOX_PERFORMANCE_COMPARISON",value,sizeof(value))==1
            &&value[0]=='1';
    }();
    return enabled;
}

namespace performance_cpu_detail {
struct Counter {
    std::atomic<std::uint64_t> count{},ticks{},maximumTicks{};
};
inline auto& counters() noexcept {
    static std::array<Counter,static_cast<std::size_t>(PerformanceCpuScope::Count)> values{};
    return values;
}
inline double millisecondsPerTick() noexcept {
    static const double factor=[]{
        LARGE_INTEGER frequency{};
        return QueryPerformanceFrequency(&frequency)&&frequency.QuadPart>0
            ?1000.0/static_cast<double>(frequency.QuadPart):0.0;
    }();
    return factor;
}
} // namespace performance_cpu_detail

// Cumulative, relaxed snapshots can include concurrently completed samples.
// No reset, logging, allocation, mutex or worker thread is required per scope.
inline PerformanceCpuSnapshot performanceCpuSnapshot(PerformanceCpuScope scope) noexcept {
    const auto index=static_cast<std::size_t>(scope);
    if(!performanceCpuEnabled()||index>=performance_cpu_detail::counters().size())return {};
    const auto& counter=performance_cpu_detail::counters()[index];
    const auto factor=performance_cpu_detail::millisecondsPerTick();
    return {counter.count.load(std::memory_order_relaxed),
        counter.ticks.load(std::memory_order_relaxed)*factor,
        counter.maximumTicks.load(std::memory_order_relaxed)*factor};
}

class PerformanceCpuTimer {
    PerformanceCpuScope scope_;
    LARGE_INTEGER started_{};
    bool active_{};
public:
    explicit PerformanceCpuTimer(PerformanceCpuScope scope) noexcept:scope_(scope) {
        if(performanceCpuEnabled()
            &&static_cast<std::size_t>(scope)<performance_cpu_detail::counters().size())
            active_=QueryPerformanceCounter(&started_)!=0;
    }
    PerformanceCpuTimer(const PerformanceCpuTimer&)=delete;
    PerformanceCpuTimer& operator=(const PerformanceCpuTimer&)=delete;
    ~PerformanceCpuTimer() noexcept {
        if(!active_)return;
        LARGE_INTEGER ended{};
        if(!QueryPerformanceCounter(&ended)||ended.QuadPart<started_.QuadPart)return;
        const auto elapsed=static_cast<std::uint64_t>(ended.QuadPart-started_.QuadPart);
        auto& counter=performance_cpu_detail::counters()[static_cast<std::size_t>(scope_)];
        counter.ticks.fetch_add(elapsed,std::memory_order_relaxed);
        auto maximum=counter.maximumTicks.load(std::memory_order_relaxed);
        while(maximum<elapsed&&!counter.maximumTicks.compare_exchange_weak(
            maximum,elapsed,std::memory_order_relaxed,std::memory_order_relaxed)){}
        counter.count.fetch_add(1,std::memory_order_relaxed);
    }
};

} // namespace kharvox

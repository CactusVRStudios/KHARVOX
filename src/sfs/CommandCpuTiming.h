#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>

namespace kharvox::sfs {
struct CommandCpuTiming {
    inline static std::atomic<bool> enabled{false};
    inline static std::atomic<uint64_t> samples{},waitNs{},bodyNs{};
    uint64_t start{},locked{};
    static uint64_t now(){return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
    CommandCpuTiming(){thread_local uint32_t calls{};if(enabled.load(std::memory_order_relaxed)&&!(++calls%64))start=now();}
    void acquired(){if(start)locked=now();}
    ~CommandCpuTiming(){if(locked){waitNs.fetch_add(locked-start,std::memory_order_relaxed);bodyNs.fetch_add(now()-locked,std::memory_order_relaxed);samples.fetch_add(1,std::memory_order_relaxed);}}
};
}

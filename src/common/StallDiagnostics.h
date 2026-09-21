#pragma once
#include "RuntimeLog.h"
#include <atomic>

namespace kharvox {
inline long long diagnosticTicks() noexcept {
    LARGE_INTEGER value{}; QueryPerformanceCounter(&value); return value.QuadPart;
}
inline void logDiagnosticDuration(const char* operation, long long begin,
    long long end, double thresholdMs = 20.0) noexcept {
    if (!begin) return;
    static const double frequency = [] {
        LARGE_INTEGER value{}; QueryPerformanceFrequency(&value);
        return double(value.QuadPart);
    }();
    const double ms = frequency > 0 ? (end - begin) * 1000.0 / frequency : 0;
    if (ms < thresholdMs) return;
    try {
        DWORD foregroundPid{};GetWindowThreadProcessId(GetForegroundWindow(),&foregroundPid);
        writeRuntimeLog("[KHARVOX][STALL]", std::string(operation) + " ms="
            + std::to_string(ms) + " thread=" + std::to_string(GetCurrentThreadId())
            + " foregroundPid=" + std::to_string(foregroundPid)
            + " gameForeground=" + std::to_string(foregroundPid==GetCurrentProcessId()));
    } catch (...) {}
}
class DiagnosticDuration {
public:
    explicit DiagnosticDuration(const char* operation) noexcept : operation_(operation),
        begin_(extendedDiagnosticsEnabled() ? diagnosticTicks() : 0) {}
    ~DiagnosticDuration() { if(begin_)logDiagnosticDuration(operation_,begin_,diagnosticTicks()); }
private:
    const char* operation_;
    long long begin_;
};
// Declared first in the wrapper so its destructor includes lease retirement,
// Native completion, and all other wrapper cleanup. The gap is wall time from
// the most recent completed Present to the next entry, not GPU execution time.
class DiagnosticPresentDuration {
public:
    DiagnosticPresentDuration() noexcept : begin_(extendedDiagnosticsEnabled() ? diagnosticTicks() : 0) {
        if(begin_)logDiagnosticDuration("between-present-calls",lastReturn_.load(),begin_,50.0);
    }
    ~DiagnosticPresentDuration() {
        if(!begin_)return;
        const auto end=diagnosticTicks();
        logDiagnosticDuration("present-wrapper-total",begin_,end);
        lastReturn_.store(diagnosticTicks());
    }
private:
    long long begin_;
    static inline std::atomic<long long> lastReturn_{};
};
}

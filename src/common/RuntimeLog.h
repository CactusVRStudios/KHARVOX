#pragma once
#include "DiagnosticLogging.h"
#include <fstream>
#include <mutex>
#include <string>

namespace kharvox {
// Startup acknowledgements are control signals consumed by the launcher.
// Keep them available without enabling per-frame diagnostic logging.
inline void writeRuntimeLog(const char* component, const std::string& text,
    bool operational = false) noexcept {
    if (!operational && !extendedDiagnosticsEnabled()) return;
    try {
        static std::mutex mutex;
        std::lock_guard<std::mutex> guard(mutex);
        char temp[MAX_PATH]{};
        if (!GetTempPathA(MAX_PATH, temp)) return;
        std::ofstream out(std::string(temp) + "KHARVOX.log", std::ios::app);
        SYSTEMTIME t{}; GetLocalTime(&t);
        out << '[' << t.wHour << ':' << t.wMinute << ':' << t.wSecond
            << '.' << t.wMilliseconds << "] " << component << ' ' << text << '\n';
    } catch (...) { /* Logging failure must not terminate the game. */ }
}
}

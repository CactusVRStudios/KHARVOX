#include "../src/common/RuntimeLog.h"
#include "../src/common/StallDiagnostics.h"
#include <cassert>
#include <filesystem>
#include <iterator>

int main(int argc, char** argv) {
    const bool extended = argc > 1 && std::string(argv[1]) == "1";
    SetEnvironmentVariableA("KHARVOX_EXTENDED_LOGGING", extended ? "1" : "0");
    const auto dir = std::filesystem::temp_directory_path() /
        ("kharvox-log-test-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(dir);
    SetEnvironmentVariableA("TEMP", dir.string().c_str());
    SetEnvironmentVariableA("TMP", dir.string().c_str());
    const auto path = dir / "KHARVOX.log";
    kharvox::writeRuntimeLog("[KHARVOX]", "verbose-only");
    assert(std::filesystem::exists(path) == extended);
    kharvox::writeRuntimeLog("[KHARVOX]", "runtimeDir=D:\\release\\.", true);
    kharvox::writeRuntimeLog("[KHARVOX][XR]", "Frame 120 stable mode=PROJECTION result=0", true);
    LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
    kharvox::logDiagnosticDuration("below-threshold", 1, 1);
    kharvox::logDiagnosticDuration("known-one-second-stall", 1, 1 + frequency.QuadPart);
    std::ifstream input(path);
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    assert((text.find("verbose-only") != std::string::npos) == extended);
    assert(text.find("runtimeDir=D:\\release\\.") != std::string::npos);
    assert(text.find("[KHARVOX][XR] Frame 120 stable mode=PROJECTION result=0") != std::string::npos);
    assert(text.find("below-threshold") == std::string::npos);
    assert((text.find("known-one-second-stall ms=1000.") != std::string::npos) == extended);
    input.close();
    std::filesystem::remove(path);
    std::filesystem::remove(dir);
}

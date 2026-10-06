#include "ProtubeClient.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cwchar>
#include <fstream>
#include <mutex>
#include <string>

#include "../common/RuntimePaths.h"

namespace {

using namespace kharvox::protube;

// ForceTubeVR_API_x64.dll C exports (see ProTubeVR's public SDK plugins).
// The channel argument is a plain int enum:
// 0 all, 1 rifle, 2 rifleButt, 3 rifleBolt, 4 pistol1, 5 pistol2, 6 other, 7 vest.
using InitFn = void (*)();
using KickChannelFn = void (*)(std::uint8_t power, int channel);
using RumbleChannelFn = void (*)(std::uint8_t power, float seconds, int channel);
using ListConnectedFn = const char* (*)();

constexpr const wchar_t* sdkDllName = L"ForceTubeVR_API_x64.dll";
constexpr const wchar_t* iniName = L"protube.ini";
constexpr std::size_t queueCapacity = 16;

std::atomic<bool> started{};
std::atomic<bool> ready{};
std::atomic<bool> stopRequested{};
HANDLE wakeEvent{};

std::mutex queueMutex;
std::array<Command, queueCapacity> queue{};
std::size_t queueHead{};
std::size_t queueCount{};

// Owned by the game thread after `ready` is published.
ShotPolicy* policy{};

struct Settings {
    bool enabled{true};
    int channel{0};
    int powerPercent{100};
    bool initRifle{false};
    ProfileTable profiles{defaultProfiles()};
};
Settings settings;

void writeLog(const std::string& text) noexcept {
    try {
        static std::mutex mutex;
        std::lock_guard<std::mutex> guard(mutex);
        const auto path = kharvox::logPath(L"protube.log");
        if (path.empty()) return;
        std::ofstream out(path.c_str(), std::ios::app);
        SYSTEMTIME t{};
        GetLocalTime(&t);
        out << '[' << t.wHour << ':' << t.wMinute << ':' << t.wSecond << '.'
            << t.wMilliseconds << "] " << text << '\n';
    } catch (...) {
    }
}

bool fileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES
        && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring widen(const char* text) {
    std::wstring result;
    while (text && *text) result.push_back(static_cast<unsigned char>(*text++));
    return result;
}

int readInt(const std::wstring& ini, const wchar_t* section,
    const wchar_t* key, int fallback) {
    return static_cast<int>(GetPrivateProfileIntW(section, key, fallback, ini.c_str()));
}

std::wstring readString(const std::wstring& ini, const wchar_t* section,
    const wchar_t* key, const wchar_t* fallback) {
    wchar_t buffer[64]{};
    GetPrivateProfileStringW(section, key, fallback, buffer,
        static_cast<DWORD>(std::size(buffer)), ini.c_str());
    return buffer;
}

FireMode parseMode(const std::wstring& text, FireMode fallback) {
    if (_wcsicmp(text.c_str(), L"none") == 0) return FireMode::None;
    if (_wcsicmp(text.c_str(), L"repeat") == 0) return FireMode::Repeat;
    if (_wcsicmp(text.c_str(), L"charge") == 0) return FireMode::Charge;
    if (_wcsicmp(text.c_str(), L"continuous") == 0) return FireMode::Continuous;
    return fallback;
}

const wchar_t* modeName(FireMode mode) {
    switch (mode) {
    case FireMode::Repeat: return L"repeat";
    case FireMode::Charge: return L"charge";
    case FireMode::Continuous: return L"continuous";
    default: return L"none";
    }
}

std::uint8_t byteValue(int value) {
    return static_cast<std::uint8_t>(value < 0 ? 0 : value > 255 ? 255 : value);
}

std::uint16_t msValue(int value) {
    return static_cast<std::uint16_t>(value < 0 ? 0 : value > 10000 ? 10000 : value);
}

void loadSettings(const std::wstring& ini) {
    if (!fileExists(ini)) {
        writeLog("protube.ini not found; using built-in DOOM profiles");
        return;
    }
    settings.enabled = readInt(ini, L"general", L"enabled", 1) != 0;
    settings.channel = readInt(ini, L"general", L"channel", 0);
    if (settings.channel < 0 || settings.channel > 7) settings.channel = 0;
    settings.powerPercent = readInt(ini, L"general", L"power_percent", 100);
    settings.initRifle = _wcsicmp(
        readString(ini, L"general", L"init", L"pistol").c_str(), L"rifle") == 0;

    for (std::size_t i = 1; i < weaponKindCount; ++i) {
        const auto kind = static_cast<KharvoxWeaponKind>(i);
        const auto section = widen(KharvoxWeaponKindKey(kind));
        auto& p = settings.profiles[i];
        p.mode = parseMode(readString(ini, section.c_str(), L"mode",
            modeName(p.mode)), p.mode);
        p.kick = byteValue(readInt(ini, section.c_str(), L"kick", p.kick));
        p.rumble = byteValue(readInt(ini, section.c_str(), L"rumble", p.rumble));
        p.rumbleMs = msValue(readInt(ini, section.c_str(), L"rumble_ms", p.rumbleMs));
        p.intervalMs = msValue(readInt(ini, section.c_str(), L"interval_ms", p.intervalMs));
        p.rampMs = msValue(readInt(ini, section.c_str(), L"ramp_ms", p.rampMs));
        p.chargeMs = msValue(readInt(ini, section.c_str(), L"charge_ms", p.chargeMs));
    }
    writeLog("loaded protube.ini channel=" + std::to_string(settings.channel)
        + " power_percent=" + std::to_string(settings.powerPercent)
        + " init=" + (settings.initRifle ? "rifle" : "pistol"));
}

bool popCommand(Command& out) {
    std::lock_guard<std::mutex> guard(queueMutex);
    if (queueCount == 0) return false;
    out = queue[queueHead];
    queueHead = (queueHead + 1) % queueCapacity;
    --queueCount;
    return true;
}

void pushCommand(const Command& command) {
    {
        std::lock_guard<std::mutex> guard(queueMutex);
        if (queueCount == queueCapacity) {
            // Drop the oldest: a late kick is worse than a missing one.
            queueHead = (queueHead + 1) % queueCapacity;
            --queueCount;
        }
        queue[(queueHead + queueCount) % queueCapacity] = command;
        ++queueCount;
    }
    if (wakeEvent) SetEvent(wakeEvent);
}

DWORD WINAPI workerMain(LPVOID) {
    const auto dllPath = kharvox::runtimePath(sdkDllName);
    HMODULE sdk = LoadLibraryExW(dllPath.c_str(), nullptr,
        LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!sdk) {
        writeLog("failed to load ForceTubeVR_API_x64.dll error="
            + std::to_string(GetLastError()));
        return 0;
    }
    // The module is intentionally never freed: the SDK owns Bluetooth
    // threads that may outlive this worker during process shutdown.
    const auto initPistol = reinterpret_cast<InitFn>(GetProcAddress(sdk, "InitPistol"));
    const auto initRifle = reinterpret_cast<InitFn>(GetProcAddress(sdk, "InitRifle"));
    const auto kick = reinterpret_cast<KickChannelFn>(GetProcAddress(sdk, "KickChannel"));
    const auto rumble = reinterpret_cast<RumbleChannelFn>(GetProcAddress(sdk, "RumbleChannel"));
    const auto listConnected = reinterpret_cast<ListConnectedFn>(
        GetProcAddress(sdk, "ListConnectedForceTube"));
    if (!kick || !rumble || (!initPistol && !initRifle)) {
        writeLog("ForceTube SDK is missing required exports "
            "(InitPistol/InitRifle, KickChannel, RumbleChannel); disabled");
        return 0;
    }

    const auto init = settings.initRifle && initRifle ? initRifle
        : initPistol ? initPistol : initRifle;
    init();
    writeLog(std::string("ForceTube SDK initialized (")
        + (init == initRifle ? "rifle" : "pistol")
        + " order); waiting for paired devices");

    std::string lastDevices;
    std::uint64_t nextDeviceLog = GetTickCount64() + 3000;
    std::uint64_t sent = 0;
    while (!stopRequested.load(std::memory_order_acquire)) {
        WaitForSingleObject(wakeEvent, 250);
        if (stopRequested.load(std::memory_order_acquire)) break;

        Command command{};
        while (popCommand(command)) {
            if (command.kick)
                kick(command.kick, settings.channel);
            if (command.rumble)
                rumble(command.rumble, command.rumbleMs / 1000.0f, settings.channel);
            if (++sent == 1)
                writeLog("first recoil command sent kick="
                    + std::to_string(command.kick) + " rumble="
                    + std::to_string(command.rumble));
        }

        const auto now = GetTickCount64();
        if (listConnected && now >= nextDeviceLog) {
            nextDeviceLog = now + 10000;
            const char* devices = listConnected();
            std::string current = devices ? devices : "";
            if (current != lastDevices) {
                writeLog("connected devices: " + (current.empty() ? "(none)" : current));
                lastDevices = std::move(current);
            }
        }
    }
    writeLog("worker stopped after " + std::to_string(sent) + " commands");
    return 0;
}

} // namespace

void KharvoxProtubeStart() {
    bool expected = false;
    if (!started.compare_exchange_strong(expected, true)) return;

    const auto dllPath = kharvox::runtimePath(sdkDllName);
    if (!fileExists(dllPath)) return; // Feature is opt-in by DLL presence.

    writeLog("ForceTubeVR_API_x64.dll found; ProTube recoil starting");
    loadSettings(kharvox::runtimePath(iniName));
    if (!settings.enabled) {
        writeLog("disabled by protube.ini");
        return;
    }

    wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wakeEvent) return;
    policy = new ShotPolicy(settings.profiles, settings.powerPercent);
    HANDLE thread = CreateThread(nullptr, 0, workerMain, nullptr, 0, nullptr);
    if (!thread) {
        writeLog("failed to start worker thread");
        return;
    }
    CloseHandle(thread);
    ready.store(true, std::memory_order_release);
}

void KharvoxProtubeUpdate(const Input& input) {
    if (!ready.load(std::memory_order_acquire)) return;
    if (stopRequested.load(std::memory_order_relaxed)) return;
    const auto command = policy->step(input);
    if (command.valid()) pushCommand(command);
}

void KharvoxProtubeRequestStop() {
    stopRequested.store(true, std::memory_order_release);
    if (wakeEvent) SetEvent(wakeEvent);
}

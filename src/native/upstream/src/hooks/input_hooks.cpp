#include "kharvoxnative/input_hooks.h"
#include "kharvoxnative/log.h"
#include <Windows.h>
#include <MinHook.h>
#include <atomic>
#include <format>

namespace kharvoxnative::input_hooks {
namespace {

using RegisterRawInputDevicesFn = BOOL(WINAPI*)(PCRAWINPUTDEVICE, UINT, UINT);
using GetRawInputDataFn = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);

RegisterRawInputDevicesFn real_register_raw_input_devices{};
GetRawInputDataFn real_get_raw_input_data{};
std::atomic_bool installed{false};

// Self-limiting: logs the first N mouse-delta samples only, so this stays
// cheap even though GetRawInputData is called every frame during gameplay.
std::atomic_uint32_t mouse_sample_count{0};
constexpr uint32_t kMaxMouseSamples = 100;

BOOL WINAPI hook_register_raw_input_devices(PCRAWINPUTDEVICE devices, UINT count, UINT size) {
    // Rare (once at startup / on device-focus changes) - log unconditionally.
    for (UINT i = 0; i < count; ++i) {
        log::info(std::format(
            "RegisterRawInputDevices[{}]: usagePage={:#x} usage={:#x} flags={:#x} hasTargetWindow={}",
            i, devices[i].usUsagePage, devices[i].usUsage, devices[i].dwFlags,
            devices[i].hwndTarget != nullptr));
    }
    return real_register_raw_input_devices(devices, count, size);
}

UINT WINAPI hook_get_raw_input_data(HRAWINPUT raw_input, UINT command, LPVOID data,
                                     PUINT size, UINT header_size) {
    const UINT result = real_get_raw_input_data(raw_input, command, data, size, header_size);
    if (command == RID_INPUT && data && result != static_cast<UINT>(-1) &&
        mouse_sample_count.load(std::memory_order_relaxed) < kMaxMouseSamples) {
        const auto* raw = static_cast<const RAWINPUT*>(data);
        if (raw->header.dwType == RIM_TYPEMOUSE) {
            const auto& mouse = raw->data.mouse;
            if (mouse.lLastX != 0 || mouse.lLastY != 0) {
                const uint32_t sample = mouse_sample_count.fetch_add(1);
                if (sample < kMaxMouseSamples) {
                    log::info(std::format("RawMouse sample={} dx={} dy={} flags={:#x}",
                                           sample, mouse.lLastX, mouse.lLastY, mouse.usFlags));
                }
            }
        }
    }
    return result;
}

template <typename T>
bool add_hook(HMODULE module, const char* export_name, void* replacement, T& original) {
    void* target = reinterpret_cast<void*>(GetProcAddress(module, export_name));
    if (!target) {
        log::warn(std::string("input_hooks: export not found: ") + export_name);
        return false;
    }
    if (MH_CreateHook(target, replacement, reinterpret_cast<void**>(&original)) != MH_OK) {
        log::error(std::string("input_hooks: MH_CreateHook failed: ") + export_name);
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        log::error(std::string("input_hooks: MH_EnableHook failed: ") + export_name);
        return false;
    }
    return true;
}

}  // namespace

// Must be called after kharvoxnative::vk_hooks::install() has succeeded at least
// once: this module does not call MH_Initialize() itself and relies on
// vk_hooks having already initialized the shared global MinHook instance.
bool install() {
    if (installed.exchange(true)) return true;
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) {
        log::warn("input_hooks: user32.dll not loaded yet");
        installed = false;
        return false;
    }
    bool ok = true;
    ok &= add_hook(user32, "RegisterRawInputDevices",
                    reinterpret_cast<void*>(&hook_register_raw_input_devices),
                    real_register_raw_input_devices);
    ok &= add_hook(user32, "GetRawInputData",
                    reinterpret_cast<void*>(&hook_get_raw_input_data),
                    real_get_raw_input_data);
    if (!ok) {
        log::error("input_hooks: installation incomplete");
        installed = false;
        return false;
    }
    log::info("input_hooks: Raw Input diagnostic hooks installed");
    return true;
}

void uninstall() {
    // No MH_Initialize/MH_Uninitialize or per-hook MH_DisableHook here: this
    // module shares one global MinHook instance with vk_hooks (install()
    // below requires vk_hooks::install() to have run first, so MH_Initialize
    // has already happened there). vk_hooks::uninstall()'s
    // MH_DisableHook(MH_ALL_HOOKS) + MH_Uninitialize() tears these hooks
    // down too; doing it again here with the trampoline pointer instead of
    // the original target address would be incorrect.
    installed = false;
}

}  // namespace kharvoxnative::input_hooks

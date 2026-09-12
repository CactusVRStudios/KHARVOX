#pragma once
#include <windows.h>
#include <vulkan/vulkan.h>
#include <climits>

namespace kharvox {
// Core Win32 WSI requires the real client size, not fabricated capabilities.
// Keep renderer-sized images even when presentation scaling is unavailable.
inline bool matchCoreSurfaceWindow(HWND window, VkExtent2D extent) {
    if (!window || !extent.width || !extent.height ||
        extent.width > INT_MAX / 2 || extent.height > INT_MAX / 2) return false;
    RECT client{};
    if (!GetClientRect(window, &client)) return false;
    if (client.right == extent.width && client.bottom == extent.height) return true;
    RECT outer{0, 0, LONG(extent.width), LONG(extent.height)};
    if (!AdjustWindowRectEx(&outer, DWORD(GetWindowLongPtrW(window, GWL_STYLE)),
            GetMenu(window) != nullptr, DWORD(GetWindowLongPtrW(window, GWL_EXSTYLE)))) return false;
    if (!SetWindowPos(window, nullptr, 0, 0, outer.right - outer.left,
            outer.bottom - outer.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) return false;
    return GetClientRect(window, &client) && client.right == extent.width && client.bottom == extent.height;
}
}

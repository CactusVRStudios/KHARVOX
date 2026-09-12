#pragma once
#include <windows.h>
#include <vulkan/vulkan.h>
#include <climits>

namespace kharvox {
struct CoreSurfaceResize { unsigned attempts{}; DWORD error{}; LONG width{},height{}; };
// Core Win32 WSI requires the real client size, not fabricated capabilities.
// Keep renderer-sized images even when presentation scaling is unavailable.
inline bool matchCoreSurfaceWindow(HWND window, VkExtent2D extent, CoreSurfaceResize* diagnostic=nullptr) {
    if (!window || !extent.width || !extent.height ||
        extent.width > INT_MAX / 2 || extent.height > INT_MAX / 2) return false;
    RECT client{};
    if (!GetClientRect(window, &client)) return false;
    if (client.right == extent.width && client.bottom == extent.height) return true;
    RECT outer{0, 0, LONG(extent.width), LONG(extent.height)};
    if (!AdjustWindowRectEx(&outer, DWORD(GetWindowLongPtrW(window, GWL_STYLE)),
            GetMenu(window) != nullptr, DWORD(GetWindowLongPtrW(window, GWL_EXSTYLE)))) return false;
    // The normal sizing notification may constrain a render-sized window to
    // desktop/engine limits. Retry only that case without WM_WINDOWPOSCHANGING;
    // WM_WINDOWPOSCHANGED / WM_SIZE still run, and the real client is verified.
    for(unsigned attempt=1;attempt<=2;++attempt) {
        const UINT flags=SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE|
            (attempt==2?SWP_NOSENDCHANGING:0);
        SetLastError(ERROR_SUCCESS);
        const BOOL resized=SetWindowPos(window,nullptr,0,0,outer.right-outer.left,outer.bottom-outer.top,flags);
        const DWORD error=resized?ERROR_SUCCESS:GetLastError();
        const BOOL measured=GetClientRect(window,&client);
        if(diagnostic)*diagnostic={attempt,error,measured?client.right:0,measured?client.bottom:0};
        if(resized&&measured&&client.right==extent.width&&client.bottom==extent.height)return true;
        if(!resized||!measured)return false;
    }
    return false;
}
}

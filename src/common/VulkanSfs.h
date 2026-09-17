#pragma once
#include <windows.h>

namespace kharvox {
inline bool vulkanSfsEnabled() {
    static const bool enabled=[] {
        wchar_t value[8]{};
        return GetEnvironmentVariableW(L"KHARVOX_VULKAN_SFS",value,8)==1 && value[0]==L'1';
    }();
    return enabled;
}
}

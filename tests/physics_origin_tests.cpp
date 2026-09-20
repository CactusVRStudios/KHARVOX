#include <Windows.h>
#include <array>
#include <cassert>
#include <cmath>
#include <limits>

using PhysicsGetOriginFn = const float*(__fastcall*)(void*, int);
bool readableMemory(const void* address, size_t) { return address != nullptr; }
bool executableMemory(const void* address) { return address != nullptr; }
#include "../src/camera/PhysicsOrigin.inc"

struct Physics {
    void** vtable;
    const float* origin;
};
const float* __fastcall getOrigin(void* physics, int index) {
    assert(index == 0);
    return static_cast<Physics*>(physics)->origin;
}
const float* __fastcall faultingOrigin(void*, int) {
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    return nullptr;
}

int main() {
    std::array<void*, 17> vtable{};
    vtable[16] = reinterpret_cast<void*>(getOrigin);
    float coordinates[3]{1, 2, 3};
    Physics physics{vtable.data(), coordinates};
    float output[3]{};
    assert(readPhysicsOriginSafely(&physics, output));
    assert(output[0] == 1 && output[1] == 2 && output[2] == 3);
    const auto rejected = [&](void* candidate) {
        output[0] = 7; output[1] = 8; output[2] = 9;
        assert(!readPhysicsOriginSafely(candidate, output));
        assert(output[0] == 7 && output[1] == 8 && output[2] == 9);
    };
    rejected(nullptr);
    assert(!readPhysicsOriginSafely(&physics, nullptr));
    physics.origin = nullptr;
    rejected(&physics);
    physics.origin = coordinates;
    for (int index = 0; index < 3; ++index) {
        const auto saved = coordinates[index];
        coordinates[index] = std::numeric_limits<float>::quiet_NaN();
        rejected(&physics);
        coordinates[index] = std::numeric_limits<float>::infinity();
        rejected(&physics);
        coordinates[index] = saved;
    }
    physics.vtable = nullptr;
    rejected(&physics);
    physics.vtable = vtable.data();
    vtable[16] = nullptr;
    rejected(&physics);
    vtable[16] = reinterpret_cast<void*>(faultingOrigin);
    rejected(&physics);

    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    auto pages = static_cast<unsigned char*>(VirtualAlloc(nullptr,
        info.dwPageSize * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    assert(pages);
    DWORD oldProtection{};
    assert(VirtualProtect(pages + info.dwPageSize, info.dwPageSize,
        PAGE_NOACCESS, &oldProtection));
    void* revoked = pages + info.dwPageSize;
    rejected(revoked);
    physics.vtable = static_cast<void**>(revoked);
    rejected(&physics);
    physics.vtable = vtable.data();
    vtable[16] = revoked;
    rejected(&physics);
    vtable[16] = reinterpret_cast<void*>(getOrigin);
    physics.origin = static_cast<float*>(revoked);
    rejected(&physics);
    auto partial = reinterpret_cast<float*>(pages + info.dwPageSize - sizeof(float));
    *partial = 42;
    physics.origin = partial;
    rejected(&physics);
    assert(VirtualFree(pages, 0, MEM_RELEASE));
    physics.origin = coordinates;
    assert(readPhysicsOriginSafely(&physics, output));
    assert(output[0] == 1 && output[1] == 2 && output[2] == 3);
}

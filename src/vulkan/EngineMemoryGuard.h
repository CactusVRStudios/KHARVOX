#pragma once
#include "EngineMemoryCapacity.h"
#include "../common/RuntimeLog.h"
#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace kharvox {
using EngineMemoryAllocate = void* (__fastcall*)(void*,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint64_t,uint64_t);
inline EngineMemoryAllocate engineMemoryAllocateOriginal{};
inline const uint8_t* engineMemoryAllocator{};
inline std::atomic<bool> engineMemoryStartupComplete{false};
inline bool engineMemorySfsDedicated{};

[[noreturn]] inline void stopOversizedEngineImage(uint32_t bytes,uint32_t alignment,
    uint32_t types,uint32_t flags,uint64_t capacity) {
    char message[384]{};
    const int length=std::snprintf(message,sizeof(message),
        "[KHARVOX][MEMORY-CAPACITY] Startup stopped: imageBytes=%u alignment=%u memoryTypeBits=0x%x flags=0x%x poolBlockBytes=%llu exitCode=0x4B480001\r\n",
        bytes,alignment,types,flags,static_cast<unsigned long long>(capacity));
    wchar_t path[MAX_PATH]{};
    const auto n=GetTempPathW(MAX_PATH,path);
    if(n && n+12<MAX_PATH) {
        wcscat_s(path,L"KHARVOX.log");
        const auto file=CreateFileW(path,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file!=INVALID_HANDLE_VALUE) {
            DWORD written{}; WriteFile(file,message,static_cast<DWORD>(length),&written,nullptr);
            FlushFileBuffers(file); CloseHandle(file);
        }
    }
    // Never return an invalid allocation or throw through DOOM's fatal-error
    // handler. ExitProcess can deadlock in driver DLL detach while other game
    // threads hold locks. The launcher owns cleanup and displays the error.
    TerminateProcess(GetCurrentProcess(),engineMemoryCapacityExitCode);
    for(;;) Sleep(INFINITE);
}

inline void* __fastcall guardedEngineMemoryAllocate(void* output,uint32_t bytes,
    uint32_t alignment,uint32_t tag,uint32_t types,uint32_t flags,uint64_t image,uint64_t buffer) {
    if(engineMemorySfsDedicated||!engineMemoryStartupComplete.load(std::memory_order_relaxed)) {
        uint64_t deviceLocal{},hostVisible{};
        std::memcpy(&deviceLocal,engineMemoryAllocator+0x18,sizeof(deviceLocal));
        std::memcpy(&hostVisible,engineMemoryAllocator+0x28,sizeof(hostVisible));
        const auto capacity=enginePoolCapacity(flags,deviceLocal,hostVisible);
        const auto effective=engineImageAllocationFlags(bytes,flags,image!=0,capacity,engineMemorySfsDedicated);
        if(effective!=flags){
            char detail[256]{};
            std::snprintf(detail,sizeof(detail),"SFS dedicated engine image: bytes=%u poolBlockBytes=%llu flags=0x%x effectiveFlags=0x%x",bytes,static_cast<unsigned long long>(capacity),flags,effective);
            writeRuntimeLog("[MEMORY-CAPACITY]",detail,true);
            flags=effective;
        }
        if(oversizedEnginePoolImage(bytes,flags,image!=0,capacity))
            stopOversizedEngineImage(bytes,alignment,types,flags,capacity);
    }
    return engineMemoryAllocateOriginal(output,bytes,alignment,tag,types,flags,image,buffer);
}

inline bool installEngineMemoryGuard(uint8_t* base,uint32_t imageSize,bool nativeSfsVr=false) {
    if(engineMemoryAllocateOriginal)return true;
    if(imageSize<0x571d2f0)return false;
    // Full wrapper signature verifies argument forwarding, allocator address
    // and called allocator RVA for the supported DOOM 20240321 executable.
    constexpr uint8_t signature[]={
        0x40,0x53,0x48,0x83,0xec,0x50,0x48,0x8b,0x84,0x24,0x98,0,0,0,0x48,0x8b,0xd9,
        0x48,0x89,0x44,0x24,0x40,0x48,0x8b,0x84,0x24,0x90,0,0,0,0x48,0x89,0x44,0x24,0x38,
        0x8b,0x84,0x24,0x88,0,0,0,0x89,0x44,0x24,0x30,0x8b,0x84,0x24,0x80,0,0,0,
        0x89,0x44,0x24,0x28,0x44,0x89,0x4c,0x24,0x20,0x45,0x8b,0xc8,0x44,0x8b,0xc2,
        0x48,0x8b,0xd1,0x48,0x8d,0x0d,0xc2,0xa4,0xce,0x03,0xe8,0x2d,0xda,0xff,0xff,
        0x48,0x8b,0xc3,0x48,0x83,0xc4,0x50,0x5b,0xc3};
    void* target=base+0x1a32da0;
    if(std::memcmp(target,signature,sizeof(signature)))return false;
    if(nativeSfsVr){
        // Validate bit-0 extraction and its branch into the native standalone
        // vkAllocateMemory path, plus the non-pooled ownership record.
        constexpr uint8_t flagsCode[]={0x44,0x8b,0x8d,0x50,0x01,0,0,0x45,0x0f,0xb6,0xc1,0x41,0x80,0xe0,0x01};
        constexpr uint8_t branchCode[]={0x45,0x84,0xc0,0x0f,0x84,0xb1,0x01,0,0};
        constexpr uint8_t ownershipCode[]={0x41,0xff,0x47,0x04,0x4d,0x01,0x67,0x08,0x48,0x89,0x7d,0x88,0xc6,0x45,0xa0,0,0x33,0xff,0x89,0x7d,0xb0,0x48,0x89,0x7d,0x90,0x4c,0x89,0x65,0xa8};
        if(std::memcmp(base+0x1a308cb,flagsCode,sizeof(flagsCode))||std::memcmp(base+0x1a30980,branchCode,sizeof(branchCode))||std::memcmp(base+0x1a30a9e,ownershipCode,sizeof(ownershipCode)))return false;
    }
    if(MH_CreateHook(target,reinterpret_cast<void*>(&guardedEngineMemoryAllocate),
        reinterpret_cast<void**>(&engineMemoryAllocateOriginal))!=MH_OK)return false;
    engineMemoryAllocator=base+0x571d2b0;
    engineMemorySfsDedicated=nativeSfsVr;
    if(MH_EnableHook(target)==MH_OK)return true;
    MH_RemoveHook(target); engineMemoryAllocateOriginal=nullptr; engineMemoryAllocator=nullptr;
    return false;
}
}

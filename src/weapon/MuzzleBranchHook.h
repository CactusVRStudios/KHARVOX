#pragma once
#include <MinHook.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace kharvox {
class MuzzleBranchHook {
    alignas(4) volatile LONG enabled_{};
    std::atomic<bool> installed_{};
public:
    bool install(unsigned char* branch,const std::array<unsigned char,6>& expected){
        static_assert(sizeof(void*)==8);
        if(installed_.load(std::memory_order_acquire))return true;
        if(!branch||expected[0]!=0x0f||expected[1]!=0x85
            ||std::memcmp(branch,expected.data(),expected.size()))return false;
        const auto initialized=MH_Initialize();
        if(initialized!=MH_OK&&initialized!=MH_ERROR_ALREADY_INITIALIZED)return false;
        std::array<unsigned char,50> code{
            0x9c,0x50,0x48,0xb8,0,0,0,0,0,0,0,0,
            0x83,0x38,0x00,0x58,0x75,0x11,0x9d,0x75,0x0f,
            0xff,0x25,0,0,0,0,0,0,0,0,0,0,0,0,
            0x9d,0xff,0x25,0,0,0,0,0,0,0,0,0,0,0,0};
        int32_t displacement{};
        std::memcpy(&displacement,expected.data()+2,sizeof(displacement));
        const auto flag=reinterpret_cast<uintptr_t>(&enabled_);
        const auto fallthrough=reinterpret_cast<uintptr_t>(branch)+expected.size();
        const auto taken=uintptr_t(intptr_t(fallthrough)+displacement);
        std::memcpy(code.data()+4,&flag,sizeof(flag));
        std::memcpy(code.data()+27,&fallthrough,sizeof(fallthrough));
        std::memcpy(code.data()+42,&taken,sizeof(taken));
        const auto stub=VirtualAlloc(nullptr,code.size(),MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
        if(!stub)return false;
        std::memcpy(stub,code.data(),code.size());
        DWORD previous{};
        if(!VirtualProtect(stub,code.size(),PAGE_EXECUTE_READ,&previous)
            ||!FlushInstructionCache(GetCurrentProcess(),stub,code.size())){
            VirtualFree(stub,0,MEM_RELEASE);return false;
        }
        if(MH_CreateHook(branch,stub,nullptr)!=MH_OK){VirtualFree(stub,0,MEM_RELEASE);return false;}
        if(MH_EnableHook(branch)!=MH_OK){
            if(MH_RemoveHook(branch)==MH_OK)VirtualFree(stub,0,MEM_RELEASE);
            return false;
        }
        installed_.store(true,std::memory_order_release);
        return true;
    }
    bool set(bool enabled){
        if(!installed_.load(std::memory_order_acquire))return false;
        return InterlockedExchange(&enabled_,enabled?1:0)!=(enabled?1:0);
    }
};
}

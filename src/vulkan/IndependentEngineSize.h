#pragma once
#include <windows.h>
#include <MinHook.h>
#include <cstdint>
#include <cstring>
#include "EngineMemoryGuard.h"
#include "VirtualTextureGuard.h"

namespace kharvox {
// Supported DOOM 20240321: the window object's public size accessors feed
// renderer setup. Native window creation/WM_SIZE retain their own backing
// values. Overriding Vulkan capabilities alone does not update the engine.
inline uint32_t independentEngineWidth{};
inline uint32_t independentEngineHeight{};
inline uint32_t __fastcall independentWidth(void*) { return independentEngineWidth; }
inline uint32_t __fastcall independentHeight(void*) { return independentEngineHeight; }

inline bool installIndependentEngineSize(uint32_t width,uint32_t height) {
    if(independentEngineWidth)return independentEngineWidth==width&&independentEngineHeight==height;
    if(!width||!height)return false;
    auto base=reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    if(!base)return false;
    auto dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if(!base||dos->e_magic!=IMAGE_DOS_SIGNATURE)return false;
    auto nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    if(nt->Signature!=IMAGE_NT_SIGNATURE||nt->OptionalHeader.SizeOfImage<0x384b438)return false;
    constexpr uint8_t widthCode[]={0x8b,0x05,0xaa,0x9f,0xfd,0x01,0xc3,0xcc};
    constexpr uint8_t heightCode[]={0x8b,0x05,0xde,0xa1,0xfd,0x01,0xc3,0xcc};
    void* widthTarget=base+0x1871480;void* heightTarget=base+0x1871250;
    if(std::memcmp(widthTarget,widthCode,sizeof(widthCode))||std::memcmp(heightTarget,heightCode,sizeof(heightCode)))return false;
    auto initialized=MH_Initialize();if(initialized!=MH_OK&&initialized!=MH_ERROR_ALREADY_INITIALIZED)return false;
    if(!installEngineMemoryGuard(base,nt->OptionalHeader.SizeOfImage))return false;
    if(!installVirtualTextureGuard(base,nt->OptionalHeader.SizeOfImage))return false;
    if(MH_CreateHook(widthTarget,reinterpret_cast<void*>(&independentWidth),nullptr)!=MH_OK)return false;
    if(MH_CreateHook(heightTarget,reinterpret_cast<void*>(&independentHeight),nullptr)!=MH_OK){MH_RemoveHook(widthTarget);return false;}
    independentEngineWidth=width;independentEngineHeight=height;
    if(MH_EnableHook(widthTarget)==MH_OK&&MH_EnableHook(heightTarget)==MH_OK)return true;
    MH_DisableHook(widthTarget);MH_DisableHook(heightTarget);MH_RemoveHook(widthTarget);MH_RemoveHook(heightTarget);
    independentEngineWidth=independentEngineHeight=0;return false;
}
}

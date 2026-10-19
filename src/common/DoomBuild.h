#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>

namespace kharvox::game {
enum class Build { Unknown, Steam20240321, Gog20250225 };
struct AddressPair { uint32_t steam,gog; };
struct GuardByte { uint32_t gog;unsigned char steamByte,gogByte; };
struct GuardHash { uint32_t steam,length;uint64_t steamHash,gogHash; };
#include "DoomBuildData.inc"
inline uint64_t hashBytes(const unsigned char* bytes,size_t size){
    uint64_t hash=14695981039346656037ull;
    for(size_t i=0;i<size;++i)hash=(hash^bytes[i])*1099511628211ull;
    return hash;
}
inline Build identify(uint32_t timestamp,uint32_t size,uint64_t textHash){
    if(timestamp==1711036533&&size==0x14161000&&textHash==steamTextHash)return Build::Steam20240321;
    if(timestamp==1740486338&&size==0x1415f000&&textHash==gogTextHash)return Build::Gog20250225;
    return Build::Unknown;
}
inline Build inspectImage(const unsigned char* image){
    if(!image)return Build::Unknown;
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE||dos->e_lfanew<64||dos->e_lfanew>4096)return Build::Unknown;
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(image+dos->e_lfanew);
    if(nt->Signature!=IMAGE_NT_SIGNATURE||nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64)return Build::Unknown;
    const auto ts=nt->FileHeader.TimeDateStamp,size=nt->OptionalHeader.SizeOfImage;
    if(!((ts==1711036533&&size==0x14161000)||(ts==1740486338&&size==0x1415f000)))return Build::Unknown;
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i){
        const auto& section=IMAGE_FIRST_SECTION(nt)[i];
        if(std::memcmp(section.Name,".text\0\0\0",8))continue;
        if(section.VirtualAddress!=0x1000||section.Misc.VirtualSize>size-section.VirtualAddress)return Build::Unknown;
        return identify(ts,size,hashBytes(image+section.VirtualAddress,section.Misc.VirtualSize));
    }
    return Build::Unknown;
}
// Initialize before the first game detour. Subsequent calls retain the
// original identity even after KHARVOX patches the executable's code.
inline Build build(){static const Build value=inspectImage(reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr)));return value;}
inline const char* name(){return build()==Build::Steam20240321?"Steam 2024-03-21":build()==Build::Gog20250225?"GOG 2025-02-25 (experimental)":"unsupported executable";}
inline uintptr_t mappedRva(Build profile,uintptr_t steam){
    if(profile!=Build::Gog20250225)return steam;
    const auto* found=std::lower_bound(std::begin(addresses),std::end(addresses),steam,[](const auto& a,uintptr_t b){return a.steam<b;});
    return found!=std::end(addresses)&&found->steam==steam?found->gog:0;
}
inline uintptr_t rva(uintptr_t steam){return mappedRva(build(),steam);}
inline uintptr_t canonicalRva(Build profile,uintptr_t actual){
    if(profile!=Build::Gog20250225)return actual;
    for(const auto& a:addresses)if(a.gog==actual)return a.steam;
    // GOG addresses can numerically equal unrelated Steam addresses. Never
    // let an unmapped object/caller impersonate a known canonical owner.
    return 0;
}
inline uintptr_t canonicalRva(uintptr_t actual){return canonicalRva(build(),actual);}
inline unsigned char translatedGuardByte(uintptr_t actual,unsigned char expected){
    const auto* p=std::lower_bound(std::begin(guardBytes),std::end(guardBytes),actual,[](const auto& a,uintptr_t b){return a.gog<b;});
    for(;p!=std::end(guardBytes)&&p->gog==actual;++p)if(p->steamByte==expected)return p->gogByte;
    return expected;
}
inline int compare(const void* actual,const void* expected,size_t count){
    if(build()!=Build::Gog20250225)return std::memcmp(actual,expected,count);
    static const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto start=reinterpret_cast<uintptr_t>(actual)-base;
    if(start<0x1000||start>=0x1fc0acc||count>0x1fc0acc-start)
        return std::memcmp(actual,expected,count);
    const auto* a=static_cast<const unsigned char*>(actual);const auto* e=static_cast<const unsigned char*>(expected);
    for(size_t i=0;i<count;++i){const auto want=translatedGuardByte(start+i,e[i]);if(a[i]!=want)return int(a[i])-int(want);}
    return 0;
}
inline uint64_t expectedHash(uintptr_t steam,uint32_t count,uint64_t original){
    if(build()!=Build::Gog20250225)return original;
    for(const auto& h:guardHashes)if(h.steam==steam&&h.length==count&&h.steamHash==original)return h.gogHash;
    return 0;
}
}

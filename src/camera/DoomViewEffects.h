#pragma once
#include <windows.h>
#include <cstddef>
#include <cstring>

namespace kharvox {
// Fingerprinted DOOM 20240321 layout. The live booleans are read by the
// native view-effects path; launch arguments alone can leave them at zero.
inline constexpr size_t viewShakesValueRva=0x5bcffd0;
inline constexpr size_t viewKicksValueRva=0x5bd0110;
inline bool viewEffectsLayoutMatches(const unsigned char* image,size_t size) {
    constexpr unsigned char guard[]{0x44,0x39,0x25,0xbd,0x3c,0xd6,0x04,0x75,0x40};
    return image&&size>=viewKicksValueRva+sizeof(LONG)
        &&!std::memcmp(image+0x22b7668,"view_skipShakes",sizeof("view_skipShakes"))
        &&!std::memcmp(image+0x22b76e8,"view_skipKicks",sizeof("view_skipKicks"))
        &&!std::memcmp(image+0xe6c30c,guard,sizeof(guard));
}
// Report the old values as a change mask. Already-correct values require no
// write. Unexpected representations are rejected, never coerced blindly.
inline unsigned enforceViewEffects(unsigned char* image,size_t size) {
    if(!viewEffectsLayoutMatches(image,size))return 4;
    auto shakes=reinterpret_cast<volatile LONG*>(image+viewShakesValueRva);
    auto kicks=reinterpret_cast<volatile LONG*>(image+viewKicksValueRva);
    const LONG a=InterlockedCompareExchange(shakes,0,0),b=InterlockedCompareExchange(kicks,0,0);
    if((a!=0&&a!=1)||(b!=0&&b!=1))return 4;
    unsigned changed=0;
    if(a==0){InterlockedExchange(shakes,1);changed|=1;}
    if(b==0){InterlockedExchange(kicks,1);changed|=2;}
    return changed;
}
}

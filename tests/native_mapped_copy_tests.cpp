#define NOMINMAX
#include <windows.h>
#include "../src/native/NativeMappedCopy.h"
#include <algorithm>
#include <cstdio>
#include <vector>

int main() {
    constexpr size_t page = 4096;
    auto* source = static_cast<unsigned char*>(VirtualAlloc(nullptr, page*3, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE));
    if(!source) return 1;
    DWORD old{};
    if(!VirtualProtect(source+page*2,page,PAGE_NOACCESS,&old)) return 2;
    for(size_t i=0;i<page*2;++i) source[i]=static_cast<unsigned char>((i*97)^(i>>3));
    // All alignments, short/tail/large sizes, and source spans ending immediately
    // at a guard page: catches out-of-bounds SIMD reads, not just value changes.
    for(bool stream : {false,true}) {
        if(stream && !kharvox::native::streamingMappedCopyAvailable()) continue;
        for(size_t offset=0;offset<64;++offset) for(size_t size : {size_t(0),size_t(1),size_t(15),size_t(16),size_t(63),size_t(64),size_t(65),size_t(127),size_t(1023),page}) {
            std::vector<unsigned char> dst(size+128,0xa5);
            kharvox::native::copyMappedInput(dst.data()+offset,source+offset,size,stream);
            if(!std::equal(source+offset,source+offset+size,dst.data()+offset)) return 3;
            for(size_t i=0;i<dst.size();++i) if((i<offset||i>=offset+size)&&dst[i]!=0xa5) return 4;
        }
        for(size_t size=0;size<512;++size) {
            std::vector<unsigned char> dst(size+1,0xa5);
            kharvox::native::copyMappedInput(dst.data(),source+page*2-size,size,stream);
            if(!std::equal(source+page*2-size,source+page*2,dst.data())||dst[size]!=0xa5) return 5;
        }
    }
    VirtualFree(source,0,MEM_RELEASE);
    std::puts("Mapped copies match at all alignments, tails and guard boundaries; scalar fallback passed.");
}

#include "../src/vulkan/VirtualTextureGuard.h"
#include <memory>
#include <stdexcept>
#include <iostream>
void check(bool value) { if(!value) throw std::runtime_error("VT guard regression"); }
int main() {
    auto dst=std::make_unique<kharvox::VtPageList>();
    auto src=std::make_unique<kharvox::VtPageList>();
    src->count=3719; dst->count=4582;
    for(size_t i=0;i<8192;++i) {src->pages[i]=i+123;dst->pages[i]=42;}
    const auto original=*src;
    auto result=kharvox::appendVirtualTexturePages(dst.get(),src.get());
    check(result.valid&&result.appended==3610&&result.deferred==109&&dst->count==8192);
    check(dst->pages[4581]==42&&dst->pages[4582]==123&&dst->pages[8191]==3732);
    check(memcmp(src.get(),&original,sizeof(original))==0);
    result=kharvox::appendVirtualTexturePages(dst.get(),src.get());
    check(result.valid&&result.appended==0&&result.deferred==3719);
    dst->count=0;
    result=kharvox::appendVirtualTexturePages(dst.get(),src.get());
    check(result.valid&&result.appended==3719&&result.deferred==0);
    dst->count=8193;check(!kharvox::appendVirtualTexturePages(dst.get(),src.get()).valid);
    dst->count=0;src->count=8193;check(!kharvox::appendVirtualTexturePages(dst.get(),src.get()).valid);
    src->count=0;check(kharvox::appendVirtualTexturePages(dst.get(),src.get()).valid);
    check(!kharvox::appendVirtualTexturePages(src.get(),src.get()).valid);
    // Execute the actual generated x64 thunk and inline jump against synthetic
    // engine memory, not just the C++ policy. The native continuation returns.
    constexpr size_t imageSize=0x17e4000;
    auto image=static_cast<uint8_t*>(VirtualAlloc(nullptr,imageSize,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));
    check(image!=nullptr);
    check(!kharvox::installVirtualTextureGuard(image,imageSize));
    memcpy(image+0x17e3567,kharvox::vtAppendSignature,sizeof(kharvox::vtAppendSignature));
    check(!kharvox::installVirtualTextureGuard(image,imageSize));
    memcpy(image+0x17e35d7,kharvox::vtResidencySignature,sizeof(kharvox::vtResidencySignature));
    check(kharvox::installVirtualTextureGuard(image,imageSize));
    image[0x17e35d7]=0x5b;image[0x17e35d8]=0xc3; // synthetic continuation: pop rbx; ret
    auto object=std::make_unique<uint8_t[]>(0x20a00);
    auto dp=dst.get();auto sp=src.get();
    memcpy(object.get()+0x209a8,&dp,8);memcpy(object.get()+0x209c0,&sp,8);
    dst->count=4582;src->count=3719;
    uint8_t entry[]={0x53,0x48,0x89,0xcb,0x48,0xb8,0,0,0,0,0,0,0,0,0xff,0xe0};
    auto address=reinterpret_cast<uintptr_t>(image+0x17e3567);memcpy(entry+6,&address,8);
    memcpy(image,entry,sizeof(entry));FlushInstructionCache(GetCurrentProcess(),image,imageSize);
    reinterpret_cast<void(*)(void*)>(image)(object.get());
    check(dst->count==8192&&src->count==3719&&dst->pages[8191]==3732);
    VirtualFree(image,0,MEM_RELEASE);
    std::cout<<"VT overflow policy and executable thunk passed\n";
}

#pragma once
#include <windows.h>
#include <cstddef>
namespace kharvox::intro {
struct Asset { const void* data{}; std::size_t size{}; };
inline Asset asset(int id){
    HMODULE module{};GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&asset),&module);auto resource=FindResourceW(module,MAKEINTRESOURCEW(id),MAKEINTRESOURCEW(10));
    if(!resource)return {};auto loaded=LoadResource(module,resource);if(!loaded)return {};
    return {LockResource(loaded),SizeofResource(module,resource)};
}
struct PrivateFont {
    HANDLE handle{};
    PrivateFont(){auto bytes=asset(101);DWORD count{};if(bytes.data)handle=AddFontMemResourceEx(const_cast<void*>(bytes.data),DWORD(bytes.size),nullptr,&count);}
    ~PrivateFont(){if(handle)RemoveFontMemResourceEx(handle);}
    PrivateFont(const PrivateFont&)=delete;
    PrivateFont& operator=(const PrivateFont&)=delete;
};
}

#pragma once
#include <windows.h>
#include <cstring>
#include <string>

namespace kharvox {
// Validated Beta renderer defaults; no deployment marker files required.
namespace rendererDefaults {
inline constexpr bool earlyXrRelease=true, descriptorReuse=true, freshShadows=true,
    gpuInputCopies=true, uncachedShadows=true, compiledImagePlans=true, snapshotBatch=true;
}
inline std::wstring logPath(const wchar_t* name) {
    wchar_t temp[32768]{};
    const auto length=GetTempPathW(32768,temp);
    if(!length||length>=32768) return {}; // Never fall back to the release/game directory.
    std::wstring file(name), directory(temp);
    const auto dot=file.find_last_of(L'.');
    if(dot!=std::wstring::npos&&file.substr(dot)==L".log") {
        for(size_t i=0;i<dot;++i) {
            if(file[i]==L'_')file[i]=L'-';
            else if(file[i]>=L'a'&&file[i]<=L'z')file[i]-=L'a'-L'A';
        }
        if(file.rfind(L"KHARVOX-",0)!=0)file=L"KHARVOX-"+file;
    } else {
        directory+=L"KHARVOX-Diagnostics\\";
        CreateDirectoryW(directory.c_str(),nullptr);
    }
    return directory+file;
}
inline std::string logPathA(const char* name) {
    std::wstring file;
    while(*name)file.push_back(static_cast<unsigned char>(*name++));
    const auto wide=logPath(file.c_str());
    const int size=WideCharToMultiByte(CP_UTF8,0,wide.c_str(),-1,nullptr,0,nullptr,nullptr);
    std::string result(size,'\0');
    if(size)WideCharToMultiByte(CP_UTF8,0,wide.c_str(),-1,result.data(),size,nullptr,nullptr);
    if(!result.empty())result.pop_back();
    return result;
}
inline std::wstring runtimeDirectory() {
    HMODULE module{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&runtimeDirectory), &module);
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(module, path, MAX_PATH);
    std::wstring result(path);
    const auto slash = result.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : result.substr(0, slash);
}
inline std::wstring runtimePath(const wchar_t* name) { return runtimeDirectory() + L"\\" + name; }
inline std::string runtimePathA(const char* name) {
    std::wstring file;
    while (*name) file.push_back(static_cast<unsigned char>(*name++));
    const auto wide = runtimePath(file.c_str());
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, result.data(), size, nullptr, nullptr);
    if (!result.empty()) result.pop_back();
    return result;
}
inline bool runtimeFileExists(const wchar_t* name) { return GetFileAttributesW(runtimePath(name).c_str()) != INVALID_FILE_ATTRIBUTES; }
}

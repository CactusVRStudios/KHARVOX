#pragma once
#include "../common/DoomBuild.h"
#include "VirtualTextureAppend.h"
#include "VirtualTextureAppendSignature.h"
#include "VirtualTextureDiagnostics.h"
#include "../common/RuntimeLog.h"
#include "../common/VulkanSfs.h"
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <vector>
#include <mutex>
#include <sstream>

namespace kharvox {
inline void vtGuardLog(const char* text) {
    char path[MAX_PATH]{};
    if (!GetTempPathA(MAX_PATH, path)) return;
    strcat_s(path, "KHARVOX.log");
    auto file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written{};
        WriteFile(file, text, static_cast<DWORD>(strlen(text)), &written, nullptr);
        CloseHandle(file);
    }
}
inline void observeVtAppend(std::uintptr_t owner,const VtPageList& source,
    std::uint64_t destinationBefore,const VtAppendResult& result) noexcept {
    if(!extendedDiagnosticsEnabled())return;
    try {
        static std::mutex mutex;
        static VtAppendDiagnostics diagnostics;
        static std::uint64_t firstTick{},nextReport{};
        std::lock_guard<std::mutex> guard(mutex);
        const auto now=GetTickCount64();
        diagnostics.observe(owner,source,destinationBefore,result,now);
        if(now<nextReport)return;
        if(!firstTick)firstTick=now;
        nextReport=now+1000;
        const auto& s=diagnostics.statistics();
        std::ostringstream out;
        out<<"[VT-STATS] renderer="<<(vulkanSfsEnabled()?"SFS":"AER")
            <<" pid="<<GetCurrentProcessId()
            <<" elapsed_ms="<<now-firstTick<<" capacity="<<vtPageCapacity
            <<" calls="<<s.calls<<" request_observations="<<s.requests
            <<" appended="<<s.appended<<" deferred_observations="<<s.deferred
            <<" saturated_calls="<<s.saturatedCalls<<" destination_peak="<<s.destinationPeak
            <<" observed_retries="<<s.observedRetries<<" retry_wait_ms_total="<<s.retryWaitMs
            <<" retry_wait_ms_max="<<s.maxRetryWaitMs<<" tracked_pending="<<diagnostics.pendingCount()
            <<" untracked_deferrals="<<s.untrackedDeferrals
            <<" gpu_residency=unavailable virtual_page_mip=opaque";
        writeRuntimeLog("VT",out.str());
    } catch(...) { /* Optional diagnostics must not change native residency. */ }
}
inline void __fastcall guardedVtAppend(uint8_t* object) {
    uint32_t destinationIndex{}, sourceIndex{};
    std::memcpy(&destinationIndex, object + 0x209a0, 4);
    std::memcpy(&sourceIndex, object + 0x209b8, 4);
    VtPageList *destination{}, *source{};
    if (destinationIndex <= 1 && sourceIndex <= 1) {
        std::memcpy(&destination, object + 0x209a8 + destinationIndex * 8, 8);
        std::memcpy(&source, object + 0x209c0 + sourceIndex * 8, 8);
    }
    auto result = appendVirtualTexturePages(destination, source);
    if (!result.valid) {
        vtGuardLog("[KHARVOX][VT-GUARD] Invalid virtual-texture list state; stopping before memory corruption. exit=0x4B480004\r\n");
        TerminateProcess(GetCurrentProcess(), 0x4B480004);
        for (;;) Sleep(INFINITE);
    }
    observeVtAppend(reinterpret_cast<std::uintptr_t>(object),*source,
        destination->count-result.appended,result);
    if (result.deferred) {
        static std::atomic<unsigned> reports{};
        if (reports.fetch_add(1) < 8) {
            char message[256]{};
            sprintf_s(message, "[KHARVOX][VT-GUARD] capacity=8192 appended=%llu deferred=%llu; source retained for native residency retry\r\n",
                static_cast<unsigned long long>(result.appended), static_cast<unsigned long long>(result.deferred));
            vtGuardLog(message);
        }
    }
}
// The replaced block is inside a function, with RSP 16-byte aligned and RBX
// pointing to VirtualTextureSystem. Preserve every volatile GPR, XMM0..5 and
// flags; the surrounding native code remains responsible for residency/swap.
inline std::vector<uint8_t> vtAppendThunk(uintptr_t helper, uintptr_t continuation) {
    std::vector<uint8_t> code;
    auto emit = [&](std::initializer_list<uint8_t> bytes) { code.insert(code.end(), bytes); };
    auto address = [&](uintptr_t value) { for (int i=0;i<8;++i) code.push_back(static_cast<uint8_t>(value>>(i*8))); };
    emit({0x9c,0x50,0x51,0x52,0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53});
    emit({0x48,0x81,0xec,0x80,0,0,0});
    for (uint8_t i=0;i<6;++i) emit({0xf3,0x0f,0x7f,static_cast<uint8_t>(0x44+i*8),0x24,static_cast<uint8_t>(0x20+i*16)});
    emit({0x48,0x89,0xd9,0x48,0xb8}); address(helper); emit({0xff,0xd0});
    for (uint8_t i=0;i<6;++i) emit({0xf3,0x0f,0x6f,static_cast<uint8_t>(0x44+i*8),0x24,static_cast<uint8_t>(0x20+i*16)});
    emit({0x48,0x81,0xc4,0x80,0,0,0,0x41,0x5b,0x41,0x5a,0x41,0x59,0x41,0x58,0x5a,0x59,0x58,0x9d});
    emit({0xff,0x25,0,0,0,0}); address(continuation);
    return code;
}
inline bool installVirtualTextureGuard(uint8_t* base, uint32_t imageSize) {
    if (imageSize < kharvox::game::rva(0x17e35d7) + sizeof(vtResidencySignature)) return false;
    auto target = base + kharvox::game::rva(0x17e3567);
    if (kharvox::game::compare(target, vtAppendSignature, sizeof(vtAppendSignature))) return false;
    if (kharvox::game::compare(base+kharvox::game::rva(0x17e35d7), vtResidencySignature, sizeof(vtResidencySignature))) return false;
    auto bytes = vtAppendThunk(reinterpret_cast<uintptr_t>(&guardedVtAppend), reinterpret_cast<uintptr_t>(base+kharvox::game::rva(0x17e35d7)));
    auto thunk = VirtualAlloc(nullptr, bytes.size(), MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
    if (!thunk) return false;
    memcpy(thunk, bytes.data(), bytes.size());
    DWORD old{};
    if (!VirtualProtect(thunk, bytes.size(), PAGE_EXECUTE_READ, &old)) { VirtualFree(thunk,0,MEM_RELEASE); return false; }
    FlushInstructionCache(GetCurrentProcess(), thunk, bytes.size());
    uint8_t jump[14]={0xff,0x25,0,0,0,0};
    memcpy(jump+6,&thunk,8);
    if (!VirtualProtect(target,sizeof(jump),PAGE_EXECUTE_READWRITE,&old)) { VirtualFree(thunk,0,MEM_RELEASE); return false; }
    // Installed during initial renderer setup, before the texture worker runs.
    memcpy(target,jump,sizeof(jump));
    DWORD unused{};
    VirtualProtect(target,sizeof(jump),old,&unused);
    FlushInstructionCache(GetCurrentProcess(),target,sizeof(jump));
    vtGuardLog("[KHARVOX][VT-GUARD] DOOM 20240321 bounded texture-page append installed (capacity=8192)\r\n");
    return true;
}
}

#pragma once
#include "HandPerformanceComparison.h"
#include "../common/RuntimePaths.h"
#include "../common/PerformanceCpuCounters.h"
#include <windows.h>
#include <dxgi1_4.h>
#include <fstream>

namespace kharvox {
class PerformanceComparisonLog {
    static bool requested(){
        char value[8]{};
        return GetEnvironmentVariableA("KHARVOX_PERFORMANCE_COMPARISON",value,sizeof(value))==1&&value[0]=='1';
    }
    HandPerformanceComparison comparison_{requested()};
    ULONGLONG beginCpu_{},beginTick_{};
    static ULONGLONG processCpu(){
        FILETIME created{},exited{},kernel{},user{};
        if(!GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user))return 0;
        ULARGE_INTEGER k{},u{};k.LowPart=kernel.dwLowDateTime;k.HighPart=kernel.dwHighDateTime;
        u.LowPart=user.dwLowDateTime;u.HighPart=user.dwHighDateTime;return k.QuadPart+u.QuadPart;
    }
    static void videoMemory(std::ostream&out){
        // Query the process's WDDM budget only at phase boundaries. Dynamic
        // loading keeps this optional and adds no DXGI dependency to rendering.
        const auto module=LoadLibraryW(L"dxgi.dll");if(!module)return;
        using CreateFactory=HRESULT(WINAPI*)(REFIID,void**);
        const auto create=reinterpret_cast<CreateFactory>(GetProcAddress(module,"CreateDXGIFactory1"));
        IDXGIFactory1*factory{};
        if(create&&SUCCEEDED(create(__uuidof(IDXGIFactory1),reinterpret_cast<void**>(&factory)))){
            for(UINT index=0;;++index){
                IDXGIAdapter1*adapter{};
                if(factory->EnumAdapters1(index,&adapter)!=S_OK)break;
                DXGI_ADAPTER_DESC1 desc{};adapter->GetDesc1(&desc);
                IDXGIAdapter3*budgetAdapter{};
                if(!(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)&&SUCCEEDED(adapter->QueryInterface(
                    __uuidof(IDXGIAdapter3),reinterpret_cast<void**>(&budgetAdapter)))){
                    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
                    if(SUCCEEDED(budgetAdapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&info)))
                        out<<"video_memory adapter="<<index<<" vendor="<<desc.VendorId<<" device="<<desc.DeviceId
                            <<" dedicated_bytes="<<desc.DedicatedVideoMemory<<" process_usage_bytes="<<info.CurrentUsage
                            <<" process_budget_bytes="<<info.Budget<<'\n';
                    budgetAdapter->Release();
                }
                adapter->Release();
            }
            factory->Release();
        }
        FreeLibrary(module);
    }
    void write(const char* renderer){
        // One write per phase, independent of verbose per-draw diagnostics.
        // This file is diagnostic output, never consumed as game configuration.
        std::ofstream out(runtimePathA("performance_comparison.txt"),std::ios::trunc);
        if(!out)return;
        out<<"KHARVOX hand comparison v1\nrenderer="<<renderer<<" pid="<<GetCurrentProcessId()
            <<" phase="<<comparison_.phase()<<" complete="<<comparison_.finished()
            <<"\nSame resolution, inputs and resident assets. Only custom hand drawing/depth integration changes.\n"
            <<"Windows: on / off / on; 14 seconds each, first 2 seconds excluded. Menus/cinematics excluded.\n";
        const auto tick=GetTickCount64(),cpu=processCpu();
        if(tick>beginTick_&&cpu>=beginCpu_)out<<"process_cpu_average_cores="
            <<double(cpu-beginCpu_)/10000.0/double(tick-beginTick_)<<'\n';
        videoMemory(out);
        const char*cpuNames[]{"native_hands_update","custom_hands_update","stance_read","gameplay_actions","weapon_update"};
        for(unsigned i=0;i<unsigned(PerformanceCpuScope::Count);++i){
            const auto sample=performanceCpuSnapshot(static_cast<PerformanceCpuScope>(i));
            out<<"cpu_scope="<<cpuNames[i]<<" calls="<<sample.count<<" total_ms="<<sample.totalMs
                <<" mean_ms="<<(sample.count?sample.totalMs/double(sample.count):0)<<" max_ms="<<sample.maxMs
                <<" accumulation=process_lifetime overlapping_scopes=not_additive\n";
        }
        for(int i=0;i<3;++i){const auto&w=comparison_.windows[i];
            out<<"window="<<i<<" hands="<<(i==1?"off":"on")<<" frames="<<w.frames.count
                <<" mean_frame_ms="<<w.frames.meanMs()<<" max_frame_ms="<<w.frames.maximumMs
                <<" fps="<<(w.frames.totalMs>0?double(w.frames.count)*1000/w.frames.totalMs:0)
                <<" xr_wall_mean_ms="<<w.xrWall.meanMs()<<" xr_gpu_samples="<<w.xrGpu.count
                <<" xr_gpu_mean_ms="<<w.xrGpu.meanMs()<<" xr_gpu_max_ms="<<w.xrGpu.maximumMs<<'\n';
        }
        out<<"GPU scope: XR copy+hands+HUD only; excludes prior DOOM work. AER GPU timing unavailable here.\n"
            <<"Textures stay resident when hidden: this does not exclude VRAM pressure.\n"
            <<"Remain in the same quiet scene. Different scene activity or unequal on windows makes the comparison inconclusive.\n";
    }
public:
    bool enabled()const{return comparison_.enabled();}
    bool hideHands()const{return comparison_.hideHands();}
    void update(bool gameplay,bool handsConfigured,const char*renderer){
        if(!enabled())return;
        const auto now=GetTickCount64();
        if(!beginTick_){beginTick_=now;beginCpu_=processCpu();}
        if(comparison_.update(now,gameplay,handsConfigured))write(renderer);
    }
    void gpu(double ms){comparison_.gpu(ms);}
    void xr(double ms){comparison_.xr(ms);}
};
} // namespace kharvox

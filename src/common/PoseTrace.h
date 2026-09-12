#pragma once
#include "DiagnosticLogging.h"
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <locale>
#include <string>

// CPU observations only. A programmed eye is NOT proof of the eye rendered
// by an asynchronous engine job. No GPU waits, engine writes, or hot-path I/O.
namespace kharvox::pose_trace {
enum Kind { Located=1, Programmed=2, Camera=3, CacheRecorded=4,
    Submitted=5, EndBegin=6, EndReturn=7, PresentBegin=8, HeadApplied=9,
    CameraBase=10, HeadPublished=11, QuadSubmitted=12, BodyAnchor=13,
    HeadRenderCenter=14, WorldProducerInput=15, WorldProducerOutput=16, WorldProducerMatrix=17,
    AerSourceWindow=18,AerSourceResolved=19,
    WeaponSourceRoot=20,WeaponSourceProp=21,WeaponSourceInput=22,ControllerTurnFrame=23,WeaponDrawModel=24,
    WeaponMatrixOriginal=25,WeaponMatrixSelected=26,WeaponMatrixOutput=27,WeaponMatrixView=28,
    NearbyModelPose=29 };
struct Event {
    std::int64_t qpc{}, displayTime{}, status{};
    std::uint64_t frame{}, source{}, revision{}, poseId{};
    unsigned thread{}, kind{}, flags{};
    int eye{-1};
    float data[16]{};
};
inline constexpr size_t capacity=131072;
inline SRWLOCK lock=SRWLOCK_INIT;
inline std::atomic<bool> active{};
inline std::atomic<unsigned> dropped{};
inline std::unique_ptr<Event[]> events;
inline size_t count{};
inline std::uint64_t deadline{};
inline std::int64_t startQpc{};
inline std::uint64_t startUtcFileTime{};
inline constexpr unsigned captureDurationMs=8000;
inline bool keyDown{};
inline bool record(Event event) noexcept {
    if(!active.load(std::memory_order_relaxed))return false;
    if(!TryAcquireSRWLockExclusive(&lock)){++dropped;return false;}
    bool stored=false;
    if(active.load(std::memory_order_relaxed)){
        if(count<capacity){LARGE_INTEGER now{};QueryPerformanceCounter(&now);
            event.qpc=now.QuadPart;event.thread=GetCurrentThreadId();events[count++]=event;stored=true;}
        else ++dropped;
    }
    ReleaseSRWLockExclusive(&lock);return stored;
}
inline void begin() {
    // Allocate before activation. Failure propagates only to the guarded poll.
    if(!events)events=std::make_unique<Event[]>(capacity);
    AcquireSRWLockExclusive(&lock);
    count=0;dropped=0;LARGE_INTEGER now{};QueryPerformanceCounter(&now);
    startQpc=now.QuadPart;deadline=GetTickCount64()+captureDurationMs;
    FILETIME utc{};GetSystemTimePreciseAsFileTime(&utc);
    startUtcFileTime=(std::uint64_t(utc.dwHighDateTime)<<32)|utc.dwLowDateTime;
    active.store(true,std::memory_order_release);
    ReleaseSRWLockExclusive(&lock);
}
inline void stop() noexcept {
    AcquireSRWLockExclusive(&lock);active.store(false,std::memory_order_release);
    ReleaseSRWLockExclusive(&lock);
}
inline void writeCsv(std::ostream& out) {
    out.imbue(std::locale::classic());out<<std::setprecision(9);
    out<<"qpc,kind,thread,frame,eye,flags,displayTime,source,revision,status,poseId";
    for(int i=0;i<16;++i)out<<",d"<<i;
    out<<'\n';
    for(size_t n=0;n<count;++n){const auto& e=events[n];
        out<<e.qpc<<','<<e.kind<<','<<e.thread<<','<<e.frame<<','<<e.eye<<','<<e.flags<<','<<e.displayTime<<','<<e.source<<','<<e.revision<<','<<e.status<<','<<e.poseId;
        for(float v:e.data)out<<','<<v;
        out<<'\n';
    }
}
// Called only by the serialized XR Present owner. Saving can cause one
// post-capture stall; it is deliberately outside the recorded interval.
inline std::string poll(bool gameplay=false,bool fixedQuad=false) noexcept {
    if (!kharvox::extendedDiagnosticsEnabled()) return {};
    try {
        const bool down=(GetAsyncKeyState(VK_CONTROL)&0x8000)
            &&(GetAsyncKeyState(VK_SHIFT)&0x8000)&&(GetAsyncKeyState('T')&0x8000);
        DWORD pid{};GetWindowThreadProcessId(GetForegroundWindow(),&pid);
        const bool start=down&&!keyDown&&pid==GetCurrentProcessId();keyDown=down;
        // Extended Logging alone must not schedule a large synchronous CSV save
        // during gameplay. Explicit Ctrl+Shift+T captures remain available.
        if(active.load()&&(GetTickCount64()>=deadline||start)){
            stop();wchar_t temp[MAX_PATH]{};
            if(!GetTempPathW(MAX_PATH,temp))return "[POSE-TRACE] temp path unavailable";
            const auto dir=std::filesystem::path(temp)/L"KHARVOX-PoseTraces"/
                (std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
            std::filesystem::create_directories(dir);
            std::ofstream csv(dir/L"events.csv");csv.exceptions(std::ios::badbit|std::ios::failbit);writeCsv(csv);csv.close();
            LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);
            std::ofstream meta(dir/L"capture.json");meta.exceptions(std::ios::badbit|std::ios::failbit);
            meta<<"{\"schema\":2,\"pid\":"<<GetCurrentProcessId()<<",\"qpcFrequency\":"<<frequency.QuadPart
                <<",\"startQpc\":"<<startQpc<<",\"events\":"<<count<<",\"dropped\":"<<dropped.load()
                <<",\"startUtcFileTime100ns\":"<<startUtcFileTime<<",\"plannedDurationMs\":"<<captureDurationMs
                <<",\"cpuOnly\":true,\"gpuProvenanceVerified\":false,\"saveOutsideCapture\":true}";
            meta.close();return "[POSE-TRACE] saved "+dir.string()+" events="+std::to_string(count)+" dropped="+std::to_string(dropped.load());
        }
        if(start&&!active.load()){begin();return "[POSE-TRACE] started 8-second CPU capture; Ctrl+Shift+T saves early";}
    }catch(...){stop();return "[POSE-TRACE] capture/save failed; gameplay unchanged";}
    return {};
}
inline void camera(std::uint64_t present, std::uint64_t object, const unsigned char* bytes,
    float eyeOffset, unsigned flags, std::uint64_t caller, std::uint64_t poseId=0,int anatomicalEye=-2) noexcept {
    if(!active.load(std::memory_order_relaxed))return;
    // Camera flags: 1=gameplay, 2=cinematic, 4=explicit AER anatomical eye (r264).
    Event e{};e.kind=Camera;e.frame=present;e.source=object;e.flags=flags;e.revision=caller;e.poseId=poseId;
    e.eye=eyeOffset<0?0:eyeOffset>0?1:-1;
    if(anatomicalEye!=-2)e.eye=anatomicalEye;
    std::memcpy(e.data,bytes+0xC0,12*sizeof(float));
    std::memcpy(e.data+12,bytes+0x70,2*sizeof(float));e.data[14]=eyeOffset;
    record(e);
}
}

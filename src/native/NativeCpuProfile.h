#pragma once
#include "../common/DiagnosticLogging.h"
#include <array>
#include <chrono>
#include <cstdint>
#include "../common/RuntimePaths.h"
#include "NativeThreadTiming.h"
#include "NativeCpuSamplingPolicy.h"
#include "NativePacingSequence.h"
#include "NativeFrameTrace.h"

namespace kharvox::native::cpu {
enum Slot : size_t { Root, Original, Doubled, SnapshotBind, SnapshotCopy, StorageSeed, DeviceIdle, XrWaitFrame, XrCopyCompletion, XrEndFrame, ReplayDescriptors, CaptureImageInputs, PrepareImageLayouts, ReplayPass, ValidateStorageDraw, GpuInputWait, XrQueueLock, XrQueueSubmit, XrQueueWait, ForwardDescriptors, ForwardDraw, ForwardOther, StorageSelection, ReplayVertex, ReplayPipeline, ReplayDraw, SnapshotClassLookup, SnapshotMemoLookup, SnapshotMemoRestore, SnapshotMemoMiss, SnapshotForward, Count };
struct Frame {
    bool active{}, detailed{}, sampleThreads{}, sparseDetail{}, afterSparse{};
    uint64_t serial{}, started{}, previousEnd{}, previousSerial{};
    std::array<uint64_t,Count> ns{}, calls{};
    std::array<uint64_t,Count> cpuNs{}, cycles{}, threadSamples{};
    uint64_t copiedBytes{}, seededBytes{}, seededBuffers{};
    uint64_t reusedReplayPayloads{},newReplayPayloads{},borrowedComputeBinds{};
    uint64_t bindingMemoHits{},bindingMemoMisses{};
    uint64_t snapshotMemoCloneHits{},snapshotMemoPassThroughHits{};
    bool snapshotMemoLegacy{};
    uint64_t trackedSourceBuffers{},indexedStorageBuffers{};
    uint64_t vertexBindBatches{},vertexBindElements{};
    uint64_t identicalDescriptorCommands{},identicalVertexStates{};
    uint64_t snapshotClassHits{},snapshotClassMisses{},snapshotDirectHits{},snapshotStorageKept{},snapshotStorageReplaced{};
    uint64_t passIndexBuilds{},passIndexSources{},passIndexInputs{},passIndexTargets{},passIndexLookups{},passScanCandidatesAvoided{};
};
inline thread_local Frame current;
inline bool detailedEnabled(){static const bool value=kharvox::extendedDiagnosticsEnabled()&&kharvox::runtimeFileExists(L"profile_native_stereo_cpu");return value;}
inline bool sparseEnabled(){static const bool value=kharvox::extendedDiagnosticsEnabled()&&kharvox::runtimeFileExists(L"profile_native_cpu_sparse");return value;}
inline bool pacingEnabled(){static const bool value=kharvox::extendedDiagnosticsEnabled()&&kharvox::runtimeFileExists(L"profile_native_frame_pacing");return value;}
inline bool enabled(){return detailedEnabled()||pacingEnabled()||sparseEnabled()||trace::enabled();}
inline bool coarseSlot(Slot slot){return slot==Root||slot==Original||slot==Doubled||slot==DeviceIdle||slot==XrWaitFrame||slot==XrCopyCompletion||slot==XrEndFrame||slot==GpuInputWait||slot==XrQueueSubmit||slot==XrQueueWait;}
inline uint64_t now(){return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
inline ThreadTimingSample threadSample(){
 FILETIME created{},exited{},kernel{},user{};ULONG64 cycles{};
 const auto handle=GetCurrentThread();
 const bool valid=GetThreadTimes(handle,&created,&exited,&kernel,&user)&&QueryThreadCycleTime(handle,&cycles);
 auto ticks=[](FILETIME v){return (uint64_t(v.dwHighDateTime)<<32)|v.dwLowDateTime;};
 return {ticks(kernel)+ticks(user),cycles,GetCurrentThreadId(),valid};
}
inline void elapsed(Slot slot,uint64_t start){if(start){current.ns[slot]+=now()-start;++current.calls[slot];}}
struct Scope {
    trace::Scope tracing;
    Slot slot; uint64_t start{};
    ThreadTimingSample threadStart{};
    explicit Scope(Slot value):tracing("cpu-slot",uint64_t(value)),slot(value),start(current.active&&(current.detailed||coarseSlot(value))?now():0){
        // Kernel accounting only at coarse boundaries, never on each draw/bind.
        if(start&&current.sampleThreads&&(slot==Root||slot==Original||slot==Doubled||slot==XrCopyCompletion))threadStart=threadSample();
    }
    ~Scope(){if(start){elapsed(slot,start);if(threadStart.valid){auto d=threadTimingDelta(threadStart,threadSample());if(d.valid){current.cpuNs[slot]+=d.cpuNs;current.cycles[slot]+=d.cycles;++current.threadSamples[slot];}}}}
    Scope(const Scope&)=delete;Scope& operator=(const Scope&)=delete;
};
void begin(uint64_t serial);
void finish(bool ready);
void flushPacingSequence();
}

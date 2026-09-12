// Wall spans and coarse thread accounting. No image readback or extra wait.
namespace cpu {
#include "NativePacingSequence.inc"
static thread_local std::array<uint64_t,FB_H_COUNT> hookRealBefore{},hookDoubleBefore{};
static thread_local std::array<uint64_t,FB_D_COUNT> jobRealBefore{},jobDoubleBefore{};
static thread_local uint64_t donorMappedReadsBefore{},donorMappedSkipsBefore{};
void begin(uint64_t serial){
    if(!enabled())return;
    const auto lastEnd=current.previousEnd,lastSerial=current.previousSerial;const bool afterSparse=current.sparseDetail;
    current={};current.afterSparse=afterSparse;current.active=true;current.detailed=detailFrame(serial,sparseEnabled(),detailedEnabled());current.sparseDetail=sparseDetailFrame(serial,sparseEnabled(),detailedEnabled());current.serial=serial;current.started=now();
    if(trace::active()){current.detailed=true;current.sparseDetail=true;}
    current.sampleThreads=current.detailed||(serial%8==0);
    current.previousEnd=lastEnd;current.previousSerial=lastSerial;
    if(!current.detailed)return;
    donorMappedReadsBefore=g_dbw_snapshot_read.load(std::memory_order_relaxed);
    donorMappedSkipsBefore=g_dbw_snapshot_skipped.load(std::memory_order_relaxed);
    for(size_t i=0;i<FB_H_COUNT;++i){hookRealBefore[i]=fb_hook_real_ns[i].load(std::memory_order_relaxed);hookDoubleBefore[i]=fb_hook_dbl_ns[i].load(std::memory_order_relaxed);}
    for(size_t i=0;i<FB_D_COUNT;++i){jobRealBefore[i]=fb_doom_tramp_ns[i].load(std::memory_order_relaxed);jobDoubleBefore[i]=fb_doom_tramp_dbl_ns[i].load(std::memory_order_relaxed);}
}
void finish(bool ready){
    if(!current.active)return;
    const auto end=now();current.active=false;
    const auto intervalNs=current.previousEnd&&current.serial==current.previousSerial+1?end-current.previousEnd:uint64_t(0);
    const double interval=double(intervalNs)/1e6;
    current.previousEnd=end;current.previousSerial=current.serial;
    if(trace::active()){
        trace::row("cpu-frame-summary",0,{current.serial,uint64_t(ready),intervalNs,end-current.started});
        for(size_t i=0;i<Count;++i)trace::row("cpu-slot-total",0,{i,current.calls[i],current.ns[i],current.cpuNs[i],current.cycles[i],current.threadSamples[i]});
        for(size_t i=0;i<FB_D_COUNT;++i){const auto a=fb_doom_tramp_ns[i].load(),b=fb_doom_tramp_dbl_ns[i].load();if(a>=jobRealBefore[i]&&b>=jobDoubleBefore[i])trace::row(kFbDoomName[i],0,{i,a-jobRealBefore[i],b-jobDoubleBefore[i]});}
        current.previousEnd=0;current.detailed=false;return;
    }
    if(current.sparseDetail){
        // This frame has per-call clock overhead. Report all components but
        // exclude it and the following interval from the coarse FPS baseline.
        static std::ofstream sparseFile(kharvox::logPathA("native_cpu_sparse.log"),std::ios::app);
        sparseFile<<std::format("Native sparse CPU frame={} ready={} pid={} intervalMs={:.3f} rootToCompleteMs={:.3f} descriptorReuse={} vertexReuse={} (one selected serial per 120; inclusive wall spans include instrumentation; CPU not GPU; do not sum nested slots)",current.serial,ready,GetCurrentProcessId(),interval,double(end-current.started)/1e6,current.identicalDescriptorCommands,current.identicalVertexStates)<<'\n';
        static constexpr const char* names[]={"Root","Original","Doubled","SnapshotBind","SnapshotCopy","StorageSeed","DeviceIdle","XrWaitFrame","XrCopyCompletion","XrEndFrame","ReplayDescriptors","CaptureImageInputs","PrepareImageLayouts","ReplayPass","ValidateStorageDraw","GpuInputWait","XrQueueLock","XrQueueSubmit","XrQueueWait","ForwardDescriptors","ForwardDraw","ForwardOther","StorageSelection","ReplayVertex","ReplayPipeline","ReplayDraw","SnapshotClassLookup","SnapshotMemoLookup","SnapshotMemoRestore","SnapshotMemoMiss","SnapshotForward"};
        static_assert(std::size(names)==Count);
        for(size_t i=0;i<Count;++i)sparseFile<<std::format("Native sparse slot frame={} pid={} name={} calls={} wallMs={:.6f}",current.serial,GetCurrentProcessId(),names[i],current.calls[i],double(current.ns[i])/1e6)<<'\n';
        for(size_t i=0;i<FB_H_COUNT;++i){const auto a=fb_hook_real_ns[i].load(std::memory_order_relaxed),b=fb_hook_dbl_ns[i].load(std::memory_order_relaxed);if(a>=hookRealBefore[i]&&b>=hookDoubleBefore[i]&&(a>hookRealBefore[i]||b>hookDoubleBefore[i]))sparseFile<<std::format("Native sparse hook frame={} pid={} name={} wallMs={:.6f}",current.serial,GetCurrentProcessId(),kFbHookExport[i],double(a-hookRealBefore[i]+b-hookDoubleBefore[i])/1e6)<<'\n';}
        for(size_t i=0;i<FB_D_COUNT;++i){const auto a=fb_doom_tramp_ns[i].load(std::memory_order_relaxed),b=fb_doom_tramp_dbl_ns[i].load(std::memory_order_relaxed);if(a>=jobRealBefore[i]&&b>=jobDoubleBefore[i]&&(a>jobRealBefore[i]||b>jobDoubleBefore[i]))sparseFile<<std::format("Native sparse job frame={} pid={} name={} realMs={:.6f} doubledMs={:.6f}",current.serial,GetCurrentProcessId(),kFbDoomName[i],double(a-jobRealBefore[i])/1e6,double(b-jobDoubleBefore[i])/1e6)<<'\n';}
        sparseFile.flush();current.previousEnd=0;current.detailed=false;return;
    }
    if(!current.detailed){
        if(current.afterSparse)return; // exclude the interval containing sparse log I/O
        static uint64_t frames{};
        const bool logThisFrame=++frames<=4||frames%120==0;
        capturePacingSequence(end,intervalNs,ready,logThisFrame);
        // Sample coarse thread accounting on one frame in eight. Accumulate
        // matching wall/CPU intervals only; do not compare sparse CPU samples
        // against wall time from every frame. No queries in draw/bind hooks.
        static Frame threadWindow;
        if(ready){for(size_t i=0;i<Count;++i)if(current.threadSamples[i]){
            threadWindow.ns[i]+=current.ns[i];threadWindow.cpuNs[i]+=current.cpuNs[i];
            threadWindow.cycles[i]+=current.cycles[i];threadWindow.threadSamples[i]+=current.threadSamples[i];
        }}else threadWindow={};
        if(logThisFrame){
            auto ms=[](uint64_t value){return double(value)/1e6;};
            const auto line=std::format("Native frame pacing frame={} ready={} intervalMs={:.3f} rootToCompleteMs={:.3f} rootMs={:.3f} originalMs={:.3f} doubledMs={:.3f} deviceIdleMs={:.3f} xrWaitFrameMs={:.3f} xrCopyCompletionMs={:.3f} xrEndFrameMs={:.3f} inputSnapshotWaitMs={:.3f} xrQueueLockMs={:.3f} xrQueueSubmitMs={:.3f} xrQueueWaitMs={:.3f} pid={} (nested coarse wall spans; thread accounting sampled 1/8 frames; no per-draw/per-bind clocks or GPU readback)",
                current.serial,ready,interval,ms(end-current.started),ms(current.ns[Root]),ms(current.ns[Original]),ms(current.ns[Doubled]),ms(current.ns[DeviceIdle]),ms(current.ns[XrWaitFrame]),ms(current.ns[XrCopyCompletion]),ms(current.ns[XrEndFrame]),ms(current.ns[GpuInputWait]),ms(current.ns[XrQueueLock]),ms(current.ns[XrQueueSubmit]),ms(current.ns[XrQueueWait]),GetCurrentProcessId());
            kharvoxnative::log::info(line);
            static std::ofstream file(kharvox::logPathA("native_frame_pacing.log"),std::ios::app);
            file<<line<<'\n';
            file<<std::format("Native snapshot fast path frame={} classHits={} classMisses={} directMemoHits={} storageKept={} storageReplaced={} pid={} (full mutation stamp; per-frame caches; original GPU bind retained)",current.serial,current.snapshotClassHits,current.snapshotClassMisses,current.snapshotDirectHits,current.snapshotStorageKept,current.snapshotStorageReplaced,GetCurrentProcessId())<<'\n';
            file<<std::format("Native replay reuse frame={} identicalDescriptorCommands={} identicalVertexStates={} pid={} (CPU state copies avoided; every GPU bind and final command retained)",current.serial,current.identicalDescriptorCommands,current.identicalVertexStates,GetCurrentProcessId())<<'\n';
            file<<std::format("Native vertex batching frame={} applicationCalls={} bindings={} formerlySplitCalls={} (counts for this frame; state restore/replay excluded; no GPU work removed)",current.serial,current.vertexBindBatches,current.vertexBindElements,current.vertexBindElements>current.vertexBindBatches?current.vertexBindElements-current.vertexBindBatches:0)<<'\n';
            file<<std::format("Native pass index frame={} builds={} sources={} inputVisits={} targets={} lookups={} oldScanCandidates={} (CPU work counts; same attachment identity and layout checks)",current.serial,current.passIndexBuilds,current.passIndexSources,current.passIndexInputs,current.passIndexTargets,current.passIndexLookups,current.passScanCandidatesAvoided)<<'\n';
            file<<std::format("Native input memo frame={} legacyPipelineKey={} cloneHits={} passThroughHits={} misses={} (per-frame mutation stamp; original draw checks and copies retained)",current.serial,current.snapshotMemoLegacy,current.snapshotMemoCloneHits,current.snapshotMemoPassThroughHits,current.bindingMemoMisses)<<'\n';
            for(auto slot:{Root,Original,Doubled,XrCopyCompletion})if(threadWindow.threadSamples[slot]){
                const double n=double(threadWindow.threadSamples[slot]);
                file<<std::format("Native coarse thread endFrame={} tid={} slot={} sampledIntervals={} meanWallMs={:.3f} meanThreadCpuMs={:.3f} meanCycles={:.0f} (CPU=user+kernel; wall-minus-CPU includes blocking AND descheduling; nested slots)",current.serial,GetCurrentThreadId(),int(slot),threadWindow.threadSamples[slot],ms(threadWindow.ns[slot])/n,ms(threadWindow.cpuNs[slot])/n,double(threadWindow.cycles[slot])/n)<<'\n';
            }
            threadWindow={};file.flush();
        }
        return;
    }
    static Frame schedulingWindow;static uint64_t windowFrames{};
    if(ready){++windowFrames;for(size_t i=0;i<Count;++i){schedulingWindow.ns[i]+=current.ns[i];schedulingWindow.cpuNs[i]+=current.cpuNs[i];schedulingWindow.cycles[i]+=current.cycles[i];schedulingWindow.threadSamples[i]+=current.threadSamples[i];}}
    else {schedulingWindow={};windowFrames=0;}
    static uint64_t samples{};if(++samples>8&&samples%30!=0)return;
    auto ms=[](uint64_t value){return double(value)/1e6;};
    std::array<double,FB_H_COUNT> hookMs{};
    for(size_t i=0;i<FB_H_COUNT;++i){auto real=fb_hook_real_ns[i].load(std::memory_order_relaxed),doubled=fb_hook_dbl_ns[i].load(std::memory_order_relaxed);if(real>=hookRealBefore[i]&&doubled>=hookDoubleBefore[i])hookMs[i]=ms(real-hookRealBefore[i]+doubled-hookDoubleBefore[i]);}
    size_t top{};for(size_t i=1;i<FB_H_COUNT;++i)if(hookMs[i]>hookMs[top])top=i;
    const auto line=std::format("Native CPU profile frame={} ready={} intervalMs={:.3f} rootToCompleteMs={:.3f} rootMs={:.3f} originalMs={:.3f} doubledMs={:.3f} descriptorHookMs={:.3f} snapshotBindMs={:.3f} snapshotCopyMs={:.3f} storageSeedMs={:.3f} deviceIdleMs={:.3f} snapshotBindCalls={} copiedBytes={} seededBuffers={} seededBytes={} topHook={} topHookMs={:.3f} depthLeaks={} xrWaitFrameMs={:.3f} xrCopyCompletionMs={:.3f} xrEndFrameMs={:.3f} pid={} (CPU inclusive spans; do not sum nested columns; no GPU readback)",
        current.serial,ready,interval,ms(end-current.started),ms(current.ns[Root]),ms(current.ns[Original]),ms(current.ns[Doubled]),hookMs[FB_H_CMD_BIND_DESCRIPTOR_SETS],ms(current.ns[SnapshotBind]),ms(current.ns[SnapshotCopy]),ms(current.ns[StorageSeed]),ms(current.ns[DeviceIdle]),current.calls[SnapshotBind],current.copiedBytes,current.seededBuffers,current.seededBytes,kFbHookExport[top],hookMs[top],fb_depth_leaks.load(),ms(current.ns[XrWaitFrame]),ms(current.ns[XrCopyCompletion]),ms(current.ns[XrEndFrame]),GetCurrentProcessId());
    kharvoxnative::log::info(line);
    static std::ofstream file(kharvox::logPathA("native_cpu_profile.log"),std::ios::app);
    file<<line<<'\n';
    // Outer adapter work was outside the donor's FbHook spans. Attribute it
    // explicitly before deciding which preservation/layout work to optimize.
    const auto reads=g_dbw_snapshot_read.load(std::memory_order_relaxed);
    const auto skips=g_dbw_snapshot_skipped.load(std::memory_order_relaxed);
    const auto integration=std::format("Native integration CPU frame={} descriptorOuterMs={:.3f} imageInputCaptureMs={:.3f} imageLayoutPrepareMs={:.3f} passOuterMs={:.3f} storageDrawValidationMs={:.3f} gpuInputSubmitWaitMs={:.3f} descriptorOuterCalls={} inputCaptureCalls={} layoutPrepareCalls={} gpuInputWaitCalls={} donorMappedReads={} donorMappedSkips={} donorCountersMonotonic={} donorGpuCam={} donorFreqHigh={} (inclusive CPU spans; outer includes nested donor/native work)",
        current.serial,ms(current.ns[ReplayDescriptors]),ms(current.ns[CaptureImageInputs]),ms(current.ns[PrepareImageLayouts]),ms(current.ns[ReplayPass]),ms(current.ns[ValidateStorageDraw]),ms(current.ns[GpuInputWait]),current.calls[ReplayDescriptors],current.calls[CaptureImageInputs],current.calls[PrepareImageLayouts],current.calls[GpuInputWait],reads>=donorMappedReadsBefore?reads-donorMappedReadsBefore:0,skips>=donorMappedSkipsBefore?skips-donorMappedSkipsBefore:0,reads>=donorMappedReadsBefore&&skips>=donorMappedSkipsBefore,kharvoxnative::wrap::config_gpucam(),kharvoxnative::wrap::config().fhcensus);
    file<<integration<<'\n';
    file<<std::format("Native storage selection frame={} trackedSources={} indexedSources={} selectionMs={:.3f} seededBuffers={} seededBytes={} (selection is CPU only; same full-buffer GPU copies and barriers)",current.serial,current.trackedSourceBuffers,current.indexedStorageBuffers,ms(current.ns[StorageSelection]),current.seededBuffers,current.seededBytes)<<'\n';
    file<<std::format("Native forwarding CPU frame={} descriptorMs={:.3f} drawMs={:.3f} otherMs={:.3f} calls={} bindingMemoHits={} bindingMemoMisses={} (outermost selected Vulkan calls into KHARVOX/driver; CPU not GPU; includes final-pass calls)",current.serial,ms(current.ns[ForwardDescriptors]),ms(current.ns[ForwardDraw]),ms(current.ns[ForwardOther]),current.calls[ForwardDescriptors]+current.calls[ForwardDraw]+current.calls[ForwardOther],current.bindingMemoHits,current.bindingMemoMisses)<<'\n';
    file<<std::format("Native replay payload frame={} reused={} new={} borrowedCompute={} capturedArgumentsRemainOwned=true",current.serial,current.reusedReplayPayloads,current.newReplayPayloads,current.borrowedComputeBinds)<<'\n';
    file<<std::format("Native queue timing frame={} lockMs={:.3f} submitMs={:.3f} waitMs={:.3f} (lock outside xrCopyCompletion; submit and wait inside)",current.serial,ms(current.ns[XrQueueLock]),ms(current.ns[XrQueueSubmit]),ms(current.ns[XrQueueWait]))<<'\n';
    if(windowFrames){
        const double n=double(windowFrames);
        for(auto slot:{Root,Original,Doubled,XrCopyCompletion})file<<std::format(
            "Native thread accounting endFrame={} tid={} frames={} slot={} meanWallMs={:.3f} meanThreadCpuMs={:.3f} meanCycles={:.0f} validSamples={} (CPU=user+kernel; coarse accounting, aggregate only; wall-minus-CPU includes blocking AND descheduling; nested slots)",
            current.serial,GetCurrentThreadId(),windowFrames,int(slot),ms(schedulingWindow.ns[slot])/n,ms(schedulingWindow.cpuNs[slot])/n,double(schedulingWindow.cycles[slot])/n,schedulingWindow.threadSamples[slot])<<'\n';
        schedulingWindow={};windowFrames=0;
    }
    if(pairWatchEnabled()||xrTargetCaptureEnabled())file<<"Native CPU profile capture diagnostic enabled: source/XR readback spans are included; do not use this run as a capture-free performance baseline\n";
    file.flush();
    std::array<size_t,FB_H_COUNT> order{};for(size_t i=0;i<order.size();++i)order[i]=i;
    std::partial_sort(order.begin(),order.begin()+5,order.end(),[&](size_t a,size_t b){return hookMs[a]>hookMs[b];});
    std::string detail=std::format("Native CPU top hooks frame={}",current.serial);
    for(size_t n=0;n<5;++n){auto i=order[n];detail+=std::format(" {}={:.3f}ms",kFbHookExport[i],hookMs[i]);}
    file<<detail<<'\n';file.flush();
    // Reuse existing trampoline counters: no new clock reads in draw hooks.
    // These are inclusive CPU job spans and must not be added together.
    std::array<double,FB_D_COUNT> jobReal{},jobDouble{};
    std::array<size_t,FB_D_COUNT> jobs{};
    for(size_t i=0;i<FB_D_COUNT;++i){jobs[i]=i;const auto a=fb_doom_tramp_ns[i].load(),b=fb_doom_tramp_dbl_ns[i].load();
        if(a>=jobRealBefore[i])jobReal[i]=ms(a-jobRealBefore[i]);if(b>=jobDoubleBefore[i])jobDouble[i]=ms(b-jobDoubleBefore[i]);}
    std::partial_sort(jobs.begin(),jobs.begin()+6,jobs.end(),[&](size_t a,size_t b){return jobReal[a]+jobDouble[a]>jobReal[b]+jobDouble[b];});
    detail=std::format("Native CPU jobs frame={} realMs/doubledMs (inclusive)",current.serial);
    for(size_t n=0;n<6;++n){auto i=jobs[n];detail+=std::format(" {}={:.3f}/{:.3f}",kFbDoomName[i],jobReal[i],jobDouble[i]);}
    file<<detail<<'\n';file.flush();
}
}

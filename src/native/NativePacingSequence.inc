// Consecutive coarse timing samples expose period-2 patterns hidden by the
// normal 1/120 log cadence. No new GPU queries/waits or draw/bind instrumentation.
struct PacingRecord {
 uint64_t serial{},endNs{},intervalNs{},rootToCompleteNs{};
 std::array<uint64_t,Count> ns{};
 uint64_t vertices{},passes{},memoHits{},memoMisses{},copiedBytes{},seededBytes{},seededBuffers{};
 bool threadAccounting{},logAfterRow{};
};
static PacingSequence<PacingRecord,4096,120> pacingSequence;
static std::mutex pacingSequenceMutex;
static std::atomic_bool pacingSequenceStopped{};
static void writePacingSequence(){
 // Called only after capture has stopped. File I/O cannot affect an included
 // sample. Existing sparse log/thread-accounting frames are explicitly marked.
 const auto name=std::format("native_pacing_sequence-{}.tsv",GetCurrentProcessId());
 const auto path=kharvox::logPathA(name.c_str());
 std::ofstream file(path,std::ios::trunc);
 if(!file){kharvoxnative::log::warn("Native pacing sequence could not open output file");return;}
 file<<"# r157 consecutive coarse CPU wall spans in nanoseconds; nested, not GPU busy time; 120 ready-frame warmup; bounded 4096 rows; writes only after capture; pid="<<GetCurrentProcessId()<<'\n';
 file<<"frame\tendNs\tintervalNs\trootToCompleteNs\trootNs\toriginalNs\tdoubledNs\tdeviceIdleNs\txrWaitFrameNs\txrCopyCompletionNs\txrEndFrameNs\tinputWaitNs\txrSubmitNs\txrQueueWaitNs\tvertexBindings\tpasses\tmemoHits\tmemoMisses\tcopiedBytes\tseededBytes\tseededBuffers\tthreadAccounting\tlogAfterRow\n";
 for(size_t i=0;i<pacingSequence.size();++i){const auto&r=pacingSequence[i];
  file<<r.serial<<'\t'<<r.endNs<<'\t'<<r.intervalNs<<'\t'<<r.rootToCompleteNs;
  for(auto slot:{Root,Original,Doubled,DeviceIdle,XrWaitFrame,XrCopyCompletion,XrEndFrame,GpuInputWait,XrQueueSubmit,XrQueueWait})file<<'\t'<<r.ns[slot];
  file<<'\t'<<r.vertices<<'\t'<<r.passes<<'\t'<<r.memoHits<<'\t'<<r.memoMisses<<'\t'<<r.copiedBytes<<'\t'<<r.seededBytes<<'\t'<<r.seededBuffers<<'\t'<<r.threadAccounting<<'\t'<<r.logAfterRow<<'\n';
 }
 file.close();
 kharvoxnative::log::info(std::format("Native pacing sequence rows={} saved={} path={} (capture stopped before file I/O; no runtime/ASW inference)",pacingSequence.size(),bool(file),path));
}
static void capturePacingSequence(uint64_t end,uint64_t interval,bool ready,bool logAfterRow){
 if(pacingSequenceStopped.load(std::memory_order_relaxed))return;
 PacingRecord r{current.serial,end,interval,end-current.started,current.ns,
  current.vertexBindElements,current.passIndexSources,current.bindingMemoHits,current.bindingMemoMisses,
  current.copiedBytes,current.seededBytes,current.seededBuffers,current.sampleThreads,logAfterRow};
 bool write{};
 {std::scoped_lock lock(pacingSequenceMutex);write=pacingSequence.push(r,ready);
  if(write)pacingSequenceStopped.store(true,std::memory_order_relaxed);}
 if(write)writePacingSequence();
}
void flushPacingSequence(){
 bool write{};
 {std::scoped_lock lock(pacingSequenceMutex);write=pacingSequence.seal();
  if(write)pacingSequenceStopped.store(true,std::memory_order_relaxed);}
 if(write)writePacingSequence();
}

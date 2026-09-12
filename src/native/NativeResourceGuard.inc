// Failure-only diagnostic. Never query/wait/free GPU resources or take native
// bookkeeping locks here: a rejecting caller may already hold such a lock.
void beforeDestroy(const char* operation,uintptr_t resource){
 if(!outstanding.load())return;
 const auto tid=GetCurrentThreadId();
 std::string report=std::format("Native resource lifetime guard r158: operation={} resource={:#x} pid={} tid={} phase={} threadFrame={} insideOriginal={} insideDoubled={} stage={} outstanding=true\n",
  operation,resource,GetCurrentProcessId(),tid,int(phase()),cpu::current.serial,
  inside_real_frame_root,inside_frame_root_double,frame_root_stage.load());
 void* stack[48]{};
 const auto count=CaptureStackBackTrace(1,48,stack,nullptr);
 for(USHORT i=0;i<count;++i){
  HMODULE module{};char path[MAX_PATH]{};
  const bool known=GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
   reinterpret_cast<LPCSTR>(stack[i]),&module)!=FALSE;
  if(known)GetModuleFileNameA(module,path,MAX_PATH);
  const auto pc=reinterpret_cast<uintptr_t>(stack[i]);
  report+=std::format("stack[{}] pc={:#x} module={} rva={:#x}\n",i,pc,
   path[0]?path:"unresolved",known?pc-reinterpret_cast<uintptr_t>(module):uintptr_t(0));
 }
 const auto name=std::format("native_resource_guard-{}.log",GetCurrentProcessId());
 {std::ofstream file(kharvox::logPathA(name.c_str()),std::ios::trunc);file<<report;}
 kharvoxnative::log::error(report);
 // The same guard still rejects the same condition before the underlying
 // destruction. Capturing a stack is not a resource-lifetime proof.
 fail("resource destruction overlaps an uncompleted native frame");
}

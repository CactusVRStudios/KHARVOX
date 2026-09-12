#include "../common/DiagnosticLogging.h"
#include "NativeFrameTrace.h"
#include "NativeFrameTracePolicy.h"
#include "../common/RuntimePaths.h"
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>
namespace kharvox::native::trace {
static constexpr size_t rowLimit=1048576;
struct Event {uint64_t id{},frame{},start{},end{},parent{},object{};uint32_t thread{},eye{};const char* name{};int64_t result{};std::array<uint64_t,8> args{};};
struct Command {VkCommandPool pool{};uint32_t level{},flags{};uint64_t generation{},beginNs{},endNs{};bool recording{};};
static std::mutex mutex;
static std::vector<Event> events;
static std::map<VkCommandBuffer,Command> commands;
static std::map<VkCommandPool,uint32_t> pools;
static std::map<VkQueue,std::array<uint32_t,3>> queues;
static std::map<VkSemaphore,std::pair<uint32_t,uint64_t>> semaphores;
static std::vector<std::pair<std::string,std::string>> deferredFiles;
static std::atomic_bool capturing{};
static std::atomic_uint64_t serial{},ids{};
static std::atomic_uint64_t captureId{},openScopes{};
static thread_local uint64_t parent{};
static thread_local uint32_t eye=2,submitDepth{};
static FrameTracePolicy policy;
static uint64_t dropped{},registryDropped{},started{},firstFrame{};
static bool initialized{};
static bool boundaryPending{},boundaryReady{};static uint64_t boundaryFrame{};
static bool resourceSnapshotPending{};
bool enabled(){static const bool value=kharvox::extendedDiagnosticsEnabled()&&kharvox::runtimeFileExists(L"debug_native_frame_analysis");return value;}
bool active(){return capturing.load(std::memory_order_relaxed);}
uint64_t timestamp(){return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
template<class T> static uint64_t handle(T value){if constexpr(std::is_pointer_v<T>)return reinterpret_cast<uintptr_t>(value);else return uint64_t(value);}
static void append(Event event){std::scoped_lock l(mutex);if(!active())return;if(events.size()<rowLimit)events.push_back(event);else ++dropped;}
void row(const char* name,uint64_t link,std::array<uint64_t,8> args){if(!active())return;const auto time=timestamp();append({++ids,serial.load(),time,time,link,0,GetCurrentThreadId(),eye,name,0,args});}
Scope::Scope(const char* label,uint64_t value){if(!active())return;capture=captureId.load();++openScopes;id=++ids;start=timestamp();frame=serial.load();name=label;object=value;thread=GetCurrentThreadId();eye=trace::eye;previous=parent;parent=id;}
Scope::~Scope(){if(id){const auto end=timestamp();parent=previous;--openScopes;if(capture==captureId.load())append({id,frame,start,end,previous,object,thread,eye,name,result,{}});}}
void setEye(uint32_t value){eye=value;}
void initialize(){if(!enabled()||initialized)return;initialized=true;events.reserve(rowLimit);}
static void status(const std::string& message){std::ofstream(kharvox::logPathA("native_frame_analysis_status.txt"))<<message<<'\n';}
void writeOrDefer(const char* name,const std::string& text){
 if(active()){std::scoped_lock l(mutex);deferredFiles.emplace_back(name,text);}
 else std::ofstream(kharvox::logPathA(name),std::ios::app)<<text;
}
static void save(const char* reason){
 capturing.store(false);std::scoped_lock l(mutex);
 const auto stem="native_frame_analysis-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(policy.capture());
 std::ofstream file(kharvox::logPathA((stem+".tsv").c_str()));
 file<<"# r179 capture="<<policy.capture()<<" pid="<<GetCurrentProcessId()<<" reason="<<reason<<" firstFrame="<<firstFrame<<" frames="<<policy.completed()<<" dropped="<<dropped<<" registryDropped="<<registryDropped<<" openScopesAtStop="<<openScopes.load()<<" cpuClock=steady_clock_ns gpuClock=separate noExtraSubmitOrWait=true\n";
 file<<"id\tframe\tstartNs\tendNs\tparent\tobject\tthread\teye\tevent\tresult\ta0\ta1\ta2\ta3\ta4\ta5\ta6\ta7\n";
 for(const auto& e:events){file<<e.id<<'\t'<<e.frame<<'\t'<<e.start<<'\t'<<e.end<<'\t'<<e.parent<<'\t'<<e.object<<'\t'<<e.thread<<'\t'<<e.eye<<'\t'<<e.name<<'\t'<<e.result;for(auto v:e.args)file<<'\t'<<v;file<<'\n';}
 file.close();bool saved=bool(file);
 for(const auto& [name,text]:deferredFiles){std::ofstream output(kharvox::logPathA(name.c_str()),std::ios::app);output<<text;output.close();saved&=bool(output);}deferredFiles.clear();
 status(std::string(saved?"SAVED ":"WRITE FAILED ")+stem+" frames="+std::to_string(policy.completed())+" dropped="+std::to_string(dropped)+" reason="+reason);
}
void poll(){
 if(!enabled())return;initialize();
 const bool down=(GetAsyncKeyState(VK_CONTROL)&0x8000)&&(GetAsyncKeyState(VK_MENU)&0x8000)&&(GetAsyncKeyState(VK_SCROLL)&0x8000);
 policy.key(down);
 if(active()&&timestamp()-started>10'000'000'000ull){policy.stop();save("timeout-partial");}
}
void beginFrame(uint64_t frame){if(!enabled())return;serial=frame;setEye(2);row("root-frame-begin");}
void endFrame(uint64_t frame,bool ready){
 if(!enabled())return;boundaryPending=true;boundaryFrame=frame;boundaryReady=ready;row("native-completed",0,{frame,uint64_t(ready)});
}
void requestCapture(){if(enabled()){policy.key(false);policy.key(true);}}
bool consumeResourceSnapshot(){const bool wanted=active()&&resourceSnapshotPending;resourceSnapshotPending=false;return wanted;}
void presentFinished(){
 if(!enabled())return;
 if(!boundaryPending)return;boundaryPending=false;const auto frame=boundaryFrame;const auto ready=boundaryReady;
 if(active()){row("frame-completed",0,{frame,uint64_t(ready)});if(policy.finishBoundary(ready))save(ready?"four-frames":"not-ready-partial");else{serial=frame+1;setEye(2);}return;}
 if(!policy.startBoundary(ready))return;
 initialize();{std::scoped_lock l(mutex);events.clear();deferredFiles.clear();dropped=0;}
 captureId=policy.capture();
 firstFrame=frame+1;serial=firstFrame;started=timestamp();resourceSnapshotPending=true;capturing=true;
 status("CAPTURING four frames; do not use captured FPS as baseline");
 row("capture-start",0,{firstFrame,FrameTracePolicy::frameLimit});
 // Snapshot existing command metadata: buffers may have been allocated long
 // before the hotkey. No borrowed Vulkan pointer survives its API call.
 std::vector<std::pair<VkCommandBuffer,Command>> snapshot;std::vector<std::pair<VkCommandPool,uint32_t>> poolSnapshot;std::vector<std::pair<VkSemaphore,std::pair<uint32_t,uint64_t>>> semaphoreSnapshot;std::vector<std::pair<VkQueue,std::array<uint32_t,3>>> queueSnapshot;
 {std::scoped_lock l(mutex);snapshot.assign(commands.begin(),commands.end());poolSnapshot.assign(pools.begin(),pools.end());semaphoreSnapshot.assign(semaphores.begin(),semaphores.end());queueSnapshot.assign(queues.begin(),queues.end());}
 for(const auto& [pool,family]:poolSnapshot)row("pool-existing",0,{handle(pool),family});
 for(const auto& [cb,c]:snapshot)row("command-existing",0,{handle(cb),handle(c.pool),c.level,c.flags,c.generation,c.beginNs,c.endNs,c.recording});
 for(const auto& [semaphore,type]:semaphoreSnapshot)row("semaphore-existing",0,{handle(semaphore),type.first,type.second});
 for(const auto& [queue,q]:queueSnapshot)row("queue-existing",0,{handle(queue),q[0],q[1],q[2]});
}

static void extensionRows(const void* next,uint64_t link){
 unsigned count{};for(auto p=static_cast<const VkBaseInStructure*>(next);p&&count++<32;p=p->pNext){row("pnext-type",link,{uint64_t(p->sType)});if(p->sType!=VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO&&p->sType!=VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO)row("coverage-gap-pnext-unparsed",link,{uint64_t(p->sType)});}
 if(count>32)row("coverage-gap-pnext-depth",link);
}
static const VkTimelineSemaphoreSubmitInfo* timeline(const void* next){for(auto p=static_cast<const VkBaseInStructure*>(next);p;p=p->pNext)if(p->sType==VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO)return reinterpret_cast<const VkTimelineSemaphoreSubmitInfo*>(p);return nullptr;}
SubmitScope::SubmitScope(const char* origin,VkQueue q,uint32_t count,const VkSubmitInfo* infos,VkFence fence):scope(submitDepth?"nested-submit":origin,handle(q)),owner(submitDepth++==0){
 if(!scope.id||!owner)return;row("submit",scope.id,{handle(q),handle(fence),count,1});
 for(uint32_t i=0;i<count;++i){const auto& s=infos[i];const auto t=timeline(s.pNext);extensionRows(s.pNext,scope.id);
  for(uint32_t j=0;j<s.waitSemaphoreCount;++j)row("submit-wait",scope.id,{i,j,handle(s.pWaitSemaphores[j]),s.pWaitDstStageMask[j],t&&j<t->waitSemaphoreValueCount?t->pWaitSemaphoreValues[j]:0,uint64_t(t!=nullptr)});
  for(uint32_t j=0;j<s.commandBufferCount;++j)row("submit-command",scope.id,{i,j,handle(s.pCommandBuffers[j])});
  for(uint32_t j=0;j<s.signalSemaphoreCount;++j)row("submit-signal",scope.id,{i,j,handle(s.pSignalSemaphores[j]),0,t&&j<t->signalSemaphoreValueCount?t->pSignalSemaphoreValues[j]:0,uint64_t(t!=nullptr)});
 }
}
SubmitScope::SubmitScope(const char* origin,VkQueue q,uint32_t count,const VkSubmitInfo2* infos,VkFence fence):scope(submitDepth?"nested-submit":origin,handle(q)),owner(submitDepth++==0){
 if(!scope.id||!owner)return;row("submit",scope.id,{handle(q),handle(fence),count,2});
 for(uint32_t i=0;i<count;++i){const auto& s=infos[i];extensionRows(s.pNext,scope.id);row("submit2-flags",scope.id,{i,s.flags});
  for(uint32_t j=0;j<s.waitSemaphoreInfoCount;++j){const auto& v=s.pWaitSemaphoreInfos[j];row("submit-wait",scope.id,{i,j,handle(v.semaphore),v.stageMask,v.value,1,v.deviceIndex});extensionRows(v.pNext,scope.id);}
  for(uint32_t j=0;j<s.commandBufferInfoCount;++j){const auto& v=s.pCommandBufferInfos[j];row("submit-command",scope.id,{i,j,handle(v.commandBuffer),v.deviceMask});extensionRows(v.pNext,scope.id);}
  for(uint32_t j=0;j<s.signalSemaphoreInfoCount;++j){const auto& v=s.pSignalSemaphoreInfos[j];row("submit-signal",scope.id,{i,j,handle(v.semaphore),v.stageMask,v.value,1,v.deviceIndex});extensionRows(v.pNext,scope.id);}
 }
}
SubmitScope::~SubmitScope(){--submitDepth;}
// Generic call records include scalar/handle arguments only. Array structures
// are expanded by the explicit decoders below; raw pointer values are not data.
template<class T> static uint64_t scalar(T value){if constexpr(std::is_pointer_v<T>||std::is_integral_v<T>||std::is_enum_v<T>)return handle(value);else return 0;}
template<class... A> static void arguments(uint64_t link,A... values){std::array<uint64_t,sizeof...(A)> a{scalar(values)...};std::array<uint64_t,8> rowValues{};for(size_t i=0;i<a.size()&&i<8;++i)rowValues[i]=a[i];row("api-arguments",link,rowValues);if(a.size()>8)row("coverage-gap-argument-tail",link,{a.size()});}
#include "NativeFrameTraceDecode.inc"
template<class T,int Tag> struct Call;
template<int Tag,class R,class... A> struct Call<R(VKAPI_PTR*)(A...),Tag>{
 using Fn=R(VKAPI_PTR*)(A...);static inline Fn next{};static inline const char* name{};
 static R VKAPI_PTR call(A... a){
  Scope scope(name);if(scope.id){arguments(scope.id,a...);decode(name,scope.id,a...);}
  if constexpr(std::is_void_v<R>){next(a...);registry(name,VK_SUCCESS,a...);}
  else {auto result=next(a...);scope.result=int64_t(scalar(result));registry(name,result,a...);return result;}
 }
};
template<bool Driver> struct Submit2KhrCall {
 static inline PFN_vkQueueSubmit2KHR next{};
 static VkResult VKAPI_PTR call(VkQueue queue,uint32_t count,const VkSubmitInfo2* infos,VkFence fence){SubmitScope capture(Driver?"driver-submit2-khr":"adapter-submit2-khr",queue,count,infos,fence);auto result=next(queue,count,infos,fence);capture.result(result);return result;}
};
PFN_vkVoidFunction wrap(const char* name,PFN_vkVoidFunction next,bool driver){
 if(!enabled()||!next)return next;
 if(!std::strcmp(name,"vkQueueSubmit2KHR")){
  if(driver){using W=Submit2KhrCall<true>;auto fn=reinterpret_cast<PFN_vkVoidFunction>(&W::call);if(next!=fn)W::next=reinterpret_cast<PFN_vkQueueSubmit2KHR>(next);return fn;}
  using W=Submit2KhrCall<false>;auto fn=reinterpret_cast<PFN_vkVoidFunction>(&W::call);if(next!=fn)W::next=reinterpret_cast<PFN_vkQueueSubmit2KHR>(next);return fn;
 }
#define TRACE(api) if(!strcmp(name,#api)){if(driver){using W=Call<PFN_##api,__COUNTER__>;auto fn=reinterpret_cast<PFN_vkVoidFunction>(&W::call);if(next!=fn)W::next=reinterpret_cast<PFN_##api>(next);W::name="driver:" #api;return fn;}using W=Call<PFN_##api,__COUNTER__>;auto fn=reinterpret_cast<PFN_vkVoidFunction>(&W::call);if(next!=fn)W::next=reinterpret_cast<PFN_##api>(next);W::name="adapter:" #api;return fn;}
#include "NativeFrameTraceDispatch.inc"
#undef TRACE
 return next;
}
}

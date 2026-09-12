#include "../src/native/NativeFrameTrace.h"
#include "../src/native/NativeFrameTracePolicy.h"
#include "../src/native/NativeXrFrameTrace.h"
#include "../src/common/RuntimePaths.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <cstdio>
#include <source_location>
using namespace kharvox::native;
static void require(bool ok,const std::source_location location=std::source_location::current()){
 if(!ok){std::fprintf(stderr,"native-frame-trace assertion at line %u\n",location.line());std::abort();}
}
static uint32_t draws{},vertices{};
static void VKAPI_PTR draw(VkCommandBuffer,uint32_t v,uint32_t,uint32_t,uint32_t){++draws;vertices=v;}
static PFN_vkCmdDraw driverDraw{};
static void VKAPI_PTR adapter(VkCommandBuffer cb,uint32_t a,uint32_t b,uint32_t c,uint32_t d){driverDraw(cb,a,b,c,d);}
static VkResult VKAPI_PTR createPool(VkDevice,const VkCommandPoolCreateInfo*,const VkAllocationCallbacks*,VkCommandPool* out){*out=reinterpret_cast<VkCommandPool>(101);return VK_SUCCESS;}
static VkResult VKAPI_PTR allocate(VkDevice,const VkCommandBufferAllocateInfo*,VkCommandBuffer* out){*out=reinterpret_cast<VkCommandBuffer>(102);return VK_SUCCESS;}
static VkResult VKAPI_PTR begin(VkCommandBuffer,const VkCommandBufferBeginInfo*){return VK_SUCCESS;}
static void VKAPI_PTR endPass(VkCommandBuffer){}
static PFN_vkQueueSubmit2KHR driverSubmit2{};
static uint32_t submit2Calls{};
static const VkSubmitInfo2* expectedSubmit2{};
static VkResult VKAPI_PTR submit2(VkQueue q,uint32_t count,const VkSubmitInfo2* infos,VkFence fence){require(q==reinterpret_cast<VkQueue>(9)&&count==1&&infos==expectedSubmit2&&fence==reinterpret_cast<VkFence>(10));++submit2Calls;return VK_ERROR_DEVICE_LOST;}
static VkResult VKAPI_PTR submit2Adapter(VkQueue q,uint32_t count,const VkSubmitInfo2* infos,VkFence fence){return driverSubmit2(q,count,infos,fence);}
static uint32_t xrCalls{};
static XrResult XRAPI_PTR xrWait(XrSession,const XrFrameWaitInfo*,XrFrameState* state){++xrCalls;state->predictedDisplayTime=7654321012345;state->predictedDisplayPeriod=8333333;state->shouldRender=XR_TRUE;return XR_SUCCESS;}
static std::string read(const std::string& path){std::ifstream file(path);return {std::istreambuf_iterator<char>(file),{}};}
int main(){
 SetEnvironmentVariableA("KHARVOX_EXTENDED_LOGGING","1");
 FrameTracePolicy p;p.key(true);require(!p.startBoundary(false));require(p.startBoundary(true));
 p.key(true);for(int i=0;i<3;++i)require(!p.finishBoundary(true));require(p.finishBoundary(true));require(!p.startBoundary(true));
 p.key(false);p.key(true);require(p.startBoundary(true));require(p.finishBoundary(false));
 for(unsigned c=2;c<FrameTracePolicy::captureLimit;++c){p.key(false);p.key(true);require(p.startBoundary(true));for(int i=0;i<3;++i)require(!p.finishBoundary(true));require(p.finishBoundary(true));}
 p.key(false);p.key(true);require(!p.startBoundary(true));
 const auto marker=kharvox::runtimePath(L"debug_native_frame_analysis");
 require(!std::filesystem::exists(marker));std::ofstream(marker.c_str())<<"";
 struct Cleanup {std::wstring path;~Cleanup(){std::filesystem::remove(path);}} cleanup{marker};
 trace::initialize();require(trace::enabled()&&!trace::active());
 PFN_xrWaitFrame xrWaitCall=&xrWait;trace::wrapXrFrameTrace("xrWaitFrame",xrWaitCall);
 driverDraw=reinterpret_cast<PFN_vkCmdDraw>(trace::wrap("vkCmdDraw",reinterpret_cast<PFN_vkVoidFunction>(&draw),true));
 auto appDraw=reinterpret_cast<PFN_vkCmdDraw>(trace::wrap("vkCmdDraw",reinterpret_cast<PFN_vkVoidFunction>(&adapter)));
 driverSubmit2=reinterpret_cast<PFN_vkQueueSubmit2KHR>(trace::wrap("vkQueueSubmit2KHR",reinterpret_cast<PFN_vkVoidFunction>(&submit2),true));
 auto appSubmit2=reinterpret_cast<PFN_vkQueueSubmit2KHR>(trace::wrap("vkQueueSubmit2KHR",reinterpret_cast<PFN_vkVoidFunction>(&submit2Adapter)));
 const auto cb=reinterpret_cast<VkCommandBuffer>(102);
 appDraw(cb,7,2,3,4);require(draws==1&&vertices==7&&!trace::active());
 auto poolCall=reinterpret_cast<PFN_vkCreateCommandPool>(trace::wrap("vkCreateCommandPool",reinterpret_cast<PFN_vkVoidFunction>(&createPool)));
 auto allocateCall=reinterpret_cast<PFN_vkAllocateCommandBuffers>(trace::wrap("vkAllocateCommandBuffers",reinterpret_cast<PFN_vkVoidFunction>(&allocate)));
 VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pi.queueFamilyIndex=3;VkCommandPool pool{};
 require(poolCall({},&pi,nullptr,&pool)==VK_SUCCESS);VkCommandBuffer out{};
 VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ai.commandPool=pool;ai.commandBufferCount=1;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;
 require(allocateCall({},&ai,&out)==VK_SUCCESS&&out==cb);
 auto beginCall=reinterpret_cast<PFN_vkBeginCommandBuffer>(trace::wrap("vkBeginCommandBuffer",reinterpret_cast<PFN_vkVoidFunction>(&begin)));
 auto endPassCall=reinterpret_cast<PFN_vkCmdEndRenderPass>(trace::wrap("vkCmdEndRenderPass",reinterpret_cast<PFN_vkVoidFunction>(&endPass)));
 VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};require(beginCall(cb,&bi)==VK_SUCCESS);endPassCall(cb);
 trace::requestCapture();trace::endFrame(100,true);require(!trace::active());trace::presentFinished();require(trace::active());
 const auto deferred="trace-test-deferred-"+std::to_string(GetCurrentProcessId())+".txt";
 trace::writeOrDefer(deferred.c_str(),"deferred payload\n");require(!std::filesystem::exists(kharvox::logPathA(deferred.c_str())));
 for(uint64_t frame=101;frame<=104;++frame){trace::row("pre-root-probe",0,{frame});trace::beginFrame(frame);trace::setEye(0);appDraw(cb,19,1,0,0);
  XrFrameWaitInfo xw{XR_TYPE_FRAME_WAIT_INFO};XrFrameState xs{XR_TYPE_FRAME_STATE};require(xrWaitCall({},&xw,&xs)==XR_SUCCESS&&xs.predictedDisplayPeriod==8333333);
  VkSemaphore wait=reinterpret_cast<VkSemaphore>(777),signal=reinterpret_cast<VkSemaphore>(888);VkPipelineStageFlags stage=VK_PIPELINE_STAGE_TRANSFER_BIT;
  uint64_t waitValue=1234567890123ull,signalValue=1234567890124ull;
  VkTimelineSemaphoreSubmitInfo ti{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};ti.waitSemaphoreValueCount=ti.signalSemaphoreValueCount=1;ti.pWaitSemaphoreValues=&waitValue;ti.pSignalSemaphoreValues=&signalValue;
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.pNext=&ti;submit.waitSemaphoreCount=submit.signalSemaphoreCount=submit.commandBufferCount=1;submit.pWaitSemaphores=&wait;submit.pSignalSemaphores=&signal;submit.pWaitDstStageMask=&stage;submit.pCommandBuffers=&cb;
  {trace::SubmitScope scoped("test-submit",reinterpret_cast<VkQueue>(9),1,&submit,reinterpret_cast<VkFence>(10));scoped.result(VK_ERROR_DEVICE_LOST);waitValue=0;signalValue=0;}
  VkSemaphoreSubmitInfo sw{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};sw.semaphore=wait;sw.value=9876543210123ull;sw.stageMask=VK_PIPELINE_STAGE_2_COPY_BIT;sw.deviceIndex=2;
  VkCommandBufferSubmitInfo sc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};sc.commandBuffer=cb;sc.deviceMask=3;
  VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};si.waitSemaphoreInfoCount=1;si.pWaitSemaphoreInfos=&sw;si.commandBufferInfoCount=1;si.pCommandBufferInfos=&sc;expectedSubmit2=&si;
  require(appSubmit2(reinterpret_cast<VkQueue>(9),1,&si,reinterpret_cast<VkFence>(10))==VK_ERROR_DEVICE_LOST);
  trace::endFrame(frame,true);require(trace::active());trace::presentFinished();require(trace::active()==(frame<104));
 }
 require(draws==5&&vertices==19&&submit2Calls==4);
 const auto stem="native_frame_analysis-"+std::to_string(GetCurrentProcessId())+"-1.tsv";
 const auto data=read(kharvox::logPathA(stem.c_str()));
 for(const auto* text:{"frames=4","dropped=0","openScopesAtStop=0","command-existing","pool-existing","driver:vkCmdDraw","adapter:vkCmdDraw","submit-command","submit-wait","submit-signal","1234567890123","1234567890124","\t-4\t"})require(data.find(text)!=std::string::npos);
 const auto command=data.find("command-existing");const auto lineEnd=data.find('\n',command);require(data.substr(command,lineEnd-command).ends_with("\t1")); // ending a PASS must not close its command buffer
 require(data.find("9876543210123")!=std::string::npos&&data.find("adapter-submit2-khr")!=std::string::npos&&data.find("nested-submit")!=std::string::npos);
 require(xrCalls==4&&data.find("xr-frame-state")!=std::string::npos&&data.find("7654321012345")!=std::string::npos&&data.find("xrWaitFrame")!=std::string::npos);
 std::istringstream lines(data);std::string line;uint32_t probes{};
 while(std::getline(lines,line))if(line.find("\tpre-root-probe\t")!=std::string::npos){std::istringstream columns(line);std::string value;uint64_t frame{};for(unsigned col=0;std::getline(columns,value,'\t');++col){if(col==1)frame=std::stoull(value);if(col==10)require(frame==std::stoull(value));}++probes;}
 require(probes==4);
 require(read(kharvox::logPathA(deferred.c_str()))=="deferred payload\n");
 return 0;
}

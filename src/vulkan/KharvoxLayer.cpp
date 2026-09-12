#include "../common/RuntimeLog.h"
#include "../common/StallDiagnostics.h"
#include "../common/DiagnosticLogging.h"
#include "../native/NativeStereo.h"
#include "../native/NativeFrameTrace.h"
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <windows.h>
#include <atomic>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include "../common/RuntimePaths.h"
#include "../openxr/OpenXRRuntimePolicy.h"
#include "GameImageRetirementDispatch.h"
#include "../fsr/Fsr1Policy.h"
#include "DispatchMemberLookup.h"
#include "IndependentSurface.h"
#include "IndependentEngineSize.h"
#include "CoreSurfaceWindow.h"
#include "../openxr/RuntimeVulkanDispatch.h"

static DWORD portableGetFileAttributesW(LPCWSTR path) {
    const auto name = wcsrchr(path, L'\\');
    return GetFileAttributesW(kharvox::runtimePath(name ? name + 1 : path).c_str());
}
#define GetFileAttributesW portableGetFileAttributesW
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "../openxr/OpenXRBootstrap.h"
#include "../camera/CameraHook.h"
#include "../hands/HandSceneDepthTracker.h"
#include "../hud/HudGpuDiagnostic.h"
#include "../weapon/WeaponHook.h"

namespace {
// An implicit layer may be discovered by any Vulkan application. Fail closed
// on process identification and leave all non-DOOM Vulkan calls untouched.
bool isDoomProcess() {
    static const bool supported = [] {
        wchar_t path[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, path, 32768);
        if (!length || length >= 32768) return false;
        const wchar_t* name = wcsrchr(path, L'\\');
        return _wcsicmp(name ? name + 1 : path, L"DOOMx64vk.exe") == 0;
    }();
    return supported;
}

struct ChainBase { VkStructureType sType; const ChainBase* pNext; };
template<class H> void* key(H h) { return h ? *reinterpret_cast<void**>(h) : nullptr; }

float configuredRenderScale() {
    std::ifstream config(kharvox::runtimePathA("render_scale.cfg"));
    float value{};
    if (config >> value) return kharvox::effectiveRenderScale(value, false);
    char text[64]{};
    if (GetEnvironmentVariableA("KHARVOX_RENDER_SCALE", text, sizeof(text))) {
        char* end{};
        value = std::strtof(text, &end);
        if (end != text) return kharvox::effectiveRenderScale(value, false);
    }
    return 1.f;
}

struct InstanceDispatch {
    VkInstance instance{};
    PFN_vkGetInstanceProcAddr gipa{};
    PFN_vkDestroyInstance destroy{};
    PFN_vkCreateDevice createDevice{};
    PFN_vkCreateWin32SurfaceKHR createWin32Surface{};
    PFN_vkDestroySurfaceKHR destroySurface{};
    PFN_vkGetPhysicalDeviceProperties getPhysicalDeviceProperties{};
    PFN_vkEnumerateDeviceExtensionProperties enumerateDeviceExtensionProperties{};
    PFN_vkGetPhysicalDeviceFeatures2 getPhysicalDeviceFeatures2{};
    PFN_vkGetPhysicalDeviceQueueFamilyProperties getPhysicalDeviceQueueFamilyProperties{};
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR surfaceCaps{};
    PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR surfaceCaps2{};
    bool independentSurface{};
    bool coreSurface{};
    VkExtent2D sourceExtent{};
    bool runtimeAuxiliary{};
};
struct DeviceDispatch {
    VkPhysicalDevice physical{};
    bool independentSurface{};
    bool coreSurface{};
    PFN_vkGetDeviceProcAddr gdpa{};
    PFN_vkDestroyDevice destroy{};
    PFN_vkGetDeviceQueue getQueue{};
    PFN_vkGetDeviceQueue2 getQueue2{};
    PFN_vkCreateSwapchainKHR createSwapchain{};
    PFN_vkDestroySwapchainKHR destroySwapchain{};
    PFN_vkGetSwapchainImagesKHR getSwapchainImages{};
    PFN_vkAcquireNextImageKHR acquire{};
    PFN_vkAcquireNextImage2KHR acquire2{};
    PFN_vkQueueSubmit submit{};
    PFN_vkQueueSubmit2 submit2{};
    PFN_vkQueuePresentKHR present{};
    PFN_vkCmdPushConstants cmdPushConstants{};
    PFN_vkCmdUpdateBuffer cmdUpdateBuffer{};
    PFN_vkMapMemory mapMemory{};
    PFN_vkUnmapMemory unmapMemory{};
    PFN_vkBindBufferMemory bindBufferMemory{};
    PFN_vkUpdateDescriptorSets updateDescriptorSets{};
    PFN_vkCmdBindDescriptorSets cmdBindDescriptorSets{};
    PFN_vkCmdBindPipeline cmdBindPipeline{};
    PFN_vkCmdBindVertexBuffers cmdBindVertexBuffers{};
    PFN_vkCmdBindIndexBuffer cmdBindIndexBuffer{};
    PFN_vkCmdDraw cmdDraw{};
    PFN_vkCmdDrawIndexed cmdDrawIndexed{};
    PFN_vkCmdDrawIndirect cmdDrawIndirect{};
    PFN_vkCmdDrawIndexedIndirect cmdDrawIndexedIndirect{};
    PFN_vkCmdSetViewport cmdSetViewport{};
    PFN_vkCmdSetScissor cmdSetScissor{};
    PFN_vkCreateGraphicsPipelines createGraphicsPipelines{};
    PFN_vkDestroyPipeline destroyPipeline{};
    PFN_vkCreateImage createImage{};
    PFN_vkDestroyImage destroyImage{};
    PFN_vkCreateImageView createImageView{};
    PFN_vkDestroyImageView destroyImageView{};
    PFN_vkCreateFramebuffer createFramebuffer{};
    PFN_vkDestroyFramebuffer destroyFramebuffer{};
    PFN_vkCreateRenderPass createRenderPass{};
    PFN_vkCreateRenderPass2 createRenderPass2{};
    PFN_vkDestroyRenderPass destroyRenderPass{};
    PFN_vkCmdBeginRenderPass cmdBeginRenderPass{};
    PFN_vkCmdBeginRenderPass2 cmdBeginRenderPass2{};
    PFN_vkCmdNextSubpass cmdNextSubpass{};
    PFN_vkCmdNextSubpass2 cmdNextSubpass2{};
    PFN_vkCmdEndRenderPass cmdEndRenderPass{};
    PFN_vkCmdEndRenderPass2 cmdEndRenderPass2{};
    KharvoxVulkanDispatch xr{};
    bool runtimeAuxiliary{};
};

std::mutex stateMutex, logMutex;
std::recursive_mutex queueAccessMutex;
void lockQueueAccess(){queueAccessMutex.lock();}
void unlockQueueAccess(){queueAccessMutex.unlock();}
std::unordered_map<void*, InstanceDispatch> instances;
std::unordered_map<void*, DeviceDispatch> devices;
std::atomic<uint32_t> instanceCreateDepth{0};
struct InstanceCreateScope {
    bool nested{};
    InstanceCreateScope()
        : nested(instanceCreateDepth.fetch_add(1, std::memory_order_acq_rel) != 0) {}
    ~InstanceCreateScope() {
        instanceCreateDepth.fetch_sub(1, std::memory_order_acq_rel);
    }
};
std::atomic<uint64_t> acquireCount{0}, submitCount{0}, presentCount{0};
std::atomic<uint64_t> swapchainCreateCount{0}, swapchainDestroyCount{0};
std::atomic<uint64_t> explicitEyeSubmitCount{0};
std::atomic<uint64_t> doomSwapchainBarrierCount{0};
std::array<std::atomic<uint64_t>, 2> eyeViewportProbeCount{};
std::array<std::atomic<uint64_t>, 2> eyeScissorProbeCount{};
struct TraceImage{uint32_t width{},height{};VkFormat format{};VkImageUsageFlags usage{};uint64_t renderPassUses{},leftUses{},rightUses{},monoUses{};};
struct TraceFramebuffer{uint32_t width{},height{};std::vector<VkImage> images;};
std::mutex renderTargetMutex;
std::unordered_map<uint64_t,TraceImage> traceImages;
std::unordered_map<uint64_t,VkImage> traceViews;
std::unordered_map<uint64_t,TraceFramebuffer> traceFramebuffers;
std::atomic<bool> renderTargetSummaryLogged{false};
std::atomic<bool> worldRenderTraceStarted{false};
std::atomic<uint64_t> worldRenderTraceStartPresent{0};
std::atomic<uint64_t> constantMatrixHits{0};
struct MappedMemory { VkDeviceMemory memory{}; unsigned char* data{}; VkDeviceSize offset{}; size_t size{}; };
std::mutex mappedMutex;
std::vector<MappedMemory> mappedMemory;
size_t mappedScanCursor=0;
struct BufferBinding{VkDeviceMemory memory{};VkDeviceSize offset{};};
struct DescriptorBuffer{uint32_t binding{},arrayElement{};VkDescriptorType type{};VkBuffer buffer{};VkDeviceSize offset{},range{};};
struct BoundBlock{uint32_t setIndex{},binding{},arrayElement{};uint64_t semanticKey{};unsigned char* data{};size_t bytes{};};
struct CommandState{VkPipeline pipeline{};std::vector<BoundBlock> blocks;uint64_t draws{};};
std::mutex descriptorMutex;
std::unordered_map<uint64_t,BufferBinding> bufferBindings;
std::unordered_map<uint64_t,std::vector<DescriptorBuffer>> descriptorBuffers;
std::unordered_map<uint64_t,CommandState> commandStates;
struct DrawMatrixAggregate{uint64_t key{};size_t uses{},directCalls{},indirectCalls{},submittedDraws{};uint64_t pipelineMask{};std::array<float,16> values{};float rowScore{},columnScore{},variance{};};
std::unordered_map<uint64_t,DrawMatrixAggregate> drawCaptureAggregates,drawBaselineAggregates;
uint64_t drawPatchSemanticKey=0;
struct DescriptorMatrixSample{size_t ordinal{};std::array<float,16> values{};};
std::vector<DescriptorMatrixSample> descriptorFrameSamples,descriptorBaselineSamples;
int descriptorCaptureMode=0;
size_t descriptorOrdinal=0,descriptorPatchOrdinal=1;
size_t descriptorSlotOrdinal=0;
std::vector<size_t> descriptorPatchSlots{1,4};
size_t selectedDescriptorCandidate=0;
constexpr size_t candidateCenter=0x3c00000,candidateRadius=0x400,candidateBytes=candidateRadius*2;
std::array<unsigned char,candidateBytes> candidateBaseline{};
bool candidateBaselineValid=false,candidatePatchLogged=false;
size_t candidateMatrixOffset=candidateRadius;
size_t candidateArenaOffset=candidateCenter;
constexpr std::array<size_t,10> rankedArenaOffsets={0x37E8930,0x3803AF0,0x3C00000,0x3D00300,0x3E00100,0x3F00000,0x2DB9C0,0x301A10,0x405320,0x532FD0};
struct RankedBaseline{size_t arenaOffset{};std::array<float,16> values{};float score{};};
std::vector<RankedBaseline> rankedBaselines;

void logLine(const std::string& text, bool operational = false) {
    kharvox::writeRuntimeLog("[KHARVOX]", text, operational);
}

bool extendedLoggingEnabled() {
    static const bool enabled=[] {
        char value[16]{};
        const DWORD length=GetEnvironmentVariableA("KHARVOX_EXTENDED_LOGGING",value,sizeof(value));
        return length>0&&length<sizeof(value)&&std::strcmp(value,"0")!=0;
    }();
    return enabled;
}

double elapsedMilliseconds(const LARGE_INTEGER& begin,const LARGE_INTEGER& end) {
    static const double frequency=[] { LARGE_INTEGER value{}; QueryPerformanceFrequency(&value); return double(value.QuadPart); }();
    return frequency>0.0?double(end.QuadPart-begin.QuadPart)*1000.0/frequency:0.0;
}

bool traceDiagnosticCall(uint64_t sequence) { return sequence<=240||sequence%600==0; }

void logExtended(const std::string& text) {
    if(extendedLoggingEnabled())logLine("[EXTENDED] "+text);
}

std::mutex surfaceWindowMutex;
std::unordered_map<uint64_t,HWND> surfaceWindows;
bool isGameSurface(VkSurfaceKHR surface){
    std::lock_guard<std::mutex> lock(surfaceWindowMutex);
    return surfaceWindows.count(reinterpret_cast<uint64_t>(surface))!=0;
}
std::mutex independentSwapchainMutex;
std::unordered_set<uint64_t> independentSwapchains;
bool isIndependentSwapchain(VkSwapchainKHR swapchain){
    std::lock_guard<std::mutex> lock(independentSwapchainMutex);
    return independentSwapchains.count(reinterpret_cast<uint64_t>(swapchain))!=0;
}

std::string windowSnapshot(HWND window,const char* stage) {
    std::ostringstream out;
    out<<"[WINDOW] stage="<<stage<<" hwnd="<<reinterpret_cast<uint64_t>(window);
    if(!window){out<<" missing";return out.str();}
    RECT rect{},client{};
    const BOOL rectOk=GetWindowRect(window,&rect);
    const BOOL clientOk=GetClientRect(window,&client);
    DWORD pid{};
    const DWORD thread=GetWindowThreadProcessId(window,&pid);
    const HWND foreground=GetForegroundWindow();
    DWORD foregroundPid{};
    GetWindowThreadProcessId(foreground,&foregroundPid);
    out<<" pid="<<pid<<" thread="<<thread
       <<" visible="<<(IsWindowVisible(window)?1:0)
       <<" iconic="<<(IsIconic(window)?1:0)
       <<" zoomed="<<(IsZoomed(window)?1:0)
       <<" hung="<<(IsHungAppWindow(window)?1:0)
       <<" foreground="<<(foreground==window?1:0)
       <<" foregroundHwnd="<<reinterpret_cast<uint64_t>(foreground)
       <<" foregroundPid="<<foregroundPid
       <<" style=0x"<<std::hex<<static_cast<uint64_t>(GetWindowLongPtrW(window,GWL_STYLE))
       <<" exStyle=0x"<<static_cast<uint64_t>(GetWindowLongPtrW(window,GWL_EXSTYLE))<<std::dec;
    if(rectOk)out<<" rect="<<rect.left<<','<<rect.top<<','<<rect.right<<','<<rect.bottom;
    else out<<" rect=error:"<<GetLastError();
    if(clientOk)out<<" client="<<(client.right-client.left)<<'x'<<(client.bottom-client.top);
    else out<<" client=error:"<<GetLastError();
    const HMONITOR monitor=MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    if(monitor&&GetMonitorInfoW(monitor,&info))
        out<<" monitor="<<info.rcMonitor.left<<','<<info.rcMonitor.top<<','<<info.rcMonitor.right<<','<<info.rcMonitor.bottom
           <<" work="<<info.rcWork.left<<','<<info.rcWork.top<<','<<info.rcWork.right<<','<<info.rcWork.bottom;
    return out.str();
}

void recordRenderPass(VkFramebuffer framebuffer){const auto eye=KharvoxCameraCurrentEye();std::lock_guard<std::mutex>lock(renderTargetMutex);auto fit=traceFramebuffers.find(reinterpret_cast<uint64_t>(framebuffer));if(fit==traceFramebuffers.end())return;for(auto image:fit->second.images){auto it=traceImages.find(reinterpret_cast<uint64_t>(image));if(it==traceImages.end())continue;auto&info=it->second;info.renderPassUses++;if(eye==VREye::Left)info.leftUses++;else if(eye==VREye::Right)info.rightUses++;else info.monoUses++;}}
void logRenderTargetSummary(){if(renderTargetSummaryLogged.exchange(true))return;struct Candidate{uint64_t image;TraceImage info;};std::vector<Candidate> candidates;{std::lock_guard<std::mutex>lock(renderTargetMutex);for(const auto&pair:traceImages)if((pair.second.usage&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)&&pair.second.width>=800&&pair.second.height>=600)candidates.push_back({pair.first,pair.second});}std::sort(candidates.begin(),candidates.end(),[](const Candidate&a,const Candidate&b){const auto ae=a.info.leftUses+a.info.rightUses,be=b.info.leftUses+b.info.rightUses;if(ae!=be)return ae>be;if(a.info.renderPassUses!=b.info.renderPassUses)return a.info.renderPassUses>b.info.renderPassUses;return uint64_t(a.info.width)*a.info.height>uint64_t(b.info.width)*b.info.height;});logLine("[RT] concise world render-target candidates="+std::to_string(candidates.size()));for(size_t i=0;i<std::min<size_t>(12,candidates.size());i++){const auto&c=candidates[i];std::ostringstream x;x<<"[RT] #"<<(i+1)<<" image="<<c.image<<" "<<c.info.width<<'x'<<c.info.height<<" format="<<c.info.format<<" usage=0x"<<std::hex<<c.info.usage<<std::dec<<" uses="<<c.info.renderPassUses<<" left="<<c.info.leftUses<<" right="<<c.info.rightUses<<" mono="<<c.info.monoUses;logLine(x.str());}}
bool logFrame(uint64_t n) { return n <= 3; }
float matrixScore(const float* m,int stride){float lens[3]{};for(int r=0;r<3;r++)for(int c=0;c<3;c++){float v=m[r*stride+c];if(!std::isfinite(v)||std::fabs(v)>100.f)return 0;lens[r]+=v*v;}for(float l:lens)if(l<0.0001f||l>10000.f)return 0;float e=0;for(int a=0;a<3;a++)for(int b=a+1;b<3;b++){float d=0;for(int c=0;c<3;c++)d+=m[a*stride+c]*m[b*stride+c];e+=std::fabs(d)/std::sqrt(lens[a]*lens[b]);}return std::max(0.f,1.f-e/3.f);}
float matrixColumnScore(const float*m){float lens[3]{};for(int c=0;c<3;c++)for(int r=0;r<3;r++){float v=m[r*4+c];if(!std::isfinite(v)||std::fabs(v)>100.f)return 0;lens[c]+=v*v;}for(float l:lens)if(l<0.0001f||l>10000.f)return 0;float e=0;for(int a=0;a<3;a++)for(int b=a+1;b<3;b++){float d=0;for(int r=0;r<3;r++)d+=m[r*4+a]*m[r*4+b];e+=std::fabs(d)/std::sqrt(lens[a]*lens[b]);}return std::max(0.f,1.f-e/3.f);}
void captureDrawMatrices(VkCommandBuffer cb,bool indirect=false,uint32_t drawCount=1){if(!descriptorCaptureMode)return;std::lock_guard<std::mutex> lock(descriptorMutex);if(drawCaptureAggregates.size()>20000)return;auto state=commandStates.find(reinterpret_cast<uint64_t>(cb));if(state==commandStates.end())return;uint64_t pipelineValue=reinterpret_cast<uint64_t>(state->second.pipeline);uint64_t pipelineBit=1ull<<((pipelineValue^(pipelineValue>>17)^(pipelineValue>>37))&63);size_t blockCount=0;for(auto&block:state->second.blocks){if(blockCount++>=16)break;const size_t scanBytes=std::min<size_t>(block.bytes,4096);for(size_t off=0;off+64<=scanBytes;off+=16){auto values=reinterpret_cast<float*>(block.data+off);float row=matrixScore(values,4),column=matrixColumnScore(values);if(std::max(row,column)<0.75f)continue;uint64_t semantic=block.semanticKey^(uint64_t(off)*0x9E3779B185EBCA87ull);auto found=drawCaptureAggregates.find(semantic);if(found==drawCaptureAggregates.end()){DrawMatrixAggregate a;a.key=semantic;a.uses=1;a.directCalls=indirect?0:1;a.indirectCalls=indirect?1:0;a.submittedDraws=drawCount;a.pipelineMask=pipelineBit;std::memcpy(a.values.data(),values,64);a.rowScore=row;a.columnScore=column;drawCaptureAggregates.emplace(semantic,a);}else{auto&a=found->second;a.uses++;a.directCalls+=indirect?0:1;a.indirectCalls+=indirect?1:0;a.submittedDraws+=drawCount;a.pipelineMask|=pipelineBit;for(int i=0;i<16;i++)a.variance+=std::fabs(values[i]-a.values[i]);}}}}
void finishDrawCapture(int mode){if(mode==1&&!drawCaptureAggregates.empty()){drawBaselineAggregates=drawCaptureAggregates;drawPatchSemanticKey=0;logLine("[CAMDRAW] baseline semantics="+std::to_string(drawBaselineAggregates.size()));}else if(mode==2&&!drawCaptureAggregates.empty()){struct R{uint64_t key{};size_t uses{},indirectCalls{},submittedDraws{},pipelines{},basisChanged{};float change{},basisChange{},maxDelta{},variance{},score{};};std::vector<R> ranked;for(auto&entry:drawBaselineAggregates){auto found=drawCaptureAggregates.find(entry.first);if(found==drawCaptureAggregates.end())continue;auto&a=entry.second;auto&b=found->second;size_t indirectCalls=std::min(a.indirectCalls,b.indirectCalls);if(!indirectCalls)continue;float variance=a.variance/std::max<size_t>(1,a.uses)+b.variance/std::max<size_t>(1,b.uses);if(variance>0.05f)continue;float change=0,basisChange=0,maxDelta=0;size_t basisChanged=0;for(int i=0;i<16;i++){float d=std::fabs(a.values[i]-b.values[i]);change+=d;maxDelta=std::max(maxDelta,d);if((i%4)<3&&(i/4)<3){basisChange+=d;if(d>0.001f)basisChanged++;}}if(change<0.01f||maxDelta>4.f||basisChanged<2||basisChange<0.01f)continue;size_t uses=std::min(a.uses,b.uses),submittedDraws=std::min(a.submittedDraws,b.submittedDraws);uint64_t mask=a.pipelineMask|b.pipelineMask;size_t pipelines=0;while(mask){mask&=mask-1;pipelines++;}float shape=std::max({a.rowScore,a.columnScore,b.rowScore,b.columnScore});ranked.push_back({entry.first,uses,indirectCalls,submittedDraws,pipelines,basisChanged,change,basisChange,maxDelta,variance,float(submittedDraws)*shape*basisChange*(1.f+0.25f*float(pipelines))/(1.f+variance)});}std::sort(ranked.begin(),ranked.end(),[](auto&a,auto&b){return a.score>b.score;});logLine("[CAMDRAW] coordinated-basis world semantics="+std::to_string(ranked.size()));for(size_t i=0;i<std::min<size_t>(10,ranked.size());i++){auto&r=ranked[i];std::ostringstream x;x<<"[CAMDRAW] ranked#"<<i+1<<" key=0x"<<std::hex<<r.key<<std::dec<<" uses="<<r.uses<<" indirectCalls="<<r.indirectCalls<<" submittedDraws="<<r.submittedDraws<<" pipelines="<<r.pipelines<<" basisChanged="<<r.basisChanged<<" basisChange="<<r.basisChange<<" change="<<r.change<<" maxDelta="<<r.maxDelta<<" variance="<<r.variance;logLine(x.str());}if(!ranked.empty()){drawPatchSemanticKey=ranked.front().key;logLine("[CAMDRAW] strongest coordinated world-camera semantic armed for F9/F10");}else{drawPatchSemanticKey=0;logLine("[CAMDRAW] no coordinated world-camera semantic armed; F9/F10 disabled");}}if(mode&&!drawCaptureAggregates.empty())descriptorCaptureMode=0;drawCaptureAggregates.clear();}
void scanConstants(const char* source,const void* data,size_t bytes){if(!data||bytes<64)return;auto b=static_cast<const unsigned char*>(data);for(size_t o=0;o+64<=bytes;o+=16){auto f=reinterpret_cast<const float*>(b+o);float s=matrixScore(f,4);if(s>0.995f){auto n=++constantMatrixHits;if(n<=80||n%1000==0){std::ostringstream x;x<<"[CAMCONST] "<<source<<" hit="<<n<<" address="<<static_cast<const void*>(b+o)<<" offset="<<o<<" bytes="<<bytes<<" score="<<s;logLine(x.str());}return;}}}
void scanMappedMemory(){std::lock_guard<std::mutex> lock(mappedMutex);for(auto&m:mappedMemory){if(!m.data||m.size<16*1024*1024)continue;constexpr size_t chunk=1024*1024;size_t off=mappedScanCursor%m.size;size_t bytes=std::min(chunk,m.size-off);scanConstants("mapped",m.data+off,bytes);mappedScanCursor=(off+bytes)%m.size;return;}size_t scanned=0;for(auto it=mappedMemory.rbegin();it!=mappedMemory.rend()&&scanned<2*1024*1024;++it){auto&m=*it;if(!m.data||!m.size)continue;size_t bytes=std::min<size_t>(m.size,2*1024*1024-scanned);scanConstants("mapped",m.data,bytes);scanned+=bytes;}}
MappedMemory* cameraArena(){for(auto&m:mappedMemory)if(m.data&&m.size>=candidateCenter+candidateRadius)return &m;return nullptr;}
void captureMappedCandidate(){std::lock_guard<std::mutex> lock(mappedMutex);auto*m=cameraArena();if(!m){logLine("[CAMTARGET] F6 failed: 64 MiB arena unavailable");return;}std::memcpy(candidateBaseline.data(),m->data+candidateCenter-candidateRadius,candidateBytes);candidateBaselineValid=true;candidateMatrixOffset=candidateRadius;candidateArenaOffset=candidateCenter;rankedBaselines.clear();for(size_t off:rankedArenaOffsets){if(off+64>m->size)continue;RankedBaseline b;b.arenaOffset=off;std::memcpy(b.values.data(),m->data+off,64);b.score=matrixScore(b.values.data(),4);if(b.score>0.8f)rankedBaselines.push_back(b);}logLine("[CAMTARGET] CAM BASELINE CAPTURED arena="+std::to_string(reinterpret_cast<uintptr_t>(m->data))+" focusedMatrices="+std::to_string(rankedBaselines.size()));}
struct MatrixHypothesis{size_t offset{};float change{},beforeScore{},afterScore{};};
void compareMappedCandidate(){std::lock_guard<std::mutex> lock(mappedMutex);auto*m=cameraArena();if(!m||!candidateBaselineValid){logLine("[CAMTARGET] F7 failed: capture F6 baseline first");return;}struct RankedResult{size_t arenaOffset{};float change{},beforeScore{},afterScore{},score{};};std::vector<RankedResult> results;for(auto&b:rankedBaselines){float now[16]{};std::memcpy(now,m->data+b.arenaOffset,64);float change=0,maxDelta=0;for(int i=0;i<16;i++){float d=std::fabs(now[i]-b.values[i]);change+=d;maxDelta=std::max(maxDelta,d);}float after=matrixScore(now,4);if(change>0.0001f&&after>0.8f&&maxDelta<4.f)results.push_back({b.arenaOffset,change,b.score,after,change*(b.score+after)});}std::sort(results.begin(),results.end(),[](auto&a,auto&b){return a.score>b.score;});logLine("[CAMTARGET] F7 focused reactive matrices="+std::to_string(results.size()));for(size_t i=0;i<std::min<size_t>(10,results.size());i++){auto&c=results[i];std::ostringstream x;x<<"[CAMTARGET] ranked#"<<i+1<<" arenaOffset=0x"<<std::hex<<c.arenaOffset<<std::dec<<" change="<<c.change<<" orthBefore="<<c.beforeScore<<" orthAfter="<<c.afterScore;logLine(x.str());}if(!results.empty()){candidateArenaOffset=results.front().arenaOffset;logLine("[CAMTARGET] strongest focused candidate assigned to F9 yaw / F10 pitch");}else logLine("[CAMTARGET] focused top-10 correlation: NO stable matrix response");}
void descriptorFrameBoundary(){
    std::lock_guard<std::mutex> lock(descriptorMutex);
    finishDrawCapture(descriptorCaptureMode);
    if(descriptorCaptureMode==1&&!descriptorFrameSamples.empty()){
        descriptorBaselineSamples=descriptorFrameSamples;descriptorCaptureMode=0;
        logLine("[CAMDESC] CAM BASELINE CAPTURED matrices="+std::to_string(descriptorBaselineSamples.size()));
    }else if(descriptorCaptureMode==2&&!descriptorFrameSamples.empty()){
        struct R{size_t slot{};float change{},beforeScore{},afterScore{},maxDelta{};size_t repeats{1};std::array<float,16> before{},after{};};
        std::unordered_map<size_t,const DescriptorMatrixSample*> current;
        for(auto&sample:descriptorFrameSamples)current[sample.ordinal]=&sample;
        std::vector<R> reactive;
        for(auto&a:descriptorBaselineSamples){
            auto found=current.find(a.ordinal);if(found==current.end())continue;auto&b=*found->second;
            float change=0,maxDelta=0;for(int j=0;j<16;j++){float d=std::fabs(a.values[j]-b.values[j]);change+=d;maxDelta=std::max(maxDelta,d);}
            float bs=matrixScore(a.values.data(),4),as=matrixScore(b.values.data(),4);
            if(change>0.01f&&maxDelta<1.5f&&bs>0.95f&&as>0.95f)reactive.push_back({a.ordinal,change,bs,as,maxDelta,1,a.values,b.values});
        }
        std::vector<R> grouped;
        for(auto&r:reactive){bool merged=false;for(auto&g:grouped){float difference=0;for(int j=0;j<16;j++)difference+=std::fabs(r.before[j]-g.before[j])+std::fabs(r.after[j]-g.after[j]);if(difference<0.05f){g.repeats++;merged=true;break;}}if(!merged)grouped.push_back(r);}
        std::sort(grouped.begin(),grouped.end(),[](auto&a,auto&b){if(a.repeats!=b.repeats)return a.repeats>b.repeats;return a.change>b.change;});
        logLine("[CAMDESC] F7 slotMatched="+std::to_string(current.size())+" cameraLike="+std::to_string(reactive.size())+" groups="+std::to_string(grouped.size()));
        for(size_t i=0;i<std::min<size_t>(10,grouped.size());i++){auto&r=grouped[i];std::ostringstream x;x<<"[CAMDESC] ranked#"<<i+1<<" slot="<<r.slot<<" repeats="<<r.repeats<<" change="<<r.change<<" maxDelta="<<r.maxDelta<<" orthBefore="<<r.beforeScore<<" orthAfter="<<r.afterScore;logLine(x.str());}
        descriptorPatchSlots.clear();for(size_t i=0;i<std::min<size_t>(10,grouped.size());i++)descriptorPatchSlots.push_back(grouped[i].slot);selectedDescriptorCandidate=0;
        if(!grouped.empty()){descriptorPatchOrdinal=grouped.front().slot;logLine("[CAMDESC] candidate #1 assigned to F9/F10; candidate #2 assigned to Shift+F9/Shift+F10");}
        descriptorCaptureMode=0;
    }
    descriptorFrameSamples.clear();descriptorOrdinal=0;descriptorSlotOrdinal=0;
}
void pollMappedCandidateKeys(){/* F5-F12 are reserved exclusively for weapon calibration. */}
void patchMappedCandidate(){candidatePatchLogged=false;}
InstanceDispatch instanceState(void* k) { std::lock_guard<std::mutex> l(stateMutex); auto i=instances.find(k); return i==instances.end()?InstanceDispatch{}:i->second; }
DeviceDispatch deviceState(void* k) { std::lock_guard<std::mutex> l(stateMutex); auto i=devices.find(k); return i==devices.end()?DeviceDispatch{}:i->second; }
template<class Member>
Member commandFunction(VkCommandBuffer cb,Member DeviceDispatch::* member){
    return kharvox::lookupDispatchMember(stateMutex,devices,key(cb),member);
}

#if 0 // retired: desktop mirror synchronizes the VR queue to the desktop compositor
constexpr wchar_t kDesktopMirrorClass[] = L"KharvoxDesktopMirrorClass";
constexpr UINT kDesktopMirrorShutdown = WM_APP + 42;

LRESULT CALLBACK desktopMirrorWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CLOSE:
        ShowWindow(window, SW_MINIMIZE);
        return 0;
    case kDesktopMirrorShutdown:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

DWORD WINAPI desktopMirrorWindowThread(void*) {
    const HINSTANCE module = GetModuleHandleW(nullptr);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = desktopMirrorWindowProc;
    windowClass.hInstance = module;
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = kDesktopMirrorClass;
    if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        logLine("Desktop Vulkan mirror window class registration FAILED");
        if (desktopMirror.windowReady) SetEvent(desktopMirror.windowReady);
        return 1;
    }

    RECT outer{0, 0, 1280, 720};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&outer, style, FALSE);
    const int width = outer.right - outer.left;
    const int height = outer.bottom - outer.top;
    const int x = std::max(0, (GetSystemMetrics(SM_CXSCREEN) - width) / 2);
    const int y = std::max(0, (GetSystemMetrics(SM_CYSCREEN) - height) / 2);
    HWND window = CreateWindowExW(
        WS_EX_APPWINDOW, kDesktopMirrorClass, L"KHARVOX Mirror",
        style, x, y, width, height, nullptr, nullptr, module, nullptr);
    desktopMirror.window.store(window);
    if (desktopMirror.windowReady) SetEvent(desktopMirror.windowReady);
    if (!window) {
        logLine("Desktop Vulkan mirror window creation FAILED");
        return 2;
    }
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    logLine("Desktop Vulkan mirror window created client=1280x720");

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    desktopMirror.window.store(nullptr);
    return 0;
}

bool startDesktopMirrorWindow() {
    std::lock_guard<std::mutex> lock(desktopMirrorMutex);
    if (desktopMirror.window.load()) return true;
    if (!desktopMirror.windowReady)
        desktopMirror.windowReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!desktopMirror.windowReady) {
        logLine("Desktop Vulkan mirror ready event creation FAILED");
        return false;
    }
    ResetEvent(desktopMirror.windowReady);
    if (!desktopMirror.windowThread)
        desktopMirror.windowThread = CreateThread(nullptr, 0, desktopMirrorWindowThread, nullptr, 0, nullptr);
    if (!desktopMirror.windowThread) {
        logLine("Desktop Vulkan mirror thread creation FAILED");
        return false;
    }
    const DWORD wait = WaitForSingleObject(desktopMirror.windowReady, 5000);
    return wait == WAIT_OBJECT_0 && desktopMirror.window.load();
}

VkCompositeAlphaFlagBitsKHR chooseCompositeAlpha(VkCompositeAlphaFlagsKHR supported) {
    constexpr VkCompositeAlphaFlagBitsKHR choices[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
    };
    for (auto choice : choices) if (supported & choice) return choice;
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

void destroyDesktopMirrorDeviceResources(const DeviceDispatch& dispatch) {
    std::lock_guard<std::mutex> lock(desktopMirrorMutex);
    auto& mirror = desktopMirror;
    if (!mirror.device) return;
    if (dispatch.xr.queueWaitIdle && mirror.queue) dispatch.xr.queueWaitIdle(mirror.queue);
    if (mirror.fence && dispatch.destroyFence) dispatch.destroyFence(mirror.device, mirror.fence, nullptr);
    if (mirror.copyDone && dispatch.destroySemaphore) dispatch.destroySemaphore(mirror.device, mirror.copyDone, nullptr);
    if (mirror.imageAvailable && dispatch.destroySemaphore) dispatch.destroySemaphore(mirror.device, mirror.imageAvailable, nullptr);
    if (mirror.commandPool && dispatch.xr.destroyCommandPool) dispatch.xr.destroyCommandPool(mirror.device, mirror.commandPool, nullptr);
    if (mirror.swapchain && dispatch.destroySwapchain) dispatch.destroySwapchain(mirror.device, mirror.swapchain, nullptr);
    mirror.swapchain = VK_NULL_HANDLE;
    mirror.commandPool = VK_NULL_HANDLE;
    mirror.commandBuffer = VK_NULL_HANDLE;
    mirror.imageAvailable = VK_NULL_HANDLE;
    mirror.copyDone = VK_NULL_HANDLE;
    mirror.fence = VK_NULL_HANDLE;
    mirror.images.clear();
    mirror.initialized.clear();
    mirror.ready = false;
    mirror.device = VK_NULL_HANDLE;
}

bool createDesktopMirrorSwapchain(VkDevice device, const DeviceDispatch& dispatch, const LayerSwapchain& source) {
    std::lock_guard<std::mutex> lock(desktopMirrorMutex);
    auto& mirror = desktopMirror;
    if (!mirror.enabled || mirror.ready || !mirror.surface || !mirror.physical ||
        !mirror.queueFamilyKnown || !dispatch.createSwapchain || !dispatch.getSwapchainImages ||
        !dispatch.xr.createCommandPool || !dispatch.xr.allocateCommandBuffers ||
        !dispatch.createSemaphore || !dispatch.createFence) return false;

    const auto instance = instanceState(key(mirror.instance));
    if (!instance.getSurfaceCapabilities || !instance.getSurfaceFormats || !instance.getSurfacePresentModes) return false;
    VkSurfaceCapabilitiesKHR capabilities{};
    if (instance.getSurfaceCapabilities(mirror.physical, mirror.surface, &capabilities) != VK_SUCCESS) {
        logLine("Desktop Vulkan mirror surface capabilities query FAILED");
        return false;
    }
    if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        logLine("Desktop Vulkan mirror surface lacks TRANSFER_DST support");
        mirror.enabled = false;
        return false;
    }

    uint32_t formatCount = 0;
    if (instance.getSurfaceFormats(mirror.physical, mirror.surface, &formatCount, nullptr) != VK_SUCCESS || !formatCount) return false;
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    if (instance.getSurfaceFormats(mirror.physical, mirror.surface, &formatCount, formats.data()) != VK_SUCCESS) return false;
    VkSurfaceFormatKHR selected = formats.front();
    for (const auto& format : formats) if (format.format == source.format) { selected = format; break; }

    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = std::clamp(1280u, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        extent.height = std::clamp(720u, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    }
    uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount && imageCount > capabilities.maxImageCount) imageCount = capabilities.maxImageCount;

    VkSwapchainCreateInfoKHR createInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    createInfo.surface = mirror.surface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = selected.format;
    createInfo.imageColorSpace = selected.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.preTransform = capabilities.currentTransform;
    createInfo.compositeAlpha = chooseCompositeAlpha(capabilities.supportedCompositeAlpha);
    createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    createInfo.clipped = VK_TRUE;

    VkResult result = dispatch.createSwapchain(device, &createInfo, nullptr, &mirror.swapchain);
    if (result != VK_SUCCESS) {
        logLine("Desktop Vulkan mirror swapchain creation FAILED result=" + std::to_string(result));
        mirror.swapchain = VK_NULL_HANDLE;
        return false;
    }
    uint32_t mirrorImageCount = 0;
    result = dispatch.getSwapchainImages(device, mirror.swapchain, &mirrorImageCount, nullptr);
    if (result != VK_SUCCESS || !mirrorImageCount) return false;
    mirror.images.resize(mirrorImageCount);
    result = dispatch.getSwapchainImages(device, mirror.swapchain, &mirrorImageCount, mirror.images.data());
    if (result != VK_SUCCESS) return false;
    mirror.initialized.assign(mirrorImageCount, false);

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = mirror.queueFamily;
    if (dispatch.xr.createCommandPool(device, &poolInfo, nullptr, &mirror.commandPool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandPool = mirror.commandPool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    if (dispatch.xr.allocateCommandBuffers(device, &allocation, &mirror.commandBuffer) != VK_SUCCESS) return false;

    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (dispatch.createSemaphore(device, &semaphoreInfo, nullptr, &mirror.imageAvailable) != VK_SUCCESS ||
        dispatch.createSemaphore(device, &semaphoreInfo, nullptr, &mirror.copyDone) != VK_SUCCESS) return false;
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (dispatch.createFence(device, &fenceInfo, nullptr, &mirror.fence) != VK_SUCCESS) return false;

    mirror.device = device;
    mirror.extent = extent;
    mirror.format = selected.format;
    mirror.ready = true;
    mirror.frames = 0;
    std::ostringstream message;
    message << "Desktop Vulkan mirror ACTIVE source=" << source.extent.width << 'x' << source.extent.height
            << " output=" << extent.width << 'x' << extent.height << " images=" << mirrorImageCount
            << " format=" << selected.format;
    logLine(message.str());
    return true;
}

void presentDesktopMirror(VkQueue queue, const DeviceDispatch& dispatch, const VkPresentInfoKHR* presentInfo) {
    if (!presentInfo || !presentInfo->pSwapchains || !presentInfo->pImageIndices) return;
    LayerSwapchain source{};
    uint32_t sourceImageIndex = 0;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        for (uint32_t index = 0; index < presentInfo->swapchainCount; ++index) {
            const auto swapchain = layerSwapchains.find(presentInfo->pSwapchains[index]);
            if (swapchain == layerSwapchains.end()) continue;
            source = swapchain->second;
            sourceImageIndex = presentInfo->pImageIndices[index];
            found = sourceImageIndex < source.images.size();
            if (found) break;
        }
    }
    if (!found) return;
    if (!desktopMirror.ready && desktopMirror.device) createDesktopMirrorSwapchain(desktopMirror.device, dispatch, source);

    std::lock_guard<std::mutex> lock(desktopMirrorMutex);
    auto& mirror = desktopMirror;
    if (!mirror.ready || !mirror.window.load() || !dispatch.waitForFences || !dispatch.resetFences ||
        !dispatch.acquire || !dispatch.xr.resetCommandBuffer || !dispatch.xr.beginCommandBuffer ||
        !dispatch.xr.endCommandBuffer || !dispatch.xr.cmdPipelineBarrier || !dispatch.xr.cmdBlitImage ||
        !dispatch.xr.queueSubmit || !dispatch.present) return;

    VkResult result = dispatch.waitForFences(mirror.device, 1, &mirror.fence, VK_TRUE, UINT64_MAX);
    if (result != VK_SUCCESS) return;
    uint32_t mirrorImageIndex = 0;
    result = dispatch.acquire(mirror.device, mirror.swapchain, UINT64_MAX, mirror.imageAvailable, VK_NULL_HANDLE, &mirrorImageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        logLine("Desktop Vulkan mirror disabled: swapchain out of date");
        mirror.ready = false;
        return;
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) return;
    if (mirrorImageIndex >= mirror.images.size()) return;

    if (dispatch.xr.resetCommandBuffer(mirror.commandBuffer, 0) != VK_SUCCESS) return;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (dispatch.xr.beginCommandBuffer(mirror.commandBuffer, &begin) != VK_SUCCESS) return;

    VkImageMemoryBarrier beginBarriers[2]{};
    beginBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    beginBarriers[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    beginBarriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    beginBarriers[0].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    beginBarriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    beginBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    beginBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    beginBarriers[0].image = source.images[sourceImageIndex];
    beginBarriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    beginBarriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    beginBarriers[1].srcAccessMask = mirror.initialized[mirrorImageIndex] ? VK_ACCESS_MEMORY_READ_BIT : 0;
    beginBarriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    beginBarriers[1].oldLayout = mirror.initialized[mirrorImageIndex] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED;
    beginBarriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    beginBarriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    beginBarriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    beginBarriers[1].image = mirror.images[mirrorImageIndex];
    beginBarriers[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    dispatch.xr.cmdPipelineBarrier(mirror.commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, beginBarriers);

    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {static_cast<int32_t>(source.extent.width), static_cast<int32_t>(source.extent.height), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = {static_cast<int32_t>(mirror.extent.width), static_cast<int32_t>(mirror.extent.height), 1};
    dispatch.xr.cmdBlitImage(mirror.commandBuffer, source.images[sourceImageIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        mirror.images[mirrorImageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

    VkImageMemoryBarrier endBarriers[2]{};
    endBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    endBarriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    endBarriers[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    endBarriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    endBarriers[0].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    endBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    endBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    endBarriers[0].image = source.images[sourceImageIndex];
    endBarriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    endBarriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    endBarriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    endBarriers[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    endBarriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    endBarriers[1].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    endBarriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    endBarriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    endBarriers[1].image = mirror.images[mirrorImageIndex];
    endBarriers[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    dispatch.xr.cmdPipelineBarrier(mirror.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 2, endBarriers);
    if (dispatch.xr.endCommandBuffer(mirror.commandBuffer) != VK_SUCCESS) return;

    if (dispatch.resetFences(mirror.device, 1, &mirror.fence) != VK_SUCCESS) return;
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &mirror.imageAvailable;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &mirror.commandBuffer;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &mirror.copyDone;
    result = dispatch.xr.queueSubmit(queue, 1, &submit, mirror.fence);
    if (result != VK_SUCCESS) return;

    VkPresentInfoKHR mirrorPresent{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    mirrorPresent.waitSemaphoreCount = 1;
    mirrorPresent.pWaitSemaphores = &mirror.copyDone;
    mirrorPresent.swapchainCount = 1;
    mirrorPresent.pSwapchains = &mirror.swapchain;
    mirrorPresent.pImageIndices = &mirrorImageIndex;
    result = dispatch.present(queue, &mirrorPresent);
    mirror.initialized[mirrorImageIndex] = true;
    const uint64_t frame = ++mirror.frames;
    if (frame == 1) {
        logLine("Desktop Vulkan mirror frame=" + std::to_string(frame) + " result=" + std::to_string(result));
    }
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_ERROR_SURFACE_LOST_KHR) {
        logLine("Desktop Vulkan mirror disabled after present result=" + std::to_string(result));
        mirror.ready = false;
    }
}

#endif
template<class T> T findLinkChain(const void* pNext, VkStructureType type) {
    auto p = reinterpret_cast<const ChainBase*>(pNext);
    while (p) {
        if (p->sType == type) {
            auto candidate = reinterpret_cast<T>(const_cast<ChainBase*>(p));
            if (candidate->function == VK_LAYER_LINK_INFO) return candidate;
        }
        p = p->pNext;
    }
    return nullptr;
}
}

extern "C" {
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance, const char*);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice, const char*);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance, const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice*);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateWin32SurfaceKHR(VkInstance,const VkWin32SurfaceCreateInfoKHR*,const VkAllocationCallbacks*,VkSurfaceKHR*);
VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice, const VkAllocationCallbacks*);
VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice,uint32_t,uint32_t,VkQueue*);
VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue2(VkDevice,const VkDeviceQueueInfo2*,VkQueue*);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice,const VkSwapchainCreateInfoKHR*,const VkAllocationCallbacks*,VkSwapchainKHR*);
VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(VkDevice,VkSwapchainKHR,const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(VkDevice,VkSwapchainKHR,uint32_t*,VkImage*);
VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice,VkSwapchainKHR,uint64_t,VkSemaphore,VkFence,uint32_t*);
VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImage2KHR(VkDevice,const VkAcquireNextImageInfoKHR*,uint32_t*);
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue,uint32_t,const VkSubmitInfo*,VkFence);
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2(VkQueue,uint32_t,const VkSubmitInfo2*,VkFence);
VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue,const VkPresentInfoKHR*);
VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants(VkCommandBuffer,VkPipelineLayout,VkShaderStageFlags,uint32_t,uint32_t,const void*);
VKAPI_ATTR void VKAPI_CALL vkCmdUpdateBuffer(VkCommandBuffer,VkBuffer,VkDeviceSize,VkDeviceSize,const void*);
VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice,VkDeviceMemory,VkDeviceSize,VkDeviceSize,VkMemoryMapFlags,void**);
VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice,VkDeviceMemory);
VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice,VkBuffer,VkDeviceMemory,VkDeviceSize);
VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(VkDevice,uint32_t,const VkWriteDescriptorSet*,uint32_t,const VkCopyDescriptorSet*);
VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer,VkPipelineBindPoint,VkPipelineLayout,uint32_t,uint32_t,const VkDescriptorSet*,uint32_t,const uint32_t*);
VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer,VkPipelineBindPoint,VkPipeline);
VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers(VkCommandBuffer,uint32_t,uint32_t,const VkBuffer*,const VkDeviceSize*);
VKAPI_ATTR void VKAPI_CALL vkCmdBindIndexBuffer(VkCommandBuffer,VkBuffer,VkDeviceSize,VkIndexType);
VKAPI_ATTR void VKAPI_CALL vkCmdDraw(VkCommandBuffer,uint32_t,uint32_t,uint32_t,uint32_t);
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexed(VkCommandBuffer,uint32_t,uint32_t,uint32_t,int32_t,uint32_t);
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirect(VkCommandBuffer,VkBuffer,VkDeviceSize,uint32_t,uint32_t);
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexedIndirect(VkCommandBuffer,VkBuffer,VkDeviceSize,uint32_t,uint32_t);
VKAPI_ATTR void VKAPI_CALL vkCmdSetViewport(VkCommandBuffer,uint32_t,uint32_t,const VkViewport*);
VKAPI_ATTR void VKAPI_CALL vkCmdSetScissor(VkCommandBuffer,uint32_t,uint32_t,const VkRect2D*);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice,VkPipelineCache,uint32_t,const VkGraphicsPipelineCreateInfo*,const VkAllocationCallbacks*,VkPipeline*);
VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(VkDevice,VkPipeline,const VkAllocationCallbacks*);
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer,VkPipelineStageFlags,VkPipelineStageFlags,VkDependencyFlags,uint32_t,const VkMemoryBarrier*,uint32_t,const VkBufferMemoryBarrier*,uint32_t,const VkImageMemoryBarrier*);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice,const VkImageCreateInfo*,const VkAllocationCallbacks*,VkImage*);
VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice,VkImage,const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice,const VkImageViewCreateInfo*,const VkAllocationCallbacks*,VkImageView*);
VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice,VkImageView,const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateFramebuffer(VkDevice,const VkFramebufferCreateInfo*,const VkAllocationCallbacks*,VkFramebuffer*);
VKAPI_ATTR void VKAPI_CALL vkDestroyFramebuffer(VkDevice,VkFramebuffer,const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass(VkDevice,const VkRenderPassCreateInfo*,const VkAllocationCallbacks*,VkRenderPass*);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass2(VkDevice,const VkRenderPassCreateInfo2*,const VkAllocationCallbacks*,VkRenderPass*);
VKAPI_ATTR void VKAPI_CALL vkDestroyRenderPass(VkDevice,VkRenderPass,const VkAllocationCallbacks*);
VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass(VkCommandBuffer,const VkRenderPassBeginInfo*,VkSubpassContents);
VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass2(VkCommandBuffer,const VkRenderPassBeginInfo*,const VkSubpassBeginInfo*);
VKAPI_ATTR void VKAPI_CALL vkCmdNextSubpass(VkCommandBuffer,VkSubpassContents);
VKAPI_ATTR void VKAPI_CALL vkCmdNextSubpass2(VkCommandBuffer,const VkSubpassBeginInfo*,const VkSubpassEndInfo*);
VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer);
VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass2(VkCommandBuffer,const VkSubpassEndInfo*);

VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* v){
    if(!v||v->sType!=LAYER_NEGOTIATE_INTERFACE_STRUCT)return VK_ERROR_INITIALIZATION_FAILED;
    if(v->loaderLayerInterfaceVersion>2)v->loaderLayerInterfaceVersion=2;
    v->pfnGetInstanceProcAddr=vkGetInstanceProcAddr; v->pfnGetDeviceProcAddr=vkGetDeviceProcAddr; v->pfnGetPhysicalDeviceProcAddr=nullptr;
    if(!isDoomProcess())return VK_SUCCESS;
    // Engine detours and XInput imports outlive Vulkan instance destruction.
    // Retain their code until process exit, before installing any engine hook.
    HMODULE pinnedModule{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&vkNegotiateLoaderLayerInterfaceVersion), &pinnedModule)) {
        logLine("Layer initialization failed: cannot retain hook module", true);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    KharvoxCameraInstallDiagnosticHook();
    logLine("Layer loaded build=2026.09.04-native-frame-inputs-r139 runtimeDir=" + kharvox::runtimePathA("."), true);
    logLine("Loader negotiation successful"); return VK_SUCCESS;
}

[[noreturn]] static void stopVulkanStartup(const char* reason) {
    logLine(std::string("[WSI-STARTUP] controlled stop: ")+reason,true);
    MessageBoxA(nullptr,reason,"KHARVOX - Vulkan presentation unavailable",MB_OK|MB_ICONERROR);
    TerminateProcess(GetCurrentProcess(),0x4B480003);
    for(;;)Sleep(INFINITE);
}
static void ensureCoreSurface(const InstanceDispatch& dispatch,VkSurfaceKHR surface) {
    HWND window{};
    {std::lock_guard<std::mutex> lock(surfaceWindowMutex);
     const auto found=surfaceWindows.find(reinterpret_cast<uint64_t>(surface));
     if(found!=surfaceWindows.end())window=found->second;}
    if(window&&!kharvox::matchCoreSurfaceWindow(window,dispatch.sourceExtent))
        stopVulkanStartup("Cannot size the DOOM window to the requested VR render resolution.");
}

static VkResult independentCaps(const InstanceDispatch& dispatch,VkPhysicalDevice physical,
    VkSurfaceKHR surface,VkPresentModeKHR mode,VkSurfaceCapabilitiesKHR* caps,
    VkSurfacePresentScalingCapabilitiesEXT* scaling) {
    if(!dispatch.surfaceCaps2)return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkSurfacePresentModeEXT presentMode{VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_EXT};
    presentMode.presentMode=mode;
    VkPhysicalDeviceSurfaceInfo2KHR query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR};
    query.surface=surface;query.pNext=&presentMode;
    VkSurfaceCapabilities2KHR answer{VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR};
    answer.pNext=scaling;
    const auto result=dispatch.surfaceCaps2(physical,&query,&answer);
    if(result==VK_SUCCESS&&caps)*caps=answer.surfaceCapabilities;
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physical,VkSurfaceKHR surface,VkSurfaceCapabilitiesKHR* caps) {
    const auto dispatch=instanceState(key(physical));
    if(!dispatch.surfaceCaps)return VK_ERROR_EXTENSION_NOT_PRESENT;
    if(dispatch.coreSurface)ensureCoreSurface(dispatch,surface);
    const auto result=dispatch.surfaceCaps(physical,surface,caps);
    if(result==VK_SUCCESS&&caps&&dispatch.independentSurface&&isGameSurface(surface)) {
        VkSurfacePresentScalingCapabilitiesEXT scaling{VK_STRUCTURE_TYPE_SURFACE_PRESENT_SCALING_CAPABILITIES_EXT};
        const auto query=independentCaps(dispatch,physical,surface,VK_PRESENT_MODE_IMMEDIATE_KHR,nullptr,&scaling);
        if(query!=VK_SUCCESS)return query;
        if(!kharvox::supportsIndependentExtent(scaling,dispatch.sourceExtent))return VK_ERROR_FEATURE_NOT_PRESENT;
        kharvox::exposeIndependentExtent(*caps,dispatch.sourceExtent);
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice physical,const VkPhysicalDeviceSurfaceInfo2KHR* info,VkSurfaceCapabilities2KHR* caps) {
    const auto dispatch=instanceState(key(physical));
    if(!dispatch.surfaceCaps2)return VK_ERROR_EXTENSION_NOT_PRESENT;
    if(dispatch.coreSurface&&info)ensureCoreSurface(dispatch,info->surface);
    const auto result=dispatch.surfaceCaps2(physical,info,caps);
    if(result==VK_SUCCESS&&info&&caps&&dispatch.independentSurface&&isGameSurface(info->surface)) {
        const auto mode=kharvox::surfaceChain<VkSurfacePresentModeEXT>(info->pNext,VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_EXT);
        VkSurfacePresentScalingCapabilitiesEXT scaling{VK_STRUCTURE_TYPE_SURFACE_PRESENT_SCALING_CAPABILITIES_EXT};
        const auto query=independentCaps(dispatch,physical,info->surface,mode?mode->presentMode:VK_PRESENT_MODE_IMMEDIATE_KHR,nullptr,&scaling);
        if(query!=VK_SUCCESS)return query;
        if(!kharvox::supportsIndependentExtent(scaling,dispatch.sourceExtent))return VK_ERROR_FEATURE_NOT_PRESENT;
        kharvox::exposeIndependentExtent(caps->surfaceCapabilities,dispatch.sourceExtent);
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL vkDestroySurfaceKHR(VkInstance instance,VkSurfaceKHR surface,const VkAllocationCallbacks* allocator){
    const auto dispatch=instanceState(key(instance));
    {
        std::lock_guard<std::mutex> lock(surfaceWindowMutex);
        surfaceWindows.erase(reinterpret_cast<uint64_t>(surface));
    }
    if(dispatch.destroySurface)dispatch.destroySurface(instance,surface,allocator);
}

#define MATCH(name) if(!std::strcmp(n,#name)) return reinterpret_cast<PFN_vkVoidFunction>(name)
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance i,const char* n){
    if(!n)return nullptr;
    if(!isDoomProcess()) {
        MATCH(vkGetInstanceProcAddr); MATCH(vkGetDeviceProcAddr);
        MATCH(vkCreateInstance); MATCH(vkDestroyInstance); MATCH(vkCreateDevice);
        const auto dispatch=instanceState(key(i));
        return dispatch.gipa?dispatch.gipa(i,n):nullptr;
    }
    auto d=instanceState(key(i));if(i&&d.runtimeAuxiliary)return d.gipa?d.gipa(i,n):nullptr; MATCH(vkGetInstanceProcAddr); MATCH(vkGetDeviceProcAddr); MATCH(vkCreateInstance); MATCH(vkDestroyInstance); MATCH(vkCreateDevice); MATCH(vkCreateWin32SurfaceKHR);
    if(d.independentSurface||d.coreSurface){MATCH(vkGetPhysicalDeviceSurfaceCapabilitiesKHR); if(d.surfaceCaps2){MATCH(vkGetPhysicalDeviceSurfaceCapabilities2KHR);} MATCH(vkDestroySurfaceKHR);}
    return d.gipa?d.gipa(i,n):nullptr;
}
static PFN_vkVoidFunction deviceProcBase(VkDevice d,const char* n){
    if(!n)return nullptr;
    if(!isDoomProcess()) {
        MATCH(vkGetDeviceProcAddr); MATCH(vkDestroyDevice);
        const auto dispatch=deviceState(key(d));
        return dispatch.gdpa?dispatch.gdpa(d,n):nullptr;
    }
    auto s=deviceState(key(d));if(d&&s.runtimeAuxiliary)return s.gdpa?s.gdpa(d,n):nullptr; MATCH(vkGetDeviceProcAddr); MATCH(vkDestroyDevice); MATCH(vkGetDeviceQueue); MATCH(vkGetDeviceQueue2); MATCH(vkCreateSwapchainKHR); MATCH(vkDestroySwapchainKHR); MATCH(vkGetSwapchainImagesKHR); MATCH(vkAcquireNextImageKHR); MATCH(vkAcquireNextImage2KHR); MATCH(vkQueueSubmit); MATCH(vkQueueSubmit2); MATCH(vkQueuePresentKHR); MATCH(vkCmdPushConstants); MATCH(vkCmdUpdateBuffer); MATCH(vkMapMemory); MATCH(vkUnmapMemory); MATCH(vkBindBufferMemory); MATCH(vkUpdateDescriptorSets); MATCH(vkCmdBindDescriptorSets); MATCH(vkCmdBindPipeline); MATCH(vkCmdBindVertexBuffers); MATCH(vkCmdBindIndexBuffer); MATCH(vkCmdDraw); MATCH(vkCmdDrawIndexed); MATCH(vkCmdDrawIndirect); MATCH(vkCmdDrawIndexedIndirect); MATCH(vkCmdSetViewport); MATCH(vkCmdSetScissor); MATCH(vkCreateGraphicsPipelines); MATCH(vkDestroyPipeline); MATCH(vkCmdPipelineBarrier); MATCH(vkCreateImage); MATCH(vkDestroyImage); MATCH(vkCreateImageView); MATCH(vkDestroyImageView); MATCH(vkCreateFramebuffer); MATCH(vkDestroyFramebuffer); MATCH(vkCreateRenderPass); MATCH(vkCreateRenderPass2); MATCH(vkDestroyRenderPass); MATCH(vkCmdBeginRenderPass); MATCH(vkCmdBeginRenderPass2); MATCH(vkCmdNextSubpass); MATCH(vkCmdNextSubpass2); MATCH(vkCmdEndRenderPass); MATCH(vkCmdEndRenderPass2);
    return s.gdpa?s.gdpa(d,n):nullptr;
}
// These entry points sit OUTSIDE Native's wrappers so no Native bookkeeping
// lock is held while waiting for an XR copy to release a borrowed game image.
// Native's internal mirror destruction still calls deviceProcBase directly.
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice d,const char* n){
    auto next=deviceProcBase(d,n);
    if(!isDoomProcess()||!n||deviceState(key(d)).runtimeAuxiliary)return next;
    next=kharvox::native::wrapProc(n,next);
    if(next&&!std::strcmp(n,"vkDestroyImage"))return kharvox::GameImageRetirementEntry<VkImage>::wrap(next);
    if(next&&!std::strcmp(n,"vkDestroyImageView"))return kharvox::GameImageRetirementEntry<VkImageView>::wrap(next);
    return next;
}
#undef MATCH

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo* ci,const VkAllocationCallbacks* a,VkInstance* out){
    if(!ci||!out)return VK_ERROR_INITIALIZATION_FAILED;
    *out=VK_NULL_HANDLE;
    if(!isDoomProcess()) {
        auto chain=findLinkChain<VkLayerInstanceCreateInfo*>(ci->pNext,VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO);
        if(!chain||chain->function!=VK_LAYER_LINK_INFO||!chain->u.pLayerInfo)return VK_ERROR_INITIALIZATION_FAILED;
        auto link=chain->u.pLayerInfo;
        auto nextGipa=link->pfnNextGetInstanceProcAddr;
        chain->u.pLayerInfo=link->pNext;
        auto create=reinterpret_cast<PFN_vkCreateInstance>(nextGipa(nullptr,"vkCreateInstance"));
        const auto result=create?create(ci,a,out):VK_ERROR_INITIALIZATION_FAILED;
        if(result==VK_SUCCESS) {
            InstanceDispatch dispatch{};dispatch.gipa=nextGipa;dispatch.runtimeAuxiliary=true;
            dispatch.destroy=reinterpret_cast<PFN_vkDestroyInstance>(nextGipa(*out,"vkDestroyInstance"));
            dispatch.createDevice=reinterpret_cast<PFN_vkCreateDevice>(nextGipa(*out,"vkCreateDevice"));
            std::lock_guard<std::mutex> lock(stateMutex);instances[key(*out)]=dispatch;
        }
        return result;
    }
    InstanceCreateScope createScope;
    const char* applicationName=ci&&ci->pApplicationInfo&&ci->pApplicationInfo->pApplicationName
        ?ci->pApplicationInfo->pApplicationName:"<unnamed>";
    const auto runtimeKind=kharvox::classifyOpenXRRuntime(kharvox::activeOpenXRRuntimeManifest());
    const bool namedSteamRuntimeAuxiliary=std::strcmp(applicationName,"steamvr_vrclient_interop")==0;
    const bool steamRuntimeAuxiliary=kharvox::shouldPassthroughSteamRuntimeAuxiliary(
        runtimeKind,createScope.nested,applicationName);
    logLine(std::string("vkCreateInstance application=")+applicationName+
        " nested="+(createScope.nested?"yes":"no")+
        (steamRuntimeAuxiliary
            ?std::string(" Steam-backed runtime auxiliary passthrough trigger=")+
                (namedSteamRuntimeAuxiliary?"interop-name":"nested-create")
            :std::string()));
    auto chain=findLinkChain<VkLayerInstanceCreateInfo*>(ci->pNext,VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO);
    if(!chain||chain->function!=VK_LAYER_LINK_INFO||!chain->u.pLayerInfo)return VK_ERROR_INITIALIZATION_FAILED;
    auto link=chain->u.pLayerInfo; auto nextGipa=link->pfnNextGetInstanceProcAddr; chain->u.pLayerInfo=link->pNext;
    auto fn=reinterpret_cast<PFN_vkCreateInstance>(nextGipa(nullptr,"vkCreateInstance"));
    if(steamRuntimeAuxiliary){
        const VkResult auxiliaryResult=fn?fn(ci,a,out):VK_ERROR_INITIALIZATION_FAILED;
        if(auxiliaryResult==VK_SUCCESS){
            InstanceDispatch auxiliary{};auxiliary.gipa=nextGipa;auxiliary.runtimeAuxiliary=true;
            auxiliary.destroy=reinterpret_cast<PFN_vkDestroyInstance>(nextGipa(*out,"vkDestroyInstance"));
            auxiliary.createDevice=reinterpret_cast<PFN_vkCreateDevice>(nextGipa(*out,"vkCreateDevice"));
            {std::lock_guard<std::mutex>l(stateMutex);instances[key(*out)]=auxiliary;}
            logLine("Steam-backed auxiliary Vulkan instance created without KHARVOX/OpenXR recursion");
        }else logLine("Steam-backed auxiliary Vulkan instance passthrough failed result="+std::to_string(auxiliaryResult));
        return auxiliaryResult;
    }
    KharvoxXRInitialize(VK_NULL_HANDLE);auto required=KharvoxXRRequiredInstanceExtensions();std::vector<const char*> enabled;enabled.reserve(ci->enabledExtensionCount+required.size()+1);for(uint32_t i=0;i<ci->enabledExtensionCount;i++)enabled.push_back(ci->ppEnabledExtensionNames[i]);auto addExtension=[&](const char*name,const char*reason){for(auto*e:enabled)if(!std::strcmp(e,name))return;enabled.push_back(name);logLine(std::string("Enabling ")+reason+" instance extension "+name);};for(auto&name:required)addExtension(name.c_str(),"XR");
    // Runtime auxiliary instances returned above retain untouched WSI.
    bool independent=true;
    bool coreSurface=false;
    VkExtent2D sourceExtent{};
    if(independent){
        auto enumerate=reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(nextGipa(nullptr,"vkEnumerateInstanceExtensionProperties"));
        uint32_t count{};
        if(!enumerate||enumerate(nullptr,&count,nullptr)!=VK_SUCCESS)return VK_ERROR_EXTENSION_NOT_PRESENT;
        std::vector<VkExtensionProperties> extensions(count);
        if(enumerate(nullptr,&count,extensions.data())!=VK_SUCCESS)return VK_ERROR_EXTENSION_NOT_PRESENT;
        for(const char* extension:{VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME,VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME}){
            if(std::none_of(extensions.begin(),extensions.end(),[&](const auto& e){return !std::strcmp(e.extensionName,extension);})){
                logLine(std::string("[WSI-STARTUP] scaling unavailable: ")+extension+"; selecting core Win32 presentation with real render-sized window",true);
                independent=false;coreSurface=true;continue;
            }
            addExtension(extension,"independent presentation");
        }
        uint32_t eyeWidth{},eyeHeight{};
        if(!KharvoxXRRecommendedSourceSize(&eyeWidth,&eyeHeight)) {
            logLine("[XR-STARTUP] No usable headset view configuration; controlled startup stop exitCode=0x4B480002");
            // Returning Vulkan failure reaches DOOM's crashing fatal-error path.
            // As with the memory guard, the launcher owns cleanup and the dialog.
            TerminateProcess(GetCurrentProcess(),0x4B480002);
            for(;;)Sleep(INFINITE);
        }
        sourceExtent=kharvox::independentSourceExtent(eyeWidth,eyeHeight,configuredRenderScale(),INT_MAX);
        if(!sourceExtent.width)return VK_ERROR_INITIALIZATION_FAILED;
        if(!kharvox::installIndependentEngineSize(sourceExtent.width,sourceExtent.height)){logLine("[INDEPENDENT-SURFACE] supported engine size accessors could not be installed");return VK_ERROR_INITIALIZATION_FAILED;}
        logLine(std::string("[WSI-STARTUP] engine render size accessors armed; presentation=")+(coreSurface?"core-sized-window":"scaled-small-window"),true);
        logLine("[INDEPENDENT-SURFACE] headset recommendation="+std::to_string(eyeWidth)+"x"+std::to_string(eyeHeight)+" source="+std::to_string(sourceExtent.width)+"x"+std::to_string(sourceExtent.height)+" scale="+std::to_string(configuredRenderScale())+" desktopIndependent="+(coreSurface?"no; real window matches source":"yes"));
    }
    VkInstanceCreateInfo modified=*ci;    modified.enabledExtensionCount=uint32_t(enabled.size());modified.ppEnabledExtensionNames=enabled.data();VkResult r=VK_ERROR_INITIALIZATION_FAILED;VkResult mediatedVk=VK_ERROR_INITIALIZATION_FAILED;if(KharvoxXRCreateVulkanInstance(nextGipa,&modified,a,out,&mediatedVk))r=mediatedVk;else r=fn?fn(&modified,a,out):VK_ERROR_INITIALIZATION_FAILED;
    if(r==VK_SUCCESS){InstanceDispatch d{};d.independentSurface=independent;d.coreSurface=coreSurface;d.sourceExtent=sourceExtent;d.surfaceCaps=reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(nextGipa(*out,"vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));d.surfaceCaps2=reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR>(nextGipa(*out,"vkGetPhysicalDeviceSurfaceCapabilities2KHR"));
        d.destroySurface=reinterpret_cast<PFN_vkDestroySurfaceKHR>(nextGipa(*out,"vkDestroySurfaceKHR"));
        d.instance=*out;d.gipa=nextGipa;d.destroy=reinterpret_cast<PFN_vkDestroyInstance>(nextGipa(*out,"vkDestroyInstance"));d.createDevice=reinterpret_cast<PFN_vkCreateDevice>(nextGipa(*out,"vkCreateDevice"));d.createWin32Surface=reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(nextGipa(*out,"vkCreateWin32SurfaceKHR"));d.getPhysicalDeviceProperties=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(nextGipa(*out,"vkGetPhysicalDeviceProperties"));d.enumerateDeviceExtensionProperties=reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(nextGipa(*out,"vkEnumerateDeviceExtensionProperties"));d.getPhysicalDeviceFeatures2=reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(nextGipa(*out,"vkGetPhysicalDeviceFeatures2"));d.getPhysicalDeviceQueueFamilyProperties=reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(nextGipa(*out,"vkGetPhysicalDeviceQueueFamilyProperties"));{std::lock_guard<std::mutex>l(stateMutex);instances[key(*out)]=d;}logLine("[VK-STARTUP] instance created handle="+std::to_string(reinterpret_cast<uintptr_t>(*out))+" nextGIPA="+std::to_string(reinterpret_cast<uintptr_t>(nextGipa)),true);logLine("Instance created");KharvoxXRInitialize(*out);}else {logLine("[VK-STARTUP] instance creation failed result="+std::to_string(r),true);stopVulkanStartup("Vulkan/OpenXR instance initialization failed. See the KHARVOX log in %TEMP%.");} return r;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance i,const VkAllocationCallbacks* a){auto k=key(i);auto d=instanceState(k);if(isDoomProcess())logLine("vkDestroyInstance");if(d.destroy)d.destroy(i,a);std::lock_guard<std::mutex>l(stateMutex);instances.erase(k);}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateWin32SurfaceKHR(VkInstance instance,const VkWin32SurfaceCreateInfoKHR* info,const VkAllocationCallbacks* allocator,VkSurfaceKHR* surface){
    KharvoxXRStartSessionIfReady();
    auto dispatch=instanceState(key(instance));
    if(!dispatch.createWin32Surface)return VK_ERROR_EXTENSION_NOT_PRESENT;
    if(extendedLoggingEnabled()&&info)logExtended(windowSnapshot(info->hwnd,"surface-create-entry"));
    if(dispatch.coreSurface&&info&&!kharvox::matchCoreSurfaceWindow(info->hwnd,dispatch.sourceExtent))stopVulkanStartup("Cannot size the DOOM window to the requested VR render resolution.");
    LARGE_INTEGER begin{},end{};QueryPerformanceCounter(&begin);
    const VkResult result=dispatch.createWin32Surface(instance,info,allocator,surface);
    QueryPerformanceCounter(&end);
    if(result==VK_SUCCESS&&surface&&info){
        std::lock_guard<std::mutex> lock(surfaceWindowMutex);
        surfaceWindows[reinterpret_cast<uint64_t>(*surface)]=info->hwnd;
    }
    if(extendedLoggingEnabled()){
        std::ostringstream detail;
        detail<<"[WSI] vkCreateWin32SurfaceKHR return result="<<result
              <<" surface="<<(surface?reinterpret_cast<uint64_t>(*surface):0)
              <<" thread="<<GetCurrentThreadId()<<" downstreamMs="<<elapsedMilliseconds(begin,end);
        logExtended(detail.str());
        if(info)logExtended(windowSnapshot(info->hwnd,"surface-create-return"));
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice p,const VkDeviceCreateInfo* ci,const VkAllocationCallbacks* a,VkDevice* out){
    if(!ci||!out||!p)return VK_ERROR_INITIALIZATION_FAILED;
    *out=VK_NULL_HANDLE;
    if(isDoomProcess())logLine("vkCreateDevice"); auto chain=findLinkChain<VkLayerDeviceCreateInfo*>(ci->pNext,VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO);
    if(!chain||chain->function!=VK_LAYER_LINK_INFO||!chain->u.pLayerInfo)return VK_ERROR_INITIALIZATION_FAILED;
    auto link=chain->u.pLayerInfo;auto nextGipa=link->pfnNextGetInstanceProcAddr;auto nextGdpa=link->pfnNextGetDeviceProcAddr;chain->u.pLayerInfo=link->pNext;
    auto physicalDispatch=instanceState(key(p));
    auto fn=kharvox::resolveLayerCreateDevice(nextGipa,physicalDispatch.instance);if(!fn)fn=physicalDispatch.createDevice;
    if(physicalDispatch.runtimeAuxiliary){
        const VkResult auxiliaryResult=fn?fn(p,ci,a,out):VK_ERROR_INITIALIZATION_FAILED;
        if(auxiliaryResult==VK_SUCCESS){
            DeviceDispatch auxiliary{};auxiliary.gdpa=nextGdpa;auxiliary.runtimeAuxiliary=true;
            auxiliary.destroy=reinterpret_cast<PFN_vkDestroyDevice>(nextGdpa(*out,"vkDestroyDevice"));
            {std::lock_guard<std::mutex>l(stateMutex);devices[key(*out)]=auxiliary;}
            if(isDoomProcess())logLine("Steam-backed auxiliary Vulkan device created without KHARVOX/OpenXR hooks");
        }else if(isDoomProcess())logLine("Steam-backed auxiliary Vulkan device passthrough failed result="+std::to_string(auxiliaryResult));
        return auxiliaryResult;
    }
    if(KharvoxXRMediationReentry()){auto mapped=KharvoxXRMappedPhysicalForReentry();auto create=KharvoxXRCreateDeviceForReentry();logLine("XR internal vkCreateDevice reentry -> original next layer with mapped physical");return create?create(mapped?mapped:p,ci,a,out):VK_ERROR_INITIALIZATION_FAILED;}
    auto required=KharvoxXRRequiredDeviceExtensions();
    logLine("vkCreateDevice: XR extension query complete");
    std::vector<const char*> enabled;
    enabled.reserve(ci->enabledExtensionCount+required.size()+3);
    for(uint32_t i=0;i<ci->enabledExtensionCount;i++)enabled.push_back(ci->ppEnabledExtensionNames[i]);
    auto addExtension=[&](const char* name,const char* reason){
        for(auto* existing:enabled)if(!std::strcmp(existing,name))return;
        enabled.push_back(name);logLine(std::string("Enabling ")+reason+" device extension "+name);
    };
    for(auto&name:required)addExtension(name.c_str(),"XR");

    VkDeviceCreateInfo modified=*ci;
    const void* applicationDeviceNext=ci->pNext;
    while(applicationDeviceNext&&reinterpret_cast<const ChainBase*>(applicationDeviceNext)->sType==VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO)
        applicationDeviceNext=reinterpret_cast<const ChainBase*>(applicationDeviceNext)->pNext;
    modified.enabledExtensionCount=uint32_t(enabled.size());modified.ppEnabledExtensionNames=enabled.data();
    VkDeviceCreateInfo runtimeModified=modified;
    runtimeModified.pNext=applicationDeviceNext;
    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT runtimeMaintenance{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
    if(physicalDispatch.independentSurface){
        uint32_t count{};
        if(!physicalDispatch.enumerateDeviceExtensionProperties||physicalDispatch.enumerateDeviceExtensionProperties(p,nullptr,&count,nullptr)!=VK_SUCCESS)return VK_ERROR_EXTENSION_NOT_PRESENT;
        std::vector<VkExtensionProperties> extensions(count);
        if(physicalDispatch.enumerateDeviceExtensionProperties(p,nullptr,&count,extensions.data())!=VK_SUCCESS)return VK_ERROR_EXTENSION_NOT_PRESENT;
        const bool hasMaintenance=std::any_of(extensions.begin(),extensions.end(),[](const auto& e){return !std::strcmp(e.extensionName,VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);});
        auto features2=physicalDispatch.getPhysicalDeviceFeatures2;
        if(!features2)features2=reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(physicalDispatch.gipa(physicalDispatch.instance,"vkGetPhysicalDeviceFeatures2KHR"));
        if(hasMaintenance&&features2){VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};features.pNext=&maintenance;features2(p,&features);}
        if(!hasMaintenance||!maintenance.swapchainMaintenance1){
            physicalDispatch.independentSurface=false;physicalDispatch.coreSurface=true;
            {std::lock_guard<std::mutex> lock(stateMutex);instances[key(p)]=physicalDispatch;}
            logLine("[WSI-STARTUP] device present scaling unavailable; using real render-sized Win32 surface",true);
        }
        if(physicalDispatch.independentSurface){
            addExtension(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,"independent presentation");
            modified.enabledExtensionCount=uint32_t(enabled.size());modified.ppEnabledExtensionNames=enabled.data();
            runtimeModified.enabledExtensionCount=uint32_t(enabled.size());runtimeModified.ppEnabledExtensionNames=enabled.data();
            const auto existing=kharvox::surfaceChain<VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT>(ci->pNext,VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT);
            if(existing&&!existing->swapchainMaintenance1)return VK_ERROR_FEATURE_NOT_PRESENT;
            if(!existing){
                maintenance.pNext=const_cast<void*>(modified.pNext);modified.pNext=&maintenance;
                runtimeMaintenance.swapchainMaintenance1=VK_TRUE;
                runtimeMaintenance.pNext=const_cast<void*>(runtimeModified.pNext);runtimeModified.pNext=&runtimeMaintenance;
            }
        }
    }
    VkPhysicalDeviceProperties startupProperties{};
    if(!physicalDispatch.getPhysicalDeviceProperties)return VK_ERROR_INITIALIZATION_FAILED;
    physicalDispatch.getPhysicalDeviceProperties(p,&startupProperties);
    logLine(std::string("[VK-STARTUP] DOOM device=")+startupProperties.deviceName+
        " vendor="+std::to_string(startupProperties.vendorID)+" deviceId="+std::to_string(startupProperties.deviceID)+
        " instance="+std::to_string(reinterpret_cast<uintptr_t>(physicalDispatch.instance))+
        " layeredPhysical="+std::to_string(reinterpret_cast<uintptr_t>(p))+
        " createDevice="+std::to_string(reinterpret_cast<uintptr_t>(fn))+
        " presentation="+(physicalDispatch.coreSurface?"core-sized-window":"scaled-window"),true);
    if(physicalDispatch.sourceExtent.width>startupProperties.limits.maxImageDimension2D ||
       physicalDispatch.sourceExtent.height>startupProperties.limits.maxImageDimension2D)
        stopVulkanStartup("Render Scale is too high for this GPU's maximum image size.");
    KharvoxXRPreparePhysicalDeviceBinding(p);logLine("Preserving layered physical device for downstream vkCreateDevice; XR binding keeps runtime physical");
    VkResult r=VK_ERROR_INITIALIZATION_FAILED;VkResult mediatedVk=VK_ERROR_INITIALIZATION_FAILED;if(KharvoxXRCreateVulkanDevice(nextGipa,p,&runtimeModified,&modified,a,out,&mediatedVk))r=mediatedVk;else r=fn?fn(p,&modified,a,out):VK_ERROR_INITIALIZATION_FAILED;
    if(r==VK_SUCCESS){DeviceDispatch d{};d.gdpa=nextGdpa;d.physical=p;d.independentSurface=physicalDispatch.independentSurface;d.coreSurface=physicalDispatch.coreSurface;
      d.xr.getDeviceProcAddr=nextGdpa;
#define LOAD(field,name) d.field=reinterpret_cast<decltype(d.field)>(kharvox::native::trace::wrap(#name,nextGdpa(*out,#name),true))
      LOAD(destroy,vkDestroyDevice);LOAD(getQueue,vkGetDeviceQueue);LOAD(getQueue2,vkGetDeviceQueue2);LOAD(createSwapchain,vkCreateSwapchainKHR);LOAD(destroySwapchain,vkDestroySwapchainKHR);LOAD(getSwapchainImages,vkGetSwapchainImagesKHR);LOAD(acquire,vkAcquireNextImageKHR);LOAD(acquire2,vkAcquireNextImage2KHR);LOAD(submit,vkQueueSubmit);LOAD(submit2,vkQueueSubmit2);LOAD(present,vkQueuePresentKHR);
      LOAD(cmdPushConstants,vkCmdPushConstants);LOAD(cmdUpdateBuffer,vkCmdUpdateBuffer);
      LOAD(mapMemory,vkMapMemory);LOAD(unmapMemory,vkUnmapMemory);LOAD(bindBufferMemory,vkBindBufferMemory);LOAD(updateDescriptorSets,vkUpdateDescriptorSets);LOAD(cmdBindDescriptorSets,vkCmdBindDescriptorSets);LOAD(cmdBindPipeline,vkCmdBindPipeline);LOAD(cmdBindVertexBuffers,vkCmdBindVertexBuffers);LOAD(cmdBindIndexBuffer,vkCmdBindIndexBuffer);LOAD(cmdDraw,vkCmdDraw);LOAD(cmdDrawIndexed,vkCmdDrawIndexed);LOAD(cmdDrawIndirect,vkCmdDrawIndirect);LOAD(cmdDrawIndexedIndirect,vkCmdDrawIndexedIndirect);LOAD(cmdSetViewport,vkCmdSetViewport);LOAD(cmdSetScissor,vkCmdSetScissor);LOAD(createGraphicsPipelines,vkCreateGraphicsPipelines);LOAD(destroyPipeline,vkDestroyPipeline);LOAD(createImage,vkCreateImage);LOAD(destroyImage,vkDestroyImage);LOAD(createImageView,vkCreateImageView);LOAD(destroyImageView,vkDestroyImageView);LOAD(createFramebuffer,vkCreateFramebuffer);LOAD(destroyFramebuffer,vkDestroyFramebuffer);LOAD(createRenderPass,vkCreateRenderPass);LOAD(createRenderPass2,vkCreateRenderPass2);LOAD(destroyRenderPass,vkDestroyRenderPass);LOAD(cmdBeginRenderPass,vkCmdBeginRenderPass);LOAD(cmdBeginRenderPass2,vkCmdBeginRenderPass2);LOAD(cmdNextSubpass,vkCmdNextSubpass);LOAD(cmdNextSubpass2,vkCmdNextSubpass2);LOAD(cmdEndRenderPass,vkCmdEndRenderPass);LOAD(cmdEndRenderPass2,vkCmdEndRenderPass2);
      LOAD(xr.createCommandPool,vkCreateCommandPool);LOAD(xr.destroyCommandPool,vkDestroyCommandPool);LOAD(xr.allocateCommandBuffers,vkAllocateCommandBuffers);LOAD(xr.resetCommandBuffer,vkResetCommandBuffer);LOAD(xr.beginCommandBuffer,vkBeginCommandBuffer);LOAD(xr.endCommandBuffer,vkEndCommandBuffer);LOAD(xr.cmdPipelineBarrier,vkCmdPipelineBarrier);LOAD(xr.cmdBlitImage,vkCmdBlitImage);LOAD(xr.cmdCopyImage,vkCmdCopyImage);LOAD(xr.cmdCopyBufferToImage,vkCmdCopyBufferToImage);LOAD(xr.cmdClearColorImage,vkCmdClearColorImage);LOAD(xr.createImage,vkCreateImage);LOAD(xr.destroyImage,vkDestroyImage);LOAD(xr.getImageMemoryRequirements,vkGetImageMemoryRequirements);LOAD(xr.allocateMemory,vkAllocateMemory);LOAD(xr.freeMemory,vkFreeMemory);LOAD(xr.bindImageMemory,vkBindImageMemory);LOAD(xr.createBuffer,vkCreateBuffer);LOAD(xr.destroyBuffer,vkDestroyBuffer);LOAD(xr.getBufferMemoryRequirements,vkGetBufferMemoryRequirements);LOAD(xr.bindBufferMemory,vkBindBufferMemory);LOAD(xr.mapMemory,vkMapMemory);LOAD(xr.unmapMemory,vkUnmapMemory);
      LOAD(xr.createImageView,vkCreateImageView);LOAD(xr.destroyImageView,vkDestroyImageView);LOAD(xr.createSampler,vkCreateSampler);LOAD(xr.destroySampler,vkDestroySampler);LOAD(xr.createShaderModule,vkCreateShaderModule);LOAD(xr.destroyShaderModule,vkDestroyShaderModule);LOAD(xr.createDescriptorSetLayout,vkCreateDescriptorSetLayout);LOAD(xr.destroyDescriptorSetLayout,vkDestroyDescriptorSetLayout);LOAD(xr.createDescriptorPool,vkCreateDescriptorPool);LOAD(xr.destroyDescriptorPool,vkDestroyDescriptorPool);LOAD(xr.allocateDescriptorSets,vkAllocateDescriptorSets);LOAD(xr.updateDescriptorSets,vkUpdateDescriptorSets);LOAD(xr.createPipelineLayout,vkCreatePipelineLayout);LOAD(xr.destroyPipelineLayout,vkDestroyPipelineLayout);LOAD(xr.createComputePipelines,vkCreateComputePipelines);LOAD(xr.createGraphicsPipelines,vkCreateGraphicsPipelines);LOAD(xr.destroyPipeline,vkDestroyPipeline);LOAD(xr.cmdBindPipeline,vkCmdBindPipeline);LOAD(xr.cmdBindDescriptorSets,vkCmdBindDescriptorSets);LOAD(xr.cmdPushConstants,vkCmdPushConstants);LOAD(xr.cmdDispatch,vkCmdDispatch);LOAD(xr.createRenderPass,vkCreateRenderPass);LOAD(xr.destroyRenderPass,vkDestroyRenderPass);LOAD(xr.createFramebuffer,vkCreateFramebuffer);LOAD(xr.destroyFramebuffer,vkDestroyFramebuffer);LOAD(xr.cmdBeginRenderPass,vkCmdBeginRenderPass);LOAD(xr.cmdEndRenderPass,vkCmdEndRenderPass);LOAD(xr.cmdBindVertexBuffers,vkCmdBindVertexBuffers);LOAD(xr.cmdBindIndexBuffer,vkCmdBindIndexBuffer);LOAD(xr.cmdDrawIndexed,vkCmdDrawIndexed);LOAD(xr.cmdSetViewport,vkCmdSetViewport);LOAD(xr.cmdSetScissor,vkCmdSetScissor);LOAD(xr.createSemaphore,vkCreateSemaphore);LOAD(xr.destroySemaphore,vkDestroySemaphore);LOAD(xr.createFence,vkCreateFence);LOAD(xr.destroyFence,vkDestroyFence);LOAD(xr.resetFences,vkResetFences);LOAD(xr.waitForFences,vkWaitForFences);LOAD(xr.queueSubmit,vkQueueSubmit);LOAD(xr.queueWaitIdle,vkQueueWaitIdle);
      {auto vulkan=GetModuleHandleW(L"vulkan-1.dll");d.xr.getPhysicalDeviceMemoryProperties=reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(vulkan?GetProcAddress(vulkan,"vkGetPhysicalDeviceMemoryProperties"):nullptr);d.xr.getPhysicalDeviceProperties=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(vulkan?GetProcAddress(vulkan,"vkGetPhysicalDeviceProperties"):nullptr);d.xr.getPhysicalDeviceQueueFamilyProperties=reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(vulkan?GetProcAddress(vulkan,"vkGetPhysicalDeviceQueueFamilyProperties"):nullptr);}
#undef LOAD
      {std::lock_guard<std::mutex>l(stateMutex);devices[key(*out)]=d;}logLine("Device created");KharvoxXRSetQueueAccessCallbacks(lockQueueAccess,unlockQueueAccess);KharvoxXRSetDevice(p,*out,d.xr);kharvox::native::setQueueAccessCallbacks(lockQueueAccess,unlockQueueAccess);kharvox::native::setDevice(physicalDispatch.instance,p,*out,nextGdpa,nextGipa);}else {logLine("[VK-STARTUP] device creation failed result="+std::to_string(r),true);stopVulkanStartup("Vulkan/OpenXR device initialization failed. Ensure DOOM and the VR runtime use the same GPU. See the KHARVOX log in %TEMP%.");}return r;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice d,const VkAllocationCallbacks*a){auto k=key(d);auto s=deviceState(k);if(!s.runtimeAuxiliary){logLine("vkDestroyDevice");kharvox::native::beforeDeviceDestroy(d);KharvoxXRDeviceDestroyed();kharvox::hands::handSceneDeviceDestroyed();kharvox::hudgpu::deviceDestroyed();}if(s.destroy)s.destroy(d,a);std::lock_guard<std::mutex>l(stateMutex);devices.erase(k);}
VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice d,uint32_t f,uint32_t q,VkQueue*out){auto s=deviceState(key(d));if(out)*out=VK_NULL_HANDLE;if(s.getQueue&&out)s.getQueue(d,f,q,out);std::ostringstream x;x<<"Queue acquired family="<<f<<" index="<<q;logLine(x.str());if(out&&*out){KharvoxXRSetQueue(*out,f,q);kharvox::native::setQueue(*out,f,q);}}
VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue2(VkDevice d,const VkDeviceQueueInfo2*i,VkQueue*out){auto s=deviceState(key(d));if(out)*out=VK_NULL_HANDLE;if(s.getQueue2&&out&&i)s.getQueue2(d,i,out);std::ostringstream x;x<<"Queue acquired family="<<(i?i->queueFamilyIndex:0)<<" index="<<(i?i->queueIndex:0);logLine(x.str());if(out&&*out&&i){KharvoxXRSetQueue(*out,i->queueFamilyIndex,i->queueIndex);kharvox::native::setQueue(*out,i->queueFamilyIndex,i->queueIndex);}}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice d,const VkSwapchainCreateInfoKHR*i,const VkAllocationCallbacks*a,VkSwapchainKHR*out){
    auto s=deviceState(key(d));
    if(!s.createSwapchain)return VK_ERROR_EXTENSION_NOT_PRESENT;
    if(!i)return VK_ERROR_INITIALIZATION_FAILED;

    // Kharvox copies the completed game image into the OpenXR eye swapchains.
    // Alternating stereo also writes the stable left-eye cache back into the
    // desktop swapchain before it is presented.  Both operations require
    // explicit WSI usage bits; relying on Steam's overlay layer to add
    // TRANSFER_SRC made the isolated SteamVR path invalid and could produce
    // VK_ERROR_DEVICE_LOST/black output.
    VkSwapchainCreateInfoKHR effective=*i;
    VkSwapchainPresentScalingCreateInfoEXT scalingInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_SCALING_CREATE_INFO_EXT};
    const bool independent=s.independentSurface&&isGameSurface(i->surface);
    if(independent){
        const auto instance=instanceState(key(s.physical));
        VkSurfacePresentScalingCapabilitiesEXT scaling{VK_STRUCTURE_TYPE_SURFACE_PRESENT_SCALING_CAPABILITIES_EXT};
        const auto query=independentCaps(instance,s.physical,i->surface,i->presentMode,nullptr,&scaling);
        if(query!=VK_SUCCESS)return query;
        if(!kharvox::supportsIndependentExtent(scaling,instance.sourceExtent)){logLine("[INDEPENDENT-SURFACE] requested present mode cannot scale this source extent");return VK_ERROR_FEATURE_NOT_PRESENT;}
        // Never silently resize images behind DOOM's framebuffers/viewports.
        if(i->imageExtent.width!=instance.sourceExtent.width||i->imageExtent.height!=instance.sourceExtent.height){logLine("[INDEPENDENT-SURFACE] DOOM requested="+std::to_string(i->imageExtent.width)+"x"+std::to_string(i->imageExtent.height)+" expected="+std::to_string(instance.sourceExtent.width)+"x"+std::to_string(instance.sourceExtent.height)+"; refusing mismatched images");return VK_ERROR_INITIALIZATION_FAILED;}
        if(kharvox::surfaceChain<VkSwapchainPresentScalingCreateInfoEXT>(i->pNext,VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_SCALING_CREATE_INFO_EXT))return VK_ERROR_INITIALIZATION_FAILED;
        scalingInfo.scalingBehavior=VK_PRESENT_SCALING_STRETCH_BIT_EXT;
        scalingInfo.pNext=effective.pNext;effective.pNext=&scalingInfo;
        logLine("[INDEPENDENT-SURFACE] Vulkan scales desktop presentation only; XR source="+std::to_string(i->imageExtent.width)+"x"+std::to_string(i->imageExtent.height));
    }
    if(s.coreSurface&&isGameSurface(i->surface)) {
        const auto instance=instanceState(key(s.physical));
        ensureCoreSurface(instance,i->surface);
        VkSurfaceCapabilitiesKHR caps{};
        if(!instance.surfaceCaps||instance.surfaceCaps(s.physical,i->surface,&caps)!=VK_SUCCESS ||
           caps.currentExtent.width!=i->imageExtent.width || caps.currentExtent.height!=i->imageExtent.height ||
           i->imageExtent.width!=instance.sourceExtent.width || i->imageExtent.height!=instance.sourceExtent.height)
            stopVulkanStartup("The Vulkan surface does not match the VR render resolution. Try a lower Render Scale.");
        logLine("[WSI-STARTUP] core Win32 extent verified; no present scaling structure; source="+
            std::to_string(i->imageExtent.width)+"x"+std::to_string(i->imageExtent.height),true);
    }
    effective.imageUsage|=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const auto sequence=++swapchainCreateCount;
    HWND surfaceWindow{};
    {
        std::lock_guard<std::mutex> lock(surfaceWindowMutex);
        const auto found=surfaceWindows.find(reinterpret_cast<uint64_t>(i->surface));
        if(found!=surfaceWindows.end())surfaceWindow=found->second;
    }
    if(extendedLoggingEnabled()){
        std::ostringstream entry;
        entry<<"[WSI] vkCreateSwapchainKHR entry call="<<sequence
             <<" thread="<<GetCurrentThreadId()
             <<" device="<<reinterpret_cast<uint64_t>(d)
             <<" surface="<<reinterpret_cast<uint64_t>(i->surface)
             <<" oldSwapchain="<<reinterpret_cast<uint64_t>(i->oldSwapchain)
             <<" extent="<<i->imageExtent.width<<'x'<<i->imageExtent.height
             <<" format="<<i->imageFormat<<" colorSpace="<<i->imageColorSpace
             <<" minImages="<<i->minImageCount<<" presentMode="<<i->presentMode
             <<" preTransform=0x"<<std::hex<<i->preTransform
             <<" compositeAlpha=0x"<<i->compositeAlpha
             <<" requestedUsage=0x"<<i->imageUsage
             <<" effectiveUsage=0x"<<effective.imageUsage<<std::dec
             <<" clipped="<<i->clipped<<" queueFamilies="<<i->queueFamilyIndexCount;
        logExtended(entry.str());
        logExtended(windowSnapshot(surfaceWindow,"swapchain-create-entry"));
    }
    std::ostringstream x;
    x<<"Swapchain created request resolution="<<i->imageExtent.width<<'x'<<i->imageExtent.height
     <<" format="<<i->imageFormat<<" colorSpace="<<i->imageColorSpace<<" minImages="<<i->minImageCount
     <<" presentMode="<<i->presentMode<<" requestedUsage=0x"<<std::hex<<i->imageUsage
     <<" effectiveUsage=0x"<<effective.imageUsage;
    logLine(x.str());
    LARGE_INTEGER begin{},end{};QueryPerformanceCounter(&begin);
    auto r=s.createSwapchain(d,&effective,a,out);
    if(r==VK_SUCCESS&&independent){std::lock_guard<std::mutex> lock(independentSwapchainMutex);independentSwapchains.insert(reinterpret_cast<uint64_t>(*out));}
    QueryPerformanceCounter(&end);
    if(r==VK_SUCCESS){KharvoxXRSwapchainCreated(*out,effective);kharvox::hudgpu::swapchainCreated(*out,effective);KharvoxXRStartSessionIfReady();}
    else logLine("Swapchain creation with KHARVOX transfer usage failed result="+std::to_string(r));
    if(extendedLoggingEnabled()){
        std::ostringstream returned;
        returned<<"[WSI] vkCreateSwapchainKHR return call="<<sequence
                <<" thread="<<GetCurrentThreadId()<<" result="<<r
                <<" swapchain="<<(out?reinterpret_cast<uint64_t>(*out):0)
                <<" downstreamMs="<<elapsedMilliseconds(begin,end);
        logExtended(returned.str());
        logExtended(windowSnapshot(surfaceWindow,"swapchain-create-return"));
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(VkDevice d,VkSwapchainKHR sc,const VkAllocationCallbacks*a){
    {std::lock_guard<std::mutex> lock(independentSwapchainMutex);independentSwapchains.erase(reinterpret_cast<uint64_t>(sc));}
    auto s=deviceState(key(d));const auto sequence=++swapchainDestroyCount;logLine("Swapchain destroyed");
    LARGE_INTEGER begin{},afterKharvox{},end{};QueryPerformanceCounter(&begin);
    if(extendedLoggingEnabled()){std::ostringstream entry;entry<<"[WSI] vkDestroySwapchainKHR entry call="<<sequence<<" thread="<<GetCurrentThreadId()<<" device="<<reinterpret_cast<uint64_t>(d)<<" swapchain="<<reinterpret_cast<uint64_t>(sc);logExtended(entry.str());}
    kharvox::hudgpu::swapchainDestroyed(sc);KharvoxXRSwapchainDestroyed(sc);QueryPerformanceCounter(&afterKharvox);
    if(s.destroySwapchain)s.destroySwapchain(d,sc,a);QueryPerformanceCounter(&end);
    if(extendedLoggingEnabled()){std::ostringstream returned;returned<<"[WSI] vkDestroySwapchainKHR return call="<<sequence<<" thread="<<GetCurrentThreadId()<<" kharvoxMs="<<elapsedMilliseconds(begin,afterKharvox)<<" downstreamMs="<<elapsedMilliseconds(afterKharvox,end)<<" totalMs="<<elapsedMilliseconds(begin,end);logExtended(returned.str());}
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(VkDevice d,VkSwapchainKHR sc,uint32_t*c,VkImage*i){auto s=deviceState(key(d));auto r=s.getSwapchainImages?s.getSwapchainImages(d,sc,c,i):VK_ERROR_EXTENSION_NOT_PRESENT;if(r==VK_SUCCESS&&c&&i){std::ostringstream x;x<<"Swapchain images="<<*c;logLine(x.str());kharvox::hudgpu::swapchainImages(sc,*c,i);KharvoxXRSwapchainImages(sc,*c,i);}return r;}
VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice d,VkSwapchainKHR sc,uint64_t t,VkSemaphore sem,VkFence f,uint32_t*i){
    KharvoxCameraFinishNativeStereoBootstrap();auto s=deviceState(key(d));const auto n=++acquireCount;LARGE_INTEGER enter{},downstreamDone{},prepareDone{};QueryPerformanceCounter(&enter);
    if(extendedLoggingEnabled()&&traceDiagnosticCall(n)){std::ostringstream x;x<<"[ACQUIRE] entry call="<<n<<" thread="<<GetCurrentThreadId()<<" device="<<reinterpret_cast<uint64_t>(d)<<" swapchain="<<reinterpret_cast<uint64_t>(sc)<<" timeoutNs="<<t<<" semaphore="<<reinterpret_cast<uint64_t>(sem)<<" fence="<<reinterpret_cast<uint64_t>(f);logExtended(x.str());}
    auto r=s.acquire?s.acquire(d,sc,t,sem,f,i):VK_ERROR_EXTENSION_NOT_PRESENT;QueryPerformanceCounter(&downstreamDone);
    if(logFrame(n)){std::ostringstream x;x<<"[Frame "<<n<<"] Acquire image="<<(i?*i:~0u)<<" result="<<r;logLine(x.str());}
    if(r==VK_SUCCESS||r==VK_SUBOPTIMAL_KHR)KharvoxXRPrepareFrame(sc);QueryPerformanceCounter(&prepareDone);
    const double downstreamMs=elapsedMilliseconds(enter,downstreamDone),prepareMs=elapsedMilliseconds(downstreamDone,prepareDone);
    if(extendedLoggingEnabled()&&(traceDiagnosticCall(n)||r!=VK_SUCCESS||downstreamMs>=10.0||prepareMs>=10.0)){std::ostringstream x;x<<"[ACQUIRE] return call="<<n<<" thread="<<GetCurrentThreadId()<<" result="<<r<<" image="<<(i?*i:~0u)<<" downstreamMs="<<downstreamMs<<" xrPrepareMs="<<prepareMs<<" totalMs="<<elapsedMilliseconds(enter,prepareDone);logExtended(x.str());}
    return kharvox::independentSurfaceResult(r,isIndependentSwapchain(sc));
}
VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImage2KHR(VkDevice d,const VkAcquireNextImageInfoKHR*i,uint32_t*out){
    KharvoxCameraFinishNativeStereoBootstrap();auto s=deviceState(key(d));const auto n=++acquireCount;LARGE_INTEGER enter{},downstreamDone{},prepareDone{};QueryPerformanceCounter(&enter);
    if(extendedLoggingEnabled()&&traceDiagnosticCall(n)){std::ostringstream x;x<<"[ACQUIRE2] entry call="<<n<<" thread="<<GetCurrentThreadId()<<" device="<<reinterpret_cast<uint64_t>(d)<<" swapchain="<<(i?reinterpret_cast<uint64_t>(i->swapchain):0)<<" timeoutNs="<<(i?i->timeout:0)<<" semaphore="<<(i?reinterpret_cast<uint64_t>(i->semaphore):0)<<" fence="<<(i?reinterpret_cast<uint64_t>(i->fence):0)<<" deviceMask="<<(i?i->deviceMask:0);logExtended(x.str());}
    auto r=s.acquire2?s.acquire2(d,i,out):VK_ERROR_EXTENSION_NOT_PRESENT;QueryPerformanceCounter(&downstreamDone);
    if(logFrame(n)){std::ostringstream x;x<<"[Frame "<<n<<"] Acquire2 image="<<(out?*out:~0u)<<" result="<<r;logLine(x.str());}
    if(i&&(r==VK_SUCCESS||r==VK_SUBOPTIMAL_KHR))KharvoxXRPrepareFrame(i->swapchain);QueryPerformanceCounter(&prepareDone);
    const double downstreamMs=elapsedMilliseconds(enter,downstreamDone),prepareMs=elapsedMilliseconds(downstreamDone,prepareDone);
    if(extendedLoggingEnabled()&&(traceDiagnosticCall(n)||r!=VK_SUCCESS||downstreamMs>=10.0||prepareMs>=10.0)){std::ostringstream x;x<<"[ACQUIRE2] return call="<<n<<" thread="<<GetCurrentThreadId()<<" result="<<r<<" image="<<(out?*out:~0u)<<" downstreamMs="<<downstreamMs<<" xrPrepareMs="<<prepareMs<<" totalMs="<<elapsedMilliseconds(enter,prepareDone);logExtended(x.str());}
    return kharvox::independentSurfaceResult(r,i&&isIndependentSwapchain(i->swapchain));
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue q,uint32_t c,const VkSubmitInfo*i,VkFence f){
    kharvox::native::trace::SubmitScope traceSubmit("engine-submit",q,c,i,f);
    auto s=deviceState(key(q));const auto n=++submitCount;LARGE_INTEGER enter{},returned{};QueryPerformanceCounter(&enter);if(logFrame(n)){std::ostringstream x;x<<"[Submit "<<n<<"] batches="<<c;logLine(x.str());}
    VkResult result=VK_ERROR_DEVICE_LOST;{std::lock_guard<std::recursive_mutex>queueLock(queueAccessMutex);result=s.submit?s.submit(q,c,i,f):VK_ERROR_DEVICE_LOST;kharvox::native::submitted(q,c,i,result);}QueryPerformanceCounter(&returned);
    const double totalMs=elapsedMilliseconds(enter,returned);if(result==VK_ERROR_DEVICE_LOST)logLine("[DEVICE-LOST] vkQueueSubmit returned VK_ERROR_DEVICE_LOST call="+std::to_string(n));if(extendedLoggingEnabled()&&(traceDiagnosticCall(n)||result!=VK_SUCCESS||totalMs>=10.0)){std::ostringstream x;x<<"[SUBMIT] return call="<<n<<" thread="<<GetCurrentThreadId()<<" queue="<<reinterpret_cast<uint64_t>(q)<<" batches="<<c<<" fence="<<reinterpret_cast<uint64_t>(f)<<" result="<<result<<" totalMs="<<totalMs;logExtended(x.str());}traceSubmit.result(result);return result;
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2(VkQueue q,uint32_t c,const VkSubmitInfo2*i,VkFence f){
    kharvox::native::trace::SubmitScope traceSubmit("engine-submit2",q,c,i,f);
    auto s=deviceState(key(q));const auto n=++submitCount;LARGE_INTEGER enter{},returned{};QueryPerformanceCounter(&enter);if(logFrame(n)){std::ostringstream x;x<<"[Submit2 "<<n<<"] batches="<<c;logLine(x.str());}
    VkResult result=VK_ERROR_DEVICE_LOST;{std::lock_guard<std::recursive_mutex>queueLock(queueAccessMutex);result=s.submit2?s.submit2(q,c,i,f):VK_ERROR_DEVICE_LOST;kharvox::native::submitted2(q,c,i,result);}QueryPerformanceCounter(&returned);
    const double totalMs=elapsedMilliseconds(enter,returned);if(result==VK_ERROR_DEVICE_LOST)logLine("[DEVICE-LOST] vkQueueSubmit2 returned VK_ERROR_DEVICE_LOST call="+std::to_string(n));if(extendedLoggingEnabled()&&(traceDiagnosticCall(n)||result!=VK_SUCCESS||totalMs>=10.0)){std::ostringstream x;x<<"[SUBMIT2] return call="<<n<<" thread="<<GetCurrentThreadId()<<" queue="<<reinterpret_cast<uint64_t>(q)<<" batches="<<c<<" fence="<<reinterpret_cast<uint64_t>(f)<<" result="<<result<<" totalMs="<<totalMs;logExtended(x.str());}traceSubmit.result(result);return result;
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue q,const VkPresentInfoKHR*i){
    kharvox::DiagnosticPresentDuration presentDuration;
    kharvox::engineMemoryStartupComplete.store(true,std::memory_order_relaxed);
    // Keep the lease through native::completed(), including empty XR frames.
    // Otherwise retirement can enter between XR return and Native owner drain.
    kharvox::GameImageLifetime::Use gameImages(kharvox::gameImageLifetime());
    struct TraceBoundary {~TraceBoundary(){kharvox::native::trace::presentFinished();}} traceBoundary;
    kharvox::native::trace::Scope tracePresent("engine-present",reinterpret_cast<uintptr_t>(q));
    if(i&&tracePresent.id)for(uint32_t n=0;n<i->waitSemaphoreCount;++n)kharvox::native::trace::row("present-wait",tracePresent.id,{n,reinterpret_cast<uintptr_t>(i->pWaitSemaphores[n])});
    KharvoxXRStartSessionIfReady();
    auto s=deviceState(key(q));
    auto n=++presentCount;
    LARGE_INTEGER enter{},afterKharvox{},downstreamDone{};QueryPerformanceCounter(&enter);
    if(logFrame(n)){
        std::ostringstream x;
        x<<"[Present "<<n<<"] swapchains="<<(i?i->swapchainCount:0);
        logLine(x.str());
    }
    if(extendedLoggingEnabled()&&traceDiagnosticCall(n)){
        std::ostringstream x;x<<"[PRESENT] entry call="<<n<<" thread="<<GetCurrentThreadId()<<" queue="<<reinterpret_cast<uint64_t>(q)<<" swapchains="<<(i?i->swapchainCount:0)<<" waits="<<(i?i->waitSemaphoreCount:0);
        if(i&&i->swapchainCount){x<<" firstSwapchain="<<reinterpret_cast<uint64_t>(i->pSwapchains[0])<<" firstImage="<<(i->pImageIndices?i->pImageIndices[0]:~0u);}
        logExtended(x.str());
    }
    kharvox::hudgpu::present(n);
    KharvoxCameraCompleteNativeStereoBootstrapAfterRender();
    KharvoxCameraPollDiagnostic();
    bool consumedPresentWaits=false;
    kharvox::native::presentStarting();
    KharvoxXRPresent(q,i,&consumedPresentWaits,gameImages);
    kharvox::native::completed();
    QueryPerformanceCounter(&afterKharvox);
    if(extendedLoggingEnabled()&&traceDiagnosticCall(n)){std::ostringstream x;x<<"[PRESENT] XR return call="<<n<<" thread="<<GetCurrentThreadId()<<" consumedWaits="<<(consumedPresentWaits?1:0)<<" xrMs="<<elapsedMilliseconds(enter,afterKharvox);logExtended(x.str());}
    if(!s.present){tracePresent.result=VK_ERROR_DEVICE_LOST;return VK_ERROR_DEVICE_LOST;}
    VkResult presentResult=VK_ERROR_DEVICE_LOST;
    {
        std::lock_guard<std::recursive_mutex>queueLock(queueAccessMutex);
        if(!consumedPresentWaits||!i)presentResult=s.present(q,i);
        else{
            VkPresentInfoKHR ready=*i;
            ready.waitSemaphoreCount=0;
            ready.pWaitSemaphores=nullptr;
            presentResult=s.present(q,&ready);
        }
    }
    QueryPerformanceCounter(&downstreamDone);
    if(i&&i->swapchainCount){
        bool allIndependent=true;
        for(uint32_t index=0;index<i->swapchainCount;++index){
            const bool independent=isIndependentSwapchain(i->pSwapchains[index]);
            allIndependent=allIndependent&&independent;
            if(i->pResults)i->pResults[index]=kharvox::independentSurfaceResult(i->pResults[index],independent);
        }
        if(presentResult==VK_SUBOPTIMAL_KHR&&allIndependent&&n<=3)logLine("[INDEPENDENT-SURFACE] accepting successful scaled desktop Present (SUBOPTIMAL)");
        presentResult=kharvox::independentSurfaceResult(presentResult,allIndependent);
        KharvoxXRNativePresentCompleted(i->pSwapchains[0],presentResult);
        // Direct VDXR deliberately reaches this call only after the native WSI
        // swapchain has proven stable. Session/resource creation therefore
        // happens between completed native frames, never during startup churn.
        KharvoxXRStartSessionIfReady();
    }
    const double xrMs=elapsedMilliseconds(enter,afterKharvox),downstreamMs=elapsedMilliseconds(afterKharvox,downstreamDone);
    if(extendedLoggingEnabled()&&(traceDiagnosticCall(n)||presentResult!=VK_SUCCESS||xrMs>=10.0||downstreamMs>=10.0)){
        std::ostringstream x;x<<"[PRESENT] return call="<<n<<" thread="<<GetCurrentThreadId()<<" result="<<presentResult<<" consumedWaits="<<(consumedPresentWaits?1:0)<<" xrMs="<<xrMs<<" downstreamMs="<<downstreamMs<<" totalMs="<<elapsedMilliseconds(enter,downstreamDone);
        if(i&&i->pResults&&i->swapchainCount)x<<" firstPerSwapchainResult="<<i->pResults[0];logExtended(x.str());
    }
    tracePresent.result=presentResult;
    return presentResult;
}
VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants(VkCommandBuffer cb,VkPipelineLayout l,VkShaderStageFlags stages,uint32_t off,uint32_t size,const void* values){auto fn=commandFunction(cb,&DeviceDispatch::cmdPushConstants);if(fn)fn(cb,l,stages,off,size,values);}
VKAPI_ATTR void VKAPI_CALL vkCmdUpdateBuffer(VkCommandBuffer cb,VkBuffer dst,VkDeviceSize off,VkDeviceSize size,const void* data){auto fn=commandFunction(cb,&DeviceDispatch::cmdUpdateBuffer);if(fn)fn(cb,dst,off,size,data);}
VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice d,VkDeviceMemory memory,VkDeviceSize off,VkDeviceSize size,VkMemoryMapFlags flags,void**out){auto s=deviceState(key(d));return s.mapMemory?s.mapMemory(d,memory,off,size,flags,out):VK_ERROR_MEMORY_MAP_FAILED;}
VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice d,VkDeviceMemory memory){auto s=deviceState(key(d));if(s.unmapMemory)s.unmapMemory(d,memory);}
VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice d,VkBuffer buffer,VkDeviceMemory memory,VkDeviceSize offset){auto s=deviceState(key(d));return s.bindBufferMemory?s.bindBufferMemory(d,buffer,memory,offset):VK_ERROR_DEVICE_LOST;}
VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(VkDevice d,uint32_t writeCount,const VkWriteDescriptorSet*writes,uint32_t copyCount,const VkCopyDescriptorSet*copies){kharvox::hudgpu::updateDescriptorSets(writeCount,writes,copyCount,copies);auto s=deviceState(key(d));if(s.updateDescriptorSets)s.updateDescriptorSets(d,writeCount,writes,copyCount,copies);}
VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer cb,VkPipelineBindPoint point,VkPipelineLayout layout,uint32_t firstSet,uint32_t setCount,const VkDescriptorSet*sets,uint32_t dynamicOffsetCount,const uint32_t*dynamicOffsets){kharvox::hudgpu::bindDescriptorSets(cb,point,firstSet,setCount,sets);auto fn=commandFunction(cb,&DeviceDispatch::cmdBindDescriptorSets);if(fn)fn(cb,point,layout,firstSet,setCount,sets,dynamicOffsetCount,dynamicOffsets);}
VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer cb,VkPipelineBindPoint point,VkPipeline pipeline){kharvox::hands::handSceneBindPipeline(cb,point,pipeline);kharvox::hudgpu::bindPipeline(cb,point,pipeline);auto fn=commandFunction(cb,&DeviceDispatch::cmdBindPipeline);if(fn)fn(cb,point,pipeline);}
VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers(VkCommandBuffer cb,uint32_t firstBinding,uint32_t bindingCount,const VkBuffer*buffers,const VkDeviceSize*offsets){kharvox::hudgpu::bindVertexBuffers(cb,firstBinding,bindingCount,buffers,offsets);auto fn=commandFunction(cb,&DeviceDispatch::cmdBindVertexBuffers);if(fn)fn(cb,firstBinding,bindingCount,buffers,offsets);}
VKAPI_ATTR void VKAPI_CALL vkCmdBindIndexBuffer(VkCommandBuffer cb,VkBuffer buffer,VkDeviceSize offset,VkIndexType indexType){kharvox::hudgpu::bindIndexBuffer(cb,buffer,offset,indexType);auto fn=commandFunction(cb,&DeviceDispatch::cmdBindIndexBuffer);if(fn)fn(cb,buffer,offset,indexType);}
VKAPI_ATTR void VKAPI_CALL vkCmdDraw(VkCommandBuffer cb,uint32_t vertexCount,uint32_t instanceCount,uint32_t firstVertex,uint32_t firstInstance){kharvox::hudgpu::draw(cb,kharvox::hudgpu::DrawKind::Direct,1,uint64_t(vertexCount)*instanceCount);auto fn=commandFunction(cb,&DeviceDispatch::cmdDraw);if(fn)fn(cb,vertexCount,instanceCount,firstVertex,firstInstance);}
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexed(VkCommandBuffer cb,uint32_t indexCount,uint32_t instanceCount,uint32_t firstIndex,int32_t vertexOffset,uint32_t firstInstance){kharvox::hudgpu::draw(cb,kharvox::hudgpu::DrawKind::Indexed,1,uint64_t(indexCount)*instanceCount);auto fn=commandFunction(cb,&DeviceDispatch::cmdDrawIndexed);if(fn)fn(cb,indexCount,instanceCount,firstIndex,vertexOffset,firstInstance);}
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirect(VkCommandBuffer cb,VkBuffer buffer,VkDeviceSize offset,uint32_t drawCount,uint32_t stride){kharvox::hudgpu::draw(cb,kharvox::hudgpu::DrawKind::Indirect,drawCount,drawCount);auto fn=commandFunction(cb,&DeviceDispatch::cmdDrawIndirect);if(fn)fn(cb,buffer,offset,drawCount,stride);}
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexedIndirect(VkCommandBuffer cb,VkBuffer buffer,VkDeviceSize offset,uint32_t drawCount,uint32_t stride){kharvox::hudgpu::draw(cb,kharvox::hudgpu::DrawKind::IndexedIndirect,drawCount,drawCount);auto fn=commandFunction(cb,&DeviceDispatch::cmdDrawIndexedIndirect);if(fn)fn(cb,buffer,offset,drawCount,stride);}
VKAPI_ATTR void VKAPI_CALL vkCmdSetViewport(VkCommandBuffer cb,uint32_t first,uint32_t count,const VkViewport*viewports){kharvox::hudgpu::setViewport(cb,count,viewports);auto fn=commandFunction(cb,&DeviceDispatch::cmdSetViewport);if(fn)fn(cb,first,count,viewports);}
VKAPI_ATTR void VKAPI_CALL vkCmdSetScissor(VkCommandBuffer cb,uint32_t first,uint32_t count,const VkRect2D*scissors){kharvox::hudgpu::setScissor(cb,count,scissors);auto fn=commandFunction(cb,&DeviceDispatch::cmdSetScissor);if(fn)fn(cb,first,count,scissors);}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice d,VkPipelineCache cache,uint32_t count,const VkGraphicsPipelineCreateInfo*i,const VkAllocationCallbacks*a,VkPipeline*out){
    kharvox::DiagnosticDuration total("graphics-pipeline-total");
    auto s=deviceState(key(d));
    VkResult r;
    {
        kharvox::DiagnosticDuration downstream("graphics-pipeline-driver");
        r=s.createGraphicsPipelines?s.createGraphicsPipelines(d,cache,count,i,a,out):VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    if(r==VK_SUCCESS){
        kharvox::DiagnosticDuration tracking("graphics-pipeline-tracking");
        kharvox::hands::handSceneGraphicsPipelinesCreated(count,i,out);
        kharvox::hudgpu::graphicsPipelinesCreated(count,i,out);
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(VkDevice d,VkPipeline pipeline,const VkAllocationCallbacks*a){auto s=deviceState(key(d));kharvox::hands::handScenePipelineDestroyed(pipeline);kharvox::hudgpu::pipelineDestroyed(pipeline);if(s.destroyPipeline)s.destroyPipeline(d,pipeline,a);}
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer cb,VkPipelineStageFlags srcStage,VkPipelineStageFlags dstStage,VkDependencyFlags dependencies,uint32_t memoryBarrierCount,const VkMemoryBarrier*memoryBarriers,uint32_t bufferBarrierCount,const VkBufferMemoryBarrier*bufferBarriers,uint32_t imageBarrierCount,const VkImageMemoryBarrier*imageBarriers){kharvox::hands::handSceneImageBarriers(imageBarrierCount,imageBarriers);auto fn=kharvox::lookupDispatchMember(stateMutex,devices,key(cb),&DeviceDispatch::xr,&KharvoxVulkanDispatch::cmdPipelineBarrier);if(fn)fn(cb,srcStage,dstStage,dependencies,memoryBarrierCount,memoryBarriers,bufferBarrierCount,bufferBarriers,imageBarrierCount,imageBarriers);}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice d,const VkImageCreateInfo*i,const VkAllocationCallbacks*a,VkImage*out){auto s=deviceState(key(d));if(!s.createImage)return VK_ERROR_EXTENSION_NOT_PRESENT;if(!i)return s.createImage(d,i,a,out);VkImageCreateInfo effective=*i;const bool hudQuadCandidate=kharvox::hudgpu::enabled()&&i->imageType==VK_IMAGE_TYPE_2D&&i->format==VK_FORMAT_R8G8B8A8_UNORM&&i->extent.width==960&&i->extent.height==540&&i->extent.depth==1&&(i->usage&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)&&(i->usage&VK_IMAGE_USAGE_SAMPLED_BIT);if(hudQuadCandidate)effective.usage|=VK_IMAGE_USAGE_TRANSFER_SRC_BIT;auto r=s.createImage(d,&effective,a,out);if(r!=VK_SUCCESS&&hudQuadCandidate){logLine("[HUD9-SOURCE] internal candidate transfer-source usage rejected; retrying native image usage");effective=*i;r=s.createImage(d,i,a,out);}if(r==VK_SUCCESS&&out){kharvox::hands::handSceneImageCreated(*out,effective);kharvox::hudgpu::imageCreated(*out,effective);if(hudQuadCandidate&&(effective.usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT)){static std::atomic<bool> logged{};if(!logged.exchange(true))logLine("[HUD9-SOURCE] 960x540 internal RGBA candidate created with TRANSFER_SRC usage (not a headset resolution)");}}return r;}
VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice d,VkImage image,const VkAllocationCallbacks*a){auto s=deviceState(key(d));kharvox::hands::handSceneImageDestroyed(image);kharvox::hudgpu::imageDestroyed(image);if(s.destroyImage)s.destroyImage(d,image,a);}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice d,const VkImageViewCreateInfo*i,const VkAllocationCallbacks*a,VkImageView*out){auto s=deviceState(key(d));auto r=s.createImageView?s.createImageView(d,i,a,out):VK_ERROR_EXTENSION_NOT_PRESENT;if(r==VK_SUCCESS&&i&&out){kharvox::hands::handSceneImageViewCreated(*out,*i);kharvox::hudgpu::imageViewCreated(*out,*i);}return r;}
VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice d,VkImageView view,const VkAllocationCallbacks*a){auto s=deviceState(key(d));kharvox::hands::handSceneImageViewDestroyed(view);kharvox::hudgpu::imageViewDestroyed(view);if(s.destroyImageView)s.destroyImageView(d,view,a);}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateFramebuffer(VkDevice d,const VkFramebufferCreateInfo*i,const VkAllocationCallbacks*a,VkFramebuffer*out){auto s=deviceState(key(d));auto r=s.createFramebuffer?s.createFramebuffer(d,i,a,out):VK_ERROR_EXTENSION_NOT_PRESENT;if(r==VK_SUCCESS&&i&&out){kharvox::hands::handSceneFramebufferCreated(*out,*i);kharvox::hudgpu::framebufferCreated(*out,*i);}return r;}
VKAPI_ATTR void VKAPI_CALL vkDestroyFramebuffer(VkDevice d,VkFramebuffer framebuffer,const VkAllocationCallbacks*a){auto s=deviceState(key(d));kharvox::hands::handSceneFramebufferDestroyed(framebuffer);kharvox::hudgpu::framebufferDestroyed(framebuffer);if(s.destroyFramebuffer)s.destroyFramebuffer(d,framebuffer,a);}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass(VkDevice d,const VkRenderPassCreateInfo*i,const VkAllocationCallbacks*a,VkRenderPass*out){auto s=deviceState(key(d));if(!s.createRenderPass)return VK_ERROR_EXTENSION_NOT_PRESENT;if(!i)return s.createRenderPass(d,i,a,out);VkRenderPassCreateInfo effective=*i;std::vector<VkAttachmentDescription>attachments;if(kharvox::hands::handSceneTrackingEnabled()&&i->pAttachments){attachments.assign(i->pAttachments,i->pAttachments+i->attachmentCount);for(auto&attachment:attachments)if(kharvox::hands::handSceneDepthFormat(attachment.format))attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;effective.pAttachments=attachments.data();}auto r=s.createRenderPass(d,&effective,a,out);if(r==VK_SUCCESS&&out){kharvox::hands::handSceneRenderPassCreated(*out,effective);kharvox::hudgpu::renderPassCreated(*out,effective.attachmentCount,effective.pAttachments);}return r;}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass2(VkDevice d,const VkRenderPassCreateInfo2*i,const VkAllocationCallbacks*a,VkRenderPass*out){auto s=deviceState(key(d));if(!s.createRenderPass2)return VK_ERROR_EXTENSION_NOT_PRESENT;if(!i)return s.createRenderPass2(d,i,a,out);VkRenderPassCreateInfo2 effective=*i;std::vector<VkAttachmentDescription2>attachments;if(kharvox::hands::handSceneTrackingEnabled()&&i->pAttachments){attachments.assign(i->pAttachments,i->pAttachments+i->attachmentCount);for(auto&attachment:attachments)if(kharvox::hands::handSceneDepthFormat(attachment.format))attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;effective.pAttachments=attachments.data();}auto r=s.createRenderPass2(d,&effective,a,out);if(r==VK_SUCCESS&&out){kharvox::hands::handSceneRenderPass2Created(*out,effective);kharvox::hudgpu::renderPass2Created(*out,effective.attachmentCount,effective.pAttachments);}return r;}
VKAPI_ATTR void VKAPI_CALL vkDestroyRenderPass(VkDevice d,VkRenderPass renderPass,const VkAllocationCallbacks*a){auto s=deviceState(key(d));kharvox::hands::handSceneRenderPassDestroyed(renderPass);kharvox::hudgpu::renderPassDestroyed(renderPass);if(s.destroyRenderPass)s.destroyRenderPass(d,renderPass,a);}
VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass(VkCommandBuffer cb,const VkRenderPassBeginInfo*i,VkSubpassContents contents){kharvox::hands::handSceneBeginRenderPass(cb,i);kharvox::hudgpu::beginRenderPass(cb,i);auto fn=commandFunction(cb,&DeviceDispatch::cmdBeginRenderPass);if(fn)fn(cb,i,contents);}
VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass2(VkCommandBuffer cb,const VkRenderPassBeginInfo*i,const VkSubpassBeginInfo*begin){kharvox::hands::handSceneBeginRenderPass(cb,i);kharvox::hudgpu::beginRenderPass(cb,i);auto fn=commandFunction(cb,&DeviceDispatch::cmdBeginRenderPass2);if(fn)fn(cb,i,begin);}
VKAPI_ATTR void VKAPI_CALL vkCmdNextSubpass(VkCommandBuffer cb,VkSubpassContents contents){kharvox::hands::handSceneNextSubpass(cb);kharvox::hudgpu::nextSubpass(cb);auto fn=commandFunction(cb,&DeviceDispatch::cmdNextSubpass);if(fn)fn(cb,contents);}
VKAPI_ATTR void VKAPI_CALL vkCmdNextSubpass2(VkCommandBuffer cb,const VkSubpassBeginInfo*begin,const VkSubpassEndInfo*end){kharvox::hands::handSceneNextSubpass(cb);kharvox::hudgpu::nextSubpass(cb);auto fn=commandFunction(cb,&DeviceDispatch::cmdNextSubpass2);if(fn)fn(cb,begin,end);}
VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer cb){kharvox::hands::handSceneEndRenderPass(cb);kharvox::hudgpu::endRenderPass(cb);auto fn=commandFunction(cb,&DeviceDispatch::cmdEndRenderPass);if(fn)fn(cb);}
VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass2(VkCommandBuffer cb,const VkSubpassEndInfo*end){kharvox::hands::handSceneEndRenderPass(cb);kharvox::hudgpu::endRenderPass(cb);auto fn=commandFunction(cb,&DeviceDispatch::cmdEndRenderPass2);if(fn)fn(cb,end);}
}

BOOL APIENTRY DllMain(HMODULE module,DWORD reason,LPVOID){if(reason==DLL_PROCESS_ATTACH)DisableThreadLibraryCalls(module);else if(reason==DLL_PROCESS_DETACH&&isDoomProcess())KharvoxXRShutdownHaptics();return TRUE;}

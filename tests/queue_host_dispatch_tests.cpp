#include <windows.h>
#include <vulkan/vulkan.h>
#include <cassert>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>

static std::recursive_mutex queueAccessMutex;
static uint64_t submitCount{};
static bool logFrame(uint64_t){return false;}
static bool extendedLoggingEnabled(){return false;}
static bool traceDiagnosticCall(uint64_t){return false;}
static void logLine(const std::string&){}
static void logExtended(const std::string&){}
static double elapsedMilliseconds(const LARGE_INTEGER&,const LARGE_INTEGER&){return 0;}
template<class T>static void* key(T value){return reinterpret_cast<void*>(value);}
static struct DeviceDispatch {
    VkDevice device=reinterpret_cast<VkDevice>(uintptr_t(1));
    PFN_vkQueueSubmit2 submit2{};
    PFN_vkQueueSubmit2KHR submit2Khr{};
    bool runtimeAuxiliary{};
} dispatch;
static DeviceDispatch deviceState(void*){return dispatch;}
static unsigned observed{};
static VkResult observedResult{};
namespace kharvox::native {
static void submitted2(VkQueue,uint32_t,const VkSubmitInfo2*,VkResult){}
namespace trace {
struct SubmitScope {
    SubmitScope(const char*,VkQueue,uint32_t,const VkSubmitInfo2*,VkFence){}
    void result(VkResult){}
};
}
}
namespace kharvox::sfs {
static bool vrEnabled(){return true;}
static void submitted(VkDevice device,VkQueue,VkResult result){assert(device==dispatch.device);++observed;observedResult=result;}
}
#include "../src/vulkan/QueueSubmit2.inc"

static unsigned coreCalls{},khrCalls{};
static bool coreCallable{};
static VkResult downstreamResult=VK_SUCCESS;
static VkResult VKAPI_CALL core(VkQueue,uint32_t,const VkSubmitInfo2*,VkFence){assert(coreCallable);++coreCalls;return downstreamResult;}
static VkResult VKAPI_CALL khr(VkQueue,uint32_t,const VkSubmitInfo2*,VkFence){++khrCalls;return downstreamResult;}

int main(){
    dispatch.submit2=core;dispatch.submit2Khr=khr;
    const auto queue=reinterpret_cast<VkQueue>(uintptr_t(2));
    VkSubmitInfo2 info{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    VkCommandBufferSubmitInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    info.commandBufferInfoCount=1;info.pCommandBufferInfos=&command;
    assert(vkQueueSubmit2KHR(queue,1,&info,{})==VK_SUCCESS);
    assert(khrCalls==1&&coreCalls==0&&observed==1);
    coreCallable=true;
    assert(vkQueueSubmit2(queue,1,&info,{})==VK_SUCCESS);
    assert(coreCalls==1&&khrCalls==1&&observed==2);
    dispatch.submit2Khr=nullptr;
    assert(vkQueueSubmit2KHR(queue,1,&info,{})==VK_ERROR_DEVICE_LOST);
    assert(coreCalls==1&&khrCalls==1&&observedResult==VK_ERROR_DEVICE_LOST);
    dispatch.submit2=nullptr;dispatch.submit2Khr=khr;
    assert(vkQueueSubmit2(queue,1,&info,{})==VK_ERROR_DEVICE_LOST);
    assert(khrCalls==1);
    assert(vkQueueSubmit2KHR(queue,1,&info,{})==VK_SUCCESS);
    info.commandBufferInfoCount=0;
    const auto previous=observed;
    assert(vkQueueSubmit2KHR(queue,1,&info,{})==VK_SUCCESS&&observed==previous);
    downstreamResult=VK_ERROR_DEVICE_LOST;
    assert(vkQueueSubmit2KHR(queue,1,&info,{})==downstreamResult);
    assert(observed==previous+1&&observedResult==downstreamResult);
    dispatch.runtimeAuxiliary=true;
    assert(vkQueueSubmit2KHR(queue,1,&info,{})==downstreamResult&&observed==previous+1);
}

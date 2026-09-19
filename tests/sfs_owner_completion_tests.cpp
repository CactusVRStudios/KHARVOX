#include "../src/sfs/NativeSfs.h"
#include "../src/sfs/FrameProjection.h"
#include "../src/sfs/ShaderCompiler.h"
#include "../src/native/NativeStereo.h"
#include "../src/common/AerSourceTracking.h"
#include <windows.h>
#include <cstring>
#include <future>
#include <stdexcept>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

namespace kharvox::sfs {
CompiledShader compileStereoShader(const std::vector<uint32_t>&,const ShaderCompileOptions&){throw std::runtime_error("Unexpected shader compilation");}
bool hasStereoStorageOutput(const std::vector<uint32_t>&){throw std::runtime_error("Unexpected shader inspection");}
bool needsHeadsetProjection(const std::vector<uint32_t>&,bool,bool){throw std::runtime_error("Unexpected shader projection");}
}

using namespace kharvox::sfs;
static void* dispatch=reinterpret_cast<void*>(uintptr_t(1));
static const auto device=reinterpret_cast<VkDevice>(&dispatch);
static const auto owner=reinterpret_cast<VkQueue>(uintptr_t(2));
static const auto other=reinterpret_cast<VkQueue>(uintptr_t(3));
static const auto fence=reinterpret_cast<VkFence>(uintptr_t(4));
static std::mutex queueMutex;
static bool queueLocked{},verifyQueue{};
static unsigned waits{},uploads{};
static FrameUniforms memory;

static void lockQueue(){queueMutex.lock();assert(!queueLocked);queueLocked=true;}
static void unlockQueue(){assert(queueLocked);queueLocked=false;queueMutex.unlock();}
static VkResult VKAPI_CALL createBuffer(VkDevice,const VkBufferCreateInfo*,const VkAllocationCallbacks*,VkBuffer* out){*out=reinterpret_cast<VkBuffer>(uintptr_t(5));return VK_SUCCESS;}
static void VKAPI_CALL destroyBuffer(VkDevice,VkBuffer,const VkAllocationCallbacks*){}
static void VKAPI_CALL requirements(VkDevice,VkBuffer,VkMemoryRequirements* out){*out={sizeof(memory),16,1};}
static VkResult VKAPI_CALL allocateMemory(VkDevice,const VkMemoryAllocateInfo*,const VkAllocationCallbacks*,VkDeviceMemory* out){*out=reinterpret_cast<VkDeviceMemory>(uintptr_t(6));return VK_SUCCESS;}
static void VKAPI_CALL freeMemory(VkDevice,VkDeviceMemory,const VkAllocationCallbacks*){}
static VkResult VKAPI_CALL bindMemory(VkDevice,VkBuffer,VkDeviceMemory,VkDeviceSize){return VK_SUCCESS;}
static VkResult VKAPI_CALL waitIdle(VkDevice){assert(!verifyQueue||queueLocked);++waits;return VK_SUCCESS;}
static VkResult VKAPI_CALL mapMemory(VkDevice,VkDeviceMemory,VkDeviceSize,VkDeviceSize,VkMemoryMapFlags,void** out){
    if(verifyQueue){
        assert(queueLocked);
        assert(std::async(std::launch::async,[]{
            if(!queueMutex.try_lock())return true;
            queueMutex.unlock();return false;
        }).get());
    }
    ++uploads;*out=&memory;return VK_SUCCESS;
}
static void VKAPI_CALL unmapMemory(VkDevice,VkDeviceMemory){assert(!verifyQueue||queueLocked);}
static PFN_vkVoidFunction VKAPI_CALL resolver(VkDevice,const char* name){
#define ENTRY(api,fn) if(!std::strcmp(name,#api))return reinterpret_cast<PFN_vkVoidFunction>(fn)
    ENTRY(vkCreateBuffer,createBuffer);ENTRY(vkDestroyBuffer,destroyBuffer);
    ENTRY(vkGetBufferMemoryRequirements,requirements);ENTRY(vkAllocateMemory,allocateMemory);
    ENTRY(vkFreeMemory,freeMemory);ENTRY(vkBindBufferMemory,bindMemory);
    ENTRY(vkDeviceWaitIdle,waitIdle);ENTRY(vkMapMemory,mapMemory);ENTRY(vkUnmapMemory,unmapMemory);
#undef ENTRY
    return nullptr;
}

static void testPolicy(){
    {OwnerQueueAccess access(nullptr,unlockQueue);assert(!access);}
    {OwnerQueueAccess access(lockQueue,nullptr);assert(!access);}
    OwnerCompletion retirement;
    assert(!retirement.canRetire(device));
    retirement.uploaded(10);
    assert(!retirement.capture(device,owner,fence,10).device);
    retirement.submitted(owner,VK_SUCCESS);
    const auto copy=retirement.capture(device,owner,fence,10);
    assert(copy.device==device&&copy.queue==owner&&copy.frame==10&&copy.generation);
    assert(!retirement.canRetire(device));
    for(auto result:{VK_NOT_READY,VK_TIMEOUT,VK_ERROR_DEVICE_LOST}){
        retirement.completed(device,copy,VK_SUCCESS,result);
        assert(!retirement.canRetire(device));
        retirement.completed(device,copy,result,VK_SUCCESS);
        assert(!retirement.canRetire(device));
    }
    retirement.completed(device,copy,VK_SUCCESS,VK_SUCCESS);
    assert(retirement.canRetire(device));
    assert(!retirement.canRetire(reinterpret_cast<VkDevice>(uintptr_t(7))));
    for(unsigned mismatch=0;mismatch<6;++mismatch){
        auto bad=copy;
        switch(mismatch){
        case 0:bad.device=VK_NULL_HANDLE;break;
        case 1:bad.queue=other;break;
        case 2:bad.fence=VK_NULL_HANDLE;break;
        case 3:++bad.frame;break;
        case 4:++bad.generation;break;
        case 5:++bad.submission;break;
        }
        retirement.completed(device,bad,VK_SUCCESS,VK_SUCCESS);
        assert(!retirement.canRetire(device));
    }
    retirement.uploaded(10);
    retirement.submitted(owner,VK_SUCCESS);
    retirement.completed(device,copy,VK_SUCCESS,VK_SUCCESS);
    assert(!retirement.canRetire(device));
    const auto current=retirement.capture(device,owner,fence,10);
    retirement.invalidate();
    retirement.completed(device,current,VK_SUCCESS,VK_SUCCESS);
    assert(!retirement.canRetire(device));
    for(auto queue:{owner,other}){
        retirement.uploaded(11);
        retirement.submitted(owner,VK_SUCCESS);
        const auto pending=retirement.capture(device,owner,fence,11);
        retirement.submitted(queue,VK_SUCCESS);
        retirement.completed(device,pending,VK_SUCCESS,VK_SUCCESS);
        assert(!retirement.canRetire(device));
    }
    retirement.uploaded(12);
    retirement.submitted(other,VK_SUCCESS);
    retirement.submitted(owner,VK_SUCCESS);
    assert(!retirement.capture(device,owner,fence,12).device);
    retirement.uploaded(13);
    retirement.submitted(owner,VK_ERROR_DEVICE_LOST);
    assert(!retirement.capture(device,owner,fence,13).device);
    retirement.uploaded(0);
    retirement.submitted(owner,VK_SUCCESS);
    assert(!retirement.capture(device,owner,fence,0).device);
}

static void submit(VkQueue queue=owner){OwnerQueueAccess access(lockQueue,unlockQueue);submitted(device,queue,VK_SUCCESS);}
static OwnerCopyCompletion capture(uint64_t frame,VkFence completionFence=fence){
    OwnerQueueAccess access(lockQueue,unlockQueue);
    return captureOwnerCopy(device,owner,completionFence,frame);
}
static void prepareFrame(uint64_t serial){
    kharvox::native::FramePose pose{};pose.serial=serial;
    prepare(device,pose,{});
}
static void finish(const OwnerCopyCompletion& copy){copyCompleted(device,copy,VK_SUCCESS,VK_SUCCESS);}
static void testRuntime(){
    SetEnvironmentVariableA("KHARVOX_SFS_NATIVE_PROBE","1");
    SetEnvironmentVariableA("KHARVOX_SFS_NATIVE_VR","1");
    VkPhysicalDeviceMemoryProperties properties{};properties.memoryTypeCount=1;
    properties.memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    assert(initialize(device,VK_NULL_HANDLE,resolver,properties,lockQueue,unlockQueue));
    verifyQueue=true;
    prepareFrame(1);beginFrame(device);assert(waits==1&&uploads==2);
    prepareFrame(2);beginFrame(device);assert(uploads==2);
    submit();const auto first=capture(1);
    copyCompleted(device,first,VK_SUCCESS,VK_TIMEOUT);
    beginFrame(device);assert(uploads==2);
    finish(first);beginFrame(device);assert(waits==1&&uploads==3);

    submit();const auto late=capture(2);finish(late);
    std::async(std::launch::async,[]{submit(other);}).get();
    prepareFrame(3);beginFrame(device);assert(waits==2&&uploads==4);
    submit();const auto early=capture(3);
    std::async(std::launch::async,[]{submit();}).get();
    finish(early);prepareFrame(4);beginFrame(device);assert(waits==3&&uploads==5);

    submit(other);submit();finish(capture(4));
    prepareFrame(5);beginFrame(device);assert(waits==4&&uploads==6);
    submit();finish(capture(5,VK_NULL_HANDLE));
    prepareFrame(6);beginFrame(device);assert(waits==5&&uploads==7);
    submit();finish(capture(6));
    prepareFrame(7);beginFrame(device);assert(waits==5&&uploads==8);
    finish(capture(6));prepareFrame(8);beginFrame(device);assert(waits==6&&uploads==9);

    const auto chain=reinterpret_cast<VkSwapchainKHR>(uintptr_t(8));
    const auto image=reinterpret_cast<VkImage>(uintptr_t(9));
    submit();finish(capture(8));swapchainImages(device,chain,1,&image);
    prepareFrame(9);beginFrame(device);assert(waits==7&&uploads==10);
    submit();finish(capture(9));swapchainDestroyed(device,chain);
    prepareFrame(10);beginFrame(device);assert(waits==8&&uploads==11);
    shutdown(device);

    verifyQueue=false;
    assert(initialize(device,VK_NULL_HANDLE,resolver,properties));
    prepareFrame(11);beginFrame(device);assert(waits==9);
    submit();finish(capture(11));prepareFrame(12);beginFrame(device);assert(waits==10);
    shutdown(device);
}

static void testQualifiedPose(){
    VkPhysicalDeviceMemoryProperties properties{};properties.memoryTypeCount=1;
    properties.memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    assert(initialize(device,VK_NULL_HANDLE,resolver,properties,lockQueue,unlockQueue));
    verifyQueue=true;
    const auto chain=reinterpret_cast<VkSwapchainKHR>(uintptr_t(10));
    const auto image=reinterpret_cast<VkImage>(uintptr_t(11));
    swapchainImages(device,chain,1,&image);
    kharvox::native::FramePose pose{};pose.serial=20;pose.head.orientation.w=1;pose.gameplay=true;
    pose.source={20,1,kharvox::native::SceneDomain::Gameplay};
    const XrFovf fov{-.7f,.7f,.7f,-.7f};
    for(auto& view:pose.views){view.pose=pose.head;view.fov=fov;}
    prepare(device,pose,fov);beginFrame(device,chain,0);
    submit();finish(capture(20));
    pose.serial=21;pose.source.poseId=21;
    prepare(device,pose,fov);beginFrame(device,chain,0);
    const kharvox::AerSourceObservation observed{{20,1,0,0},1,false};
    kharvox::native::StereoFrame pairFrame;
    assert(pair(device,image,{8,8},VK_FORMAT_R8G8B8A8_UNORM,pairFrame,&observed));
    assert(pairFrame.pose.serial==20&&pairFrame.generation==21);
    submit();finish(capture(pairFrame.generation));
    const auto before=waits;
    prepareFrame(22);beginFrame(device,chain,0);assert(waits==before);
    swapchainDestroyed(device,chain);shutdown(device);
}

int main(){testPolicy();testRuntime();testQualifiedPose();}

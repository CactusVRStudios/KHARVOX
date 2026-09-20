#include <vulkan/vulkan.h>
#include <cassert>
#include <cstdint>
#include <future>
#include <mutex>

using KharvoxQueueAccessCallback=void(*)();
static struct { struct { PFN_vkQueueWaitIdle queueWaitIdle{}; } vk; } s;
#include "../src/openxr/QueueAccess.inc"

static std::recursive_mutex queueMutex;
static unsigned calls{};
static VkResult result=VK_SUCCESS;
static void lockQueue(){queueMutex.lock();}
static void unlockQueue(){queueMutex.unlock();}
static VkResult VKAPI_CALL idle(VkQueue queue){
    assert(queue);
    assert(std::async(std::launch::async,[]{
        if(!queueMutex.try_lock())return true;
        queueMutex.unlock();return false;
    }).get());
    ++calls;
    return result;
}
int main(){
    const auto queue=reinterpret_cast<VkQueue>(uintptr_t(1));
    s.vk.queueWaitIdle=idle;
    assert(waitForQueueIdle(queue)==VK_ERROR_INITIALIZATION_FAILED);
    queueAccessLockCallback=lockQueue;
    assert(waitForQueueIdle(queue)==VK_ERROR_INITIALIZATION_FAILED);
    queueAccessUnlockCallback=unlockQueue;
    assert(waitForQueueIdle(VK_NULL_HANDLE)==VK_ERROR_INITIALIZATION_FAILED);
    assert(calls==0);
    assert(waitForQueueIdle(queue)==VK_SUCCESS);
    result=VK_ERROR_DEVICE_LOST;
    assert(waitForQueueIdle(queue)==VK_ERROR_DEVICE_LOST);
    assert(calls==2);
    assert(std::async(std::launch::async,[]{
        if(!queueMutex.try_lock())return false;
        queueMutex.unlock();return true;
    }).get());
    s.vk.queueWaitIdle=nullptr;
    assert(waitForQueueIdle(queue)==VK_ERROR_INITIALIZATION_FAILED);
    assert(calls==2);
}

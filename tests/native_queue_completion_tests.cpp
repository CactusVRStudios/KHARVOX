#include "../src/native/NativeQueueCompletion.h"
#include <cassert>
#include <cstdint>
#include <future>
#include <mutex>

static std::mutex queueMutex;
static unsigned locks{}, unlocks{}, waits{};
static VkResult result = VK_SUCCESS;
static const auto device = reinterpret_cast<VkDevice>(uintptr_t(1));

static void lockQueue() { queueMutex.lock(); ++locks; }
static void unlockQueue() { ++unlocks; queueMutex.unlock(); }
static VkResult VKAPI_CALL waitDevice(VkDevice value) {
    assert(value == device && locks == unlocks + 1);
    ++waits;
    const auto blocked = std::async(std::launch::async, [] {
        if (!queueMutex.try_lock()) return true;
        queueMutex.unlock();
        return false;
    }).get();
    assert(blocked);
    return result;
}

int main() {
    using kharvox::native::waitForDeviceIdle;
    assert(waitForDeviceIdle(device, waitDevice, lockQueue, unlockQueue) == VK_SUCCESS);
    assert(locks == 1 && unlocks == 1 && waits == 1);
    result = VK_ERROR_DEVICE_LOST;
    assert(waitForDeviceIdle(device, waitDevice, lockQueue, unlockQueue) == result);
    assert(locks == 2 && unlocks == 2 && waits == 2);
    assert(waitForDeviceIdle(device, waitDevice, nullptr, unlockQueue) == VK_ERROR_INITIALIZATION_FAILED);
    assert(waitForDeviceIdle(device, waitDevice, lockQueue, nullptr) == VK_ERROR_INITIALIZATION_FAILED);
    assert(waitForDeviceIdle(device, nullptr, lockQueue, unlockQueue) == VK_ERROR_INITIALIZATION_FAILED);
    assert(waitForDeviceIdle(nullptr, waitDevice, lockQueue, unlockQueue) == VK_ERROR_INITIALIZATION_FAILED);
    assert(locks == 2 && unlocks == 2 && waits == 2);
}

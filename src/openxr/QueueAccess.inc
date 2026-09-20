KharvoxQueueAccessCallback queueAccessLockCallback{},queueAccessUnlockCallback{};
struct QueueAccessScope {
    QueueAccessScope(){if(queueAccessLockCallback)queueAccessLockCallback();}
    ~QueueAccessScope(){if(queueAccessUnlockCallback)queueAccessUnlockCallback();}
};
VkResult waitForQueueIdle(VkQueue queue){
    if(!queue||!s.vk.queueWaitIdle||!queueAccessLockCallback||!queueAccessUnlockCallback)
        return VK_ERROR_INITIALIZATION_FAILED;
    QueueAccessScope queueAccess;
    return s.vk.queueWaitIdle(queue);
}

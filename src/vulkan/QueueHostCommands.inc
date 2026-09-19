VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue q){
    const auto s=deviceState(key(q));
    std::lock_guard<std::recursive_mutex> lock(queueAccessMutex);
    const auto next=s.gdpa?reinterpret_cast<PFN_vkQueueWaitIdle>(s.gdpa(s.device,"vkQueueWaitIdle")):nullptr;
    return next?next(q):VK_ERROR_DEVICE_LOST;
}
VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice d){
    const auto s=deviceState(key(d));
    std::lock_guard<std::recursive_mutex> lock(queueAccessMutex);
    const auto next=s.gdpa?reinterpret_cast<PFN_vkDeviceWaitIdle>(s.gdpa(d,"vkDeviceWaitIdle")):nullptr;
    return next?next(d):VK_ERROR_DEVICE_LOST;
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueueBindSparse(VkQueue q,uint32_t count,const VkBindSparseInfo* infos,VkFence fence){
    const auto s=deviceState(key(q));
    std::lock_guard<std::recursive_mutex> lock(queueAccessMutex);
    const auto next=s.gdpa?reinterpret_cast<PFN_vkQueueBindSparse>(s.gdpa(s.device,"vkQueueBindSparse")):nullptr;
    const auto result=next?next(q,count,infos,fence):VK_ERROR_DEVICE_LOST;
    if(kharvox::sfs::vrEnabled()&&!s.runtimeAuxiliary)kharvox::sfs::submitted(s.device,q,result);
    return result;
}
#define QUEUE_HOST_VOID(name,parameters,arguments) \
VKAPI_ATTR void VKAPI_CALL name parameters { \
    const auto s=deviceState(key(q)); \
    std::lock_guard<std::recursive_mutex> lock(queueAccessMutex); \
    const auto next=s.gdpa?reinterpret_cast<PFN_##name>(s.gdpa(s.device,#name)):nullptr; \
    if(next)next arguments; \
}
QUEUE_HOST_VOID(vkQueueBeginDebugUtilsLabelEXT,(VkQueue q,const VkDebugUtilsLabelEXT* info),(q,info))
QUEUE_HOST_VOID(vkQueueEndDebugUtilsLabelEXT,(VkQueue q),(q))
QUEUE_HOST_VOID(vkQueueInsertDebugUtilsLabelEXT,(VkQueue q,const VkDebugUtilsLabelEXT* info),(q,info))
QUEUE_HOST_VOID(vkQueueNotifyOutOfBandNV,(VkQueue q,const VkOutOfBandQueueTypeInfoNV* info),(q,info))
QUEUE_HOST_VOID(vkQueueNotifyOutOfBandLegacyNV,(VkQueue q,uint32_t type),(q,type))
#undef QUEUE_HOST_VOID
#define QUEUE_HOST_RESULT(name,parameters,arguments) \
VKAPI_ATTR VkResult VKAPI_CALL name parameters { \
    const auto s=deviceState(key(q)); \
    std::lock_guard<std::recursive_mutex> lock(queueAccessMutex); \
    const auto next=s.gdpa?reinterpret_cast<PFN_##name>(s.gdpa(s.device,#name)):nullptr; \
    return next?next arguments:VK_ERROR_EXTENSION_NOT_PRESENT; \
}
QUEUE_HOST_RESULT(vkQueueSetPerformanceConfigurationINTEL,(VkQueue q,VkPerformanceConfigurationINTEL config),(q,config))
QUEUE_HOST_RESULT(vkQueueSetPerfHintQCOM,(VkQueue q,const VkPerfHintInfoQCOM* hint),(q,hint))
#undef QUEUE_HOST_RESULT

static PFN_vkVoidFunction queueHostProc(const char* n){
#define QUEUE_PROC(name) if(!std::strcmp(n,#name))return reinterpret_cast<PFN_vkVoidFunction>(&name)
    QUEUE_PROC(vkQueueSubmit);QUEUE_PROC(vkQueueSubmit2);QUEUE_PROC(vkQueuePresentKHR);
    if(kharvox::sfs::vrEnabled()){QUEUE_PROC(vkQueueSubmit2KHR);}
    QUEUE_PROC(vkQueueWaitIdle);QUEUE_PROC(vkQueueBindSparse);QUEUE_PROC(vkDeviceWaitIdle);
    QUEUE_PROC(vkQueueBeginDebugUtilsLabelEXT);QUEUE_PROC(vkQueueEndDebugUtilsLabelEXT);QUEUE_PROC(vkQueueInsertDebugUtilsLabelEXT);
    QUEUE_PROC(vkQueueNotifyOutOfBandNV);QUEUE_PROC(vkQueueNotifyOutOfBandLegacyNV);
    QUEUE_PROC(vkQueueSetPerformanceConfigurationINTEL);QUEUE_PROC(vkQueueSetPerfHintQCOM);
#undef QUEUE_PROC
    return nullptr;
}

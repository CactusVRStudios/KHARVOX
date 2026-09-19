static VkResult submitQueue2(VkQueue q,uint32_t c,const VkSubmitInfo2*i,VkFence f,bool khr){
    kharvox::native::trace::SubmitScope traceSubmit(khr?"engine-submit2-khr":"engine-submit2",q,c,i,f);
    auto s=deviceState(key(q));const auto next=khr?s.submit2Khr:s.submit2;
    const auto n=++submitCount;LARGE_INTEGER enter{},returned{};QueryPerformanceCounter(&enter);if(logFrame(n)){std::ostringstream x;x<<"[Submit2 "<<n<<"] batches="<<c;logLine(x.str());}
    VkResult result=VK_ERROR_DEVICE_LOST;{std::lock_guard<std::recursive_mutex>queueLock(queueAccessMutex);result=next?next(q,c,i,f):VK_ERROR_DEVICE_LOST;kharvox::native::submitted2(q,c,i,result);
        if(kharvox::sfs::vrEnabled()&&!s.runtimeAuxiliary){
            bool commands=false;for(uint32_t n=0;i&&n<c;++n)commands|=i[n].commandBufferInfoCount!=0;
            if(commands||result!=VK_SUCCESS)kharvox::sfs::submitted(s.device,q,result);
        }
    }QueryPerformanceCounter(&returned);
    const double totalMs=elapsedMilliseconds(enter,returned);if(result==VK_ERROR_DEVICE_LOST)logLine(std::string("[DEVICE-LOST] ")+(khr?"vkQueueSubmit2KHR":"vkQueueSubmit2")+" returned VK_ERROR_DEVICE_LOST call="+std::to_string(n));if(extendedLoggingEnabled()&&(traceDiagnosticCall(n)||result!=VK_SUCCESS||totalMs>=10.0)){std::ostringstream x;x<<"[SUBMIT2] return call="<<n<<" thread="<<GetCurrentThreadId()<<" queue="<<reinterpret_cast<uint64_t>(q)<<" batches="<<c<<" fence="<<reinterpret_cast<uint64_t>(f)<<" result="<<result<<" totalMs="<<totalMs;logExtended(x.str());}traceSubmit.result(result);return result;
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2(VkQueue q,uint32_t c,const VkSubmitInfo2*i,VkFence f){
    return submitQueue2(q,c,i,f,false);
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2KHR(VkQueue q,uint32_t c,const VkSubmitInfo2*i,VkFence f){
    return submitQueue2(q,c,i,f,true);
}

#pragma once
#include <cstdint>
#include <mutex>
#include <vulkan/vulkan.h>

namespace kharvox::sfs {
struct OwnerCopyCompletion {
    VkDevice device{};
    VkQueue queue{};
    VkFence fence{};
    uint64_t frame{},generation{},submission{};
};

class OwnerCompletion {
    mutable std::mutex mutex_;
    VkQueue queue_{};
    uint64_t frame_{},generation_{},submission_{};
    bool uncertain_{};
    OwnerCopyCompletion completed_{};

    bool matches(const OwnerCopyCompletion& copy,VkDevice device) const {
        return copy.device==device&&device&&copy.fence&&copy.queue&&copy.queue==queue_
            &&copy.frame&&copy.frame==frame_&&copy.generation==generation_
            &&copy.submission&&copy.submission==submission_&&!uncertain_;
    }
public:
    void submitted(VkQueue queue,VkResult result) {
        std::lock_guard lock(mutex_);
        ++submission_;
        uncertain_|=result!=VK_SUCCESS||!queue||(queue_&&queue_!=queue);
        queue_=queue;
        completed_={};
    }
    OwnerCopyCompletion capture(VkDevice device,VkQueue queue,VkFence fence,uint64_t frame) const {
        std::lock_guard lock(mutex_);
        const OwnerCopyCompletion copy{device,queue,fence,frame,generation_,submission_};
        return matches(copy,device)?copy:OwnerCopyCompletion{};
    }
    void completed(VkDevice device,const OwnerCopyCompletion& copy,VkResult submit,VkResult wait) {
        std::lock_guard lock(mutex_);
        completed_=submit==VK_SUCCESS&&wait==VK_SUCCESS&&matches(copy,device)?copy:OwnerCopyCompletion{};
    }
    bool canRetire(VkDevice device) const {
        std::lock_guard lock(mutex_);
        return matches(completed_,device);
    }
    void uploaded(uint64_t frame) {
        std::lock_guard lock(mutex_);
        ++generation_;
        frame_=frame;queue_=VK_NULL_HANDLE;submission_=0;uncertain_=false;completed_={};
    }
    void invalidate() {
        std::lock_guard lock(mutex_);
        uncertain_=true;completed_={};
    }
};

class OwnerQueueAccess {
    void (*unlock_)(){};
public:
    OwnerQueueAccess(void(*lock)(),void(*unlock)()):unlock_(lock&&unlock?unlock:nullptr) {
        if(unlock_)lock();
    }
    ~OwnerQueueAccess(){if(unlock_)unlock_();}
    OwnerQueueAccess(const OwnerQueueAccess&)=delete;
    OwnerQueueAccess& operator=(const OwnerQueueAccess&)=delete;
    explicit operator bool() const {return unlock_!=nullptr;}
};
}

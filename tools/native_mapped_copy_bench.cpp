// Explicit standalone Vulkan memory test; never creates a window or OpenXR session.
#define NOMINMAX
#include <windows.h>
#include <vulkan/vulkan.h>
#include "../src/native/NativeMappedCopy.h"
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <array>
#include <algorithm>

static void check(VkResult result) { if(result!=VK_SUCCESS) throw std::runtime_error("Vulkan call failed"); }
int main(int argc,char** argv) try {
    auto dll=LoadLibraryW(L"vulkan-1.dll");if(!dll) return 1;
#define PROC(name) auto name=reinterpret_cast<PFN_##name>(GetProcAddress(dll,#name));if(!name) return 2
    PROC(vkCreateInstance);PROC(vkEnumeratePhysicalDevices);PROC(vkGetPhysicalDeviceProperties);
    PROC(vkGetPhysicalDeviceMemoryProperties);PROC(vkGetPhysicalDeviceQueueFamilyProperties);
    PROC(vkCreateDevice);PROC(vkCreateBuffer);PROC(vkGetBufferMemoryRequirements);
    PROC(vkAllocateMemory);PROC(vkBindBufferMemory);PROC(vkMapMemory);PROC(vkUnmapMemory);
    PROC(vkDestroyBuffer);PROC(vkFreeMemory);PROC(vkDestroyDevice);PROC(vkDestroyInstance);
#undef PROC
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};VkInstance instance{};check(vkCreateInstance(&ci,nullptr,&instance));
    uint32_t count{};check(vkEnumeratePhysicalDevices(instance,&count,nullptr));std::vector<VkPhysicalDevice> devices(count);check(vkEnumeratePhysicalDevices(instance,&count,devices.data()));
    constexpr size_t bytes=4*1024*1024;
    std::vector<unsigned char> pattern(bytes),output(bytes);for(size_t i=0;i<bytes;++i)pattern[i]=static_cast<unsigned char>((i*97)^(i>>3));
    for(auto physical:devices){
        VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(physical,&properties);
        VkPhysicalDeviceMemoryProperties memory{};vkGetPhysicalDeviceMemoryProperties(physical,&memory);
        uint32_t familyCount{};vkGetPhysicalDeviceQueueFamilyProperties(physical,&familyCount,nullptr);std::vector<VkQueueFamilyProperties> families(familyCount);vkGetPhysicalDeviceQueueFamilyProperties(physical,&familyCount,families.data());
        uint32_t family{};while(family<familyCount&&!(families[family].queueFlags&VK_QUEUE_GRAPHICS_BIT))++family;if(family==familyCount)continue;
        float priority=1;VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;
        VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;VkDevice device{};check(vkCreateDevice(physical,&di,nullptr,&device));
        std::printf("GPU=%s bytes=%zu SSE41=%d\n",properties.deviceName,bytes,kharvox::native::streamingMappedCopyAvailable());
        for(uint32_t type=0;type<memory.memoryTypeCount;++type){
            auto flags=memory.memoryTypes[type].propertyFlags;
            constexpr auto needed=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;if((flags&needed)!=needed)continue;
            VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=bytes;bi.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            VkBuffer buffer{};check(vkCreateBuffer(device,&bi,nullptr,&buffer));VkMemoryRequirements requirements{};vkGetBufferMemoryRequirements(device,buffer,&requirements);
            if(!(requirements.memoryTypeBits&(1u<<type))){vkDestroyBuffer(device,buffer,nullptr);continue;}
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=requirements.size;ai.memoryTypeIndex=type;VkDeviceMemory allocation{};check(vkAllocateMemory(device,&ai,nullptr,&allocation));check(vkBindBufferMemory(device,buffer,allocation,0));
            void* mapped{};check(vkMapMemory(device,allocation,0,bytes,0,&mapped));std::memcpy(mapped,pattern.data(),bytes);_mm_mfence();
            MEMORY_BASIC_INFORMATION info{};VirtualQuery(mapped,&info,sizeof(info));

            VkDeviceMemory uploadMemory{};VkBuffer uploadBuffer{};void* upload{};
            check(vkCreateBuffer(device,&bi,nullptr,&uploadBuffer));
            VkMemoryRequirements uploadRequirements{};vkGetBufferMemoryRequirements(device,uploadBuffer,&uploadRequirements);
            uint32_t uploadType{};while(uploadType<memory.memoryTypeCount&&(!(uploadRequirements.memoryTypeBits&(1u<<uploadType))||(memory.memoryTypes[uploadType].propertyFlags&needed)!=needed))++uploadType;
            if(uploadType==memory.memoryTypeCount)throw std::runtime_error("No coherent upload type");
            ai.allocationSize=uploadRequirements.size;ai.memoryTypeIndex=uploadType;check(vkAllocateMemory(device,&ai,nullptr,&uploadMemory));check(vkBindBufferMemory(device,uploadBuffer,uploadMemory,0));check(vkMapMemory(device,uploadMemory,0,bytes,0,&upload));
            for(bool stream:{false,true}){
                if(stream&&!kharvox::native::streamingMappedCopyAvailable())continue;
                double elapsed{};
                for(int iteration=0;iteration<4;++iteration){
                    auto begin=std::chrono::steady_clock::now();kharvox::native::copyMappedInput(output.data(),mapped,bytes,stream);auto end=std::chrono::steady_clock::now();
                    if(output!=pattern)throw std::runtime_error("Mapped copy differs from CPU-written source");
                    if(iteration)elapsed+=std::chrono::duration<double,std::milli>(end-begin).count();
                }
                std::printf("type=%u flags=0x%x protect=0x%lx mode=%s averageMs=%.3f bytesVerified=true\n",type,flags,info.Protect,stream?"stream":"memcpy",elapsed/3);
                for(bool staged:{false,true}){
                    double span{};
                    for(int iteration=0;iteration<3;++iteration){
                        auto start=std::chrono::steady_clock::now();
                        if(staged){kharvox::native::copyMappedInput(output.data(),mapped,bytes,stream);std::memcpy(upload,output.data(),bytes);_mm_sfence();}
                        else kharvox::native::copyMappedInput(upload,mapped,bytes,stream);
                        auto end=std::chrono::steady_clock::now();span+=std::chrono::duration<double,std::milli>(end-start).count();
                        kharvox::native::copyMappedInput(output.data(),upload,bytes);
                        if(output!=pattern)throw std::runtime_error("Coherent upload differs from source");
                    }
                    std::printf("type=%u uploadType=%u mode=%s staged=%d averageMs=%.3f uploadBytesVerified=true\n",type,uploadType,stream?"stream":"memcpy",staged,span/3);
                }
            }
            vkUnmapMemory(device,uploadMemory);vkDestroyBuffer(device,uploadBuffer,nullptr);vkFreeMemory(device,uploadMemory,nullptr);
            vkUnmapMemory(device,allocation);vkDestroyBuffer(device,buffer,nullptr);vkFreeMemory(device,allocation,nullptr);
        }
        vkDestroyDevice(device,nullptr);
    }
    vkDestroyInstance(instance,nullptr);FreeLibrary(dll);return 0;
} catch(const std::exception& error){std::fprintf(stderr,"%s\n",error.what());return 3;}

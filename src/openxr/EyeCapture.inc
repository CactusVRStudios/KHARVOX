// Opt-in diagnostic. Copies acquired XR images in the existing owner command
// buffer; host access and destruction require successful owner completion.
bool eyeCaptureEnabled(){static const bool enabled=[] {char value[8]{};return GetEnvironmentVariableA("KHARVOX_CAPTURE_EYES",value,sizeof(value))==1&&value[0]=='1';}();return enabled;}
struct EyeCaptureState {
    VkBuffer buffer{}; VkDeviceMemory memory{}; void* mapped{};
    std::array<VkExtent2D,2> extents{}; std::array<VkDeviceSize,2> offsets{};
    bool keyDown{},requested{},pending{}; uint64_t due{}; unsigned count{};
} eyeCapture;
// Raw DOOM source capture precedes KHARVOX hands, cache, crop and FSR.
// Buffers survive alternating Presents; only proven completed buffers may retire.
struct EyeSourceCapture {
    VkBuffer buffer{}; VkDeviceMemory memory{}; void* mapped{};
    VkExtent2D extent{}; VkFormat format{}; VkDeviceSize bytes{};
    uint64_t revision{}, frame{}, generation{}; VkImage cache{}; int aa{-1}; bool completed{}, valid{};
};
std::array<EyeSourceCapture,2> eyeSourceCapture{};
std::array<std::array<int32_t,4>,2> eyeCaptureCrop{};
std::array<bool,2> eyeCaptureFsr{};
std::array<const char*,2> eyeCaptureTransfer{"unknown","unknown"};
bool eyeCaptureSourcesMatched{};
int readCaptureAa() {
    const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uintptr_t name{}; SIZE_T count{}; char text[15]{}; int value{};
    auto read=[&](uintptr_t address,void* target,size_t bytes){return ReadProcessMemory(
        GetCurrentProcess(),reinterpret_cast<const void*>(address),target,bytes,&count)&&count==bytes;};
    if(!read(base+0x66dd200+0x50,&name,sizeof(name))
        ||!read(name,text,sizeof(text))||std::memcmp(text,"r_antialiasing",14)
        ||!read(base+0x66dd200+0x30,&value,sizeof(value)))return -1;
    return value;
}
void releaseEyeSource(EyeSourceCapture& c) {
    if(c.valid&&!c.completed)return; // Retain on unproven GPU completion.
    if(c.mapped)s.vk.unmapMemory(s.device,c.memory);
    if(c.buffer)s.vk.destroyBuffer(s.device,c.buffer,nullptr);
    if(c.memory)s.vk.freeMemory(s.device,c.memory,nullptr);
    c={};
}
bool recordEyeSource(int eye,VkImage source,VkImageLayout layout,VkExtent2D extent,
                     VkFormat format,uint64_t revision,uint32_t sourceLayer=0) {
    if(!eyeCapture.requested||GetTickCount64()<eyeCapture.due)return false;
    auto& c=eyeSourceCapture[eye];
    if(c.valid&&!c.completed)return false;
    const VkDeviceSize bytes=VkDeviceSize(extent.width)*extent.height*4;
    const bool supported=format==VK_FORMAT_R8G8B8A8_UNORM||format==VK_FORMAT_R8G8B8A8_SRGB
        ||format==VK_FORMAT_B8G8R8A8_UNORM||format==VK_FORMAT_B8G8R8A8_SRGB;
    if(!supported||!bytes||bytes>128ull*1024*1024)return false;
    auto copy=reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(s.vk.getDeviceProcAddr(s.device,"vkCmdCopyImageToBuffer"));
    if(!copy||!s.vk.mapMemory||!s.vk.createBuffer||!s.vk.getPhysicalDeviceMemoryProperties)return false;
    if(c.buffer&&(c.bytes!=bytes||c.format!=format))releaseEyeSource(c);
    if(!c.buffer){
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=bytes;bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if(s.vk.createBuffer(s.device,&bi,nullptr,&c.buffer)!=VK_SUCCESS)return false;
        VkMemoryRequirements req{};s.vk.getBufferMemoryRequirements(s.device,c.buffer,&req);
        VkPhysicalDeviceMemoryProperties props{};s.vk.getPhysicalDeviceMemoryProperties(s.physical,&props);
        uint32_t type=UINT32_MAX;
        for(uint32_t i=0;i<props.memoryTypeCount;++i)if((req.memoryTypeBits&(1u<<i))
            &&(props.memoryTypes[i].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            ==(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){type=i;break;}
        auto failed=[&]{releaseEyeSource(c);log("[EYE-CAPTURE] raw source allocation unavailable");return false;};
        if(type==UINT32_MAX)return failed();
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=type;
        if(s.vk.allocateMemory(s.device,&ai,nullptr,&c.memory)!=VK_SUCCESS
            ||s.vk.bindBufferMemory(s.device,c.buffer,c.memory,0)!=VK_SUCCESS
            ||s.vk.mapMemory(s.device,c.memory,0,VK_WHOLE_SIZE,0,&c.mapped)!=VK_SUCCESS)return failed();
    }
    c.extent=extent;c.bytes=bytes;c.format=format;c.revision=revision;c.frame=s.frame;
    c.generation=KharvoxCameraLevelTransitionGeneration();c.cache=s.stereoCache[eye];
    c.aa=readCaptureAa();c.completed=false;c.valid=true;
    barrierAspect(s.commandBuffer,source,layout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_IMAGE_ASPECT_COLOR_BIT,sourceLayer);
    VkBufferImageCopy region{};region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,sourceLayer,1};
    region.imageExtent={extent.width,extent.height,1};
    copy(s.commandBuffer,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,c.buffer,1,&region);
    barrierAspect(s.commandBuffer,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,layout,
        VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_IMAGE_ASPECT_COLOR_BIT,sourceLayer);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    s.vk.cmdPipelineBarrier(s.commandBuffer,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
    return true;
}
void pollEyeCapture(){
    if(!eyeCaptureEnabled())return;
    static const bool automatic=[] {char value[8]{};return GetEnvironmentVariableA("KHARVOX_SFS_CAPTURE_ONCE",value,sizeof(value))==1&&value[0]=='1';}();
    if(automatic&&kharvox::sfs::vrEnabled()&&!s.quadMode&&KharvoxCameraGameplayActive()
        &&!eyeCapture.count&&!eyeCapture.requested&&!eyeCapture.pending){
        eyeCapture.requested=true;eyeCapture.due=GetTickCount64()+2000;
        log("[EYE-CAPTURE] automatic SFS diagnostic armed");
    }
    const bool down=(GetAsyncKeyState(VK_CONTROL)&0x8000)&&(GetAsyncKeyState(VK_SHIFT)&0x8000)&&(GetAsyncKeyState('P')&0x8000);
    DWORD pid{};GetWindowThreadProcessId(GetForegroundWindow(),&pid);
    if(down&&!eyeCapture.keyDown&&pid==GetCurrentProcessId()&&!eyeCapture.requested&&eyeCapture.count<8){
        eyeCaptureSourcesMatched=false;
        for(auto& c:eyeSourceCapture)if(c.completed)c.valid=false;
        eyeCapture.requested=true;eyeCapture.due=GetTickCount64()+2000;
        log("[EYE-CAPTURE] armed; capture in two seconds at next completed gameplay pair");
    }
    eyeCapture.keyDown=down;
}
void releaseEyeCapture(){
    auto& c=eyeCapture;
    if(c.mapped)s.vk.unmapMemory(s.device,c.memory);
    if(c.buffer)s.vk.destroyBuffer(s.device,c.buffer,nullptr);
    if(c.memory)s.vk.freeMemory(s.device,c.memory,nullptr);
    c.mapped=nullptr;c.buffer={};c.memory={};c.pending=false;
}
bool recordEyeCapture(const std::array<uint32_t,2>& indices){
    auto& c=eyeCapture;
    if(!c.requested||c.pending||GetTickCount64()<c.due)return false;
    c.requested=false;
    const auto format=VkFormat(s.format);
    if(format!=VK_FORMAT_B8G8R8A8_SRGB&&format!=VK_FORMAT_B8G8R8A8_UNORM
        &&format!=VK_FORMAT_R8G8B8A8_SRGB&&format!=VK_FORMAT_R8G8B8A8_UNORM){log("[EYE-CAPTURE] unsupported format; skipped");return false;}
    auto copy=reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(s.vk.getDeviceProcAddr(s.device,"vkCmdCopyImageToBuffer"));
    if(!copy||!s.vk.createBuffer||!s.vk.mapMemory||!s.vk.getPhysicalDeviceMemoryProperties){log("[EYE-CAPTURE] readback dispatch unavailable");return false;}
    VkDeviceSize size{};
    for(int e=0;e<2;++e){c.extents[e]={s.eyes[e].width,s.eyes[e].height};c.offsets[e]=size;size+=VkDeviceSize(c.extents[e].width)*c.extents[e].height*4;}
    if(!size||size>512ull*1024*1024){log("[EYE-CAPTURE] pair exceeds 512 MiB readback budget; skipped");return false;}
    auto failed=[&]{releaseEyeCapture();log("[EYE-CAPTURE] allocation failed; capture skipped");return false;};
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=size;bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if(s.vk.createBuffer(s.device,&bi,nullptr,&c.buffer)!=VK_SUCCESS)return failed();
    VkMemoryRequirements req{};s.vk.getBufferMemoryRequirements(s.device,c.buffer,&req);
    VkPhysicalDeviceMemoryProperties props{};s.vk.getPhysicalDeviceMemoryProperties(s.physical,&props);
    uint32_t type=UINT32_MAX;
    for(uint32_t i=0;i<props.memoryTypeCount;++i)if((req.memoryTypeBits&(1u<<i))&&(props.memoryTypes[i].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){type=i;break;}
    if(type==UINT32_MAX)return failed();
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=type;
    if(s.vk.allocateMemory(s.device,&ai,nullptr,&c.memory)!=VK_SUCCESS||s.vk.bindBufferMemory(s.device,c.buffer,c.memory,0)!=VK_SUCCESS||s.vk.mapMemory(s.device,c.memory,0,VK_WHOLE_SIZE,0,&c.mapped)!=VK_SUCCESS)return failed();
    for(int e=0;e<2;++e){
        auto img=s.eyes[e].images[indices[e]].image;
        barrier(s.commandBuffer,img,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};region.bufferOffset=c.offsets[e];region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.imageExtent={c.extents[e].width,c.extents[e].height,1};
        copy(s.commandBuffer,img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,c.buffer,1,&region);
        barrier(s.commandBuffer,img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT);
    }
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    s.vk.cmdPipelineBarrier(s.commandBuffer,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
    c.pending=true;return true;
}
void finishEyeCapture(const std::array<XrCompositionLayerProjectionView,2>& views,XrTime displayTime,bool native){
    auto& c=eyeCapture;if(!c.pending)return;
    const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    try{
        wchar_t temp[MAX_PATH]{};if(!GetTempPathW(MAX_PATH,temp))throw std::runtime_error("temp path unavailable");
        const auto dir=std::filesystem::path(temp)/L"KHARVOX-EyeCaptures"/(std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(dir);
        std::ofstream meta(dir/L"capture.json");
        meta<<"{\"schema\":2,\"renderer\":\""<<(kharvox::sfs::vrEnabled()?"Vulkan SFS":native?"Native":"AER")<<"\",\"frame\":"<<s.frame<<",\"predictedDisplayTime\":"<<displayTime<<",\"beforeXrEndFrame\":true,\"gpuCompletionVerified\":true,\"compositorOutput\":false,\"alphaForcedOpaque\":true,\"separateHudLaserLayersIncluded\":false,\"eyes\":[";
        bool success=true;
        for(int e=0;e<2;++e){
            const auto extent=c.extents[e];const size_t bytes=size_t(extent.width)*extent.height*4;
            std::vector<BYTE> pixels(bytes);std::memcpy(pixels.data(),static_cast<BYTE*>(c.mapped)+c.offsets[e],bytes);
            const bool rgba=s.format==VK_FORMAT_R8G8B8A8_SRGB||s.format==VK_FORMAT_R8G8B8A8_UNORM;
            for(size_t i=0;i<bytes;i+=4){if(rgba)std::swap(pixels[i],pixels[i+2]);pixels[i+3]=255;}
            success=kharvox::writeEyePng(dir/(e?L"right.png":L"left.png"),extent.width,extent.height,pixels.data())&&success;
            const auto& source=eyeSourceCapture[e];
            if(eyeCaptureSourcesMatched&&source.completed){
                std::vector<BYTE> raw(static_cast<size_t>(source.bytes));std::memcpy(raw.data(),source.mapped,raw.size());
                const bool sourceRgba=source.format==VK_FORMAT_R8G8B8A8_UNORM||source.format==VK_FORMAT_R8G8B8A8_SRGB;
                for(size_t i=0;i<raw.size();i+=4){if(sourceRgba)std::swap(raw[i],raw[i+2]);raw[i+3]=255;}
                success=kharvox::writeEyePng(dir/(e?L"source_right.png":L"source_left.png"),source.extent.width,source.extent.height,raw.data())&&success;
            }
            const auto& v=views[e];const auto& q=v.pose.orientation;const auto& p=v.pose.position;const auto& f=v.fov;const auto& r=v.subImage.imageRect;
            if(e)meta<<',';
            meta<<"{\"sourceMatched\":"<<(eyeCaptureSourcesMatched&&source.completed?"true":"false")
                <<",\"sourceWidth\":"<<source.extent.width<<",\"sourceHeight\":"<<source.extent.height
                <<",\"sourceVkFormat\":"<<source.format<<",\"sourceFrame\":"<<source.frame
                <<",\"sourceCacheRevision\":"<<source.revision<<",\"sourceAA\":"<<source.aa
                <<",\"transfer\":\""<<eyeCaptureTransfer[e]<<"\",\"sourceBeforeKharvoxHands\":true,\"fsrApplied\":"<<(eyeCaptureFsr[e]?"true":"false")
                <<",\"sourceCropXYXY\":["<<eyeCaptureCrop[e][0]<<','<<eyeCaptureCrop[e][1]<<','<<eyeCaptureCrop[e][2]<<','<<eyeCaptureCrop[e][3]<<"]"
                <<",\"eye\":"<<e<<",\"width\":"<<extent.width<<",\"height\":"<<extent.height<<",\"vkFormat\":"<<s.format<<",\"rect\":["<<r.offset.x<<','<<r.offset.y<<','<<r.extent.width<<','<<r.extent.height<<"],\"position\":["<<p.x<<','<<p.y<<','<<p.z<<"],\"orientation\":["<<q.x<<','<<q.y<<','<<q.z<<','<<q.w<<"],\"fovRadians\":["<<f.angleLeft<<','<<f.angleRight<<','<<f.angleUp<<','<<f.angleDown<<"]}";
        }
        meta<<"]}";meta.close();
        log(std::string("[EYE-CAPTURE] ")+(success&&meta?"saved ":"write failed/partial ")+dir.string()+" (capture hitch is not a performance measurement)");
    }catch(const std::exception& ex){log(std::string("[EYE-CAPTURE] failed: ")+ex.what());}
    if(SUCCEEDED(com))CoUninitialize();
    ++c.count;releaseEyeCapture();
    for(auto& source:eyeSourceCapture)releaseEyeSource(source);
}

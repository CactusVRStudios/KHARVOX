// Included inside the bootstrap namespace. One static compositor image is
// uploaded during session setup; opening Pause never decodes or uploads it.
bool loadPauseBindingsPixels(std::vector<unsigned char>& pixels, UINT& width, UINT& height) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = false;
    {
        using Microsoft::WRL::ComPtr;
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        const auto path = kharvox::runtimePath(L"assets\\branding\\quest-controller-mapping.png");
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(factory.GetAddressOf())))
            && SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                WICDecodeMetadataCacheOnLoad, &decoder))
            && SUCCEEDED(decoder->GetFrame(0, &frame))
            && SUCCEEDED(factory->CreateFormatConverter(&converter))
            && SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))
            && SUCCEEDED(converter->GetSize(&width, &height))
            && width && height && width <= 4096 && height <= 4096) {
            pixels.resize(size_t(width) * height * 4);
            ok = SUCCEEDED(converter->CopyPixels(nullptr, width * 4,
                static_cast<UINT>(pixels.size()), pixels.data()));
        }
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return ok;
}

bool initializePauseBindings(const std::vector<int64_t>& formats) {
    if (pauseBindingsReady) return true;
    auto& vk = s.vk;
    if (!vk.createBuffer || !vk.destroyBuffer || !vk.getBufferMemoryRequirements
        || !vk.allocateMemory || !vk.freeMemory || !vk.bindBufferMemory
        || !vk.mapMemory || !vk.unmapMemory || !vk.getPhysicalDeviceMemoryProperties
        || !vk.cmdCopyBufferToImage || !vk.queueWaitIdle) return false;
    constexpr int64_t format = VK_FORMAT_R8G8B8A8_SRGB;
    if (std::find(formats.begin(), formats.end(), format) == formats.end()) return false;
    std::vector<unsigned char> pixels;
    UINT width{}, height{};
    if (!loadPauseBindingsPixels(pixels, width, height)) return false;
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    VkCommandPool pool{};
    bool ok = false;
    // Every failure unwinds resources. Submitted work is completed before
    // freeing its staging buffer/pool or releasing the OpenXR image.
    do {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = pixels.size(); bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (vk.createBuffer(s.device, &bi, nullptr, &buffer) != VK_SUCCESS) break;
        VkMemoryRequirements requirements{};
        vk.getBufferMemoryRequirements(s.device, buffer, &requirements);
        VkPhysicalDeviceMemoryProperties properties{};
        vk.getPhysicalDeviceMemoryProperties(s.physical, &properties);
        uint32_t type = properties.memoryTypeCount;
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
            constexpr auto flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            if ((requirements.memoryTypeBits & (1u << i))
                && (properties.memoryTypes[i].propertyFlags & flags) == flags) { type = i; break; }
        }
        if (type == properties.memoryTypeCount) break;
        VkMemoryAllocateInfo mi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mi.allocationSize = requirements.size; mi.memoryTypeIndex = type;
        if (vk.allocateMemory(s.device, &mi, nullptr, &memory) != VK_SUCCESS
            || vk.bindBufferMemory(s.device, buffer, memory, 0) != VK_SUCCESS) break;
        void* mapped{};
        if (vk.mapMemory(s.device, memory, 0, pixels.size(), 0, &mapped) != VK_SUCCESS) break;
        std::memcpy(mapped, pixels.data(), pixels.size()); vk.unmapMemory(s.device, memory);
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
        ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        ci.format = format; ci.sampleCount = 1; ci.width = width; ci.height = height;
        ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
        if (XR_FAILED(s.createSwapchain(s.session, &ci, &pauseBindings.handle))) break;
        pauseBindings.width = width; pauseBindings.height = height;
        if (!initializeSwapchainImages(pauseBindings)) break;
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        uint32_t index{};
        if (XR_FAILED(acquireSwapchainImage(pauseBindings.handle, &acquire, &index))
            || index >= pauseBindings.images.size()) break;
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = XR_INFINITE_DURATION;
        if (s.waitImage(pauseBindings.handle, &wait) != XR_SUCCESS) break;
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pi.queueFamilyIndex = s.queueFamily;
        if (vk.createCommandPool(s.device, &pi, nullptr, &pool) != VK_SUCCESS) break;
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        VkCommandBuffer command{};
        if (vk.allocateCommandBuffers(s.device, &ai, &command) != VK_SUCCESS) break;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vk.beginCommandBuffer(command, &begin) != VK_SUCCESS) break;
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = pauseBindings.images[index].image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vk.cmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
        VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        vk.cmdCopyBufferToImage(command, buffer, barrier.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
        vk.cmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
        if (vk.endCommandBuffer(command) != VK_SUCCESS) break;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
        {
            QueueAccessScope access;
            if (vk.queueSubmit(s.queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) break;
            if (vk.queueWaitIdle(s.queue) != VK_SUCCESS) break;
        }
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        ok = XR_SUCCEEDED(releaseSwapchainImage(pauseBindings.handle, &release));
    } while (false);
    if (pool) vk.destroyCommandPool(s.device, pool, nullptr);
    if (buffer) vk.destroyBuffer(s.device, buffer, nullptr);
    if (memory) vk.freeMemory(s.device, memory, nullptr);
    if (!ok && pauseBindings.handle) { s.destroySwapchain(pauseBindings.handle); pauseBindings = {}; }
    pauseBindingsReady = ok;
    return ok;
}

// 05_mirrors.inc - mirror framebuffers and images, and the per-eye targets
// 4063 of these lines sit inside #ifdef DOOMVR_RESEARCH_TOOLS,
// which is ON by default - the guard is NOT a shipping/research boundary.
//
// PART OF src/hooks/vulkan_hooks.cpp - NOT a translation unit. It is
// #included, in order, inside `namespace kharvoxnative::vk_hooks { namespace {`.
// The fragments share ONE scope and internal linkage, so declaration order
// matters and the include list must not be reordered.
//
// This fragment begins INSIDE the DOOMVR_RESEARCH_TOOLS guard, so it
// reopens it below and closes it at the end. That is a split artefact,
// not a scope change: the preprocessed output is identical either way,
// and it was verified with the flag both OFF and ON.

#ifdef DOOMVR_RESEARCH_TOOLS

// STORAGE-IMAGE MIRRORING (2026-08-18, Run A / review P3).
//
// WHY THIS EXISTS, and why framebuffer mirroring could never have covered
// it: the mirror set is derived ONLY from framebuffers the doubled call
// begins a render pass on. An image written through a COMPUTE storage
// binding has no framebuffer, so ensure_mirror_framebuffer is never even
// asked about it. That is a property of the mechanism, not a gap in its
// configuration - see docs/P1-CONTAMINATION-MAP-2026-08-18.md.
//
// Run A measured the consequence: every doubled call writes TWO of DOOM's
// own intermediates, one of them a FULL-SCREEN 3818x2032 HDR colour image
// (StorageImageID #1), with shifted-camera content, every frame. That is a
// contamination path into eye 0 at 80 Hz.
//
// Must be called with mirror_mutex HELD. Takes framebuffer_depth_view_mutex
// underneath it, which is the ordering hook_create_image_view already
// establishes (mirror creation calls into it); never the reverse.
//
// Returns nullptr on any failure, and every failure logs - a silent null
// here would be indistinguishable from "DOOM stopped writing that image".
const MirroredView* ensure_mirror_storage_image(VkImageView src_view) {
    if (src_view == VK_NULL_HANDLE) return nullptr;
    if (auto it = mirrored_views.find(src_view); it != mirrored_views.end())
        return &it->second;
    // Resource count alone cannot distinguish a small mip chain from many
    // full-screen HDR targets. Bound actual held bytes and objects below,
    // after querying Vulkan's allocation requirements, before allocation.

    ImageViewInfo vi{};
    CreatedImageInfo ii{};
    bool have_view = false, have_image = false;
    {
        std::scoped_lock lock(framebuffer_depth_view_mutex);
        if (auto v = image_view_infos.find(src_view); v != image_view_infos.end()) {
            vi = v->second;
            have_view = true;
            if (auto c = created_image_infos.find(vi.image); c != created_image_infos.end()) {
                ii = c->second;
                have_image = true;
            }
        }
    }
    if (!have_view || !have_image) {
        // Rule 3. Before this build vkCreateImage was not hooked at all, so
        // this was the *normal* case; if it recurs the identity has to come
        // from somewhere else and the mirror cannot be built blind.
        const uint32_t n = storage_mirror_no_info.fetch_add(1, std::memory_order_relaxed);
        if (n < 8) {
            log::error(std::format(
                "StorageMirror NO INFO #{} view={} viewKnown={} imageKnown={} - cannot size or "
                "format a mirror for it, so the doubled call keeps writing DOOM's own image "
                "here. Created before our hooks installed?",
                n, reinterpret_cast<void*>(src_view), have_view, have_image));
        }
        return nullptr;
    }

    const auto& vk = vulkan_state();
    auto create_image = load_device_proc<PFN_vkCreateImage>(vk.device, "vkCreateImage");
    auto get_reqs = load_device_proc<PFN_vkGetImageMemoryRequirements>(vk.device, "vkGetImageMemoryRequirements");
    auto allocate = load_device_proc<PFN_vkAllocateMemory>(vk.device, "vkAllocateMemory");
    auto bind = load_device_proc<PFN_vkBindImageMemory>(vk.device, "vkBindImageMemory");
    auto create_view = load_device_proc<PFN_vkCreateImageView>(vk.device, "vkCreateImageView");
    auto destroy_image = load_device_proc<PFN_vkDestroyImage>(vk.device, "vkDestroyImage");
    auto free_memory = load_device_proc<PFN_vkFreeMemory>(vk.device, "vkFreeMemory");
    if (!create_image || !get_reqs || !allocate || !bind || !create_view || !destroy_image || !free_memory) {
        log::error("StorageMirror ABORT: a required device proc could not be loaded");
        return nullptr;
    }
    VkPhysicalDeviceMemoryProperties mem_props{};
    if (auto gmp = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
            kharvox::native::instanceProc("vkGetPhysicalDeviceMemoryProperties")))
        gmp(vk.physical_device, &mem_props);

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = ii.type;
    ici.format = ii.format;
    ici.extent = ii.extent;
    ici.mipLevels = ii.mips ? ii.mips : 1;
    ici.arrayLayers = ii.layers ? ii.layers : 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    // DOOM's OWN usage flags verbatim, plus transfers so a diagnostic can
    // read the mirror back. Guessing a usage set here would be the same class
    // of error as guessing SHADER_READ_ONLY for a storage image: the mirror
    // has to be legal everywhere the original is legal, because DOOM's
    // barriers for the original get re-pointed onto it.
    ici.usage = ii.usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img = VK_NULL_HANDLE;
    if (create_image(vk.device, &ici, nullptr, &img) != VK_SUCCESS) {
        log::error(std::format(
            "StorageMirror ABORT: vkCreateImage failed for view={} format={} {}x{} usage={:#x}",
            reinterpret_cast<void*>(src_view), static_cast<int>(ii.format),
            ii.extent.width, ii.extent.height, static_cast<unsigned>(ici.usage)));
        return nullptr;
    }
    VkMemoryRequirements req{};
    get_reqs(vk.device, img, &req);
    const auto budgetKey=reinterpret_cast<uintptr_t>(img);
    if(!storage_mirror_budget.reserve(budgetKey,req.size)){
        log::error(std::format("StorageMirror BUDGET HIT view={} requestedBytes={} heldBytes={} heldImages={} maxBytes={} maxImages={}; Native preservation refused",
            reinterpret_cast<void*>(src_view),req.size,storage_mirror_budget.bytes(),storage_mirror_budget.count(),storage_mirror_budget.maxBytes,storage_mirror_budget.maxCount));
        destroy_image(vk.device,img,nullptr);return nullptr;
    }
    // These objects have not been published, bound in descriptors or submitted.
    VkDeviceMemory mem = VK_NULL_HANDLE;
    auto abandon=[&]{destroy_image(vk.device,img,nullptr);if(mem)free_memory(vk.device,mem,nullptr);storage_mirror_budget.release(budgetKey);};
    uint32_t type_index = UINT32_MAX;
    if (!find_memory_type(mem_props, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                          type_index)) {
        log::error(std::format(
            "StorageMirror ABORT: no device-local memory type for view={} (size={} bytes)",
            reinterpret_cast<void*>(src_view), req.size));
        abandon();
        return nullptr;
    }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size; mai.memoryTypeIndex = type_index;
    VkDeviceMemory allocatedMemory{};
    const auto allocated=allocate(vk.device,&mai,nullptr,&allocatedMemory);
    if(allocated==VK_SUCCESS)mem=allocatedMemory;
    if (allocated != VK_SUCCESS ||
        bind(vk.device, img, mem, 0) != VK_SUCCESS) {
        log::error(std::format(
            "StorageMirror ABORT: allocate/bind failed for view={} size={} bytes",
            reinterpret_cast<void*>(src_view), req.size));
        abandon();
        return nullptr;
    }
    // The SOURCE view's parameters verbatim, so the mirror view matches what
    // the shader's descriptor expects - same reasoning as 6.7a's alias views.
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.flags = vi.flags;
    vci.image = img;
    vci.viewType = vi.view_type;
    vci.format = vi.format;
    vci.components = vi.components;
    vci.subresourceRange = vi.range;
    VkImageView nv = VK_NULL_HANDLE;
    if (create_view(vk.device, &vci, nullptr, &nv) != VK_SUCCESS) {
        log::error(std::format("StorageMirror ABORT: vkCreateImageView failed for view={}",
                               reinterpret_cast<void*>(src_view)));
        abandon();
        return nullptr;
    }
    mirror_bytes_allocated.fetch_add(req.size, std::memory_order_relaxed);
    mirrored_views[src_view] = MirroredView{img, nv, vi.format, mem, req.size,
                                            VkExtent2D{ii.extent.width, ii.extent.height}, ici.mipLevels, ici.arrayLayers};
    // This function creates no framebuffer. The source view owns the mirror;
    // only actual mirrored framebuffer users may contribute references.
    mirrored_view_refs[src_view] = {};
    // Barrier substitution keys on the SOURCE image. Registering here is what
    // makes DOOM's own barriers for this image, inside the doubled call, land
    // on our mirror - which is also how the mirror reaches the right layout
    // without us inventing transitions of our own.
    if (vi.image != VK_NULL_HANDLE) mirrored_images[vi.image] = img;
    // TRANSITION IT OURSELVES, to GENERAL, now.
    //
    // Run B measured that leaving this at UNDEFINED and waiting for barrier
    // substitution does not work: no barrier for these images is ever issued
    // inside the frame root, so the mirror stayed UNDEFINED all run and the
    // safety gate refused every single time (`have=0 want=1`). The
    // substitution was built, shipped, and never fired - the exact failure
    // mode this project keeps paying for.
    //
    // GENERAL is the layout a STORAGE_IMAGE descriptor declares, so this
    // makes the WRITE binding match. The SAMPLED bindings of the same image
    // declare SHADER_READ_ONLY_OPTIMAL (want=5) and will still be refused:
    // one image cannot sit in two layouts at once, and with no in-frame-root
    // transitions to piggyback on there is nowhere legal to flip it (a
    // layout transition inside a render pass is invalid). That half is a
    // known, stated limitation of this step, not an oversight.
    // KHARVOX seeds every mip/layer before the first readable storage bind.
    register_mirror_layouts(img,ici.mipLevels,ici.arrayLayers,VK_IMAGE_LAYOUT_UNDEFINED,vi.format);
    // A new mirror invalidates every cached clone verdict, and convergence
    // deliberately goes LOST for kMirrorStableCalls doubled calls while the
    // set settles. That is the gate doing its job, not a fault.
    mirror_generation.fetch_add(1, std::memory_order_relaxed);
    const uint32_t n = storage_mirror_created.fetch_add(1, std::memory_order_relaxed);
    log::warn(std::format(
        "StorageMirror #{} CREATED for view={} image={} -> mirrorImage={} mirrorView={} "
        "{}x{} format={} usage={:#x} bytes={}. The doubled call's compute now writes OUR image "
        "here instead of DOOM's; framebuffer mirroring could never have reached this one "
        "because a compute storage write has no framebuffer.",
        n, reinterpret_cast<void*>(src_view), reinterpret_cast<void*>(vi.image),
        reinterpret_cast<void*>(img), reinterpret_cast<void*>(nv),
        ii.extent.width, ii.extent.height, static_cast<int>(ii.format),
        static_cast<unsigned>(ici.usage), req.size));
    log::info(std::format("StorageMirror budget heldImages={} heldBytes={} lifetimeCreated={}",
        storage_mirror_budget.count(),storage_mirror_budget.bytes(),n+1));
    return &mirrored_views[src_view];
}

void clear_composite_before_double() {
    if (last_frame_root_cmd_buffer == VK_NULL_HANDLE ||
        last_frame_root_framebuffer == VK_NULL_HANDLE) return;
    VkImage src = VK_NULL_HANDLE;
    {
        std::scoped_lock lock(framebuffer_depth_view_mutex);
        auto fb = framebuffer_color0_views.find(last_frame_root_framebuffer);
        if (fb == framebuffer_color0_views.end()) return;
        auto im = image_view_to_image.find(fb->second);
        if (im == image_view_to_image.end()) return;
        src = im->second;
    }
    if (!src) return;
    const auto& vk = vulkan_state();
    auto cmd_barrier = load_device_proc<PFN_vkCmdPipelineBarrier>(vk.device, "vkCmdPipelineBarrier");
    auto cmd_clear = load_device_proc<PFN_vkCmdClearColorImage>(vk.device, "vkCmdClearColorImage");
    if (!cmd_barrier || !cmd_clear) return;

    auto barrier = [&](VkImageLayout from, VkImageLayout to,
                       VkAccessFlags srcA, VkAccessFlags dstA) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = srcA; b.dstAccessMask = dstA;
        b.oldLayout = from; b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = src;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        cmd_barrier(last_frame_root_cmd_buffer,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    };

    barrier(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    // ORDERING PROBE (2026-08-16): clear to bright GREEN, not black.
    //
    // Two hypotheses explain the failed full-frame test equally well:
    //   (a) DOOM composite accumulates across frames and is not fully
    //       rewritten by a frame-root run;
    //   (b) our recorded blit and clear simply EXECUTE LATER than intended,
    //       after the doubled call, because last_frame_root_cmd_buffer is
    //       not the buffer being recorded at that moment.
    // Both predict eye 1 doubled and eye 0 black, so the black told us
    // nothing. A distinctive clear colour separates them outright:
    //   eye 0 GREEN        -> nothing was drawn after the clear, so the
    //                         clear ran LAST. Hypothesis (b), an ordering
    //                         bug on our side, and the approach is alive.
    //   eye 0 SCENE        -> the clear ran before the doubled call and the
    //                         doubled call drew over it correctly.
    //                         Hypothesis (a).
    VkClearColorValue black{};
    black.float32[0] = 0.0f; black.float32[1] = 1.0f;
    black.float32[2] = 0.0f; black.float32[3] = 1.0f;
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    cmd_clear(last_frame_root_cmd_buffer, src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              &black, 1, &range);
    barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
}

bool create_offscreen_image(VkDevice device, VkPhysicalDeviceMemoryProperties& mem_props,
    PFN_vkCreateImage create_image, PFN_vkGetImageMemoryRequirements get_reqs,
    PFN_vkAllocateMemory allocate_memory, PFN_vkBindImageMemory bind_image_memory,
    VkFormat format, VkExtent2D extent, VkImageUsageFlags usage,
    VkImage& out_image, VkDeviceMemory& out_memory) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {extent.width, extent.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (create_image(device, &info, nullptr, &out_image) != VK_SUCCESS) return false;

    VkMemoryRequirements reqs{};
    get_reqs(device, out_image, &reqs);
    uint32_t type_index = 0;
    if (!find_memory_type(mem_props, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type_index))
        return false;
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = reqs.size;
    alloc.memoryTypeIndex = type_index;
    if (allocate_memory(device, &alloc, nullptr, &out_memory) != VK_SUCCESS) return false;
    if (bind_image_memory(device, out_image, out_memory, 0) != VK_SUCCESS) return false;
    return true;
}

bool create_offscreen_view(VkDevice device, PFN_vkCreateImageView create_view,
    VkImage image, VkFormat format, VkImageAspectFlags aspect, VkImageView& out_view) {
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = format;
    info.subresourceRange = {aspect, 0, 1, 0, 1};
    return create_view(device, &info, nullptr, &out_view) == VK_SUCCESS;
}

// Called once per distinct render pass observed (see offscreen_targets
// capture logic in hook_cmd_begin_render_pass), so up to twice total - once
// per RenderOpaque internal pass. Not SEH-guarded: these are ordinary
// Vulkan API calls against our own valid data, not reads of DOOM's memory,
// so a VkResult check is the right failure signal, not a structured-
// exception one.
//
// real_depth_view: DOOM's own depth VkImageView for this render pass, if
// found via framebuffer_depth_views (see docs/RE-NOTES.md "Depth-prepass
// hypothesis: CONFIRMED" - RenderOpaque's real depth attachment is
// literally RenderDepth's output, and a synthetic/cleared depth image
// almost certainly can't satisfy whatever depth test RenderOpaque's
// opaque-geometry pipelines use). When non-null, reused directly as this
// target's depth attachment instead of allocating/clearing our own -
// falls back to synthetic depth only if this is VK_NULL_HANDLE.
void prepare_offscreen_target(OffscreenTarget& target, VkRenderPass render_pass, VkExtent2D extent,
    VkImageView real_depth_view) {
    const auto& vk = vulkan_state();
    if (!vk.device || !vk.physical_device || !vk.queue || vk.queue_family == UINT32_MAX) {
        log::warn("Offscreen redirect target: Vulkan state incomplete, skipping");
        return;
    }
    auto create_image = load_device_proc<PFN_vkCreateImage>(vk.device, "vkCreateImage");
    auto get_reqs = load_device_proc<PFN_vkGetImageMemoryRequirements>(vk.device, "vkGetImageMemoryRequirements");
    auto allocate_memory = load_device_proc<PFN_vkAllocateMemory>(vk.device, "vkAllocateMemory");
    auto bind_image_memory = load_device_proc<PFN_vkBindImageMemory>(vk.device, "vkBindImageMemory");
    auto create_view = load_device_proc<PFN_vkCreateImageView>(vk.device, "vkCreateImageView");
    auto create_framebuffer_direct = load_device_proc<PFN_vkCreateFramebuffer>(vk.device, "vkCreateFramebuffer");
    auto create_command_pool = load_device_proc<PFN_vkCreateCommandPool>(vk.device, "vkCreateCommandPool");
    auto allocate_command_buffers = load_device_proc<PFN_vkAllocateCommandBuffers>(vk.device, "vkAllocateCommandBuffers");
    auto begin_command_buffer = load_device_proc<PFN_vkBeginCommandBuffer>(vk.device, "vkBeginCommandBuffer");
    auto cmd_pipeline_barrier = load_device_proc<PFN_vkCmdPipelineBarrier>(vk.device, "vkCmdPipelineBarrier");
    auto cmd_clear_depth_stencil = load_device_proc<PFN_vkCmdClearDepthStencilImage>(vk.device, "vkCmdClearDepthStencilImage");
    auto end_command_buffer = load_device_proc<PFN_vkEndCommandBuffer>(vk.device, "vkEndCommandBuffer");
    auto queue_submit = load_device_proc<PFN_vkQueueSubmit>(vk.device, "vkQueueSubmit");
    auto queue_wait_idle = load_device_proc<PFN_vkQueueWaitIdle>(vk.device, "vkQueueWaitIdle");
    auto free_command_buffers = load_device_proc<PFN_vkFreeCommandBuffers>(vk.device, "vkFreeCommandBuffers");
    auto destroy_command_pool = load_device_proc<PFN_vkDestroyCommandPool>(vk.device, "vkDestroyCommandPool");
    PFN_vkGetPhysicalDeviceMemoryProperties get_mem_props = nullptr;
    {
        HMODULE mod = GetModuleHandleW(L"vulkan-1.dll");
        if (mod) get_mem_props = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
            kharvox::native::instanceProc("vkGetPhysicalDeviceMemoryProperties"));
    }
    if (!create_image || !get_reqs || !allocate_memory || !bind_image_memory || !create_view ||
        !create_framebuffer_direct || !create_command_pool || !allocate_command_buffers ||
        !begin_command_buffer || !cmd_pipeline_barrier || !cmd_clear_depth_stencil || !end_command_buffer ||
        !queue_submit || !queue_wait_idle || !free_command_buffers || !destroy_command_pool || !get_mem_props) {
        log::error("Offscreen redirect target: could not load required Vulkan device procs");
        return;
    }

    const bool use_real_depth = real_depth_view != VK_NULL_HANDLE;

    VkPhysicalDeviceMemoryProperties mem_props{};
    get_mem_props(vk.physical_device, &mem_props);

    bool all_ok = true;
    for (size_t i = 0; i < kOffscreenColorFormats.size() && all_ok; ++i) {
        all_ok = create_offscreen_image(vk.device, mem_props, create_image, get_reqs,
            allocate_memory, bind_image_memory, kOffscreenColorFormats[i], extent,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            target.color_images[i], target.color_memory[i]);
    }
    if (all_ok && !use_real_depth) {
        all_ok = create_offscreen_image(vk.device, mem_props, create_image, get_reqs,
            allocate_memory, bind_image_memory, kOffscreenDepthFormat, extent,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            target.depth_image, target.depth_memory);
    }
    if (!all_ok) {
        log::error("Offscreen redirect target: image/memory creation failed");
        return;
    }

    for (size_t i = 0; i < kOffscreenColorFormats.size() && all_ok; ++i) {
        all_ok = create_offscreen_view(vk.device, create_view, target.color_images[i],
            kOffscreenColorFormats[i], VK_IMAGE_ASPECT_COLOR_BIT, target.color_views[i]);
    }
    if (use_real_depth) {
        // Reuse DOOM's real depth data directly - see docs/RE-NOTES.md
        // "Depth-prepass hypothesis: CONFIRMED". We don't own this image and
        // must not transition or clear it below; it's already in whatever
        // layout DOOM's own RenderDepth/RenderOpaque passes leave it in.
        target.depth_view = real_depth_view;
    } else if (all_ok) {
        all_ok = create_offscreen_view(vk.device, create_view, target.depth_image,
            kOffscreenDepthFormat, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
            target.depth_view);
    }
    if (!all_ok) {
        log::error("Offscreen redirect target: image view creation failed");
        return;
    }

    // One-time layout transition for the 3 color images we own: fresh images
    // start VK_IMAGE_LAYOUT_UNDEFINED; the captured render pass declares
    // initialLayout=COLOR_ATTACHMENT_OPTIMAL for them (see docs/RE-NOTES.md),
    // so they must be transitioned once before the render pass ever begins
    // against them. When NOT using DOOM's real depth, the synthetic depth
    // image is also transitioned and explicitly CLEARed here (to 1.0/0) as a
    // fallback - see the comment on real_depth_view above for why this
    // fallback path is expected to be strictly worse, not why it's wrong.
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = vk.queue_family;
    if (create_command_pool(vk.device, &pool_info, nullptr, &pool) != VK_SUCCESS) {
        log::error("Offscreen redirect target: command pool creation failed");
        return;
    }
    VkCommandBufferAllocateInfo cb_alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cb_alloc.commandPool = pool;
    cb_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb_alloc.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    bool transitioned = allocate_command_buffers(vk.device, &cb_alloc, &command) == VK_SUCCESS;
    if (transitioned) {
        VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        begin_command_buffer(command, &begin_info);
        std::array<VkImageMemoryBarrier, 3> to_color_attachment{};
        for (size_t i = 0; i < 3; ++i) {
            to_color_attachment[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            to_color_attachment[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            to_color_attachment[i].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            to_color_attachment[i].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            to_color_attachment[i].image = target.color_images[i];
            to_color_attachment[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        if (use_real_depth) {
            cmd_pipeline_barrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                0, 0, nullptr, 0, nullptr, static_cast<uint32_t>(to_color_attachment.size()),
                to_color_attachment.data());
        } else {
            // Depth goes to TRANSFER_DST_OPTIMAL first (for the clear below),
            // then to DEPTH_STENCIL_ATTACHMENT_OPTIMAL after.
            VkImageMemoryBarrier depth_to_transfer_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            depth_to_transfer_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            depth_to_transfer_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            depth_to_transfer_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            depth_to_transfer_dst.image = target.depth_image;
            depth_to_transfer_dst.subresourceRange = {
                VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
            std::array<VkImageMemoryBarrier, 4> pre{to_color_attachment[0], to_color_attachment[1],
                to_color_attachment[2], depth_to_transfer_dst};
            cmd_pipeline_barrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, nullptr, 0, nullptr, static_cast<uint32_t>(pre.size()), pre.data());
            VkClearDepthStencilValue clear_value{};
            clear_value.depth = 1.0f;
            clear_value.stencil = 0;
            VkImageSubresourceRange depth_range{
                VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
            cmd_clear_depth_stencil(command, target.depth_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                &clear_value, 1, &depth_range);
            VkImageMemoryBarrier depth_to_attachment{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            depth_to_attachment.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            depth_to_attachment.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depth_to_attachment.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            depth_to_attachment.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            depth_to_attachment.image = target.depth_image;
            depth_to_attachment.subresourceRange = depth_range;
            cmd_pipeline_barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                0, 0, nullptr, 0, nullptr, 1, &depth_to_attachment);
        }
        end_command_buffer(command);
        VkSubmitInfo submit_info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &command;
        transitioned = queue_submit(vk.queue, 1, &submit_info, VK_NULL_HANDLE) == VK_SUCCESS;
        if (transitioned) queue_wait_idle(vk.queue);
        free_command_buffers(vk.device, pool, 1, &command);
    }
    destroy_command_pool(vk.device, pool, nullptr);
    if (!transitioned) {
        log::error("Offscreen redirect target: initial layout transition/clear failed");
        return;
    }

    std::array<VkImageView, 4> attachments{target.color_views[0],
        target.color_views[1], target.color_views[2], target.depth_view};
    VkFramebufferCreateInfo fb_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb_info.renderPass = render_pass;
    fb_info.attachmentCount = static_cast<uint32_t>(attachments.size());
    fb_info.pAttachments = attachments.data();
    fb_info.width = extent.width;
    fb_info.height = extent.height;
    fb_info.layers = 1;
    if (create_framebuffer_direct(vk.device, &fb_info, nullptr, &target.framebuffer) != VK_SUCCESS) {
        log::error("Offscreen redirect target: vkCreateFramebuffer against captured RenderOpaque render pass failed");
        return;
    }

    target.render_pass = render_pass;
    target.extent = extent;
    target.ready.store(true, std::memory_order_relaxed);
    log::info(std::format(
        "Offscreen redirect target ready: framebuffer={} renderPass={} extent={}x{} depth={}",
        reinterpret_cast<void*>(target.framebuffer), reinterpret_cast<void*>(render_pass),
        extent.width, extent.height, use_real_depth ? "REAL(RenderDepth)" : "synthetic-fallback"));
}

// --- Offscreen dump: is the redirected content actually correct? ----------
// VK_FORMAT_B10G11R11_UFLOAT_PACK32 unpack per the Khronos spec (same
// layout as OpenGL's GL_R11F_G11F_B10F / D3D's R11G11B10_FLOAT): a 32-bit
// value packing R (bits 0-10, 5-bit exponent + 6-bit mantissa unsigned
// float), G (bits 11-21, same shape as R), B (bits 22-31, 5-bit exponent +
// 5-bit mantissa). General graphics-format knowledge, not DOOM-specific.
float unpack_unsigned_float(uint32_t bits, int mantissa_bits) {
    const uint32_t exponent = (bits >> mantissa_bits) & 0x1Fu;
    const uint32_t mantissa = bits & ((1u << mantissa_bits) - 1u);
    if (exponent == 0) {
        return mantissa == 0 ? 0.0f : std::ldexp(static_cast<float>(mantissa), -14 - mantissa_bits);
    }
    if (exponent == 31) {
        return mantissa ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
    }
    const float normalized = 1.0f + static_cast<float>(mantissa) / static_cast<float>(1u << mantissa_bits);
    return std::ldexp(normalized, static_cast<int>(exponent) - 15);
}

// IEEE 754 binary16 -> float. Handles zero/subnormal, normal, and inf/NaN;
// an exposure buffer that has never been written reads as 0.0 rather than as
// a plausible small number, which is exactly the distinction J1 needs.
float half_to_float(uint16_t h) {
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    const uint32_t mant = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;  // +/- zero
        } else {
            // Subnormal: normalise it.
            uint32_t e = 0;
            uint32_t m = mant;
            while ((m & 0x400u) == 0) { m <<= 1; ++e; }
            m &= 0x3FFu;
            bits = sign | ((127 - 15 - e + 1) << 23) | (m << 13);
        }
    } else if (exp == 0x1Fu) {
        bits = sign | 0x7F800000u | (mant << 13);  // inf / NaN
    } else {
        bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    float out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

void unpack_b10g11r11(uint32_t packed, float& r, float& g, float& b) {
    r = unpack_unsigned_float(packed & 0x7FFu, 6);
    g = unpack_unsigned_float((packed >> 11) & 0x7FFu, 6);
    b = unpack_unsigned_float((packed >> 22) & 0x3FFu, 5);
}

// Reinhard tonemap + approximate gamma - purely for making an HDR value
// visible in an 8-bit file, no correctness claim beyond "roughly viewable."
uint8_t tonemap_to_u8(float linear) {
    if (!std::isfinite(linear) || linear < 0.0f) linear = 0.0f;
    const float mapped = linear / (1.0f + linear);
    const float gamma = std::pow(std::clamp(mapped, 0.0f, 1.0f), 1.0f / 2.2f);
    return static_cast<uint8_t>(std::clamp(gamma, 0.0f, 1.0f) * 255.0f + 0.5f);
}

// Phase 1 (STEREO-IMPLEMENTATION-PLAN.md v2): a minimal 1-attachment
// (depth-only) framebuffer for RenderDepth's own render pass, wrapping an
// already-created depth view (owned by the corresponding stereo_eye_targets
// slot - this function does not create or own the image/view itself).
bool prepare_depth_only_framebuffer(VkRenderPass render_pass, VkExtent2D extent,
    VkImageView depth_view, VkFramebuffer& out) {
    const auto& vk = vulkan_state();
    if (!vk.device || render_pass == VK_NULL_HANDLE || depth_view == VK_NULL_HANDLE) return false;
    auto create_framebuffer_direct = load_device_proc<PFN_vkCreateFramebuffer>(vk.device, "vkCreateFramebuffer");
    if (!create_framebuffer_direct) {
        log::error("Depth-only framebuffer: could not load vkCreateFramebuffer");
        return false;
    }
    VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    info.renderPass = render_pass;
    info.attachmentCount = 1;
    info.pAttachments = &depth_view;
    info.width = extent.width;
    info.height = extent.height;
    info.layers = 1;
    const VkResult result = create_framebuffer_direct(vk.device, &info, nullptr, &out);
    if (result != VK_SUCCESS) {
        log::error(std::format("Depth-only framebuffer: vkCreateFramebuffer failed ({})",
            static_cast<int>(result)));
        return false;
    }
    return true;
}

// PassDrawCountProbe follow-up fix - see stereo_eye_pass1_framebuffers'
// declaration above. Builds a SECOND VkFramebuffer wrapping an ALREADY-READY
// target's existing color/depth VkImageViews, against a DIFFERENT
// (compatible) VkRenderPass - does not create or own any image/view itself,
// same discipline as prepare_depth_only_framebuffer above.
bool prepare_offscreen_target_secondary_framebuffer(const OffscreenTarget& source,
    VkRenderPass render_pass, VkFramebuffer& out) {
    const auto& vk = vulkan_state();
    if (!vk.device || render_pass == VK_NULL_HANDLE) return false;
    if (!source.ready.load(std::memory_order_relaxed)) return false;
    auto create_framebuffer_direct = load_device_proc<PFN_vkCreateFramebuffer>(vk.device, "vkCreateFramebuffer");
    if (!create_framebuffer_direct) {
        log::error("Secondary framebuffer: could not load vkCreateFramebuffer");
        return false;
    }
    std::array<VkImageView, 4> attachments{source.color_views[0], source.color_views[1],
        source.color_views[2], source.depth_view};
    VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    info.renderPass = render_pass;
    info.attachmentCount = static_cast<uint32_t>(attachments.size());
    info.pAttachments = attachments.data();
    info.width = source.extent.width;
    info.height = source.extent.height;
    info.layers = 1;
    const VkResult result = create_framebuffer_direct(vk.device, &info, nullptr, &out);
    if (result != VK_SUCCESS) {
        log::error(std::format("Secondary framebuffer: vkCreateFramebuffer failed ({})",
            static_cast<int>(result)));
        return false;
    }
    return true;
}

// Phase 1 destructive plumbing check (STEREO-IMPLEMENTATION-PLAN.md v2
// §9.2): the first live test showed the GPU camera write firing
// (confirmed, wroteOk=true, opposite signs) but produced a PIXEL-IDENTICAL
// result between eyes and the SAME near-field collapse as before private
// depth existed - consistent with the private depth image sitting at its
// untouched clear value (1.0/far) rather than actually being populated by
// RenderDepth's redirected draws, which would explain BOTH symptoms
// regardless of camera shift (an EQUAL-vs-1.0 test rejects everything not
// exactly at the far plane). This one-shot diagnostic clears eye 1's
// private depth to 0.0 (the opposite extreme from the normal 1.0 far
// clear) immediately before its first use, leaving eye 0 untouched - if
// the redirect and depth consumption are genuinely wired up, eye 1 should
// look DRAMATICALLY different from eye 0 (total blackout or total
// pass-through, depending on compare direction). If it looks identical to
// eye 0 even under this extreme a change, the private depth image isn't
// being consumed by RenderOpaque's fragments at all - a redirect/
// compatibility bug, not a camera-divergence problem.
std::atomic_bool eye1_depth_destructive_clear_done{false};
bool diagnostic_clear_depth_image_to_near(VkImage depth_image) {
    const auto& vk = vulkan_state();
    if (!vk.device || !vk.queue || depth_image == VK_NULL_HANDLE) return false;
    auto create_command_pool = load_device_proc<PFN_vkCreateCommandPool>(vk.device, "vkCreateCommandPool");
    auto allocate_command_buffers = load_device_proc<PFN_vkAllocateCommandBuffers>(vk.device, "vkAllocateCommandBuffers");
    auto begin_command_buffer = load_device_proc<PFN_vkBeginCommandBuffer>(vk.device, "vkBeginCommandBuffer");
    auto cmd_pipeline_barrier = load_device_proc<PFN_vkCmdPipelineBarrier>(vk.device, "vkCmdPipelineBarrier");
    auto cmd_clear_depth_stencil = load_device_proc<PFN_vkCmdClearDepthStencilImage>(vk.device, "vkCmdClearDepthStencilImage");
    auto end_command_buffer = load_device_proc<PFN_vkEndCommandBuffer>(vk.device, "vkEndCommandBuffer");
    auto queue_submit = load_device_proc<PFN_vkQueueSubmit>(vk.device, "vkQueueSubmit");
    auto queue_wait_idle = load_device_proc<PFN_vkQueueWaitIdle>(vk.device, "vkQueueWaitIdle");
    auto free_command_buffers = load_device_proc<PFN_vkFreeCommandBuffers>(vk.device, "vkFreeCommandBuffers");
    auto destroy_command_pool = load_device_proc<PFN_vkDestroyCommandPool>(vk.device, "vkDestroyCommandPool");
    if (!create_command_pool || !allocate_command_buffers || !begin_command_buffer ||
        !cmd_pipeline_barrier || !cmd_clear_depth_stencil || !end_command_buffer ||
        !queue_submit || !queue_wait_idle || !free_command_buffers || !destroy_command_pool) {
        return false;
    }
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = vulkan_state().queue_family;
    if (create_command_pool(vk.device, &pool_info, nullptr, &pool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo cb_alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cb_alloc.commandPool = pool;
    cb_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb_alloc.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    bool ok = allocate_command_buffers(vk.device, &cb_alloc, &command) == VK_SUCCESS;
    if (ok) {
        VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        begin_command_buffer(command, &begin_info);
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier to_transfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        to_transfer.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_transfer.image = depth_image;
        to_transfer.subresourceRange = range;
        cmd_pipeline_barrier(command, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_transfer);
        VkClearDepthStencilValue clear_value{};
        clear_value.depth = 0.0f;  // opposite extreme from the normal 1.0 far clear
        clear_value.stencil = 0;
        cmd_clear_depth_stencil(command, depth_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_value, 1, &range);
        VkImageMemoryBarrier to_attachment{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        to_attachment.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_attachment.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        to_attachment.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_attachment.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        to_attachment.image = depth_image;
        to_attachment.subresourceRange = range;
        cmd_pipeline_barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            0, 0, nullptr, 0, nullptr, 1, &to_attachment);
        end_command_buffer(command);
        VkSubmitInfo submit_info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &command;
        ok = queue_submit(vk.queue, 1, &submit_info, VK_NULL_HANDLE) == VK_SUCCESS;
        if (ok) queue_wait_idle(vk.queue);
        free_command_buffers(vk.device, pool, 1, &command);
    }
    destroy_command_pool(vk.device, pool, nullptr);
    return ok;
}

// Eagerly (not lazily-on-first-redirect, unlike offscreen_targets[]) ensure
// an eye's color target plus a RenderDepth-compatible framebuffer before the
// per-eye reinvoke. Avenue 1 proved the private synthetic-depth route is the
// black-image blocker even for DOOM's literal real call, so both eyes reuse
// the captured real RenderDepth view through prepare_offscreen_target's
// proven use_real_depth path.
bool ensure_stereo_eye_target_ready(int eye) {
    if (stereo_eye_targets[eye].ready.load(std::memory_order_relaxed)) return true;
    if (!offscreen_targets[1].ready.load(std::memory_order_relaxed)) return false;
    prepare_offscreen_target(stereo_eye_targets[eye], offscreen_targets[1].render_pass,
        offscreen_targets[1].extent, offscreen_targets[1].depth_view);
    return stereo_eye_targets[eye].ready.load(std::memory_order_relaxed);
}

bool ensure_stereo_eye_depth_framebuffer_ready(int eye) {
    if (stereo_eye_depth_framebuffer_ready[eye].load(std::memory_order_relaxed)) return true;
    const VkRenderPass pass = render_depth_captured_pass.load(std::memory_order_relaxed);
    if (pass == VK_NULL_HANDLE) return false;
    if (!ensure_stereo_eye_target_ready(eye)) return false;
    if (prepare_depth_only_framebuffer(pass, stereo_eye_targets[eye].extent,
            stereo_eye_targets[eye].depth_view, stereo_eye_depth_framebuffers[eye])) {
        stereo_eye_depth_framebuffer_ready[eye].store(true, std::memory_order_relaxed);
        return true;
    }
    return false;
}

// PassDrawCountProbe follow-up fix - see stereo_eye_pass1_framebuffers'
// declaration above. Eagerly ensures each eye's SECOND framebuffer (against
// pass 1's render pass, wrapping the SAME images stereo_eye_targets[eye]
// already owns) exists, same "prepare before RenderOpaque redirect needs it"
// discipline as ensure_stereo_eye_target_ready.
bool ensure_stereo_eye_pass1_framebuffer_ready(int eye) {
    if (stereo_eye_pass1_framebuffer_ready[eye].load(std::memory_order_relaxed)) return true;
    if (!ensure_stereo_eye_target_ready(eye)) return false;
    if (!offscreen_targets[0].ready.load(std::memory_order_relaxed)) return false;
    if (prepare_offscreen_target_secondary_framebuffer(stereo_eye_targets[eye],
            offscreen_targets[0].render_pass, stereo_eye_pass1_framebuffers[eye])) {
        stereo_eye_pass1_framebuffer_ready[eye].store(true, std::memory_order_relaxed);
        return true;
    }
    return false;
}

// RealCallRedirectProbe - see real_call_redirect_target's declaration
// above. Mirrors ensure_stereo_eye_target_ready/ensure_stereo_eye_pass1_
// framebuffer_ready/ensure_stereo_eye_depth_framebuffer_ready exactly, just
// singular (no eye index) - this target is driven by the real call, camera
// untouched, so there's only ever one of it.
bool ensure_real_call_redirect_target_ready() {
    if (real_call_redirect_target.ready.load(std::memory_order_relaxed)) return true;
    if (!offscreen_targets[1].ready.load(std::memory_order_relaxed)) return false;
    // Round 5 proved the target's private synthetic-depth route black even
    // for DOOM's literal real call. Reuse the already captured real depth
    // view, matching the proven offscreen_targets[] path, so this control
    // isolates that construction choice without changing camera or color.
    prepare_offscreen_target(real_call_redirect_target, offscreen_targets[1].render_pass,
        offscreen_targets[1].extent, offscreen_targets[1].depth_view);
    return real_call_redirect_target.ready.load(std::memory_order_relaxed);
}

bool ensure_real_call_redirect_pass1_ready() {
    if (real_call_redirect_pass1_ready.load(std::memory_order_relaxed)) return true;
    if (!ensure_real_call_redirect_target_ready()) return false;
    if (!offscreen_targets[0].ready.load(std::memory_order_relaxed)) return false;
    if (prepare_offscreen_target_secondary_framebuffer(real_call_redirect_target,
            offscreen_targets[0].render_pass, real_call_redirect_pass1_framebuffer)) {
        real_call_redirect_pass1_ready.store(true, std::memory_order_relaxed);
        return true;
    }
    return false;
}

bool ensure_real_call_redirect_depth_ready() {
    if (real_call_redirect_depth_ready.load(std::memory_order_relaxed)) return true;
    const VkRenderPass pass = render_depth_captured_pass.load(std::memory_order_relaxed);
    if (pass == VK_NULL_HANDLE) return false;
    if (!ensure_real_call_redirect_target_ready()) return false;
    if (prepare_depth_only_framebuffer(pass, real_call_redirect_target.extent,
            real_call_redirect_target.depth_view, real_call_redirect_depth_framebuffer)) {
        real_call_redirect_depth_ready.store(true, std::memory_order_relaxed);
        return true;
    }
    return false;
}

// One-shot: copies an arbitrary colour VkImage to a host-visible staging
// image, decodes it, and writes a plain .ppm file next to doomvr.log (name
// distinguished by label). Not SEH-guarded, same reasoning as
// prepare_offscreen_target - ordinary Vulkan calls against our own data,
// VkResult is the right failure signal. Runs a vkQueueWaitIdle first since
// this is called right after the work that produced the image, before the
// GPU has necessarily finished executing it - a real image needs the actual
// draws done, not just the CPU-side recording call having returned.
//
// 2026-08-17: was `dump_offscreen_color_snapshot(const OffscreenTarget&)`,
// hard-wired to target.color_images[0] and kOffscreenColorFormats[0]. That
// made the SHIFT+Pause A/B instrument unable to see the mirrored
// configuration at all (review headline 2: mirroring returns from
// hook_cmd_begin_render_pass before the legacy offscreen redirect block, so
// offscreen_targets[] is never written and the dump produced stale images
// with no error). Taking a raw (image, extent, format) makes the SAME proven
// readback point at the mirror image the headset actually shows.
// dump_offscreen_color_snapshot below is now a thin wrapper, so every
// existing caller keeps byte-identical behaviour.
//
// SOURCE LAYOUT is assumed COLOR_ATTACHMENT_OPTIMAL. True for the offscreen
// targets (prepare_offscreen_target's initial transition + the render pass
// finalLayout) and equally true for mirror colour images
// (transition_mirror_images_to_color_attachment, plus DOOM's own render
// passes declaring initialLayout/finalLayout=2 on them) - the same
// assumption the live eye-1 presentation blit already makes about this exact
// image at hook_queue_present.
// `write_ppm=false` runs the readback and the BrightCensus line but skips the
// file. H1 censuses every mirror/source pair we hold, and at 4K a PPM per image
// would be hundreds of megabytes per run for numbers we read out of the log.
void dump_image_snapshot(VkImage source_image, VkExtent2D extent, VkFormat format,
                         const char* label,
                         VkImageLayout source_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         bool write_ppm = true) {
    const auto& vk = vulkan_state();
    if (!vk.device || !vk.physical_device || !vk.queue) return;
    if (source_image == VK_NULL_HANDLE) {
        log::error(std::format("Image dump ({}): no source image (VK_NULL_HANDLE)", label));
        return;
    }
    if (extent.width == 0 || extent.height == 0) {
        log::error(std::format(
            "Image dump ({}): zero extent {}x{} - refusing to write a meaningless file",
            label, extent.width, extent.height));
        return;
    }
    // Decoders below. All four supported formats are 32 bits per texel, so
    // the row walk reads one uint32 per pixel in every case.
    const bool packed_hdr = format == VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    const bool bgra8 = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
    const bool rgba8 = format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB;
    // J1 (2026-08-26 run 13). R16G16_SFLOAT is the format of the 59x31 target -
    // 3818/64 x 2032/64, the 1/64 luminance reduction that feeds auto-exposure,
    // and the ONE mirror H2 flagged as not written by the doubled call. It is
    // also 32 bits per texel, so the existing row walk already handles it; only
    // a decoder was missing. Without this the single most interesting target in
    // the set is the one we cannot read.
    const bool rg16f = format == VK_FORMAT_R16G16_SFLOAT;
    // R3 (2026-08-27). THREE OF THE SIX INPUTS TO THE FAILING DISPATCH WERE
    // UNREADABLE. R2 named pass 22's bindings end to end and then could not
    // census half of them: `unsupported format 9` (R8_UNORM) for tex4 and for
    // the half-res input at b3, `unsupported format 76` (R16_SFLOAT) elsewhere,
    // `129` (D24S8) for the depth input. Reasoning about which input starves
    // uavSSDO while three of them have no number at all is guessing, and an
    // input we cannot see is not an input we have cleared.
    //
    // These two are added; DEPTH IS NOT. D24S8 needs a DEPTH-aspect copy and
    // the whole readback path here is COLOR-aspect, so it is a larger change
    // than this run should carry - b2 stays a known hole and is named as one
    // in the census rather than quietly missing.
    //
    // Neither is 32 bits per texel, which the row walk below used to assume,
    // so the fetch is now sized per format.
    const bool r8un = format == VK_FORMAT_R8_UNORM;
    const bool r16f = format == VK_FORMAT_R16_SFLOAT;
    // R4 (2026-08-27). DEPTH, the last unmeasured input - and now the prime
    // suspect rather than a loose end.
    //
    // R3's new decoders showed the two R16_SFLOAT mirrors of the depth
    // downsample chain (1909x1016 and 954x508, written by passes 15/16 of
    // jobRva 0x18733c0) come back SATURATED: the quarter-res mirror reads
    // min=max=mean=1.0000 exactly, against DOOM's 0..0.2295. Those two passes
    // have exactly ONE input between them - `b2 view=0x…dd4ed0`, which is the
    // SAME view pass 22's lighting dispatch binds at b2, and it is the
    // full-res D24S8 depth. A depth downsample that outputs a uniform 1.0 is a
    // downsample of an empty depth buffer - everything at the far plane.
    //
    // So the question "is eye 1's depth mirror correct" is now load-bearing for
    // the saturated f76 chain, for uavSSDO at 18% (SSDO is depth-driven), and
    // plausibly for tex4 at 8.4%. It has been the one hole in the input table
    // for three runs because this readback is COLOUR-aspect throughout.
    //
    // D24S8 is 32 bits per texel like the colour formats, so the row walk needs
    // only a decoder; the copy needs the DEPTH aspect, which is what
    // `aspect` below carries. Stencil is deliberately dropped: the question is
    // the depth distribution, and mixing an 8-bit stencil into the luma would
    // make every reading uninterpretable.
    const bool d24s8 = format == VK_FORMAT_D24_UNORM_S8_UINT;
    const VkImageAspectFlags aspect =
        d24s8 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    if (!packed_hdr && !bgra8 && !rgba8 && !rg16f && !r8un && !r16f && !d24s8) {
        log::error(std::format(
            "Image dump ({}): unsupported format {} - no decoder, refusing to write a file "
            "rather than emit garbage that would be measured as if it were real",
            label, static_cast<int>(format)));
        return;
    }
    const uint32_t texel_bytes = r8un ? 1u : (r16f ? 2u : 4u);
    auto create_image = load_device_proc<PFN_vkCreateImage>(vk.device, "vkCreateImage");
    auto get_reqs = load_device_proc<PFN_vkGetImageMemoryRequirements>(vk.device, "vkGetImageMemoryRequirements");
    auto allocate_memory = load_device_proc<PFN_vkAllocateMemory>(vk.device, "vkAllocateMemory");
    auto bind_image_memory = load_device_proc<PFN_vkBindImageMemory>(vk.device, "vkBindImageMemory");
    auto map_memory = load_device_proc<PFN_vkMapMemory>(vk.device, "vkMapMemory");
    auto unmap_memory = load_device_proc<PFN_vkUnmapMemory>(vk.device, "vkUnmapMemory");
    auto get_subresource_layout = load_device_proc<PFN_vkGetImageSubresourceLayout>(vk.device, "vkGetImageSubresourceLayout");
    auto create_command_pool = load_device_proc<PFN_vkCreateCommandPool>(vk.device, "vkCreateCommandPool");
    auto allocate_command_buffers = load_device_proc<PFN_vkAllocateCommandBuffers>(vk.device, "vkAllocateCommandBuffers");
    auto begin_command_buffer = load_device_proc<PFN_vkBeginCommandBuffer>(vk.device, "vkBeginCommandBuffer");
    auto cmd_pipeline_barrier = load_device_proc<PFN_vkCmdPipelineBarrier>(vk.device, "vkCmdPipelineBarrier");
    auto cmd_copy_image = load_device_proc<PFN_vkCmdCopyImage>(vk.device, "vkCmdCopyImage");
    auto end_command_buffer = load_device_proc<PFN_vkEndCommandBuffer>(vk.device, "vkEndCommandBuffer");
    auto queue_submit = load_device_proc<PFN_vkQueueSubmit>(vk.device, "vkQueueSubmit");
    auto queue_wait_idle = load_device_proc<PFN_vkQueueWaitIdle>(vk.device, "vkQueueWaitIdle");
    auto free_command_buffers = load_device_proc<PFN_vkFreeCommandBuffers>(vk.device, "vkFreeCommandBuffers");
    auto destroy_command_pool = load_device_proc<PFN_vkDestroyCommandPool>(vk.device, "vkDestroyCommandPool");
    auto destroy_image = load_device_proc<PFN_vkDestroyImage>(vk.device, "vkDestroyImage");
    auto free_memory_fn = load_device_proc<PFN_vkFreeMemory>(vk.device, "vkFreeMemory");
    PFN_vkGetPhysicalDeviceMemoryProperties get_mem_props = nullptr;
    {
        HMODULE mod = GetModuleHandleW(L"vulkan-1.dll");
        if (mod) get_mem_props = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
            kharvox::native::instanceProc("vkGetPhysicalDeviceMemoryProperties"));
    }
    if (!create_image || !get_reqs || !allocate_memory || !bind_image_memory || !map_memory ||
        !unmap_memory || !get_subresource_layout || !create_command_pool || !allocate_command_buffers ||
        !begin_command_buffer || !cmd_pipeline_barrier || !cmd_copy_image || !end_command_buffer ||
        !queue_submit || !queue_wait_idle || !free_command_buffers || !destroy_command_pool ||
        !destroy_image || !free_memory_fn || !get_mem_props) {
        log::error("Offscreen dump: could not load required Vulkan device procs");
        return;
    }

    queue_wait_idle(vk.queue);

    VkImage staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    // R4: DEPTH GOES THROUGH A BUFFER, NOT A LINEAR IMAGE.
    //
    // The staging path below creates a LINEAR-tiled image of the source's own
    // format. That is proven for the colour formats and is very nearly never
    // supported for D24_UNORM_S8_UINT - vkCreateImage would simply fail, the
    // dump would log "staging image creation failed", and a live run would come
    // back having measured nothing. vkCmdCopyImageToBuffer has no tiling
    // constraint at all, so the depth case uses a tightly-packed host-visible
    // buffer and feeds the SAME decode loop with rowPitch = width * 4.
    VkBuffer staging_buffer = VK_NULL_HANDLE;
    auto create_buffer_fn = load_device_proc<PFN_vkCreateBuffer>(vk.device, "vkCreateBuffer");
    auto destroy_buffer_fn = load_device_proc<PFN_vkDestroyBuffer>(vk.device, "vkDestroyBuffer");
    auto get_buf_reqs = load_device_proc<PFN_vkGetBufferMemoryRequirements>(
        vk.device, "vkGetBufferMemoryRequirements");
    auto bind_buffer_memory = load_device_proc<PFN_vkBindBufferMemory>(vk.device, "vkBindBufferMemory");
    auto cmd_copy_image_to_buffer = load_device_proc<PFN_vkCmdCopyImageToBuffer>(
        vk.device, "vkCmdCopyImageToBuffer");
    if (d24s8 && (!create_buffer_fn || !destroy_buffer_fn || !get_buf_reqs ||
                  !bind_buffer_memory || !cmd_copy_image_to_buffer)) {
        log::error(std::format(
            "Image dump ({}): depth readback needs vkCmdCopyImageToBuffer and it could not be "
            "loaded - refusing rather than reporting a colour-aspect copy of a depth image",
            label));
        return;
    }
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    // Same format as the source: vkCmdCopyImage needs size-compatible
    // formats and identical is the safest way to satisfy that. LINEAR tiling
    // with TRANSFER_DST on B10G11R11 is already proven live (this is the
    // exact staging image every previous dump used).
    image_info.format = format;
    image_info.extent = {extent.width, extent.height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_LINEAR;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    const VkDeviceSize depth_buffer_size =
        static_cast<VkDeviceSize>(extent.width) * extent.height * 4u;
    VkMemoryRequirements reqs{};
    if (d24s8) {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = depth_buffer_size;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (create_buffer_fn(vk.device, &bci, nullptr, &staging_buffer) != VK_SUCCESS) {
            log::error(std::format("Image dump ({}): depth staging buffer creation failed", label));
            return;
        }
        get_buf_reqs(vk.device, staging_buffer, &reqs);
    } else {
        if (create_image(vk.device, &image_info, nullptr, &staging) != VK_SUCCESS) {
            log::error("Offscreen dump: staging image creation failed");
            return;
        }
        get_reqs(vk.device, staging, &reqs);
    }
    // One teardown for both shapes; vkDestroyImage/vkDestroyBuffer on
    // VK_NULL_HANDLE are legal no-ops, so each early return below stays a
    // single call.
    const auto destroy_staging = [&]() {
        if (staging_buffer != VK_NULL_HANDLE) destroy_buffer_fn(vk.device, staging_buffer, nullptr);
        if (staging != VK_NULL_HANDLE) destroy_image(vk.device, staging, nullptr);
    };
    VkPhysicalDeviceMemoryProperties mem_props{};
    get_mem_props(vk.physical_device, &mem_props);
    uint32_t type_index = 0;
    if (!find_memory_type(mem_props, reqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, type_index)) {
        log::error("Offscreen dump: no host-visible memory type for staging image");
        destroy_staging();
        return;
    }
    VkMemoryAllocateInfo alloc_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc_info.allocationSize = reqs.size;
    alloc_info.memoryTypeIndex = type_index;
    const bool bound = allocate_memory(vk.device, &alloc_info, nullptr, &staging_memory) ==
                           VK_SUCCESS &&
                       (d24s8 ? bind_buffer_memory(vk.device, staging_buffer, staging_memory, 0)
                              : bind_image_memory(vk.device, staging, staging_memory, 0)) ==
                           VK_SUCCESS;
    if (!bound) {
        log::error("Offscreen dump: staging memory alloc/bind failed");
        if (staging_memory) free_memory_fn(vk.device, staging_memory, nullptr);
        destroy_staging();
        return;
    }

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = vk.queue_family;
    bool copied = create_command_pool(vk.device, &pool_info, nullptr, &pool) == VK_SUCCESS;
    if (copied) {
        VkCommandBufferAllocateInfo cb_alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cb_alloc.commandPool = pool;
        cb_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cb_alloc.commandBufferCount = 1;
        VkCommandBuffer command = VK_NULL_HANDLE;
        copied = allocate_command_buffers(vk.device, &cb_alloc, &command) == VK_SUCCESS;
        if (copied) {
            VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            begin_command_buffer(command, &begin_info);
            // The source's finalLayout, per the captured render pass (see
            // docs/RE-NOTES.md), is confirmed COLOR_ATTACHMENT_OPTIMAL -
            // this is what the render pass leaves it in, not an assumption.
            // See the SOURCE LAYOUT note on this function for why that also
            // holds for mirror colour images.
            VkImageMemoryBarrier to_src{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            to_src.oldLayout = source_layout;
            to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            // R4: a depth image is left by DEPTH_STENCIL_ATTACHMENT_WRITE, not
            // COLOR_ATTACHMENT_WRITE. Naming the wrong source access on a
            // depth barrier is the kind of thing that reads back stale data
            // instead of failing loudly.
            to_src.srcAccessMask = d24s8 ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                                         : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            to_src.image = source_image;
            to_src.subresourceRange = {aspect, 0, 1, 0, 1};
            VkImageMemoryBarrier to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_dst.image = staging;
            to_dst.subresourceRange = {aspect, 0, 1, 0, 1};
            // The depth path has no staging IMAGE, so its to_dst barrier would
            // name VK_NULL_HANDLE. Only the source is transitioned there.
            std::array<VkImageMemoryBarrier, 2> pre{to_src, to_dst};
            cmd_pipeline_barrier(command,
                d24s8 ? VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
                      : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                d24s8 ? 1u : static_cast<uint32_t>(pre.size()), pre.data());
            if (d24s8) {
                VkBufferImageCopy bregion{};
                bregion.bufferOffset = 0;
                // 0/0 = tightly packed to the image extent, which is what the
                // decode loop's rowPitch below assumes.
                bregion.bufferRowLength = 0;
                bregion.bufferImageHeight = 0;
                bregion.imageSubresource = {aspect, 0, 0, 1};
                bregion.imageExtent = {extent.width, extent.height, 1};
                cmd_copy_image_to_buffer(command, source_image,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging_buffer, 1, &bregion);
            } else {
                VkImageCopy region{};
                region.srcSubresource = {aspect, 0, 0, 1};
                region.dstSubresource = {aspect, 0, 0, 1};
                region.extent = {extent.width, extent.height, 1};
                cmd_copy_image(command, source_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    staging, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            }
            // Put the source back where we found it. The legacy callers own
            // their targets and never cared, but this now runs against MIRROR
            // images that DOOM's own render passes use next frame with a
            // declared initialLayout - leaving one in TRANSFER_SRC_OPTIMAL
            // would be invalid usage on the very next pass.
            VkImageMemoryBarrier restore{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            restore.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            restore.newLayout = source_layout;
            restore.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            restore.dstAccessMask = d24s8 ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                                          : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            restore.image = source_image;
            restore.subresourceRange = {aspect, 0, 1, 0, 1};
            cmd_pipeline_barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                d24s8 ? VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                      : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                0, 0, nullptr, 0, nullptr, 1, &restore);
            end_command_buffer(command);
            VkSubmitInfo submit_info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit_info.commandBufferCount = 1;
            submit_info.pCommandBuffers = &command;
            copied = queue_submit(vk.queue, 1, &submit_info, VK_NULL_HANDLE) == VK_SUCCESS;
            if (copied) queue_wait_idle(vk.queue);
            free_command_buffers(vk.device, pool, 1, &command);
        }
        destroy_command_pool(vk.device, pool, nullptr);
    }
    if (!copied) {
        log::error("Offscreen dump: copy to staging image failed");
        free_memory_fn(vk.device, staging_memory, nullptr);
        destroy_staging();
        return;
    }

    VkImageSubresource subresource{aspect, 0, 0};
    VkSubresourceLayout layout{};
    if (d24s8) {
        // The buffer copy above is tightly packed, so the decode loop's row
        // walk is width * 4 bytes with no offset.
        layout.offset = 0;
        layout.rowPitch = static_cast<VkDeviceSize>(extent.width) * 4u;
    } else {
        get_subresource_layout(vk.device, staging, &subresource, &layout);
    }
    void* mapped = nullptr;
    if (map_memory(vk.device, staging_memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS || !mapped) {
        log::error("Offscreen dump: mapping staging memory failed");
        free_memory_fn(vk.device, staging_memory, nullptr);
        destroy_staging();
        return;
    }

    std::vector<uint8_t> rgb(static_cast<size_t>(extent.width) * extent.height * 3);
    float min_luma = std::numeric_limits<float>::infinity();
    float max_luma = 0.0f;
    double sum_luma = 0.0;
    // J1 accumulators - raw R/G for R16G16_SFLOAT targets. Kept separate from
    // the luma census because for an exposure buffer the float values are the
    // result and the tonemapped byte histogram is not.
    // N1: where the dark pixels are, not just how many.
    uint32_t dark_min_x = UINT32_MAX, dark_max_x = 0;
    uint32_t dark_min_y = UINT32_MAX, dark_max_y = 0;
    uint64_t dark_sum_x = 0, dark_sum_y = 0;
    // N2 (run 20): a centroid collapses a distribution to one point, and run 19
    // showed that is not enough - `viewTargetMS` had its dark centroid at
    // y=0.243 against eye 0's y=0.543, which says "upper" but not "how sharply"
    // or "with what edge". Band histograms turn that into a profile that can be
    // matched against a pass's known output region. Bands are fractions of the
    // image, so they are directly comparable between targets of different
    // resolutions and between the two eyes.
    constexpr uint32_t kBands = 8;
    uint64_t dark_row[kBands]{}, dark_col[kBands]{};
    float rg_r_min = std::numeric_limits<float>::max();
    float rg_r_max = -std::numeric_limits<float>::max();
    float rg_g_min = std::numeric_limits<float>::max();
    float rg_g_max = -std::numeric_limits<float>::max();
    double rg_r_sum = 0.0, rg_g_sum = 0.0;
    uint64_t rg_finite = 0;
    const auto* base = static_cast<const uint8_t*>(mapped);
    for (uint32_t y = 0; y < extent.height; ++y) {
        const auto* row = base + layout.offset + static_cast<size_t>(y) * layout.rowPitch;
        for (uint32_t x = 0; x < extent.width; ++x) {
            // Sized per format - see texel_bytes. memcpy rather than a cast so
            // a 1- or 2-byte texel cannot be read as an over-aligned uint32.
            uint32_t p = 0;
            std::memcpy(&p, row + static_cast<size_t>(x) * texel_bytes, texel_bytes);
            float r = 0.0f, g = 0.0f, b = 0.0f;
            uint8_t r8 = 0, g8 = 0, b8 = 0;
            if (d24s8) {
                // Low 24 bits are the UNORM depth; the high byte is stencil and
                // is dropped - mixing it into the luma would make every reading
                // uninterpretable. 1.0 = far plane, so a mirror that reads a
                // uniform 1.0 is a depth buffer nothing was drawn into.
                r = g = b = static_cast<float>(p & 0x00FFFFFFu) / 16777215.0f;
                r8 = g8 = b8 = static_cast<uint8_t>(r * 255.0f + 0.5f);
            } else if (r8un) {
                // Single-channel UNORM: censused as grayscale so its luma is on
                // the same scale as every other target's and the MIR/SRC ratio
                // means the same thing.
                r8 = g8 = b8 = static_cast<uint8_t>(p & 0xFFu);
                r = g = b = static_cast<float>(r8) / 255.0f;
            } else if (r16f) {
                const float v = half_to_float(static_cast<uint16_t>(p & 0xFFFFu));
                r = g = b = std::isfinite(v) ? v : 0.0f;
                r8 = g8 = b8 = tonemap_to_u8(r);
            } else if (rg16f) {
                r = half_to_float(static_cast<uint16_t>(p & 0xFFFFu));
                g = half_to_float(static_cast<uint16_t>((p >> 16) & 0xFFFFu));
                b = 0.0f;
                // The raw channel values ARE the measurement for an exposure
                // target; the tonemapped bytes below are only so the shared
                // census arithmetic still has something well-defined to chew.
                if (std::isfinite(r)) {
                    rg_r_min = std::min(rg_r_min, r);
                    rg_r_max = std::max(rg_r_max, r);
                    rg_r_sum += r;
                    ++rg_finite;
                }
                if (std::isfinite(g)) {
                    rg_g_min = std::min(rg_g_min, g);
                    rg_g_max = std::max(rg_g_max, g);
                    rg_g_sum += g;
                }
                r8 = tonemap_to_u8(r);
                g8 = tonemap_to_u8(g);
                b8 = 0;
            } else if (packed_hdr) {
                unpack_b10g11r11(p, r, g, b);
                r8 = tonemap_to_u8(r);
                g8 = tonemap_to_u8(g);
                b8 = tonemap_to_u8(b);
            } else {
                // 8-bit UNORM/SRGB: already display-encoded bytes, so they go
                // straight out - tonemapping them would only destroy contrast.
                // Little-endian word: B8G8R8A8 has B in the low byte,
                // R8G8B8A8 has R there.
                b8 = static_cast<uint8_t>(bgra8 ? (p & 0xFFu) : ((p >> 16) & 0xFFu));
                g8 = static_cast<uint8_t>((p >> 8) & 0xFFu);
                r8 = static_cast<uint8_t>(bgra8 ? ((p >> 16) & 0xFFu) : (p & 0xFFu));
                r = static_cast<float>(r8) / 255.0f;
                g = static_cast<float>(g8) / 255.0f;
                b = static_cast<float>(b8) / 255.0f;
            }
            const float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            min_luma = std::min(min_luma, luma);
            max_luma = std::max(max_luma, luma);
            sum_luma += luma;
            const size_t pixel = (static_cast<size_t>(y) * extent.width + x) * 3;
            rgb[pixel + 0] = r8;
            rgb[pixel + 1] = g8;
            rgb[pixel + 2] = b8;
        }
    }
    unmap_memory(vk.device, staging_memory);
    free_memory_fn(vk.device, staging_memory, nullptr);
    destroy_staging();

    const double mean_luma = sum_luma / (static_cast<double>(extent.width) * extent.height);
    log::info(std::format(
        "Offscreen dump readback stats ({}): image={} {}x{} format={} "
        "minLuma={:.4f} maxLuma={:.4f} meanLuma={:.4f}",
        label, reinterpret_cast<void*>(source_image), extent.width, extent.height,
        static_cast<int>(format), min_luma, max_luma, mean_luma));

    // Phase 0 per-band stats (deleted plan; kept as a same-run sweep harness). Same band and
    // metric definitions as tools/ppmstats.ps1 so magnitudes captured here
    // and PPMs re-analysed later line up exactly: bands are the same
    // absolute row ranges (0-400, 400-800, 800-1200, 1200-1600, 1600-2032),
    // clamped to this image's real height; luma is Rec.601 on the same
    // tonemapped u8 RGB the .ppm is written from; "isolated speckle" is the
    // fraction of pixels below luma 8 whose x-4 and x+4 same-row neighbours
    // are both >=8. One log line per band (5 per dump, not per frame - per
    // docs/HANDOFF-NEXT-SESSION.md do-not-do #3). Not a hot path: this
    // function is called at most once per dump pending, and the sweep is
    // capped at four entries per F1 press.
    {
        constexpr std::array<std::pair<uint32_t, uint32_t>, 5> kBands{{
            {0, 400}, {400, 800}, {800, 1200}, {1200, 1600}, {1600, 2032}}};
        for (const auto& band : kBands) {
            const uint32_t y0 = band.first;
            uint32_t y1 = band.second;
            if (y0 >= extent.height) {
                log::info(std::format(
                    "PhaseStats {} rows {:4}-{:4}  (out of image, height={})",
                    label, y0, band.second, extent.height));
                continue;
            }
            if (y1 > extent.height) y1 = extent.height;
            double luma_sum = 0.0;
            uint64_t samples = 0, dark = 0, isolated = 0;
            for (uint32_t y = y0; y < y1; y += 4) {
                const size_t row_base = static_cast<size_t>(y) * extent.width * 3;
                for (uint32_t x = 4; x + 4 < extent.width; x += 4) {
                    const size_t i = row_base + static_cast<size_t>(x) * 3;
                    const double l = 0.299 * rgb[i + 0] + 0.587 * rgb[i + 1] + 0.114 * rgb[i + 2];
                    luma_sum += l;
                    ++samples;
                    if (l < 8.0) {
                        ++dark;
                        const size_t iL = row_base + static_cast<size_t>(x - 4) * 3;
                        const size_t iR = row_base + static_cast<size_t>(x + 4) * 3;
                        const double lL = 0.299 * rgb[iL + 0] + 0.587 * rgb[iL + 1] + 0.114 * rgb[iL + 2];
                        const double lR = 0.299 * rgb[iR + 0] + 0.587 * rgb[iR + 1] + 0.114 * rgb[iR + 2];
                        if (lL >= 8.0 && lR >= 8.0) ++isolated;
                    }
                }
            }
            const double mean = samples ? luma_sum / static_cast<double>(samples) : 0.0;
            const double dark_pct = samples ? 100.0 * static_cast<double>(dark) / static_cast<double>(samples) : 0.0;
            const double iso_pct = samples ? 100.0 * static_cast<double>(isolated) / static_cast<double>(samples) : 0.0;
            log::info(std::format(
                "PhaseStats {} rows {:4}-{:4}  meanLuma={:6.2f}  dark<8={:6.2f}%  isolated-dark-speckle={:6.3f}%",
                label, y0, y1, mean, dark_pct, iso_pct));
        }
    }

    // BRIGHT-PIXEL CENSUS, IN THE LOG (2026-08-21).
    //
    // px>200 on the finalstage_eye1_graded / finalstage_eye0_control pair is
    // THE established measure for "does eye 1 carry the additive layer", and
    // until now it existed only as an offline PowerShell pass over the PPMs.
    // That made every run's headline number depend on a pile of separate scans
    // after the fact, and on all the PPMs surviving un-overwritten by the next
    // run. Computed here it is log-only and deterministic.
    //
    // ⚠ IDENTICAL SEMANTICS TO tools/ppmbright.ps1, deliberately, so numbers
    // from the two are directly comparable: same integer Rec.601 luma
    // ((299r + 587g + 114b) / 1000) on the same tonemapped u8 RGB the .ppm is
    // written from, same >= threshold test, EVERY pixel scanned rather than
    // subsampled. ppmstats-style subsampling drops ~94% of a sparse spark
    // population and cannot tell a full buffer from an empty one - that
    // reading error already cost this project a measurement.
    //
    // Cost is bounded by construction: once per dump, and dumps are capped by
    // the series counter. It is a linear integer pass over a buffer that has
    // already been read back and is about to be written to disk at 23 MB.
    //
    // THE BBOX IS NOT DECORATION. eye 1 matching eye 0's bright population at
    // the SAME bounding box would mean eye 1 is a copy of eye 0's viewpoint -
    // effects restored, parallax destroyed. A shifted bbox is what says the
    // two are genuinely different viewpoints.
    {
        uint64_t bright200 = 0, bright128 = 0, nonzero = 0, dark30 = 0, luma_sum = 0;
        uint32_t peak = 0;
        uint32_t min_x = 0, max_x = 0, min_y = 0, max_y = 0;
        bool any_bright = false;
        for (uint32_t y = 0; y < extent.height; ++y) {
            const size_t row_base = static_cast<size_t>(y) * extent.width * 3;
            for (uint32_t x = 0; x < extent.width; ++x) {
                const size_t i = row_base + static_cast<size_t>(x) * 3;
                const uint32_t l = (299u * rgb[i + 0] + 587u * rgb[i + 1] +
                                    114u * rgb[i + 2]) / 1000u;
                if (l > 0) ++nonzero;
                luma_sum += l;
                // THE FOG METRIC, and it is the one with the correct sign.
                // Volumetric fog is ADDITIVE: it adds light everywhere and
                // lifts dark regions. Its absence therefore shows up as MORE
                // dark pixels and LOWER mean luma - both measured whole-frame,
                // both present in every frame, neither needing a rare event.
                // N1 (2026-08-26 run 19). dark30 COUNTS dark pixels but never
                // said WHERE they are, and that is the discriminator now. On
                // the full-res HDR target eye 1 carries ~34x the dark pixels of
                // eye 0 (20k -> 686k, ~8.5% of the frame) while meanLuma is only
                // 11% lower. A uniform exposure error would move both together;
                // it does not. So the dark pixels are REGIONS, and their shape
                // separates two very different faults:
                //   contiguous band/corner -> eye 1's render does not COVER
                //                             that area (viewport, scissor,
                //                             clip, or a pass rendering to the
                //                             wrong sub-rect)
                //   scattered frame-wide   -> additive content is MISSING
                //                             everywhere, which is the
                //                             volumetric/glow reading
                // The bbox and centroid cost two compares per dark pixel on a
                // loop that already runs.
                if (l < 30) {
                    ++dark30;
                    if (x < dark_min_x) dark_min_x = x;
                    if (x > dark_max_x) dark_max_x = x;
                    if (y < dark_min_y) dark_min_y = y;
                    if (y > dark_max_y) dark_max_y = y;
                    dark_sum_x += x;
                    dark_sum_y += y;
                    if (extent.width && extent.height) {
                        ++dark_row[std::min<uint32_t>(kBands - 1, y * kBands / extent.height)];
                        ++dark_col[std::min<uint32_t>(kBands - 1, x * kBands / extent.width)];
                    }
                }
                if (l >= 128) ++bright128;
                if (l >= 200) {
                    ++bright200;
                    if (!any_bright) {
                        any_bright = true;
                        min_x = max_x = x;
                        min_y = max_y = y;
                    } else {
                        if (x < min_x) min_x = x;
                        if (x > max_x) max_x = x;
                        if (y < min_y) min_y = y;
                        if (y > max_y) max_y = y;
                    }
                }
            }
        }
        const double total = static_cast<double>(extent.width) * extent.height;
        log::warn(std::format(
            "BrightCensus {} FOG[dark30={} meanLuma={:.2f}] px>200={} ({:.4f}%) px>128={} "
            "nonzero={} peak={} bbox200=[x {}..{} y {}..{}]{}.\n"
            "  ⚠ READ FOG FIRST, NOT px>200. Measured 2026-08-21: whole-frame px>200 has the "
            "WRONG SIGN for volumetric fog - stripping the haze RAISES bright-pixel count "
            "because fog desaturates and darkens bright regions as well as lifting dark ones. "
            "Eye 1 scored 3x the control on px>200 in the very run that proved it had no fog at "
            "all. FOG IS THE CORRECT-SIGN MEASURE: dark30 HIGHER and meanLuma LOWER than the "
            "same-sample eye0 control = the additive atmospheric layer is missing (measured 8 of "
            "8 samples, no exception). px>200 is trustworthy only INSIDE a region known to "
            "contain a discrete effect - a flame, a spark shower - compared same-frame; "
            "whole-frame it is an index, never a verdict. bbox equal to the control's would mean "
            "eye 1 is a copy of eye 0's viewpoint rather than its own.",
            label, dark30,
            total > 0.0 ? static_cast<double>(luma_sum) / total : 0.0,
            bright200, total > 0.0 ? 100.0 * static_cast<double>(bright200) / total : 0.0,
            bright128, nonzero, peak, min_x, max_x, min_y, max_y,
            any_bright ? "" : " (NO pixel reached 200 - the bbox is meaningless, and this is a "
                              "quiet frame, not necessarily a broken one; read the CONTROL)"));
    // N1: the dark-pixel geometry. Compare SRC against MIR of the same target.
    if (dark30 > 0) {
        const double fw = extent.width > 0 ? static_cast<double>(extent.width) : 1.0;
        const double fh = extent.height > 0 ? static_cast<double>(extent.height) : 1.0;
        const double bw = static_cast<double>(dark_max_x - dark_min_x + 1) / fw;
        const double bh = static_cast<double>(dark_max_y - dark_min_y + 1) / fh;
        log::warn(std::format(
            "DarkGeom {} dark30={} ({:.2f}% of frame) bbox=[x {}..{} y {}..{}] "
            "bboxCoverage={:.1f}%x{:.1f}% centroid=({:.3f},{:.3f}) fill={:.1f}%. READING: "
            "compare against the same target's other side. bboxCoverage near 100%x100% with a "
            "centroid near (0.5,0.5) and a LOW fill means the dark pixels are spread over the "
            "whole frame - additive content missing everywhere. A bbox confined to part of the "
            "frame, or a high fill, means eye 1's render does not COVER that area and the fault "
            "is viewport/scissor/clip, not lighting.",
            label, dark30, 100.0 * static_cast<double>(dark30) /
                (fw * fh),
            dark_min_x, dark_max_x, dark_min_y, dark_max_y,
            100.0 * bw, 100.0 * bh,
            static_cast<double>(dark_sum_x) / static_cast<double>(dark30) / fw,
            static_cast<double>(dark_sum_y) / static_cast<double>(dark30) / fh,
            100.0 * static_cast<double>(dark30) /
                (static_cast<double>(dark_max_x - dark_min_x + 1) *
                 static_cast<double>(dark_max_y - dark_min_y + 1))));
        // N2: the profile. Each band is the percentage of THAT band's pixels
        // that are dark, so rows and columns are self-normalising and two
        // targets of different resolution - or the two eyes - line up directly.
        const double band_rows = (fw * fh) / static_cast<double>(kBands);
        std::string rows, cols;
        for (uint32_t b = 0; b < kBands; ++b) {
            if (b) { rows += " "; cols += " "; }
            rows += std::format("{:5.1f}", 100.0 * static_cast<double>(dark_row[b]) / band_rows);
            cols += std::format("{:5.1f}", 100.0 * static_cast<double>(dark_col[b]) / band_rows);
        }
        log::warn(std::format(
            "DarkProfile {} rowsTopToBottom[{}] colsLeftToRight[{}] (% of each band dark). "
            "READING: subtract the SRC profile from the MIR profile of the SAME target. A flat "
            "difference across all bands is content missing everywhere; a difference concentrated "
            "in adjacent bands with a sharp edge is a region, and the band boundary is where the "
            "responsible pass stops writing.",
            label, rows, cols));
    }
    }


    // J1: for an R16G16_SFLOAT target the raw channel statistics ARE the
    // measurement. Compare the SRC and MIR lines of the same target: an
    // exposure buffer eye 1 never receives reads all-zero, which is a
    // different finding from one that receives a wrong value.
    if (rg16f) {
        log::warn(std::format(
            "RG16FStats {} finitePixels={} R[min={:.6g} max={:.6g} mean={:.6g}] "
            "G[min={:.6g} max={:.6g} mean={:.6g}]. THE CONTROL: finitePixels=0 means every texel "
            "was NaN/inf and nothing here may be read. All-zero R and G on the MIR line with a "
            "non-zero SRC line means eye 1's copy of this target was never written with real "
            "content - for the 1/64 luminance reduction that feeds auto-exposure, that would "
            "explain a STABLE multiplicative luminance deficit and an empty bloom pyramid at "
            "once, because the bloom threshold is scaled by exposure.",
            label, rg_finite,
            rg_finite ? rg_r_min : 0.0f, rg_finite ? rg_r_max : 0.0f,
            rg_finite ? rg_r_sum / static_cast<double>(rg_finite) : 0.0,
            rg_finite ? rg_g_min : 0.0f, rg_finite ? rg_g_max : 0.0f,
            rg_finite ? rg_g_sum / static_cast<double>(rg_finite) : 0.0));
    }

    // Census-only: the staging image and its memory were already released at
    // the readback (see the free/destroy pair above), and `rgb` is a host
    // vector, so there is nothing to clean up here - just skip the file.
    if (!write_ppm) return;
    const std::string filename = std::string("doomvr_offscreen_dump_") + label + ".ppm";
    const std::filesystem::path dump_path =
        std::filesystem::u8path(kharvox::logPathA(filename.c_str()));
    FILE* file = nullptr;
    if (_wfopen_s(&file, dump_path.c_str(), L"wb") == 0 && file) {
        std::fprintf(file, "P6\n%u %u\n255\n", extent.width, extent.height);
        std::fwrite(rgb.data(), 1, rgb.size(), file);
        std::fclose(file);
        log::info("Offscreen dump written: " + dump_path.string());
    } else {
        log::error("Offscreen dump: could not open output file for writing");
    }
}
// ------------------- H1: PER-TARGET DIVERGENCE, EVERY MIRROR PAIR --------
// Runs 5/9/10 removed three confirmed divergences (dt=0, the drained bounds
// count, the binding-2 duplicate) and eye 1's deficit did not move once:
// meanLuma ~81%, dark30 ~111%, stable across ten runs. Chasing jobs one at a
// time is not converging, and the stability of those numbers says the missing
// contribution is large and consistent.
//
// So stop guessing which pass matters. We hold a mirror image for each of eye
// 1's render targets and we know its DOOM source. Censusing EVERY pair at the
// same moment gives a per-target divergence map in one shot: the targets where
// the mirror is dark against its source name the pass that is missing, with no
// job guessing at all.
//
// BROAD by request: every pair we hold, not a hand-picked subset. Scoping
// assumptions have cost this investigation several runs.
//
// WHERE: hook_queue_present, on the present thread AFTER DOOM's submit - the
// same point the final-stage dumps use, and for the same reason (dumping
// inside the hook returned all zeroes because the pass had only been RECORDED).
//
// COST, and it is real: each census is a full-resolution readback. This run
// will hitch for several seconds. Capped at kH1MaxPairs pairs and run ONCE.
// write_ppm=false - at 4K a file per image would be hundreds of MB for numbers
// we read out of the log.
//
// CONTROLS:
//   * pairs are logged with extent/format/type BEFORE any census, so a pair
//     skipped for an untrackable layout is visible rather than silently absent;
//   * images whose tracked layout is UNDEFINED/PREINITIALIZED are SKIPPED, not
//     censused - reading one in the wrong layout is undefined behaviour and
//     would produce a confident wrong number;
//   * 3D and depth images are labelled and skipped: a luma census over them is
//     meaningless, and a meaningless number next to real ones invites reading;
//   * source and mirror carry matched labels (H1_pNN_SRC / H1_pNN_MIR) so the
//     ratio is computed from a known pair, never by position in the log.
constexpr uint32_t kH1MaxPairs = 26;
std::atomic_bool h1_armed{false};
std::atomic_bool h1_pending{false};
std::atomic_uint32_t h1_done{0};

struct H1Pair {
    VkImage src, mir;
    VkImageLayout src_layout, mir_layout;
    VkExtent3D extent;
    VkFormat format;
    // Y1 (2026-08-27). THE MIRROR'S OWN FORMAT, which this census has never
    // looked at - it decoded the mirror using the SOURCE image's format.
    //
    // That is an assumption, and the mirrors are not created from the source
    // IMAGE's format: ensure_mirror_framebuffer and ensure_mirror_storage_image
    // both use `vi.format`, the format of the VIEW DOOM bound. Where an engine
    // reinterprets a format through a view - and DOOM demonstrably creates
    // separate views of the same image, which is the whole reason the alias
    // path exists - the two differ, and every MIR number for that target has
    // been a misdecode reported as a measurement.
    //
    // This matters right now because X1 produced the cleanest control this
    // investigation has had (eye 1's depth became BIT-IDENTICAL to eye 0's,
    // several targets went to exactly 100.0%) and the R16_SFLOAT chain still
    // read a uniform 1.0000 from a byte-identical input. "Same shader, same
    // input, different output" is a strong claim, and a decode bug in my own
    // census is a cheaper explanation than a GPU that disagrees with itself.
    // Checked before anything is built on it.
    VkFormat mir_format;
    VkImageType type;
    bool have_meta;
    // Did the doubled call ever RENDER into this mirror? Run 11 found eye 1's
    // half- and quarter-res HDR targets empty of bright content (p09: px>200
    // 3623 -> 0). The leading explanation is that those passes run AFTER the
    // frame root and the doubled call never performs them at all - which is
    // what the PostFrameRoot probe already says about post-frame-root work.
    // writtenThisFrame=false on a diverging pair distinguishes "we rendered it
    // wrong" from "we never rendered it", and those need opposite fixes.
    // written_ever is the cumulative substitution-safety gate and is kept only
    // to show the difference between the two - run 12 read it as the per-frame
    // answer and got YES for every pair, which meant nothing.
    bool written_ever;
    bool written_this_frame;
    // O1: framebuffers holding this image as colour 0, both sides. Capped at 4
    // each - a target bound as colour 0 of many framebuffers is itself worth
    // knowing, and the cap keeps the line readable.
    std::vector<VkFramebuffer> src_fb, mir_fb;
};

// ⚠ THE PAIR INDEX IS NOT STABLE ACROSS RUNS. mirrored_images is an
// unordered_map, so iteration order changes run to run - run 11's "p09" was run
// 12's "p11". Comparing two runs by pair number would silently compare two
// different render targets. This key is derived from the target's own
// dimensions and format, so it names the same thing in every run and is what
// the census labels use.
// ⚠ NOT UNIQUE ON ITS OWN. Run 18 held TWO 3818x2032 f122 targets - one healthy
// at 91.7% and one nearly empty at 20.2% - and both printed as the same key, so
// the census could not say WHICH was which, and K1's names could not be matched
// to either. The occurrence index disambiguates them within a run. It is still
// not stable ACROSS runs (mirrored_images iteration order is not), so compare
// runs by the size/format part and use the #N only to separate same-shaped
// targets inside one run.
std::string h1_key(const H1Pair& p, uint32_t occurrence) {
    return std::format("{}x{}f{}#{}", p.extent.width, p.extent.height,
                       static_cast<int>(p.format), occurrence);
}

void h1_run_pair_census() {
    if (!h1_armed.load(std::memory_order_relaxed)) return;
    if (!h1_pending.exchange(false, std::memory_order_relaxed)) return;
    if (h1_done.fetch_add(1, std::memory_order_relaxed) != 0) return;
    h1_armed.store(false, std::memory_order_relaxed);

    std::vector<H1Pair> pairs;
    size_t mirror_pairs_total = 0;
    {
        std::scoped_lock lock(mirror_mutex);
        // ⚠ COLLECT EVERY PAIR. Runs 11-22 broke out of this loop at
        // kH1MaxPairs and reported `mirrorPairsHeld=12 (cap 12)` - the exact
        // signature of truncation - so every per-target map so far has been an
        // ARBITRARY 12 of N, chosen by unordered_map iteration order. That also
        // explains the target set appearing to change between runs, which run 19
        // wrongly attributed to the game: it was the cap plus unstable order.
        // The metadata/name/framebuffer reporting below is cheap; only the
        // CENSUS is expensive, so the cap now applies to the census alone and
        // the full population is always reported.
        mirror_pairs_total = mirrored_images.size();
        for (const auto& [src, mir] : mirrored_images) {
            H1Pair p{};
            p.src = src;
            p.mir = mir;
            p.written_ever = mirror_images_written.count(mir) != 0;
            p.written_this_frame = h2_doubled_written_this_frame.count(mir) != 0;
            // ⚠ MIRROR FRAMEBUFFERS CANNOT BE FOUND VIA image_view_to_image.
            // Run 21 returned an empty mirFB list for all twelve targets and it
            // is a blind spot, not a finding: our mirror and alias views are
            // created with real_create_image_view, deliberately bypassing the
            // hook that populates that map. P1 run 1 hit precisely this ("an
            // image-only classifier cannot see our own mirrors") and the
            // warning is in this file.
            //
            // mirror_targets IS the map that knows about them - it is keyed by
            // DOOM's original framebuffer and carries the mirror's colour0 - so
            // the mirror framebuffer is found here instead.
            for (const auto& [orig_fb, mt] : mirror_targets) {
                if (mt.color0 == mir && p.mir_fb.size() < 4) p.mir_fb.push_back(orig_fb);
            }
            p.mir_layout = mirror_layout_of(mir);
            // Y1: our mirrors are created with the REAL vkCreateImage, so they
            // are absent from created_image_infos. mirrored_views is where the
            // format they were actually built with lives.
            p.mir_format = VK_FORMAT_UNDEFINED;
            for (const auto& [sv, mv] : mirrored_views) {
                (void)sv;
                if (mv.image != mir) continue;
                p.mir_format = mv.format;
                break;
            }
            auto it = doom_image_layouts.find(src);
            p.src_layout = it != doom_image_layouts.end()
                ? it->second : VK_IMAGE_LAYOUT_UNDEFINED;
            pairs.push_back(p);
        }
    }
    {
        std::scoped_lock lock(framebuffer_depth_view_mutex);
        for (auto& p : pairs) {
            if (auto c = created_image_infos.find(p.src); c != created_image_infos.end()) {
                p.extent = c->second.extent;
                p.format = c->second.format;
                p.type = c->second.type;
                p.have_meta = true;
            }
            // O1 (run 21): which framebuffer has this image as colour 0. The log
            // records `MirrorFramebuffer original->mirror` and DescCensus
            // records `fb=... jobRva=...`, but nothing tied an IMAGE to a
            // framebuffer, so the run-20 profile could name the target
            // (`viewTargetMS`) without naming the passes that fill it. With
            // this, the existing DescCensus lines resolve to the jobs that
            // render into it - and the REAL-vs-DOUBLED difference in that pass
            // list is the next question.
            for (const auto& [fb, view] : framebuffer_color0_views) {
                auto iv = image_view_to_image.find(view);
                if (iv == image_view_to_image.end()) continue;
                if (iv->second == p.src && p.src_fb.size() < 4) p.src_fb.push_back(fb);
                // NOT used for mirrors - see below.
                if (iv->second == p.mir && p.mir_fb.size() < 4) p.mir_fb.push_back(fb);
            }
        }
    }
    // ⚠ SORTING LARGEST-FIRST COST US THE MIPS. Run 23 with the cap at 12 put
    // twelve full-resolution targets inside the cap and pushed the ENTIRE mip
    // chain - the evidence of runs 11-20 - into the 36 over-cap. A size-ordered
    // cap is deterministic but it is also a size FILTER, which is the same
    // scoping-by-assumption error in a new coat. The cap is now 26 so both ends
    // of the population are covered; the freeze is correspondingly longer and
    // that is the price of not choosing what to look at in advance.
    //
    // DETERMINISTIC ORDER. unordered_map iteration order is not stable, so with
    // a cap the census was sampling a different arbitrary subset every run and
    // the maps were not comparable. Sorting by pixel count (largest first), then
    // format, then extent, makes the selection reproducible and puts the
    // full-resolution targets - the ones that matter - inside the cap by
    // construction rather than by luck.
    std::sort(pairs.begin(), pairs.end(), [](const H1Pair& a, const H1Pair& b) {
        const uint64_t pa = static_cast<uint64_t>(a.extent.width) * a.extent.height;
        const uint64_t pb = static_cast<uint64_t>(b.extent.width) * b.extent.height;
        if (pa != pb) return pa > pb;
        if (a.format != b.format) return a.format < b.format;
        if (a.extent.width != b.extent.width) return a.extent.width < b.extent.width;
        return a.extent.height < b.extent.height;
    });

    // Occurrence index per (size, format), so two targets of the same shape get
    // distinct keys - see h1_key. Run 18 could not tell its two 3818x2032 f122
    // targets apart, and one of them was at 20.2% while the other was healthy.
    std::vector<uint32_t> occ(pairs.size(), 0);
    {
        std::unordered_map<uint64_t, uint32_t> seen;
        for (uint32_t i = 0; i < pairs.size(); ++i) {
            const uint64_t k = (static_cast<uint64_t>(pairs[i].extent.width) << 40) ^
                               (static_cast<uint64_t>(pairs[i].extent.height) << 16) ^
                               static_cast<uint64_t>(pairs[i].format);
            occ[i] = seen[k]++;
        }
    }

    // K1: NAME THE TARGETS FROM DOOM'S OWN DATA BEFORE READING ANYTHING INTO
    // THEM. Run 13 measured the 59x31 R16G16 target sign-flipped between the
    // eyes (SRC mean +6.84e-6, MIR mean -6.96e-6) and it is tempting to call it
    // a motion/velocity reduction from its shape - tiny magnitudes, two
    // channels, 1/64 resolution. Shape-guessing is what kept 0x19AE9E0
    // mislabelled for weeks. C2 already builds a live slot->name map from
    // DOOM's declaration list; C3 already knows how to find which slot's
    // resource object holds a given handle. Applying both to the H1 targets
    // names them instead of guessing.
    //
    // ONE PASS: the slot table is read once, then every object is scanned once
    // against ALL target handles at the same time, rather than once per target.
    // Capped by kC3MaxScans and kC3Window like C3.
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"DOOMx64vk.exe"));
        uintptr_t ctx = 0;
        for (uint32_t j = 0; j < kV1Jobs && !ctx; ++j)
            for (uint32_t s = 0; s < kV1Sides && !ctx; ++s)
                if (c2_taken[j][s].load(std::memory_order_relaxed)) ctx = c2_snap[j][s].ctx;
        std::unordered_map<int32_t, std::vector<std::string>> names;
        bool clean = false;
        const uint32_t nodes = c2_build_name_map(base, names, clean);
        uint32_t ambiguous = 0;
        for (const auto& kv : names) if (kv.second.size() > 1) ++ambiguous;
        log::warn(std::format(
            "K1TargetNames BEGINS. ctx={:#x} declNodes={} terminatedCleanly={} slotsWithNames={} "
            "slotsWithMULTIPLEClaimants={}. THE CONTROLS: ctx=0 means C2 never captured a context "
            "this run and NOTHING below can be named - do not fall back to guessing from "
            "resolution and format. A name rendered <AMBIGUOUS:a|b> means several declarations "
            "report that slot and NONE of them may be quoted as the target's name; run 15 "
            "silently took the first and produced 'AMD_VK_EXTENSIONS' for an R16G16 target and "
            "'viewNormalMap' for a depth target.",
            ctx, nodes, clean, names.size(), ambiguous));
        uint32_t named = 0, scanned = 0;
        if (ctx) {
            for (uint32_t s = 0; s < kC2MaxSlots && scanned < kC3MaxScans; ++s) {
                uintptr_t obj = 0;
                if (!safe_read_bytes(ctx + (static_cast<size_t>(s) + 7) * 16,
                        &obj, sizeof(obj))) continue;
                if (obj < 0x10000 || (obj & 0x7) != 0 || obj > 0x7FFFFFFFFFFFull) continue;
                ++scanned;
                uint8_t buf[kC3Window]{};
                if (!safe_read_bytes(obj, buf, sizeof(buf))) continue;
                for (uint32_t off = 0; off + 8 <= sizeof(buf); off += 8) {
                    uintptr_t v = 0;
                    std::memcpy(&v, buf + off, sizeof(v));
                    if (!v) continue;
                    for (uint32_t i = 0; i < pairs.size(); ++i) {
                        const bool is_src = v == reinterpret_cast<uintptr_t>(pairs[i].src);
                        const bool is_mir = v == reinterpret_cast<uintptr_t>(pairs[i].mir);
                        if (!is_src && !is_mir) continue;
                        // c2_name_of refuses to present a colliding slot as named.
                        log::warn(std::format(
                            "K1 {} <- slot {} name={} ({} handle at obj{:#x}+{:#x})",
                            h1_key(pairs[i], occ[i]), s,
                            c2_name_of(names, static_cast<int32_t>(s)),
                            is_src ? "SRC" : "MIR", obj, off));
                        ++named;
                    }
                }
            }
        }
        log::warn(std::format(
            "K1TargetNames COMPLETE. slotObjectsScanned={} matches={}. THE CONTROL: matches=0 "
            "means no context slot references these images within {}B of its object base - the "
            "targets stay UNNAMED and must not be described by their shape.",
            scanned, named, kC3Window));
    }

    log::warn(std::format(
        "H1PairCensus BEGINS. mirrorPairsTOTAL={} listed={} censusCap={}. ⚠ If mirrorPairsTOTAL "
        "exceeds censusCap, the CENSUS covers only the largest targets (deterministic order) "
        "while names and framebuffers are reported for ALL of them - so an absent census row is "
        "the cap, not a measurement. Runs 11-22 reported 'held=12 (cap 12)' and were an "
        "arbitrary 12 of this total. This censuses BOTH halves of every "
        "pair at full resolution and WILL hitch for several seconds - that is expected. Compare "
        "H1_pNN_MIR against H1_pNN_SRC: the pair whose meanLuma ratio departs furthest from "
        "100% names the render target eye 1 is dark on, and therefore the pass that is missing.",
        mirror_pairs_total, pairs.size(), kH1MaxPairs));

    uint32_t censused = 0, skipped = 0, unwritten = 0, over_cap = 0;
    for (uint32_t i = 0; i < pairs.size(); ++i) {
        const H1Pair& p = pairs[i];
        const char* skip = nullptr;
        if (!p.have_meta) skip = "no created_image_infos entry";
        else if (p.type == VK_IMAGE_TYPE_3D) skip = "3D image - luma census meaningless";
        else if (p.extent.width == 0 || p.extent.height == 0) skip = "zero extent";
        else if (p.src_layout == VK_IMAGE_LAYOUT_UNDEFINED ||
                 p.src_layout == VK_IMAGE_LAYOUT_PREINITIALIZED) skip = "source layout untracked";
        else if (p.mir_layout == VK_IMAGE_LAYOUT_UNDEFINED ||
                 p.mir_layout == VK_IMAGE_LAYOUT_PREINITIALIZED) skip = "mirror layout untracked";
        log::warn(std::format(
            "H1 {} (pair{:02}) src={} mir={} {}x{} fmt={} mirFmt={} type={} srcLayout={} "
            "mirLayout={} doubledWroteTHISFRAME={} writtenEverCumulative={} {}",
            h1_key(p, occ[i]), i, reinterpret_cast<void*>(p.src), reinterpret_cast<void*>(p.mir),
            p.extent.width, p.extent.height, static_cast<int>(p.format),
            static_cast<int>(p.mir_format),
            static_cast<int>(p.type), static_cast<int>(p.src_layout),
            static_cast<int>(p.mir_layout),
            p.written_this_frame ? "YES" : "NO", p.written_ever ? "yes" : "no",
            skip ? std::string("SKIPPED: ") + skip : "censusing"));
        {
            std::string sfb, mfb;
            for (auto fb : p.src_fb) sfb += std::format("{} ", reinterpret_cast<void*>(fb));
            for (auto fb : p.mir_fb) mfb += std::format("{} ", reinterpret_cast<void*>(fb));
            log::warn(std::format(
                "H1   {} colour0-of srcFB=[{}] mirFB(DOOM-original handles)=[{}]. Grep these in "
                "the DescCensus lines for the passes and jobRva values that render into this "
                "target, then diff REAL against DOUBLED. NOTE the mirFB list is the DOOM "
                "framebuffer each mirror target was built FROM (mirror_targets is keyed that "
                "way), which is what DescCensus records anyway. An empty srcFB list means the "
                "image is never a colour-0 attachment and is filled some other way (compute / "
                "UAV) - itself the answer for that target.",
                h1_key(p, occ[i]), sfb, mfb));
        }
        if (!p.written_this_frame) ++unwritten;
        if (skip) { ++skipped; continue; }
        // The cap now bounds the CENSUS only - names and framebuffers above are
        // reported for every pair. Deterministic order means this keeps the
        // largest targets, and an omission is explicit rather than silent.
        if (censused >= kH1MaxPairs) {
            log::warn(std::format("H1   {} NOT CENSUSED - census cap {} reached. This is the cap, "
                                  "not a measurement.", h1_key(p, occ[i]), kH1MaxPairs));
            ++over_cap;
            continue;
        }
        const VkExtent2D e{p.extent.width, p.extent.height};
        // Labels keyed by target identity, not by pair index - see h1_key.
        const std::string key = h1_key(p, occ[i]);
        // Y1: the MIRROR is decoded with the format it was BUILT with, not the
        // source image's. A mismatch is logged loudly because every historical
        // MIR/SRC ratio for such a target was a misdecode, not a measurement.
        const VkFormat mfmt =
            p.mir_format != VK_FORMAT_UNDEFINED ? p.mir_format : p.format;
        if (mfmt != p.format) {
            log::error(std::format(
                "Y1 FORMAT MISMATCH on {}: source image format={} but the MIRROR was created "
                "with format={} (the format of the VIEW DOOM bound). Every previous MIR number "
                "for this target decoded the mirror as format {} and is a MISDECODE, not a "
                "measurement. This run decodes it as {}.",
                key, static_cast<int>(p.format), static_cast<int>(mfmt),
                static_cast<int>(p.format), static_cast<int>(mfmt)));
        }
        dump_image_snapshot(p.src, e, p.format,
            std::format("H1_{}_SRC", key).c_str(), p.src_layout, false);
        dump_image_snapshot(p.mir, e, mfmt,
            std::format("H1_{}_MIR", key).c_str(), p.mir_layout, false);
        ++censused;
    }
    log::warn(std::format(
        "H1PairCensus COMPLETE. censused={} skipped={} notWrittenByDoubledCallThisFrame={} overCap={}. "
        "THE CONTROL: censused=0 means every pair was skipped and this table says NOTHING about "
        "eye 1 - read the per-pair SKIPPED reasons rather than concluding the targets agree. "
        "AND THE DECIDING READ, on doubledWroteTHISFRAME (never on writtenEverCumulative, which "
        "is a session-wide safety gate that is YES for everything and answers nothing): a pair "
        "that DIVERGES with doubledWroteTHISFRAME=NO was never rendered by the doubled call in "
        "this frame - the fix is to make that pass run for eye 1, not to correct its inputs. A "
        "pair that diverges with YES was rendered, and rendered wrong.",
        censused, skipped, unwritten, over_cap));
}

// FINAL STAGE (2026-08-18) - see inside_final_pass_double.
//
// Four pointer args is a guess, exactly as it was for the refuted 0x189A350,
// and it is still the right shape of guess: on x64 the first four
// integer/pointer arguments live in RCX/RDX/R8/R9, so forwarding four is
// correct for any function taking four or fewer of them and wrong if it takes
// more or takes floats. The difference from last time is that this function
// runs ONCE PER FRAME rather than a thousand times across ten threads, so
// hooking it is cheap to observe and cannot perturb the renderer the way the
// last probe did.
void __fastcall hook_final_stage(void* a1, void* a2, void* a3, void* a4) {
    FbDoom fbd_(FB_D_FINAL_STAGE);
    const uint32_t n = final_stage_calls.fetch_add(1, std::memory_order_relaxed);
    if (n < 8 || (n % 600) == 0) {
        log::warn(std::format(
            "FinalStageProbe #{} presentedFrame={} a1={} a2={} a3={} a4={} tid={}. This is "
            "DOOM+0x186E6BA, the outermost function specific to the grade+HUD pass. One line "
            "per frame means once-per-frame is confirmed; stable args mean a context pointer we "
            "can simply pass again.",
            n, presented_frame.load(std::memory_order_relaxed), a1, a2, a3, a4,
            GetCurrentThreadId()));
    }
    inside_final_pass_real = true;
    last_final_pass_framebuffer = VK_NULL_HANDLE;
    { FbTramp fbt_(FB_D_FINAL_STAGE); real_final_stage(a1, a2, a3, a4); }
    inside_final_pass_real = false;

    if (!final_pass_double_armed.load(std::memory_order_relaxed)) return;
    if (!mirror_enabled.load(std::memory_order_relaxed) || !mirrors_converged()) return;
    if (last_final_pass_framebuffer == VK_NULL_HANDLE) return;

    // PRE-FLIGHT, and it is not optional. If the second invocation runs and
    // framebuffer mirroring then MISSES, the pass writes straight into the
    // swapchain - overwriting the finished frame the owner is looking at,
    // with a shifted-camera grade, live in a headset. So the mirror is
    // created and checked HERE, before anything is re-invoked, and a miss
    // refuses the doubling outright instead of discovering it too late.
    //
    // MONITOR EYE-1 VIEW skips the pre-flight because it is asking for exactly
    // the outcome the pre-flight prevents (2026-08-29). The distinction that
    // makes that safe is not "we changed our minds": it is that the mode
    // REFUSES TO ARM while the headset path is live, so the frame being
    // overwritten is a monitor frame on a flat run and never eye 0 in a
    // headset. See monitor_eye1_direct and the SHIFT+F5 handler.
    const bool monitor_direct = monitor_eye1_direct.load(std::memory_order_relaxed);
    if (!monitor_direct) {
        std::scoped_lock mirror_lock(mirror_mutex);
        if (!ensure_mirror_framebuffer(last_final_pass_framebuffer)) {
            const uint32_t r = final_pass_refused_no_mirror.fetch_add(1, std::memory_order_relaxed);
            if (r < 5) {
                log::error(std::format(
                    "FinalStage REFUSED to double: no mirror for the final pass's framebuffer {} "
                    "(created before our hooks installed?). Re-invoking without one would write "
                    "DOOM's swapchain image a second time and corrupt the presented frame, so "
                    "the doubling is skipped. Eye 1 keeps its ungraded composite.",
                    reinterpret_cast<void*>(last_final_pass_framebuffer)));
            }
            return;
        }
    }
    final_pass_mirror_color = VK_NULL_HANDLE;
    inside_final_pass_double = true;
    { FbTramp fbt_(FB_D_FINAL_STAGE); real_final_stage(a1, a2, a3, a4); }
    inside_final_pass_double = false;
    kharvox::native::endFinal();
    const uint32_t d = final_pass_doubles.fetch_add(1, std::memory_order_relaxed);
    if (d < 5 || (d % 600) == 0) {
        log::warn(std::format(
            "FinalStage DOUBLED #{}{}: second invocation complete, mirrorColour={} {}x{} fmt={}. "
            "{}",
            d, monitor_direct ? " [MONITOR EYE-1 VIEW]" : "",
            reinterpret_cast<void*>(final_pass_mirror_color),
            final_pass_mirror_extent.width, final_pass_mirror_extent.height,
            static_cast<int>(final_pass_mirror_format),
            monitor_direct
                ? "mirrorColour=0 is EXPECTED here and is not a fault: monitor eye-1 view "
                  "deliberately declines the mirror so this pass writes DOOM's swapchain image, "
                  "which is how eye 1 reaches the screen. The picture on the monitor IS eye 1."
                : "mirrorColour=0 means the render pass was NOT mirrored despite the pre-flight "
                  "and the output went somewhere we do not own - treat any image from this run "
                  "as unattributable."));
    }
    // THE SNAPSHOT CANNOT BE TAKEN HERE (2026-08-18, learned the hard way).
    //
    // The first attempt dumped right at this point and produced a uniformly
    // ZERO image - every sampled byte 0, not merely a dark one. That was not
    // the render failing: this function RECORDS commands into DOOM's command
    // buffer, and they do not execute until DOOM SUBMITS. The submit is at
    // chain level 6, which this target deliberately EXCLUDES - that exclusion
    // is the whole reason it does not crash. So by construction the GPU has
    // not run the doubled pass by the time we return here, and a freshly
    // created mirror still reads as zeroes.
    //
    // The dump is therefore handed to the present thread, which runs after
    // the submit, and takes the mirror's TRACKED layout rather than assuming
    // COLOR_ATTACHMENT_OPTIMAL - a wrong source layout discards contents and
    // would produce the same black image for a completely different reason.
    if (final_pass_mirror_color != VK_NULL_HANDLE &&
        final_pass_dump_pending.load(std::memory_order_relaxed)) {
        final_pass_dump_ready.store(true, std::memory_order_release);
    }
}

// Legacy entry point, unchanged in behaviour: colour attachment 0 of an
// OffscreenTarget, which is always kOffscreenColorFormats[0] (see
// prepare_offscreen_target, which creates the images from that same array).
void dump_offscreen_color_snapshot(const OffscreenTarget& target, const char* label) {
    dump_image_snapshot(target.color_images[0], target.extent, kOffscreenColorFormats[0], label);
}

// DepthDumpProbe (docs/STEREO-RESTART-PLAN.md Task 3, Round 5 step 1):
// dump_offscreen_color_snapshot above only ever read back the 3 color
// attachments - nobody has ever looked at a depth image's own content
// directly. Takes a raw VkImage + extent (not an OffscreenTarget&) so the
// exact same readback logic can be pointed at either our own private
// synthetic depth image OR DOOM's real depth image, for the control test
// in docs/STEREO-RESTART-PLAN.md Task 3, Round 5 continued step 3 (dump_
// offscreen_depth_snapshot is brand-new code that already had one real bug
// this round, see below - before trusting its result about the private
// image, verify it against a depth image already known to be correct).
// Format assumed VK_FORMAT_D24_UNORM_S8_UINT (see kOffscreenDepthFormat) -
// true for both the private synthetic images and DOOM's own real depth
// image (RE-NOTES.md, confirmed same format for both). Aspect DEPTH_BIT
// only. Copies to a linear BUFFER via vkCmdCopyImageToBuffer, not a
// linear-tiled staging IMAGE: a first attempt mirroring the color dump's
// staging-image approach failed at vkCreateImage for a LINEAR-tiled
// D24_UNORM_S8_UINT image (logged "Depth dump: staging image creation
// failed", live, 2026-08-12) - LINEAR tiling support for depth/stencil
// formats is optional in Vulkan and commonly absent on desktop GPU
// drivers, unlike color formats. A buffer has no tiling concept, so this
// is the standard, portable way to read back a depth/stencil image.
// Assumes the image is in DEPTH_STENCIL_ATTACHMENT_OPTIMAL at dump time -
// true for our own owned images (see prepare_offscreen_target's initial
// transition) and for DOOM's real depth image (RE-NOTES.md: never
// transitioned away, persists in this layout for the image's whole
// lifetime).
void dump_depth_image_snapshot(VkImage depth_image, VkExtent2D extent, const char* label) {
    const auto& vk = vulkan_state();
    if (!vk.device || !vk.physical_device || !vk.queue) return;
    if (depth_image == VK_NULL_HANDLE) {
        log::error(std::format("Depth dump ({}): no depth image to dump (VK_NULL_HANDLE)", label));
        return;
    }
    auto create_buffer = load_device_proc<PFN_vkCreateBuffer>(vk.device, "vkCreateBuffer");
    auto get_buffer_reqs = load_device_proc<PFN_vkGetBufferMemoryRequirements>(vk.device, "vkGetBufferMemoryRequirements");
    auto allocate_memory = load_device_proc<PFN_vkAllocateMemory>(vk.device, "vkAllocateMemory");
    auto bind_buffer_memory = load_device_proc<PFN_vkBindBufferMemory>(vk.device, "vkBindBufferMemory");
    auto map_memory = load_device_proc<PFN_vkMapMemory>(vk.device, "vkMapMemory");
    auto unmap_memory = load_device_proc<PFN_vkUnmapMemory>(vk.device, "vkUnmapMemory");
    auto create_command_pool = load_device_proc<PFN_vkCreateCommandPool>(vk.device, "vkCreateCommandPool");
    auto allocate_command_buffers = load_device_proc<PFN_vkAllocateCommandBuffers>(vk.device, "vkAllocateCommandBuffers");
    auto begin_command_buffer = load_device_proc<PFN_vkBeginCommandBuffer>(vk.device, "vkBeginCommandBuffer");
    auto cmd_pipeline_barrier = load_device_proc<PFN_vkCmdPipelineBarrier>(vk.device, "vkCmdPipelineBarrier");
    auto cmd_copy_image_to_buffer = load_device_proc<PFN_vkCmdCopyImageToBuffer>(vk.device, "vkCmdCopyImageToBuffer");
    auto end_command_buffer = load_device_proc<PFN_vkEndCommandBuffer>(vk.device, "vkEndCommandBuffer");
    auto queue_submit = load_device_proc<PFN_vkQueueSubmit>(vk.device, "vkQueueSubmit");
    auto queue_wait_idle = load_device_proc<PFN_vkQueueWaitIdle>(vk.device, "vkQueueWaitIdle");
    auto free_command_buffers = load_device_proc<PFN_vkFreeCommandBuffers>(vk.device, "vkFreeCommandBuffers");
    auto destroy_command_pool = load_device_proc<PFN_vkDestroyCommandPool>(vk.device, "vkDestroyCommandPool");
    auto destroy_buffer = load_device_proc<PFN_vkDestroyBuffer>(vk.device, "vkDestroyBuffer");
    auto free_memory_fn = load_device_proc<PFN_vkFreeMemory>(vk.device, "vkFreeMemory");
    PFN_vkGetPhysicalDeviceMemoryProperties get_mem_props = nullptr;
    {
        HMODULE mod = GetModuleHandleW(L"vulkan-1.dll");
        if (mod) get_mem_props = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
            kharvox::native::instanceProc("vkGetPhysicalDeviceMemoryProperties"));
    }
    if (!create_buffer || !get_buffer_reqs || !allocate_memory || !bind_buffer_memory || !map_memory ||
        !unmap_memory || !create_command_pool || !allocate_command_buffers ||
        !begin_command_buffer || !cmd_pipeline_barrier || !cmd_copy_image_to_buffer || !end_command_buffer ||
        !queue_submit || !queue_wait_idle || !free_command_buffers || !destroy_command_pool ||
        !destroy_buffer || !free_memory_fn || !get_mem_props) {
        log::error("Depth dump: could not load required Vulkan device procs");
        return;
    }

    queue_wait_idle(vk.queue);

    const VkDeviceSize buffer_size =
        static_cast<VkDeviceSize>(extent.width) * extent.height * 4;  // tightly packed, 4 bytes/texel
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = buffer_size;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (create_buffer(vk.device, &buffer_info, nullptr, &staging) != VK_SUCCESS) {
        log::error("Depth dump: staging buffer creation failed");
        return;
    }
    VkMemoryRequirements reqs{};
    get_buffer_reqs(vk.device, staging, &reqs);
    VkPhysicalDeviceMemoryProperties mem_props{};
    get_mem_props(vk.physical_device, &mem_props);
    uint32_t type_index = 0;
    if (!find_memory_type(mem_props, reqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, type_index)) {
        log::error("Depth dump: no host-visible memory type for staging buffer");
        destroy_buffer(vk.device, staging, nullptr);
        return;
    }
    VkMemoryAllocateInfo alloc_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc_info.allocationSize = reqs.size;
    alloc_info.memoryTypeIndex = type_index;
    if (allocate_memory(vk.device, &alloc_info, nullptr, &staging_memory) != VK_SUCCESS ||
        bind_buffer_memory(vk.device, staging, staging_memory, 0) != VK_SUCCESS) {
        log::error("Depth dump: staging memory alloc/bind failed");
        if (staging_memory) free_memory_fn(vk.device, staging_memory, nullptr);
        destroy_buffer(vk.device, staging, nullptr);
        return;
    }

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = vk.queue_family;
    bool copied = create_command_pool(vk.device, &pool_info, nullptr, &pool) == VK_SUCCESS;
    if (copied) {
        VkCommandBufferAllocateInfo cb_alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cb_alloc.commandPool = pool;
        cb_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cb_alloc.commandBufferCount = 1;
        VkCommandBuffer command = VK_NULL_HANDLE;
        copied = allocate_command_buffers(vk.device, &cb_alloc, &command) == VK_SUCCESS;
        if (copied) {
            VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            begin_command_buffer(command, &begin_info);
            // Depth aspect only - the packed stencil bits aren't relevant to
            // this check and vkCmdCopyImageToBuffer's aspectMask is one
            // aspect per region, same as vkCmdCopyImage.
            VkImageMemoryBarrier to_src{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            to_src.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            to_src.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            to_src.image = depth_image;
            to_src.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
            cmd_pipeline_barrier(command,
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_src);
            VkBufferImageCopy region{};
            region.bufferOffset = 0;
            region.bufferRowLength = 0;    // tightly packed
            region.bufferImageHeight = 0;  // tightly packed
            region.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
            region.imageExtent = {extent.width, extent.height, 1};
            cmd_copy_image_to_buffer(command, depth_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                staging, 1, &region);
            end_command_buffer(command);
            VkSubmitInfo submit_info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit_info.commandBufferCount = 1;
            submit_info.pCommandBuffers = &command;
            copied = queue_submit(vk.queue, 1, &submit_info, VK_NULL_HANDLE) == VK_SUCCESS;
            if (copied) queue_wait_idle(vk.queue);
            free_command_buffers(vk.device, pool, 1, &command);
        }
        destroy_command_pool(vk.device, pool, nullptr);
    }
    if (!copied) {
        log::error("Depth dump: copy to staging buffer failed");
        free_memory_fn(vk.device, staging_memory, nullptr);
        destroy_buffer(vk.device, staging, nullptr);
        return;
    }

    void* mapped = nullptr;
    if (map_memory(vk.device, staging_memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS || !mapped) {
        log::error("Depth dump: mapping staging memory failed");
        free_memory_fn(vk.device, staging_memory, nullptr);
        destroy_buffer(vk.device, staging, nullptr);
        return;
    }

    // D24_UNORM_S8_UINT copied via aspect DEPTH_BIT: each texel is a 32-bit
    // word with the 24-bit UNORM depth in the low bits (confirmed via the
    // "atFarPlane" sanity check below - this driver packs it this way, not
    // an unverified guess). Grayscale output: depth 0.0 (near) -> white,
    // 1.0 (far) -> black, so a near-field-shaped bright region would
    // visually match the "near-field black, sky survives" color symptom's
    // silhouette for easy comparison.
    std::vector<uint8_t> gray(static_cast<size_t>(extent.width) * extent.height);
    float min_depth = std::numeric_limits<float>::infinity();
    float max_depth = -std::numeric_limits<float>::infinity();
    double sum_depth = 0.0;
    uint64_t at_far_plane = 0;
    const auto* words = static_cast<const uint32_t*>(mapped);
    for (uint32_t y = 0; y < extent.height; ++y) {
        const uint32_t* row = words + static_cast<size_t>(y) * extent.width;
        for (uint32_t x = 0; x < extent.width; ++x) {
            const uint32_t packed = row[x];
            const float depth = static_cast<float>(packed & 0x00FFFFFFu) / static_cast<float>(0x00FFFFFFu);
            min_depth = std::min(min_depth, depth);
            max_depth = std::max(max_depth, depth);
            sum_depth += depth;
            if (depth >= 0.9999f) ++at_far_plane;
            const size_t pixel = static_cast<size_t>(y) * extent.width + x;
            gray[pixel] = static_cast<uint8_t>(std::clamp(1.0f - depth, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
    unmap_memory(vk.device, staging_memory);
    free_memory_fn(vk.device, staging_memory, nullptr);
    destroy_buffer(vk.device, staging, nullptr);

    const double mean_depth = sum_depth / (static_cast<double>(extent.width) * extent.height);
    const double far_pct = 100.0 * static_cast<double>(at_far_plane) /
        (static_cast<double>(extent.width) * extent.height);
    log::info(std::format(
        "Depth dump readback stats ({}): minDepth={:.6f} maxDepth={:.6f} meanDepth={:.6f} atFarPlane(>=0.9999)={:.2f}%",
        label, min_depth, max_depth, mean_depth, far_pct));

    const std::string filename = std::string("doomvr_offscreen_dump_") + label + ".ppm";
    const std::filesystem::path dump_path =
        std::filesystem::u8path(kharvox::logPathA(filename.c_str()));
    FILE* file = nullptr;
    if (_wfopen_s(&file, dump_path.c_str(), L"wb") == 0 && file) {
        std::fprintf(file, "P5\n%u %u\n255\n", extent.width, extent.height);
        std::fwrite(gray.data(), 1, gray.size(), file);
        std::fclose(file);
        log::info("Depth dump written: " + dump_path.string());
    } else {
        log::error("Depth dump: could not open output file for writing");
    }
}

// FULL SERIALISATION INSIDE THE DOUBLED CALL (2026-08-20).
//
// THE LAST STANDING CANDIDATE, and the evidence points straight at it.
//
// With the RenderDoc layer merely loaded the doubled call renders its full
// additive layer; without it, it does not. Everything else that could differ
// has been eliminated by measurement: OpenXR on/off, shadow-atlas suppression
// on/off, resource usage flags (RenderDoc-parity widening, zero refusals, no
// change), and internal render resolution (half AND full res both work WITH
// RenderDoc; full res fails without it).
//
// What remains is the one thing RenderDoc unavoidably does to every frame: it
// serialises GPU execution to a crawl. And the DeferredInputMap says the
// doubled passes read DOOM's REAL images - identical view and image handles,
// including the 3D froxel fog volumes that carry the missing volumetrics. So
// the inputs are correct data; only their ORDERING relative to DOOM's own
// writes is in question, and heavy serialisation is exactly what would hide a
// missing barrier.
//
// This inserts a full memory barrier before every render pass of the doubled
// call - every write made visible to every read, ALL_COMMANDS to ALL_COMMANDS.
// Deliberately the blunt instrument: it is a DIAGNOSTIC, not the fix. If the
// effects return, the bug is a missing barrier inside the doubled call and the
// next job is to find which one; if they do not, synchronisation is eliminated
// as a class and RenderDoc's remaining differences are driver-internal.
//
// RESULT: NEGATIVE (2026-08-20, run 12:34). Barriers were emitted and logged,
// so the path was live, and eye 1's mirror stayed pinned at px>200 =
// 15 666..16 024, mean 30.6 - the same no-effects signature, while DOOM's
// control ranged to 21 877. Synchronisation inside the doubled call, at
// render-pass granularity, is ELIMINATED.
//
// Switched OFF rather than deleted: it is the shape the next serialisation
// test would take if one is ever needed, and leaving an eliminated variable
// switched ON would confound every later run.
//
// Forced at load, not toggled - the owner cannot verify a keypress in a
// headset, and this must hold for every pass of every frame in the run.
constexpr bool kSerialiseDoubledCall = false;

void serialise_before_doubled_pass(VkCommandBuffer command_buffer) {
    if (!kSerialiseDoubledCall) return;
    if (!inside_frame_root_double && !inside_final_pass_double) return;
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                       VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
                       VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT |
                       VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
                       VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                       VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_READ_BIT |
                       VK_ACCESS_MEMORY_READ_BIT;
    real_cmd_pipeline_barrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    const uint32_t n = serialise_barriers_emitted.fetch_add(1, std::memory_order_relaxed);
    if (n < 3) {
        log::warn(std::format(
            "SerialiseDoubled: full ALL_COMMANDS memory barrier #{} emitted before a doubled "
            "render pass. Diagnostic for the RenderDoc discriminator - if the additive layer "
            "returns, a barrier is missing inside the doubled call.", n));
    }
}

void VKAPI_PTR hook_cmd_begin_render_pass(VkCommandBuffer command_buffer,
    const VkRenderPassBeginInfo* info, VkSubpassContents contents) {
    kharvox::native::noteCommand(command_buffer);
    FbHook fb_(FB_H_CMD_BEGIN_RENDER_PASS);
    serialise_before_doubled_pass(command_buffer);
    // FINAL STAGE (2026-08-18). Which framebuffer DOOM's REAL final pass
    // rendered into, so hook_final_stage can mirror exactly that one and
    // pre-flight the doubling against it. DOOM alternates two framebuffers for
    // double buffering, so this is read per frame and never cached.
    //
    // FIRST STATEMENT IN THE FUNCTION, deliberately. Six early returns follow;
    // all six are traced to guards that cannot be true after the frame root
    // returns (inside_render_opaque/inside_render_depth) or to the END key's
    // legacy probe (real_call_redirect_armed), so none can fire here - but
    // being correct BECAUSE of a trace is worse than not depending on it.
    if (inside_final_pass_real && info) last_final_pass_framebuffer = info->framebuffer;
    // RPCENSUS denominator, counted at ENTRY so "fell through" has something to
    // be a fraction OF. fellthrough == total means NOTHING is being redirected
    // to a mirror and every render pass the doubled call begins is landing on
    // DOOM's own framebuffer.
    if (inside_frame_root_double || inside_final_pass_double)
        g_rpc_total.fetch_add(1, std::memory_order_relaxed);
    // CONTAINMENT event 0: the grade+HUD pass itself, identified the way it is
    // everywhere else in this file - by writing a SWAPCHAIN image - and not via
    // inside_final_pass_real, which is only set by a hook that is deliberately
    // not installed while containment is deciding what is safe to hook.
    if (info && containment_armed.load(std::memory_order_relaxed)) {
        VkImage ct_img = VK_NULL_HANDLE;
        {
            std::scoped_lock lock(framebuffer_depth_view_mutex);
            if (auto f = framebuffer_color0_views.find(info->framebuffer);
                f != framebuffer_color0_views.end()) {
                if (auto i = image_view_to_image.find(f->second); i != image_view_to_image.end())
                    ct_img = i->second;
            }
        }
        bool ct_swapchain = false;
        {
            std::unique_lock lock(swapchains_mutex);
            for (const auto& [sc, rec] : swapchains) {
                (void)sc;
                for (VkImage s : rec.images) if (s == ct_img) { ct_swapchain = true; break; }
                if (ct_swapchain) break;
            }
        }
        if (ct_swapchain) containment_sample(0);
    }
    // CALLER SPECIFICITY (2026-08-18) - see CallerTally. Runs at EVERY render
    // pass, not just the post-frame-root one, because the whole question is
    // which callers are UNIQUE to the swapchain-writing pass and that cannot
    // be answered from the final pass's chain alone. Armed and bounded.
    if (info && caller_specificity_armed.load(std::memory_order_relaxed) &&
        caller_specificity_walks.fetch_add(1, std::memory_order_relaxed) <
            kMaxCallerSpecificityWalks) {
        VkImage cs_img = VK_NULL_HANDLE;
        {
            std::scoped_lock lock(framebuffer_depth_view_mutex);
            if (auto f = framebuffer_color0_views.find(info->framebuffer);
                f != framebuffer_color0_views.end()) {
                if (auto i = image_view_to_image.find(f->second);
                    i != image_view_to_image.end())
                    cs_img = i->second;
            }
        }
        bool cs_swapchain = false;
        {
            std::unique_lock lock(swapchains_mutex);
            for (const auto& [sc, rec] : swapchains) {
                (void)sc;
                for (VkImage s : rec.images) if (s == cs_img) { cs_swapchain = true; break; }
                if (cs_swapchain) break;
            }
        }
        const uintptr_t cs_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"DOOMx64vk.exe"));
        void* cs_frames[16]{};
        const USHORT cs_captured = CaptureStackBackTrace(0, 16, cs_frames, nullptr);
        // Both maps above are already released; this is a leaf lock.
        std::scoped_lock lock(caller_tally_mutex);
        for (USHORT i = 0; i < cs_captured; ++i) {
            const uintptr_t pc = reinterpret_cast<uintptr_t>(cs_frames[i]);
            if (!cs_base || pc < cs_base) continue;
            if (pc - cs_base > 0x10000000) continue;
            DWORD64 image_base = 0;
            PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(static_cast<DWORD64>(pc),
                                                          &image_base, nullptr);
            if (!rf || image_base != static_cast<DWORD64>(cs_base)) continue;
            const uintptr_t fn = static_cast<uintptr_t>(rf->BeginAddress);
            uint32_t slot = 0;
            for (; slot < caller_tally_count; ++slot)
                if (caller_tallies[slot].fn_rva == fn) break;
            if (slot == caller_tally_count) {
                if (caller_tally_count >= kMaxCallerTallies) continue;
                caller_tallies[caller_tally_count++] = CallerTally{fn, 0, 0, 0};
            }
            if (cs_swapchain) ++caller_tallies[slot].swapchain_passes;
            else ++caller_tallies[slot].other_passes;
            if (i > caller_tallies[slot].max_depth) caller_tallies[slot].max_depth = i;
        }
    }
    if (inside_render_opaque && info &&
        render_pass_begin_sample_count.load(std::memory_order_relaxed) < kMaxRenderPassBeginSamples) {
        const uint32_t sample = render_pass_begin_sample_count.fetch_add(1, std::memory_order_relaxed);
        if (sample < kMaxRenderPassBeginSamples) {
            log::info(std::format(
                "RenderOpaque vkCmdBeginRenderPass sample={} framebuffer={} renderPass={} "
                "area=({},{} {}x{}) clearValueCount={}",
                sample, reinterpret_cast<void*>(info->framebuffer),
                reinterpret_cast<void*>(info->renderPass), info->renderArea.offset.x,
                info->renderArea.offset.y, info->renderArea.extent.width,
                info->renderArea.extent.height, info->clearValueCount));
        }
    }
    // Capture, once each, the (up to) two distinct render passes RenderOpaque
    // begins per invocation - live-discovered, never hardcoded (session-
    // specific handles, see docs/RE-NOTES.md). First live redirect test only
    // covered slot 0 (the first-seen pass) and read back flat black; slot 1
    // is captured here too so both get their own compatible framebuffer.
    if (inside_render_opaque && info && info->renderPass) {
        bool already_known = false;
        const uint32_t known_count = std::min<uint32_t>(
            offscreen_targets_captured.load(std::memory_order_relaxed),
            static_cast<uint32_t>(offscreen_targets.size()));
        for (uint32_t i = 0; i < known_count; ++i) {
            if (offscreen_targets[i].render_pass == info->renderPass) { already_known = true; break; }
        }
        if (!already_known) {
            const uint32_t slot = offscreen_targets_captured.fetch_add(1, std::memory_order_relaxed);
            if (slot < offscreen_targets.size()) {
                VkImageView real_depth_view = VK_NULL_HANDLE;
                {
                    std::scoped_lock lock(framebuffer_depth_view_mutex);
                    const auto found = framebuffer_depth_views.find(info->framebuffer);
                    if (found != framebuffer_depth_views.end()) real_depth_view = found->second;
                }
                prepare_offscreen_target(offscreen_targets[slot], info->renderPass, info->renderArea.extent,
                    real_depth_view);
            }
        }
    }
    // PassDrawCountProbe (docs/STEREO-RESTART-PLAN.md Task 3, 2026-08-11):
    // tracks which of RenderOpaque's two internal passes (offscreen_targets[0]
    // = first-seen/Clear, offscreen_targets[1] = second-seen/Load) is
    // currently active on this command buffer, so hook_doom_render_view can
    // tally draw-command counts per pass. Answers docs/RE-NOTES.md's
    // never-resolved question: which fcn.141a37b80 dispatch iterations land
    // in pass 1 vs pass 2. Cleared by hook_cmd_end_render_pass.
    if (inside_render_opaque && info && info->renderPass) {
        current_render_opaque_pass_slot = -1;
        for (uint32_t i = 0; i < offscreen_targets.size(); ++i) {
            if (offscreen_targets[i].render_pass == info->renderPass) {
                current_render_opaque_pass_slot = static_cast<int>(i);
                break;
            }
        }
    }
    // Phase 1 (STEREO-IMPLEMENTATION-PLAN.md v2): capture RenderDepth's own
    // render pass exactly once, from its REAL (non-redirected) invocation -
    // inside_render_depth only brackets that real call (see hook_render_depth).
    // Separate render pass from RenderOpaque's (depth-only, 1 attachment) -
    // never hardcoded, always live-discovered, same discipline as every
    // other captured handle in this file.
    if (inside_render_depth && info && info->renderPass &&
        render_depth_captured_pass.load(std::memory_order_relaxed) == VK_NULL_HANDLE) {
        render_depth_captured_pass.store(info->renderPass, std::memory_order_relaxed);
        log::info(std::format("RenderDepth own render pass captured: renderPass={} "
            "framebuffer={} extent={}x{}",
            reinterpret_cast<void*>(info->renderPass), reinterpret_cast<void*>(info->framebuffer),
            info->renderArea.extent.width, info->renderArea.extent.height));
    }
    // RealCallRedirectProbe (docs/STEREO-RESTART-PLAN.md Task 3, control
    // test) - see real_call_redirect_target's declaration above. Strictly
    // separate from inside_injected_extra_call - only ever touches the
    // REAL, once-per-frame RenderDepth/RenderOpaque calls, one-shot per
    // arm. Checked before every extra-call block below so it can never be
    // shadowed by them (though the inside_injected_extra_call/
    // !inside_injected_extra_call split already makes them mutually
    // exclusive by construction).
    if (real_call_redirect_armed.load(std::memory_order_relaxed) && info && info->renderPass) {
        if (inside_render_depth &&
            info->renderPass == render_depth_captured_pass.load(std::memory_order_relaxed) &&
            real_call_redirect_depth_framebuffer != VK_NULL_HANDLE) {
            log::warn(std::format(
                "RealCallRedirectProbe: RenderDepth redirect framebuffer {} -> {} "
                "(renderPass={} unchanged)",
                reinterpret_cast<void*>(info->framebuffer),
                reinterpret_cast<void*>(real_call_redirect_depth_framebuffer),
                reinterpret_cast<void*>(info->renderPass)));
            VkRenderPassBeginInfo redirected = *info;
            redirected.framebuffer = real_call_redirect_depth_framebuffer;
            real_cmd_begin_render_pass(command_buffer, &redirected, contents);
            return;
        }
        if (inside_render_opaque && !inside_injected_extra_call) {
            if (info->renderPass == offscreen_targets[0].render_pass &&
                real_call_redirect_pass1_framebuffer != VK_NULL_HANDLE) {
                log::warn(std::format(
                    "RealCallRedirectProbe: pass1(Clear) redirect framebuffer {} -> {} "
                    "(renderPass={} unchanged)",
                    reinterpret_cast<void*>(info->framebuffer),
                    reinterpret_cast<void*>(real_call_redirect_pass1_framebuffer),
                    reinterpret_cast<void*>(info->renderPass)));
                VkRenderPassBeginInfo redirected = *info;
                redirected.framebuffer = real_call_redirect_pass1_framebuffer;
                real_cmd_begin_render_pass(command_buffer, &redirected, contents);
                return;
            }
            if (info->renderPass == offscreen_targets[1].render_pass &&
                real_call_redirect_target.ready.load(std::memory_order_relaxed)) {
                log::warn(std::format(
                    "RealCallRedirectProbe: pass2(Load) redirect framebuffer {} -> {} "
                    "(renderPass={} unchanged) - one-shot complete, disarming",
                    reinterpret_cast<void*>(info->framebuffer),
                    reinterpret_cast<void*>(real_call_redirect_target.framebuffer),
                    reinterpret_cast<void*>(info->renderPass)));
                VkRenderPassBeginInfo redirected = *info;
                redirected.framebuffer = real_call_redirect_target.framebuffer;
                real_cmd_begin_render_pass(command_buffer, &redirected, contents);
                real_call_redirect_dump_pending.store(true, std::memory_order_relaxed);
                real_call_redirect_armed.store(false, std::memory_order_relaxed);
                return;
            }
        }
    }
    // Phase 1: redirect RenderDepth's per-eye reinvoke into that eye's
    // private depth-only framebuffer. Checked before the RenderOpaque
    // per-eye block below since this is a structurally different render
    // pass (depth-only) that would never match offscreen_targets[1]'s
    // render pass anyway, but kept as an explicit early branch for clarity.
    if (inside_injected_extra_call && current_eye_render >= 0 && info && info->renderPass &&
        info->renderPass == render_depth_captured_pass.load(std::memory_order_relaxed)) {
        const VkFramebuffer fb = stereo_eye_depth_framebuffers[current_eye_render];
        if (fb != VK_NULL_HANDLE) {
            log::warn(std::format(
                "StereoDualEyeDump: eye={} RenderDepth redirect framebuffer {} -> {} "
                "(renderPass={} unchanged)",
                current_eye_render, reinterpret_cast<void*>(info->framebuffer),
                reinterpret_cast<void*>(fb), reinterpret_cast<void*>(info->renderPass)));
            VkRenderPassBeginInfo redirected = *info;
            redirected.framebuffer = fb;
            real_cmd_begin_render_pass(command_buffer, &redirected, contents);
            return;
        }
        // Not ready - the driver is expected to call
        // ensure_stereo_eye_depth_framebuffer_ready(eye) before ever
        // reinvoking RenderDepth, so this shouldn't happen in practice.
        // Deliberately NOT redirecting into anything and NOT skipping the
        // begin-render-pass call either (leaving a draw recorded with no
        // active render pass is worse - undefined driver behavior, real
        // crash risk after tonight's other one). Falling through to
        // info's original real framebuffer instead: RenderDepth would just
        // redundantly redraw the SAME real camera's depth into DOOM's real
        // depth buffer a second time - wasted GPU work, not corruption,
        // same "safe, idempotent no-op" property F7 already proved for
        // RenderOpaque called twice with an identical camera. Logged so a
        // violation of the ordering assumption is visible, not silent.
        log::error(std::format(
            "StereoDualEyeDump: eye={} RenderDepth redirect requested but framebuffer not ready - "
            "ordering bug, falling through to a redundant real-depth-buffer pass (safe, wasted work)",
            current_eye_render));
    }
    // PassDrawCountProbe follow-up fix (docs/STEREO-RESTART-PLAN.md Task 3,
    // 2026-08-11): checked BEFORE the pass-2 block below, since pass 1 fires
    // first within a single RenderOpaque invocation. The original
    // "offscreen_targets[0] (Clear) is always empty" assumption below this
    // comment is now known WRONG - PassDrawCountProbe confirmed exactly 1
    // draw command every real frame, never 0. That single draw's content
    // was never reaching either eye's redirect target (pass 1 fell through
    // to the shared, non-per-eye offscreen_targets[0] for both eyes, every
    // time) - the leading theory for the observed corruption. Fix: redirect
    // pass 1 into a SECOND per-eye framebuffer wrapping the SAME
    // stereo_eye_targets[eye] images pass 2 uses (see
    // stereo_eye_pass1_framebuffers' declaration), so pass 2's loadOp=LOAD
    // picks up whatever pass 1 actually drew, per-eye, exactly like DOOM's
    // own real frame.
    if (inside_injected_extra_call && current_eye_render >= 0 && info && info->renderPass &&
        info->renderPass == offscreen_targets[0].render_pass) {
        const VkFramebuffer fb = stereo_eye_pass1_framebuffers[current_eye_render];
        if (fb != VK_NULL_HANDLE) {
            log::warn(std::format(
                "StereoDualEyeDump: eye={} pass1(Clear) redirect framebuffer {} -> {} "
                "(renderPass={} unchanged)",
                current_eye_render, reinterpret_cast<void*>(info->framebuffer),
                reinterpret_cast<void*>(fb), reinterpret_cast<void*>(info->renderPass)));
            VkRenderPassBeginInfo redirected = *info;
            redirected.framebuffer = fb;
            real_cmd_begin_render_pass(command_buffer, &redirected, contents);
            return;
        }
        // Not ready - same "don't skip begin, fall through to DOOM's real
        // framebuffer" safety reasoning as the RenderDepth branch above:
        // leaving a draw recorded with no active render pass is worse.
        log::error(std::format(
            "StereoDualEyeDump: eye={} pass1 redirect requested but framebuffer not ready - "
            "ordering bug, falling through to DOOM's real framebuffer (safe, wasted work)",
            current_eye_render));
    }
    // Milestone 1 (StereoDualEyeDump, F12): checked before the shared
    // offscreen_targets[] block below so each eye's content-carrying draw
    // lands in its own dedicated target instead of the shared slot0/slot1
    // pair. Only fires when current_eye_render is actually set (0 or 1) -
    // every pre-existing mechanism leaves it at -1 and is completely
    // unaffected. This is pass 2 (offscreen_targets[1], Load) specifically -
    // pass 1 is handled by the block immediately above.
    if (inside_injected_extra_call && current_eye_render >= 0 && info && info->renderPass) {
        OffscreenTarget& eye_target = stereo_eye_targets[current_eye_render];
        if (!eye_target.ready.load(std::memory_order_relaxed)) {
            // Fallback only - the driver calls ensure_stereo_eye_target_ready()
            // eagerly before RenderDepth's per-eye reinvoke, so this should
            // already be true by the time RenderOpaque's redirect fires.
            // Kept as a safety net using the exact same call, in case that
            // ordering is ever bypassed.
            ensure_stereo_eye_target_ready(current_eye_render);
        }
        if (eye_target.ready.load(std::memory_order_relaxed) && info->renderPass == eye_target.render_pass) {
            log::warn(std::format(
                "StereoDualEyeDump: eye={} redirect framebuffer {} -> {} (renderPass={} unchanged)",
                current_eye_render, reinterpret_cast<void*>(info->framebuffer),
                reinterpret_cast<void*>(eye_target.framebuffer),
                reinterpret_cast<void*>(info->renderPass)));
            VkRenderPassBeginInfo redirected = *info;
            redirected.framebuffer = eye_target.framebuffer;
            real_cmd_begin_render_pass(command_buffer, &redirected, contents);
            if (!stereo_eye_dump_done[current_eye_render].load(std::memory_order_relaxed)) {
                stereo_eye_dump_pending[current_eye_render].store(true, std::memory_order_relaxed);
            }
            return;
        }
        // Fell through: pass 1 already returned earlier (see the block
        // above this one) whenever its own per-eye framebuffer was ready, so
        // reaching here with info->renderPass neither matching pass 1 nor
        // pass 2 means an unrelated render pass - falls into the shared
        // offscreen_targets[] block below rather than the real screen, so
        // worst case it's redundant, not visible corruption.
    }
    // CompositeTargetProbe: record every render pass seen while inside the
    // frame root, so the LAST one before the frame root returns identifies
    // DOOM's final composite target.
    if (inside_real_frame_root && info) {
        last_frame_root_framebuffer = info->framebuffer;
        last_frame_root_cmd_buffer = command_buffer;
        ++frame_root_render_pass_count;
        // DrawParity pass context for the REAL call, the exact mirror of the
        // doubled call's block below. Kept adjacent to the pass counter it
        // shadows so the two can never drift apart.
        ++real_pass_seq;
        real_current_framebuffer = info->framebuffer;
        // REAL-side BEFORE image for the begin/end delta - the control for
        // "pass 25 is where the effects appear". Same image, same pass, copied
        // at entry; the AFTER is the existing real timeline slot.
        if (timeline_beginend_armed.load(std::memory_order_relaxed) &&
            real_pass_seq == kBeginEndPass) {
            VkImage src = VK_NULL_HANDLE;
            VkExtent2D ext{};
            VkFormat fmt = VK_FORMAT_UNDEFINED;
            VkImageLayout entry = VK_IMAGE_LAYOUT_UNDEFINED;
            bool entry_known = false;
            {
                std::scoped_lock lock(framebuffer_depth_view_mutex);
                if (auto f = framebuffer_color0_views.find(info->framebuffer);
                    f != framebuffer_color0_views.end()) {
                    if (auto i = image_view_to_image.find(f->second);
                        i != image_view_to_image.end()) {
                        src = i->second;
                        if (auto c = created_image_infos.find(src); c != created_image_infos.end()) {
                            ext = {c->second.extent.width, c->second.extent.height};
                            fmt = c->second.format;
                        }
                    }
                }
                if (auto il = render_pass_initial_layouts.find(info->renderPass);
                    il != render_pass_initial_layouts.end() && !il->second.empty()) {
                    entry = il->second[0];
                    entry_known = true;
                }
            }
            const auto& vkr = vulkan_state();
            auto cb = load_device_proc<PFN_vkCmdPipelineBarrier>(vkr.device, "vkCmdPipelineBarrier");
            auto cc = load_device_proc<PFN_vkCmdCopyImage>(vkr.device, "vkCmdCopyImage");
            if (src != VK_NULL_HANDLE && ext.width && entry_known && cb && cc &&
                entry != VK_IMAGE_LAYOUT_UNDEFINED) {
                std::scoped_lock lk(chainwalk_mutex);
                if (beginend_real_image == VK_NULL_HANDLE) {
                    ChainWalkSlot tmp{}; tmp.extent = ext; tmp.format = fmt;
                    if (chainwalk_ensure_slot_image(tmp)) {
                        beginend_real_image = tmp.image; beginend_real_memory = tmp.memory;
                        beginend_real_extent = ext; beginend_real_format = fmt;
                    }
                }
                if (beginend_real_image != VK_NULL_HANDLE &&
                    beginend_real_extent.width == ext.width) {
                    VkImageMemoryBarrier a{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                    a.oldLayout = entry; a.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                    a.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
                    a.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                    a.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    a.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    a.image = src; a.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    VkImageMemoryBarrier b = a;
                    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    b.image = beginend_real_image;
                    std::array<VkImageMemoryBarrier, 2> pre{a, b};
                    cb(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, pre.data());
                    VkImageCopy r{};
                    r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    r.extent = {ext.width, ext.height, 1};
                    cc(command_buffer, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       beginend_real_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
                    VkImageMemoryBarrier back = a;
                    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                    back.newLayout = entry;
                    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                    back.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                    cb(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &back);
                    beginend_real_filled = true;
                    beginend_real_pass = real_pass_seq;
                    log::warn(std::format(
                        "BeginEnd [gpu] REAL BEFORE captured: image {} at pass {} entryLayout={}",
                        reinterpret_cast<void*>(src), real_pass_seq, static_cast<int>(entry)));
                }
            } else {
                log::error(std::format(
                    "BeginEnd REAL: BEFORE not captured at pass {} (src={} entryKnown={}) - "
                    "without it the doubled side's zero delta proves nothing.",
                    real_pass_seq, reinterpret_cast<void*>(src), entry_known));
            }
        }
        // BeginInfoParity, REAL side - the half that makes the doubled side's
        // line mean anything. Same fields, same pass, so the two can be read
        // against each other directly.
        if (indirect_args_probe_armed.load(std::memory_order_relaxed) &&
            real_pass_seq == kBeginEndPass) {
            log::warn(std::format(
                "BeginInfoParity [rec] REAL    pass={} renderPass={} framebuffer={} "
                "renderArea=({},{} {}x{}) clearValues={}{}",
                real_pass_seq, reinterpret_cast<void*>(info->renderPass),
                reinterpret_cast<void*>(info->framebuffer),
                info->renderArea.offset.x, info->renderArea.offset.y,
                info->renderArea.extent.width, info->renderArea.extent.height,
                info->clearValueCount,
                (info->renderArea.extent.width == 0 || info->renderArea.extent.height == 0)
                    ? "  <<<< DEGENERATE RENDER AREA" : ""));
        }
        // PassStateParity, real side: which depth buffer this pass tests
        // against. See pass_state_record - particles at pass 25 are
        // depth-tested, so the wrong depth rejects them wholesale while the
        // opaque geometry, drawn earlier with depth writes, still looks right.
        if (pass_state_parity_armed.load(std::memory_order_relaxed)) {
            VkImageView dv = VK_NULL_HANDLE;
            VkImage di = VK_NULL_HANDLE;
            {
                std::scoped_lock lock(framebuffer_depth_view_mutex);
                if (auto f = framebuffer_any_depth_views.find(info->framebuffer);
                    f != framebuffer_any_depth_views.end()) {
                    dv = f->second;
                    if (auto i = image_view_to_image.find(dv); i != image_view_to_image.end())
                        di = i->second;
                }
            }
            pass_state_record(3, reinterpret_cast<uint64_t>(di),
                [&]{return std::format("view={} image={} (DOOM's own - the real call has no mirror by "
                            "definition){}", reinterpret_cast<void*>(dv),
                            reinterpret_cast<void*>(di),
                            di == VK_NULL_HANDLE ? "  (none resolved)" : "");});
        }
        // PASS TIMELINE, real side. Record what this pass renders into and the
        // layout it is guaranteed to end in, so vkCmdEndRenderPass can copy it.
        real_timeline_current_color = VK_NULL_HANDLE;
        real_timeline_layout_known = false;
        real_timeline_current_cmd = command_buffer;
        if (pass_timeline_armed.load(std::memory_order_relaxed)) {
            bool wanted = false;
            for (uint32_t p : kTimelinePasses) if (p == real_pass_seq) wanted = true;
            if (wanted) {
                std::scoped_lock lock(framebuffer_depth_view_mutex);
                if (auto f = framebuffer_color0_views.find(info->framebuffer);
                    f != framebuffer_color0_views.end()) {
                    if (auto i = image_view_to_image.find(f->second);
                        i != image_view_to_image.end()) {
                        real_timeline_current_color = i->second;
                        if (auto c = created_image_infos.find(i->second);
                            c != created_image_infos.end()) {
                            real_timeline_current_format = c->second.format;
                            real_timeline_current_extent = {c->second.extent.width,
                                                            c->second.extent.height};
                        }
                    }
                }
                if (auto fl = render_pass_final_layouts.find(info->renderPass);
                    fl != render_pass_final_layouts.end() && !fl->second.empty()) {
                    real_timeline_current_layout = fl->second[0];
                    real_timeline_layout_known = true;
                }
            }
        }
        // REAL-CALL COUNTERPART (2026-08-18). The comparison the adversarial
        // review asked for in §5 and that has never been made: DOOM's OWN
        // colour target at the same pass, in the same frame, as the doubled
        // call's mirror. Without it, eye 1's dump alone cannot separate "the
        // effects are missing" from "no sparks were on screen at capture
        // time" - which is exactly where the last run stalled.
        //
        // DOOM's real images are not mirrors, so they are resolved through the
        // framebuffer's colour view. Same full-screen HDR filter as the
        // doubled side, so the two lists line up pass for pass.
        if (effects_stage_dump_armed.load(std::memory_order_relaxed) &&
            real_stage_count < kEffectsStages) {
            VkImage rimg = VK_NULL_HANDLE;
            {
                std::scoped_lock lock(framebuffer_depth_view_mutex);
                if (auto f = framebuffer_color0_views.find(info->framebuffer);
                    f != framebuffer_color0_views.end()) {
                    if (auto i = image_view_to_image.find(f->second);
                        i != image_view_to_image.end()) rimg = i->second;
                }
            }
            if (rimg != VK_NULL_HANDLE) {
                CreatedImageInfo ii{};
                bool known = false;
                {
                    std::scoped_lock lock(framebuffer_depth_view_mutex);
                    if (auto c = created_image_infos.find(rimg); c != created_image_infos.end()) {
                        ii = c->second; known = true;
                    }
                }
                if (known && ii.format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 &&
                    ii.extent.width >= 1800 && ii.extent.height >= 900) {
                    bool seen = false;
                    for (uint32_t s = 0; s < real_stage_count; ++s)
                        if (real_stage_colors[s] == rimg) { seen = true; break; }
                    if (!seen) {
                        real_stage_colors[real_stage_count] = rimg;
                        real_stage_passes[real_stage_count] = real_pass_seq;
                        ++real_stage_count;
                    }
                }
            }
        }
    }
    // POST-FRAME-ROOT COVERAGE PROBE. Log-only, bounded, armed by the
    // scripted test. Every render pass DOOM begins AFTER the frame root has
    // returned is work the doubled call never performs, so eye 1 cannot have
    // it. The one that writes a swapchain image is the final grade/HUD step
    // that makes DOOM's frame differ from our pass-78 composite.
    if (post_frame_root_window && !inside_frame_root_double && !inside_real_frame_root && info &&
        post_frame_root_probe_armed.load(std::memory_order_relaxed) &&
        post_frame_root_lines.load(std::memory_order_relaxed) < kMaxPostFrameRootLines) {
        VkImageView v = VK_NULL_HANDLE;
        VkImage img = VK_NULL_HANDLE;
        {
            std::scoped_lock lock(framebuffer_depth_view_mutex);
            if (auto f = framebuffer_color0_views.find(info->framebuffer);
                f != framebuffer_color0_views.end()) {
                v = f->second;
                if (auto i = image_view_to_image.find(v); i != image_view_to_image.end())
                    img = i->second;
            }
        }
        bool is_swapchain = false;
        {
            std::unique_lock lock(swapchains_mutex);
            for (const auto& [sc, rec] : swapchains) {
                (void)sc;
                for (VkImage s : rec.images) if (s == img) { is_swapchain = true; break; }
                if (is_swapchain) break;
            }
        }
        // WHO CALLS IT (2026-08-17, run 23). Run 23 showed this window holds
        // exactly ONE render pass per frame, always the same VkRenderPass,
        // writing straight into the swapchain image - no transfers, no
        // compute. So DOOM's whole final stage is one full-screen pass, and
        // giving eye 1 an equivalent means doubling whatever DOOM function
        // contains it, exactly as the frame root is doubled today. That needs
        // the function's identity, and the return address is how this project
        // has identified every other call site (all 18 job dispatch sites).
        const void* ret = _ReturnAddress();
        const uintptr_t doom_base =
            reinterpret_cast<uintptr_t>(GetModuleHandleW(L"DOOMx64vk.exe"));
        const uintptr_t ret_rva =
            doom_base ? reinterpret_cast<uintptr_t>(ret) - doom_base : 0;
        // WHICH FUNCTION, not just which instruction (2026-08-17, run 25).
        //
        // A return address is an address INSIDE a function; hooking needs its
        // ENTRY. x64 PE images carry that in .pdata, and RtlLookupFunctionEntry
        // reads it straight out - so the entry point is resolvable at runtime
        // with no disassembler and no offline analysis.
        //
        // The caller chain matters as much as the immediate frame: doubling
        // the wrong level would re-run far more than this one pass. The stack
        // walk shows how deep the final pass sits and whether its parent is
        // something already known (the frame root is DOOM+0x17B7900), so the
        // hook can be placed deliberately rather than at whatever frame
        // happened to be nearest.
        auto function_rva = [doom_base](uintptr_t pc) -> uintptr_t {
            if (!pc || !doom_base) return 0;
            DWORD64 image_base = 0;
            PRUNTIME_FUNCTION rf =
                RtlLookupFunctionEntry(static_cast<DWORD64>(pc), &image_base, nullptr);
            if (!rf || image_base != static_cast<DWORD64>(doom_base)) return 0;
            return static_cast<uintptr_t>(rf->BeginAddress);
        };
        std::string chain;
        {
            void* frames[12]{};
            const USHORT captured = CaptureStackBackTrace(0, 12, frames, nullptr);
            for (USHORT i = 0; i < captured; ++i) {
                const uintptr_t pc = reinterpret_cast<uintptr_t>(frames[i]);
                if (!doom_base || pc < doom_base) continue;  // not DOOM code
                const uintptr_t rva = pc - doom_base;
                if (rva > 0x10000000) continue;              // outside the image
                chain += std::format(" [{}: retRva={:#x} fnRva={:#x}]", i, rva,
                                     function_rva(pc));
            }
        }
        const uint32_t n = post_frame_root_lines.fetch_add(1, std::memory_order_relaxed);
        log::warn(std::format(
            "PostFrameRoot #{} renderPass BEGIN fb={} renderPass={} colour={} extent={}x{} "
            "isSwapchainImage={} retRva={:#x} fnRva={:#x}. This pass runs AFTER the frame root "
            "returned, so the doubled call never performs it and eye 1 cannot have its output. "
            "fnRva is the ENTRY of the containing function (from .pdata). ⚠ THAT IS NOT THE "
            "SAME AS A HOOKABLE TARGET, which is what this line used to claim: 0x189A350, the "
            "fnRva printed here, turned out to be a generic helper called ~1000 times per frame "
            "across 10+ threads. A stack walk names the function a call site sits IN, not one "
            "unique to it. Read 'CallerSpecificity' for the levels that are actually specific to "
            "this pass. Chain (nearest frame first, DOOM code only):{}",
            n, reinterpret_cast<void*>(info->framebuffer),
            reinterpret_cast<void*>(info->renderPass), reinterpret_cast<void*>(img),
            info->renderArea.extent.width, info->renderArea.extent.height, is_swapchain,
            ret_rva, function_rva(reinterpret_cast<uintptr_t>(ret)), chain));
    }
    if (inside_frame_root_double && info) {
        doubled_last_framebuffer = info->framebuffer;
        ++doubled_render_pass_count;
        // DescriptorCensus pass context. Set BEFORE the mirror block below so
        // it is recorded even when mirroring returns early, and defaults to
        // "not mirrored" - the mirror block overwrites it on success, so a
        // census line can never claim a mirror that was not actually made.
        doubled_current_framebuffer = info->framebuffer;
        doubled_current_fb_mirrored = false;
        ++doubled_pass_seq;
        // R5: THE CLEAR VALUES OF THE DEPTH-DOWNSAMPLE PASSES, both sides.
        //
        // Eye 1's two R16_SFLOAT downsample mirrors read min=max=mean=1.0000
        // exactly while their depth input measures 100.6% of DOOM's. An exact
        // uniform value over a whole target is what a CLEAR looks like, not
        // what a reduction computes - but "1.0 is DOOM's clear value" has been
        // an assumption, and this prints it. Rides DrawParity's arm so it costs
        // nothing otherwise, and is capped at four lines.
        if (draw_parity_active && (doubled_pass_seq == 15 || doubled_pass_seq == 16) &&
            info->clearValueCount && info->pClearValues) {
            static std::atomic_uint32_t r5_clear_lines{0};
            if (r5_clear_lines.fetch_add(1, std::memory_order_relaxed) < 4) {
                std::string cv;
                for (uint32_t i = 0; i < info->clearValueCount && i < 4; ++i) {
                    cv += std::format(" [{}] colorF=({:.4f},{:.4f},{:.4f},{:.4f}) depth={:.4f}"
                                      " stencil={}",
                        i, info->pClearValues[i].color.float32[0],
                        info->pClearValues[i].color.float32[1],
                        info->pClearValues[i].color.float32[2],
                        info->pClearValues[i].color.float32[3],
                        info->pClearValues[i].depthStencil.depth,
                        info->pClearValues[i].depthStencil.stencil);
                }
                log::warn(std::format(
                    "R5ClearValues DOUBLED pass={} framebuffer={} clearValueCount={}{}. If the "
                    "colour clear here is 1.0 and DrawParity shows DOUBLED draws=0 at this pass, "
                    "the saturated downsample mirror is simply this clear showing through and "
                    "nothing ever drew over it.",
                    doubled_pass_seq, reinterpret_cast<void*>(info->framebuffer),
                    info->clearValueCount, cv));
            }
        }
        // ATTACHMENT CENSUS AT PASS 25 (2026-08-19). Checks the premise every
        // conclusion of the last several runs rests on.
        //
        // MirrorTarget tracks color0 and NOTHING ELSE - there is not one
        // reference to a second colour attachment in this file - and every
        // capture built here (PassTimeline, BeginEnd) reads color0. But
        // multi-colour framebuffers demonstrably exist in this game: the log
        // has 118 with colour=3 and 173 with colour=2. If pass 25's is one of
        // them, the effects could be written to attachment 1 and be invisible
        // to every instrument, and "the doubled call's pass 25 writes nothing"
        // would be measuring the wrong image rather than a real discard.
        if (indirect_args_probe_armed.load(std::memory_order_relaxed) &&
            doubled_pass_seq == kBeginEndPass) {
            std::vector<VkImageView> atts;
            {
                std::scoped_lock lk(mirror_mutex);
                if (auto it = framebuffer_infos.find(info->framebuffer);
                    it != framebuffer_infos.end()) atts = it->second.attachments;
            }
            std::string detail;
            uint32_t colour_count = 0;
            for (size_t i = 0; i < atts.size(); ++i) {
                VkImage img = VK_NULL_HANDLE;
                int fmt = 0;
                {
                    std::scoped_lock lock(framebuffer_depth_view_mutex);
                    if (auto v = image_view_to_image.find(atts[i]); v != image_view_to_image.end()) {
                        img = v->second;
                        if (auto c = created_image_infos.find(img); c != created_image_infos.end())
                            fmt = static_cast<int>(c->second.format);
                    }
                }
                const bool is_depth = fmt >= 124 && fmt <= 130;
                if (!is_depth) ++colour_count;
                detail += std::format(" [{}{}={} fmt={}]", is_depth ? "depth" : "colour", i,
                                      reinterpret_cast<void*>(img), fmt);
            }
            // BEGIN-INFO PARITY. The last unexamined state at pass 25.
            //
            // Viewport and scissor were compared, but those are DYNAMIC state.
            // VkRenderPassBeginInfo itself never has been: renderArea clips
            // every fragment regardless of viewport and scissor, so a
            // degenerate one discards the whole pass silently - and this is
            // the struct we COPY AND MODIFY when substituting the mirror
            // framebuffer, which makes it the most likely place for the
            // doubled call to differ from the real one.
            log::warn(std::format(
                "BeginInfoParity [rec] {} pass={} renderPass={} framebuffer={} renderArea=({},{} "
                "{}x{}) clearValues={}{}",
                inside_frame_root_double ? "DOUBLED" : "REAL   ", doubled_pass_seq,
                reinterpret_cast<void*>(info->renderPass),
                reinterpret_cast<void*>(info->framebuffer),
                info->renderArea.offset.x, info->renderArea.offset.y,
                info->renderArea.extent.width, info->renderArea.extent.height,
                info->clearValueCount,
                (info->renderArea.extent.width == 0 || info->renderArea.extent.height == 0)
                    ? "  <<<< DEGENERATE RENDER AREA - clips every fragment in the pass" : ""));
            log::warn(std::format(
                "AttachmentCensus [rec] DOUBLED pass={} framebuffer={} attachments={} "
                "colourAttachments={}{} | {}",
                doubled_pass_seq, reinterpret_cast<void*>(info->framebuffer), atts.size(),
                colour_count, detail,
                colour_count > 1
                    ? "MORE THAN ONE COLOUR TARGET - every capture in this project reads color0 "
                      "only, so anything written to the others has been invisible and 'pass 25 "
                      "writes nothing' may be measuring the wrong image."
                    : "single colour target - color0 is the whole output, so the captures were "
                      "looking at the right image and the discard is real."));
        }
        // PRIVATE QUERY POOL: create once, then reset the whole pool at the
        // FIRST pass of every doubled call. vkCmdResetQueryPool is illegal
        // inside a render pass, and this point - before real_cmd_begin_render_
        // pass - is outside one. Resetting the whole pool each call is what
        // makes every subsequent begin legal.
        if (doubled_pass_seq == 1 && redirect_doubled_queries.load(std::memory_order_relaxed) &&
            !private_query_pool_failed.load(std::memory_order_relaxed)) {
            const auto& vkq = vulkan_state();
            if (private_query_pool == VK_NULL_HANDLE && vkq.device) {
                auto create_pool = load_device_proc<PFN_vkCreateQueryPool>(
                    vkq.device, "vkCreateQueryPool");
                VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
                qi.queryType = VK_QUERY_TYPE_OCCLUSION;
                qi.queryCount = kPrivateQuerySlots;
                if (!create_pool ||
                    create_pool(vkq.device, &qi, nullptr, &private_query_pool) != VK_SUCCESS) {
                    private_query_pool = VK_NULL_HANDLE;
                    private_query_pool_failed.store(true, std::memory_order_relaxed);
                    log::error("DoubledQuery: could not create the private occlusion pool - the "
                               "doubled call keeps writing DOOM's unreset slots, so an unchanged "
                               "headset this run means NOTHING.");
                } else {
                    log::warn(std::format(
                        "DoubledQuery: private occlusion pool {} created with {} slots.",
                        reinterpret_cast<void*>(private_query_pool), kPrivateQuerySlots));
                }
            }
            doubled_query_pool_reset_this_call = false;
            // Any redirect left open by the previous doubled call is void: its
            // slots are about to be reset. Cleared so the first end of this
            // call cannot consume a stale slot from the last one.
            doubled_query_redirect_active = false;
            if (private_query_pool != VK_NULL_HANDLE) {
                if (auto reset_pool = load_device_proc<PFN_vkCmdResetQueryPool>(
                        vkq.device, "vkCmdResetQueryPool")) {
                    reset_pool(command_buffer, private_query_pool, 0, kPrivateQuerySlots);
                    doubled_query_next = 0;
                    doubled_query_pool_reset_this_call = true;
                }
            }
        }
        // BEFORE-image for the begin/end delta. Copied here, before the render
        // pass begins, from the SAME mirror the pass is about to draw into -
        // so the AFTER copy at pass end is a like-for-like comparison rather
        // than two different buffers.
        if (timeline_beginend_armed.load(std::memory_order_relaxed) &&
            doubled_pass_seq == kBeginEndPass) {
            VkImage src = VK_NULL_HANDLE;
            VkImageLayout have = VK_IMAGE_LAYOUT_UNDEFINED;
            VkExtent2D ext{};
            VkFormat fmt = VK_FORMAT_UNDEFINED;
            {
                std::scoped_lock lk(mirror_mutex);
                if (const MirrorTarget* mt = ensure_mirror_framebuffer(info->framebuffer)) {
                    if (mt->color0 != VK_NULL_HANDLE) {
                        src = mt->color0; ext = mt->extent; fmt = mt->color0_format;
                        have = mirror_layout_of(src);
                    }
                }
            }
            const auto& vkb = vulkan_state();
            auto cb = load_device_proc<PFN_vkCmdPipelineBarrier>(vkb.device, "vkCmdPipelineBarrier");
            auto cc = load_device_proc<PFN_vkCmdCopyImage>(vkb.device, "vkCmdCopyImage");
            if (src != VK_NULL_HANDLE && cb && cc) {
                std::scoped_lock lk(chainwalk_mutex);
                if (beginend_image == VK_NULL_HANDLE) {
                    ChainWalkSlot tmp{}; tmp.extent = ext; tmp.format = fmt;
                    if (chainwalk_ensure_slot_image(tmp)) {
                        beginend_image = tmp.image; beginend_memory = tmp.memory;
                        beginend_extent = ext; beginend_format = fmt;
                    }
                }
                if (beginend_image != VK_NULL_HANDLE && beginend_extent.width == ext.width) {
                    VkImageMemoryBarrier a{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                    a.oldLayout = have; a.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                    a.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
                    a.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                    a.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    a.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    a.image = src; a.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    VkImageMemoryBarrier b = a;
                    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    b.image = beginend_image;
                    std::array<VkImageMemoryBarrier, 2> pre{a, b};
                    cb(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, pre.data());
                    VkImageCopy r{};
                    r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    r.extent = {ext.width, ext.height, 1};
                    cc(command_buffer, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       beginend_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
                    VkImageMemoryBarrier back = a;
                    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                    back.newLayout = have;
                    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                    back.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                    cb(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &back);
                    beginend_filled = true;
                    beginend_pass = doubled_pass_seq;
                    log::warn(std::format(
                        "BeginEnd [gpu] BEFORE captured: mirror {} at pass {} {}x{} - the AFTER "
                        "copy is the same image at this pass's end, so their difference is what "
                        "THIS pass drew.", reinterpret_cast<void*>(src), doubled_pass_seq,
                        ext.width, ext.height));
                }
            } else {
                log::error(std::format(
                    "BeginEnd: no mirror colour0 for the pass-{} framebuffer - BEFORE not "
                    "captured, so this run's comparison means nothing.", doubled_pass_seq));
            }
        }
        // FRAGMENT REJECTION EXPERIMENT. Acts on OUR mirror's depth image only,
        // before the render pass begins (a clear is illegal inside one), and
        // only at the first pass of the effects range so it happens once per
        // doubled call. See doubled_depth_experiment.
        if (const uint32_t mode = doubled_depth_experiment.load(std::memory_order_relaxed);
            mode != 0 && doubled_pass_seq >= kDepthExperimentFirstPass &&
            doubled_pass_seq <= kDepthExperimentLastPass) {
            VkImage mirror_depth = VK_NULL_HANDLE;
            VkImageLayout have = VK_IMAGE_LAYOUT_UNDEFINED;
            // Resolve through the path PassStateParity already PROVED works on
            // this framebuffer - framebuffer_any_depth_views -> image ->
            // mirrored_images - which is how it reported mirror=0x214bd62aed0
            // mirrorWritten=true at pass 25. The previous version asked
            // MirrorTarget::depth_images instead, which is a different
            // structure and is empty here, so it found nothing and applied
            // nothing. MirrorTarget stays as a fallback rather than a
            // replacement.
            VkImage doom_depth = VK_NULL_HANDLE;
            {
                std::scoped_lock lock(framebuffer_depth_view_mutex);
                if (auto f = framebuffer_any_depth_views.find(info->framebuffer);
                    f != framebuffer_any_depth_views.end()) {
                    if (auto i = image_view_to_image.find(f->second);
                        i != image_view_to_image.end()) doom_depth = i->second;
                }
            }
            {
                std::scoped_lock lk(mirror_mutex);
                if (doom_depth != VK_NULL_HANDLE) {
                    if (auto m = mirrored_images.find(doom_depth); m != mirrored_images.end())
                        mirror_depth = m->second;
                }
                if (mirror_depth == VK_NULL_HANDLE) {
                    if (const MirrorTarget* mt = ensure_mirror_framebuffer(info->framebuffer))
                        if (!mt->depth_images.empty()) mirror_depth = mt->depth_images[0];
                }
                if (mirror_depth != VK_NULL_HANDLE) have = mirror_layout_of(mirror_depth);
            }
            const auto& vk = vulkan_state();
            auto cmd_barrier = load_device_proc<PFN_vkCmdPipelineBarrier>(
                vk.device, "vkCmdPipelineBarrier");
            auto cmd_clear = load_device_proc<PFN_vkCmdClearDepthStencilImage>(
                vk.device, "vkCmdClearDepthStencilImage");
            if (mirror_depth != VK_NULL_HANDLE && cmd_barrier && cmd_clear) {
                // Clear only the aspect under test, so mode 1/2 leave depth
                // alone and mode 3 leaves stencil alone - otherwise a positive
                // result would not say WHICH of the two admitted the fragments.
                const VkImageAspectFlags aspect = (mode >= 3)
                    ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_STENCIL_BIT;
                VkImageSubresourceRange range{aspect, 0, 1, 0, 1};
                VkImageMemoryBarrier to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                to_dst.oldLayout = have;
                to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                to_dst.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
                to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                to_dst.image = mirror_depth;
                to_dst.subresourceRange = {
                    VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
                cmd_barrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_dst);
                VkClearDepthStencilValue cv{};
                // REVERSED-Z, measured 2026-08-19 rather than assumed.
                //
                // Mode 3 clears depth to 1.0 and was documented as "far plane,
                // every LESS/LEQUAL test passes". Live, it did the opposite:
                // the right eye lost everything except the sky and the HUD,
                // with the near geometry culled hardest. That is the signature
                // of REVERSED-Z - 1.0 is the NEAR plane and the test is
                // GREATER/GEQUAL, so clearing to 1.0 rejects the whole scene,
                // and only the passes that do not depth-test normally (sky,
                // HUD) survive.
                //
                // So 1.0 tests nothing about the effects. Mode 4 clears to 0.0,
                // which under reversed-Z is the far plane and admits EVERY
                // fragment. That is the actual test: if the sparks and fog
                // appear at mode 4, the depth test was rejecting them.
                cv.depth = (mode == 4) ? 0.0f : 1.0f;
                cv.stencil = (mode == 2) ? 0xFFu : 0u;
                cmd_clear(command_buffer, mirror_depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          &cv, 1, &range);
                // Back to exactly where it was, or the render pass we are about
                // to begin has a false initialLayout.
                VkImageMemoryBarrier restore = to_dst;
                restore.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                restore.newLayout = have;
                restore.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                restore.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                cmd_barrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &restore);
                if (doubled_depth_experiment_applied.fetch_add(1, std::memory_order_relaxed) < 8) {
                    log::warn(std::format(
                        "DepthExperiment APPLIED mode={} to MIRROR depth {} at pass {} "
                        "(aspect={}, depth=1.0, stencil={}). DOOM's own depth is untouched. "
                        "The set of passes listed here is also where depth actually lives.",
                        mode, reinterpret_cast<void*>(mirror_depth), doubled_pass_seq,
                        mode == 3 ? "DEPTH" : "STENCIL", cv.stencil));
                }
            } else if (doubled_depth_experiment_missed.fetch_add(1,
                           std::memory_order_relaxed) < 6) {
                // Says WHICH lookup failed. The previous version reported only
                // "no mirror depth image", which was true but useless: the
                // real reason was that pass 22 has no depth attachment at all,
                // and the message could not distinguish that from "there is a
                // depth attachment but we hold no mirror for it".
                log::error(std::format(
                    "DepthExperiment: NOTHING APPLIED at pass {} - doomDepth={} mirrorDepth={}. "
                    "{} An unchanged headset means NOTHING this run.",
                    doubled_pass_seq, reinterpret_cast<void*>(doom_depth),
                    reinterpret_cast<void*>(mirror_depth),
                    doom_depth == VK_NULL_HANDLE
                        ? "This framebuffer has NO depth attachment - wrong pass chosen."
                        : "Depth attachment exists but we hold no mirror for it."));
            }
        }
        // PassStateParity, doubled side. Logs DOOM's ORIGINAL depth attachment
        // for this framebuffer plus whether we hold a mirror for it, so the
        // two sides' lines are comparable: the real call prints DOOM's image,
        // and this prints DOOM's image AND what the doubled call will actually
        // test against. mirror=none here means the doubled call depth-tests
        // eye 1's particles against eye 0's depth.
        if (pass_state_parity_armed.load(std::memory_order_relaxed)) {
            VkImageView dv = VK_NULL_HANDLE;
            VkImage di = VK_NULL_HANDLE;
            {
                std::scoped_lock lock(framebuffer_depth_view_mutex);
                if (auto f = framebuffer_any_depth_views.find(info->framebuffer);
                    f != framebuffer_any_depth_views.end()) {
                    dv = f->second;
                    if (auto i = image_view_to_image.find(dv); i != image_view_to_image.end())
                        di = i->second;
                }
            }
            VkImage mirror = VK_NULL_HANDLE;
            bool written = false;
            if (di != VK_NULL_HANDLE) {
                std::scoped_lock lk(mirror_mutex);
                if (auto m = mirrored_images.find(di); m != mirrored_images.end()) {
                    mirror = m->second;
                    // THE question, not just "does a mirror exist": a private
                    // depth buffer nothing ever rendered into is worse than
                    // none, because every particle depth-tests against
                    // whatever it was cleared to.
                    written = mirror_images_written.count(mirror) != 0;
                }
            }
            pass_state_record(3, reinterpret_cast<uint64_t>(di),
                [&]{return std::format("view={} image={} mirror={} mirrorWritten={}{}",
                    reinterpret_cast<void*>(dv), reinterpret_cast<void*>(di),
                    mirror ? std::format("{}", reinterpret_cast<void*>(mirror)) : "NONE",
                    written,
                    di == VK_NULL_HANDLE
                        ? "  (no depth attachment resolved - this framebuffer has none, or its "
                          "views were created before our hook)"
                        : (!mirror ? "  <<<< no private depth: eye 1's particles depth-test "
                                     "against eye 0's geometry"
                                   : (!written ? "  <<<< private depth exists but NOTHING EVER "
                                                 "RENDERED INTO IT - every particle tests against "
                                                 "cleared depth"
                                               : "")));});
        }
        // SHADOW IDENTITY (2026-08-18). The suppression fix targeted the
        // shared 16384 atlas framebuffer, but I never verified that the
        // DIVERGENT passes (3 and 5, per DrawParity) actually render into it -
        // they may be the main-view depth prepass, which we MIRROR rather than
        // share, so suppression keyed on the atlas would never match them. One
        // line per doubled pass, first frame only: handle, extent, and whether
        // it equals the recorded shared atlas. Read alongside DrawParity's pass
        // numbers to see exactly which framebuffer the divergent shadow draws
        // hit and whether the fix targets it.
        if (shadow_identity_lines.load(std::memory_order_relaxed) < kMaxShadowIdentityLines) {
            uint32_t fw = 0, fh = 0;
            {
                std::scoped_lock lock(framebuffer_depth_view_mutex);
                if (auto it = framebuffer_infos.find(info->framebuffer);
                    it != framebuffer_infos.end()) { fw = it->second.width; fh = it->second.height; }
            }
            const uintptr_t atlas = shared_shadow_framebuffer.load(std::memory_order_relaxed);
            const uint32_t n = shadow_identity_lines.fetch_add(1, std::memory_order_relaxed);
            log::warn(std::format(
                "ShadowIdentity doubledPass={} fb={} extent={}x{} isSharedAtlas={} "
                "suppressionTarget={:#x}",
                doubled_pass_seq, reinterpret_cast<void*>(info->framebuffer), fw, fh,
                reinterpret_cast<uintptr_t>(info->framebuffer) == atlas, atlas));
        }
        // ChainWalk pass context. Cleared FIRST, like the census's own
        // fb_mirrored default, so a pass that is not mirrored cannot inherit
        // the previous pass's colour image and have its snapshot attributed
        // to the wrong stage. The mirror block below fills them in on
        // success.
        chainwalk_current_color = VK_NULL_HANDLE;
        chainwalk_current_format = VK_FORMAT_UNDEFINED;
        chainwalk_current_extent = VkExtent2D{};
        chainwalk_current_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        chainwalk_current_cmd = command_buffer;
        chainwalk_current_rva = doubled_current_job_rva;
        if (chainwalk_observe.load(std::memory_order_relaxed)) {
            const uint32_t slot = doubled_pass_seq < kCensusPassSlots
                                      ? doubled_pass_seq : kCensusPassSlots - 1;
            chainwalk_map[slot] = ChainWalkPass{doubled_current_job_rva, info->framebuffer,
                                                false, VK_NULL_HANDLE, VK_FORMAT_UNDEFINED,
                                                VkExtent2D{}, false};
        }
    }
    // GENERIC MIRRORING: during the doubled call, swap every framebuffer for
    // our own mirror so the second render is isolated by construction.
    // Also fires for the DOUBLED FINAL PASS (2026-08-18): without a mirrored
    // framebuffer the second invocation writes DOOM's swapchain image and
    // corrupts the presented frame. hook_final_stage pre-flights this, so a
    // miss here should be impossible - it refuses to re-invoke without one.
    //
    // MONITOR EYE-1 VIEW deliberately declines that mirror for the final pass
    // (2026-08-29) - see monitor_eye1_direct. Writing DOOM's swapchain image
    // is the FEATURE there, not the hazard: it is what puts eye 1 on the
    // monitor. Scoped to inside_final_pass_double ONLY, so the frame-root
    // doubling keeps its isolation in every mode.
    const bool monitor_direct_pass =
        inside_final_pass_double && monitor_eye1_direct.load(std::memory_order_relaxed);
    if (monitor_direct_pass) monitor_eye1_passes.fetch_add(1, std::memory_order_relaxed);
    if ((inside_frame_root_double || inside_final_pass_double) && info &&
        !monitor_direct_pass &&
        info->framebuffer != VK_NULL_HANDLE &&
        mirror_enabled.load(std::memory_order_relaxed)) {
        // MIRROR COVERAGE CENSUS (2026-08-22). The remaining execution
        // property, and the last one that can still explain the deficit.
        //
        // With the pass range corrected, the two calls are handed IDENTICAL
        // inputs at and before p15 where the loss occurs: 56 bindings matched,
        // 0 differing, no side-only, no truncation. Same passes, same draws,
        // same indices, same depth, same camera, same constants, same froxel
        // bytes, no descriptor arrays. Identical inputs cannot produce
        // different output, so the divergence must be in where the output GOES.
        //
        // If ensure_mirror_framebuffer MISSES for a doubled pass, the code
        // below falls through and that pass renders into DOOM'S REAL TARGET.
        // Its contribution then lands in eye 0's image and eye 1's mirror never
        // receives it - which is exactly "the doubled call draws the effects
        // (DrawParity confirms identical draws) and eye 1 does not have them".
        // It would also explain the deficit being COMPLETE at the first HDR
        // stage rather than fading in gradually.
        //
        // Bounded to one frame's worth of lines, and both outcomes are logged:
        // a census that only reported misses could not distinguish "no misses"
        // from "the census never ran".
        if (inside_frame_root_double &&
            mirror_coverage_lines.load(std::memory_order_relaxed) < kMaxMirrorCoverageLines) {
            const bool hit = mirror_targets.find(info->framebuffer) != mirror_targets.end();
            if (!hit) mirror_coverage_misses.fetch_add(1, std::memory_order_relaxed);
            mirror_coverage_lines.fetch_add(1, std::memory_order_relaxed);
            log::warn(std::format(
                "MirrorCoverage doubledPass={} fb={} mirrored={} jobRva={:#x}{}",
                doubled_pass_seq, reinterpret_cast<void*>(info->framebuffer), hit,
                doubled_current_job_rva,
                hit ? "" : "  <<<< UNMIRRORED: this pass renders into DOOM'S OWN target, so "
                           "whatever it draws goes to eye 0 and eye 1 never receives it"));
        }
        // Held across the ensure AND the use of what it returns: the pointer
        // is into mirror_targets, which a destroy hook may erase.
        std::scoped_lock mirror_lock(mirror_mutex);
        if (const MirrorTarget* m = ensure_mirror_framebuffer(info->framebuffer)) {
            // FINAL STAGE (2026-08-18). The doubled final pass's colour target
            // IS eye 1's finished frame - graded, with the HUD - so it is
            // captured under its own flag and never through the frame-root
            // path below, whose eye1_source_candidate/job-RVA filtering is
            // about world jobs and would reject this pass outright.
            if (inside_final_pass_double && m->color0 != VK_NULL_HANDLE) {
                final_pass_mirror_color = m->color0;
                final_pass_mirror_format = m->color0_format;
                final_pass_mirror_extent = m->extent;
            }
            // EFFECTS STAGE DUMP (2026-08-18). Capture the doubled call's own
            // mirror at the two stages that decide the effects: after lighting
            // (glow/emissive should be present) and after forward/transparency
            // (fire, sparks, additive). If the effects are in one and gone by
            // the next, that pass is where they are lost. Handles only - the
            // pixels do not exist until DOOM submits, so the dump itself is
            // deferred to the present thread, same as the final stage.
            // Selected by SHAPE, not pass number (the last run pinned pass
            // 18/27 from a stale map and captured two 954x508 side buffers).
            // Keep the FIRST full-screen HDR mirror (post-lighting) and keep
            // overwriting the LAST (final composite); their difference is
            // where the additive effects should have entered.
            if (inside_frame_root_double && effects_stage_dump_armed.load(
                    std::memory_order_relaxed) && m->color0 != VK_NULL_HANDLE &&
                m->extent.width >= 1800 && m->extent.height >= 900 &&
                m->color0_format == VK_FORMAT_B10G11R11_UFLOAT_PACK32) {
                bool already = false;
                for (uint32_t s = 0; s < effects_stage_count; ++s)
                    if (effects_stage_colors[s] == m->color0) { already = true; break; }
                if (!already && effects_stage_count < kEffectsStages) {
                    effects_stage_colors[effects_stage_count] = m->color0;
                    effects_stage_passes[effects_stage_count] = doubled_pass_seq;
                    effects_stage_extent_shared = m->extent;
                    ++effects_stage_count;
                }
                if (effects_stage_light_color == VK_NULL_HANDLE) {
                    effects_stage_light_color = m->color0;
                    effects_stage_light_format = m->color0_format;
                    effects_stage_light_extent = m->extent;
                    effects_stage_light_pass = doubled_pass_seq;
                }
                effects_stage_xparency_color = m->color0;
                effects_stage_xparency_format = m->color0_format;
                effects_stage_xparency_extent = m->extent;
                effects_stage_xparency_pass = doubled_pass_seq;
            }
            // Only COLOUR-carrying mirrors are candidates for eye 1. Depth-only
            // framebuffers (DOOM's shadow atlas among them - verified in the
            // live log: 16384x16384, one attachment, format 125 = X8_D24) have
            // no colour image at all, so blindly assigning color0 here would
            // null out the last good eye-1 source and silently drop the
            // presentation path back to the legacy offscreen target.
            // Only the world jobs' output is a candidate for eye 1 - see
            // is_world_job_rva. Without this the HUD's own mirror wins by
            // being last.
            // NEVER let the doubled FINAL pass become eye 1's source by
            // accident (2026-08-18). It did: eye1_source_candidate returns
            // true unconditionally while mirror_unfiltered is set, so the
            // final stage's mirror was stored as eye 1 and the owner saw a
            // fully graded right eye with the HUD - and NO PARALLAX, because
            // that pass reads DOOM's real composite rather than eye 1's. The
            // pair was also format-mismatched (122 vs 44), which sends the
            // two eyes down different colour paths.
            //
            // Full quality with the wrong viewpoint is not an improvement on
            // correct stereo, so eye 1 goes back to the frame-root composite
            // and the final-stage output stays diagnostic until its read is
            // genuinely redirected.
            if (!inside_final_pass_double && m->color0 != VK_NULL_HANDLE &&
                eye1_source_candidate(doubled_current_job_rva)) {
                store_doubled_mirror(m->color0, m->color0_format, m->extent,
                                     g_frame_root_serial.load(std::memory_order_relaxed));
                doubled_mirror_source_job = doubled_current_job_rva;
            }
            // Independent of eye 1: always remember RenderOpaque's own
            // output, so the A/B dump can measure the G-buffer under either
            // filter state and compare like for like against the 13.9x
            // filtered result.
            if (m->color0 != VK_NULL_HANDLE &&
                doubled_current_job_rva == kRenderOpaqueFnRva) {
                doubled_gbuffer_mirror = m->color0;
                doubled_gbuffer_format = m->color0_format;
                doubled_gbuffer_extent = m->extent;
            }
            doubled_current_fb_mirrored = true;
            // This pass will leave each mirrored attachment in the render
            // pass's declared finalLayout. Recorded so the present blit and
            // the A/B dump can name the layout the image is ACTUALLY in
            // rather than assuming COLOR_ATTACHMENT_OPTIMAL.
            // ChainWalk needs THIS pass's own declared finalLayout for
            // color0, not the tracked layout in general: a snapshot barrier
            // that names the wrong oldLayout is undefined behaviour, and
            // undefined behaviour on a mirror image is what hung the GPU
            // three times this stream. If this render pass has no recorded
            // finalLayouts we do not guess - we skip the snapshot and say so.
            bool color0_final_known = false;
            VkImageLayout color0_final = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            {
                std::vector<VkImageLayout> finals;
                {
                    std::scoped_lock rp(render_pass_layouts_mutex);
                    if (auto f = render_pass_final_layouts.find(info->renderPass);
                        f != render_pass_final_layouts.end())
                        finals = f->second;
                }
                if (!finals.empty()) {
                    std::vector<VkImageView> src_attachments;
                    {
                        std::scoped_lock fb(framebuffer_depth_view_mutex);
                        if (auto it = framebuffer_infos.find(info->framebuffer);
                            it != framebuffer_infos.end())
                            src_attachments = it->second.attachments;
                    }
                    for (size_t i = 0; i < src_attachments.size() && i < finals.size(); ++i) {
                        if (auto mv = mirrored_views.find(src_attachments[i]);
                            mv != mirrored_views.end()) {
                            set_mirror_range_layout(mv->second.image,{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1},finals[i]);
                            if (mv->second.image == m->color0) {
                                color0_final_known = true;
                                color0_final = finals[i];
                            }
                            // This pass renders into it, so from here on it
                            // holds real content in a known layout and
                            // becomes eligible for substitution.
                            if (mirror_images_written.insert(mv->second.image).second)
                                mirror_generation.fetch_add(1, std::memory_order_relaxed);
                            if (inside_frame_root_double)
                                h2_doubled_written_this_frame.insert(mv->second.image);
                        }
                    }
                }
            }
            // ChainWalk (STEP 6.6). Everything vkCmdEndRenderPass needs to
            // snapshot this stage, captured HERE where mirror_mutex is
            // already held (no new lock, per Rule 9).
            if (m->color0 != VK_NULL_HANDLE && color0_final_known) {
                chainwalk_current_color = m->color0;
                chainwalk_current_format = m->color0_format;
                chainwalk_current_extent = m->extent;
                chainwalk_current_layout = color0_final;
            }
            if (m->color0 != VK_NULL_HANDLE) {
                if (chainwalk_observe.load(std::memory_order_relaxed)) {
                    const uint32_t slot = doubled_pass_seq < kCensusPassSlots
                                              ? doubled_pass_seq : kCensusPassSlots - 1;
                    chainwalk_map[slot] = ChainWalkPass{doubled_current_job_rva,
                                                        info->framebuffer, true, m->color0,
                                                        m->color0_format, m->extent,
                                                        color0_final_known};
                }
            } else if (chainwalk_observe.load(std::memory_order_relaxed)) {
                const uint32_t slot = doubled_pass_seq < kCensusPassSlots
                                          ? doubled_pass_seq : kCensusPassSlots - 1;
                chainwalk_map[slot].mirrored = true;
            }
            VkRenderPassBeginInfo redirected = *info;
            redirected.framebuffer = m->framebuffer;
            real_cmd_begin_render_pass(command_buffer, &redirected, contents);
            return;
        }
    }
    if (inside_frame_root_double && info && doubled_fb_census.size() < 128) {
        // FRAMEBUFFER CENSUS: how many DISTINCT framebuffers does one doubled
        // run touch? Sizes the "redirect more passes" approach before a line
        // of it is written. A handful is tractable to mirror; dozens is not,
        // and we should pivot rather than grind.
        doubled_fb_census.insert(info->framebuffer);
    }
    if (inside_injected_extra_call && info) {
        for (uint32_t slot = 0; slot < offscreen_targets.size(); ++slot) {
            OffscreenTarget& target = offscreen_targets[slot];
            if (!target.ready.load(std::memory_order_relaxed) || info->renderPass != target.render_pass)
                continue;
            const uint32_t attempt = redirect_attempt_count.fetch_add(1, std::memory_order_relaxed);
            // Sampled, not unconditional: F6+F7's original 20-attempt burst
            // could log every one cheaply, but StereoPreview can drive this
            // into the tens of thousands over several minutes - logging (and
            // log.cpp flushes to disk on every line) that often would add
            // real per-frame I/O overhead on top of what this is already
            // testing. First 20 plus every 500th thereafter is enough to
            // confirm it's still working without the volume/overhead.
            if (attempt < 20 || attempt % 500 == 0) {
                log::warn(std::format(
                    "REDIRECT slot={} attempt={} framebuffer {} -> {} (renderPass={} unchanged)",
                    slot, attempt, reinterpret_cast<void*>(info->framebuffer),
                    reinterpret_cast<void*>(target.framebuffer),
                    reinterpret_cast<void*>(info->renderPass)));
            }
            VkFramebuffer use_fb = target.framebuffer;
            // SHARED-IMAGE FIX (2026-08-16). DOOM's own two RenderOpaque
            // framebuffers have byte-identical VkImageView handles - both
            // internal passes render into the SAME images, pass 1 with
            // loadOp=CLEAR and pass 2 with loadOp=LOAD. Our offscreen_targets
            // broke that: slot 0 and slot 1 own SEPARATE image sets, so pass
            // 1 cleared images nobody reads while pass 2 loaded slot 1's
            // images, which nothing ever clears. Slot 1 therefore accumulated
            // stale content across frames indefinitely.
            //
            // That contaminated every dump taken from slot 1: geometry from
            // previous frames showed through wherever the current camera's
            // draws did not cover, which is why a 200 uu camera move left the
            // near doorframe looking pixel-identical. It also invalidates the
            // per-row disparity gradient measured from those dumps.
            //
            // Fix: for slot 1, render into a framebuffer built over SLOT 0's
            // images against slot 1's own render pass - reproducing DOOM's
            // actual arrangement, so pass 1's clear is what pass 2 loads.
            if (slot == 1 && offscreen_targets[0].ready.load(std::memory_order_relaxed)) {
                if (offscreen_pass2_shared_fb == VK_NULL_HANDLE) {
                    if (!prepare_offscreen_target_secondary_framebuffer(
                            offscreen_targets[0], target.render_pass, offscreen_pass2_shared_fb)) {
                        offscreen_pass2_shared_fb = VK_NULL_HANDLE;
                    }
                }
                if (offscreen_pass2_shared_fb != VK_NULL_HANDLE) use_fb = offscreen_pass2_shared_fb;
            }
            VkRenderPassBeginInfo redirected = *info;
            redirected.framebuffer = use_fb;
            real_cmd_begin_render_pass(command_buffer, &redirected, contents);
            if (!offscreen_dump_done[slot].load(std::memory_order_relaxed)) {
                offscreen_dump_pending[slot].store(true, std::memory_order_relaxed);
            }
            return;
        }
    }
    // ===== RPCENSUS (2026-08-31 night) - THE FALL-THROUGH COUNT ===========
    //
    // This is the ONLY exit from this function that hands DOOM's OWN
    // framebuffer to a render pass. Every other exit redirected to a mirror
    // and returned before here.
    //
    // WHY IT MATTERS. STEPLADDER put all three defects at scripted step 2 -
    // the moment live doubling is armed - and at step 2 the following are all
    // eliminated with engagement proven: every one of the 13 jobs, the doubled
    // final pass, both together, the frame rate, and barrier substitution.
    // What still happens with ZERO jobs dispatched is that the doubled call
    // begins and ends RENDER PASSES. A pass that falls through to here is
    // begun on DOOM'S REAL FRAMEBUFFER, and if it carries clears
    // (clearValueCount > 0) its loadOp wipes DOOM's own attachment - twice per
    // displayed frame, reachable by nothing tested tonight.
    //
    // This file already calls that path "safe, wasted work" at the
    // StereoDualEyeDump fall-through. Safe is an ASSUMPTION about loadOp that
    // has never been measured. This measures it. LOG-ONLY - nothing is
    // redirected, suppressed or changed here.
    if (inside_frame_root_double || inside_final_pass_double) {
        g_rpc_fellthrough.fetch_add(1, std::memory_order_relaxed);
        // ===== RPFIX (2026-08-31 night) - MEASURED, THEN FIXED =============
        //
        // The census ran first and came back: fellThrough=3,728 and
        // withClears=3,728 - EVERY fall-through carries clears, none is a
        // harmless one. They are full screen size (3818x2032,
        // clearValueCount=2) on one render pass, alternating across DOOM's two
        // framebuffers, roughly once per displayed frame.
        //
        // So ~once a frame the doubled call begins a CLEARING full-screen
        // render pass on DOOM'S OWN framebuffer. It falls through because
        // every redirect branch above is keyed to a SPECIFIC render pass and
        // this one is not among them - the partial-substitution class the
        // barrier finding already demonstrated is real, one level up.
        //
        // The fix is what every other branch already does: send it to the
        // mirror. If no mirror can be built we do NOT skip the pass - this
        // file's own rule, from the StereoDualEyeDump fall-through: "leaving a
        // draw recorded with no active render pass is worse". We fall through
        // exactly as before and count it, so a missing mirror is visible as a
        // refusal rather than looking like a fix that did not help.
        if (g_rpfix_on.load(std::memory_order_relaxed) && info &&
            info->clearValueCount && info->framebuffer != VK_NULL_HANDLE) {
            VkFramebuffer mirror_fb = VK_NULL_HANDLE;
            {
                std::scoped_lock lk(mirror_mutex);
                if (const MirrorTarget* mt = ensure_mirror_framebuffer(info->framebuffer))
                    if (mt->ok) mirror_fb = mt->framebuffer;
            }
            if (mirror_fb != VK_NULL_HANDLE) {
                const uint32_t n = g_rpfix_redirected.fetch_add(1, std::memory_order_relaxed);
                if (n < 6) {
                    log::warn(std::format(
                        "RPFIX #{}: doubled-call clearing render pass redirected {} -> {} "
                        "(renderPass={} unchanged, {}x{}). DOOM's own framebuffer is no "
                        "longer cleared by the doubled call.",
                        n, reinterpret_cast<void*>(info->framebuffer),
                        reinterpret_cast<void*>(mirror_fb),
                        reinterpret_cast<void*>(info->renderPass),
                        info->renderArea.extent.width, info->renderArea.extent.height));
                }
                VkRenderPassBeginInfo redirected = *info;
                redirected.framebuffer = mirror_fb;
                real_cmd_begin_render_pass(command_buffer, &redirected, contents);
                return;
            }
            kharvox::native::fail("render pass mirror unavailable");
            g_rpfix_nomirror.fetch_add(1, std::memory_order_relaxed);
        }
        if (info && info->clearValueCount) {
            const uint32_t n = g_rpc_withclears.fetch_add(1, std::memory_order_relaxed);
            if (n < 10) {
                log::warn(std::format(
                    "RPCENSUS FALL-THROUGH #{}: the doubled call began a render pass on "
                    "DOOM'S OWN framebuffer {} with clearValueCount={} (renderPass={}, "
                    "{}x{}). If any attachment's loadOp is CLEAR, DOOM's real image is "
                    "being wiped by the doubled call.",
                    n, reinterpret_cast<void*>(info->framebuffer),
                    info->clearValueCount,
                    reinterpret_cast<void*>(info->renderPass),
                    info->renderArea.extent.width, info->renderArea.extent.height));
            }
        }
    }
    real_cmd_begin_render_pass(command_buffer, info, contents);
}

// PassDrawCountProbe - see current_render_opaque_pass_slot's declaration
// above. DOOM only ever uses the classic vkCmdBeginRenderPass/EndRenderPass
// pair (confirmed - the RenderPass2/2KHR/Rendering/RenderingKHR sibling
// hooks below have never once logged anything all session), so this is the
// only END hook that actually matters; kept minimal.
void VKAPI_PTR hook_cmd_end_render_pass(VkCommandBuffer command_buffer) {
    kharvox::native::noteCommand(command_buffer);
    FbHook fb_(FB_H_CMD_END_RENDER_PASS);
    // FROXEL CONTENT COPY, OUTSIDE THE RENDER PASS INSTANCE. The bind site
    // latched which volumes each side sampled; this is the first legal point to
    // read them. Same placement depth_strip_copy has always used, and putting
    // it inside the pass is what made the first attempt return zeros.
    if (froxel_probe_armed.load(std::memory_order_relaxed)) {
        const uint32_t side = inside_frame_root_double ? 1u : 0u;
        for (uint32_t v = 0; v < kFroxelVolumes; ++v) {
            if (froxel_pending_image[side][v] == VK_NULL_HANDLE) continue;
            if (froxel_slice_captured[side][v]) continue;
            froxel_slice_copy(command_buffer, froxel_pending_image[side][v],
                              froxel_pending_extent[side][v],
                              froxel_pending_format[side][v], side, v);
            froxel_pending_image[side][v] = VK_NULL_HANDLE;
        }
    }
    if (inside_render_opaque) current_render_opaque_pass_slot = -1;
    real_cmd_end_render_pass(command_buffer);
    // ChainWalk (STEP 6.6): AFTER the real vkCmdEndRenderPass, so the copy is
    // recorded outside the render pass, where an image layout transition is
    // legal. Self-gating on chainwalk_capture_attempt == UINT32_MAX, which is
    // the state in every configuration except the two A/B attempts of a
    // scripted run.
    chainwalk_record_snapshot(command_buffer);
    real_timeline_record_snapshot(command_buffer);
    real_timeline_current_color = VK_NULL_HANDLE;
    // DEPTH CONTENT at the pass-25 boundary, both calls. Here rather than at
    // pass begin because a transfer is legal outside the render pass and the
    // depth has been fully written by this point - which is the state the
    // effects shaders sampled it in.
    if (indirect_args_probe_armed.load(std::memory_order_relaxed)) {
        const bool dbl = inside_frame_root_double;
        const uint32_t pass = dbl ? doubled_pass_seq : real_pass_seq;
        if ((dbl || inside_real_frame_root) && pass == kBeginEndPass) {
            VkImage doom_depth = VK_NULL_HANDLE;
            VkExtent2D ext{};
            {
                std::scoped_lock lock(framebuffer_depth_view_mutex);
                auto fb = dbl ? doubled_current_framebuffer : real_current_framebuffer;
                if (auto f = framebuffer_any_depth_views.find(fb);
                    f != framebuffer_any_depth_views.end()) {
                    if (auto i = image_view_to_image.find(f->second);
                        i != image_view_to_image.end()) {
                        doom_depth = i->second;
                        if (auto c = created_image_infos.find(doom_depth);
                            c != created_image_infos.end())
                            ext = {c->second.extent.width, c->second.extent.height};
                    }
                }
            }
            if (doom_depth != VK_NULL_HANDLE && ext.width) {
                if (!dbl) {
                    // DOOM's own depth. Its layout is READ from the tracked
                    // map, never assumed: the first version passed
                    // DEPTH_STENCIL_READ_ONLY_OPTIMAL as a guess, which made
                    // the barrier's oldLayout wrong, the copy undefined, and
                    // the whole run void while still printing a verdict.
                    // doom_image_layouts is maintained from the barriers DOOM
                    // itself issues, so it is observation rather than a guess.
                    VkImageLayout dl = VK_IMAGE_LAYOUT_UNDEFINED;
                    bool known = false;
                    {
                        std::scoped_lock lk(mirror_mutex);
                        if (auto it = doom_image_layouts.find(doom_depth);
                            it != doom_image_layouts.end()) { dl = it->second; known = true; }
                    }
                    if (known && dl != VK_IMAGE_LAYOUT_UNDEFINED) {
                        depth_strip_copy(command_buffer, doom_depth, dl, ext, 0);
                    } else {
                        log::error(std::format(
                            "DepthContent: DOOM's depth image {} has no tracked layout at pass "
                            "{} - skipping rather than copying from a guessed one, which is what "
                            "voided the last run.",
                            reinterpret_cast<void*>(doom_depth), pass));
                    }
                } else {
                    VkImage mirror = VK_NULL_HANDLE;
                    VkImageLayout ml = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
                    {
                        std::scoped_lock lk(mirror_mutex);
                        if (auto m = mirrored_images.find(doom_depth); m != mirrored_images.end()) {
                            mirror = m->second;
                            ml = mirror_layout_of(mirror);
                        }
                    }
                    if (mirror != VK_NULL_HANDLE)
                        depth_strip_copy(command_buffer, mirror, ml, ext, 1);
                    else
                        log::error("DepthContent: no mirror depth resolved at pass 25 - the "
                                   "comparison is void this run.");
                }
            }
        }
    }
    // IndirectArgs: same placement and the same reason - vkCmdCopyBuffer is
    // illegal inside a render pass, so the args regions recorded by the draw
    // hooks are copied out here, after the real vkCmdEndRenderPass.
    indirect_args_flush_copies(command_buffer);
    chainwalk_current_color = VK_NULL_HANDLE;
}

void VKAPI_PTR hook_cmd_begin_render_pass2(VkCommandBuffer command_buffer,
    const VkRenderPassBeginInfo* info, const VkSubpassBeginInfo* subpass_info) {
    kharvox::native::noteCommand(command_buffer);
    kharvox::native::checkFinalCommand(command_buffer,"hook_cmd_begin_render_pass2");
    kharvox::native::fail("RenderPass2/dynamic rendering is unsupported in Native Stereo");
    FbHook fb_(FB_H_CMD_BEGIN_RENDER_PASS2);
    if (inside_render_opaque && info &&
        render_pass_begin_sample_count.load(std::memory_order_relaxed) < kMaxRenderPassBeginSamples) {
        const uint32_t sample = render_pass_begin_sample_count.fetch_add(1, std::memory_order_relaxed);
        if (sample < kMaxRenderPassBeginSamples) {
            log::info(std::format(
                "RenderOpaque vkCmdBeginRenderPass2 sample={} framebuffer={} renderPass={} "
                "area=({},{} {}x{}) clearValueCount={}",
                sample, reinterpret_cast<void*>(info->framebuffer),
                reinterpret_cast<void*>(info->renderPass), info->renderArea.offset.x,
                info->renderArea.offset.y, info->renderArea.extent.width,
                info->renderArea.extent.height, info->clearValueCount));
        }
    }
    real_cmd_begin_render_pass2(command_buffer, info, subpass_info);
}

void VKAPI_PTR hook_cmd_begin_render_pass2_khr(VkCommandBuffer command_buffer,
    const VkRenderPassBeginInfo* info, const VkSubpassBeginInfo* subpass_info) {
    kharvox::native::noteCommand(command_buffer);
    kharvox::native::checkFinalCommand(command_buffer,"hook_cmd_begin_render_pass2_khr");
    kharvox::native::fail("RenderPass2/dynamic rendering is unsupported in Native Stereo");
    FbHook fb_(FB_H_CMD_BEGIN_RENDER_PASS2_KHR);
    if (inside_render_opaque && info &&
        render_pass_begin_sample_count.load(std::memory_order_relaxed) < kMaxRenderPassBeginSamples) {
        const uint32_t sample = render_pass_begin_sample_count.fetch_add(1, std::memory_order_relaxed);
        if (sample < kMaxRenderPassBeginSamples) {
            log::info(std::format(
                "RenderOpaque vkCmdBeginRenderPass2KHR sample={} framebuffer={} renderPass={} "
                "area=({},{} {}x{}) clearValueCount={}",
                sample, reinterpret_cast<void*>(info->framebuffer),
                reinterpret_cast<void*>(info->renderPass), info->renderArea.offset.x,
                info->renderArea.offset.y, info->renderArea.extent.width,
                info->renderArea.extent.height, info->clearValueCount));
        }
    }
    real_cmd_begin_render_pass2_khr(command_buffer, info, subpass_info);
}

void log_rendering_info(const char* label, const VkRenderingInfo* info) {
    const uint32_t sample = render_pass_begin_sample_count.fetch_add(1, std::memory_order_relaxed);
    if (sample >= kMaxRenderPassBeginSamples) return;
    std::string text = std::format(
        "RenderOpaque {} sample={} area=({},{} {}x{}) layerCount={} colorAttachmentCount={}",
        label, sample, info->renderArea.offset.x, info->renderArea.offset.y,
        info->renderArea.extent.width, info->renderArea.extent.height,
        info->layerCount, info->colorAttachmentCount);
    for (uint32_t i = 0; i < info->colorAttachmentCount && info->pColorAttachments; ++i) {
        text += std::format(" color[{}].imageView={}", i,
            reinterpret_cast<void*>(info->pColorAttachments[i].imageView));
    }
    if (info->pDepthAttachment) {
        text += std::format(" depth.imageView={}",
            reinterpret_cast<void*>(info->pDepthAttachment->imageView));
    }
    log::info(text);
}

void VKAPI_PTR hook_cmd_begin_rendering(VkCommandBuffer command_buffer, const VkRenderingInfo* info) {
    kharvox::native::noteCommand(command_buffer);
    kharvox::native::checkFinalCommand(command_buffer,"hook_cmd_begin_rendering");
    kharvox::native::fail("RenderPass2/dynamic rendering is unsupported in Native Stereo");
    FbHook fb_(FB_H_CMD_BEGIN_RENDERING);
    if (inside_render_opaque && info &&
        render_pass_begin_sample_count.load(std::memory_order_relaxed) < kMaxRenderPassBeginSamples) {
        log_rendering_info("vkCmdBeginRendering", info);
    }
    real_cmd_begin_rendering(command_buffer, info);
}

void VKAPI_PTR hook_cmd_begin_rendering_khr(VkCommandBuffer command_buffer, const VkRenderingInfo* info) {
    kharvox::native::noteCommand(command_buffer);
    kharvox::native::checkFinalCommand(command_buffer,"hook_cmd_begin_rendering_khr");
    kharvox::native::fail("RenderPass2/dynamic rendering is unsupported in Native Stereo");
    FbHook fb_(FB_H_CMD_BEGIN_RENDERING_KHR);
    if (inside_render_opaque && info &&
        render_pass_begin_sample_count.load(std::memory_order_relaxed) < kMaxRenderPassBeginSamples) {
        log_rendering_info("vkCmdBeginRenderingKHR", info);
    }
    real_cmd_begin_rendering_khr(command_buffer, info);
}

// Read-only diagnostic for Phase 2.4's "create a compatible VkFramebuffer"
// next step (see docs/HANDOFF-NEXT-SESSION.md). Logs every render pass'
// attachment formats/samples/layouts and every framebuffer's renderPass/
// extent/attachment views as DOOM creates them at init. Never modifies the
// create info or return value - purely observes. Correlate against the two
// known RenderOpaque VkFramebuffer/VkRenderPass handles already logged by
// hook_cmd_begin_render_pass above (same doomvr.log, same handle values).
void log_render_pass_attachments(const char* label, uint32_t sample,
    VkRenderPass render_pass, uint32_t attachment_count, uint32_t subpass_count,
    const VkAttachmentDescription* legacy_attachments,
    const VkAttachmentDescription2* v2_attachments) {
    std::string text = std::format(
        "{} sample={} renderPass={} attachmentCount={} subpassCount={}",
        label, sample, reinterpret_cast<void*>(render_pass), attachment_count, subpass_count);
    for (uint32_t i = 0; i < attachment_count; ++i) {
        VkFormat format; VkSampleCountFlagBits samples; VkAttachmentLoadOp load_op;
        VkAttachmentStoreOp store_op; VkAttachmentLoadOp stencil_load_op;
        VkAttachmentStoreOp stencil_store_op; VkImageLayout initial_layout, final_layout;
        if (legacy_attachments) {
            const auto& a = legacy_attachments[i];
            format = a.format; samples = a.samples; load_op = a.loadOp; store_op = a.storeOp;
            stencil_load_op = a.stencilLoadOp; stencil_store_op = a.stencilStoreOp;
            initial_layout = a.initialLayout; final_layout = a.finalLayout;
        } else if (v2_attachments) {
            const auto& a = v2_attachments[i];
            format = a.format; samples = a.samples; load_op = a.loadOp; store_op = a.storeOp;
            stencil_load_op = a.stencilLoadOp; stencil_store_op = a.stencilStoreOp;
            initial_layout = a.initialLayout; final_layout = a.finalLayout;
        } else {
            break;
        }
        text += std::format(
            " attach[{}](format={} samples={} loadOp={} storeOp={} stencilLoadOp={} "
            "stencilStoreOp={} initialLayout={} finalLayout={})",
            i, static_cast<int>(format), static_cast<int>(samples), static_cast<int>(load_op),
            static_cast<int>(store_op), static_cast<int>(stencil_load_op),
            static_cast<int>(stencil_store_op), static_cast<int>(initial_layout),
            static_cast<int>(final_layout));
    }
    log::info(text);
}

VkResult VKAPI_PTR hook_create_render_pass(VkDevice device, const VkRenderPassCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkRenderPass* render_pass) {
    FbHook fb_(FB_H_CREATE_RENDER_PASS);
    const VkResult result = real_create_render_pass(device, info, allocator, render_pass);
    if (result == VK_SUCCESS && info && render_pass) {
        // Store what layout this pass leaves each attachment in. Parsed for
        // logging since forever, never stored - so nothing could know a
        // mirror's layout after a pass ran over it.
        if (info->pAttachments) {
            std::vector<VkImageLayout> finals(info->attachmentCount);
            for (uint32_t i = 0; i < info->attachmentCount; ++i)
                finals[i] = info->pAttachments[i].finalLayout;
            std::scoped_lock lock(render_pass_layouts_mutex);
            render_pass_final_layouts[*render_pass] = std::move(finals);
            // INITIAL layouts too. The real call's colour target has to be
            // copied at pass BEGIN for the begin/end delta, and copying from a
            // guessed layout is the class of bug that hung the GPU three times
            // - so the layout the render pass itself declares on entry is
            // recorded rather than assumed.
            std::vector<VkImageLayout> initials(info->attachmentCount);
            for (uint32_t i = 0; i < info->attachmentCount; ++i)
                initials[i] = info->pAttachments[i].initialLayout;
            render_pass_initial_layouts[*render_pass] = std::move(initials);
        }
        const uint32_t sample = render_pass_create_sample_count.fetch_add(1, std::memory_order_relaxed);
        if (sample < kMaxRenderPassCreateSamples) {
            log_render_pass_attachments("vkCreateRenderPass", sample, *render_pass,
                info->attachmentCount, info->subpassCount, info->pAttachments, nullptr);
        }
    }
    return result;
}

VkResult VKAPI_PTR hook_create_render_pass2(VkDevice device, const VkRenderPassCreateInfo2* info,
    const VkAllocationCallbacks* allocator, VkRenderPass* render_pass) {
    FbHook fb_(FB_H_CREATE_RENDER_PASS2);
    const VkResult result = real_create_render_pass2(device, info, allocator, render_pass);
    if (result == VK_SUCCESS && info && render_pass) {
        const uint32_t sample = render_pass_create_sample_count.fetch_add(1, std::memory_order_relaxed);
        if (sample < kMaxRenderPassCreateSamples) {
            log_render_pass_attachments("vkCreateRenderPass2", sample, *render_pass,
                info->attachmentCount, info->subpassCount, nullptr, info->pAttachments);
        }
    }
    return result;
}

VkResult VKAPI_PTR hook_create_render_pass2_khr(VkDevice device, const VkRenderPassCreateInfo2* info,
    const VkAllocationCallbacks* allocator, VkRenderPass* render_pass) {
    FbHook fb_(FB_H_CREATE_RENDER_PASS2_KHR);
    const VkResult result = real_create_render_pass2_khr(device, info, allocator, render_pass);
    if (result == VK_SUCCESS && info && render_pass) {
        const uint32_t sample = render_pass_create_sample_count.fetch_add(1, std::memory_order_relaxed);
        if (sample < kMaxRenderPassCreateSamples) {
            log_render_pass_attachments("vkCreateRenderPass2KHR", sample, *render_pass,
                info->attachmentCount, info->subpassCount, nullptr, info->pAttachments);
        }
    }
    return result;
}

// RESOURCE LIFETIME (2026-08-17, review risk 2). Until these hooks existed,
// framebuffer_infos / image_view_infos / mirror_targets / mirrored_views
// were never invalidated and mirror GPU memory was never freed.
//
// Why that is a correctness bug and not just a leak: Vulkan is free to hand
// back a previously-destroyed handle VALUE for a new object. After a level
// transition or a resolution change, a lookup keyed on a recycled
// VkFramebuffer would therefore SUCCEED and return the mirror of a
// completely different image, at the wrong extent - no MISS, no VIEW MISS,
// no ABORT, nothing in the log. Silent wrong-target rendering, and the
// trigger is simply "play on past this room". Correctness here comes from
// ERASING the map entries; freeing the GPU memory is the secondary win, so
// the erase is unconditional and never depends on the free succeeding.
void VKAPI_PTR hook_destroy_framebuffer(VkDevice device, VkFramebuffer framebuffer,
    const VkAllocationCallbacks* allocator) {
    kharvox::native::beforeDestroy("vkDestroyFramebuffer",reinterpret_cast<uintptr_t>(framebuffer));
    FbHook fb_(FB_H_DESTROY_FRAMEBUFFER);
    if (framebuffer != VK_NULL_HANDLE) {
        std::vector<VkImageView> source_attachments;
        {
            std::scoped_lock lock(framebuffer_depth_view_mutex);
            if (auto it = framebuffer_infos.find(framebuffer); it != framebuffer_infos.end()) {
                source_attachments = it->second.attachments;
                framebuffer_infos.erase(it);
            }
            framebuffer_depth_views.erase(framebuffer);
            framebuffer_color0_views.erase(framebuffer);
        }
        VkFramebuffer mirror_fb = VK_NULL_HANDLE;
        {
            std::scoped_lock lock(mirror_mutex);
            if (auto it = mirror_targets.find(framebuffer); it != mirror_targets.end()) {
                // A MirrorTarget owns ONLY its own VkFramebuffer. Its images
                // and views are owned by mirrored_views and may still be
                // referenced by other mirror framebuffers, so they are
                // released by refcount in hook_destroy_image_view, not here.
                mirror_fb = it->second.framebuffer;
                mirror_targets.erase(it);
            }
            for (VkImageView v : source_attachments) {
                if (auto r = mirrored_view_refs.find(v); r != mirrored_view_refs.end())
                    r->second.remove(mirror_fb != VK_NULL_HANDLE);
            }
        }
        if (mirror_fb != VK_NULL_HANDLE) {
            kharvox::native::waitForMirrorRetirement(device);
            // real_ rather than a fresh vkGetDeviceProcAddr: this symbol is
            // hooked, and resolving it could hand back our own trampoline and
            // re-enter this function with our mirror handle. Harmless (the
            // mirror is in no map) but pointless indirection.
            real_destroy_framebuffer(device, mirror_fb, nullptr);
            const uint32_t n = mirror_destroy_count.fetch_add(1, std::memory_order_relaxed);
            if (n < 5) {
                log::warn(std::format(
                    "MirrorFramebuffer DESTROY #{}: source framebuffer {} destroyed by DOOM; "
                    "mirror framebuffer {} released and every map entry keyed on the source "
                    "handle erased, so a recycled handle cannot alias this mirror.",
                    n, reinterpret_cast<void*>(framebuffer), reinterpret_cast<void*>(mirror_fb)));
            }
        }
    }
    real_destroy_framebuffer(device, framebuffer, allocator);
}
#endif  // DOOMVR_RESEARCH_TOOLS

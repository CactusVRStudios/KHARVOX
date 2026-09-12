// 06_renderpass.inc - render-pass hooks, RenderDoc glue, the CPU camera scan
// 3523 of these lines sit inside #ifdef DOOMVR_RESEARCH_TOOLS,
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

void VKAPI_PTR hook_destroy_image_view(VkDevice device, VkImageView view,
    const VkAllocationCallbacks* allocator) {
    kharvox::native::beforeDestroy("vkDestroyImageView",reinterpret_cast<uintptr_t>(view));
    FbHook fb_(FB_H_DESTROY_IMAGE_VIEW);
    if (view != VK_NULL_HANDLE) {
        bool ownsMirror=false;
        std::vector<VkImageView> invalidViews{view};
        {std::scoped_lock lock(mirror_mutex);
            ownsMirror=mirrored_views.contains(view)||alias_mirror_views.contains(view);
            if(auto owner=mirrored_views.find(view);owner!=mirrored_views.end())
                for(const auto& [source,alias]:alias_mirror_views)
                    if(source!=view&&alias.mirror_image==owner->second.image)invalidViews.push_back(source);
        }
        if(ownsMirror)kharvox::native::waitForMirrorRetirement(device);
        std::vector<VkFramebuffer> retiredFramebuffers;
        {
            std::scoped_lock lock(mirror_mutex,framebuffer_depth_view_mutex);
            for(auto invalidView:invalidViews) {
                auto detached=kharvox::native::detachFramebuffersUsingView(
                    invalidView,framebuffer_infos,mirror_targets,mirrored_view_refs,
                    [](VkFramebuffer fb){framebuffer_depth_views.erase(fb);framebuffer_color0_views.erase(fb);},
                    invalidView==view);
                retiredFramebuffers.insert(retiredFramebuffers.end(),detached.begin(),detached.end());
            }
        }
        // Framebuffers must retire before the attachment views/images. This
        // also removes stale extent mappings when DOOM recycles a view handle.
        if(!retiredFramebuffers.empty()) {
            if(!ownsMirror)kharvox::native::waitForMirrorRetirement(device);
            for(auto fb:retiredFramebuffers)real_destroy_framebuffer(device,fb,nullptr);
            mirror_generation.fetch_add(1,std::memory_order_relaxed);
            log::info(std::format("Native view retirement detached {} dependent mirror framebuffer(s) before source view {}",retiredFramebuffers.size(),reinterpret_cast<void*>(view)));
        }
        // Also invalidate the old clones when an owning source framebuffer
        // outlives its view; the former leak branch skipped this invalidation.
        if(ownsMirror||!retiredFramebuffers.empty())invalidate_read_redirect_cache("source view retirement");
        {
            std::scoped_lock lock(framebuffer_depth_view_mutex);
            image_view_infos.erase(view);
            image_view_to_image.erase(view);
        }
        MirroredView doomed{};
        bool release = false, in_use = false;
        uint32_t refs = 0;
        std::vector<VkImageView> doomed_alias_views;
        {
            std::scoped_lock lock(mirror_mutex);
            mirror_shared_source_views.erase(view);
            if (auto it = mirrored_views.find(view); it != mirrored_views.end()) {
                if (auto r = mirrored_view_refs.find(view); r != mirrored_view_refs.end())
                    refs = r->second.count;
                doomed = it->second;
                for (auto mi = mirrored_images.begin(); mi != mirrored_images.end(); ) {
                    if (mi->second == it->second.image) mi = mirrored_images.erase(mi);
                    else ++mi;
                }
                mirror_image_layouts.erase(it->second.image);
                native_image_layouts.erase(it->second.image);
                in_use = refs > 0;
                release = !in_use;
                // Erased either way. Correctness does not depend on the free:
                // leaving the entry would let a recycled view handle alias
                // this mirror, which is the whole bug being fixed.
                mirrored_views.erase(it);
                mirrored_view_refs.erase(view);
            }
            // STEP 6.7a: alias views are built OVER a mirror image, so a
            // mirror image going away dangles every alias view over it.
            // Collected here under mirror_mutex and destroyed below with the
            // rest, so a level transition cannot leave a clone pointing at a
            // freed view.
            for (auto ai = alias_mirror_views.begin(); ai != alias_mirror_views.end(); ) {
                if (doomed.image != VK_NULL_HANDLE && ai->second.mirror_image == doomed.image) {
                    doomed_alias_views.push_back(ai->second.view);
                    ai = alias_mirror_views.erase(ai);
                } else {
                    ++ai;
                }
            }
            // The alias view built over THIS source view (if this view was
            // itself an aliased sampling view) also dies with it.
            if (auto ai = alias_mirror_views.find(view); ai != alias_mirror_views.end()) {
                doomed_alias_views.push_back(ai->second.view);
                alias_mirror_views.erase(ai);
            }
        }
        if (!doomed_alias_views.empty()) {
            // Outside mirror_mutex: real_destroy_image_view is the un-hooked
            // pointer, but invalidate_read_redirect_cache takes
            // read_redirect_mutex and there is no reason to nest it.
            for (VkImageView av : doomed_alias_views) real_destroy_image_view(device, av, nullptr);
            invalidate_read_redirect_cache("an alias mirror view was destroyed");
        }
        if (in_use) {
            // A source view can precede its unused framebuffer in teardown.
            // The dependency cleanup above handles that order. A remaining
            // reference indicates incomplete metadata; deliberately LEAK the
            // image rather than destroy something still bound (Rule 3 - a
            // visible leak beats a silent use-after-free).
            log::error(std::format(
                "MirrorView DESTROY: source view {} destroyed while {} mirror framebuffer(s) "
                "still reference its mirror. Map entry erased (no aliasing) but the mirror "
                "image is LEAKED rather than destroyed while possibly bound. This means "
                "framebuffer/view destroy ordering is not what we assumed - investigate.",
                reinterpret_cast<void*>(view), refs));
        }
        if (release) {
            // real_destroy_image_view for the same reason as the framebuffer
            // case above; vkDestroyImage is not hooked, so it resolves normally.
            auto destroy_image = load_device_proc<PFN_vkDestroyImage>(device, "vkDestroyImage");
            auto free_mem = load_device_proc<PFN_vkFreeMemory>(device, "vkFreeMemory");
            if(!destroy_image||!free_mem)kharvox::native::fail("mirror retirement dispatch unavailable");
            {std::scoped_lock lock(mirror_mutex);
            if (doomed.view) real_destroy_image_view(device, doomed.view, nullptr);
            if (doomed.image) destroy_image(device, doomed.image, nullptr);
            if (doomed.memory) {
                free_mem(device, doomed.memory, nullptr);
                mirror_bytes_allocated.fetch_sub(doomed.size, std::memory_order_relaxed);
                storage_mirror_budget.release(reinterpret_cast<uintptr_t>(doomed.image));
            }
            }
            // Any ReadRedirect clone may point at the view we just
            // destroyed. Cheaper and far safer to drop them all than to
            // track which.
            invalidate_read_redirect_cache("a mirror view was destroyed");
            const uint32_t n = mirror_view_destroy_count.fetch_add(1, std::memory_order_relaxed);
            if (n < 5) {
                log::warn(std::format(
                    "MirrorView DESTROY #{}: source view {} destroyed; mirror image/view "
                    "released, {} MB still held by mirrors overall.",
                    n, reinterpret_cast<void*>(view),
                    mirror_bytes_allocated.load(std::memory_order_relaxed) / (1024 * 1024)));
            }
        }
    }
    real_destroy_image_view(device, view, allocator);
}

VkResult VKAPI_PTR hook_create_framebuffer(VkDevice device, const VkFramebufferCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkFramebuffer* framebuffer) {
    FbHook fb_(FB_H_CREATE_FRAMEBUFFER);
    const VkResult result = real_create_framebuffer(device, info, allocator, framebuffer);
    if (result == VK_SUCCESS && info && framebuffer) {
        if (info->attachmentCount == 4 && info->pAttachments) {
            std::scoped_lock lock(framebuffer_depth_view_mutex);
            framebuffer_depth_views[*framebuffer] = info->pAttachments[3];
        }
        // GENERIC depth attachment, ANY attachment count (2026-08-19).
        //
        // framebuffer_depth_views above only fires for attachmentCount==4 -
        // the G-buffer framebuffers. The transparency pass at 25 has TWO
        // attachments (one colour, one depth), so it was never recorded, and
        // PassStateParity's first run reported view=0x0 on both sides and
        // printed a "no private depth" warning that was purely a failed
        // lookup. A separate map rather than widening the one above: other
        // code keys depth-prepass behaviour off that map, and quietly making
        // it match more framebuffers would change behaviour, not just
        // measurement.
        //
        // Identified by FORMAT, not by position: attachment order is not
        // guaranteed and index 3 is a G-buffer-specific convention.
        if (info->attachmentCount >= 1 && info->pAttachments) {
            std::scoped_lock lock(framebuffer_depth_view_mutex);
            for (uint32_t i = 0; i < info->attachmentCount; ++i) {
                auto iv = image_view_to_image.find(info->pAttachments[i]);
                if (iv == image_view_to_image.end()) continue;
                auto ci = created_image_infos.find(iv->second);
                if (ci == created_image_infos.end()) continue;
                const int f = static_cast<int>(ci->second.format);
                // VK_FORMAT_D16_UNORM(124) .. VK_FORMAT_D32_SFLOAT_S8_UINT(130)
                if (f >= 124 && f <= 130) {
                    framebuffer_any_depth_views[*framebuffer] = info->pAttachments[i];
                    break;
                }
            }
        }
        // CompositeTargetProbe: remember colour attachment 0 for EVERY
        // framebuffer, not just the 4-attachment G-buffer ones. Needed to
        // identify what DOOM's final composite (RenderPostProcess) actually
        // writes into, which is the image the full-frame architecture has to
        // copy between the two frame-root runs.
        if (info->attachmentCount >= 1 && info->pAttachments) {
            std::scoped_lock lock(framebuffer_depth_view_mutex);
            framebuffer_color0_views[*framebuffer] = info->pAttachments[0];
        }
        // GENERIC MIRRORING producer (2026-08-16). framebuffer_infos was
        // declared for ensure_mirror_framebuffer and NEVER WRITTEN - the map
        // had no producer at all, so every lookup missed and mirroring could
        // not have created a single mirror in any run, whatever the flags
        // said. That, not injection timing, is why mirroring has never
        // executed. Imageless framebuffers carry no attachment list and
        // cannot be mirrored, so they are deliberately left unrecorded and
        // will report an honest MISS.
        if (info->attachmentCount >= 1 && info->pAttachments) {
            FramebufferInfo fi;
            fi.attachments.assign(info->pAttachments,
                                  info->pAttachments + info->attachmentCount);
            fi.render_pass = info->renderPass;
            fi.width = info->width;
            fi.height = info->height;
            fi.layers = info->layers;
            std::scoped_lock lock(framebuffer_depth_view_mutex);
            framebuffer_infos[*framebuffer] = std::move(fi);
        }
        const uint32_t sample = framebuffer_create_sample_count.fetch_add(1, std::memory_order_relaxed);
        if (sample < kMaxFramebufferCreateSamples) {
            std::string text = std::format(
                "vkCreateFramebuffer sample={} framebuffer={} renderPass={} attachmentCount={} "
                "extent={}x{} layers={}",
                sample, reinterpret_cast<void*>(*framebuffer),
                reinterpret_cast<void*>(info->renderPass), info->attachmentCount,
                info->width, info->height, info->layers);
            if (info->pAttachments) {
                for (uint32_t i = 0; i < info->attachmentCount; ++i) {
                    text += std::format(" view[{}]={}", i,
                        reinterpret_cast<void*>(info->pAttachments[i]));
                }
            } else {
                text += " imageless=true";
            }
            log::info(text);
        }
    }
    return result;
}

// DepthDumpControlTest - see CreateImageViewFn's declaration above. Thin
// passthrough, read-only, no behavior change - records every view's
// underlying image so a later diagnostic dump can resolve a real depth
// VIEW (framebuffer_depth_views) back to the VkImage vkCmdCopyImageToBuffer
// actually needs.
// 2026-08-18, review P1 - see CreatedImageInfo. Records only; DOOM's call is
// untouched.
// RENDERDOC IN-APPLICATION API (2026-08-22). Two jobs, both of which the
// capture campaign was otherwise going to do by hand and by eye.
//
// 1. REBIND THE CAPTURE KEY FROM INSIDE THE PROCESS. RenderDoc's UI on this
//    setup offers no way to change it, and its defaults are F12 and PrtScrn.
//    F12 is claimed twice over - DOOM's own screenshot key and Steam's - so
//    every capture also wrote two screenshot files, with their readbacks
//    landing inside the very frame being captured. SetCaptureKeys needs no UI.
//
// 2. SUPPLY THE ATTACHMENT CONTROL. The plan's control for "did the layer
//    actually attach" was "the overlay text is visible on the monitor", which
//    is a memory of a screen rather than a record. RENDERDOC_GetAPI resolving
//    is a positive, in-process statement that the layer is loaded, and it
//    lands in doomvr.log beside everything else. Absent means nothing that run
//    measured is the RenderDoc arm, and the session is void.
//
// Read-only with respect to rendering: no Vulkan object is created, bound or
// submitted here. On a normal run renderdoc.dll is not in the process, every
// entry point below returns early, and the only trace is one arm-time line
// saying so.
constexpr const char* kRdcCaptureDir = "C:\\dev\\rdccap";
constexpr const char* kRdcCapturePathTemplate = "C:\\dev\\rdccap\\doomvr";

std::atomic<RENDERDOC_API_1_6_0*> g_rdc_api{nullptr};
std::atomic_uint32_t g_rdc_resolve_attempts{0};
std::atomic_bool g_rdc_configured{false};
std::atomic_uint32_t g_rdc_captures_reported{0};

// DELIBERATELY NOT A `static const` ONCE-ONLY RESOLVE, unlike the switch-file
// helpers above. renderdoc.dll is injected before Vulkan initialises so it
// should always be there by the time any of this runs - but a once-only
// lambda that happened to run one call too early would latch "absent" for the
// whole process, and an attached session would report itself unattached. That
// failure mode is indistinguishable from the real thing and would void a
// capture session that was actually fine. Retrying costs a GetModuleHandleA.
RENDERDOC_API_1_6_0* rdc_api() {
    if (auto* have = g_rdc_api.load(std::memory_order_relaxed)) return have;
    g_rdc_resolve_attempts.fetch_add(1, std::memory_order_relaxed);
    HMODULE mod = GetModuleHandleA("renderdoc.dll");
    if (!mod) return nullptr;
    auto get_api = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(mod, "RENDERDOC_GetAPI"));
    if (!get_api) return nullptr;
    RENDERDOC_API_1_6_0* api = nullptr;
    // 1.6.0, not 1.7.0: nothing below needs the 1.7 additions, and asking for
    // the lowest version that covers the calls made keeps this working against
    // an older RenderDoc than the one installed today.
    if (get_api(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void**>(&api)) != 1 || !api) {
        return nullptr;
    }
    g_rdc_api.store(api, std::memory_order_relaxed);
    return api;
}

// Idempotent. Called at arm time and retried from the periodic block, so a
// layer that somehow appears late is still picked up and still says so.
void rdc_configure_if_present(const char* why) {
    if (g_rdc_configured.load(std::memory_order_relaxed)) return;
    RENDERDOC_API_1_6_0* api = rdc_api();
    if (!api) return;
    int major = 0, minor = 0, patch = 0;
    api->GetAPIVersion(&major, &minor, &patch);
    // F11 ALONE. Every other function key is claimed by this plugin on a flat
    // run: bare F1 (compute suppression), F2 (the capture battery), F9, and
    // bare else-branches on F3-F8 and F10. F11 is claimed only as SHIFT+F11
    // (the bind-time volume redirect) here. update_look_injection in
    // openxr_context.cpp used to take BARE F11 too, ignoring modifiers, so on a
    // headset run every capture press silently flipped head-look; that toggle
    // was demoted to C:\dev\doomvr-lookinject.txt on 2026-08-24 and F11 is now
    // shared with nothing outside this file. Passing
    // exactly one key also CLEARS F12 and PrtScrn, which is the point.
    static RENDERDOC_InputButton keys[1] = {eRENDERDOC_Key_F11};
    api->SetCaptureKeys(keys, 1);
    CreateDirectoryA(kRdcCaptureDir, nullptr);
    api->SetCaptureFilePathTemplate(kRdcCapturePathTemplate);
    g_rdc_configured.store(true, std::memory_order_relaxed);
    log::warn(std::format(
        "RenderDocAPI ({}): PRESENT, in-app API v{}.{}.{}. CAPTURE KEY REBOUND TO F11 ALONE - "
        "F12 and PrtScrn are now cleared, so DOOM's own screenshot key and Steam's no longer "
        "fire alongside a capture. Captures are written to '{}_frameN.rdc'. capturesSoFar={} "
        "targetControlConnected={}. ⚠ Do NOT hold SHIFT with F11 - SHIFT+F11 is the bind-time "
        "volume redirect. This line is also THE ATTACHMENT CONTROL: it can only print when "
        "renderdoc.dll is loaded in this process.",
        why, major, minor, patch, kRdcCapturePathTemplate, api->GetNumCaptures(),
        api->IsTargetControlConnected() ? "yes" : "no"));
}

// Names each capture file in the log as it appears, so validating the session
// does not depend on hunting through RenderDoc's UI afterwards.
//
// It says a capture EXISTS. It does not say the frame is usable - that is
// still §2.4's four checks, done in the UI before detaching.
void rdc_report_new_captures() {
    RENDERDOC_API_1_6_0* api = g_rdc_api.load(std::memory_order_relaxed);
    if (!api) return;
    const uint32_t total = api->GetNumCaptures();
    uint32_t reported = g_rdc_captures_reported.load(std::memory_order_relaxed);
    while (reported < total) {
        uint32_t len = 0;
        uint64_t stamp = 0;
        char path[1024] = {};
        if (api->GetCapture(reported, nullptr, &len, nullptr) && len > 0 &&
            len < static_cast<uint32_t>(sizeof(path))) {
            api->GetCapture(reported, path, &len, &stamp);
        }
        log::warn(std::format(
            "RenderDocCapture #{} of {} WRITTEN: '{}' (unixTime={}). The instrument's own record "
            "that a capture file exists. It does NOT say the frame is usable - validate in the "
            "UI before detaching: it opens, two heavily-written full-res HDR targets, two "
            "~69-pass halves, and the presented image at the last event SHOWS THE HAZE.",
            reported, total, path[0] ? path : "<path unavailable>", stamp));
        ++reported;
    }
    g_rdc_captures_reported.store(total, std::memory_order_relaxed);
}

// RENDERDOC PARITY - TRANSFER USAGE ON EVERY RESOURCE (2026-08-20).
//
// THE ONE MEASURED DISCRIMINATOR. With the RenderDoc Vulkan layer merely
// LOADED - no capture taken - the doubled call renders its full additive
// layer; without it, it does not. Measured same build, same headset-free
// configuration, by the in-process dump path, so it is not a replay artefact:
//
//   11:22, no RenderDoc  eye1 mirror px>200 = 16 048..16 533 (flat, no fog/fire)
//   11:47, RenderDoc     eye1 mirror px>200 = 20 804..23 953 (fog and fire present)
//
// and the 11:47 dumps were written 8-22 s BEFORE the capture key was pressed,
// so it is the layer's continuous behaviour, not capturing.
//
// This replicates the most-cited of those continuous behaviours: RenderDoc
// adds TRANSFER_SRC|TRANSFER_DST to every image and buffer so it can read
// anything back at any moment. That is not cosmetic on NVIDIA - TRANSFER_SRC
// defeats DCC/compression and changes how the driver aliases and reuses
// render-target memory, which is exactly the class of state the doubled call
// reads after DOOM has finished with it.
//
// If the effects return with this on, the mechanism is resource
// compression/aliasing and the fix is a targeted version of this flag on the
// specific targets the doubled call samples. If they do not, this candidate
// is eliminated outright and the next one is the layer's added serialisation.
//
// RESULT: NEGATIVE, and cleanly so (2026-08-20, run 12:07). Forced ON, zero
// driver refusals, so every image and buffer in the process was widened. Eye
// 1's mirror stayed pinned at px>200 = 16 140..16 238 while DOOM's control
// reached 74 897 - identical to the un-widened runs. Resource usage flags,
// and with them the DCC/compression and allocation-size effects they carry,
// are ELIMINATED as the mechanism.
//
// Left in place, switched off, because the next candidate is tested the same
// way and this is the shape that test takes.
//
// Forced at load, not toggled - the owner cannot verify a keypress from
// inside a headset, and this must be identical on every frame of the run.
// RE-OPENED AS A SWITCH FILE (2026-08-22), for one cheap re-test.
//
// The negative above stands on its own terms but rests on two things this
// project has since learned to distrust:
//   * it ran on the 2026-08-20 baseline, BEFORE the corrupting render-surface
//     hook at DOOM+0x1A367E0 was removed - quarantined, therefore unknown;
//   * it was judged by px>200, which the 2026-08-29 matched pairs showed is a
//     poor proxy for this signature. Fog both ADDS and REMOVES bright pixels -
//     the same scene type read 1.62 in the effects-rich pair and ~1.0 in three
//     effects-poor ones. The owner's eye on the canyon haze is the instrument
//     that actually reads it.
//
// CANNOT BE A KEY: usage flags are fixed at vkCreateImage, so this must be
// decided before DOOM creates anything. A file is read once at load, needs no
// rebuild to flip, and follows the project's file-based switch convention -
// DOOM inherits Steam's environment, not the injector's, so env vars are void.
//
//   create  C:/dev/doomvr-rdcparity.txt   -> widening ON  (forward slashes:
//   fopen accepts them on Windows and they cannot be mangled into an invalid
//   escape, which is exactly what happened to the backslash version)
//   delete it                             -> widening OFF (default)
//
// The ArmState line reports which, so a run can never be misattributed.
inline bool renderdoc_parity_usage_enabled() { return true; }

VkResult VKAPI_PTR hook_create_image(VkDevice device, const VkImageCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkImage* image) {
    FbHook fb_(FB_H_CREATE_IMAGE);
    VkResult result;
    if (renderdoc_parity_usage_enabled() && info) {
        VkImageCreateInfo patched = *info;
        patched.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        result = real_create_image(device, &patched, allocator, image);
        // Rule 3: a silent refusal here would look exactly like the flags
        // having had no effect. If the driver rejects the widened usage for
        // this format, fall back to DOOM's own info and say so.
        if (result != VK_SUCCESS) {
            static std::atomic_uint32_t refused{0};
            if (refused.fetch_add(1, std::memory_order_relaxed) < 5) {
                log::error(std::format(
                    "RenderDocParity: vkCreateImage REFUSED the widened usage (fmt={} usage=0x{:x} "
                    "-> 0x{:x}, result={}). Falling back to DOOM's own usage for this image, so "
                    "it is NOT covered by the test.",
                    static_cast<int>(info->format), static_cast<unsigned>(info->usage),
                    static_cast<unsigned>(patched.usage), static_cast<int>(result)));
            }
            kharvox::native::fail("required image transfer usage unsupported");
        }
    } else {
        result = real_create_image(device, info, allocator, image);
    }
    if (result == VK_SUCCESS && info && image) {
        std::scoped_lock lock(framebuffer_depth_view_mutex);
        created_image_infos[*image] = CreatedImageInfo{
            info->extent, info->format, info->usage, info->mipLevels, info->arrayLayers,
            info->imageType};
    }
    if(result==VK_SUCCESS&&info&&image){auto actual=*info;if(renderdoc_parity_usage_enabled())actual.usage|=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;kharvox::native::recordSourceImage(*image,actual);}
    return result;
}

VkResult VKAPI_PTR hook_create_image_view(VkDevice device, const VkImageViewCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkImageView* view) {
    FbHook fb_(FB_H_CREATE_IMAGE_VIEW);
    const VkResult result = real_create_image_view(device, info, allocator, view);
    if (result == VK_SUCCESS && info && view) {
        std::scoped_lock lock(framebuffer_depth_view_mutex);
        image_view_to_image[*view] = info->image;
        // Generic framebuffer mirroring needs each attachment's FORMAT and
        // aspect, not just its image. Recorded here because it is the only
        // place Vulkan hands them to us. See the framebuffer census: a doubled
        // run touches 7 distinct framebuffers with the job filter on, which is
        // small enough that mirroring all of them is a straightforward
        // extension of the machinery already proven for RenderOpaque.
        ImageViewInfo vinfo{};
        vinfo.image = info->image;
        vinfo.format = info->format;
        vinfo.aspect = info->subresourceRange.aspectMask;
        vinfo.layers = info->subresourceRange.layerCount;
        // STEP 6.7a needs the rest verbatim to rebuild this view over a
        // mirror image.
        vinfo.view_type = info->viewType;
        vinfo.components = info->components;
        vinfo.range = info->subresourceRange;
        vinfo.flags = info->flags;
        image_view_infos[*view] = vinfo;
    }
    return result;
}

// Phase 0 (STEREO-IMPLEMENTATION-PLAN.md v2): capture depth/stencil state
// for every pipeline DOOM creates, keyed by handle. Storage only here - no
// logging (pipeline creation is a startup burst of hundreds; logging all of
// them would be noise with nothing to correlate against yet). The
// meaningful log point is the first time a given pipeline is actually
// BOUND while inside_render_opaque - see hook_cmd_bind_pipeline below.
VkResult VKAPI_PTR hook_create_graphics_pipelines(VkDevice device, VkPipelineCache cache,
    uint32_t create_info_count, const VkGraphicsPipelineCreateInfo* create_infos,
    const VkAllocationCallbacks* allocator, VkPipeline* pipelines) {
    FbHook fb_(FB_H_CREATE_GRAPHICS_PIPELINES);
    const VkResult result = real_create_graphics_pipelines(device, cache, create_info_count,
        create_infos, allocator, pipelines);
    if ((result == VK_SUCCESS || result == VK_PIPELINE_COMPILE_REQUIRED) &&
        create_infos && pipelines) {
        std::unique_lock lock(swapchains_mutex);
        for (uint32_t i = 0; i < create_info_count; ++i) {
            if (pipelines[i] == VK_NULL_HANDLE) continue;
            if (pipeline_create_sample_count.fetch_add(1, std::memory_order_relaxed) >=
                kMaxPipelineCreateSamples) continue;
            PipelineDepthStencilInfo info;
            const auto* ds = create_infos[i].pDepthStencilState;
            if (ds) {
                info.has_depth_stencil_state = true;
                info.depth_test_enable = ds->depthTestEnable;
                info.depth_write_enable = ds->depthWriteEnable;
                info.depth_compare_op = ds->depthCompareOp;
                info.stencil_test_enable = ds->stencilTestEnable;
                info.front_compare_op = ds->front.compareOp;
                info.front_reference = ds->front.reference;
                info.back_compare_op = ds->back.compareOp;
                info.back_reference = ds->back.reference;
            }
            // ColorBlendProbe - see PipelineDepthStencilInfo's declaration
            // above for the full reasoning.
            const auto* cb = create_infos[i].pColorBlendState;
            if (cb && cb->pAttachments) {
                info.has_color_blend_state = true;
                for (uint32_t a = 0; a < cb->attachmentCount && a < info.blend_enable.size(); ++a) {
                    info.blend_enable[a] = cb->pAttachments[a].blendEnable;
                }
                if (cb->attachmentCount > 0) {
                    const auto& a0 = cb->pAttachments[0];
                    info.src_color_blend_factor = a0.srcColorBlendFactor;
                    info.dst_color_blend_factor = a0.dstColorBlendFactor;
                    info.color_blend_op = a0.colorBlendOp;
                    info.src_alpha_blend_factor = a0.srcAlphaBlendFactor;
                    info.dst_alpha_blend_factor = a0.dstAlphaBlendFactor;
                    info.alpha_blend_op = a0.alphaBlendOp;
                }
            }
            pipeline_depth_stencil_info[pipelines[i]] = info;
        }
    }
    return result;
}

struct CpuCameraScanJob {
    std::array<float, 16> matrix{};
};

uint32_t scan_region_orientation(unsigned char* region, size_t region_size,
    const float* matrix, const void* excluded_object, size_t excluded_size,
    void** matches, uint32_t capacity) {
    uint32_t count = 0;
    __try {
        for (size_t offset = 0; offset + 64 <= region_size && count < capacity;
             offset += sizeof(float)) {
            auto* candidate = region + offset;
            auto* excluded = static_cast<const unsigned char*>(excluded_object);
            if (candidate >= excluded && candidate < excluded + excluded_size) continue;
            const float* value = reinterpret_cast<const float*>(candidate);
            constexpr uint32_t orientation_indices[] = {
                0,1,2, 4,5,6, 8,9,10, 12,13,14};
            bool orientation_match = true;
            for (uint32_t index : orientation_indices) {
                if (!std::isfinite(value[index]) ||
                    std::fabs(value[index] - matrix[index]) > 0.001f) {
                    orientation_match = false;
                    break;
                }
            }
            if (orientation_match) matches[count++] = candidate;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return count;
    }
    return count;
}

bool safe_copy_64(const void* source, void* destination) {
    __try {
        std::memcpy(destination, source, 64);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool safe_read_pointer(const void* source, uintptr_t& value) {
    __try {
        value = *static_cast<const uintptr_t*>(source);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool apply_temporary_world_yaw(uintptr_t view, std::array<float, 16>& original,
    float*& matrix) {
    matrix = nullptr;
    // Permanently disabled: DOOM consumes the current/previous camera pair from
    // multiple render jobs. Even a scoped write/restore at the owning view
    // produced visible frame-to-frame jitter, proving the access is asynchronous.
    return false;
#if 0
    if (!upstream_yaw_test_active.load(std::memory_order_relaxed) ||
        view == 0 || view != world_view_address.load(std::memory_order_relaxed))
        return false;
    const size_t offset = world_view_matrix_offset.load(std::memory_order_relaxed);
    if (offset == SIZE_MAX) return false;
    auto* candidate = reinterpret_cast<float*>(view + offset);
    if (!safe_copy_64(candidate, original.data())) return false;
    constexpr float angle = 3.0f * 3.14159265358979323846f / 180.0f;
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    std::array<float, 16> rotated = original;
    for (int column = 0; column < 4; ++column) {
        rotated[column] = c * original[column] + s * original[8 + column];
        rotated[8 + column] = -s * original[column] + c * original[8 + column];
    }
    __try { std::memcpy(candidate, rotated.data(), 64); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    matrix = candidate;
    return true;
#endif
}

size_t find_matrix_in_render_view(uintptr_t view, const float* matrix) {
    if (!view || !matrix) return SIZE_MAX;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(view), &mbi, sizeof(mbi))) return SIZE_MAX;
    const uintptr_t region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    const size_t available = region_end > view ? static_cast<size_t>(region_end - view) : 0;
    const size_t search_size = std::min<size_t>(available, 0x10000);
    constexpr uint32_t indices[] = {0,1,2,4,5,6,8,9,10,12,13,14};
    __try {
        for (size_t offset = 0; offset + 64 <= search_size; offset += sizeof(float)) {
            const float* candidate = reinterpret_cast<const float*>(view + offset);
            bool match = true;
            for (uint32_t index : indices) {
                if (!std::isfinite(candidate[index]) ||
                    std::fabs(candidate[index] - matrix[index]) > 0.001f) {
                    match = false;
                    break;
                }
            }
            if (match) return offset;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return SIZE_MAX;
}

void log_doom_camera_bind_stack() {
    void* frames[48]{};
    const USHORT count = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
    const uintptr_t doom_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"DOOMx64vk.exe"));
    if (!doom_base) return;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(doom_base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(doom_base + dos->e_lfanew);
    const auto* section = IMAGE_FIRST_SECTION(nt);
    uint32_t logged = 0;
    for (USHORT frame = 0; frame < count; ++frame) {
        const uintptr_t address = reinterpret_cast<uintptr_t>(frames[frame]);
        bool executable = false;
        for (uint16_t index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
            if (!(section[index].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            const uintptr_t begin = doom_base + section[index].VirtualAddress;
            const uintptr_t end = begin + std::max(section[index].Misc.VirtualSize,
                section[index].SizeOfRawData);
            if (address >= begin && address < end) { executable = true; break; }
        }
        if (!executable) continue;
        char text[192];
        std::snprintf(text, sizeof(text),
            "F10 immediate camera bind stack frame=%u address=%p moduleOffset=0x%llX",
            logged, frames[frame],
            static_cast<unsigned long long>(address - doom_base));
        log::info(text);
        ++logged;
    }
    char text[128];
    std::snprintf(text, sizeof(text),
        "F10 immediate camera bind stack complete: doomFrames=%u", logged);
    log::info(text);
}

uint32_t scan_region_pointer_range(unsigned char* region, size_t region_size,
    uintptr_t target_begin, uintptr_t target_end, void** reference_addresses,
    uintptr_t* reference_values, uint32_t capacity) {
    uint32_t count = 0;
    __try {
        for (size_t offset = 0; offset + sizeof(uintptr_t) <= region_size && count < capacity;
             offset += sizeof(uintptr_t)) {
            const uintptr_t value = *reinterpret_cast<const uintptr_t*>(region + offset);
            if (value >= target_begin && value <= target_end) {
                reference_addresses[count] = region + offset;
                reference_values[count] = value;
                ++count;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return count;
    }
    return count;
}

DWORD WINAPI cpu_camera_scan_worker(void* parameter) {
    auto* job = static_cast<CpuCameraScanJob*>(parameter);
    std::vector<std::pair<const unsigned char*, size_t>> excluded;
    {
        std::unique_lock lock(swapchains_mutex);
        for (const auto& [memory, mapping] : memory_mappings) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (mapping.host && VirtualQuery(mapping.host, &mbi, sizeof(mbi)))
                excluded.emplace_back(static_cast<const unsigned char*>(mbi.BaseAddress), mbi.RegionSize);
        }
    }
    char start_text[256];
    std::snprintf(start_text, sizeof(start_text),
        "F10 CPU camera scan started; excluding %zu Vulkan mapped regions", excluded.size());
    log::info(start_text);

    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    auto* address = static_cast<unsigned char*>(system_info.lpMinimumApplicationAddress);
    auto* maximum = static_cast<unsigned char*>(system_info.lpMaximumApplicationAddress);
    uint32_t matches = 0;
    uint64_t bytes_scanned = 0;
    std::array<void*, 8> candidates{};
    bool adjacent_pair_found = false;
    while (address < maximum && matches < candidates.size() && !adjacent_pair_found) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(address, &mbi, sizeof(mbi)) || !mbi.RegionSize) break;
        auto* region = static_cast<unsigned char*>(mbi.BaseAddress);
        const size_t region_size = mbi.RegionSize;
        const DWORD blocked = PAGE_GUARD | PAGE_NOACCESS;
        const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & blocked) &&
            (mbi.Type == MEM_PRIVATE || mbi.Type == MEM_IMAGE) &&
            region_size != 2 * 1024 * 1024; // known transient render rings
        bool is_excluded = false;
        for (const auto& [base, size] : excluded) {
            if (region < base + size && base < region + region_size) {
                is_excluded = true;
                break;
            }
        }
        if (readable && !is_excluded && region_size >= sizeof(job->matrix)) {
            bytes_scanned += region_size;
            void* found[32]{};
            const uint32_t found_count = scan_region_orientation(region, region_size,
                job->matrix.data(), job, sizeof(*job), found,
                static_cast<uint32_t>(std::size(found)));
            for (uint32_t i = 0; i < found_count && matches < candidates.size(); ++i) {
                char text[288];
                const uintptr_t render_view =
                    captured_render_view.load(std::memory_order_relaxed);
                const long long delta_from_view = render_view
                    ? static_cast<long long>(reinterpret_cast<uintptr_t>(found[i]) - render_view)
                    : 0;
                std::snprintf(text, sizeof(text),
                    "F10 CPU camera orientation match=%u address=%p regionBase=%p regionSize=%zu type=0x%lx protect=0x%lx renderView=%p deltaFromView=%lld",
                    matches,
                    found[i], region, region_size,
                    static_cast<unsigned long>(mbi.Type),
                    static_cast<unsigned long>(mbi.Protect),
                    reinterpret_cast<void*>(render_view), delta_from_view);
                log::info(text);
                candidates[matches++] = found[i];
                for (uint32_t a = 0; a < matches && !adjacent_pair_found; ++a) {
                    for (uint32_t b = a + 1; b < matches; ++b) {
                        const auto pa = reinterpret_cast<uintptr_t>(candidates[a]);
                        const auto pb = reinterpret_cast<uintptr_t>(candidates[b]);
                        if ((pa > pb ? pa - pb : pb - pa) == 64) {
                            void* first = pa < pb ? candidates[a] : candidates[b];
                            void* second = pa < pb ? candidates[b] : candidates[a];
                            candidates[0] = first;
                            candidates[1] = second;
                            matches = 2;
                            adjacent_pair_found = true;
                            log::info("F10 adjacent 64-byte CPU camera pair found");
                            break;
                        }
                    }
                }
            }
        }
        if (region_size > static_cast<size_t>(maximum - region)) break;
        address = region + region_size;
    }
    char done_text[192];
    std::snprintf(done_text, sizeof(done_text),
        "F10 CPU camera scan complete: matches=%u scanned=%llu MB",
        matches, static_cast<unsigned long long>(bytes_scanned / (1024 * 1024)));
    log::info(done_text);
    if (matches) {
        log::info("F10 CPU camera candidate monitor started; move game camera now");
        std::array<std::array<float, 16>, 8> previous{};
        std::array<uint32_t, 8> changes{};
        std::array<uint32_t, 8> camera_shape_samples{};
        std::array<bool, 8> readable{};
        for (uint32_t candidate = 0; candidate < matches; ++candidate)
            readable[candidate] = safe_copy_64(candidates[candidate], previous[candidate].data());
        for (uint32_t sample = 0; sample < 15; ++sample) {
            Sleep(200);
            for (uint32_t candidate = 0; candidate < matches; ++candidate) {
                if (!readable[candidate]) continue;
                std::array<float, 16> current{};
                if (!safe_copy_64(candidates[candidate], current.data())) {
                    readable[candidate] = false;
                    continue;
                }
                const float l0 = std::sqrt(current[0]*current[0] + current[1]*current[1] + current[2]*current[2]);
                const float l1 = std::sqrt(current[4]*current[4] + current[5]*current[5] + current[6]*current[6]);
                const float l2 = std::sqrt(current[8]*current[8] + current[9]*current[9] + current[10]*current[10]);
                const float l3 = std::sqrt(current[12]*current[12] + current[13]*current[13] + current[14]*current[14]);
                if (l0 > 0.85f && l0 < 1.05f && l1 > 1.65f && l1 < 1.90f &&
                    l2 > 0.90f && l2 < 1.10f && l3 > 0.90f && l3 < 1.10f)
                    ++camera_shape_samples[candidate];
                bool orientation_changed = false;
                constexpr uint32_t indices[] = {0,1,2,4,5,6,8,9,10,12,13,14};
                for (uint32_t index : indices)
                    orientation_changed |= std::fabs(current[index] - previous[candidate][index]) > 0.0001f;
                if (orientation_changed) {
                    if (changes[candidate] < 5) {
                        char text[320];
                        std::snprintf(text, sizeof(text),
                            "F10 CPU candidate=%u change=%u address=%p orientation="
                            "[%.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f]",
                            candidate, changes[candidate], candidates[candidate],
                            current[0], current[1], current[2], current[4], current[5], current[6],
                            current[8], current[9], current[10], current[12], current[13], current[14]);
                        log::info(text);
                    }
                    ++changes[candidate];
                }
                previous[candidate] = current;
            }
        }
        for (uint32_t candidate = 0; candidate < matches; ++candidate) {
            char monitor_text[192];
            std::snprintf(monitor_text, sizeof(monitor_text),
                "F10 CPU candidate=%u monitor complete: changes=%u cameraShape=%u/15 readable=%d address=%p",
                candidate, changes[candidate], camera_shape_samples[candidate],
                readable[candidate] ? 1 : 0,
                candidates[candidate]);
            log::info(monitor_text);
        }
        uint32_t best = 0;
        for (uint32_t candidate = 1; candidate < matches; ++candidate) {
            if (camera_shape_samples[candidate] > camera_shape_samples[best]) best = candidate;
        }
        if (camera_shape_samples[best] >= 12) {
            const uintptr_t matrix_address = reinterpret_cast<uintptr_t>(candidates[best]);
            const uintptr_t target_begin = matrix_address > 0x2000 ? matrix_address - 0x2000 : 0;
            const uintptr_t target_end = matrix_address + 0x80;
            char selected_text[224];
            std::snprintf(selected_text, sizeof(selected_text),
                "F10 selected stable CPU camera candidate=%u address=%p; pointer target range=%p-%p",
                best, candidates[best], reinterpret_cast<void*>(target_begin),
                reinterpret_cast<void*>(target_end));
            log::info(selected_text);

            uint32_t references = 0;
            uint32_t code_references = 0;
            std::unordered_set<uintptr_t> seen_code_addresses;
            const auto doom_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"DOOMx64vk.exe"));
            uintptr_t doom_end = doom_base;
            uintptr_t doom_code_begin = 0;
            uintptr_t doom_code_end = 0;
            if (doom_base) {
                const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(doom_base);
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(doom_base + dos->e_lfanew);
                doom_end += nt->OptionalHeader.SizeOfImage;
                const auto* section = IMAGE_FIRST_SECTION(nt);
                for (uint16_t index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
                    if (!(section[index].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
                    const uintptr_t begin = doom_base + section[index].VirtualAddress;
                    const uintptr_t end = begin + std::max(section[index].Misc.VirtualSize,
                        section[index].SizeOfRawData);
                    if (!doom_code_begin || begin < doom_code_begin) doom_code_begin = begin;
                    if (end > doom_code_end) doom_code_end = end;
                }
            }
            auto* pointer_address = static_cast<unsigned char*>(system_info.lpMinimumApplicationAddress);
            while (pointer_address < maximum && references < 32) {
                MEMORY_BASIC_INFORMATION pointer_mbi{};
                if (!VirtualQuery(pointer_address, &pointer_mbi, sizeof(pointer_mbi)) ||
                    !pointer_mbi.RegionSize) break;
                auto* pointer_region = static_cast<unsigned char*>(pointer_mbi.BaseAddress);
                const size_t pointer_region_size = pointer_mbi.RegionSize;
                const bool pointer_readable = pointer_mbi.State == MEM_COMMIT &&
                    !(pointer_mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                    (pointer_mbi.Type == MEM_PRIVATE || pointer_mbi.Type == MEM_IMAGE);
                if (pointer_readable) {
                    void* found_addresses[32]{};
                    uintptr_t found_values[32]{};
                    const uint32_t found_count = scan_region_pointer_range(
                        pointer_region, pointer_region_size, target_begin, target_end,
                        found_addresses, found_values, 32);
                    for (uint32_t i = 0; i < found_count && references < 32; ++i) {
                        // Ignore the matrix block referring to itself by chance.
                        if (reinterpret_cast<uintptr_t>(found_addresses[i]) >= target_begin &&
                            reinterpret_cast<uintptr_t>(found_addresses[i]) <= target_end) continue;
                        char reference_text[224];
                        std::snprintf(reference_text, sizeof(reference_text),
                            "F10 camera pointer reference=%u at=%p value=%p deltaFromMatrix=%lld",
                            references, found_addresses[i],
                            reinterpret_cast<void*>(found_values[i]),
                            static_cast<long long>(found_values[i] - matrix_address));
                        log::info(reference_text);
                        const long long delta = static_cast<long long>(found_values[i] - matrix_address);
                        if ((delta == 0 || delta == 64) && doom_base && code_references < 32) {
                            auto* stack_reference = static_cast<unsigned char*>(found_addresses[i]);
                            for (int displacement = -384; displacement <= 384 && code_references < 32;
                                 displacement += static_cast<int>(sizeof(uintptr_t))) {
                                uintptr_t possible_code = 0;
                                if (!safe_read_pointer(stack_reference + displacement, possible_code)) continue;
                                if (possible_code >= doom_code_begin && possible_code < doom_code_end &&
                                    seen_code_addresses.insert(possible_code).second) {
                                    char code_text[224];
                                    std::snprintf(code_text, sizeof(code_text),
                                        "F10 DOOM stack code candidate=%u value=%p moduleOffset=0x%llX nearCameraRef=%p stackDelta=%d",
                                        code_references, reinterpret_cast<void*>(possible_code),
                                        static_cast<unsigned long long>(possible_code - doom_base),
                                        found_addresses[i], displacement);
                                    log::info(code_text);
                                    ++code_references;
                                }
                            }
                        }
                        ++references;
                    }
                }
                if (pointer_region_size > static_cast<size_t>(maximum - pointer_region)) break;
                pointer_address = pointer_region + pointer_region_size;
            }
            char references_text[128];
            std::snprintf(references_text, sizeof(references_text),
                "F10 camera pointer scan complete: references=%u codeCandidates=%u",
                references, code_references);
            log::info(references_text);
        } else {
            log::warn("F10 found no candidate stable enough for pointer scan");
        }
    }
    delete job;
    cpu_camera_scan_state = 0;
    return 0;
}

void VKAPI_PTR hook_cmd_bind_pipeline(VkCommandBuffer command_buffer,
    VkPipelineBindPoint bind_point, VkPipeline pipeline) {
    kharvox::native::noteCommand(command_buffer);
    FbHook fb_(FB_H_CMD_BIND_PIPELINE);
    // PIPELINE PARITY (2026-08-21) - the last untested category.
    //
    // Eye 1's lighting pass now has: identical draws (DrawParity), correct
    // constants (HOLEStotal=0), redirected descriptors, every attachment
    // mirrored, DOOM's own render pass with DOOM's load ops and clear values,
    // and full barrier coverage on the only path DOOM uses. Everything the
    // pass is GIVEN is verified. What it DOES has never been measured, and it
    // accumulates 21% of the bright light while the same code under RenderDoc
    // accumulates 117%.
    //
    // Blend state is already captured per pipeline at creation
    // (pipeline_depth_stencil_info: blendEnable, src/dst factors, op). So the
    // question reduces to a sharp one: DOES THE DOUBLED CALL BIND THE SAME
    // PIPELINE HANDLES AS THE REAL CALL? Same handle means identical blend
    // state by construction and the category dies; a different handle, or a
    // pipeline bound by one call and not the other, is the divergence.
    //
    // Cost bounded by construction, the same way the uniform diff is: gated
    // on one relaxed atomic, dedupe keyed (pass, pipeline, side) BEFORE any
    // lock, hard cap, and it captures exactly ONE frame. Recording is
    // single-threaded only after scripted step 1 forces r_useSMP inline, so
    // this is additionally gated on frame_root_double_enabled - the same
    // premise, and the same scope, as the uniform diff's containers.
    if (bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS && pipeline != VK_NULL_HANDLE &&
        pipeline_parity_capturing.load(std::memory_order_relaxed) &&
        frame_root_double_enabled.load(std::memory_order_relaxed)) {
        const bool dbl = inside_frame_root_double;
        const uint32_t pass = dbl ? doubled_pass_seq : real_pass_seq;
        const uint64_t key = (static_cast<uint64_t>(pass) << 49) ^
                             (reinterpret_cast<uintptr_t>(pipeline) >> 4) ^
                             (dbl ? 0x8000000000000000ull : 0ull);
        auto& seen = dbl ? pipeline_parity_seen_doubled : pipeline_parity_seen_real;
        if (seen.size() < kMaxPipelineParityRecords && seen.insert(key).second) {
            (dbl ? pipeline_parity_doubled : pipeline_parity_real)[pass].insert(pipeline);
        }
    }
    if (bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        pass_state_record(0, reinterpret_cast<uint64_t>(pipeline),
            [&]{return std::format("handle={}", reinterpret_cast<void*>(pipeline));});
        bool should_log = false;
        PipelineDepthStencilInfo info;
        {
            std::unique_lock lock(swapchains_mutex);
            bound_graphics_pipelines[command_buffer] = pipeline;
            // Phase 0 (STEREO-IMPLEMENTATION-PLAN.md v2): log this
            // pipeline's depth/stencil state the FIRST time it's bound
            // while inside_render_opaque - real call and extra call both
            // qualify (inside_render_opaque stays true across both, see its
            // declaration), tagged below by which. Keyed per (pipeline,
            // callKind) - see logged_render_opaque_pipelines' declaration
            // above for why pipeline alone was wrong: it silently prevented
            // this from ever logging the SAME pipeline's state on both the
            // real and extra call, which is exactly the comparison Task 3
            // needs. Also excludes inside_extra_render_depth_reinvoke - see
            // its declaration above for why: without this, RenderDepth's own
            // pipelines (a different, depth-only pass) get mislabeled as
            // RenderOpaque's "extra" pipelines, since inside_render_opaque
            // stays true across that reinvocation too.
            const uint64_t pipeline_log_key =
                (reinterpret_cast<uint64_t>(pipeline) << 1) | (inside_injected_extra_call ? 1u : 0u);
            if (inside_render_opaque && !inside_extra_render_depth_reinvoke &&
                logged_render_opaque_pipelines.insert(pipeline_log_key).second) {
                const auto found = pipeline_depth_stencil_info.find(pipeline);
                if (found != pipeline_depth_stencil_info.end()) {
                    info = found->second;
                    should_log = true;
                }
            }
            // WorldPipelineBindingProbe - see representative_world_pipeline_
            // this_call's declaration above. Deliberately NOT gated by
            // logged_render_opaque_pipelines' dedup (that's a "log this
            // pipeline's state once ever per callKind" concern; this is "pick
            // the first qualifying pipeline THIS call," independent of
            // whether it's been seen before).
            if (inside_render_opaque && !inside_extra_render_depth_reinvoke &&
                representative_world_pipeline_this_call == VK_NULL_HANDLE) {
                const auto found = pipeline_depth_stencil_info.find(pipeline);
                if (found != pipeline_depth_stencil_info.end() &&
                    found->second.has_depth_stencil_state &&
                    found->second.depth_test_enable && found->second.depth_write_enable) {
                    representative_world_pipeline_this_call = pipeline;
                    representative_world_pipeline_cmdbuf_this_call = command_buffer;
                }
            }
        }
        // RenderDepthPipelineProbe (docs/STEREO-RESTART-PLAN.md Task 3, Round
        // 5 step 2 check (b)) - see render_depth_pipeline_probe_samples'
        // declaration above. Deliberately separate from the
        // inside_render_opaque-gated block above: this targets the REAL
        // RenderDepth invocation (inside_render_depth), not RenderOpaque.
        bool should_log_render_depth = false;
        PipelineDepthStencilInfo render_depth_info;
        if (inside_render_depth && real_call_redirect_armed.load(std::memory_order_relaxed) &&
            render_depth_pipeline_probe_samples.load(std::memory_order_relaxed) <
                kMaxRenderDepthPipelineProbeSamples) {
            std::unique_lock lock(swapchains_mutex);
            const auto found = pipeline_depth_stencil_info.find(pipeline);
            if (found != pipeline_depth_stencil_info.end()) {
                render_depth_info = found->second;
                should_log_render_depth =
                    render_depth_pipeline_probe_samples.fetch_add(1, std::memory_order_relaxed) <
                    kMaxRenderDepthPipelineProbeSamples;
            }
        }
        if (should_log_render_depth) {
            log::warn(std::format(
                "RenderDepthPipelineProbe pipeline={} hasDepthStencilState={} "
                "depthTestEnable={} depthWriteEnable={} depthCompareOp={}",
                reinterpret_cast<void*>(pipeline), render_depth_info.has_depth_stencil_state,
                render_depth_info.depth_test_enable, render_depth_info.depth_write_enable,
                static_cast<int>(render_depth_info.depth_compare_op)));
        }
        if (should_log) {
            log::warn(std::format(
                "PipelineDepthStencilProbe pipeline={} callKind={} eye={} "
                "hasDepthStencilState={} depthTestEnable={} depthWriteEnable={} "
                "depthCompareOp={} stencilTestEnable={} frontCompareOp={} frontRef={} "
                "backCompareOp={} backRef={} hasColorBlendState={} blendEnable=({},{},{}) "
                "srcColorFactor={} dstColorFactor={} colorBlendOp={} srcAlphaFactor={} "
                "dstAlphaFactor={} alphaBlendOp={}",
                reinterpret_cast<void*>(pipeline),
                inside_injected_extra_call ? "extra" : "real", current_eye_render,
                info.has_depth_stencil_state, info.depth_test_enable, info.depth_write_enable,
                static_cast<int>(info.depth_compare_op), info.stencil_test_enable,
                static_cast<int>(info.front_compare_op), info.front_reference,
                static_cast<int>(info.back_compare_op), info.back_reference,
                info.has_color_blend_state, info.blend_enable[0], info.blend_enable[1],
                info.blend_enable[2], static_cast<int>(info.src_color_blend_factor),
                static_cast<int>(info.dst_color_blend_factor), static_cast<int>(info.color_blend_op),
                static_cast<int>(info.src_alpha_blend_factor),
                static_cast<int>(info.dst_alpha_blend_factor),
                static_cast<int>(info.alpha_blend_op)));
        }
    }
    real_cmd_bind_pipeline(command_buffer, bind_point, pipeline);
}

// WorldVertexIndexProbe (docs/STEREO-RESTART-PLAN.md Task 3, last
// candidate) - see world_vertex_captured_this_call's declaration above.
// Same gating shape as WorldPipelineBindingProbe: HOME-armed for the real
// call, always active once INSERT arms the extra side.
bool world_vertex_index_probe_wanted(VkCommandBuffer command_buffer) {
    return representative_world_pipeline_this_call != VK_NULL_HANDLE &&
        command_buffer == representative_world_pipeline_cmdbuf_this_call &&
        (inside_injected_extra_call ||
         real_frame_query_gate_probe_armed.load(std::memory_order_relaxed));
}

void VKAPI_PTR hook_cmd_bind_vertex_buffers(VkCommandBuffer command_buffer, uint32_t first_binding,
    uint32_t binding_count, const VkBuffer* buffers, const VkDeviceSize* offsets) {
    kharvox::native::noteCommand(command_buffer);
    FbHook fb_(FB_H_CMD_BIND_VERTEX_BUFFERS);
    // VERTEX SOURCE PARITY (2026-08-18). Same-frame DrawParity proved the
    // doubled call issues IDENTICAL draws and index counts in the spark frame
    // (the entire difference, 141 draws and 444912 indices, is exactly my own
    // suppressed shadow passes). So the effects draws execute and render
    // nothing, and with counts identical the remaining variable is what they
    // read: the vertex/index buffer and OFFSET bound for them.
    //
    // DOOM streams per-frame geometry through a ring buffer, so if the real
    // call advances the ring, the doubled call re-draws from an offset that no
    // longer holds this frame's particles - identical commands, empty output.
    // This logs the bindings for both calls so the two can be compared
    // directly instead of inferred.
    if (vertex_parity_armed.load(std::memory_order_relaxed) &&
        (inside_real_frame_root || inside_frame_root_double) && buffers && binding_count) {
        const bool dbl = inside_frame_root_double;
        const uint32_t pass = dbl ? doubled_pass_seq : real_pass_seq;
        auto& lines = dbl ? vertex_parity_lines_doubled : vertex_parity_lines_real;
        if (pass >= kVertexParityFirstPass && pass <= kVertexParityLastPass &&
            lines.load(std::memory_order_relaxed) < kMaxVertexParityLines) {
            const uint64_t off = offsets ? static_cast<uint64_t>(offsets[0]) : 0ull;
            const uint64_t key = reinterpret_cast<uint64_t>(buffers[0]) ^ (off << 1);
            bool fresh = false;
            {
                std::scoped_lock lk(vertex_parity_mutex);
                auto& seen = dbl ? vertex_parity_seen_doubled : vertex_parity_seen_real;
                fresh = seen.insert({pass, key}).second;
            }
            if (fresh) {
                lines.fetch_add(1, std::memory_order_relaxed);
                log::warn(std::format(
                    "VertexParity call={} pass={} bindings={} vb0=buf {} off={}",
                    dbl ? "DOUBLED" : "real", pass, binding_count,
                    reinterpret_cast<void*>(buffers[0]),
                    static_cast<unsigned long long>(off)));
            }
        }
    }
    // VERTEX CONTENT readback - see vertex_content_record. Records only; the
    // copy happens at the pass boundary where a transfer is legal.
    // EVERY binding, not just [0]. The first version recorded buffers[0] only,
    // which is the per-vertex stream; particle INSTANCE data - position, size,
    // colour, one record per quad - is bound at 1 and above. So the single
    // buffer that decides whether a particle is drawn or degenerate was never
    // compared, while the probe reported "vertex contents identical" and that
    // claim was used to rule the whole class out.
    for (uint32_t i = 0; buffers && i < binding_count; ++i)
        vertex_content_record(buffers[i], offsets ? offsets[i] : 0, first_binding + i);
    if (!world_vertex_captured_this_call && world_vertex_index_probe_wanted(command_buffer)) {
        world_vertex_captured_this_call = true;
        std::string detail;
        for (uint32_t i = 0; buffers && i < binding_count; ++i) {
            detail += std::format(" vb{}=(buf={} off={})", first_binding + i,
                reinterpret_cast<void*>(buffers[i]), offsets ? static_cast<unsigned long long>(offsets[i]) : 0ull);
        }
        log::warn(std::format(
            "WorldVertexIndexProbe callKind={} eye={} pipeline={} stage=vertexBuffers{}",
            inside_injected_extra_call ? "extra" : "real", current_eye_render,
            reinterpret_cast<void*>(representative_world_pipeline_this_call), detail));
    }
    // Only scratch CPU handles: Vulkan consumes the array synchronously.
    // The lease preserves separate storage for recursive hooks/other threads.
    kharvox::native::BindScratchLease<std::vector<VkBuffer>> eyeLease;
    auto& eyeBuffers=eyeLease.get();eyeBuffers.clear();
    if(binding_count)eyeBuffers.assign(buffers,buffers+binding_count);
    for(auto& b:eyeBuffers)b=kharvox::native::bufferForEye(b);
    real_cmd_bind_vertex_buffers(command_buffer, first_binding, binding_count, eyeBuffers.data(), offsets);
}

void VKAPI_PTR hook_cmd_bind_index_buffer(VkCommandBuffer command_buffer, VkBuffer buffer,
    VkDeviceSize offset, VkIndexType index_type) {
    kharvox::native::noteCommand(command_buffer);
    FbHook fb_(FB_H_CMD_BIND_INDEX_BUFFER);
    if (!world_index_captured_this_call && world_vertex_index_probe_wanted(command_buffer)) {
        world_index_captured_this_call = true;
        log::warn(std::format(
            "WorldVertexIndexProbe callKind={} eye={} pipeline={} stage=indexBuffer "
            "buf={} off={} indexType={}",
            inside_injected_extra_call ? "extra" : "real", current_eye_render,
            reinterpret_cast<void*>(representative_world_pipeline_this_call),
            reinterpret_cast<void*>(buffer), static_cast<unsigned long long>(offset),
            static_cast<int>(index_type)));
    }
    real_cmd_bind_index_buffer(command_buffer, kharvox::native::bufferForEye(buffer), offset, index_type);
}

// Is this draw the doubled call writing into DOOM's shared shadow atlas?
// Those writes corrupt eye 0: the atlas is DOOM's own image, and the doubled
// call re-renders it from a shifted camera with different culling. Shadows are
// light-space, so eye 1 loses nothing by using DOOM's - it is the one resource
// where sharing genuinely is correct, provided we only READ it.
inline bool doubled_draw_hits_shared_shadow_atlas() {
    if (!inside_frame_root_double) return false;
    if (!suppress_doubled_shadow_draws.load(std::memory_order_relaxed)) return false;
    const uintptr_t atlas = shared_shadow_framebuffer.load(std::memory_order_relaxed);
    if (!atlas) return false;
    if (reinterpret_cast<uintptr_t>(doubled_current_framebuffer) != atlas) return false;
    suppressed_shadow_draws.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ---------------------------------------------------------------------------
// DEFERRED INPUT MAP (2026-08-19). Run 2's instrument, and the review's §3
// item (d) - the one part of its Run 1 that was never built.
//
// WHY THIS AND NOT THE STORAGE BUFFERS. Run 1 refuted H-A outright. With
// light-culling suppression toggled OFF (verified on the [rec] timeline:
// DrawParity printed dispatchesSuppressed DOUBLED=0, down from thousands),
// the mirror's pass-19 output was UNCHANGED - 54 bright pixels against the
// suppression-ON baseline range of 16-99, and 0.627% nonzero against a
// baseline of 0.584-0.656%. Running the culling compute for eye 1 does not
// restore its lighting, so the light lists are not what is missing.
//
// What IS missing, from the p20 crops: the entire additive atmospheric layer.
// The real call's frame is hazy and warm with volumetric fog, glowing lava on
// the canyon floor and a spark burst on the wall. The doubled call's is
// crystal clear - the distant terrain is sharp because there is no fog on it
// at all. Base lighting and shadows are present and correct. Only the
// additive/emissive/volumetric layer is absent.
//
// That is H-B: a sampled input of the deferred chain is empty or unsubstituted
// in the mirror. This names it.
//
// HOOKED AT THE DRAW, NOT IN hook_cmd_bind_descriptor_sets - the same
// structural reason DrawParity gives: that function has four
// real_cmd_bind_descriptor_sets call sites and a stack of early returns which
// have silently killed three probes. A vkCmdDraw* hook is a top-level entry
// from DOOM and nothing in this DLL can pre-empt it.
//
// WHAT THIS PROBE CANNOT SEE: it reports which RESOURCE each binding names and
// whether we hold a mirror for it. That is the [rec] timeline. It cannot say
// whether the resource has CONTENT - an input correctly substituted with a
// mirror nothing ever wrote looks identical here to one that is full. If both
// sides come back binding the same resources with mirrors held throughout,
// the answer is content, and the next instrument is a recorded copy of the
// named images, not another binding listing.
// deferred_input_map_armed / deferred_input_budget are declared up with
// ParityBudget - the frame-root disarm site runs far above this point and
// needs them.
//
// Dedupe on (pass, setIndex, binding) per side - the deferred passes issue one
// draw each, but the probe also covers the transparency passes where a binding
// recurs across many draws. Without this the budget goes on repeats, which is
// exactly how FogHunt spent all 60 of its lines on pass 12.
std::set<std::tuple<uint32_t, uint32_t, uint32_t>> deferred_input_seen_real;
std::set<std::tuple<uint32_t, uint32_t, uint32_t>> deferred_input_seen_doubled;
std::mutex deferred_input_mutex;
// The job-0x1873830 deferred chain (17-20) plus the transparency pass after
// it. Pass 16 is excluded deliberately: 152 draws, and it is the G-buffer
// pass whose output is already known to be correct on both sides.
// WINDOW MOVED 2026-08-19 after the two-sided PassTimeline. It was 17-21,
// chosen when the divergence was believed to be at pass 19 - which was an
// artefact of reading end-of-frame dumps as a timeline. The measured answer:
// at pass 20 BOTH eyes have 0 bright pixels; at pass 25 the left has 10122 and
// the right 273. The effects are composited between 20 and 25, and passes
// 22-25 were OUTSIDE the old window, so their bindings have never been listed
// by anything (FogHunt died at pass 12).
// PASS RANGE WIDENED TO COVER WHERE THE LOSS ACTUALLY IS (2026-08-22).
//
// This was 20..26, and every "the inputs are identical" conclusion drawn from
// it - 31/31 bindings, both froxel volumes, and the byte-identical froxel
// CONTENT comparison that rides this same walk - therefore described passes
// 20 to 26 only.
//
// The loss is complete at p15, the first full-resolution HDR stage. Passes
// 20..26 are entirely DOWNSTREAM of it. So those results were parity
// measurements taken on passes that had already been exonerated, and the pass
// where the effects actually go missing has never been compared at all.
//
// Widened to start at 0. The first HDR stage, the lighting that feeds it and
// everything before them are now in scope. The budget above was raised to
// match, and the COMPLETE sentinel reports truncation - which matters more
// here than anywhere, because this probe is used to argue from ABSENCE.
constexpr uint32_t kDeferredInputFirstPass = 0;
constexpr uint32_t kDeferredInputLastPass = 26;

void probe_deferred_inputs(VkCommandBuffer command_buffer) {
    if (!deferred_input_map_armed.load(std::memory_order_relaxed)) return;
    const bool doubled = inside_frame_root_double;
    if (!doubled && !inside_real_frame_root) return;
    const uint32_t pass = doubled ? doubled_pass_seq : real_pass_seq;
    if (pass < kDeferredInputFirstPass || pass > kDeferredInputLastPass) return;
    if (last_bound_graphics_sets.empty()) return;

    // Gathered under the locks, EMITTED after they are released - log.cpp
    // flushes per line and the render thread must not do file I/O while
    // holding mirror_mutex (Rule 9, the run-17 wedge).
    std::vector<std::string> lines;
    {
        std::scoped_lock mirror_lock(mirror_mutex);
        std::unique_lock lock(swapchains_mutex);
        for (const auto& [set_index, set] : last_bound_graphics_sets) {
            // BUFFER descriptors first - the half this probe never walked.
            // Records the region; the copy is emitted at the pass boundary
            // through the same pending list as the vertex and indirect ones.
            if (auto buf_it = descriptor_sets.find(set);
                buf_it != descriptor_sets.end() && pass >= kUboFirstPass) {
                const uint32_t side = doubled ? 1u : 0u;
                for (const auto& [binding, b] : buf_it->second) {
                    if (b.buffer == VK_NULL_HANDLE) continue;
                    {
                        std::scoped_lock lk(ubo_seen_mutex);
                        if (!ubo_seen[side].insert({pass, set_index, binding}).second) continue;
                    }
                    if (ubo_record_count[side] >= kUboSlots) break;
                    const uint32_t slot = ubo_record_count[side]++;
                    ubo_records[side][slot] = UboRecord{b.buffer, b.offset, b.range, pass,
                                                       set_index, binding, b.type, true};
                    VkDeviceSize size = b.range == VK_WHOLE_SIZE ? kUboSlotStride : b.range;
                    if (size > kUboSlotStride) size = kUboSlotStride;
                    pending_arg_copies.push_back(PendingArgCopy{
                        b.buffer, b.offset, size, side,
                        kIndirectSlots + kVertexSlots + slot});
                }
            }
            auto img_it = descriptor_image_sets.find(set);
            if (img_it == descriptor_image_sets.end()) continue;
            for (const auto& [binding, d] : img_it->second) {
                {
                    std::scoped_lock lk(deferred_input_mutex);
                    auto& seen = doubled ? deferred_input_seen_doubled : deferred_input_seen_real;
                    if (!seen.insert({pass, set_index, binding}).second) continue;
                }
                if (!deferred_input_budget.take(doubled)) continue;
                const VkImageView view = d.image_view;
                VkImage image = VK_NULL_HANDLE;
                if (auto v = image_view_to_image.find(view); v != image_view_to_image.end())
                    image = v->second;
                // Do we hold a private copy reachable from this view?
                //
                // CORRECTED 2026-08-19, same day, after this probe's first run
                // reported mirrorHeld=false for the depth-stencil input and I
                // read that as "the deferred chain samples DOOM's real depth".
                // It does not. There are THREE ways a binding reaches a mirror
                // and the first version checked only one:
                //   1. mirrored_views  - the view is itself a mirror key
                //   2. alias_mirror_views - we built a matching view over the
                //      mirror IMAGE because DOOM's view is not a mirror key
                //      (this is the depth path, and it works: AliasView
                //      CREATED, no GATED, no NO-INFO, barrier-substituted and
                //      layout-locked)
                //   3. mirrored_images via the view's image - a mirror exists
                //      and the alias path can build a view for it on demand
                // Reporting only (1) turned a working substitution into a
                // false "unmirrored" and nearly sent the next run at a
                // non-bug. Each path is now reported separately so the reader
                // sees WHICH one carries the binding rather than a single bit
                // that conflates them.
                const bool keyed = mirrored_views.count(view) != 0;
                const bool aliased = alias_mirror_views.count(view) != 0;
                bool image_mirrored = false;
                if (image != VK_NULL_HANDLE)
                    image_mirrored = mirrored_images.count(image) != 0;
                const bool mirror_held = keyed || aliased || image_mirrored;
                const char* extent_note = "unknown-extent";
                std::string detail;
                if (image != VK_NULL_HANDLE) {
                    if (auto c = created_image_infos.find(image); c != created_image_infos.end()) {
                        extent_note = c->second.type == VK_IMAGE_TYPE_3D
                            ? "3D-VOLUME (froxel fog/scattering candidate)"
                            : "2D";
                        detail = std::format(
                            " extent={}x{}x{} format={} usage={:#x} mips={}",
                            c->second.extent.width, c->second.extent.height,
                            c->second.extent.depth, static_cast<int>(c->second.format),
                            static_cast<unsigned>(c->second.usage), c->second.mips);
                        // FROXEL CONTENT - LATCH ONLY. The copy cannot happen
                        // here: this runs from hook_cmd_draw, inside a render
                        // pass instance, where vkCmdCopyImageToBuffer and image
                        // barriers are illegal. Recorded now, copied at the
                        // next end-of-pass. See froxel_pending_image.
                        //
                        // Up to kFroxelVolumes DISTINCT volumes per side, in
                        // bind order - DOOM binds two and only the second, the
                        // 256x256x96 half-float one, looks like the scattering
                        // volume. Capturing one was what made two runs void.
                        if (froxel_probe_armed.load(std::memory_order_relaxed) &&
                            c->second.type == VK_IMAGE_TYPE_3D) {
                            const uint32_t side = doubled ? 1u : 0u;
                            bool known = false;
                            for (uint32_t v = 0; v < kFroxelVolumes; ++v)
                                if (froxel_pending_image[side][v] == image ||
                                    froxel_slice_source[side][v] == image) known = true;
                            if (!known) {
                                for (uint32_t v = 0; v < kFroxelVolumes; ++v) {
                                    if (froxel_pending_image[side][v] != VK_NULL_HANDLE ||
                                        froxel_slice_captured[side][v]) continue;
                                    froxel_pending_image[side][v] = image;
                                    froxel_pending_extent[side][v] = c->second.extent;
                                    froxel_pending_format[side][v] = c->second.format;
                                    break;
                                }
                            }
                        }
                        // Select the additive HDR layer for content capture -
                        // see deferred_hdr_input_real. Matched by SHAPE, not by
                        // a pinned pass/binding number, because those move
                        // between builds and a stale pin captures the wrong
                        // image silently. Full-screen B10G11R11 that is both a
                        // colour attachment and sampled is unique in this
                        // chain; the DeferredInputMap listing above is the
                        // check that it stayed unique.
                        if (c->second.format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 &&
                            c->second.extent.width > 2048 && c->second.type == VK_IMAGE_TYPE_2D) {
                            if (!doubled) {
                                deferred_hdr_input_real = image;
                                deferred_hdr_input_pass = pass;
                                deferred_hdr_input_extent = {c->second.extent.width,
                                                             c->second.extent.height};
                            } else {
                                // Resolve OUR copy the same three ways the
                                // substitution does, so the dump follows the
                                // binding rather than assuming one path.
                                VkImage mirror = VK_NULL_HANDLE;
                                if (auto mi = mirrored_images.find(image);
                                    mi != mirrored_images.end()) mirror = mi->second;
                                if (mirror == VK_NULL_HANDLE) {
                                    if (auto mv = mirrored_views.find(view);
                                        mv != mirrored_views.end()) mirror = mv->second.image;
                                }
                                if (mirror == VK_NULL_HANDLE) {
                                    if (auto av = alias_mirror_views.find(view);
                                        av != alias_mirror_views.end())
                                        mirror = av->second.mirror_image;
                                }
                                deferred_hdr_input_mirror = mirror;
                            }
                        }
                    }
                }
                lines.push_back(std::format(
                    "DeferredInputMap [rec] {} pass={:2} setIdx={} binding={:2} type={} "
                    "view={} image={} {}{} mirrorHeld={} (keyed={} aliased={} imageMirrored={})",
                    doubled ? "DOUBLED" : "REAL   ", pass, set_index, binding,
                    static_cast<int>(d.type), reinterpret_cast<void*>(view),
                    reinterpret_cast<void*>(image), extent_note, detail, mirror_held,
                    keyed, aliased, image_mirrored));
            }
        }
    }
    for (const std::string& l : lines) log::warn(l);
}

// ---------------------------------------------------------------------------
// INDIRECT ARGS READBACK (2026-08-19). The review's Run 1 item (a), named on
// day one and never built. It is the only measurement that can distinguish
// "the doubled call submits the same draws" from "the doubled call submits
// the same draws and they render nothing".
//
// vkCmdDrawIndexedIndirect takes indexCount and instanceCount FROM A BUFFER,
// read by the GPU at execution time. DrawParity sums the CPU-side drawCount
// argument, which is identical by construction because the doubled call
// replays the same code. A draw whose buffer says instanceCount=0 is a no-op
// that NO CPU-side counter can see. Passes 25 carries 10 such draws, and
// small-index high-count draws are exactly the shape of a particle system.
//
// WHY IT MATTERS NOW: the doubled call runs strictly AFTER the real call
// (see the FrameRootDouble comment). If DOOM's per-frame transient data is
// consumed or advanced by the real call, the doubled call's indirect draws
// read spent args. instanceCount=0 on the doubled side and nonzero on the
// real side would confirm that mechanism outright and name the buffer.
//
// RECORDED COPY, AT A PASS BOUNDARY. vkCmdCopyBuffer is illegal inside a
// render pass, so the draw hook only RECORDS the (buffer, offset, drawCount,
// stride) it saw, and hook_cmd_end_render_pass emits the copies after the
// real vkCmdEndRenderPass - the same placement ChainWalk already uses safely
// for its image snapshots.
//
// WHAT THIS CANNOT SEE: it reads the args as they stand when the copy
// executes, which is at the END of the pass that issued the draw, not at the
// instant the draw consumed them. A buffer rewritten between the draw and the
// pass end would read wrong. It also cannot see draws issued through
// vkCmdDrawIndexedIndirectCount, which is still unhooked - if a side shows
// FEWER recorded commands than expected, that is the reason to suspect.
// kIndirectSlots is declared up with ParityBudget - see there.
// The readback LAYOUT constants moved up beside ParityBudget - see there. Only
// the explanatory notes stay here, with the code that uses them.
// VERTEX CONTENT (2026-08-19). The last unmeasured input at pass 25.
//
// PassTimeline pinned the pass: at 22/23/24 both eyes are identical, at 25 the
// left gains the additive layer (maxLuma 8.5178 -> 12.3018, mean 0.0172 ->
// 0.0280) and the right gains nothing (9.1887 -> 9.1887, mean 0.0138 ->
// 0.0140). DeferredInputMap then showed every one of pass 25's sampled
// bindings is IDENTICAL between the two calls, and DrawParity shows identical
// draws (97 + 10 indirect, 5634 indices). Push constants were already ruled
// out in August - the hook was installed for 48 s of live play and DOOM never
// called vkCmdPushConstants once.
//
// That leaves exactly one thing the draws read that nobody has ever compared:
// the CONTENTS of the vertex buffer. VertexParity compared buffer HANDLES and
// OFFSETS and found them identical - which is precisely the case where handle
// identity proves nothing, because DOOM streams per-frame geometry through a
// ring buffer. If the real call advances or overwrites that ring, the doubled
// call re-draws from an offset that no longer holds this frame's particles:
// identical commands, identical bindings, empty output. Exactly the symptom.
//
// Same recorded-copy machinery as the indirect args, which ran clean.
// kVertexSlots is declared up with ParityBudget - see there.
// kVertexSlotStride / kVertexSideStride / kVertexRegionBase: see the layout
// block beside ParityBudget.
// UNIFORM/STORAGE BUFFER CONTENTS (2026-08-19). The gap in "every binding at
// pass 25 is identical" - that claim covered IMAGES ONLY.
//
// descriptor_image_sets and descriptor_sets are separate maps, and
// probe_deferred_inputs walks only the first. So texture bindings were
// compared and BUFFER descriptors - the uniform buffers carrying camera
// matrices, particle parameters and fog constants - were never listed, let
// alone compared. A UBO holding the wrong values makes the shader compute
// degenerate positions or zero alpha: fragments discarded, every other
// measurement identical, which is precisely the standing symptom.
//
// Same recorded-copy machinery as the vertex contents, which ran clean.
// kUboSlots / kUboSlotStride / kUboSideStride / kUboRegionBase: see the layout
// block beside ParityBudget.
// DEPTH CONTENT (2026-08-19). The last substituted input never compared.
//
// At pass 25 exactly ONE resource is swapped for the right eye: the depth
// buffer, via the alias path. Everything else is either shared with DOOM
// (identical by construction) or measured byte-identical. Depth is proven to
// be USED - clearing it wrecks the scene - but its CONTENT has never been
// checked against DOOM's.
//
// Why that is the remaining candidate: soft particles and volumetric fog
// SAMPLE depth in the shader and fade against it. That is a shader-side
// discard, not the depth test - so wrong depth values make alpha compute to
// ~0 and the effects vanish while identical draws, bindings, pipeline state
// and depth-test configuration all report clean. It is the only mechanism
// that survives every measurement made this session.
//
// A strip rather than the whole image: 3818x2032 of D24S8 is 31 MB per side
// and a 64 KB band across the middle of the frame is where the sparks and the
// canyon floor are, which is enough to tell "same depth" from "different
// depth". The constants live in the layout block beside ParityBudget.
// Window narrowed to the confirmed divergence. PassTimeline has now measured
// it with the control: at pass 25 the LEFT eye's mean luma goes 0.0170 ->
// 0.0347 (+104%, peak +1.22) while the RIGHT eye goes 0.0133 -> 0.0135 with
// peak unchanged to four decimals. Passes 22-24 are identical between the
// eyes, so spending slots there is how the last comparison starved before
// reaching 25.
constexpr uint32_t kVertexContentFirstPass = 24;
constexpr uint32_t kVertexContentLastPass = 26;

// indirect_args_probe_armed / _ready are declared up with ParityBudget - the
// frame-root disarm site and the F12 handler both run above this point.
VkBuffer indirect_readback_buffer = VK_NULL_HANDLE;
VkDeviceMemory indirect_readback_memory = VK_NULL_HANDLE;
void* indirect_readback_mapped = nullptr;
// Accessor for the frame-root code above, which reads the depth strip but is
// compiled before this declaration.
void* indirect_readback_mapped_fwd() { return indirect_readback_mapped; }

struct IndirectArgRecord {
    VkBuffer buffer;
    VkDeviceSize offset;
    uint32_t draw_count;
    uint32_t stride;
    uint32_t pass;
    bool indexed;
    bool valid;
};
// [side][slot], side 0 = real, side 1 = doubled. Plain arrays: written only on
// the render thread inside one call or the other, read at present after both
// have finished - the same discipline DrawParity uses.
IndirectArgRecord indirect_args[2][kIndirectSlots]{};
uint32_t indirect_arg_count[2]{};
// Vertex-content records, same shape and the same threading discipline.
struct VertexContentRecord {
    VkBuffer buffer;
    VkDeviceSize offset;
    uint32_t pass;
    // Which vertex binding this is. Needed both to report WHICH stream differs
    // and to match the two sides: a mirrored instance buffer legitimately has
    // a different handle, so matching on (pass, buffer, offset) would fail to
    // pair it and report it as real-only instead of comparing it.
    uint32_t binding;
    bool valid;
};
VertexContentRecord vertex_contents[2][kVertexSlots]{};
uint32_t vertex_content_count[2]{};
// Dedupe per side on (pass, buffer, offset): DOOM rebinds the same vertex
// buffer for many consecutive draws, and without this the 12 slots would be
// spent on one binding repeated. Same failure that starved FogHunt.
std::set<std::tuple<uint32_t, uint64_t, uint64_t>> vertex_content_seen[2];
std::mutex vertex_content_mutex;
// UboRecord and its state are declared up with ParityBudget: the recorder
// lives in probe_deferred_inputs, which is defined ABOVE this block.
// Regions recorded during the current render pass, flushed as copies at its
// end. thread_local: only the render thread records draws.
// PendingArgCopy / pending_arg_copies are declared up with ParityBudget - all
// three recorders share the one list so there is a single flush site.

bool ensure_indirect_readback() {
    if (indirect_readback_mapped) return true;
    const VulkanState& vk = vulkan_state();
    if (!vk.device || !vk.physical_device) return false;
    auto create_buffer = load_device_proc<PFN_vkCreateBuffer>(vk.device, "vkCreateBuffer");
    auto get_reqs = load_device_proc<PFN_vkGetBufferMemoryRequirements>(
        vk.device, "vkGetBufferMemoryRequirements");
    auto allocate_memory = load_device_proc<PFN_vkAllocateMemory>(vk.device, "vkAllocateMemory");
    auto bind_memory = load_device_proc<PFN_vkBindBufferMemory>(vk.device, "vkBindBufferMemory");
    auto map_memory = load_device_proc<PFN_vkMapMemory>(vk.device, "vkMapMemory");
    // Same module-based lookup the other allocation sites use: this is an
    // INSTANCE-level entry, so load_device_proc is the wrong tool for it.
    PFN_vkGetPhysicalDeviceMemoryProperties get_mem_props = nullptr;
    if (HMODULE mod = GetModuleHandleW(L"vulkan-1.dll"))
        get_mem_props = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
            kharvox::native::instanceProc("vkGetPhysicalDeviceMemoryProperties"));
    if (!create_buffer || !get_reqs || !allocate_memory || !bind_memory || !map_memory ||
        !get_mem_props) {
        log::error("IndirectArgs: could not load the Vulkan entry points for the readback buffer");
        return false;
    }
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = kIndirectReadbackSize;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (create_buffer(vk.device, &bi, nullptr, &indirect_readback_buffer) != VK_SUCCESS) {
        log::error("IndirectArgs: readback buffer creation failed");
        return false;
    }
    VkMemoryRequirements reqs{};
    get_reqs(vk.device, indirect_readback_buffer, &reqs);
    VkPhysicalDeviceMemoryProperties mem_props{};
    get_mem_props(vk.physical_device, &mem_props);
    uint32_t type_index = 0;
    if (!find_memory_type(mem_props, reqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            type_index)) {
        log::error("IndirectArgs: no host-visible memory type for the readback buffer");
        return false;
    }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = reqs.size;
    ai.memoryTypeIndex = type_index;
    if (allocate_memory(vk.device, &ai, nullptr, &indirect_readback_memory) != VK_SUCCESS ||
        bind_memory(vk.device, indirect_readback_buffer, indirect_readback_memory, 0) !=
            VK_SUCCESS ||
        map_memory(vk.device, indirect_readback_memory, 0, VK_WHOLE_SIZE, 0,
                   &indirect_readback_mapped) != VK_SUCCESS) {
        log::error("IndirectArgs: readback memory alloc/bind/map failed");
        indirect_readback_mapped = nullptr;
        return false;
    }
    std::memset(indirect_readback_mapped, 0, static_cast<size_t>(kIndirectReadbackSize));
    log::warn(std::format(
        "IndirectArgs: readback buffer ready ({} bytes, {} slots/side, {} structs/slot)",
        static_cast<unsigned long long>(kIndirectReadbackSize), kIndirectSlots,
        kIndirectStructsPerSlot));
    return true;
}

// Called from the indirect draw hooks. Records the args region; the copy
// itself is emitted at the end of the render pass.
void indirect_args_record(VkBuffer buffer, VkDeviceSize offset, uint32_t draw_count,
                          uint32_t stride, bool indexed) {
    if (!indirect_args_probe_armed.load(std::memory_order_relaxed)) return;
    const bool doubled = inside_frame_root_double;
    if (!doubled && !inside_real_frame_root) return;
    if (!ensure_indirect_readback()) return;
    const uint32_t side = doubled ? 1u : 0u;
    if (indirect_arg_count[side] >= kIndirectSlots) return;
    const uint32_t slot = indirect_arg_count[side]++;
    const uint32_t struct_size = indexed ? 20u : 16u;
    const uint32_t effective_stride = stride ? stride : struct_size;
    const uint32_t structs = std::min(draw_count, kIndirectStructsPerSlot);
    indirect_args[side][slot] = IndirectArgRecord{
        buffer, offset, draw_count, stride, doubled ? doubled_pass_seq : real_pass_seq, indexed,
        true};
    VkDeviceSize size = static_cast<VkDeviceSize>(structs) * effective_stride;
    if (size > kIndirectSlotStride) size = kIndirectSlotStride;
    pending_arg_copies.push_back(PendingArgCopy{buffer, offset, size, side, slot});
}

// Called from hook_cmd_bind_vertex_buffers. Records the first binding's
// region; the copy is emitted at the end of the render pass.
void vertex_content_record(VkBuffer buffer, VkDeviceSize offset, uint32_t binding) {
    if (!indirect_args_probe_armed.load(std::memory_order_relaxed)) return;
    const bool doubled = inside_frame_root_double;
    if (!doubled && !inside_real_frame_root) return;
    const uint32_t pass = doubled ? doubled_pass_seq : real_pass_seq;
    if (pass < kVertexContentFirstPass || pass > kVertexContentLastPass) return;
    if (buffer == VK_NULL_HANDLE) return;
    const uint32_t side = doubled ? 1u : 0u;
    {
        std::scoped_lock lk(vertex_content_mutex);
        // Dedupe on (pass, binding, offset), NOT on the buffer handle: the two
        // sides use different handles for a mirrored buffer, and a handle in
        // the key would make the same logical stream look like two.
        if (!vertex_content_seen[side]
                 .insert({pass, binding, static_cast<uint64_t>(offset)})
                 .second)
            return;
    }
    if (vertex_content_count[side] >= kVertexSlots) return;
    if (!ensure_indirect_readback()) return;
    const uint32_t slot = vertex_content_count[side]++;
    vertex_contents[side][slot] = VertexContentRecord{buffer, offset, pass, binding, true};
    pending_arg_copies.push_back(PendingArgCopy{
        buffer, offset, kVertexSlotStride, side,
        // Encode the vertex region in the slot index: flush maps slots >=
        // kIndirectSlots into the vertex area. Keeping ONE pending list means
        // one flush site and no second chance to forget one.
        kIndirectSlots + slot});
}

// Called from hook_cmd_end_render_pass, AFTER the real vkCmdEndRenderPass, so
// the transfer is recorded outside the render pass where it is legal.
// Copies a horizontal strip of a DEPTH image into the shared readback buffer.
// Called at a pass boundary, where a transfer is legal. side 0 = DOOM's real
// depth, side 1 = our mirror of it.
// See the declaration for why this exists. Modelled on depth_strip_copy,
// including its layout discipline - transition from the layout the image is
// actually in and restore it exactly, because a wrong restore is what hangs
// the GPU and a wrong oldLayout is permitted to discard contents.
void froxel_slice_copy(VkCommandBuffer command_buffer, VkImage image,
                       VkExtent3D extent, VkFormat format, uint32_t side, uint32_t vol) {
    if (image == VK_NULL_HANDLE || !indirect_readback_buffer) return;
    if (side > 1 || vol >= kFroxelVolumes || froxel_slice_captured[side][vol]) return;
    // THE LAYOUT IS LOOKED UP, NOT GUESSED (2026-08-29, fourth version).
    //
    // The first three versions hardcoded SHADER_READ_ONLY_OPTIMAL and every one
    // of them read back all zeros on both sides. depth_strip_copy never made
    // that mistake - it is passed the TRACKED layout of the image it copies -
    // and it is the one content probe in this file that has ever produced real
    // hashes.
    //
    // A froxel volume written by compute as a storage image and sampled by
    // graphics is commonly left in GENERAL rather than transitioned, so the
    // guess was probably wrong from the start. Vulkan permits oldLayout to
    // match the current layout or be UNDEFINED, and UNDEFINED explicitly means
    // "discard the contents" - so a wrong guess here does not fail loudly, it
    // silently returns zeros. Which is exactly what happened, three times.
    //
    // doom_image_layouts is fed from the barriers we already hook. Rule 3: if
    // the image is not in it we REFUSE rather than guess, because another
    // all-zero readback would be indistinguishable from an empty volume.
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    {
        std::scoped_lock lock(mirror_mutex);
        if (auto it = doom_image_layouts.find(image); it != doom_image_layouts.end())
            layout = it->second;
    }
    if (layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        static std::atomic_uint32_t unknown_layout_lines{0};
        if (unknown_layout_lines.fetch_add(1, std::memory_order_relaxed) < 4)
            log::error(std::format(
                "FroxelContent REFUSED vol{} side{}: no tracked layout for image {} - we have "
                "never seen DOOM transition it. Copying from a guessed oldLayout is permitted to "
                "DISCARD the contents and would return zeros that look exactly like an empty "
                "volume. Not copying.",
                vol, side, reinterpret_cast<void*>(image)));
        return;
    }
    if (!extent.width || !extent.height) return;
    const auto& vk = vulkan_state();
    auto cmd_barrier = load_device_proc<PFN_vkCmdPipelineBarrier>(vk.device, "vkCmdPipelineBarrier");
    auto cmd_copy = load_device_proc<PFN_vkCmdCopyImageToBuffer>(
        vk.device, "vkCmdCopyImageToBuffer");
    if (!cmd_barrier || !cmd_copy) return;
    // ONE Z SLICE, and only as many rows as the region holds. The measured
    // volumes are 256x256x32 (fmt 37) and 256x256x96 (fmt 97); at 8 bytes per
    // texel a 256-wide row is 2048 bytes, so 65536 bytes is 32 rows - an eighth
    // of the slice, taken from its middle. Bounded by construction rather than
    // by hoping the volume is small.
    // ONE ROW, ALL DEPTH SLICES - not one slice through the middle
    // (2026-08-29, fifth version).
    //
    // The DepthContent probe produced real hashes 20 ms before this one
    // reported all zeros, in the same run, through the same buffer and the same
    // present-time read. So the copy machinery is FINE and the layout lookup
    // returned SHADER_READ_ONLY_OPTIMAL, which was right. What is left is WHERE
    // the sample was taken.
    //
    // Both earlier attempts copied a horizontal band of ONE z slice from the
    // middle of the volume: z=16 of 32, and z=48 of 96. A froxel volume stores
    // scattering per depth slice and is routinely EMPTY in its far half - so an
    // all-zero read there says nothing about whether the volume is populated.
    //
    // This copies a single ROW spanning EVERY depth slice instead, as wide as
    // the region allows. If any slice at that row carries scattering, it lands
    // in the window. Coverage across the axis that matters, rather than a
    // gamble on one slice being the interesting one.
    const uint32_t bpp = format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8u : 4u;
    const uint32_t depth = extent.depth ? extent.depth : 1u;
    const uint32_t max_w = kFroxelSliceBytes / (depth * bpp);
    const uint32_t w = std::min(extent.width, std::max(1u, max_w));
    const uint32_t y0 = extent.height / 2;
    VkImageMemoryBarrier to_src{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_src.oldLayout = layout;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.image = image;
    to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    cmd_barrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_src);
    VkBufferImageCopy region{};
    region.bufferOffset = kFroxelRegionBase + (side * kFroxelVolumes + vol) * kFroxelSliceBytes;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {0, static_cast<int32_t>(y0), 0};
    region.imageExtent = {w, 1, depth};
    cmd_copy(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
             indirect_readback_buffer, 1, &region);
    VkImageMemoryBarrier back = to_src;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = layout;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    cmd_barrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &back);
    froxel_slice_captured[side][vol] = true;
    froxel_slice_source[side][vol] = image;
    froxel_compare_ready.store(true, std::memory_order_release);
    log::warn(std::format(
        "FroxelContent captured {} vol{} image={} {}x{}x{} row y={} width {} across ALL {} "
        "depth slices | fmt={} bpp={} srcLayout={} bytes={}",
        side ? "DOUBLED" : "REAL   ", vol, reinterpret_cast<void*>(image), extent.width,
        extent.height, extent.depth, y0, w, depth, static_cast<int>(format), bpp,
        static_cast<int>(layout), w * depth * bpp));
}

// Read AT PRESENT, never on the render thread. The copy has only been RECORDED
// when the doubled call returns; reading then gives the memset zeros. That
// split already voided two DepthContent runs and the comment there says so.
void emit_froxel_content_verdict() {
    if (!froxel_compare_ready.exchange(false, std::memory_order_acquire)) return;
    const uint8_t* p = static_cast<const uint8_t*>(indirect_readback_mapped_fwd());
    if (!p) {
        log::error("FroxelContent VOID - readback buffer not mapped.");
        return;
    }
    uint32_t populated_any = 0;
    for (uint32_t v = 0; v < kFroxelVolumes; ++v) {
        if (!froxel_slice_captured[0][v] || !froxel_slice_captured[1][v]) {
            log::warn(std::format(
                "FroxelContent vol{}: not captured on both sides (real={} doubled={}) - no "
                "comparison for this volume. Not a result either way.",
                v, froxel_slice_captured[0][v], froxel_slice_captured[1][v]));
            continue;
        }
        uint64_t h[2] = {1469598103934665603ull, 1469598103934665603ull};
        uint32_t nonzero[2] = {0, 0};
        uint32_t differing = 0;
        for (uint32_t s = 0; s < 2; ++s) {
            const uint8_t* q = p + kFroxelRegionBase +
                               (s * kFroxelVolumes + v) * kFroxelSliceBytes;
            for (uint32_t i = 0; i < kFroxelSliceBytes; ++i) {
                h[s] = (h[s] ^ q[i]) * 1099511628211ull;
                if (q[i]) ++nonzero[s];
            }
        }
        {
            const uint8_t* a = p + kFroxelRegionBase + (0 * kFroxelVolumes + v) * kFroxelSliceBytes;
            const uint8_t* b = p + kFroxelRegionBase + (1 * kFroxelVolumes + v) * kFroxelSliceBytes;
            for (uint32_t i = 0; i < kFroxelSliceBytes; ++i) if (a[i] != b[i]) ++differing;
        }
        // Rule 3, per volume: two empty slices hash alike and would read as
        // "identical" for entirely the wrong reason. Reported, never silently
        // folded into the verdict.
        if (!nonzero[0] || !nonzero[1]) {
            log::error(std::format(
                "FroxelContent vol{} VOID - realNonzero={} doubledNonzero={} image={} fmt={}. "
                "A slice of zeros hashes the same as another slice of zeros. If the OTHER volume "
                "below reports data, the copy path and the declared layout are FINE and this one "
                "is simply an empty texture in the sampled region; if BOTH are empty, the copy "
                "is still not landing and the declared SHADER_READ_ONLY_OPTIMAL is the next "
                "suspect, since a wrong oldLayout may discard contents.",
                v, nonzero[0], nonzero[1],
                reinterpret_cast<void*>(froxel_slice_source[0][v]),
                static_cast<int>(froxel_pending_format[0][v])));
            continue;
        }
        ++populated_any;
        log::warn(std::format(
            "FroxelContent vol{} REAL hash={:016x} nonzero={} | DOUBLED hash={:016x} nonzero={} "
            "| differingBytes={} ({:.1f}%) | sameImage={} | {}",
            v, h[0], nonzero[0], h[1], nonzero[1], differing,
            100.0 * differing / static_cast<double>(kFroxelSliceBytes),
            froxel_slice_source[0][v] == froxel_slice_source[1][v],
            h[0] == h[1]
                ? "IDENTICAL - the doubled call samples the SAME bytes the real call did, so this "
                  "volume is NOT being consumed between the calls. Consume-once is closed here "
                  "and the loss is in whatever READS the volume."
                : "DIFFERENT - the contents CHANGED between the two calls' sample points. The "
                  "descriptors are correct and the data behind them is not: eye 1 samples a "
                  "volume cleared or rewritten since the real call read it. That is "
                  "consume-once, located, and the fix is to give the doubled call the real "
                  "call's volume - snapshot/restore, or mirror it outright."));
    }
    if (!populated_any)
        log::error("FroxelContent: NO volume was populated on both sides. The whole comparison is "
                   "VOID - do not read it as 'the volumes match'.");
}

void depth_strip_copy(VkCommandBuffer command_buffer, VkImage image, VkImageLayout layout,
                      VkExtent2D extent, uint32_t side) {
    if (image == VK_NULL_HANDLE || !indirect_readback_buffer) return;
    if (depth_strip_captured[side]) return;
    const auto& vk = vulkan_state();
    auto cmd_barrier = load_device_proc<PFN_vkCmdPipelineBarrier>(vk.device, "vkCmdPipelineBarrier");
    auto cmd_copy = load_device_proc<PFN_vkCmdCopyImageToBuffer>(
        vk.device, "vkCmdCopyImageToBuffer");
    if (!cmd_barrier || !cmd_copy) return;
    // D24S8 is 4 bytes per texel, so 65536 bytes is 16384 texels - a band a
    // few rows tall across the middle of the frame, where the sparks and the
    // canyon floor sit.
    const uint32_t rows = std::max(1u, kDepthStripBytes / (extent.width * 4u));
    const uint32_t y0 = extent.height / 2;
    VkImageMemoryBarrier to_src{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_src.oldLayout = layout;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.image = image;
    to_src.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    cmd_barrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_src);
    VkBufferImageCopy region{};
    region.bufferOffset = kDepthRegionBase + side * kDepthStripBytes;
    region.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    region.imageOffset = {0, static_cast<int32_t>(y0), 0};
    region.imageExtent = {extent.width, rows, 1};
    cmd_copy(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
             indirect_readback_buffer, 1, &region);
    // Restored to exactly the layout it was in, so the next pass's declared
    // initialLayout stays true - the discipline that keeps these copies from
    // hanging the GPU.
    VkImageMemoryBarrier back = to_src;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = layout;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    cmd_barrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &back);
    depth_strip_captured[side] = true;
    depth_strip_source[side] = image;
    depth_compare_ready.store(true, std::memory_order_release);
    log::warn(std::format(
        "DepthContent [gpu] captured {} depth image={} strip {}x{} at y={} srcLayout={}",
        side ? "MIRROR" : "REAL  ", reinterpret_cast<void*>(image), extent.width, rows, y0,
        static_cast<int>(layout)));
}

// Read at PRESENT, after the queue has run the strip copies. Called from the
// same place as emit_indirect_args_table, whose dumps drain the queue first.
void emit_depth_content_verdict() {
    if (!depth_strip_captured[0] || !depth_strip_captured[1]) {
        log::error(std::format(
            "DepthContent [gpu] INCOMPLETE realCaptured={} mirrorCaptured={} - one side never "
            "copied, so this run's comparison is void.",
            depth_strip_captured[0], depth_strip_captured[1]));
        return;
    }
    if (!indirect_readback_mapped) return;
    const uint8_t* p = static_cast<const uint8_t*>(indirect_readback_mapped);
    uint64_t h[2] = {1469598103934665603ull, 1469598103934665603ull};
    uint32_t nz[2] = {0, 0};
    for (uint32_t s = 0; s < 2; ++s) {
        const uint8_t* q = p + kDepthRegionBase + s * kDepthStripBytes;
        for (uint32_t i = 0; i < kDepthStripBytes; ++i) {
            if (q[i]) ++nz[s];
            h[s] = (h[s] ^ q[i]) * 1099511628211ull;
        }
    }
    const uint8_t* a = p + kDepthRegionBase;
    const uint8_t* b = a + kDepthStripBytes;
    uint64_t diff_bytes = 0;
    for (uint32_t i = 0; i < kDepthStripBytes; ++i) if (a[i] != b[i]) ++diff_bytes;
    // All-zero on either side means that copy never landed - not that the
    // depth is empty and not that the two match. Checked before any verdict.
    if (nz[0] == 0 || nz[1] == 0) {
        log::error(std::format(
            "DepthContent [gpu] VOID - realNonzero={} mirrorNonzero={}. An all-zero strip means "
            "that side's copy did not land. No conclusion from this run.", nz[0], nz[1]));
        return;
    }
    const double pct = 100.0 * static_cast<double>(diff_bytes) /
                       static_cast<double>(kDepthStripBytes);
    log::warn(std::format(
        "DepthContent [gpu] REAL hash={:016x} nonzero={} | MIRROR hash={:016x} nonzero={} | "
        "differingBytes={} ({:.1f}%) | {}",
        h[0], nz[0], h[1], nz[1], diff_bytes, pct,
        h[0] == h[1]
            ? "IDENTICAL - the right eye's depth is a byte-for-byte copy of the LEFT eye's. It "
              "should NOT be: the eyes see from different positions, so their depth must differ. "
              "Identical depth means eye 1's effects fade against eye 0's geometry, which is "
              "exactly a shader-side discard and would be the cause."
            : "DIFFERENT and both populated - depth is eye-specific as it should be, so this is "
              "NOT the cause."));
}

void indirect_args_flush_copies(VkCommandBuffer command_buffer) {
    if (pending_arg_copies.empty()) return;
    if (!indirect_readback_buffer) { pending_arg_copies.clear(); return; }
    const VulkanState& vk = vulkan_state();
    auto copy_buffer = vk.device
        ? load_device_proc<PFN_vkCmdCopyBuffer>(vk.device, "vkCmdCopyBuffer") : nullptr;
    if (copy_buffer) {
        for (const PendingArgCopy& p : pending_arg_copies) {
            VkBufferCopy region{};
            region.srcOffset = p.offset;
            // Three bands in one readback buffer, so there is one flush site
            // and no second one to forget: [0,kIndirectSlots) indirect args,
            // then vertex regions, then buffer descriptors.
            region.dstOffset = p.slot >= kIndirectSlots + kVertexSlots
                ? kUboRegionBase + p.side * kUboSideStride +
                      (p.slot - kIndirectSlots - kVertexSlots) * kUboSlotStride
                : (p.slot >= kIndirectSlots
                    ? kVertexRegionBase + p.side * kVertexSideStride +
                          (p.slot - kIndirectSlots) * kVertexSlotStride
                    : p.side * kIndirectSideStride + p.slot * kIndirectSlotStride);
            region.size = p.size;
            copy_buffer(command_buffer, p.buffer, indirect_readback_buffer, 1, &region);
        }
    }
    pending_arg_copies.clear();
}

// Emitted at present, once both calls have executed and the copies have
// landed. Prints REAL and DOUBLED side by side per slot.
void emit_indirect_args_table() {
    if (!indirect_readback_mapped) {
        log::error("IndirectArgs: armed but the readback buffer never came up - nothing measured");
        return;
    }
    const uint8_t* base = static_cast<const uint8_t*>(indirect_readback_mapped);
    log::warn(std::format(
        "IndirectArgs [gpu] BEGIN realCommands={} doubledCommands={} - instanceCount is read from "
        "the BUFFER by the GPU; a nonzero REAL beside a zero DOUBLED is the doubled call's draws "
        "rendering nothing, which no CPU-side draw count can see.",
        indirect_arg_count[0], indirect_arg_count[1]));
    const uint32_t rows = std::max(indirect_arg_count[0], indirect_arg_count[1]);
    for (uint32_t slot = 0; slot < rows; ++slot) {
        std::string line = std::format("IndirectArgs [gpu] slot={:2}", slot);
        for (uint32_t side = 0; side < 2; ++side) {
            const IndirectArgRecord& r = indirect_args[side][slot];
            line += std::format(" | {}", side ? "DOUBLED" : "REAL");
            if (!r.valid) { line += " (no command)"; continue; }
            const uint32_t struct_size = r.indexed ? 20u : 16u;
            const uint32_t eff = r.stride ? r.stride : struct_size;
            const uint32_t structs = std::min(r.draw_count, kIndirectStructsPerSlot);
            line += std::format(" pass={} drawCount={}", r.pass, r.draw_count);
            uint64_t total_instances = 0;
            for (uint32_t i = 0; i < structs; ++i) {
                const uint8_t* p = base + side * kIndirectSideStride + slot * kIndirectSlotStride +
                                   static_cast<size_t>(i) * eff;
                uint32_t a = 0, b = 0;
                std::memcpy(&a, p, sizeof(a));
                std::memcpy(&b, p + 4, sizeof(b));
                // indexed: {indexCount, instanceCount, ...}
                // non-indexed: {vertexCount, instanceCount, ...}
                total_instances += b;
                if (i < 3) line += std::format(" [{}:{}x{}]", i, a, b);
            }
            line += std::format(" totalInstances={}", total_instances);
        }
        log::warn(line);
    }
    log::warn("IndirectArgs [gpu] END");

    // VERTEX CONTENTS. The comparison VertexParity could not make: same
    // handle, same offset, and now the actual BYTES. Matched by (pass, buffer,
    // offset) rather than by slot index, because the two sides can record the
    // same bindings in a different order.
    log::warn(std::format(
        "VertexContent [gpu] BEGIN realBindings={} doubledBindings={} - VertexParity already "
        "proved the HANDLES and OFFSETS identical; this is the {} bytes each one actually points "
        "at. DIFFER means the doubled call draws from a region the real call has since "
        "overwritten, which is identical commands with empty output.",
        vertex_content_count[0], vertex_content_count[1], kVertexSlotStride));
    auto hash_at = [&](uint32_t side, uint32_t slot) {
        const uint8_t* p = base + kVertexRegionBase + side * kVertexSideStride +
                           slot * kVertexSlotStride;
        uint64_t h = 1469598103934665603ull;
        uint32_t nonzero = 0;
        for (uint32_t i = 0; i < kVertexSlotStride; ++i) {
            if (p[i]) ++nonzero;
            h = (h ^ p[i]) * 1099511628211ull;
        }
        return std::pair<uint64_t, uint32_t>{h, nonzero};
    };
    uint32_t matched = 0, differing = 0, unmatched = 0;
    for (uint32_t r = 0; r < vertex_content_count[0]; ++r) {
        const VertexContentRecord& rr = vertex_contents[0][r];
        if (!rr.valid) continue;
        int32_t peer = -1;
        for (uint32_t d = 0; d < vertex_content_count[1]; ++d) {
            const VertexContentRecord& dr = vertex_contents[1][d];
            // Paired on (pass, binding, offset). Handle is deliberately NOT in
            // the key - a mirrored buffer has a different handle on each side
            // and is still the same stream.
            if (dr.valid && dr.pass == rr.pass && dr.binding == rr.binding &&
                dr.offset == rr.offset) { peer = static_cast<int32_t>(d); break; }
        }
        const auto [rh, rn] = hash_at(0, r);
        if (peer < 0) {
            ++unmatched;
            log::warn(std::format(
                "VertexContent [gpu] pass={} vb{} buf={} off={} REAL hash={:016x} nonzero={}/{} | "
                "DOUBLED NEVER BOUND THIS - the two calls do not even read the same regions",
                rr.pass, rr.binding, reinterpret_cast<void*>(rr.buffer),
                static_cast<unsigned long long>(rr.offset), rh, rn, kVertexSlotStride));
            continue;
        }
        const auto [dh, dn] = hash_at(1, static_cast<uint32_t>(peer));
        const bool same = rh == dh;
        if (same) ++matched; else ++differing;
        log::warn(std::format(
            "VertexContent [gpu] pass={} vb{} REAL buf={} off={} hash={:016x} nonzero={} | "
            "DOUBLED buf={} hash={:016x} nonzero={} | {}",
            rr.pass, rr.binding, reinterpret_cast<void*>(rr.buffer),
            static_cast<unsigned long long>(rr.offset), rh, rn,
            reinterpret_cast<void*>(vertex_contents[1][peer].buffer), dh, dn,
            same ? "SAME" : "DIFFER <<<<  (vb1+ is per-particle INSTANCE data)"));
    }
    // BUFFER DESCRIPTORS. The gap: "every binding identical" was images only.
    log::warn(std::format(
        "UboContent [gpu] BEGIN realBuffers={} doubledBuffers={} - uniform/storage BUFFER "
        "descriptors, which DeferredInputMap never walked. Camera matrices, particle and fog "
        "parameters live here; wrong values make the shader discard or zero-alpha every "
        "fragment while every other measurement stays identical.",
        ubo_record_count[0], ubo_record_count[1]));
    auto ubo_hash = [&](uint32_t side, uint32_t slot) {
        const uint8_t* p = base + kUboRegionBase + side * kUboSideStride + slot * kUboSlotStride;
        uint64_t h = 1469598103934665603ull;
        uint32_t nonzero = 0;
        for (uint32_t i = 0; i < kUboSlotStride; ++i) {
            if (p[i]) ++nonzero;
            h = (h ^ p[i]) * 1099511628211ull;
        }
        return std::pair<uint64_t, uint32_t>{h, nonzero};
    };
    uint32_t usame = 0, udiff = 0, uonly = 0;
    for (uint32_t r = 0; r < ubo_record_count[0]; ++r) {
        const UboRecord& rr = ubo_records[0][r];
        if (!rr.valid) continue;
        int32_t peer = -1;
        for (uint32_t d = 0; d < ubo_record_count[1]; ++d) {
            const UboRecord& dr = ubo_records[1][d];
            // Matched by WHERE the shader reads it, not by handle: a mirrored
            // buffer would legitimately have a different handle and still be
            // the same binding.
            if (dr.valid && dr.pass == rr.pass && dr.set_index == rr.set_index &&
                dr.binding == rr.binding) { peer = static_cast<int32_t>(d); break; }
        }
        const auto [rh, rn] = ubo_hash(0, r);
        if (peer < 0) {
            ++uonly;
            log::warn(std::format(
                "UboContent [gpu] pass={} set={} b{} type={} REAL hash={:016x} nonzero={}/{} | "
                "DOUBLED NEVER BOUND THIS BINDING",
                rr.pass, rr.set_index, rr.binding, static_cast<int>(rr.type), rh, rn,
                kUboSlotStride));
            continue;
        }
        const UboRecord& dr = ubo_records[1][peer];
        const auto [dh, dn] = ubo_hash(1, static_cast<uint32_t>(peer));
        // AN ALL-ZERO READ IS NOT A DIFFERENCE - it is a copy that never
        // landed. The first run armed mid-frame, so the real call's early
        // passes were recorded but never copied, and seven all-zero REAL slots
        // were reported as "DIFFER <<<<" against real doubled data. That is a
        // probe artifact reading exactly like the finding the probe exists to
        // detect, so it is now called out by name and excluded from the tally.
        if (rn == 0 || dn == 0) {
            log::warn(std::format(
                "UboContent [gpu] pass={} set={} b{} NOT CAPTURED (real nonzero={}, doubled "
                "nonzero={}) - a side reading all zeros means its copy never executed, most "
                "likely armed mid-frame. NOT a difference; excluded from the tally.",
                rr.pass, rr.set_index, rr.binding, rn, dn));
            continue;
        }
        const bool same = rh == dh;
        if (same) ++usame; else ++udiff;
        log::warn(std::format(
            "UboContent [gpu] pass={} set={} b{} type={} REAL buf={} hash={:016x} nonzero={} | "
            "DOUBLED buf={} hash={:016x} nonzero={} | {}",
            rr.pass, rr.set_index, rr.binding, static_cast<int>(rr.type),
            reinterpret_cast<void*>(rr.buffer), rh, rn, reinterpret_cast<void*>(dr.buffer), dh,
            dn, same ? "SAME" : "DIFFER <<<<"));
    }
    log::warn(std::format(
        "UboContent [gpu] END same={} differ={} realOnly={} | {}", usame, udiff, uonly,
        (udiff || uonly)
            ? "A BUFFER DESCRIPTOR DIFFERS. Expect at least ONE legitimate difference - the view "
              "matrix, which MUST differ or there would be no parallax; it shows as same "
              "nonzero-count, different hash. A difference that is NOT that shape is the cause."
            : "every buffer descriptor is byte-identical too. Then textures, buffers, vertices, "
              "args, state and target are ALL identical, and the difference has to be outside "
              "the draw entirely - execution order or a resource written between the two calls."));

    log::warn(std::format(
        "VertexContent [gpu] END same={} differ={} realOnly={} | {}",
        matched, differing, unmatched,
        differing || unmatched
            ? "AT LEAST ONE REGION DIFFERS - the doubled call's effects draws read different "
              "bytes than the real call's, which is the cause and names the buffer."
            : "every sampled region is byte-identical. Then the draws read the same data and "
              "produce different output, and the cause is render STATE, not input data - the "
              "next thing to compare is pipeline/blend state at pass 25."));
}

// DrawParity: the indirect draw entry point that was NEVER HOOKED until
// 2026-08-18. See DrawParitySlot's declaration - if DOOM's effect lists are
// indirect, every previous probe was blind to them by construction.
void VKAPI_PTR hook_cmd_draw_indirect(VkCommandBuffer command_buffer, VkBuffer buffer,
    VkDeviceSize offset, uint32_t draw_count, uint32_t stride) {
    kharvox::native::noteCommand(command_buffer);
    FbHook fb_(FB_H_CMD_DRAW_INDIRECT);
    if (doubled_draw_hits_shared_shadow_atlas()) return;
    if (DrawParitySlot* p = draw_parity_slot()) p->indirects += draw_count;
    indirect_args_record(buffer, offset, draw_count, stride, /*indexed=*/false);
    real_cmd_draw_indirect(command_buffer, kharvox::native::bufferForEye(buffer), offset, draw_count, stride);
}

void VKAPI_PTR hook_cmd_draw_indexed_indirect(VkCommandBuffer command_buffer, VkBuffer buffer,
    VkDeviceSize offset, uint32_t draw_count, uint32_t stride) {
    kharvox::native::noteCommand(command_buffer);
    FbHook fb_(FB_H_CMD_DRAW_INDEXED_INDIRECT);
    if (doubled_draw_hits_shared_shadow_atlas()) return;
    if (DrawParitySlot* p = draw_parity_slot()) p->indirects += draw_count;
    indirect_args_record(buffer, offset, draw_count, stride, /*indexed=*/true);
    if (!world_indirect_captured_this_call && world_vertex_index_probe_wanted(command_buffer)) {
        world_indirect_captured_this_call = true;
        log::warn(std::format(
            "WorldVertexIndexProbe callKind={} eye={} pipeline={} stage=drawIndirect "
            "buf={} off={} drawCount={} stride={}",
            inside_injected_extra_call ? "extra" : "real", current_eye_render,
            reinterpret_cast<void*>(representative_world_pipeline_this_call),
            reinterpret_cast<void*>(buffer), static_cast<unsigned long long>(offset),
            draw_count, stride));
    }
    real_cmd_draw_indexed_indirect(command_buffer, kharvox::native::bufferForEye(buffer), offset, draw_count, stride);
}

// RenderDepthDrawCountProbe - see render_depth_draw_probe_samples'
// declaration above. Logs inline, live, at the call site (like
// RenderDepthPipelineProbe) - gated tightly enough (inside_render_depth &&
// real_call_redirect_armed, both false almost always) that this is not an
// always-on hot-path cost, same discipline as ScissorViewportProbe's fix
// after the earlier unguarded-hook crash (see its declaration).
void VKAPI_PTR hook_cmd_draw(VkCommandBuffer command_buffer, uint32_t vertex_count,
    uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance) {
    kharvox::native::noteCommand(command_buffer);
    FbHook fb_(FB_H_CMD_DRAW);
    if (doubled_draw_hits_shared_shadow_atlas()) return;
    if (DrawParitySlot* p = draw_parity_slot()) {
        ++p->draws;
        p->vertices += static_cast<uint64_t>(vertex_count) * instance_count;
    }
    if (inside_frame_root_double || inside_real_frame_root) {
        scene_census_draw(static_cast<uint64_t>(vertex_count) * instance_count,
                          inside_frame_root_double);
    }
    // The deferred chain's passes are single FULL-SCREEN draws (DrawParity:
    // passes 17-20 are 1 draw / 3 indices each), so this is where their
    // sampled inputs are visible.
    probe_deferred_inputs(command_buffer);
    dispatch_fate_record_draw();
    if (inside_render_depth && real_call_redirect_armed.load(std::memory_order_relaxed) &&
        render_depth_draw_probe_samples.fetch_add(1, std::memory_order_relaxed) <
            kMaxRenderDepthDrawProbeSamples) {
        log::warn(std::format(
            "RenderDepthDrawCountProbe kind=vkCmdDraw vertexCount={} instanceCount={}",
            vertex_count, instance_count));
    }
    real_cmd_draw(command_buffer, vertex_count, instance_count, first_vertex, first_instance);
}

void VKAPI_PTR hook_cmd_draw_indexed(VkCommandBuffer command_buffer, uint32_t index_count,
    uint32_t instance_count, int32_t first_index, uint32_t vertex_offset, uint32_t first_instance) {
    kharvox::native::noteCommand(command_buffer);
    FbHook fb_(FB_H_CMD_DRAW_INDEXED);
    if (doubled_draw_hits_shared_shadow_atlas()) return;
    if (DrawParitySlot* p = draw_parity_slot()) {
        ++p->draws;
        p->indices += static_cast<uint64_t>(index_count) * instance_count;
    }
    if (inside_frame_root_double || inside_real_frame_root) {
        scene_census_draw(static_cast<uint64_t>(index_count) * instance_count,
                          inside_frame_root_double);
    }
    probe_deferred_inputs(command_buffer);
    dispatch_fate_record_draw();
    // RELEVANCE TEST: is this pass-25 draw inside an occlusion query? See
    // query_open_now. Counting only, on both calls.
    if (inside_frame_root_double || inside_real_frame_root) {
        const uint32_t pass = inside_frame_root_double ? doubled_pass_seq : real_pass_seq;
        if (pass == 25) {
            const uint32_t side = inside_frame_root_double ? 1u : 0u;
            (query_open_now ? p25_draws_gated : p25_draws_ungated)[side]
                .fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (inside_render_depth && real_call_redirect_armed.load(std::memory_order_relaxed) &&
        render_depth_draw_probe_samples.fetch_add(1, std::memory_order_relaxed) <
            kMaxRenderDepthDrawProbeSamples) {
        log::warn(std::format(
            "RenderDepthDrawCountProbe kind=vkCmdDrawIndexed indexCount={} instanceCount={}",
            index_count, instance_count));
    }
    real_cmd_draw_indexed(command_buffer, index_count, instance_count, first_index, vertex_offset, first_instance);
}

// QueryPoolProbe (STEREO-IMPLEMENTATION-PLAN.md v3 follow-up, 2026-08-11):
// read-only. func_0x141893680/func_0x141894620 inside RenderOpaque's own
// dispatch loop open/close a GPU query per batch, ring-allocated mod 4096 -
// decompiled live (radare2), NOT the "descriptor cache" RE-NOTES originally
// guessed (never actually verified at the time). This logs every begin/end/
// reset seen, tagged real-vs-extra-call, so a live capture can show directly
// whether the extra call's query indices land on slots the real call already
// used THIS SAME FRAME without an intervening reset - Vulkan requires a
// reset between reuses of a query index; skipping it is undefined behaviour,
// driver-dependent, and would explain a symptom invariant to camera position
// and depth-buffer content (neither of which this mechanism touches at all).
// Learned from the crash just above: gate on inside_render_opaque (cheap,
// already proven safe) so this does nothing outside RenderOpaque, and bound
// every counter - do not repeat the unguarded-hook mistake.
constexpr uint32_t kMaxQueryPoolProbeSamplesPerCallKind = 4000;
std::atomic_uint32_t query_pool_probe_begin_real_samples{0};
std::atomic_uint32_t query_pool_probe_begin_extra_samples{0};
std::atomic_uint32_t query_pool_probe_end_real_samples{0};
std::atomic_uint32_t query_pool_probe_end_extra_samples{0};
constexpr uint32_t kMaxQueryPoolProbeResetSamples = 200;
std::atomic_uint32_t query_pool_probe_reset_samples{0};

void VKAPI_PTR hook_cmd_begin_query(VkCommandBuffer command_buffer, VkQueryPool query_pool,
    uint32_t query, VkQueryControlFlags flags) {
    kharvox::native::noteCommand(command_buffer);
    kharvox::native::checkFinalCommand(command_buffer,"hook_cmd_begin_query");
    FbHook fb_(FB_H_CMD_BEGIN_QUERY);
    // OCCLUSION QUERIES IN THE DOUBLED CALL (2026-08-19).
    //
    // Vulkan requires a query slot to be RESET before it is begun again. DOOM
    // resets its pools once per frame and then, in the real call, begins/ends
    // a query around each gated draw batch - this file already tracks that as
    // query_gate_seen_this_real_frame, so DOOM query-gating draw batches is
    // established, not assumed.
    //
    // The doubled call replays that recorded stream. Its vkCmdBeginQuery lands
    // on slots the real call has ALREADY written and which nothing has reset
    // since, which is undefined behaviour - and a driver is entitled to drop
    // or corrupt the work inside such a query. That is the only mechanism
    // found so far that discards draws while leaving every input, every piece
    // of state and every recorded command byte-identical, and it explains why
    // ONLY the query-gated batches (the effects) are lost while ordinary
    // geometry survives.
    //
    // Skipping begin/end for the doubled call leaves its draws ungated, which
    // is exactly what we want: we never read the doubled call's occlusion
    // results, and DOOM's own queries in the real call are untouched.
    if (inside_frame_root_double) {
        const uint32_t n = doubled_query_begins.fetch_add(1, std::memory_order_relaxed);
        // REDIRECT to our own pool rather than skipping. The begin/end pair is
        // preserved exactly as DOOM recorded it - only the pool and slot
        // change - so nothing can go unbalanced, which is what froze the game
        // when begin and end were suppressed independently.
        // Decide once, HERE, and record the decision for the end to consume.
        doubled_query_redirect_active = false;
        if (redirect_doubled_queries.load(std::memory_order_relaxed) &&
            private_query_pool != VK_NULL_HANDLE && doubled_query_pool_reset_this_call) {
            if (doubled_query_next >= kPrivateQuerySlots) {
                // Out of private slots: fall through to DOOM's pool rather
                // than begin a second query on a slot we already used this
                // call, which would reintroduce the very bug being fixed. The
                // flag stays false, so the matching end falls through too.
                doubled_query_overflow.fetch_add(1, std::memory_order_relaxed);
            } else {
                doubled_query_active_slot = doubled_query_next++;
                doubled_query_redirect_active = true;
                query_open_now = true;
                doubled_query_redirected.fetch_add(1, std::memory_order_relaxed);
                if (n < 3) {
                    log::warn(std::format(
                        "DoubledQuery REDIRECT: DOOM pool={} query={} -> private pool {} slot {}. "
                        "The original slot was written by the real call and never reset; ours is "
                        "reset at the start of every doubled call.",
                        reinterpret_cast<void*>(query_pool), query,
                        reinterpret_cast<void*>(private_query_pool), doubled_query_active_slot));
                }
                real_cmd_begin_query(command_buffer, private_query_pool, doubled_query_active_slot,
                                     flags);
                return;
            }
        }
    }
    if (inside_render_opaque) {
        if (!inside_injected_extra_call) {
            query_gate_seen_this_real_frame = true;
        }
        auto& counter = inside_injected_extra_call ?
            query_pool_probe_begin_extra_samples : query_pool_probe_begin_real_samples;
        const uint32_t sample = counter.fetch_add(1, std::memory_order_relaxed);
        if (sample < kMaxQueryPoolProbeSamplesPerCallKind) {
            log::warn(std::format(
                "QueryPoolProbe op=begin callKind={} eye={} pool={} query={} sample={}",
                inside_injected_extra_call ? "extra" : "real", current_eye_render,
                reinterpret_cast<void*>(query_pool), query, sample));
        }
    }
    // Relevance test: a query is now open on this thread. Set on the
    // passthrough path AND on the redirect path above, so it tracks the real
    // state regardless of which pool the query landed on.
    query_open_now = true;
    real_cmd_begin_query(command_buffer, query_pool, query, flags);
}

void VKAPI_PTR hook_cmd_end_query(VkCommandBuffer command_buffer, VkQueryPool query_pool,
    uint32_t query) {
    kharvox::native::noteCommand(command_buffer);
    kharvox::native::checkFinalCommand(command_buffer,"hook_cmd_end_query");
    FbHook fb_(FB_H_CMD_END_QUERY);
    // Must be skipped in lockstep with the begin above: ending a query that
    // was never begun is itself invalid, so suppressing one without the other
    // would trade one undefined-behaviour bug for another.
    // Counted unconditionally; skipped only while the flag is on, and the flag
    // is off by default because independent begin/end skipping unbalances the
    // pair and froze the game. See skip_doubled_queries.
    if (inside_frame_root_double) {
        doubled_query_ends.fetch_add(1, std::memory_order_relaxed);
        // Consume the begin's own decision. No flag re-read, no comparison of
        // running totals - the previous version did the latter and could end a
        // query on our pool that had been begun on DOOM's, which is invalid
        // and hangs. Clearing it here means a stray end without a begin falls
        // through untouched rather than inventing a query.
        if (doubled_query_redirect_active) {
            doubled_query_redirect_active = false;
            query_open_now = false;
            real_cmd_end_query(command_buffer, private_query_pool, doubled_query_active_slot);
            return;
        }
    }
    if (inside_render_opaque) {
        auto& counter = inside_injected_extra_call ?
            query_pool_probe_end_extra_samples : query_pool_probe_end_real_samples;
        const uint32_t sample = counter.fetch_add(1, std::memory_order_relaxed);
        if (sample < kMaxQueryPoolProbeSamplesPerCallKind) {
            log::warn(std::format(
                "QueryPoolProbe op=end callKind={} eye={} pool={} query={} sample={}",
                inside_injected_extra_call ? "extra" : "real", current_eye_render,
                reinterpret_cast<void*>(query_pool), query, sample));
        }
    }
    query_open_now = false;
    real_cmd_end_query(command_buffer, query_pool, query);
}

void VKAPI_PTR hook_cmd_reset_query_pool(VkCommandBuffer command_buffer, VkQueryPool query_pool,
    uint32_t first_query, uint32_t query_count) {
    kharvox::native::noteCommand(command_buffer);
    kharvox::native::checkFinalCommand(command_buffer,"hook_cmd_reset_query_pool");
    FbHook fb_(FB_H_CMD_RESET_QUERY_POOL);
    // Deliberately NOT gated on inside_render_opaque - static analysis (both
    // reset call sites decompiled) found this fires around once per real
    // frame, tied to a frame-in-flight rotation, not from inside RenderOpaque
    // at all. Logging it unconditionally (but bounded) is how a live capture
    // answers "does reset happen before or after the extra call's queries."
    const uint32_t sample = query_pool_probe_reset_samples.fetch_add(1, std::memory_order_relaxed);
    if (sample < kMaxQueryPoolProbeResetSamples) {
        log::warn(std::format(
            "QueryPoolProbe op=reset insideRenderOpaque={} callKind={} pool={} "
            "firstQuery={} queryCount={} sample={}",
            inside_render_opaque, inside_injected_extra_call ? "extra" : "real",
            reinterpret_cast<void*>(query_pool), first_query, query_count, sample));
    }
    real_cmd_reset_query_pool(command_buffer, query_pool, first_query, query_count);
}

// ScissorViewportProbe (docs/STEREO-RESTART-PLAN.md Task 3) - see
// scissor_captured_this_call's declaration above for the full reasoning.
// Gated tightly on inside_render_opaque (and NOT inside RenderDepth's
// reinvocation) so this only ever fires during RenderOpaque's own draws,
// not thousands of times/second across the whole game - the mistake that
// crashed the earlier, unguarded version of this hook.
void VKAPI_PTR hook_cmd_set_scissor(VkCommandBuffer command_buffer, uint32_t first_scissor,
    uint32_t scissor_count, const VkRect2D* scissors) {
    kharvox::native::noteCommand(command_buffer);
    // Manual pair, not RAII: this frame uses __try (C2712). Single exit
    // path below, so no return can bypass fb_hook_exit.
    const uint64_t fb_t0 = fb_hook_enter(FB_H_CMD_SET_SCISSOR);
    if (inside_render_opaque && !inside_extra_render_depth_reinvoke &&
        !scissor_captured_this_call && scissor_count > 0 && scissors) {
        __try {
            captured_scissor = scissors[0];
            scissor_captured_this_call = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    // PassStateParity: an empty scissor discards a draw with no counter
    // anywhere noticing, which is the shape of this bug. Delegated to a helper
    // because this function uses __try, and MSVC forbids that in a frame that
    // also needs object unwinding (C2712) - which std::format does.
    if (scissors && scissor_count > 0) pass_state_record_scissor(scissors[0], scissor_count);
    real_cmd_set_scissor(command_buffer, first_scissor, scissor_count, scissors);
    fb_hook_exit(FB_H_CMD_SET_SCISSOR, fb_t0);
}

void VKAPI_PTR hook_cmd_set_viewport(VkCommandBuffer command_buffer, uint32_t first_viewport,
    uint32_t viewport_count, const VkViewport* viewports) {
    kharvox::native::noteCommand(command_buffer);
    // Manual pair, not RAII: __try (C2712). Single exit path below.
    const uint64_t fb_t0 = fb_hook_enter(FB_H_CMD_SET_VIEWPORT);
    if (inside_render_opaque && !inside_extra_render_depth_reinvoke &&
        !viewport_captured_this_call && viewport_count > 0 && viewports) {
        __try {
            captured_viewport = viewports[0];
            viewport_captured_this_call = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    // Helper, for the same C2712 reason as the scissor hook above.
    if (viewports && viewport_count > 0) pass_state_record_viewport(viewports[0], viewport_count);
    real_cmd_set_viewport(command_buffer, first_viewport, viewport_count, viewports);
    fb_hook_exit(FB_H_CMD_SET_VIEWPORT, fb_t0);
}

// Phase 0 (STEREO-IMPLEMENTATION-PLAN.md v2): scissor/viewport during
// RenderOpaque - REMOVED 2026-08-11 after a live crash during plain
// gameplay (not even during the stereo mechanism). These four hooks
// (vkCmdSetScissor/vkCmdSetViewport/vkCmdDrawIndexed/vkCmdDrawIndexedIndirect)
// are called thousands of times per second across the WHOLE game, not just
// RenderOpaque - a categorically hotter path than anything else this
// project has ever hooked, and unlike every other risky operation in this
// file they had zero SEH protection. The crash's timing (doomvr.log simply
// stopped mid-stream, right after a newly-seen pipeline+draw-kind pair got
// logged for the first time) is consistent with a bug in this code, though
// not proven beyond doubt. Not needed anymore anyway: the one successful
// DrawKindProbe/ScissorProbe/ViewportProbe sample each already answered
// Phase 0's question (plain, not indirect, draws; full-extent scissor/
// viewport on the real call) before the crash. vkCreateGraphicsPipelines +
// hook_cmd_bind_pipeline's depth/stencil logging (below) are kept - lower
// frequency (pipeline creation is a startup burst; pipeline BINDS happen
// far less often than draws), and ran stably for ~30s across 14 distinct
// pipelines before the crash, unlike this block.
#endif  // DOOMVR_RESEARCH_TOOLS

void log_handle(const char* label, const void* handle) {
    char text[128];
    std::snprintf(text, sizeof(text), "%s=%p", label, handle);
    log::info(text);
}

VkResult VKAPI_PTR hook_create_instance(const VkInstanceCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkInstance* instance) {
    FbHook fb_(FB_H_CREATE_INSTANCE);
    const VkResult result = real_create_instance(info, allocator, instance);
    if (result == VK_SUCCESS && instance) {
        vulkan_state().instance = *instance;
        log_handle("captured VkInstance", reinterpret_cast<void*>(*instance));
    }
    return result;
}

VkResult VKAPI_PTR hook_enumerate_physical_devices(VkInstance instance,
    uint32_t* count, VkPhysicalDevice* devices) {
    FbHook fb_(FB_H_ENUMERATE_PHYSICAL_DEVICES);
    const VkResult result = real_enumerate_physical_devices(instance, count, devices);
    if ((result == VK_SUCCESS || result == VK_INCOMPLETE) && devices && count && *count) {
        vulkan_state().instance = instance;
        vulkan_state().physical_device = devices[0];
        log_handle("captured VkPhysicalDevice", reinterpret_cast<void*>(devices[0]));
    }
    return result;
}

VkResult VKAPI_PTR hook_create_device(VkPhysicalDevice physical_device,
    const VkDeviceCreateInfo* info, const VkAllocationCallbacks* allocator, VkDevice* device) {
    FbHook fb_(FB_H_CREATE_DEVICE);
    // DOOM'S OWN DEVICE REQUEST, logged for the record (2026-08-20).
    //
    // ⚠ THIS CANNOT SEE WHAT RENDERDOC CHANGES, and it was nearly used for
    // exactly that. We MinHook the EXPORTED vkCreateDevice in vulkan-1.dll,
    // which is ABOVE the loader's layer chain - so this observes DOOM's
    // original call and will read identically with and without any layer
    // loaded. Diffing this line between a RenderDoc run and a plain one would
    // have burned a run to prove nothing.
    //
    // The same holds for every other hook in this file: from our vantage the
    // two configurations are byte-identical. Whatever RenderDoc changes, it
    // changes BELOW us - driver-side memory aliasing, compression, or timing -
    // which is why the difference has to be found by testing mechanisms in
    // our own code or by comparing rendered OUTPUT, never by watching our own
    // inputs.
    if (info) {
        std::string exts;
        for (uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
            if (i) exts += ", ";
            exts += info->ppEnabledExtensionNames[i];
        }
        uint32_t feature_count = 0;
        if (info->pEnabledFeatures) {
            const auto* f = reinterpret_cast<const VkBool32*>(info->pEnabledFeatures);
            for (size_t i = 0; i < sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32); ++i)
                if (f[i]) ++feature_count;
        }
        log::warn(std::format(
            "LayerFootprint vkCreateDevice: queueCreateInfoCount={} enabledExtensionCount={} "
            "featuresEnabled={} pNextChain={} extensions=[{}]",
            info->queueCreateInfoCount, info->enabledExtensionCount, feature_count,
            info->pNext ? "present" : "null", exts));
    }
    const VkResult result = real_create_device(physical_device, info, allocator, device);
    if (result == VK_SUCCESS && device) {
        vulkan_state().physical_device = physical_device;
        vulkan_state().device = *device;
        log_handle("captured VkDevice", reinterpret_cast<void*>(*device));
    }
    return result;
}

void VKAPI_PTR hook_get_device_queue(VkDevice device, uint32_t family, uint32_t index, VkQueue* queue) {
    FbHook fb_(FB_H_GET_DEVICE_QUEUE);
    real_get_device_queue(device, family, index, queue);
    if (queue && *queue) {
        auto& state = vulkan_state();
        state.device = device;
        state.queue = *queue;
        state.queue_family = family;
        state.queue_index = index;
        char text[160];
        std::snprintf(text, sizeof(text), "captured VkQueue=%p family=%u index=%u",
            reinterpret_cast<void*>(*queue), family, index);
        log::info(text);
    }
}

VkResult VKAPI_PTR hook_queue_submit(VkQueue queue, uint32_t count,
    const VkSubmitInfo* submits, VkFence fence) {
    // CONTAINMENT event 1. A candidate on the stack HERE contains the submit,
    // and re-invoking it double-submits - which is what crashed 0x186E520.
    containment_sample(1);
    // ===== FREQHIGH SUBMIT-TIME LAYER (see the FhTracked block) ============
    // Runs BEFORE the real submit, so the bytes examined are the state the
    // GPU is being handed. Gated on the census being armed; <=8 records, so
    // the cost is a few compares per frame. Writes happen ONLY under slot 1.
    if (wrap::config_fhcensus() &&
        g_fh_tracked_n.load(std::memory_order_relaxed) > 0) {
        // LEAF mutex only - NEVER swapchains_mutex here: the mod's own
        // present-time submits re-enter this hook while holding it.
        std::scoped_lock fh_lock(g_fh_tracked_mutex);
        alignas(16) float cw[16];
        float ref[3];
        const bool have_ref =
            wrap::clean_world_copy(cw) && fh::solve_cam_origin(cw, ref);
        float hy = 0.0f, hp = 0.0f, hr = 0.0f;
        const bool have_head = wrap::residual_head_angles(hy, hp, hr);
        float cr[3], cu[3], cf[3];
        const bool have_basis = wrap::reference_basis(cr, cu, cf);
        const uint32_t tn =
            (std::min)(g_fh_tracked_n.load(std::memory_order_relaxed), kFhTrackedMax);
        for (uint32_t ti = 0; ti < tn; ++ti) {
            FhTracked& t = g_fh_tracked[ti];
            if (!t.host || (t.range != 160 && t.range != 128 &&
                            t.range != 208 && t.range != 496)) continue;
            g_fh_sub_checks.fetch_add(1, std::memory_order_relaxed);
            const float* cur = reinterpret_cast<const float*>(t.host);
            // Only the first 160 bytes are windowed (the mvp lives at +16 in
            // every layout except rigid-128's +0; the 208/496 tails are not
            // diffed).
            const uint32_t nfl = (std::min)(t.range, 160u) / 4;
            const uint32_t mvp0 = (t.range == 128) ? 0u : 4u;
            const uint32_t mdl0 = (t.range == 160) ? 20u
                                 : (t.range == 128) ? 16u : 0u;
            const uint32_t mdl1 = mdl0 ? mdl0 + 12u : 0u;
            bool mvp_diff = false, mdl_diff = false, oth_diff = false;
            for (uint32_t i = 0; i < nfl; ++i) {
                if (cur[i] == t.last_seen[i]) continue;
                if (i >= mvp0 && i < mvp0 + 16) mvp_diff = true;
                else if (i >= mdl0 && i < mdl1) mdl_diff = true;
                else oth_diff = true;
            }
            if (mvp_diff) g_fh_sub_mvp_changed.fetch_add(1, std::memory_order_relaxed);
            if (mdl_diff) g_fh_sub_model_changed.fetch_add(1, std::memory_order_relaxed);
            if (oth_diff) g_fh_sub_other_changed.fetch_add(1, std::memory_order_relaxed);
            // Did the engine overwrite a ctrl2 write from an earlier submit?
            if (t.ctrl2_written &&
                std::memcmp(cur + mvp0, t.our_mvp, 64) != 0) {
                g_fh_ctrl2_clobbered.fetch_add(1, std::memory_order_relaxed);
                t.ctrl2_written = false;
            }
            std::memcpy(t.last_seen, cur, nfl * 4);
            // ===== SIBLING records (208 prepass / 496 forward variant) =====
            // No parse/census - they carry the same mvp as a forward record
            // (gun9 Q2: bit-identical) plus fields our Record does not model.
            // Correction only: mvp' = C * mvp, the SAME operator as the
            // forward records, so depth and color stay byte-coherent.
            if (t.sibling) {
                const bool ours_sib = t.ctrl2_written &&
                    std::memcmp(cur + mvp0, t.our_mvp, 64) == 0;
                if (wrap::config_gunfix2_worldp() && exp_on(2) && !ours_sib) {
                    const fh::Slab sl = fh::depth_slab(cur + mvp0);
                    if (!(sl.ok && std::fabs(sl.a - 0.1f) < 0.02f)) {
                        g_gf2_pre_unmatched.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        float C[16], mvp2[16];
                        if (!fh_read_c(t.side, C)) {
                            g_gf2_no_wvp.fetch_add(1, std::memory_order_relaxed);
                        } else {
                            wrap::mat4_multiply(C, cur + mvp0, mvp2);
                            bool okf = true;
                            for (int i = 0; okf && i < 16; ++i)
                                okf = std::isfinite(mvp2[i]);
                            if (!okf) {
                                g_gf2_refused.fetch_add(1, std::memory_order_relaxed);
                            } else {
                                std::memcpy(t.host + mvp0 * 4, mvp2, 64);
                                std::memcpy(t.our_mvp, mvp2, 64);
                                t.ctrl2_written = true;
                                std::memcpy(t.last_seen + mvp0, mvp2, 64);
                                g_gf2_pre_applied.fetch_add(1, std::memory_order_relaxed);
                                const fh::Slab ps = fh::depth_slab(mvp2);
                                ((ps.ok && std::fabs(ps.a - 1.0f) < 0.1f)
                                     ? g_gf2_post_ok
                                     : g_gf2_post_bad)
                                    .fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                    }
                }
                continue;
            }
            if (!have_ref) continue;
            fh::Record r;
            if (!fh::parse(cur, t.range, r)) {
                g_fh_sub_reparse_fail.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            const fh::Verdict v = fh::validate(r, ref, 100.0f);
            // Skip records currently carrying OUR ctrl2 mvp: their facing is
            // the control's, not the engine's, and must not enter the census.
            const bool ours =
                t.ctrl2_written && std::memcmp(cur + mvp0, t.our_mvp, 64) == 0;
            if (!fh::is_viewmodel(v)) {
                if (!ours)
                    g_fh_sub_reparse_fail.fetch_add(1, std::memory_order_relaxed);
            } else if (!ours && mvp_diff) {
                // Sampled ONLY when the mvp JUST changed under us: these are
                // exactly the late writer's outputs, never a blend of stale
                // and fresh states across submits. n=0 with mvpChanged=0 is
                // the meaningful "no late write before submit" result, not a
                // dead instrument - the diff counters carry that verdict.
                // FHSUBDUMP - offline cross-check corpus for the FINAL bytes.
                if (have_head && have_basis &&
                    g_fh_sub_dumped.load(std::memory_order_relaxed) < 8) {
                    wrap::VpSnapshot gvp;
                    if (wrap::world_vp(gvp) && wrap::is_gameplay_view(gvp.m) &&
                        g_fh_sub_dumped.fetch_add(1, std::memory_order_relaxed) < 8) {
                        float xf[3] = {0, 0, 0}, mf[3];
                        fh::vp_forward(v.X, xf);
                        fh::model_forward(r.model, mf);
                        std::string raw;
                        for (uint32_t i = 0; i < nfl; ++i)
                            raw += std::format("{}{:.6f}", i ? " " : "", cur[i]);
                        log::warn(std::format(
                            "FHSUBDUMP kind={} side={} which={} eyeDist={:.2f} "
                            "vpYaw={:.4f} mYaw={:.4f} headYaw={:.4f} raw=[{}]",
                            t.range, t.side ? "DBL" : "REAL",
                            t.which ? "gun" : "arms", v.eye_dist,
                            fh::signed_yaw(xf, cr, cf),
                            fh::signed_yaw(mf, cr, cf), hy, raw));
                    }
                }
                // SUBMIT-state census, same convention as the bind census.
                if (have_head && have_basis && std::fabs(hy) > 0.0873f) {
                    float xf[3], mf[3];
                    if (fh::vp_forward(v.X, xf))
                        fh_slope_note(g_fh_sub_slope[t.side][t.which][0],
                                      fh::signed_yaw(xf, cr, cf), hy);
                    fh::model_forward(r.model, mf);
                    fh_slope_note(g_fh_sub_slope[t.side][t.which][1],
                                  fh::signed_yaw(mf, cr, cf), hy);
                }
            }
            // ===== GUNFIX2 - the world-projection substitution =============
            // mvp' = VP_world[side] * model. The record's own weapon
            // projection (1.5-1.9x the world's angular scale - the oversized
            // gun AND its faster-than-walls slide, one cause) is replaced by
            // the same pass's world view-projection, latched from the
            // identity-model world records. Model untouched: animations,
            // placement and lighting stay the engine's.
            const bool gf2 = wrap::config_gunfix2_worldp() && exp_on(2);
            if (gf2 && fh::is_viewmodel(v) && !ours && t.range == 160) {
                float C[16], mvp2[16];
                if (!fh_read_c(t.side, C)) {
                    g_gf2_no_wvp.fetch_add(1, std::memory_order_relaxed);
                } else {
                    // mvp' = C * mvp = P_world * inv(P_weapon) * mvp - the
                    // projection swap; identical operator and input bytes as
                    // the prepass siblings, so depth stays glued to color.
                    wrap::mat4_multiply(C, cur + mvp0, mvp2);
                    bool ok = true;
                    for (int i = 0; ok && i < 16; ++i)
                        ok = std::isfinite(mvp2[i]);
                    if (!ok) {
                        g_gf2_refused.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        std::memcpy(t.host + mvp0 * 4, mvp2, 64);
                        std::memcpy(t.our_mvp, mvp2, 64);
                        t.ctrl2_written = true;  // "ours" marker, shared
                        std::memcpy(t.last_seen + mvp0, mvp2, 64);
                        g_gf2_applied.fetch_add(1, std::memory_order_relaxed);
                        // POST: the swapped mvp must now carry the WORLD's
                        // depth slab (a~1.0) - a real structural check, not a
                        // read-back of our own memcpy.
                        {
                            const fh::Slab ps = fh::depth_slab(mvp2);
                            ((ps.ok && std::fabs(ps.a - 1.0f) < 0.1f)
                                 ? g_gf2_post_ok
                                 : g_gf2_post_bad)
                                .fetch_add(1, std::memory_order_relaxed);
                        }
                        // FHFIXDUMP - first 8 corrected records, gameplay-
                        // gated, for the offline cross-check corpus.
                        wrap::VpSnapshot gvp;
                        if (g_gf2_dumped.load(std::memory_order_relaxed) < 8 &&
                            wrap::world_vp(gvp) &&
                            wrap::is_gameplay_view(gvp.m) &&
                            g_gf2_dumped.fetch_add(1, std::memory_order_relaxed) < 8) {
                            std::string raw;
                            for (uint32_t i = 0; i < 16; ++i)
                                raw += std::format("{}{:.6f}", i ? " " : "",
                                                   mvp2[i]);
                            log::warn(std::format(
                                "FHFIXDUMP side={} which={} eyeDist={:.2f} "
                                "mvp2=[{}]",
                                t.side ? "DBL" : "REAL",
                                t.which ? "gun" : "arms", v.eye_dist, raw));
                        }
                    }
                }
            }
            // ===== FHCTRL2: RETIRED (2026-08-28 night) =====================
            // Its verdicts are banked (18:17 + the 19:06 capture pair proved
            // consumption and pass coverage). Slot 1 is now the SSFIX kill
            // switch, so this write is hard-disconnected - a reassigned slot
            // must never re-arm a retired write site.
        }
    }
    const uint64_t t0 = fb_ns();
    const VkResult r = real_queue_submit(queue, count, submits, fence);
    fb_submit_ns.fetch_add(fb_ns() - t0, std::memory_order_relaxed);
    fb_submit_calls.fetch_add(1, std::memory_order_relaxed);
    fb_clock_reads.fetch_add(2, std::memory_order_relaxed);
    return r;
}

// Pass-through. The ONLY thing either of these does is time the real call -
// no argument is inspected, nothing is substituted, no state is touched. If
// either ever does more than this, it stops being a measurement.
VkResult VKAPI_PTR hook_acquire_next_image(VkDevice device, VkSwapchainKHR swapchain,
    uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t* image_index) {
    const uint64_t t0 = fb_ns();
    const VkResult r =
        real_acquire_next_image(device, swapchain, timeout, semaphore, fence, image_index);
    fb_acquire_ns.fetch_add(fb_ns() - t0, std::memory_order_relaxed);
    fb_acquire_calls.fetch_add(1, std::memory_order_relaxed);
    fb_clock_reads.fetch_add(2, std::memory_order_relaxed);
    return r;
}

VkResult VKAPI_PTR hook_wait_for_fences(VkDevice device, uint32_t fence_count,
    const VkFence* fences, VkBool32 wait_all, uint64_t timeout) {
    const uint64_t t0 = fb_ns();
    const VkResult r = real_wait_for_fences(device, fence_count, fences, wait_all, timeout);
    fb_fence_ns.fetch_add(fb_ns() - t0, std::memory_order_relaxed);
    fb_fence_calls.fetch_add(1, std::memory_order_relaxed);
    fb_clock_reads.fetch_add(2, std::memory_order_relaxed);
    return r;
}

VkResult VKAPI_PTR hook_create_swapchain(VkDevice device,
    const VkSwapchainCreateInfoKHR* info, const VkAllocationCallbacks* allocator,
    VkSwapchainKHR* swapchain) {
    FbHook fb_(FB_H_CREATE_SWAPCHAIN);
    const VkResult result = real_create_swapchain(device, info, allocator, swapchain);
    if (result == VK_SUCCESS && info && swapchain) {
        std::unique_lock lock(swapchains_mutex);
        auto& state = swapchains[*swapchain];
        state.format = info->imageFormat;
        state.extent = info->imageExtent;
        // WARN, not INFO, and it now carries the HANDLE (2026-08-24). A
        // cutscene-driven swapchain recreate is signature (c) of
        // docs/PLAN-2026-08-24-CUTSCENE-VR-EXIT.md §3.3 - resource churn at a
        // scene transition, with no XR state change and no exception - and it
        // is unreadable without the handle: "created" and "destroyed" only
        // pair up into a recreate if you can see they name the SAME object.
        // These fire a handful of times per run, not per frame, so they are
        // uncapped. There is no rate limit to mistake for a null.
        log::warn(std::format(
            "SWAPCHAIN CREATED handle={:#x} format={} extent={}x{} minImageCount={} (the "
            "REQUESTED count - the actual images arrive via vkGetSwapchainImagesKHR). If this "
            "appears between two CUTSCENE MARKER lines with no 'XR SESSION STATE' transition "
            "and no FrameRootDouble exception, that is SIGNATURE (c): scene-transition resource "
            "churn. Mirrors and framebuffers keyed on the OLD images are not obviously "
            "re-derived - see the carried 'hooks stop participating at scene transitions' item.",
            reinterpret_cast<uintptr_t>(*swapchain), static_cast<int>(state.format),
            state.extent.width, state.extent.height, info->minImageCount));
    }
    return result;
}

VkResult VKAPI_PTR hook_get_swapchain_images(VkDevice device, VkSwapchainKHR swapchain,
    uint32_t* count, VkImage* images) {
    FbHook fb_(FB_H_GET_SWAPCHAIN_IMAGES);
    const VkResult result = real_get_swapchain_images(device, swapchain, count, images);
    if ((result == VK_SUCCESS || result == VK_INCOMPLETE) && count && images) {
        std::unique_lock lock(swapchains_mutex);
        auto& state = swapchains[swapchain];
        state.images.assign(images, images + *count);
        char text[128];
        std::snprintf(text, sizeof(text), "captured %u DOOM swapchain images", *count);
        log::info(text);
    }
    return result;
}

void VKAPI_PTR hook_destroy_swapchain(VkDevice device, VkSwapchainKHR swapchain,
    const VkAllocationCallbacks* allocator) {
    FbHook fb_(FB_H_DESTROY_SWAPCHAIN);
    {
        std::unique_lock lock(swapchains_mutex);
        // THIS ERASE WAS COMPLETELY SILENT until 2026-08-24, which is half of
        // why signature (c) has never been observed: the record vanished and
        // nothing said so. Read the details BEFORE erasing - after the erase
        // there is nothing left to print, and a line that says only "some
        // swapchain was destroyed" cannot be paired with its recreate.
        const auto it = swapchains.find(swapchain);
        if (it != swapchains.end()) {
            log::warn(std::format(
                "SWAPCHAIN DESTROYED handle={:#x} format={} extent={}x{} imagesRecorded={}. "
                "Everything keyed on those images - mirrors, framebuffers - now refers to "
                "objects that no longer exist.",
                reinterpret_cast<uintptr_t>(swapchain), static_cast<int>(it->second.format),
                it->second.extent.width, it->second.extent.height, it->second.images.size()));
        } else {
            log::warn(std::format(
                "SWAPCHAIN DESTROYED handle={:#x} - NO RECORD HELD for it. Either it was created "
                "before our hooks were installed, or it was already erased.",
                reinterpret_cast<uintptr_t>(swapchain)));
        }
        swapchains.erase(swapchain);
    }
    real_destroy_swapchain(device, swapchain, allocator);
}

#ifdef DOOMVR_RESEARCH_TOOLS
void VKAPI_PTR hook_cmd_push_constants(VkCommandBuffer command_buffer,
    VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t offset,
    uint32_t size, const void* values) {
    kharvox::native::noteCommand(command_buffer);
    uint32_t sample = push_constant_samples.load();
    if (values && size >= 64 && sample < 32 &&
        push_constant_samples.compare_exchange_strong(sample, sample + 1)) {
        const float* f = static_cast<const float*>(values);
        char text[512];
        std::snprintf(text, sizeof(text),
            "push constants sample=%u stages=0x%x offset=%u size=%u first16="
            "[%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | "
            "%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]",
            sample, static_cast<unsigned>(stages), offset, size,
            f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7],
            f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
        log::info(text);
    }
    real_cmd_push_constants(command_buffer, layout, stages, offset, size, values);
}

VkResult VKAPI_PTR hook_create_buffer(VkDevice device, const VkBufferCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkBuffer* buffer) {
    FbHook fb_(FB_H_CREATE_BUFFER);
    // See renderdoc_parity_usage_enabled() above hook_create_image. RenderDoc widens
    // buffer usage the same way it widens image usage; the particle and
    // indirect-args buffers the effects passes read are buffers, so covering
    // only images would test half the hypothesis.
    VkResult result;
    if (renderdoc_parity_usage_enabled() && info) {
        VkBufferCreateInfo patched = *info;
        patched.usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        result = real_create_buffer(device, &patched, allocator, buffer);
        if (result != VK_SUCCESS) {
            static std::atomic_uint32_t refused{0};
            if (refused.fetch_add(1, std::memory_order_relaxed) < 5) {
                log::error(std::format(
                    "RenderDocParity: vkCreateBuffer REFUSED the widened usage (usage=0x{:x} -> "
                    "0x{:x}, result={}). Falling back to DOOM's own usage, so this buffer is NOT "
                    "covered by the test.",
                    static_cast<unsigned>(info->usage), static_cast<unsigned>(patched.usage),
                    static_cast<int>(result)));
            }
            kharvox::native::fail("required buffer transfer usage unsupported");
        }
    } else {
        result = real_create_buffer(device, info, allocator, buffer);
    }
    if (result == VK_SUCCESS && info && buffer) {
        std::unique_lock lock(swapchains_mutex);
        buffers[*buffer] = {info->size, info->usage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        kharvox::native::indexSourceBuffer(*buffer, (info->usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) != 0);
        if ((info->usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) && uniform_samples.fetch_add(1) < 40) {
            char text[160];
            std::snprintf(text, sizeof(text), "uniform buffer created handle=%p size=%llu usage=0x%x",
                reinterpret_cast<void*>(*buffer), static_cast<unsigned long long>(info->size),
                static_cast<unsigned>(info->usage));
            log::info(text);
        }
    }
    return result;
}

VkResult VKAPI_PTR hook_bind_buffer_memory(VkDevice device, VkBuffer buffer,
    VkDeviceMemory memory, VkDeviceSize offset) {
    FbHook fb_(FB_H_BIND_BUFFER_MEMORY);
    const VkResult result = real_bind_buffer_memory(device, buffer, memory, offset);
    if (result == VK_SUCCESS) {
        std::unique_lock lock(swapchains_mutex);
        buffer_bindings[buffer] = {memory, offset};
        const auto found = buffers.find(buffer);
        if (found != buffers.end() && (found->second.usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) {
            static std::atomic_uint32_t bind_samples{0};
            if (bind_samples.fetch_add(1) >= 40) return result;
            char text[192];
            std::snprintf(text, sizeof(text), "uniform buffer bound buffer=%p memory=%p offset=%llu size=%llu",
                reinterpret_cast<void*>(buffer), reinterpret_cast<void*>(memory),
                static_cast<unsigned long long>(offset),
                static_cast<unsigned long long>(found->second.size));
            log::info(text);
        }
    }
    return result;
}

VkResult VKAPI_PTR hook_map_memory(VkDevice device, VkDeviceMemory memory,
    VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void** data) {
    FbHook fb_(FB_H_MAP_MEMORY);
    const VkResult result = real_map_memory(device, memory, offset, size, flags, data);
    static std::atomic_uint32_t map_samples{0};
    if (result == VK_SUCCESS && data && map_samples.fetch_add(1) < 40) {
        char text[192];
        std::snprintf(text, sizeof(text), "device memory mapped memory=%p offset=%llu size=%llu host=%p",
            reinterpret_cast<void*>(memory), static_cast<unsigned long long>(offset),
            static_cast<unsigned long long>(size), *data);
        log::info(text);
    }
    if (result == VK_SUCCESS && data) {
        std::unique_lock lock(swapchains_mutex);
        memory_mappings[memory] = {static_cast<unsigned char*>(*data), offset, size};
    }
    return result;
}

void VKAPI_PTR hook_unmap_memory(VkDevice device, VkDeviceMemory memory) {
    FbHook fb_(FB_H_UNMAP_MEMORY);
    // Remove the host pointer before Vulkan can invalidate it. The same mutex is
    // used by the snapshot path, so an unmap cannot race a 64-byte copy.
    {
        std::unique_lock lock(swapchains_mutex);
        memory_mappings.erase(memory);
    }
    real_unmap_memory(device, memory);
}

void VKAPI_PTR hook_free_memory(VkDevice device, VkDeviceMemory memory,
    const VkAllocationCallbacks* allocator) {
    FbHook fb_(FB_H_FREE_MEMORY);
    kharvox::native::freeNativeMemory(device,memory,allocator,real_free_memory);
}

// ReadRedirect state (STEP 6). Declared here rather than beside the
// substitution logic near hook_cmd_bind_descriptor_sets because the
// allocate/update hooks that MAINTAIN it are defined above that point.
std::mutex read_redirect_mutex;
std::unordered_map<VkDescriptorSet, VkDescriptorSetLayout> descriptor_set_layouts;
// STEP 6.5 follow-up (2026-08-17). noRecord went 0 -> 5 after 6.7d started
// clearing content records at allocation, and two sets on the deferred chain
// are now refused as "no recorded contents". Their mirrorBacked=0 is
// guaranteed by construction - there is nothing to walk - so it says nothing
// about what they carry, and a count cannot close this.
//
// These two make it answerable: if a set's last update PRECEDES its last
// allocation, DOOM bound a handle it had not written since we cleared it,
// and the clear is reaching further than intended. If the update is later,
// the record is missing for some other reason and the clear is exonerated.
std::unordered_map<VkDescriptorSet, uint64_t> descriptor_set_alloc_frame;
std::unordered_map<VkDescriptorSet, uint64_t> descriptor_set_update_frame;
// A clone plus the mirror generation its substitution decisions were made
// at. When the generation moves - a new mirror, or one becoming written for
// the first time - the decisions may now be different (more bindings safe to
// substitute), so the clone is freed and rebuilt.
struct RedirectedSet {
    VkDescriptorSet clone = VK_NULL_HANDLE;
    uint32_t generation = 0;
    // FROXEL FIX - see kFixDoubledLightingVolume. The froxel volume this clone
    // was built against. DOOM alternates two volumes (temporal reprojection),
    // so a clone cached across a swap would bind the wrong one; storing it
    // makes the swap a rebuild trigger instead of a silent staleness.
    VkImageView volume_used = VK_NULL_HANDLE;
};
std::unordered_map<VkDescriptorSet, RedirectedSet> redirected_sets;
// CLONE AUDIT - READ THE LIVE CLONES, DO NOT CENSUS NEW ONES (2026-08-22).
//
// CloneDrop and VolumeSubst both reported zero and both were uninformative,
// for a reason the code states outright beside p1_clone_views: "clones are
// cached per (set, mirrorGeneration) and are usually built BEFORE the armed
// frame". Those censuses fire when a clone is BUILT. The clones actually in
// use at pass 8 were built earlier, so neither census ever saw them.
//
// This audits the clones that EXIST, by walking redirected_sets (source ->
// clone) and comparing, per pair:
//
//   asked3D  - 3D volume views in DOOM's own source set
//   clone3D  - 3D volume views actually written into the clone, read straight
//              out of p1_clone_views, which records every written view
//              unfiltered and post-substitution
//
// asked3D > 0 with clone3D == 0 is the pass-8 defect, named and localised to a
// specific source set and clone, with no inference left in it. asked3D ==
// clone3D everywhere means the clones DO carry the volumes and P1's bound-side
// classification is what is wrong - which is equally worth knowing and would
// stop me hunting a bug that is not there.
void emit_clone_audit() {
    uint32_t pairs = 0, with_asked_3d = 0, lost_3d = 0, ok_3d = 0, no_record = 0;
    std::string lines;
    {
        std::scoped_lock lk(read_redirect_mutex);
        std::scoped_lock lk2(p1_clone_mutex);
        std::unique_lock lk3(swapchains_mutex);
        for (const auto& [src, rs] : redirected_sets) {
            if (rs.clone == VK_NULL_HANDLE) continue;
            ++pairs;
            uint32_t asked3d = 0;
            if (auto imgs = descriptor_image_sets.find(src); imgs != descriptor_image_sets.end()) {
                for (const auto& [b, rec] : imgs->second) {
                    (void)b;
                    if (auto vi = image_view_infos.find(rec.image_view);
                        vi != image_view_infos.end() &&
                        vi->second.view_type == VK_IMAGE_VIEW_TYPE_3D) ++asked3d;
                }
            }
            if (!asked3d) continue;
            ++with_asked_3d;
            uint32_t clone3d = 0;
            const auto cv = p1_clone_views.find(rs.clone);
            if (cv == p1_clone_views.end()) { ++no_record; continue; }
            for (VkImageView v : cv->second) {
                if (auto vi = image_view_infos.find(v);
                    vi != image_view_infos.end() &&
                    vi->second.view_type == VK_IMAGE_VIEW_TYPE_3D) ++clone3d;
            }
            if (clone3d) ++ok_3d; else ++lost_3d;
            if (lines.size() < 1200)
                lines += std::format("\n  src={} clone={} asked3D={} clone3D={} cloneViews={}{}",
                                     reinterpret_cast<void*>(src),
                                     reinterpret_cast<void*>(rs.clone), asked3d, clone3d,
                                     cv->second.size(),
                                     clone3d ? "" : "   <<<< VOLUMES LOST IN THIS CLONE");
        }
    }
    log::warn(std::format(
        "CloneAudit: {} live clones, {} whose source set asks for a 3D volume | carried={} "
        "LOST={} noCloneViewRecord={}.{}\n"
        "⚠ CONTROLS: pairs=0 means redirected_sets is empty and nothing was audited - VOID, not "
        "clean. withAsked3D=0 means no redirected set samples a volume at all, so the clone "
        "cannot be where the volumes go missing and the pass-5 ASKED divergence is the whole "
        "story. noCloneViewRecord>0 means those clones predate p1_clone_views and their contents "
        "are unknown rather than empty.",
        pairs, with_asked_3d, ok_3d, lost_3d, no_record,
        lines.empty() ? std::string(" (no volume-carrying clones)") : lines));
}
// Sets checked and found to need no substitution, with the mirror
// generation the check was made at. Cached so the scan is paid once per set
// rather than once per bind (one pass alone binds 132 times), but re-checked
// when mirrors change, because "nothing to redirect" is just as
// generation-dependent as a clone: a binding can become substitutable once
// its mirror exists and has been written.
std::unordered_map<VkDescriptorSet, uint32_t> sets_needing_no_redirect;
// Sets carrying at least one descriptor type we cannot reproduce from our
// records. Cloning MUST refuse these - see the untracked-type guard in
// hook_update_descriptor_sets.
std::unordered_set<VkDescriptorSet> sets_with_untracked_bindings;
VkDescriptorPool read_redirect_pool = VK_NULL_HANDLE;
bool read_redirect_pool_failed = false;

// ============ CLONE POOL LEAK FIX (2026-08-24) ==============================
//
// THE DEFECT, MEASURED ACROSS TWO RUNS (23:04 and 21:09).
//
// The pool is maxSets=1024. SIX sites drop a cached clone; only ONE freed it
// (the stale_clone path in get_redirected_set). The other five erased the map
// entry and left the VkDescriptorSet allocated. invalidate_read_redirect_cache
// said so in its own comment and justified it as "bounded by the number of
// distinct sets DOOM uses".
//
// That premise is false, and this run measured it: DOOM allocates ONE set per
// vkAllocateDescriptorSets call, ~1200 per 6-second window, and
// staleBufCleared ~= allocSets - i.e. it RECYCLES handles continuously. The
// number of distinct handles over time is unbounded, so every clone dropped by
// one of those five sites burned a pool slot permanently.
//
// What that produced, identically in both runs:
//   clones FREEZES        (1195 at 23:07:21; 1434 at 21:11:04) - allocation now
//                         fails, so no further clone is ever built;
//   fallbacks JUMPS       (+110; +274) while noRecords contributes only +2/+3,
//                         because the failing site prints no SetRefusedID line
//                         and its ERROR shares a 10-line budget spent minutes
//                         earlier;
//   substitutedBinds FREEZES permanently - every set takes
//                         sets_needing_no_redirect[src] = generation, and with
//                         mirror_generation static nothing ever re-evaluates.
//   => eye 1 stops being redirected and both eyes show the same picture.
//
// ⚠ WHY THE FREE IS DEFERRED AND NOT IMMEDIATE. vkFreeDescriptorSets on a set
// still referenced by a command buffer in flight is UNDEFINED BEHAVIOUR - a
// device-lost hang, not a glitch. These clones were bound by the doubled call
// on frames the GPU may not have finished. So a dropped clone is RETIRED with
// the frame number it was dropped on, and freed only once the present counter
// has moved kCloneRetireFrames beyond it. At the measured rate (~10 clones/s)
// the queue holds single digits, so this costs nothing and cannot itself
// exhaust the pool.
//
// The pool size is DELIBERATELY LEFT AT 1024. Raising it would mask the fix
// and make the run un-judgeable; the whole point is that the ceiling stops
// being reached.
VkDevice read_redirect_device = VK_NULL_HANDLE;   // captured when the pool is created
struct RetiredClone {
    VkDescriptorSet clone{VK_NULL_HANDLE};
    uint64_t frame{0};
};
std::vector<RetiredClone> retired_clones;   // guarded by read_redirect_mutex, frame-ascending
// The counters and kCloneRetireFrames live in the early block beside
// read_redirect_clones - the frame-root audit line is compiled before this
// point and reads them. rr_clone_alloc_failures is THE FALSIFIER: previously
// this failure fed only the shared read_redirect_failures, which is why 271 of
// the 21:09 run's 274 collapse-window refusals had no attributable reason.

// Call with read_redirect_mutex HELD. Retires the clone rather than freeing it
// here: no Vulkan call is made under the lock (Rule 9, the run-17 lesson).
void retire_clone(VkDescriptorSet clone) {
    if (clone == VK_NULL_HANDLE) return;
    retired_clones.push_back(
        RetiredClone{clone, presented_frame.load(std::memory_order_relaxed)});
    rr_clones_retired.fetch_add(1, std::memory_order_relaxed);
    const uint32_t sz = static_cast<uint32_t>(retired_clones.size());
    rr_retire_queue_depth.store(sz, std::memory_order_relaxed);
    uint32_t prev = rr_retire_queue_peak.load(std::memory_order_relaxed);
    while (sz > prev &&
           !rr_retire_queue_peak.compare_exchange_weak(prev, sz, std::memory_order_relaxed)) {}
}

// Call with read_redirect_mutex HELD. The one-line replacement for every bare
// redirected_sets.erase() - dropping the entry without this is the leak.
void drop_redirected_set(VkDescriptorSet src) {
    if (auto it = redirected_sets.find(src); it != redirected_sets.end()) {
        retire_clone(it->second.clone);
        redirected_sets.erase(it);
    }
}

// Call with NO lock held. Frees everything retired at least kCloneRetireFrames
// presents ago. The queue is frame-ascending, so this is a prefix take.
void drain_retired_clones() {
    return; // KHARVOX owns completion-based reclamation; elapsed presents prove nothing.
    if (read_redirect_pool == VK_NULL_HANDLE || read_redirect_device == VK_NULL_HANDLE) return;
    const uint64_t now = presented_frame.load(std::memory_order_relaxed);
    std::vector<VkDescriptorSet> ready;
    {
        std::scoped_lock lock(read_redirect_mutex);
        size_t n = 0;
        while (n < retired_clones.size() &&
               now - retired_clones[n].frame >= kCloneRetireFrames) {
            ready.push_back(retired_clones[n].clone);
            ++n;
        }
        if (n == 0) return;
        retired_clones.erase(retired_clones.begin(), retired_clones.begin() + n);
        rr_retire_queue_depth.store(static_cast<uint32_t>(retired_clones.size()),
                                    std::memory_order_relaxed);
    }
    if (auto free_sets =
            load_device_proc<PFN_vkFreeDescriptorSets>(read_redirect_device, "vkFreeDescriptorSets")) {
        free_sets(read_redirect_device, read_redirect_pool,
                  static_cast<uint32_t>(ready.size()), ready.data());
        rr_clones_freed.fetch_add(ready.size(), std::memory_order_relaxed);
    }
}
// ============ END CLONE POOL LEAK FIX =======================================

// BARRIER SUBSTITUTION (2026-08-17, the STEP 6 hang fix).
//
// During the doubled call, rewrite every image barrier that names an image
// we hold a mirror for so it names the MIRROR instead. Two things this
// fixes, one of which we knew about and one we did not:
//
//  1. The hang. DOOM's barriers transition its own G-buffer to
//     SHADER_READ_ONLY_OPTIMAL before the deferred pass samples it. Our
//     mirrors got no such transition, so the STEP 6 clone bound a mirror
//     view declaring a layout the mirror image was not in - undefined
//     behaviour, and it hung the GPU on frame one.
//  2. Contamination nobody had looked at. Until now the doubled call issued
//     DOOM's layout transitions against DOOM'S OWN REAL IMAGES, a second
//     time per frame, out of sequence with DOOM's own pass order. Mirroring
//     was supposed to make the doubled call touch nothing DOOM owns and it
//     never covered this. Substituting rather than duplicating the barrier
//     means the doubled call now leaves DOOM's images entirely alone. This
//     is a candidate mechanism for the frame-root residual (claim 9), which
//     has never had one - do NOT treat that as established, but it is the
//     first concrete thing to test against it.
//
// Buffer and global memory barriers pass through untouched: we mirror no
// buffers, so there is nothing to substitute.
void VKAPI_PTR hook_cmd_pipeline_barrier(VkCommandBuffer command_buffer,
    VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage,
    VkDependencyFlags flags, uint32_t memory_count, const VkMemoryBarrier* memories,
    uint32_t buffer_count, const VkBufferMemoryBarrier* buffers,
    uint32_t image_count, const VkImageMemoryBarrier* images) {
    kharvox::native::noteCommand(command_buffer);
    kharvox::native::checkFinalCommand(command_buffer,"hook_cmd_pipeline_barrier");
    FbHook fb_(FB_H_CMD_PIPELINE_BARRIER);
    std::vector<VkBufferMemoryBarrier> ownerBuffers;
    if((inside_frame_root_double||inside_final_pass_double)&&buffer_count&&buffers){
        ownerBuffers.assign(buffers,buffers+buffer_count);
        for(uint32_t i=0;i<buffer_count;i++){auto b=buffers[i];auto mirrored=kharvox::native::bufferForEye(b.buffer);if(mirrored!=b.buffer){b.buffer=mirrored;ownerBuffers.push_back(b);}}
        buffer_count=uint32_t(ownerBuffers.size());buffers=ownerBuffers.data();
    }

    // Doubled FINAL PASS too (2026-08-18) - otherwise the second invocation
    // transitions DOOM's real images a second time, out of sequence, which is
    // the contamination this substitution exists to prevent.
    // DOOM's own image layouts, recorded for EVERY barrier in every scope and
    // before any substitution below rewrites the array. This is the ground
    // truth the rewind restores mirrors to; our own mirror tracking cannot
    // serve as it, because by the time the doubled call starts it has already
    // followed the real call through the entire frame root.
    if (image_count && images && mirror_enabled.load(std::memory_order_relaxed)) {
        std::scoped_lock lock(mirror_mutex);
        for (uint32_t i = 0; i < image_count; ++i)
            doom_image_layouts[images[i].image] = images[i].newLayout;
    }
    if ((inside_frame_root_double || inside_final_pass_double) &&
        mirror_enabled.load(std::memory_order_relaxed) && image_count && images) {
        std::vector<VkImageMemoryBarrier> rewritten;
        // BARFIX: parallel barriers for our mirrors, emitted ALONGSIDE DOOM's
        // own rather than in place of them.
        std::vector<VkImageMemoryBarrier> appended;
        uint32_t substituted = 0;
        {
            std::scoped_lock lock(mirror_mutex);
            for (uint32_t i = 0; i < image_count; ++i) {
                if (mirrored_images.find(images[i].image) != mirrored_images.end()) {
                    if (rewritten.empty()) rewritten.assign(images, images + image_count);
                    const VkImage mirror = mirrored_images[images[i].image];
                    // ===== BARFIX (2026-08-31 night) - APPEND, DO NOT STEAL ==
                    //
                    // THE MEASUREMENT THAT POINTS HERE. The STEPLADDER bisect
                    // put the three defects at scripted-test STEP 2 exactly -
                    // clean at step 0 and step 1, all three the moment step 2
                    // armed live doubling. BarrierSubstitute #0 fires 17 ms
                    // later, and it re-points only SOME barriers per call:
                    // "2 of 4", "1 of 4", "2 of 2".
                    //
                    // Each re-pointed barrier is a layout transition DOOM'S OWN
                    // IMAGE NEVER RECEIVES. DOOM asks for 4->5
                    // (COLOR_ATTACHMENT -> SHADER_READ) on its image; we hand
                    // that transition to our mirror and leave DOOM's image in
                    // layout 4 for whatever samples it next.
                    //
                    // Why every other lever nulled: BARRIERS ARE EMITTED
                    // WHETHER OR NOT ANY JOB DRAWS. Suppressing all 13 jobs,
                    // killing the doubled final pass, both together, and the
                    // frame rate could not touch this by construction.
                    //
                    // THE FIX IS THIS FILE'S OWN RULE, applied where it was
                    // not. Outside the doubled call it already says: "APPEND,
                    // never substitute, here ... Adding a parallel barrier for
                    // our mirror leaves DOOM's untouched and keeps the mirror's
                    // layout in lockstep." Inside the doubled call it steals
                    // instead. barfix makes both paths agree: DOOM's barrier is
                    // left alone and the mirror gets its own, appended.
                    if (g_barfix_on.load(std::memory_order_relaxed)) {
                        VkImageMemoryBarrier extra = images[i];
                        extra.image = mirror;
                        const VkImageLayout mk = mirror_layout_of(mirror);
                        if (mk != images[i].oldLayout &&
                            images[i].oldLayout != VK_IMAGE_LAYOUT_UNDEFINED)
                            extra.oldLayout = mk;
                        append_mirror_transitions(appended,extra,images[i].oldLayout==VK_IMAGE_LAYOUT_UNDEFINED);
                        g_barfix_appended.fetch_add(1, std::memory_order_relaxed);
                        continue;   // DOOM's own barrier stays untouched
                    }
                    rewritten[i].image = mirror;
                    // The barrier's oldLayout describes DOOM's image, not
                    // ours. Ours is wherever we last tracked it to, and
                    // naming the wrong oldLayout is exactly the class of bug
                    // this whole block exists to fix. UNDEFINED would be
                    // legal but discards contents, so only use it when we
                    // genuinely have no record.
                    const VkImageLayout known = mirror_layout_of(mirror);
                    if (known != images[i].oldLayout &&
                        images[i].oldLayout != VK_IMAGE_LAYOUT_UNDEFINED)
                        rewritten[i].oldLayout = known;
                    mirror_image_layouts[mirror] = images[i].newLayout;
                    ++substituted;
                }
            }
        }
        // BARFIX emission: DOOM's ORIGINAL array, unmodified, plus our
        // mirrors' parallel barriers. One call, so the ordering DOOM relies on
        // between its own barrier and the work either side of it is unchanged.
        if (!appended.empty()) {
            std::vector<VkImageMemoryBarrier> all(images, images + image_count);
            all.insert(all.end(), appended.begin(), appended.end());
            const uint32_t n = g_barfix_calls.fetch_add(1, std::memory_order_relaxed);
            if (n < 8) {
                log::warn(std::format(
                    "BARFIX #{}: {} mirror barrier(s) APPENDED alongside DOOM's {} - DOOM's "
                    "own images keep every transition. (first: image {} stays, mirror {} "
                    "added, oldLayout={} newLayout={})",
                    n, appended.size(), image_count,
                    reinterpret_cast<void*>(images[0].image),
                    reinterpret_cast<void*>(appended[0].image),
                    static_cast<int>(images[0].oldLayout),
                    static_cast<int>(images[0].newLayout)));
            }
            real_cmd_pipeline_barrier(command_buffer, src_stage, dst_stage, flags,
                memory_count, memories, buffer_count, buffers,
                static_cast<uint32_t>(all.size()), all.data());
            return;
        }
        if (substituted) {
            barrier_image_substitutions.fetch_add(substituted, std::memory_order_relaxed);
            const uint32_t n = barrier_calls_rewritten.fetch_add(1, std::memory_order_relaxed);
            if (n < 20) {
                log::warn(std::format(
                    "BarrierSubstitute #{}: {} of {} image barrier(s) re-pointed at mirrors "
                    "(first: image {} -> {}, oldLayout={} newLayout={})",
                    n, substituted, image_count,
                    reinterpret_cast<void*>(images[0].image),
                    reinterpret_cast<void*>(rewritten[0].image),
                    static_cast<int>(images[0].oldLayout),
                    static_cast<int>(images[0].newLayout)));
            }
            real_cmd_pipeline_barrier(command_buffer, src_stage, dst_stage, flags,
                memory_count, memories, buffer_count, buffers, image_count, rewritten.data());
            return;
        }
    }
    // MIRROR LAYOUT LOCKSTEP (2026-08-18). The reason the grade landed on the
    // wrong eye.
    //
    // The doubled final pass refused to substitute its composite read with
    // reason=layout-mismatch have=2 want=5: our mirror was still in
    // COLOR_ATTACHMENT_OPTIMAL, because that is how the doubled frame root
    // left it, while the descriptor asks for SHADER_READ_ONLY_OPTIMAL. DOOM
    // makes exactly that transition on its OWN composite - but BETWEEN the
    // frame root and the final pass, which is outside both scopes where
    // barriers are substituted. So the mirror never followed, the gate
    // refused, the pass read DOOM's real composite, and the output was eye
    // 0's viewpoint with a perfect grade on it.
    //
    // APPEND, never substitute, here. Outside the doubled call DOOM's barriers
    // are DOOM's own business: re-pointing one would transition our image
    // instead of the one DOOM needs and break the frame the owner is looking
    // at. Adding a parallel barrier for our mirror leaves DOOM's untouched and
    // keeps the mirror's layout in lockstep, which fixes the whole class of
    // drift rather than this one instance.
    if (!inside_frame_root_double && !inside_final_pass_double && image_count && images &&
        mirror_enabled.load(std::memory_order_relaxed) &&
        final_pass_double_armed.load(std::memory_order_relaxed)) {
        std::vector<VkImageMemoryBarrier> extra;
        {
            std::scoped_lock lock(mirror_mutex);
            for (uint32_t i = 0; i < image_count; ++i) {
                auto it = mirrored_images.find(images[i].image);
                if (it == mirrored_images.end()) continue;
                const VkImage mirror = it->second;
                const VkImageLayout known = mirror_layout_of(mirror);
                if (known == images[i].newLayout) continue;  // already there
                VkImageMemoryBarrier b = images[i];
                b.image = mirror;
                b.oldLayout = known;  // OUR layout, not DOOM's
                append_mirror_transitions(extra,b);
            }
        }
        if (!extra.empty()) {
            const uint32_t n = mirror_lockstep_barriers.fetch_add(
                static_cast<uint32_t>(extra.size()), std::memory_order_relaxed);
            if (n < 12) {
                log::warn(std::format(
                    "MirrorLockstep: {} mirror(s) followed DOOM's transition (first: mirror {} "
                    "{} -> {}). Without this the mirror stays in the layout the doubled frame "
                    "root left it in and every sampled read of it is refused.",
                    extra.size(), reinterpret_cast<void*>(extra[0].image),
                    static_cast<int>(extra[0].oldLayout),
                    static_cast<int>(extra[0].newLayout)));
            }
            real_cmd_pipeline_barrier(command_buffer, src_stage, dst_stage, flags, 0, nullptr,
                0, nullptr, static_cast<uint32_t>(extra.size()), extra.data());
        }
    }
    real_cmd_pipeline_barrier(command_buffer, src_stage, dst_stage, flags,
        memory_count, memories, buffer_count, buffers, image_count, images);
}

// ReadRedirect needs each set's layout to allocate a clone of it. Recorded
// here because there is no other way to recover it from a VkDescriptorSet.
VkResult VKAPI_PTR hook_allocate_descriptor_sets(VkDevice device,
    const VkDescriptorSetAllocateInfo* info, VkDescriptorSet* sets) {
    FbHook fb_(FB_H_ALLOCATE_DESCRIPTOR_SETS);
    const VkResult result = real_allocate_descriptor_sets(device, info, sets);
    if (result == VK_SUCCESS && info && sets && info->pSetLayouts) {
        kharvox::native::recordDescriptorPoolAllocation(info->descriptorPool,info->descriptorSetCount,sets);
        {
            std::scoped_lock lock(read_redirect_mutex);
            const uint64_t now = presented_frame.load(std::memory_order_relaxed);
            for (uint32_t i = 0; i < info->descriptorSetCount; ++i) {
                descriptor_set_layouts[sets[i]] = info->pSetLayouts[i];
                descriptor_set_alloc_frame[sets[i]] = now;
                // A recycled set handle must not inherit an old clone, an old
                // no-redirect mark, or an old untracked-type mark.
                drop_redirected_set(sets[i]);
                sets_needing_no_redirect.erase(sets[i]);
                sets_with_untracked_bindings.erase(sets[i]);
            }
        }
        // STEP 6.7d (2026-08-17, from run 20's localisation).
        //
        // ...and it must not inherit its previous incarnation's CONTENTS
        // either. descriptor_sets / descriptor_image_sets were never pruned,
        // so a handle that carried a uniform buffer at binding N before, and
        // carries a sampled image at binding N now, appears in BOTH maps.
        // get_redirected_set correctly refuses to clone such a set - writing
        // two descriptors of different types to one binding is invalid usage
        // and a plausible GPU hang - but the refusal is ALL-OR-NOTHING, so
        // every mirror-backed read in that set falls back to DOOM's real,
        // unshifted image.
        //
        // Run 20 measured exactly what that costs. The chain walk showed the
        // G-buffer at pass 18 strongly shifted (near/far MAD 4.1x, weapon
        // crossing the screen) and the composite at pass 64 geometrically
        // identical - so the shift dies in between. And every full-extent
        // pass of the post chain (35, 38, 39, 64) refuses its set 0 for
        // precisely this reason, each throwing away 2-5 full-screen
        // mirror-backed reads. Pass 35's discarded set includes binding 4
        // over the G-BUFFER image itself: the first post pass re-reads
        // DOOM's unshifted G-buffer, and everything after it is built on
        // that.
        //
        // The record is what is wrong, not the check. A freshly allocated
        // handle has no contents until something writes them, so this is
        // also simply correct bookkeeping.
        //
        // Separate scope, not nested under read_redirect_mutex: these two
        // locks are only ever taken sequentially elsewhere (get_redirected_
        // set), and nesting them here would invent a lock order the rest of
        // the file does not observe.
        // ===== THE CUTSCENE FIX (2026-08-24) =============================
        //
        // MEASURED, not theorised. When VR collapses to mono at a cutscene:
        //   substitutedBinds FREEZES (1075197, unchanged for the rest of the
        //     run) while attempt keeps climbing;
        //   the refusal reason is "no recorded contents for this set";
        //   staleRecordsCleared climbs 14940 -> 16722 through exactly that
        //     window - this erase, firing on recycled handles;
        //   templateSets = templateMisses = 0, so update templates are not
        //     involved at all and that whole hypothesis is dead;
        //   typeConflict = 0, so the defect this erase was written to prevent
        //     is not occurring.
        //
        // The chain: DOOM reallocates descriptor sets across the scene
        // transition, vkAllocateDescriptorSets hands back RECYCLED handles,
        // this erase drops their content records - and DOOM then binds those
        // sets without a write we observe, so the records never come back.
        // Eye 1 stops being redirected and reads DOOM's unshifted images, so
        // both eyes render the same picture. It is permanent because nothing
        // can reconstruct the contents: Vulkan has no API to read a descriptor
        // set back, which is also why no keypress could ever recover it.
        //
        // THE TRADE, STATED HONESTLY. Keeping the record risks describing a
        // recycled handle by its previous life. That is what the erase was
        // added to prevent - a binding appearing as both an image and a buffer,
        // refusing the whole set. But that defect has its OWN counter and it
        // reads ZERO in this run, while the erase is measurably costing every
        // redirected read after a transition. So the image record is KEPT and
        // the buffer record is still dropped: the two disagreeing is precisely
        // what produced the type conflict, and keeping only one cannot.
        //
        // Reversible without a rebuild, because I have been wrong about this
        // three times today: create C:\dev\doomvr-drop-set-records.txt to
        // restore the old always-erase behaviour and A/B it.
        // ⚠ REVERTED TO DROP-BY-DEFAULT (2026-08-24, same day).
        //
        // Keeping the records was TRIED AND IT IS WRONG. The run came back with
        // green artifacts across eye 1 and no stereo separation at all, from
        // the opening sequence onward - which is precisely what a retained
        // record does: it describes the handle's PREVIOUS life, so redirection
        // substitutes images that have nothing to do with the current set and
        // eye 1 samples garbage.
        //
        // So the erase is CORRECT, and typeConflict=0 did not mean the hazard
        // was absent - it meant that particular counter was not the one that
        // would have caught it. That is the third time today I have taken a
        // zero counter as permission; the artifact was the real control.
        //
        // The measured facts survive the retraction and still need explaining:
        // substitutedBinds freezes permanently at the cutscene, the reason is
        // "no recorded contents", and staleRecordsCleared climbs in the same
        // window. What is now ruled out is fixing it by keeping stale data.
        // The records must be REPOPULATED, which means finding the write path
        // that fills these sets after realloc - it is not templates
        // (templateSets=0) and it is not push descriptors (the export is
        // absent), so it is a path we are not yet seeing.
        static const bool drop_records = [] {
            FILE* f = nullptr;
            if (fopen_s(&f, "C:\\dev\\doomvr-keep-set-records.txt", "r") == 0 && f) {
                std::fclose(f);
                log::warn("SetRecords: KEEP-on-realloc enabled by switch file. ⚠ This produced "
                          "green artifacts in eye 1 and no separation on 2026-08-24 - it is the "
                          "known-bad arm, kept only so the experiment can be repeated.");
                return false;
            }
            return true;   // DEFAULT: drop, the known-good behaviour
        }();
        uint32_t cleared = 0;
        // Ring + rate bookkeeping only - no descriptor behaviour changes here.
        desc_alloc_calls.fetch_add(1, std::memory_order_relaxed);
        desc_alloc_sets.fetch_add(info->descriptorSetCount, std::memory_order_relaxed);
        {
            std::unique_lock lock(swapchains_mutex);
            for (uint32_t i = 0; i < info->descriptorSetCount; ++i) {
                // ALLOC is stamped here rather than in the read_redirect_mutex
                // scope above so that every ring event lives under ONE lock.
                // Same call, same handle, same frame.
                desc_ring_push(sets[i], kDescRingAlloc, 0);
                // Buffer-side record always goes: it is the half that made a
                // binding look like two different types at once.
                const uint32_t buf_gone = static_cast<uint32_t>(descriptor_sets.erase(sets[i]));
                cleared += buf_gone;
                uint32_t img_gone = 0;
                if (drop_records) {
                    img_gone = static_cast<uint32_t>(descriptor_image_sets.erase(sets[i]));
                    cleared += img_gone;
                } else {
                    set_records_retained.fetch_add(
                        static_cast<uint32_t>(descriptor_image_sets.count(sets[i])),
                        std::memory_order_relaxed);
                }
                stale_img_records_cleared.fetch_add(img_gone, std::memory_order_relaxed);
                stale_buf_records_cleared.fetch_add(buf_gone, std::memory_order_relaxed);
                desc_ring_push(sets[i], kDescRingErase,
                               static_cast<uint16_t>((img_gone ? 1 : 0) | (buf_gone ? 2 : 0)));
            }
        }
        if (cleared) {
            const uint32_t n = stale_set_records_cleared.fetch_add(cleared,
                                                                   std::memory_order_relaxed);
            if (n < 10) {
                log::warn(std::format(
                    "StaleSetRecords: vkAllocateDescriptorSets recycled {} handle(s) that still "
                    "carried {} stale content record(s); dropped. Left in place these are what "
                    "made a binding appear as both an image and a buffer, refusing the whole set "
                    "and costing every mirror-backed read in it.",
                    info->descriptorSetCount, cleared));
            }
        }
    }
    return result;
}

void VKAPI_PTR hook_update_descriptor_sets(VkDevice device, uint32_t write_count,
    const VkWriteDescriptorSet* writes, uint32_t copy_count,
    const VkCopyDescriptorSet* copies) {
    FbHook fb_(FB_H_UPDATE_DESCRIPTOR_SETS);
    static std::atomic_uint32_t descriptor_samples{0};
    // Rate counters (plan §2 iii). updCalls is the discriminator for the third
    // row of the plan's table: if this delta goes to zero across the collapse
    // while allocCalls carries on, the loader-export route went quiet.
    desc_update_calls.fetch_add(1, std::memory_order_relaxed);
    if (copy_count) desc_copy_calls.fetch_add(1, std::memory_order_relaxed);
    // ReadRedirect cache invalidation. A clone is a SNAPSHOT of the source
    // set's contents; the moment DOOM rewrites the source, the clone is
    // stale and would bind last frame's textures. Drop it and let the next
    // bind rebuild. Also drops the no-redirect mark: a set that sampled
    // nothing we mirror may well sample a mirrored view after a rewrite.
    if (writes && write_count) {
        std::scoped_lock lock(read_redirect_mutex);
        const uint64_t now = presented_frame.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < write_count; ++i) {
            drop_redirected_set(writes[i].dstSet);
            sets_needing_no_redirect.erase(writes[i].dstSet);
            descriptor_set_update_frame[writes[i].dstSet] = now;
        }
    }
    if (copies && copy_count) {
        const uint64_t now = presented_frame.load(std::memory_order_relaxed);
        {
            std::scoped_lock lock(read_redirect_mutex);
            for (uint32_t i = 0; i < copy_count; ++i) {
                drop_redirected_set(copies[i].dstSet);
                sets_needing_no_redirect.erase(copies[i].dstSet);
                // Review §4: a set populated purely by COPY was previously
                // never stamped, so it looked eternally un-updated -
                // producing run 26's allocFrame=668 updateFrame=355
                // boundWithoutUpdateSinceAlloc=true signature with no record.
                descriptor_set_update_frame[copies[i].dstSet] = now;
                // Safety inherits across a copy. If the SOURCE holds a
                // descriptor type we cannot reproduce, a clone of the
                // destination would leave it undefined - a device-lost hang,
                // not a glitch - so the refusal mark travels with the copy.
                if (sets_with_untracked_bindings.count(copies[i].srcSet))
                    sets_with_untracked_bindings.insert(copies[i].dstSet);
            }
        }
        // CONTENT PROPAGATION. A copy moves real contents from srcSet to
        // dstSet; without mirroring that here, dstSet keeps whatever stale
        // record it had (or none) and ReadRedirect either refuses the whole
        // set or clones the wrong thing. Taken in its own scope with only
        // swapchains_mutex held - never nested under read_redirect_mutex,
        // which is the lock-ordering discipline Rule 9 was written for after
        // run 17 wedged the render thread.
        //
        // Copies are per-binding ranges, so each descriptor index is walked
        // individually rather than assuming binding == binding.
        {
            std::unique_lock lock(swapchains_mutex);
            for (uint32_t i = 0; i < copy_count; ++i) {
                const auto& c = copies[i];
                descriptor_copies_seen.fetch_add(1, std::memory_order_relaxed);
                desc_ring_push(c.dstSet, kDescRingCopyDst,
                               static_cast<uint16_t>(c.descriptorCount));
                for (uint32_t e = 0; e < c.descriptorCount; ++e) {
                    const uint32_t src_b = c.srcBinding + e;
                    const uint32_t dst_b = c.dstBinding + e;
                    if (auto s = descriptor_sets.find(c.srcSet); s != descriptor_sets.end()) {
                        if (auto b = s->second.find(src_b); b != s->second.end()) {
                            descriptor_sets[c.dstSet][dst_b] = b->second;
                            descriptor_copy_records_propagated.fetch_add(
                                1, std::memory_order_relaxed);
                        }
                    }
                    if (auto s = descriptor_image_sets.find(c.srcSet);
                        s != descriptor_image_sets.end()) {
                        if (auto b = s->second.find(src_b); b != s->second.end()) {
                            descriptor_image_sets[c.dstSet][dst_b] = b->second;
                            descriptor_copy_image_records_propagated.fetch_add(
                                1, std::memory_order_relaxed);
                        }
                    }
                }
            }
        }
    }
    for (uint32_t i = 0; writes && i < write_count; ++i) {
        const auto& write = writes[i];
        // ===== WRITE-SHAPE TAP (plan §2 iv) =====
        //
        // Placed at the TOP of the loop body, ahead of every record branch and
        // ahead of every lock, so it sees writes that BOTH branches reject -
        // which is the entire point. Reads nothing but the write itself.
        //
        // POLARITY, before the run: if the ring for a refused handle shows an
        // ALLOC with no *_WRITE after it while updCalls stays high, these lines
        // name the field responsible. hits=0 with the same ring means DOOM
        // never wrote the set again at all, which is a different answer.
        if (is_recent_refused_set(write.dstSet)) {
            shape_tap_hits.fetch_add(1, std::memory_order_relaxed);
            uint32_t budget = shape_tap_budget.load(std::memory_order_relaxed);
            while (budget > 0 && !shape_tap_budget.compare_exchange_weak(
                                     budget, budget - 1, std::memory_order_relaxed)) {}
            if (budget > 0) {
                const uint32_t n = shape_tap_lines.fetch_add(1, std::memory_order_relaxed);
                log::warn(std::format(
                    "WriteShape #{} set={} dstBinding={} dstArrayElement={} type={} "
                    "descriptorCount={} pImageInfo={} pBufferInfo={} pTexelBufferView={} "
                    "pNext={} frame={} (a write that lands on a handle a whole-set refusal "
                    "already named; descriptorCount=0 or a null info pointer means BOTH record "
                    "branches decline it and the set stays contentless to us)",
                    n, reinterpret_cast<const void*>(write.dstSet), write.dstBinding,
                    write.dstArrayElement, static_cast<int>(write.descriptorType),
                    write.descriptorCount, write.pImageInfo ? "set" : "NULL",
                    write.pBufferInfo ? "set" : "NULL",
                    write.pTexelBufferView ? "set" : "NULL", write.pNext ? "non-null" : "null",
                    presented_frame.load(std::memory_order_relaxed)));
            }
        }
        // STORAGE_BUFFER added 2026-08-17: ReadRedirect reproduces a set from
        // these records, so any buffer type it does NOT record would be left
        // undefined in the clone. See the untracked-type guard below.
        if ((write.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
             write.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
             write.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
             write.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) &&
            write.pBufferInfo && write.descriptorCount) {
            {
                std::unique_lock lock(swapchains_mutex);
                const auto& b = write.pBufferInfo[0];
                descriptor_sets[write.dstSet][write.dstBinding] = {
                    b.buffer, b.offset, b.range, write.descriptorType};
                desc_ring_push(write.dstSet, kDescRingBufWrite,
                               static_cast<uint16_t>(write.dstBinding));
            }
            desc_update_buf_records.fetch_add(1, std::memory_order_relaxed);
            uint32_t sample = descriptor_samples.load(std::memory_order_relaxed);
            if (sample < 40) {
                sample = descriptor_samples.fetch_add(1);
            }
            if (sample < 40) {
                const auto& b = write.pBufferInfo[0];
                char text[224];
                std::snprintf(text, sizeof(text),
                    "uniform descriptor sample=%u type=%d binding=%u buffer=%p offset=%llu range=%llu",
                    sample, static_cast<int>(write.descriptorType), write.dstBinding,
                    reinterpret_cast<void*>(b.buffer), static_cast<unsigned long long>(b.offset),
                    static_cast<unsigned long long>(b.range));
                log::info(text);
            }
        }
        // WorldPipelineBindingProbe - see ImageDescriptorBinding's
        // declaration above. Read-only, no sampling/logging here (unlike the
        // buffer path above) - this just keeps descriptor_image_sets current
        // so hook_cmd_bind_descriptor_sets can look up what's actually bound
        // when the representative world-geometry pipeline draws.
        const bool image_type =
            write.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
            write.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
            write.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
            write.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
            write.descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        if (image_type && write.pImageInfo && write.descriptorCount) {
            std::unique_lock lock(swapchains_mutex);
            const auto& img = write.pImageInfo[0];
            ImageDescriptorBinding rec{img.imageView, img.sampler, write.descriptorType};
            rec.count = write.descriptorCount;
            for (uint32_t e = 0; e < write.descriptorCount && e < rec.views.size(); ++e)
                rec.views[e] = write.pImageInfo[e].imageView;
            rec.layout = img.imageLayout;
            // VOLUME RESTORE (2026-08-22). The intervention the evidence points at.
            //
            // CloneAudit: 81 live clones, ZERO whose source set asks for a 3D
            // volume. Volume-carrying sets are never redirected, so our clone
            // is not where they go missing. What P1 shows instead is an ASKED
            // divergence - at pass 5 the real call's sets bind 2 froxel volumes
            // and the doubled call's bind none. DOOM itself declines to bind
            // them on re-invocation, which P1W confirmed at this very write
            // site.
            //
            // So the repair belongs here, at the write, not in the clone: while
            // inside the doubled call, if DOOM writes a NON-3D view into a
            // binding where the real call of this frame wrote a 3D volume,
            // put the real call's volume back.
            //
            // Keyed and OFF by default. Bounded logging. The per-binding record
            // is captured on the real call each frame and cleared at frame-root
            // entry, so a stale volume from an earlier frame can never be
            // written - that is how a repair turns into a corruption.
            if (volume_restore_enabled.load(std::memory_order_relaxed) &&
                write.dstBinding < kVolumeRestoreBindings) {
                const bool incoming_3d =
                    image_view_infos.count(img.imageView) &&
                    image_view_infos[img.imageView].view_type == VK_IMAGE_VIEW_TYPE_3D;
                if (inside_frame_root_double) {
                    const VkImageView want = real_volume_by_binding[write.dstBinding];
                    if (!incoming_3d && want != VK_NULL_HANDLE) {
                        rec.image_view = want;
                        rec.views[0] = want;
                        volume_restores.fetch_add(1, std::memory_order_relaxed);
                        if (volume_restore_lines.fetch_add(1, std::memory_order_relaxed) < 8)
                            log::warn(std::format(
                                "VolumeRestore: binding {} - the doubled call wrote a NON-3D view "
                                "{} where the real call wrote volume {}. Restored.",
                                write.dstBinding, reinterpret_cast<void*>(img.imageView),
                                reinterpret_cast<void*>(want)));
                    }
                    vr_writes_doubled.fetch_add(1, std::memory_order_relaxed);
                } else if (inside_real_frame_root && incoming_3d) {
                    vr_volumes_recorded.fetch_add(1, std::memory_order_relaxed);
                    real_volume_by_binding[write.dstBinding] = img.imageView;
                }
            }
            descriptor_image_sets[write.dstSet][write.dstBinding] = rec;
            desc_ring_push(write.dstSet, kDescRingImgWrite,
                           static_cast<uint16_t>(write.dstBinding));
            desc_update_img_records.fetch_add(1, std::memory_order_relaxed);
            // P1W: where binding 2 of the lighting job's sets comes from.
            p1_capture_write(write.dstSet, write.dstBinding, rec.image_view);
        }
        // ReadRedirect safety (STEP 6). A clone is built ONLY from the
        // records above. A descriptor type we do not reproduce - texel
        // buffers, inline uniform blocks, acceleration structures - would
        // therefore be left UNDEFINED in the clone, and a shader reading an
        // undefined descriptor is a device-lost hang, not a visual glitch.
        // Mark the set so cloning refuses it outright and says why.
        const bool reproducible = image_type ||
            write.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
            write.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
            write.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
            write.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        if (!reproducible || write.descriptorCount != 1 || write.dstArrayElement != 0) {
            std::scoped_lock lock(read_redirect_mutex);
            sets_with_untracked_bindings.insert(write.dstSet);
        }
    }
    real_update_descriptor_sets(device, write_count, writes, copy_count, copies);
}

// DescriptorCensus classification - see descriptor_census_enabled above.
//
//   mirror  - we hold a private mirrored copy of this exact source view. The
//             doubled call's WRITES went to the mirror; a pass SAMPLING the
//             original here is reading DOOM's unshifted content. This is the
//             set STEP 6 must redirect, and the count of these is the whole
//             point of the census.
//   shared  - deliberately shared, view-independent (the light-space shadow
//             atlas). Correct to leave alone - the precedent applies
//             per-resource, not as an approach change.
//   unknown - not an attachment of any framebuffer the doubled call rendered
//             into: ordinary textures, megatexture pages, lookup tables.
//             Expected to be the large majority and expected to need nothing.
//
// Reads mirrored_views/mirror_shared_source_views without a lock, on the
// same render thread that writes them under r_useSMP=1 - the same assumption
// ensure_mirror_framebuffer itself already relies on.
const char* census_classify_view(VkImageView view) {
    if (view == VK_NULL_HANDLE) return "null";
    if (mirrored_views.find(view) != mirrored_views.end()) return "mirror";
    if (mirror_shared_source_views.count(view) != 0) return "shared";
    return "unknown";
}

// UNIFORM BLOCK DIFF (2026-08-20). The last place the two calls can differ.
//
// Everything else is measured and matched. DrawParity: the doubled call issues
// the SAME draws and index counts as the real one on every world and effects
// pass (405/405, 417/417, 140/140, 103/103). DeferredInputMap: it binds the
// SAME view and image handles, including both 3D froxel fog volumes that carry
// the missing volumetrics. Same draws, same inputs, and yet the mirror's
// lighting comes out at 30% of DOOM's and its additive composite at 0.4%.
//
// So the difference has to be in what the shaders READ PER DRAW - the constant
// blocks - because that is the only input left that is not already proven
// identical. This hashes every uniform block bound by each call, keyed by
// (pass, set, binding), and reports where REAL and DOUBLED disagree.
//
// A mismatch is not automatically the bug: the camera constants MUST differ,
// that is the entire point of the doubled call. What matters is a block that
// differs and ISN'T the camera - or one that is identical where it should
// have been shifted.
//
// Read at RECORD time from the host mapping DOOM writes through, so this is
// CPU-side data with no GPU ordering question attached.
struct UniformBlockRecord {
    uint64_t hash = 0;
    uint32_t bytes = 0;
    std::array<float, 8> head{};
    // FULL CONTENTS, so follow-up questions cost an offline read and not
    // another run. The first report showed 16 blocks differing with IDENTICAL
    // leading floats - the divergence is deeper in the block, and printing
    // four floats could never locate it. Deduped to ~173 keys per side, so
    // this is ~1.4 MB at the 4096 cap.
    std::vector<unsigned char> data;
};
std::mutex uniform_diff_mutex;
std::map<std::tuple<uint32_t, uint32_t, uint32_t>, UniformBlockRecord> uniform_diff_real;
std::map<std::tuple<uint32_t, uint32_t, uint32_t>, UniformBlockRecord> uniform_diff_doubled;
std::atomic_bool uniform_diff_armed{false};
std::atomic_uint32_t uniform_diff_reports{0};
// LIGHT-CULLING A/B, BOTH ARMS IN ONE PRESS (2026-08-21).
//
// The question: does suppress_doubled_light_culling cause the doubled call's
// large lighting constant blocks to read zeros? It was previously "eliminated"
// by an A/B whose two arms BOTH read approximately nothing (pass-25 delta
// +0.0002 ON, +0.0001 OFF) - a null instrument reporting null twice, at a pass
// the zeros were never seen at. That eliminates nothing.
//
// So both arms run inside a single scripted run, set HERE in code, never by a
// keypress the owner cannot verify from inside a headset:
//   samples 0,1 -> suppression FORCED ON   (the shipping configuration)
//   samples 2,3 -> suppression FORCED OFF  (the doubled call's own culling)
// The flip happens on the present thread, AFTER both instruments have emitted
// for the frame just rendered, so no artifact can straddle the two arms.
//
// ⚠ THE POSITIVE CONTROL IS PART OF THE MEASUREMENT, not an optional extra.
// suppressedDispatchesDelta below must be > 0 in the ON arm and exactly 0 in
// the OFF arm. If it is 0 in BOTH, the flag never reached the dispatch path,
// the run measured nothing, and the correct conclusion is "instrument failed"
// - never "hypothesis eliminated". That distinction is the whole reason this
// question is being re-opened.
constexpr uint32_t kLightCullAbOnSamples = 2;
std::atomic_bool light_cull_ab_active{false};
std::atomic_uint32_t light_cull_ab_flips{0};
// Snapshot taken when a sample is armed; the delta at emit time is how many
// dispatches this sample's frame actually suppressed.
std::atomic_uint64_t light_cull_suppressed_mark{0};
std::atomic_uint64_t light_cull_sample_suppressed{0};
// The arm that was in force during the frame the instruments captured, latched
// at capture-arm time so a report can never be labelled with a state that only
// became true after its frame was rendered.
std::atomic_bool light_cull_sample_arm_on{true};
#endif  // DOOMVR_RESEARCH_TOOLS

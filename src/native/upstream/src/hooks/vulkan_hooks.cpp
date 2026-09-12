// Must precede EVERY include: doomvr/vulkan_hooks.h pulls in vulkan.h, so a
// define after it arrives too late and the Win32 surface types stay hidden.
// Additive only - it declares VkWin32SurfaceCreateInfoKHR and its PFN_ so
// vkCreateWin32SurfaceKHR (which DOOM imports) can be hooked like the rest.
#define VK_USE_PLATFORM_WIN32_KHR
#include "kharvoxnative/vulkan_hooks.h"
#include "kharvoxnative/openxr_context.h"
#include "kharvoxnative/wraparound.h"
#include "kharvoxnative/freqhigh_record.h"
#include "kharvoxnative/log.h"
// Vendored verbatim from C:\Program Files\RenderDoc\renderdoc_app.h so the
// build does not depend on RenderDoc being installed at a particular path, or
// installed at all. Header only, no link-time dependency: everything is
// resolved with GetProcAddress at runtime and every entry point is a no-op
// when renderdoc.dll is not in the process. See rdc_api() below.
#include "kharvoxnative/renderdoc_app.h"
#include <Windows.h>
#include <MinHook.h>
#include <vulkan/vulkan.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <type_traits>
#include <array>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kharvoxnative::vk_hooks {
namespace {

// ===========================================================================
// THE BODY OF THIS FILE LIVES IN src/hooks/vulkan_hooks/*.inc
// ===========================================================================
//
// Split 2026-09-03 to make ~38,000 lines navigable. These are FRAGMENTS, not
// translation units: each is #included here, in order, inside the anonymous
// namespace opened above, so the preprocessor reassembles exactly the file
// that was here before. Verified by diffing the preprocessed token stream
// with DOOMVR_RESEARCH_TOOLS both OFF and ON.
//
// WHY NOT SEPARATE .cpp FILES. This body declares ~1,065 file-scope symbols,
// all internal linkage in one anonymous namespace. Real separate compilation
// means giving every cross-file symbol external linkage and a header, on a
// file with NO test coverage - a large refactor whose failures are silent.
// This buys the whole navigability win with a proof attached.
//
// ⚠ 89% OF THIS CODE SITS INSIDE #ifdef DOOMVR_RESEARCH_TOOLS - 34,219 of
// 38,579 lines - BUT THAT GUARD IS NOT A SHIPPING/RESEARCH BOUNDARY, and the
// option is ON by default because OFF does not compile. Real shipped
// behaviour lives inside it, most importantly gunfix2's submit-time viewmodel
// correction. Do not read the guard as "this part does not ship".
// See the note on the option in CMakeLists.txt.
//
// ⚠ THE ORDER IS LOAD-BEARING - one scope, so a declaration must precede
// its use. Do not reorder these includes.

#include "vulkan_hooks/01_state.inc"          //  4286 lines
#include "vulkan_hooks/02_probes.inc"         //  3931 lines
#include "vulkan_hooks/03_camera.inc"         //  4020 lines
#include "vulkan_hooks/04_render_jobs.inc"    //  4010 lines
#include "vulkan_hooks/05_mirrors.inc"        //  4078 lines
#include "vulkan_hooks/06_renderpass.inc"     //  3952 lines
#include "vulkan_hooks/07_descriptors.inc"    //  4082 lines
#include "vulkan_hooks/08_stereo.inc"         //  3981 lines
#include "vulkan_hooks/09_present.inc"        //  5900 lines

}

bool install() {
    if (installed.exchange(true)) return true;
    HMODULE vk = GetModuleHandleW(L"vulkan-1.dll");
    if (!vk) { installed = false; return false; }
    if (MH_Initialize() != MH_OK) {
        log::error("MH_Initialize failed"); installed = false; return false;
    }
    bool ok = true;
    ok &= add_hook(vk, "vkCreateInstance", reinterpret_cast<void*>(&hook_create_instance), real_create_instance);
    ok &= add_hook(vk, "vkEnumeratePhysicalDevices", reinterpret_cast<void*>(&hook_enumerate_physical_devices), real_enumerate_physical_devices);
    ok &= add_hook(vk, "vkCreateDevice", reinterpret_cast<void*>(&hook_create_device), real_create_device);
    ok &= add_hook(vk, "vkGetDeviceQueue", reinterpret_cast<void*>(&hook_get_device_queue), real_get_device_queue);
    ok &= add_hook(vk, "vkQueueSubmit", reinterpret_cast<void*>(&hook_queue_submit), real_queue_submit);
    // Timing-only, see their definitions. Not folded into `ok`: if either
    // export is absent the budget loses one field, which is a degraded
    // measurement, not a reason to fail the whole install.
    add_hook(vk, "vkAcquireNextImageKHR", reinterpret_cast<void*>(&hook_acquire_next_image),
             real_acquire_next_image);
    add_hook(vk, "vkWaitForFences", reinterpret_cast<void*>(&hook_wait_for_fences),
             real_wait_for_fences);
    ok &= add_hook(vk, "vkQueuePresentKHR", reinterpret_cast<void*>(&hook_queue_present), real_queue_present);
    ok &= add_hook(vk, "vkCreateSwapchainKHR", reinterpret_cast<void*>(&hook_create_swapchain), real_create_swapchain);
    ok &= add_hook(vk, "vkGetSwapchainImagesKHR", reinterpret_cast<void*>(&hook_get_swapchain_images), real_get_swapchain_images);
    ok &= add_hook(vk, "vkDestroySwapchainKHR", reinterpret_cast<void*>(&hook_destroy_swapchain), real_destroy_swapchain);
#ifdef DOOMVR_RESEARCH_TOOLS
    // Safe, bounded camera diagnostics. Mapping lifetime hooks prevent stale host
    // pointers; the command hook only copies a 64-byte snapshot while protected.
    // A failure anywhere in this block only disables research tooling - it must
    // never take down the core Vulkan/virtual-screen hooks above.
    bool research_ok = true;
    research_ok &= add_hook(vk, "vkCreateBuffer", reinterpret_cast<void*>(&hook_create_buffer), real_create_buffer);
    research_ok &= add_hook(vk, "vkBindBufferMemory", reinterpret_cast<void*>(&hook_bind_buffer_memory), real_bind_buffer_memory);
    research_ok &= add_hook(vk, "vkMapMemory", reinterpret_cast<void*>(&hook_map_memory), real_map_memory);
    research_ok &= add_hook(vk, "vkUnmapMemory", reinterpret_cast<void*>(&hook_unmap_memory), real_unmap_memory);
    research_ok &= add_hook(vk, "vkFreeMemory", reinterpret_cast<void*>(&hook_free_memory), real_free_memory);
    research_ok &= add_hook(vk, "vkUpdateDescriptorSets", reinterpret_cast<void*>(&hook_update_descriptor_sets), real_update_descriptor_sets);
    // ReadRedirect (STEP 6): records set -> layout, the only way to recover a
    // layout from a VkDescriptorSet in order to clone it.
    research_ok &= add_hook(vk, "vkAllocateDescriptorSets", reinterpret_cast<void*>(&hook_allocate_descriptor_sets), real_allocate_descriptor_sets);
    // Barrier substitution: without this the doubled call transitions DOOM's
    // OWN images and never transitions the mirrors - which hung the GPU.
    research_ok &= add_hook(vk, "vkCmdPipelineBarrier", reinterpret_cast<void*>(&hook_cmd_pipeline_barrier), real_cmd_pipeline_barrier);
    // COMPUTE PROBE: does the doubled call write storage images we have no
    // mirror for - i.e. DOOM's own - see hook_cmd_dispatch.
    research_ok &= add_hook(vk, "vkCmdDispatch", reinterpret_cast<void*>(&hook_cmd_dispatch), real_cmd_dispatch);
    research_ok &= add_hook(vk, "vkCmdDispatchIndirect", reinterpret_cast<void*>(&hook_cmd_dispatch_indirect), real_cmd_dispatch_indirect);
    // TRANSFER PROBE: these name images directly and bypass framebuffer
    // mirroring entirely - the same gap the barriers had.
    research_ok &= add_hook(vk, "vkCmdCopyImage", reinterpret_cast<void*>(&hook_cmd_copy_image), real_cmd_copy_image);
    research_ok &= add_hook(vk, "vkCmdBlitImage", reinterpret_cast<void*>(&hook_cmd_blit_image), real_cmd_blit_image);
    research_ok &= add_hook(vk, "vkCmdResolveImage", reinterpret_cast<void*>(&hook_cmd_resolve_image), real_cmd_resolve_image);
    // COVERAGE PROBE. Deliberately NOT folded into research_ok: these are
    // extension / Vulkan-1.3 entry points and a missing export is a valid
    // answer ("DOOM's driver does not expose it, so DOOM cannot be using
    // it"), not a failure. add_hook logs the absence either way.
    add_hook(vk, "vkCmdPushDescriptorSetKHR", reinterpret_cast<void*>(&hook_cmd_push_descriptor_set), real_cmd_push_descriptor_set);
    add_hook(vk, "vkCmdPipelineBarrier2", reinterpret_cast<void*>(&hook_cmd_pipeline_barrier2), real_cmd_pipeline_barrier2);
    add_hook(vk, "vkCmdWaitEvents", reinterpret_cast<void*>(&hook_cmd_wait_events), real_cmd_wait_events);
    add_hook(vk, "vkCmdCopyBufferToImage", reinterpret_cast<void*>(&hook_cmd_copy_buffer_to_image), real_cmd_copy_buffer_to_image);
    // Must be installed BEFORE the update hook is useful - without the entry
    // layout the payload cannot be decoded and template-written sets stay
    // excluded from redirection. See descriptor_update_templates.
    add_hook(vk, "vkCreateDescriptorUpdateTemplate", reinterpret_cast<void*>(&hook_create_descriptor_update_template), real_create_descriptor_update_template);
    add_hook(vk, "vkUpdateDescriptorSetWithTemplate", reinterpret_cast<void*>(&hook_update_descriptor_set_with_template), real_update_descriptor_set_with_template);
    research_ok &= add_hook(vk, "vkCmdBindDescriptorSets", reinterpret_cast<void*>(&hook_cmd_bind_descriptor_sets), real_cmd_bind_descriptor_sets);
    research_ok &= add_hook(vk, "vkCmdBindPipeline", reinterpret_cast<void*>(&hook_cmd_bind_pipeline), real_cmd_bind_pipeline);
    // WorldVertexIndexProbe - see hook_cmd_bind_vertex_buffers' declaration above.
    research_ok &= add_hook(vk, "vkCmdBindVertexBuffers", reinterpret_cast<void*>(&hook_cmd_bind_vertex_buffers), real_cmd_bind_vertex_buffers);
    research_ok &= add_hook(vk, "vkCmdBindIndexBuffer", reinterpret_cast<void*>(&hook_cmd_bind_index_buffer), real_cmd_bind_index_buffer);
    research_ok &= add_hook(vk, "vkCmdDrawIndexedIndirect", reinterpret_cast<void*>(&hook_cmd_draw_indexed_indirect), real_cmd_draw_indexed_indirect);
    // DrawParity: previously unhooked, so indirect non-indexed draws were a
    // blind spot in every count this project has ever taken.
    research_ok &= add_hook(vk, "vkCmdDrawIndirect", reinterpret_cast<void*>(&hook_cmd_draw_indirect), real_cmd_draw_indirect);
    // RenderDepthDrawCountProbe - see hook_cmd_draw's declaration above.
    research_ok &= add_hook(vk, "vkCmdDraw", reinterpret_cast<void*>(&hook_cmd_draw), real_cmd_draw);
    research_ok &= add_hook(vk, "vkCmdDrawIndexed", reinterpret_cast<void*>(&hook_cmd_draw_indexed), real_cmd_draw_indexed);
    // QueryPoolProbe - see hook_cmd_begin_query's declaration above.
    research_ok &= add_hook(vk, "vkCmdBeginQuery", reinterpret_cast<void*>(&hook_cmd_begin_query), real_cmd_begin_query);
    research_ok &= add_hook(vk, "vkCmdEndQuery", reinterpret_cast<void*>(&hook_cmd_end_query), real_cmd_end_query);
    research_ok &= add_hook(vk, "vkCmdResetQueryPool", reinterpret_cast<void*>(&hook_cmd_reset_query_pool), real_cmd_reset_query_pool);
    // ScissorViewportProbe - see hook_cmd_set_scissor's declaration above.
    // Gated on inside_render_opaque, unlike the removed unguarded version.
    research_ok &= add_hook(vk, "vkCmdSetScissor", reinterpret_cast<void*>(&hook_cmd_set_scissor), real_cmd_set_scissor);
    research_ok &= add_hook(vk, "vkCmdSetViewport", reinterpret_cast<void*>(&hook_cmd_set_viewport), real_cmd_set_viewport);
    // vkCmdPushConstants: installed briefly (2026-08-10) to verify the
    // theory that DOOM might deliver per-draw matrices via push constants.
    // 48 seconds of live gameplay produced zero samples in doomvr.log -
    // DOOM never calls this API. Consistent with the decompile: id-Tech 6
    // packages per-draw state into its own command-list format (see
    // fcn.1418f6b50, fcn.1417e4650), not through Vulkan's per-draw APIs.
    // Uninstalled - the hook body remains for a future need but no longer
    // consumes a hook slot.
    // Render-target diagnostic for the RenderOpaque hook below (see
    // docs/RE-NOTES.md open question 2). Some of these exports legitimately
    // won't exist depending on which render-pass style DOOM uses -
    // add_hook already treats a missing export as non-fatal.
    add_hook(vk, "vkCmdBeginRenderPass", reinterpret_cast<void*>(&hook_cmd_begin_render_pass), real_cmd_begin_render_pass);
    // PassDrawCountProbe - see hook_cmd_end_render_pass's declaration above.
    add_hook(vk, "vkCmdEndRenderPass", reinterpret_cast<void*>(&hook_cmd_end_render_pass), real_cmd_end_render_pass);
    add_hook(vk, "vkCmdBeginRenderPass2", reinterpret_cast<void*>(&hook_cmd_begin_render_pass2), real_cmd_begin_render_pass2);
    add_hook(vk, "vkCmdBeginRenderPass2KHR", reinterpret_cast<void*>(&hook_cmd_begin_render_pass2_khr), real_cmd_begin_render_pass2_khr);
    add_hook(vk, "vkCmdBeginRendering", reinterpret_cast<void*>(&hook_cmd_begin_rendering), real_cmd_begin_rendering);
    add_hook(vk, "vkCmdBeginRenderingKHR", reinterpret_cast<void*>(&hook_cmd_begin_rendering_khr), real_cmd_begin_rendering_khr);
    // Read-only vkCreateRenderPass/vkCreateFramebuffer diagnostic (Phase 2.4
    // next step - see docs/HANDOFF-NEXT-SESSION.md). vkCreateRenderPass and
    // vkCreateFramebuffer are core and must exist; the RenderPass2 variants
    // are optional/defensive, matching the begin-render-pass family above.
    research_ok &= add_hook(vk, "vkCreateRenderPass", reinterpret_cast<void*>(&hook_create_render_pass), real_create_render_pass);
    add_hook(vk, "vkCreateRenderPass2", reinterpret_cast<void*>(&hook_create_render_pass2), real_create_render_pass2);
    add_hook(vk, "vkCreateRenderPass2KHR", reinterpret_cast<void*>(&hook_create_render_pass2_khr), real_create_render_pass2_khr);
    research_ok &= add_hook(vk, "vkCreateFramebuffer", reinterpret_cast<void*>(&hook_create_framebuffer), real_create_framebuffer);
    // Resource lifetime (review risk 2). Without these, a recycled handle
    // after a level transition aliases a stale mirror SILENTLY - see
    // hook_destroy_framebuffer.
    research_ok &= add_hook(vk, "vkDestroyFramebuffer", reinterpret_cast<void*>(&hook_destroy_framebuffer), real_destroy_framebuffer);
    research_ok &= add_hook(vk, "vkDestroyImageView", reinterpret_cast<void*>(&hook_destroy_image_view), real_destroy_image_view);
    // DepthDumpControlTest - see hook_create_image_view's declaration above.
    research_ok &= add_hook(vk, "vkCreateImageView", reinterpret_cast<void*>(&hook_create_image_view), real_create_image_view);
    // Review P1: extents for the storage images the doubled call writes.
    research_ok &= add_hook(vk, "vkCreateImage", reinterpret_cast<void*>(&hook_create_image), real_create_image);
    // Phase 0 (STEREO-IMPLEMENTATION-PLAN.md v2): pipeline depth/stencil
    // state capture, to confirm (not assume) what's actually rejecting
    // near-field fragments for a shifted eye. The scissor/viewport/draw-kind
    // hooks that originally accompanied this were removed after a live
    // crash - see hook_cmd_bind_pipeline's comment below for why this one
    // was kept.
    research_ok &= add_hook(vk, "vkCreateGraphicsPipelines",
        reinterpret_cast<void*>(&hook_create_graphics_pipelines), real_create_graphics_pipelines);
    HMODULE doom = GetModuleHandleW(L"DOOMx64vk.exe");
    if (!doom) {
        log::error("DOOM module not found for internal render-view hook; research tooling disabled");
        research_ok = false;
    } else {
        // ------------------------------------------------ HOOK BUDGET (bisect) --
        // 2026-08-28. The injector alone corrupts DOOM's rendering: loading a
        // level whose megatexture is not already cached comes up with large
        // numbers of world surfaces missing (skybox through walls), and the
        // owner's A/B has the injector as the only variable. Removing the
        // mis-targeted AppendDrawCommand hook (see below) did NOT fix it, so
        // one of the remaining hooks into DOOM's own code is responsible.
        //
        // Rather than rebuild per candidate, the count of DOOM-function hooks
        // installed is taken from the environment:
        //
        //     DOOMVR_DOOM_HOOKS=0   install NONE  (is it these hooks at all?)
        //     DOOMVR_DOOM_HOOKS=8   install the first eight, in table order
        //     unset                 install all, i.e. previous behaviour
        //
        // Every hook logs INSTALLED or SKIPPED with its index, so the log says
        // exactly which set was live - a run whose config has to be inferred
        // is how this project has lost runs before.
        //
        // NOTE the Vulkan hooks are NOT covered by this budget. If 0 still
        // reproduces, the fault is in the Vulkan layer or the XR path, and
        // that is a different bisect.
        // READ FROM A FILE, NOT THE ENVIRONMENT. The first version of this read
        // DOOMVR_DOOM_HOOKS from the environment and the very first bisect run
        // was VOID: DOOM inherits STEAM's environment, not the injector's, so
        // the variable never arrived and the log reported UINT32_MAX with all
        // nineteen hooks live. Its own control caught it. A file is read by the
        // DLL inside DOOM's process, so it needs no Steam restart and can be
        // changed between launches - which is what makes bisecting cheap.
        uint32_t doom_hook_limit = UINT32_MAX;
        {
            FILE* f = nullptr;
            if (fopen_s(&f, "C:\\dev\\doomvr-hooks.txt", "r") == 0 && f) {
                unsigned long v = 0;
                if (std::fscanf(f, "%lu", &v) == 1) doom_hook_limit = static_cast<uint32_t>(v);
                std::fclose(f);
            }
        }
        uint32_t doom_hook_index = 0;
        const auto try_doom_hook = [&](uintptr_t offset, void* fn, void** tramp,
                                       const char* name) {
            const uint32_t idx = doom_hook_index++;
            if (idx >= doom_hook_limit) {
                log::warn(std::format("DoomHook[{}] SKIPPED {} at {:#x} (DOOMVR_DOOM_HOOKS={})",
                    idx, name, offset, doom_hook_limit));
                return;
            }
            auto* addr = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(doom) + offset);
            if (MH_CreateHook(addr, fn, tramp) != MH_OK) {
                log::error(std::format("DoomHook[{}] FAILED {} at {:#x}", idx, name, offset));
                research_ok = false;
            } else {
                log::info(std::format("DoomHook[{}] INSTALLED {} at {:#x}", idx, name, offset));
            }
        };
        log::warn(std::format(
            "DoomHookBudget: DOOMVR_DOOM_HOOKS={} (UINT32_MAX = all). Grep 'DoomHook[' for the "
            "exact set this run installed.", doom_hook_limit));

        try_doom_hook(0x1A37910, reinterpret_cast<void*>(&hook_doom_render_view),
            reinterpret_cast<void**>(&real_doom_render_view), "render-view");
        // RENDER-SURFACE HOOK REMOVED (2026-08-28). IT CORRUPTED DOOM'S OWN
        // RENDERING, AND IT HAD DONE SO SINCE IT WAS ADDED.
        //
        // Symptom: loading any level whose megatexture was not already cached
        // came up with large numbers of world surfaces missing - skybox
        // straight through walls and floors. The long-used starting level
        // never showed it, which is why this survived for weeks and why every
        // eye-1 measurement taken in that level was taken on a quietly
        // corrupted baseline.
        //
        // Found by bisecting the DOOM-function hook table with the
        // C:\dev\doomvr-hooks.txt budget (owner's A/B, flat screen, no
        // headset): limit 19/10/5/2 all broken, limit 1 CLEAN. Hook index 1 -
        // this one, DOOM+0x1A367E0 - is the only difference between 1 and 2.
        //
        // WHY, as far as it is established: the body is harmless. It sets a
        // thread_local, calls apply_temporary_world_yaw (which is permanently
        // disabled and returns false without touching anything), then calls
        // through. So the damage is not what the hook DOES, it is the hook
        // ITSELF - the signature we trampoline through is wrong. It is
        // declared void __fastcall(void*, void*, void*, uintptr_t); if the
        // real function returns a value we discard whatever it left in RAX,
        // and if it takes float/vector arguments our prologue can clobber the
        // volatile XMM registers before the trampoline runs. Either produces
        // exactly "some surfaces are never submitted". NOT yet proven which -
        // the address itself is a valid entry (pescan extent 1A367E0 resolves
        // to itself), so re-pointing it is not the fix.
        //
        // The hook was labelled "read-only" in every log line it ever printed.
        // It was not read-only in effect, and nothing consumes what it
        // recorded: hook 0 (render-view) already maintains active_render_view,
        // and limit=1 renders correctly.
        //
        // If this path is ever wanted again, derive the real signature from
        // the binary first and re-verify with the same flat-screen A/B.
        (void)&hook_doom_render_surface;
        (void)&real_doom_render_surface;
        try_doom_hook(0x1A37CC0, reinterpret_cast<void*>(&hook_doom_render_opaque),
            reinterpret_cast<void**>(&real_doom_render_opaque), "RenderOpaque");
        // UpstreamPipelineProbe - see its declaration above. Module offsets
        // resolved via radare2 (job-registration-string technique, see
        // docs/RE-NOTES.md): search for each job's name string, find the
        // code xref (the RegisterJob call site), read the function pointer
        // registered alongside it.
        struct UpstreamHookTarget {
            uintptr_t offset;
            void* hook_fn;
            void** trampoline;
            const char* name;
        };
        const UpstreamHookTarget upstream_targets[] = {
            {0x1A35F20, reinterpret_cast<void*>(&hook_render_gather_prepare),
                reinterpret_cast<void**>(&real_render_gather_prepare), "RenderGatherPrepare"},
            {0x1A36440, reinterpret_cast<void*>(&hook_render_gather_world_surfaces_prepare),
                reinterpret_cast<void**>(&real_render_gather_world_surfaces_prepare),
                "RenderGatherWorldSurfacesPrepare"},
            {0x1A3B6C0, reinterpret_cast<void*>(&hook_render_cull),
                reinterpret_cast<void**>(&real_render_cull), "RenderCull"},
            {0x1A3BE50, reinterpret_cast<void*>(&hook_render_walk_bsp),
                reinterpret_cast<void**>(&real_render_walk_bsp), "RenderWalkBSP"},
            {0x1A34B10, reinterpret_cast<void*>(&hook_render_gather_finish),
                reinterpret_cast<void**>(&real_render_gather_finish), "RenderGatherFinish"},
            {0x1A3CB60, reinterpret_cast<void*>(&hook_render_sort),
                reinterpret_cast<void**>(&real_render_sort), "RenderSort"},
            {0x1A35670, reinterpret_cast<void*>(&hook_render_gather_models_prepare),
                reinterpret_cast<void**>(&real_render_gather_models_prepare), "RenderGatherModelsPrepare"},
            {0x1A34880, reinterpret_cast<void*>(&hook_render_gather_decals_prepare),
                reinterpret_cast<void**>(&real_render_gather_decals_prepare), "RenderGatherDecalsPrepare"},
            {0x1A34D60, reinterpret_cast<void*>(&hook_render_gather_lights_prepare),
                reinterpret_cast<void**>(&real_render_gather_lights_prepare), "RenderGatherLightsPrepare"},
            {0x1A340B0, reinterpret_cast<void*>(&hook_render_gather_add_always),
                reinterpret_cast<void**>(&real_render_gather_add_always), "RenderGatherAddAlways"},
            {0x17B35C0, reinterpret_cast<void*>(&hook_render_gui_models),
                reinterpret_cast<void**>(&real_render_gui_models), "RenderGuiModels"},
            {0x17B3D70, reinterpret_cast<void*>(&hook_setup_view_gui),
                reinterpret_cast<void**>(&real_setup_view_gui), "SetupView_gui (Lever V)"},
            // VIEWSRC (2026-08-29): the pre-cull camera producer. READ-ONLY.
            // pescan-checked before install: `pescan pdata 1830B80` reports
            // begin=1830B80 / real function 1830B80, so this is a .pdata root
            // and not a mid-function address.
            {0x1830B80, reinterpret_cast<void*>(&hook_build_view_matrices),
                reinterpret_cast<void**>(&real_build_view_matrices),
                "BuildViewMatrices (VIEWSRC producer)"},
            // VIEWSRC rider. READ-ONLY. RE-POINTED 2026-08-30 from
            // ComputeClustersFrustums (0x187B910) to CullLights (0x187E330):
            // the step-2 run read ZERO on the old address, and the binary says
            // why. RenderView branches on byte [jobArg+0x21] at 0x17B3BD4; this
            // scene takes the ==0 arm, which calls the SYNCHRONOUS clustered
            // path 0x1881FF0, and inside it 0x187B910 is gated on
            // `*0x146EAC230 != 0` - a global that read 0. CullLights is called
            // unconditionally on that same path (0x188206D), takes the cluster
            // context directly, and reads *(arg1+0x38) - the very viewParms
            // this thread writes. pescan extent 187E330 -> root 187E330.
            {0x187E330, reinterpret_cast<void*>(&hook_cull_lights),
                reinterpret_cast<void**>(&real_cull_lights),
                "CullLights (VIEWSRC rider)"},
            {0x17B32E0, reinterpret_cast<void*>(&hook_setup_view_world),
                reinterpret_cast<void**>(&real_setup_view_world), "SetupView_world (Lever W)"},
            {0x1A36C10, reinterpret_cast<void*>(&hook_render_depth),
                reinterpret_cast<void**>(&real_render_depth), "RenderDepth"},
            // FrameRootProbe targets (2026-08-16). Both hooks are inert
            // until PageUp arms them; see their declarations above.
            {0x17B7900, reinterpret_cast<void*>(&hook_frame_root),
                reinterpret_cast<void**>(&real_frame_root), "FrameRoot (per-view)"},
            {0x1A33090, reinterpret_cast<void*>(&hook_job_dispatch),
                reinterpret_cast<void**>(&real_job_dispatch), "JobDispatch"},
        };
        for (const auto& upstream_target : upstream_targets) {
            try_doom_hook(upstream_target.offset, upstream_target.hook_fn,
                upstream_target.trampoline, upstream_target.name);
        }
        // APPENDDRAWCOMMAND HOOK REMOVED (2026-08-28) - IT WAS NEVER A FUNCTION
        // ENTRY, AND IT CORRUPTED DOOM'S OWN RENDERING.
        //
        // The offset used was 0x1A3CA89. The PE's own exception table says the
        // function begins at 0x1A3CA80:
        //
        //     pescan pdata DOOMx64vk.exe 1A3CA89
        //       -> entry begin=1A3CA80 end=1A3CA8E  real function 1A3CA80
        //     pescan dis   DOOMx64vk.exe 1A3CA89
        //       -> 1A3CA89  56           push rsi
        //          1A3CA8A  48 83 EC 20  sub rsp, 0x20
        //
        // So the hook landed NINE BYTES INTO THE PROLOGUE, and MinHook's
        // five-byte jump overwrote `push rsi; sub rsp,0x20` - i.e. the tail of
        // the frame setup. A hook entered mid-prologue does not see a
        // function-entry stack: the callee-saved pushes at 0x1A3CA80..88 have
        // already happened, so the sixth argument this probe read as
        // `pass_table` came off the wrong stack slot, and the trampoline
        // return path unwound against a frame nobody had built.
        //
        // SYMPTOM IT PRODUCED: loading any level the megatexture had not
        // already cached rendered with large numbers of world surfaces simply
        // absent - skybox visible through walls and floors. Owner's A/B,
        // 2026-08-28, same level and same position, injector the only
        // variable. The long-used starting level never showed it, which is why
        // this survived two weeks.
        //
        // This is the standing rule in this project, and it was broken here:
        // .pdata-check every address before hooking it. Every other DOOM hook
        // in the table above was swept with `pescan extent` on 2026-08-28 and
        // all of them resolve to their own function entry; this was the only
        // bad one.
        //
        // The probe itself was pure logging (AppendDrawCommandProbe) with no
        // consumer, so it is removed rather than re-pointed at 0x1A3CA80. If
        // it is ever wanted again, hook the REAL entry and re-derive the
        // argument list there - the old signature is not valid at that address.
        (void)&hook_append_draw_command;
        (void)&real_append_draw_command;
    }
    if (!research_ok) log::warn("Research tooling hooks incomplete; core virtual-screen path is unaffected");
#endif  // DOOMVR_RESEARCH_TOOLS
    // ⚠ THESE MUST BE REGISTERED BEFORE MH_EnableHook. They were placed
    // after it in the first version and were therefore CREATED BUT NEVER
    // ENABLED - every one reported 0 calls, which the coverage audit could
    // not see because it checks registration, not liveness. That cost a run.
    add_hook(vk, "vkDeviceWaitIdle", reinterpret_cast<void*>(&hook_device_wait_idle), real_device_wait_idle);
    add_hook(vk, "vkQueueWaitIdle", reinterpret_cast<void*>(&hook_queue_wait_idle), real_queue_wait_idle);
    add_hook(vk, "vkGetQueryPoolResults", reinterpret_cast<void*>(&hook_get_query_pool_results), real_get_query_pool_results);
    add_hook(vk, "vkAllocateMemory", reinterpret_cast<void*>(&hook_allocate_memory), real_allocate_memory);
    add_hook(vk, "vkFlushMappedMemoryRanges", reinterpret_cast<void*>(&hook_flush_mapped_memory_ranges), real_flush_mapped_memory_ranges);
    add_hook(vk, "vkAllocateCommandBuffers", reinterpret_cast<void*>(&hook_allocate_command_buffers), real_allocate_command_buffers);
    add_hook(vk, "vkFreeCommandBuffers", reinterpret_cast<void*>(&hook_free_command_buffers), real_free_command_buffers);
    add_hook(vk, "vkBeginCommandBuffer", reinterpret_cast<void*>(&hook_begin_command_buffer), real_begin_command_buffer);
    add_hook(vk, "vkEndCommandBuffer", reinterpret_cast<void*>(&hook_end_command_buffer), real_end_command_buffer);
    add_hook(vk, "vkResetFences", reinterpret_cast<void*>(&hook_reset_fences), real_reset_fences);
    add_hook(vk, "vkCreateFence", reinterpret_cast<void*>(&hook_create_fence), real_create_fence);
    add_hook(vk, "vkDestroyFence", reinterpret_cast<void*>(&hook_destroy_fence), real_destroy_fence);
    add_hook(vk, "vkResetDescriptorPool", reinterpret_cast<void*>(&hook_reset_descriptor_pool), real_reset_descriptor_pool);
    add_hook(vk, "vkCreateDescriptorPool", reinterpret_cast<void*>(&hook_create_descriptor_pool), real_create_descriptor_pool);
    add_hook(vk, "vkDestroyDescriptorPool", reinterpret_cast<void*>(&hook_destroy_descriptor_pool), real_destroy_descriptor_pool);
    add_hook(vk, "vkCreateComputePipelines", reinterpret_cast<void*>(&hook_create_compute_pipelines), real_create_compute_pipelines);
    add_hook(vk, "vkCreateShaderModule", reinterpret_cast<void*>(&hook_create_shader_module), real_create_shader_module);
    add_hook(vk, "vkDestroyShaderModule", reinterpret_cast<void*>(&hook_destroy_shader_module), real_destroy_shader_module);
    add_hook(vk, "vkDestroyImage", reinterpret_cast<void*>(&hook_destroy_image), real_destroy_image);
    add_hook(vk, "vkDestroyBuffer", reinterpret_cast<void*>(&hook_destroy_buffer), real_destroy_buffer);
    add_hook(vk, "vkDestroyPipeline", reinterpret_cast<void*>(&hook_destroy_pipeline), real_destroy_pipeline);
    add_hook(vk, "vkCmdCopyBuffer", reinterpret_cast<void*>(&hook_cmd_copy_buffer), real_cmd_copy_buffer);
    add_hook(vk, "vkCmdCopyImageToBuffer", reinterpret_cast<void*>(&hook_cmd_copy_image_to_buffer), real_cmd_copy_image_to_buffer);
    add_hook(vk, "vkCmdWriteTimestamp", reinterpret_cast<void*>(&hook_cmd_write_timestamp), real_cmd_write_timestamp);
    add_hook(vk, "vkCmdSetDepthBias", reinterpret_cast<void*>(&hook_cmd_set_depth_bias), real_cmd_set_depth_bias);
    add_hook(vk, "vkCmdSetStencilCompareMask", reinterpret_cast<void*>(&hook_cmd_set_stencil_compare_mask), real_cmd_set_stencil_compare_mask);
    add_hook(vk, "vkCmdSetStencilReference", reinterpret_cast<void*>(&hook_cmd_set_stencil_reference), real_cmd_set_stencil_reference);
    add_hook(vk, "vkBindImageMemory", reinterpret_cast<void*>(&hook_bind_image_memory), real_bind_image_memory);
    add_hook(vk, "vkCreateCommandPool", reinterpret_cast<void*>(&hook_create_command_pool), real_create_command_pool);
    add_hook(vk, "vkDestroyCommandPool", reinterpret_cast<void*>(&hook_destroy_command_pool), real_destroy_command_pool);
    add_hook(vk, "vkCreateDescriptorSetLayout", reinterpret_cast<void*>(&hook_create_descriptor_set_layout), real_create_descriptor_set_layout);
    add_hook(vk, "vkDestroyDescriptorSetLayout", reinterpret_cast<void*>(&hook_destroy_descriptor_set_layout), real_destroy_descriptor_set_layout);
    add_hook(vk, "vkCreatePipelineLayout", reinterpret_cast<void*>(&hook_create_pipeline_layout), real_create_pipeline_layout);
    add_hook(vk, "vkDestroyPipelineLayout", reinterpret_cast<void*>(&hook_destroy_pipeline_layout), real_destroy_pipeline_layout);
    add_hook(vk, "vkCreateQueryPool", reinterpret_cast<void*>(&hook_create_query_pool), real_create_query_pool);
    add_hook(vk, "vkDestroyQueryPool", reinterpret_cast<void*>(&hook_destroy_query_pool), real_destroy_query_pool);
    add_hook(vk, "vkCreateSampler", reinterpret_cast<void*>(&hook_create_sampler), real_create_sampler);
    add_hook(vk, "vkCreateSemaphore", reinterpret_cast<void*>(&hook_create_semaphore), real_create_semaphore);
    add_hook(vk, "vkDestroySemaphore", reinterpret_cast<void*>(&hook_destroy_semaphore), real_destroy_semaphore);
    add_hook(vk, "vkDestroyRenderPass", reinterpret_cast<void*>(&hook_destroy_render_pass), real_destroy_render_pass);
    add_hook(vk, "vkGetBufferMemoryRequirements", reinterpret_cast<void*>(&hook_get_buffer_memory_requirements), real_get_buffer_memory_requirements);
    add_hook(vk, "vkGetImageMemoryRequirements", reinterpret_cast<void*>(&hook_get_image_memory_requirements), real_get_image_memory_requirements);
    add_hook(vk, "vkEnumerateDeviceExtensionProperties", reinterpret_cast<void*>(&hook_enumerate_device_extension_properties), real_enumerate_device_extension_properties);
    add_hook(vk, "vkGetPhysicalDeviceFeatures", reinterpret_cast<void*>(&hook_get_physical_device_features), real_get_physical_device_features);
    add_hook(vk, "vkGetPhysicalDeviceFormatProperties", reinterpret_cast<void*>(&hook_get_physical_device_format_properties), real_get_physical_device_format_properties);
    add_hook(vk, "vkGetPhysicalDeviceMemoryProperties", reinterpret_cast<void*>(&hook_get_physical_device_memory_properties), real_get_physical_device_memory_properties);
    add_hook(vk, "vkGetPhysicalDeviceProperties", reinterpret_cast<void*>(&hook_get_physical_device_properties), real_get_physical_device_properties);
    add_hook(vk, "vkGetPhysicalDeviceQueueFamilyProperties", reinterpret_cast<void*>(&hook_get_physical_device_queue_family_properties), real_get_physical_device_queue_family_properties);
    add_hook(vk, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", reinterpret_cast<void*>(&hook_get_physical_device_surface_capabilities_khr), real_get_physical_device_surface_capabilities_khr);
    add_hook(vk, "vkGetPhysicalDeviceSurfaceFormatsKHR", reinterpret_cast<void*>(&hook_get_physical_device_surface_formats_khr), real_get_physical_device_surface_formats_khr);
    add_hook(vk, "vkGetPhysicalDeviceSurfacePresentModesKHR", reinterpret_cast<void*>(&hook_get_physical_device_surface_present_modes_khr), real_get_physical_device_surface_present_modes_khr);
    add_hook(vk, "vkGetPhysicalDeviceSurfaceSupportKHR", reinterpret_cast<void*>(&hook_get_physical_device_surface_support_khr), real_get_physical_device_surface_support_khr);
    add_hook(vk, "vkCreateWin32SurfaceKHR", reinterpret_cast<void*>(&hook_create_win32_surface_khr), real_create_win32_surface_khr);

    if (!ok || MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        log::error("Vulkan hook installation failed");
        MH_Uninitialize(); installed = false; return false;
    }
    log::info("Vulkan interception hooks installed");
    // Emitted AFTER MH_EnableHook succeeds, so it describes hooks that are
    // actually live. Grep 'HookCoverage' before reading any FrameBudget line.
    emit_hook_coverage();
    return true;
}

void uninstall() {
    if (!installed.exchange(false)) return;
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}
}

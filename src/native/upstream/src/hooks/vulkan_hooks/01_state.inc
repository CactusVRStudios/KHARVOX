// 01_state.inc - types, globals, and the diagnostic state they hang off
// 4208 of these lines sit inside #ifdef DOOMVR_RESEARCH_TOOLS,
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

using CreateInstanceFn = VkResult(VKAPI_PTR*)(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
using EnumeratePhysicalDevicesFn = VkResult(VKAPI_PTR*)(VkInstance, uint32_t*, VkPhysicalDevice*);
using CreateDeviceFn = VkResult(VKAPI_PTR*)(VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice*);
using GetDeviceQueueFn = void(VKAPI_PTR*)(VkDevice, uint32_t, uint32_t, VkQueue*);
using QueueSubmitFn = VkResult(VKAPI_PTR*)(VkQueue, uint32_t, const VkSubmitInfo*, VkFence);
using QueuePresentFn = VkResult(VKAPI_PTR*)(VkQueue, const VkPresentInfoKHR*);
using CreateSwapchainFn = VkResult(VKAPI_PTR*)(VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*);
using GetSwapchainImagesFn = VkResult(VKAPI_PTR*)(VkDevice, VkSwapchainKHR, uint32_t*, VkImage*);
using DestroySwapchainFn = void(VKAPI_PTR*)(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*);
#ifdef DOOMVR_RESEARCH_TOOLS
using CmdPushConstantsFn = void(VKAPI_PTR*)(VkCommandBuffer, VkPipelineLayout,
    VkShaderStageFlags, uint32_t, uint32_t, const void*);
using CreateBufferFn = VkResult(VKAPI_PTR*)(VkDevice, const VkBufferCreateInfo*,
    const VkAllocationCallbacks*, VkBuffer*);
using BindBufferMemoryFn = VkResult(VKAPI_PTR*)(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize);
using MapMemoryFn = VkResult(VKAPI_PTR*)(VkDevice, VkDeviceMemory, VkDeviceSize,
    VkDeviceSize, VkMemoryMapFlags, void**);
using UnmapMemoryFn = void(VKAPI_PTR*)(VkDevice, VkDeviceMemory);
using FreeMemoryFn = void(VKAPI_PTR*)(VkDevice, VkDeviceMemory, const VkAllocationCallbacks*);
using UpdateDescriptorSetsFn = void(VKAPI_PTR*)(VkDevice, uint32_t,
    const VkWriteDescriptorSet*, uint32_t, const VkCopyDescriptorSet*);
using CmdBindDescriptorSetsFn = void(VKAPI_PTR*)(VkCommandBuffer, VkPipelineBindPoint,
    VkPipelineLayout, uint32_t, uint32_t, const VkDescriptorSet*, uint32_t,
    const uint32_t*);
using CmdBindPipelineFn = void(VKAPI_PTR*)(VkCommandBuffer, VkPipelineBindPoint, VkPipeline);
// WorldVertexIndexProbe (docs/STEREO-RESTART-PLAN.md Task 3, last candidate
// 2026-08-11): everything checked so far went through vkCmdBindDescriptorSets
// (textures, uniform/storage buffers). Vertex/index/indirect-draw-parameter
// bindings are a completely separate Vulkan binding mechanism, never
// checked. Motivated by an already-proven pattern in this exact codebase:
// the camera uniform is ring-allocated with a private slot per invocation,
// and descriptor sets are ring-allocated per draw slot (both confirmed this
// project). If vertex/index data for dynamic geometry is ALSO a transient
// per-frame resource, a second traversal could read stale/reused memory -
// sky surviving fits, since a skybox mesh is almost certainly static,
// persistent data that never needs streaming, unlike near-field geometry.
using CmdBindVertexBuffersFn = void(VKAPI_PTR*)(VkCommandBuffer, uint32_t, uint32_t,
    const VkBuffer*, const VkDeviceSize*);
using CmdBindIndexBufferFn = void(VKAPI_PTR*)(VkCommandBuffer, VkBuffer, VkDeviceSize, VkIndexType);
using CmdDrawIndexedIndirectFn = void(VKAPI_PTR*)(VkCommandBuffer, VkBuffer, VkDeviceSize, uint32_t, uint32_t);
// RenderDepthDrawCountProbe (docs/STEREO-RESTART-PLAN.md Task 3, Round 5
// step 2 follow-up, next diagnostic): RenderDepth's own pipelines are
// confirmed depthTestEnable=1/depthWriteEnable=1 when redirected, and
// pass1's own render pass is confirmed to LOAD (not clear) depth
// afterward - both checks the plan named came back clean, yet the
// depth buffer still reads back uniform far-plane. The one fact neither
// check touches: whether RenderDepth's redirected call issues any actual
// vkCmdDraw*/vkCmdDrawIndexed* commands at all, as opposed to binding
// pipelines with nothing to draw. Tightly gated (see hook_cmd_draw/
// hook_cmd_draw_indexed below) on inside_render_depth &&
// real_call_redirect_armed - true only during the one END-armed
// redirected frame, never an always-on hot path.
using CmdDrawFn = void(VKAPI_PTR*)(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t);
using CmdDrawIndexedFn = void(VKAPI_PTR*)(VkCommandBuffer, uint32_t, uint32_t, int32_t, uint32_t, uint32_t);
using DoomRenderViewFn = void(__fastcall*)(void*, void*);
// ⚠ DOOM+0x189A350 IS NOT THE FINAL-PASS FUNCTION. DO NOT HOOK IT.
// (2026-08-18, FinalPassProbe, refuted and removed the same day.)
//
// The reasoning that led here was: the whole post-frame-root contribution is
// ONE render pass - same VkRenderPass every frame, alternating framebuffers,
// writing the swapchain at 3818x2032, grade and HUD together - and the stack
// walk at that render pass resolves its containing function, from .pdata, to
// 0x189A350 with an 11-level caller chain that is byte-identical every frame.
// That looked like a hookable, doubleable target.
//
// It is not. Hooked log-only for one run, 0x189A350 was called ~4.5 MILLION
// times across 10+ distinct worker threads, both inside and outside the frame
// root - roughly a thousand calls per frame. It is a generic helper that the
// final pass merely happens to be inside. Doubling it would have re-run a
// thousand unrelated operations per frame.
//
// It was not even free to OBSERVE: a MinHook trampoline plus one atomic
// increment on a function that hot, across that many threads, produced
// visible shadow artifacts while running around. That is the cost of a
// passthrough probe in the wrong place.
//
// THE LESSON, because it generalises: a stack walk names the function a call
// site sits in, NOT a function that is unique to it. "fnRva is the ENTRY of
// the containing function - that is the hookable address" (the PostFrameRoot
// probe's own log text) is true about the address and misleading about what
// it is good for. Frequency has to be measured before any address from a
// stack walk is treated as a doubling target.
//
// The final pass is uniquely identifiable at the VULKAN level - its
// VkRenderPass handle, and the fact that it writes a swapchain image - so
// that is where duplicating it has to be approached from.
using DoomRenderSurfaceFn = void(__fastcall*)(void*, void*, void*, uintptr_t);
using DoomRenderOpaqueFn = void(__fastcall*)(void*);
using CmdBeginRenderPassFn = void(VKAPI_PTR*)(VkCommandBuffer, const VkRenderPassBeginInfo*, VkSubpassContents);
using CmdEndRenderPassFn = void(VKAPI_PTR*)(VkCommandBuffer);
using CmdBeginRenderPass2Fn = void(VKAPI_PTR*)(VkCommandBuffer, const VkRenderPassBeginInfo*, const VkSubpassBeginInfo*);
using CmdBeginRenderingFn = void(VKAPI_PTR*)(VkCommandBuffer, const VkRenderingInfo*);
using CreateRenderPassFn = VkResult(VKAPI_PTR*)(VkDevice, const VkRenderPassCreateInfo*, const VkAllocationCallbacks*, VkRenderPass*);
using CreateRenderPass2Fn = VkResult(VKAPI_PTR*)(VkDevice, const VkRenderPassCreateInfo2*, const VkAllocationCallbacks*, VkRenderPass*);
using CreateFramebufferFn = VkResult(VKAPI_PTR*)(VkDevice, const VkFramebufferCreateInfo*, const VkAllocationCallbacks*, VkFramebuffer*);
// DepthDumpControlTest (docs/STEREO-RESTART-PLAN.md Task 3, Round 5
// continued, step 3): dump_offscreen_depth_snapshot is brand-new code that
// already had one real bug this round (unsupported LINEAR tiling) - before
// trusting its "uniform far-plane" result about the PRIVATE depth image,
// point the same readback logic at DOOM's REAL, already-known-correct
// depth image instead. framebuffer_depth_views already tracks the real
// depth VkImageView per real framebuffer, but vkCmdCopyImageToBuffer needs
// a VkImage, not a view - nothing previously tracked view->image, so this
// read-only hook adds exactly that (real_create_image_view is a thin
// passthrough plus a map insert, no behavior change to DOOM's own calls).
using CreateImageViewFn = VkResult(VKAPI_PTR*)(VkDevice, const VkImageViewCreateInfo*, const VkAllocationCallbacks*, VkImageView*);
// 2026-08-18, review P1. vkCreateImage was never hooked, so no image in this
// process has ever had a recorded EXTENT - which is exactly why the two
// unmirrored storage images the doubled call writes could only be named by
// handle and never identified. Thin passthrough plus a map insert.
using CreateImageFn = VkResult(VKAPI_PTR*)(VkDevice, const VkImageCreateInfo*, const VkAllocationCallbacks*, VkImage*);
using CreateGraphicsPipelinesFn = VkResult(VKAPI_PTR*)(VkDevice, VkPipelineCache, uint32_t,
    const VkGraphicsPipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline*);
// QueryPoolProbe (STEREO-IMPLEMENTATION-PLAN.md v3 Phase 0 follow-up): DOOM's
// RenderOpaque dispatch loop (fcn.141a37b80) opens/closes a GPU query per
// batch via func_0x141893680/func_0x141894620 - decompiled live, NOT the
// "descriptor cache" RE-NOTES originally guessed. Read-only, to see whether
// a redirected extra call reuses query indices the real call already used
// this same frame without an intervening vkCmdResetQueryPool - Vulkan
// requires a reset between uses of the same query index; reusing one
// unreset is undefined behaviour and driver-dependent.
using CmdBeginQueryFn = void(VKAPI_PTR*)(VkCommandBuffer, VkQueryPool, uint32_t, VkQueryControlFlags);
using CmdEndQueryFn = void(VKAPI_PTR*)(VkCommandBuffer, VkQueryPool, uint32_t);
using CmdResetQueryPoolFn = void(VKAPI_PTR*)(VkCommandBuffer, VkQueryPool, uint32_t, uint32_t);
// ScissorViewportProbe (docs/STEREO-RESTART-PLAN.md Task 3, 2026-08-11):
// redo of the scissor/viewport hooks removed 2026-08-11 after they crashed
// the game - that version was unguarded (fired on every vkCmdSetScissor/
// vkCmdSetViewport across the WHOLE game, thousands of times/second) and had
// zero SEH protection. This version is gated on inside_render_opaque (and
// excludes inside_extra_render_depth_reinvoke, see its declaration above -
// the same RenderDepth-mislabeling lesson PipelineDepthStencilProbe already
// hit), capturing only the FIRST rect seen per RenderOpaque call rather than
// every call, and SEH-wraps the capture.
using CmdSetScissorFn = void(VKAPI_PTR*)(VkCommandBuffer, uint32_t, uint32_t, const VkRect2D*);
using CmdSetViewportFn = void(VKAPI_PTR*)(VkCommandBuffer, uint32_t, uint32_t, const VkViewport*);
#endif  // DOOMVR_RESEARCH_TOOLS

CreateInstanceFn real_create_instance{};
EnumeratePhysicalDevicesFn real_enumerate_physical_devices{};
CreateDeviceFn real_create_device{};
GetDeviceQueueFn real_get_device_queue{};
QueueSubmitFn real_queue_submit{};
QueuePresentFn real_queue_present{};
// GPU-WAIT TIMING (2026-08-25). Pass-through hooks, added for one reason: the
// FrameBudget line attributed 35-53 ms/frame to RESIDUAL in the pause menu and
// I read that as "DOOM's own work, not ours". That reading was WRONG, and the
// control proved it - a post-cutscene menu ran at 80.0 fps before the clone
// pool fix and 21-30 fps after it. RESIDUAL includes the CPU BLOCKING ON THE
// GPU, and our doubled call loads the GPU twice, so "DOOM's time" and "waiting
// for work we added" were indistinguishable in that number. These two calls
// are where a Vulkan app actually blocks; timing them splits the difference.
PFN_vkAcquireNextImageKHR real_acquire_next_image{};
PFN_vkWaitForFences real_wait_for_fences{};
CreateSwapchainFn real_create_swapchain{};
GetSwapchainImagesFn real_get_swapchain_images{};
DestroySwapchainFn real_destroy_swapchain{};
#ifdef DOOMVR_RESEARCH_TOOLS
CmdPushConstantsFn real_cmd_push_constants{};
CreateBufferFn real_create_buffer{};
BindBufferMemoryFn real_bind_buffer_memory{};
MapMemoryFn real_map_memory{};
UnmapMemoryFn real_unmap_memory{};
FreeMemoryFn real_free_memory{};
UpdateDescriptorSetsFn real_update_descriptor_sets{};
CmdBindDescriptorSetsFn real_cmd_bind_descriptor_sets{};
CmdBindPipelineFn real_cmd_bind_pipeline{};
CmdBindVertexBuffersFn real_cmd_bind_vertex_buffers{};
CmdBindIndexBufferFn real_cmd_bind_index_buffer{};
CmdDrawIndexedIndirectFn real_cmd_draw_indexed_indirect{};
// Same signature as the indexed form. DrawParity (2026-08-18) is the first
// thing in this project ever to look at non-indexed indirect draws.
CmdDrawIndexedIndirectFn real_cmd_draw_indirect{};
// FINAL STAGE DOUBLING (2026-08-18) - DOOM+0x186E6BA. Four pointer args; see
// hook_final_stage for why that is the right shape of guess and why this
// function, unlike 0x189A350, is safe to hook.
using DoomFinalStageFn = void(__fastcall*)(void*, void*, void*, void*);
DoomFinalStageFn real_final_stage{};
CmdDrawFn real_cmd_draw{};
CmdDrawIndexedFn real_cmd_draw_indexed{};
CmdBeginQueryFn real_cmd_begin_query{};
CmdEndQueryFn real_cmd_end_query{};
CmdResetQueryPoolFn real_cmd_reset_query_pool{};
CmdSetScissorFn real_cmd_set_scissor{};
CmdSetViewportFn real_cmd_set_viewport{};
DoomRenderViewFn real_doom_render_view{};
DoomRenderSurfaceFn real_doom_render_surface{};
DoomRenderOpaqueFn real_doom_render_opaque{};
// Read-only diagnostic trampolines for the upstream visibility pipeline
// (Gather/Cull/WalkBSP/Sort) - see UpstreamPipelineProbe below. Found via
// the same job-registration-string technique used for RenderOpaque itself
// (docs/RE-NOTES.md): "RenderGatherPrepare"/"RenderGatherWorldSurfaces
// Prepare"/"RenderCull"/"RenderWalkBSP"/"RenderGatherFinish"/"RenderSort"
// job registrations resolved to these function addresses via radare2.
DoomRenderOpaqueFn real_render_gather_prepare{};
DoomRenderOpaqueFn real_render_gather_world_surfaces_prepare{};
DoomRenderOpaqueFn real_render_cull{};
DoomRenderOpaqueFn real_render_walk_bsp{};
DoomRenderOpaqueFn real_render_gather_finish{};
DoomRenderOpaqueFn real_render_sort{};
// Sibling Gather*Prepare jobs (2026-08-09) - UpstreamPipelineReinvoke
// Stage A's first live test only called RenderGatherWorldSurfacesPrepare,
// producing a visually SMALLER (not corrupted) redraw: world geometry only,
// missing whatever these siblings normally contribute (models, decals,
// lights, always-added objects) - the black square/shrink was almost
// certainly incomplete content, not a viewport bug. Found via the same
// job-registration-string technique.
DoomRenderOpaqueFn real_render_gather_models_prepare{};
DoomRenderOpaqueFn real_render_gather_decals_prepare{};
DoomRenderOpaqueFn real_render_gather_lights_prepare{};
DoomRenderOpaqueFn real_render_gather_add_always{};
// Phase 1 (STEREO-IMPLEMENTATION-PLAN.md v2). RenderDepth at DOOM+0x1A36C10,
// found via the same job-registration-string radare2 technique as every
// other job above.
DoomRenderOpaqueFn real_render_depth{};
// Lever G target (2026-08-10). RenderGuiModels at DOOM+0x17b35c0 - not on
// the world visibility pipeline; renders viewmodel/HUD 3D via its own
// per-context view matrix. See apply_lever_g_mutation() for the exact
// intervention.
DoomRenderOpaqueFn real_render_gui_models{};

// Lever V target (2026-08-11). SetupView (gui variant) at DOOM+0x17b3d70.
// Decompile of this fn (see docs/RE-NOTES.md) shows the critical multiply
// on line 382:
//   fcn.1402c5840(arg1_01 + 0x1170, arg2_00 + 0x11F0, arg1_00 + 0x4955A4);
//                 [projection]      [view matrix]     [output MVP]
// where arg1_01 = arg2_00 = *(SetupView_arg1 + 0x18). Hooking here and
// mutating the source view matrix at (*(arg1+0x18)) + 0x11F0 BEFORE the
// real fn runs propagates the mutation into the MVP at renderer+0x4955A4,
// which is what every downstream GUI/viewmodel draw actually reads.
// Correct level for the whole GUI pass.
DoomRenderOpaqueFn real_setup_view_gui{};

// Lever W target (2026-08-11). SetupView (world variant) at DOOM+0x17b32e0.
// Decompile shows the 6-destination copy loop at lines 106-156: reads
// arg1_00 = **0x146fd5fb8 (renderer.first_field = CameraSrc pointer) and
// copies ~8KB from CameraSrc into each of [0x146fd5fc0]..[0x146fd5fe8].
// Lever A modifies renderView+0xC44 which is ONE of those 6 destinations
// - the other 5 keep the un-shifted value, and different downstream jobs
// read different destinations, producing mid-terrain glitchiness. Lever W
// mutates the SOURCE (CameraSrc+0xC44) BEFORE the copy runs, so all 6
// destinations inherit the shift coherently. Same pattern as Lever V but
// for the world path.
DoomRenderOpaqueFn real_setup_view_world{};

// VIEWSRC target (2026-08-29). BuildViewMatrices at DOOM+0x1830B80 - the
// PRE-CULL CAMERA PRODUCER. Verified this session against the binary, not
// quoted from a standing doc:
//   * pescan pdata 1830B80 -> begin=1830B80 ... real function 1830B80, so the
//     address is a genuine .pdata root and the 5-byte prologue
//     (mov [rsp+0x10],rbx) is a legal MinHook target. Two mid-function hooks
//     have already bitten this project; this one was checked first.
//   * pescan callers 1830B80 -> five sites, ALL inside 0x1554CB0, which is the
//     front end that builds the stride-0xD8 view-descriptor array RenderViews
//     (0x186E520) later iterates. So this runs BEFORE the frame root and
//     before A1's RenderGatherPrepare hook.
//   * Each call site does 0x375200(viewParms, gameRecord+0x20) - a field copy
//     of the GAME's own render view into this render-side object - and then
//     calls this function. viewParms+0x60/+0x6C (vieworg / idMat3 viewaxis)
//     are therefore refreshed every call, which is what makes a persistent
//     write here idempotent instead of accumulating.
// Signature read off the call sites: rcx = viewParms, rdx = int32 rect[4],
// r8b = an alt-scale flag. r8b is declared as a full uint64_t deliberately:
// the `mov r8b,1` site writes only the low byte, so the upper bits are
// caller garbage and must be masked, not trusted.
using DoomBuildViewMatricesFn = void(__fastcall*)(void*, void*, uint64_t);
DoomBuildViewMatricesFn real_build_view_matrices{};
constexpr uintptr_t kViewParmsVieworgOffset = 0x60;   // vec3
constexpr uintptr_t kViewParmsViewaxisOffset = 0x6C;  // idMat3 {fwd, left, up}
constexpr uintptr_t kViewParmsWorldVpOffset = 0x1270; // proj*view -> renderView+0xC44
constexpr uintptr_t kViewParmsIdentityFlagOffset = 0xD8;  // != 0 -> view matrix forced identity
// ===== THE SSR PREVIOUS-FRAME BLOCK (2026-08-31) ==========================
//
// Read out of the binary, not inferred: DOOM+0x1830BB2..0x1830C55, the first
// thing BuildViewMatrices does on entry, is a run of movups that copies three
// 64-byte matrices into one contiguous 192-byte previous-frame block:
//
//     viewParms+0x11F0 -> +0x1510      (also Lever V's target)
//     viewParms+0x1130 -> +0x1550
//     viewParms+0x12B0 -> +0x1590
//
// i.e. `prev := cur`, EVERY CALL. That single fact retires the standing claim
// in hook_build_view_matrices that "0x1830B80 is a pure function of (object,
// rect, flag) ... a second call leaves exactly the state the engine would
// have had". It is not pure: a second call overwrites the previous-frame
// history with the FIRST call's own output, and the history is what SSR
// reprojects against (freqHigh prevglobalpostowindow - measured differing
// between the two passes in doomvr_frame8976.rdc while the freqLow current
// camera was byte-identical, because freqLow is per-FRAME and shared).
//
// Under this build the producer runs FOUR times per displayed frame: twice
// for doubling (one per pass) and twice again because the precull write calls
// it a second time per pass. So prev is never last frame - it is whatever the
// immediately preceding internal call produced.
// RE FACT, kept after PREVFIX was deleted 2026-09-03: the previous-matrix
// block is a contiguous 3 x 64 bytes at viewParms+0x1510, and the producer
// sources it from +0x11F0, +0x1130 and +0x12B0.
constexpr uintptr_t kRendererPtrGlobalRva = 0x6FD5FB8;  // VA 0x146fd5fb8 - imagebase
// Retargeted 2026-08-11 from 0xC44 to 0x3AC0 after the CameraSrc scan
// showed the real world view matrix source is at 0x3AC0 (row0=(-0.033,
// -0.999, 0, -2964.52) with world-scale translations). 0xC44 was the
// destination-side offset; the SOURCE struct has a different layout.
constexpr size_t kCameraSrcWorldVpOffset = 0x3AC0;
// The draw-command-append helper (DOOM+0x1A3CA89, decompiled in
// docs/RE-NOTES.md - the function that increments the shared pass-slot
// table's +0x38000 count and writes one 0x30-byte entry). Disassembly of
// its prologue shows 6 real parameters: RCX, RDX, R8, R9, then two stack
// args at [entryRSP+0x28]/[entryRSP+0x30] - the LAST of which is the
// pass-table pointer AppendDrawCommandProbe below reads. All 6 declared
// (even though only the last is meaningfully read) purely so a passthrough
// hook preserves the exact calling convention.
using AppendDrawCommandFn = void(__fastcall*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
AppendDrawCommandFn real_append_draw_command{};

// ---------------------------------------------------------------------------
// FrameRootProbe (PageUp, 2026-08-16) - read-only, bounded, human-triggered.
//
// Static analysis this session (radare2 + r2ghidra + a PE .pdata/.rdata
// scan, see docs/FRAME-ROOT-FINDINGS-2026-08-16.md) located DOOM's real
// per-view frame root and found that the sync-vs-async selector at that
// level is NOT the global at DOOM+0x7061830 this project has been reading.
// Both facts need live confirmation before anything is built on them, and
// both come out of one capture.
//
//   RenderViews (job, DOOM+0x186E520)
//     -> loops *(ctx+0x88) views, stride 0xd8, building a view descriptor
//        on its own stack, then calls:
//   FrameRoot (DOOM+0x17B7900)(renderer_ctx, view_desc)
//     -> SetupView / ComputePortalVisibility / RenderCull / RenderWalkBSP
//        / the 8-job RenderGather* chain / SetupView / RenderSort
//        / RenderView, EACH dispatched as:
//            if (*(view_desc + 0x11) == 0)  (**job_slot)(args);   // INLINE
//            else { Enqueue(queue, *job_slot, args); Barrier(queue); }
//   RenderView (job, DOOM+0x17B36B0)
//     -> 20 dispatches through JobDispatch (DOOM+0x1A33090), which is the
//        one that checks the global flag.
//
// So there are two independent selectors, at two different levels. This
// probe reads both, mutates nothing.
using DoomFrameRootFn = void(__fastcall*)(void*, void*);
DoomFrameRootFn real_frame_root{};
// JobDispatch's true signature, re-verified by disassembly this session -
// it takes FOUR arguments, not the three docs/RE-NOTES.md recorded:
//   (job_ctx /*rcx*/, job_fn /*rdx*/, job_arg /*r8*/, wait_flag /*r9b*/)
// The synchronous branch is `mov rcx, r8; call rdx` - i.e. job_fn(job_arg),
// with rcx and r9b unused. The async branch ignores rdx/r8 entirely and
// enqueues via *(rcx+0x18).
using DoomJobDispatchFn = void(__fastcall*)(void*, void*, void*, uint8_t);
DoomJobDispatchFn real_job_dispatch{};

CmdBeginRenderPassFn real_cmd_begin_render_pass{};
CmdEndRenderPassFn real_cmd_end_render_pass{};
CmdBeginRenderPass2Fn real_cmd_begin_render_pass2{};
CmdBeginRenderPass2Fn real_cmd_begin_render_pass2_khr{};
CmdBeginRenderingFn real_cmd_begin_rendering{};
CmdBeginRenderingFn real_cmd_begin_rendering_khr{};
CreateRenderPassFn real_create_render_pass{};
CreateRenderPass2Fn real_create_render_pass2{};
CreateRenderPass2Fn real_create_render_pass2_khr{};
CreateFramebufferFn real_create_framebuffer{};
CreateImageViewFn real_create_image_view{};
CreateImageFn real_create_image{};
// Resource lifetime (2026-08-17, review risk 2) - see hook_destroy_framebuffer.
PFN_vkDestroyFramebuffer real_destroy_framebuffer{};
PFN_vkDestroyImageView real_destroy_image_view{};
// STEP 6 read-side redirection - see the ReadRedirect block below.
PFN_vkAllocateDescriptorSets real_allocate_descriptor_sets{};
// Barrier substitution - see hook_cmd_pipeline_barrier.
PFN_vkCmdPipelineBarrier real_cmd_pipeline_barrier{};
// COMPUTE PROBE - see hook_cmd_dispatch.
PFN_vkCmdDispatch real_cmd_dispatch{};
PFN_vkCmdDispatchIndirect real_cmd_dispatch_indirect{};
// Sets bound at the COMPUTE bind point on this thread, so a dispatch can be
// asked what it is about to write. thread_local rather than a map keyed on
// command buffer: r_useSMP=1 is a hard precondition of doubling at all, so
// recording is inline on one thread by construction.
thread_local std::vector<VkDescriptorSet> last_bound_compute_sets;
// Parallel to the above - see the assignment site for why the byte range
// matters and cannot be recovered from the set handle alone.
thread_local std::vector<uint32_t> last_bound_compute_dynoffsets;
thread_local uint32_t last_bound_compute_first_set = 0;
// R3 (2026-08-27). WHAT WAS ACTUALLY HANDED TO THE DRIVER, parallel to
// last_bound_compute_sets, one entry per set.
//
// R2's key-error control failed on exactly this: the manifest asked
// df_clone_effective "does a clone exist for this source set", which is not the
// same question as "was a clone bound at this dispatch". Redirection runs only
// inside the doubled call, so the REAL call binds DOOM's sets unmodified - and
// the manifest reported realBoundToOurMirror=11 where it must report 0, since
// both calls bind the same SOURCE set handles. The doubled side's answer was
// corroborated elsewhere and is very likely right, but a probe whose control
// fails does not get to keep the half of its output that looks correct.
//
// VK_NULL_HANDLE, or the source handle itself, means no clone was bound here.
//
// R6: keyed BY SOURCE SET rather than positionally, because the same fact is
// now needed for GRAPHICS binds too and those are tracked as (setIndex, set)
// pairs updated incrementally - there is no position to be parallel to. One
// mechanism for both bind points; the realBoundToOurMirror control re-verifies
// the keying on every run.
thread_local std::map<VkDescriptorSet, VkDescriptorSet> df_bound_clone_by_set;

// Defined below, beside dispatch_fate_record; declared here because the draw
// hooks are compiled far above it.
void dispatch_fate_record_draw();

// R7: the DYNAMIC OFFSETS of the last graphics bind, per side.
//
// R6 closed the resource question for the depth-downsample draws: both calls
// bind the SAME source view, the doubled one is substituted to the depth mirror
// measured at 100.6%, and the pass has exactly ONE image binding. DOOM never
// calls vkCmdPushConstants (measured, 2026-08-10), so everything else that draw
// reads arrives through descriptor BUFFERS - and DOOM ring-allocates a separate
// constant slot per call, which the redirect deliberately passes through
// untouched. Which slot each side was pointed at has never been recorded.
thread_local std::vector<uint32_t> last_graphics_dynamic_offsets;

// X1 - RING-SLOT REPLAY. See the long note at the top of
// hook_cmd_bind_descriptor_sets. Recording is unconditional (one map insert per
// bind with dynamic offsets, on the real call only); only the SUBSTITUTION is
// behind the flag, so arming it cannot depend on a recording window that was
// never open.
// Compile-time master switch, so turning the lever off is a one-line change and
// not an archaeology exercise (the lesson of scripted_test_symmetric).
// ⚠ TURNED OFF 2026-08-27 after its own run. The lever fired cleanly
// (replayed=1,597,589, missing=1) and took hold - eye 1's depth became
// BIT-IDENTICAL to eye 0's - and it did NOT restore any effect. It also made
// the picture visibly WORSE: the owner reported geometry flashing in the right
// eye (walls, or the door of the starting room). That is what a per-object
// constant slot bound to the wrong object looks like, and it is not surprising:
// the replay keys on (pass, bindPoint, bind ordinal), which assumes the two
// calls issue binds in lockstep, and pass 12 alone has 384 per-object draws.
// The hypothesis it tested is dead either way - identical constants did not
// move a single broken target - so the key alignment is not worth repairing.
constexpr bool kX1ReplayRingSlots = false;
std::atomic_bool x1_replay_ring_offsets{false};
std::atomic_uint32_t x1_replayed{0};
std::atomic_uint32_t x1_missing{0};
std::atomic_uint32_t x1_count_mismatch{0};
thread_local std::map<uint64_t, std::vector<uint32_t>> x1_real_dyn;
thread_local uint32_t x1_real_bind_seq = 0;
thread_local uint32_t x1_doubled_bind_seq = 0;
// DeferredInputMap (2026-08-19) needs the GRAPHICS sets too, and needs them
// for BOTH calls - the whole point is comparing what the real call's deferred
// draw samples against what the doubled call's samples. Recorded with the set
// INDEX because a binding is only identified by (setIndex, binding).
// Assigned at the top of hook_cmd_bind_descriptor_sets, above every early
// return, for the same reason the storage-image mirroring sits there: three
// probes have already died silently below those returns.
thread_local std::vector<std::pair<uint32_t, VkDescriptorSet>> last_bound_graphics_sets;
std::atomic_uint32_t doubled_dispatch_count{0};
std::atomic_uint32_t doubled_storage_mirrored{0};
std::atomic_uint32_t doubled_storage_unmirrored{0};
std::atomic_uint32_t doubled_dispatch_lines{0};
// TRANSFER PROBE - see hook_cmd_copy_image. Same shape of gap as the
// barriers were: these commands name images directly and were never hooked,
// so inside the doubled call they read and write DOOM's REAL images with no
// substitution at all.
PFN_vkCmdCopyImage real_cmd_copy_image{};
PFN_vkCmdBlitImage real_cmd_blit_image{};
PFN_vkCmdResolveImage real_cmd_resolve_image{};
std::atomic_uint32_t doubled_transfer_count{0};
std::atomic_uint32_t doubled_transfer_src_unmirrored{0};
std::atomic_uint32_t doubled_transfer_dst_unmirrored{0};
std::atomic_uint32_t doubled_transfer_lines{0};
// COVERAGE PROBE (2026-08-17). Enumerated from source rather than
// discovered one live run at a time, which is how barriers and transfers
// were found - each after it had already cost runs.
//
// Mirroring substitutes FRAMEBUFFERS. Every Vulkan command that names an
// image, an image view, or a descriptor by handle bypasses it. Checking all
// of them at once:
//
//   vkCmdPushDescriptorSetKHR      GAP - names image views INLINE, never
//                                  passing through vkUpdateDescriptorSets or
//                                  a VkDescriptorSet at all. Invisible to
//                                  the census AND unreachable by
//                                  ReadRedirect, which can only substitute a
//                                  set it can clone. If DOOM's post chain
//                                  uses these, that is run 10's answer.
//   vkUpdateDescriptorSetWithTemplate  GAP - contents we never see, so a set
//                                  can look partially recorded rather than
//                                  unrecorded (the census's noRecord=0 only
//                                  covers sets with SOME record).
//   vkCmdPipelineBarrier2 / vkCmdWaitEvents  GAP - barrier substitution
//                                  covers only the Vulkan 1.0 entry point.
//   vkCmdCopyBufferToImage         GAP - writes images directly.
//
// All log-only, all on the existing census arm. One run answers all five.
PFN_vkCmdPushDescriptorSetKHR real_cmd_push_descriptor_set{};
PFN_vkCmdPipelineBarrier2 real_cmd_pipeline_barrier2{};
PFN_vkCmdWaitEvents real_cmd_wait_events{};
PFN_vkCmdCopyBufferToImage real_cmd_copy_buffer_to_image{};
PFN_vkCreateDescriptorUpdateTemplate real_create_descriptor_update_template{};
PFN_vkUpdateDescriptorSetWithTemplate real_update_descriptor_set_with_template{};
std::atomic_uint32_t doubled_push_descriptor_count{0};
std::atomic_uint32_t doubled_push_images_mirrored{0};
std::atomic_uint32_t doubled_push_images_unmirrored{0};
std::atomic_uint32_t doubled_barrier2_count{0};
std::atomic_uint32_t doubled_wait_events_count{0};
std::atomic_uint32_t doubled_copy_buffer_to_image_count{0};
std::atomic_uint32_t update_template_count{0};
std::atomic_uint32_t mirror_destroy_count{0};
std::atomic_uint32_t mirror_view_destroy_count{0};
CreateGraphicsPipelinesFn real_create_graphics_pipelines{};
// Set only for the thread currently inside the real RenderOpaque call (see
// hook_doom_render_opaque) so the render-pass-begin hooks below log only
// the target(s) this specific pass uses, not DOOM's other ~47 render jobs.
thread_local bool inside_render_opaque = false;
// Set only around the F7 double-call's SECOND (injected/extra) invocation of
// RenderOpaque - see the F6 offscreen-redirect experiment below. Lets
// hook_cmd_begin_render_pass tell "this vkCmdBeginRenderPass belongs to our
// own extra call" apart from "this is DOOM's real, once-per-frame call",
// without which redirecting output would also corrupt DOOM's own frame.
thread_local bool inside_injected_extra_call = false;
// PipelineDepthStencilProbe scoping fix (docs/STEREO-RESTART-PLAN.md Task 3,
// 2026-08-11): inside_render_opaque stays true for the ENTIRE StereoDualEyeDump
// extra-call bracket, including the real_render_depth(render_depth_ctx)
// reinvocation that runs immediately before each eye's RenderOpaque call - so
// without this flag, hook_cmd_bind_pipeline's probe silently captured
// RenderDepth's own pipeline binds too, mislabeled as RenderOpaque's "extra"
// pipelines. Caught live: a first pass showed every extra-call pipeline with
// stencilTestEnable=1 vs every real-call pipeline with stencilTestEnable=0 -
// looked like Task 3's stencil divergence, until cross-checking exact
// VkPipeline handles within one session showed the pipelines that genuinely
// appear in BOTH real and extra (RenderOpaque's own) agree exactly on
// stencil state; the stencilTestEnable=1 set never appears in the real
// call's list at all - they're RenderDepth's distinct pipeline set, not
// RenderOpaque's. Set true only around that specific reinvocation so the
// probe can exclude it.
thread_local bool inside_extra_render_depth_reinvoke = false;
// RealFrameQueryGateProbe (docs/STEREO-RESTART-PLAN.md Task 2, 2026-08-11):
// set by hook_cmd_begin_query whenever a query-gated draw batch is seen
// during the REAL (non-extra) RenderOpaque call, read and reset once per
// real frame right after that call returns (see hook_doom_render_opaque).
// Distinguishes "the two zero-event extra-call tests were sampling
// variance" from "a real bug" without needing RenderDoc - see QueryPoolProbe
// below for the mechanism this observes.
thread_local bool query_gate_seen_this_real_frame = false;
// DOUBLED-CALL OCCLUSION QUERIES - see hook_cmd_begin_query for why replaying
// them is undefined and why skipping them is the safe direction.
//
// These counters exist because the QueryPoolProbe could never have answered
// this: it is gated on inside_render_opaque and labels by
// inside_injected_extra_call, and the doubled call sets NEITHER - so its
// "115064 begins, all callKind=real, zero from the doubled call" was a
// blind spot being read as a measurement, not evidence of asymmetry.
std::atomic_uint64_t doubled_query_begins{0};
std::atomic_uint64_t doubled_query_ends{0};

// PRIVATE QUERY POOL FOR THE DOUBLED CALL (2026-08-19). The spec-correct fix,
// replacing the skip that froze the game.
//
// Measured: begins=23308 ends=23308. The doubled call issues balanced query
// pairs, but onto DOOM's pools, on slots the real call has already written and
// which nothing has reset since. Beginning an unreset query is undefined, and
// a driver may drop the draws it wraps - which is the only mechanism found
// that discards draws while every input, state and recorded command stays
// byte-identical, and it fits that ONLY query-gated batches (the effects) are
// lost while ordinary geometry survives.
//
// Skipping the calls was wrong twice over: it froze on the first try, and the
// balanced counts show there was never an imbalance to fix. Giving the doubled
// call its OWN pool keeps every begin/end exactly as DOOM recorded it, makes
// each one legal because we reset our pool ourselves, and leaves DOOM's real
// results untouched - we never read ours.
constexpr uint32_t kPrivateQuerySlots = 8192;
VkQueryPool private_query_pool = VK_NULL_HANDLE;
std::atomic_bool private_query_pool_failed{false};
// Occlusion queries cannot nest, so one live index is enough to pair a begin
// with its end. Reset per doubled call along with the pool.
thread_local uint32_t doubled_query_next = 0;
// THE PAIRING INVARIANT (rebuilt 2026-08-19 after the second freeze).
//
// The end must do EXACTLY what the immediately preceding begin did, decided by
// what that begin actually recorded - never by re-evaluating a flag or
// comparing running totals, which is what the frozen version did.
//
// Vulkan forbids nesting two queries of the same type in one command buffer,
// so at most ONE occlusion query is open at a time and a single slot of state
// is sufficient to remember it. Set only by a begin that redirected, cleared
// by the end that consumes it:
//   redirect_active == true   the open query is on OUR pool at active_slot,
//                             so the end goes to our pool at that slot
//   redirect_active == false  the open query is on DOOM's pool, so the end
//                             goes through untouched
// There is no third case, so an end can never target a pool its begin did not
// use - which is the specific way the previous version could hang.
thread_local bool doubled_query_redirect_active = false;
thread_local uint32_t doubled_query_active_slot = 0;
// RELEVANCE TEST (2026-08-19). Settles whether the query hypothesis can matter
// at all, before another freeze is spent trying to act on it.
//
// The hypothesis is that the doubled call re-begins queries on DOOM's unreset
// slots and the driver discards the draws they wrap. That can only explain the
// missing effects if the pass-25 effects draws are actually INSIDE a query.
// Nothing has ever checked that - "DOOM query-gates draw batches" came from a
// comment about the real call, not from a measurement at pass 25.
//
// Tracks whether a query is open, and counts pass-25 draws both ways for both
// calls. Log-only, no behavioural change, cannot freeze anything.
//   gated > 0   the effects draws ARE query-wrapped; the hypothesis is live
//               and solving the reset hazard is worth another attempt
//   gated == 0  they are not; the hypothesis is irrelevant and gets dropped on
//               evidence, closing no door
thread_local bool query_open_now = false;
std::atomic_uint64_t p25_draws_gated[2]{};
std::atomic_uint64_t p25_draws_ungated[2]{};
// Depth-content comparison state - see kDepthStripBytes. Captured at the
// pass-25 boundary for both calls: DOOM's real depth, and OUR mirror of it.
bool depth_strip_captured[2] = {false, false};
VkImage depth_strip_source[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
std::atomic_bool depth_compare_ready{false};
// Set on the render thread when the strip copies have been RECORDED; consumed
// at PRESENT, once the GPU has actually run them. Reading on the render thread
// returns the memset zeros and voided two runs - the same disarm-here /
// read-there split IndirectArgs already uses.
std::atomic_bool depth_verdict_pending{false};
void emit_depth_content_verdict();
void emit_froxel_content_verdict();
thread_local bool doubled_query_pool_reset_this_call = false;
// BACK ON, with the pairing rebuilt (2026-08-19, third attempt).
//
// The two freezes both came from the SAME defect class - an end that did not
// mirror its own begin. v1 suppressed begin and end independently, each
// re-testing a flag. v2 paired them by comparing two session-cumulative
// atomics, so a begin that fell through to DOOM's pool on overflow would still
// have its end redirected to ours: ending a query never begun there, which is
// invalid and hangs.
//
// v3 removes the class rather than patching the instance. The begin RECORDS
// what it did in doubled_query_redirect_active, and the end does exactly that
// and nothing else - no flag re-read, no arithmetic. Queries of one type
// cannot nest, so a single slot of state is provably sufficient, and the
// writers are exhaustive: cleared at pool reset, cleared at every begin, set
// only by a begin that redirected, cleared by the end that consumes it. A
// stray end with no matching begin falls through untouched instead of
// inventing a query.
//
// OFF AGAIN - third freeze (2026-08-19). The pairing was not the fault.
//
// v3's invariant is sound and was verified by enumerating every writer, yet it
// froze exactly like v1 and v2. That rules the pairing OUT and says the hazard
// is in the pool work itself. The most likely candidate, unverified: we
// vkCmdResetQueryPool all 8192 slots at the start of every doubled call while
// the previous frame's submission may still be executing queries from that
// same pool, with no fence between them. Resetting a pool with queries in
// flight is undefined and can hang.
//
// I am NOT dropping the hypothesis - only this mechanism. The hypothesis is
// that the doubled call re-beginning queries on DOOM's unreset slots makes the
// driver discard the draws they wrap. Before spending another run or another
// freeze on a fix, the RELEVANCE test below settles whether it can possibly
// matter: if the pass-25 effects draws are not inside a query at all, the
// whole idea is irrelevant and gets dropped on evidence rather than fatigue.
std::atomic_bool redirect_doubled_queries{false};
std::atomic_uint64_t doubled_query_redirected{0};
std::atomic_uint64_t doubled_query_overflow{0};
// DEFAULT OFF after it froze the game on the first try (2026-08-19).
//
// Skipping begin and end independently is not safe: each is gated on
// inside_frame_root_double at the moment it runs, so if that flips between a
// query's begin and its end - or if the recorded stream ends a query begun
// outside the doubled call - the pair goes UNBALANCED. An unended query is
// never available, and DOOM waiting on its results with vkGetQueryPoolResults
// blocks forever. That is exactly the freeze PageUp produced.
//
// The counters below cost nothing and still answer the only question that
// matters right now: does the doubled call issue queries AT ALL? If it does
// not, the whole hypothesis is dead and no risky change was needed. If it
// does, the skip gets rebuilt properly - tracking which (pool, query) pairs WE
// skipped the begin of, and skipping only those ends - rather than by two
// independent guesses.
std::atomic_bool skip_doubled_queries{false};
// Milestone 1 (docs/STEREO-IMPLEMENTATION-PLAN.md §2.2/§3): -1 = no per-eye
// context (every existing mechanism above is unaffected). Set to 0 or 1
// immediately before each eye's redirected extra RenderOpaque call in the
// StereoDualEyeDump (F12) mechanism below, restored to -1 immediately after.
// Same bracket-and-restore shape as inside_injected_extra_call. Read by
// hook_cmd_begin_render_pass to route that eye's draw into its own
// dedicated stereo_eye_targets[] slot instead of the shared offscreen_targets[]
// pair.
thread_local int current_eye_render = -1;
// Set at the very top of hook_doom_render_opaque (before the real call runs)
// to true_stereo_attempt_count's current value whenever TrueStereo is armed,
// else UINT32_MAX. Lets both the real call's and the same invocation's later
// extra call tag their vkCmdBindDescriptorSets activity with the same frame
// number - see TrueStereoGpuProbe below, the diagnostic correlating a
// specific render-view to its actual GPU uniform-buffer slot (buffer +
// dynamic offset), the same way renderView+0xC44 was originally found.
thread_local uint32_t current_true_stereo_frame = UINT32_MAX;
// Cleared right before each extra-call invocation starts (see the
// true_stereo_enabled block in hook_doom_render_opaque). TrueStereoGpuWrite
// consults/inserts into this so a GPU uniform slot rebound by multiple
// draw commands within the SAME extra call only gets rotated once - without
// it, the second draw's bind reads back the first draw's already-rotated
// write and rotates it again, compounding N times across N draws into a
// garbled transform. Confirmed live: successive reads of one slot within
// one extra call showed exactly rotate(rotate(original)).
thread_local std::unordered_set<uintptr_t> true_stereo_rotated_addresses;
// Read-only candidate tracer's per-eye dedup.  Unlike the write dedup above,
// this deliberately applies only to the opaque call, so its small budget is
// not consumed by repeated RenderDepth bindings.
thread_local std::unordered_set<uintptr_t> stereo_dual_eye_candidate_addresses;
// One write per eye for the current Avenue 1 candidate experiment.  This is
// deliberately separate from the address dedup: the candidate selector below
// chooses the first matching record in command order, then this guard prevents
// any second qualifying record from being touched.
thread_local bool stereo_dual_eye_targeted_write_done = false;
thread_local bool stereo_dual_eye_depth_write_done = false;
thread_local uint32_t stereo_dual_eye_worldspace_128_seen = 0;
std::atomic_uint32_t render_pass_begin_sample_count{0};
constexpr uint32_t kMaxRenderPassBeginSamples = 300;
// vkCreateRenderPass/vkCreateFramebuffer are called at renderer init, not
// per-frame - unconditional bounded logging here is initialization-time
// diagnostic, not the render-hot-path logging FORBIDDEN-BOUNDARIES.md
// warns against. A first live test with a 256 render-pass cap confirmed
// DOOM creates far more than that in a single ~20ms startup burst (the cap
// was exhausted before reaching either of the two known RenderOpaque
// passes) - raised well past the observed burst size; still finite/
// self-limiting, not unconditional.
std::atomic_uint32_t render_pass_create_sample_count{0};
constexpr uint32_t kMaxRenderPassCreateSamples = 8192;
std::atomic_uint32_t framebuffer_create_sample_count{0};
constexpr uint32_t kMaxFramebufferCreateSamples = 2048;

// Phase 2.4 next step: a VkFramebuffer we own, compatible with whichever
// RenderOpaque VkRenderPass we capture live (never hardcoded - handles are
// per-session, see docs/RE-NOTES.md). Attachment formats ARE hardcoded
// below, sourced from this session's live vkCreateRenderPass diagnostic
// (docs/RE-NOTES.md, "RESOLVED by the new vkCreateRenderPass/
// vkCreateFramebuffer diagnostic") - like every other hardcoded value in
// this file, specific to the installed build until it goes through
// pattern_scan (see docs/RE-NOTES.md "Outstanding instrumentation gap").
struct OffscreenTarget {
    std::atomic_bool ready{false};
    VkRenderPass render_pass{VK_NULL_HANDLE};
    VkExtent2D extent{};
    std::array<VkImage, 3> color_images{};
    std::array<VkDeviceMemory, 3> color_memory{};
    std::array<VkImageView, 3> color_views{};
    VkImage depth_image{VK_NULL_HANDLE};
    VkDeviceMemory depth_memory{VK_NULL_HANDLE};
    VkImageView depth_view{VK_NULL_HANDLE};
    VkFramebuffer framebuffer{VK_NULL_HANDLE};
};
// RenderOpaque begins exactly two internal render passes per invocation
// (confirmed live, docs/RE-NOTES.md) - one framebuffer per slot, captured
// in whichever order they're actually seen (never assumed). First live
// redirect test only covered slot 0 (the first-seen, "clear color" pass)
// and read back as flat black - inconclusive on its own, since slot 0
// might legitimately carry only a small subset of RenderOpaque's internal
// draw batches while slot 1 (the "load/continue" pass) carries the rest.
// Covering both in one test is the direct way to find out.
std::array<OffscreenTarget, 2> offscreen_targets;
// Framebuffer over offscreen_targets[0] images bound to pass 2 render pass.
// See the SHARED-IMAGE FIX note in hook_cmd_begin_render_pass.
VkFramebuffer offscreen_pass2_shared_fb = VK_NULL_HANDLE;
std::atomic_uint32_t offscreen_targets_captured{0};
constexpr std::array<VkFormat, 3> kOffscreenColorFormats{
    VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM};
constexpr VkFormat kOffscreenDepthFormat = VK_FORMAT_D24_UNORM_S8_UINT;

// F6: whether the F7 double-call's extra invocation should have its
// vkCmdBeginRenderPass redirected into the matching offscreen_targets slot
// (see hook_cmd_begin_render_pass). Composes with F7 rather than duplicating
// it - F6 alone (F7 off) does nothing, since there is no extra call to
// redirect; this does NOT add its own extra RenderOpaque call, only
// changes where F7's existing one's output goes. Off by default,
// independently self-limited to kMaxRedirectAttempts (tighter than F7's
// own 60-attempt cap, given the elevated risk below) and counted only on
// attempts where redirection actually engages - this budget is now SHARED
// across both offscreen_targets slots (a single F7 extra call can redirect
// both of RenderOpaque's internal render passes back-to-back, each
// consuming one unit), so ~10 full F7 attempts get both passes redirected
// before the cap disables further redirection.
//
// Real risk this experiment carries beyond prior ones: a genuinely
// incompatible framebuffer can cause GPU-level corruption or a driver
// TDR/hang, which - unlike a CPU access violation - __try/__except cannot
// catch or recover from. Keep attempts low and watch the headset closely
// the first time this is armed.
std::atomic_bool redirect_extra_call_enabled{false};
constexpr uint32_t kMaxRedirectAttempts = 20;
std::atomic_uint32_t redirect_attempt_count{0};

// One-shot per slot: the first time each offscreen_targets slot's
// redirection actually succeeds, snapshot its color image to disk so its
// content can be visually checked - "didn't crash" (confirmed) is not the
// same as "drew correct geometry" (still unknown at that point). See
// dump_offscreen_color_snapshot.
std::array<std::atomic_bool, 2> offscreen_dump_pending{false, false};
std::array<std::atomic_bool, 2> offscreen_dump_done{false, false};

// StereoDualEyeDump (F12, Milestone 1): dedicated per-eye targets, separate
// from offscreen_targets[] above so eye 0 and eye 1's output can't overwrite
// each other before both get dumped (offscreen_targets[] is keyed only by
// render-pass identity, shared across whichever single extra call is
// currently armed - fine for F1/F2/F6/F7's "one extra call" mechanisms, not
// fine here where TWO extra calls happen back-to-back in the same frame and
// both need to survive to the dump). Reduced scope, deliberately: only the
// SECOND render pass RenderOpaque begins is covered here, matching
// offscreen_targets[1]/slot1 - this is the world-path-only fallback the
// plan itself sanctions if the full GUI/weapon path turns out to have its
// own complications (docs/STEREO-IMPLEMENTATION-PLAN.md Milestone 1's
// "Fallback if broken in some other way"). Originally keyed on the FIRST
// pass (offscreen_targets[0]/slot0) instead; the first live capture
// (2026-08-11) read back flat black on both eyes - every historical
// slot0/slot1 dump in this project's log confirms slot0 is always empty
// ("clear color" only) while slot1 always carries real geometry, so this
// was corrected to key on [1]. The FIRST (empty) pass isn't captured
// per-eye; it falls through to the existing shared offscreen_targets[]
// handling below (never onto the real screen), which is harmless but not
// meaningful data - RenderGuiModels is out of scope for this pass
// entirely, so no weapon/HUD divergence yet.
std::array<OffscreenTarget, 2> stereo_eye_targets;
std::array<std::atomic_bool, 2> stereo_eye_dump_pending{false, false};
std::array<std::atomic_bool, 2> stereo_eye_dump_done{false, false};
// Phase 1 (STEREO-IMPLEMENTATION-PLAN.md v2): RenderDepth's own render pass
// (separate from RenderOpaque's - depth-only, 1 attachment), captured once
// from the real invocation (see inside_render_depth), plus one private
// depth-only framebuffer per eye wrapping that SAME eye's private depth
// image (stereo_eye_targets[eye].depth_view) - RenderDepth writes it,
// RenderOpaque's own per-eye framebuffer (already built by
// prepare_offscreen_target, now passed VK_NULL_HANDLE instead of DOOM's
// real depth view - see the fix in hook_cmd_begin_render_pass) reads it.
std::atomic<VkRenderPass> render_depth_captured_pass{VK_NULL_HANDLE};
std::array<VkFramebuffer, 2> stereo_eye_depth_framebuffers{VK_NULL_HANDLE, VK_NULL_HANDLE};
std::array<std::atomic_bool, 2> stereo_eye_depth_framebuffer_ready{false, false};
// PassDrawCountProbe follow-up fix (docs/STEREO-RESTART-PLAN.md Task 3,
// 2026-08-11): RenderOpaque's FIRST internal pass (offscreen_targets[0],
// Clear) was confirmed to draw real content (PassDrawCountProbe: exactly 1
// draw command, every real frame, never 0) - not a bare clear as previously
// assumed. The per-eye redirect only ever captured the SECOND pass
// (stereo_eye_targets[eye]/offscreen_targets[1], Load); pass 1 fell through
// to the shared, non-per-eye offscreen_targets[0] for both eyes, every
// time. Fix: a SECOND VkFramebuffer per eye, wrapping the SAME color/depth
// VkImageViews stereo_eye_targets[eye] already owns, built against
// offscreen_targets[0]'s render pass instead - same pattern DOOM's own real
// setup already uses (both its real passes share identical VkImageView
// handles across two different VkFramebuffer/VkRenderPass pairs, confirmed
// in docs/RE-NOTES.md). This does NOT allocate new images - pass 1 and
// pass 2 must render into the literal same physical images for pass 2's
// loadOp=LOAD to see what pass 1 drew, exactly like DOOM's own real frame.
std::array<VkFramebuffer, 2> stereo_eye_pass1_framebuffers{VK_NULL_HANDLE, VK_NULL_HANDLE};
std::array<std::atomic_bool, 2> stereo_eye_pass1_framebuffer_ready{false, false};
// RealCallRedirectProbe (docs/STEREO-RESTART-PLAN.md Task 3, control test,
// 2026-08-11): a different KIND of test than every prior candidate check -
// not "real inputs vs extra inputs" but "the real call's OWN output,
// redirected through the exact same per-eye-style mechanism (private depth
// via a redirected RenderDepth, private G-buffer via redirected pass1+
// pass2), camera completely untouched." Every prior eye0/eye1 dump came
// from the EXTRA call. The one prior evidence the redirect mechanism can
// show correct content (the old F6/F7 test) used the OLDER offscreen_targets[]
// setup, which reuses DOOM's REAL depth view directly - not this session's
// stereo_eye_targets setup with a PRIVATE synthetic depth populated by a
// redirected RenderDepth call. Those are not proven to be the same code
// path. This target mirrors stereo_eye_targets[eye] exactly but is driven
// by the literal real, once-per-frame RenderDepth+RenderOpaque calls - NOT
// a reinvocation - kept strictly separate from inside_injected_extra_call
// (see the redirect gate in hook_cmd_begin_render_pass) so it can't
// interfere with anything already working. One-shot, human-triggered
// (END): fires on the NEXT real RenderDepth+RenderOpaque sequence only,
// then auto-disarms - the real frame's actual depth buffer and screen
// output are diverted away for exactly that one frame (a visible one-frame
// glitch is the expected, accepted cost of this test, not a bug).
OffscreenTarget real_call_redirect_target;
VkFramebuffer real_call_redirect_pass1_framebuffer{VK_NULL_HANDLE};
VkFramebuffer real_call_redirect_depth_framebuffer{VK_NULL_HANDLE};
std::atomic_bool real_call_redirect_pass1_ready{false};
std::atomic_bool real_call_redirect_depth_ready{false};
std::atomic_bool real_call_redirect_armed{false};
std::atomic_bool real_call_redirect_dump_pending{false};
std::atomic_bool real_call_redirect_dump_done{false};
// RenderDepthPipelineProbe (docs/STEREO-RESTART-PLAN.md Task 3, Round 5
// step 2 check (b)): the depth dump above (step 1) came back essentially
// uniform far-plane (minDepth=0.9996, 100% of pixels >=0.9999) - not a
// plausible gradient, so the bug is upstream of consumption, per the plan's
// own branch. Nothing has ever captured RenderDepth's OWN pipelines'
// depthTestEnable/depthWriteEnable when its real, once-per-frame invocation
// is redirected through this mechanism (PipelineDepthStencilProbe
// deliberately EXCLUDES RenderDepth's pipelines via
// inside_extra_render_depth_reinvoke - see its declaration - because that
// flag only brackets the REINVOKED RenderDepth call used by the extra-call
// mechanism, not this one, which goes through the real invocation and
// inside_render_depth instead). Gated on real_call_redirect_armed so this
// only ever fires during the one END-armed redirected frame, never every
// ordinary real frame - same one-shot scoping as the rest of
// RealCallRedirectProbe, not a new always-on hot-path cost.
std::atomic_uint32_t render_depth_pipeline_probe_samples{0};
constexpr uint32_t kMaxRenderDepthPipelineProbeSamples = 64;
// RenderDepthDrawCountProbe - see CmdDrawFn's declaration above for the
// full reasoning. Logs inline, live, at the actual vkCmdDraw*/
// vkCmdDrawIndexed* call site - NOT accumulated and logged after
// real_render_depth() returns (a first version did that and never printed
// a single line: RenderDepth turns out to run as a separate job/thread
// from RenderOpaque's pass1/pass2 recording, not strictly sequential with
// it, so by the time hook_render_depth's post-call check ran,
// real_call_redirect_armed had already raced to false on the other thread
// - confirmed live via interleaved RenderDepthPipelineProbe/pass2-disarm
// log lines, 2026-08-12). Same reasoning is exactly why
// RenderDepthPipelineProbe above already logs inline instead of
// accumulating - mirror that proven-working pattern instead of
// reinventing it.
std::atomic_uint32_t render_depth_draw_probe_samples{0};
constexpr uint32_t kMaxRenderDepthDrawProbeSamples = 64;
std::atomic_bool stereo_dual_eye_enabled{false};
// Avenue 1 follow-up control (2026-08-13): keep StereoDualEyeDump's exact
// RenderDepth reinvocation, per-eye framebuffer redirects, and scheduling,
// but suppress only the GPU camera-memory write. The real-depth version of
// the test still blackened near-field geometry despite successful +/-3uu
// writes. This false-by-default build-time control determines whether those
// writes are causal or whether any manufactured extra call is incomplete.
// It is safer than the preceding test: it removes writes and introduces no
// extra invocation, target, or hotkey beyond the existing bounded INSERT
// path.
// Enabled only for the single-record Avenue 1 test below.  The write gate
// additionally requires the first depth-writing opaque-world pipeline, so it
// cannot repeat the old all-candidates sweep.
std::atomic_bool stereo_dual_eye_camera_write_enabled{false};
std::atomic_uint32_t stereo_dual_eye_attempt_count{0};
// Small cap: two redirected extra RenderOpaque calls per attempt (double
// the per-attempt cost of F7's single extra call), and this mechanism is
// genuinely new territory (divergent, not identical, cameras feeding a
// shared depth buffer - see plan §2.3). Keep the live sample small until
// the offline dumps confirm it's safe to raise.
constexpr uint32_t kMaxStereoDualEyeAttempts = 5;
// Stage C (2026-08-11): now re-runs F9's full 10-job set TWICE per attempt
// (once per eye) - twice the per-attempt cost F9 already measured at
// ~562ms. Spread attempts out the same way F9 does, for the same reason
// (avoid 5 back-to-back double-heavy attempts landing as one multi-second
// stutter instead of individually observable ones).
std::atomic_uint32_t stereo_dual_eye_frame_counter{0};
constexpr uint32_t kStereoDualEyeFrameGap = 45;
// Half of stereoRender_separation's confirmed live default (1.5 world
// units, "world units from center to eyes") - DOOM's own first-party IPD
// reference, not a diagnostic-magnitude guess like F5's sweep. Eye 0 gets
// -this, eye 1 gets +this, applied via the same apply_translation() already
// proven (Milestone 0) to be what reaches the final image.
//
// 0.75 is DOOM's native per-eye half-separation. A previous broad write
// sweep used a diagnostic 3.0, but that compounded unrelated records and
// therefore could not distinguish magnitude sensitivity from corruption.
// Candidate C is now the sole record proven to reach the image; use the
// native magnitude for its first clean test.
//
// Formerly TEMPORARILY 3.0 (2026-08-11): the real 0.75 value produced a small,
// ambiguous per-pixel diff that didn't look like real parallax. Tried 15.0
// (20x) for an unambiguous signal per this project's "test destructively
// before plausibly" rule, but the user flagged a real confound that
// magnitude-scaling reasoning missed: unlike F5's whole-scene lever
// (validated clean up to 100uu - it shifts world+camera together,
// camera-dolly style), Stage D's GPU-direct write only moves the extra
// call's eye relative to geometry gathered from the ORIGINAL camera. In
// the tight interior spaces tested, 15uu is plausibly enough to push that
// eye past nearby wall geometry into a degenerate near-clip regime -
// which would produce noise-level, non-scaling diffs for a REASON UNRELATED
// to whether the translation mechanism itself works, confounding the
// exact test this constant exists to run. 3.0 (4x the real value) keeps
// a clear-enough signal while staying well clear of nearby walls. Dial
// back to 0.75 once the mechanism itself is confirmed.
constexpr float kStereoDualEyeHalfIpd = 0.75f;

// Confirmed via static analysis + log cross-reference (docs/RE-NOTES.md,
// "Depth-prepass hypothesis: CONFIRMED"): RenderOpaque's real depth
// attachment is literally RenderDepth's output VkImageView, not something
// our own synthetic/cleared depth image can substitute for if RenderOpaque's
// opaque-geometry pipelines use an equal-to-prepass depth test (the
// standard zero-overdraw deferred-G-buffer technique). Every framebuffer
// with 4 attachments gets its 4th (depth) view remembered here, keyed by
// framebuffer handle, so prepare_offscreen_target can reuse DOOM's REAL
// depth view instead of allocating its own - see below.
std::mutex framebuffer_depth_view_mutex;
std::unordered_map<VkFramebuffer, VkImageView> framebuffer_depth_views;
// Depth attachment for framebuffers of ANY attachment count, identified by
// format at creation - see the population site in hook_create_framebuffer.
// Diagnostic only; nothing keys behaviour off it.
std::unordered_map<VkFramebuffer, VkImageView> framebuffer_any_depth_views;
// CompositeTargetProbe (2026-08-16): colour attachment 0 of every framebuffer,
// used to identify what DOOM final composite writes into.
std::unordered_map<VkFramebuffer, VkImageView> framebuffer_color0_views;
// Last render pass observed while inside the REAL frame root, and the command
// buffer it was recorded into - the candidate copy point for eye 0.
thread_local VkFramebuffer last_frame_root_framebuffer = VK_NULL_HANDLE;
thread_local VkCommandBuffer last_frame_root_cmd_buffer = VK_NULL_HANDLE;
thread_local uint32_t frame_root_render_pass_count = 0;
thread_local bool inside_real_frame_root = false;

// FINAL STAGE DOUBLING (2026-08-18). DOOM+0x186E6BA is the OUTERMOST function
// that runs for the swapchain-writing grade+HUD pass and for no other render
// pass (CallerSpecificity: swapchainPasses=25 otherPasses=0, once per frame).
// Its caller is shared with 3196 other passes, so this is the largest scope
// that touches the final stage and nothing else.
//
// Doubling it is what makes eye 1 a FINISHED frame - graded, and with the
// HUD - and lets eye 0 go back to being DOOM's own output rather than a
// pre-final composite downgraded to match eye 1.
//
// inside_final_pass_double is the exact analogue of inside_frame_root_double,
// and it is deliberately routed into the SAME three substitution mechanisms,
// because all three are already proven and each is load-bearing here:
//   framebuffer mirroring - so the second pass writes OUR image and not the
//     swapchain. Without it the second pass overwrites DOOM's finished frame.
//   read redirection      - so it samples eye 1's composite instead of the
//     real one. Without it eye 1 would be a graded copy of EYE 0.
//   barrier substitution  - so it transitions our images, not DOOM's.
thread_local bool inside_final_pass_real = false;
thread_local bool inside_final_pass_double = false;
// True from the moment the REAL frame root returns until the next present -
// the window in which DOOM produces the image that actually reaches the
// monitor. See the PostFrameRoot probe at hook_frame_root.
//
// A plain atomic rather than thread_local ON PURPOSE: it is SET on the
// render thread and CLEARED in hook_queue_present, and nothing here has
// established that those are the same thread. As a thread_local a
// present-thread clear would never reach the render thread's copy, the
// window would stay open for ever, and the probe would quietly log ordinary
// mid-frame passes as though they were post-frame-root work.
std::atomic_bool post_frame_root_window{false};
std::atomic_bool post_frame_root_probe_armed{false};
std::atomic_uint32_t post_frame_root_lines{0};
constexpr uint32_t kMaxPostFrameRootLines = 80;
// PING-PONG PARITY PROBE state.
thread_local VkFramebuffer doubled_last_framebuffer = VK_NULL_HANDLE;
// SAME-FRAME TAG (G2, docs/IMPLEMENTATION-PLAN-2026-08-17.md STEP 2).
// Incremented once per REAL per-view frame-root entry, immediately before
// real_frame_root is called. The doubled call that follows records the
// current value alongside the mirror image it produced; the present path
// logs both. Equal every line => the pair really is same-frame. A persistent
// mirrorSerial == presentSerial - 1 => the eye-1 blit is reading the
// previous frame's mirror. Log-only, no behaviour depends on it.
std::atomic_uint32_t g_frame_root_serial{0};
// Rate limiter for the same-frame tag line: first 10 presents after each
// arm, then every 500th. Reset at arm time so a later run is not silenced by
// an earlier one having burned the budget.
std::atomic_uint32_t mirror_serial_log_count{0};
// Final mirrored colour image of the doubled run - this is eye 1.
//
// Written at command-RECORD time on the render thread (hook_cmd_begin_render_
// pass) and read on the PRESENT thread (both the eye-1 presentation override
// and the A/B dump). These used to be three plain, unsynchronised globals -
// a data race, and worse, a torn triple: the present path could pair one
// frame's image handle with another frame's extent/format (review claim 10).
// One struct behind a small mutex, so a reader always gets a consistent set.
// The mutex is taken once per mirrored render pass (7 filtered / ~77
// unfiltered per doubled call) and once per present, uncontended, with no
// other lock held at either site - it cannot deadlock against
// framebuffer_depth_view_mutex.
struct DoubledMirrorState {
    VkImage color = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    uint32_t serial = 0;
};
std::mutex doubled_mirror_mutex;
DoubledMirrorState doubled_mirror_state;
// Which world job produced the image eye 1 is showing. Diagnostic only, but
// it is the difference between "eye 1 is the lit deferred output" and "eye 1
// is the raw G-buffer" being a fact rather than a guess.
std::atomic_uintptr_t doubled_mirror_source_job{0};
// The doubled call's own G-BUFFER mirror (RenderOpaque's output),
// tracked separately from whatever eye 1 presents.
//
// Why: parallax was measured objectively at 13.9x near/far in the FILTERED
// configuration, where eye 1 WAS this image. Unfiltered, eye 1 is the final
// composite and shows no parallax. Those two facts have a fork between
// them: either the doubled call's geometry is still shifted and something
// downstream flattens it, or the extra jobs that only run when unfiltered
// defeat the camera lever itself. Every image-entry path has now been
// verified covered (compute, transfers, push descriptors, barrier2,
// templates - all refuted or unavailable), which makes the second far more
// likely and makes this the measurement that separates them.
//
// Pointing the A/B dump here measures the G-buffer under BOTH filter
// states, so the 13.9x number has a like-for-like comparison.
VkImage doubled_gbuffer_mirror = VK_NULL_HANDLE;
VkFormat doubled_gbuffer_format = VK_FORMAT_UNDEFINED;
VkExtent2D doubled_gbuffer_extent{};

void store_doubled_mirror(VkImage color, VkFormat format, VkExtent2D extent, uint32_t serial) {
    std::scoped_lock lock(doubled_mirror_mutex);
    doubled_mirror_state = DoubledMirrorState{color, format, extent, serial};
}

DoubledMirrorState load_doubled_mirror() {
    std::scoped_lock lock(doubled_mirror_mutex);
    return doubled_mirror_state;
}
// ---------------------------------------------------------------------------
// DescriptorCensus (2026-08-17, IMPLEMENTATION-PLAN-2026-08-17.md STEP 5).
//
// The re-scoped G1. The binary "is DOOM bindless?" question is already
// answered - the sibling track established the world-camera set is
// conventional (set 0, dynamic uniforms 0-3, sampled images 4-7) and proved
// whole-set cloning with per-binding substitution. What STEP 6 cannot start
// without is the CLASSIFICATION: during the doubled call, per pass, which
// image views are bound for SAMPLING, and which of those we hold mirrors
// for. Mirroring substitutes what a pass WRITES; every one of these is
// something a pass READS, and any that resolve to DOOM's originals are why
// dropping the job filter gives eye 1 lighting but loses its parallax
// (claim 8).
//
// READ-ONLY. Logs and nothing else. The one thing it changes is the job
// filter, which the arm drops and restores automatically (see the
// SHIFT+PageUp handler) - the census is worthless with the filter on,
// because the lighting and post passes whose reads we need to classify are
// exactly the ones the filter skips.
// ReadRedirect (STEP 6 PILOT) switches and counters. Declared here, with the
// census globals, because hook_frame_root's per-attempt audit line needs
// them and is defined well before the substitution logic.
//
// read_redirect_enabled off => the pilot job still runs but its reads are
// NOT redirected. That is the claim-8 state: eye 1 gains lighting and should
// LOSE parallax. Kept as a switch so that comparison is a one-line change
// rather than a rebuild of the mechanism.
// DEFAULT OFF 2026-08-17 for the ISOLATION RUN. Four attempts at STEP 6
// have produced three GPU hangs, all on attempt 0, and each fix addressed a
// real defect without settling whether READ REDIRECTION ITSELF is what
// hangs. Run 9 makes that question sharp: the safety gate reported
// refusedMirrors=0 on nearly every clone - substitution was happening, 138
// binds of it - and it hung anyway. So either layout tracking is wrong
// where it claims a match, or the hang was never about layouts.
//
// This is the control that separates them, and it should have been run
// before the third fix rather than after: everything else stays exactly as
// it is - unfiltered, barrier substitution, layout tracking, eye-1 source
// selection - and only the descriptor substitution is off.
//
//   stable  => the hang is NOT read redirection. Suspicion moves to barrier
//              substitution, which is the other thing that changed and has
//              never been run unfiltered on its own.
//   hangs   => read redirection is not the cause either, and the culprit is
//              unfiltered mirroring plus barrier substitution.
//
// SHIFT+Delete toggles it live, so one run can test both states from one
// standing position instead of costing two.
std::atomic_bool read_redirect_enabled{false};
// Barrier substitution counters - see hook_cmd_pipeline_barrier. Declared
// here for the same reason as the ReadRedirect counters: hook_frame_root's
// audit line is defined before the hook itself.
std::atomic_uint32_t barrier_image_substitutions{0};
std::atomic_uint32_t barrier_calls_rewritten{0};
// BARFIX (2026-08-31 night) - see the barrier block in hook_cmd_pipeline_barrier.
// Declared HERE, ahead of its use at ~26612, not beside the other 2026-08-31
// globals further down: a use-before-declaration is a compile error, and this
// file is large enough that "further down" is easy to get wrong.
// Default OFF - the shipped substitute path is unchanged unless armed.
std::atomic_bool g_barfix_on{false};
std::atomic_uint64_t g_barfix_appended{0};
std::atomic_uint32_t g_barfix_calls{0};
// RPCENSUS (2026-08-31 night) - see the fall-through at the tail of
// hook_cmd_begin_render_pass. Log-only. total counts every render pass the
// doubled call begins; fellthrough counts those that got DOOM.s OWN
// framebuffer rather than a mirror; withclears is the subset of those that
// carry clear values, i.e. that can wipe DOOM.s real attachment.
std::atomic_uint64_t g_rpc_total{0};
std::atomic_uint64_t g_rpc_fellthrough{0};
std::atomic_uint32_t g_rpc_withclears{0};
// RPFIX (2026-08-31 night) - redirect the doubled call.s CLEARING render passes
// to a mirror so DOOM.s own framebuffer is not wiped. Default off.
std::atomic_bool g_rpfix_on{false};
std::atomic_uint32_t g_rpfix_redirected{0};
std::atomic_uint32_t g_rpfix_nomirror{0};
std::atomic_uint32_t read_redirect_substitutions{0};
std::atomic_uint32_t read_redirect_clones{0};
std::atomic_uint32_t read_redirect_failures{0};
// CLONE POOL LEAK FIX (2026-08-24) - counters declared HERE, in the early
// block, for the same reason the ones above are: hook_frame_root's audit line
// is defined before the hook and before the pool itself. See the full
// derivation beside read_redirect_pool. rr_retire_queue_depth is maintained as
// an atomic rather than read off the vector so the audit line needs no lock.
std::atomic_uint64_t rr_clones_retired{0};
std::atomic_uint64_t rr_clones_freed{0};
std::atomic_uint32_t rr_clone_alloc_failures{0};
std::atomic_uint32_t rr_retire_queue_peak{0};
std::atomic_uint32_t rr_retire_queue_depth{0};
constexpr uint64_t kCloneRetireFrames = 8;
// WHY the clone was refused, counted UNCAPPED (2026-08-23). The FALLBACK log
// line is capped at 10 per run and 607 fallbacks were recorded in one session,
// so the reason mix could not be read from it. An attempt to infer the mix by
// grepping "reason=layout-mismatch" counted a DIFFERENT instrument - the
// per-binding storage verdicts - and would have aimed the next fix at the
// wrong cause. These four are the actual set-level refusal reasons.
std::atomic_uint32_t rr_reason_no_records{0};
std::atomic_uint32_t rr_reason_array_binding{0};
std::atomic_uint32_t rr_reason_type_conflict{0};
std::atomic_uint32_t rr_reason_other{0};
// Lever A's counterpart to lever_v_clean_rebased. See its increment site.
std::atomic_uint32_t lever_a_applied{0};

// THE WORLD-SPACE CAMERA DISPLACEMENT LEVER A JUST APPLIED (2026-08-23).
//
// Published so the fog-constant repair (mode 4) can move the deferred
// composite's reconstruction by the SAME vector the camera moved by, instead
// of deriving one from theory. Deriving an offset from first principles is
// exactly what produced the apply_translation normalisation that made VR
// visibly worse and had to be reverted.
//
// apply_translation's `t` IS that vector: it does r[3] -= dot(r.xyz, t) for
// every row, which is the standard way to translate a camera by world vector t
// in a view matrix whose rows are basis vectors. So t is taken from the same
// place, at the same moment, with no second derivation to disagree.
//
// thread_local: DOOM records, submits and presents on one thread, and the real
// and doubled calls are sequential on it, so the value read during the doubled
// call's descriptor binds is the doubled call's own.
thread_local float lever_a_world_delta[3]{};
thread_local bool lever_a_world_delta_doubled = false;
// Mode 4's own switch. Deliberately NOT kRepairDoubledConstants: that flag
// turns on the uniform-diff capture for every block in the frame - a vector
// copy, an FNV hash and a map insert each - which is the cost this file warns
// made earlier runs slow motion, and which mode 4 needs none of.
std::atomic_bool kFogRepairOnly{false};
std::atomic_uint32_t fog_blocks_examined{0};
std::atomic_uint32_t fog_patch_applied{0};
std::atomic_uint32_t fog_patch_no_delta{0};
std::atomic_uint32_t fog_patch_blocks_seen{0};
// Sampled bindings whose VIEW we do not mirror but whose underlying IMAGE
// we do - see the measurement in get_redirected_set.
std::atomic_uint32_t read_redirect_view_gap{0};
// Bindings the gap measurement had to skip because the lock was contended -
// see the try_lock in get_redirected_set. Makes the lower bound visible.
std::atomic_uint32_t read_redirect_gap_skipped{0};

std::atomic_bool descriptor_census_enabled{false};
std::atomic_uint32_t descriptor_census_lines{0};
// 2026-08-17 RUN 2 FIX. A flat 600-line budget spent itself inside ONE
// doubled call and never got past render pass 18 of ~77 - RenderDepth alone
// (pass 12, per-object binds) took 362 of them - so the deferred lighting
// and post chain, the entire point of the census, was never reached. Raising
// the number would not have fixed it; the shape was wrong.
//
// Now: accumulate per-pass totals for EVERY bind (cheap, no logging), emit
// one complete DescCensusPass summary table when the doubled call ends, and
// sample detail lines under a PER-PASS cap so no single high-volume pass can
// starve the rest. Lines carrying a mirror hit get their own budget because
// they are the actual signal. One doubled call produces the whole table, so
// the census self-disarms after it - shorter run, complete coverage.
constexpr uint32_t kMaxDescriptorCensusLines = 2500;
constexpr uint32_t kMaxCensusLinesPerPass = 12;
constexpr uint32_t kMaxCensusMirrorLines = 300;
// CENSUS MISS PATH (2026-08-21) - the 08-20 handoff's section 7, finally armed.
//
// The census says the additive composite reads mirror=0 shared=22 unknown=88,
// and 4099 views are 'unknown' project-wide. 'unknown' means only that
// census_classify_view found no entry in mirrored_views for that VIEW HANDLE.
// It does NOT mean no mirror exists for the IMAGE behind the view, and those
// are very different diagnoses:
//
//   MISS-KEYING   we DO hold a mirror for the underlying image, but it is
//                 filed under a different VkImageView - DOOM sampled the
//                 target through a view other than the attachment view we
//                 mirrored. Fix: resolve mirrors by image, not by view handle.
//   MISS-NOMIRROR no mirror exists for the image at all. Fix is upstream, in
//                 which targets ever become mirrored attachments.
//   MISS-NOIMAGE  we cannot even resolve the view to an image - our own
//                 bookkeeping lost it, and that is a third bug.
//
// The capture of the WORKING arm proves those images ARE mirrorable: the
// doubled twin of that pass binds their mirrors (235->50562, 239->50570,
// 244->50516, 370->50647, 386->50634, 389->50638). So this is the measurement
// that says which of the three we are looking at.
//
// Bounded: the census already runs for ONE doubled call and self-disarms.
// This adds two map lookups per DISTINCT unknown view, deduped, with a line
// cap. The rollup counters are unbounded but are plain atomics.
constexpr uint32_t kMaxCensusMissLines = 60;
// Resolution budget, SEPARATE from the line budget on purpose. Gating the
// resolve on the line cap would stop the rollup counters at 60 too, and a
// truncated KEYING:NOMIRROR ratio reads exactly like a complete one - the
// class of silent cap this project has already been burned by. Lines are
// capped for readability; the counters get a much larger budget so the ratio
// is a real sample. Dedupe means this is DISTINCT views, not occurrences.
constexpr size_t kMaxCensusMissResolve = 400;
std::atomic_uint32_t census_miss_lines{0};
std::atomic_uint32_t census_miss_keying{0};
std::atomic_uint32_t census_miss_nomirror{0};
std::atomic_uint32_t census_miss_noimage{0};
// Render-thread only, same premise as the rest of the census state (recording
// is single-threaded after scripted step 1 forces r_useSMP inline).
std::unordered_set<VkImageView> census_miss_seen;
constexpr uint32_t kCensusPassSlots = 160;
std::atomic_uint32_t descriptor_census_mirror_lines{0};
// Per-pass accumulators for the summary table. thread_local: the doubled
// call is inline on one thread by construction (r_useSMP=1 is a hard
// precondition of arming FrameRootDouble at all).
struct CensusPassTotals {
    uint32_t binds{}, mirror{}, shared{}, unknown{}, no_record{}, max_array{};
    VkFramebuffer fb{VK_NULL_HANDLE};
    bool fb_mirrored{};
    uintptr_t job_rva{};
};
thread_local std::array<CensusPassTotals, kCensusPassSlots> census_pass_totals{};
thread_local std::array<uint16_t, kCensusPassSlots> census_pass_line_count{};
// Emit the summary table once, at the end of the doubled call that filled
// it. Defined below hook_frame_root's helpers; declared here.
void emit_descriptor_census_summary();
// Separate, much smaller budget for camera-shaped uniform records seen
// during the REAL call. The doubled-call half of this answers "is the
// doubled call's camera record shifted"; only the real call's own record
// makes that a COMPARISON, which is what claim 12(c) - "the shift reaches
// DOOM's temporal pass through GPU-visible uniform memory" - actually
// needs. Kept apart so real-call lines cannot eat the census budget.
std::atomic_uint32_t descriptor_census_real_lines{0};
constexpr uint32_t kMaxDescriptorCensusRealLines = 40;
// Job filter state to put back when the census self-disarms.
std::atomic_bool descriptor_census_restore_unfiltered{false};
// Pass context for a census line. Which framebuffer the doubled call is
// currently rendering into, whether we hold a mirror for it, and which job
// (if any) we are inside.
//
// ⚠ doubled_current_job_rva is 0 for most passes and that is CORRECT, not a
// bug: under r_useSMP=1 the frame root's own eight top-level dispatches use
// the view_desc+0x11 INLINE branch and never pass through hook_job_dispatch
// (review headline 1 - the same coverage gap that invalidated the "noJobs"
// bisection's stated baseline). Only the ~20 RenderView-subtree dispatches
// are labelled. The framebuffer is the reliable pass identity here.
thread_local VkFramebuffer doubled_current_framebuffer = VK_NULL_HANDLE;
thread_local bool doubled_current_fb_mirrored = false;
thread_local uint32_t doubled_pass_seq = 0;
thread_local uintptr_t doubled_current_job_rva = 0;


// ---------------------------------------------------------------------------
// DRAW PARITY (2026-08-18, adversarial review P2).
//
// The review's §5 finding: PingPongParity established realPasses ==
// doubledPasses and that was treated as run equivalence. Rule 9 names that
// trap exactly. The missing fire/fog is not a missing render PASS - both
// calls run 78 of them - it is missing DRAWS or missing DATA inside passes,
// and no instrument has ever compared draw counts, vertex/index counts, or
// dispatch counts between the real call and the doubled call.
//
// This is that instrument. Log-only, one frame, one table.
//
// WHY IT IS HOOKED AT THE DRAW/DISPATCH ENTRY POINTS AND NOT IN
// hook_cmd_bind_descriptor_sets: that function has four separate
// real_cmd_bind_descriptor_sets call sites and a stack of early returns
// which have silently killed three probes already (review §7 item 4). The
// vkCmd{Draw,Dispatch}* hooks are top-level entries from DOOM - our code is
// the first thing that runs and nothing in this DLL can pre-empt it. Rule 1
// is satisfied structurally, not by inspection of a return chain.
//
// vkCmdDrawIndirect was NEVER HOOKED before this build. If DOOM issues its
// particle/effect lists as indirect draws, their absence from the doubled
// call would have been invisible to every existing probe - which is
// precisely the blind spot this step exists to close, so all four draw
// entry points are hooked here.
//
// Accumulators are plain (non-atomic) arrays: every write happens on the
// render thread inside the frame root (real call, then doubled call,
// sequentially, never nested), and the emit happens on that same thread
// immediately after the doubled call returns. No new lock - Rule 9.
struct DrawParitySlot {
    uint32_t draws;        // vkCmdDraw + vkCmdDrawIndexed
    uint32_t indirects;    // vkCmdDrawIndirect + vkCmdDrawIndexedIndirect commands
    uint64_t vertices;     // vertexCount * instanceCount, non-indexed
    uint64_t indices;      // indexCount  * instanceCount, indexed
    uint32_t dispatches;
    // Of those `dispatches`, how many we SUPPRESSED and never handed to the
    // driver (see doubled_dispatch_writes_shared_buffer).
    //
    // [rec] vs [gpu], the review's §4 rule 1, caught in this project's own
    // code: `dispatches` is incremented at the top of the hook, BEFORE the
    // suppression decides to skip the call. So the table printed pass 16 as
    // disp=10 on both sides and labelled it SAME while up to ten of the
    // doubled call's ten dispatches never executed. A recorded-command
    // counter reported parity on the exact quantity that diverges at GPU
    // time, and "identical dispatches" went into the handoff on the strength
    // of it. This field is what makes the divergence visible.
    uint32_t dispatches_suppressed;
    uintptr_t job_rva;
    VkFramebuffer fb;
};
constexpr uint32_t kDrawParitySlots = 96;
DrawParitySlot draw_parity_real[kDrawParitySlots]{};
DrawParitySlot draw_parity_doubled[kDrawParitySlots]{};

// ===================== ALWAYS-ON SCENE CENSUS (2026-08-24) ==================
//
// THE QUESTION IT ANSWERS. At the cutscene the two eye images come out
// BYTE-IDENTICAL while every camera write still lands at -1.500 / +1.500. The
// owner's description is the clue: it is "a true VERY SHORT VIDEO cutscene".
// A video plays as a fullscreen quad - a handful of draws, no world geometry,
// no dependence on the view matrix. Offsetting a camera that nothing renders
// from produces exactly this: two identical pictures, with every counter we
// own reporting health.
//
// DrawParity already measures this, but it arms for ONE frame from the
// scripted test, so it has never been looking when the cutscene arrived. These
// counters are always on and summarised once a second, which is what makes the
// transition legible without anyone pressing anything at the right moment.
//
// COST: one relaxed increment per draw, on a path that already does a
// thread_local load and several branches. Nothing is allocated and nothing is
// logged per draw.
//
// POLARITY, stated before the run: in gameplay the real call runs ~1300 draws
// over ~69 passes. If the cutscene collapses that to a handful, the scene has
// no geometry and the mono result is fully explained - and the fix is to
// DETECT this state, not to try to force parallax onto a video. If the draw
// count stays high and the eyes are still identical, the camera write is not
// reaching the shader and the fix is upstream in the write path.
std::atomic_uint32_t census_draws_real{0};
std::atomic_uint32_t census_draws_doubled{0};
std::atomic_uint64_t census_indices_real{0};
std::atomic_uint64_t census_indices_doubled{0};
std::atomic_uint32_t census_passes_real{0};
std::atomic_uint32_t census_passes_doubled{0};
std::atomic_uint32_t census_frames{0};
// Set by the cadence detector when a cutscene ENDS; consumed by the fast-VR
// block, which rebuilds the mirrors and re-arms. This is what makes VR come
// back by itself instead of needing a keypress.
std::atomic_bool fast_vr_auto_recover{false};
// Lever A write-persistence, checked at end of frame. See the write site.
std::atomic<uintptr_t> lever_a_persist_addr{0};
std::atomic<float> lever_a_persist_wrote{0.0f};
std::atomic_bool lever_a_persist_valid{false};
inline void scene_census_draw(uint64_t verts_or_indices, bool doubled) {
    if (doubled) {
        census_draws_doubled.fetch_add(1, std::memory_order_relaxed);
        census_indices_doubled.fetch_add(verts_or_indices, std::memory_order_relaxed);
    } else {
        census_draws_real.fetch_add(1, std::memory_order_relaxed);
        census_indices_real.fetch_add(verts_or_indices, std::memory_order_relaxed);
    }
}
// Armed by the scripted test for exactly one frame. draw_parity_active is
// the hot-path gate: one relaxed load per draw call and nothing else when
// disarmed, which matters because DOOM issues hundreds of draws per pass.
std::atomic_bool draw_parity_armed{false};
std::atomic_bool draw_parity_done{false};
thread_local bool draw_parity_active = false;
// The real call's pass counter, the mirror image of doubled_pass_seq. Never
// existed before: every instrument so far only ever numbered the doubled
// call's passes, so there was nothing to tabulate the real call against.
thread_local uint32_t real_pass_seq = 0;
thread_local VkFramebuffer real_current_framebuffer = VK_NULL_HANDLE;
thread_local uintptr_t real_current_job_rva = 0;

// Defined far below (the frame-root hook's own state block); declared here
// because the accessor under it needs to know which call we are inside.
extern thread_local bool inside_frame_root_double;

// Records the current draw/dispatch into whichever call we are inside.
// Returns nullptr when the frame is not armed or we are outside both calls.
inline DrawParitySlot* draw_parity_slot() {
    if (!draw_parity_active) return nullptr;
    if (inside_frame_root_double) {
        const uint32_t s = doubled_pass_seq < kDrawParitySlots
                               ? doubled_pass_seq : kDrawParitySlots - 1;
        DrawParitySlot* p = &draw_parity_doubled[s];
        if (!p->job_rva) p->job_rva = doubled_current_job_rva;
        if (!p->fb) p->fb = doubled_current_framebuffer;
        return p;
    }
    if (inside_real_frame_root) {
        const uint32_t s = real_pass_seq < kDrawParitySlots
                               ? real_pass_seq : kDrawParitySlots - 1;
        DrawParitySlot* p = &draw_parity_real[s];
        if (!p->job_rva) p->job_rva = real_current_job_rva;
        if (!p->fb) p->fb = real_current_framebuffer;
        return p;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// P1 - BROKEN-ARM READ CLASSIFICATION (2026-08-23).
// docs/ANALYSIS-2026-08-22-MISSING-ARM.md §5 P1.
//
// THE MISSING HALF. tools/rdc_eye1_reads.py produced this table for the
// WORKING arm, at the deferred lighting pass (22 draws), from doom-capture-2:
//
//   real     eid=6498  draws=22  rdMIR=0  rdREAL=9  rdOTH=4  rd3D=2
//   DOUBLED  eid=8352  draws=22  rdMIR=8  rdREAL=1  rdOTH=4  rd3D=2
//
// i.e. in the working arm the doubled lighting pass reads EIGHT mirror-chain
// images where the real pass reads their real-chain counterparts. Redirection
// is fully resolved there. No broken-arm equivalent has ever been measured,
// and it cannot come from a capture: taking one requires RenderDoc, and with
// RenderDoc loaded eye 1 is correct, so every capture is the working arm.
//
// The census does NOT answer this and must not be compared to it. Its
// "mirror" bucket counts a DOOM source view we hold a mirror FOR - the
// OPPOSITE polarity from rdMIR, on a different denominator, in a different
// counting unit (binds, not distinct views). `mirror=12` and `rdMIR=8` are
// the draws-vs-binds mistake of handoff §6.3 wearing a different hat.
//
// WHAT THIS CLASSIFIES, and why it is post-substitution. The capture records
// what was ACTUALLY BOUND. The census deliberately classifies DOOM's ORIGINAL
// sets, before redirection swaps anything, because it answers a different
// question. So this instrument records BOTH, per pass, per side:
//
//   ASKED - the descriptor sets DOOM's job handed us
//   BOUND - the clone that redirection actually bound in its place
//
// and the difference between them IS the redirection, made visible per pass.
// A doubled lighting pass whose BOUND row shows rdMIR~8 means redirection
// resolves identically in both arms and the answer is genuinely in memory
// contents. A BOUND row still showing real-chain reads where the working arm
// shows mirror-chain means the bug is in our redirect resolution and the
// failing view is named on the same line.
//
// CLASSIFICATION, by the rule the capture tool uses - membership of our
// mirror set, resolved by IMAGE rather than by view handle, because
// MISS-KEYING=2 reproduces in every run and a view-keyed test would file
// those as OTH:
//
//   MIR  the view's image is one of OUR mirror images
//   REAL the view's image is a DOOM image we hold a mirror for
//   VOL  a 3D view - the froxel volumes, shared by both sides by design
//        (rd3D=2 on both sides of the working table); checked FIRST so a
//        volume never lands in MIR/REAL and inflates the discriminator
//   OTH  everything else: content textures, megatexture pages, LUTs
//
// SCOPED BY RVA, never by pass number. Pass ordinals move between runs - the
// first HDR stage has been p15 and p17 in two runs four minutes apart - and
// §6.4 is explicit about it. The table prints every pass but flags the ones
// whose job_rva is the lighting job.
//
// COST. Gated on a thread_local bool, so a disarmed run pays one predictable
// branch per bind on a path that runs ~2700x/frame. Armed, it runs for ONE
// frame and self-disarms. Per-side tables, per-side dedupe, fixed capacity
// with an explicit overflow counter - a truncated distinct-view list reads
// exactly like a complete one otherwise.
// ⚠ NAME CORRECTED 2026-08-26. This constant was called kP1LightingJobRva and
// described as "the deferred lighting job" for many sessions. It is not.
// DOOM registers its render jobs with stubs that `jmp 0x319B50` carrying
// (rcx=object, rdx=function, r8=nameString); the stub at 0x240FB0 registers
// 0x19AE9E0 under the name **"GPU Particle Simulate"**, and its four
// 0x570B7xx declarations resolve, from their own records' +0x10 name pointers,
// to `particleIndirectDrawArgs`, `particleRandom` and `particleVectorFields`.
// All 120 stubs were swept into
// docs/evidence/JOBMAP-2026-08-26-registered-render-jobs.txt.
//
// Every P1/P1W MEASUREMENT at this RVA stands; only their INTERPRETATION as
// deferred lighting was wrong. Kept as a target because "eye 1 is missing an
// additive layer" is what missing GPU particles looks like.
constexpr uintptr_t kP1ParticleSimJobRva = 0x19AE9E0;
// The REAL deferred lighting job, never previously targeted. Registered at
// 0x232AF1 under the name "RenderDeferredPasses"; its companion
// "DeferredPassesPrepare" is 0x18733C0, which matches the third named context
// found at [0x6FD3390]. Both are enqueued from the same frame job-graph
// builder, 0x17B61A0, that enqueues the particle job.
constexpr uintptr_t kP1DeferredPassesJobRva = 0x1873830;
constexpr uint32_t kP1PassSlots = 96;
constexpr uint32_t kP1SlotViews = 64;
enum P1Class : uint8_t { kP1Oth = 0, kP1Mir = 1, kP1Real = 2, kP1Vol = 3 };

struct P1Slot {
    uint32_t binds;
    uint32_t distinct;
    uint32_t overflow;
    // Descriptor sets bound at this job for which we hold NO content record,
    // and the underlying bind count. RUN 2 made this mandatory: at the
    // lighting job the doubled call showed rdVOL=0 where the real call showed
    // rdVOL=2, which is either the finding of the investigation or simple
    // blindness - a set we cannot see into contributes zero views and reads
    // exactly like a set that binds none. Same shape as the census's
    // [NO-RECORD] marker, and the same reason for it.
    uint32_t no_record;
    uint32_t sets_seen;
    uint32_t cls_count[4];
    uintptr_t job_rva;
    VkFramebuffer fb;
    VkImageView views[kP1SlotViews];
    uint8_t cls[kP1SlotViews];
};
P1Slot p1_real_asked[kP1PassSlots]{};
P1Slot p1_doubled_asked[kP1PassSlots]{};
P1Slot p1_doubled_bound[kP1PassSlots]{};
// ⚠ RUN 1 (2026-08-23 23:39) PROVED THE PASS ORDINAL IS THE WRONG KEY.
//
// A render-pass ordinal is not a job. Pass 16 of that run carried binds from
// BOTH 0x1A37CC0 (RenderOpaque) and 0x19AE9E0 (the deferred lighting job), and
// the per-pass slot latched whichever RVA the FIRST bind of the pass reported.
// The asked row latched RenderOpaque and the bound row latched the lighting
// job - two rows of the same slot, disagreeing about which job they describe,
// which is precisely the §6.4 trap arriving through a side door: I scoped by
// RVA in the emit but keyed the accumulation by ordinal.
//
// The per-pass tables are kept as context. The RVA tables below are the ones
// the control and the verdict read.
constexpr uint32_t kP1RvaSlots = 32;
uintptr_t p1_rva_keys[kP1RvaSlots]{};
uint32_t p1_rva_count = 0;
uint32_t p1_rva_overflow = 0;
P1Slot p1_real_asked_rva[kP1RvaSlots]{};
P1Slot p1_doubled_asked_rva[kP1RvaSlots]{};
P1Slot p1_doubled_bound_rva[kP1RvaSlots]{};
std::atomic_bool p1_armed{false};
std::atomic_bool p1_done{false};
thread_local bool p1_active = false;
// Arm-time snapshots. Built ONCE per armed frame on the render thread, with
// each lock taken and released before the next - never nested. Resolving a
// view to an image needs framebuffer_depth_view_mutex, and taking that
// underneath mirror_mutex+swapchains_mutex on the render thread is what
// wedged run 17; the census carries the same warning. Snapshotting removes
// the need for any of those three locks on the hot path.
std::unordered_set<VkImage> p1_mirror_images;
std::unordered_set<VkImage> p1_source_images;
// ⚠ RUN 1 ALSO PROVED AN IMAGE-ONLY CLASSIFIER CANNOT SEE OUR OWN MIRRORS.
//
// p1_view_image is built from image_view_infos, which hook_create_image_view
// populates - and our mirror and alias views are created with
// real_create_image_view, deliberately bypassing that hook. So a substituted
// view resolved to no image at all and fell through to OTH. Every bound row
// read rdMIR=0 rdREAL=0 with the whole population in rdOTH, while
// ReadRedirect logged clone after clone: a redirection that demonstrably
// happened, reported as no redirection at all.
//
// Our mirror views are therefore matched by HANDLE, before any image lookup.
// The REAL side needs no equivalent - DOOM's own views ARE in
// image_view_infos, and run 1's real rows classified correctly.
std::unordered_set<VkImageView> p1_mirror_views;
// Per-binding detail at the lighting job. Bounded by construction.
constexpr uint32_t kP1MaxDetailLines = 200;
std::atomic_uint32_t p1_detail_lines{0};

// ------------------------------------------------------ P1W: WRITE ORIGIN --
// RUN 3 (2026-08-19 23:55) established, with both noRecord=0 and 11 sets a
// side, that at the deferred lighting job the doubled call's descriptor sets
// hold at BINDING 2 a duplicate of binding 3's non-volume view, where the real
// call's eleven sets all hold a 3D froxel volume there:
//
//   REAL    set=0x..cafa0 binding=2 view=0x1919fca5950 class=VOL
//   DOUBLED set=0x..30700 binding=2 view=0x1919ae4bf50 class=OTH  <- == binding 3
//
// Bindings 3, 4 and 5 are identical between the two sides. Only binding 2
// diverges, and it does so in all eleven sets.
//
// THAT IS READ FROM OUR RECORD OF THOSE SETS, and there are two very different
// explanations: DOOM really wrote binding 2 that way when we re-invoked the
// job, or our record of those sets is wrong. Acting on the first while the
// second is true would be the third instrument artefact in this investigation,
// so it gets measured before it gets believed.
//
// This captures (set, binding, view) at the moment each image descriptor is
// WRITTEN - both through vkUpdateDescriptorSets and through the update-template
// decode - and replays them at emit time against the lighting job's sets. A
// captured write of the OTH view into binding 2 of a doubled set means DOOM
// wrote it; no captured write at all means our record came from somewhere else
// and the next thread is where.
//
// Bounded: only image writes to binding < 8, deduped, hard-capped. Armed with
// P1 and cleared at the same emit, so it costs nothing outside that window.
struct P1Write { VkDescriptorSet set; uint32_t binding; VkImageView view; };
// RUN 4 (2026-08-20 00:07) CAME BACK BLIND: 0 writes captured, dropped=0. The
// probe's own control caught it, and the cause was the arming window, not the
// hooks. P1W armed at step 35 and the table emitted 23 ms later - the frame
// -root latch fires on the very next frame - so the window was a few
// milliseconds wide and the writes that populate a frame's descriptor sets had
// already happened.
//
// Two changes, and a third counter so a repeat cannot be ambiguous:
//   * armed at scripted-test step 1, roughly ten seconds ahead of the emit
//   * the cap raised, with hashed dedupe - a linear scan over 16k entries on
//     every descriptor write would be quadratic on a path DOOM runs constantly
//   * p1_write_calls counts EVERY call while armed, before any filtering, so
//     "the write hooks never fired" is distinguishable from "they fired and
//     everything was filtered out". Run 4 could not tell those apart.
constexpr size_t kP1MaxWrites = 65536;
std::vector<P1Write> p1_writes;
std::unordered_set<uint64_t> p1_write_keys;
std::mutex p1_write_mutex;
std::atomic_bool p1_write_probe_armed{false};
std::atomic_uint32_t p1_writes_dropped{0};
std::atomic_uint32_t p1_write_calls{0};
std::atomic_uint32_t p1_write_in_range{0};
// W1 rides this hook - defined with the D1 block, ~2000 lines down.
void w1_capture(uint32_t binding, VkImageView view);

void p1_capture_write(VkDescriptorSet set, uint32_t binding, VkImageView view) {
    if (!p1_write_probe_armed.load(std::memory_order_relaxed)) return;
    p1_write_calls.fetch_add(1, std::memory_order_relaxed);
    if (binding >= 8 || set == VK_NULL_HANDLE) return;
    p1_write_in_range.fetch_add(1, std::memory_order_relaxed);
    w1_capture(binding, view);
    const uint64_t key = (reinterpret_cast<uint64_t>(set) * 0x9E3779B97F4A7C15ull) ^
                         (reinterpret_cast<uint64_t>(view) * 0xC2B2AE3D27D4EB4Full) ^
                         (static_cast<uint64_t>(binding) << 1);
    std::scoped_lock lock(p1_write_mutex);
    if (!p1_write_keys.insert(key).second) return;
    if (p1_writes.size() >= kP1MaxWrites) {
        p1_writes_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    p1_writes.push_back({set, binding, view});
}
// --------------------------------------------------- THE FROXEL FIX (P2) --
// MEASURED, NOT GUESSED (2026-08-20, P1 runs 2-5).
//
// At the deferred lighting job, all eleven of the doubled call's descriptor
// sets hold at BINDING 2 - the froxel volume slot - a duplicate of binding 3's
// non-volume view, where all eleven of the real call's sets hold a 3D volume.
// Bindings 3, 4 and 5 are identical between the two sides. Reproduced in three
// consecutive runs with noRecord=0 on both sides, and confirmed at the WRITE
// site by P1W (calls=3506 volumeWrites=546 dropped=0): DOOM itself writes the
// wrong view into those sets, so it is not our bookkeeping.
//
// The froxel volumes carry the volumetric fog. The deficit is localised to
// this job's output - the first full-resolution HDR target. The working arm's
// capture reads rd3D=2 on BOTH sides of this pass. Eye 1's lighting never
// samples the volumes at all.
//
// This substitutes the volume the REAL call bound in the SAME FRAME into
// binding 2 of the doubled call's clone. It rides the clone machinery that
// already rewrites bindings 4 and 5 for this job, so it is one more entry in
// an array that is already being built - not a new mechanism.
//
// IT IS A CAUSAL TEST FIRST AND A FIX SECOND. If eye 1's volumetrics return,
// the mechanism is confirmed and this is the shape of the repair. If they do
// not, the divergence is real but not the cause, and the substitution count
// below says whether it even happened.
// RESULT: NEGATIVE AS BUILT (2026-08-20, run 00:26). It FIRED - substitutions=22,
// missing=0, and the doubled call's BOUND row at the lighting job went from
// rdVOL=0 to rdVOL=1, so binding 2 really did carry a froxel volume. Eye 1 did
// not change: p15 px>128 ratio 32.6% and 29.8% against an untreated band of
// 25.7-30.5%, final-stage meanLuma ratio 82.9% and 81.5% against 79.1-83.6%.
//
// ⚠ ONE VARIANT REMAINS UNTESTED. The real call binds at least TWO distinct
// views at this binding across its eleven sets; this substituted a single one
// into all eleven, so roughly four of them got a volume the real call does not
// use there. A majority-correct substitution producing exactly zero movement is
// evidence against the hypothesis, not proof - but it is evidence.
//
// Switched OFF: an armed behavioural change to descriptors the GPU consumes,
// which demonstrably moves nothing, is pure risk in every later run.
// REOPENED 2026-08-29, AND ITS NEGATIVE VERDICT IS QUARANTINED.
//
// The "demonstrably moves nothing" above was measured on 2026-08-20 - BEFORE
// the corrupting render-surface hook at DOOM+0x1A367E0 was removed - and it was
// judged by PPM luma ratios in scenes whose additive content was never
// controlled for. Under the plan's quarantine rule that verdict is UNKNOWN, not
// wrong. It was also, by its own admission two paragraphs up, a PARTIAL fix.
//
// What reopened it is a picture rather than an argument. The 2026-08-29 matched
// pair, on a controlled scene with a spark burst and heavy fog in eye 0, shows
// eye 1 losing the volumetric haze and the sparks while KEEPING its local
// analytic lighting - the landscape comes out crisp instead of veiled. That is
// the signature of a missing froxel volume, and the froxel volume is exactly
// what P1 measured diverging at binding 2 in all eleven of the doubled call's
// sets, confirmed at the WRITE site by P1W.
//
// NOW RUNTIME-SETTABLE so it can be A/B'd live against that same scene instead
// of costing a rebuild and a second run with a different view. std::atomic_bool
// converts to bool implicitly, so every existing use site keeps working
// unchanged; the name keeps its k-prefix only to avoid churning six call sites
// that are correct as they stand.
//
// The capture at the real call is gated on this same flag, so a flip takes
// effect from the NEXT frame: the real call runs first and records the volume,
// then the doubled call substitutes it. Forced OFF at arm time.
std::atomic_bool kFixDoubledLightingVolume{false};
constexpr uint32_t kLightingVolumeBinding = 2;
// The volume the REAL call bound at binding 2 this frame. thread_local: the
// real call and the doubled call run on the same render thread, one after the
// other, and this must never carry across to another thread's frame. Cleared
// at frame-root entry so a frame where the real call bound no volume cannot
// inherit the previous frame's.
thread_local VkImageView real_lighting_volume_view = VK_NULL_HANDLE;
std::atomic_uint32_t lighting_volume_substitutions{0};
std::atomic_uint32_t lighting_volume_missing{0};
// The no-op case: doubled binding already equals the real one. See the FroxelFix
// NO-OP line - a fix with nothing to do must not read as a fix that failed.
std::atomic_uint32_t lighting_volume_already_ok{0};
// CLONE ARRAY TRUNCATION - see the CloneArray block in get_redirected_set.
std::atomic_uint32_t clone_array_bindings{0};
std::atomic_uint32_t clone_elements_dropped{0};
std::atomic_uint32_t clone_max_array{0};
std::atomic_uint32_t clone_array_lines{0};
// MirrorCoverage - see the census in hook_cmd_begin_render_pass.
constexpr uint32_t kMaxMirrorCoverageLines = 120;
std::atomic_uint32_t mirror_coverage_lines{0};
std::atomic_uint32_t mirror_coverage_misses{0};
// CloneDrop - see the clone completeness census in get_redirected_set.
constexpr uint32_t kMaxCloneCompletenessLines = 20;
std::atomic_uint32_t clone_completeness_lines{0};
std::atomic_uint32_t clone_bindings_dropped{0};
// VolumeSubst - see the census in get_redirected_set.
constexpr uint32_t kMaxVolumeSubstLines = 12;
std::atomic_uint32_t volume_subst_lines{0};
std::atomic_uint32_t volume_substitutions_seen{0};
// VOLUME RESTORE - see the block in the descriptor-write hook. Per-binding
// record of the 3D volume the REAL call wrote this frame; cleared at frame-root
// entry so a stale handle can never be replayed into a later frame.
constexpr uint32_t kVolumeRestoreBindings = 16;
// The binding the clone cache keys its staleness on. Binding 2 is the froxel
// slot P1 measured diverging; binding 3 alternates in lockstep with it.
constexpr uint32_t kVolumeRestorePrimaryBinding = 2;
VkImageView real_volume_by_binding[kVolumeRestoreBindings]{};
// ⚠ BINDING INDEX ALONE IS NOT AN IDENTITY (2026-08-22, black screen).
//
// The first bind-time version substituted wherever a binding index matched,
// and binding 2 of one set is not the same resource as binding 2 of another -
// different sets have different layouts. It wrote froxel volumes into binding 2
// of every set holding a non-3D view there, across the whole frame. A 3D view
// in a slot whose layout declares a 2D sampler is an invalid descriptor, and
// the screen went black. P2 was scoped to a single job RVA precisely for this
// reason and I dropped that scoping when generalising it.
//
// The job RVA at which the REAL call bound the volume for each binding. A
// substitution now requires the doubled call to be at the SAME job AND the same
// binding, which is a real identity rather than an index collision.
uintptr_t real_volume_job_rva[kVolumeRestoreBindings]{};
// PER (JOB, BINDING), NOT PER BINDING (2026-08-22, after the cap trip).
//
// The per-binding record thrashed. Bindings 2 and 3 are the CURRENT and
// PREVIOUS froxel volumes of DOOM's temporal reprojection, so both handles
// appear at both bindings within a single frame. Recording "the last 3D view
// seen at binding b" therefore flipped constantly: every cached clone looked
// stale, was rebuilt, re-substituted, and the runaway guard tripped at 24 in
// one frame with the SAME source set appearing three times in six lines.
//
// Keyed by the job the real call bound it at, the record is stable within a
// frame: at job X binding 3 the real call binds one specific volume, and that
// is what the doubled call at job X binding 3 should get.
constexpr uint32_t kVolumeJobSlots = 8;
struct VolumeJobRecord {
    uintptr_t rva = 0;
    VkImageView view[kVolumeRestoreBindings]{};
};
VolumeJobRecord volume_job_records[kVolumeJobSlots]{};
std::atomic_uint32_t volume_job_overflow{0};
// FORCED SERIALISATION - see the barrier before the doubled call.
std::atomic_bool force_doubled_serialisation{false};
std::atomic_uint32_t doubled_serialisations{0};

// Slot for an RVA, or nullptr when the table is full. Linear over 8 entries:
// the doubled call visits a handful of volume-binding jobs, and a hash would
// buy nothing but a collision to reason about.
inline VolumeJobRecord* volume_job_slot(uintptr_t rva, bool create) {
    if (!rva) return nullptr;
    for (auto& r : volume_job_records) if (r.rva == rva) return &r;
    if (!create) return nullptr;
    for (auto& r : volume_job_records) if (!r.rva) { r.rva = rva; return &r; }
    volume_job_overflow.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}
// Runaway guard: a correct repair touches a handful of sets per frame. If this
// trips, the scoping is wrong again and the mode disarms ITSELF rather than
// leaving the owner looking at a black screen and guessing which key undoes it.
// RAISED 24 -> 512 (2026-08-22). At 24 the guard was tripping on legitimate
// breadth, not corruption: with (job,binding) scoping the substituted sets came
// back all DISTINCT and sequential (…36b98, …36f10, …37288, …37600) at job
// 0x19ae9e0 binding 2, which is DOOM having many per-object sets at that job -
// exactly what P1 saw. The guard still exists to catch the index-collision
// failure that blacked the screen, which produced substitutions across
// unrelated sets and would blow past this too.
constexpr uint32_t kMaxVolumeRedirectsPerFrame = 512;
std::atomic_uint32_t volume_redirects_this_frame{0};
std::atomic_bool volume_restore_enabled{false};
std::atomic_uint32_t volume_restores{0};
std::atomic_uint32_t volume_restore_lines{0};
// THE CONTROLS THE FIRST VERSION LACKED. volumeRestores=0 has three completely
// different causes and the counter alone cannot tell them apart:
//   vr_volumes_recorded=0 -> the REAL call's volume writes never reach this hook,
//        i.e. DOOM writes them through the update-TEMPLATE path and the
//        recorder is blind. The restore could never fire. VOID.
//   vr_writes_doubled=0   -> DOOM does not write image descriptors during the
//        doubled call at all; it re-binds sets written earlier. Patching writes
//        is then the wrong lever entirely and the asked divergence means the
//        doubled call BINDS different sets.
//   both>0, restores=0    -> DOOM never wrote a non-3D view at a recorded
//        binding, and the divergence is somewhere other than this write.
std::atomic_uint32_t vr_volumes_recorded{0};
std::atomic_uint32_t vr_writes_doubled{0};
std::atomic_uint32_t vr_template_writes{0};
std::atomic_uint32_t vr_record_lines{0};
// Records captured with no job RVA. If this tracks vrRecorded, the volumes are
// bound outside any job we attribute and the RVA key cannot work - which is a
// different answer from "no set needed a substitution".
std::atomic_uint32_t vr_records_no_rva{0};
std::atomic_uint32_t lighting_volume_rebuilds{0};

// The lighting job's descriptor sets, per side, recorded as P1 walks them.
constexpr uint32_t kP1MaxLightSets = 24;
VkDescriptorSet p1_light_sets_real[kP1MaxLightSets]{};
VkDescriptorSet p1_light_sets_doubled[kP1MaxLightSets]{};
uint32_t p1_light_sets_real_count = 0;
uint32_t p1_light_sets_doubled_count = 0;
std::unordered_map<VkImageView, VkImage> p1_view_image;
std::unordered_set<VkImageView> p1_volume_views;
// Effective image views per redirect clone, recorded where the clone is
// built. The clone is written with real_update_descriptor_sets - deliberately
// not our hook, so our records of DOOM's sets never contain clone handles -
// which means descriptor_image_sets has no entry for it and the BOUND side
// would come back empty without this.
std::unordered_map<VkDescriptorSet, std::vector<VkImageView>> p1_clone_views;
std::mutex p1_clone_mutex;
void p1_build_snapshot();
void p1_emit_table(uint32_t attempt);
const char* p1_class_name(uint8_t c);
void p1_record(uint32_t set_count, const VkDescriptorSet* sets, bool bound_phase);

// ---------------------------------------------------------------------------
// PARITY BUDGET (2026-08-19, review §4 rule 2). One shared line cap between
// the REAL call and the DOUBLED call is not a sample, it is a blind spot: the
// real call runs FIRST and spends the whole quota before the rare side it is
// being compared against logs anything.
//
// This has now cost the project FIVE probes - FinalPassBindings,
// StorageBufferProbe, VertexParity, and (found 2026-08-19, still live)
// FogHunt, whose entire 60-line budget went on pass 12 repeating three
// bindings across many sets, so it never reached pass 19 and its "no shared
// effects input exists" conclusion was never actually tested.
//
// Two rules, enforced here instead of re-remembered per probe:
//   1. Separate counters per side. take() picks by which call we are inside.
//   2. A COMPLETE sentinel per side, so "no lines" is distinguishable from
//      "budget exhausted" - absence has to be detectable, or a starved probe
//      reads exactly like a negative result. That is the whole failure mode.
//
// Dedupe stays the probe's own job: what counts as a repeat is probe-specific
// (a buffer handle, a (pass,binding) pair), but a probe that does not dedupe
// will spend its budget on repeats no matter how the budget is split.
struct ParityBudget {
    std::atomic_uint32_t real{0};
    std::atomic_uint32_t doubled{0};
    uint32_t cap;

    explicit ParityBudget(uint32_t per_side_cap) : cap(per_side_cap) {}

    void reset() {
        real.store(0, std::memory_order_relaxed);
        doubled.store(0, std::memory_order_relaxed);
    }
    // Returns true if this side may log one more line. Caller must already
    // know it is inside one of the two calls.
    bool take(bool is_doubled) {
        std::atomic_uint32_t& side = is_doubled ? doubled : real;
        return side.fetch_add(1, std::memory_order_relaxed) < cap;
    }
    // Emit once when the probe closes. Prints BOTH sides unconditionally: a
    // side that logged nothing is the single most important thing a
    // comparison probe can tell you, and it is exactly what a missing line
    // hides.
    void emit_complete(const char* probe) const {
        const uint32_t r = real.load(std::memory_order_relaxed);
        const uint32_t d = doubled.load(std::memory_order_relaxed);
        log::warn(std::format(
            "{} COMPLETE n_real={} n_doubled={} cap={}/side | {}",
            probe, r, d, cap,
            (r >= cap || d >= cap)
                ? "AT LEAST ONE SIDE HIT THE CAP - the listing is TRUNCATED, not exhaustive; "
                  "raise the cap or tighten the dedupe before reading absence as evidence"
                : "both sides under cap - absence of a binding here is real, not truncation"));
    }
};

// DeferredInputMap's arm flag and budget. Declared HERE rather than beside the
// probe body far below because the frame-root disarm site - which emits the
// COMPLETE sentinel - runs above it. See probe_deferred_inputs() for what the
// probe measures and, more importantly, what it cannot see.
std::atomic_bool deferred_input_map_armed{false};
// RAISED 2026-08-22 with the pass range. At 64/side the widened walk would
// truncate and the COMPLETE sentinel would say so, but a truncated map is a
// map whose absences mean nothing - and absence is exactly what this probe is
// being used to establish.
ParityBudget deferred_input_budget{320};

// Buffer-descriptor records for UboContent. Declared here rather than beside
// the rest of the readback machinery because the RECORDER lives in
// probe_deferred_inputs, which is defined above that block - the same
// ordering constraint that already applies to deferred_input_map_armed.
//
// Matched between the two calls by (pass, setIdx, binding) - where the shader
// reads the value - and never by buffer handle, because a mirrored buffer
// would legitimately have a different handle and still be the same binding.
// 32, not 16, AND a window that starts at the effects pass.
//
// The first run filled all 16 slots per side on passes 20 and 22 and never
// reached pass 25 - the only pass that matters. That is the starvation bug
// this project has now hit six times, and I hit it in a probe written AFTER
// building ParityBudget to prevent it, because I used a raw counter instead
// of the helper.
//
// Widening alone would not fix it: the fix is to stop spending the budget on
// passes that are already known identical. The window starts at 24 so the
// quota goes where the divergence is, and 32 slots leave headroom for the
// pass renumbering that already cost two runs.
constexpr uint32_t kUboSlots = 192;
constexpr uint32_t kUboSlotStride = 256;
// WIDENED 24 -> 0 (2026-08-22). At 24 this probe compared BUFFER contents only
// at passes 24 and 25 - deep downstream of p15, where the volumetric loss is
// already complete. Its END line concludes "textures, buffers, vertices, args,
// state and target are ALL identical", and that conclusion was drawn from two
// passes that were already exonerated. Buffer contents at the passes where the
// effects actually go missing have never been compared.
//
// This is the same gate mistake DeferredInputMap had, in a second probe with
// its own constant. kUboSlots was raised with it so the wider walk reports
// truncation rather than silently dropping the early passes.
constexpr uint32_t kUboFirstPass = 0;
// Slot counts and the pending-copy list live here for the same reason: three
// recorders feed ONE pending list so there is a single flush site, and one of
// those recorders (the UBO one) is inside probe_deferred_inputs above.
constexpr uint32_t kIndirectSlots = 16;
// 32, not 12: recording EVERY vertex binding rather than just binding 0
// multiplies the distinct regions per pass, and a starved side reads exactly
// like a clean one.
constexpr uint32_t kVertexSlots = 32;
// THE WHOLE READBACK LAYOUT lives here, not beside the code that fills it.
// Four recorders write into one buffer and three of them - the UBO one, the
// depth one, and the emit - sit in functions defined ABOVE that code. Keeping
// the layout in one early block is what stops the "declared after its first
// use" churn that has now cost three compile rounds.
constexpr uint32_t kIndirectStructsPerSlot = 8;
constexpr uint32_t kIndirectSlotStride = 256;   // bytes reserved per slot
constexpr uint32_t kIndirectSideStride = kIndirectSlots * kIndirectSlotStride;
constexpr uint32_t kVertexSlotStride = 256;     // bytes sampled per binding
constexpr uint32_t kVertexSideStride = kVertexSlots * kVertexSlotStride;
constexpr uint32_t kVertexRegionBase = kIndirectSideStride * 2;
constexpr uint32_t kUboSideStride = kUboSlots * kUboSlotStride;
constexpr uint32_t kUboRegionBase = kVertexRegionBase + kVertexSideStride * 2;
// FROXEL VOLUME CONTENT (2026-08-29). The last mechanism standing.
//
// Every INPUT dimension is now closed on a clean baseline: same passes, same
// draws, same indices, same bound images (31/31, both froxel volumes), same
// depth, same camera at 0.0 separation, and every differing constant either the
// intended offset, shadow-atlas tiles (repaired, engaged, negative) or
// motion-dependent reprojection terms that are IDENTICAL when standing still -
// which is exactly when the fog is observed missing.
//
// What has never been compared is the CONTENT of the froxel volume at the
// moment each call samples it. Both calls bind the SAME image - the volumes
// report mirrorHeld=false imageMirrored=false, so they are DOOM's own, not
// mirrored - and the doubled call is never seen binding them as STORAGE_IMAGE,
// so it samples rather than rebuilds. If something clears or rewrites that
// volume between the real call and the doubled call, eye 1 samples a spent
// volume through a perfectly correct descriptor. That is consume-once in the
// one place still unmeasured, and it is the only account left that fits
// "volumetrics gone, analytic lighting intact".
//
// One Z slice per side into the readback buffer, hashed at present. Same shape
// as depth_strip_copy, including its layout discipline: transition from the
// layout the image is actually in and put it back exactly, because a wrong
// restore is what hangs the GPU.
constexpr uint32_t kFroxelSliceBytes = 65536;
// LATCHED AT THE BIND, COPIED AT END-OF-PASS (2026-08-29, second version).
//
// The first version recorded the copy from probe_deferred_inputs, which runs
// out of hook_cmd_draw - i.e. INSIDE a render pass instance. vkCmdCopyImageToBuffer
// and image-layout barriers are not permitted there, so the copy was invalid
// and both slices read back as zeros. The probe's own nonzero control caught
// it and called the run VOID rather than "identical", which is the only reason
// it was not read as a result.
//
// depth_strip_copy had this right all along: it is called from
// hook_cmd_end_render_pass. So the bind site now only LATCHES which volume
// each side sampled, and the copy happens at the next end-of-pass, outside the
// instance.
// TWO VOLUMES PER SIDE, NOT ONE (2026-08-29, third version).
//
// The first two versions captured "the first 3D volume each call binds" and
// got all zeros both times. The input map shows DOOM binds TWO:
//
//   binding 2  256x256x32  fmt 37 = R8G8B8A8_UNORM        <- what was captured
//   binding 3  256x256x96  fmt 97 = R16G16B16A16_SFLOAT   <- almost certainly
//                                                            the scattering volume
//
// 96 z-slices of half-float RGBA is the classic froxel fog layout; an 8-bit
// 32-slice volume is more likely a LUT or a noise/dither texture and can be
// legitimately empty in the sampled region.
//
// Capturing BOTH makes the run self-diagnosing rather than another void:
//   vol 1 populated, vol 0 empty -> the copy path and the declared layout are
//        FINE, and vol 0 was simply an empty texture. Read vol 1's verdict.
//   both empty                   -> the copy is still not landing. The next
//        suspect is the declared SHADER_READ_ONLY_OPTIMAL, since a wrong
//        oldLayout is permitted to discard contents.
constexpr uint32_t kFroxelVolumes = 2;
VkImage froxel_pending_image[2][kFroxelVolumes]{};
VkExtent3D froxel_pending_extent[2][kFroxelVolumes]{};
VkFormat froxel_pending_format[2][kFroxelVolumes]{};
bool froxel_slice_captured[2][kFroxelVolumes]{};
VkImage froxel_slice_source[2][kFroxelVolumes]{};
std::atomic_bool froxel_compare_ready{false};
std::atomic_bool froxel_probe_armed{false};
void froxel_slice_copy(VkCommandBuffer command_buffer, VkImage image,
                       VkExtent3D extent, VkFormat format, uint32_t side, uint32_t vol);
constexpr uint32_t kDepthStripBytes = 65536;
constexpr uint32_t kDepthRegionBase = kUboRegionBase + kUboSideStride * 2;
constexpr uint32_t kFroxelRegionBase = kDepthRegionBase + kDepthStripBytes * 2;
constexpr VkDeviceSize kIndirectReadbackSize = kFroxelRegionBase + kFroxelSliceBytes * 4;
void* indirect_readback_mapped_fwd();
void depth_strip_copy(VkCommandBuffer command_buffer, VkImage image, VkImageLayout layout,
                      VkExtent2D extent, uint32_t side);
struct PendingArgCopy { VkBuffer buffer; VkDeviceSize offset; VkDeviceSize size;
                        uint32_t side, slot; };
thread_local std::vector<PendingArgCopy> pending_arg_copies;
struct UboRecord {
    VkBuffer buffer;
    VkDeviceSize offset;
    VkDeviceSize range;
    uint32_t pass, set_index, binding;
    VkDescriptorType type;
    bool valid;
};
UboRecord ubo_records[2][kUboSlots]{};
uint32_t ubo_record_count[2]{};
std::set<std::tuple<uint32_t, uint32_t, uint32_t>> ubo_seen[2];
std::mutex ubo_seen_mutex;
// IndirectArgs is armed on the render thread and READ at present - see the
// disarm site in the frame-root block for why those are two separate flags.
std::atomic_bool indirect_args_ready{false};
std::atomic_bool indirect_args_probe_armed{false};
void emit_indirect_args_table();
void indirect_args_flush_copies(VkCommandBuffer command_buffer);
// hook_cmd_bind_vertex_buffers sits above the definition.
void vertex_content_record(VkBuffer buffer, VkDeviceSize offset, uint32_t binding);

// ---------------------------------------------------------------------------
// PASS STATE PARITY (2026-08-19). Everything a draw at pass 25 READS is now
// measured and identical between the two calls: the draws themselves, the
// indirect args, every sampled binding, the vertex buffer CONTENTS
// (12/12 byte-identical hashes), and push constants (already closed in August
// - DOOM never calls vkCmdPushConstants). The pass renders into a real mirror
// on both sides.
//
// Same inputs, same commands, different output leaves only the STATE the
// rasteriser runs under. Three things can silently discard a draw without any
// counter noticing:
//   - an empty or wrong SCISSOR rect: rasterisation is clipped away entirely
//   - a wrong VIEWPORT: geometry lands outside the target
//   - a different PIPELINE: different blend/depth/write masks
// and a fourth that is not state but is invisible to everything so far: which
// DEPTH attachment the pass tests against. Particles at pass 25 are
// depth-tested, so a depth buffer holding the other eye's geometry rejects
// them wholesale while opaque geometry, drawn earlier with depth writes, still
// looks correct.
//
// All four, both calls, passes 22-26. Log-only.
std::atomic_bool pass_state_parity_armed{false};
ParityBudget pass_state_budget{48};

// ---------------------------------------------------------------------------
// FRAGMENT REJECTION EXPERIMENT (2026-08-19). Not a probe - an experiment with
// a visible answer, because probes have run out of things to say.
//
// At pass 25 every measurable thing is now IDENTICAL between the two calls:
// draws, indirect args, sampled bindings, vertex bytes, push constants,
// pipeline handle, viewport, scissor, and the depth attachment (mirror exists,
// mirrorWritten=true). A GPU handed identical commands and identical data does
// not produce different output. So the fragments are being DISCARDED, and only
// four things discard a fragment: depth test, stencil test, shader discard,
// and a zero blend factor. Shader inputs and blend state live in bindings and
// the pipeline, both proven identical - which leaves the depth and stencil
// CONTENTS of our mirror, the one thing no probe can see and the one thing
// that differs by construction, because our mirror's depth/stencil is whatever
// the doubled call happened to leave there rather than what DOOM built.
//
// Modes cycle on SHIFT+F9 and act ONLY on our own mirror depth image, never on
// DOOM's - eye 0 cannot be affected by any of them:
//   1 stencil=0x00 : if the effects test stencil against 0, this admits them
//   2 stencil=0xFF : the opposite polarity, so one of 1/2 admits either way
//   3 depth=1.0    : far plane everywhere, so every LESS/LEQUAL depth test
//                    passes. If the sparks appear here, depth is rejecting
//                    them and the fix is to give eye 1 correct depth.
// If NONE of the three changes anything, depth and stencil are eliminated
// behaviourally rather than by inference, and the cause is elsewhere.
std::atomic_uint32_t doubled_depth_experiment{0};
// SEPARATE counters for hit and miss. Sharing one meant three misses could
// exhaust the quota before the first hit ever printed, so a working run could
// look identical to a void one - the same starvation shape as the probe
// budgets, in a two-line diagnostic.
std::atomic_uint32_t doubled_depth_experiment_applied{0};
std::atomic_uint32_t doubled_depth_experiment_missed{0};
// APPLIED ACROSS A RANGE, NOT AT A PINNED PASS.
//
// Pinning cost two runs. First 22, which PassStateParity had already shown has
// no depth attachment. Then 23, which PassStateParity HAD resolved depth for -
// and which still came back doomDepth=0x0, because pass NUMBERS ARE NOT STABLE
// BETWEEN FRAMES: DrawParity has recorded pass 25 with 95, 97 and 98 draws on
// different runs, and the storage-buffer probe moved between pass 16 and 18
// across sessions. A number that identifies the right pass in one frame
// identifies a different one in the next.
//
// So the clear is attempted at EVERY doubled pass in the effects range and
// applies wherever a mirror depth actually resolves. Repeated clears are
// harmless here - each happens before its render pass begins, so what pass 25
// sees is the most recent one - and this cannot miss the target because it
// never has to name it. The log records which passes it landed on, which also
// answers where depth actually lives without another run to find out.
constexpr uint32_t kDepthExperimentFirstPass = 18;
constexpr uint32_t kDepthExperimentLastPass = 30;
constexpr uint32_t kPassStateFirstPass = 22;
constexpr uint32_t kPassStateLastPass = 26;
// Dedupe per side on (pass, kind, value) - DOOM re-sets scissor and rebinds
// pipelines per draw, and 97 draws would spend the budget on repeats.
std::set<std::tuple<uint32_t, uint32_t, uint64_t>> pass_state_seen[2];
std::mutex pass_state_mutex;

// kind: 0=pipeline 1=scissor 2=viewport 3=depth attachment
// Keep the diagnostic available, but do not format thousands of discarded
// messages per frame when it is unarmed (or the pass/budget rejects a record).
template<class Detail>
void pass_state_record(uint32_t kind, uint64_t key, const Detail& detail) {
    if (!pass_state_parity_armed.load(std::memory_order_relaxed)) return;
    const bool doubled = inside_frame_root_double;
    if (!doubled && !inside_real_frame_root) return;
    const uint32_t pass = doubled ? doubled_pass_seq : real_pass_seq;
    if (pass < kPassStateFirstPass || pass > kPassStateLastPass) return;
    const uint32_t side = doubled ? 1u : 0u;
    {
        std::scoped_lock lk(pass_state_mutex);
        if (!pass_state_seen[side].insert({pass, kind, key}).second) return;
    }
    if (!pass_state_budget.take(doubled)) return;
    static const char* kKind[] = {"pipeline", "scissor ", "viewport", "depthAtt"};
    log::warn(std::format("PassState [rec] {} pass={:2} {} {}",
        doubled ? "DOUBLED" : "REAL   ", pass, kKind[kind < 4 ? kind : 0], detail()));
}

// Separate functions, not inlined into the scissor/viewport hooks: those use
// __try, and MSVC rejects __try in any frame that also requires object
// unwinding (C2712), which std::format does.
void pass_state_record_scissor(const VkRect2D& r, uint32_t count) {
    pass_state_record(1,
        (static_cast<uint64_t>(r.extent.width) << 32) ^ r.extent.height ^
            (static_cast<uint64_t>(static_cast<uint32_t>(r.offset.x)) << 16) ^
            static_cast<uint32_t>(r.offset.y),
        [&]{return std::format("count={} rect0=({},{} {}x{}){}", count, r.offset.x, r.offset.y,
            r.extent.width, r.extent.height,
            (r.extent.width == 0 || r.extent.height == 0)
                ? "  <<<< EMPTY - DISCARDS EVERY DRAW" : "");});
}

void pass_state_record_viewport(const VkViewport& v, uint32_t count) {
    pass_state_record(2,
        (static_cast<uint64_t>(static_cast<uint32_t>(v.width)) << 32) ^
            static_cast<uint32_t>(v.height),
        [&]{return std::format("count={} vp0=({:.0f},{:.0f} {:.0f}x{:.0f} z {:.3f}..{:.3f})",
            count, v.x, v.y, v.width, v.height, v.minDepth, v.maxDepth);});
}

// THE ONE HDR COLOUR INPUT OF THE DEFERRED CHAIN (2026-08-19, Run 3).
//
// Run 2 established that every deferred input is mirror-backed and that both
// calls bind identical views, so the remaining explanation is CONTENT: a
// mirror correctly created, bound and substituted, into which the right data
// was never written. Run 2's own falsification clause named this instrument.
//
// Which mirror? The deferred chain's inputs are, by format: depth (D24S8),
// R8G8B8A8 albedo, two R16G16 half-float G-buffer channels, R16/R8 masks -
// and exactly ONE full-screen B10G11R11_UFLOAT (format 122) at pass 19
// binding 6, carrying usage 0x14: COLOUR ATTACHMENT plus SAMPLED. Something
// renders into it and pass 19 reads it. It is the only input in the chain
// shaped like an additive HDR layer, which is what fog, lava glow and sparks
// collectively are.
//
// Captured for BOTH sides so the comparison is same-frame: DOOM's real image
// and our mirror of it. Dumped through the existing dump_image_snapshot path
// - the same machinery that already produces the stage PPMs - so this adds a
// measurement, not a new mechanism.
VkImage deferred_hdr_input_real = VK_NULL_HANDLE;
VkImage deferred_hdr_input_mirror = VK_NULL_HANDLE;
uint32_t deferred_hdr_input_pass = 0;
VkExtent2D deferred_hdr_input_extent{};

// CALLER SPECIFICITY (2026-08-18), the replacement for the refuted FinalPass
// hook - see the ⚠ block near DoomRenderViewFn.
//
// 0x189A350 failed as a doubling target because it is the function the final
// pass's call site sits IN, and that function is generic: ~1000 calls per
// frame across 10+ threads. What is needed is a function SPECIFIC to the
// final pass - one that runs when the swapchain-writing pass runs and at no
// other time.
//
// That is answerable with NO new DOOM hooks. The stack walk already exists;
// the only change is to run it at EVERY render pass rather than only the
// post-frame-root one, tally each caller fnRva by whether its pass wrote a
// swapchain image, and report the ones that appear for the swapchain pass and
// NEVER for anything else. Those are the candidates - and everything else is
// disqualified by measurement instead of by assumption, which is the step
// that was skipped last time.
//
// Bounded and armed: a stack walk per render pass is not free and DOOM begins
// ~65 of them per frame.
struct CallerTally {
    uintptr_t fn_rva;
    uint32_t swapchain_passes;
    uint32_t other_passes;
    // Deepest chain index this caller was seen at. The OUTERMOST specific
    // frame is the one to double: it is the largest scope that still touches
    // only the final pass, where a shallower one may be a leaf helper doing
    // just part of it.
    uint32_t max_depth;
};
constexpr uint32_t kMaxCallerTallies = 96;
CallerTally caller_tallies[kMaxCallerTallies]{};
uint32_t caller_tally_count = 0;
// Leaf mutex - nothing is locked inside it, and it is taken only while armed.
std::mutex caller_tally_mutex;
// FINAL STAGE DOUBLING - see inside_final_pass_double.
std::atomic_bool final_pass_double_armed{false};
// Set only when the derived entry actually hooked; step 35 refuses to arm
// the doubling without it, so a failed hook cannot look like a failed fix.
std::atomic_bool final_stage_hook_installed{false};
// GRADED STEREO presentation - see the block in hook_queue_present.
std::atomic_bool graded_stereo_enabled{false};
// GRADE-GAP MODE (2026-08-29, plan EYE1-GRADE-AND-EFFECTS step 1).
//
// One knob with three rungs instead of two independent booleans, because the
// falsifier this step carries is about the SECOND INVOCATION, not about
// presentation: re-entering DOOM's final pass may disturb its next frame the
// way a second frame-root entry does. Two booleans cannot separate those; a
// ladder can.
//
//   0 OFF     - no doubled final pass, eye 1 keeps its ungraded frame-root
//               mirror. This IS the confirmed eye-0 config (the rollback
//               point in plan section 1) and is the ladder's POSITIVE
//               CONTROL: eye 0 must be clean here or the run is void.
//   1 DOUBLE  - the final pass is re-invoked, but eye 1 is still presented
//               its ungraded mirror. Isolates the COST of the second
//               invocation on eye 0 from the presentation change. Not a
//               single-variable step: the same flag also enables the
//               MirrorLockstep barrier append, so "cost" here means re-entry
//               plus those barriers. Named on the arm line so it cannot be
//               misread as re-entry alone.
//   2 GRADED  - re-invoked AND presented: eye 1 = final_pass_mirror_color,
//               graded + HUD, fmt 44. The state under test.
//
// So "eye 0 regressed" is attributable in one run: it appears at rung 1 and
// the second invocation is the cost; it appears only at rung 2 and the fault
// is in presentation/layout, not in re-entry.
std::atomic_uint32_t graded_mode{0};
// Forces a short burst of BaselinePair lines immediately after any mode
// change, so toneMatch is readable AT the transition rather than up to 500
// presents later. Read the arm label at the measurement: the mode is printed
// on the line itself.
std::atomic_uint32_t baseline_pair_burst{0};
// MONITOR EYE-1 VIEW (2026-08-29). The workflow change that makes eye-1
// questions answerable on a FLAT RUN.
//
// THE PROBLEM IT SOLVES. Every eye-1 judgement so far has cost a headset run,
// because eye 1 exists only as an image handed to the XR submission path - and
// that whole path, including the eye-source selection itself, sits inside
// `if (xr().ready())`. Headset-free, eye 1 is never even selected, which is
// why xrPresentPathHits exists and why a run was voided on 2026-08-28.
//
// THE MECHANISM, and it is not a blit. hook_cmd_begin_render_pass normally
// swaps the doubled final pass's framebuffer for one of OUR mirrors, so the
// second invocation writes our image instead of DOOM's swapchain. The comment
// there calls writing DOOM's swapchain "corruption", and for a headset run it
// is. On a flat run it is exactly what we want: let the doubled final pass
// keep DOOM's own framebuffer and it renders eye 1's graded, HUD-carrying
// frame straight into the image on the monitor, using DOOM's own code, DOOM's
// own command buffer and DOOM's own barriers. Nothing new is submitted and no
// layout is guessed at.
//
// WHY THIS IS STRONGER THAN A BLIT WOULD BE. Both eyes are then produced by
// the IDENTICAL final pass into the IDENTICAL target, one after the other in
// the same frame. The tone confound that §3 of the plan spent a whole phase
// removing is structurally impossible here - there is no second colour path
// for it to hide in. Toggling the mode is a true like-for-like A/B of eye 0
// against eye 1 on one screen.
//
// THE BUILT-IN POSITIVE CONTROL: eye 1 renders at +half separation, so the
// image MUST visibly shift viewpoint when this is switched on. If it does not
// shift, the doubled pass read DOOM's real composite rather than eye 1's and
// the mode is VOID - not evidence that the eyes match. Read 'FinalPassSubst'
// and final_pass_redirects for the same fact in the log.
//
// ⚠ REFUSED WHILE THE HEADSET PATH IS LIVE. In this mode the doubled pass
// overwrites the presented frame, which IS eye 0 - the confirmed reference.
// Eye 0 outranks eye 1, so the refusal is in code rather than in a comment.
std::atomic_bool monitor_eye1_direct{false};
std::atomic_uint32_t monitor_eye1_passes{0};
std::atomic_uint32_t final_stage_calls{0};
std::atomic_uint32_t final_pass_doubles{0};
std::atomic_uint32_t final_pass_refused_no_mirror{0};
std::atomic_bool final_pass_dump_pending{false};
// Set by the final-stage hook, consumed on the PRESENT thread - the doubled
// pass has only been RECORDED when the hook returns, not executed.
std::atomic_bool final_pass_dump_ready{false};
// THE question this run has to answer: did the doubled final pass actually
// READ eye 1's composite, or did it grade DOOM's real one? Effects are
// present in its output, and eye 1's composite is supposed to LACK them -
// which points at the second. If this counter is 0, the graded right eye is a
// copy of the left eye's viewpoint: full quality, ZERO parallax.
std::atomic_uint64_t final_pass_redirects{0};
// SEPARATE BUDGETS (2026-08-18). One shared cap let the REAL invocation, which
// runs first, consume all 40 lines before the DOUBLED one logged anything - so
// the run produced no record of the half that actually matters. A shared quota
// between a frequent event and a rare one is not a sample, it is a blind spot.
std::atomic_uint32_t final_pass_subst_lines{0};
std::atomic_uint32_t mirror_lockstep_barriers{0};
std::atomic_uint32_t storage_to_general_barriers{0};
// The 16384x16384 light-space shadow atlas DOOM owns and we deliberately do
// NOT mirror (a private copy costs 1 GB). Recorded so the doubled call can be
// stopped from writing into it - see the branch in ensure_mirror_framebuffer.
std::atomic_uintptr_t shared_shadow_framebuffer{0};
// DEFAULT FLIPPED TO FALSE, 2026-08-28. This suppression was S3's cause, not
// its cure: with it OFF the shadow sliding stops, confirmed live by A/B in the
// headset on the baseline where eye 0 is DOOM's own presented frame.
//
// The mechanism is the gap this flag never closed: suppressing DRAWS does not
// suppress the RENDER PASS. The doubled call still enters DOOM's shared
// 16384x16384 shadow pass every frame - the census measured ~44 blocked draws a
// frame, over 2.3 million in one session - and if that pass clears, the atlas
// is wiped and then never refilled, so DOOM's own next frame samples shadows
// that are partly missing. Shadows that lag and slide against geometry as the
// player moves is exactly what that produces, and exactly what was reported.
//
// The justification for turning it on in the first place (2026-08-18: the
// doubled call re-renders the atlas from a shifted camera with different
// culling) was measured BEFORE the render-surface hook at DOOM+0x1A367E0 was
// found to be corrupting surface submission and removed on 2026-08-28. Every
// measurement from before that date is on a corrupted baseline, this one
// included, so it does not outweigh a clean live A/B.
//
// The re-render it permits is also far cheaper than it looks: a shadow atlas is
// LIGHT-space, so a lateral camera shift changes only which geometry is culled
// into it, not the shadow projection itself - and at 3.0 uu that difference is
// tiny next to a wiped atlas.
//
// A cleaner fix probably exists - keep the doubled call out of the shadow pass
// entirely, or force its loadOp to LOAD so the real call's content survives -
// but that is a change to pass setup and this is a one-line default backed by a
// live test. SHIFT+F7 A/Bs it either way.
std::atomic_bool suppress_doubled_shadow_draws{false};
std::atomic_uint64_t suppressed_shadow_draws{0};
std::atomic_uint32_t shadow_identity_lines{0};
std::atomic_bool fog_hunt_armed{false};
std::atomic_bool storage_buffer_probe_armed{false};
std::atomic_bool suppress_doubled_light_culling{true};
std::atomic_uint64_t suppressed_light_culling_dispatches{0};
std::unordered_set<VkBuffer> storage_buffer_seen;
std::mutex storage_buffer_seen_mutex;
std::atomic_uint32_t storage_buffer_probe_lines{0};
constexpr uint32_t kMaxStorageBufferProbeLines = 40;
// EFFECTS STAGE DUMP (2026-08-18) - see the block in hook_cmd_begin_render_pass.
// Selected by SHAPE, not pass number: the first full-screen HDR mirror
// ("light", post-lighting) and the last one ("xparency", final composite).
// The previous version pinned pass 18/27 from a stale map and dumped two
// 954x508 side buffers - the effects live in the full-screen composite.
// MULTI-STAGE (2026-08-18). "First full-screen mirror of any format" picked
// the 2-channel G-buffer at pass 10, which cannot show effects at all. Sample
// only the HDR colour chain (B10G11R11), at up to kEffectsStages DISTINCT
// images across the frame, so the pass where fog/fire/sparks should appear is
// inside the captured range rather than at one of its ends.
constexpr uint32_t kEffectsStages = 5;
VkImage effects_stage_colors[kEffectsStages]{};
uint32_t effects_stage_passes[kEffectsStages]{};
uint32_t effects_stage_count = 0;
VkExtent2D effects_stage_extent_shared{};
// DOOM's OWN colour targets at the same passes, same frame - the counterpart
// the adversarial review's §5 required before any buffer could be dismissed.
VkImage real_stage_colors[kEffectsStages]{};
uint32_t real_stage_passes[kEffectsStages]{};
uint32_t real_stage_count = 0;
// VERTEX SOURCE PARITY - see hook_cmd_bind_vertex_buffers. Window covers the
// deferred/forward range where the effects are submitted.
// SEPARATE BUDGETS AND DEDUPE. A single shared cap has now starved this
// project's probes THREE times - FinalPassBindings, StorageBufferProbe, and
// this one - always the same way: the frequent event (the real call, running
// first) spends the whole budget before the rare one it is being compared
// against logs anything. A comparison probe must never share a quota between
// the two things it compares, and must dedupe, or the budget goes on repeats.
std::atomic_bool vertex_parity_armed{false};
std::atomic_uint32_t vertex_parity_lines_real{0};
std::atomic_uint32_t vertex_parity_lines_doubled{0};
constexpr uint32_t kMaxVertexParityLines = 24;
std::set<std::pair<uint32_t, uint64_t>> vertex_parity_seen_real;
std::set<std::pair<uint32_t, uint64_t>> vertex_parity_seen_doubled;
std::mutex vertex_parity_mutex;
constexpr uint32_t kVertexParityFirstPass = 19;
constexpr uint32_t kVertexParityLastPass = 34;
std::atomic_bool effects_stage_dump_armed{false};
std::atomic_bool effects_stage_dump_ready{false};
std::atomic_uint32_t effects_capture_index{0};
std::atomic_uint32_t effects_capture_suffix{0};
VkImage effects_stage_light_color = VK_NULL_HANDLE;
VkImage effects_stage_xparency_color = VK_NULL_HANDLE;
VkFormat effects_stage_light_format = VK_FORMAT_UNDEFINED;
VkFormat effects_stage_xparency_format = VK_FORMAT_UNDEFINED;
VkExtent2D effects_stage_light_extent{};
VkExtent2D effects_stage_xparency_extent{};
uint32_t effects_stage_light_pass = 0;
uint32_t effects_stage_xparency_pass = 0;
std::atomic_uint32_t fog_hunt_lines{0};
constexpr uint32_t kMaxFogHuntLines = 60;
constexpr uint32_t kMaxShadowIdentityLines = 40;
std::atomic_uint32_t final_pass_binding_lines_real{0};
std::atomic_uint32_t final_pass_binding_lines_doubled{0};
constexpr uint32_t kMaxFinalPassBindingLines = 12;
// The framebuffer DOOM's REAL final pass rendered into, recorded so the
// second invocation can be pre-flighted: we mirror that framebuffer BEFORE
// re-invoking, and refuse to re-invoke at all if we cannot. Without that
// pre-flight a mirroring miss would let the second pass write straight into
// the swapchain and corrupt the frame the owner is looking at.
VkFramebuffer last_final_pass_framebuffer = VK_NULL_HANDLE;
VkImage final_pass_mirror_color = VK_NULL_HANDLE;
VkFormat final_pass_mirror_format = VK_FORMAT_UNDEFINED;
VkExtent2D final_pass_mirror_extent{};
// First few Eye1Layout lines, then decimated - see the emit site in
// hook_queue_present.
std::atomic_uint32_t eye1_layout_lines{0};
// FINAL-STAGE DUMP SERIES (2026-08-20). The dump used to be one-shot, and
// that is unreadable for this question: fog is continuous but fire and
// SPARKS are intermittent, so a single automatic frame can easily land on a
// moment with no sparks in EITHER eye - a pair that shows nothing and proves
// nothing, at the cost of a whole run. Four pairs spaced ~150 presents apart
// (~2.5 s at 60 Hz) make it very unlikely that every sample is a quiet one,
// and a quiet sample is self-evident because the CONTROL is empty too.
std::atomic_uint32_t final_pass_dump_series{0};
std::atomic_uint32_t final_pass_dump_rearm_at{0};
// VISUAL-ONLY SWITCH - see the arming site. Read ONCE, from a file rather than
// the environment: DOOM inherits Steam's environment, not the injector's, so an
// env var never arrives (that mistake voided a bisect run on 2026-08-28).
// EYE SWAP switch - see the two offset sites and openxr_context.cpp's
// true_stereo branch. File-based for the same reason as the others: DOOM
// inherits Steam's environment, not the injector's.
inline bool stereo_swap_eyes_enabled() { return false; // KHARVOX owns eye order and diagnostics.
}
inline bool diagnostics_readbacks_enabled() { return false; // KHARVOX owns eye order and diagnostics.
}
// SYMPTOM RE-MEASUREMENT (2026-08-21). Widened from 4 samples / 150 presents
// to 8 / 240 - about 45 s of wall clock at the ~43 fps these runs measure,
// instead of 14 s.
//
// The reason is the measurement's known weakness, not a preference: eye 1 is
// judged by whether it TRACKS the eye-0 control, so a run where the control
// never varies proves nothing either way. Four samples in 14 s all landed on
// the same quiet moment three times out of four. Eight samples over 45 s is
// long enough for the owner to walk to a fire or a lava pool and for the
// control to actually swing, which is the only condition under which "eye 1
// is flat while the control moves" can be distinguished from "nothing was
// happening in either eye".
constexpr uint32_t kFinalPassDumpSeries = 2;   // cut from 8: this run is the stage bracket, the pair is only the symptom check.
constexpr uint32_t kFinalPassDumpGapFrames = 240;
// EFFECTS-STAGE AUTO SERIES (2026-08-20). The stage table is the measurement
// that names WHICH pass loses the additive layer, and until now it fired only
// on an F2 press "while sparks are visible". That cost a RenderDoc run: the
// press landed on a quiet frame, DOOM's own p22 came back px>200 = 0, and a
// control that is empty compares to nothing. The finalstage dumps in the very
// same run were fine because they auto-fire four times.
//
// So the stage capture arms itself the same way: four samples, spaced, no
// keypress and no timing judgement from someone who cannot see the screen
// properly anyway. F2 still works and is still useful for aiming at a
// specific moment; this just guarantees a floor.
std::atomic_uint32_t effects_series_taken{0};
std::atomic_uint32_t effects_series_rearm_at{0};
// OFF: ten full-res dumps per sample, each a queue drain and a 23 MB PPM.
// See the arm site in the scripted test for why. F2 still arms one sample.
// ON, FOR ONE SAMPLE (2026-08-21). THE STAGE BRACKET.
//
// This was off because each sample dumps ten full-resolution images, and four
// samples meant ~1 GB of mid-frame writes and "tons of hitches". Both true,
// and both the reason it is ONE sample here rather than four.
//
// It is back because it is the only measurement that can say WHERE the fog is
// lost rather than THAT it is lost, and because the last time it ran it was
// judged with whole-frame px>200 - which was later measured to have the WRONG
// SIGN for fog (stripping haze RAISES bright-pixel count). Every dump now
// carries BrightCensus FOG[dark30, meanLuma], which has the correct sign, so
// the same ten images finally answer the question they were taken for.
//
// COMPARE effects_stageN_pP against real_stageM_pP AT THE SAME PASS P, never
// across stages. A tone curve moves every value, and comparing stage 2 against
// stage 3 has already wasted one measurement in this project.
constexpr bool kEffectsAutoSeriesEnabled = true;
// Counts the diagnostic barriers - see serialise_before_doubled_pass. A zero
// here with the flag on means the doubled call never reached a render pass,
// which reads identically to "the barrier did not help" and must not.
std::atomic_uint32_t serialise_barriers_emitted{0};
constexpr uint32_t kEffectsSeries = 2;   // TWO samples: one per light-culling A/B arm.
constexpr uint32_t kEffectsSeriesGapFrames = 200;

// ---------------------------------------------------------------- TEST B --
// FRAME-RATE THROTTLE (2026-08-23). Nobody has ever separated "RenderDoc is
// loaded" from "the game is crawling", and RenderDoc does both.
//
// TEST A settled the first half and did NOT settle the second. The Khronos
// validation layer was loaded on the live broken run with synchronization
// validation on; it reported 1470 messages, so it was demonstrably live, and
// eye 1's deficit did not move (final-stage meanLuma ratio 80.9% with the
// layer against 79.1% without; the p17 stage ratio 24.5% against 25.7%). It
// also cost only ~28% of the frame rate - 31 fps against the baseline's ~43 -
// so it never made the game crawl. "A layer is loaded" is eliminated. "The
// game is slow" is untouched, because nothing has yet made it slow.
//
// This is the other half: sleep in the present path until the game runs at a
// few frames per second, with NO layer loaded and nothing else changed.
//
//   effects return  -> a race/ordering bug, and the fix is synchronisation.
//   effects missing -> timing is not the variable, RenderDoc changes memory
//                      contents or allocation behaviour, and the target
//                      becomes what the doubled call's lighting inputs
//                      actually CONTAIN.
//
// SCOPE, STATED BEFORE THE RESULT, because an elimination scoped narrower
// than its claim is not an elimination. A sleep in PRESENT serialises frame
// against frame and lets every background job quiesce between them. It cannot
// separate the real call's writes from the doubled call's reads INSIDE one
// submit. That half is already closed separately: kSerialiseDoubledCall put a
// full ALL_COMMANDS barrier before every doubled render pass on 2026-08-20,
// the path was proven live by serialise_barriers_emitted, and the result was
// negative. Intra-frame GPU ordering is therefore already eliminated with a
// positive control, and this run closes the inter-frame and CPU-side half.
//
// RESULT: NEGATIVE (2026-08-23, run 17:49). The throttle took - PresentThrottle
// measured 287.8 ms/frame, 3.5 fps, against the baseline's ~23 ms, and held
// there across the whole sample window - and the additive layer did NOT come
// back. At the first full-resolution HDR stage eye 1 read 33.9% of DOOM's
// px>128 where the working RenderDoc arm reads 117%; the final-stage meanLuma
// ratio was 84.4% against 79.1/79.4% untreated. TIMING IS NOT THE VARIABLE.
//
// With Test A (a layer is loaded: negative, 2026-08-23) and kSerialiseDoubledCall
// (intra-frame GPU ordering: negative, 2026-08-20), the whole synchronisation
// and timing family is now closed, each leg with its own positive control.
//
// Switched OFF rather than deleted, exactly like kSerialiseDoubledCall: it is
// the shape any future pacing test would take, and a throttle left switched on
// would make every later run 3 fps and be indistinguishable from a performance
// regression - the same class of trap as an A/B left in arm B.
constexpr bool kPresentThrottleEnabled = false;  // TEST B ran and is closed
constexpr uint32_t kPresentThrottleMs = 280;     // ~3.5 fps ceiling
std::atomic_bool present_throttle_armed{false};

std::atomic_bool caller_specificity_armed{false};
std::atomic_uint32_t caller_specificity_walks{0};
constexpr uint32_t kMaxCallerSpecificityWalks = 4000;
// Reports the caller tally - see CallerTally. Called from the scripted test
// once the observation window closes.
// RESOLVING A REAL, HOOKABLE FUNCTION ENTRY (2026-08-18).
//
// RtlLookupFunctionEntry returns the RUNTIME_FUNCTION whose range covers a
// PC. In an optimised build that is frequently a CHUNK of a function, not the
// function itself: MSVC splits functions into separated code ranges, and each
// chunk gets its own RUNTIME_FUNCTION whose UNWIND_INFO carries
// UNW_FLAG_CHAININFO pointing at the parent. BeginAddress is then the chunk's
// start - an address control never enters through, and therefore NOT a call
// target and NOT hookable.
//
// That is exactly what happened to 0x186E6BA: MinHook installed cleanly, DOOM
// ran normally, and the hook was never called ONCE in a whole session. A hook
// on a non-entry is not a loud failure, it is silence - which is the worst
// kind here, because it is indistinguishable from "the feature did not work".
//
// Following the chain gives the primary entry, which is a real call target.
// Defined below, next to the mirror machinery it depends on; the installer
// here needs its address.
void __fastcall hook_final_stage(void* a1, void* a2, void* a3, void* a4);
uintptr_t primary_function_entry(uintptr_t doom_base, uintptr_t pc, bool* was_chunk);

// CONTAINMENT (2026-08-18). Why the doubling crashed, and how the next target
// is chosen without guessing again.
//
// The outermost specific caller (fnRva 0x186E6BA -> entry 0x186E520) hooked
// correctly and ran once per frame with stable arguments - a3=0x7f0=2032 and
// a4=0xeea=3818, the frame height and width. Re-invoking it crashed DOOM
// before the doubling could even log completion. A final stage that writes
// the swapchain typically also SUBMITS it, and a submit is not re-entrant:
// calling it twice per frame double-submits and corrupts driver state.
//
// Doubling therefore needs the narrowest scope that still contains the whole
// grade+HUD RENDER PASS but does NOT contain the submit or the present. Three
// more specific candidates already exist (chain levels 4, 5 and 6), and which
// of them qualifies is measurable rather than guessable.
//
// Measured with NO new DOOM hooks, which matters after 0x189A350's artifacts:
// at each event of interest we already control - the swapchain render pass
// begin, vkQueueSubmit, vkQueuePresentKHR - walk the stack and record which
// candidate entries are on it. A candidate present at the render pass and
// ABSENT at submit/present is a safe target; one present at both is another
// 0x186E520 waiting to happen.
constexpr uint32_t kMaxFinalCandidates = 8;
uintptr_t final_candidates[kMaxFinalCandidates]{};
uint32_t final_candidate_depth[kMaxFinalCandidates]{};
uint32_t final_candidate_count = 0;
// [candidate][event] where event: 0=swapchain render pass, 1=queue submit,
// 2=queue present.
uint32_t containment_hits[kMaxFinalCandidates][3]{};
std::atomic_bool containment_armed{false};
std::mutex containment_mutex;

// Records, for one event, which candidate entries are currently on the stack.
void containment_sample(uint32_t event_index) {
    if (!containment_armed.load(std::memory_order_relaxed)) return;
    if (event_index > 2) return;
    const uintptr_t doom_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"DOOMx64vk.exe"));
    if (!doom_base) return;
    void* frames[24]{};
    const USHORT captured = CaptureStackBackTrace(0, 24, frames, nullptr);
    bool seen[kMaxFinalCandidates]{};
    for (USHORT i = 0; i < captured; ++i) {
        const uintptr_t pc = reinterpret_cast<uintptr_t>(frames[i]);
        if (pc < doom_base || pc - doom_base > 0x10000000) continue;
        bool chunk = false;
        const uintptr_t entry = primary_function_entry(doom_base, pc, &chunk);
        if (!entry) continue;
        for (uint32_t c = 0; c < final_candidate_count; ++c)
            if (final_candidates[c] == entry) seen[c] = true;
    }
    std::scoped_lock lock(containment_mutex);
    for (uint32_t c = 0; c < final_candidate_count; ++c)
        if (seen[c]) ++containment_hits[c][event_index];
}

void emit_containment() {
    std::scoped_lock lock(containment_mutex);
    for (uint32_t c = 0; c < final_candidate_count; ++c) {
        const uint32_t pass = containment_hits[c][0];
        const uint32_t submit = containment_hits[c][1];
        const uint32_t present = containment_hits[c][2];
        const bool safe = pass > 0 && submit == 0 && present == 0;
        log::warn(std::format(
            "Containment entryRva={:#x} swapchainRenderPass={} queueSubmit={} queuePresent={} "
            "| {}",
            final_candidates[c], pass, submit, present,
            safe ? "SAFE TO DOUBLE - contains the pass, not the submit"
                 : (pass == 0 ? "does not contain the pass - wrong scope"
                              : "CONTAINS SUBMIT/PRESENT - re-invoking it double-submits, which "
                                "is what crashed 0x186E520")));
    }
    log::warn(std::format(
        "Containment TOTAL candidates={}. Pick the one with swapchainRenderPass>0 and "
        "queueSubmit=queuePresent=0. If NONE qualifies, no DOOM function corresponds to the "
        "grade pass alone and the grade has to be duplicated by re-recording the pass's Vulkan "
        "commands instead of by re-invoking DOOM code.",
        final_candidate_count));

    // SELECT AND INSTALL, from this run's own containment data.
    //
    // OUTERMOST SAFE, not outermost: the previous attempt took the outermost
    // specific caller full stop, which turned out to enclose the submit and
    // crashed DOOM. The submit enters somewhere in the chain, and the target
    // has to be the largest scope strictly BELOW that boundary - big enough to
    // contain the whole grade+HUD pass, small enough to exclude the submit.
    uintptr_t best = 0;
    uint32_t best_depth = 0;
    for (uint32_t c = 0; c < final_candidate_count; ++c) {
        if (!containment_hits[c][0]) continue;                       // no grade pass
        if (containment_hits[c][1] || containment_hits[c][2]) continue;  // submits/presents
        if (best && final_candidate_depth[c] <= best_depth) continue;
        best = final_candidates[c];
        best_depth = final_candidate_depth[c];
    }
    if (!best) {
        log::error("Containment: NO candidate contains the grade pass without also containing "
                   "the submit. No DOOM function corresponds to the grade pass alone, so "
                   "re-invoking DOOM code cannot work at any level and the grade must be "
                   "duplicated by re-recording the pass's Vulkan commands. The doubling stays "
                   "disarmed - this is a refusal, not a silent no-op.");
        return;
    }
    void* target = reinterpret_cast<void*>(GetModuleHandleW(L"DOOMx64vk.exe"));
    target = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(target) + best);
    const MH_STATUS created = MH_CreateHook(target, reinterpret_cast<void*>(&hook_final_stage),
                                            reinterpret_cast<void**>(&real_final_stage));
    const MH_STATUS enabled = created == MH_OK ? MH_EnableHook(target) : created;
    if (enabled == MH_OK) {
        final_stage_hook_installed.store(true, std::memory_order_relaxed);
        log::warn(std::format(
            "FinalStage HOOK INSTALLED at entryRva={:#x} (chainDepth={}) - the OUTERMOST "
            "candidate that contains the grade+HUD pass and contains NEITHER the submit nor the "
            "present. Derived entirely from this run's own measurement. 'FinalStageProbe' lines "
            "must now appear once per frame; if they do not, nothing should be read from the "
            "doubling.",
            best, best_depth));
    } else {
        log::error(std::format(
            "FinalStage HOOK FAILED at entryRva={:#x} (create={} enable={}). Doubling stays "
            "disarmed.", best, static_cast<int>(created), static_cast<int>(enabled)));
    }
}

uintptr_t primary_function_entry(uintptr_t doom_base, uintptr_t pc, bool* was_chunk) {
    if (was_chunk) *was_chunk = false;
    if (!doom_base || !pc) return 0;
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(static_cast<DWORD64>(pc), &image_base, nullptr);
    if (!rf || image_base != static_cast<DWORD64>(doom_base)) return 0;
    // Bounded: a malformed or hostile chain must not spin the render thread.
    for (int guard = 0; guard < 8; ++guard) {
        const auto* unwind = reinterpret_cast<const uint8_t*>(doom_base + rf->UnwindData);
        const uint8_t flags = static_cast<uint8_t>(unwind[0] >> 3);
        constexpr uint8_t kChainInfo = 0x4;  // UNW_FLAG_CHAININFO
        if (!(flags & kChainInfo)) return rf->BeginAddress;
        if (was_chunk) *was_chunk = true;
        // The chained RUNTIME_FUNCTION follows the unwind codes, which are
        // padded to an even count. Layout is fixed by the x64 ABI.
        const uint8_t code_count = unwind[2];
        rf = reinterpret_cast<PRUNTIME_FUNCTION>(
            const_cast<uint8_t*>(unwind + 4 + ((code_count + 1) & ~1) * 2));
    }
    return rf->BeginAddress;
}

void emit_caller_specificity() {
    std::scoped_lock lock(caller_tally_mutex);
    uint32_t specific = 0;
    for (uint32_t i = 0; i < caller_tally_count; ++i) {
        const CallerTally& t = caller_tallies[i];
        if (!t.swapchain_passes) continue;          // never in the final pass at all
        const bool unique_to_final = t.other_passes == 0;
        if (unique_to_final) ++specific;
        // The chunk-vs-entry distinction, which cost a whole run: the address
        // tallied here is where the PC fell, and that is not necessarily a
        // call target. entryRva is the address worth hooking.
        bool was_chunk = false;
        const uintptr_t entry =
            primary_function_entry(reinterpret_cast<uintptr_t>(GetModuleHandleW(L"DOOMx64vk.exe")),
                                   reinterpret_cast<uintptr_t>(GetModuleHandleW(L"DOOMx64vk.exe")) +
                                       t.fn_rva,
                                   &was_chunk);
        log::warn(std::format(
            "CallerSpecificity fnRva={:#x} entryRva={:#x} isChunk={} swapchainPasses={} "
            "otherPasses={} | {}",
            t.fn_rva, entry, was_chunk, t.swapchain_passes, t.other_passes,
            unique_to_final ? "SPECIFIC TO THE FINAL PASS - candidate doubling target"
                            : "shared with other passes - NOT a doubling target"));
    }
    log::warn(std::format(
        "CallerSpecificity TOTAL walks={} distinctCallers={} specificToFinalPass={}.\n"
        "  A candidate must have otherPasses=0: it runs for the swapchain-writing pass and for "
        "nothing else. 0x189A350 will appear here with a LARGE otherPasses, which is exactly why "
        "doubling it was wrong - it is the function the call site sits in, not one unique to it.\n"
        "  specificToFinalPass=0 would mean every caller on that stack is shared, so no DOOM "
        "function corresponds to the final pass alone and duplicating it has to be done by "
        "re-recording the pass's Vulkan commands rather than by re-invoking DOOM code.\n"
        "  Pick the DEEPEST specific frame (largest chain index): the shallowest one may be a "
        "leaf helper that does only part of the pass.\n"
        "  ⚠ HOOK entryRva, NEVER fnRva. fnRva is where the PC fell, and in an optimised build "
        "that is often a CHUNK of a function whose UNWIND_INFO chains to a parent - an address "
        "control never enters through, so a hook there installs cleanly and is never called. "
        "0x186E6BA cost a full run exactly that way: MinHook succeeded, DOOM ran fine, and the "
        "hook fired ZERO times. isChunk=true says the two differ and only entryRva is a call "
        "target.",
        caller_specificity_walks.load(std::memory_order_relaxed), caller_tally_count, specific));

    // SELF-TARGETING (2026-08-18). The address to hook is now DERIVED here and
    // hooked immediately, rather than read out of a log by a human and typed
    // into the hook table next session. That closes both failures this step
    // has already had: a hardcoded RVA that was generic (0x189A350), and a
    // hardcoded RVA that was a chunk and therefore never called (0x186E6BA).
    // A target that is measured, entry-resolved and installed in one pass
    // cannot be either.
    // COLLECT the candidates instead of hooking one. Doubling the outermost
    // (entry 0x186E520) crashed DOOM: it contains the submit, and a submit is
    // not re-entrant. Which candidate is narrow enough is now MEASURED by the
    // containment probe rather than chosen by depth, and nothing is hooked or
    // re-invoked until it has been.
    const uintptr_t doom_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"DOOMx64vk.exe"));
    {
        std::scoped_lock lock(containment_mutex);
        final_candidate_count = 0;
        std::memset(containment_hits, 0, sizeof(containment_hits));
        for (uint32_t i = 0; i < caller_tally_count; ++i) {
            const CallerTally& t = caller_tallies[i];
            if (!t.swapchain_passes || t.other_passes) continue;  // not specific
            if (final_candidate_count >= kMaxFinalCandidates) break;
            bool was_chunk = false;
            const uintptr_t entry =
                primary_function_entry(doom_base, doom_base + t.fn_rva, &was_chunk);
            if (!entry) continue;
            final_candidate_depth[final_candidate_count] = t.max_depth;
            final_candidates[final_candidate_count++] = entry;
            log::warn(std::format(
                "FinalStage CANDIDATE entryRva={:#x} (fnRva={:#x} isChunk={} chainDepth={}). "
                "Not hooked: containment decides which of these is narrow enough to re-invoke.",
                entry, t.fn_rva, was_chunk, t.max_depth));
        }
    }
    if (!final_candidate_count) {
        log::error("CallerSpecificity: no specific caller resolved to a function entry. The "
                   "final-stage doubling CANNOT run - a refusal, not a silent no-op.");
    }
}

// Emits the one table this whole step exists to produce. Called on the
// render thread immediately after the doubled call returns.
//
// READ IT LIKE THIS. Pass N is the Nth vkCmdBeginRenderPass of each call,
// and PingPongParity has already established both calls run the same NUMBER
// of passes - so if the counts printed on the TOTAL line differ, the rows
// are misaligned and nothing below the first divergence means anything.
// Dispatches outside a render pass are attributed to the most recently
// begun pass, the same convention on both sides, so they stay comparable.
void emit_draw_parity_table(uint32_t attempt) {
    uint64_t rd = 0, dd = 0, ri = 0, di = 0, rv = 0, dv = 0, rx = 0, dx = 0;
    uint64_t rs = 0, ds = 0;  // dispatches suppressed, per side
    uint32_t rp = 0, dp = 0, divergent = 0;
    for (uint32_t s = 0; s < kDrawParitySlots; ++s) {
        const DrawParitySlot& r = draw_parity_real[s];
        const DrawParitySlot& d = draw_parity_doubled[s];
        rd += r.draws; dd += d.draws;
        ri += r.indirects; di += d.indirects;
        rv += r.vertices; dv += d.vertices;
        rx += r.indices; dx += d.indices;
        rs += r.dispatches_suppressed; ds += d.dispatches_suppressed;
        if (r.draws || r.indirects || r.dispatches) ++rp;
        if (d.draws || d.indirects || d.dispatches) ++dp;
        // EXECUTED, not recorded. A suppressed dispatch is recorded by this
        // hook and never reaches the driver, so the comparison that means
        // anything is against dispatches-minus-suppressed on each side.
        const uint32_t r_exec = r.dispatches - r.dispatches_suppressed;
        const uint32_t d_exec = d.dispatches - d.dispatches_suppressed;
        const bool same = r.draws == d.draws && r.indirects == d.indirects &&
                          r.vertices == d.vertices && r.indices == d.indices &&
                          r_exec == d_exec;
        if (!same) ++divergent;
        if (!r.draws && !d.draws && !r.indirects && !d.indirects &&
            !r.dispatches && !d.dispatches) continue;
        log::warn(std::format(
            "DrawParity [rec] pass={:3} REAL draws={:5} indirect={:4} verts={:9} idx={:9} "
            "disp={:3}(exec {:3}) job={:#x} | DOUBLED draws={:5} indirect={:4} verts={:9} "
            "idx={:9} disp={:3}(exec {:3}) job={:#x} | {}",
            s, r.draws, r.indirects, r.vertices, r.indices, r.dispatches, r_exec, r.job_rva,
            d.draws, d.indirects, d.vertices, d.indices, d.dispatches, d_exec, d.job_rva,
            same ? "SAME" : (d.draws == 0 && d.indirects == 0 && (r.draws || r.indirects)
                                 ? "DOUBLED-EMPTY"
                                 : (d_exec < r_exec ? "DISPATCHES-SUPPRESSED" : "DIVERGENT"))));
    }
    log::warn(std::format(
        "DrawParity TOTAL attempt={} realPasses={} doubledPasses={} (from the pass counters: "
        "real={} doubled={}) | REAL draws={} indirect={} verts={} idx={} | DOUBLED draws={} "
        "indirect={} verts={} idx={} | divergentPasses={} | dispatchesSuppressed REAL={} "
        "DOUBLED={}.\n"
        "  TIMELINE TAG [rec]: every number here is the CPU-RECORDED command stream. It can "
        "never rule out a GPU-time hypothesis. dispatchesSuppressed is the one column that "
        "bridges: it counts commands this DLL recorded and then did NOT hand to the driver, so "
        "disp minus exec is work that exists on the CPU timeline and not on the GPU one.\n"
        "  HOW TO READ THIS (review P3): passes with fewer or ZERO doubled draws => the "
        "per-frame draw data (particle/effect lists) is CONSUMED or GATED by the real call and "
        "the doubled call finds nothing to submit. Equal draws but divergent dispatch counts => "
        "the volumetric/effects COMPUTE is skipped or mis-fed. Full parity everywhere => the "
        "loss is in buffer CONTENTS, not in submitted work, and the fix moves to snapshotting "
        "per-frame data. Unequal realPasses/doubledPasses => the rows are MISALIGNED and this "
        "table says nothing; re-read PingPongParity first.",
        attempt, rp, dp, real_pass_seq, doubled_pass_seq,
        rd, ri, rv, rx, dd, di, dv, dx, divergent, rs, ds));
}

// COPY-DESCRIPTOR TRACKING (2026-08-18, adversarial review §4).
//
// The review's finding: hook_update_descriptor_sets receives
// vkCopyDescriptorSet copies and uses them ONLY to invalidate the clone
// cache. It never propagates content records srcSet -> dstSet and never
// stamps descriptor_set_update_frame, and nothing has ever measured whether
// DOOM uses copies at all. A set populated purely by copy after
// reallocation produces exactly run 26's signature (allocFrame=668
// updateFrame=355 boundWithoutUpdateSinceAlloc=true, no record) - so
// "noRecord contents unknowable by construction" was false; the path was
// visible in the very hook and dropped on the floor.
//
// descriptor_copies_seen answers the prior question the propagation depends
// on. If it reports 0, DOOM issues no copies, the propagation below
// provably did nothing this run, and the build stays log-only in effect as
// well as in intent.
std::atomic_uint32_t storage_identity_lines{0};
constexpr uint32_t kMaxStorageIdentityLines = 24;
// STORAGE-IMAGE MIRRORING (2026-08-18, Run A / review P3) - see
// ensure_mirror_storage_image. THE one behavioural change of run B.
std::atomic_bool storage_mirror_enabled{true};
std::atomic_uint32_t storage_mirror_created{0};
kharvox::native::StorageMirrorBudget<> storage_mirror_budget;
std::atomic_uint32_t storage_mirror_no_info{0};

std::atomic_uint64_t storage_mirror_substitutions{0};
std::atomic_uint64_t storage_mirror_layout_refusals{0};
std::atomic_uint64_t descriptor_copies_seen{0};
std::atomic_uint64_t descriptor_copy_records_propagated{0};
std::atomic_uint64_t descriptor_copy_image_records_propagated{0};

// ---------------------------------------------------------------------------
// ATTRIBUTION LOGGING (2026-08-17 evening, STEP 6.6 item 1).
//
// Two candidates were dismissed on COUNTS where CONTENT decides. The
// view-keying gap (76/run) and the safety-gate fallback (46/run) were each
// divided by 21,542 substituted binds and called negligible. That is the
// wrong denominator: a full-screen post pass has ONE input bind carrying the
// whole image, while a material pass has hundreds carrying almost nothing.
// 76/run is ~1 per frame - exactly the shape of one full-screen pass's
// primary input - and ONE such bind is enough to reconstitute eye 1 out of
// DOOM's unshifted content.
//
// So: identity, not counts. Every gap hit and every gate refusal names the
// pass, the job, the set/binding, the view, the image, and (for refusals)
// the reason. Log-only, bounded, and - Rule 9, the run-17 lesson - the
// strings are BUILT under the locks that are already held and EMITTED after
// they are released. No lock is taken beneath mirror_mutex, and no file I/O
// happens while the render thread holds it.
//
// Note on volume: get_redirected_set caches its verdict per (set,
// mirrorGeneration), so both numbers are per distinct SET EVALUATION, not
// per bind. 76 gaps therefore means 76 distinct sets, each of which then
// binds unredirected for the rest of the run. That makes identity cheaper
// AND makes the "one bind carries the whole image" reading more likely, not
// less.
constexpr uint32_t kMaxAttributionLines = 200;
// Gap lines are numbered by read_redirect_view_gap itself, so a line's #N is
// literally "the Nth gap of the run" and a gap in the numbering is visible.
//
// ⚠ CORRECTION to REVIEW UPDATE 2, from run 18's own log rather than from
// the source comments: the "46 safety-gate fallbacks" it describes are NOT
// safety-gate refusals. 46 is read_redirect_failures, which counts WHOLE-SET
// clone refusals - untracked descriptor types, no recorded layout,
// unclonable records - and all ten of run 18's sampled FALLBACK lines are
// the "binding appears as BOTH an image and a buffer" case. The per-binding
// safety-gate refusal (!written || layout mismatch) was NEVER counted at
// all; it only ever appeared as refusedMirrors=N inside the first 40 CLONE
// lines. This counter closes that hole so the two are never conflated again.
std::atomic_uint32_t attribution_gate_lines{0};
std::atomic_uint32_t read_redirect_gate_refusals{0};

// ---------------------------------------------------------------------------
// CHAIN WALK (2026-08-17 evening, STEP 6.6 item 2) - the one behavioural
// change in this step.
//
// Known: the mirrored G-buffer carries real parallax (13.9x near/far) and
// the final composite carries none. Both facts are solid; what is missing is
// everything BETWEEN them. Worse, the two were measured in different
// configurations - the G-buffer number is from FILTERED runs, and run 15
// established that unfiltered, later passes overwrite the opaque mirror
// before any end-of-frame dump can see it (mirrored views are deliberately
// shared). So "the composite lost the shift the G-buffer has" has never been
// a within-configuration comparison at all.
//
// Fix both at once: snapshot the colour mirror ON THE GPU TIMELINE at four
// pass boundaries of ONE doubled call, at both A/B offsets. A private
// vkCmdCopyImage recorded right after vkCmdEndRenderPass captures what that
// stage produced before anything downstream can overwrite it.
//
// Reading the four pairs left to right localises the failure to one link:
//   opaque pair already flat  -> hypothesis 6, the shift never reaches
//                                unfiltered opaque geometry; the read path is
//                                irrelevant and the fix is camera plumbing.
//   flat first at stage K     -> stage K reconstitutes geometry from an
//                                unshifted source; its identity lines say
//                                which bind.
//   shifted all the way       -> rendering is right and the fault is eye-1
//                                source selection or the present path.
//
// TARGETS are job fn RVAs, not pass indices, because pass indices are scene-
// shaped and RVAs are code addresses. Taken from the STEP 5 census table.
constexpr uint32_t kChainWalkSlots = 4;
constexpr std::array<uintptr_t, kChainWalkSlots> kChainWalkTargetRvas{{
    0x1A37CC0,   // RenderOpaque      - the G-buffer boundary (hypothesis 6)
    0x19AE9E0,   // pass 18           - shadow-consuming lighting
    0x1A39930,   // passes 25-46      - deferred chain
    0x186A2F0}}; // passes 49-78      - post chain, ending in the composite

// OBSERVE first, CAPTURE second. A slot's private image cannot be destroyed
// and re-created mid-call (a copy into it may already be recorded and not yet
// submitted - that would be a use-after-free on the GPU), so its extent and
// format must be known BEFORE the first copy. One observation pass over a
// live doubled call records the whole pass map, picks the last full-extent
// pass of each target RVA, and only then is capture armed. This also yields
// the pass map itself, which is the insurance against mis-targeting: if the
// four RVAs turn out to be wrong, the map says what to aim at without
// spending another live run.
// PASS TIMELINE (2026-08-19). The instrument this session actually needed.
//
// Every capture built so far - effects_stage, real_stage, the HDR input pair -
// reads its images AT PRESENT TIME, after the whole frame. "pass=N" in those
// filenames is where an image was first SEEN, not when its contents were
// taken. They are N different buffers read at one moment, not one buffer read
// at N moments, and reading them as a timeline is what put three runs of this
// session onto the wrong stage.
//
// This records vkCmdCopyImage at four PASS BOUNDARIES, so the copies execute
// in GPU order inside the frame and the four dumps really are four moments.
// The pass at which the doubled call's mirror fails to gain the sparks is then
// directly visible instead of inferred.
//
// PASSES RE-AIMED 2026-08-19 after the first two-sided run. That run measured
// pass 20 as 0 bright pixels in BOTH eyes and pass 25 as 10122 left / 273
// right, with pass 33 unchanged from 25 - so the additive layer is composited
// somewhere in 21..25 and nothing after 25 matters. Pass 30 was a wasted slot:
// its 102 eight-index quads are the HUD (format 37, rows 1748-1867), not
// particles.
//
// These four subdivide the surviving range. Passes with draws in there are 22
// (18 draws), 23 (7 draws) and 25 (95 draws + 10 indirect); 24 is included as
// the control - if it is empty on both sides the compositing is at 25 alone.
constexpr std::array<uint32_t, 4> kTimelinePasses{{22, 23, 24, 25}};

// BEFORE/AFTER THE SAME IMAGE (2026-08-19). The question every capture so far
// has dodged.
//
// All four timeline slots copy at pass END, and passes 24, 25 and 26 render
// into DIFFERENT images. So "the doubled call's p25 is no brighter than its
// p24" never showed that pass 25 added nothing - it compared two unrelated
// buffers. The only way to ask "did THIS pass add the effects" is to copy the
// SAME image at that pass's begin and at its end and difference them.
//
// It matters now because the input search is finished: at pass 25 the two
// calls are byte-identical in textures, uniform buffers, vertex data, indirect
// args, pipeline, viewport, scissor and target, differing only in the view
// matrix, which must differ. Identical inputs cannot produce different output,
// so either the draws do not execute as recorded, or the effects ARE rendered
// and lost afterwards. This separates those two, and they need opposite fixes.
//
// Doubled side only: that is the eye that is broken, and the fork does not
// need the other one. If BEFORE and AFTER match, pass 25 wrote nothing and the
// fragments are being discarded. If AFTER has the effects, they are being
// rendered correctly and destroyed downstream - a completely different bug
// from the one this session has been chasing.
std::atomic_bool timeline_beginend_armed{false};
VkImage beginend_image = VK_NULL_HANDLE;
VkDeviceMemory beginend_memory = VK_NULL_HANDLE;
VkExtent2D beginend_extent{};
VkFormat beginend_format = VK_FORMAT_UNDEFINED;
bool beginend_filled = false;
uint32_t beginend_pass = 0;
constexpr uint32_t kBeginEndPass = 25;
// REAL-SIDE counterpart, and the control this claim has never had.
//
// "Pass 25 is where the effects appear" came from comparing the p24 dump with
// the p25 dump - DIFFERENT IMAGES - which is the same cross-image error I
// identified and then repeated. The doubled call's own begin/end delta at pass
// 25 is +0.0001 mean, i.e. nothing; that is only evidence of a DISCARD if the
// real call's delta at the same pass is LARGE. If the real call also adds
// nothing there, then pass 25 is not where the effects are added and the
// localisation is wrong.
VkImage beginend_real_image = VK_NULL_HANDLE;
VkDeviceMemory beginend_real_memory = VK_NULL_HANDLE;
VkExtent2D beginend_real_extent{};
VkFormat beginend_real_format = VK_FORMAT_UNDEFINED;
bool beginend_real_filled = false;
uint32_t beginend_real_pass = 0;
std::atomic_bool pass_timeline_armed{false};
std::atomic_bool pass_timeline_ready{false};
// REAL-CALL SIDE (2026-08-19, second half of the instrument). The first
// version captured only the doubled call, which showed that eye 1 never gains
// the bright content at ANY of the four passes - but with nothing to compare
// against it could not name the pass where the two calls part company. That
// is the whole question, so both sides get a timeline.
//
// DOOM's targets are not mirrors, so the source layout cannot be taken from
// mirror_image_layouts. It is read from render_pass_final_layouts, which is
// recorded at vkCreateRenderPass from pAttachments[i].finalLayout - the layout
// the render pass itself guarantees on exit. Guessing this is what hung the
// GPU three times earlier in the project; it is not guessed here, and a pass
// whose final layout is unknown is SKIPPED with a line rather than copied.
struct RealTimelineSlot {
    VkImage image{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    VkExtent2D extent{};
    VkFormat format{VK_FORMAT_UNDEFINED};
    bool create_failed{};
    bool filled{};
    uint32_t filled_pass{};
};
std::array<RealTimelineSlot, 4> real_timeline_slots{};
// Set at begin-render-pass for the REAL call, consumed at end-render-pass -
// the exact counterpart of chainwalk_current_color for the doubled side.
thread_local VkImage real_timeline_current_color = VK_NULL_HANDLE;
thread_local VkFormat real_timeline_current_format = VK_FORMAT_UNDEFINED;
thread_local VkExtent2D real_timeline_current_extent{};
thread_local VkImageLayout real_timeline_current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
thread_local bool real_timeline_layout_known = false;
thread_local VkCommandBuffer real_timeline_current_cmd = VK_NULL_HANDLE;
std::atomic_bool chainwalk_observe{false};
std::atomic_bool chainwalk_ready{false};
std::atomic_bool chainwalk_capture{false};
// Camera-record classification rider (STEP 6.6 item 3, hypothesis 5/6).
std::atomic_bool chainwalk_camera_armed{false};

struct ChainWalkPass {
    uintptr_t rva{};
    VkFramebuffer fb{VK_NULL_HANDLE};
    bool mirrored{};
    VkImage color{VK_NULL_HANDLE};
    VkFormat format{VK_FORMAT_UNDEFINED};
    VkExtent2D extent{};
    // Whether this render pass has a RECORDED finalLayout for its colour
    // mirror. Without one, a snapshot barrier would have to guess an
    // oldLayout, and a wrong guess on a mirror image is undefined behaviour -
    // the failure mode behind three GPU hangs this stream. Such a pass is
    // excluded from slot selection rather than snapshotted on a guess.
    bool layout_known{};
};
// Indexed by doubled_pass_seq, same slot count and clamping as the census.
thread_local std::array<ChainWalkPass, kCensusPassSlots> chainwalk_map{};

// Per-pass camera-record classification. The census already knows how to
// find camera-shaped dynamic uniform records; this records the translation
// COLUMN (v[3] v[7] v[11]) per pass so each pass can be classified against
// the same frame's REAL call. Hypothesis 5 predicts lighting/post binding an
// unshifted record; hypothesis 6 predicts even the opaque passes binding one.
// A pass can bind MORE than one camera-shaped record - a world camera and a
// light or reflection view, say. Keeping only one of them and classifying
// that would make the verdict depend on which happened to be last, which is
// how a probe manufactures a clean answer. Keep several DISTINCT ones and
// classify the pass on the whole set.
constexpr uint32_t kChainWalkCamRecords = 4;
struct ChainWalkCam {
    bool seen{};
    uint32_t records{};   // total seen, including duplicates and overflow
    uint32_t distinct{};  // how many of the arrays below are populated
    uintptr_t rva{};
    std::array<float, kChainWalkCamRecords> x{}, y{}, z{};
};
thread_local std::array<ChainWalkCam, kCensusPassSlots> chainwalk_cams{};
// Attempt 0's records, kept so attempt 1 can be differenced against them.
//
// ⚠ THIS, not the real-call reference, is the measurement that worked in run
// 19. The reference is "the last camera-shaped record the REAL call bound",
// and the real call binds several - it picked a non-world one, so every
// verdict printed "other-record" and the column was useless. Attempt 0 vs
// attempt 1 has no such ambiguity: same pass, same record index, same
// standing position, one frame apart, differing ONLY by the offset we
// applied. Run 19's numbers came out of reading those two log blocks by
// hand; the code does it now.
thread_local std::array<ChainWalkCam, kCensusPassSlots> chainwalk_cams_attempt0{};
// Reference: the last camera-shaped record bound during the REAL call. Plain
// atomics rather than a mutex - three independent floats, written on the
// render thread and read on the same thread at the end of the doubled call.
std::atomic<float> chainwalk_real_cam_x{0.0f};
std::atomic<float> chainwalk_real_cam_y{0.0f};
std::atomic<float> chainwalk_real_cam_z{0.0f};
std::atomic_bool chainwalk_real_cam_seen{false};

struct ChainWalkSlot {
    uintptr_t rva{};
    // Chosen during observation: the LAST pass of this RVA whose mirrored
    // colour attachment is at the main view's full extent.
    bool observed{};
    uint32_t observed_pass{};
    VkExtent2D extent{};
    VkFormat format{VK_FORMAT_UNDEFINED};
    // Our own snapshot image. Created lazily at the first copy, never
    // destroyed while the process lives.
    VkImage image{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    bool create_failed{};
    // Which A/B attempt last wrote into this image, and from where. Dumps are
    // refused unless this matches the attempt being dumped - a stale snapshot
    // measured as if it were fresh is precisely how this project has been
    // fooled twice.
    uint32_t filled_attempt{UINT32_MAX};
    uint32_t filled_pass{};
    VkImage filled_source{VK_NULL_HANDLE};
};
// Written on the render thread (recording), read on the present thread
// (dumping). Leaf lock: never taken while holding any other.
std::mutex chainwalk_mutex;
std::array<ChainWalkSlot, kChainWalkSlots> chainwalk_slots{};
// The main view's extent, learned from the RenderOpaque mirror during
// observation. Everything narrower (bloom mips, small post targets) is
// excluded so all four snapshots are the same size and directly comparable.
VkExtent2D chainwalk_full_extent{};
// Which A/B attempt the doubled call currently being recorded belongs to, or
// UINT32_MAX for "not capturing". Set once per doubled call in
// hook_frame_root; read at every vkCmdEndRenderPass.
thread_local uint32_t chainwalk_capture_attempt = UINT32_MAX;
// True only for a doubled call that started with observation already armed,
// so the map it produced covers the WHOLE call. Without this, arming
// observation part-way through a call would finalise slots from a partial
// map - a wrong pass map is worse than none, because it still produces
// plausible-looking snapshots.
thread_local bool chainwalk_observing_this_call = false;
// The mirrored colour attachment of the render pass currently open, captured
// in hook_cmd_begin_render_pass while mirror_mutex is already held, so
// vkCmdEndRenderPass needs no lock at all to know what to copy and which
// layout the pass leaves it in.
thread_local VkImage chainwalk_current_color = VK_NULL_HANDLE;
thread_local VkFormat chainwalk_current_format = VK_FORMAT_UNDEFINED;
thread_local VkExtent2D chainwalk_current_extent{};
thread_local VkImageLayout chainwalk_current_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
// The command buffer the open render pass belongs to. Render passes cannot
// nest, but Vulkan does allow two command buffers to have passes open at the
// same time - begin(A), begin(B), end(B), end(A) is legal - and that would
// pair one pass's colour mirror with another pass's end. Checked rather than
// assumed away: a snapshot attributed to the wrong stage is worse than a
// missing one, because it still produces a picture.
thread_local VkCommandBuffer chainwalk_current_cmd = VK_NULL_HANDLE;
std::atomic_uint32_t chainwalk_cmd_mismatches{0};
// ⚠ THE JOB RVA AS IT WAS AT vkCmdBeginRenderPass, which is NOT what it is
// at vkCmdEndRenderPass. Run 19 lost two of its three snapshots to exactly
// this: jobs nest, so a pass opened by RenderOpaque (0x1A37CC0) can have the
// lighting job (0x19AE9E0) dispatch and bind inside it, and
// doubled_current_job_rva is saved/restored around each dispatch. Matching
// on the value at END made the slot's rva test fail for the SECOND
// qualifying pass of a pair - silently, because the "wrong extent/format"
// SKIP branch is only reached once the rva has already matched. The result
// was slot 0 capturing RenderOpaque's CLEAR pass (1 draw, black) instead of
// its G-buffer, and two all-zero PPMs that looked like a rendering result.
thread_local uintptr_t chainwalk_current_rva = 0;
std::atomic_uint32_t chainwalk_copy_lines{0};
std::atomic_uint64_t chainwalk_bytes_allocated{0};

// End of an OBSERVED doubled call: print the complete pass map and choose the
// snapshot target of each slot. Runs on the render thread with no lock held
// but chainwalk_mutex (leaf), after inside_frame_root_double is already
// false, so nothing it logs can re-enter the map.
void chainwalk_finish_observation(uint32_t passes_seen) {
    const uint32_t n = std::min<uint32_t>(passes_seen, kCensusPassSlots - 1);
    // The main view's extent comes from RenderOpaque's own mirror. Falling
    // back to "the widest mirrored colour pass" would be a silent guess, so
    // if RenderOpaque never appeared that is reported and capture stays off.
    VkExtent2D full{};
    for (uint32_t p = 1; p <= n; ++p) {
        const ChainWalkPass& cp = chainwalk_map[p];
        if (cp.rva == kChainWalkTargetRvas[0] && cp.mirrored && cp.color != VK_NULL_HANDLE &&
            cp.layout_known) {
            full = cp.extent;
        }
    }
    uint32_t mirrored_passes = 0, unmirrored_passes = 0;
    log::warn(std::format(
        "ChainWalkMap BEGIN passesInDoubledCall={} mainViewExtent={}x{} (from RenderOpaque's "
        "own mirror). Columns: pass jobRva mirrored layoutKnown colourMirrorImage extent format "
        "fb. mirrored=0 on a pass means the doubled call rendered into DOOM'S OWN framebuffer - "
        "that pass's output is unshifted by construction and contaminates eye 0 as well. "
        "layoutKnown=0 means we hold no recorded finalLayout for its colour attachment, so it "
        "cannot be snapshotted safely and is excluded from slot selection.",
        n, full.width, full.height));
    for (uint32_t p = 1; p <= n; ++p) {
        const ChainWalkPass& cp = chainwalk_map[p];
        if (cp.fb == VK_NULL_HANDLE) continue;
        if (cp.mirrored) ++mirrored_passes; else ++unmirrored_passes;
        log::warn(std::format(
            "ChainWalkMap pass={:3} jobRva={:#x} mirrored={} layoutKnown={} colour={} {}x{} "
            "format={} fb={}",
            p, cp.rva, cp.mirrored, cp.layout_known, reinterpret_cast<void*>(cp.color),
            cp.extent.width, cp.extent.height, static_cast<int>(cp.format),
            reinterpret_cast<void*>(cp.fb)));
    }
    std::scoped_lock lock(chainwalk_mutex);
    chainwalk_full_extent = full;
    uint32_t armed = 0;
    for (uint32_t s = 0; s < kChainWalkSlots; ++s) {
        ChainWalkSlot& slot = chainwalk_slots[s];
        slot.rva = kChainWalkTargetRvas[s];
        slot.observed = false;
        if (full.width == 0 || full.height == 0) continue;
        // LAST qualifying pass of this RVA wins: a chain's final pass is the
        // one whose output feeds the next stage.
        for (uint32_t p = 1; p <= n; ++p) {
            const ChainWalkPass& cp = chainwalk_map[p];
            if (cp.rva != slot.rva || !cp.mirrored || cp.color == VK_NULL_HANDLE) continue;
            if (!cp.layout_known) continue;
            if (cp.extent.width != full.width || cp.extent.height != full.height) continue;
            slot.observed = true;
            slot.observed_pass = p;
            slot.extent = cp.extent;
            slot.format = cp.format;
        }
        if (slot.observed) {
            ++armed;
            log::warn(std::format(
                "ChainWalkSlot {} ARMED jobRva={:#x} lastFullExtentPass={} {}x{} format={}",
                s, slot.rva, slot.observed_pass, slot.extent.width, slot.extent.height,
                static_cast<int>(slot.format)));
        } else {
            // Rule 3. A slot that never fires must say so, or its missing
            // dump reads as "the stage produced nothing".
            log::error(std::format(
                "ChainWalkSlot {} NOT ARMED jobRva={:#x} - no pass of this job rendered into a "
                "mirrored colour attachment at the main view extent {}x{}. Its pair of PPMs will "
                "be absent; read the ChainWalkMap above and re-target rather than treating the "
                "absence as a result.",
                s, kChainWalkTargetRvas[s], full.width, full.height));
        }
    }
    log::warn(std::format(
        "ChainWalkMap END mirroredPasses={} unmirroredPasses={} slotsArmed={}/{}. "
        "unmirroredPasses>0 is itself a candidate for where the shift dies.",
        mirrored_passes, unmirrored_passes, armed, kChainWalkSlots));
    chainwalk_ready.store(armed > 0, std::memory_order_release);
}

// Camera-record rider. Prints, per pass of the doubled call that bound a
// camera-shaped uniform, the translation column and its distance from the
// same frame's REAL call. At A/B attempt 0 the doubled camera IS the real
// camera, so every delta must read ~0 - that attempt is the instrument's own
// control, and a non-zero delta there means the reading is not to be trusted
// at attempt 1 either.
void chainwalk_emit_camera_classification(uint32_t attempt, uint32_t passes_seen,
                                          float effective_offset) {
    // Saved FIRST, ahead of the reference check below: the attempt-0 vs
    // attempt-1 comparison is the measurement that does not depend on the
    // reference at all, so a missing reference must not cost it.
    if (attempt == 0) chainwalk_cams_attempt0 = chainwalk_cams;
    if (!chainwalk_real_cam_seen.load(std::memory_order_relaxed)) {
        log::error(std::format(
            "ChainWalkCam attempt={} NO REFERENCE: the real call bound no camera-shaped record "
            "this frame, so no pass can be classified shifted/unshifted. Not a negative result - "
            "the comparison never ran.", attempt));
        return;
    }
    const float rx = chainwalk_real_cam_x.load(std::memory_order_relaxed);
    const float ry = chainwalk_real_cam_y.load(std::memory_order_relaxed);
    const float rz = chainwalk_real_cam_z.load(std::memory_order_relaxed);
    const uint32_t n = std::min<uint32_t>(passes_seen, kCensusPassSlots - 1);
    uint32_t shifted = 0, unshifted = 0, other = 0;
    // A camera-SHAPED record is not necessarily the WORLD camera: shadow and
    // reflection passes bind light-space matrices that pass the same shape
    // test and sit thousands of units away. Classifying purely on "delta >
    // epsilon" would report those as SHIFTED and manufacture exactly the
    // reassuring answer this step exists to avoid. So a shift must be the
    // size of the shift we applied, not merely non-zero.
    const float band = std::fabs(effective_offset) * 1.5f + 1.0f;
    for (uint32_t p = 1; p <= n; ++p) {
        const ChainWalkCam& c = chainwalk_cams[p];
        if (!c.seen) continue;
        // 0.5 uu: an order of magnitude below the 20 uu A/B offset and well
        // above float noise on coordinates of magnitude ~10^4.
        bool any_shifted = false, any_matching = false;
        std::string detail;
        for (uint32_t i = 0; i < c.distinct; ++i) {
            const float dx = c.x[i] - rx, dy = c.y[i] - ry, dz = c.z[i] - rz;
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            const char* tag = "other";
            if (d <= 0.5f) { tag = "same-as-real"; any_matching = true; }
            else if (d <= band) { tag = "SHIFTED"; any_shifted = true; }
            detail += std::format(" [({:.4f} {:.4f} {:.4f}) delta={:.4f} {}]",
                                  c.x[i], c.y[i], c.z[i], d, tag);
        }
        const char* verdict = any_shifted ? "SHIFTED"
                            : any_matching ? "unshifted"
                                           : "other-record";
        if (any_shifted) ++shifted;
        else if (any_matching) ++unshifted;
        else ++other;
        // THE CONTROLLED COMPARISON: this pass's records at attempt 1 against
        // its OWN records at attempt 0. Same pass, same record index, one
        // frame apart, standing still, differing only by the offset. A
        // per-record delta of ~|offset| here is the shift reaching that
        // pass's camera; ~0 is it not. Immune to the reference-selection
        // problem that made the verdict column above unreadable in run 19.
        std::string ab;
        if (attempt == 1) {
            const ChainWalkCam& c0 = chainwalk_cams_attempt0[p];
            if (!c0.seen) {
                ab = " abVsAttempt0=NO-ATTEMPT0-RECORD";
            } else {
                for (uint32_t i = 0; i < c.distinct && i < c0.distinct; ++i) {
                    const float ax = c.x[i] - c0.x[i];
                    const float ay = c.y[i] - c0.y[i];
                    const float az = c.z[i] - c0.z[i];
                    ab += std::format(" [r{} dAB=({:.4f} {:.4f} {:.4f}) |dAB|={:.4f}]",
                        i, ax, ay, az, std::sqrt(ax * ax + ay * ay + az * az));
                }
            }
        }
        log::warn(std::format(
            "ChainWalkCam attempt={} pass={:3} jobRva={:#x} records={} distinct={} "
            "real=({:.4f} {:.4f} {:.4f}) band={:.4f} verdict={}{}{}",
            attempt, p, c.rva, c.records, c.distinct, rx, ry, rz, band, verdict, detail, ab));
    }
    log::warn(std::format(
        "ChainWalkCam TOTAL attempt={} effectiveOffset={:.2f} passesWithCameraRecord={} "
        "shifted={} unshifted={} otherRecord={}. At attempt 0 (offset 0) shifted MUST be 0 - "
        "that is the control, and a non-zero count there means the instrument is wrong, not the "
        "renderer. At attempt 1, 'unshifted' passes bind a world camera the doubled call did NOT "
        "shift: hypothesis 5 if they are lighting/post, hypothesis 6 if opaque is among them. "
        "'otherRecord' is a camera-shaped matrix too far away to be this shift - a light or "
        "reflection view, not evidence either way.",
        attempt, effective_offset, shifted + unshifted + other, shifted, unshifted, other));
}

// Generic framebuffer mirroring master switch.
//
// SWITCH MATRIX. These three overlap and only some combinations are legal.
// Two of the three pairwise combinations have now no-op'd or faulted in a
// live run, each costing a headset test, so the rules are written down here
// rather than rediscovered one run at a time:
//
//   mirror | filter | legacy redirect | status
//   -------+--------+-----------------+--------------------------------------
//   off    | on     | on              | KNOWN GOOD - clean coherent images,
//          |        |                 | full camera coverage, but dim
//          |        |                 | (G-buffer only, no post)
//   off    | off    | off             | full-frame mode: ran 2057 attempts
//          |        |                 | stably, but the composite is
//          |        |                 | read-modify-write, so CLOSED
//   on     | off    | on              | FAULTED on attempt 0
//   on     | off    | off             | FAULTED on attempt 0
//   on     | ON     | off             | UNTESTED - and the only combination
//          |        |                 | that changes exactly ONE thing from
//          |        |                 | known-good
//
// Mirroring is therefore now tested with the filter still ON: only
// depth+opaque run, but into mirrored targets. If that is stable,
// unfiltering becomes a separate second step. Flipping both at once is what
// produced two dead runs.
//
// 2026-08-16 correction. The row "on | ON | off = UNTESTED" was right that it
// was untested, but wrong about why the earlier mirroring rows faulted.
// framebuffer_infos - the map ensure_mirror_framebuffer looks every
// framebuffer up in - HAD NO PRODUCER: it was declared and never written to by
// anything. Every lookup therefore missed, no mirror could ever be built, and
// the MISS branch then re-locked a non-recursive mutex it already held. So
// mirroring was not merely untested, it was incapable of running. Producer now
// wired into hook_create_framebuffer, deadlock removed. THIS is the switch's
// first genuine test.
std::atomic_bool mirror_enabled{true};
// Independent of mirroring: drop the depth+opaque job filter.
//
// DEFAULT FLIPPED TO TRUE 2026-08-17. With read-side redirection working
// (clones + substituted barriers + tracked layouts), unfiltered is the
// configuration the whole project is aiming at: the doubled call renders a
// COMPLETE frame - lighting, post, its own HUD - into mirrors, reading
// mirrors. Filtered can only ever produce an unlit G-buffer for eye 1, and
// a partial job set produces a composite built from inputs that were never
// rendered, which is exactly how eye 1 came back black-with-a-HUD.
// Plain ScrollLock still toggles it, so filtered remains one keypress away
// for comparison.
std::atomic_bool mirror_unfiltered{true};
std::atomic_int frame_root_stage{0};
// Mirroring outcome counters. Declared here rather than beside
// ensure_mirror_framebuffer (~4500 lines below) because FrameRootDouble's
// exception log needs them: a fault report that cannot say whether any mirror
// was created is the ambiguity that cost the last four runs.
// DoubledJobBisect - see hook_job_dispatch. Makes the doubled call dispatch
// NO jobs, so it runs the frame root's scaffolding and renders nothing.
// Bisection complete: with no jobs at all, and zero offset, eye 0 STILL
// changes - walls darker, lighting shifted, near shadows lost, one area
// brighter. Measured through a correct colour pipeline, so unlike every
// earlier visual result this one is trustworthy. Losing a shadow makes an
// area brighter and losing a light makes walls darker, which together point
// at the frame root allocating from fixed per-frame pools (shadow atlas
// slots, light lists) that a second call consumes or reassigns.
//
// That is a real defect and it is NOT fixable by target isolation. But it is
// a perturbation of lighting, not the wholesale texture destruction it looked
// like through the gamma bug - so the question now is whether the stereo is
// usable in spite of it. Rendering back on to find out.
std::atomic_bool doubled_dispatch_no_jobs{false};
// Rung of the doubled-call floor ladder: 0 normal, 1 no SetupView_world,
// 2 no SetupView_gui, 3 neither SetupView, 4 the retained floor (no jobs, no
// gather, no SetupView).
//
// ⚠ FIXED AT 1 as of 2026-09-03. The SHIFT+INSERT cycle that walked the
// rungs was DELETED - it could reach rung 4, which HANGS THE GAME, and the
// ladder had already returned its answer. Rung 1 is not a probe: it is the
// S2 fix from the 2026-08-28 result (rung 0 flicker YES, rung 1 NO), so this
// value is SHIPPED BEHAVIOUR and must stay 1. Nothing writes it any more.
// See doubled_skip_setup_world_now; rungs 2-4 are now unreachable.
std::atomic_int doubled_floor_level{1};
std::atomic_uint32_t doubled_jobs_skipped{0};
// SharedDepthIsolation - see hook_job_dispatch.
// Now DEFAULTS TRUE: with depth mirrored rather than shared, the doubled
// call's RenderDepth writes OUR private buffer, which eye 1 needs as its
// prepass. Suppressing it was only ever the isolation test. ScrollLock still
// toggles it, so the fix can be A/B'd against the damaged behaviour live.
std::atomic_bool doubled_allow_render_depth{true};
std::atomic_uint32_t doubled_depth_skipped{0};
std::atomic_uint32_t mirror_created_count{0};
// Colour mirrors cleared to black at creation - see the white-flash block in
// transition_mirror_images_to_color_attachment. Should track mirror_created_count.
std::atomic_uint64_t g_mirror_cleared{0};
// Light-list divergence between the two frame-root passes. See hook_cull_lights.
std::atomic_uint64_t g_cl_pairs_moving{0};
std::atomic_uint64_t g_cl_pairs_still{0};
std::atomic_uint64_t g_cl_diverge_moving{0};
std::atomic_uint64_t g_cl_diverge_still{0};
std::atomic_int32_t g_cl_worst_delta{0};
uint64_t g_cl_frame[2]{};
int32_t g_cl_count[2]{};
float g_cl_last_yaw{0.0f};
std::atomic_uint32_t mirror_miss_count{0};
std::atomic_uint32_t mirror_view_miss_count{0};
std::atomic_uint64_t mirror_bytes_allocated{0};
std::atomic_uint32_t mirror_shared_depth_count{0};
// ISOLATION: run the doubled call with ZERO camera offset. Eye 1 is then a
// duplicate viewpoint, so any corruption left in EYE 0 is caused by the act of
// doubling itself (shared depth writes, shared renderer state) rather than by
// camera divergence. Distinguishes two causes that have been conflated.
std::atomic_bool frame_root_zero_offset{false};
thread_local uint32_t doubled_render_pass_count = 0;
std::unordered_set<VkFramebuffer> doubled_fb_census;
std::atomic_bool doubled_fb_census_reported{false};
std::atomic_uint32_t composite_probe_lines{0};
constexpr uint32_t kMaxCompositeProbeLines = 40;
// DepthDumpControlTest - see CreateImageViewFn's declaration above. Shares
// framebuffer_depth_view_mutex (small, session-lifetime maps, no
// contention concern) rather than adding a second lock.
std::unordered_map<VkImageView, VkImage> image_view_to_image;
// Per-view format/aspect, needed to MIRROR a framebuffer rather than merely
// identify it. Populated in hook_create_image_view.
// 2026-08-17, STEP 6.7a: the full create parameters, not just the four
// fields mirroring needed. To substitute an ALIASED view - a sampling view
// over an image we mirror, which is not the attachment view we key on - we
// have to build a mirror-side view that matches the sampler's expectations
// exactly: same type, same swizzle, same mip/layer range. Keeping only
// format/aspect/layers made that impossible, which is the whole reason the
// 67 full-screen aliased binds of run 19 could only be counted, not fixed.
struct ImageViewInfo {
    VkImage image;
    VkFormat format;
    VkImageAspectFlags aspect;
    uint32_t layers;
    VkImageViewType view_type;
    VkComponentMapping components;
    VkImageSubresourceRange range;
    VkImageViewCreateFlags flags;
};
std::unordered_map<VkImageView, ImageViewInfo> image_view_infos;
// 2026-08-18, review P1. Extent/format/usage per VkImage - the identity the
// ComputeProbe's "view=0x… hit=unknown" lines could never print. Shares
// framebuffer_depth_view_mutex with the two maps above (creation-time only,
// no hot path).
struct CreatedImageInfo {
    VkExtent3D extent;
    VkFormat format;
    VkImageUsageFlags usage;
    uint32_t mips;
    uint32_t layers;
    VkImageType type;  // 2026-08-18: 3D marks a froxel/volumetric candidate.
};
std::unordered_map<VkImage, CreatedImageInfo> created_image_infos;
// Full attachment list per framebuffer, plus its render pass and extent -
// everything needed to build a compatible mirror.
struct FramebufferInfo { std::vector<VkImageView> attachments; VkRenderPass render_pass; uint32_t width; uint32_t height; uint32_t layers; };
std::unordered_map<VkFramebuffer, FramebufferInfo> framebuffer_infos;

// StereoPreview (F3): the real payoff of tonight's work - present the
// offscreen redirect's captured content LIVE, continuously, to both OpenXR
// eyes, instead of only dumping a one-shot PPM. Same image both eyes for
// now (no parallax - that's the actual native-stereo step, deliberately
// NOT attempted here; it requires mutating renderView camera state, which
// is a standing "Never re-enable" item in docs/FORBIDDEN-BOUNDARIES.md
// after a real prior failure, and that decision needs to be made awake,
// together, not inferred alone overnight). This only proves the
// presentation half of the pipeline: can our own redirected render
// actually reach the headset display.
//
// Mutually exclusive with F6/F7 (enforced at arm-time in both directions,
// see the hotkey handlers below) - never runs a second/third extra
// RenderOpaque call in the same frame alongside F7's own, only ever one
// extra call total, same as everything validated tonight.
//
// Runs far longer than F6/F7's tight smoke-test bounds (~15000 attempts,
// several minutes at the observed ~79 attempts/sec) BECAUSE it's now
// something the user will actually look at continuously, not a blink-and-
// miss-it burst - still finite and still auto-disables on any caught
// exception, same safety shape as everything else, just a longer leash
// given the mechanism itself is now validated stable across 4 independent
// live tests tonight with zero exceptions/crashes/hangs.
std::atomic_bool stereo_preview_enabled{false};
std::atomic_uint32_t stereo_preview_attempt_count{0};
constexpr uint32_t kMaxStereoPreviewAttempts = 15000;

// VK_FORMAT_B10G11R11_UFLOAT_PACK32 (our offscreen color format) blit
// support isn't guaranteed by the Vulkan spec - checked once, lazily, the
// first time F3 is armed, via vkGetPhysicalDeviceFormatProperties. Fails
// safe: if blit-src isn't supported, StereoPreview refuses to arm at all
// (clear log, no visual output, no crash) rather than attempting an
// unsupported GPU operation blind.
std::atomic_bool offscreen_blit_capability_checked{false};
std::atomic_bool offscreen_blit_supported{false};
std::atomic_bool offscreen_blit_linear_supported{false};

void check_offscreen_blit_capability() {
    if (offscreen_blit_capability_checked.exchange(true, std::memory_order_relaxed)) return;
    const auto& vk = vulkan_state();
    if (!vk.physical_device) {
        log::error("StereoPreview blit capability check: no physical device yet");
        return;
    }
    HMODULE mod = GetModuleHandleW(L"vulkan-1.dll");
    auto get_format_props = mod ? reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(
        kharvox::native::instanceProc("vkGetPhysicalDeviceFormatProperties")) : nullptr;
    if (!get_format_props) {
        log::error("StereoPreview blit capability check: could not load vkGetPhysicalDeviceFormatProperties");
        return;
    }
    VkFormatProperties props{};
    get_format_props(vk.physical_device, kOffscreenColorFormats[0], &props);
    const bool blit_src = (props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0;
    const bool linear = (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    offscreen_blit_supported.store(blit_src, std::memory_order_relaxed);
    offscreen_blit_linear_supported.store(linear, std::memory_order_relaxed);
    log::info(std::format(
        "StereoPreview blit capability check: format={} blitSrcSupported={} linearFilterSupported={}",
        static_cast<int>(kOffscreenColorFormats[0]), blit_src, linear));
}
#endif  // DOOMVR_RESEARCH_TOOLS
std::atomic_bool installed{false};

struct SwapchainState {
    VkFormat format{VK_FORMAT_UNDEFINED};
    VkExtent2D extent{};
    std::vector<VkImage> images;
};
// ===== SHARED, NOT EXCLUSIVE (2026-08-30) ==================================
//
// THIS WAS THE FRAMERATE. Measured, from the 10:21 headset run:
//   fps=33.1 frameMs=30.24 | GPUWAIT=0.07 | RESIDUAL=2.45 | ALLHOOKS=12.66
// The GPU is idle and DOOM own CPU work is 2.45 ms of a 30 ms frame - roughly
// 25 ms of it is this mod. Per call, CmdDrawIndexed costs 0.04 us and
// CmdBindVertexBuffers 0.025 us, which is what a forwarding hook should cost;
// CmdBindDescriptorSets costs 13-16 us, 300-800x more, at ~1667 calls a frame.
// The body is 1-2 us of work. The rest was threads waiting for each other:
// this ONE global mutex was held across the entire per-set, per-binding walk,
// and DOOM records its command buffers on parallel job threads. We had turned
// a multithreaded renderer into a serial one.
//
// Now a shared_mutex. EVERY existing acquisition became unique_lock - that
// half is a no-op refactor with identical behaviour - and only the descriptor
// walk takes it SHARED, because that walk only READS the maps. Its writes go
// to atomics, to a caller-local vector, to mapped GPU memory at per-record
// addresses, and to the freqHigh census state, which now has its own lock.
// Readers still exclude every writer (swapchain create/destroy, memory map,
// descriptor update); what they no longer do is exclude each other.
std::shared_mutex swapchains_mutex;
std::unordered_map<VkSwapchainKHR, SwapchainState> swapchains;

#ifdef DOOMVR_RESEARCH_TOOLS
std::atomic_uint32_t push_constant_samples{0};
std::atomic_uint32_t uniform_samples{0};
std::atomic_bool camera_capture_active{false};
std::atomic_uint32_t camera_sample_count{0};
std::atomic_uint64_t presented_frame{0};
std::atomic_bool view_shift_active{false};
std::atomic_uint32_t view_shift_samples{0};
std::atomic_uint32_t view_shift_rejections{0};
std::atomic<float> head_base_x{0.0f};
std::atomic<float> head_base_y{0.0f};
std::atomic<float> head_base_z{0.0f};
std::atomic<float> head_base_w{1.0f};
struct BufferInfo { VkDeviceSize size{}; VkBufferUsageFlags usage{}; };
std::unordered_map<VkBuffer, BufferInfo> buffers;
struct BufferBinding { VkDeviceMemory memory{}; VkDeviceSize offset{}; };
struct MemoryMapping { unsigned char* host{}; VkDeviceSize offset{}; VkDeviceSize size{}; };
struct DescriptorBinding {
    VkBuffer buffer{};
    VkDeviceSize offset{};
    VkDeviceSize range{};
    VkDescriptorType type{};
};
// WorldPipelineBindingProbe (docs/STEREO-RESTART-PLAN.md Task 3, new
// candidate 2026-08-11): hook_update_descriptor_sets previously only tracked
// buffer descriptors (needed for the camera-matrix scanning below) - image/
// sampler writes were silently ignored entirely. Per-object texture bindings
// are exactly what the vanilla RenderDoc capture's samp_magnificationclouds
// finding (Task 1's update) flagged as worth checking: sky rendering looked
// structurally simpler/more static than world geometry's bindings, which is
// the shape of thing that would explain "sky survives, world doesn't" if
// world geometry's per-object image bindings are what's going stale on the
// extra call specifically.
struct ImageDescriptorBinding {
    VkImageView image_view{};
    VkSampler sampler{};
    VkDescriptorType type{};
    // DescriptorCensus (2026-08-17). image_view above is pImageInfo[0] only.
    // That was fine for the world-pipeline probe but it hides the ONE thing
    // the census exists to size: whether a pass samples a large indexed
    // ARRAY of images (review risk 1 - the bindless-shaped fallback), which
    // would need per-resource classification rather than a whole-set clone.
    // count is the write's descriptorCount; views holds as many leading
    // elements as fit, so a census line can report both "how many" and a
    // representative sample of what.
    uint32_t count{};
    std::array<VkImageView, 4> views{};
    // ReadRedirect (STEP 6) needs the layout DOOM declared: a clone that
    // guesses SHADER_READ_ONLY_OPTIMAL would be wrong for a STORAGE_IMAGE
    // (GENERAL) and is undefined behaviour, not a cosmetic slip.
    VkImageLayout layout{};
};
std::unordered_map<VkBuffer, BufferBinding> buffer_bindings;
std::unordered_map<VkDeviceMemory, MemoryMapping> memory_mappings;
std::unordered_map<VkDescriptorSet, std::unordered_map<uint32_t, DescriptorBinding>> descriptor_sets;
std::unordered_map<VkDescriptorSet, std::unordered_map<uint32_t, ImageDescriptorBinding>> descriptor_image_sets;

// ===================== DESCRIPTOR EVENT RING (2026-08-24) ==================
// PLAN-2026-08-24-CUTSCENE-DESCRIPTOR-RING §2. LOG-ONLY. Nothing here changes
// a descriptor, a barrier, a bind or a frame; every write below is to our own
// bookkeeping and every read is emitted as text.
//
// WHAT IT MEASURES: the ORDER of our own bookkeeping events per descriptor set
// handle - allocation, our erase of its content records, and each record we
// write for it. It does NOT see DOOM's writes that miss both record branches;
// that is exactly what the write-shape tap below is for, and the two are read
// together.
//
// LOCK: descriptor_set_rings is guarded by swapchains_mutex, the same lock
// that guards descriptor_sets / descriptor_image_sets, and desc_ring_push must
// only ever be called with that lock already held. No new lock and therefore
// no new lock order - Rule 9, the run-17 lesson.
enum : uint8_t {
    kDescRingAlloc = 0,     // vkAllocateDescriptorSets returned this handle
    kDescRingErase = 1,     // our realloc erase ran; arg bit0=image record dropped, bit1=buffer
    kDescRingImgWrite = 2,  // an image content record was written; arg = dstBinding
    kDescRingBufWrite = 3,  // a buffer content record was written; arg = dstBinding
    kDescRingCopyDst = 4,   // this handle was a vkCopyDescriptorSet destination; arg = count
};
struct DescRingEvent {
    uint32_t frame{0};
    uint16_t arg{0};
    uint8_t kind{0};
    uint8_t pad{0};
};
constexpr size_t kDescRingEvents = 8;
struct DescRing {
    std::array<DescRingEvent, kDescRingEvents> e{};
    uint32_t total{0};   // events EVER pushed for this handle - so "8 shown, total=17"
    uint8_t head{0};     // next slot to write
};
// Bounded by live-handle count, exactly as descriptor_image_sets is, plus an
// explicit hard cap so a runaway handle count cannot grow this without bound.
// The cap's own state is published on the DescRingCaps line: a skipped count
// above zero means some handle has NO ring and its absence is truncation, not
// a measurement.
constexpr size_t kDescRingMaxHandles = 8192;
std::unordered_map<VkDescriptorSet, DescRing> descriptor_set_rings;
std::atomic_uint32_t desc_ring_skipped{0};

// Call ONLY with swapchains_mutex held.
void desc_ring_push(VkDescriptorSet set, uint8_t kind, uint16_t arg) {
    if (set == VK_NULL_HANDLE) return;
    auto it = descriptor_set_rings.find(set);
    if (it == descriptor_set_rings.end()) {
        if (descriptor_set_rings.size() >= kDescRingMaxHandles) {
            desc_ring_skipped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        it = descriptor_set_rings.emplace(set, DescRing{}).first;
    }
    DescRing& r = it->second;
    r.e[r.head] = DescRingEvent{
        static_cast<uint32_t>(presented_frame.load(std::memory_order_relaxed)), arg, kind, 0};
    r.head = static_cast<uint8_t>((r.head + 1) % kDescRingEvents);
    if (r.total != UINT32_MAX) ++r.total;
}

// Formats a ring snapshot oldest -> newest. Called AFTER the locks are
// released, from a copy taken under them.
std::string desc_ring_format(const DescRing& r) {
    const uint32_t shown = r.total < kDescRingEvents ? r.total
                                                     : static_cast<uint32_t>(kDescRingEvents);
    if (shown == 0) return "[] total=0";
    std::string out = "[";
    for (uint32_t i = 0; i < shown; ++i) {
        const size_t idx = (r.head + kDescRingEvents - shown + i) % kDescRingEvents;
        const DescRingEvent& ev = r.e[idx];
        if (i) out += ' ';
        switch (ev.kind) {
            case kDescRingAlloc:
                out += std::format("ALLOC@{}", ev.frame);
                break;
            case kDescRingErase:
                out += std::format("ERASE(img={},buf={})@{}", (ev.arg & 1) ? 1 : 0,
                                   (ev.arg & 2) ? 1 : 0, ev.frame);
                break;
            case kDescRingImgWrite:
                out += std::format("IMG_WRITE(b{})@{}", ev.arg, ev.frame);
                break;
            case kDescRingBufWrite:
                out += std::format("BUF_WRITE(b{})@{}", ev.arg, ev.frame);
                break;
            case kDescRingCopyDst:
                out += std::format("COPY_DST(n{})@{}", ev.arg, ev.frame);
                break;
            default:
                out += std::format("?{}@{}", ev.kind, ev.frame);
                break;
        }
    }
    out += std::format("] total={} shown={}", r.total, shown);
    return out;
}

// ---- Global per-window rate counters (plan §2 iii) -------------------------
// Totals; the ReadRedirect audit line publishes the DELTA since its own last
// emission, because a total cannot show a path going quiet.
std::atomic_uint64_t desc_update_calls{0};        // hook_update_descriptor_sets invocations
std::atomic_uint64_t desc_update_img_records{0};  // image content records written
std::atomic_uint64_t desc_update_buf_records{0};  // buffer content records written
std::atomic_uint64_t desc_copy_calls{0};          // invocations carrying copyCount > 0
std::atomic_uint64_t desc_alloc_calls{0};         // successful vkAllocateDescriptorSets calls
std::atomic_uint64_t desc_alloc_sets{0};          // handles those calls returned
// The split the handoff asked for: staleRecordsCleared mixed the two and so
// could not say WHICH record class the realloc erase was dropping.
std::atomic_uint64_t stale_img_records_cleared{0};
std::atomic_uint64_t stale_buf_records_cleared{0};

// ---- Refusal-line budget, re-armed at the cutscene edge (plan §2 ii) -------
// The existing SetRefusedID budget is kMaxAttributionLines shared across every
// attribution site, and the 21:09 run exhausted it at #185 BEFORE the
// collapse, so the window that mattered printed nothing. This is a SEPARATE
// budget, re-armed by cutscene_dense_burst - which the cadence detector's
// edges and the UP marker both call - so the collapse window gets fresh lines
// whether or not the flat run produces a cadence edge at all.
constexpr uint32_t kRefusalRingRearmLines = 10;
std::atomic_uint32_t refusal_ring_budget{0};
std::atomic_uint32_t refusal_ring_rearms{0};
std::atomic_uint32_t refusal_ring_lines{0};

// ---- Write-shape tap (plan §2 iv) -----------------------------------------
// The last 16 DISTINCT handles a whole-set refusal has named. When
// vkUpdateDescriptorSets writes to one of them we log the write's SHAPE, so
// "DOOM's writes for these sets miss both record branches" can name the field
// that does it rather than being inferred. Lock-free by construction: a fixed
// array of atomic handles, published at the refusal, scanned at the write. No
// lock is taken, so no lock order exists to get wrong.
constexpr size_t kRecentRefusedSets = 16;
constexpr uint32_t kShapeTapLines = 12;
std::atomic<VkDescriptorSet> recent_refused_sets[kRecentRefusedSets];  // zero-init = VK_NULL_HANDLE
std::atomic_uint32_t recent_refused_head{0};
std::atomic_uint32_t shape_tap_budget{kShapeTapLines};  // armed from the start: healthy reference
std::atomic_uint32_t shape_tap_rearms{0};
std::atomic_uint32_t shape_tap_lines{0};
std::atomic_uint64_t shape_tap_hits{0};  // writes seen targeting a refused handle, UNCAPPED

void note_refused_set(VkDescriptorSet set) {
    if (set == VK_NULL_HANDLE) return;
    // Dedupe: without this one handle refused every frame fills all 16 slots
    // and the tap can only ever see that one set.
    for (size_t i = 0; i < kRecentRefusedSets; ++i)
        if (recent_refused_sets[i].load(std::memory_order_relaxed) == set) return;
    const uint32_t slot = recent_refused_head.fetch_add(1, std::memory_order_relaxed);
    recent_refused_sets[slot % kRecentRefusedSets].store(set, std::memory_order_relaxed);
}

bool is_recent_refused_set(VkDescriptorSet set) {
    if (set == VK_NULL_HANDLE) return false;
    for (size_t i = 0; i < kRecentRefusedSets; ++i)
        if (recent_refused_sets[i].load(std::memory_order_relaxed) == set) return true;
    return false;
}
// ===================== END DESCRIPTOR EVENT RING ============================

// DESCRIPTOR UPDATE TEMPLATES (2026-08-20) - THE ROOT CAUSE OF THE MISSING
// EFFECTS, named by the first DescCensusPass table ever captured.
//
// Pass 22, the additive composite - the exact pass where the additive layer
// dies (mirror 47 against DOOM's 11 490) - reads ZERO of our mirrors:
//   pass=22 binds=19 mirror=0 shared=19 unknown=76
// while every other pass in the chain reads several (17:4, 18:3, 19:4, 20:9,
// 23:4, 25:80). Across the whole doubled call: mirror=157, unknown=2623.
//
// The reason is here. DOOM writes those sets through
// vkUpdateDescriptorSetWithTemplate, and the old hook could not decode the
// payload - "contents cannot be decoded without the template's own layout" -
// so it marked the set untracked and ERASED it from redirected_sets. Any set
// written by template was therefore excluded from redirection by construction:
// we never learn which views it samples, never mirror them, and explicitly
// refuse to substitute. vkCreateDescriptorUpdateTemplate was not hooked at
// all, so the layout needed to decode was never captured.
//
// Capturing the entries makes the payload decodable: each entry gives
// dstBinding, descriptorCount, descriptorType, and the offset/stride at which
// its VkDescriptorImageInfo array sits inside the opaque blob.
std::mutex descriptor_template_mutex;
std::unordered_map<VkDescriptorUpdateTemplate, std::vector<VkDescriptorUpdateTemplateEntry>>
    descriptor_update_templates;
std::atomic_uint32_t template_decode_sets{0};
std::atomic_uint32_t template_decode_images{0};
std::atomic_uint32_t template_decode_misses{0};
std::unordered_set<const void*> modified_view_records;
std::unordered_map<VkCommandBuffer, VkPipeline> bound_graphics_pipelines;
std::unordered_set<uint64_t> sampled_camera_pipeline_bindings;
// Phase 0 (STEREO-IMPLEMENTATION-PLAN.md v2, 2026-08-11): per-pipeline
// depth/stencil state, captured at vkCreateGraphicsPipelines time, keyed by
// the returned handle. Populated for every pipeline DOOM creates (a
// startup burst, hundreds of pipelines) but only ever LOGGED the first time
// a given pipeline is actually bound while inside_render_opaque - see
// hook_cmd_bind_pipeline. This answers whether RenderOpaque's world/weapon
// pipelines use an EQUAL depth compare against a shared prepass (the
// leading hypothesis for Attempt 5's near-geometry collapse) without
// guessing from the decompile.
struct PipelineDepthStencilInfo {
    bool has_depth_stencil_state{false};
    VkBool32 depth_test_enable{VK_FALSE};
    VkBool32 depth_write_enable{VK_FALSE};
    VkCompareOp depth_compare_op{VK_COMPARE_OP_NEVER};
    VkBool32 stencil_test_enable{VK_FALSE};
    VkCompareOp front_compare_op{VK_COMPARE_OP_NEVER};
    uint32_t front_reference{0};
    VkCompareOp back_compare_op{VK_COMPARE_OP_NEVER};
    uint32_t back_reference{0};
    // ColorBlendProbe (docs/STEREO-RESTART-PLAN.md Task 3, new candidate
    // 2026-08-11): never previously captured. Matters because of Task 3
    // item 1's finding - the per-eye redirect target's 3 color images never
    // receive DOOM's own CLEAR (only a one-time layout transition, no clear
    // value), only the second (LOAD) pass draws into them. If any of these
    // pipelines blend against existing buffer content, that combines
    // directly with the never-cleared buffer to explain "opaque content
    // (e.g. sky) looks fine, blended content reads uninitialized memory and
    // comes out wrong" - the exact asymmetry in the observed symptom.
    // RenderOpaque's G-buffer has 3 color attachments (see kOffscreenColorFormats);
    // captured per-attachment, up to 3.
    bool has_color_blend_state{false};
    std::array<VkBool32, 3> blend_enable{VK_FALSE, VK_FALSE, VK_FALSE};
    VkBlendFactor src_color_blend_factor{VK_BLEND_FACTOR_ZERO};
    VkBlendFactor dst_color_blend_factor{VK_BLEND_FACTOR_ZERO};
    VkBlendOp color_blend_op{VK_BLEND_OP_ADD};
    VkBlendFactor src_alpha_blend_factor{VK_BLEND_FACTOR_ZERO};
    VkBlendFactor dst_alpha_blend_factor{VK_BLEND_FACTOR_ZERO};
    VkBlendOp alpha_blend_op{VK_BLEND_OP_ADD};
};
std::unordered_map<VkPipeline, PipelineDepthStencilInfo> pipeline_depth_stencil_info;
// Dedup key is (pipeline, callKind), NOT pipeline alone (docs/STEREO-RESTART-PLAN.md
// Task 3 / SS0 item 6 - a diagnostic's "no difference found" is only meaningful if it
// can actually distinguish the two cases being compared). Deduping by pipeline alone
// meant a pipeline already logged on the real call's first bind would silently never
// re-log on the extra call, making a real-vs-extra comparison structurally incapable
// of ever showing a difference. VkPipeline handles are non-dispatchable and observed
// 8-byte-aligned in this driver, so the low bit is free to carry callKind.
std::unordered_set<uint64_t> logged_render_opaque_pipelines;
std::atomic_uint32_t pipeline_create_sample_count{0};
constexpr uint32_t kMaxPipelineCreateSamples = 4000;
// PIPELINE PARITY - see the probe in hook_cmd_bind_pipeline. Per-pass sets of
// distinct pipeline handles bound by each call during ONE captured frame.
// Plain containers, render-thread only, on the same premise and within the
// same window as the uniform diff's.
std::atomic_bool pipeline_parity_armed{false};
std::atomic_bool pipeline_parity_capturing{false};
std::atomic_uint32_t pipeline_parity_at{0};
std::map<uint32_t, std::set<VkPipeline>> pipeline_parity_real;
std::map<uint32_t, std::set<VkPipeline>> pipeline_parity_doubled;
std::unordered_set<uint64_t> pipeline_parity_seen_real;
std::unordered_set<uint64_t> pipeline_parity_seen_doubled;
constexpr size_t kMaxPipelineParityRecords = 3000;
std::atomic_uint32_t cpu_camera_scan_state{0}; // 0 idle, 1 armed, 2 running
std::atomic_bool camera_bind_stack_captured{false};
std::atomic_uintptr_t captured_render_view{0};
thread_local uintptr_t active_render_view = 0;
std::atomic_uintptr_t world_view_address{0};
std::atomic_size_t world_view_matrix_offset{SIZE_MAX};
std::atomic_bool upstream_yaw_test_active{false};

bool safe_read_pointer(const void* source, uintptr_t& value);
bool safe_read_bytes(uintptr_t address, void* dest, size_t size);
bool apply_temporary_world_yaw(uintptr_t view, std::array<float, 16>& original,
    float*& matrix);

// ------------------------------------------------ V1: THE VIEW INDEX ------
// docs/RESULT-2026-08-26-STATIC-PERVIEW-CONTEXT.md. The 2026-08-26 static pass
// closed the shader-variant AND the shared-context lines and left exactly one
// live question. Both arms bind out of the PER-VIEW context (PATH B: the async
// predicate's [0x6FD3218] is a queue-family index and BOTH arms create the
// device with one queue - the working-arm capture's pQueueCreateInfos has one
// element, and doomvr.log's own LayerFootprint line reads
// queueCreateInfoCount=1 family=0). So binding 2's content on the doubled call
// is whatever the per-view context holds, and what selects that content is:
//
//     viewIndex = jobArg[0x20]     (0x1A33623: movsx eax, word [rdi+0x2A])
//     perViewStruct = [0x705CC28] + viewIndex*0x170
//     froxelIndex   = ((int32*)[0x705CC30])[viewIndex]   (and 0x705CC38)
//
// all read by the setup 0x19AE740 and re-read by the job's own draw loop at
// 0x19AF0EE/0x19AF108. If the two invocations carry the SAME viewIndex the
// doubled call reuses view 0's context and the deficit is elsewhere; if they
// DIFFER, the second index selects a per-view struct and a froxel index that
// nothing populated, which is exactly a default in the slot P1W measured.
//
// COST. One relaxed atomic load per job dispatch when disarmed. Armed, it
// captures at most kV1PerSide records PER SIDE - separate budgets, because a
// shared cap lets the frequent side starve the rare one - and every read goes
// through safe_read_bytes.
//
// THE CONFUSABLE FAILURE MODE, and the controls aimed at it. jobArg is a
// POINTER INTO 0x1A33410's OWN STACK FRAME (built at 0x1A33605..0x1A33630).
// A stale or garbage read there yields small plausible integers that look
// exactly like a real view index. So:
//   * every field is a guarded read and its success is recorded in `ok`, never
//     collapsed into a zero value;
//   * `ctxLive` is a guarded read at viewCtx+0xB974 - a field the resolver's
//     sibling 0x18F5F90 reads, so a genuine context is at least that large;
//     it fails on stack garbage and on a null deref;
//   * `earlyOut` records [0x705C400], which when non-zero makes the ENTIRE job
//     return at 0x19AEA00 without binding anything. Those dispatches are
//     no-ops and must not be counted as invocations;
//   * the arm-time self-test feeds a SYNTHETIC jobArg through this same
//     decoder and refuses to arm unless every field round-trips - and also
//     requires the decoder to REJECT a null jobArg and a null *jobArg, because
//     a decoder that cannot say "invalid" reports blindness as data.
constexpr uint32_t kV1PerSide = 12;
constexpr uintptr_t kV1EarlyOutRva   = 0x705C400;  // job-wide early-out latch
constexpr uintptr_t kV1PredG9A0Rva   = 0x705C9A0;  // read ONLY by this predicate
constexpr uintptr_t kV1PredG9F40Rva  = 0x6729F40;
constexpr uintptr_t kV1QueueFamRva   = 0x6FD3218;  // queue family index, -1 = none
constexpr uintptr_t kV1PerViewArrRva = 0x705CC28;  // renderer+0x218, stride 0x170
constexpr uintptr_t kV1FroxArrARva   = 0x705CC30;  // renderer+0x220
constexpr uintptr_t kV1FroxArrBRva   = 0x705CC38;  // renderer+0x228
constexpr uintptr_t kV1FrameDeltaRva = 0x705CA28;  // latches jobArg[0x10]
constexpr size_t    kV1PerViewStride = 0x170;

// ok bits - which guarded reads succeeded. Never fold a failure into a 0.
enum : uint32_t {
    kV1OkArg      = 1u << 0,  // jobArg[0x00..0x20] read
    kV1OkMid      = 1u << 1,  // *jobArg read
    kV1OkCtx      = 1u << 2,  // **jobArg read (the per-view context pointer)
    kV1OkCtxLive  = 1u << 3,  // viewCtx+0xB974 readable => a real context
    kV1OkGlobals  = 1u << 4,  // the predicate/latch dwords read
    kV1OkArrays   = 1u << 5,  // the three renderer array POINTERS read
    kV1OkFroxA    = 1u << 6,  // froxA[viewIndex] read
    kV1OkFroxB    = 1u << 7,  // froxB[viewIndex] read
    kV1OkPerView  = 1u << 8,  // the 0x170 per-view struct read and hashed
};

struct V1Rec {
    uint32_t ok;
    int32_t  view_index;
    uintptr_t job_arg, mid, view_ctx;
    uint64_t arg08, arg10, arg18;
    int32_t  early_out, pred_g9a0, pred_g9f40, queue_family, frame_delta;
    uintptr_t per_view_arr, frox_a, frox_b;
    int32_t  frox_a_val, frox_b_val;
    uint64_t per_view_hash;
    // E1 (2026-08-26 run 8). C3 proved binding 2 is `gpuParticleVectorField1`
    // (slot 1056) and that on the doubled side it holds the SAME object as
    // `particles` (slot 1057) - the duplicate P1W has reported all along. The
    // binding loop at 0x19AF143 explains how:
    //
    //   idx = perSystemIndexArray[i]
    //   if (idx >= [0x705CA24] || idx < 0)  r8 = [[0x3848D98]+0x10]  // DEFAULT
    //   else                                r8 = [[0x705CA18] + idx*16]
    //   bind(ctx, decl, r8)                 // decl = vectorField1, then particles
    //
    // and [0x705CA24] - the table COUNT the bounds check uses - is written in
    // exactly one place at runtime: `mov [0x705CA24], r13d` at 0x19AF2C4, with
    // r13d zeroed at 0x19AEC89. **The job zeroes the count when it finishes.**
    // So the doubled dispatch, which runs after the real one, may see count=0,
    // fail the bounds check for every system, and take the default for both
    // declarations - which is exactly a duplicate.
    // These three fields test that directly.
    uintptr_t tbl_ptr;      // [0x705CA18] - renderer+0x08, the resource table
    int32_t   tbl_count;    // [0x705CA24] - renderer+0x14, the bounds-check count
    int32_t   tbl_c20;      // [0x705CA20] - renderer+0x10, a sibling count
};

// TWO jobs now, both sides: [job][side]. job 0 = GPU Particle Simulate
// (0x19AE9E0), job 1 = RenderDeferredPasses (0x1873830). FOUR SEPARATE
// BUDGETS - a shared cap would let whichever job dispatches first consume
// every slot and report the other as absent.
constexpr uint32_t kV1Jobs = 2;
constexpr uint32_t kV1Sides = 2;
const uintptr_t kV1JobRva[kV1Jobs] = { kP1ParticleSimJobRva, kP1DeferredPassesJobRva };
const char* const kV1JobName[kV1Jobs] = { "ParticleSim", "DeferredPasses" };
const char* const kV1SideName[kV1Sides] = { "REAL   ", "DOUBLED" };

std::atomic_bool v1_armed{false};
std::atomic_bool v1_selftest_passed{false};
V1Rec v1_recs[kV1Jobs][kV1Sides][kV1PerSide]{};
std::atomic_uint32_t v1_kept[kV1Jobs][kV1Sides]{};
std::atomic_uint32_t v1_seen[kV1Jobs][kV1Sides]{};

uint64_t v1_fnv1a(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// THE PRODUCTION PATH for the field that carries the answer. Takes jobArg by
// value so the self-test can hand it a synthetic one. Returns false only when
// jobArg itself is unreadable - every other failure is reported in rec.ok.
bool v1_decode_jobarg(uintptr_t job_arg, V1Rec& rec) {
    rec.job_arg = job_arg;
    if (!job_arg) return false;
    uint8_t raw[0x28]{};
    if (!safe_read_bytes(job_arg, raw, sizeof(raw))) return false;
    rec.ok |= kV1OkArg;
    std::memcpy(&rec.mid,   raw + 0x00, sizeof(rec.mid));
    std::memcpy(&rec.arg08, raw + 0x08, sizeof(rec.arg08));
    std::memcpy(&rec.arg10, raw + 0x10, sizeof(rec.arg10));
    std::memcpy(&rec.arg18, raw + 0x18, sizeof(rec.arg18));
    std::memcpy(&rec.view_index, raw + 0x20, sizeof(rec.view_index));
    if (rec.mid) {
        uintptr_t ctx = 0;
        if (safe_read_bytes(rec.mid, &ctx, sizeof(ctx))) {
            rec.ok |= kV1OkMid;
            rec.view_ctx = ctx;
            if (ctx) {
                rec.ok |= kV1OkCtx;
                uint32_t live = 0;
                if (safe_read_bytes(ctx + 0xB974, &live, sizeof(live))) rec.ok |= kV1OkCtxLive;
            }
        }
    }
    return true;
}

// The globals half. Separate because it cannot be exercised with a synthetic
// base without faking 117 MB of module, and because it carries no decode
// ambiguity - each field is one dword at one fixed RVA.
void v1_decode_globals(uintptr_t base, V1Rec& rec) {
    if (!base) return;
    bool all = true;
    all &= safe_read_bytes(base + kV1EarlyOutRva,   &rec.early_out,    sizeof(rec.early_out));
    all &= safe_read_bytes(base + kV1PredG9A0Rva,   &rec.pred_g9a0,    sizeof(rec.pred_g9a0));
    all &= safe_read_bytes(base + kV1PredG9F40Rva,  &rec.pred_g9f40,   sizeof(rec.pred_g9f40));
    all &= safe_read_bytes(base + kV1QueueFamRva,   &rec.queue_family, sizeof(rec.queue_family));
    all &= safe_read_bytes(base + kV1FrameDeltaRva, &rec.frame_delta,  sizeof(rec.frame_delta));
    if (all) rec.ok |= kV1OkGlobals;
    // E1: the bounds-check triple. Read here so it is captured at the same
    // instant as everything else in the record, on both sides.
    safe_read_bytes(base + 0x705CA18, &rec.tbl_ptr, sizeof(rec.tbl_ptr));
    safe_read_bytes(base + 0x705CA24, &rec.tbl_count, sizeof(rec.tbl_count));
    safe_read_bytes(base + 0x705CA20, &rec.tbl_c20, sizeof(rec.tbl_c20));
    bool arrs = true;
    arrs &= safe_read_bytes(base + kV1PerViewArrRva, &rec.per_view_arr, sizeof(rec.per_view_arr));
    arrs &= safe_read_bytes(base + kV1FroxArrARva,   &rec.frox_a,       sizeof(rec.frox_a));
    arrs &= safe_read_bytes(base + kV1FroxArrBRva,   &rec.frox_b,       sizeof(rec.frox_b));
    if (arrs) rec.ok |= kV1OkArrays;
    // Index the arrays ONLY with a view index we actually decoded and that is
    // in a sane range. An out-of-range index is the finding, not a licence to
    // read at it.
    const int32_t vi = rec.view_index;
    if ((rec.ok & kV1OkArg) && vi >= 0 && vi < 64) {
        if (rec.frox_a && safe_read_bytes(rec.frox_a + static_cast<size_t>(vi) * 4,
                &rec.frox_a_val, sizeof(rec.frox_a_val))) rec.ok |= kV1OkFroxA;
        if (rec.frox_b && safe_read_bytes(rec.frox_b + static_cast<size_t>(vi) * 4,
                &rec.frox_b_val, sizeof(rec.frox_b_val))) rec.ok |= kV1OkFroxB;
        if (rec.per_view_arr) {
            uint8_t pv[kV1PerViewStride]{};
            if (safe_read_bytes(rec.per_view_arr + static_cast<size_t>(vi) * kV1PerViewStride,
                    pv, sizeof(pv))) {
                rec.per_view_hash = v1_fnv1a(pv, sizeof(pv));
                rec.ok |= kV1OkPerView;
            }
        }
    }
}

// Refuses to arm unless the decoder round-trips a synthetic record AND
// correctly rejects two invalid ones. Storage is file-static so the pointers
// stay valid; 48 KB of BSS, sized past the 0xB974 liveness probe.
alignas(16) uint8_t v1_fake_ctx[0xC000]{};
uintptr_t v1_fake_mid = 0;
alignas(16) uint8_t v1_fake_jobarg[0x28]{};
uintptr_t v1_fake_mid_null = 0;

bool v1_selftest(std::string& detail) {
    constexpr int32_t kPlantView = 0x2B7;
    constexpr uint64_t kPlant08 = 0xA1A1A1A1B2B2B2B2ull;
    constexpr uint64_t kPlant10 = 0xC3C3C3C3D4D4D4D4ull;
    constexpr uint64_t kPlant18 = 0xE5E5E5E5F6F6F6F6ull;
    v1_fake_mid = reinterpret_cast<uintptr_t>(v1_fake_ctx);
    const uintptr_t midptr = reinterpret_cast<uintptr_t>(&v1_fake_mid);
    std::memcpy(v1_fake_jobarg + 0x00, &midptr, sizeof(midptr));
    std::memcpy(v1_fake_jobarg + 0x08, &kPlant08, sizeof(kPlant08));
    std::memcpy(v1_fake_jobarg + 0x10, &kPlant10, sizeof(kPlant10));
    std::memcpy(v1_fake_jobarg + 0x18, &kPlant18, sizeof(kPlant18));
    std::memcpy(v1_fake_jobarg + 0x20, &kPlantView, sizeof(kPlantView));

    V1Rec good{};
    const bool ret = v1_decode_jobarg(reinterpret_cast<uintptr_t>(v1_fake_jobarg), good);
    const uint32_t want = kV1OkArg | kV1OkMid | kV1OkCtx | kV1OkCtxLive;
    const bool fields_ok = ret && good.ok == want &&
        good.view_index == kPlantView && good.arg08 == kPlant08 &&
        good.arg10 == kPlant10 && good.arg18 == kPlant18 &&
        good.mid == midptr && good.view_ctx == reinterpret_cast<uintptr_t>(v1_fake_ctx);

    // Control 1: a null jobArg must be REJECTED, not decoded as zeros.
    V1Rec nul{};
    const bool null_rejected = !v1_decode_jobarg(0, nul) && nul.ok == 0;

    // Control 2: a jobArg whose *jobArg is null must decode the argument block
    // but must NOT claim a context. This is the exact shape a stale stack
    // frame produces, and it is the reading we must never mistake for data.
    alignas(16) uint8_t stale[0x28]{};
    const uintptr_t nullmid = reinterpret_cast<uintptr_t>(&v1_fake_mid_null);
    std::memcpy(stale + 0x00, &nullmid, sizeof(nullmid));
    V1Rec st{};
    const bool stale_ok = v1_decode_jobarg(reinterpret_cast<uintptr_t>(stale), st) &&
        (st.ok & kV1OkArg) && (st.ok & kV1OkMid) &&
        !(st.ok & kV1OkCtx) && !(st.ok & kV1OkCtxLive);

    detail = std::format("roundTrip={} nullRejected={} staleRejected={} okBits={:#x} view={}",
        fields_ok, null_rejected, stale_ok, good.ok, good.view_index);
    return fields_ok && null_rejected && stale_ok;
}
#endif  // DOOMVR_RESEARCH_TOOLS

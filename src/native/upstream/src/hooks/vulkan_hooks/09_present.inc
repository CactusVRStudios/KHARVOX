// 09_present.inc - the present path, key handlers, forced render resolution
// 3874 of these lines sit inside #ifdef DOOMVR_RESEARCH_TOOLS,
// which is ON by default - the guard is NOT a shipping/research boundary.
//
// PART OF src/hooks/vulkan_hooks.cpp - NOT a translation unit. It is
// #included, in order, inside `namespace kharvoxnative::vk_hooks { namespace {`.
// The fragments share ONE scope and internal linkage, so declaration order
// matters and the include list must not be reordered.


void cutscene_detector_sample() {
    static uint32_t presents = 0;
    static uint64_t window_start_tick = 0;
    static float settled_fps = 0.0f;
    static float pending_fps = 0.0f;
    static bool have_settled = false;
    static bool have_pending = false;

    const uint64_t now = GetTickCount64();
    if (window_start_tick == 0) { window_start_tick = now; presents = 0; return; }
    if (++presents < 60) return;

    const uint64_t elapsed = now - window_start_tick;
    presents = 0;
    window_start_tick = now;
    if (elapsed == 0) return;
    const float fps = 60000.0f / static_cast<float>(elapsed);

    if (!have_settled) { settled_fps = fps; have_settled = true; return; }

    // 8 fps separates a regime change from frame-to-frame jitter; 4 fps is
    // "the same regime" for the confirmation. Both come from the measured
    // 80 -> 60 step, which is 20 fps wide - they are not tuned against
    // anything, and there is no knob here to turn.
    constexpr float kRegimeDelta = 8.0f;
    constexpr float kSameRegime = 4.0f;
    if (std::fabs(fps - settled_fps) < kRegimeDelta) { have_pending = false; return; }

    if (!have_pending || std::fabs(fps - pending_fps) >= kSameRegime) {
        // First sight of a new value, or the candidate moved again (the 72.5
        // transition window). Re-arm on the newest value rather than confirm.
        pending_fps = fps;
        have_pending = true;
        return;
    }

    // Two consecutive windows agree on a value materially different from the
    // settled one. That is an edge.
    const float from = settled_fps;
    settled_fps = fps;
    have_pending = false;

    const bool looks_like_cutscene_enter = fps < 65.0f && from > fps + kRegimeDelta;
    const bool looks_like_cutscene_exit = from < 65.0f && fps > from + kRegimeDelta;
    // Dense capture at BOTH edges, automatically. The 19:14 run validated this
    // detector against the human marker (it fired 1.8 s after marker #1, inside
    // its own stated lag), so it is trustworthy enough to drive coverage - and
    // unlike the marker it does not depend on anyone reacting in time.
    cutscene_dense_burst(looks_like_cutscene_enter   ? "cadence edge: cutscene ENTER"
                         : looks_like_cutscene_exit  ? "cadence edge: cutscene EXIT"
                                                     : "cadence edge");
    // ===== AUTOMATIC RECOVERY (2026-08-24) =====
    //
    // A cutscene is a scene transition, and a scene transition is what leaves
    // our mirrors pointing at framebuffers and views the game has torn down.
    // That is why VR never comes back on its own today: nothing rebuilds them,
    // and every flag stays correct so nothing notices.
    //
    // The cadence detector has been reliable at both edges across four runs, so
    // it is trustworthy enough to drive this. On the EXIT edge - the moment
    // gameplay resumes and the scene is stable again - the mirrors are rebuilt
    // and stereo re-armed, without anyone pressing anything.
    //
    // The EXIT edge, not the ENTER edge, deliberately: during the cutscene
    // there may be no stable scene to build mirrors against, and rebuilding
    // into a transition is what produced the mismatched-eyes flip earlier.
    // AUTO-RECOVER WITHDRAWN (2026-08-24). It was built on the theory that the
    // mirrors go stale at a scene transition. That theory was WRONG - it was
    // tested and VR still did not come back - and the real cause turned out to
    // be a missing descriptor_set_update_frame stamp in the template update
    // path. Rebuilding mirrors on every cutscene exit therefore buys nothing
    // and costs a visible re-arm each time, so it is removed rather than left
    // in "just in case". SHIFT+DOWN still does it by hand if ever needed.
    (void)fast_vr_auto_recover;
    // Capture pixels AT the transition, and again once it has settled. Only on
    // the first cutscene of a session, so the freeze budget is bounded.
    if (looks_like_cutscene_enter) {
        static bool captured_once = false;
        if (!captured_once) {
            captured_once = true;
            arm_pixel_pair("cadence edge: cutscene ENTER");
            // The second capture rides the existing series re-arm, which fires
            // kFinalPassDumpGapFrames later - so the settled state is recorded
            // without a second detector.
            final_pass_dump_rearm_at.store(
                presented_frame.load(std::memory_order_relaxed) + 360,
                std::memory_order_relaxed);
        }
    }
    log::warn(std::format(
        "PRESENT CADENCE {:.1f} -> {:.1f} fps (confirmed over 2 windows of 60 presents, so this "
        "line lands ~1-2 s AFTER the true edge - do not read the timestamp as the edge). {}",
        from, settled_fps,
        looks_like_cutscene_enter
            ? "⚠ CUTSCENE CANDIDATE: ENTER. A drop to a ~60 fps DOOM cinematic cadence from a "
              "materially higher headset-driven one. PROVISIONAL - this rule comes from exactly "
              "one cutscene in one run; the cadence figures above are the measurement, this "
              "label is the inference."
        : looks_like_cutscene_exit
            ? "⚠ CUTSCENE CANDIDATE: EXIT. Cadence has come back up off ~60. Same provisional "
              "rule, same caveat."
            : "Not a cutscene-shaped edge by the current rule - reported because the raw cadence "
              "change is a measurement regardless of what caused it."));
}

// Per-second scene census. See the counters' declaration for why this exists
// and what its polarity is. Emitted from the present hook, uncapped - it is one
// line a second and it is the line that decides whether the cutscene has any
// geometry to apply a camera to.
void scene_census_emit() {
    census_frames.fetch_add(1, std::memory_order_relaxed);
    static uint64_t last_tick = 0;
    const uint64_t now = GetTickCount64();
    if (last_tick == 0) { last_tick = now; return; }
    if (now - last_tick < 1000) return;
    last_tick = now;
    const uint32_t frames = census_frames.exchange(0, std::memory_order_relaxed);
    const uint32_t dr = census_draws_real.exchange(0, std::memory_order_relaxed);
    const uint32_t dd = census_draws_doubled.exchange(0, std::memory_order_relaxed);
    const uint64_t ir = census_indices_real.exchange(0, std::memory_order_relaxed);
    const uint64_t id = census_indices_doubled.exchange(0, std::memory_order_relaxed);
    if (frames == 0) return;
    const double dpf_r = static_cast<double>(dr) / frames;
    const double dpf_d = static_cast<double>(dd) / frames;
    // Persistence verdict, read here because this runs after DOOM has finished
    // the frame and submitted it.
    std::string persist = "no sample";
    if (lever_a_persist_valid.load(std::memory_order_relaxed)) {
        const uintptr_t a = lever_a_persist_addr.load(std::memory_order_relaxed);
        const float wrote = lever_a_persist_wrote.load(std::memory_order_relaxed);
        float now_val = 0.0f;
        if (a && safe_read_bytes(a, &now_val, sizeof(now_val))) {
            const float d = now_val - wrote;
            persist = std::format(
                "wrote {:.3f}, reads {:.3f} at end of frame, delta {:.3f} -> {}",
                wrote, now_val, d,
                (d > -0.001f && d < 0.001f)
                    ? "INTACT (our camera shift is still there when DOOM finishes the frame)"
                    : "⚠ REVERTED - something rewrote the camera after we patched it, which "
                      "would make both eyes render the same view");
        } else {
            persist = "address no longer readable";
        }
    }
    log::warn(std::format("LeverAPersist: {}", persist));
    log::warn(std::format(
        "SceneCensus over {} frames: REAL {:.0f} draws/frame ({} indices/frame) | DOUBLED "
        "{:.0f} draws/frame ({} indices/frame) | doubled/real ratio {:.2f}. "
        "⚠ READ THE MAGNITUDE FIRST. Gameplay runs on the order of a thousand draws per frame. "
        "A collapse to a handful means the frame is a FULLSCREEN VIDEO QUAD with no world "
        "geometry - nothing is rendered from a camera, so offsetting one cannot produce "
        "parallax and both eyes MUST come out identical. That is a different problem from a "
        "broken camera write, and it needs a different fix: detect the state, do not fight it. "
        "A ratio near 1.00 with high counts means the doubled call IS drawing the scene and the "
        "divergence is failing somewhere else.",
        frames, dpf_r, frames ? ir / frames : 0, dpf_d, frames ? id / frames : 0,
        dpf_r > 0.0 ? dpf_d / dpf_r : 0.0));
}

VkResult VKAPI_PTR hook_queue_present(VkQueue queue, const VkPresentInfoKHR* info) {
    // ===== INSTRUMENT SELF-TEST (2026-08-25) ==============================
    //
    // WHY THIS EXISTS. Three runs have now been lost to defects in this file
    // that only became visible after the owner played for five minutes: a
    // format-argument ordering slip, a scan regex that silently excluded every
    // export with a digit in its name, and 53 hooks registered AFTER
    // MH_EnableHook so they were created but never enabled and every one
    // reported zero calls. The coverage audit could not catch the last one: it
    // checks REGISTRATION, not LIVENESS.
    //
    // These hooks CANNOT legitimately read zero while the game is drawing.
    // DOOM cannot record 2000 draws without beginning and ending a command
    // buffer, and it cannot display them without submitting and presenting. So
    // a zero here is proof the instrument is broken, not a measurement.
    //
    // Fires once, ~300 presents in - about five seconds of gameplay - so a
    // broken build costs thirty seconds instead of a five-minute run.
    {
        static bool self_tested = false;
        if (!self_tested && presented_frame.load(std::memory_order_relaxed) >= 300) {
            self_tested = true;
            struct MustFire { int id; const char* why; };
            const MustFire must[] = {
                {FB_H_BEGIN_COMMAND_BUFFER, "DOOM cannot record any draw without it"},
                {FB_H_END_COMMAND_BUFFER,   "every recorded buffer must be ended"},
                {FB_H_CMD_BIND_DESCRIPTOR_SETS, "every draw binds descriptors"},
                {FB_H_CMD_DRAW_INDEXED,     "the scene is drawn with indexed draws"},
                {FB_H_CMD_BIND_PIPELINE,    "every pass binds a pipeline"},
            };
            std::string dead;
            for (const MustFire& m : must)
                if (!fb_hook_ever[m.id].load(std::memory_order_relaxed))
                    dead += std::format(" {} ({})", kFbHookExport[m.id], m.why);
            uint32_t live = 0;
            for (int i = 0; i < FB_H_COUNT; ++i)
                if (fb_hook_ever[i].load(std::memory_order_relaxed)) ++live;
            if (dead.empty()) {
                log::warn(std::format(
                    "INSTRUMENT SELF-TEST: PASS - {} of {} hooks have fired at least once, and "
                    "every must-fire hook is live. The FrameBudget numbers can be trusted and "
                    "this run is worth playing out.",
                    live, static_cast<int>(FB_H_COUNT)));
            } else {
                log::error(std::format(
                    "INSTRUMENT SELF-TEST: ***FAIL - ABORT THIS RUN***. These hooks reported ZERO "
                    "calls while the game is drawing, which is impossible unless they are not "
                    "live:{}\n"
                    "Only {} of {} hooks ever fired. Do NOT play on - quit now and say so; the "
                    "build is broken and the run would be wasted.",
                    dead, live, static_cast<int>(FB_H_COUNT)));
            }
            // VIEWSRC LIVENESS, on the same 300-present timer and for the same
            // reason: prove the probe RUNS before the owner spends five minutes
            // of play on it. DOOM cannot present 300 frames of anything without
            // building a view's matrices, so zero here is a broken hook, never
            // a null. It is reported separately from the must-fire list above
            // because this is a DOOM-side detour, not a Vulkan export, and
            // folding it in would change the meaning of that line's counts.
            const uint64_t vs_n = g_vs_calls.load(std::memory_order_relaxed);
            const uint64_t a1_n = fb_doom_calls[FB_D_RENDER_GATHER_PREPARE]
                                      .load(std::memory_order_relaxed);
            if (vs_n == 0) {
                log::error(std::format(
                    "VIEWSRC SELF-TEST: ***FAIL - ABORT THIS RUN***. The producer hook at "
                    "DOOM+0x1830B80 reported ZERO calls across 300 presents. DOOM cannot draw "
                    "a frame without building a view's matrices, so this is the hook not being "
                    "live, not a measurement. (RenderGatherPrepare fired {} times over the same "
                    "period, so the DOOM-side detour table itself is working.) Quit now and say "
                    "so - the run would be wasted.",
                    a1_n));
            } else {
                log::warn(std::format(
                    "VIEWSRC SELF-TEST: PASS - {} producer calls in 300 presents "
                    "({} before A1, {} after). Read the VIEWSRC line for the verdict.",
                    vs_n, g_vs_before_a1.load(std::memory_order_relaxed),
                    g_vs_after_a1.load(std::memory_order_relaxed)));
            }
            // THE WRITE'S OWN LIVENESS, not the hook's. A live hook whose write
            // is refused every call looks exactly like a live hook that is
            // working, right up until the run is over and the picture never
            // changed. Checked on the same 300-present timer so a dead arm
            // costs thirty seconds instead of five minutes of play.
            if (wrap::precull_active() && wrap::armed()) {
                const uint64_t w = g_pc_written.load(std::memory_order_relaxed);
                if (w == 0) {
                    log::error(std::format(
                        "PRECULL SELF-TEST: ***FAIL - ABORT THIS RUN***. precull is ARMED and "
                        "the producer hook fired {} times, but ZERO writes landed. Refusals: "
                        "noHead={} noBasis={} axisBad={} postBad={} doubleGuard={} "
                        "writeFail={} readbackBad={} angleBad={}. The named reason is the "
                        "finding; quit now and say so rather than playing out a parked arm.",
                        vs_n,
                        g_pc_no_head.load(std::memory_order_relaxed),
                        g_pc_no_basis.load(std::memory_order_relaxed),
                        g_pc_axis_bad.load(std::memory_order_relaxed),
                        g_pc_post_bad.load(std::memory_order_relaxed),
                        g_pc_double_guard.load(std::memory_order_relaxed),
                        g_pc_write_fail.load(std::memory_order_relaxed),
                        g_pc_readback_bad.load(std::memory_order_relaxed),
                        g_pc_angle_bad.load(std::memory_order_relaxed)));
                } else {
                    log::warn(std::format(
                        "PRECULL SELF-TEST: PASS - {} axis writes landed in 300 presents, "
                        "rebuilt +0x1270 clean={} rot1={} rot2={} other={}. rot1 is the "
                        "target; clean would mean the write is not reaching the matrices.",
                        w,
                        g_pc_post_state[0].load(std::memory_order_relaxed),
                        g_pc_post_state[1].load(std::memory_order_relaxed),
                        g_pc_post_state[2].load(std::memory_order_relaxed),
                        g_pc_post_state[3].load(std::memory_order_relaxed)));
                }
            }
        }
    }
    // DEPTH-LEAK GUARD. fb_depth must be 0 between frames on this thread. A
    // manual-timed hook that missed an exit path would leave it non-zero, and
    // every ALLHOOKS number after that point would be wrong. Caught, counted,
    // reset, and published as depthLeaks on the FrameBudget line - so the
    // failure announces itself instead of quietly poisoning the measurement.
    if (fb_depth != 0) {
        fb_depth_leaks.fetch_add(1, std::memory_order_relaxed);
        fb_depth = 0;
    }
    const uint64_t fb_entry_ns = fb_ns();
    (void)fb_clock_cost_ns();   // calibrate on the first present, never in a hook
    cutscene_detector_sample();
    scene_census_emit();
    containment_sample(2);  // CONTAINMENT event 2 - see containment_sample.
#ifdef DOOMVR_RESEARCH_TOOLS
    // EFFECTS STAGE DUMP, on the present thread after submit - same reasoning
    // as the final-stage snapshot below. Two PPMs: the doubled mirror after
    // lighting and after transparency. Whichever first shows the fire/glow,
    // and whichever first does not, brackets the pass that loses the effects.
    if (effects_stage_dump_armed.load(std::memory_order_relaxed) &&
        effects_stage_dump_ready.exchange(false, std::memory_order_relaxed)) {
        effects_stage_dump_armed.store(false, std::memory_order_relaxed);
        // Every distinct HDR stage captured this frame, in render order. The
        // first one that should carry fog/fire/sparks and does not is the pass
        // that loses them.
        for (uint32_t s = 0; s < effects_stage_count; ++s) {
            VkImageLayout sl = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            { std::scoped_lock lk(mirror_mutex); sl = mirror_layout_of(effects_stage_colors[s]); }
            log::warn(std::format(
                "EffectsStage[{}]: HDR mirror {} pass={} {}x{} layout={}", s,
                reinterpret_cast<void*>(effects_stage_colors[s]), effects_stage_passes[s],
                effects_stage_extent_shared.width, effects_stage_extent_shared.height,
                static_cast<int>(sl)));
            dump_image_snapshot(effects_stage_colors[s], effects_stage_extent_shared,
                                VK_FORMAT_B10G11R11_UFLOAT_PACK32,
                                std::format("effects_stage{}_p{}_{}", s, effects_stage_passes[s],
                                    effects_capture_suffix.load(std::memory_order_relaxed)).c_str(),
                                sl);
        }
        // DOOM's own targets at the same passes. These are DOOM's real images,
        // not mirrors, so their layout is not in mirror_image_layouts - by this
        // point in the frame a colour target DOOM has finished with is in
        // SHADER_READ_ONLY, which is what the chain samples it as.
        for (uint32_t s = 0; s < real_stage_count; ++s) {
            log::warn(std::format(
                "RealStage[{}]: DOOM's own HDR target {} pass={} {}x{}", s,
                reinterpret_cast<void*>(real_stage_colors[s]), real_stage_passes[s],
                effects_stage_extent_shared.width, effects_stage_extent_shared.height));
            dump_image_snapshot(real_stage_colors[s], effects_stage_extent_shared,
                                VK_FORMAT_B10G11R11_UFLOAT_PACK32,
                                std::format("real_stage{}_p{}_{}", s, real_stage_passes[s],
                                    effects_capture_suffix.load(std::memory_order_relaxed)).c_str(),
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        // PAIRING TABLE. effects_stage[s] and real_stage[s] are captured by
        // independent walks, so index s does NOT necessarily mean the same
        // pass on both sides. Pair by PASS NUMBER, which is in every filename
        // and printed here, and never by index - and never compare two
        // different passes to each other, which is the tone-curve confound
        // that has already cost this project a measurement.
        {
            std::string es, rs;
            for (uint32_t s = 0; s < effects_stage_count; ++s)
                es += (es.empty() ? "" : ",") + std::format("s{}=p{}", s, effects_stage_passes[s]);
            for (uint32_t s = 0; s < real_stage_count; ++s)
                rs += (rs.empty() ? "" : ",") + std::format("s{}=p{}", s, real_stage_passes[s]);
            log::warn(std::format(
                "StageBracket PAIRING sample={} suppressLightCulling={}: eye1 [{}] real [{}]. "
                "The arm is read HERE, before the switch below, so it is the state that was in "
                "force while these ten images were rendered. Files carry the same sample "
                "suffix. Compare eye1 and real AT THE SAME "
                "pass p. THE READING: FOG dark30 is the correct-sign measure - fog adds light "
                "everywhere and lifts darks, so its absence shows as MORE dark pixels and LOWER "
                "meanLuma. Walk the passes in order; the first pass where eye1's dark30 rises "
                "clearly above real's at the SAME pass is where the additive layer is lost. If "
                "eye1 tracks real at every shared pass, the loss is downstream of this chain and "
                "the bracket has excluded it. An empty list on either side means nothing was "
                "captured for that side and NOTHING may be concluded.",
                effects_capture_suffix.load(std::memory_order_relaxed),
                suppress_doubled_light_culling.load(std::memory_order_relaxed) ? "ON" : "OFF",
                es.empty() ? std::string("NONE") : es,
                rs.empty() ? std::string("NONE") : rs));
        }
        // ---- LIGHT-CULLING A/B, DRIVEN BY THE STAGE BRACKET (2026-08-21).
        //
        // The flip lives here, immediately after a stage sample has been
        // dumped and labelled, and the next sample does not arm for another
        // kEffectsSeriesGapFrames presents - so no set of ten images can
        // straddle the two arms.
        //
        // Driven by the bracket rather than by uniform_diff_reports because
        // the UniformDiff instrument is switched off for this run; leaving the
        // old driver in place is harmless (it cannot fire with reports stuck
        // at 0) and keeps a uniform-diff A/B working if one is ever wanted.
        //
        // WHY THIS IS NOT THE A/B THAT ALREADY RAN. The 15:10 A/B compared the
        // two arms with whole-frame px>200, which reads 0 against 0 at p15 -
        // the stage where the deficit actually lives. This reads the same two
        // arms at p15's dark30, which is 32 386 vs 441 083 between the eyes
        // and is the measure that can see it.
        if (light_cull_ab_active.load(std::memory_order_relaxed) &&
            suppress_doubled_light_culling.load(std::memory_order_relaxed)) {
            suppress_doubled_light_culling.store(false, std::memory_order_relaxed);
            light_cull_ab_flips.fetch_add(1, std::memory_order_relaxed);
            log::warn(std::format(
                "LightCullAB: ARM B from here - suppress_doubled_light_culling now OFF, set in "
                "code. The stage sample just dumped is ARM A (suppression ON, the shipping "
                "state); the next one is ARM B. suppressedDispatches so far={} - it must now "
                "STOP climbing, and that is the positive control: if it keeps rising the flip "
                "did not take and arm B means nothing. THE READING: eye1's p15 dark30 falling "
                "toward DOOM's ~32k in arm B confirms the doubled call's own light culling is "
                "what its lighting was missing. Unchanged at ~441k refutes it and the starvation "
                "is elsewhere. EXPECTED SIDE EFFECT: eye 0's sparks degrade in arm B - that is "
                "the cost suppression pays and is not a fault.",
                suppressed_light_culling_dispatches.load(std::memory_order_relaxed)));
        }
        // THE DECIDING PAIR (Run 3). The deferred chain's only additive HDR
        // input, DOOM's real one against ours, same frame. If ours is empty
        // and DOOM's is not, the missing fog/lava/sparks are missing because
        // nothing ever wrote our copy - and the fix is to find and double
        // whatever writes it, not to touch bindings again.
        //
        // Both are dumped with the SAME format and extent, so ppmbright.ps1
        // compares them directly against the p19 baselines already on disk.
        if (deferred_hdr_input_real != VK_NULL_HANDLE &&
            deferred_hdr_input_extent.width) {
            log::warn(std::format(
                "DeferredHdrInput [gpu] REAL image={} pass={} {}x{} fmt=122 - DOOM's own copy of "
                "the deferred chain's only additive HDR input",
                reinterpret_cast<void*>(deferred_hdr_input_real), deferred_hdr_input_pass,
                deferred_hdr_input_extent.width, deferred_hdr_input_extent.height));
            dump_image_snapshot(deferred_hdr_input_real, deferred_hdr_input_extent,
                                VK_FORMAT_B10G11R11_UFLOAT_PACK32,
                                std::format("hdrinput_real_p{}_{}", deferred_hdr_input_pass,
                                    effects_capture_suffix.load(std::memory_order_relaxed)).c_str(),
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        if (deferred_hdr_input_mirror != VK_NULL_HANDLE &&
            deferred_hdr_input_extent.width) {
            VkImageLayout ml = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            { std::scoped_lock lk(mirror_mutex); ml = mirror_layout_of(deferred_hdr_input_mirror); }
            log::warn(std::format(
                "DeferredHdrInput [gpu] MIRROR image={} pass={} {}x{} fmt=122 layout={} - OUR copy "
                "of the same input. Empty here with content on the REAL side names the buffer.",
                reinterpret_cast<void*>(deferred_hdr_input_mirror), deferred_hdr_input_pass,
                deferred_hdr_input_extent.width, deferred_hdr_input_extent.height,
                static_cast<int>(ml)));
            dump_image_snapshot(deferred_hdr_input_mirror, deferred_hdr_input_extent,
                                VK_FORMAT_B10G11R11_UFLOAT_PACK32,
                                std::format("hdrinput_mirror_p{}_{}", deferred_hdr_input_pass,
                                    effects_capture_suffix.load(std::memory_order_relaxed)).c_str(),
                                ml);
        } else if (deferred_hdr_input_real != VK_NULL_HANDLE) {
            log::error(
                "DeferredHdrInput: the REAL side was captured but NO MIRROR could be resolved for "
                "it by any of the three paths (mirrored_images / mirrored_views / "
                "alias_mirror_views). That is itself the answer - the doubled call samples DOOM's "
                "real additive HDR layer, and Run 2's mirrorHeld reading was wrong for THIS "
                "binding specifically.");
        }
        // INDIRECT ARGS, read here and not on the render thread: the copies
        // were only RECORDED during the frame. The stage dumps above each
        // drain the queue, so by this point the GPU has executed them and the
        // mapped buffer holds this frame's values.
        if (indirect_args_ready.exchange(false, std::memory_order_acquire))
            emit_indirect_args_table();
        // Depth strip, read here for the same reason as the args table: the
        // copies were only RECORDED during the frame, and the dumps above
        // drain the queue, so by this point the GPU has executed them.
        if (depth_verdict_pending.exchange(false, std::memory_order_acquire))
            emit_depth_content_verdict();
        // Its own ready flag, so it is deliberately NOT inside the depth
        // verdict's gate - the two probes arm and complete independently.
        emit_froxel_content_verdict();
        // PASS TIMELINE. Four moments inside ONE frame of the doubled call,
        // in GPU order. Read the four PPMs in pass order: the first one that
        // should carry sparks and does not is the pass that loses them - and
        // unlike every previous capture, "pass" here means when, not which.
        if (pass_timeline_ready.exchange(false, std::memory_order_acquire)) {
            for (uint32_t s = 0; s < kChainWalkSlots; ++s) {
                VkImage image = VK_NULL_HANDLE;
                VkExtent2D extent{};
                VkFormat format = VK_FORMAT_UNDEFINED;
                uint32_t pass = 0;
                {
                    std::scoped_lock lock(chainwalk_mutex);
                    const ChainWalkSlot& slot = chainwalk_slots[s];
                    if (slot.filled_attempt == 0 && slot.image != VK_NULL_HANDLE) {
                        image = slot.image;
                        extent = slot.extent;
                        format = slot.format;
                        pass = slot.filled_pass;
                    }
                }
                if (image == VK_NULL_HANDLE) {
                    log::error(std::format(
                        "PassTimeline slot={} (pass {}) EMPTY - no copy was recorded at that pass "
                        "boundary this frame. Either the doubled call never reached that pass, or "
                        "its colour attachment was not a mirror with a known final layout. An "
                        "absent file, not a stale one.",
                        s, kTimelinePasses[s]));
                    continue;
                }
                const std::string label = std::format(
                    "timeline_p{}_{}", pass,
                    effects_capture_suffix.load(std::memory_order_relaxed));
                log::warn(std::format(
                    "PassTimeline [gpu] slot={} pass={} image={} {}x{} format={} -> {}",
                    s, pass, reinterpret_cast<void*>(image), extent.width, extent.height,
                    static_cast<int>(format), label));
                // TRANSFER_DST_OPTIMAL: the pass-boundary copy left it there
                // and nothing else touches these images.
                dump_image_snapshot(image, extent, format, label.c_str(),
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            }
            // BEFORE image of pass 25, to difference against timeline_p25
            // (the AFTER of the SAME image). ppmbright on the pair answers the
            // fork directly: identical means the pass drew nothing, and the
            // fragments are discarded; AFTER carrying the sparks means they
            // are drawn correctly and destroyed later.
            {
                VkImage img = VK_NULL_HANDLE;
                VkExtent2D ext{};
                VkFormat fmt = VK_FORMAT_UNDEFINED;
                uint32_t bp = 0;
                {
                    std::scoped_lock lk(chainwalk_mutex);
                    if (beginend_filled && beginend_image != VK_NULL_HANDLE) {
                        img = beginend_image; ext = beginend_extent;
                        fmt = beginend_format; bp = beginend_pass;
                    }
                }
                if (img == VK_NULL_HANDLE) {
                    log::error("BeginEnd: BEFORE image was never captured this frame - the "
                               "pass-25 comparison is void, do not read the pair.");
                } else {
                    const std::string label = std::format(
                        "beginend_before_p{}_{}", bp,
                        effects_capture_suffix.load(std::memory_order_relaxed));
                    log::warn(std::format(
                        "BeginEnd [gpu] dumping BEFORE pass={} {}x{} -> {}. Compare with "
                        "timeline_p{}_* which is the SAME image at that pass's END.",
                        bp, ext.width, ext.height, label, bp));
                    dump_image_snapshot(img, ext, fmt, label.c_str(),
                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                }
            }
            // REAL-side BEFORE, the control. Read alongside timelinereal_p25
            // (the AFTER of the SAME image): a LARGE real delta beside the
            // doubled call's +0.0001 confirms both the pass and the discard; a
            // small real delta means pass 25 is not where the effects appear
            // and the localisation is wrong.
            {
                VkImage img = VK_NULL_HANDLE;
                VkExtent2D ext{};
                VkFormat fmt = VK_FORMAT_UNDEFINED;
                uint32_t bp = 0;
                {
                    std::scoped_lock lk(chainwalk_mutex);
                    if (beginend_real_filled && beginend_real_image != VK_NULL_HANDLE) {
                        img = beginend_real_image; ext = beginend_real_extent;
                        fmt = beginend_real_format; bp = beginend_real_pass;
                    }
                }
                if (img == VK_NULL_HANDLE) {
                    log::error("BeginEnd REAL: BEFORE never captured - the control is missing and "
                               "the doubled side's zero delta proves nothing this run.");
                } else {
                    const std::string label = std::format(
                        "beginend_realbefore_p{}_{}", bp,
                        effects_capture_suffix.load(std::memory_order_relaxed));
                    log::warn(std::format(
                        "BeginEnd [gpu] dumping REAL BEFORE pass={} {}x{} -> {}. Compare with "
                        "timelinereal_p{}_* (same image, that pass's END).",
                        bp, ext.width, ext.height, label, bp));
                    dump_image_snapshot(img, ext, fmt, label.c_str(),
                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                }
            }
            // REAL side, same four moments. This is the half that makes the
            // doubled side's numbers mean something: eye 1 having 0 bright
            // pixels at pass 20 only names a divergence if eye 0 has them.
            for (uint32_t s = 0; s < real_timeline_slots.size(); ++s) {
                VkImage image = VK_NULL_HANDLE;
                VkExtent2D extent{};
                VkFormat format = VK_FORMAT_UNDEFINED;
                uint32_t pass = 0;
                {
                    std::scoped_lock lock(chainwalk_mutex);
                    const RealTimelineSlot& slot = real_timeline_slots[s];
                    if (slot.filled && slot.image != VK_NULL_HANDLE) {
                        image = slot.image; extent = slot.extent;
                        format = slot.format; pass = slot.filled_pass;
                    }
                }
                if (image == VK_NULL_HANDLE) {
                    log::error(std::format(
                        "PassTimeline REAL slot={} (pass {}) EMPTY - no copy recorded there this "
                        "frame.", s, kTimelinePasses[s]));
                    continue;
                }
                const std::string label = std::format(
                    "timelinereal_p{}_{}", pass,
                    effects_capture_suffix.load(std::memory_order_relaxed));
                log::warn(std::format(
                    "PassTimeline [gpu] REAL DUMP slot={} pass={} {}x{} format={} -> {}",
                    s, pass, extent.width, extent.height, static_cast<int>(format), label));
                dump_image_snapshot(image, extent, format, label.c_str(),
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            }
        }
        if (effects_stage_light_color != VK_NULL_HANDLE) {
            VkImageLayout have = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            { std::scoped_lock lk(mirror_mutex); have = mirror_layout_of(effects_stage_light_color); }
            log::warn(std::format("EffectsStage: dumping FIRST full-screen HDR mirror {} pass={} "
                "{}x{} fmt={} layout={}", reinterpret_cast<void*>(effects_stage_light_color),
                effects_stage_light_pass, effects_stage_light_extent.width,
                effects_stage_light_extent.height,
                static_cast<int>(effects_stage_light_format), static_cast<int>(have)));
            dump_image_snapshot(effects_stage_light_color, effects_stage_light_extent,
                                effects_stage_light_format,
                                std::format("effects_after_lighting_{}",
                                    effects_capture_suffix.load(std::memory_order_relaxed)).c_str(),
                                have);
        }
        if (effects_stage_xparency_color != VK_NULL_HANDLE) {
            VkImageLayout have = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            { std::scoped_lock lk(mirror_mutex); have = mirror_layout_of(effects_stage_xparency_color); }
            log::warn(std::format("EffectsStage: dumping LAST full-screen HDR mirror {} pass={} "
                "{}x{} fmt={} layout={}", reinterpret_cast<void*>(effects_stage_xparency_color),
                effects_stage_xparency_pass, effects_stage_xparency_extent.width,
                effects_stage_xparency_extent.height,
                static_cast<int>(effects_stage_xparency_format), static_cast<int>(have)));
            dump_image_snapshot(effects_stage_xparency_color, effects_stage_xparency_extent,
                                effects_stage_xparency_format,
                                std::format("effects_after_transparency_{}",
                                    effects_capture_suffix.load(std::memory_order_relaxed)).c_str(),
                                have);
        }
    }
    if (effects_stage_dump_armed.load(std::memory_order_relaxed) &&
        effects_stage_light_color != VK_NULL_HANDLE)
        effects_stage_dump_ready.store(true, std::memory_order_release);
    // Re-arm for the next sample in the series (see kFinalPassDumpSeries).
    // Gated on the same switch file as arm_pixel_pair: this is the follow-up
    // that produced the SECOND of the two ~1.5 s freezes, so gating only the
    // first arming site would have halved the symptom and looked like a
    // partial fix.
    if (const uint32_t at = final_pass_dump_rearm_at.load(std::memory_order_relaxed);
        at != 0 && presented_frame.load(std::memory_order_relaxed) >= at) {
        final_pass_dump_rearm_at.store(0, std::memory_order_relaxed);
        if (diagnostics_readbacks_enabled())
            final_pass_dump_pending.store(true, std::memory_order_relaxed);
    }
    // UniformDiff report - see emit_uniform_diff_table. Four samples spaced
    // out, then disarm; each report covers the frames since the last clear, so
    // the values are the most recent frame's.
    // Per-frame reset for the constant repair and the dedupe sets. The real
    // call's blocks are only valid for the frame that wrote them, and the
    // doubled call runs later in that same frame - so this must clear once per
    // present, never mid-frame.
    // Same gate as the capture site: these containers must not be touched
    // while DOOM is still rendering multithreaded.
    // ⚠ kFogRepairOnly MUST BE HERE TOO (2026-08-23). This is where
    // repair_seen_binds is cleared each present. Mode 4 deliberately leaves
    // kRepairDoubledConstants off, so without this the dedupe set was never
    // cleared: the walk ran on frame one, examined 37 blocks, and every later
    // frame found every key already present and did nothing. fogExamined froze
    // at 37 with +0 per window, which is exactly what that looks like.
    //
    // FIFTH blocker of the same shape - a gate keyed on the flag mode 4
    // bypasses. They were found one per headset run, which is three runs too
    // many; the full list was grepped after this one rather than waiting for
    // the next symptom.
    if ((kRepairDoubledConstants || kFogRepairOnly.load(std::memory_order_relaxed)) &&
        frame_root_double_enabled.load(std::memory_order_relaxed)) {
        // Close discovery after two full frames: enough to see every pass, and
        // the last frame the owner has to sit through at diagnostic speed.
        if (!repair_discovery_done.load(std::memory_order_relaxed) &&
            repair_discovery_frames.fetch_add(1, std::memory_order_relaxed) >= 2) {
            repair_discovery_done.store(true, std::memory_order_relaxed);
            std::string passes;
            for (uint32_t p : repair_interesting_passes)
                passes += (passes.empty() ? "" : ",") + std::to_string(p);
            log::warn(std::format(
                "ConstantRepair: discovery closed. Passes carrying a large uniform block this "
                "run: [{}]. Every later frame only walks these, so the run should be full speed "
                "from here. An EMPTY list means nothing was found and no repair can happen - "
                "that is a failure, not a clean result.",
                passes.empty() ? std::string("NONE") : passes));
        }
        repair_real_blocks.clear();
        repair_seen_binds.clear();
        uniform_diff_seen_real.clear();
        uniform_diff_seen_doubled.clear();
        const uint32_t blocks = repair_blocks_touched.load(std::memory_order_relaxed);
        if (blocks && presented_frame.load(std::memory_order_relaxed) % 600 == 0) {
            log::warn(std::format(
                "ConstantRepair: {} blocks repaired, {} zero floats filled, cumulative. Zero here "
                "means the doubled call's constant allocation was never written - the measured "
                "cause of the missing volumetrics and additive light.",
                blocks, repair_floats_written.load(std::memory_order_relaxed)));
        }
    }
    // PIPELINE PARITY: one captured frame, then emit and disarm. Same shape as
    // the uniform diff - arm on a due frame, capture for exactly one frame,
    // report on the next present.
    if (pipeline_parity_armed.load(std::memory_order_relaxed)) {
        if (pipeline_parity_capturing.load(std::memory_order_relaxed)) {
            pipeline_parity_capturing.store(false, std::memory_order_relaxed);
            pipeline_parity_armed.store(false, std::memory_order_relaxed);
            emit_pipeline_parity();
        } else if (presented_frame.load(std::memory_order_relaxed) >=
                   pipeline_parity_at.load(std::memory_order_relaxed)) {
            pipeline_parity_real.clear();
            pipeline_parity_doubled.clear();
            pipeline_parity_seen_real.clear();
            pipeline_parity_seen_doubled.clear();
            pipeline_parity_capturing.store(true, std::memory_order_relaxed);
        }
    }
    if (uniform_diff_armed.load(std::memory_order_relaxed)) {
        if (uniform_diff_capturing.load(std::memory_order_relaxed)) {
            // The frame just recorded was the captured one - both calls have
            // now bound everything they were going to.
            uniform_diff_capturing.store(false, std::memory_order_relaxed);
            // POSITIVE CONTROL for this sample: how many doubled-call
            // dispatches the suppression actually skipped during the frame we
            // just measured. Computed BEFORE the emit so the table can print
            // it, and read as a delta so it means "this sample" rather than
            // "since the process started".
            light_cull_sample_suppressed.store(
                suppressed_light_culling_dispatches.load(std::memory_order_relaxed) -
                    light_cull_suppressed_mark.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            emit_uniform_diff_table();
            if (uniform_diff_reports.fetch_add(1, std::memory_order_relaxed) + 1 >= 4) {
                uniform_diff_armed.store(false, std::memory_order_relaxed);
            } else {
                uniform_diff_next_at.store(
                    presented_frame.load(std::memory_order_relaxed) + kUniformDiffGapFrames,
                    std::memory_order_relaxed);
            }
        } else if (presented_frame.load(std::memory_order_relaxed) >=
                   uniform_diff_next_at.load(std::memory_order_relaxed)) {
            {
                std::scoped_lock lk(uniform_diff_mutex);
                uniform_diff_real.clear();
                uniform_diff_doubled.clear();
            }
            uniform_diff_seen_real.clear();
            uniform_diff_seen_doubled.clear();
            // ⚠ THE BUG THAT MADE SAMPLES 1-3 EMPTY (found 2026-08-21 in the
            // 14:30 and 14:41 logs, before this run was built).
            //
            //   sample 0: recordsReal=118 recordsDoubled=118
            //   sample 1: recordsReal=0   recordsDoubled=0
            //   sample 2: recordsReal=0   recordsDoubled=0
            //   sample 3: recordsReal=0   recordsDoubled=0
            //
            // repair_seen_binds is the PRE-LOCK dedupe filter in
            // hook_cmd_bind_descriptor_sets. It is per-FRAME by design, but it
            // was only ever cleared inside the `if (kRepairDoubledConstants
            // && ...)` block above - and that constant is false. So after the
            // first captured frame it held every key it had already seen, and
            // every later sample was filtered out before it could take the
            // lock: three of the four samples silently measured nothing.
            //
            // That is fatal for an A/B specifically: the OFF arm is samples 2
            // and 3, so without this line the entire second arm would have
            // come back empty and looked exactly like "no change". Clearing it
            // here - at the moment a sample is armed, which is once per
            // captured frame - is the reset the filter always needed.
            repair_seen_binds.clear();
            uniform_diff_records_real.store(0, std::memory_order_relaxed);
            uniform_diff_records_doubled.store(0, std::memory_order_relaxed);
            // Latch the arm and the suppression counter for the frame about to
            // be captured, so the emitted table describes the state that was
            // actually in force while it was recorded.
            light_cull_sample_arm_on.store(
                suppress_doubled_light_culling.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            light_cull_suppressed_mark.store(
                suppressed_light_culling_dispatches.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            uniform_diff_capturing.store(true, std::memory_order_relaxed);
        }
    }
    // Effects-stage auto series - see kEffectsSeries. Same shape: a due time,
    // a fresh stage list, a distinct filename suffix per sample.
    if (const uint32_t at = effects_series_rearm_at.load(std::memory_order_relaxed);
        at != 0 && presented_frame.load(std::memory_order_relaxed) >= at &&
        graded_stereo_enabled.load(std::memory_order_relaxed)) {
        effects_series_rearm_at.store(0, std::memory_order_relaxed);
        effects_stage_light_color = VK_NULL_HANDLE;
        effects_stage_xparency_color = VK_NULL_HANDLE;
        effects_stage_count = 0;
        real_stage_count = 0;
        deferred_hdr_input_real = VK_NULL_HANDLE;
        deferred_hdr_input_mirror = VK_NULL_HANDLE;
        deferred_hdr_input_extent = {};
        effects_stage_dump_ready.store(false, std::memory_order_relaxed);
        const uint32_t idx = effects_capture_index.fetch_add(1, std::memory_order_relaxed);
        effects_capture_suffix.store(idx, std::memory_order_relaxed);
        effects_stage_dump_armed.store(true, std::memory_order_relaxed);
        const uint32_t n = effects_series_taken.fetch_add(1, std::memory_order_relaxed);
        if (n + 1 < kEffectsSeries) {
            effects_series_rearm_at.store(
                presented_frame.load(std::memory_order_relaxed) + kEffectsSeriesGapFrames,
                std::memory_order_relaxed);
        }
        log::warn(std::format(
            "EffectsStage AUTO sample {}/{} armed (suffix {}). No keypress - the F2-only version "
            "landed on a frame with DOOM's own p22 at px>200=0, and an empty control compares to "
            "nothing.", n + 1, kEffectsSeries, idx));
    }
    // FINAL STAGE snapshot, taken HERE and not in the hook: this runs after
    // DOOM has submitted, so the doubled pass has actually executed. See the
    // comment at the hook for why dumping there returned all zeroes.
    if (final_pass_dump_ready.load(std::memory_order_acquire) &&
        final_pass_dump_pending.exchange(false, std::memory_order_relaxed)) {
        final_pass_dump_ready.store(false, std::memory_order_relaxed);
        const uint32_t series = final_pass_dump_series.fetch_add(1, std::memory_order_relaxed);
        if (series + 1 < kFinalPassDumpSeries) {
            final_pass_dump_rearm_at.store(
                presented_frame.load(std::memory_order_relaxed) + kFinalPassDumpGapFrames,
                std::memory_order_relaxed);
        }
        h1_pending.store(true, std::memory_order_relaxed);
        VkImageLayout have = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        {
            std::scoped_lock lock(mirror_mutex);
            have = mirror_layout_of(final_pass_mirror_color);
        }
        log::warn(std::format(
            "FinalStage SNAPSHOT series={} suppressLightCulling={}: dumping mirrorColour={} "
            "{}x{} fmt={} from trackedLayout={}. "
            "Taken on the present thread, after DOOM's submit - dumping inside the hook "
            "returned all zeroes because the pass had only been RECORDED, not executed. The arm "
            "label is read here, BEFORE the A/B switch below runs, so it is the state that was "
            "in force while this frame rendered - finalstage_eye1_graded_{}.ppm and its "
            "finalstage_eye0_control_{}.ppm belong to that arm.",
            series, suppress_doubled_light_culling.load(std::memory_order_relaxed) ? "ON" : "OFF",
            reinterpret_cast<void*>(final_pass_mirror_color), final_pass_mirror_extent.width,
            final_pass_mirror_extent.height, static_cast<int>(final_pass_mirror_format),
            static_cast<int>(have), series, series));
        dump_image_snapshot(final_pass_mirror_color, final_pass_mirror_extent,
                            final_pass_mirror_format,
                            std::format("finalstage_eye1_graded_{}", series).c_str(), have);
        // finalstage_eye0_reference RETIRED FROM THE SERIES (2026-08-21).
        //
        // It dumped composite_holding - the real call's PRE-final, UNGRADED
        // composite - against a POST-grade eye 1. The block below already
        // explains at length why that comparison cannot settle "are the
        // effects missing": a contrast grade crushes low-contrast haze and
        // moves every bright value, so the difference is tonal, not content.
        // That confound already wasted one measurement.
        //
        // Keeping it cost a third of the run's dump budget - 23 MB and a queue
        // drain per sample - to produce an artifact whose own comment says not
        // to draw conclusions from it, and at 8 samples that is 8 extra
        // full-resolution writes and 8 extra hitches for the owner. The
        // like-for-like control below is the one that decides; this is the one
        // that misleads. Restore it only for a question about the PRE-grade
        // stage specifically, and then compare it against a pre-grade eye 1.
        // LIKE-FOR-LIKE eye-0 dump (2026-08-20). THE ONE THAT DECIDES.
        //
        // finalstage_eye0_reference above is composite_holding - the REAL
        // call's PRE-final, UNGRADED composite. Comparing it against the
        // POST-grade eye-1 image cannot settle "are the effects missing":
        // a contrast grade crushes low-contrast haze and moves every bright
        // value, so fog and fire can appear to vanish for a reason that is
        // purely tonal. That confound wasted a measurement this session -
        // the eye-1 image looked clear next to a hazy eye-0 reference, and
        // the difference was not attributable.
        //
        // DOOM's own PRESENTED swapchain image is the correct control: it is
        // the SAME pipeline stage as final_pass_mirror_color (post-grade,
        // post-HUD, same format 44), the same frame, and it is by definition
        // the picture with the effects in it. Any difference between these
        // two is content, not tone.
        //
        // Resolved from `info` the same way the presentation path resolves it
        // further down; `source` is not computed until later in this function.
        // Layout is PRESENT_SRC_KHR - that is what DOOM leaves the swapchain
        // image in before presenting it (measured, doom-capture.rdc barrier
        // at eid 11710), and naming a wrong one here would discard it.
        if (info && info->swapchainCount && info->pSwapchains && info->pImageIndices) {
            VkImage presented = VK_NULL_HANDLE;
            VkExtent2D presented_extent{};
            VkFormat presented_format = VK_FORMAT_UNDEFINED;
            {
                std::unique_lock lock(swapchains_mutex);
                auto found = swapchains.find(info->pSwapchains[0]);
                if (found != swapchains.end() &&
                    info->pImageIndices[0] < found->second.images.size()) {
                    presented = found->second.images[info->pImageIndices[0]];
                    presented_extent = found->second.extent;
                    presented_format = found->second.format;
                }
            }
            if (presented != VK_NULL_HANDLE) {
                log::warn(std::format(
                    "FinalStage CONTROL: dumping DOOM's own presented image={} {}x{} fmt={} "
                    "from PRESENT_SRC_KHR. Same stage and same frame as eye 1, so "
                    "finalstage_eye0_control vs finalstage_eye1_graded is a CONTENT "
                    "comparison with no tone-curve confound. Effects present in the control "
                    "and absent in eye 1 puts the defect in what the doubled call RENDERS.",
                    reinterpret_cast<void*>(presented), presented_extent.width,
                    presented_extent.height, static_cast<int>(presented_format)));
                dump_image_snapshot(presented, presented_extent, presented_format,
                                    std::format("finalstage_eye0_control_{}", series).c_str(),
                                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
            } else {
                log::error("FinalStage CONTROL: could not resolve DOOM's presented image from "
                           "the present info, so NO control was written. Do not read its "
                           "absence as the control having been empty.");
            }
        }
        log::warn(std::format(
            "FinalStage REDIRECT AUDIT: substitutions inside the doubled final pass = {}. ZERO "
            "means it graded DOOM's OWN composite, so the right eye is a graded copy of the "
            "LEFT eye's viewpoint - effects restored, parallax destroyed. Non-zero means it "
            "read eye 1's own composite and the pair is genuinely stereo.",
            final_pass_redirects.load(std::memory_order_relaxed)));
    }
    // ---- LIGHT-CULLING A/B: the arm switch, and it lives HERE deliberately.
    //
    // Both instrument blocks above have already emitted for the frame that was
    // just rendered, so flipping now cannot produce an artifact that straddles
    // the two arms: everything labelled ON was recorded with it on, everything
    // labelled OFF with it off.
    //
    // Driven by the PRIMARY instrument's sample count, not by a frame number.
    // A fixed frame threshold would split the two series differently - the PPM
    // pairs and the uniform samples are not phase-locked to the same frame
    // exactly - and one instrument reporting a different arm from the other is
    // precisely the mislabelling this run cannot afford.
    if (light_cull_ab_active.load(std::memory_order_relaxed) &&
        uniform_diff_reports.load(std::memory_order_relaxed) >= kLightCullAbOnSamples &&
        suppress_doubled_light_culling.load(std::memory_order_relaxed)) {
        suppress_doubled_light_culling.store(false, std::memory_order_relaxed);
        light_cull_ab_flips.fetch_add(1, std::memory_order_relaxed);
        log::warn(std::format(
            "LightCullAB: SWITCHING TO ARM B after {} samples - suppress_doubled_light_culling "
            "is now OFF, set in code, no keypress involved. Samples 0-{} were taken with it ON "
            "(DOOM's own light lists survive the frame, the doubled call's culling never runs); "
            "samples {}+ are taken with it OFF (the doubled call runs its own culling and writes "
            "DOOM's shared light-list buffers). suppressedDispatches so far={} - that counter "
            "must now STOP climbing, and LightBlocks' suppressedDispatchesThisSample must read 0 "
            "for every remaining sample. If it keeps climbing, the flip did not take and no "
            "conclusion may be drawn from arm B. finalStageSeries={} at the moment of the flip - "
            "PPM pairs at or after that index belong to arm B. EXPECTED SIDE EFFECT: eye 0's "
            "sparks degrade in arm B; that is the cost suppression was paying and is not a "
            "fault.",
            kLightCullAbOnSamples, kLightCullAbOnSamples - 1, kLightCullAbOnSamples,
            suppressed_light_culling_dispatches.load(std::memory_order_relaxed),
            final_pass_dump_series.load(std::memory_order_relaxed)));
    }
    // X1 SUMMARY, on the same present as the final-stage snapshot so the arm
    // state and the picture it produced are next to each other in the log.
    if (x1_replay_ring_offsets.load(std::memory_order_relaxed)) {
        static std::atomic_uint32_t x1_summary_lines{0};
        if (x1_summary_lines.fetch_add(1, std::memory_order_relaxed) % 300 == 0) {
            log::warn(std::format(
                "X1 Summary: replayed={} missing={} countMismatch={}. replayed=0 means the lever "
                "NEVER FIRED and nothing below it may be read as a test of it. missing counts "
                "doubled binds with no matching real bind for that (pass, bindPoint, ordinal) - "
                "some is expected where the two calls' bind order differs, a lot means the key "
                "is not aligning and the replay is partial.",
                x1_replayed.load(std::memory_order_relaxed),
                x1_missing.load(std::memory_order_relaxed),
                x1_count_mismatch.load(std::memory_order_relaxed)));
        }
    }
    // R1 DISPATCH FATE. Disarmed and emitted from the PRESENT thread, one
    // frame-root serial after the capture, and deliberately AHEAD of
    // h1_run_pair_census: H1 freezes the present thread for seconds while it
    // reads back 26 targets, and a manifest that emitted after it would be
    // separated from its own arming line by thousands of lines of census. The
    // frame latch is what makes "one frame" true regardless of when this runs.
    if (df_armed.load(std::memory_order_relaxed) &&
        df_frame_latched.load(std::memory_order_relaxed) != 0 &&
        g_frame_root_serial.load(std::memory_order_relaxed) >
            df_frame_latched.load(std::memory_order_relaxed)) {
        df_armed.store(false, std::memory_order_relaxed);
        df_emit_manifest("one frame captured, self-disarmed");
    }
    h1_run_pair_census();
#endif
    auto& state = vulkan_state();
    state.queue = queue;
#ifdef DOOMVR_RESEARCH_TOOLS
    presented_frame.fetch_add(1, std::memory_order_relaxed);
    {
        std::unique_lock lock(swapchains_mutex);
        modified_view_records.clear();
        sampled_camera_pipeline_bindings.clear();
    }
    // CLONE RETIREMENT DRAIN (2026-08-24). Placed here, once per present, with
    // NO lock held: the frame counter has just advanced, so anything retired
    // kCloneRetireFrames ago is provably past the GPU. Freeing a descriptor set
    // still in flight is a device-lost hang, which is why this is deferred
    // rather than done at the drop site.
    drain_retired_clones();
    // Sampled ONCE - the &1 bit is cleared by the read. Same pattern as
    // SHIFT+F4 / F5 / F6 above. Shift is read locally rather than reused from
    // shift_held_now: that local is declared FURTHER DOWN this function, and
    // 0x8000 consumes nothing, so a second read here cannot starve it.
    const bool f8_shift_held = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool f8_pressed = (GetAsyncKeyState(VK_F8) & 1) != 0;
    if (f8_pressed && f8_shift_held) {
        // SHIFT+F8: UNIFORM DIFF, ARMED ALONE (2026-08-29).
        //
        // THE LAST INPUT DIMENSION NOT YET COMPARED ON A CLEAN BASELINE.
        //
        // Everything structural is now measured identical between the real and
        // doubled calls: 69/69 passes, 1322/1322 draws, 11/11 indirect,
        // identical index counts, exactly ONE divergent pass whose only
        // difference is the 11 suppressed dispatches (A/B'd, no effect), all 31
        // recorded IMAGE bindings identical including both froxel volumes, depth
        // eye-specific and correct, and at 0.0 separation the cameras are
        // identical too - yet eye 1 still loses the volumetric layer.
        //
        // But "31 image bindings identical" is NOT "all inputs identical", and
        // saying so would be the same overreach this project has already paid
        // for. DeferredInputMap compares VIEWS and IMAGES. It does not compare
        // BUFFER CONTENTS, and DOOM ring-allocates a separate constant slot per
        // call - so the two calls can hold the same descriptor handle and read
        // DIFFERENT BYTES through a different dynamic offset. That is the one
        // place a difference can still hide, and UniformDiff is the instrument
        // built for exactly it.
        //
        // Armed ALONE rather than through step 35's battery: no census, no P1,
        // no PPM series. Four samples, ~150 presents apart, self-disarming.
        // Press it standing in the fog, where the deficit is visible.
        uniform_diff_reports.store(0, std::memory_order_relaxed);
        uniform_diff_next_at.store(presented_frame.load(std::memory_order_relaxed) + 1,
                                   std::memory_order_relaxed);
        uniform_diff_armed.store(true, std::memory_order_relaxed);
        vwc_arm("uniform diff armed alone");
        log::warn(std::format(
            "SHIFT+F8: UniformDiff ARMED ALONE - 4 samples, {} presents apart, self-disarming. "
            "Compares the CONTENTS of every uniform buffer the two calls read, not their "
            "handles. Grep 'UniformDiff' for the table.\n"
            "READING IT: 'different' counts buffers whose BYTES differ between the real and the "
            "doubled call. different=0 with a healthy recordsReal/recordsDoubled means the two "
            "calls are fed byte-identical constants and the divergence is NOT in what they are "
            "told - which would corner the last remaining family, consume-once state that the "
            "descriptors point at but the real call has already spent.\n"
            "⚠ THE CONTROL: recordsReal=0 or recordsDoubled=0 means NOTHING was captured for "
            "that side and the sample proves nothing in either direction. Sample 0 is the one to "
            "trust most - a stale repair_seen_binds filter emptied samples 1-3 once before.",
            kUniformDiffGapFrames));
    } else if (f8_pressed) {
        // Lever G v5 (2026-08-11): single-slot cycle. Each press advances
        // lever_g_slot_index by 1. -1 = OFF, else clobber only slot[i]
        // with a fixed 90-degree yaw. After the last slot wraps back to
        // -1. First armed press populates lever_g_slots by scanning
        // ctx28 and ctx30 (see apply_lever_g_mutation). Log each press so
        // we can correlate the on-screen result to the specific offset
        // being tested. Additive with F5/F12.
        stereo_preview_enabled.store(false, std::memory_order_relaxed);
        render_opaque_double_call_enabled.store(false, std::memory_order_relaxed);
        redirect_extra_call_enabled.store(false, std::memory_order_relaxed);
        render_view_clone_test_enabled.store(false, std::memory_order_relaxed);
        true_stereo_enabled.store(false, std::memory_order_relaxed);
        upstream_reinvoke_enabled.store(false, std::memory_order_relaxed);
        widen_gather_probe_enabled.store(false, std::memory_order_relaxed);
        scoped_reinvoke_enabled.store(false, std::memory_order_relaxed);
        int cur = lever_g_slot_index.load(std::memory_order_relaxed);
        int next = cur + 1;
        // If the cache is populated, wrap after the last real slot.
        // Otherwise the first press advances -1 -> 0; the populate step
        // runs on the first mutation call after that.
        {
            std::scoped_lock lock(lever_g_slots_mutex);
            const int n = static_cast<int>(lever_g_slots.size());
            if (n > 0 && next >= n) next = -1;
        }
        lever_g_slot_index.store(next, std::memory_order_relaxed);
        if (cur < 0 && next >= 0) {
            // Fresh arm: enable the probe + reset cache so populate runs.
            lever_g_probe_pending.store(true, std::memory_order_relaxed);
            lever_g_slots_populated.store(false, std::memory_order_release);
        } else if (next < 0) {
            // Cycled off: invalidate cache for the next arm session.
            lever_g_slots_populated.store(false, std::memory_order_release);
        }
        // Log which slot is now active. If the cache has been populated
        // already, include the specific (ctx, offset, kind) triple.
        std::string slot_desc = (next < 0) ? "OFF" : std::format("slot {}", next);
        {
            std::scoped_lock lock(lever_g_slots_mutex);
            if (next >= 0 && static_cast<size_t>(next) < lever_g_slots.size()) {
                const auto& s = lever_g_slots[next];
                slot_desc = std::format("slot {}/{} ({} offset={:#06x} kind={} addr={:#x})",
                    next, lever_g_slots.size(), s.ctx_label, s.offset, s.kind,
                    s.base + s.offset);
            } else if (next >= 0) {
                slot_desc = std::format("slot {} (cache empty; will populate on first RenderGuiModels)", next);
            }
        }
        log::warn(std::format(
            "F8: Lever G v5 (single-slot yaw-90) -> {}. "
            "Additive with F5/F12. Watch the SCREEN. If it moves, this offset drives the weapon. "
            "If nothing moves, next press advances to the next slot.",
            slot_desc));
    }
    // Shift read locally - shift_held_now is declared further down this
    // function, and 0x8000 consumes nothing. Same as SHIFT+F8 above.
    const bool f10_shift_held = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool f10_pressed = (GetAsyncKeyState(VK_F10) & 1) != 0;
    if (f10_pressed && f10_shift_held) {
        // SHIFT+F10: DOUBLED-CALL CONSTANT REPAIR, THREE RUNGS.
        //
        // Cycles 0 OFF -> 2 TILES -> 3 REPROJECTION -> 1 WHOLE -> 0. Surgical comes FIRST
        // because whole-block is already tested: on 2026-08-29 it engaged
        // visibly (the viewmodel hand vanished) and moved the effects not at
        // all. The hand is the diagnosis, not a side effect - a whole-block
        // copy keyed by (pass,set,binding) writes the real call's data over a
        // DIFFERENT OBJECT's slot whenever DOOM's ring allocator has moved on,
        // which is the same failure X1 hit. It repairs some floats and corrupts
        // others, so its null is not clean.
        //
        // Surgical writes only the floats the 0.0-separation dump names, keyed
        // by binding and block size rather than by pass number:
        //   16 B binding 0        -> f0,f1   ({0, +/-2048, +/-4096, +/-6144})
        //   4096 B binding 9/13   -> f18,f19,f38,f39  ({0.625,0.750,0.875})
        // Both are shadow-atlas tile allocation - 16384 = 8 x 2048, and eighths
        // are that same index normalised - so this tests ONE claim: the doubled
        // call is handed different atlas tiles than the real call.
        const uint32_t cur = constant_repair_mode.load(std::memory_order_relaxed);
        // MODE 4 FIRST (2026-08-23). Modes 2 and 3 are spent - both were run,
        // both came back negative, and their notes above say so. Mode 4 is the
        // fog reconstruction repair and is the only rung with a confirmed
        // defect behind it, so it is what one press reaches. The others stay in
        // the cycle rather than being deleted, because "clean in this test" is
        // not "eliminated" and a future question may want them back.
        // A PLAIN TOGGLE, 0 <-> 4 (2026-08-23, second revision).
        //
        // The five-position cycle cost a whole headset run. The owner pressed
        // SHIFT+F10 looking for a change, cycled 4 -> 2 -> 3 -> 1 -> 0, and
        // judged "no improvement" while the log shows mode 4 was off by then.
        // Four of the five positions are spent experiments whose own notes say
        // they came back negative, and none of them is legible from inside a
        // headset.
        //
        // Modes 1-3 stay implemented and reachable from code - "clean in this
        // test" is not "eliminated" - but they are off this key.
        const uint32_t next = cur == 4 ? 0 : 4;
        constant_repair_mode.store(next, std::memory_order_relaxed);
        // Mode 4 takes its OWN flag and leaves kRepairDoubledConstants off.
        // Turning that on drags in the whole uniform-diff walk - a copy, a hash
        // and a map insert per block per frame - which is what made the game
        // crawl the first time mode 4 was pressed.
        kFogRepairOnly.store(next == 4, std::memory_order_relaxed);
        kRepairDoubledConstants.store(next != 0 && next != 4, std::memory_order_relaxed);
        repair_seen_binds.clear();
        repair_floats_written.store(0, std::memory_order_relaxed);
        repair_blocks_touched.store(0, std::memory_order_relaxed);
        repair_log_lines.store(0, std::memory_order_relaxed);
        static const char* kMode[] = {
            "0 OFF - no repair. The reference.",
            "1 WHOLE-BLOCK - copies every large block entire. ALREADY TESTED 2026-08-29: engaged "
            "(the viewmodel hand vanished, which is mis-keyed per-object data being overwritten) "
            "and changed the effects not at all. Kept only as the A/B.",
            "2 SURGICAL TILES - writes ONLY the shadow-atlas tile floats: f0,f1 "
            "of 16-byte binding-0 blocks and f18,f19,f38,f39 of 4096-byte binding-9/13 blocks. "
            "Nothing else is touched, so the viewmodel must NOT break this time - if the hand "
            "still vanishes, the whitelist is wrong and the run is void. TESTED 2026-08-29 at a steady "
            "+660 floats/180 presents: shadows visibly flashed, effects unchanged - engaged and "
            "negative.",
            "3 REPROJECTION MATRIX (THE STATE UNDER TEST) - writes ONLY floats 36..51 of the "
            "208-byte binding-0 block: a 4x4 matrix whose xyz terms differ by ~1e-4 between two "
            "calls rendering the IDENTICAL camera. Volumetric fog is reprojected, so a wrong "
            "reprojection term is the last constant that could plausibly erase it. Tiles are NOT "
            "touched in this rung, so mode 2's negative stays reproducible. ⚠ ITS OWN CONTROL: "
            "if eye 1's PARALLAX VISIBLY FLATTENS toward eye 0, this is the per-pass camera and "
            "the rung is VOID for the fog question - say so and stop.",
            "4 FOG RECONSTRUCTION - the only rung with a confirmed defect behind it. The "
            "deferred composite's freqLow_computeUniforms (256 B) reconstructs a world position "
            "per pixel and fogs by its distance from globalvieworigin. Both inputs are EYE 0's: "
            "eye 1 gets its own ring allocation (different byte offset) filled with byte-"
            "identical contents, and R19 showed those contents do not move when eye 0's camera "
            "moves 6 units. So each eye fogs by distance from the WRONG camera, in OPPOSITE "
            "directions - the fog's stereo disparity is inverted relative to the geometry, which "
            "is why the ceiling windows read as contradictory depth. This adds the world camera "
            "displacement to wpg_x/y/z (scaled by wpg_w, because the shader divides by w) and to "
            "globalvieworigin. EYE 1 ONLY - eye 0 is wrong the same way and is left alone so the "
            "A/B has one variable. ⚠ WATCH THE WINDOWS AND ANY DEEP HAZY VIEW; also confirm eye "
            "0 is UNCHANGED, which it must be by construction."};
        vwc_arm(std::format("constant repair mode {}", next).c_str());
        log::warn(std::format(
            "SHIFT+F10: constant repair -> mode {} ({}). Counters zeroed, so 'ConstantRepair' "
            "lines and floatsWritten below describe THIS rung.\n"
            "⚠ ENGAGEMENT CHECK: floatsWritten must be > 0. Mode 2 writing ZERO floats means the "
            "binding/size signature never matched and the rung is VOID, not negative - it would "
            "look exactly like a repair that did nothing.\n"
            "⚠ THE FRAME RATE WARNING FROM THE LAST RUN WAS WRONG. The pre-lock filter added "
            "after the 2026-08-20 note already fixed that cost; no slow motion is expected, and "
            "its absence is not evidence the repair is inactive.\n"
            "WATCH: the haze and the god rays in the canyon, and whether the viewmodel hand "
            "stays put.",
            next, kMode[next]));
    } else if (f10_pressed) {
        // WorldVpTrace (2026-08-11): arms one burst of log_world_vp_at()
        // calls covering all instrumented hook points. Each hook logs the
        // current renderView+0xC44 value plus its own name; the sequence
        // shows WHICH upstream hook (if any) precedes a change in the
        // value. That's the writer we need to hook for a coherent world
        // Lever V equivalent. Budget of 40 lines = ~2 frames of hook fires
        // (SetupView + gather chain + RenderOpaque + RenderGuiModels).
        world_vp_trace_remaining.store(kMaxWorldVpTraceLines, std::memory_order_relaxed);
        log::warn(std::format(
            "F10: WorldVpTrace armed with budget {}. Grep 'WorldVpTrace' after ~1s.",
            kMaxWorldVpTraceLines));
    }
    // F9 IS POLLED TWICE IN THIS FUNCTION - sample it ONCE.
    //
    // GetAsyncKeyState's &1 bit is "pressed since the last query" and reading
    // it CLEARS it. The Lever-G handler below and the DepthExperiment handler
    // further down both wanted F9, and because this one runs first it consumed
    // every press: the experiment's branch was unreachable and its first run
    // produced not one log line. An unreachable probe is indistinguishable
    // from a broken one, which cost a headset run.
    //
    // One sample, shared, with SHIFT discriminating. Any future F9 consumer
    // must use these locals rather than call GetAsyncKeyState again.
    const bool f9_pressed = (GetAsyncKeyState(VK_F9) & 1) != 0;
    const bool shift_held_now = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (f9_pressed && !shift_held_now) {
        // Reclaimed 2026-08-11 from the retired UpstreamPipelineReinvoke.
        // Lever G v5 fast-advance: jump forward by 10 slots. Same shape
        // as F8 but +10 per press so we can skip through boring stretches.
        // Wraps to -1 (OFF) after the last cached slot the same way F8 does.
        int cur = lever_g_slot_index.load(std::memory_order_relaxed);
        int next = (cur < 0) ? 10 : cur + 10;
        {
            std::scoped_lock lock(lever_g_slots_mutex);
            const int n = static_cast<int>(lever_g_slots.size());
            if (n > 0 && next >= n) next = -1;
        }
        lever_g_slot_index.store(next, std::memory_order_relaxed);
        if (cur < 0 && next >= 0) {
            lever_g_probe_pending.store(true, std::memory_order_relaxed);
            lever_g_slots_populated.store(false, std::memory_order_release);
        } else if (next < 0) {
            lever_g_slots_populated.store(false, std::memory_order_release);
        }
        std::string slot_desc = (next < 0) ? "OFF" : std::format("slot {}", next);
        {
            std::scoped_lock lock(lever_g_slots_mutex);
            if (next >= 0 && static_cast<size_t>(next) < lever_g_slots.size()) {
                const auto& s = lever_g_slots[next];
                slot_desc = std::format("slot {}/{} ({} offset={:#06x} kind={} addr={:#x})",
                    next, lever_g_slots.size(), s.ctx_label, s.offset, s.kind,
                    s.base + s.offset);
            }
        }
        log::warn(std::format(
            "F9: Lever G v5 (+10 slots) -> {}. F8 = +1, F9 = +10.",
            slot_desc));
    }
    const bool f7_pressed = (GetAsyncKeyState(VK_F7) & 1) != 0;
    if (f7_pressed && shift_held_now) {
        // SHIFT+F7: SHADOW-ATLAS SUPPRESSION, LIVE A/B.
        //
        // suppress_doubled_shadow_draws has been FORCED ON at arm time since
        // 2026-08-20 and has never been toggled against the symptom it was
        // built for. The census now shows it doing real work - over 2.3 million
        // draws blocked in one session, ~44 per frame in steady state - so the
        // doubled call is asking to redraw DOOM's shared 16384x16384 atlas every
        // single frame and being stopped.
        //
        // Which raises the question nobody has tested: is stopping it the right
        // move? Suppressing DRAWS does not suppress the RENDER PASS. If the
        // doubled call's shadow pass begins with a clear, the atlas is wiped and
        // then not refilled, and DOOM's next frame samples shadows that are
        // partly missing - which would present as shadows that lag and slide
        // against geometry as the player moves. That is S3's description.
        //
        // Both arms are informative, which is why this is worth a key:
        //   sliding STOPS with suppression OFF -> the suppression is causing it,
        //       and the fix is to stop the doubled call entering the shadow pass
        //       at all rather than to block its draws mid-pass.
        //   sliding UNCHANGED -> the atlas is cleared here, and S3 stays with
        //       the second frame-root entry.
        // Eye 1's shadows may change either way; judge EYE 0.
        const bool on = !suppress_doubled_shadow_draws.load(std::memory_order_relaxed);
        suppress_doubled_shadow_draws.store(on, std::memory_order_relaxed);
        vwc_arm(on ? "shadow suppression ON" : "shadow suppression OFF");
        log::warn(std::format(
            "SHIFT+F7: shadow-atlas suppression is now {} (suppressedShadowDrawsTotal={} so "
            "far). {} Walk around and judge EYE 0's shadows. The census header reports the "
            "total every frame - with suppression OFF it must STOP climbing, which is how you "
            "know the toggle engaged rather than the symptom being insensitive.",
            on ? "ON" : "OFF",
            suppressed_shadow_draws.load(std::memory_order_relaxed),
            on ? "The doubled call is blocked from drawing into DOOM's shared shadow atlas "
                 "again."
               : "The doubled call may now draw into DOOM's shared shadow atlas - the state "
                 "this build has never been run in since the suppression was forced on."));
    } else if (f7_pressed) {
        const bool now_enabled = !render_opaque_double_call_enabled.load(std::memory_order_relaxed);
        if (now_enabled) {
            render_opaque_double_call_count.store(0, std::memory_order_relaxed);
            // Also reset the render-pass-begin logger's cap: normal gameplay
            // before F7 is pressed can easily exhaust its budget, leaving
            // nothing left to log the actually-interesting double-call
            // frames.
            render_pass_begin_sample_count.store(0, std::memory_order_relaxed);
            // Mutual exclusion with StereoPreview (F3), RenderViewClone
            // (F2), and TrueStereo (F1): never stack a second independent
            // extra-call mechanism onto the same frame.
            stereo_preview_enabled.store(false, std::memory_order_relaxed);
            render_view_clone_test_enabled.store(false, std::memory_order_relaxed);
            true_stereo_enabled.store(false, std::memory_order_relaxed);
            upstream_reinvoke_enabled.store(false, std::memory_order_relaxed);
            widen_gather_probe_enabled.store(false, std::memory_order_relaxed);
            stereo_dual_eye_enabled.store(false, std::memory_order_relaxed);
            scoped_reinvoke_enabled.store(false, std::memory_order_relaxed);
        }
        render_opaque_double_call_enabled.store(now_enabled, std::memory_order_relaxed);
        log::warn(now_enabled
            ? "F7: RenderOpaque double-call smoke test ARMED (re-entrancy test, watch for visual glitches/crash)"
            : "F7: RenderOpaque double-call smoke test disabled");
    }
    // Sampled ONCE - reading the &1 bit CLEARS it, so testing it in both
    // branch conditions would let the SHIFT test swallow a plain press. Same
    // trap already paid for on F9 and on PageUp.
    const bool f4_pressed = (GetAsyncKeyState(VK_F4) & 1) != 0;
    if (f4_pressed && shift_held_now) {
        // SHIFT+F4: THE GRADE-GAP LADDER, LIVE (2026-08-29).
        //
        // This exists because of the falsifier the grade step carries: the
        // doubled final pass is a SECOND ENTRY into DOOM's rendering, and a
        // second frame-root entry is already known to disturb DOOM's next
        // frame even with jobs, gather, SetupView, the offset, the composite
        // capture and the layout barriers all suppressed. If the same is true
        // here, EYE 0 - the confirmed reference - regresses, and that is a
        // REGRESSION, NOT A TRADE. Eye 0 outranks eye 1's grade.
        //
        // Three rungs rather than an on/off, because on/off cannot say WHY:
        //   0 OFF          the confirmed eye-0 config. ROLLBACK, and the
        //                  ladder's POSITIVE CONTROL - eye 0 must be clean
        //                  here or the run is void rather than informative.
        //   1 DOUBLE ONLY  the second invocation runs; eye 1 is still shown
        //                  the ungraded mirror. Eye 1 should be
        //                  indistinguishable from rung 0.
        //   2 GRADED       the state under test.
        //
        // So: eye 0 damaged at rung 1 => the cost is the SECOND INVOCATION,
        // and no amount of presentation work can fix it. Eye 0 clean at rung 1
        // and damaged at rung 2 => the fault is in presentation/layout, which
        // is fixable. Eye 0 clean at both => the grade is free and the run
        // moves on to judging eye 1.
        //
        // IS CYCLING SAFE? Checked, because "GPU-state toggles are one-way" is
        // a lesson this project has already paid for - suspending layout
        // bookkeeping while transitions keep happening leaves a tracked layout
        // that lies, and a barrier whose oldLayout lies is permitted to
        // DISCARD the image. final_pass_double_armed gates the MirrorLockstep
        // append, which is exactly that kind of bookkeeping. It is safe here
        // for one specific reason: that flag gates the BARRIER and the
        // mirror_image_layouts write at the SAME site, so rung 0 suspends both
        // together. Our mirrors are our own images; nothing else transitions
        // them, so at rung 0 the tracked layout stays TRUE rather than going
        // stale, and rung 1 resumes onto a correct oldLayout. If a future
        // change ever separates those two writes, this key stops being
        // cyclable and has to become one-way.
        //
        // Order is 2 -> 0 -> 1 -> 2 deliberately: the run arms at 2, so the
        // FIRST press from a picture that looks wrong lands on the rollback.
        const uint32_t next = (graded_mode.load(std::memory_order_relaxed) + 1) % 3;
        apply_graded_mode(next, "SHIFT+F4 ladder");
        log::warn(std::format(
            "SHIFT+F4: grade-gap ladder -> rung {}. VERIFY FROM THE INSTRUMENT, NOT FROM THIS "
            "PRESS: the next 'GradedMode' line reports the forced state and the next 8 "
            "'BaselinePair' lines report the pair that is actually being submitted, with the "
            "rung stamped on each. JUDGE EYE 0 FIRST at every rung.", next));
    } else if (f4_pressed) {
        // ScopedPipelineReinvoke - see its declaration above. F4 was free
        // (only double-bound to DOOM's own quick-item-slot 4, same as
        // F1-F3, no observed ill effect per docs/HANDOFF-NEXT-SESSION.md).
        const bool now_enabled = !scoped_reinvoke_enabled.load(std::memory_order_relaxed);
        if (now_enabled) {
            scoped_reinvoke_attempt_count.store(0, std::memory_order_relaxed);
            scoped_reinvoke_frame_counter.store(0, std::memory_order_relaxed);
            // Mutual exclusion: never stack a second independent extra-call
            // mechanism onto the same frame.
            stereo_preview_enabled.store(false, std::memory_order_relaxed);
            render_opaque_double_call_enabled.store(false, std::memory_order_relaxed);
            redirect_extra_call_enabled.store(false, std::memory_order_relaxed);
            render_view_clone_test_enabled.store(false, std::memory_order_relaxed);
            true_stereo_enabled.store(false, std::memory_order_relaxed);
            upstream_reinvoke_enabled.store(false, std::memory_order_relaxed);
            widen_gather_probe_enabled.store(false, std::memory_order_relaxed);
            stereo_dual_eye_enabled.store(false, std::memory_order_relaxed);
        }
        scoped_reinvoke_enabled.store(now_enabled, std::memory_order_relaxed);
        log::warn(now_enabled
            ? std::format(
                "F4: ScopedPipelineReinvoke ARMED - re-runs ONLY RenderCull/RenderWalkBSP/"
                "RenderGatherFinish/RenderSort (skips all Gather*Prepare jobs, unlike F9's full "
                "10-job Stage A), then redraws into DOOM's real frame with the SAME unmutated camera "
                "- zero camera divergence, testing whether skipping Prepare is both SAFE (does "
                "WalkBSP even work without a fresh Prepare pass) and produces a COMPLETE scene, not "
                "just cheap. Watch for missing geometry as well as glitches/crash. Attempts spread "
                "{} real frames apart, cap {}.",
                kScopedReinvokeFrameGap, kMaxScopedReinvokeAttempts)
            : "F4: ScopedPipelineReinvoke disabled");
    }
    // Sampled ONCE - the &1 bit is cleared by the read, so testing it in both
    // branch conditions would let the SHIFT test swallow a plain press. Same
    // pattern as SHIFT+F4 above.
    const bool f5_pressed = (GetAsyncKeyState(VK_F5) & 1) != 0;
    if (f5_pressed && shift_held_now) {
        // SHIFT+F5: MONITOR EYE-1 VIEW - see monitor_eye1_direct.
        //
        // The workflow key. With this on, the doubled final pass keeps DOOM's
        // own framebuffer instead of taking a mirror, so it renders eye 1's
        // graded, HUD-carrying frame straight onto the monitor. Toggling it is
        // a like-for-like A/B of eye 0 against eye 1 on one screen, in the
        // same frame position, with no second colour path for a tone confound
        // to hide in - both images are produced by the identical final pass
        // into the identical target.
        const bool want = !monitor_eye1_direct.load(std::memory_order_relaxed);
        if (want && xr().ready()) {
            // EYE 0 OUTRANKS EYE 1, ENFORCED IN CODE RATHER THAN IN A COMMENT.
            // With the headset path live the presented frame IS eye 0, the
            // confirmed reference, and this mode overwrites it. A run that
            // silently corrupted the reference while the owner was wearing the
            // headset is exactly the failure the pre-flight was built to stop.
            log::error(std::format(
                "SHIFT+F5 REFUSED: the headset path is live (xrPresentPathHits={}), and this "
                "mode overwrites the presented frame - which in a headset run IS EYE 0, the "
                "confirmed reference. This is a FLAT-RUN instrument. Close the XR session and "
                "run on the monitor, or judge eye 1 in the headset as usual.",
                xr_present_path_hits.load(std::memory_order_relaxed)));
        } else if (want && graded_mode.load(std::memory_order_relaxed) == 0) {
            // Rule 3 in arming form: at rung 0 there is no doubled final pass
            // to redirect, so this would sit silent and look like a mode that
            // changed nothing about eye 1.
            log::error("SHIFT+F5 REFUSED: the grade ladder is at rung 0, so the final pass is "
                       "NOT being doubled and there is nothing to put on the monitor. Press "
                       "SHIFT+F4 until the rung is 1 or 2, then try again.");
        } else {
            monitor_eye1_direct.store(want, std::memory_order_relaxed);
            monitor_eye1_passes.store(0, std::memory_order_relaxed);
            vwc_arm(want ? "monitor eye-1 view ON" : "monitor eye-1 view OFF");
            log::warn(std::format(
                "SHIFT+F5: monitor eye-1 view is now {}. {} gradeRung={}.\n"
                "THE POSITIVE CONTROL IS BUILT IN AND YOU CAN SEE IT: eye 1 renders at +half "
                "separation, so THE VIEWPOINT MUST VISIBLY SHIFT when this is switched on. If "
                "the picture does NOT shift, the doubled pass graded DOOM's real composite "
                "instead of eye 1's and this mode is VOID - it is not evidence that the eyes "
                "match. Cross-check 'FinalPassSubst' and monitorEye1Passes below.",
                want ? "ON - THE MONITOR IS NOW SHOWING EYE 1" : "OFF - the monitor is back to "
                       "DOOM's own frame (eye 0)",
                want ? "Everything you see, including any missing fog/fire/sparks, is eye 1's "
                       "content at the same pipeline stage as eye 0."
                     : "This is the reference image.",
                graded_mode.load(std::memory_order_relaxed)));
        }
    } else if (f5_pressed) {
        // Unified stereo-offset cycle (2026-08-11). One knob shifts EVERY
        // layer of the scene: Lever A applies to renderView+0xC44/+0xC84
        // (world mid-range), Lever V applies to
        // *(SetupView_gui_arg1+0x18)+0x11F0 (weapon, HUD 3D, sky,
        // particles, near geometry - everything Lever A doesn't cover,
        // confirmed today). Cycles OFF -> +2uu -> +5uu -> +20uu ->
        // +100uu -> OFF so coverage is testable at multiple scales.
        // Extra-call mechanisms still disarmed for cleanness.
        stereo_preview_enabled.store(false, std::memory_order_relaxed);
        render_opaque_double_call_enabled.store(false, std::memory_order_relaxed);
        redirect_extra_call_enabled.store(false, std::memory_order_relaxed);
        render_view_clone_test_enabled.store(false, std::memory_order_relaxed);
        true_stereo_enabled.store(false, std::memory_order_relaxed);
        upstream_reinvoke_enabled.store(false, std::memory_order_relaxed);
        widen_gather_probe_enabled.store(false, std::memory_order_relaxed);
        stereo_dual_eye_enabled.store(false, std::memory_order_relaxed);
        scoped_reinvoke_enabled.store(false, std::memory_order_relaxed);
        const int cur = g_stereo_offset_index.load(std::memory_order_relaxed);
        const int next = (cur + 1) % static_cast<int>(kStereoOffsetSequence.size());
        g_stereo_offset_index.store(next, std::memory_order_relaxed);
        g_stereo_offset.store(kStereoOffsetSequence[next], std::memory_order_relaxed);
        // Also arm the render-view layout scan on transition into any
        // non-zero offset (fires once from apply_lever_a_mutation on the
        // next armed frame). And arm the Lever V probe similarly.
        if (kStereoOffsetSequence[next] != 0.0f) {
            render_view_layout_scan_pending.store(true, std::memory_order_relaxed);
            lever_v_probe_pending.store(true, std::memory_order_relaxed);
            lever_v_probe_log_count.store(0, std::memory_order_relaxed);
            lever_w_probe_pending.store(true, std::memory_order_relaxed);
            lever_w_probe_log_count.store(0, std::memory_order_relaxed);
        }
        log::warn(std::format(
            "F5: unified stereo offset -> {:.1f} uu (Lever A + Lever V, whole scene). "
            "Cycle: 0 / 2 / 5 / 20 / 100. If any layer lags behind at any magnitude, "
            "coverage is incomplete.",
            kStereoOffsetSequence[next]));
    }
    // Sampled ONCE - &1 consumes the toggle bit, so testing it in both branch
    // conditions would let the SHIFT test swallow a plain press.
    const bool insert_pressed = (GetAsyncKeyState(VK_INSERT) & 1) != 0;
    if (insert_pressed && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) {
        // ===== CTRL+INSERT: EXPERIMENT SLOT 3 ==============================
        //
        // ⚠ THIS KEY WAS NOT REWIRED WHEN THE SLOTS WERE INTRODUCED and it
        // still toggled the retired destFixed path, so the 21:34 run's "slot 3
        // did nothing" was a key that never reached slot 3. Slot keys route
        // through exp_toggle() and NOWHERE ELSE from now on - that is the
        // whole point of a fixed slot table, and hand-wiring one of them
        // defeats it.
        // 2026-08-28: slot 3 = DEROTATE (the anchor experiment is retired -
        // projected content is ruled out permanently). Gates the re-armed
        // remove-k correction in the bind path, now carrying the WORLD delta
        // the corpus finally justified.
        {
            const bool now_on = exp_toggle(2);
            log::warn(std::format(
                "CTRL+Insert: GPUFIX now {} - bringing every per-draw camera record to "
                "world-locked (gen 1) via C=VP_1*inv(VP_g), the operator corpus_check --fix "
                "proved lands 0-failure on real records.\n"
                "READING IT: PREDICTION - the reticle/effects (gen 0) AND the gun/windows "
                "(gen 2) should ALL move with the room now; only the correct populations are "
                "untouched (skipped by the arithmetic). PIPEFIX POST must read ~-1.00 across "
                "the board. Safety: records that do not factor at their generation are never "
                "written - if the GPU corrupts, press again to kill it live.",
                now_on ? "ON" : "OFF"));
        }
    } else if (insert_pressed) {
        // Moved off F12 to INSERT (2026-08-11): DOOMConfig.cfg binds F12 to
        // DOOM's own native "screenshot" command (confirmed - user heard a
        // camera-snapshot sound on every press), a real GPU readback + disk
        // write, categorically more active than F1-F4's already-accepted
        // double-binds (those are pure input actions - _quick0-3/_taunt0-3
        // - with no GPU side effects). Unlikely to corrupt OUR dump content
        // (DOOM's screenshot reads the swapchain via its own path, doesn't
        // mutate render state) but unnecessary noise/risk during a live
        // test - moved to INSERT, confirmed unbound in DOOMConfig.cfg.
        // Originally reclaimed 2026-08-11 (was a no-op, "legacy Lever A2
        // folded into F5") for StereoDualEyeDump - Milestone 1's offline
        // per-eye proof
        // (docs/STEREO-IMPLEMENTATION-PLAN.md). World-path only, real IPD
        // magnitude, two dedicated offscreen targets so eye 0/eye 1 don't
        // overwrite each other. Compare the resulting PPMs with
        // tools/ppmshift.ps1 (look for differential near/far parallax, not
        // a uniform slide) and tools/ppmstats.ps1 (occlusion-edge artifacts
        // would indicate the shared-depth-buffer risk flagged in the plan).
        const bool now_enabled = !stereo_dual_eye_enabled.load(std::memory_order_relaxed);
        if (now_enabled) {
            stereo_dual_eye_attempt_count.store(0, std::memory_order_relaxed);
            stereo_dual_eye_frame_counter.store(0, std::memory_order_relaxed);
            stereo_dual_eye_candidate_log_count.store(0, std::memory_order_relaxed);
            stereo_eye_dump_done[0].store(false, std::memory_order_relaxed);
            stereo_eye_dump_done[1].store(false, std::memory_order_relaxed);
            // Mutual exclusion: never stack a second independent extra-call
            // mechanism onto the same frame.
            stereo_preview_enabled.store(false, std::memory_order_relaxed);
            render_opaque_double_call_enabled.store(false, std::memory_order_relaxed);
            redirect_extra_call_enabled.store(false, std::memory_order_relaxed);
            render_view_clone_test_enabled.store(false, std::memory_order_relaxed);
            true_stereo_enabled.store(false, std::memory_order_relaxed);
            upstream_reinvoke_enabled.store(false, std::memory_order_relaxed);
            widen_gather_probe_enabled.store(false, std::memory_order_relaxed);
            scoped_reinvoke_enabled.store(false, std::memory_order_relaxed);
        }
        stereo_dual_eye_enabled.store(now_enabled, std::memory_order_relaxed);
        log::warn(now_enabled
            ? std::format(
                "INSERT: StereoDualEyeDump Phase 1 ARMED (STEREO-IMPLEMENTATION-PLAN.md v2) - the "
                "gather/cull/sort reinvoke is GONE (proven a no-op for visibility at IPD scale - "
                "RenderWalkBSP's real cull key is a separate BSP-leaf object, never renderView). Per "
                "eye: re-invoke ONLY RenderDepth through the same captured real-depth view, THEN a "
                "redirected RenderOpaque call. "
                "GPU camera-write control is {} for this run. When enabled, it translates the matched 64-byte RenderDepth record and "
                "the second world-space 128-byte binding-0 candidate in RenderOpaque, both at the same native eye offset; when disabled "
                "this is a no-camera-write control. Attempts spread {} real frames apart. World path "
                "only. Same risk class as F9/F4/F1/F6+F7. Watch closely. Attempt cap {}.",
                stereo_dual_eye_camera_write_enabled.load(std::memory_order_relaxed) ? "ENABLED" : "DISABLED",
                kStereoDualEyeFrameGap, kMaxStereoDualEyeAttempts)
            : "INSERT: StereoDualEyeDump disabled");
    }
    // Sampled ONCE. &1 consumes the toggle bit, so calling GetAsyncKeyState in
    // both branch conditions would let the SHIFT test swallow a plain press -
    // the same unreachable-branch bug SHIFT+F9 above already documents.
    // ================= CUTSCENE MARKER - UP ARROW (2026-08-24) ==============
    //
    // docs/PLAN-2026-08-24-CUTSCENE-VR-EXIT.md §3.1. Pressed ONCE as a cutscene
    // STARTS and ONCE as it ENDS, so the log brackets the event and the state
    // on both sides is directly comparable in one run.
    //
    // KEY CHOICE. VK_UP, verified free by the Phase 1 audit
    // (docs/AUDIT-2026-08-24-HOTKEY-OWNERSHIP.md §3): `grep -rn "VK_UP\|VK_DOWN"
    // src/` returns nothing across the tree. ⚠ The Lever Q comment above claims
    // "DOWN resets to 1.0; UP widens the target group" - that half was
    // DOCUMENTED AND NEVER IMPLEMENTED, and the code reads only RIGHT and LEFT.
    // Trusting that comment instead of the code would have rejected this key.
    // The arrow cluster is findable by touch in a headset, which is the whole
    // requirement. NOT the numpad: this keyboard does not have one.
    //
    // SCOPE / DATA SOURCE / POLARITY / CAPS, stated before the run:
    //   scope       - one line per press, from the present thread, log-only.
    //   data source - live atomics plus OpenXRContext's own state_/running_,
    //                 read on the same thread poll() writes them.
    //   polarity    - if VR is HEALTHY at a marker: running_=true,
    //                 xrState=FOCUSED, and leverAapplied's recent delta is
    //                 NON-ZERO. Any of those otherwise AT THE START MARKER
    //                 means the exit PRECEDED the cutscene and the run brackets
    //                 the wrong window - VOID, re-run, do not interpret.
    //   caps        - NONE. A press-driven line cannot flood, and a cap here
    //                 could truncate the one press that matters.
    //
    // THE RECENCY SAMPLER BELOW RUNS EVERY PRESENT, NOT ONLY ON A PRESS. The
    // "delta since the previous marker" is what measures the cutscene window,
    // but at marker #1 there IS no previous marker - and marker #1 is exactly
    // where the polarity check has to be made. So a ~1 s rolling snapshot is
    // kept alongside it, which answers "is the doubled call applying
    // separation RIGHT NOW" at the very first press. A frozen recent delta
    // while the doubling is armed is a BROKEN/DISENGAGED probe, not a null.
    {
        static uint32_t la_recent_base = 0;   // lever_a_applied ~1 s ago
        static uint32_t la_recent_delta = 0;  // its delta over that window
        static uint64_t la_recent_tick = 0;
        const uint32_t la_now = lever_a_applied.load(std::memory_order_relaxed);
        const uint64_t tick_now = GetTickCount64();
        if (tick_now - la_recent_tick >= 1000) {
            la_recent_delta = la_now - la_recent_base;
            la_recent_base = la_now;
            la_recent_tick = tick_now;
        }
        // SHIFT read with 0x8000, which consumes nothing, so it cannot starve
        // the UP press below. Read BEFORE the &1 test for the same reason the
        // rest of this file does it: the &1 bit is cleared by the read.
        const bool marker_shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool marker_ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        // SAMPLED ONCE. The &1 bit is cleared by the read, so testing it in both
        // branch conditions would let the CTRL branch swallow a bare press -
        // the exact trap this handler already documents above.
        const bool up_pressed = (GetAsyncKeyState(VK_UP) & 1) != 0;
        if (up_pressed && marker_ctrl) {
            // ===== CTRL+Up: LEVER V ROTATION ON/OFF ========================
            //
            // The discriminator four runs of weapon work have been missing.
            // Lever V carries the walls, sky, particles and - per the
            // 2026-08-16 pixel measurement - supposedly the weapon too. But
            // every intervention aimed through it has left the gun unchanged,
            // so the premise that the weapon is downstream of it has never
            // actually been tested. This tests it in one keypress.
            const bool now_on = exp_toggle(1);
            lever_v_rot_enabled.store(now_on, std::memory_order_relaxed);
            // ===== SLOT 2 REASSIGNED 2026-08-30: THE PRECULL A/B ===========
            //
            // In a precull build this key is the run's whole comparison AND
            // its safety exit, and it swaps the two modes rather than just
            // switching one off: ON = the producer write owns the camera (the
            // new site), OFF = the producer write stands down and the legacy
            // seam rotations come straight back, which is byte-for-byte the
            // shipped behaviour. One press is therefore both "show me the old
            // build" and "get me out of this".
            //
            // Gated on the config so a non-precull build keeps this key's old
            // meaning exactly - a reassigned slot with a live old consumer is
            // the half-migrated-indirection defect, and this avoids it by
            // deciding the role once, from the switch file.
            if (wrap::config().precull) {
                wrap::set_precull_runtime(now_on ? 1 : 0);
                // The legacy sites must come back ON in the same press, or OFF
                // would mean "no head rotation at all" instead of "the old
                // build" - and a dead-camera arm would be read as the new
                // site failing.
                lever_v_rot_enabled.store(true, std::memory_order_relaxed);
                log::warn(std::format(
                    "CTRL+Up: PRECULL now {}.\n"
                    "READING IT: this is the A/B, not a plain off switch. ON = the head "
                    "rotation is written into viewParms+0x6C at DOOM+0x1830B80, the producer "
                    "every camera matrix descends from, and Lever A / Lever V stand down. "
                    "OFF = the producer write stands down and the legacy seam rotations come "
                    "back, which is exactly the shipped build. Both arms rotate the view by "
                    "the same angle; what differs is WHICH matrices inherit it. Read the "
                    "PRECULL line for written/refused counts and the arm label.",
                    now_on ? "ON (producer write owns the camera)"
                           : "OFF (legacy seam rotation - the shipped build)"));
            } else
            log::warn(std::format(
                "CTRL+Up: LEGACY derived rotations (Lever A renderView + Lever V GUI) now {}.\n"
                "READING IT: 2026-08-27 - this now gates BOTH legacy head-rotation writes as "
                "one arm (exp_on(1) gates Lever A, lever_v_rot_enabled gates Lever V; one "
                "keypress drives both). The sites still run and still publish; only the "
                "rotations change. The seam arm that used to pair with this on SHIFT+Up "
                "is gone (2026-09-03) - this chord is the whole A/B now.",
                now_on ? "ON (the previous behaviour)" : "OFF"));
        } else if (up_pressed) {
            static uint32_t marker_index = 0;
            static uint32_t la_at_last_marker = 0;
            static bool have_last_marker = false;
            const uint32_t n = ++marker_index;
            // DENSE CAPTURE AT THE MOMENT OF THE PRESS - see
            // cutscene_dense_burst. Without this the nearest write census to
            // marker #3 in the 19:14 run was 8 s before it.
            cutscene_dense_burst("cutscene marker");
            // SHIFT+UP ALSO CAPTURES THE LIKE-FOR-LIKE PPM PAIR. This is the
            // only instrument that looks at PIXELS rather than at flags, and
            // it is the one thing that can settle "the layer says PROJECTION
            // but it looks flat": if finalstage_eye0_control_N and
            // finalstage_eye1_graded_N are near-identical, the two eyes are
            // being handed the same picture despite every offset being written.
            //
            // ⚠ IT COSTS ONE DELIBERATE FREEZE - a queue drain plus a 23 MB
            // readback per image. That is expected, not a hang. Press it ONCE,
            // at the moment the picture looks wrong, and not during anything
            // you need to react to.
            if (marker_shift) {
                if (final_pass_mirror_color != VK_NULL_HANDLE) {
                    final_pass_dump_pending.store(true, std::memory_order_relaxed);
                    log::warn("CUTSCENE MARKER: SHIFT held - matched PPM pair ARMED. The next "
                              "completed final-stage double writes finalstage_eye0_control_N.ppm "
                              "and finalstage_eye1_graded_N.ppm. Expect ONE freeze; that is the "
                              "readback, not a hang.");
                } else {
                    log::error("CUTSCENE MARKER: SHIFT held but final_pass_mirror_color is NULL, "
                               "so there is no eye-1 image to dump and no pair can be written. "
                               "That is itself a finding - eye 1 has no graded mirror right now.");
                }
            }
            const bool running = xr().session_running();
            const XrSessionState xr_state = xr().session_state();
            const char* xr_state_name = OpenXRContext::session_state_name(xr_state);
            const bool double_on = frame_root_double_enabled.load(std::memory_order_relaxed);

            // Every DOOM swapchain record we still hold, with handle, format
            // and extent - the (c) signature is only legible if the marker
            // says which objects were live at each end of the window.
            std::string sc_desc;
            {
                std::unique_lock lock(swapchains_mutex);
                if (swapchains.empty()) {
                    sc_desc = "NONE HELD";
                } else {
                    for (const auto& [handle, rec] : swapchains) {
                        if (!sc_desc.empty()) sc_desc += ", ";
                        sc_desc += std::format("{:#x} fmt={} {}x{} imgs={}",
                            reinterpret_cast<uintptr_t>(handle), static_cast<int>(rec.format),
                            rec.extent.width, rec.extent.height, rec.images.size());
                    }
                }
            }

            const std::string since_marker = have_last_marker
                ? std::format("{}", la_now - la_at_last_marker)
                : std::string("n/a (first marker)");

            SYSTEMTIME st{};
            GetLocalTime(&st);

            log::warn(std::format(
                "CUTSCENE MARKER #{} at {:02}:{:02}:{:02}.{:03} | "
                "headsetIsShowing={} | "
                "xrRunning={} xrState={} | "
                "frameRootDoubleEnabled={} attempts={} ipd={:.2f} zeroOffset={} | "
                "leverAapplied={} (+{} since previous marker) (+{} in the last ~1 s) | "
                "swapchains=[{}] | mirrorGeneration={} mirrorsCreated={} | scriptedStep={}\n"
                "READ IT LIKE THIS. Compare this line against the OTHER marker of the pair, and "
                "look at what sits BETWEEN them:\n"
                "  (a) an 'XR SESSION STATE ... -> STOPPING/EXITING/LOSS_PENDING' line => the "
                "runtime ended the session; the fix is session lifecycle.\n"
                "  (b) a 'FrameRootDouble ... threw a structured exception ... disarming' line "
                "=> the doubled call faulted, and frame_root_stage in it says where.\n"
                "  (c) 'SWAPCHAIN DESTROYED'/'SWAPCHAIN CREATED' with NO xr transition and NO "
                "exception => scene-transition resource churn.\n"
                "  (d) none of those, and leverAapplied's delta simply goes to 0 => the doubled "
                "call stopped being reached at all.\n"
                "⚠ POLARITY AT THE START MARKER: healthy is xrRunning=true, xrState=FOCUSED, and "
                "a NON-ZERO last-1s leverAapplied delta. If this FIRST marker already shows "
                "otherwise, VR had already exited before the cutscene and this run brackets the "
                "wrong window - it is VOID, not a result. Re-run rather than interpret.\n"
                "⚠ leverAapplied=0 with frameRootDoubleEnabled=false at marker #1 means PAGE UP "
                "was never pressed and there was no stereo to lose.",
                n, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                OpenXRContext::layer_kind_name(xr().layer_kind()),
                running ? "true" : "false", xr_state_name,
                double_on ? "true" : "false",
                frame_root_double_attempts.load(std::memory_order_relaxed),
                frame_root_double_ipd.load(std::memory_order_relaxed),
                frame_root_zero_offset.load(std::memory_order_relaxed) ? "true" : "false",
                la_now, since_marker, la_recent_delta,
                sc_desc,
                mirror_generation.load(std::memory_order_relaxed),
                mirror_created_count.load(std::memory_order_relaxed),
                scripted_test_step.load(std::memory_order_relaxed)));

            la_at_last_marker = la_now;
            have_last_marker = true;
        }
    }
    // ================= FAST VR TOGGLE - DOWN ARROW (2026-08-24) =============
    //
    // WHY THIS EXISTS. Measured from the 11:47 run: PageUp to "SYMMETRIC
    // STEREO LIVE" took 3.76 s, and the picture kept changing until 11.6 s.
    // None of that is VR initialisation - the XR session was already FOCUSED
    // 30 s before the press and never changed state again. The delay is a
    // scripted DIAGNOSTIC BATTERY that happens to end in stereo:
    //
    //   step 0  wait for world          0.82 s   a real gate
    //   step 1  pipeline inline         0.03 s   REQUIRED
    //   step 2  doubling armed          0.39 s   REQUIRED (30-frame cvar settle)
    //   step 3  read redirection        0.16 s   REQUIRED (mirror convergence)
    //   step 34 CallerSpecificity       0.02 s   log-only probe, 90 frames
    //   step 36 Containment             1.15 s   log-only probe, 90 frames
    //   step 4  SYMMETRIC STEREO LIVE   1.19 s
    //   step 37 grade gap closing       3.14 s   deliberate judging pause
    //   step 39 monitor eye-1           4.71 s   flat-run only
    //
    // Steps 34 and 36 are ~2.3 s of log-only probes sitting on the critical
    // path to VR. Steps 37 and 39 change the picture AGAIN after stereo is
    // already live - those are the "multiple visible view shifts".
    //
    // ⚠⚠ CORRECTION, 2026-08-24, SAME DAY, FOUND BY LOOKING AT THE PICTURE.
    // The table above marked steps 34 and 36 "log-only probe" and step 37 a
    // "deliberate judging pause". BOTH LABELS WERE WRONG, and the first
    // version of this toggle shipped with all three skipped. The owner armed
    // it, looked, and reported "the right eye is not graded properly, the left
    // eye is" within about two minutes. That is exactly rung 0's documented
    // signature - "eye 1 keeps its ungraded frame-root mirror (fmt 122)".
    //
    // The real dependency chain is 34 -> 36 -> 37, and every link is
    // load-bearing:
    //   step 34  emit_caller_specificity() POPULATES final_candidates from the
    //            caller tallies. Without it the candidate list is empty.
    //   step 36  emit_containment() CONSUMES that list, derives the outermost
    //            safe target and INSTALLS THE FINAL-STAGE HOOK via MinHook.
    //            Its own log line calls itself "log-only", which is what misled
    //            me; the hook install is the part that is not.
    //   step 37  requires final_stage_hook_installed, then calls
    //            apply_graded_mode(2) - the ONLY writer of graded_mode,
    //            final_pass_double_armed and graded_stereo_enabled, and
    //            therefore the only thing that makes eye 1 graded (fmt 44)
    //            rather than raw HDR world colour (fmt 122).
    //
    // So the two windows cost ~2.3 s and cannot be skipped on a cold session.
    // They CAN be skipped on re-entry: final_stage_hook_installed is a one-way
    // latch and MinHook installs persist for the process, so once a session
    // has paid for them it never pays again. That is what matters for the
    // cutscene case, which is re-entry, not first entry.
    //
    // The claim in this comment block that reusing arm_frame_root_double made
    // "fast VR" and "PageUp VR" the same configuration was therefore FALSE as
    // shipped: identical arming did not make identical state, because the
    // state that differed was set three steps later. It is true now.
    //
    // THIS PATH TAKES ONLY THE REQUIRED STEPS, IN PAGEUP'S ORDER. PageUp is
    // not modified in any way: this is a PARALLEL route to the same state, not
    // a change to the existing one, so nothing about the scripted test moves.
    //
    // ⚠ IT ARMS THROUGH arm_frame_root_double(false), THE SAME HELPER STEP 2
    // USES, deliberately. That function force-sets the whole measurement state
    // at arm time (light-culling suppression ON, shadow draws OFF, throttle
    // per kPresentThrottleEnabled) precisely because a toggle earlier in the
    // same process leaves those flags somewhere else. If this path hand-rolled
    // its own arming, "fast VR" and "PageUp VR" would be two different
    // configurations that look identical, and every judgement made in one
    // would silently not transfer to the other.
    //
    // WHAT MAKES RE-ENTRY INSTANT: the exit deliberately does NOT tear down
    // the two expensive things. The inline pipeline stays inline and the
    // mirrors stay converged, because both are inert with no doubled call
    // running and both are what cost the seconds. Only the flags that select
    // stereo are cleared. First entry pays the cvar settle and convergence;
    // every entry after that is the next frame.
    {
        // 0 = off
        // 1 = waiting for the r_useSMP write to settle (first entry only)
        // 2 = waiting for mirrors to converge + fresh doubled calls
        // 3 = CallerSpecificity window   (skipped once the hook is installed)
        // 4 = Containment window          (skipped once the hook is installed)
        // 5 = live
        constexpr uint32_t kFastVrLive = 5;
        static uint32_t fast_vr_state = 0;
        static uint32_t fast_vr_frames = 0;
        static uint64_t fast_vr_started_tick = 0;
        static uint32_t fast_vr_entries = 0;

        // THE READINESS GATE, KEPT - BUT MEASURED CONTINUOUSLY INSTEAD OF ON
        // THE PRESS. Step 0 exists for a good reason the owner asked for
        // explicitly: arming doubling in a menu, on a loading screen or during
        // a level transition forces the pipeline inline against a world that
        // is not rendering. Dropping that gate to save time would be trading a
        // real safety property for 0.8 s.
        //
        // But step 0 spends those 0.8 s STARTING a 60-present measurement when
        // the press arrives. For a toggle used mid-gameplay the answer is
        // already known - so the same measurement runs continuously in a
        // rolling window and the press just reads the latest verdict. Same
        // evidence, same threshold (50 real frame-root entries per 60
        // presents), zero latency.
        static uint32_t world_window_presents = 0;
        static uint32_t world_serial_mark = 0;
        static bool world_rendering_recent = false;
        {
            const uint32_t serial = g_frame_root_serial.load(std::memory_order_relaxed);
            if (++world_window_presents >= 60) {
                world_rendering_recent = (serial - world_serial_mark) >= 50;
                world_serial_mark = serial;
                world_window_presents = 0;
            }
        }

        // ================= AUTOMATIC ENTRY (2026-08-24) ====================
        //
        // GOAL, in the owner's words: be in VR without pressing anything, with
        // the arming cost hidden behind the loading screen.
        //
        // ⚠ WHY IT CANNOT LITERALLY RUN DURING THE LOAD. The two derivation
        // windows observe the GRADE+HUD RENDER PASS to find a hookable caller,
        // and the readiness gate exists because arming against a world that is
        // not rendering forces the pipeline inline at exactly the wrong moment.
        // A loading screen renders no world, so there is nothing for
        // CallerSpecificity to tally and the derivation would either fail or
        // pick a target from the wrong pipeline.
        //
        // WHAT IT DOES INSTEAD, which reaches the same place: it fires at the
        // FIRST MOMENT THE WORLD IS ACTUALLY RENDERING, unprompted. In this
        // game that is the opening cutscene - the same moment the owner has
        // always pressed PageUp by hand, so it is a proven-safe arming point,
        // not a new one. Combined with fast_vr_hold_presentation the headset
        // stays on the flat quad for the whole ~3 s and then flips once, so the
        // cost lands on content nobody is judging. That is the masking.
        //
        // ONE-SHOT. Latched so that a manual DOWN-out is never undone by the
        // auto path re-arming behind the owner's back - after the first entry
        // the toggle is theirs alone.
        static bool fast_vr_auto_fired = false;
        // ⚠ DEFAULT OFF AS OF THE 19:38 RUN - OPT-IN, NOT OPT-OUT.
        //
        // It shipped default-on and cost a run. It fired while the CONTINUE
        // screen was still up (DOOM's menu renders a 3D world behind it, so the
        // readiness gate was legitimately satisfied) and then completed ~3.7 s
        // later, mid-load - so the flip landed in the middle of a scene
        // transition and the owner got the loading screen in one eye and the
        // previous menu frame in the other.
        //
        // THE GATE CANNOT BE FIXED BY WAITING LONGER. The menu is stable and
        // renders a world; the load that breaks the arm happens AFTER the
        // decision to arm. No amount of looking at the past distinguishes
        // "stable menu" from "stable menu about to load a level". The real fix
        // is transition resilience - and the stability gate added to go-live
        // below is the first half of it - but that is not proven yet.
        //
        // So this is off by default while the cutscene investigation is live,
        // because a confounded 5-minute headset run is expensive. Create
        // C:\dev\doomvr-autovr.txt to turn it back on.
        static const bool auto_vr_disabled = [] {
            FILE* f = nullptr;
            if (fopen_s(&f, "C:\\dev\\doomvr-autovr.txt", "r") == 0 && f) {
                std::fclose(f);
                return false;   // file present = auto-entry ENABLED
            }
            return true;        // absent = disabled, the default
        }();
        const bool auto_enter_now =
            !fast_vr_auto_fired && !auto_vr_disabled && fast_vr_state == 0 &&
            world_rendering_recent &&
            !scripted_test_active.load(std::memory_order_relaxed) &&
            // Never fight a hand-armed run: if doubling is already on, someone
            // else (PageUp, or a previous arm) owns these flags.
            !frame_root_double_enabled.load(std::memory_order_relaxed);
        if (auto_enter_now) {
            fast_vr_auto_fired = true;
            log::warn("FAST VR: AUTO-ENTERING - the world has started rendering, so VR is arming "
                      "by itself. No keypress needed. The headset stays on the flat quad for the "
                      "whole arming window and flips ONCE at the end, so the ~3 s first-entry "
                      "cost lands during the opening cutscene rather than on anything you are "
                      "judging. This fires ONCE per session; DOWN owns the toggle from here on. "
                      "Create C:\\dev\\doomvr-noautovr.txt to disable it.");
        }

        // SHIFT+DOWN = FORCE RE-ENTER, one press (2026-08-24).
        //
        // WHY IT EXISTS. In the 20:23 run the owner reported "going back into
        // VR with DOWN did not work". The log says otherwise, and the truth is
        // worse than a broken key: the plugin was STILL LIVE - all four markers
        // read headsetIsShowing=PROJECTION - so their first DOWN did exactly
        // what a toggle should and took them OUT, and the second brought them
        // back IN. From the inside it looked like the key was ignored.
        //
        // The real fault is that a TOGGLE is unusable when the picture lies
        // about which state you are in. So this is a key with one meaning:
        // whatever state you are in, put me back into VR from scratch. It
        // cannot be got the wrong way round.
        static bool fast_vr_reenter_pending = false;
        // Automatic recovery, armed by the cadence detector at the end of a
        // cutscene. Same path as SHIFT+DOWN, just nobody had to press it.
        if (fast_vr_auto_recover.exchange(false, std::memory_order_relaxed)) {
            if (fast_vr_state == kFastVrLive) {
                log::warn("AUTO-RECOVER: cutscene over - rebuilding mirrors and re-arming stereo "
                          "so VR comes back by itself.");
                mirror_hard_reset("auto-recover after cutscene");
                fast_vr_reenter_pending = true;
            } else if (fast_vr_state != 0) {
                // Mid-arm already; leave it alone rather than race it.
                log::warn("AUTO-RECOVER: skipped - an entry is already arming.");
            }
        }
        const bool down_shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool down_pressed = (GetAsyncKeyState(VK_DOWN) & 1) != 0;
        if (down_pressed && down_shift) {
            if (fast_vr_state == kFastVrLive) {
                log::warn("SHIFT+DOWN: FORCE RE-ENTER, WITH A FULL MIRROR REBUILD. Re-arming the "
                          "flags alone was already proven not to bring VR back - the mirrors are "
                          "the one thing the fast exit deliberately leaves standing, so they are "
                          "what this drops.");
                mirror_hard_reset("SHIFT+DOWN force re-enter");
                fast_vr_reenter_pending = true;   // ENTER fires next frame, once state is 0
            } else if (fast_vr_state == 0) {
                log::warn("SHIFT+DOWN: not live - rebuilding mirrors and entering from scratch.");
                mirror_hard_reset("SHIFT+DOWN cold entry");
            } else {
                log::warn(std::format("SHIFT+DOWN: ignored - entry already arming (stage {} of 4).",
                                      fast_vr_state));
            }
        }
        const bool reenter_now = fast_vr_reenter_pending && fast_vr_state == 0;
        if (reenter_now) fast_vr_reenter_pending = false;
        // A plain DOWN toggles. SHIFT+DOWN exits first (above) and re-enters
        // here. auto_enter_now is the one-shot startup path.
        if ((down_pressed && !down_shift) || auto_enter_now || reenter_now ||
            (down_pressed && down_shift && fast_vr_state == kFastVrLive)) {
            if (fast_vr_state == 0 && !world_rendering_recent) {
                // Refuse, immediately and legibly. Unlike step 0 this does not
                // queue the action for later: a toggle that fires seconds after
                // the press, once you have walked somewhere else, is worse than
                // one that says no.
                log::error("DOWN: FAST VR REFUSED - the world is not rendering (fewer than 50 "
                           "frame-root entries in the last 60 presents). In a menu, a loading "
                           "screen or a level transition? Press DOWN again once you are in the "
                           "world. Nothing was armed and no cvar was touched.");
            } else if (scripted_test_active.load(std::memory_order_relaxed)) {
                // Refuse rather than fight it. Two arming paths driving the
                // same flags from different clocks is how a run ends up in a
                // state neither one describes.
                log::error("DOWN: FAST VR REFUSED - the PageUp scripted test is active and owns "
                           "these flags. Press PageUp once to stop it, then DOWN.");
            } else if (fast_vr_state == kFastVrLive) {
                // ---- EXIT ----
                frame_root_double_enabled.store(false, std::memory_order_relaxed);
                frame_root_full_frame.store(false, std::memory_order_relaxed);
                symmetric_stereo_enabled.store(false, std::memory_order_relaxed);
                g_stereo_offset.store(0.0f, std::memory_order_relaxed);
                // The graded final-pass doubling goes with it. Leaving
                // final_pass_double_armed set while the frame-root doubling is
                // off would keep re-invoking the final stage into a mirror
                // nothing presents - work with no consumer, and a state the
                // PageUp teardown explicitly clears too.
                apply_graded_mode(0, "fast VR out");
                // Unconditional, not conditional. If an entry is abandoned
                // part-way - the world stops rendering, convergence never
                // arrives, the 600-frame timeout fires - the hold must not
                // survive into the idle state, where it would suppress stereo
                // with no toggle running and look exactly like VR failing to
                // arm. Clearing it on every exit makes that unreachable.
                fast_vr_hold_presentation.store(false, std::memory_order_relaxed);
                fast_vr_state = 0;
                log::warn(std::format(
                    "FAST VR: OUT. Stereo off, the headset falls back to the flat quad layer. "
                    "DELIBERATELY LEFT STANDING so re-entry is instant: the pipeline stays "
                    "inline, the {} mirrors stay converged, and read redirection stays on - all "
                    "three are inert with no doubled call running, and all three are what cost "
                    "the seconds. Press DOWN again to go back in.",
                    mirror_created_count.load(std::memory_order_relaxed)));
            } else if (fast_vr_state == 0) {
                // ---- ENTER ----
                fast_vr_started_tick = GetTickCount64();
                ++fast_vr_entries;
                // Hold the headset on the flat quad until the FINAL state is
                // ready, so entering VR is one visible transition instead of
                // two. Cleared at go-live below, and unconditionally on exit.
                fast_vr_hold_presentation.store(true, std::memory_order_relaxed);
                const uintptr_t doom_base =
                    reinterpret_cast<uintptr_t>(GetModuleHandleW(L"DOOMx64vk.exe"));
                int32_t use_smp = -1;
                if (doom_base) {
                    safe_read_bytes(doom_base + kUseSmpCvarRva + 0x30, &use_smp, sizeof(use_smp));
                }
                if (use_smp == 2) {
                    // Only pay the 30-frame settle when the cvar actually has
                    // to change. On every entry after the first it does not.
                    toggle_force_single_threaded();
                    fast_vr_state = 1;
                    fast_vr_frames = 0;
                    log::warn(std::format(
                        "FAST VR: entry #{} - r_useSMP was {}, forcing inline and waiting 30 "
                        "frames for the write to settle. THIS WAIT IS FIRST-ENTRY ONLY.",
                        fast_vr_entries, use_smp));
                } else {
                    arm_frame_root_double(false);
                    fast_vr_state = 2;
                    fast_vr_frames = 0;
                    log::warn(std::format(
                        "FAST VR: entry #{} - pipeline already inline (r_useSMP={}), skipping the "
                        "cvar wait. Doubling armed; waiting on mirror convergence.",
                        fast_vr_entries, use_smp));
                }
            } else {
                // States 1-4: an entry is already in flight. The press is
                // IGNORED rather than acted on - cancelling half-way would
                // leave the doubled call armed with nothing presenting it, and
                // a second entry racing the first is worse than a short wait.
                // Logged so "I pressed it and nothing happened" has an answer
                // in the log instead of being a mystery. Every one of these
                // states is bounded (30, 90, 90 frames and a 600-frame
                // timeout), so this cannot wait forever.
                log::warn(std::format(
                    "FAST VR: press IGNORED - entry #{} is still arming (stage {} of 4). The "
                    "headset is deliberately held on the flat quad until the FINAL state is "
                    "ready, so entering VR is one visible transition rather than two. Wait for "
                    "the 'FAST VR: IN' line.",
                    fast_vr_entries, fast_vr_state));
            }
        }
        // The two waits, advanced once per present. Both are CONDITIONAL, and
        // both are skipped outright on a re-entry, which is the whole point.
        if (fast_vr_state == 1) {
            if (++fast_vr_frames >= 30) {
                arm_frame_root_double(false);
                fast_vr_state = 2;
                fast_vr_frames = 0;
            }
        } else if (fast_vr_state == 2) {
            // Same condition step 3 uses, and the same timeout discipline: the
            // 600-frame cap exists so a failure to converge is REPORTED rather
            // than hanging the toggle. Enabling redirection during the lazy
            // mirror-creation window is what hung the GPU three times, so this
            // gate is not decorative.
            ++fast_vr_frames;
            // ⚠ THE STALE-CONVERGENCE HAZARD, AND WHY THE ATTEMPT COUNT IS IN
            // THIS CONDITION. mirror_stable_calls is only ever advanced or
            // reset by update_mirror_convergence(), which runs at the END of a
            // doubled call. The exit path above deliberately leaves it alone,
            // so on a re-entry it is STALE-HIGH and mirrors_converged() reads
            // true before a single doubled call has validated the current
            // mirror set. If a level or scene transition rebuilt the mirrors
            // while VR was off - and mirror_generation moves with no keypress
            // involved, which is exactly what a cutscene might do - the first
            // doubled call after re-arming would substitute against a set that
            // convergence was never established on. That is the frame-0 hang.
            //
            // This is the same shape as the shared-reset trap that has bitten
            // this project repeatedly: a reset gated on one consumer (the
            // doubled call) while another consumer (this toggle) reads the
            // value on a different clock.
            //
            // arm_frame_root_double zeroes frame_root_double_attempts as its
            // last act, so requiring kMirrorStableCalls attempts guarantees
            // update_mirror_convergence has run that many times IN THIS ENTRY -
            // and if the generation moved, the first of them reset the counter
            // and mirrors_converged() is false until it genuinely re-converges.
            // Cost: 8 doubled calls, ~100-130 ms at these frame rates. The
            // seconds were never here; they were in the cvar settle and in
            // steps 34/36.
            //
            // The substitution site at the descriptor-write hook checks
            // mirrors_converged() independently on every call, so this is a
            // second line of defence rather than the only one - but it is the
            // one that stops the single unvalidated frame slipping through.
            const bool fresh_calls =
                frame_root_double_attempts.load(std::memory_order_relaxed) >= kMirrorStableCalls;
            // SCENE-STABILITY GATE (2026-08-24, after the 19:38 run).
            //
            // The flip into stereo must not land in the middle of a scene
            // transition. It did: the owner saw the loading screen in one eye
            // and the previous menu frame in the other, because eye 1's mirror
            // still held pre-transition content when the layer changed. The
            // log agrees - MirrorConvergence was LOST at 19:38:28.142 and again
            // at .285, i.e. twice within 150 ms of the flip at 19:38:28.133.
            //
            // mirror_generation moving IS the transition signal, and it is
            // already maintained for us. Requiring it to hold still for a
            // stretch before going live means the quad stays up until the new
            // scene's mirrors exist and have been rendered into. Costs nothing
            // in the steady state, where the generation has not moved for
            // minutes; costs a fraction of a second at a real transition.
            static uint32_t stable_gen = 0;
            static uint32_t stable_gen_frames = 0;
            const uint32_t gen_now = mirror_generation.load(std::memory_order_relaxed);
            if (gen_now != stable_gen) { stable_gen = gen_now; stable_gen_frames = 0; }
            else { ++stable_gen_frames; }
            const bool scene_stable = stable_gen_frames >= 60;
            if ((mirrors_converged() && fresh_calls && scene_stable) || fast_vr_frames >= 600) {
                if (!mirrors_converged()) {
                    log::error(std::format(
                        "FAST VR: mirrors did NOT converge in {} frames. Redirection is enabled "
                        "but its own convergence gate will keep refusing, so eye 1 will render "
                        "WITHOUT redirection. Grep 'MirrorConvergence'.", fast_vr_frames));
                }
                read_redirect_enabled.store(true, std::memory_order_relaxed);
                invalidate_read_redirect_cache("fast VR toggle enabling redirection");
                // The two derivation windows, in PageUp's order, and ONLY if
                // this session has not already paid for them.
                if (final_stage_hook_installed.load(std::memory_order_relaxed)) {
                    fast_vr_state = 5;  // handled immediately below
                    fast_vr_frames = 0;
                } else {
                    caller_specificity_walks.store(0, std::memory_order_relaxed);
                    caller_specificity_armed.store(true, std::memory_order_relaxed);
                    fast_vr_state = 3;
                    fast_vr_frames = 0;
                    log::warn("FAST VR: final-stage hook not yet derived this session - running "
                              "the CallerSpecificity and Containment windows (~2.3 s, 90 frames "
                              "each). THIS IS FIRST-ENTRY ONLY: the hook install is a one-way "
                              "latch, so every later entry skips both.");
                }
            }
        } else if (fast_vr_state == 3) {
            if (++fast_vr_frames >= 90) {
                caller_specificity_armed.store(false, std::memory_order_relaxed);
                emit_caller_specificity();   // POPULATES final_candidates
                containment_armed.store(true, std::memory_order_relaxed);
                fast_vr_state = 4;
                fast_vr_frames = 0;
            }
        } else if (fast_vr_state == 4) {
            if (++fast_vr_frames >= 90) {
                containment_armed.store(false, std::memory_order_relaxed);
                emit_containment();          // DERIVES AND INSTALLS THE HOOK
                fast_vr_state = 5;
                fast_vr_frames = 0;
            }
        }
        if (fast_vr_state == 5 && fast_vr_started_tick != 0) {
            // Step 4's state, and only step 4's: eye 0 falls through to
            // DOOM's presented swapchain image (graded, HUD and all,
            // rendered from the -half camera), eye 1 comes from its own
            // mirror override. symmetric_stereo_enabled stays FALSE - see
            // step 4's own note on why pointing eye 0 at the composite
            // holding image was wrong.
            frame_root_full_frame.store(true, std::memory_order_relaxed);
            symmetric_stereo_enabled.store(false, std::memory_order_relaxed);
            symmetric_log_count.store(0, std::memory_order_relaxed);
            // Step 37's state. Both of step 37's preconditions are kept: a
            // missing hook or unconverged mirrors would make apply_graded_mode
            // a silent no-op that looks exactly like "the grade did not work",
            // which is the ambiguity that has already cost this project runs.
            const bool hooked = final_stage_hook_installed.load(std::memory_order_relaxed);
            if (hooked && mirrors_converged()) {
                apply_graded_mode(2, "fast VR go-live");
            } else {
                log::error(std::format(
                    "FAST VR: GRADE GAP NOT CLOSED - hookInstalled={} mirrorsConverged={}. Eye 1 "
                    "will keep its UNGRADED frame-root mirror (fmt 122) while eye 0 is DOOM's "
                    "graded frame (fmt 44). That is rung 0's signature and it is what 'the right "
                    "eye is not graded properly' looks like. VR is live but the pair is "
                    "mismatched; do not read eye 1's tone as a result.",
                    hooked, mirrors_converged()));
            }
            // RELEASED LAST, after every flag above is set. This is the whole
            // point of the hold: the layer flips QUAD -> PROJECTION on the
            // NEXT present, by which time fullFrame, the symmetric offsets and
            // the graded eye-1 mirror are all already in place. One transition.
            fast_vr_hold_presentation.store(false, std::memory_order_relaxed);
            fast_vr_frames = 0;
            log::warn(std::format(
                "FAST VR: IN. Entry #{} took {} ms from the keypress, gradeRung={}. "
                "⚠ ENGAGEMENT IS NOT PROVEN BY THIS LINE - it says the flags were set, not that "
                "the doubled final pass ran. The FastVrArmCheck line ~1 s from now reads the "
                "counters and is the one to trust.",
                fast_vr_entries, GetTickCount64() - fast_vr_started_tick,
                graded_mode.load(std::memory_order_relaxed)));
            fast_vr_started_tick = 0;  // latch: this block runs once per entry
        } else if (fast_vr_state == 5) {
            // THE BASELINE CONTROL, TAKEN AUTOMATICALLY. ~20 s after VR goes
            // live, while it is known good. This is the pair that says what
            // WORKING stereo looks like at pixel level, and it is captured by
            // code precisely so nobody has to press a key at the right moment
            // or demonstrate anything.
            static bool baseline_taken = false;
            static uint32_t baseline_frames = 0;
            if (!baseline_taken && ++baseline_frames >= 1400) {
                baseline_taken = true;
                arm_pixel_pair("BASELINE - VR live and healthy");
            }
        }
        if (fast_vr_state == 5 && fast_vr_frames < 61) {
            // ENGAGEMENT CHECK, from the instrument's own counters, ~1 s after
            // going live. Non-blocking: the toggle is already live, so this
            // costs no latency. It exists because the first version of this
            // toggle shipped a silently ungraded eye 1 and the OWNER found it
            // by looking - which is precisely the failure the standing rule
            // about verifying engagement from counters is meant to prevent.
            if (++fast_vr_frames == 60) {
                const uint32_t entries = final_stage_calls.load(std::memory_order_relaxed);
                const uint32_t doubles = final_pass_doubles.load(std::memory_order_relaxed);
                const uint32_t refused =
                    final_pass_refused_no_mirror.load(std::memory_order_relaxed);
                const uint32_t mode = graded_mode.load(std::memory_order_relaxed);
                log::warn(std::format(
                    "FastVrArmCheck: gradeRung={} finalStageEntries={} finalStageDoubles={} "
                    "preflightRefusals={} leverAapplied={}. ENTRIES and DOUBLES are counted "
                    "SEPARATELY and must never be read as one number.",
                    mode, entries, doubles, refused,
                    lever_a_applied.load(std::memory_order_relaxed)));
                if (mode >= 2 && entries == 0) {
                    log::error("FastVrArmCheck: the final-stage hook is installed but its target "
                               "NEVER RAN in this window. Eye 1 is still its ungraded composite.");
                } else if (mode >= 2 && doubles == 0) {
                    log::error(std::format(
                        "FastVrArmCheck: {} real final-stage calls and ZERO second invocations "
                        "({} pre-flight refusals). The mode is armed and doing nothing - eye 1 "
                        "keeps its ungraded composite. Do NOT read eye 1's tone as a result.",
                        entries, refused));
                }
            }
        }
    }
    const bool home_pressed = (GetAsyncKeyState(VK_HOME) & 1) != 0;
    // home_pressed is read ONCE above and reused by every arm below.
    // GetAsyncKeyState's &1 bit is the "pressed since last query" flag and is
    // CLEARED by the read, so re-reading VK_HOME here would let this branch
    // swallow the press that bare HOME is waiting for - the unreachable-branch
    // bug SHIFT+F9 already documents. VK_CONTROL/VK_SHIFT are read with 0x8000,
    // which is a STATE bit and safe to sample repeatedly.
    if (home_pressed && shift_held_now) {
        // SHIFT+HOME: LEVER V VIEWMODEL LOCK, live A/B. Same reasoning as
        // SHIFT+ScrollLock's separation cycle - judging eye 0 across a toggle
        // from ONE standing position is far more reliable than judging it
        // across a relaunch, where lighting, scene state and standing position
        // all drift. The run starts LOCKED, so the state under test is the
        // default and no press is needed for the run to be valid; this exists
        // to flip BACK to the old behaviour for comparison.
        //
        // Modifier-checked and placed ABOVE the plain-HOME branch so a plain
        // press still reaches RealFrameQueryGateProbe and a SHIFT press does
        // not fire both. This handler consumes the press either way.
        const bool on = !gun_unshift_enabled.load(std::memory_order_relaxed);
        gun_unshift_enabled.store(on, std::memory_order_relaxed);
        if (!on) gun_unshift_invalidate();
        vwc_arm(on ? "gun unshift ON" : "gun unshift OFF");
        log::warn(std::format(
            "SHIFT+HOME: gun unshift is now {}. {} Watch TWO things, not one: the GUN (must "
            "stay put across the whole separation cycle) and the INTERIOR WALLS (must STILL "
            "shift with each press - both previous attempts locked the gun by stopping the "
            "walls too, and that is the failure this shape exists to avoid). Grep 'GunUnshift'.",
            on ? "ON" : "OFF",
            on ? "Lever V still shifts ctx+0x11F0 for the whole frame, but the clean matrix is "
                 "swapped in for the duration of RenderGuiModels - the job that draws the "
                 "viewmodel - and the shifted one put straight back afterwards."
               : "The shift stands for every draw - this is the OLD behaviour, in which eye 0's "
                 "gun steps through three positions across the separation cycle."));
    } else if (home_pressed) {
        // RealFrameQueryGateProbe arm/rearm (docs/STEREO-RESTART-PLAN.md Task 2,
        // corrected 2026-08-11) - see real_frame_query_gate_probe_armed's
        // declaration above for why this must be human-triggered rather than
        // auto-started from DLL load. Confirmed unbound in DOOMConfig.cfg.
        // Press this immediately before/after INSERT to get a same-moment
        // real-call baseline instead of whatever menu/loading state happened
        // to be running when the DLL was injected.
        real_frame_query_gate_probe_samples.store(0, std::memory_order_relaxed);
        real_frame_query_gate_probe_armed.store(true, std::memory_order_relaxed);
        log::warn(std::format(
            "HOME: RealFrameQueryGateProbe armed - capturing the next {} real frames",
            kMaxRealFrameQueryGateProbeSamples));
    }
    if (f9_pressed && shift_held_now) {
        // SHIFT+F9 cycles the fragment-rejection experiment. Uses the SHARED
        // sample taken at the top of this function - calling GetAsyncKeyState
        // again here is what made this branch unreachable on its first build,
        // because the Lever-G handler above had already consumed the press.
        // See doubled_depth_experiment for what each mode tests.
        const uint32_t next = (doubled_depth_experiment.load(std::memory_order_relaxed) + 1) % 5;
        doubled_depth_experiment.store(next, std::memory_order_relaxed);
        doubled_depth_experiment_applied.store(0, std::memory_order_relaxed);
        doubled_depth_experiment_missed.store(0, std::memory_order_relaxed);
        static const char* kWhat[] = {
            "OFF - mirror depth/stencil left exactly as the doubled call built it",
            "STENCIL=0x00 - if the effects draws stencil-test against 0, this admits them",
            "STENCIL=0xFF - the opposite polarity, so one of modes 1/2 admits either way",
            "DEPTH=1.0 - MEASURED to be the NEAR plane (reversed-Z): this REJECTS the whole "
            "scene and leaves only sky and HUD. Kept as the proof the experiment reaches the "
            "right eye's depth at all; it says nothing about the effects",
            "DEPTH=0.0 - the FAR plane under reversed-Z, so EVERY fragment passes the depth "
            "test. THIS is the real test: if the sparks and fog appear now, depth was what "
            "was rejecting them"};
        log::warn(std::format(
            "SHIFT+F9: DepthExperiment mode {} - {}. Acts on OUR mirror only; DOOM's own "
            "depth is never touched, so the LEFT eye cannot change in any mode. Watch the "
            "RIGHT eye for fire/sparks/fog appearing.",
            next, kWhat[next]));
    }
    // OWN-EDGE, NOT THE CONSUMED BIT. F11 also toggles look injection in
    // openxr_context.cpp and F12 also toggles AlternateEye there, on a
    // different thread. &1 clears the flag for whoever reads next, so those
    // pairs raced: presses landed on one handler or the other at random, and
    // an unknown number of the effects captures this session were actually
    // AlternateEye toggles. 0x8000 reads the CURRENT key state and consumes
    // nothing, so both handlers see every press.
    // F1 - SOLE OWNER, freed by retiring TrueStereo above.
    //
    // Was F11, then CTRL+F11, and both were wrong. F11 also toggles look
    // injection in openxr_context.cpp, and that handler does not check
    // modifiers - so CTRL+F11 flipped head-tracking too. A modifier cannot
    // un-share a key when the other claimant ignores modifiers. One handler
    // per key is the only thing that works, so the probe took a key nothing
    // else claims instead.
    //
    // Own-edge on 0x8000 rather than the consume-on-read &1 bit, so this can
    // never starve another handler or be starved by one.
    static bool f1_prev = false;
    const bool f1_down = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
    const bool f11_edge = f1_down && !f1_prev;
    f1_prev = f1_down;
    if (f11_edge) {
        // LIGHT-CULLING SUPPRESSION TOGGLE (2026-08-18). The matched-pair dump
        // localised eye 1's loss to the DEFERRED LIGHTING output: DOOM's stage
        // has the sparks and the distant lava glow, eye 1's has neither, same
        // frame. Both are EMISSIVE contributions, so the loss is in lighting
        // accumulation - and the light-culling suppression removes four
        // dispatches from exactly that pass. It fixed eye 0's sparks, but it
        // may now be blocking eye 1's emissive lighting.
        //
        // A live toggle answers that in one run instead of two: flip it, look
        // at the right eye, flip back. If the effects return with it OFF, the
        // suppression is too broad and the fix is to mirror those buffers
        // (substituting the indirect one at vkCmdDispatchIndirect, which we
        // hook) rather than to skip the dispatches.
        const bool now_on = !suppress_doubled_light_culling.load(std::memory_order_relaxed);
        suppress_doubled_light_culling.store(now_on, std::memory_order_relaxed);
        log::warn(std::format(
            "F1: compute suppression (\"light-culling\") now {}. ⚠ ITS SCOPE IS BROADER THAN ITS "
            "NAME: the gate is doubled_dispatch_writes_shared_buffer, so ON blocks EVERY compute "
            "dispatch in the doubled call whose bound sets contain a storage buffer - GPU "
            "particle simulation and emitter state as well as light lists. Storage IMAGES get a "
            "private mirror; storage BUFFERS have no counterpart and are skipped outright, so "
            "eye 1's additive layer may simply never be COMPUTED. ON = DOOM's buffers survive "
            "(eye 0 correct, eye 1 may lose fog/fire/sparks). OFF = the doubled call rewrites "
            "them (expect eye 0's sparks to degrade - that is the known trade). WATCH THE RIGHT "
            "EYE for fire/sparks/fog/glow appearing. VERIFY ENGAGEMENT FROM THE "
            "'ComputeSuppression' DELTA, not from this total: total={}",
            now_on ? "ON" : "OFF",
            suppressed_light_culling_dispatches.load(std::memory_order_relaxed)));
    }
    // Own-edge, same reason as F11 above - this arm and AlternateEye in
    // openxr_context.cpp were racing for the same consumed bit.
    // F2 - SOLE OWNER, freed by retiring RenderViewClone above.
    //
    // Was F12, which is THREE claimants at once: DOOM's own screenshot key,
    // AlternateEye in openxr_context.cpp, and this capture. Every capture this
    // session also made the game write a screenshot. See the F1 note.
    static bool f2_prev = false;
    const bool f2_down = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
    const bool f12_edge = f2_down && !f2_prev;
    f2_prev = f2_down;
    // MONITOR EYE-1 VIEW MAKES ONLY THE *DUMP* IMPOSSIBLE, NOT THE WALKS
    // (2026-08-29, second version).
    //
    // The first version refused the WHOLE F2 handler while the monitor view was
    // on. Then step 39 made the monitor view the default on every flat run, and
    // the two changes silently cancelled the FroxelContent test: the owner
    // pressed F2, got a refusal, and DeferredInputMap - which FroxelContent
    // rides - never armed. A guard I added to prevent a silent no-op created
    // one somewhere else.
    //
    // The mirror-dependent part is the matched-pair PPM only. The
    // DeferredInputMap walk, the froxel slice copy, UboContent and the
    // effects-stage capture all read DOOM's own images and are unaffected by
    // where the doubled final pass writes. So the dump is skipped, everything
    // else arms, and the log says which half happened.
    const bool f2_dump_blocked = monitor_eye1_direct.load(std::memory_order_relaxed);
    if (f12_edge && f2_dump_blocked) {
        log::warn("F2: the matched-pair PPM dump is SKIPPED because the monitor eye-1 view is on "
                  "- it leaves final_pass_mirror_color NULL by design, so there would be nothing "
                  "to read and the missing files would look like a capture that found no "
                  "effects. EVERYTHING ELSE ON THIS PRESS STILL ARMED (DeferredInputMap, "
                  "FroxelContent, UboContent, effects-stage). For the PPM pair specifically: "
                  "SHIFT+F5 to turn the monitor view off, then F2.");
    }
    if (f12_edge) {
        // EFFECTS STAGE CAPTURE ON DEMAND (2026-08-18). The sparks are
        // intermittent - a few seconds of nothing, then a burst - so a
        // scripted one-shot would almost certainly catch a dead frame. The
        // owner presses F12 the moment sparks are on screen; the next doubled
        // call captures the post-lighting and post-transparency mirrors, and
        // the present after that writes the two PPMs. Filenames are indexed so
        // repeated presses do not overwrite each other.
        //
        // Requires graded stereo already live (the doubled call must be
        // running). Harmless if it is not: the handles stay null and nothing
        // dumps.
        effects_stage_light_color = VK_NULL_HANDLE;
        effects_stage_xparency_color = VK_NULL_HANDLE;
        effects_stage_count = 0;  // fresh stage list per press
        real_stage_count = 0;
        // Fresh per press too - a stale handle from the last press would dump
        // an image this frame never bound, which is exactly the kind of quiet
        // wrong answer that costs a run.
        deferred_hdr_input_real = VK_NULL_HANDLE;
        deferred_hdr_input_mirror = VK_NULL_HANDLE;
        deferred_hdr_input_extent = {};
        // DRAW PARITY on the SAME frame (2026-08-18). The original table came
        // from a quiet scene in Run A. What matters now is the frame where
        // DOOM demonstrably has sparks: if the doubled call issues the same
        // draw COUNT there but the effects never appear, the remaining
        // explanation is that its indirect draws render ZERO primitives,
        // because their per-frame argument data was consumed by the real call.
        // Same-frame is the point - a table from a different frame cannot
        // settle it.
        draw_parity_done.store(false, std::memory_order_relaxed);
        draw_parity_armed.store(true, std::memory_order_relaxed);
        vertex_parity_lines_real.store(0, std::memory_order_relaxed);
        vertex_parity_lines_doubled.store(0, std::memory_order_relaxed);
        {
            std::scoped_lock lk(vertex_parity_mutex);
            vertex_parity_seen_real.clear();
            vertex_parity_seen_doubled.clear();
        }
        vertex_parity_armed.store(true, std::memory_order_relaxed);
        // DEFERRED INPUT MAP on the SAME frame. Run 1 refuted the light-list
        // hypothesis; this names the deferred chain's sampled inputs and says
        // which of them we hold a mirror for. Per-side budget and per-side
        // dedupe are reset here so each press is a fresh, complete listing
        // rather than a continuation of the last one's exhausted quota.
        deferred_input_budget.reset();
        {
            std::scoped_lock lk(deferred_input_mutex);
            deferred_input_seen_real.clear();
            deferred_input_seen_doubled.clear();
        }
        deferred_input_map_armed.store(true, std::memory_order_relaxed);
        // MirrorCoverage rides the same press - fresh census per press so a
        // second press is a new sample, not an exhausted continuation.
        mirror_coverage_lines.store(0, std::memory_order_relaxed);
        mirror_coverage_misses.store(0, std::memory_order_relaxed);
        // FROXEL CONTENT PROBE on the same press - see froxel_slice_copy. It
        // rides the DeferredInputMap walk, so it needs that armed anyway, and
        // arming it separately would just be a second key for one question.
        for (uint32_t v = 0; v < kFroxelVolumes; ++v) {
            froxel_slice_captured[0][v] = false; froxel_slice_captured[1][v] = false;
            froxel_pending_image[0][v] = VK_NULL_HANDLE;
            froxel_pending_image[1][v] = VK_NULL_HANDLE;
        }
        froxel_compare_ready.store(false, std::memory_order_relaxed);
        froxel_probe_armed.store(true, std::memory_order_relaxed);
        log::warn(
            "FroxelContent ARMED with the DeferredInputMap walk. One Z slice of EACH of the two 3D "
            "volumes each call binds (vol0 = 256x256x32 fmt37, vol1 = 256x256x96 fmt97 - the "
            "likelier scattering volume), copied at END-OF-PASS and hashed AT PRESENT. THE "
            "QUESTION: both calls bind the SAME unmirrored froxel volume, so does the doubled "
            "call still SEE the same bytes in it? DIFFERENT = the volume is consumed or rewritten "
            "between the two calls and eye 1 samples a spent one - consume-once, located. "
            "IDENTICAL = the volume is intact for both and the loss is in whatever READS it. "
            "⚠ VOID if either side reports nonzero=0: two slices of zeros hash alike, and that "
            "would read as 'identical' for the wrong reason.");
        // INDIRECT ARGS on the same press. Fresh slot counters per press so a
        // second press is a new sample and not a continuation of an exhausted
        // one; the records are cleared too so a stale row cannot be read as
        // this frame's.
        indirect_arg_count[0] = 0;
        indirect_arg_count[1] = 0;
        std::memset(indirect_args, 0, sizeof(indirect_args));
        depth_strip_captured[0] = false;
        depth_strip_captured[1] = false;
        depth_compare_ready.store(false, std::memory_order_relaxed);
        vertex_content_count[0] = 0;
        vertex_content_count[1] = 0;
        std::memset(vertex_contents, 0, sizeof(vertex_contents));
        ubo_record_count[0] = 0;
        ubo_record_count[1] = 0;
        std::memset(ubo_records, 0, sizeof(ubo_records));
        {
            std::scoped_lock lk(vertex_content_mutex);
            vertex_content_seen[0].clear();
            vertex_content_seen[1].clear();
        }
        {
            std::scoped_lock lk(ubo_seen_mutex);
            ubo_seen[0].clear();
            ubo_seen[1].clear();
        }
        if (indirect_readback_mapped)
            std::memset(indirect_readback_mapped, 0,
                        static_cast<size_t>(kIndirectReadbackSize));
        indirect_args_probe_armed.store(true, std::memory_order_relaxed);
        // PASS TIMELINE on the same press. Slots are reset so each press
        // records a fresh frame: without this the "filled_attempt==0" test
        // below would accept last press's image as this one's.
        {
            std::scoped_lock lock(chainwalk_mutex);
            for (uint32_t s = 0; s < kChainWalkSlots; ++s) {
                chainwalk_slots[s].observed = false;
                chainwalk_slots[s].filled_attempt = UINT32_MAX;
            }
            for (auto& slot : real_timeline_slots) slot.filled = false;
        }
        pass_timeline_armed.store(true, std::memory_order_relaxed);
        {
            std::scoped_lock lk(chainwalk_mutex);
            beginend_filled = false;
            beginend_real_filled = false;
        }
        timeline_beginend_armed.store(true, std::memory_order_relaxed);
        pass_state_budget.reset();
        {
            std::scoped_lock lk(pass_state_mutex);
            pass_state_seen[0].clear();
            pass_state_seen[1].clear();
        }
        // RE-ARM THE FINAL-STAGE DUMP (2026-08-19). This is now the decisive
        // capture, and it was previously armed ONLY by the scripted test at
        // step 35 - i.e. it fired seconds after PageUp, long before anyone
        // could be standing in front of a spark burst, which is why it has
        // never shown anything useful.
        //
        // finalstage_eye1_graded is EXACTLY the image handed to eye 1:
        // override_source_image = final_pass_mirror_color when graded stereo
        // is live. RenderDoc has now shown that flat, with doubling and graded
        // stereo running, the doubled call's final composite contains the full
        // effects AND correct parallax - so the render is not broken and the
        // question is what eye 1 actually receives in XR.
        // The one line that needs the mirror. Everything else on this press
        // reads DOOM's own images and arms regardless.
        if (!f2_dump_blocked) final_pass_dump_pending.store(true, std::memory_order_relaxed);
        pass_state_parity_armed.store(true, std::memory_order_relaxed);
        effects_stage_dump_ready.store(false, std::memory_order_relaxed);
        const uint32_t idx = effects_capture_index.fetch_add(1, std::memory_order_relaxed);
        effects_capture_suffix.store(idx, std::memory_order_relaxed);
        effects_stage_dump_armed.store(true, std::memory_order_relaxed);
        log::warn(std::format(
            "F2: EffectsStage capture #{} armed - next doubled call grabs the post-lighting and "
            "post-transparency mirrors, dumped after the following present. Press it WHILE "
            "sparks/fire are visible. Files: effects_after_lighting_{}.ppm / "
            "effects_after_transparency_{}.ppm.", idx, idx, idx));
    }
    // Sampled ONCE - &1 consumes the toggle bit, so testing it in both branch
    // conditions would let the SHIFT test swallow a plain press.
    const bool end_pressed = (GetAsyncKeyState(VK_END) & 1) != 0;
    // ⚠ CTRL+END FIRST, reusing end_pressed rather than re-reading the key -
    // GetAsyncKeyState's &1 bit is consumed by the read, so a second call here
    // would swallow the press SHIFT+END and bare END below are waiting for.
    if (end_pressed && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 && !shift_held_now) {
        const bool on = !translation_normalise.load(std::memory_order_relaxed);
        translation_normalise.store(on, std::memory_order_relaxed);
        log::warn(std::format(
            "TranslationNormalise -> {}. apply_translation now displaces by {} world units "
            "along the UNIT right vector. OFF displaces by |right| * offset, which is short by "
            "each site's own row length: renderView 0.946x, the six destination copies 0.866x, "
            "Lever V 1.000x. R22 measured the consequence on the ceiling window - our patched "
            "matrices at 11.354 uu against DOOM's own rebuilt ones at 12.000 uu, commanded 12.0. "
            "normalised={} degenerateSkipped={}. "
            "⚠ JUDGE THIS FROM A CAPTURE FIRST: take one with this ON and re-run "
            "tools/rdc_r22_window_layer_disparity.py - every layer should read 12.000. This is "
            "the change that was reverted on feel on 2026-08-22 with no such prediction to check "
            "against, and it RAISES effective world separation at every site above.",
            on ? "ON (normalised)" : "OFF (original, unnormalised)",
            on ? "exactly the commanded" : "|right| x the commanded",
            translation_normalised_count.load(std::memory_order_relaxed),
            translation_degenerate_count.load(std::memory_order_relaxed)));
    } else if (end_pressed && shift_held_now) {
        // SHIFT+END: EYE 0's SOURCE. The discriminator S3 needs, and the one
        // thing no run so far separates.
        //
        // S3 - shadows sliding with player movement - survives 0.0 separation
        // and does not scale with it, so it is not the camera offset. It also
        // survives a doubled call that dispatches no jobs and sets up no view.
        // It stops on PageUp. But PageUp does THREE things at once: it disarms
        // the doubling, zeroes the offset, AND clears symmetric_stereo_enabled,
        // which is what redirects eye 0 away from DOOM's finished swapchain
        // frame and onto the composite holding image.
        //
        // That third thing is a real candidate in its own right, and it is not
        // contamination at all. The holding image is captured at the frame
        // root's last render pass - deliberately BEFORE DOOM's final grade and
        // HUD, as step 4's own log line says. If any shadow-relevant resolve
        // happens after the frame root - a temporal resolve, contact shadows, a
        // screen-space pass - then the holding image legitimately lacks it, and
        // what the owner is seeing is raw un-resolved shadows rather than
        // damage. That would look exactly like "shadows moving unnaturally with
        // movement" and would be invisible standing still.
        //
        // This toggles ONLY the presentation source. Doubling, mirroring, the
        // offset and every lever keep running untouched:
        //   shadows behave with eye 0 = DOOM's finished frame
        //       -> S3 is the holding image's pipeline stage, not contamination,
        //          and the question becomes WHERE to capture, not what to undo.
        //   shadows still slide
        //       -> S3 is genuinely the doubling, and it survives a doubled call
        //          that renders nothing, which puts it with the second
        //          frame-root entry itself.
        //
        // Eye 1 is unaffected either way; it always comes from its mirror. With
        // eye 0 back on DOOM's finished frame the pair is MISMATCHED - graded
        // against ungraded - so judge shadows only, not eye quality.
        // BACK TO EYE 0's SOURCE, and this time it is the right variable to own
        // this key. The first version of this toggle was void, because these
        // runs were headset-free and it only affects XR submission; the second
        // toggled the composite capture, whose question is now answered (it is
        // not S3). Eye 0's source is the live question again, because the
        // default has changed: eye 0 is now DOOM's presented frame - the
        // monitor image - and this toggles back to the holding composite.
        //
        // STILL XR-ONLY. It cannot change anything on the monitor. The log line
        // prints xrPresentPathHits so a headset-free run says so in its own
        // words instead of inviting the same mistake a third time.
        const bool holding = !symmetric_stereo_enabled.load(std::memory_order_relaxed);
        symmetric_stereo_enabled.store(holding, std::memory_order_relaxed);
        vwc_arm(holding ? "eye0 = holding composite" : "eye0 = DOOM presented frame");
        const uint32_t xr_hits = xr_present_path_hits.load(std::memory_order_relaxed);
        log::warn(std::format(
            "SHIFT+END: eye 0 is now {}. xrPresentPathHits={} - if that is ZERO the headset "
            "path has never run this session and THIS KEY CHANGES NOTHING YOU CAN SEE; the "
            "monitor shows DOOM's presented frame either way. Eye 1 is unaffected: it comes "
            "from its own mirror override, which this flag does not gate.",
            holding ? "the COMPOSITE HOLDING IMAGE (pre-grade, pre-HUD - the old default, kept "
                      "for comparison)"
                    : "DOOM's PRESENTED FRAME - byte-for-byte the monitor image, graded, HUD, "
                      "rendered from the -half camera. This is the baseline",
            xr_hits));
    } else if (end_pressed) {
        // RealCallRedirectProbe arm (docs/STEREO-RESTART-PLAN.md Task 3,
        // control test) - see real_call_redirect_target's declaration above.
        // Confirmed unbound in DOOMConfig.cfg. One-shot: fires on the NEXT
        // real RenderDepth+RenderOpaque sequence only, then auto-disarms -
        // that one frame's real depth+color output is diverted away from
        // the screen (a visible one-frame glitch is expected).
        const bool target_ready = ensure_real_call_redirect_target_ready();
        const bool pass1_ready = ensure_real_call_redirect_pass1_ready();
        const bool depth_ready = ensure_real_call_redirect_depth_ready();
        real_call_redirect_dump_done.store(false, std::memory_order_relaxed);
        // Fresh capture window each press, same reasoning as HOME's reset of
        // real_frame_query_gate_probe_samples - without this, these two
        // probes' fixed sample caps would exhaust across repeated END
        // presses within one session instead of covering each new capture.
        render_depth_pipeline_probe_samples.store(0, std::memory_order_relaxed);
        render_depth_draw_probe_samples.store(0, std::memory_order_relaxed);
        real_call_redirect_armed.store(true, std::memory_order_relaxed);
        log::warn(std::format(
            "END: RealCallRedirectProbe armed - targetReady={} pass1Ready={} depthReady={} "
            "(one-shot, next real frame only, expect a brief visible glitch)",
            target_ready, pass1_ready, depth_ready));
    }
    // Sampled ONCE - the &1 bit is cleared by the read. Same pattern as
    // SHIFT+F4 and SHIFT+F5 above.
    const bool f6_pressed = (GetAsyncKeyState(VK_F6) & 1) != 0;
    if (f6_pressed && shift_held_now) {
        // SHIFT+F6: THE FROXEL FIX (P2), LIVE - see kFixDoubledLightingVolume.
        //
        // Reopened because of what the 2026-08-29 matched pair actually shows.
        // At pass 15, the FIRST full-resolution HDR stage, eye 1 has already
        // lost the volumetric haze and the spark burst while KEEPING its local
        // analytic lighting: eye 0's landscape is veiled in dust, eye 1's is
        // crisp. Everything downstream of p15 is therefore exonerated, and the
        // signature is a missing froxel volume rather than a missing pass.
        //
        // P1 measured exactly that divergence: at the deferred lighting job all
        // ELEVEN of the doubled call's descriptor sets hold at binding 2 - the
        // froxel slot - a duplicate of binding 3's non-volume view, where the
        // real call's sets hold a 3D volume. P1W confirmed it at the WRITE
        // site, so DOOM itself writes it that way when we re-invoke.
        //
        // ⚠ THIS IS THE PARTIAL FIX, AND THAT MATTERS FOR READING IT. The real
        // call binds at least TWO distinct views at this binding across its
        // eleven sets; this substitutes ONE into all eleven, so roughly four
        // get a volume the real call does not use there. So:
        //   fog RETURNS, even partly or wrongly -> mechanism CONFIRMED, and the
        //       per-set version is worth building.
        //   fog UNCHANGED -> weaker than it looks, because a majority-correct
        //       substitution is not a correct one. Judge it against the
        //       substitution count, not against the press.
        const bool want = !kFixDoubledLightingVolume.load(std::memory_order_relaxed);
        kFixDoubledLightingVolume.store(want, std::memory_order_relaxed);
        lighting_volume_substitutions.store(0, std::memory_order_relaxed);
        lighting_volume_missing.store(0, std::memory_order_relaxed);
        vwc_arm(want ? "froxel fix ON" : "froxel fix OFF");
        log::warn(std::format(
            "SHIFT+F6: FROXEL FIX (P2) now {}. Counters zeroed, so what follows is THIS arm. "
            "Takes effect from the NEXT frame - the real call records the volume, then the "
            "doubled call substitutes it. VERIFY FROM 'FroxelFix' / 'DfSubst' counts, not from "
            "this press: substitutions climbing means it engaged, substitutions=0 with "
            "missing>0 means the real call never bound a 3D volume at binding {} this frame and "
            "the run is VOID rather than negative. WATCH: does the volumetric haze come back "
            "into the landscape, and do the sparks return? Stand where you took the captures.",
            want ? "ON" : "OFF", kLightingVolumeBinding));
    } else if (f6_pressed) {
        const bool now_enabled = !redirect_extra_call_enabled.load(std::memory_order_relaxed);
        if (now_enabled) {
            redirect_attempt_count.store(0, std::memory_order_relaxed);
            stereo_preview_enabled.store(false, std::memory_order_relaxed);
            render_view_clone_test_enabled.store(false, std::memory_order_relaxed);
            true_stereo_enabled.store(false, std::memory_order_relaxed);
            upstream_reinvoke_enabled.store(false, std::memory_order_relaxed);
            widen_gather_probe_enabled.store(false, std::memory_order_relaxed);
            stereo_dual_eye_enabled.store(false, std::memory_order_relaxed);
            scoped_reinvoke_enabled.store(false, std::memory_order_relaxed);
        }
        redirect_extra_call_enabled.store(now_enabled, std::memory_order_relaxed);
        log::warn(now_enabled
            ? "F6: offscreen redirect ARMED - has NO effect unless F7 is also armed (redirects F7's extra "
              "call's output into our own framebuffer; genuinely new risk - GPU corruption/hang is possible "
              "and NOT recoverable the way a caught CPU exception is; watch the headset closely)"
            : "F6: offscreen redirect disabled");
    }
    // Shift read locally - shift_held_now is declared further down. 0x8000
    // Shift read locally; 0x8000 consumes nothing. Own-edge because F12 USED TO
    // also toggle AlternateEye in openxr_context.cpp with a handler that ignored
    // modifiers - a modifier cannot un-share a key when the other claimant does
    // not check them. That handler was retired on 2026-08-24
    // (kXrAlternateEyeKeyEnabled), so SHIFT+F12 is now the sole plugin claimant
    // and RenderDoc's F12 binding is already cleared by rdc_configure_if_present.
    // The own-edge detector STAYS: it is strictly safer than the consumed &1
    // bit, it costs nothing, and DOOM's own screenshot key is still F12.
    static bool f12s_prev = false;
    const bool f12s_down = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
    const bool f12s_edge = f12s_down && !f12s_prev;
    f12s_prev = f12s_down;
    if (f12s_edge && (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) {
        const bool want = !force_doubled_serialisation.load(std::memory_order_relaxed);
        force_doubled_serialisation.store(want, std::memory_order_relaxed);
        doubled_serialisations.store(0, std::memory_order_relaxed);
        vwc_arm(want ? "forced serialisation ON" : "forced serialisation OFF");
        log::warn(std::format(
            "SHIFT+F12: forced serialisation now {}. Counter zeroed, so what follows is THIS "
            "arm.\n"
            "WHAT IT TESTS: the last RenderDoc behaviour standing. Usage widening is eliminated "
            "(ON, zero refusals, no change), Test A eliminated the validation layer and Test B "
            "eliminated slowness. What RenderDoc still does that we do not is SERIALISE - it "
            "stops GPU work overlapping, so the real call's writes have COMPLETED before the "
            "doubled call reads them. Every descriptor we compare is identical; what is IN those "
            "resources at read time is the one thing left that can differ.\n"
            "⚠ ENGAGEMENT: 'ForcedSerialisation' lines and doubledSerialisations must CLIMB, one "
            "per doubled call. Zero means last_frame_root_cmd_buffer was null and no barrier was "
            "recorded - VOID, not negative.\n"
            "WATCH: the haze and the god rays. Expect a frame-rate cost; that is the drain, not "
            "a hang.",
            want ? "ON" : "OFF"));
    }
    // Shift read locally; 0x8000 consumes nothing.
    const bool f11_shift_held = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    static bool f11k_prev = false;
    const bool f11k_down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    const bool f11k_edge = f11k_down && !f11k_prev;
    f11k_prev = f11k_down;
    if (f11k_edge && f11_shift_held) {
        // SHIFT+F11: VOLUME RESTORE - see the block in the descriptor-write hook.
        //
        // Own-edge on 0x8000 rather than the consumed &1 bit because F11 USED TO
        // also toggle look injection in openxr_context.cpp with a handler that
        // ignored modifiers. A modifier cannot un-share a key when the other
        // claimant does not check them - so this reads the edge itself and
        // starves nobody. That toggle was demoted to a switch file on
        // 2026-08-24, so the only other F11 claimant left is RenderDoc's capture
        // key, which is deliberately bound to F11 and is NOT ours to move. The
        // own-edge detector stays for exactly that reason.
        const bool want = !volume_restore_enabled.load(std::memory_order_relaxed);
        volume_restore_enabled.store(want, std::memory_order_relaxed);
        volume_restores.store(0, std::memory_order_relaxed);
        volume_restore_lines.store(0, std::memory_order_relaxed);
        vwc_arm(want ? "volume restore ON" : "volume restore OFF");
        log::warn(std::format(
            "SHIFT+F11: VOLUME RESTORE now {}. Counters zeroed, so what follows is THIS arm.\n"
            "WHAT IT DOES: while inside the doubled call, if DOOM writes a NON-3D view into a "
            "binding where the real call of the SAME FRAME wrote a 3D froxel volume, the volume "
            "is put back. It repairs the ASKED divergence P1 measured at pass 5 - real binds 2 "
            "volumes, the doubled call binds 0 - which CloneAudit showed is NOT our redirection "
            "(81 clones, none volume-carrying) but DOOM declining to bind them on re-invocation.\n"
            "⚠ ENGAGEMENT: 'volumeRestores' must CLIMB. Zero means DOOM never wrote a non-3D "
            "view at a recorded binding during the doubled call, and the rung is VOID rather "
            "than negative - exactly the trap the last four probes fell into.\n"
            "WATCH: the haze and the god rays in the canyon.",
            want ? "ON" : "OFF"));
    }
    // consumes nothing, so this cannot starve it.
    const bool f3_shift_held = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool f3_pressed = (GetAsyncKeyState(VK_F3) & 1) != 0;
    if (f3_pressed && f3_shift_held) {
        // SHIFT+F3: P1 READ CLASSIFICATION, ARMED ALONE (2026-08-22).
        //
        // WHY THIS AND NOT DeferredInputMap. The parity result I reported -
        // "56 bindings matched, 0 differing" at and before p15 - is VACUOUS,
        // and its own instrument says so. probe_deferred_inputs walks
        // descriptor_image_sets, the record of what DOOM WROTE INTO ITS OWN
        // SETS. That is the ASKED side, before redirection substitutes
        // anything. Both calls ask with the same sets by construction, so
        // "identical" there was never capable of being anything else.
        //
        // AND THE POLARITY IS INVERTED FROM WHAT I ASSUMED. For a correctly
        // redirected doubled call the bound images SHOULD DIFFER: the real
        // call reads DOOM's chain, the doubled call should read OUR MIRRORS.
        // The working-arm capture is explicit - at the deferred lighting pass,
        // real rdMIR=0 rdREAL=9, DOUBLED rdMIR=8 rdREAL=1. Eight mirror-chain
        // reads on the doubled side. Identical bound images would mean
        // redirection is NOT resolving, which is a defect rather than health.
        //
        // P1 is the instrument built for exactly this: it records ASKED and
        // BOUND separately, per pass, per side, and classifies every sampled
        // view as MIR / REAL / VOL / OTH by membership of our mirror set.
        // Armed ALONE here - no census, no PPM series, no P1W write capture -
        // and it self-disarms after ONE complete doubled call.
        p1_done.store(false, std::memory_order_relaxed);
        p1_detail_lines.store(0, std::memory_order_relaxed);
        p1_rva_count = 0;
        p1_rva_overflow = 0;
        std::memset(p1_rva_keys, 0, sizeof(p1_rva_keys));
        std::memset(p1_real_asked_rva, 0, sizeof(p1_real_asked_rva));
        std::memset(p1_doubled_asked_rva, 0, sizeof(p1_doubled_asked_rva));
        std::memset(p1_doubled_bound_rva, 0, sizeof(p1_doubled_bound_rva));
        p1_armed.store(true, std::memory_order_relaxed);
        emit_clone_audit();
        vwc_arm("P1 read classification armed alone");
        log::warn(
            "SHIFT+F3: P1 READ CLASSIFICATION ARMED ALONE - one frame, self-disarming. Grep "
            "'P1' for the per-RVA table.\n"
            "READ THE **BOUND** ROWS, NOT THE ASKED ONES. Asked is DOOM's own sets and is "
            "identical on both sides by construction - that is not a result. Bound is what "
            "redirection actually put in front of the shader.\n"
            "THE TARGET, from the working-arm capture at the deferred lighting pass: real "
            "rdMIR=0 rdREAL=9 | DOUBLED rdMIR=8 rdREAL=1. So the doubled BOUND row SHOULD show "
            "mirror-chain reads. rdMIR~0 with rdREAL high on the doubled bound row means "
            "redirection is not resolving for those reads - the doubled call is sampling DOOM'S "
            "OWN eye-0 chain - and that is the defect, not the healthy case.\n"
            "⚠ CONTROL: if the table does not emit at all, or a side's row is empty, NOTHING may "
            "be concluded - P1 waits for a converged frame with redirection live and will simply "
            "not spend the arm otherwise.");
    } else if (f3_pressed) {
        if (stereo_preview_enabled.load(std::memory_order_relaxed)) {
            stereo_preview_enabled.store(false, std::memory_order_relaxed);
            log::warn("F3: StereoPreview disabled");
        } else {
            check_offscreen_blit_capability();
            if (!offscreen_blit_supported.load(std::memory_order_relaxed)) {
                log::error("F3: StereoPreview NOT armed - this GPU/driver does not report blit-src "
                    "support for the offscreen color format (see the capability-check log line above); "
                    "nothing would display, so refusing to arm rather than attempt it blind.");
            } else {
                stereo_preview_attempt_count.store(0, std::memory_order_relaxed);
                // Mutual exclusion with F1/F2/F6/F7: never run two
                // independent extra-call mechanisms against the same frame.
                render_opaque_double_call_enabled.store(false, std::memory_order_relaxed);
                redirect_extra_call_enabled.store(false, std::memory_order_relaxed);
                render_view_clone_test_enabled.store(false, std::memory_order_relaxed);
                true_stereo_enabled.store(false, std::memory_order_relaxed);
                upstream_reinvoke_enabled.store(false, std::memory_order_relaxed);
                widen_gather_probe_enabled.store(false, std::memory_order_relaxed);
                stereo_dual_eye_enabled.store(false, std::memory_order_relaxed);
                scoped_reinvoke_enabled.store(false, std::memory_order_relaxed);
                stereo_preview_enabled.store(true, std::memory_order_relaxed);
                log::warn(std::format(
                    "F3: StereoPreview ARMED - offscreen redirect content will be presented LIVE to "
                    "both eyes continuously (same image both eyes, no parallax yet - that's a separate, "
                    "deliberately-not-yet-attempted step, see docs/HANDOFF-NEXT-SESSION.md), for up to "
                    "{} frames (several minutes) or until disarmed (press F3 again). Same risk class as "
                    "F6+F7 (GPU corruption/hang possible, not SEH-catchable) now sustained much longer - "
                    "watch closely, especially in the first several seconds.",
                    kMaxStereoPreviewAttempts));
            }
        }
    }
    // RETIRED 2026-08-19 - F2 RELEASED. See the F1 note.
    if (kLegacyStereoModesEnabled && (GetAsyncKeyState(VK_F2) & 1) != 0) {
        const bool now_enabled = !render_view_clone_test_enabled.load(std::memory_order_relaxed);
        if (now_enabled) {
            render_view_clone_test_count.store(0, std::memory_order_relaxed);
            // Mutual exclusion with F1/F3/F6/F7/F9: never run two independent
            // extra-call mechanisms against the same frame.
            stereo_preview_enabled.store(false, std::memory_order_relaxed);
            render_opaque_double_call_enabled.store(false, std::memory_order_relaxed);
            redirect_extra_call_enabled.store(false, std::memory_order_relaxed);
            true_stereo_enabled.store(false, std::memory_order_relaxed);
            upstream_reinvoke_enabled.store(false, std::memory_order_relaxed);
            widen_gather_probe_enabled.store(false, std::memory_order_relaxed);
            stereo_dual_eye_enabled.store(false, std::memory_order_relaxed);
            scoped_reinvoke_enabled.store(false, std::memory_order_relaxed);
        }
        render_view_clone_test_enabled.store(now_enabled, std::memory_order_relaxed);
        log::warn(now_enabled
            ? "F2: RenderViewClone Stage 1 test ARMED - clones the render-view object and "
              "temporarily redirects its pointer slot to the clone (identical content, no camera "
              "change) during a real-framebuffer extra call, isolating whether anything else reads "
              "that slot concurrently. Does NOT write into DOOM's shared render-view object at any "
              "point. Watch for visual glitches/crash exactly like F7's original re-entrancy test."
            : "F2: RenderViewClone Stage 1 test disabled");
    }
    // RETIRED 2026-08-19 - F1 RELEASED. TrueStereo is a superseded extra-call
    // stereo mode; frame-root doubling plus graded stereo replaced it and it
    // has not been used in weeks. The body is kept (dead) rather than deleted
    // so the mechanism can be revived, but it no longer OWNS A KEY.
    //
    // Keys are now single-owner by rule. F11 and F12 each had three claimants
    // - this file, openxr_context.cpp, and DOOM itself - and modifiers do not
    // help, because the rival handlers do not check modifiers: CTRL+F11 still
    // toggled look injection. The only fix is one handler per key.
    if (kLegacyStereoModesEnabled && (GetAsyncKeyState(VK_F1) & 1) != 0) {
        if (true_stereo_enabled.load(std::memory_order_relaxed)) {
            true_stereo_enabled.store(false, std::memory_order_relaxed);
            log::warn("F1: TrueStereo disabled");
        } else {
            check_offscreen_blit_capability();
            if (!offscreen_blit_supported.load(std::memory_order_relaxed)) {
                log::error("F1: TrueStereo NOT armed - this GPU/driver does not report blit-src "
                    "support for the offscreen color format (see the capability-check log line above); "
                    "eye 1 would show nothing, so refusing to arm rather than attempt it blind.");
            } else {
                true_stereo_attempt_count.store(0, std::memory_order_relaxed);
                // Mutual exclusion with F2/F3/F6/F7: never run two
                // independent extra-call mechanisms against the same frame.
                stereo_preview_enabled.store(false, std::memory_order_relaxed);
                render_opaque_double_call_enabled.store(false, std::memory_order_relaxed);
                redirect_extra_call_enabled.store(false, std::memory_order_relaxed);
                render_view_clone_test_enabled.store(false, std::memory_order_relaxed);
                upstream_reinvoke_enabled.store(false, std::memory_order_relaxed);
                widen_gather_probe_enabled.store(false, std::memory_order_relaxed);
                stereo_dual_eye_enabled.store(false, std::memory_order_relaxed);
                scoped_reinvoke_enabled.store(false, std::memory_order_relaxed);
                true_stereo_enabled.store(true, std::memory_order_relaxed);
                // Phase 0 (deleted plan; kept as a same-run sweep harness): one F1 press now
                // walks kPhase0SweepOffsets in sequence from the same
                // standing position, dumping slot 1 once per magnitude with
                // per-band stats appended to doomvr.log. Sweep advances on
                // the present thread after each dump completes; the atomic
                // g_true_stereo_translation_offset is the only field the
                // worker thread reads. Also re-arms both offscreen_dump_done
                // flags so slot 0 gets a fresh capture at the start of the
                // sweep and slot 1 captures its first magnitude.
                g_phase0_sweep_active = true;
                g_phase0_sweep_index = 0;
                g_true_stereo_translation_offset.store(
                    kPhase0SweepOffsets[0], std::memory_order_relaxed);
                offscreen_dump_done[0].store(false, std::memory_order_relaxed);
                offscreen_dump_done[1].store(false, std::memory_order_relaxed);
                log::warn(std::format(
                    "F1: TrueStereo ARMED - Phase 0 magnitude sweep active. Eye 0 stays DOOM's normal "
                    "frame; eye 1 gets a redirected extra RenderOpaque call. Two separate camera edits "
                    "happen: a {:.4f} rad toe-in rotation on a CPU-side render-view CLONE (confirmed "
                    "inert - the GPU never reads this object fresh, kept only for its own diagnostics) "
                    "and, the part that actually reaches the image, a TrueStereoGpuWrite TRANSLATION "
                    "written directly into the extra call's GPU-mapped uniform record. Sweep magnitudes "
                    "(world units): {:.2f}, {:.2f}, {:.2f}, {:.2f} - one dump each, from the same "
                    "standing position, filenames doomvr_offscreen_dump_slot1_o<N>.ppm. Attempt cap {}.",
                    kTrueStereoToeInRadians,
                    kPhase0SweepOffsets[0], kPhase0SweepOffsets[1],
                    kPhase0SweepOffsets[2], kPhase0SweepOffsets[3],
                    kMaxTrueStereoAttempts));
            }
        }
    }
    // PageUp: FrameRootProbe (2026-08-16). Read-only, mutates nothing, so it
    // needs no mutual exclusion with the extra-call mechanisms - but it is a
    // one-shot burst by design: the budget is consumed in a fraction of a
    // second and the probe disarms itself. Press again for another capture.
    // SharedDepthIsolation A/B (Scroll Lock). Flips the doubled call's
    // RenderDepth on and off WHILE ARMED, so the comparison is two states of
    // one run rather than two launches. Judging eye 0 across a toggle is far
    // more reliable than judging it across a relaunch, where lighting, scene
    // state and standing position all drift.
    if ((GetAsyncKeyState(VK_SCROLL) & 1) != 0) {
        // CTRL+ScrollLock selects the FLOOR test instead: dispatch no jobs at
        // all, reducing the doubled call to frame-root scaffolding. That
        // measures how much of eye 0's damage no rendering change can ever
        // remove. Live-toggleable so the floor, the depth cut and the full
        // configuration can all be compared from ONE standing position -
        // relaunching between them lets scene state and lighting drift.
        // SHIFT, not CTRL: Windows delivers CTRL+ScrollLock as VK_CANCEL - it
        // is the traditional Break combination - which the Pause handler
        // samples and treats as a disarm. Same collision §F already documents
        // for CTRL+Pause. SHIFT+Pause is taken by A/B mode, but SHIFT with
        // ScrollLock is free and generates no Break.
        if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) {
            // REPURPOSED from the floor test, which has served its purpose -
            // the frame-root residual is identified and no longer needs a
            // live toggle.
            //
            // "There MAY be some stereo separation" is the one question this
            // whole project exists to answer, and it must not be settled by
            // squinting. Cycling the separation live makes it decisive: at
            // 0.0 the eyes are the SAME viewpoint and must fuse flat; at 12.0
            // (4x DOOM's own 3.0 eye-to-eye) any real separation becomes
            // impossible to miss, and near geometry must shift MORE than far
            // geometry. If nothing changes across the cycle, the separation is
            // not real however convincing it looked.
            const float current = frame_root_double_ipd.load(std::memory_order_relaxed);
            const float next = current < 0.5f ? kFrameRootDoubleLiveIpd
                             : current < 6.0f ? 12.0f
                                              : 0.0f;
            frame_root_double_ipd.store(next, std::memory_order_relaxed);
            frame_root_zero_offset.store(next < 0.01f, std::memory_order_relaxed);
            // Re-arm the view-write census on every separation change, so the
            // log carries one block per separation value and the offset each
            // side actually applied can be read against the gun's three
            // positions on the monitor.
            vwc_arm(std::format("separation {:.1f} uu", next).c_str());
            log::warn(std::format(
                "SHIFT+ScrollLock: eye separation now {:.1f} uu ({}). Near geometry must move "
                "MORE than far geometry - that is parallax; a uniform shift is not.",
                next,
                next < 0.01f ? "ZERO - both eyes are the same viewpoint, must fuse FLAT"
                : next > 6.0f ? "EXAGGERATED, 4x DOOM's own - separation should be obvious"
                              : "DOOM's own eye-to-eye separation"));
        } else {
            // REPURPOSED. Plain ScrollLock used to toggle RenderDepth, which
            // was an isolation test that has served its purpose - with depth
            // private that toggle is inert by design. It now drops the JOB
            // FILTER, which is the remaining reason eye 1 has no lighting,
            // wind or post: the doubled call currently runs only RenderDepth,
            // ResolveDepth and RenderOpaque.
            //
            // Live-toggleable because unfiltering is the risky one: the census
            // measured 7 distinct framebuffers with the filter ON and 77 render
            // passes with it OFF, so this can allocate a lot more mirrors.
            // Starting filtered means the run begins in the known-good state
            // and you toggle INTO the experiment, not out of it.
            const bool unfiltered = !mirror_unfiltered.load(std::memory_order_relaxed);
            mirror_unfiltered.store(unfiltered, std::memory_order_relaxed);
            log::warn(std::format(
                "ScrollLock: job filter is now {}. Doubled call runs {}. Mirror memory so far "
                "{} MB across {} framebuffers.",
                unfiltered ? "OFF" : "ON",
                unfiltered ? "EVERY job - expect lighting, wind and post in eye 1, and many "
                             "more mirrors"
                           : "only RenderDepth/ResolveDepth/RenderOpaque (G-buffer only)",
                mirror_bytes_allocated.load(std::memory_order_relaxed) / (1024 * 1024),
                mirror_created_count.load(std::memory_order_relaxed)));
        }
    }
    // Sampled ONCE into a local: GetAsyncKeyState's low bit is "pressed since
    // the last call to this function for this key" and READING IT CLEARS IT.
    // An `if (PRIOR && shift) ... else if (PRIOR)` written with two calls
    // therefore eats the plain-PageUp press entirely - the first call
    // consumes the bit, the second returns 0 and FrameRootProbe silently
    // stops arming. Same reason the Pause handler samples pause_pressed and
    // break_pressed into locals up at the top.
    const bool pageup_pressed = (GetAsyncKeyState(VK_PRIOR) & 1) != 0;
    // ⚠ PAGEUP IS MODIFIER-INSENSITIVE (2026-08-17, after run 24 was lost).
    //
    // The census used to live on SHIFT+PageUp. It is now step 4 of the
    // scripted test, so that branch is dead weight - and worse, it was a
    // TRAP. The owner, in a headset and unable to see the keyboard, hit
    // SHIFT+PageUp three times meaning to start the test; each press landed
    // on the legacy handler, which refused (correctly - doubling was not
    // running) and then told them to perform a stale multi-key procedure
    // that Rule 8 retired. A near-miss on the intended key must not send
    // someone further down the wrong path.
    //
    // So: any PageUp, with or without shift, runs the scripted test. The
    // legacy SHIFT+PageUp DescriptorCensus branch was deleted 2026-09-03.
    if (pageup_pressed) {
        // PLAIN PAGEUP IS "RUN THE CURRENT TEST", ONE PRESS, START TO
        // FINISH. It was FrameRootProbe, a legacy read-only topology dump;
        // that branch was deleted 2026-09-03.
        //
        // Chosen because it sits directly above PageDown, so it is findable
        // by touch while wearing a headset, and because the scripted test
        // does the PageDown step itself - so the whole procedure is: launch
        // injector, launch DOOM, press PageUp once.
        if (scripted_test_active.load(std::memory_order_relaxed)) {
            scripted_test_active.store(false, std::memory_order_relaxed);
            frame_root_double_enabled.store(false, std::memory_order_relaxed);
            // Undo the symmetric-stereo state too. Leaving it set would keep
            // the REAL call's camera shifted by +half after doubling stopped,
            // so the flat/monitor view would stay off-centre with nothing
            // running to explain it - a state the owner cannot get out of
            // except by restarting the game.
            symmetric_stereo_enabled.store(false, std::memory_order_relaxed);
            frame_root_full_frame.store(false, std::memory_order_relaxed);
            g_stereo_offset.store(0.0f, std::memory_order_relaxed);
            // The second press is the owner's stop button, so it has to stop
            // EVERYTHING that press started - including the final-stage
            // doubling, which is the newest and least proven of it.
            final_pass_double_armed.store(false, std::memory_order_relaxed);
            graded_stereo_enabled.store(false, std::memory_order_relaxed);
            // The grade-gap ladder goes back to rung 0 with them. Leaving
            // graded_mode at 2 while both booleans are false would make the
            // next BaselinePair line claim a rung the run is not in - and the
            // whole point of stamping the rung is that it is trustworthy.
            graded_mode.store(0, std::memory_order_relaxed);
            baseline_pair_burst.store(0, std::memory_order_relaxed);
            monitor_eye1_direct.store(false, std::memory_order_relaxed);
            monitor_eye1_passes.store(0, std::memory_order_relaxed);
            caller_specificity_armed.store(false, std::memory_order_relaxed);
            // Put the A/B back in its shipping state. Arm B leaves the
            // suppression OFF, and a second press that stopped the test but
            // left it off would silently start the NEXT press in arm B - the
            // exact "a toggle earlier in the same process leaves it false"
            // failure that forcing it at arm time exists to prevent.
            light_cull_ab_active.store(false, std::memory_order_relaxed);
            uniform_diff_armed.store(false, std::memory_order_relaxed);
            suppress_doubled_light_culling.store(true, std::memory_order_relaxed);
            // Test B's throttle is the owner's most visible state, so the stop
            // button has to return it. A game left at 3 fps after the test was
            // stopped reads as a hang, not as a leftover toggle.
            present_throttle_armed.store(false, std::memory_order_relaxed);
            // P1 waits for a converged frame, so an arm that never got one
            // would otherwise still be set at the next press and fire against
            // a run that was not set up for it.
            p1_armed.store(false, std::memory_order_relaxed);
            // V1 arms with P1W at step 1 and only disarms in its own emit, so
            // a press that stops the test before the table emits would leave
            // it capturing into the NEXT press's run. Emit first (idempotent),
            // so a stop press publishes what was captured rather than binning
            // it - then disarm.
            v1_emit_table();
            v1_armed.store(false, std::memory_order_relaxed);
            // X1 changes what the GPU reads, so the stop button must stop it -
            // the arm-time forcing discipline in reverse.
            if (x1_replay_ring_offsets.exchange(false, std::memory_order_relaxed)) {
                log::warn(std::format(
                    "X1 DISARMED by the stop press. Final: replayed={} missing={} "
                    "countMismatch={}.",
                    x1_replayed.load(std::memory_order_relaxed),
                    x1_missing.load(std::memory_order_relaxed),
                    x1_count_mismatch.load(std::memory_order_relaxed)));
            }
            // R1: same discipline as V1's. Emit first (idempotent - a manifest
            // already published on its own frame is a no-op here), THEN
            // disarm, so a press that stops the run before the capture frame
            // completed publishes whatever was recorded rather than binning it.
            if (df_armed.exchange(false, std::memory_order_relaxed) ||
                !df_emitted.load(std::memory_order_relaxed)) {
                df_emit_manifest("stop press - fallback emit");
            }
            // D1 changes engine state, so the stop button must stop it. A
            // behavioural toggle left on after the test that armed it is the
            // exact failure the arm-time forcing discipline exists to prevent.
            d1_enabled.store(false, std::memory_order_relaxed);
            // F1 and G1 change engine behaviour; the stop button must stop both.
            f1_enabled.store(false, std::memory_order_relaxed);
            g1_suppress_particle_sim.store(false, std::memory_order_relaxed);
            log::warn("PageUp: scripted test ABORTED by second press; symmetric stereo OFF, "
                      "final-stage doubling OFF, camera offset returned to 0, light-culling "
                      "suppression returned to ON (the A/B's arm A / shipping state)");
        } else {
            scripted_test_step.store(0, std::memory_order_relaxed);
            scripted_test_frames_in_step.store(0, std::memory_order_relaxed);
            scripted_test_waits.store(0, std::memory_order_relaxed);
            // Seeded HERE and not at arm time: the arm block is itself a late
            // scripted step, so anything seeded there would not exist until the
            // test had already run past the steps that read it.
            g_barfix_on.store(wrap::config_barfix(), std::memory_order_relaxed);
            g_rpfix_on.store(wrap::config_rpfix(), std::memory_order_relaxed);
            scripted_test_active.store(true, std::memory_order_relaxed);
            log::warn(scripted_test_run_a.load(std::memory_order_relaxed)
                ? "PageUp: RUN A armed (review P2). Waits for the world to be rendering, then: "
                  "pipeline inline, doubling armed, mirrors converge (grep 'MirrorConvergence' - "
                  "the gate has never executed before), redirection on, DrawParity for ONE frame, "
                  "the descriptor census for ONE doubled call, summaries, stop. Log-only "
                  "throughout: NO captures, NO PPMs, no long freeze, and it self-disarms. Stand "
                  "still for about 15 s."
                : scripted_test_symmetric.load(std::memory_order_relaxed)
                ? "PageUp: SYMMETRIC STEREO armed (STEP 8). Waits for the world to be rendering, "
                  "then: pipeline inline, doubling armed, ~7 s for mirrors to converge, read "
                  "redirection on, and finally BOTH EYES from the same pipeline stage - the real "
                  "call at +half into eye 0, the doubled call at -half into eye 1. NO captures, "
                  "NO freeze, and it does NOT self-disarm: look around. Neither eye has DOOM's "
                  "final grade or HUD - that is expected, both are pre-final. Press PageUp again "
                  "to stop and re-centre the camera."
                : "PageUp: DIAGNOSTIC sequence armed (STEP 6.6). Inline pipeline, doubling, "
                  "mirrors, redirection, census, chain-walk map, then the A/B capture at 20 uu. "
                  "Writes up to TEN PPMs, so expect a long freeze - that is the test working, "
                  "not a hang. Stand still throughout.");
        }
    }
    // r_useSMP does NOT stay where we put it. Live evidence (2026-08-16): it
    // was 1 at 11:50:03 (FrameRootDouble armed successfully, which requires
    // it) and read 2 again at 11:50:07, with no PageDown press in between -
    // DOOM writes it back on its own within seconds. Since the sync flag is
    // recomputed from this cvar every frame at DOOM+0x1558BF5, letting it
    // revert silently drops the whole render pipeline back to threaded mid
    // experiment, which would quietly invalidate any longer test.
    //
    // So while the toggle is engaged, re-assert it every frame. Cheap: two
    // small reads and, only on drift, three small writes.
    if (force_single_threaded_applied.load(std::memory_order_relaxed)) {
        const uintptr_t doom_base =
            reinterpret_cast<uintptr_t>(GetModuleHandleW(L"DOOMx64vk.exe"));
        if (doom_base) {
            const uintptr_t obj = doom_base + kUseSmpCvarRva;
            int32_t cur = 0;
            if (safe_read_bytes(obj + 0x30, &cur, sizeof(cur)) && cur != 1) {
                const int32_t iv = 1; const float fv = 1.0f; const char one[2] = {'1', '\0'};
                safe_write_bytes(obj + 0x1c, one, sizeof(one));
                safe_write_bytes(obj + 0x30, &iv, sizeof(iv));
                safe_write_bytes(obj + 0x34, &fv, sizeof(fv));
                const uint32_t n =
                    use_smp_reassert_count.fetch_add(1, std::memory_order_relaxed);
                if (n < 10 || n % 500 == 0) {
                    log::warn(std::format(
                        "r_useSMP drifted back to {} and was re-asserted to 1 (re-assert #{})",
                        cur, n));
                }
            }
        }
    }
    // Pause/Break: FrameRootDouble Stage 1. Refuses to arm unless the render
    // pipeline is actually inline (r_useSMP == 1) - doubling the frame root
    // while the job graph is live is not a test to run by accident.
    // Ctrl+Pause is the BREAK combination and Windows delivers it as
    // VK_CANCEL, not VK_PAUSE - which is why Shift+Pause has always worked
    // and Ctrl+Pause was silent. Sample both, and treat VK_CANCEL as the
    // blit-only control since that is literally the key the OS sends.
    const bool pause_pressed = (GetAsyncKeyState(VK_PAUSE) & 1) != 0;
    const bool break_pressed = (GetAsyncKeyState(VK_CANCEL) & 1) != 0;
    if (pause_pressed || break_pressed) {
        if (frame_root_double_enabled.load(std::memory_order_relaxed)) {
            frame_root_double_enabled.store(false, std::memory_order_relaxed);
            // The census only ever completes at the END of a doubled call.
            // Disarming doubling first would strand it armed with the job
            // filter still dropped and no way to finish - a silent stuck
            // state. Tear it down here too.
            if (descriptor_census_enabled.exchange(false, std::memory_order_relaxed)) {
                const bool restore =
                    descriptor_census_restore_unfiltered.load(std::memory_order_relaxed);
                mirror_unfiltered.store(restore, std::memory_order_relaxed);
                log::warn(std::format(
                    "Pause: DescriptorCensus was still armed and is torn down with it; job "
                    "filter restored to unfiltered={}. Its table is INCOMPLETE - the summary "
                    "only emits when a doubled call finishes.", restore));
            }
            log::warn("Pause: FrameRootDouble disarmed");
        } else {
            const uintptr_t doom_base =
                reinterpret_cast<uintptr_t>(GetModuleHandleW(L"DOOMx64vk.exe"));
            int32_t use_smp = -1;
            uint8_t sync_flag = 0xFF;
            if (doom_base) {
                safe_read_bytes(doom_base + kUseSmpCvarRva + 0x30, &use_smp, sizeof(use_smp));
                safe_read_bytes(doom_base + 0x7061830, &sync_flag, sizeof(sync_flag));
            }
            if (use_smp == 2 || sync_flag != 0) {
                log::error(std::format(
                    "Pause: FrameRootDouble NOT armed - the render pipeline is still threaded "
                    "(r_useSMP={} syncFlag={}). Press PageDown first to go inline, then try "
                    "again. Doubling the frame root against a live job graph is exactly what "
                    "has broken every previous re-entry attempt in this project.",
                    use_smp, sync_flag));
            } else {
                // Hold SHIFT while pressing Pause to re-run the bounded 20-attempt
                // A/B that writes the two comparison PPMs. Plain Pause is live
                // stereo: continuous, no dumps, eye 1 presented to the headset.
                const bool ab = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                // CTRL+Pause (or Break) now selects ZERO-OFFSET ISOLATION:
                // the doubled call still runs, but with no camera divergence.
                // Any corruption remaining in EYE 0 is then caused by the act
                // of doubling - shared depth writes, shared renderer state -
                // rather than by the camera shift. Those two causes have been
                // conflated all session.
                const bool zero_off = !ab && (break_pressed ||
                    (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0);
                frame_root_zero_offset.store(zero_off, std::memory_order_relaxed);
                frame_root_blit_only.store(false, std::memory_order_relaxed);
                // NULL CONTROL (hold CTRL as well as SHIFT). Runs the exact
                // same A/B with BOTH passes at offset 0.
                //
                // Why this exists: the two A/B dumps are ~2 SECONDS apart in
                // wall time, because each one does a vkQueueWaitIdle plus a
                // 23 MB readback plus a file write. Over two seconds DOOM's
                // sky drifts and its effects animate while interior walls sit
                // still - which predicts exactly the pattern the 200 uu run
                // produced (landscape differs, near geometry bit-identical).
                // That is also the OPPOSITE of parallax, where near geometry
                // must move most.
                //
                // So: if 0-vs-0 still shows the landscape differing, the
                // difference is time, not camera, and every divergence result
                // today is unsupported. If 0-vs-0 comes back flat and 0-vs-200
                // does not, the camera really is moving. Nothing further gets
                // built until this is settled.
                const bool null_control = ab && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                frame_root_double_null_control.store(null_control, std::memory_order_relaxed);
                frame_root_double_ab.store(ab, std::memory_order_relaxed);
                frame_root_double_ipd.store(
                    null_control ? 0.0f : (ab ? kFrameRootDoubleIpd : kFrameRootDoubleLiveIpd),
                    std::memory_order_relaxed);
                frame_root_double_cap.store(
                    ab ? kMaxFrameRootDoubleAttempts : kFrameRootDoubleLiveCap,
                    std::memory_order_relaxed);
                frame_root_double_attempts.store(0, std::memory_order_relaxed);
                // Same-frame tag (G2): fresh budget of log lines per arm.
                mirror_serial_log_count.store(0, std::memory_order_relaxed);
                // Stage 2a needs the offscreen redirect path live so the second
                // call never touches DOOM's real framebuffer. No cap applies on
                // this route: kMaxRedirectAttempts is only consulted by F7's
                // RenderOpaque double-call path, whereas FrameRootDouble sets
                // inside_injected_extra_call itself. The counter is reset here
                // purely so the redirect logging (first 20, then every 500th)
                // stays readable across runs.
                redirect_extra_call_enabled.store(true, std::memory_order_relaxed);
                redirect_attempt_count.store(0, std::memory_order_relaxed);
                frame_root_double_enabled.store(true, std::memory_order_relaxed);
                log::warn(std::format(
                    "Pause: FrameRootDouble Stage 1 ARMED (r_useSMP={} syncFlag={}, inline "
                    "confirmed). Doubles DOOM+0x17B7900 per frame with a {:.1f} uu lateral "
                    "camera shift, redirected offscreen and filtered to depth+opaque. Mode={}, "
                    "cap={} attempts. {}",
                    use_smp, sync_flag,
                    frame_root_double_ipd.load(std::memory_order_relaxed),
                    null_control ? "NULL CONTROL A/B (both passes offset 0 - if the landscape still differs, the differences are TIME not CAMERA)"
                       : ab ? "A/B (writes two comparison PPMs, 20-attempt burst)"
                       : "LIVE STEREO (continuous, no dumps)",
                    frame_root_double_cap.load(std::memory_order_relaxed),
                    ab ? "Two dumps will be written; expect a 1-2 s freeze from the readbacks."
                       : "Eye 0 = DOOM's own frame, Eye 1 = the diverged render. LOOK FOR DEPTH."));
            }
        }
    }
    // CTRL+PageDown: si_splitscreen / net_maxLocalUsers.
    //
    // This is the sibling CPU track's own recommended-but-never-attempted
    // lead (docs/SESSION-HANDOFF-2026-08-13.md in doomvr-m1-cpu-lead-a):
    // "real si_splitscreen / local user count cvar strings found in the
    // binary, never tried by any prior session. Start there."
    //
    // They could not test it - the console is stripped in the shipping build
    // and they had no way to set a cvar. We do: the same idCVar field write
    // that successfully flipped r_useSMP and put the whole render pipeline
    // inline. Their static lead, our write mechanism.
    //
    // Why it might succeed where their two synthetic attempts hard-hung the
    // view builder: those forced the request count and hand-built a layout
    // record, and the builder needed hidden state they could not fabricate
    // ("Hidden state beyond count and layout record is mandatory; synthetic
    // activation is non-viable"). Setting the game's OWN splitscreen cvar
    // asks the engine to initialise that state through its own path.
    //
    // A hard hang is a real possibility - be ready to kill the process.
    //
    // Detection needs no new code: FrameRootProbe (PageUp) already logs each
    // per-view frame-root entry with its viewEntry pointer. One view per
    // frame today; two distinct viewEntry values per frame would mean DOOM
    // is genuinely building two views.
    // GetAsyncKeyState's low bit is CLEARED by the read, so PageDown must be
    // sampled exactly once and shared between the two handlers below -
    // querying it twice would let the first call swallow the press.
    const bool pagedown_pressed = (GetAsyncKeyState(VK_NEXT) & 1) != 0;
    const bool ctrl_held = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    if (pagedown_pressed && shift_held_now && !ctrl_held) {
        // SHIFT+PageDown: WHAT THIS DLL DOES TO DOOM'S OWN COMMAND BUFFER.
        //
        // S3 - shadows sliding with player movement - now survives every
        // control that has been built: 0.0 separation, every doubled-call floor
        // rung including the one that renders nothing and sets up no view, eye
        // 0's presentation source, and the composite capture. It dies only on
        // PageUp.
        //
        // What is left is not inside the doubled call at all. It is the work
        // this DLL does around it, every frame, independently of all of the
        // above. Two candidates remain and this cycle separates them, each one
        // variable against state 0:
        //
        //   1 DOUBLED CALL SKIPPED - full-frame mode, the real call's offset,
        //     the mirrors and the layout bookkeeping all stay exactly as they
        //     are, but real_frame_root is not entered a second time. Isolates
        //     the second frame-root entry itself from everything around it.
        // THE LAYOUT-OPS STATE IS REMOVED, and its removal is a result, not a
        // tidy-up. It ran: the shadows still slid with snapshot/rewind
        // suppressed, so the layout bookkeeping is cleared. It also FROZE THE
        // GAME on the way back to state 0 - re-enabling layout bookkeeping
        // mid-session leaves the tracked layouts describing transitions that
        // were never recorded, so the next rewind emits barriers against a
        // state the images are not in. That is a hazard I introduced by making
        // it a cycle rather than a one-way switch, and it cost the owner a
        // run. A toggle that corrupts tracked GPU state cannot be two-way
        // without a resync, so it is gone rather than re-armed.
        const bool skipped = !frame_root_blit_only.load(std::memory_order_relaxed);
        frame_root_blit_only.store(skipped, std::memory_order_relaxed);
        static const char* kState[] = {
            "0 NORMAL - the doubled call runs",
            "1 DOUBLED CALL SKIPPED - real_frame_root is not entered a second time. Everything "
            "else (offset, mirrors, layout bookkeeping, composite capture) is unchanged"};
        vwc_arm(skipped ? "doubled call SKIPPED" : "doubled call running");
        log::warn(std::format(
            "SHIFT+PageDown: {}. Walk around and judge the SHADOWS. Press again to toggle back.",
            kState[skipped ? 1 : 0]));
    } else if (pagedown_pressed && ctrl_held) {
        const uintptr_t doom_base =
            reinterpret_cast<uintptr_t>(GetModuleHandleW(L"DOOMx64vk.exe"));
        if (doom_base) {
            const bool turn_on = !splitscreen_applied.load(std::memory_order_relaxed);
            struct CvarSet { const char* name; uintptr_t rva; int on; int off; };
            const CvarSet sets[] = {
                {"si_splitscreen",    0x7048450, 1, 0},
                {"net_maxLocalUsers", 0x70486D0, 2, 1},
            };
            for (const auto& c : sets) {
                const uintptr_t obj = doom_base + c.rva;
                char before[32]{}; int32_t bi = 0; float bf = 0.0f;
                read_cvar_triplet(obj, before, sizeof(before), &bi, &bf);
                const int32_t iv = turn_on ? c.on : c.off;
                const float fv = static_cast<float>(iv);
                char txt[2] = {static_cast<char>('0' + iv), '\0'};
                safe_write_bytes(obj + 0x1c, txt, sizeof(txt));
                safe_write_bytes(obj + 0x30, &iv, sizeof(iv));
                safe_write_bytes(obj + 0x34, &fv, sizeof(fv));
                char after[32]{}; int32_t ai = 0; float af = 0.0f;
                read_cvar_triplet(obj, after, sizeof(after), &ai, &af);
                log::warn(std::format(
                    "Splitscreen {} obj={:#x} before=\"{}\"/{} -> after=\"{}\"/{}",
                    c.name, obj, before, bi, after, ai));
            }
            splitscreen_applied.store(turn_on, std::memory_order_relaxed);
            log::warn(std::format(
                "CTRL+PageDown: splitscreen cvars now {}. Press PageUp to capture - two distinct "
                "viewEntry values per frame would mean DOOM is building two views. A hard hang "
                "here is a known risk (the CPU track's synthetic activations hung the builder).",
                turn_on ? "ON" : "OFF (restored)"));
        }
    }
    // PageDown: set jobs_forceSingleThreaded directly, no Steam involved.
    if (pagedown_pressed && !ctrl_held) {
        toggle_force_single_threaded();
    }
    // Delete: UpstreamPipelineReinvoke Stage A. The mechanism has been in the
    // tree since 2026-08-09 and its viewport-rect fix (viewport_rect_save_
    // buffer) was written and built but never live-tested, because F9 - its
    // original hotkey - was reclaimed for Lever G on 2026-08-11 and no arm
    // site was left behind. Every other hotkey still DISARMS this flag; until
    // now nothing could set it, so the test was unrunnable. Re-armed here on a
    // free key, unchanged in every other respect.
    // Sampled ONCE - reading the low bit clears it, so a second
    // GetAsyncKeyState call in the same frame would eat the press. Same trap
    // already hit once on SHIFT+PageUp.
    const bool delete_pressed = (GetAsyncKeyState(VK_DELETE) & 1) != 0;
    if (delete_pressed && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) {
        // ===== CTRL+DELETE: SLOT 4 - PARK HEAD-LOOK ENTIRELY ==============
        //
        // Added 2026-08-26 because there was no way to run the working
        // true-stereo build WITHOUT the head-decoupling experiment. The only
        // off switch was deleting the switch file, which disarms wraparound,
        // the framing and the stereo path along with it - i.e. it threw away
        // the two weeks of work the head-look thread was built on top of.
        //
        // OFF here leaves stereo, framing and wraparound fully armed and
        // simply stops Lever A and Lever V rotating. That is the 16:9 true
        // stereo build, intact, with the experiment parked. `headlook=off` in
        // the switch file does the same thing permanently, no rebuild.
        {
            const bool now_on = !wrap::head_look_runtime();
            wrap::set_head_look_runtime(now_on);
            g_exp_slots[3].on.store(!now_on, std::memory_order_relaxed);
            log::warn(std::format(
                "CTRL+Delete: head-look {} (stereo and framing UNAFFECTED either way).\n"
                "READING IT: OFF parks the whole head-decoupling experiment - the world will "
                "follow your head again, as it did before that work started, and the gun and "
                "layers should behave as they did in the build that was working. This is the "
                "revert switch, not a diagnostic. headlook=off in the switch file makes it "
                "permanent.",
                now_on ? "ON (head decoupling active)" : "PARKED - OFF"));
        }
    } else if (delete_pressed && (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) {
        // SHIFT+Delete: toggle read-side descriptor redirection live, so the
        // isolation question - is the substitution itself what hangs - can be
        // answered from one standing position in one run.
        const bool now_on = !read_redirect_enabled.load(std::memory_order_relaxed);
        read_redirect_enabled.store(now_on, std::memory_order_relaxed);
        invalidate_read_redirect_cache(now_on ? "redirect enabled by SHIFT+Delete"
                                              : "redirect disabled by SHIFT+Delete");
        log::warn(std::format(
            "SHIFT+Delete: read-side descriptor redirection now {}. {}",
            now_on ? "ON" : "OFF",
            now_on ? "If the game hangs within a frame or two of this line, the substitution "
                     "itself is the cause and the log ends here."
                   : "Eye 1 now reads DOOM's own unshifted images, so expect lighting without "
                     "parallax."));
    } else if (delete_pressed) {
        const bool now_enabled = !upstream_reinvoke_enabled.load(std::memory_order_relaxed);
        if (now_enabled) {
            upstream_reinvoke_attempt_count.store(0, std::memory_order_relaxed);
            upstream_reinvoke_frame_counter.store(0, std::memory_order_relaxed);
            // Mutual exclusion with every other extra-call mechanism: never
            // run two of them against the same frame.
            stereo_preview_enabled.store(false, std::memory_order_relaxed);
            render_opaque_double_call_enabled.store(false, std::memory_order_relaxed);
            redirect_extra_call_enabled.store(false, std::memory_order_relaxed);
            render_view_clone_test_enabled.store(false, std::memory_order_relaxed);
            true_stereo_enabled.store(false, std::memory_order_relaxed);
            widen_gather_probe_enabled.store(false, std::memory_order_relaxed);
            stereo_dual_eye_enabled.store(false, std::memory_order_relaxed);
            scoped_reinvoke_enabled.store(false, std::memory_order_relaxed);
        }
        upstream_reinvoke_enabled.store(now_enabled, std::memory_order_relaxed);
        log::warn(now_enabled
            ? "Delete: UpstreamPipelineReinvoke Stage A ARMED - re-runs the whole "
              "gather/cull/sort/opaque chain using each job's own live arg1, with save/restore "
              "of all 16 pass-table slots AND the viewport-rect region at viewport_obj+0x14c8 "
              "(the fix for the transient window-shrink seen on 2026-08-09, built then, never "
              "live-tested until now). Zero camera divergence, no target redirect. WATCH FOR: "
              "the window briefly shrinking to a smaller square. If it is gone, Stage A is "
              "validated; if it persists, STOP - that is a real finding, not a cue to widen the "
              "save region again."
            : "Delete: UpstreamPipelineReinvoke Stage A disabled");
    }
    // The post-frame-root window ends here: everything DOOM recorded between
    // the frame root returning and this present is what eye 1 is missing.
    post_frame_root_window = false;
    drive_scripted_test();
    if (offscreen_dump_pending[0].exchange(false, std::memory_order_relaxed)) {
        dump_offscreen_color_snapshot(offscreen_targets[0], "slot0");
        offscreen_dump_done[0].store(true, std::memory_order_relaxed);
    }
    // FrameRootDouble's A/B dumps take priority and use their own filenames so
    // the offset-0 and offset-N images sit side by side on disk.
    if (frame_root_double_enabled.load(std::memory_order_relaxed) &&
        offscreen_dump_pending[1].load(std::memory_order_relaxed)) {
        offscreen_dump_pending[1].store(false, std::memory_order_relaxed);
        const int which = frame_root_double_dump_label.load(std::memory_order_relaxed);
        // 2026-08-17, STEP 1 of docs/IMPLEMENTATION-PLAN-2026-08-17.md.
        //
        // This used to dump offscreen_targets[0] unconditionally, which under
        // mirroring is STALE: hook_cmd_begin_render_pass's mirror block
        // returns before the legacy offscreen redirect block ever runs, AND
        // the doubled call sets inside_injected_extra_call = !full_frame &&
        // !mirror_enabled, i.e. FALSE whenever mirroring is on. Nothing
        // writes offscreen_targets while mirroring, so the A/B "measurement"
        // compared two old images and reported no error - a silently invalid
        // instrument, which by this project's own Rule 3 is indistinguishable
        // from a valid negative (review headline 2 / claim 5).
        //
        // Dump the mirror the headset is actually shown instead. Same image,
        // same extent, same format the eye-1 presentation blit uses, read
        // through one consistent snapshot rather than three racy globals.
        DoubledMirrorState mirror = load_doubled_mirror();
        // A/B measures the G-BUFFER, not whatever eye 1 presents. Under the
        // filter these are the same image and the 13.9x result stands
        // unchanged; unfiltered, this is what makes the comparison
        // like-for-like and answers whether the doubled call's geometry is
        // still shifted at all.
        {
            std::scoped_lock lock(mirror_mutex);
            if (doubled_gbuffer_mirror != VK_NULL_HANDLE) {
                mirror.color = doubled_gbuffer_mirror;
                mirror.format = doubled_gbuffer_format;
                mirror.extent = doubled_gbuffer_extent;
            }
        }
        const bool use_mirror = mirror_enabled.load(std::memory_order_relaxed) &&
                                mirror.color != VK_NULL_HANDLE;
        VkImageLayout mirror_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        if (use_mirror) {
            std::scoped_lock lock(mirror_mutex);
            mirror_layout = mirror_layout_of(mirror.color);
        }
        // Filter state in the FILENAME. Without it, capturing unfiltered and
        // then filtered in one session silently overwrites the first pair
        // with the second - which is exactly what happened on run 14 and
        // cost the unfiltered measurement.
        const std::string dump_label = std::format("frdouble_mirror_{}_off{}",
            mirror_unfiltered.load(std::memory_order_relaxed) ? "unfilt" : "filt",
            which == 0 ? "0" : "20");
        if (use_mirror) {
            log::warn(std::format(
                "FrameRootDouble A/B dump: source=MIRROR image={} {}x{} format={} "
                "mirrorSerial={} presentSerial={} layout={} -> {}",
                reinterpret_cast<void*>(mirror.color), mirror.extent.width,
                mirror.extent.height, static_cast<int>(mirror.format), mirror.serial,
                g_frame_root_serial.load(std::memory_order_relaxed),
                static_cast<int>(mirror_layout), dump_label));
            dump_image_snapshot(mirror.color, mirror.extent, mirror.format, dump_label.c_str(),
                                mirror_layout);
        } else {
            // Rule 3: a fallback that is not logged is a trap. This is the
            // ONLY path back to the legacy target, and it says so loudly -
            // any measurement taken from these files is about the legacy
            // redirect, not about the mirrored configuration.
            log::error(std::format(
                "FrameRootDouble A/B dump FELL BACK to the legacy offscreen target "
                "(mirrorEnabled={} mirrorColor={}). The images written are NOT the mirrored "
                "configuration and are very likely STALE - do not draw a parallax conclusion "
                "from them.",
                mirror_enabled.load(std::memory_order_relaxed),
                reinterpret_cast<void*>(mirror.color)));
            dump_offscreen_color_snapshot(offscreen_targets[0],
                                          which == 0 ? "frdouble_off0" : "frdouble_off20");
        }
        // CHAIN WALK dumps (STEP 6.6). One PPM per armed slot per offset, so
        // the same stage at 0 uu and 20 uu sits side by side on disk and the
        // near/far MAD instrument can be run on each pair in turn.
        //
        // The filename carries slot, job RVA, pass index, filter state and
        // offset - every parameter that varies. A filename collision silently
        // overwrote a capture once this stream and cost a whole run.
        //
        // A slot is dumped ONLY if it was written during THIS attempt.
        // Refusing a stale image is the difference between a missing
        // measurement (visible) and a wrong one (invisible).
        {
            const uint32_t want_attempt = (which == 0) ? 0u : 1u;
            const char* filt = mirror_unfiltered.load(std::memory_order_relaxed)
                                   ? "unfilt" : "filt";
            for (uint32_t s = 0; s < kChainWalkSlots; ++s) {
                VkImage image = VK_NULL_HANDLE;
                VkExtent2D extent{};
                VkFormat format = VK_FORMAT_UNDEFINED;
                uintptr_t rva = 0;
                uint32_t pass = 0, filled = UINT32_MAX;
                bool observed = false;
                {
                    std::scoped_lock lock(chainwalk_mutex);
                    const ChainWalkSlot& slot = chainwalk_slots[s];
                    observed = slot.observed;
                    rva = slot.rva;
                    filled = slot.filled_attempt;
                    if (slot.filled_attempt == want_attempt && slot.image != VK_NULL_HANDLE) {
                        image = slot.image;
                        extent = slot.extent;
                        format = slot.format;
                        pass = slot.filled_pass;
                    }
                }
                if (image == VK_NULL_HANDLE) {
                    if (observed) {
                        log::error(std::format(
                            "ChainWalk DUMP SKIPPED slot={} jobRva={:#x} attempt={}: nothing was "
                            "recorded into it this attempt (filledAttempt={}). No PPM is written - "
                            "an absent file, not a stale one.",
                            s, rva, want_attempt, filled));
                    }
                    continue;
                }
                const std::string label = std::format(
                    "chain_{}_s{}_rva{:x}_p{}_off{}", filt, s, rva, pass,
                    which == 0 ? "0" : "20");
                log::warn(std::format(
                    "ChainWalk DUMP slot={} jobRva={:#x} attempt={} pass={} image={} {}x{} "
                    "format={} -> {}",
                    s, rva, want_attempt, pass, reinterpret_cast<void*>(image),
                    extent.width, extent.height, static_cast<int>(format), label));
                // Source layout is TRANSFER_DST_OPTIMAL: the pass-boundary
                // copy left it there and nothing else ever touches this image.
                dump_image_snapshot(image, extent, format, label.c_str(),
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            }
        }
        // SWAPCHAIN REFERENCE (2026-08-17, after the owner reported eye 1 as
        // "very bright, doesn't match the monitor" on run 21).
        //
        // Everything this project measures comes out of OUR mirrors. Nothing
        // has ever dumped the image DOOM actually presents, so "eye 1 does
        // not match the monitor" has had no reference to be checked against.
        // This is that reference: the exact VkImage handed to
        // vkQueuePresentKHR, in DOOM's own 8-bit display-encoded format, so
        // it can be compared side by side with the pass-78 composite eye 1 is
        // built from.
        //
        // It answers a specific question. Eye 1's source is format 122
        // (B10G11R11_UFLOAT) and the composite's readback maxes at exactly
        // 1.0, so it is display-RANGE but possibly not display-ENCODED - and
        // DOOM's final grade and HUD happen AFTER the frame root returns,
        // outside the doubled call, where we cannot mirror them. If this
        // reference is materially darker or differently graded than the
        // pass-78 composite, eye 1 is showing a PRE-FINAL image and no amount
        // of blit-side colour work will fix it.
        {
            VkImage present_image = VK_NULL_HANDLE;
            VkFormat present_format = VK_FORMAT_UNDEFINED;
            VkExtent2D present_extent{};
            if (info && info->pSwapchains && info->pImageIndices && info->swapchainCount) {
                std::unique_lock lock(swapchains_mutex);
                if (auto sc = swapchains.find(info->pSwapchains[0]); sc != swapchains.end()) {
                    const uint32_t idx = info->pImageIndices[0];
                    if (idx < sc->second.images.size()) {
                        present_image = sc->second.images[idx];
                        present_format = sc->second.format;
                        present_extent = sc->second.extent;
                    }
                }
            }
            if (present_image != VK_NULL_HANDLE) {
                const std::string label =
                    std::format("swapchainref_off{}", which == 0 ? "0" : "20");
                log::warn(std::format(
                    "SwapchainReference: DOOM's presented image={} {}x{} format={} -> {}. This is "
                    "what the MONITOR shows; compare it against chain_*_s3_* which is what eye 1 "
                    "is built from.",
                    reinterpret_cast<void*>(present_image), present_extent.width,
                    present_extent.height, static_cast<int>(present_format), label));
                dump_image_snapshot(present_image, present_extent, present_format, label.c_str(),
                                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
            } else {
                // Rule 3.
                log::error("SwapchainReference: could not resolve the presented image; no "
                           "monitor reference written this attempt.");
            }
        }
        offscreen_dump_done[1].store(true, std::memory_order_relaxed);
    }
    if (offscreen_dump_pending[1].exchange(false, std::memory_order_relaxed)) {
        // Phase 0 sweep (deleted plan; kept as a same-run sweep harness): each slot-1 dump
        // carries the magnitude in its filename so all four end up on disk
        // side-by-side. After the dump returns (which itself queue-waits at
        // the start and blocks until stats are logged) we advance to the
        // next magnitude, clear done[1] so the next TrueStereo attempt
        // triggers another pending, and, on the final magnitude, disarm
        // TrueStereo so the sweep ends cleanly instead of continuing to
        // burn attempts at the last offset.
        if (g_phase0_sweep_active && g_phase0_sweep_index < kPhase0SweepOffsets.size()) {
            const float mag = kPhase0SweepOffsets[g_phase0_sweep_index];
            const int mag_whole = static_cast<int>(mag);
            const int mag_frac = static_cast<int>((mag - static_cast<float>(mag_whole)) * 100.0f + 0.5f);
            char label[32];
            std::snprintf(label, sizeof(label), "slot1_o%d_%02d", mag_whole, mag_frac);
            dump_offscreen_color_snapshot(offscreen_targets[1], label);
            g_phase0_sweep_index++;
            if (g_phase0_sweep_index >= kPhase0SweepOffsets.size()) {
                g_phase0_sweep_active = false;
                true_stereo_enabled.store(false, std::memory_order_relaxed);
                offscreen_dump_done[1].store(true, std::memory_order_relaxed);
                log::warn("Phase 0 sweep: all four magnitudes captured; TrueStereo disarmed.");
            } else {
                g_true_stereo_translation_offset.store(
                    kPhase0SweepOffsets[g_phase0_sweep_index], std::memory_order_relaxed);
                offscreen_dump_done[1].store(false, std::memory_order_relaxed);
                log::warn(std::format(
                    "Phase 0 sweep: advanced to magnitude {:.2f} world units (index {}/{}).",
                    kPhase0SweepOffsets[g_phase0_sweep_index],
                    g_phase0_sweep_index + 1, kPhase0SweepOffsets.size()));
            }
        } else {
            dump_offscreen_color_snapshot(offscreen_targets[1], "slot1");
            offscreen_dump_done[1].store(true, std::memory_order_relaxed);
        }
    }
    // StereoDualEyeDump (F12, Milestone 1): same deferred-to-present-thread
    // pattern as the offscreen_dump_pending[] pair above - dump_offscreen_
    // color_snapshot does a blocking vkQueueWaitIdle, not safe to call from
    // the render worker thread mid-command-buffer-recording.
    if (stereo_eye_dump_pending[0].exchange(false, std::memory_order_relaxed)) {
        dump_offscreen_color_snapshot(stereo_eye_targets[0], "eye0");
        stereo_eye_dump_done[0].store(true, std::memory_order_relaxed);
    }
    if (stereo_eye_dump_pending[1].exchange(false, std::memory_order_relaxed)) {
        dump_offscreen_color_snapshot(stereo_eye_targets[1], "eye1");
        stereo_eye_dump_done[1].store(true, std::memory_order_relaxed);
    }
    // RealCallRedirectProbe - see real_call_redirect_target's declaration above.
    // DepthDumpProbe (Task 3, Round 5 step 1): also dump the private depth
    // image's own content, not just color - see dump_depth_image_snapshot's
    // declaration above.
    if (real_call_redirect_dump_pending.exchange(false, std::memory_order_relaxed)) {
        dump_offscreen_color_snapshot(real_call_redirect_target, "realcall");
        dump_depth_image_snapshot(real_call_redirect_target.depth_image,
            real_call_redirect_target.extent, "realcall_depth");
        // DepthDumpControlTest (docs/STEREO-RESTART-PLAN.md Task 3, Round 5
        // continued, step 3): point the SAME readback logic at DOOM's real,
        // already-known-correct depth image (an ordinary, non-redirected
        // frame's depth buffer - offscreen_targets[1].depth_view was
        // captured once, early in the session, from DOOM's own real
        // framebuffer via framebuffer_depth_views; see prepare_offscreen_
        // target's use_real_depth branch). Resolves that already-known view
        // to its underlying VkImage via the new image_view_to_image map.
        // If either lookup is unready this early, log why instead of
        // silently skipping - this is a one-shot capture, not a retryable
        // hot path.
        if (!offscreen_targets[1].ready.load(std::memory_order_relaxed)) {
            log::error("DepthDumpControlTest: offscreen_targets[1] not ready yet, skipping control dump");
        } else {
            VkImage real_depth_image = VK_NULL_HANDLE;
            {
                std::scoped_lock lock(framebuffer_depth_view_mutex);
                const auto found = image_view_to_image.find(offscreen_targets[1].depth_view);
                if (found != image_view_to_image.end()) real_depth_image = found->second;
            }
            if (real_depth_image == VK_NULL_HANDLE) {
                log::error(std::format(
                    "DepthDumpControlTest: no image_view_to_image entry for real depth view {} "
                    "(vkCreateImageView hook installed after this view was created?)",
                    reinterpret_cast<void*>(offscreen_targets[1].depth_view)));
            } else {
                dump_depth_image_snapshot(real_depth_image, offscreen_targets[1].extent, "control_real_depth");
            }
        }
        real_call_redirect_dump_done.store(true, std::memory_order_relaxed);
    }
    view_shift_active = false;
#endif  // DOOMVR_RESEARCH_TOOLS
    static uint32_t retry_counter = 0;
    if (!xr().ready() && (retry_counter++ % 300) == 0)
        xr().initialize(state);
    // MONITOR EYE-1 VIEW: CONTINUOUS INVARIANT, NOT AN ARM-TIME CHECK
    // (2026-08-29).
    //
    // Both places that arm the mode - SHIFT+F5 and scripted step 39 - refuse
    // while the headset path is live. That is not sufficient on its own: XR
    // can become ready AFTER the mode is armed (SteamVR started late, the
    // headset put on mid-run, the session recreated), and from that moment the
    // doubled final pass would be overwriting the presented frame while the
    // owner is wearing the thing. The presented frame is EYE 0, the confirmed
    // reference, so that must be impossible rather than unlikely.
    //
    // A one-time gate cannot express "never true at the same time as". This
    // re-checks every present and disarms, which is the invariant actually
    // wanted. It is the same lesson as the mirror-convergence rewrite: a
    // condition re-arms itself, a constant does not.
    if (monitor_eye1_direct.load(std::memory_order_relaxed) && xr().ready()) {
        monitor_eye1_direct.store(false, std::memory_order_relaxed);
        log::error(std::format(
            "MonitorEye1View FORCE-DISARMED: the headset path became ready while it was on "
            "(passesRedirected={} before disarm). This mode overwrites the presented frame, "
            "which in a headset run is EYE 0 - the confirmed reference - so it cannot be "
            "allowed to coexist with XR. The monitor is back to DOOM's own frame and eye 1 "
            "goes to the headset as usual. Nothing was judged from a corrupted reference.",
            monitor_eye1_passes.load(std::memory_order_relaxed)));
    }
    VkImage source = VK_NULL_HANDLE;
    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    if (info && info->swapchainCount && info->pSwapchains && info->pImageIndices) {
        std::unique_lock lock(swapchains_mutex);
        auto found = swapchains.find(info->pSwapchains[0]);
        if (found != swapchains.end() && info->pImageIndices[0] < found->second.images.size()) {
            source = found->second.images[info->pImageIndices[0]];
            extent = found->second.extent;
            format = found->second.format;
        }
    }
    // ===== WRAPAROUND: PHASE 0 AND ITS SELF-TESTS ==========================
    //
    // Placed HERE, outside the xr().ready() guard, on purpose: RUN 1 is a
    // headset-free diagnostic and the whole point of it is DOOM's live
    // projection scale under a maxed-out in-game FOV slider. A Phase 0 line
    // that only printed with a headset attached could not answer that run's
    // question at all.
    //
    // Both self-tests follow the pattern that is the one thing worth keeping
    // from the previous agent: prove the build's own health inside ~5 s, and
    // say ***ABORT THIS RUN*** rather than let five minutes of play be spent
    // on an instrument that cannot ask the question. Order matters - the math
    // test runs first, because if the framing math is wrong then every number
    // Phase 0 prints is wrong too.
    //
    // Its own counter, not presented_frame: that one is declared AND
    // incremented inside #ifdef DOOMVR_RESEARCH_TOOLS, and Phase 0 must print
    // in every build configuration, including one where the +0xC44 sampler is
    // compiled out - which is precisely the case the line has to be able to
    // report rather than silently show zeros for.
    {
        static std::atomic_uint64_t wrap_present_counter{0};
        const uint64_t wrap_presents =
            wrap_present_counter.fetch_add(1, std::memory_order_relaxed) + 1;
        // begin_frame FIRST, and from HERE rather than from poll().
        //
        // It latches DOOM's live projection scale once per present so both
        // eyes cannot read different values. It used to live in poll(), which
        // only runs when xr().ready() - so on RUN 1, a headset-free session,
        // it would never have executed and the Phase 0 line's headline
        // "ACTIVE scale" would have printed the legacy constant with
        // live=false no matter how healthy the sampler was. That is the exact
        // shape of defect that costs a run: a number that looks like a
        // measurement and is actually an initialiser.
        // DOOM's frame aspect feeds the world-view test in the sampler. It is
        // published BEFORE begin_frame so the very first latch already has it.
        if (extent.width && extent.height) {
            const float a = static_cast<float>(extent.width) /
                            static_cast<float>(extent.height);
            wrap::publish_frame_aspect(a);
            doom_frame_aspect.store(a, std::memory_order_relaxed);
        }
        wrap::publish_gui_draw_rate(
            g_render_view_calls_this_frame.exchange(0, std::memory_order_relaxed));
        wrap::begin_frame();
        // bodyfollow=calibrate. Driven from HERE and not from the XR poll: the
        // poll does not run on a headset-free session, and this mode is
        // FLAT/MONITOR only. xr().ready() is passed so the mode can refuse
        // itself loudly if a headset is live.
        wrap::tick_body_calibration(xr().ready());
        // FREQHIGH per-frame clears - at the PRESENT, the real frame
        // boundary. Unconditional (never gated on any census/fix arm - the
        // shared-state-reset rule), and after every submit of the frame, so
        // no pass's records leave observation before their submit-time
        // processing (the gather-prepare site cleared mid-frame because the
        // doubled call runs FIRST - the 19:06 captures' coverage hole).
        // FORCED RENDER RESOLUTION, retry path. The surface hook is the right
        // place to do this and it covers a clean launch - but the injector
        // attaches to a RUNNING DOOM, so on most sessions the surface already
        // exists and that hook never fires for it. Retried once from here,
        // where a window certainly exists, rather than assuming the good case.
        if (!g_render_resize_done.load(std::memory_order_relaxed) &&
            wrap::config_render_w() > 0 && wrap::config_render_h() > 0) {
            if (!g_doom_hwnd.load(std::memory_order_relaxed)) {
                struct Finder {
                    static BOOL CALLBACK cb(HWND h, LPARAM) {
                        DWORD pid = 0;
                        GetWindowThreadProcessId(h, &pid);
                        if (pid != GetCurrentProcessId() || !IsWindowVisible(h)) return TRUE;
                        RECT r{};
                        GetClientRect(h, &r);
                        // The game window is the big visible one owned by this
                        // process; tool windows and tooltips are not.
                        if (r.right - r.left < 640 || r.bottom - r.top < 480) return TRUE;
                        g_doom_hwnd.store(h, std::memory_order_relaxed);
                        return FALSE;
                    }
                };
                EnumWindows(&Finder::cb, 0);
            }
            apply_forced_render_resolution("present retry");
        }
        // ipd override, from the switch file, refreshed each frame so the
        // PageUp scripted path cannot quietly restore the default under it.
        //
        // UNCONDITIONAL AGAIN (2026-08-31, late). Slot 1 now carries the JOB
        // FILTER A/B, not the ipd override, so gating this on the slot would
        // key the ipd value to an unrelated experiment - the
        // half-migrated-indirection defect. The override is config-only
        // (`ipd=`), refreshed each frame so the PageUp scripted path cannot
        // quietly restore the default under it.
        g_ipd_override.store(wrap::config_ipd_override(), std::memory_order_relaxed);
        g_ipd_refresh_applied.fetch_add(1, std::memory_order_relaxed);
        // VIEWSRC per-frame state, cleared at the same real frame boundary and
        // for the same reason. Unconditional (shared-state-reset rule): these
        // are read by the producer probe AND by the ordering marker, so gating
        // the clear on one consumer would silently starve the other.
        g_vs_calls_last_frame.store(
            g_vs_calls_this_frame.exchange(0, std::memory_order_relaxed),
            std::memory_order_relaxed);
        if (g_vs_calls_last_frame.load(std::memory_order_relaxed) > 0)
            g_vs_frames.fetch_add(1, std::memory_order_relaxed);
        g_vs_a1_calls_this_frame.store(0, std::memory_order_relaxed);
        g_vs_axis_hash_prevframe.store(
            g_vs_axis_hash_frame0.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        // FHPERF's divisor, advanced on the same real frame boundary as the
        // census clears so the ms/frame figure and the ns it divides always
        // cover the same window.
        g_fh_perf_frames.fetch_add(1, std::memory_order_relaxed);
        g_fh_seen_count.store(0, std::memory_order_relaxed);
        g_fh_worldref_seen[0] = false;
        g_fh_worldref_seen[1] = false;
        g_fh_tracked_n.store(0, std::memory_order_relaxed);
        wrap::startup_self_test();
        wrap::maybe_emit_phase0(wrap_presents, extent.width, extent.height);
        wrap::present_self_test(wrap_presents);
        wrap::maybe_emit_scale_heartbeat();
        // EVERY WRITE SITE'S COUNT, ON ONE LINE, ONCE A SECOND.
        //
        // Three builds each rotated a different site and the owner reported
        // "exact same" every time - and I had no single line telling me which
        // sites were actually writing. headRotApplied lived on one line, Lever
        // V's count on none at all, and the CameraSrc site did not exist. A
        // site that writes and changes nothing is a completely different
        // finding from a site that never wrote, and until now the log could
        // not tell them apart.
        if (wrap::armed()) {
            static uint64_t last_tick = 0;
            const uint64_t now = GetTickCount64();
            if (now - last_tick >= 1000) {
                last_tick = now;
                if (wrap::config_fhcensus()) {
                    auto grp = [](const FhSlope g[2]) {
                        const float sv = fh_slope_value(g[0]);
                        const float sm = fh_slope_value(g[1]);
                        return std::format("[V={:.2f} M={:.2f} nV={} nM={}]",
                                           sv, sm, g[0].samples, g[1].samples);
                    };
                    const uint64_t gd_n = g_fh_gun_eyedist_n.load(std::memory_order_relaxed);
                    const uint64_t sa_n = g_fh_slab_a_n.load(std::memory_order_relaxed);
                    // Drain every DBW counter ONCE into locals, then divide.
                    // See the note at the argument list for why this is not
                    // done inline any more.
                    struct DbwWindow {
                        double calls_per_frame, lock_us, body_us;
                        double sets_per_call, bindings_per_call, tramp_us;
                    } dbw{};
                    {
                        const double c = static_cast<double>(
                            g_dbw_calls.exchange(0, std::memory_order_relaxed));
                        const double ln = static_cast<double>(
                            g_dbw_lock_ns.exchange(0, std::memory_order_relaxed));
                        const double bn = static_cast<double>(
                            g_dbw_body_ns.exchange(0, std::memory_order_relaxed));
                        const double st = static_cast<double>(
                            g_dbw_sets.exchange(0, std::memory_order_relaxed));
                        const double bi = static_cast<double>(
                            g_dbw_bindings.exchange(0, std::memory_order_relaxed));
                        const double f = (std::max)(1.0, static_cast<double>(
                            g_fh_perf_frames.load(std::memory_order_relaxed)));
                        const double cs = (std::max)(1.0, c);
                        const double tr = static_cast<double>(
                            g_dbw_tramp_ns.exchange(0, std::memory_order_relaxed));
                        dbw = {c / f, ln / 1e3 / cs, bn / 1e3 / cs, st / cs, bi / cs,
                               tr / 1e3 / cs};
                    }
                    log::warn(std::format(
                        "DBW calls/frame={:.0f} lockUs/call={:.2f} bodyUs/call={:.2f} "
                        "sets/call={:.1f} bindings/call={:.1f} driverUs/call={:.2f}\n"
                        "READING IT: this is CmdBindDescriptorSets split four ways, because "
                        "two rounds of guessing at it were each only half right. lockUs is "
                        "time spent ACQUIRING the shared lock - if that is most of the ~10 us "
                        "then the contention survived the shared_mutex and the next move is to "
                        "stop taking a global lock here at all. bodyUs is the walk itself - if "
                        "that is most of it then the cost is real work and bindings/call says "
                        "how much of it there is to remove. Both small while the hook still "
                        "measures 10 us/call means the cost is in the trampoline or the "
                        "timing, not in this scope.\n"
                        "FHPERF censusMs/frame={:.3f} refSolved={} refServed={} "
                        "(the clean-camera reference is a PER-FRAME value; refServed is the "
                        "work removed by memoising it. refSolved should track FRAMES, "
                        "refServed should track RECORDS - if refSolved tracks records "
                        "instead, the cache key is wrong and the hoist did nothing. "
                        "censusMs/frame is the census's own share of the 20.8 ms/frame "
                        "hook_cmd_bind_descriptor_sets cost measured on 2026-08-30.)\n"
                        "FHCENSUS seen160={} seen128={} parseFail={} noRef={} notRigid={} "
                        "fwdNotUnit={} noOrigin={} originFar={} farEye={} notSlab={} "
                        "viewmodel={} dedup={} noHead={} smallYaw={} noBasis={} dumped={} | "
                        "REAL arms{} gun{} | DBL arms{} gun{} | "
                        "worldRef={} worldV REAL[{:.2f} n={}] DBL[{:.2f} n={}] | "
                        "eyeDistMean gun={:.1f} slabA={:.4f} | fhCtrl={} applied={} "
                        "refused={} postOk={} postBad={} meanDelta={:.3f}\n"
                        "READING IT: read-only census of the gun's ACTUAL transform records "
                        "(freqHigh, mvp at +16/+0 - the old instruments read the wrong 64 "
                        "bytes). seen160+seen128 climbing within seconds of gameplay is the "
                        "engagement proof; frozen = broken intake, abort the analysis, not "
                        "the run. viewmodel should climb ~2/frame/pass (arms + gun); "
                        "viewmodel=0 with seen160 climbing means the signature failed live "
                        "and the gate counters above say which gate - STOP, desk-debug. "
                        "V is the slope of X's camera forward per head yaw, M the model "
                        "forward's - POLARITY (stated pre-run): prediction V~+-1 AND M~+-1 "
                        "(2R total on screen); V~+-2 M~0 would put the doubling entirely in "
                        "the viewmodel V (Phase-2 mode vp); the measured SIGNS pick the "
                        "Phase-2 rotation direction. Slopes need n>=8; NaN = not enough "
                        "moving-head samples yet. DBL should mirror REAL (a divergence is an "
                        "eye-1 finding, logged, not chased here). worldV is the WORLD "
                        "view-projection (identity-model 128 records) through the SAME "
                        "signed_yaw path - the same-convention reference §2.1's "
                        "(V_gun - V_world) needs; PIPERATE's -1.00 measures a different "
                        "axis and its sign must NOT be differenced against these. "
                        "gun V == worldV means the record surface is already world-locked; "
                        "opposite signs mean a 2R divergence carried entirely in V. "
                        "worldRef=0 in gameplay = the reference gate is broken and the "
                        "comparison is VOID, not evidence. fhCtrl is the POSITIVE CONTROL "
                        "(slot 1): +30deg yaw written into the classified records. applied "
                        "climbing + postOk climbing + gun VISIBLY yanked = the records drive "
                        "the picture; applied climbing + no visual change = the GPU consumes "
                        "other bytes (mechanism B). meanDelta pins the sign convention: "
                        "+0.524 = signed_yaw positive IS right-handed-about-up; -0.524 = "
                        "mirrored. fhCtrl=ON with applied=0 = key landed but no viewmodel "
                        "record passed - dedupe/classifier counters above say why. "
                        "(The bind-time control is DISCONNECTED since the 18:04 verdict; "
                        "its counters stay frozen on purpose - slot 1 is FHCTRL2 now.)",
                        // ⚠ DRAINED INTO LOCALS FIRST, ONCE, BEFORE ANY DIVISION.
                        // The first version of this divided by g_dbw_calls in
                        // four arguments and exchange()d it to zero in a fifth.
                        // C++ does not order format arguments, the exchange ran
                        // first, and every figure came out divided by 1 -
                        // calls/frame=0 with bodyUs/call=766789. It cost a run.
                        // A counter that is both a divisor and drained in the
                        // same expression is the bug; drain once, then divide.
                        dbw.calls_per_frame, dbw.lock_us, dbw.body_us,
                        dbw.sets_per_call, dbw.bindings_per_call, dbw.tramp_us,
                        // FHPERF, in declaration order ahead of the census fields.
                        // Exchanged, not loaded: this is a PER-WINDOW figure and an
                        // all-time total divided by an all-time frame count would be
                        // a ratchet that never comes back down after one spike.
                        static_cast<double>(
                            g_fh_ref_ns.exchange(0, std::memory_order_relaxed)) /
                            1e6 / (std::max)(1.0, static_cast<double>(
                                g_fh_perf_frames.exchange(0, std::memory_order_relaxed))),
                        g_fh_ref_solved.load(std::memory_order_relaxed),
                        g_fh_ref_served.load(std::memory_order_relaxed),
                        g_fh_seen160.load(std::memory_order_relaxed),
                        g_fh_seen128.load(std::memory_order_relaxed),
                        g_fh_parse_fail.load(std::memory_order_relaxed),
                        g_fh_no_ref.load(std::memory_order_relaxed),
                        g_fh_not_rigid.load(std::memory_order_relaxed),
                        g_fh_fwd_not_unit.load(std::memory_order_relaxed),
                        g_fh_origin_unsolved.load(std::memory_order_relaxed),
                        g_fh_origin_mismatch.load(std::memory_order_relaxed),
                        g_fh_far_eye.load(std::memory_order_relaxed),
                        g_fh_not_slab.load(std::memory_order_relaxed),
                        g_fh_viewmodel.load(std::memory_order_relaxed),
                        g_fh_dedup.load(std::memory_order_relaxed),
                        g_fh_no_head.load(std::memory_order_relaxed),
                        g_fh_small_yaw.load(std::memory_order_relaxed),
                        g_fh_no_basis.load(std::memory_order_relaxed),
                        g_fh_dumped.load(std::memory_order_relaxed),
                        grp(g_fh_slope[0][0]), grp(g_fh_slope[0][1]),
                        grp(g_fh_slope[1][0]), grp(g_fh_slope[1][1]),
                        g_fh_worldref.load(std::memory_order_relaxed),
                        fh_slope_value(g_fh_world_v[0]), g_fh_world_v[0].samples,
                        fh_slope_value(g_fh_world_v[1]), g_fh_world_v[1].samples,
                        gd_n ? g_fh_gun_eyedist_sum.load(std::memory_order_relaxed) / gd_n : 0.0,
                        sa_n ? g_fh_slab_a_sum.load(std::memory_order_relaxed) / sa_n : 0.0,
                        legacy_fixfamily_on() ? "ON" : "off",
                        g_fh_ctrl_applied.load(std::memory_order_relaxed),
                        g_fh_ctrl_refused.load(std::memory_order_relaxed),
                        g_fh_ctrl_post_ok.load(std::memory_order_relaxed),
                        g_fh_ctrl_post_bad.load(std::memory_order_relaxed),
                        g_fh_ctrl_delta_n.load(std::memory_order_relaxed)
                            ? g_fh_ctrl_delta_sum.load(std::memory_order_relaxed) /
                                  g_fh_ctrl_delta_n.load(std::memory_order_relaxed)
                            : 0.0));
                    auto sgrp = [](const FhSlope g[2]) {
                        return std::format("[V={:.2f} M={:.2f} nV={} nM={}]",
                                           fh_slope_value(g[0]), fh_slope_value(g[1]),
                                           g[0].samples, g[1].samples);
                    };
                    log::warn(std::format(
                        "FHSUBMIT checks={} mvpChanged={} modelChanged={} otherChanged={} "
                        "reparseFail={} | SUB REAL arms{} gun{} | DBL arms{} gun{} | "
                        "ctrl2={} applied={} refused={} postOk={} postBad={} clobbered={} "
                        "subDumped={}\n"
                        "READING IT: the classified records re-examined at every "
                        "vkQueueSubmit. mvpChanged climbing = the late CPU writer exists and "
                        "fires before submit; the SUB slopes then measure the FINAL mvp's "
                        "head-rotation content in the same convention as FHCENSUS (bind V "
                        "was +1.00 = world-locked; SUB V is what the picture actually gets "
                        "- THE number the fix must correct to worldV). mvpChanged=0 all run "
                        "= the writer runs after submit (or on the GPU) and the watchpoint "
                        "round is next. ctrl2 (slot 1) rewrites the mvp ONLY, at submit: "
                        "gun visibly swung WITHOUT going black = last-writer-wins holds and "
                        "the fix lives here; applied climbing + clobbered climbing + no "
                        "movement = the writer outruns even submit time.",
                        g_fh_sub_checks.load(std::memory_order_relaxed),
                        g_fh_sub_mvp_changed.load(std::memory_order_relaxed),
                        g_fh_sub_model_changed.load(std::memory_order_relaxed),
                        g_fh_sub_other_changed.load(std::memory_order_relaxed),
                        g_fh_sub_reparse_fail.load(std::memory_order_relaxed),
                        sgrp(g_fh_sub_slope[0][0]), sgrp(g_fh_sub_slope[0][1]),
                        sgrp(g_fh_sub_slope[1][0]), sgrp(g_fh_sub_slope[1][1]),
                        legacy_fixfamily_on() ? "ON" : "off",
                        g_fh_ctrl2_applied.load(std::memory_order_relaxed),
                        g_fh_ctrl2_refused.load(std::memory_order_relaxed),
                        g_fh_ctrl2_post_ok.load(std::memory_order_relaxed),
                        g_fh_ctrl2_post_bad.load(std::memory_order_relaxed),
                        g_fh_ctrl2_clobbered.load(std::memory_order_relaxed),
                        g_fh_sub_dumped.load(std::memory_order_relaxed)));
                    if (wrap::config_gunfix2_worldp()) {
                        log::warn(std::format(
                            "GUNFIX2 arm={} fwdApplied={} refused={} noC={} "
                            "postOk={} postBad={} preApplied={} preUnmatched={} "
                            "cRefreshes={} sibSeen={} sibViewmodel={} dumped={}\n"
                            "READING IT: the projection swap mvp' = C * mvp with C = "
                            "VP_world * inv(VP_weapon) (camera cancels; C is refreshed "
                            "from same-frame pairs and is scene-constant otherwise), "
                            "applied at submit time to every classified viewmodel "
                            "record: forward 160s AND the 208 depth-prepass / 496 "
                            "forward-variant siblings, both passes. fwdApplied climbing "
                            "~4-6/frame AND preApplied ~6-8/frame with postOk == their "
                            "sum is the engagement proof (POST verifies the written "
                            "mvp's depth slab became the WORLD's a~1.0). arm=off means "
                            "slot 3 (CTRL+Insert) killed it live. noC climbing = the "
                            "constant was never latched (needs one frame of forward + "
                            "world records classifying - see FHCENSUS viewmodel/"
                            "worldRef). preApplied=0 while fwdApplied climbs = BLACK "
                            "FRINGES expected where the stale crushed-depth silhouette "
                            "blocks the world - the sibling path failing, not the "
                            "substitution.",
                            exp_on(2) ? "ON" : "off",
                            g_gf2_applied.load(std::memory_order_relaxed),
                            g_gf2_refused.load(std::memory_order_relaxed),
                            g_gf2_no_wvp.load(std::memory_order_relaxed),
                            g_gf2_post_ok.load(std::memory_order_relaxed),
                            g_gf2_post_bad.load(std::memory_order_relaxed),
                            g_gf2_pre_applied.load(std::memory_order_relaxed),
                            g_gf2_pre_unmatched.load(std::memory_order_relaxed),
                            g_gf2_c_refreshes.load(std::memory_order_relaxed),
                            g_fh_sib_seen.load(std::memory_order_relaxed),
                            g_fh_sib_viewmodel.load(std::memory_order_relaxed),
                            g_gf2_dumped.load(std::memory_order_relaxed)));
                    }
                }
                {
                    std::string cs;
                    for (int s = 0; s < CS_COUNT; ++s) {
                        const uint32_t c0 = g_cam_state[s][0].load(std::memory_order_relaxed);
                        const uint32_t c1 = g_cam_state[s][1].load(std::memory_order_relaxed);
                        const uint32_t c2 = g_cam_state[s][2].load(std::memory_order_relaxed);
                        const uint32_t c3 = g_cam_state[s][3].load(std::memory_order_relaxed);
                        if (c0 || c1 || c2 || c3)
                            cs += std::format(" {}[clean={} rot1={} rot2={} other={}]",
                                              kCamSiteNames[s], c0, c1, c2, c3);
                    }
                    log::warn(std::format(
                        "CamStateCensus{}\n"
                        "READING IT: renderView+0xC44's rotation state (vs clean / +1 head "
                        "rotation / +2) sampled at each gather job. Lever A1 fires inside "
                        "GatherPrepare, so sites reading CLEAN afterward compose their MVPs "
                        "BEFORE the write reaches them - that is where the gen-0 welded "
                        "populations (reticle, effects) are built. Sites reading rot2 are "
                        "where the gen-2 doubled populations (gun, windows) are built. This "
                        "names the producer seam for each defect with no behavioural change.",
                        cs.empty() ? std::string(" (no samples - head look off or not armed)")
                                   : cs));
                }
                log::warn(std::format(
                    "LIGHTDIV moving[pairs={} diverged={}] still[pairs={} diverged={}] "
                    "worstDelta={} | mirrorsCleared={}\n"
                    "READING IT: both frame-root passes run CullLights and both write the SAME "
                    "cluster SSBOs. If the two passes compute the same light list the overwrite "
                    "is harmless; if they DIVERGE, which data the GPU reads is a race - the "
                    "flicker and the blowouts. The owner sees both artifacts ONLY while moving, "
                    "so the prediction is diverged/pairs HIGH while moving and NEAR ZERO while "
                    "still. If it diverges just as often standing still, the race is not what is "
                    "being seen and this is the wrong tree - that is what the still bucket is "
                    "for. pairs=0 in both means the two passes never reported in the same frame "
                    "and the instrument measured nothing, which is not a null.",
                    g_cl_pairs_moving.load(std::memory_order_relaxed),
                    g_cl_diverge_moving.load(std::memory_order_relaxed),
                    g_cl_pairs_still.load(std::memory_order_relaxed),
                    g_cl_diverge_still.load(std::memory_order_relaxed),
                    g_cl_worst_delta.load(std::memory_order_relaxed),
                    g_mirror_cleared.load(std::memory_order_relaxed)));
                log::warn(std::format(
                    "JITTER real[applied={} flips={} doubleApply={} lastOff={:.3f}] "
                    "dbl[applied={} flips={} doubleApply={} lastOff={:.3f}]\n"
                    "READING IT: the owner reports BOTH eyes jittering with PageUp engaged and "
                    "rock solid without. This asks whether the stereo offset lands consistently. "
                    "HEALTHY is applied ~= frames on each side, flips ~= 0, doubleApply = 0. "
                    "flips climbing on the REAL side means DOOM's own frame is being shifted by "
                    "a different amount frame to frame - the whole scene steps back and forth by "
                    "the IPD, which is exactly a both-eyes jitter. doubleApply > 0 means the "
                    "once-per-frame dedup latch was cleared under us, which is what would happen "
                    "when RenderGatherPrepare runs twice per frame under doubling. Both zero "
                    "means the offset is stable and the jitter is NOT the camera - and that "
                    "sends this at the second frame-root entry's other side effects instead.",
                    g_jit_applied[0].load(std::memory_order_relaxed),
                    g_jit_flips[0].load(std::memory_order_relaxed),
                    g_jit_double_apply[0].load(std::memory_order_relaxed),
                    g_jit_last_offset[0].load(std::memory_order_relaxed),
                    g_jit_applied[1].load(std::memory_order_relaxed),
                    g_jit_flips[1].load(std::memory_order_relaxed),
                    g_jit_double_apply[1].load(std::memory_order_relaxed),
                    g_jit_last_offset[1].load(std::memory_order_relaxed)));
                log::warn(std::format(
                    "PRECULL arm={} (config={}) attempts={} WRITTEN={} published={} | "
                    "refused: noHead={} noBasis={} axisBad={} postBad={} doubleGuard={} "
                    "writeFail={} readbackBad={} angleBad={} | rebuilt +0x1270 clean={} "
                    "rot1={} rot2={} other={} | suppressed LeverA={} LeverV={} | "
                    "publishObj match={} MISMATCH={}\n"
                    "READING IT: the ARM LABEL IS ON THIS LINE, because CTRL+Up flips the "
                    "mode live and a count read against the wrong arm is worthless. "
                    "WRITTEN should track VIEWSRC calls ~1:1 while arm=ON; WRITTEN=0 with "
                    "attempts climbing means every write was refused and the reason is named "
                    "above - it is never an unexplained null. angleBad>0 means the rotation "
                    "came back as identity while a real angle was commanded (an unpublished "
                    "basis), which would otherwise look like a correct write that does "
                    "nothing. The decisive row is `rebuilt +0x1270`: after the write and the "
                    "rebuild, the producer's own world VP must read rot1. clean there means "
                    "the write did not reach the matrices; rot2 means it is being applied "
                    "twice and one of the legacy sites is still live. suppressed LeverA/V "
                    "climbing is the proof that the new site REPLACED them rather than "
                    "stacking with them. publishObj MISMATCH>0 means the clean camera is "
                    "being published from a different viewParms than the one SetupView copies "
                    "into renderView+0xC44 - the label would be on the wrong matrix, and the "
                    "publish must move to that object before this build is trusted.",
                    wrap::precull_active() ? "ON" : "off",
                    wrap::config().precull ? "on" : "off",
                    g_pc_attempts.load(std::memory_order_relaxed),
                    g_pc_written.load(std::memory_order_relaxed),
                    g_pc_published.load(std::memory_order_relaxed),
                    g_pc_no_head.load(std::memory_order_relaxed),
                    g_pc_no_basis.load(std::memory_order_relaxed),
                    g_pc_axis_bad.load(std::memory_order_relaxed),
                    g_pc_post_bad.load(std::memory_order_relaxed),
                    g_pc_double_guard.load(std::memory_order_relaxed),
                    g_pc_write_fail.load(std::memory_order_relaxed),
                    g_pc_readback_bad.load(std::memory_order_relaxed),
                    g_pc_angle_bad.load(std::memory_order_relaxed),
                    g_pc_post_state[0].load(std::memory_order_relaxed),
                    g_pc_post_state[1].load(std::memory_order_relaxed),
                    g_pc_post_state[2].load(std::memory_order_relaxed),
                    g_pc_post_state[3].load(std::memory_order_relaxed),
                    g_lever_a_precull_suppressed.load(std::memory_order_relaxed),
                    g_lever_v_precull_suppressed.load(std::memory_order_relaxed),
                    g_pc_publish_obj_match.load(std::memory_order_relaxed),
                    g_pc_publish_obj_mismatch.load(std::memory_order_relaxed)));
                log::warn(std::format(
                    "VIEWSRC producer=DOOM+0x1830B80 calls={} lastFrame={} frames={} | "
                    "beforeA1={} afterA1={} | +0x1270 clean={} rot1={} rot2={} other={} "
                    "noRef={} noHead={} | nullArg={} readFail={} notGameplay={} "
                    "identityView={} altScale={} | axisSameInFrame={} axisDiffInFrame={} "
                    "framePipelined={} | c44 attempts={} match={} mismatch={}\n"
                    "READING IT: calls=0 is INSTRUMENT FAILURE (hook not installed), not a "
                    "null - there is no gameplay frame without this call. lastFrame is the "
                    "per-frame RATE, which is the engagement figure; frames is how many "
                    "frames carried at least one call. beforeA1==calls confirms the producer "
                    "runs ahead of Lever A, which is what makes it the right write site. "
                    "The +0x1270 row is the decisive one: CLEAN means the producer never "
                    "sees the head rotation and the cluster camera is a SEPARATE COPY "
                    "(step 1's claim); rot1 would mean an ordering problem instead. "
                    "c44 match>0 proves live that renderView+0xC44 is a copy of "
                    "viewParms+0x1270 - the reason Lever A cannot reach the froxel lists. "
                    "framePipelined counts frames whose first axis equals the previous "
                    "frame's first axis (a frame-pipelining check, not a defect on its own).",
                    g_vs_calls.load(std::memory_order_relaxed),
                    g_vs_calls_last_frame.load(std::memory_order_relaxed),
                    g_vs_frames.load(std::memory_order_relaxed),
                    g_vs_before_a1.load(std::memory_order_relaxed),
                    g_vs_after_a1.load(std::memory_order_relaxed),
                    g_vs_state[0].load(std::memory_order_relaxed),
                    g_vs_state[1].load(std::memory_order_relaxed),
                    g_vs_state[2].load(std::memory_order_relaxed),
                    g_vs_state[3].load(std::memory_order_relaxed),
                    g_vs_no_ref.load(std::memory_order_relaxed),
                    g_vs_no_head.load(std::memory_order_relaxed),
                    g_vs_null_arg.load(std::memory_order_relaxed),
                    g_vs_read_fail.load(std::memory_order_relaxed),
                    g_vs_not_gameplay.load(std::memory_order_relaxed),
                    g_vs_identity_view.load(std::memory_order_relaxed),
                    g_vs_alt_scale.load(std::memory_order_relaxed),
                    g_vs_axis_same_as_prev.load(std::memory_order_relaxed),
                    g_vs_axis_diff_in_frame.load(std::memory_order_relaxed),
                    g_vs_frame_pipelined.load(std::memory_order_relaxed),
                    g_vs_c44_attempts.load(std::memory_order_relaxed),
                    g_vs_c44_match.load(std::memory_order_relaxed),
                    g_vs_c44_mismatch.load(std::memory_order_relaxed)));
                log::warn(std::format(
                    "VIEWSRC/CCF binner=CullLights DOOM+0x187E330 calls={} sameObject={} otherObject={} "
                    "nullArg={} readFail={} afterA1={} | its +0x1270 clean={} rot1={} rot2={} "
                    "other={}\n"
                    "READING IT: calls=0 while VIEWSRC calls climb means THIS hook is dead, not "
                    "that the binner is idle. sameObject==calls closes the identity gap: the "
                    "object the froxel binner reads IS the object the producer builds, so a "
                    "write at the producer necessarily reaches the light lists. otherObject>0 "
                    "would mean a SECOND camera copy exists and the producer write would not be "
                    "enough on its own. Its +0x1270 reading CLEAN with Lever A armed is the "
                    "EFF12 result reproduced live, from the engine side rather than from a "
                    "capture.",
                    g_ccf_calls.load(std::memory_order_relaxed),
                    g_ccf_same_object.load(std::memory_order_relaxed),
                    g_ccf_other_object.load(std::memory_order_relaxed),
                    g_ccf_null_arg.load(std::memory_order_relaxed),
                    g_ccf_read_fail.load(std::memory_order_relaxed),
                    g_ccf_after_a1.load(std::memory_order_relaxed),
                    g_ccf_state[0].load(std::memory_order_relaxed),
                    g_ccf_state[1].load(std::memory_order_relaxed),
                    g_ccf_state[2].load(std::memory_order_relaxed),
                    g_ccf_state[3].load(std::memory_order_relaxed)));
                log::warn(std::format(
                    "HeadWriteSites BUILD-B renderView={} guiView={} (attempts {} refused {}: "
                    "noRot {} shape {}) vrot={} "
                    "destFixed={}/{} (refused {} skipped {}) sitesNamed={}/12 "
                    "ARM destFixedWrites={} gunBase={} leverVrot={} (suppressed {}) tnorm={} "
                    "leverAsupp={}\n"
                    "GuiModels attempts={} written={} restored={} noArray={} | "
                    "RETIRED passSkipped={} leverVextra={}\n"
                    "GuiBake VERIFY-ONLY attempts={} verified={} mismatch={} patched={} "
                    "noInputs={} noRenderer={} noProj={} noMvp={} worstRelDiff={:.6f}\n"
                    "GPUCAM attempts={} rotated={} clean={} other={} | cleanPipeline={:#x} "
                    "rotatedPipeline={:#x}\n"
                    "READING IT: BUILD B removed four write sites, so the fields that used to "
                    "sit here are GONE ON PURPOSE, not broken - cameraSrc, destCopies, basis3, "
                    "cameraSrcOffsets, destOffsetsFound and GPUCAM WRITES all belonged to the "
                    "deleted paths (see the REMOVED 2026-08-26 BUILD B tombstone in "
                    "vulkan_hooks.cpp). THREE rotation sites remain and all three are on this "
                    "line: renderView is Lever A, destFixed is the fixed +0xC44/+0xC84 "
                    "destination rotation, guiView is Lever V. A site at ZERO never ran. "
                    "guiView: attempts frozen = unreachable; applied=0 with attempts climbing "
                    "and noRot climbing = the world basis/angles are not published; shape "
                    "climbing = the gameplay GUI view is not the affine layout and its first "
                    "three dumps say what it is. destFixed refused EXACTLY half last run; "
                    "sitesNamed counts the DestFixedSite one-shot lines that say WHICH half - "
                    "grep DestFixedSite, expect up to twelve. GPUCAM is read-only now and it "
                    "is the row that matters: it counts the camera-shaped uniforms the GPU is "
                    "actually BOUND, classified against DOOM's clean basis and against the "
                    "rotated basis we wrote. clean>0 rotated=0 means our rotation never reaches "
                    "the GPU. BOTH>0 is a split, and cleanPipeline names the pass that refuses "
                    "to move. clean=0 with rotated climbing is the 14:47 result and means every "
                    "camera-shaped record the GPU binds carries our rotation - what is left "
                    "welded then lives in `other`, which is Build C's target.",
                    wrap::head_rotations_applied(),
                    gui_head_rotations.load(std::memory_order_relaxed),
                    gui_head_attempts.load(std::memory_order_relaxed),
                    gui_head_refusals.load(std::memory_order_relaxed),
                    gui_head_norot.load(std::memory_order_relaxed),
                    gui_head_shape_refusals.load(std::memory_order_relaxed),
                    wrap::vrot_mode_name(wrap::vrot_mode()),
                    dest_fixed_rotations.load(std::memory_order_relaxed),
                    dest_fixed_attempts.load(std::memory_order_relaxed),
                    dest_fixed_refusals.load(std::memory_order_relaxed),
                    dest_fixed_skipped.load(std::memory_order_relaxed),
                    dest_fixed_sites_logged.load(std::memory_order_relaxed),
                    // THE ARM STATE, ON THE MEASUREMENT LINE ITSELF. Both of
                    // this build's toggles are live keypresses, so a line that
                    // did not carry its own arm label could be read against
                    // the wrong arm afterwards.
                    dest_fixed_write_enabled.load(std::memory_order_relaxed) ? "ON" : "OFF",
                    gun_base_raw.load(std::memory_order_relaxed) ? "RAW" : "head-rotated",
                    lever_v_rot_enabled.load(std::memory_order_relaxed) ? "ON" : "OFF",
                    gui_head_rot_suppressed.load(std::memory_order_relaxed),
                    translation_normalise.load(std::memory_order_relaxed) ? "ON" : "OFF",
                    g_lever_a_rot_suppressed.load(std::memory_order_relaxed),
                    g_model_attempts.load(std::memory_order_relaxed),
                    g_model_written.load(std::memory_order_relaxed),
                    g_model_restored.load(std::memory_order_relaxed),
                    g_model_no_array.load(std::memory_order_relaxed),
                    g_gui_pass_skipped.load(std::memory_order_relaxed),
                    g_leverv_extra.load(std::memory_order_relaxed),
                    g_bake_attempts.load(std::memory_order_relaxed),
                    g_bake_verified.load(std::memory_order_relaxed),
                    g_bake_mismatch.load(std::memory_order_relaxed),
                    g_bake_patched.load(std::memory_order_relaxed),
                    g_bake_no_inputs.load(std::memory_order_relaxed),
                    g_bake_no_renderer.load(std::memory_order_relaxed),
                    g_bake_no_proj.load(std::memory_order_relaxed),
                    g_bake_no_mvp.load(std::memory_order_relaxed),
                    g_bake_worst_diff.load(std::memory_order_relaxed),
                    g_gpu_cam_attempts.exchange(0, std::memory_order_relaxed),
                    g_gpu_cam_rotated.exchange(0, std::memory_order_relaxed),
                    g_gpu_cam_clean.exchange(0, std::memory_order_relaxed),
                    g_gpu_cam_other.exchange(0, std::memory_order_relaxed),
                    g_gpu_cam_clean_pipeline.load(std::memory_order_relaxed),
                    g_gpu_cam_rotated_pipeline.load(std::memory_order_relaxed)));
                log_slot_map();
                // ===== IPDHB - what each PASS actually rendered with =======
                // Its own line, unconditional inside the once-a-second block,
                // so it survives every other probe being off. Counters are
                // cumulative (not exchanged) so a frozen one is visibly
                // frozen rather than reading a fresh zero.
                log::warn(std::format(
                    "IPDHB arm={} override={:.2f} scripted={:.2f} | pass1 n={} zero={} last={:.3f}"
                    " | pass2 n={} zero={} last={:.3f} | toggles={} refreshApplied={} refreshSup={}\n"
                    "READING IT: read AT THE PASS, after every branch that can zero an offset, "
                    "not at the keypress. Both last= at 0.000 with both n= climbing is the ipd-0 "
                    "arm actually reaching both passes - that is the middle approach's proof. "
                    "pass2 n=0 means PG UP was never pressed: no doubled call exists, the run "
                    "cannot judge a doubling-only defect and is VOID rather than negative. "
                    "arm=ON with last!=0 is a BROKEN PROBE, not a refutation. pass1 last is a "
                    "HALF offset and pass2's is the full one by design, so they differ in "
                    "magnitude when the override is off - only ZERO must agree.",
                    (g_ipd_override.load(std::memory_order_relaxed) >= 0.0f)
                        ? "ON (override active)" : "off (scripted value)",
                    g_ipd_override.load(std::memory_order_relaxed),
                    frame_root_double_ipd.load(std::memory_order_relaxed),
                    g_ipdhb_p1_n.load(std::memory_order_relaxed),
                    g_ipdhb_p1_zero.load(std::memory_order_relaxed),
                    g_ipdhb_p1_last.load(std::memory_order_relaxed),
                    g_ipdhb_p2_n.load(std::memory_order_relaxed),
                    g_ipdhb_p2_zero.load(std::memory_order_relaxed),
                    g_ipdhb_p2_last.load(std::memory_order_relaxed),
                    g_ipd_toggles.load(std::memory_order_relaxed),
                    g_ipd_refresh_applied.load(std::memory_order_relaxed),
                    g_ipd_refresh_suppressed.load(std::memory_order_relaxed)));
                // ===== RPCENSUS + BARFIX ===================================
                log::warn(std::format(
                    "RPCENSUS doubled-call render passes: total={} fellThrough={} "
                    "withClears={} | RPFIX arm={} redirected={} noMirror={} | "
                    "BARFIX arm={} appended={} calls={} | oldSubstitutePath calls={}\n"
                    "READING IT: fellThrough is render passes the doubled call began on "
                    "DOOM'S OWN framebuffer instead of a mirror; withClears is the subset "
                    "carrying clears, whose loadOp wipes DOOM's real attachment. MEASURED "
                    "2026-08-31: fellThrough=3728 withClears=3728 - EVERY fall-through "
                    "clears, full screen size, ~once per displayed frame. Nothing tested "
                    "that day could reach it (13 jobs, final pass, both, frame rate, "
                    "barrier substitution - all null), because a render pass is begun "
                    "whether or not a job draws.\n"
                    "⚠ RPFIX ENGAGEMENT: redirected= must CLIMB and withClears must FALL "
                    "TO ~0 while armed. redirected=0 with noMirror climbing means no mirror "
                    "could be built for those framebuffers - the run is VOID, not a null, "
                    "and the fall-through is deliberately left intact because a draw with "
                    "no active render pass is worse than a wasted clear. "
                    "⚠ BARFIX: oldSubstitutePath MUST read 0 while barfix is armed - if it "
                    "and appended both climb, the two paths are racing and the run is VOID.",
                    g_rpc_total.load(std::memory_order_relaxed),
                    g_rpc_fellthrough.load(std::memory_order_relaxed),
                    g_rpc_withclears.load(std::memory_order_relaxed),
                    g_rpfix_on.load(std::memory_order_relaxed) ? "ON" : "off",
                    g_rpfix_redirected.load(std::memory_order_relaxed),
                    g_rpfix_nomirror.load(std::memory_order_relaxed),
                    g_barfix_on.load(std::memory_order_relaxed) ? "ON" : "off",
                    g_barfix_appended.load(std::memory_order_relaxed),
                    g_barfix_calls.load(std::memory_order_relaxed),
                    barrier_calls_rewritten.load(std::memory_order_relaxed)));
                {
                    const std::string bf = wrap::bodyfollow_report();
                    if (!bf.empty()) log::warn(bf);
                }
                // DESTCAMHUNT liveness, so a missing DestCamHunt line is
                // attributable without another run.
                log::warn(std::format(
                    "CamHunt STATUS destMask={:#04x}/0x3f guiCtxMask={:#x}/0x7 calls={} hunts={} noRef={} "
                    "gunAngle attempts={} lines={}/{} | doneMask stuck at 0 with calls "
                    "climbing means the hunt never reached gameplay; noRef climbing means "
                    "renderView's own world VP would not solve, which would be a defect in "
                    "the reference, not in the destinations.",
                    g_dch_done_mask.load(std::memory_order_relaxed),
                    g_gui_ctx_hunt_mask.load(std::memory_order_relaxed),
                    g_dch_calls.load(std::memory_order_relaxed),
                    g_dch_hunts.load(std::memory_order_relaxed),
                    g_dch_no_ref.load(std::memory_order_relaxed),
                    g_gunang_attempts.load(std::memory_order_relaxed),
                    g_gunang_lines.load(std::memory_order_relaxed),
                    kMaxGunAngleLines));
            }
        }
    }
    if (xr().ready()) {
#ifdef DOOMVR_RESEARCH_TOOLS
        VkImage override_source_image = VK_NULL_HANDLE;
        VkExtent2D override_source_extent{};
        VkFormat override_source_format = VK_FORMAT_UNDEFINED;
        // FrameRootDouble live mode joins the same presentation path that
        // StereoPreview and TrueStereo already use: eye 0 keeps DOOM's own
        // real composited frame, eye 1 gets our offscreen render - which, as
        // of 2026-08-16, is a genuinely diverged viewpoint rather than a copy.
        const bool frame_root_live =
            frame_root_double_enabled.load(std::memory_order_relaxed) &&
            !frame_root_double_ab.load(std::memory_order_relaxed);
        // FULL-FRAME: eye 1 comes from the holding image, which carries the
        // REAL call's fully composited +half frame. Eye 0 is the swapchain,
        // which carries the doubled call's -half frame. Both complete.
        const bool full_frame_live =
            frame_root_live && frame_root_full_frame.load(std::memory_order_relaxed) &&
            composite_holding.ready.load(std::memory_order_relaxed);
        const bool want_override =
            full_frame_live ||
            ((stereo_preview_enabled.load(std::memory_order_relaxed) ||
              true_stereo_enabled.load(std::memory_order_relaxed) ||
              frame_root_live) &&
             offscreen_targets[0].ready.load(std::memory_order_relaxed));
        // GENERIC MIRRORING: eye 1 is the doubled run's FINAL mirrored colour
        // image - the last framebuffer it rendered into, which with the job
        // filter off is the end of its own post-process chain.
        const DoubledMirrorState mirror = load_doubled_mirror();
        const bool mirrored_live = frame_root_live &&
            mirror_enabled.load(std::memory_order_relaxed) &&
            mirror.color != VK_NULL_HANDLE;
        // SAME-FRAME TAG (G2). Log-only; nothing branches on these numbers.
        // Deliberately NOT nested inside the mirrored_live branch: if that
        // branch is not taken, the reason is exactly what we would want to
        // know, so the line reports the flags too rather than going silent.
        // presentSerial == mirrorSerial every line => the eye-1 image was
        // produced by THIS frame's doubled call (claim 10 confirmed).
        // A persistent mirrorSerial == presentSerial - 1 => one-frame lag.
        if (frame_root_double_enabled.load(std::memory_order_relaxed)) {
            const uint32_t n = mirror_serial_log_count.fetch_add(1, std::memory_order_relaxed);
            if (n < 10 || n % 500 == 0) {
                const uint32_t present_serial = g_frame_root_serial.load(std::memory_order_relaxed);
                log::warn(std::format(
                    "SameFrameTag #{}: presentSerial={} mirrorSerial={} delta={} "
                    "mirroredLive={} frameRootLive={} mirrorEnabled={} mirrorColor={}",
                    n, present_serial, mirror.serial,
                    static_cast<int64_t>(present_serial) - static_cast<int64_t>(mirror.serial),
                    mirrored_live, frame_root_live,
                    mirror_enabled.load(std::memory_order_relaxed),
                    reinterpret_cast<void*>(mirror.color)));
            }
        }
        // SYMMETRIC STEREO (STEP 8). Eye 0 stops being DOOM's finished
        // swapchain image and becomes the REAL call's own pre-final composite,
        // held in composite_holding - the same pipeline stage eye 1's mirror
        // captures, one separation away. Both eyes then travel the identical
        // blit path in identical formats, so any remaining colour behaviour
        // applies to both equally instead of only to one.
        //
        // Refuses rather than half-applies: without a ready holding image
        // this would silently leave eye 0 as DOOM's graded frame and eye 1 as
        // an ungraded mirror, which is the mismatched pair this exists to
        // remove, and it would look like a colour bug rather than a missing
        // precondition.
        // GRADED STEREO (2026-08-18). The owner asked to SEE grading with
        // parallax, which until now existed only in a PPM.
        //
        // Eye 0 goes back to DOOM's OWN finished frame - graded, HUD, and free,
        // since DOOM renders it anyway - and eye 1 becomes the doubled final
        // stage's output, which is that same grade and HUD applied to eye 1's
        // own composite. Both eyes are then post-grade and both are format 44,
        // so the format mismatch that sent the eyes down different colour
        // paths (122 against 44) is gone too.
        //
        // ⚠ THE EYES WILL NOT MATCH IN CONTENT, and that is expected, not a
        // regression: eye 1's composite still has no volumetric fog, fire or
        // glow, so eye 0 shows a hellscape and eye 1 a clear-weather desert.
        // That is the unsolved P3 problem, now visible in two finished frames
        // instead of one. Judge the GRADE and the DEPTH here; the missing
        // weather is known and is not what this mode is for.
        bool graded_live = false;
        if (graded_stereo_enabled.load(std::memory_order_relaxed) &&
            final_pass_mirror_color != VK_NULL_HANDLE) {
            graded_live = true;
            static std::atomic_uint32_t graded_log{0};
            const uint32_t g = graded_log.fetch_add(1, std::memory_order_relaxed);
            if (g < 3 || (g % 600) == 0) {
                log::warn(std::format(
                    "GradedStereo #{}: eye0=DOOM's own finished frame (graded+HUD, -half, LEFT) "
                    "| eye1=finalStage mirror {} {}x{} fmt={} (graded+HUD, +half, RIGHT). Both "
                    "post-grade, both fmt 44. Content differs by the KNOWN missing effects in "
                    "eye 1, not by a presentation fault.",
                    g, reinterpret_cast<void*>(final_pass_mirror_color),
                    final_pass_mirror_extent.width, final_pass_mirror_extent.height,
                    static_cast<int>(final_pass_mirror_format)));
            }
        }
        // XR-PATH LIVENESS (2026-08-28). Counted so every log states plainly
        // whether the headset submission path ran at all this session.
        //
        // This exists because of a mistake it would have caught: SHIFT+END was
        // built to toggle eye 0 between the composite holding image and DOOM's
        // finished frame, and the owner reported "no change" - which was read
        // as a result. It was not. These runs are headset-free, the XR path
        // never executes, and the MONITOR always shows DOOM's own presented
        // swapchain image regardless of this setting. The toggle was
        // structurally incapable of changing what was being judged, and
        // nothing in the log said so.
        xr_present_path_hits.fetch_add(1, std::memory_order_relaxed);
        bool symmetric_live = false;
        // Graded stereo owns eye 0 when it is live: leaving the symmetric
        // override in place would put the UNGRADED pre-final composite in eye
        // 0 against a graded eye 1, which is the mismatched pair both of these
        // modes exist to avoid.
        if (!graded_live && symmetric_stereo_enabled.load(std::memory_order_relaxed)) {
            if (mirrored_live && composite_holding.ready.load(std::memory_order_relaxed) &&
                composite_holding.image != VK_NULL_HANDLE) {
                source = composite_holding.image;
                extent = composite_holding.extent;
                format = composite_holding.format;
                symmetric_live = true;
            } else {
                static std::atomic_uint32_t sym_refused{0};
                if (sym_refused.fetch_add(1, std::memory_order_relaxed) % 300 == 0) {
                    log::error(std::format(
                        "SymmetricStereo NOT applied: mirroredLive={} holdingReady={}. Eye 0 is "
                        "still DOOM's finished frame and eye 1 an ungraded mirror - a MISMATCHED "
                        "pair, not symmetric stereo. Do not judge eye quality from this.",
                        mirrored_live,
                        composite_holding.ready.load(std::memory_order_relaxed)));
                }
            }
        }
        if (symmetric_live) {
            const uint32_t n = symmetric_log_count.fetch_add(1, std::memory_order_relaxed);
            if (n < 5 || n % 500 == 0) {
                log::warn(std::format(
                    // storageMirrorSubs/Refusals ride along here because run B
                    // reported them ONLY at go-live, before steady state, and
                    // that snapshot (subs=1 refusals=2) was too early to mean
                    // anything. A counter you can only read before the thing
                    // starts is not a measurement.
                    "SymmetricStereo #{}: eye0=holding {} {}x{} fmt={} (real call, -half, LEFT) | "
                    "eye1=mirror {} {}x{} fmt={} (doubled call, +half, RIGHT) | ipd={:.2f} uu. Formats "
                    "must MATCH, or the eyes take different colour paths. | storageMirrors "
                    "created={} subs={} layoutRefusals={}: subs climbing means the doubled "
                    "call is writing OUR image and no longer scribbling on DOOM's; subs stuck "
                    "at 0 means it still is.",
                    n, reinterpret_cast<void*>(composite_holding.image),
                    composite_holding.extent.width, composite_holding.extent.height,
                    static_cast<int>(composite_holding.format),
                    reinterpret_cast<void*>(mirror.color), mirror.extent.width,
                    mirror.extent.height, static_cast<int>(mirror.format),
                    frame_root_double_ipd.load(std::memory_order_relaxed),
                    storage_mirror_created.load(std::memory_order_relaxed),
                    storage_mirror_substitutions.load(std::memory_order_relaxed),
                    storage_mirror_layout_refusals.load(std::memory_order_relaxed)));
            }
        }
        if (graded_live) {
            // Eye 1 = the graded, HUD-carrying frame built from eye 1's own
            // composite. Checked BEFORE mirrored_live, which would otherwise
            // win and put the ungraded frame-root mirror here.
            override_source_image = final_pass_mirror_color;
            override_source_extent = final_pass_mirror_extent;
            override_source_format = final_pass_mirror_format;
        } else if (mirrored_live) {
            override_source_image = mirror.color;
            override_source_extent = mirror.extent;
            override_source_format = mirror.format;
        } else if (full_frame_live) {
            override_source_image = composite_holding.image;
            override_source_extent = composite_holding.extent;
            override_source_format = composite_holding.format;
        } else if (want_override) {
            // Content lands in slot 0 now that both passes share its images.
            override_source_image = offscreen_targets[0].color_images[0];
            override_source_extent = offscreen_targets[0].extent;
            override_source_format = kOffscreenColorFormats[0];
        }
        // The layout of THE IMAGE WE ACTUALLY CHOSE, not of a different one.
        //
        // Hardcoding COLOR_ATTACHMENT_OPTIMAL here was correct only while
        // mirrors received no barriers; once barrier substitution began
        // applying DOOM's real transitions to them, blitting from a
        // wrongly-declared layout is undefined and blacked out eye 1. That
        // lesson was learned once and applied here - but only to
        // mirror.color, the FRAME-ROOT mirror.
        //
        // GRADED STEREO BROKE THAT (2026-08-20, measured in doom-capture.rdc).
        // When graded stereo is live, eye 1's source is
        // final_pass_mirror_color, a DIFFERENT image from mirror.color, and
        // the old `if (mirrored_live)` above handed render_eye the tracked
        // layout of the wrong one. In the capture, at the end of the frame:
        //
        //   96347  (mirror.color, frame-root)      SHADER_READ_ONLY_OPTIMAL(5)
        //                                          - barrier at eid 11696
        //   100428 (final_pass_mirror_color)       PRESENT_SRC_KHR(1000001002)
        //                                          - barrier at eid 11718
        //
        // and vkQueuePresentKHR is eid 11722, so eye 1's blit declared
        // oldLayout=5 for an image the driver holds in PRESENT_SRC_KHR. A
        // transition whose oldLayout does not match the image's actual layout
        // is permitted to DISCARD the contents - the exact failure this
        // project already hit on the dump path (see the comment above, and
        // hook_final_stage's note that the dump reads the TRACKED layout for
        // this reason). render_eye also uses this value as the newLayout of
        // its RESTORE barrier, so the wrong value additionally left the image
        // mis-tracked for the next frame's substituted barrier at eid 11696,
        // which expects PRESENT_SRC_KHR. One read of the right image's
        // tracked layout fixes both halves.
        //
        // mirror_layout_of returns COLOR_ATTACHMENT_OPTIMAL for any image it
        // does not track, so the non-mirror sources (composite_holding,
        // offscreen_targets) keep exactly the value they had before.
        VkImageLayout override_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkImageLayout frame_root_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        if (override_source_image != VK_NULL_HANDLE) {
            std::scoped_lock lock(mirror_mutex);
            override_layout = mirror_layout_of(override_source_image);
            frame_root_layout = mirror_layout_of(mirror.color);
        }
        // Rule 3: this is the value the whole fix turns on, so it is stated
        // rather than inferred. graded=1 with layout=1000001002 is the
        // repaired case; graded=1 with layout=5 is the defect reappearing.
        if (override_source_image != VK_NULL_HANDLE &&
            (presented_frame.load(std::memory_order_relaxed) % 300 == 0 ||
             eye1_layout_lines.fetch_add(1, std::memory_order_relaxed) < 5)) {
            log::warn(std::format(
                "Eye1Layout: presenting image={} {}x{} fmt={} declaredLayout={} graded={} "
                "mirroredLive={} (frameRootMirror={} its layout={}). declaredLayout is now read "
                "from the image being presented; 2=COLOR_ATTACHMENT, 5=SHADER_READ_ONLY, "
                "1000001002=PRESENT_SRC. A mismatch here discards eye 1's contents.",
                reinterpret_cast<void*>(override_source_image), override_source_extent.width,
                override_source_extent.height, static_cast<int>(override_source_format),
                static_cast<int>(override_layout), graded_live, mirrored_live,
                reinterpret_cast<void*>(mirror.color),
                static_cast<int>(frame_root_layout)));
        }
        if (mirrored_live && (frame_root_double_attempts.load(std::memory_order_relaxed) < 5 ||
                              presented_frame.load(std::memory_order_relaxed) % 500 == 0)) {
            log::warn(std::format(
                "Eye1Source: image={} {}x{} format={} layout={} sourceJobRva={:#x} "
                "(format 122 = HDR world colour; 37/44 = an 8-bit LDR/UI target, which means "
                "the wrong mirror is being presented)",
                reinterpret_cast<void*>(mirror.color), mirror.extent.width, mirror.extent.height,
                static_cast<int>(mirror.format), static_cast<int>(override_layout),
                doubled_mirror_source_job.load(std::memory_order_relaxed)));
        }
        // BASELINE PAIR (2026-08-28). Eye 0 now defaults to DOOM's presented
        // frame, and the owner reports the parallax broke when it did. The two
        // eyes come from different pipeline stages, so there is no reason to
        // assume they share an extent or a format - and the SymmetricStereo log
        // line has always warned that formats must MATCH. A stereo pair whose
        // halves are different sizes cannot fuse, and that reads exactly like
        // "parallax is messed up".
        //
        // Printed side by side rather than in two separate lines, because the
        // whole question is whether they agree. Bounded to the first few frames
        // and then one every 500.
        // BURST ON MODE CHANGE (2026-08-29). The 500-present cadence is ~12 s
        // at the rate these runs measure, so without this the line that
        // decides the exit criterion could arrive long after the transition it
        // is supposed to describe - and be read against the wrong rung. Every
        // apply_graded_mode call reloads this counter, so the pair is printed
        // AT the change and the arm label is on the line itself.
        const bool baseline_burst =
            baseline_pair_burst.load(std::memory_order_relaxed) > 0 &&
            baseline_pair_burst.fetch_sub(1, std::memory_order_relaxed) > 0;
        if (baseline_burst ||
            frame_root_double_attempts.load(std::memory_order_relaxed) < 5 ||
            presented_frame.load(std::memory_order_relaxed) % 500 == 0) {
            // GEOMETRY AND TONE ARE REPORTED SEPARATELY, because they fail
            // separately and the first version of this line did not. It ANDed
            // extent and format together and printed "cannot fuse" - which
            // would have sent a future reader hunting a parallax bug that does
            // not exist. The measured pair is 3818x2032 on BOTH sides with
            // formats 44 and 122: the extents agree, so parallax is fine (the
            // owner confirmed exactly that), and only the tone differs.
            //
            // Format 44 is the LDR, graded, post-final image DOOM presents;
            // 122 is HDR world colour straight off the frame root. That
            // difference IS eye 1's "no effects and not graded properly", now
            // quantified rather than described.
            const bool geometry_ok = extent.width == override_source_extent.width &&
                                     extent.height == override_source_extent.height;
            const bool tone_ok = format == override_source_format;
            log::warn(std::format(
                // gradeRung is READ AT THE MEASUREMENT, not at the arm. A run
                // whose own key flips the mode between arming and judging is
                // exactly how a pair gets attributed to the wrong rung.
                "BaselinePair gradeRung={} eye0={} {}x{} fmt={} | eye1={} {}x{} fmt={} | "
                "geometryMatch={} toneMatch={} | {} {}",
                graded_mode.load(std::memory_order_relaxed),
                reinterpret_cast<void*>(source), extent.width, extent.height,
                static_cast<int>(format),
                reinterpret_cast<void*>(override_source_image),
                override_source_extent.width, override_source_extent.height,
                static_cast<int>(override_source_format), geometry_ok, tone_ok,
                geometry_ok ? "extents agree, so PARALLAX IS NOT AT FAULT here"
                            : "extents DIFFER - the halves are different sizes and cannot fuse; "
                              "any parallax judgement from this pair is void",
                tone_ok ? "and the formats agree too."
                        : "but the formats DIFFER: eye 0 is the graded post-final image and eye "
                          "1 is raw HDR world colour off the frame root. That is the grading "
                          "gap, not a geometry fault - do not chase it in the camera."));
        }
        // ATOMIC ENTRY - see fast_vr_hold_presentation. Withholding the
        // override here (and nowhere earlier) is deliberate: everything above
        // still runs, so the doubled call keeps producing mirrors and
        // convergence keeps advancing, which is exactly what the hold is
        // waiting for. Only the handoff to the headset is deferred, so
        // has_override_source stays false in poll() and the layer stays QUAD.
        if (fast_vr_hold_presentation.load(std::memory_order_relaxed)) {
            override_source_image = VK_NULL_HANDLE;
            override_source_extent = VkExtent2D{};
            override_source_format = VK_FORMAT_UNDEFINED;
        }
        const uint64_t fb_xr0 = fb_ns();
        fb_pre_ns.fetch_add(fb_xr0 - fb_entry_ns, std::memory_order_relaxed);
        xr().poll(source, extent, format, override_source_image, override_source_extent,
            override_source_format, override_layout,
            offscreen_blit_linear_supported.load(std::memory_order_relaxed) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
            true_stereo_enabled.load(std::memory_order_relaxed) || frame_root_live);
        fb_xr_ns.fetch_add(fb_ns() - fb_xr0, std::memory_order_relaxed);
#else
        xr().poll(source, extent, format);
#endif  // DOOMVR_RESEARCH_TOOLS
    }
    const uint64_t fb_present0 = fb_ns();
    const VkResult present_result = real_queue_present(queue, info);
    const uint64_t fb_present1 = fb_ns();
    fb_present_ns.fetch_add(fb_present1 - fb_present0, std::memory_order_relaxed);
    // TEST B - see kPresentThrottleEnabled. AFTER the present, so the sleep
    // lands in the gap BETWEEN frames rather than between DOOM's submit and
    // its present, which would change what is in flight at present time and
    // make this a different experiment.
    if (present_throttle_armed.load(std::memory_order_relaxed)) {
        Sleep(kPresentThrottleMs);
    }
    // COMPUTE-SUPPRESSION STATE, PERIODIC (2026-08-29).
    //
    // The F1 toggle prints the CUMULATIVE total at the moment of the press,
    // and a cumulative total cannot answer the only question that matters
    // about a toggle: did it engage. "suppressedDispatches=812345" reads the
    // same whether the suppression is firing forty times a frame or has not
    // fired since the level loaded.
    //
    // So this prints the DELTA since the last line, which is the engagement
    // check stated in the units of the thing being toggled:
    //
    //   flag ON,  delta > 0  - the suppression is live and doing work. This is
    //                          the shipping state, and it is the state in
    //                          which eye 1 has no fog, fire or sparks.
    //   flag ON,  delta = 0  - NOTHING IS BEING SUPPRESSED. The doubled call
    //                          is not issuing shared-SSBO compute at all, so
    //                          an A/B of this flag is VOID rather than
    //                          negative - it would be toggling something that
    //                          is not happening.
    //   flag OFF, delta = 0  - expected, and the proof the toggle engaged.
    //   flag OFF, delta > 0  - impossible; the counter only increments behind
    //                          the flag. Treat as instrument failure.
    //
    // ⚠ WHAT THIS SUPPRESSES IS BROADER THAN ITS NAME. The gate is
    // doubled_dispatch_writes_shared_buffer: EVERY compute dispatch inside the
    // doubled call whose bound sets contain a storage buffer is skipped, not
    // just the four light-culling ones. GPU particle simulation, emitter
    // state and light lists are all compute-into-SSBO work, which is exactly
    // the additive layer eye 1 is missing - and unlike storage IMAGES, which
    // get a private mirror (storage_mirror_enabled), storage BUFFERS have no
    // mirroring counterpart. They are skipped outright.
    //
    // Bounded by construction: one line per 180 presents (~4 s at the rate
    // these runs measure). The statics are safe for the same reason the
    // throttle window's are - one present thread, after the real present.
    {
        static uint64_t lc_last_total = 0;
        static uint32_t rp_last = 0;
        static uint32_t lc_frames = 0;
        if (++lc_frames >= 180) {
            // RENDERDOC STATE, ON ITS OWN LINE AND NOT APPENDED TO THE ONE
            // BELOW. The ComputeSuppression format string already carries 20+
            // arguments, and a misaligned placeholder in exactly that kind of
            // line shifted every later field and inverted a reported
            // conclusion on 2026-08-22. A new field goes in a new line.
            //
            // Printed EVERY window, present or absent, so the arm label can be
            // read AT the measurement rather than inferred from an arm-time
            // line thousands of lines earlier in the log.
            rdc_configure_if_present("retry");
            rdc_report_new_captures();
            // PLUGIN HEALTH (2026-08-23). Two defects are visible only in the
            // log and both of their log lines are CAPPED - ReadRedirect
            // FALLBACK at `f < 10` and MirrorView DESTROY at `n < 5`, per
            // process run. A ten-minute battle showed 26 and 6 across several
            // runs, and both had hit their caps, so those are TRUNCATIONS and
            // the real frequency is unknown. That is exactly the reading error
            // this project keeps paying for, so the totals are reported here
            // uncapped, with a per-window delta so a growing RATE is visible
            // rather than a total that could have accrued at startup.
            //
            // Neither counter changes behaviour; both atomics already existed.
            {
                static uint32_t rr_last = 0;
                static uint32_t mv_last = 0;
                static uint32_t lv_last = 0;
                static uint32_t la_last = 0;
                static uint32_t fog_ex_last = 0;
                const uint32_t rr = read_redirect_failures.load(std::memory_order_relaxed);
                const uint32_t mv = mirror_view_destroy_count.load(std::memory_order_relaxed);
                const uint32_t lv_reb = lever_v_clean_rebased.load(std::memory_order_relaxed);
                const uint32_t la_app = lever_a_applied.load(std::memory_order_relaxed);
                const uint32_t fog_ex = fog_blocks_examined.load(std::memory_order_relaxed);
                log::warn(std::format(
                    "PluginHealth: readRedirectFallbacks={} (+{} this window) | "
                    "mirrorViewLeaks={} (+{} this window). BOTH LOG LINES ARE CAPPED (10 and 5 "
                    "per run) - these totals are not. readRedirectFallbacks are passes where "
                    "eye 1 binds a set UNMODIFIED and reads DOOM's unshifted images, the same "
                    "class of defect as the Lever V cancellation. mirrorViewLeaks are full-res "
                    "mirror images leaked because a source view was destroyed while "
                    "framebuffers still referenced its mirror. A delta that stays 0 during play "
                    "means both are startup transients; a climbing delta means they are not. "
                    "WHY the clones were refused: noRecords={} arrayBinding={} typeConflict={} "
                    "other={} - uncapped, unlike the FALLBACK line, which stops at 10 per run. "
                    "| STEREO STILL APPLIED? ipd={:.2f} zeroOffset={} leverVrebased={} (+{} this "
                    "window) leverAapplied={} (+{} this window) | destRebased={} destNoClean={} "
                    "destRestored={} destDrift={} | fogExamined={} (+{} this window) fogPatched={} fogSeen={} fogNoDelta={}. "
                    "fogPatched climbing = mode 4 engaged; fogSeen>0 with fogPatched=0 means the "
                    "block was found but no camera displacement was published and the run is NOT "
                    "a test of it. destDrift MUST STAY 0 - non-zero means the "
                    "dest restore is not landing and the basis is walking every frame. "
                    "THIS IS THE GAP THAT COST A WRONG READING ON 2026-08-23: "
                    "normalReturns proves the doubled call RAN, and nothing reported whether it "
                    "applied any SEPARATION. The census only arms on a toggle, so a run could go "
                    "minutes with no coverage. leverVrebased delta at 0 while the doubling still "
                    "climbs means eye 1 is rendering with NO Lever V shift - non-stereo for "
                    "everything Lever V covers - which looks like 'the problem went away'.",
                    rr, rr - rr_last, mv, mv - mv_last,
                    rr_reason_no_records.load(std::memory_order_relaxed),
                    rr_reason_array_binding.load(std::memory_order_relaxed),
                    rr_reason_type_conflict.load(std::memory_order_relaxed),
                    rr_reason_other.load(std::memory_order_relaxed),
                    frame_root_double_ipd.load(std::memory_order_relaxed),
                    frame_root_zero_offset.load(std::memory_order_relaxed),
                    lv_reb, lv_reb - lv_last, la_app, la_app - la_last,
                    dest_rebased.load(std::memory_order_relaxed),
                    dest_no_clean.load(std::memory_order_relaxed),
                    dest_restored.load(std::memory_order_relaxed),
                    dest_drift_warnings.load(std::memory_order_relaxed),
                    fog_blocks_examined.load(std::memory_order_relaxed),

                    fog_ex - fog_ex_last,
                    fog_patch_applied.load(std::memory_order_relaxed),
                    fog_patch_blocks_seen.load(std::memory_order_relaxed),
                    fog_patch_no_delta.load(std::memory_order_relaxed)));
                // ViewOriginScan heartbeat. A probe that reports nothing is
                // indistinguishable from a probe that never ran - which is
                // exactly what cost the 13:42 run - so it states where it got
                // to on every health line. viewsSeen counts every non-null
                // render view reaching the hook; gate counts the subset that
                // passed the world-camera predicate; fires counts scans run.
                //   viewsSeen=0            -> the render-view hook is not being
                //                             called at all
                //   viewsSeen>0 gate=0     -> hook runs, no view matched the
                //                             predicate at +0xC44
                //   gate>0 fires=0         -> armed and counting, has not yet
                //                             reached the first fire at 120
                //   fires>0                -> the scan ran, results are above
                log::warn(std::format(
                    "ViewOriginScanStatus: viewsSeen={} worldViews={} stillStreak={} "
                    "fires={}/3. Fires when the camera parks (streak>=2000) and at "
                    "least 8 s after the previous fire; a 45 s timeout fires a "
                    "MOVING-labelled sample so the run cannot come back empty.",
                    origin_scan_views_seen.load(std::memory_order_relaxed),
                    origin_scan_gate.load(std::memory_order_relaxed),
                    origin_scan_still_streak.load(std::memory_order_relaxed),
                    origin_scan_fires.load(std::memory_order_relaxed)));
                // THE MISSING-PAGEUP GUARD (2026-08-23).
                //
                // frame_root_double_enabled defaults to false and PageUp is
                // what sets it. Without that press there is no doubled call,
                // so Lever A, Lever V and Lever O all sit silent and every
                // eye-1 probe reports zero - indistinguishable, in the log,
                // from a probe that ran and found nothing. Three runs were
                // spent on that, plus a handover that wrongly told the owner
                // PageUp only changed the monitor view.
                //
                // So the log now says it outright, every health line, for as
                // long as the condition holds.
                if (!frame_root_double_enabled.load(std::memory_order_relaxed)) {
                    log::error(
                        "STEREO NOT ARMED: frame_root_double_enabled is FALSE. PRESS "
                        "PAGE UP. Until then there is no doubled call and no eye 1, so "
                        "Lever A / Lever V / Lever O cannot run and ANY eye-1 result "
                        "from this run is void - not a null.");
                }
                // Lever O heartbeat. armed= is read from the lever's own state
                // at the moment the line prints, never inferred from elapsed
                // time, so the arm is read at the measurement. originTrack is
                // the distance between CameraSrc+0x14b0 and the position solved
                // from the matrix: small and bounded means 0x14b0 IS the camera
                // position; unbounded growth means the restore is not landing
                // and the field is walking.
                log::warn(std::format(
                    "LeverOStatus: armed={} applied={} restored={} noSites={} | "
                    "originTrack samples={} last={:.3f} min={:.3f} max={:.3f} | "
                    "rows013 last={:.3f} residual={:.5f}.{}",
                    lever_o_enabled.load(std::memory_order_relaxed) ? "ON" : "OFF",
                    lever_o_applied.load(std::memory_order_relaxed),
                    lever_o_restored.load(std::memory_order_relaxed),
                    lever_o_no_clean.load(std::memory_order_relaxed),
                    origin_track_samples.load(std::memory_order_relaxed),
                    origin_track_last.load(std::memory_order_relaxed),
                    origin_track_min.load(std::memory_order_relaxed),
                    origin_track_max.load(std::memory_order_relaxed),
                    origin_track_last013.load(std::memory_order_relaxed),
                    origin_track_resid013.load(std::memory_order_relaxed),
                    // Names the failure instead of leaving a zero to interpret.
                    // Lever O writes only inside the doubled call, so with no
                    // OpenXR there is no eye 1 and it cannot fire at all - the
                    // shape of three wasted runs on 2026-08-23.
                    (lever_o_enabled.load(std::memory_order_relaxed) &&
                     lever_o_applied.load(std::memory_order_relaxed) == 0)
                        ? "  <- ARMED BUT NEVER WROTE. Lever O only fires inside the "
                          "doubled (eye 1) call. If leverAapplied is also 0 and the log "
                          "carries xrGetSystem failures, OpenXR never came up, there is "
                          "no eye 1, and THIS RUN CANNOT TEST LEVER O."
                        : ""));
                rr_last = rr;
                mv_last = mv;
                lv_last = lv_reb;
                la_last = la_app;

                fog_ex_last = fog_ex;
                // PERIODIC CENSUS ARM (2026-08-23). vwc_arm only fired on a
                // toggle, so the 09:07-09:15 run - eight minutes, the one with
                // the cutscene - contains exactly ONE census block, at 09:07:22.
                // There was no per-site before/after data for the transition
                // being investigated. Each arm emits kViewWriteCensusFrames = 3
                // frames, ~20 lines, so one every ten windows (~30 s at 60 fps)
                // is negligible against the 4.6 MB a ten-minute session now
                // costs, and it guarantees coverage across any transition
                // without anyone having to press anything at the right moment.
                static uint32_t census_window = 0;
                if (++census_window >= 10) {
                    census_window = 0;
                    vwc_arm("periodic heartbeat (30 s)");
                }
            }
            {
                RENDERDOC_API_1_6_0* rdc = g_rdc_api.load(std::memory_order_relaxed);
                static uint32_t rdc_last_captures = 0;
                const uint32_t caps = rdc ? rdc->GetNumCaptures() : 0;
                log::warn(std::format(
                    "RenderDocState: layer={} capturesTotal={} newSinceLastLine={} "
                    "frameCapturing={} captureKey={}. POLARITY, stated before the session: a "
                    "capture run REQUIRES layer=PRESENT - layer=ABSENT means the session is "
                    "void, not that the arm was tested and found equal.",
                    rdc ? "PRESENT" : "ABSENT", caps, caps - rdc_last_captures,
                    (rdc && rdc->IsFrameCapturing()) ? "yes" : "no",
                    rdc ? "F11 (rebound in-process)" : "n/a"));
                rdc_last_captures = caps;
            }
            const uint64_t total =
                suppressed_light_culling_dispatches.load(std::memory_order_relaxed);
            const uint64_t delta = total - lc_last_total;
            const bool on = suppress_doubled_light_culling.load(std::memory_order_relaxed);
            log::warn(std::format(
                "ComputeSuppression: flag={} total={} deltaSinceLastLine={} over {} presents "
                "(~{:.1f}/frame) | monitorEye1View={} passesRedirected={} | gradeRung={} "
                "repairMode={} floatsWritten={} (+{} since last line) | cloneArrays={} maxArray={} "
                "elementsDropped={} | mirrorCoverage lines={} MISSES={} | cloneBindingsDropped={} "
                "volumeSubstitutions={} volumeRestores={} vrRecorded={} vrDoubledWrites={} "
                "vrTemplateWrites={} vrNoRva={} volJobOverflow={} serialisations={}. {} "
                "F1 toggles the suppression, SHIFT+F5 the monitor view. THE ENGAGEMENT CHECK IS "
                "THE DELTA, NOT THE TOTAL: flag=ON with delta=0 means the doubled call issues no "
                "shared-SSBO compute, and an A/B of it proves nothing either way. Likewise "
                "monitorEye1View=ON with passesRedirected=0 means the doubled final pass never "
                "ran and the screen is STILL EYE 0.",
                on ? "ON" : "OFF", total, delta, lc_frames,
                static_cast<double>(delta) / static_cast<double>(lc_frames),
                monitor_eye1_direct.load(std::memory_order_relaxed)
                    ? "ON (screen = EYE 1)" : "OFF (screen = eye 0)",
                monitor_eye1_passes.load(std::memory_order_relaxed),
                graded_mode.load(std::memory_order_relaxed),
                constant_repair_mode.load(std::memory_order_relaxed),
                repair_floats_written.load(std::memory_order_relaxed),
                repair_floats_written.load(std::memory_order_relaxed) - rp_last,
                clone_array_bindings.load(std::memory_order_relaxed),
                clone_max_array.load(std::memory_order_relaxed),
                clone_elements_dropped.load(std::memory_order_relaxed),
                mirror_coverage_lines.load(std::memory_order_relaxed),
                mirror_coverage_misses.load(std::memory_order_relaxed),
                clone_bindings_dropped.load(std::memory_order_relaxed),
                volume_substitutions_seen.load(std::memory_order_relaxed),   // volumeSubstitutions
                volume_restores.load(std::memory_order_relaxed),
                vr_volumes_recorded.load(std::memory_order_relaxed),
                vr_writes_doubled.load(std::memory_order_relaxed),
                vr_template_writes.load(std::memory_order_relaxed),
                vr_records_no_rva.load(std::memory_order_relaxed),
                volume_job_overflow.load(std::memory_order_relaxed),
                doubled_serialisations.load(std::memory_order_relaxed),
                on ? "ON is the shipping state - the doubled call is BLOCKED from running any "
                     "compute that writes a shared storage buffer, which is what GPU particles, "
                     "emitter state and light lists are"
                   : "OFF - the doubled call is running that compute again, and is therefore "
                     "rewriting buffers DOOM's NEXT frame reads. Expect eye 0's sparks to "
                     "degrade; that is the known trade, not a new fault"));
            lc_last_total = total;
            rp_last = repair_floats_written.load(std::memory_order_relaxed);
            lc_frames = 0;
        }
    }
    // THE POSITIVE CONTROL, and it runs whether the throttle is armed or not
    // so both states print in the same units. A run whose PresentThrottle line
    // still reads ~23 ms/frame did NOT throttle, and a null result from it is
    // instrument failure rather than a refutation.
    //
    // Bounded by construction: one line per 60 presents, which at the throttled
    // rate is one line every ~17 s. The statics are safe because DOOM presents
    // from a single thread and this sits after the real present.
    {
        static uint32_t throttle_window_start_ms = 0;
        static uint32_t throttle_window_frames = 0;
        const uint32_t now_ms = GetTickCount();
        if (throttle_window_start_ms == 0) throttle_window_start_ms = now_ms;
        if (++throttle_window_frames >= 60) {
            const uint32_t elapsed = now_ms - throttle_window_start_ms;
            log::warn(std::format(
                "PresentThrottle: armed={} sleepMs={} measured={:.1f} ms/frame ({:.1f} fps) over "
                "60 presents. CONTROLS: the no-layer baseline runs ~23 ms/frame (~43 fps) and "
                "Test A's validation-layer run ~32 ms (~31 fps). If this does not read hundreds "
                "of ms the throttle did not take and the run proves nothing either way.",
                present_throttle_armed.load(std::memory_order_relaxed) ? "yes" : "no",
                kPresentThrottleMs, elapsed / 60.0,
                60000.0 / static_cast<double>(elapsed ? elapsed : 1)));
            throttle_window_start_ms = now_ms;
            throttle_window_frames = 0;
        }
    }
    // ===== FRAME TIME BUDGET, emitted once per second =====================
    //
    // Placed at the very end so `post` covers every bit of our own work after
    // the present returns. All arithmetic is on locals; the single log line is
    // the only I/O and it happens once a second, never per frame.
    fb_post_ns.fetch_add(fb_ns() - fb_present1, std::memory_order_relaxed);
    fb_frames.fetch_add(1, std::memory_order_relaxed);
    fb_clock_reads.fetch_add(6, std::memory_order_relaxed);
    {
        static uint64_t fb_window_start = 0;
        const uint64_t now = fb_ns();
        if (fb_window_start == 0) fb_window_start = now;
        // Forced emit: see fb_force_emit. `frames` is always >= 1 here
        // because fb_frames was incremented earlier this present.
        if (now - fb_window_start >= 1000000000ull ||
            fb_force_emit.exchange(false, std::memory_order_relaxed)) {
            const double wall_ms = static_cast<double>(now - fb_window_start) / 1e6;
            const uint64_t frames = fb_frames.exchange(0, std::memory_order_relaxed);
            auto take = [](std::atomic_uint64_t& a) {
                return static_cast<double>(a.exchange(0, std::memory_order_relaxed));
            };
            const double pre = take(fb_pre_ns), xr = take(fb_xr_ns);
            const double pres = take(fb_present_ns), post = take(fb_post_ns);
            const double dbl = take(fb_doubled_ns);
            const double sub = take(fb_submit_ns), acq = take(fb_acquire_ns);
            const double fen = take(fb_fence_ns);
            const uint64_t sc = fb_submit_calls.exchange(0, std::memory_order_relaxed);
            const uint64_t ac = fb_acquire_calls.exchange(0, std::memory_order_relaxed);
            const uint64_t fc = fb_fence_calls.exchange(0, std::memory_order_relaxed);
            const uint64_t reads = fb_clock_reads.exchange(0, std::memory_order_relaxed);
            fb_window_start = now;
            if (frames) {
                const double f = static_cast<double>(frames);
                const double per = 1e6 * f;   // ns totals -> ms per frame
                const double frame_ms = wall_ms / f;
                // PER-HOOK TABLE: all 53 exports, every one printed even at
                // zero, with its call count. A hook reading 0.00 with live
                // calls is a measurement; a hook ABSENT from this table would
                // be an unknown, and unknowns being read as answers is exactly
                // what went wrong before.
                std::string hook_detail;
                uint64_t hook_calls_total = 0;
                for (int i = 0; i < FB_H_COUNT; ++i) {
                    const double hr = take(fb_hook_real_ns[i]);
                    const double hd = take(fb_hook_dbl_ns[i]);
                    const uint64_t hc = fb_hook_calls[i].exchange(0, std::memory_order_relaxed);
                    hook_calls_total += hc;
                    hook_detail += std::format(" {}{}={:.2f}/{:.2f}({})",
                                               kFbDedicated[i] ? "*" : "", kFbHookExport[i] + 2,
                                               hr / per, hd / per, hc / frames);
                }
                // AUTHORITATIVE hook total: outermost-only, so a hook calling
                // another hook cannot be counted twice. The per-hook numbers
                // above are INCLUSIVE and may sum higher - they name culprits,
                // they are not the budget arithmetic.
                const double allr = take(fb_all_real_ns), alld = take(fb_all_dbl_ns);
                // DOOM-side hooks: OVERHEAD ONLY (span - trampoline), because
                // their span contains DOOM's own rendering. Negative clamps to
                // zero - a trampoline can out-measure its span by a few ns of
                // clock noise on a very cheap hook.
                double doomr = 0.0, doomd = 0.0;
                uint64_t doomc = 0;
                std::string doom_detail;
                for (int i = 0; i < FB_D_COUNT; ++i) {
                    const double sr = take(fb_doom_span_ns[i]);
                    const double tr = take(fb_doom_tramp_ns[i]);
                    const double sd = take(fb_doom_span_dbl_ns[i]);
                    const double td = take(fb_doom_tramp_dbl_ns[i]);
                    const uint64_t dc = fb_doom_calls[i].exchange(0, std::memory_order_relaxed);
                    const double ovr = sr > tr ? sr - tr : 0.0;
                    const double ovd = sd > td ? sd - td : 0.0;
                    doomr += ovr; doomd += ovd; doomc += dc;
                    doom_detail += std::format(" {}={:.2f}/{:.2f}({})", kFbDoomName[i],
                                               ovr / per, ovd / per, dc / frames);
                }
                const double gpu_wait = (sub + acq + fen) / per;
                const double named =
                    (pre + xr + pres + post + dbl + sub + acq + fen + allr + doomr) / per;
                const double probe_ms = static_cast<double>(reads * fb_clock_cost_ns()) / per;
                log::warn(std::format(
                    "FrameBudget fps={:.1f} frameMs={:.2f} over {} frames | pre={:.2f} "
                    "xrPoll={:.2f} presentCall={:.2f} post={:.2f} doubled={:.2f} "
                    "|| ALLHOOKS={:.2f} ({} calls/frame, {} exports) "
                    "|| DOOMHOOKS={:.2f} ({} calls/frame, {} hooks, overhead only) "
                    "|| GPUWAIT={:.2f} (submit={:.2f} acquire={:.2f} fence={:.2f}) "
                    "|| named={:.2f} RESIDUAL={:.2f} | insideDoubledHooks={:.2f} depthLeaks={} "
                    "| probeMs={:.3f} clockNs={}\n"
                    "PERHOOK real/doubled(calls/frame), '*' = timed by a dedicated field above "
                    "and NOT in ALLHOOKS:{}\n"
                    "PERDOOMHOOK overhead real/doubled(calls/frame), span minus trampoline:{}\n"
                    "READING IT: every figure is ms PER FRAME. ALLHOOKS is the outermost-span "
                    "total across ALL {} hooked Vulkan exports - this is the ours-or-theirs "
                    "number, and it is an UPPER BOUND because it includes the driver call DOOM "
                    "would have made anyway. RESIDUAL is frameMs minus everything named. "
                    "⚠ RESIDUAL IS NOT PURELY WORK: when DOOM paces itself (the steady 60.0 fps "
                    "cutscene cadence) idle wait lands here too, so read it against whether the "
                    "rate is STEADY (a cap) or ERRATIC (real stalling). depthLeaks MUST be 0 - "
                    "any other value means a manual-timed hook missed an exit path and every "
                    "number on this line is suspect. probeMs is this instrument's own cost.",
                    1000.0 / frame_ms, frame_ms, frames, pre / per, xr / per, pres / per,
                    post / per, dbl / per,
                    allr / per, hook_calls_total / frames, static_cast<int>(FB_H_COUNT),
                    doomr / per, doomc / frames, static_cast<int>(FB_D_COUNT),
                    gpu_wait, sub / per, acq / per, fen / per, named,
                    frame_ms - named, alld / per,
                    fb_depth_leaks.load(std::memory_order_relaxed),
                    // Order matters and is easy to get wrong: after clockNs the
                    // format takes PERHOOK, then PERDOOMHOOK, then the export
                    // count inside READING IT. std::format accepts an int where
                    // a string was meant and prints nonsense silently, so this
                    // ordering is deliberate, not incidental.
                    probe_ms, fb_clock_cost_ns(), hook_detail, doom_detail,
                    static_cast<int>(FB_H_COUNT)));
            }
        }
    }
    return present_result;
}

// ==== TIMING-ONLY HOOKS FOR DOOM'S REMAINING VULKAN IMPORTS ============
// Generated from DOOMx64vk.exe's own import table (97 Vulkan imports), not
// hand-picked. These are the calls DOOM makes that we did not hook at all -
// they executed entirely outside the budget and landed in RESIDUAL, where
// they were indistinguishable from DOOM's own CPU work. Several of them
// BLOCK: vkDeviceWaitIdle, vkQueueWaitIdle, vkGetQueryPoolResults.
// Every one is a pure pass-through; the static_assert on each guarantees the
// signature matches the loader's, so a typo cannot corrupt the stack - it
// fails the build instead.
PFN_vkDeviceWaitIdle real_device_wait_idle{};
VkResult VKAPI_PTR hook_device_wait_idle(VkDevice a) {
    FbHook fb_(FB_H_DEVICE_WAIT_IDLE);
    return real_device_wait_idle(a);
}
static_assert(std::is_same_v<decltype(&hook_device_wait_idle), PFN_vkDeviceWaitIdle>, "signature mismatch: vkDeviceWaitIdle");
PFN_vkQueueWaitIdle real_queue_wait_idle{};
VkResult VKAPI_PTR hook_queue_wait_idle(VkQueue a) {
    FbHook fb_(FB_H_QUEUE_WAIT_IDLE);
    return real_queue_wait_idle(a);
}
static_assert(std::is_same_v<decltype(&hook_queue_wait_idle), PFN_vkQueueWaitIdle>, "signature mismatch: vkQueueWaitIdle");
PFN_vkGetQueryPoolResults real_get_query_pool_results{};
VkResult VKAPI_PTR hook_get_query_pool_results(VkDevice a, VkQueryPool b, uint32_t c, uint32_t d, size_t e, void* f, VkDeviceSize g, VkQueryResultFlags h) {
    FbHook fb_(FB_H_GET_QUERY_POOL_RESULTS);
    return real_get_query_pool_results(a, b, c, d, e, f, g, h);
}
static_assert(std::is_same_v<decltype(&hook_get_query_pool_results), PFN_vkGetQueryPoolResults>, "signature mismatch: vkGetQueryPoolResults");
PFN_vkAllocateMemory real_allocate_memory{};
VkResult VKAPI_PTR hook_allocate_memory(VkDevice a, const VkMemoryAllocateInfo* b, const VkAllocationCallbacks* c, VkDeviceMemory* d) {
    FbHook fb_(FB_H_ALLOCATE_MEMORY);
    return real_allocate_memory(a, b, c, d);
}
static_assert(std::is_same_v<decltype(&hook_allocate_memory), PFN_vkAllocateMemory>, "signature mismatch: vkAllocateMemory");
PFN_vkFlushMappedMemoryRanges real_flush_mapped_memory_ranges{};
VkResult VKAPI_PTR hook_flush_mapped_memory_ranges(VkDevice a, uint32_t b, const VkMappedMemoryRange* c) {
    FbHook fb_(FB_H_FLUSH_MAPPED_MEMORY_RANGES);
    return real_flush_mapped_memory_ranges(a, b, c);
}
static_assert(std::is_same_v<decltype(&hook_flush_mapped_memory_ranges), PFN_vkFlushMappedMemoryRanges>, "signature mismatch: vkFlushMappedMemoryRanges");
PFN_vkAllocateCommandBuffers real_allocate_command_buffers{};
VkResult VKAPI_PTR hook_allocate_command_buffers(VkDevice a, const VkCommandBufferAllocateInfo* b, VkCommandBuffer* c) {
    FbHook fb_(FB_H_ALLOCATE_COMMAND_BUFFERS);
    return real_allocate_command_buffers(a, b, c);
}
static_assert(std::is_same_v<decltype(&hook_allocate_command_buffers), PFN_vkAllocateCommandBuffers>, "signature mismatch: vkAllocateCommandBuffers");
PFN_vkFreeCommandBuffers real_free_command_buffers{};
void VKAPI_PTR hook_free_command_buffers(VkDevice a, VkCommandPool b, uint32_t c, const VkCommandBuffer* d) {
    FbHook fb_(FB_H_FREE_COMMAND_BUFFERS);
    real_free_command_buffers(a, b, c, d);
}
static_assert(std::is_same_v<decltype(&hook_free_command_buffers), PFN_vkFreeCommandBuffers>, "signature mismatch: vkFreeCommandBuffers");
PFN_vkBeginCommandBuffer real_begin_command_buffer{};
VkResult VKAPI_PTR hook_begin_command_buffer(VkCommandBuffer a, const VkCommandBufferBeginInfo* b) {
    FbHook fb_(FB_H_BEGIN_COMMAND_BUFFER);
    return real_begin_command_buffer(a, b);
}
static_assert(std::is_same_v<decltype(&hook_begin_command_buffer), PFN_vkBeginCommandBuffer>, "signature mismatch: vkBeginCommandBuffer");
PFN_vkEndCommandBuffer real_end_command_buffer{};
VkResult VKAPI_PTR hook_end_command_buffer(VkCommandBuffer a) {
    FbHook fb_(FB_H_END_COMMAND_BUFFER);
    return real_end_command_buffer(a);
}
static_assert(std::is_same_v<decltype(&hook_end_command_buffer), PFN_vkEndCommandBuffer>, "signature mismatch: vkEndCommandBuffer");
PFN_vkResetFences real_reset_fences{};
VkResult VKAPI_PTR hook_reset_fences(VkDevice a, uint32_t b, const VkFence* c) {
    FbHook fb_(FB_H_RESET_FENCES);
    return real_reset_fences(a, b, c);
}
static_assert(std::is_same_v<decltype(&hook_reset_fences), PFN_vkResetFences>, "signature mismatch: vkResetFences");
PFN_vkCreateFence real_create_fence{};
VkResult VKAPI_PTR hook_create_fence(VkDevice a, const VkFenceCreateInfo* b, const VkAllocationCallbacks* c, VkFence* d) {
    FbHook fb_(FB_H_CREATE_FENCE);
    return real_create_fence(a, b, c, d);
}
static_assert(std::is_same_v<decltype(&hook_create_fence), PFN_vkCreateFence>, "signature mismatch: vkCreateFence");
PFN_vkDestroyFence real_destroy_fence{};
void VKAPI_PTR hook_destroy_fence(VkDevice a, VkFence b, const VkAllocationCallbacks* c) {
    FbHook fb_(FB_H_DESTROY_FENCE);
    real_destroy_fence(a, b, c);
}
static_assert(std::is_same_v<decltype(&hook_destroy_fence), PFN_vkDestroyFence>, "signature mismatch: vkDestroyFence");
PFN_vkResetDescriptorPool real_reset_descriptor_pool{};
VkResult VKAPI_PTR hook_reset_descriptor_pool(VkDevice a, VkDescriptorPool b, VkDescriptorPoolResetFlags c) {
    FbHook fb_(FB_H_RESET_DESCRIPTOR_POOL);
    kharvox::native::beforeDestroy("vkResetDescriptorPool",reinterpret_cast<uintptr_t>(b));const auto result=real_reset_descriptor_pool(a,b,c);if(result==VK_SUCCESS)kharvox::native::forgetDescriptorPool(b);return result;
}
static_assert(std::is_same_v<decltype(&hook_reset_descriptor_pool), PFN_vkResetDescriptorPool>, "signature mismatch: vkResetDescriptorPool");
PFN_vkCreateDescriptorPool real_create_descriptor_pool{};
VkResult VKAPI_PTR hook_create_descriptor_pool(VkDevice a, const VkDescriptorPoolCreateInfo* b, const VkAllocationCallbacks* c, VkDescriptorPool* d) {
    FbHook fb_(FB_H_CREATE_DESCRIPTOR_POOL);
    return real_create_descriptor_pool(a, b, c, d);
}
static_assert(std::is_same_v<decltype(&hook_create_descriptor_pool), PFN_vkCreateDescriptorPool>, "signature mismatch: vkCreateDescriptorPool");
PFN_vkDestroyDescriptorPool real_destroy_descriptor_pool{};
void VKAPI_PTR hook_destroy_descriptor_pool(VkDevice a, VkDescriptorPool b, const VkAllocationCallbacks* c) {
    kharvox::native::beforeDestroy("vkDestroyDescriptorPool",reinterpret_cast<uintptr_t>(b));
    FbHook fb_(FB_H_DESTROY_DESCRIPTOR_POOL);
    real_destroy_descriptor_pool(a, b, c);
    kharvox::native::forgetDescriptorPool(b);
}
static_assert(std::is_same_v<decltype(&hook_destroy_descriptor_pool), PFN_vkDestroyDescriptorPool>, "signature mismatch: vkDestroyDescriptorPool");
PFN_vkCreateComputePipelines real_create_compute_pipelines{};
VkResult VKAPI_PTR hook_create_compute_pipelines(VkDevice a, VkPipelineCache b, uint32_t c, const VkComputePipelineCreateInfo* d, const VkAllocationCallbacks* e, VkPipeline* f) {
    FbHook fb_(FB_H_CREATE_COMPUTE_PIPELINES);
    return real_create_compute_pipelines(a, b, c, d, e, f);
}
static_assert(std::is_same_v<decltype(&hook_create_compute_pipelines), PFN_vkCreateComputePipelines>, "signature mismatch: vkCreateComputePipelines");
PFN_vkCreateShaderModule real_create_shader_module{};
VkResult VKAPI_PTR hook_create_shader_module(VkDevice a, const VkShaderModuleCreateInfo* b, const VkAllocationCallbacks* c, VkShaderModule* d) {
    FbHook fb_(FB_H_CREATE_SHADER_MODULE);
    return real_create_shader_module(a, b, c, d);
}
static_assert(std::is_same_v<decltype(&hook_create_shader_module), PFN_vkCreateShaderModule>, "signature mismatch: vkCreateShaderModule");
PFN_vkDestroyShaderModule real_destroy_shader_module{};
void VKAPI_PTR hook_destroy_shader_module(VkDevice a, VkShaderModule b, const VkAllocationCallbacks* c) {
    FbHook fb_(FB_H_DESTROY_SHADER_MODULE);
    real_destroy_shader_module(a, b, c);
}
static_assert(std::is_same_v<decltype(&hook_destroy_shader_module), PFN_vkDestroyShaderModule>, "signature mismatch: vkDestroyShaderModule");
PFN_vkDestroyImage real_destroy_image{};
void VKAPI_PTR hook_destroy_image(VkDevice a, VkImage b, const VkAllocationCallbacks* c) {
    FbHook fb_(FB_H_DESTROY_IMAGE);
    kharvox::native::destroyNativeImage(a,b,c,real_destroy_image);
}
static_assert(std::is_same_v<decltype(&hook_destroy_image), PFN_vkDestroyImage>, "signature mismatch: vkDestroyImage");
PFN_vkDestroyBuffer real_destroy_buffer{};
void VKAPI_PTR hook_destroy_buffer(VkDevice a, VkBuffer b, const VkAllocationCallbacks* c) {
    FbHook fb_(FB_H_DESTROY_BUFFER);
    kharvox::native::forgetBuffer(b);
    {std::scoped_lock lock(swapchains_mutex);kharvox::native::unindexSourceBuffer(b);buffers.erase(b);}
    invalidate_read_redirect_cache("buffer destroyed");
    real_destroy_buffer(a, b, c);
}
static_assert(std::is_same_v<decltype(&hook_destroy_buffer), PFN_vkDestroyBuffer>, "signature mismatch: vkDestroyBuffer");
PFN_vkDestroyPipeline real_destroy_pipeline{};
void VKAPI_PTR hook_destroy_pipeline(VkDevice a, VkPipeline b, const VkAllocationCallbacks* c) {
    kharvox::native::beforeDestroy("vkDestroyPipeline",reinterpret_cast<uintptr_t>(b));
    FbHook fb_(FB_H_DESTROY_PIPELINE);
    real_destroy_pipeline(a, b, c);
}
static_assert(std::is_same_v<decltype(&hook_destroy_pipeline), PFN_vkDestroyPipeline>, "signature mismatch: vkDestroyPipeline");
PFN_vkCmdCopyBuffer real_cmd_copy_buffer{};
void VKAPI_PTR hook_cmd_copy_buffer(VkCommandBuffer a, VkBuffer b, VkBuffer c, uint32_t d, const VkBufferCopy* e) {
    kharvox::native::noteCommand(a);
    kharvox::native::checkFinalCommand(a,"hook_cmd_copy_buffer");
    FbHook fb_(FB_H_CMD_COPY_BUFFER);
    const auto dst=kharvox::native::bufferForEye(c);
    if((inside_frame_root_double||inside_final_pass_double)&&dst==c)kharvox::native::fail("doubled buffer transfer has an unmirrored destination");
    real_cmd_copy_buffer(a,kharvox::native::bufferForEye(b),dst,d,e);
}
static_assert(std::is_same_v<decltype(&hook_cmd_copy_buffer), PFN_vkCmdCopyBuffer>, "signature mismatch: vkCmdCopyBuffer");
PFN_vkCmdCopyImageToBuffer real_cmd_copy_image_to_buffer{};
void VKAPI_PTR hook_cmd_copy_image_to_buffer(VkCommandBuffer a, VkImage b, VkImageLayout c, VkBuffer d, uint32_t e, const VkBufferImageCopy* f) {
    kharvox::native::noteCommand(a);
    kharvox::native::checkFinalCommand(a,"hook_cmd_copy_image_to_buffer");
    FbHook fb_(FB_H_CMD_COPY_IMAGE_TO_BUFFER);
    real_cmd_copy_image_to_buffer(a, b, c, d, e, f);
}
static_assert(std::is_same_v<decltype(&hook_cmd_copy_image_to_buffer), PFN_vkCmdCopyImageToBuffer>, "signature mismatch: vkCmdCopyImageToBuffer");
PFN_vkCmdWriteTimestamp real_cmd_write_timestamp{};
void VKAPI_PTR hook_cmd_write_timestamp(VkCommandBuffer a, VkPipelineStageFlagBits b, VkQueryPool c, uint32_t d) {
    kharvox::native::noteCommand(a);
    kharvox::native::checkFinalCommand(a,"hook_cmd_write_timestamp");
    FbHook fb_(FB_H_CMD_WRITE_TIMESTAMP);
    real_cmd_write_timestamp(a, b, c, d);
}
static_assert(std::is_same_v<decltype(&hook_cmd_write_timestamp), PFN_vkCmdWriteTimestamp>, "signature mismatch: vkCmdWriteTimestamp");
PFN_vkCmdSetDepthBias real_cmd_set_depth_bias{};
void VKAPI_PTR hook_cmd_set_depth_bias(VkCommandBuffer a, float b, float c, float d) {
    kharvox::native::noteCommand(a);
    FbHook fb_(FB_H_CMD_SET_DEPTH_BIAS);
    real_cmd_set_depth_bias(a, b, c, d);
}
static_assert(std::is_same_v<decltype(&hook_cmd_set_depth_bias), PFN_vkCmdSetDepthBias>, "signature mismatch: vkCmdSetDepthBias");
PFN_vkCmdSetStencilCompareMask real_cmd_set_stencil_compare_mask{};
void VKAPI_PTR hook_cmd_set_stencil_compare_mask(VkCommandBuffer a, VkStencilFaceFlags b, uint32_t c) {
    kharvox::native::noteCommand(a);
    FbHook fb_(FB_H_CMD_SET_STENCIL_COMPARE_MASK);
    real_cmd_set_stencil_compare_mask(a, b, c);
}
static_assert(std::is_same_v<decltype(&hook_cmd_set_stencil_compare_mask), PFN_vkCmdSetStencilCompareMask>, "signature mismatch: vkCmdSetStencilCompareMask");
PFN_vkCmdSetStencilReference real_cmd_set_stencil_reference{};
void VKAPI_PTR hook_cmd_set_stencil_reference(VkCommandBuffer a, VkStencilFaceFlags b, uint32_t c) {
    kharvox::native::noteCommand(a);
    FbHook fb_(FB_H_CMD_SET_STENCIL_REFERENCE);
    real_cmd_set_stencil_reference(a, b, c);
}
static_assert(std::is_same_v<decltype(&hook_cmd_set_stencil_reference), PFN_vkCmdSetStencilReference>, "signature mismatch: vkCmdSetStencilReference");
PFN_vkBindImageMemory real_bind_image_memory{};
VkResult VKAPI_PTR hook_bind_image_memory(VkDevice a, VkImage b, VkDeviceMemory c, VkDeviceSize d) {
    FbHook fb_(FB_H_BIND_IMAGE_MEMORY);
    return real_bind_image_memory(a, b, c, d);
}
static_assert(std::is_same_v<decltype(&hook_bind_image_memory), PFN_vkBindImageMemory>, "signature mismatch: vkBindImageMemory");
PFN_vkCreateCommandPool real_create_command_pool{};
VkResult VKAPI_PTR hook_create_command_pool(VkDevice a, const VkCommandPoolCreateInfo* b, const VkAllocationCallbacks* c, VkCommandPool* d) {
    FbHook fb_(FB_H_CREATE_COMMAND_POOL);
    return real_create_command_pool(a, b, c, d);
}
static_assert(std::is_same_v<decltype(&hook_create_command_pool), PFN_vkCreateCommandPool>, "signature mismatch: vkCreateCommandPool");
PFN_vkDestroyCommandPool real_destroy_command_pool{};
void VKAPI_PTR hook_destroy_command_pool(VkDevice a, VkCommandPool b, const VkAllocationCallbacks* c) {
    kharvox::native::beforeDestroy("vkDestroyCommandPool",reinterpret_cast<uintptr_t>(b));
    kharvox::native::releaseStorageBindingBookkeeping();
    FbHook fb_(FB_H_DESTROY_COMMAND_POOL);
    real_destroy_command_pool(a, b, c);
}
static_assert(std::is_same_v<decltype(&hook_destroy_command_pool), PFN_vkDestroyCommandPool>, "signature mismatch: vkDestroyCommandPool");
PFN_vkCreateDescriptorSetLayout real_create_descriptor_set_layout{};
VkResult VKAPI_PTR hook_create_descriptor_set_layout(VkDevice a, const VkDescriptorSetLayoutCreateInfo* b, const VkAllocationCallbacks* c, VkDescriptorSetLayout* d) {
    FbHook fb_(FB_H_CREATE_DESCRIPTOR_SET_LAYOUT);
    auto result=real_create_descriptor_set_layout(a,b,c,d);if(result==VK_SUCCESS&&d)kharvox::native::recordLayout(*d,b);return result;
}
static_assert(std::is_same_v<decltype(&hook_create_descriptor_set_layout), PFN_vkCreateDescriptorSetLayout>, "signature mismatch: vkCreateDescriptorSetLayout");
PFN_vkDestroyDescriptorSetLayout real_destroy_descriptor_set_layout{};
void VKAPI_PTR hook_destroy_descriptor_set_layout(VkDevice a, VkDescriptorSetLayout b, const VkAllocationCallbacks* c) {
    FbHook fb_(FB_H_DESTROY_DESCRIPTOR_SET_LAYOUT);
    kharvox::native::beforeDestroy("vkDestroyDescriptorSetLayout",reinterpret_cast<uintptr_t>(b));real_destroy_descriptor_set_layout(a,b,c);
}
static_assert(std::is_same_v<decltype(&hook_destroy_descriptor_set_layout), PFN_vkDestroyDescriptorSetLayout>, "signature mismatch: vkDestroyDescriptorSetLayout");
PFN_vkCreatePipelineLayout real_create_pipeline_layout{};
VkResult VKAPI_PTR hook_create_pipeline_layout(VkDevice a, const VkPipelineLayoutCreateInfo* b, const VkAllocationCallbacks* c, VkPipelineLayout* d) {
    FbHook fb_(FB_H_CREATE_PIPELINE_LAYOUT);
    return real_create_pipeline_layout(a, b, c, d);
}
static_assert(std::is_same_v<decltype(&hook_create_pipeline_layout), PFN_vkCreatePipelineLayout>, "signature mismatch: vkCreatePipelineLayout");
PFN_vkDestroyPipelineLayout real_destroy_pipeline_layout{};
void VKAPI_PTR hook_destroy_pipeline_layout(VkDevice a, VkPipelineLayout b, const VkAllocationCallbacks* c) {
    FbHook fb_(FB_H_DESTROY_PIPELINE_LAYOUT);
    real_destroy_pipeline_layout(a, b, c);
}
static_assert(std::is_same_v<decltype(&hook_destroy_pipeline_layout), PFN_vkDestroyPipelineLayout>, "signature mismatch: vkDestroyPipelineLayout");
PFN_vkCreateQueryPool real_create_query_pool{};
VkResult VKAPI_PTR hook_create_query_pool(VkDevice a, const VkQueryPoolCreateInfo* b, const VkAllocationCallbacks* c, VkQueryPool* d) {
    FbHook fb_(FB_H_CREATE_QUERY_POOL);
    return real_create_query_pool(a, b, c, d);
}
static_assert(std::is_same_v<decltype(&hook_create_query_pool), PFN_vkCreateQueryPool>, "signature mismatch: vkCreateQueryPool");
PFN_vkDestroyQueryPool real_destroy_query_pool{};
void VKAPI_PTR hook_destroy_query_pool(VkDevice a, VkQueryPool b, const VkAllocationCallbacks* c) {
    FbHook fb_(FB_H_DESTROY_QUERY_POOL);
    real_destroy_query_pool(a, b, c);
}
static_assert(std::is_same_v<decltype(&hook_destroy_query_pool), PFN_vkDestroyQueryPool>, "signature mismatch: vkDestroyQueryPool");
PFN_vkCreateSampler real_create_sampler{};
VkResult VKAPI_PTR hook_create_sampler(VkDevice a, const VkSamplerCreateInfo* b, const VkAllocationCallbacks* c, VkSampler* d) {
    FbHook fb_(FB_H_CREATE_SAMPLER);
    return real_create_sampler(a, b, c, d);
}
static_assert(std::is_same_v<decltype(&hook_create_sampler), PFN_vkCreateSampler>, "signature mismatch: vkCreateSampler");
PFN_vkCreateSemaphore real_create_semaphore{};
VkResult VKAPI_PTR hook_create_semaphore(VkDevice a, const VkSemaphoreCreateInfo* b, const VkAllocationCallbacks* c, VkSemaphore* d) {
    FbHook fb_(FB_H_CREATE_SEMAPHORE);
    return real_create_semaphore(a, b, c, d);
}
static_assert(std::is_same_v<decltype(&hook_create_semaphore), PFN_vkCreateSemaphore>, "signature mismatch: vkCreateSemaphore");
PFN_vkDestroySemaphore real_destroy_semaphore{};
void VKAPI_PTR hook_destroy_semaphore(VkDevice a, VkSemaphore b, const VkAllocationCallbacks* c) {
    FbHook fb_(FB_H_DESTROY_SEMAPHORE);
    real_destroy_semaphore(a, b, c);
}
static_assert(std::is_same_v<decltype(&hook_destroy_semaphore), PFN_vkDestroySemaphore>, "signature mismatch: vkDestroySemaphore");
PFN_vkDestroyRenderPass real_destroy_render_pass{};
void VKAPI_PTR hook_destroy_render_pass(VkDevice a, VkRenderPass b, const VkAllocationCallbacks* c) {
    FbHook fb_(FB_H_DESTROY_RENDER_PASS);
    real_destroy_render_pass(a, b, c);
}
static_assert(std::is_same_v<decltype(&hook_destroy_render_pass), PFN_vkDestroyRenderPass>, "signature mismatch: vkDestroyRenderPass");
PFN_vkGetBufferMemoryRequirements real_get_buffer_memory_requirements{};
void VKAPI_PTR hook_get_buffer_memory_requirements(VkDevice a, VkBuffer b, VkMemoryRequirements* c) {
    FbHook fb_(FB_H_GET_BUFFER_MEMORY_REQUIREMENTS);
    real_get_buffer_memory_requirements(a, b, c);
}
static_assert(std::is_same_v<decltype(&hook_get_buffer_memory_requirements), PFN_vkGetBufferMemoryRequirements>, "signature mismatch: vkGetBufferMemoryRequirements");
PFN_vkGetImageMemoryRequirements real_get_image_memory_requirements{};
void VKAPI_PTR hook_get_image_memory_requirements(VkDevice a, VkImage b, VkMemoryRequirements* c) {
    FbHook fb_(FB_H_GET_IMAGE_MEMORY_REQUIREMENTS);
    real_get_image_memory_requirements(a, b, c);
}
static_assert(std::is_same_v<decltype(&hook_get_image_memory_requirements), PFN_vkGetImageMemoryRequirements>, "signature mismatch: vkGetImageMemoryRequirements");
PFN_vkEnumerateDeviceExtensionProperties real_enumerate_device_extension_properties{};
VkResult VKAPI_PTR hook_enumerate_device_extension_properties(VkPhysicalDevice a, const char* b, uint32_t* c, VkExtensionProperties* d) {
    FbHook fb_(FB_H_ENUMERATE_DEVICE_EXTENSION_PROPERTIES);
    return real_enumerate_device_extension_properties(a, b, c, d);
}
static_assert(std::is_same_v<decltype(&hook_enumerate_device_extension_properties), PFN_vkEnumerateDeviceExtensionProperties>, "signature mismatch: vkEnumerateDeviceExtensionProperties");
PFN_vkGetPhysicalDeviceFeatures real_get_physical_device_features{};
void VKAPI_PTR hook_get_physical_device_features(VkPhysicalDevice a, VkPhysicalDeviceFeatures* b) {
    FbHook fb_(FB_H_GET_PHYSICAL_DEVICE_FEATURES);
    real_get_physical_device_features(a, b);
}
static_assert(std::is_same_v<decltype(&hook_get_physical_device_features), PFN_vkGetPhysicalDeviceFeatures>, "signature mismatch: vkGetPhysicalDeviceFeatures");
PFN_vkGetPhysicalDeviceFormatProperties real_get_physical_device_format_properties{};
void VKAPI_PTR hook_get_physical_device_format_properties(VkPhysicalDevice a, VkFormat b, VkFormatProperties* c) {
    FbHook fb_(FB_H_GET_PHYSICAL_DEVICE_FORMAT_PROPERTIES);
    real_get_physical_device_format_properties(a, b, c);
}
static_assert(std::is_same_v<decltype(&hook_get_physical_device_format_properties), PFN_vkGetPhysicalDeviceFormatProperties>, "signature mismatch: vkGetPhysicalDeviceFormatProperties");
PFN_vkGetPhysicalDeviceMemoryProperties real_get_physical_device_memory_properties{};
void VKAPI_PTR hook_get_physical_device_memory_properties(VkPhysicalDevice a, VkPhysicalDeviceMemoryProperties* b) {
    FbHook fb_(FB_H_GET_PHYSICAL_DEVICE_MEMORY_PROPERTIES);
    real_get_physical_device_memory_properties(a, b);
}
static_assert(std::is_same_v<decltype(&hook_get_physical_device_memory_properties), PFN_vkGetPhysicalDeviceMemoryProperties>, "signature mismatch: vkGetPhysicalDeviceMemoryProperties");
PFN_vkGetPhysicalDeviceProperties real_get_physical_device_properties{};
void VKAPI_PTR hook_get_physical_device_properties(VkPhysicalDevice a, VkPhysicalDeviceProperties* b) {
    FbHook fb_(FB_H_GET_PHYSICAL_DEVICE_PROPERTIES);
    real_get_physical_device_properties(a, b);
}
static_assert(std::is_same_v<decltype(&hook_get_physical_device_properties), PFN_vkGetPhysicalDeviceProperties>, "signature mismatch: vkGetPhysicalDeviceProperties");
PFN_vkGetPhysicalDeviceQueueFamilyProperties real_get_physical_device_queue_family_properties{};
void VKAPI_PTR hook_get_physical_device_queue_family_properties(VkPhysicalDevice a, uint32_t* b, VkQueueFamilyProperties* c) {
    FbHook fb_(FB_H_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_PROPERTIES);
    real_get_physical_device_queue_family_properties(a, b, c);
}
static_assert(std::is_same_v<decltype(&hook_get_physical_device_queue_family_properties), PFN_vkGetPhysicalDeviceQueueFamilyProperties>, "signature mismatch: vkGetPhysicalDeviceQueueFamilyProperties");
PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR real_get_physical_device_surface_capabilities_khr{};
VkResult VKAPI_PTR hook_get_physical_device_surface_capabilities_khr(VkPhysicalDevice a, VkSurfaceKHR b, VkSurfaceCapabilitiesKHR* c) {
    FbHook fb_(FB_H_GET_PHYSICAL_DEVICE_SURFACE_CAPABILITIES_KHR);
    return real_get_physical_device_surface_capabilities_khr(a, b, c);
}
static_assert(std::is_same_v<decltype(&hook_get_physical_device_surface_capabilities_khr), PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>, "signature mismatch: vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
PFN_vkGetPhysicalDeviceSurfaceFormatsKHR real_get_physical_device_surface_formats_khr{};
VkResult VKAPI_PTR hook_get_physical_device_surface_formats_khr(VkPhysicalDevice a, VkSurfaceKHR b, uint32_t* c, VkSurfaceFormatKHR* d) {
    FbHook fb_(FB_H_GET_PHYSICAL_DEVICE_SURFACE_FORMATS_KHR);
    return real_get_physical_device_surface_formats_khr(a, b, c, d);
}
static_assert(std::is_same_v<decltype(&hook_get_physical_device_surface_formats_khr), PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>, "signature mismatch: vkGetPhysicalDeviceSurfaceFormatsKHR");
PFN_vkGetPhysicalDeviceSurfacePresentModesKHR real_get_physical_device_surface_present_modes_khr{};
VkResult VKAPI_PTR hook_get_physical_device_surface_present_modes_khr(VkPhysicalDevice a, VkSurfaceKHR b, uint32_t* c, VkPresentModeKHR* d) {
    FbHook fb_(FB_H_GET_PHYSICAL_DEVICE_SURFACE_PRESENT_MODES_KHR);
    return real_get_physical_device_surface_present_modes_khr(a, b, c, d);
}
static_assert(std::is_same_v<decltype(&hook_get_physical_device_surface_present_modes_khr), PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>, "signature mismatch: vkGetPhysicalDeviceSurfacePresentModesKHR");
PFN_vkGetPhysicalDeviceSurfaceSupportKHR real_get_physical_device_surface_support_khr{};
VkResult VKAPI_PTR hook_get_physical_device_surface_support_khr(VkPhysicalDevice a, uint32_t b, VkSurfaceKHR c, VkBool32* d) {
    FbHook fb_(FB_H_GET_PHYSICAL_DEVICE_SURFACE_SUPPORT_KHR);
    return real_get_physical_device_surface_support_khr(a, b, c, d);
}
static_assert(std::is_same_v<decltype(&hook_get_physical_device_surface_support_khr), PFN_vkGetPhysicalDeviceSurfaceSupportKHR>, "signature mismatch: vkGetPhysicalDeviceSurfaceSupportKHR");
PFN_vkCreateWin32SurfaceKHR real_create_win32_surface_khr{};
// ===== THE RENDER RESOLUTION, OWNED BY THE MOD (2026-08-30) ================
//
// DOOM renders at its swapchain size, and its swapchain size is its window's
// CLIENT AREA. So resizing the window IS setting the render resolution - no
// game video settings, no Steam launch options, no cvar writes, and no lying
// to the WSI about surface capabilities (which is spec-fragile and driver
// dependent). The window is allowed to be larger than the desktop; the part
// that hangs off-screen is never looked at in VR.
//
// Measured 2026-08-30: DOOM rendered 3818x2032 - a 3840x2160 desktop minus
// borders and title bar - into two 4032x3648 eye buffers. That is 20.2 px/deg
// vertically against a 36.2 px/deg panel, a 1.79x stretch, and it is why the
// image looks soft. It also self-corrects the field of view: DOOM's FOV slider
// is defined at 16:9 and rescaled by the frame aspect, so a 1.105 frame turns
// slider 130 into ~106x101 deg instead of 132x101 - much closer to what the
// headset can actually show, and it stops spending pixels on invisible margin.
std::atomic<HWND> g_doom_hwnd{nullptr};  // declared extern near the top
std::atomic_bool g_render_resize_done{false};
// ⚠ WHY A WNDPROC SUBCLASS IS REQUIRED, and it is not optional.
//
// The first version just called SetWindowPos and the POST check caught it:
// "RENDERRES ***CLAMPED***", client area 3818x2032 -> 3844x2130 instead of the
// 4032x3648 asked for. Windows caps every window to a MAXIMUM TRACKING SIZE
// derived from the monitor - DefWindowProc fills it in on WM_GETMINMAXINFO -
// and SetWindowPos obeys it. No window style avoids it; the only way past is
// to answer that message ourselves.
//
// So we subclass DOOM's window, raise ptMaxTrackSize/ptMaxSize, and forward
// every other message untouched. This is the game's own window procedure, so
// the rules are: call the original for everything, never swallow a message,
// and put it back on shutdown.
WNDPROC g_doom_wndproc_orig = nullptr;
LRESULT CALLBACK doomvr_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_GETMINMAXINFO && lp) {
        // Let DOOM (and DefWindowProc) fill it in first, then lift only the
        // ceiling. Overwriting the whole struct would also clobber the MINIMUM
        // tracking size, which the game may rely on.
        const LRESULT r = g_doom_wndproc_orig
            ? CallWindowProcW(g_doom_wndproc_orig, h, msg, wp, lp)
            : DefWindowProcW(h, msg, wp, lp);
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMaxTrackSize.x = (std::max)(mmi->ptMaxTrackSize.x, 16384L);
        mmi->ptMaxTrackSize.y = (std::max)(mmi->ptMaxTrackSize.y, 16384L);
        mmi->ptMaxSize.x = (std::max)(mmi->ptMaxSize.x, 16384L);
        mmi->ptMaxSize.y = (std::max)(mmi->ptMaxSize.y, 16384L);
        return r;
    }
    return g_doom_wndproc_orig ? CallWindowProcW(g_doom_wndproc_orig, h, msg, wp, lp)
                               : DefWindowProcW(h, msg, wp, lp);
}
// ===== VIRTUAL TEXTURE TUNING (2026-08-30) =================================
//
// Why textures stayed soft after the render resolution was fixed. DOOM streams
// them through a virtual-texture page cache sized at init, and its own cvar
// descriptions spell out what happens when it runs short:
//
//   vt_feedbackHighWater  "More than this percentage of pages needed this
//                          frame and we consider ourselves OVERSUBSCRIBED."
//   vt_feedbackLODIncrement
//   vt_lodBias            "Additional floating point bias for virtual texture
//                          page tables"
//
// Oversubscribed means it RAISES the LOD bias - i.e. it deliberately blurs
// textures to fit the pool. We just raised the pixel count 1.9x AND we render
// the scene twice for stereo, so page demand roughly tripled against a pool
// that did not grow. Geometry stays sharp and only streamed textures blur,
// which is exactly the report.
//
// Objects located statically this session (name string -> its single
// rip-relative xref -> the `lea rcx` feeding the idCVar constructor at
// 0x293870). Field layout is this repo's own, from the r_useSMP flip:
// +0x30 is the integer, +0x34 the float, and reads go to +0x34.
void install_size_limit_bypass(HWND hwnd) {
    if (!hwnd || g_doom_wndproc_orig) return;
    g_doom_wndproc_orig = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&doomvr_wndproc)));
    if (!g_doom_wndproc_orig) {
        log::error("RENDERRES: could not subclass DOOM's window; the size limit stands "
                   "and the render resolution cannot be raised this session.");
    }
}
void apply_forced_render_resolution(const char* why) {
    const int want_w = wrap::config_render_w();
    const int want_h = wrap::config_render_h();
    if (want_w <= 0 || want_h <= 0) return;  // 0 = leave DOOM's window alone
    HWND hwnd = g_doom_hwnd.load(std::memory_order_relaxed);
    if (!hwnd || !IsWindow(hwnd)) return;
    // Must be in place BEFORE SetWindowPos, or Windows clamps the request to
    // the monitor-derived tracking size and the resize silently comes up short.
    install_size_limit_bypass(hwnd);
    RECT before{};
    GetClientRect(hwnd, &before);
    const int cur_w = before.right - before.left;
    const int cur_h = before.bottom - before.top;
    if (cur_w == want_w && cur_h == want_h) {
        g_render_resize_done.store(true, std::memory_order_relaxed);
        return;
    }
    // Client area is what Vulkan sees, so convert the desired CLIENT size into
    // the outer window size through the window's own style. Sizing the outer
    // rect directly would come up short by exactly the border and caption -
    // which is the 22x128 that made 3840x2160 into 3818x2032 in the first place.
    RECT want{0, 0, want_w, want_h};
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    AdjustWindowRectEx(&want, static_cast<DWORD>(style), FALSE, static_cast<DWORD>(ex));
    SetWindowPos(hwnd, nullptr, 0, 0, want.right - want.left, want.bottom - want.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    RECT after{};
    GetClientRect(hwnd, &after);
    const int got_w = after.right - after.left;
    const int got_h = after.bottom - after.top;
    // ⚠ Marked done ONLY on success. The first version set this before the
    // POST check, so when Windows clamped the request there was never a second
    // attempt - the retry path was dead exactly when it was needed. Bounded so
    // a permanent refusal cannot log every frame forever.
    static std::atomic_uint32_t attempts{0};
    const uint32_t attempt = attempts.fetch_add(1, std::memory_order_relaxed);
    if (got_w == want_w && got_h == want_h) {
        g_render_resize_done.store(true, std::memory_order_relaxed);
    } else if (attempt >= 20) {
        g_render_resize_done.store(true, std::memory_order_relaxed);
        log::error("RENDERRES: giving up after 20 attempts; the window will not take the "
                   "requested client size. Render resolution is unchanged.");
        return;
    } else if (attempt > 0 && attempt < 20) {
        return;  // retry silently next present; only the first and last speak
    }
    // STRUCTURAL POST CHECK, and it is the whole point: Windows will silently
    // clamp a window to the work area on some styles, and a clamp looks exactly
    // like a resize that worked until the swapchain comes back the old size.
    if (got_w == want_w && got_h == want_h) {
        log::warn(std::format(
            "RENDERRES ({}): DOOM's client area {}x{} -> {}x{}. DOOM renders at its "
            "swapchain size and the swapchain follows the client area, so this IS the "
            "render resolution. Confirm on the next SWAPCHAIN CREATED line and on "
            "WraparoundEyes src=.",
            why, cur_w, cur_h, got_w, got_h));
    } else {
        log::error(std::format(
            "RENDERRES ({}): ***CLAMPED***. Asked for a {}x{} client area, got {}x{} "
            "(was {}x{}). Windows refused the size - most likely the window style will "
            "not exceed the work area. The render resolution is NOT what was asked for "
            "and any sharpness judgement from this run is void; say so rather than "
            "reading the picture.",
            why, want_w, want_h, got_w, got_h, cur_w, cur_h));
    }
}
VkResult VKAPI_PTR hook_create_win32_surface_khr(VkInstance a, const VkWin32SurfaceCreateInfoKHR* b, const VkAllocationCallbacks* c, VkSurfaceKHR* d) {
    FbHook fb_(FB_H_CREATE_WIN32_SURFACE_KHR);
    // Resize BEFORE the surface is created, so the very first swapchain is
    // already the size we want and DOOM never has to recreate it mid-run.
    // If we were injected after the surface already existed this hook never
    // fires - the present-path retry below covers that case.
    if (b && b->hwnd) {
        g_doom_hwnd.store(b->hwnd, std::memory_order_relaxed);
        apply_forced_render_resolution("surface create");
    }
    return real_create_win32_surface_khr(a, b, c, d);
}
static_assert(std::is_same_v<decltype(&hook_create_win32_surface_khr), PFN_vkCreateWin32SurfaceKHR>, "signature mismatch: vkCreateWin32SurfaceKHR");
// ==== END TIMING-ONLY HOOKS ================================================

// COVERAGE AUDIT (2026-08-25). Every export that install() actually registers
// is recorded here, and emit_hook_coverage() cross-checks that list against the
// FrameBudget id table. This exists because I timed 16 of 53 hooks and called
// it exhaustive; the audit makes completeness a MEASUREMENT IN THE LOG, printed
// before the run rather than a claim discovered to be false after it.
std::vector<std::string> installed_exports;
// Every export install() TRIED, whether or not the loader had it. Needed to
// tell three different things apart, which otherwise all look like a gap:
//   installed + timed        -> covered
//   attempted, not installed -> the export does not exist on this driver
//                               (vkCmdPushDescriptorSetKHR is absent here and
//                               is reported absent at startup). It executes
//                               nothing, so it is NOT a coverage gap.
//   in the id table, never attempted -> the table has drifted. A real problem.
std::vector<std::string> attempted_exports;

void emit_hook_coverage() {
    std::string missing, dedicated;
    uint32_t timed = 0, ded = 0;
    for (const std::string& ex : installed_exports) {
        int found = -1;
        for (int i = 0; i < FB_H_COUNT; ++i)
            if (ex == kFbHookExport[i]) { found = i; break; }
        if (found < 0) { missing += " " + ex; continue; }
        if (kFbDedicated[found]) { ++ded; dedicated += " " + ex; }
        else ++timed;
    }
    // Reverse direction, split three ways so an absent driver export is not
    // mistaken for a coverage gap.
    std::string absent, drift;
    for (int i = 0; i < FB_H_COUNT; ++i) {
        bool inst = false, tried = false;
        for (const std::string& ex : installed_exports)
            if (ex == kFbHookExport[i]) { inst = true; break; }
        for (const std::string& ex : attempted_exports)
            if (ex == kFbHookExport[i]) { tried = true; break; }
        if (inst) continue;
        if (tried) absent += std::string(" ") + kFbHookExport[i];   // not on this driver
        else drift += std::string(" ") + kFbHookExport[i];          // table drifted: a real gap
    }
    const bool complete = missing.empty() && drift.empty();
    log::warn(std::format(
        "HookCoverage: attempted={} installed={} timed={} dedicated={} -> {}\n"
        "  dedicated (timed by their own FrameBudget field, deliberately NOT in ALLHOOKS):{}\n"
        "  UNTIMED (installed and running but NOT measured - would land in RESIDUAL and be "
        "indistinguishable from DOOM's own work):{}\n"
        "  absent on this driver (attempted, export does not exist, executes nothing - NOT a "
        "gap):{}\n"
        "  TABLE DRIFT (id exists but install() never even attempted it - a real gap):{}\n"
        "⚠ IF THIS LINE DOES NOT SAY COMPLETE, the FrameBudget RESIDUAL cannot be attributed "
        "and no ours-or-theirs conclusion may be drawn from this run.",
        attempted_exports.size(), installed_exports.size(), timed, ded,
        complete ? "COMPLETE - every installed export is accounted for"
                 : "INCOMPLETE - DO NOT DRAW CONCLUSIONS FROM RESIDUAL",
        dedicated.empty() ? " none" : dedicated,
        missing.empty() ? " none" : missing,
        absent.empty() ? " none" : absent,
        drift.empty() ? " none" : drift));
    // DOOM-side hooks are not Vulkan exports and cannot appear in the table
    // above, so they get their own audit line. All 19 are OVERHEAD-timed
    // (span minus trampoline) into DOOMHOOKS - measuring their full span would
    // swallow DOOM's own rendering and answer nothing.
    std::string doom_list;
    for (int i = 0; i < FB_D_COUNT; ++i) doom_list += std::string(" ") + kFbDoomName[i];
    log::warn(std::format(
        "HookCoverage (DOOM-side, not Vulkan exports): {} installed hooks, ALL overhead-timed "
        "(span minus trampoline) into DOOMHOOKS:{}\n"
        "  hook_doom_render_surface and hook_append_draw_command are DEFINED BUT NOT INSTALLED "
        "(the latter was removed 2026-08-28 for corrupting DOOM's rendering), so they execute "
        "nothing and are correctly absent.\n"
        "  The doubled call lives inside hook_frame_root and is reported separately as "
        "`doubled`; frame_root's own overhead excludes it via the trampoline subtraction.",
        static_cast<int>(FB_D_COUNT), doom_list));
}

template <typename T>
bool add_hook(HMODULE module, const char* name, void* replacement, T& original) {
    attempted_exports.emplace_back(name);
    void* target = reinterpret_cast<void*>(GetProcAddress(module, name));
    if (!target) {
        log::warn(std::string("Vulkan export not found: ") + name);
        return false;
    }
    if (MH_CreateHook(target, replacement, reinterpret_cast<void**>(&original)) != MH_OK) {
        log::error(std::string("MH_CreateHook failed: ") + name);
        return false;
    }
    // Recorded ONLY on success, so the coverage audit reflects the hooks that
    // are actually live rather than the ones install() attempted.
    installed_exports.emplace_back(name);
    return true;
}

// 04_render_jobs.inc - hooks on DOOM's own render jobs and view setup
// 3995 of these lines sit inside #ifdef DOOMVR_RESEARCH_TOOLS,
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

// Put every byte back the instant the real call returns. Same discipline as
// the gun unshift above: the window is one call wide, and a failed write never
// leaves a half-applied state because only successful writes were recorded.
void gui_models_restore() {
    for (int i = 0; i < g_model_saved_count; ++i) {
        if (safe_write_bytes(g_model_saved[i].addr, g_model_saved[i].saved, 64))
            g_model_restored.fetch_add(1, std::memory_order_relaxed);
    }
    g_model_saved_count = 0;
}
void __fastcall hook_render_gui_models(void* arg1) {
    FbDoom fbd_(FB_D_RENDER_GUI_MODELS);
    // v5 armed-state = lever_g_slot_index >= 0. Fires apply_lever_g_mutation
    // which itself handles slot-cache populate + single-slot clobber. The
    // legacy probe still fires on first arm; kept for the initial context
    // dump on session start.
    if (lever_g_slot_index.load(std::memory_order_relaxed) >= 0) {
        if (arg1 && lever_g_probe_pending.exchange(false, std::memory_order_relaxed)) {
            log_render_gui_context_matrices(reinterpret_cast<uintptr_t>(arg1));
        }
        apply_lever_g_mutation(arg1, 0);
    }
    log_world_vp_at("RenderGuiModels");
    // GUN UNSHIFT. See gun_unshift_enabled for the two refuted predecessors and
    // why the unshift belongs here rather than at SetupView. The clean matrix
    // goes in for the duration of this one call and the shifted one comes
    // straight back, so the weapon draws from DOOM's own view while every draw
    // outside this window keeps the stereo shift.
    bool swapped = false;
    uintptr_t swap_addr = 0;
    // RESTORE WHAT WAS FOUND, not what was published. Nothing here guarantees
    // that this job runs after Lever V within the same call - if it runs first,
    // the standing publish is the OTHER side's, and putting back its shifted
    // matrix would hand the walls the wrong sign. Reading the value in force at
    // swap-in and writing that exact value back at swap-out is immune to the
    // ordering either way. The published clean matrix is still used going in:
    // it is the same base on both sides, so it is safe whoever published it.
    alignas(16) float swap_found[16]{};
    const bool live_symmetric_now =
        frame_root_full_frame.load(std::memory_order_relaxed) &&
        frame_root_double_enabled.load(std::memory_order_relaxed) &&
        !frame_root_double_ab.load(std::memory_order_relaxed);
    if (kharvox::native::requested() && live_symmetric_now) {
        static std::atomic_uint32_t reports{0};
        if (reports.fetch_add(1,std::memory_order_relaxed)<8)
            log::info(std::format("Native weapon eye camera side={} offset={} legacyGunUnshift=false",
                inside_frame_root_double?"right":"left",g_stereo_offset.load(std::memory_order_relaxed)));
    }
    if (kharvox::native::allowLegacyGunUnshift(kharvox::native::requested(),
            gun_unshift_enabled.load(std::memory_order_relaxed),live_symmetric_now)) {
        alignas(16) float clean[16]{};
        float offset = 0.0f;
        uint32_t seq = 0;
        bool valid = false;
        {
            std::scoped_lock lock(gun_unshift_mutex);
            valid = gun_unshift_valid;
            if (valid) {
                swap_addr = gun_unshift_addr;
                offset = gun_unshift_offset;
                seq = gun_unshift_seq;
                std::memcpy(clean, gun_unshift_clean, sizeof(clean));
            }
        }
        bool ok_pre = false;
        if (valid && swap_addr && safe_read_bytes(swap_addr, swap_found, sizeof(swap_found))) {
            ok_pre = safe_write_bytes(swap_addr, clean, sizeof(clean));
            swapped = ok_pre;
            if (swapped) vwc_gun_unshifts.fetch_add(1, std::memory_order_relaxed);
        }
        // THE LOG IS THE INSTRUMENT FOR THIS INTERVENTION, not a separate probe.
        // If the gun still moves there are three different reasons and they are
        // not distinguishable from the picture: this job never ran, it ran but
        // the write failed, or it ran and wrote an address the weapon does not
        // read. So record the fire itself, both candidate context objects and
        // whether either matches the address Lever V wrote, and the thread this
        // runs on against Lever V's - a mismatch is the concurrency hazard the
        // declaration names.
        // ===== GUNWINDOW (2026-08-26): 1x OR 2x, MEASURED ==================
        //
        // The gun and arms counter-rotate at roughly TWICE the head angle.
        // Build B removed rotate_camera_src_for_head and the anomaly did not
        // move, so the doubler is not that site. The next question is whether
        // the matrix this window swaps IN already carries the head rotation:
        // if it does, the weapon draws through R and DOOM then places the
        // viewmodel from a camera that also carries R, which is the 2R the
        // owner sees.
        //
        // This measures it directly and read-only: the angle between the
        // swapped-in matrix's right axis and DOOM's CLEAN camera right axis,
        // against the head yaw driving it. ratio ~1.0 means the matrix carries
        // R exactly once and the second copy is downstream, in the viewmodel's
        // own placement. ratio ~2.0 means this window is itself the doubler.
        // ratio ~0.0 means the swapped-in matrix is clean and the whole 2x
        // lives elsewhere.
        //
        // Own budget, NOT gun_unshift_budget: sharing one cap lets the frequent
        // side starve the rare one. Gated on a head angle big enough to divide
        // by, and on gameplay, and the raw attempt is counted before either.
        {
            g_gunang_attempts.fetch_add(1, std::memory_order_relaxed);
            float hy = 0.0f, hp = 0.0f, hr = 0.0f;
            float rr[3], ru[3], rf[3];
            wrap::VpSnapshot gvp;
            const bool gameplay = wrap::world_vp(gvp) && wrap::is_gameplay_view(gvp.m);
            if (gameplay && valid && wrap::residual_head_angles(hy, hp, hr) &&
                wrap::reference_basis(rr, ru, rf) &&
                std::fabs(hy) > 0.0873f &&   // 5 degrees - below this the ratio is noise
                g_gunang_lines.load(std::memory_order_relaxed) < kMaxGunAngleLines) {
                auto axis_angle = [&](const float* mm) {
                    const float l = wrap::row_length(mm, 0);
                    if (!(l > 1e-6f)) return -1.0f;
                    float d = (mm[0] / l) * rr[0] + (mm[1] / l) * rr[1] + (mm[2] / l) * rr[2];
                    if (d > 1.0f) d = 1.0f;
                    if (d < -1.0f) d = -1.0f;
                    return std::acos(d);
                };
                const float a_in = axis_angle(clean);
                const float a_found = axis_angle(swap_found);
                const float head = std::fabs(hy);
                g_gunang_lines.fetch_add(1, std::memory_order_relaxed);
                log::warn(std::format(
                    "GunWindow side={} headYaw={:.4f} rad ({:.2f} deg) | swappedIn vs CLEAN "
                    "basis: {:.4f} rad ({:.2f} deg) ratio={:.3f} | foundInPlace vs CLEAN: "
                    "{:.4f} rad ({:.2f} deg) ratio={:.3f}\n"
                    "READING IT: READ-ONLY measurement of the matrix this weapon window swaps "
                    "IN, against DOOM's own unrotated camera right axis. ratio ~1.0 means the "
                    "swapped-in matrix carries the head rotation EXACTLY ONCE, so the second "
                    "copy the owner sees is applied downstream in the viewmodel's own "
                    "placement and this window is not the doubler. ratio ~2.0 means this "
                    "window IS the doubler. ratio ~0.0 means the swapped-in matrix is clean "
                    "and the entire 2x lives somewhere neither of these matrices reaches. "
                    "A negative angle means row0 had no length and the number is void. "
                    "Capped at {} lines and only sampled past 5 degrees of head yaw, where "
                    "the ratio stops being noise.",
                    inside_frame_root_double ? "dbl" : "real", hy, hy * 57.29578f,
                    a_in, a_in * 57.29578f, head > 1e-4f ? a_in / head : -1.0f,
                    a_found, a_found * 57.29578f, head > 1e-4f ? a_found / head : -1.0f,
                    kMaxGunAngleLines));
            }
        }
        // ===== GUICTXHUNT (2026-08-26): FIND THE OBJECT THE WEAPON READS ===
        //
        // THE 18:29 RUN SETTLED WHY BOTH GUN LEVERS ARE INERT. Every
        // GunUnshift line reads match=NEITHER: the address Lever V writes
        // (ctx+0x11F0 of the object IT found) is neither ctx28+0x11F0 nor
        // ctx30+0x11F0 of the context RenderGuiModels is actually handed.
        // Changing WHAT we publish into that address cannot matter when the
        // weapon does not read that address - which is exactly the CTRL+Delete
        // null, and it is a wrong-target result, not a wrong-idea one.
        //
        // So stop guessing offsets and locate it the way the destinations were
        // located: solve every candidate matrix in each context object and
        // keep the ones encoding THIS FRAME'S camera position. Read-only,
        // one shot per object per session, gameplay-gated by camera_hunt
        // itself. Three objects: the two context pointers and the argument.
        if (arg1 && g_gui_ctx_hunt_mask.load(std::memory_order_relaxed) != 0x7 &&
            wrap::armed()) {
            uintptr_t cands[3]{0, 0, reinterpret_cast<uintptr_t>(arg1)};
            safe_read_bytes(reinterpret_cast<uintptr_t>(arg1) + 0x28, &cands[0], sizeof(cands[0]));
            safe_read_bytes(reinterpret_cast<uintptr_t>(arg1) + 0x30, &cands[1], sizeof(cands[1]));
            static const char* kNames[3]{"guiCtx28", "guiCtx30", "guiArg1"};
            for (size_t c = 0; c < 3; ++c) {
                const uint32_t mask = g_gui_ctx_hunt_mask.load(std::memory_order_relaxed);
                if ((mask & (1u << c)) || !cands[c]) continue;
                if (camera_hunt(kNames[c], c, cands[c]))
                    g_gui_ctx_hunt_mask.fetch_or(1u << c, std::memory_order_relaxed);
                break;   // one object per call - never three 32 KB scans in one frame
            }
        }
        // ===== CHAINANGLES (2026-08-26): WHERE DOES R APPEAR TWICE? =======
        //
        // WHY THIS AND NOT A FIFTH FIX. Four interventions aimed at the weapon
        // have all nulled, and the 19:29 run explains why none of them could
        // have worked as reasoned: CTRL+Up proved Lever V's rotation IS
        // load-bearing (turning it off welded a large scenery population to
        // the head), so the site works - the weapon simply is not where the
        // extra rotation enters.
        //
        // The gun moves OPPOSITE the head at roughly TWICE the angle. A
        // body-fixed weapon and a world-fixed wall should behave identically
        // on screen when the body is still, so "twice the world" means the
        // weapon's chain carries R twice while the world's carries it once.
        //
        // The decompile of the per-model draw names the chain explicitly:
        // ctx+0x1130, +0x11B0, +0x11F0 and +0x1230 (see
        // log_render_gui_context_matrices, which already dumps those exact
        // offsets raw). Lever V rotates ONE of them. This measures the
        // rotation angle each one actually carries, against the clean camera
        // basis and against the head yaw driving it, at the moment of the
        // weapon draw.
        //
        // ONE line names the culprit: any offset reading ratio ~1.0 carries R.
        // If exactly one does, the doubling is not in this chain at all. If
        // TWO do, that is the 2x, and the second one is the thing to cancel.
        //
        // Read-only. Own budget. Gameplay-gated, and only past 5 degrees of
        // head yaw where the ratio stops being noise.
        if (g_chain_lines.load(std::memory_order_relaxed) < kMaxChainLines) {
            g_chain_attempts.fetch_add(1, std::memory_order_relaxed);
            float hy = 0.0f, hp = 0.0f, hr = 0.0f;
            float rr[3], ru[3], rf[3];
            wrap::VpSnapshot cvp;
            if (arg1 && wrap::world_vp(cvp) && wrap::is_gameplay_view(cvp.m) &&
                wrap::residual_head_angles(hy, hp, hr) && wrap::reference_basis(rr, ru, rf) &&
                std::fabs(hy) > 0.0873f) {
                uintptr_t c28 = 0, c30 = 0;
                safe_read_bytes(reinterpret_cast<uintptr_t>(arg1) + 0x28, &c28, sizeof(c28));
                safe_read_bytes(reinterpret_cast<uintptr_t>(arg1) + 0x30, &c30, sizeof(c30));
                const uintptr_t leverv_ctx = swap_addr ? swap_addr - 0x11F0 : 0;
                const float head = std::fabs(hy);
                // 0x1170 ADDED 2026-08-26 - it was MISSING from the first ChainAngles and it
                // is the one offset the decompile actually names as the bake's other
                // operand: fcn(ctx+0x1170, ctx+0x11F0, renderer+0x4955A4).
                static constexpr uint32_t kChainOffsets[5]{0x1130, 0x1170, 0x11B0, 0x11F0, 0x1230};
                const uintptr_t bases[3]{leverv_ctx, c28, c30};
                static const char* kBaseNames[3]{"leverVctx", "ctx28", "ctx30"};
                std::string report;
                for (int b = 0; b < 3; ++b) {
                    if (!bases[b]) continue;
                    report += std::format(" | {}={:#x}", kBaseNames[b], bases[b]);
                    for (uint32_t off : kChainOffsets) {
                        alignas(16) float cm[16]{};
                        if (!safe_read_bytes(bases[b] + off, cm, sizeof(cm))) {
                            report += std::format(" {:#x}=UNREADABLE", off);
                            continue;
                        }
                        const float l = wrap::row_length(cm, 0);
                        if (!(l > 1e-6f) || !std::isfinite(l)) {
                            report += std::format(" {:#x}=noRow0", off);
                            continue;
                        }
                        float d = (cm[0] / l) * rr[0] + (cm[1] / l) * rr[1] + (cm[2] / l) * rr[2];
                        if (d > 1.0f) d = 1.0f;
                        if (d < -1.0f) d = -1.0f;
                        const float a = std::acos(d);
                        report += std::format(" {:#x}=(deg {:.2f} ratio {:.3f} r0len {:.4f})",
                                              off, a * 57.29578f, a / head, l);
                    }
                }
                g_chain_lines.fetch_add(1, std::memory_order_relaxed);
                log::warn(std::format(
                    "ChainAngles side={} headYaw={:.2f} deg{}\n"
                    "READING IT: ratio is each matrix's rotation away from DOOM's CLEAN camera "
                    "right axis, divided by the head yaw driving it. ratio ~1.0 = that matrix "
                    "carries the head rotation ONCE. ratio ~0.0 = it is clean. The weapon's "
                    "per-model draw composes these offsets together, so if TWO of them read "
                    "~1.0 their product is the 2x the owner sees and the second one is what to "
                    "cancel. If exactly ONE reads ~1.0, the doubling is not in this chain and "
                    "the weapon work leaves RenderGuiModels entirely. UNREADABLE on an offset "
                    "means that base is a short object and was never a view context - which is "
                    "itself the answer for that candidate. Capped at {} lines.",
                    inside_frame_root_double ? "dbl" : "real", hy * 57.29578f, report,
                    kMaxChainLines));
            }
        }
        if (gun_unshift_budget.take(inside_frame_root_double)) {
            uintptr_t ctx28 = 0, ctx30 = 0;
            int32_t model_count = -1;
            if (arg1) {
                safe_read_bytes(reinterpret_cast<uintptr_t>(arg1) + 0x28, &ctx28, sizeof(ctx28));
                safe_read_bytes(reinterpret_cast<uintptr_t>(arg1) + 0x30, &ctx30, sizeof(ctx30));
                safe_read_bytes(reinterpret_cast<uintptr_t>(arg1) + 0x10,
                    &model_count, sizeof(model_count));
            }
            const char* match =
                (ctx28 && ctx28 + 0x11F0 == swap_addr) ? "ctx28"
                : (ctx30 && ctx30 + 0x11F0 == swap_addr) ? "ctx30"
                : "NEITHER";
            log::warn(std::format(
                "GunUnshift side={} seq={} valid={} models={} leverVaddr={:#x} ctx28={:#x} "
                "ctx30={:#x} match={} offset={:.3f} m3 found={:.3f} clean={:.3f} wrotePre={} "
                "tid={} | match=NEITHER means this job reads a DIFFERENT object than Lever V "
                "writes, so a gun that still moves is a wrong-address result, not a wrong-job "
                "one. models=0 means it drew nothing and the weapon is somewhere else entirely",
                inside_frame_root_double ? "dbl" : "real", seq, valid, model_count,
                swap_addr, ctx28, ctx30, match, offset, swap_found[3], clean[3], ok_pre,
                GetCurrentThreadId()));
        }
    }
    gui_models_scan_and_patch(arg1);
    // ===== SLOT 1: DOES THIS PASS EVEN DRAW THE GUN? =====================
    //
    // THE TEST THAT SHOULD HAVE COME FIRST. Four interventions have now been
    // aimed inside this hook - the base swap, the bake patch, the model
    // transforms, the unshift - on the ASSUMPTION that RenderGuiModels draws
    // the weapon. That assumption has never been tested, and four nulls in one
    // hook is exactly what testing the wrong pass looks like.
    //
    // Skipping the real call is unambiguous and needs no measurement: whatever
    // vanishes from the image is what this pass draws.
    //   gun disappears        -> this IS the weapon's pass. The transform is
    //                            somewhere in its object graph we have not
    //                            reached, and the search continues here.
    //   gun stays, HUD-3D goes-> the weapon is drawn ELSEWHERE, every
    //                            intervention in this hook was aimed at the
    //                            wrong pass, and the whole search moves.
    //   nothing changes       -> the hook is not running; check the counter.
    // RETIRED 2026-08-27: the RenderGuiModels skip elimination is complete
    // (450 skipped calls, nothing vanished). Slot 1 is reassigned; hard false
    // so the reassigned slot can never silently skip this pass again.
    const bool skip_pass = false && wrap::armed();
    if (skip_pass) {
        g_gui_pass_skipped.fetch_add(1, std::memory_order_relaxed);
    } else {
        FbTramp fbt_(FB_D_RENDER_GUI_MODELS);
        real_render_gui_models(arg1);
    }
    gui_models_restore();
    // Put back exactly the bytes that were in force at swap-in, so the world
    // draws that follow are unaffected. Gated on `swapped`, so a failed
    // pre-write never leaves a half-applied state behind.
    if (swapped) safe_write_bytes(swap_addr, swap_found, sizeof(swap_found));
}

// Lever V (2026-08-11). See real_setup_view_gui declaration for the
// mechanism. Modes reuse LeverAMode enum. Fires BEFORE the real SetupView
// so the mutation propagates through the MVP compute inside SetupView.
std::atomic_int lever_v_mode{static_cast<int>(LeverAMode::Off)};
std::atomic_bool lever_v_probe_pending{false};
std::atomic_uint32_t lever_v_probe_log_count{0};
constexpr uint32_t kMaxLeverVProbeLogLines = 4;

// ===== VIEWSRC PROBE - READ-ONLY, NO BEHAVIOURAL CHANGE ====================
//
// Step 2 of the pre-cull camera producer thread. Reads only; writes nothing
// into DOOM. See the counter block near CamSite for the polarity statement
// that was written BEFORE the run.
//
// The classification is deliberately done on viewParms+0x1270 and not on the
// raw axis at +0x6C. +0x1270 is proj*view - byte-for-byte the layout SetupView
// copies into renderView+0xC44 - so the EXISTING, already-proven classifier
// (matches_reference_basis_signed, the one CamStateCensus uses on +0xC44) is
// valid on it without inventing a second comparison for a different layout.
// A new matcher for idMat3 would be an unvalidated instrument answering the
// decisive question, which is exactly how a null becomes unreadable.
void viewsrc_remember_view_parms(uintptr_t vp) {
    if (!vp) return;
    for (uint32_t i = 0; i < kViewSrcRing; ++i)
        if (g_vs_known_parms[i].load(std::memory_order_relaxed) == vp) return;
    const uint32_t slot = g_vs_known_next.fetch_add(1, std::memory_order_relaxed) % kViewSrcRing;
    g_vs_known_parms[slot].store(vp, std::memory_order_relaxed);
}
bool viewsrc_is_known_view_parms(uintptr_t vp) {
    if (!vp) return false;
    for (uint32_t i = 0; i < kViewSrcRing; ++i)
        if (g_vs_known_parms[i].load(std::memory_order_relaxed) == vp) return true;
    return false;
}

// ===== VIEWSRC RIDER: CullLights (DOOM+0x187E330) ==========================
//
// READ-ONLY, and it rides the run rather than costing one of its own.
// It answers the one question the producer probe alone cannot: is the camera
// object the LIGHT BINNER reads the same object the producer builds? A write
// at the producer depends on that identity, and an identity worth depending on
// is worth measuring rather than assuming.
//
// RE-POINTED 2026-08-30. The step-2 build aimed this at
// ComputeClustersFrustums (0x187B910) and read a hard zero over 2272 frames.
// That was my aim, not a dead hook: RenderView branches on byte [jobArg+0x21]
// at 0x17B3BD4, this scene takes the ==0 arm into the SYNCHRONOUS clustered
// path 0x1881FF0, and inside it 0x187B910 is gated on `*0x146EAC230 != 0`,
// which read 0. CullLights is on that path unconditionally (called at
// 0x188206D), takes the cluster context directly, and reads *(arg1+0x38).
//
// pescan extent 187E330 -> root 187E330, size 2102 - a .pdata root, checked
// before install like every other DOOM hook in this table.
DoomRenderOpaqueFn real_cull_lights{};
void __fastcall hook_cull_lights(void* job_arg) {
    FbDoom fbd_(FB_D_CULL_LIGHTS);
    g_ccf_calls.fetch_add(1, std::memory_order_relaxed);
    if (g_vs_a1_calls_this_frame.load(std::memory_order_relaxed) != 0)
        g_ccf_after_a1.fetch_add(1, std::memory_order_relaxed);
    uintptr_t vparms = 0;
    if (!job_arg) {
        g_ccf_null_arg.fetch_add(1, std::memory_order_relaxed);
    } else if (!safe_read_bytes(reinterpret_cast<uintptr_t>(job_arg) + 0x38,
                                &vparms, sizeof(vparms)) || vparms == 0) {
        g_ccf_read_fail.fetch_add(1, std::memory_order_relaxed);
        vparms = 0;
    } else if (viewsrc_is_known_view_parms(vparms)) {
        g_ccf_same_object.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_ccf_other_object.fetch_add(1, std::memory_order_relaxed);
    }
    int bucket = -1;
    alignas(16) float vp1270[16]{};
    if (vparms && safe_read_bytes(vparms + kViewParmsWorldVpOffset, vp1270, sizeof(vp1270))) {
        float cr[3], cu[3], cf[3], R[9];
        if (wrap::reference_basis(cr, cu, cf) && wrap::head_world_rotation(R)) {
            auto rot = [&](const float in[3], float out[3]) {
                for (int i = 0; i < 3; ++i)
                    out[i] = R[i * 3 + 0] * in[0] + R[i * 3 + 1] * in[1] + R[i * 3 + 2] * in[2];
            };
            float r1r[3], r1u[3], r1f[3], r2r[3], r2u[3], r2f[3];
            rot(cr, r1r); rot(cu, r1u); rot(cf, r1f);
            rot(r1r, r2r); rot(r1u, r2u); rot(r1f, r2f);
            bucket = 3;
            if (wrap::matches_reference_basis_signed(vp1270, cr, cu, cf))         bucket = 0;
            else if (wrap::matches_reference_basis_signed(vp1270, r1r, r1u, r1f)) bucket = 1;
            else if (wrap::matches_reference_basis_signed(vp1270, r2r, r2u, r2f)) bucket = 2;
            g_ccf_state[bucket].fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Gameplay-gated LOG (the counters above are not): a first-N dump that is
    // not gated spends its whole budget on the menu and loading screens, which
    // is a mistake this project has already paid for once.
    wrap::VpSnapshot gvp{};
    const bool gameplay = wrap::world_vp(gvp) && wrap::is_gameplay_view(gvp.m);
    if (gameplay && g_ccf_lines.fetch_add(1, std::memory_order_relaxed) < kCcfLinesTotal) {
        static const char* const kB[5] = {"CLEAN", "rot1", "rot2", "other", "noRef"};
        log::warn(std::format(
            "VIEWSRC/CCF jobArg={:#x} viewParms={:#x} known={} thread={} afterA1={} "
            "+0x1270 -> {}",
            reinterpret_cast<uintptr_t>(job_arg), vparms,
            viewsrc_is_known_view_parms(vparms) ? 1 : 0, GetCurrentThreadId(),
            g_vs_a1_calls_this_frame.load(std::memory_order_relaxed) != 0 ? 1 : 0,
            kB[bucket < 0 ? 4 : bucket]));
    }
    { FbTramp fbt_(FB_D_CULL_LIGHTS);
      if (real_cull_lights) real_cull_lights(job_arg); }
    // ===== DOES THE LIGHT LIST DIVERGE BETWEEN THE TWO PASSES? ============
    //
    // The hypothesis, and it must be measured rather than assumed - three
    // plausible mechanisms have already been wrong. Both passes run CullLights
    // (8359 calls over 4647 frames = 1.80/frame) and both write the SAME
    // cluster SSBOs. If the two passes compute the SAME list, the overwrite is
    // harmless; if they DIVERGE, whichever data the GPU reads is a race, and
    // that is the flicker and the blowouts.
    //
    // CullLights' own decompile writes the resulting light count to ctx+0x26C
    // ("if (lightCount < 1) *(arg1+0x26c) = 0"), so the count is readable
    // right here, after the real call, with no new machinery.
    //
    // THE PREDICTION THAT MAKES THIS FALSIFIABLE: the owner reports both
    // artifacts ONLY during movement and never at rest. So divergeMoving must
    // be >> divergeStill. If the two passes diverge just as often standing
    // still, the race is not what is being seen and this is the wrong tree -
    // which is exactly what the "still" bucket exists to be able to say.
    if (job_arg) {
        int32_t lights = 0;
        if (safe_read_bytes(reinterpret_cast<uintptr_t>(job_arg) + 0x26C, &lights, 4)) {
            const int side = inside_frame_root_double ? 1 : 0;
            const uint64_t f = presented_frame.load(std::memory_order_relaxed);
            if (g_cl_frame[side] != f) { g_cl_frame[side] = f; g_cl_count[side] = lights; }
            // Judge only when BOTH passes have reported for the same frame.
            if (side == 1 && g_cl_frame[0] == f) {
                float hy = 0.0f, hp = 0.0f, hr = 0.0f;
                (void)wrap::residual_head_angles(hy, hp, hr);
                const bool moving = std::fabs(hy - g_cl_last_yaw) > 0.0015f;  // ~0.09 deg/frame
                g_cl_last_yaw = hy;
                const bool diverged = g_cl_count[0] != lights;
                if (moving) {
                    g_cl_pairs_moving.fetch_add(1, std::memory_order_relaxed);
                    if (diverged) g_cl_diverge_moving.fetch_add(1, std::memory_order_relaxed);
                } else {
                    g_cl_pairs_still.fetch_add(1, std::memory_order_relaxed);
                    if (diverged) g_cl_diverge_still.fetch_add(1, std::memory_order_relaxed);
                }
                if (diverged) {
                    const int32_t d = lights - g_cl_count[0];
                    int32_t worst = g_cl_worst_delta.load(std::memory_order_relaxed);
                    if (std::abs(d) > std::abs(worst))
                        g_cl_worst_delta.store(d, std::memory_order_relaxed);
                }
            }
        }
    }
}


void __fastcall hook_build_view_matrices(void* view_parms, void* rect, uint64_t alt_scale) {
    FbDoom fbd_(FB_D_BUILD_VIEW_MATRICES);
    g_vs_calls.fetch_add(1, std::memory_order_relaxed);
    const uint32_t in_frame = g_vs_calls_this_frame.fetch_add(1, std::memory_order_relaxed);
    if ((alt_scale & 0xFFu) != 0) g_vs_alt_scale.fetch_add(1, std::memory_order_relaxed);
    // Ordering vs A1, read BEFORE the real call. The A1 counter is bumped in
    // hook_render_gather_prepare and cleared at the present - the real frame
    // boundary, not gather-prepare, which the FREQHIGH clears already learned
    // the hard way.
    const bool before_a1 = g_vs_a1_calls_this_frame.load(std::memory_order_relaxed) == 0;
    if (before_a1) g_vs_before_a1.fetch_add(1, std::memory_order_relaxed);
    else           g_vs_after_a1.fetch_add(1, std::memory_order_relaxed);

    const uintptr_t vp_addr = reinterpret_cast<uintptr_t>(view_parms);
    // Snapshot everything we want BEFORE handing control to DOOM, except the
    // derived matrix, which does not exist until the real call has run.
    alignas(16) float axis[9]{};
    int32_t rc[4]{};
    uint32_t identity_flag = 0;
    bool pre_ok = false;
    if (vp_addr == 0) {
        g_vs_null_arg.fetch_add(1, std::memory_order_relaxed);
    } else {
        pre_ok = safe_read_bytes(vp_addr + kViewParmsViewaxisOffset, axis, sizeof(axis));
        if (!pre_ok) g_vs_read_fail.fetch_add(1, std::memory_order_relaxed);
        // ⚠ A BYTE, NOT A DWORD. The step-2 build read four bytes here and
        // counted them non-zero, which made identityView read 6816 of 6816 and
        // said the view matrix was always identity - the opposite of the
        // truth. DOOM's own test is `if (*(viewParms+0x9B8) == '\0')` on the
        // +0x8E0 copy of this field, i.e. one byte. The raw per-call lines in
        // that run showed 0xffffff00, low byte 0, which is what gave the real
        // answer. Counter and log now both read the byte DOOM reads.
        uint8_t identity_byte = 0;
        if (!safe_read_bytes(vp_addr + kViewParmsIdentityFlagOffset,
                &identity_byte, sizeof(identity_byte)))
            identity_flag = 0xFFFFFFFFu;  // read failed - distinguishable from a real 0
        else {
            identity_flag = identity_byte;
            if (identity_byte != 0)
                g_vs_identity_view.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (rect && !safe_read_bytes(reinterpret_cast<uintptr_t>(rect), rc, sizeof(rc)))
        rc[0] = rc[1] = rc[2] = rc[3] = -1;

    {
        FbTramp fbt_(FB_D_BUILD_VIEW_MATRICES);
        if (real_build_view_matrices) real_build_view_matrices(view_parms, rect, alt_scale);
    }

    if (!pre_ok) return;  // counted above; nothing further is readable
    // Axis stability, within a frame and across frames. FNV-1a over the nine
    // floats' bit patterns - an exact-equality hash, which is what "did this
    // value change" wants; no tolerance to tune and nothing to get wrong.
    uint64_t h = 1469598103934665603ull;
    {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(axis);
        for (size_t i = 0; i < sizeof(axis); ++i) { h ^= b[i]; h *= 1099511628211ull; }
    }
    g_vs_axis_hash.store(h, std::memory_order_relaxed);
    if (in_frame == 0) {
        g_vs_axis_hash_frame0.store(h, std::memory_order_relaxed);
        if (h == g_vs_axis_hash_prevframe.load(std::memory_order_relaxed))
            g_vs_frame_pipelined.fetch_add(1, std::memory_order_relaxed);
    } else if (h == g_vs_axis_hash_frame0.load(std::memory_order_relaxed)) {
        g_vs_axis_same_as_prev.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_vs_axis_diff_in_frame.fetch_add(1, std::memory_order_relaxed);
    }
    // Remember this object as one the producer built, so the cluster-job probe
    // below can say whether ComputeClustersFrustums reads THE SAME object. That
    // identity is the last inferential gap between "we write the producer" and
    // "the froxel binning is what changes"; leaving it to be argued from
    // disassembly is exactly the kind of unverified link this thread exists to
    // stop paying for.
    viewsrc_remember_view_parms(vp_addr);

    // The derived world VP, now that DOOM has built it. With the write armed
    // this is the CLEAN one - the write and the rebuild happen below, after
    // this read - so the meaning of the +0x1270 census is unchanged between
    // the two modes and the step-2 numbers stay comparable.
    alignas(16) float vp1270[16]{};
    if (!safe_read_bytes(vp_addr + kViewParmsWorldVpOffset, vp1270, sizeof(vp1270))) {
        g_vs_read_fail.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // ===== PRECULL: THE PRODUCER WRITE ====================================
    //
    // Ordering inside this hook, and every part of it is load-bearing:
    //
    //   1. real() has just run on the CLEAN axis, so viewParms+0x1270 above is
    //      the genuine un-rotated world VP - byte-for-byte what renderView
    //      +0xC44 held before Lever A ever touched it (step 2: 2271/2271).
    //   2. We publish THAT as the clean reference basis and head delta. This
    //      is not decoration: once the axis is rotated, +0xC44 becomes the
    //      rotated camera, and if Lever A went on publishing it as "clean"
    //      the head rotation would be measured against an already-rotated
    //      frame and would feed back on itself. Publishing here instead also
    //      removes the one-frame staleness head_world_rotation_scaled
    //      documents, because this basis is THIS frame's and is published
    //      before R is built from it.
    //   3. Rotate the axis and call real() AGAIN. 0x1830B80 is a pure function
    //      of (object, rect, flag) - it re-copies [0..0x848] to +0x8E0 and
    //      recomputes every matrix, with no allocation, no globals and no job
    //      dispatch - so a second call with a modified axis leaves exactly the
    //      state the engine would have had if the axis had been that value all
    //      along. That is why this is a persistent write and not a
    //      write-then-restore: nothing is ever put back (FORBIDDEN #2).
    const bool precull = wrap::precull_active() && wrap::armed();
    bool rotated_here = false;
    alignas(16) float vp_post[16]{};
    if (precull) {
        // ONE publisher per mode. Lever A's publish is suppressed while this
        // is armed (see apply_lever_a_mutation), so exactly one site defines
        // the clean camera and there is no ordering race between them.
        if (in_frame == 0 && (alt_scale & 0xFFu) == 0) {
            wrap::publish_reference_basis(vp1270);
            wrap::publish_head_delta(vp1270);
            g_pc_publish_src.store(vp_addr, std::memory_order_relaxed);
            g_pc_published.fetch_add(1, std::memory_order_relaxed);
        }
        g_pc_attempts.fetch_add(1, std::memory_order_relaxed);
        float Rw[9];
        float hy = 0.0f, hp = 0.0f, hr = 0.0f;
        float bx[3], by[3], bz[3];
        // Entry-axis sanity FIRST: rotating a non-orthonormal basis produces a
        // skewed camera that would look like a subtle rendering fault rather
        // than a bad write.
        auto len3 = [](const float* v) {
            return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        };
        auto dot3 = [](const float* a, const float* b) {
            return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        };
        bool axis_ok = true;
        for (int i = 0; i < 9; ++i) if (!std::isfinite(axis[i])) axis_ok = false;
        for (int r = 0; r < 3 && axis_ok; ++r)
            if (std::fabs(len3(axis + r * 3) - 1.0f) > 1e-3f) axis_ok = false;
        if (axis_ok && (std::fabs(dot3(axis + 0, axis + 3)) > 1e-3f ||
                        std::fabs(dot3(axis + 0, axis + 6)) > 1e-3f ||
                        std::fabs(dot3(axis + 3, axis + 6)) > 1e-3f))
            axis_ok = false;
        bool already_ours = false;
        for (uint32_t i = 0; i < kPrecullWrittenRing; ++i)
            if (g_pc_written_hash[i].load(std::memory_order_relaxed) == h) already_ours = true;
        if (already_ours) {
            g_pc_double_guard.fetch_add(1, std::memory_order_relaxed);
        } else if (!axis_ok) {
            g_pc_axis_bad.fetch_add(1, std::memory_order_relaxed); wrap::note_head_rotation_refused();
        } else if (!wrap::reference_basis(bx, by, bz)) {
            g_pc_no_basis.fetch_add(1, std::memory_order_relaxed); wrap::note_head_rotation_refused();
        } else if (!wrap::residual_head_angles(hy, hp, hr) || !wrap::head_world_rotation(Rw)) {
            g_pc_no_head.fetch_add(1, std::memory_order_relaxed); wrap::note_head_rotation_refused();
        } else {
            alignas(16) float axis2[9]{};
            for (int r = 0; r < 3; ++r)
                for (int i = 0; i < 3; ++i)
                    axis2[r * 3 + i] = Rw[i * 3 + 0] * axis[r * 3 + 0] +
                                       Rw[i * 3 + 1] * axis[r * 3 + 1] +
                                       Rw[i * 3 + 2] * axis[r * 3 + 2];
            // POST on what we are about to write, BEFORE it goes anywhere.
            bool post_ok = true;
            for (int i = 0; i < 9; ++i) if (!std::isfinite(axis2[i])) post_ok = false;
            for (int r = 0; r < 3 && post_ok; ++r)
                if (std::fabs(len3(axis2 + r * 3) - 1.0f) > 1e-3f) post_ok = false;
            if (post_ok && (std::fabs(dot3(axis2 + 0, axis2 + 3)) > 1e-3f ||
                            std::fabs(dot3(axis2 + 0, axis2 + 6)) > 1e-3f ||
                            std::fabs(dot3(axis2 + 3, axis2 + 6)) > 1e-3f))
                post_ok = false;
            if (!post_ok) {
                g_pc_post_bad.fetch_add(1, std::memory_order_relaxed); wrap::note_head_rotation_refused();
            } else if (std::fabs(hy) > 0.0087f &&
                       dot3(axis + 0, axis2 + 0) > 0.99996f) {
                // R came back as (near) identity while a real angle was
                // commanded - an unpublished basis or zeroed angles. That
                // failure writes a correct-looking value that does nothing,
                // which is the hardest kind to spot from the picture.
                g_pc_angle_bad.fetch_add(1, std::memory_order_relaxed); wrap::note_head_rotation_refused();
            } else if (!safe_write_bytes(vp_addr + kViewParmsViewaxisOffset,
                                         axis2, sizeof(axis2))) {
                g_pc_write_fail.fetch_add(1, std::memory_order_relaxed); wrap::note_head_rotation_refused();
            } else {
                // STRUCTURAL POST: the bytes must actually be there. A write
                // that reads back proves little on its own, so the real proof
                // is the rebuilt +0x1270 below - this only separates "the
                // store failed" from "the store landed and changed nothing".
                alignas(16) float verify[9]{};
                if (!safe_read_bytes(vp_addr + kViewParmsViewaxisOffset,
                                     verify, sizeof(verify)) ||
                    std::memcmp(verify, axis2, sizeof(axis2)) != 0) {
                    g_pc_readback_bad.fetch_add(1, std::memory_order_relaxed); wrap::note_head_rotation_refused();
                } else {
                    uint64_t h2 = 1469598103934665603ull;
                    const uint8_t* b2 = reinterpret_cast<const uint8_t*>(axis2);
                    for (size_t i = 0; i < sizeof(axis2); ++i) {
                        h2 ^= b2[i]; h2 *= 1099511628211ull;
                    }
                    const uint32_t slot =
                        g_pc_written_next.fetch_add(1, std::memory_order_relaxed) %
                        kPrecullWrittenRing;
                    g_pc_written_hash[slot].store(h2, std::memory_order_relaxed);
                    {
                        FbTramp fbt2_(FB_D_BUILD_VIEW_MATRICES);
                        if (real_build_view_matrices)
                            real_build_view_matrices(view_parms, rect, alt_scale);
                    }
                    g_pc_written.fetch_add(1, std::memory_order_relaxed);
                    rotated_here = true;
                    // ⚠ FEEDS THE WRAPAROUND SELF-TEST, and it must.
                    // That self-test aborts the run when head_rotations_applied()
                    // is zero, on the grounds that the declared pose tracks the
                    // head while the content does not - the welded-to-your-face
                    // failure. Under precull, Lever A's rotation is suppressed by
                    // design, so the counter stayed at zero and the 09:31 run
                    // printed ***ABORT THIS RUN*** over a build that was working
                    // perfectly. A self-test that cries wolf costs a run the first
                    // time it is believed and costs every run after that if it is
                    // not. The counter means "the head rotation reached DOOM's
                    // camera", which is exactly what this write does - so it is
                    // incremented here rather than the test being special-cased,
                    // and it stays true in both arms.
                    wrap::note_head_rotation_applied();
                    if (safe_read_bytes(vp_addr + kViewParmsWorldVpOffset,
                                        vp_post, sizeof(vp_post))) {
                        auto rot = [&](const float in[3], float out[3]) {
                            for (int i = 0; i < 3; ++i)
                                out[i] = Rw[i * 3 + 0] * in[0] + Rw[i * 3 + 1] * in[1] +
                                         Rw[i * 3 + 2] * in[2];
                            };
                        float p1[3], p2[3], p3[3], q1[3], q2[3], q3[3];
                        rot(bx, p1); rot(by, p2); rot(bz, p3);
                        rot(p1, q1); rot(p2, q2); rot(p3, q3);
                        int pb = 3;
                        if (wrap::matches_reference_basis_signed(vp_post, bx, by, bz))    pb = 0;
                        else if (wrap::matches_reference_basis_signed(vp_post, p1, p2, p3)) pb = 1;
                        else if (wrap::matches_reference_basis_signed(vp_post, q1, q2, q3)) pb = 2;
                        g_pc_post_state[pb].fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        }
    }


    int bucket = -1;
    float cr[3], cu[3], cf[3], R[9];
    if (!wrap::reference_basis(cr, cu, cf)) {
        g_vs_no_ref.fetch_add(1, std::memory_order_relaxed);
    } else if (!wrap::head_world_rotation(R)) {
        g_vs_no_head.fetch_add(1, std::memory_order_relaxed);
    } else {
        auto rot = [&](const float in[3], float out[3]) {
            for (int i = 0; i < 3; ++i)
                out[i] = R[i * 3 + 0] * in[0] + R[i * 3 + 1] * in[1] + R[i * 3 + 2] * in[2];
        };
        float r1r[3], r1u[3], r1f[3], r2r[3], r2u[3], r2f[3];
        rot(cr, r1r); rot(cu, r1u); rot(cf, r1f);
        rot(r1r, r2r); rot(r1u, r2u); rot(r1f, r2f);
        bucket = 3;
        if (wrap::matches_reference_basis_signed(vp1270, cr, cu, cf))            bucket = 0;
        else if (wrap::matches_reference_basis_signed(vp1270, r1r, r1u, r1f))    bucket = 1;
        else if (wrap::matches_reference_basis_signed(vp1270, r2r, r2u, r2f))    bucket = 2;
        g_vs_state[bucket].fetch_add(1, std::memory_order_relaxed);
    }

    // Capped, gameplay-gated per-call detail. The gate is on the LOG only -
    // every counter above ran unconditionally, so a menu-only session still
    // reports its call rate instead of looking like a dead hook.
    wrap::VpSnapshot gvp{};
    const bool gameplay = wrap::world_vp(gvp) && wrap::is_gameplay_view(gvp.m);
    if (!gameplay) { g_vs_not_gameplay.fetch_add(1, std::memory_order_relaxed); return; }
    if (in_frame >= kViewSrcLinesPerFrame) return;
    if (g_vs_lines.fetch_add(1, std::memory_order_relaxed) >= kViewSrcLinesTotal) return;
    static const char* const kBucket[5] = {"CLEAN", "rot1", "rot2", "other", "noRef"};
    log::warn(std::format(
        "VIEWSRC call#{} viewParms={:#x} alt={} rect=({},{},{},{}) identityFlag(byte)={} "
        "thread={} beforeA1={} axisHash={:#x} precullWrote={}\n"
        "  axisFwd=({:.4f} {:.4f} {:.4f}) axisLeft=({:.4f} {:.4f} {:.4f}) "
        "axisUp=({:.4f} {:.4f} {:.4f})\n"
        "  +0x1270 row0=({:.4f} {:.4f} {:.4f} {:.4f}) row2=({:.4f} {:.4f} {:.4f} {:.4f}) "
        "-> {}",
        in_frame, vp_addr, static_cast<unsigned>(alt_scale & 0xFFu),
        rc[0], rc[1], rc[2], rc[3], identity_flag, GetCurrentThreadId(),
        before_a1 ? 1 : 0, h, rotated_here ? 1 : 0,
        axis[0], axis[1], axis[2], axis[3], axis[4], axis[5], axis[6], axis[7], axis[8],
        vp1270[0], vp1270[1], vp1270[2], vp1270[3],
        vp1270[8], vp1270[9], vp1270[10], vp1270[11],
        kBucket[bucket < 0 ? 4 : bucket]));
}

void __fastcall hook_setup_view_gui(void* arg1) {
    FbDoom fbd_(FB_D_SETUP_VIEW_GUI);
    // Above the census entry deliberately: at these rungs the claim being
    // tested is that the doubled call did not set up a view at all, and a
    // census row saying it did would contradict the experiment. The census
    // showing dbl e=0 here is how the rung is verified to have engaged.
    if (doubled_skip_setup_gui_now()) return;
    // Unconditional and above every gate below: the census needs "this hook
    // fired" separated from "this hook wrote", or a zero write count on one
    // side cannot be told from the hook never running on that side - which is
    // precisely the eye-to-eye asymmetry this run exists to read.
    vwc_entry(kVwsLeverVGui);
    log_world_vp_at("SetupView_gui ENTER");
    const int mode = lever_v_mode.load(std::memory_order_relaxed);
    const float unified = g_stereo_offset.load(std::memory_order_relaxed);
    // Never apply the stereo offset to the GUI/viewmodel view while inside a
    // doubled frame root. SetupView_gui is called DIRECTLY by the frame root,
    // not through JobDispatch, so Stage 2b's job filter cannot reach it - the
    // doubled call was shifting the GUI MVP at renderer+0x4955A4, which is
    // persistent renderer state that eye 0's weapon draw depends on. At a 3.0
    // uu offset that is enough to push the viewmodel out of frame, which is
    // the reported "left eye has black where the gun should be".
    //
    // Eye 1 is world geometry only and wants no GUI pass at all, so suppressing
    // the mutation here costs nothing and stops the doubled call leaking into
    // DOOM's own frame.
    // Suppressed only in LIVE stereo mode, where eye 0 is DOOM's own frame
    // and a shifted GUI MVP pushed the weapon out of it. In A/B mode we are
    // comparing offscreen dumps and eye 0 does not matter, so Lever V must
    // run - it is one of the three write sites the confirmed-working
    // "PERFECT!!!!" configuration used, and the one whose MVP bake at
    // renderer+0x4955A4 the handoff credits with covering "every downstream
    // weapon/HUD-3D/sky/particle draw". Our doubled call has been running
    // with one of those three sites active, which is a direct candidate for
    // the near geometry that refuses to move.
    // CONFIRMED 2026-08-16: Lever V now runs in BOTH modes. It is the write
    // site that covers near geometry, measured directly - with it suppressed
    // the interior walls changed 0.0% of pixels under a 2000 uu shift, with
    // it active 94.4%, and the gun went 0.5% -> 94.9%. Suppressing it in live
    // mode would re-break exactly the coverage it provides. If the weapon
    // misbehaves in eye 0 again, fix that by scoping the restore, not by
    // disabling this.
    // SUPPRESSION IS BACK OFF, and this time on evidence rather than on the
    // 2026-08-16 comment's say-so. It was switched on for one run and the
    // interior walls stopped moving with the separation while only the distant
    // canyon still shifted - the near-geometry coverage this write site
    // provides is real. The gun is now handled by the POST-BAKE RESTORE below,
    // which keeps the write and takes it back after the bake has consumed it.
    // See lever_v_post_bake_restore for the seam that makes that possible.
    const bool suppress_for_live_eye0 = false;
    // Scoped to LIVE symmetric stereo only. A/B mode - where the 94.4%
    // near-geometry coverage figure was measured, and where eye 0 is an
    // offscreen dump nobody looks at - keeps the behaviour it was measured
    // under.
    const bool live_symmetric =
        frame_root_full_frame.load(std::memory_order_relaxed) &&
        frame_root_double_enabled.load(std::memory_order_relaxed) &&
        !frame_root_double_ab.load(std::memory_order_relaxed);
    const bool publish_for_gun_unshift =
        kharvox::native::allowLegacyGunUnshift(kharvox::native::requested(),
            gun_unshift_enabled.load(std::memory_order_relaxed),live_symmetric);
    // wrap::head_look_active() joins this guard for the same reason it joined
    // Lever A's: Phase 3 needs this site to run with no stereo offset and no
    // lever mode. Without it the near-geometry/viewmodel view is never written
    // and the gun stays welded to the head.
    // PRECULL REPLACES THIS SITE'S ROTATION - AND ONLY ITS ROTATION.
    //
    // ⚠ Same trap as Lever A, and it is worth spelling out because the first
    // version of this edit fell into it: `active` does not only gate the head
    // rotation. It also gates apply_translation(m, unified), which is the
    // per-eye stereo offset for the GUI/viewmodel view. Under PageUp - every
    // headset run - killing `active` would have flattened the weapon and HUD
    // to a single eye position while the world stayed stereo.
    //
    // What precull does replace: Lever V rotates source_ctx+0x11F0, the view
    // matrix, and 0x1830B80 BUILDS +0x11F0 from the axis this thread rotates
    // (0x2C43C0(org=+0x940, axis=+0x94C, out=+0x11F0)). So under precull it
    // arrives already rotated and a second rotation here would double it for
    // near geometry and the viewmodel specifically - exactly the population
    // this run has to judge. The rotation is therefore suppressed through the
    // flag that already exists for it, `v_rot_on` below, and nothing else
    // changes. Counted, not silent.
    const bool precull_owns_camera = wrap::precull_active() && wrap::armed();
    if (precull_owns_camera) {
        g_lever_v_precull_suppressed.fetch_add(1, std::memory_order_relaxed);
        // Close the publish path's one inference, on the real call only.
        // This function copies its own viewParms+0x1270 into renderView+0xC44
        // (0x17B40B2), so its object is BY DEFINITION the one whose clean
        // matrix deserves to be published as the clean camera. If that is not
        // the object the producer published from, the label is on the wrong
        // matrix and the log has to say so.
        if (!inside_frame_root_double && arg1) {
            uintptr_t vparms_now = 0;
            if (safe_read_bytes(reinterpret_cast<uintptr_t>(arg1) + 0x18,
                    &vparms_now, sizeof(vparms_now)) && vparms_now) {
                if (vparms_now == g_pc_publish_src.load(std::memory_order_relaxed))
                    g_pc_publish_obj_match.fetch_add(1, std::memory_order_relaxed);
                else
                    g_pc_publish_obj_mismatch.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    const bool active = (mode != static_cast<int>(LeverAMode::Off) || unified != 0.0f ||
                         wrap::head_look_active()) && !suppress_for_live_eye0;
    if (active && arg1) {
        uintptr_t source_ctx = 0;
        if (safe_read_bytes(reinterpret_cast<uintptr_t>(arg1) + 0x18,
                &source_ctx, sizeof(source_ctx)) && source_ctx != 0) {
            const uintptr_t view_addr = source_ctx + 0x11F0;
            alignas(16) float m[16]{};
            alignas(16) float m_before[16]{};
            if (safe_read_bytes(view_addr, m, sizeof(m))) {
                std::memcpy(m_before, m, sizeof(m));
                // Snapshot the PRE-mutation matrix so the doubled call can put
                // DOOM's own state back byte-for-byte when it returns. Only
                // inside the doubled call - the real call's mutation is DOOM's
                // own frame and must stand. First writer wins, so a nested or
                // repeated GUI setup within one doubled call cannot overwrite
                // the genuine original with an already-shifted one.
                if (inside_frame_root_double && !lever_v_restore_valid) {
                    lever_v_restore_addr = view_addr;
                    std::memcpy(lever_v_restore_matrix, m, sizeof(m));
                    lever_v_restore_valid = true;
                }
                // REBASE THE DOUBLED CALL ONTO THE CLEAN VIEW (2026-08-22).
                // THIS IS THE BUG THE CAPTURE AND THE CENSUS BOTH POINT AT.
                //
                // The snapshot above is taken INSIDE the doubled call, so `m`
                // still holds the value the REAL call left - already shifted by
                // -half. Applying the doubled call's +half to that CANCELS the
                // shift and lands on the centre instead of mirroring to the far
                // side. Lever A does not have this problem because renderView
                // is restored to DOOM's original before its doubled write, which
                // is why its two sides straddle the centre correctly.
                //
                // MEASURED, census at 'separation 12.0 uu':
                //   LeverA  real 4673.485 -> 4678.856   dbl 4673.485 -> 4668.114
                //           (starts from the CENTRE, lands symmetric +/-5.371)
                //   LeverV  real 4939.415 -> 4945.415   dbl 4945.415 -> 4939.415
                //           (starts from the REAL'S value, lands back on centre)
                //
                // CORROBORATED INDEPENDENTLY by doomvr_frame15332.rdc: comparing
                // each eye's per-draw constants against their own 0.0-separation
                // values, 28 of 33 constant groups have eye 1 at centre +0.000
                // while eye 0 moves. Only the 3 groups fed by Lever A are
                // symmetric. That is the same defect seen from the other end.
                //
                // So: rebase onto the clean pre-real matrix, then shift. The
                // restore below is deliberately UNCHANGED - it still puts back
                // the value that was there before the doubled call, so EYE 0
                // SEES IDENTICAL STATE and cannot regress from this.
                if (!inside_frame_root_double) {
                    std::memcpy(lever_v_clean_matrix, m, sizeof(m));
                    lever_v_clean_valid = true;
                } else if (unified != 0.0f) {
                    if (lever_v_clean_valid) {
                        std::memcpy(m, lever_v_clean_matrix, sizeof(m));
                        lever_v_clean_rebased.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        // Counted separately, never silently: without the clean
                        // basis this falls back to the OLD cancelling behaviour,
                        // and a run where that happened must not be read as a
                        // test of the fix.
                        const uint32_t miss =
                            lever_v_clean_missing.fetch_add(1, std::memory_order_relaxed);
                        if (miss < 5) {
                            log::warn(std::format(
                                "LeverV REBASE UNAVAILABLE #{}: no clean pre-real snapshot for "
                                "view {:#x} this frame, so the doubled call falls back to "
                                "shifting the REAL call's already-shifted matrix - the old "
                                "cancelling behaviour. Eye 1 will sit at the centre for this "
                                "view. NOT a test of the fix.",
                                miss, view_addr));
                        }
                    }
                }
                if (lever_v_probe_pending.exchange(false, std::memory_order_relaxed)) {
                    log::warn(std::format(
                        "LeverV probe: SetupView arg1={} source_ctx={:#x} view_addr={:#x}",
                        arg1, source_ctx, view_addr));
                    log::warn(std::format(
                        "LeverV probe: view_before  row0=({:.4f} {:.4f} {:.4f} {:.4f}) "
                        "row1=({:.4f} {:.4f} {:.4f} {:.4f}) row2=({:.4f} {:.4f} {:.4f} {:.4f}) "
                        "row3=({:.4f} {:.4f} {:.4f} {:.4f})",
                        m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
                        m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]));
                }
                // ===== PHASE 3: THE NEAR-GEOMETRY / VIEWMODEL VIEW =========
                //
                // THIS IS WHY THE GUN AND "PART OF THE SCENE" FOLLOW THE HEAD -
                // and it is the opposite of what I assumed. They follow the
                // head because they are NOT rotated.
                //
                // Everything drawn from the world camera (renderView+0xC44) is
                // rotated by the head, so it stays put in the world while the
                // camera turns - correct wraparound. Everything drawn from THIS
                // view is not, so it keeps projecting to the same place on the
                // display - and since the whole image is submitted at a
                // head-tracked pose, "same place on the display" IS "welded to
                // your face". Leaving this view alone does not hold the gun
                // still in the world; it staples it to your eyes.
                //
                // WHAT THIS VIEW COVERS, measured in this repo on 2026-08-16
                // and recorded in the comment block above: with Lever V
                // suppressed the interior walls changed 0.0% of pixels under a
                // 2000 uu shift and the gun 0.5%; with it active, walls 94.4%
                // and gun 94.9%. Near geometry AND the weapon. That is exactly
                // the owner's "part of the scene, including the body/gun".
                //
                // Rotated by the SAME head angles as the world camera, so the
                // two agree and the scene is one rigid world again.
                if (wrap::head_look_active()) {
                    // RAW attempts FIRST, before any predicate - §8's rule.
                    gui_head_attempts.fetch_add(1, std::memory_order_relaxed);
                    float hy = 0.0f, hp = 0.0f, hr = 0.0f;
                    const bool have_angles = wrap::residual_head_angles(hy, hp, hr);
                    // ===== LAYOUT DUMPS, RE-SCOPED (2026-08-26). The old four
                    // lines all fired within seconds of arming - i.e. in the
                    // MENU - and the "fixed axis swizzle, not a camera" verdict
                    // was drawn from menu frames where the camera IS identity
                    // at the origin. (The stereo census's m3=17904 at this same
                    // address is world-scale - the gameplay matrix is a real
                    // view.) Two budgets now: four lines gated on GAMEPLAY, and
                    // four more gated on gameplay with the head turned >20 deg,
                    // so one run's log settles whether the rows track the body
                    // and what the sign convention is - no owner action needed.
                    static std::atomic_uint32_t gui_layout_lines{0};
                    static std::atomic_uint32_t gui_layout_turned_lines{0};
                    bool gameplay_now = false;
                    {
                        wrap::VpSnapshot vp;
                        if (wrap::world_vp(vp)) gameplay_now = wrap::is_gameplay_view(vp.m);
                    }
                    const bool turned = have_angles && std::fabs(hy) > 0.35f;
                    const bool dump_plain = gameplay_now &&
                        gui_layout_lines.fetch_add(1, std::memory_order_relaxed) < 4;
                    const bool dump_turned = gameplay_now && turned && !dump_plain &&
                        gui_layout_turned_lines.fetch_add(1, std::memory_order_relaxed) < 4;
                    if (dump_plain || dump_turned) {
                        log::warn(std::format(
                            "GuiViewLayout[{}] ctx={:#x} yaw={:.2f} rowLengths=({:.4f} {:.4f} "
                            "{:.4f} {:.4f}) | "
                            "row0=({:.4f} {:.4f} {:.4f} {:.4f}) row1=({:.4f} {:.4f} {:.4f} {:.4f}) "
                            "row2=({:.4f} {:.4f} {:.4f} {:.4f}) row3=({:.4f} {:.4f} {:.4f} {:.4f})\n"
                            "READING IT: GAMEPLAY-GATED, unlike run 8's menu-era lines. If rows "
                            "0-2 here are the same fixed permutation at every yaw, the GUI view's "
                            "input space is body/camera-local; if they track the body's facing, "
                            "it is world-aligned. Either way vrot=world is correct by "
                            "construction - this dump exists to settle the record, not to gate "
                            "the write.",
                            dump_turned ? "turned" : "level", source_ctx, have_angles ? hy : 0.0f,
                            wrap::row_length(m, 0), wrap::row_length(m, 1),
                            wrap::row_length(m, 2), wrap::row_length(m, 3),
                            m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
                            m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]));
                    }
                    // ===== THE ROTATION, UN-GATED FROM matches_reference_basis.
                    //
                    // That gate was the WRONG CRITERION and it refused 100% of
                    // calls (13:43 run: guiView=0, refused 2605): it demanded
                    // that this matrix hold renderView's camera DIRECTIONS,
                    // but a view matrix in another sign/role convention - or a
                    // body-local one - carries the same camera without passing
                    // a direction match. Meanwhile the same matrix demonstrably
                    // CARRIES the gun and the near geometry: the 2026-08-16
                    // measurement moved the gun 0.5%->94.9% of pixels through a
                    // translation written HERE, and the weapon MVP is rebuilt
                    // per draw as (ctx+0x1170)*(ctx+0x11F0)*model (Lever G
                    // decompile). "Not a camera" confused "the camera's
                    // orientation is not stored here in renderView's
                    // convention" with "rotation cannot be injected here".
                    //
                    // vrot=world applies the SAME world-space rotation the
                    // renderView pair received this frame - recovered from the
                    // published clean basis, so it is calibrated by the same
                    // headaxes bits and needs no new sign key - about the
                    // camera position this matrix itself encodes. It never
                    // interprets which row is which axis, so every convention
                    // question this site has raised is out of its blast
                    // radius. The layout gate inside it is structural
                    // (affine + orthonormal rows) and every refusal is counted
                    // by cause below.
                    // ===== CTRL+Up: IS LEVER V IN THE GUN'S PATH AT ALL? ====
                    //
                    // Four runs have now tried to fix the weapon through this
                    // site and every one produced no visible change. Before a
                    // fifth aims a new write here, ONE keypress settles
                    // whether the weapon is downstream of this matrix at all.
                    // `vrot=` is config-only and needs a relaunch, which is
                    // why this has never been asked directly.
                    //
                    // Forcing Off rather than skipping the block: the Off
                    // branch already counts attempts and writes nothing, and
                    // the publish for the gun unshift still happens below - so
                    // the arm changes exactly one thing, the rotation, and
                    // every counter stays alive to prove the site still ran.
                    // ===== SLOT 1: HALF ROTATION AT LEVER V ==================
                    //
                    // THIS FOLLOWS DIRECTLY FROM THE OWNER'S OWN TWO RESULTS,
                    // not from another discriminator:
                    //   CTRL+Up  - Lever V rotation OFF welds the gun AND a large
                    //              scenery population to the head.
                    //   CTRL+Ins - +30 deg EXTRA here moved the gun AND "every
                    //              other thing that isn't nailed down".
                    // So that whole population's on-screen rotation is driven by
                    // THIS angle, and it currently lands at twice the world's
                    // rate. If the doubling is multiplicative in Lever V's own
                    // rotation, feeding HALF the angle puts it at exactly 1x -
                    // the same rate as the walls, which is correct.
                    //
                    // Crude as a shipping fix, but decisive as a test, and it
                    // cannot break anything the +30 deg arm did not already
                    // prove reversible. Three outcomes, all informative:
                    //   gun correct, scenery correct -> the doubling is in this
                    //     path and proportional; find why it is applied twice.
                    //   gun correct, scenery now under-rotated -> two populations
                    //     share this write and need different treatment.
                    //   no change -> the doubling is not proportional to this
                    //     angle, and this whole path is off the hook.
                    // !precull_owns_camera: +0x11F0 is built FROM the axis the
                    // producer write rotates, so it is already rotated by the
                    // time this site sees it. The translation below still runs.
                    const bool v_rot_on =
                        lever_v_rot_enabled.load(std::memory_order_relaxed) &&
                        !precull_owns_camera;
                    if (!v_rot_on) gui_head_rot_suppressed.fetch_add(1, std::memory_order_relaxed);
                    const wrap::VrotMode vmode =
                        v_rot_on ? wrap::vrot_mode() : wrap::VrotMode::Off;
                    if (vmode == wrap::VrotMode::World) {
                        float R[9];
                        // SLOT 1 feeds HALF the angle. Must be applied HERE: the
                        // World path builds R from the published angles inside
                        // wrap, so scaling the local hy above reaches nothing.
                        // RETIRED 2026-08-27: half-angle experiment done; slot
                        // 1 reassigned. Always full angle.
                        if (!wrap::head_world_rotation_scaled(R, 1.0f)) {
                            gui_head_norot.fetch_add(1, std::memory_order_relaxed);
                            gui_head_refusals.fetch_add(1, std::memory_order_relaxed);
                        } else if (wrap::apply_world_rotation_affine_view(m, R)) {
                            gui_head_rotations.fetch_add(1, std::memory_order_relaxed);
                        } else {
                            const uint64_t n = gui_head_shape_refusals.fetch_add(
                                1, std::memory_order_relaxed);
                            gui_head_refusals.fetch_add(1, std::memory_order_relaxed);
                            if (n < 3) {
                                log::warn(std::format(
                                    "GuiRot SHAPE REFUSED #{} at ctx={:#x}: not the affine "
                                    "orthonormal layout. row0=({:.4f} {:.4f} {:.4f} {:.4f}) "
                                    "row1=({:.4f} {:.4f} {:.4f} {:.4f}) row2=({:.4f} {:.4f} "
                                    "{:.4f} {:.4f}) row3=({:.4f} {:.4f} {:.4f} {:.4f}). If this "
                                    "climbs with attempts, the gameplay GUI view is not the "
                                    "layout the 11:02 and census evidence measured, and the dump "
                                    "above says what it actually is.",
                                    n, source_ctx,
                                    m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
                                    m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]));
                            }
                        }
                    } else if (vmode == wrap::VrotMode::Local) {
                        // The fallback arm: the pre-2026-08-26 own-rows mix.
                        // Correct ONLY if the gameplay matrix's rows are the
                        // body camera's axes in renderView's sign convention -
                        // kept so the two frames can be A/B'd from the switch
                        // file with no rebuild if world-mode misbehaves.
                        if (have_angles &&
                            wrap::apply_head_rotation(m, hy, hp, hr, wrap::head_axes())) {
                            gui_head_rotations.fetch_add(1, std::memory_order_relaxed);
                        } else {
                            gui_head_refusals.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    // vmode == Off: attempts counted, nothing written - the
                    // exact pre-change behaviour, so an armed file without a
                    // vrot key is not silently a new arm.
                }
                // Rotated but NOT yet shifted. This, not DOOM's raw matrix, is
                // what "unshifted" has to mean for the gun once the head is
                // driving the camera - see the publish below.
                alignas(16) float m_head[16]{};
                std::memcpy(m_head, m, sizeof(m_head));
                // Unified stereo offset takes priority; legacy mode enum
                // is only consulted when unified is zero.
                if (unified != 0.0f) {
                    apply_translation(m, unified);
                } else {
                    switch (static_cast<LeverAMode>(mode)) {
                        case LeverAMode::Yaw90Deg:
                            apply_yaw_rotation(m, kLeverAYawRadians); break;
                        case LeverAMode::TranslateLateral5:
                            apply_translation(m, kLeverATranslateOffsetSmall); break;
                        case LeverAMode::TranslateLateral100:
                            apply_translation(m, kLeverATranslateOffsetLarge); break;
                        default: break;
                    }
                }
                // ===== SLOT 3: DOES THE GUN FOLLOW THIS WRITE? ===========
                //
                // ⚠ MUST BE THE LAST THING BEFORE THE WRITE. The first version
                // of this sat 22 lines further down, AFTER safe_write_bytes -
                // so the extra rotation was applied to a local that had
                // already been copied into the game, its counter climbed to
                // 212, and the run reported "no change" for a probe whose
                // value never reached memory. A counter next to a mutation
                // proves the mutation RAN, not that it LANDED.
                //
                // An unmissable +30 degrees on top of whatever Lever V just
                // did, at the site that demonstrably works, rather than at
                // draw time where every previous swap was already too late.
                // The walls are known to follow this write: if the GUN swings
                // an extra 30 degrees with them it is downstream of this
                // matrix; if the walls move and the gun does not, it is not.
                // RETIRED 2026-08-27: the +30 degree Lever-V probe served its
                // purpose; slot 3 reassigned. Hard false.
                safe_write_bytes(view_addr, m, sizeof(m));
                vwc_write(kVwsLeverVGui, view_addr, unified, m_before, m);
                // Publish the exact address and DOOM's own base matrix for the
                // gun unshift, which runs later in RenderGuiModels. Real call
                // only - see gun_unshift_publish for why the doubled call's
                // pre-write value is not the base and what publishing it broke.
                // ⚠ PUBLISH THE HEAD-ROTATED BASE, NOT DOOM'S RAW ONE.
                //
                // The gun unshift swaps this matrix in for the weapon draw in
                // RenderGuiModels so the weapon does not get pushed out of
                // frame by the stereo offset. Its job is to remove the STEREO
                // SHIFT - not the head rotation. Handing it m_before, which is
                // DOOM's unrotated matrix, would put the weapon back on the
                // body's camera for its own draw and re-weld it to the head,
                // undoing the fix above for the one object it is most about.
                //
                // m at this point is rotated by the head and not yet shifted,
                // which is exactly "unshifted" in the sense the unshift means.
                // CTRL+Delete flips which base goes out. See that handler for
                // the GunWindow measurement (ratio 1.005..1.014 - the window
                // carries R exactly once) that made the raw arm worth testing
                // against the standing comment above.
                // Stash what the post-bake patch needs. m_before is DOOM's own
                // unrotated view; view_addr-0x11F0 is the ctx whose +0x1170
                // holds the projection the bake multiplies by.
                g_bake_ctx = view_addr - 0x11F0;
                std::memcpy(g_bake_view_clean, m_before, sizeof(g_bake_view_clean));
                std::memcpy(g_bake_view_rot, m_head, sizeof(g_bake_view_rot));
                g_bake_have_inputs = true;
                // Lever V's own delta, for the bind-time correction. m_head is
                // this matrix with exactly one head rotation; m_before is DOOM's.
                if (wrap::head_look_active()) publish_gui_delta(m_before, m_head);
                if (publish_for_gun_unshift && !inside_frame_root_double) {
                    const bool raw = gun_base_raw.load(std::memory_order_relaxed);
                    gun_unshift_publish(
                        view_addr, unified,
                        (wrap::head_look_active() && !raw) ? m_head : m_before);
                }
                const uint32_t n = lever_v_probe_log_count.fetch_add(1, std::memory_order_relaxed);
                if (n < kMaxLeverVProbeLogLines) {
                    log::warn(std::format(
                        "LeverV wrote view matrix (unified={:.2f} legacy_mode={}, sample={}/{})",
                        unified, lever_a_mode_name(mode), n + 1, kMaxLeverVProbeLogLines));
                }
            }
        }
    }
    // No post-bake restore here: putting the clean matrix back at this point was
    // tested and behaved identically to never writing at all - walls and gun
    // both stopped moving. See gun_unshift_enabled for what that ruled out and
    // where the unshift went instead.
    // Invalidate whenever the REAL call did not publish, for either reason. A
    // publish left standing from an earlier frame would have RenderGuiModels
    // swapping in a matrix from a camera that has since moved - which looks
    // like a working fix right up until the player does. Keyed on the real call
    // only, to match the publish: the doubled call must find the real call's
    // publish still standing, not clear it.
    if (!inside_frame_root_double && (!active || !publish_for_gun_unshift))
        gun_unshift_invalidate();
    { FbTramp fbt_(FB_D_SETUP_VIEW_GUI); real_setup_view_gui(arg1); }
    // VIEWSRC copy proof, READ-ONLY. Static analysis says this very function
    // copies viewParms+0x1270 into renderView+0xC44 at DOOM+0x17B40B2. That is
    // the whole reason Lever A cannot reach the froxel binning, so it is worth
    // more as a measurement than as a disassembly claim. Both matrices are read
    // straight after the real call; equality here proves the copy live, and a
    // mismatch says the render view we track is not the one this call wrote.
    if (arg1) {
        uintptr_t vparms = 0;
        const uintptr_t rv_slot = captured_render_view_slot.load(std::memory_order_relaxed);
        uintptr_t rv = 0;
        if (safe_read_bytes(reinterpret_cast<uintptr_t>(arg1) + 0x18, &vparms, sizeof(vparms)) &&
            vparms && rv_slot && safe_read_bytes(rv_slot, &rv, sizeof(rv)) && rv) {
            alignas(16) float a[16]{};
            alignas(16) float b[16]{};
            if (safe_read_bytes(vparms + kViewParmsWorldVpOffset, a, sizeof(a)) &&
                safe_read_bytes(rv + 0xC44, b, sizeof(b))) {
                g_vs_c44_attempts.fetch_add(1, std::memory_order_relaxed);
                if (std::memcmp(a, b, sizeof(a)) == 0)
                    g_vs_c44_match.fetch_add(1, std::memory_order_relaxed);
                else
                    g_vs_c44_mismatch.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    gui_bake_verify_and_patch(arg1);
    log_world_vp_at("SetupView_gui EXIT");
}

// Lever W (2026-08-11). World-path equivalent of Lever V. Hooks world
// SetupView, mutates CameraSrc+0xC44 (world VP source) BEFORE the real
// SetupView runs, so the internal 6-destination copy loop propagates our
// shift to all 6 destinations coherently. Fixes the mid-terrain glitch
// caused by Lever A modifying only one of those destinations.
std::atomic_bool lever_w_probe_pending{false};
std::atomic_uint32_t lever_w_probe_log_count{0};
constexpr uint32_t kMaxLeverWProbeLogLines = 4;

// WorldVpTrace: F10-armed diagnostic that logs renderView+0xC44's value
// at every instrumented hook point until the log budget is exhausted.
// Purpose: identify WHICH upstream call actually WRITES the world VP that
// RenderOpaque reads via arg1+0x40. Whoever's fingerprints are on the
// change is our Lever V-equivalent target for the world path (Lever W v2
// wrote at CameraSrc+0x3AC0 which is not the source RenderOpaque reads).
std::atomic_uint32_t world_vp_trace_remaining{0};
// 40 was measured too small: a real frame fires log_world_vp_at ~24 extra
// times from the per-model/per-decal Gather*Prepare call sites alone, and
// the prior 40-line capture exhausted its budget at "SetupView_world
// ENTER" without ever reaching EXIT or the RenderOpaque checkpoints this
// probe exists to compare. 150 gives >3x margin over that measured
// per-frame volume (~0.3ms/line observed from that capture's own
// timestamps, so worst case ~45ms for one arm-and-fire, a single-frame
// diagnostic hit, not a sustained cost).
constexpr uint32_t kMaxWorldVpTraceLines = 150;

// ===== CAMERA-STATE CENSUS PER GATHER JOB (2026-08-28) =====================
// READ-ONLY. The corpus split the residue into gen-0 (welded: reticle,
// effects - camera NEVER rotated) and gen-2 (gun, windows - rotated TWICE).
// Both are TIMING bugs, and this names them: at each gather job it classifies
// renderView+0xC44 against clean / rotated-once / rotated-twice and tallies
// per site. A job that reads CLEAN while Lever A has already fired elsewhere
// is where a gen-0 population's MVP is composed; a job reading rotated-TWICE
// is where a gen-2 population's is. That is the producer map the whole
// in-engine road needs, and it costs no behavioural change and no run of its
// own - it rides whatever run happens next.
const char* const kCamSiteNames[CS_COUNT] = {
    "GatherPrepare", "GatherModels", "GatherDecals", "GatherLights",
    "GatherAddAlways", "SetupWorldEnter", "SetupWorldExit"};
std::atomic_uint32_t g_cam_state[CS_COUNT][4]{};  // [site][clean|rot1|rot2|other]

void sample_camera_state_at(CamSite site) {
    if (!wrap::armed() || !wrap::head_look_active()) return;
    float cr[3], cu[3], cf[3];
    if (!wrap::reference_basis(cr, cu, cf)) return;
    float R[9];
    if (!wrap::head_world_rotation(R)) return;
    const uintptr_t slot = captured_render_view_slot.load(std::memory_order_relaxed);
    if (!slot) return;
    uintptr_t rv = 0;
    if (!safe_read_bytes(slot, &rv, sizeof(rv)) || !rv) return;
    alignas(16) float m[16]{};
    if (!safe_read_bytes(rv + 0xC44, m, sizeof(m))) return;
    // Rotated-once and rotated-twice reference bases.
    auto rot = [&](const float in[3], float out[3]) {
        for (int i = 0; i < 3; ++i)
            out[i] = R[i*3+0]*in[0] + R[i*3+1]*in[1] + R[i*3+2]*in[2];
    };
    float r1r[3], r1u[3], r1f[3], r2r[3], r2u[3], r2f[3];
    rot(cr, r1r); rot(cu, r1u); rot(cf, r1f);
    rot(r1r, r2r); rot(r1u, r2u); rot(r1f, r2f);
    int bucket = 3;
    if (wrap::matches_reference_basis_signed(m, cr, cu, cf))       bucket = 0;
    else if (wrap::matches_reference_basis_signed(m, r1r, r1u, r1f)) bucket = 1;
    else if (wrap::matches_reference_basis_signed(m, r2r, r2u, r2f)) bucket = 2;
    g_cam_state[site][bucket].fetch_add(1, std::memory_order_relaxed);
}

void log_world_vp_at(const char* location) {
    // fetch_sub returns the value BEFORE decrement; only log when it was
    // positive going in. Atomic decrement so multiple worker threads
    // firing simultaneously each get a unique budget slot.
    const uint32_t before = world_vp_trace_remaining.load(std::memory_order_relaxed);
    if (before == 0) return;
    const uint32_t got = world_vp_trace_remaining.fetch_sub(1, std::memory_order_relaxed);
    if (got == 0) {
        // We over-shot the budget on this or another thread; put it back
        // and skip.
        world_vp_trace_remaining.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uintptr_t slot = captured_render_view_slot.load(std::memory_order_relaxed);
    if (!slot) return;
    uintptr_t rv = 0;
    if (!safe_read_bytes(slot, &rv, sizeof(rv)) || rv == 0) return;
    alignas(16) float m[16]{};
    if (!safe_read_bytes(rv + 0xC44, m, sizeof(m))) return;
    log::warn(std::format(
        "WorldVpTrace [{:25}] renderView={:#x} +0xC44  "
        "row0=({:.4f} {:.4f} {:.4f} {:.4f})  "
        "row2=({:.4f} {:.4f} {:.4f} {:.4f})",
        location, rv, m[0], m[1], m[2], m[3], m[8], m[9], m[10], m[11]));
}

void __fastcall hook_setup_view_world(void* arg1) {
    FbDoom fbd_(FB_D_SETUP_VIEW_WORLD);
    if (doubled_skip_setup_world_now()) return;
    vwc_entry(kVwsLeverWSrc);
    log_world_vp_at("SetupView_world ENTER");
    sample_camera_state_at(CS_SetupWorldEnter);
    const float unified = g_stereo_offset.load(std::memory_order_relaxed);
    // Did Lever W actually fire inside the doubled call, and with what offset?
    // The first probe found CameraSrc+0x3AC0 identical at offset 0 and offset
    // 200 by draw time, which is either "this hook never ran", "it ran with a
    // zero offset", or "it ran and something overwrote it afterwards". This
    // distinguishes the first two from the third. Bounded by the same budget.
    if (inside_frame_root_double &&
        world_vp_source_probe_lines.load(std::memory_order_relaxed)
            < kMaxWorldVpSourceProbeLines) {
        log::warn(std::format(
            "WorldVpSource LEVER-W-ENTRY inside_double=1 g_stereo_offset={:.1f} willApply={}",
            unified, unified != 0.0f));
    }
    if (unified != 0.0f) {
        const uintptr_t doom_base =
            reinterpret_cast<uintptr_t>(GetModuleHandleW(L"DOOMx64vk.exe"));
        if (doom_base) {
            // renderer_ptr_slot = &renderer_ptr = doom_base + kRendererPtrGlobalRva
            // renderer                          = *renderer_ptr_slot
            // CameraSrc                         = renderer.first_field
            uintptr_t renderer = 0;
            uintptr_t camera_src = 0;
            if (safe_read_bytes(doom_base + kRendererPtrGlobalRva,
                    &renderer, sizeof(renderer)) &&
                renderer != 0 &&
                safe_read_bytes(renderer, &camera_src, sizeof(camera_src)) &&
                camera_src != 0) {
                const uintptr_t vp_addr = camera_src + kCameraSrcWorldVpOffset;
                alignas(16) float m[16]{};
                // Does Lever W's write to CameraSrc actually stick? The probe
                // at RenderSort shows CameraSrc+0x3AC0 byte-identical between
                // the offset-0 and offset-200 attempts despite this hook
                // logging willApply=true, so either the write never lands or
                // something recomputes the value between here and the draws.
                // Read back immediately after writing to separate those.
                if (inside_frame_root_double &&
                    world_vp_source_probe_lines.load(std::memory_order_relaxed)
                        < kMaxWorldVpSourceProbeLines) {
                    alignas(16) float before[16]{}, after[16]{};
                    if (safe_read_bytes(vp_addr, before, sizeof(before))) {
                        alignas(16) float test[16]{};
                        std::memcpy(test, before, sizeof(test));
                        apply_translation(test, unified);
                        const bool wrote = safe_write_bytes(vp_addr, test, sizeof(test));
                        const bool reread = safe_read_bytes(vp_addr, after, sizeof(after));
                        vwc_write(kVwsLeverWSrc, vp_addr, unified, before, test);
                        log::warn(std::format(
                            "WorldVpSource LEVER-W-WRITE addr={:#x} wrote={} reread={} "
                            "before[3]={:.4f} intended[3]={:.4f} actual[3]={:.4f}",
                            vp_addr, wrote, reread, before[3], test[3], after[3]));
                    }
                }
                if (safe_read_bytes(vp_addr, m, sizeof(m))) {
                    if (lever_w_probe_pending.exchange(false, std::memory_order_relaxed)) {
                        log::warn(std::format(
                            "LeverW probe: renderer={:#x} CameraSrc={:#x} vp_addr={:#x}",
                            renderer, camera_src, vp_addr));
                        log::warn(std::format(
                            "LeverW probe: vp_before  row0=({:.4f} {:.4f} {:.4f} {:.4f}) "
                            "row1=({:.4f} {:.4f} {:.4f} {:.4f}) row2=({:.4f} {:.4f} {:.4f} {:.4f}) "
                            "row3=({:.4f} {:.4f} {:.4f} {:.4f})",
                            m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
                            m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]));
                        // Extended probe: 0xC44 was a guess based on the
                        // destination renderView layout, but the source's
                        // layout is different. Scan CameraSrc for matrix-
                        // shape hits (VP or pure-view) so we can find the
                        // real world-VP offset in one press.
                        scan_object_for_camera_matrices(camera_src, "CameraSrc");
                    }
                    // Milestone 0 (docs/STEREO-IMPLEMENTATION-PLAN.md): a
                    // fresh 150-line WorldVpTrace capture (2026-08-11,
                    // g_stereo_offset=2) showed renderView+0xC44 completely
                    // unchanged between "SetupView_world ENTER" and "EXIT"
                    // across three full frames, while Lever A's
                    // apply_lever_a_mutation (fired earlier, at
                    // RenderGatherPrepare) produced the only observed shift,
                    // which then held bit-identical all the way through
                    // "RenderOpaque post-real". This confirms tonight's
                    // isolated-test finding: this write, to CameraSrc+0x3AC0,
                    // does not reach renderView+0xC44 - it is dead weight,
                    // not a compounding double-shift, but it predates the
                    // actual fix (apply_shift_to_world_vp_destinations) and
                    // isn't part of the proven mechanism. Disabled as the
                    // sole canonical world-path write stays Lever A's,
                    // before per-eye sign is layered on top (Milestone 1).
                    // The read/scan diagnostics above (LeverW probe) are
                    // untouched - only the mutate+write is removed.
                    (void)unified;
                }
            }
        }
    }
    // BUILD B (2026-08-26): rotate_camera_src_for_head() ran here, before the
    // trampoline, and rotate_world_vp_destinations_for_head() ran after it.
    // Both are DELETED - see the "REMOVED 2026-08-26, BUILD B" block above
    // apply_shift_to_world_vp_destinations for the counters that retired them
    // (cameraSrc=9342 writes with no image change; destCopies=0, never once
    // found its offsets after being gameplay-gated). This hook is now a pure
    // trampoline plus the two read-only world-VP log points.
    { FbTramp fbt_(FB_D_SETUP_VIEW_WORLD); real_setup_view_world(arg1); }
    log_world_vp_at("SetupView_world EXIT");
    sample_camera_state_at(CS_SetupWorldExit);
}

// AppendDrawCommandProbe: read-only, logs which pass-table address and
// count each append call targets. Directly answers, from live data instead
// of more static guessing: which of the 16 pass-slots does world-geometry
// gathering actually touch, does the count ever get reset mid-frame (would
// show up as a drop back toward 0 while sample count keeps climbing), and
// how many appends happen in a typical frame. Bounded, unconditional -
// same pattern as UpstreamPipelineProbe above.
//
// Cap raised from 120 to 3000 (2026-08-09): the original 120 was consumed
// entirely within a SINGLE frame's dominant slot (which alone reached 100+
// appends), before ever getting a clean read on that slot's true per-frame
// total - meaning the "baseline ~7-8" figure quoted earlier this session
// was an artifact of the cap being hit mid-count, not a real baseline.
// widenActive tag added so a single capture (play normally, then press F5)
// gives a directly comparable normal-vs-widened dataset without needing
// separate log files.
constexpr uint32_t kMaxAppendDrawCommandSamples = 3000;
std::atomic_uint32_t append_draw_command_sample_count{0};

void __fastcall hook_append_draw_command(int64_t a1, int64_t a2, int64_t a3, int64_t a4,
    int64_t a5, int64_t pass_table) {
    const uint32_t sample = append_draw_command_sample_count.fetch_add(1, std::memory_order_relaxed);
    if (sample < kMaxAppendDrawCommandSamples) {
        int32_t count_before = -1;
        safe_read_bytes(static_cast<uintptr_t>(pass_table) + 0x38000, &count_before,
            sizeof(count_before));
        log::info(std::format(
            "AppendDrawCommandProbe sample={} a1={:#x} a2={:#x} passTable={:#x} countBefore={} "
            "widenActive={} thread={}",
            sample, a1, a2, pass_table, count_before,
            widen_gather_probe_enabled.load(std::memory_order_relaxed), GetCurrentThreadId()));
    }
    real_append_draw_command(a1, a2, a3, a4, a5, pass_table);
}

// RealFrameQueryGateProbe (docs/STEREO-RESTART-PLAN.md Task 2) - see
// query_gate_seen_this_real_frame's declaration above for the full reasoning.
// HOME-gated (2026-08-11, corrected after a same-session finding): this
// used to start counting its 200-frame window automatically from the first
// real RenderOpaque call after injection - which runs during menu/loading,
// well before any human action marks "this is a comparable moment to
// whatever extra-call test I'm about to run." A live capture that same
// session showed the 200-sample window silently spanning a menu phase
// (100% query-gate-flagged) and an early-gameplay phase (0% flagged) with
// no way to tell from the log which part aligned with an INSERT press.
// Never trust an auto-started passive capture as a baseline for a human-
// triggered test again - arm this explicitly, right before/after the test
// it's meant to be compared against, so both windows cover the same
// in-game moment.
constexpr uint32_t kMaxRealFrameQueryGateProbeSamples = 200;
std::atomic_uint32_t real_frame_query_gate_probe_samples{0};
std::atomic_bool real_frame_query_gate_probe_armed{false};
// PassDrawCountProbe - see current_render_opaque_pass_slot's declaration
// above. Shares real_frame_query_gate_probe_armed's arming window (HOME).
std::atomic_uint32_t pass_draw_count_probe_samples{0};
// Bit31Probe's extra-call side - see bit31_probe_total_draws' declaration
// above. Logged once per StereoDualEyeDump extra RenderOpaque call (per eye).
constexpr uint32_t kMaxBit31ProbeExtraSamples = 400;
std::atomic_uint32_t bit31_probe_extra_samples{0};

void __fastcall hook_doom_render_opaque(void* arg1) {
    FbDoom fbd_(FB_D_DOOM_RENDER_OPAQUE);
    log_world_vp_at("RenderOpaque ENTER");
    // DOOM+0x1A37CC0, registered with DOOM's job scheduler under the name
    // "RenderOpaque" (see docs/RE-NOTES.md). Read-only diagnostic to answer
    // Phase 2.4's open questions - whether arg1 is a stable per-renderer
    // singleton or reallocated per frame, and to sanity-check a few other
    // static-analysis hypotheses (the renderView+0xC44 cross-reference, the
    // pass-slot table pointer, the candidate dual-viewport rects, the sync/
    // async job flag). Every offset below is an UNVALIDATED static-analysis
    // hypothesis - every read is SEH-guarded and this hook never writes
    // anything or modifies control flow.
    const uint32_t sample = render_opaque_sample_count.fetch_add(1, std::memory_order_relaxed);
    // StereoCVarProbe (2026-08-09, one-shot, read-only): DOOM 2016's own
    // binary has a LIVE (not dead-code) idCVar registration for id
    // Software's built-in positional stereo separation - stereoRender_
    // separation (default "1.5", described as "world units from center to
    // eyes"), stereoRender_screenSeparation, stereoRender_guiOffset,
    // stereoRender_swapEyes - found via the same job-registration-string
    // technique already used throughout this file, confirmed via a real
    // `call idCVar::idCVar(name, default, flags, description)` at each
    // site, not just a string sitting unused in .rdata. But manually typing
    // "stereo" in DOOM's live console listed nothing starting with that
    // prefix - contradicting "these are live, visible cvars" at face value.
    // This resolves which is true: read each cvar object's value field
    // (offset +0x30, best guess from decompiling the shared idCVar
    // constructor at DOOM+0x293870/+0x2947F0 - unconfirmed, this read is
    // exactly the live test that validates or refutes the guess) directly
    // from the running process, once, regardless of what the console UI
    // shows - a hidden-from-cvarlist flag and "genuinely not constructed
    // with real data" are very different findings and this settles it.
    {
        static std::atomic_bool logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed)) {
            const uintptr_t doom_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"DOOMx64vk.exe"));
            struct StereoCvar { const char* name; uintptr_t rva; };
            constexpr StereoCvar kStereoCvars[] = {
                {"stereoRender_separation", 0x5FB8870},
                {"stereoRender_screenSeparation", 0x5FB8910},
                {"stereoRender_guiOffset", 0x5FB89B0},
                {"stereoRender_swapEyes", 0x5FB8A50},
            };
            for (const auto& cvar : kStereoCvars) {
                float value_at_0x30 = NAN, value_at_0x34 = NAN;
                const uintptr_t obj = doom_base + cvar.rva;
                safe_read_bytes(obj + 0x30, &value_at_0x30, sizeof(value_at_0x30));
                safe_read_bytes(obj + 0x34, &value_at_0x34, sizeof(value_at_0x34));
                log::warn(std::format(
                    "StereoCVarProbe name={} object={:#x} valueAt0x30={:.4f} valueAt0x34={:.4f}",
                    cvar.name, obj, value_at_0x30, value_at_0x34));
            }
        }
    }
    // Captured before the real call below runs, from the not-yet-incremented
    // true_stereo_attempt_count - the same value the TrueStereo block further
    // down will read for THIS invocation's extra call, so both calls' binds
    // get tagged with one consistent frame number. See TrueStereoGpuProbe.
    current_true_stereo_frame = true_stereo_enabled.load(std::memory_order_relaxed)
        ? true_stereo_attempt_count.load(std::memory_order_relaxed) : UINT32_MAX;
    // Captured unconditionally (not gated by kMaxRenderOpaqueSamples, unlike
    // the diagnostic block below) so WidenGatherFovProbe has an always-
    // fresh render_view_slot address to use when swapping happens during
    // the natural once-per-frame RenderGatherPrepare call, which runs
    // BEFORE this function and so can't read it fresh itself. The slot
    // ADDRESS (not the render-view it points to, which changes with camera
    // movement) is expected to be stable for the renderer's lifetime.
    {
        uintptr_t slot = 0;
        if (safe_read_bytes(reinterpret_cast<uintptr_t>(arg1) + 0x40, &slot, sizeof(slot)) && slot)
            captured_render_view_slot.store(slot, std::memory_order_relaxed);
        uintptr_t pass_table_base = 0;
        if (safe_read_bytes(reinterpret_cast<uintptr_t>(arg1) + 0x130, &pass_table_base,
                sizeof(pass_table_base)) && pass_table_base)
            captured_pass_table_base.store(pass_table_base, std::memory_order_relaxed);
    }
    if (sample < kMaxRenderOpaqueSamples) {
        const auto base = reinterpret_cast<uintptr_t>(arg1);
        char text[768];
        int n = std::snprintf(text, sizeof(text), "RenderOpaque sample=%u arg1=%p", sample, arg1);

        uintptr_t render_view_slot = 0, render_view = 0;
        if (safe_read_bytes(base + 0x40, &render_view_slot, sizeof(render_view_slot)) &&
            safe_read_bytes(render_view_slot, &render_view, sizeof(render_view))) {
            n += std::snprintf(text + n, sizeof(text) - n,
                " renderViewSlot=%p renderView=%p",
                reinterpret_cast<void*>(render_view_slot), reinterpret_cast<void*>(render_view));
            float camera_row0[4]{};
            if (safe_read_bytes(render_view + 0xc44, camera_row0, sizeof(camera_row0))) {
                n += std::snprintf(text + n, sizeof(text) - n,
                    " camera[0..3]=(%.3f %.3f %.3f %.3f)",
                    camera_row0[0], camera_row0[1], camera_row0[2], camera_row0[3]);
            }
        } else {
            n += std::snprintf(text + n, sizeof(text) - n, " renderView=<unreadable>");
        }

        uintptr_t pass_table = 0;
        if (safe_read_bytes(base + 0x130, &pass_table, sizeof(pass_table))) {
            n += std::snprintf(text + n, sizeof(text) - n, " passTable=%p",
                reinterpret_cast<void*>(pass_table));
        }

        uintptr_t viewport_obj = 0;
        int32_t rect_a[4]{}, rect_b[4]{};
        if (safe_read_bytes(base + 0x18, &viewport_obj, sizeof(viewport_obj)) &&
            safe_read_bytes(viewport_obj + 0x14c8, rect_a, sizeof(rect_a)) &&
            safe_read_bytes(viewport_obj + 0x14d8, rect_b, sizeof(rect_b))) {
            n += std::snprintf(text + n, sizeof(text) - n,
                " rectA=(%d %d %d %d) rectB=(%d %d %d %d)",
                rect_a[0], rect_a[1], rect_a[2], rect_a[3],
                rect_b[0], rect_b[1], rect_b[2], rect_b[3]);
        }

        const uintptr_t doom_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"DOOMx64vk.exe"));
        uint8_t sync_flag = 0;
        if (doom_base && safe_read_bytes(doom_base + 0x7061830, &sync_flag, sizeof(sync_flag))) {
            std::snprintf(text + n, sizeof(text) - n, " syncFlag=%u", sync_flag);
        }

        log::info(text);

        // UpstreamPipelineProbe correlation - see its declaration above.
        // Compares THIS RenderOpaque invocation's arg1 against the most
        // recent arg1 each upstream job was called with (captured earlier
        // this same frame, since they run before RenderOpaque). Answers
        // directly whether any of them share RenderOpaque's context.
        log::info(std::format(
            "UpstreamPipelineProbe correlation renderOpaqueArg1={} "
            "gatherPrepareArg1={} gatherWorldSurfacesArg1={} cullArg1={} "
            "walkBspArg1={} gatherFinishArg1={} sortArg1={}",
            arg1,
            reinterpret_cast<void*>(last_gather_prepare_arg1.load(std::memory_order_relaxed)),
            reinterpret_cast<void*>(last_gather_world_surfaces_arg1.load(std::memory_order_relaxed)),
            reinterpret_cast<void*>(last_cull_arg1.load(std::memory_order_relaxed)),
            reinterpret_cast<void*>(last_walk_bsp_arg1.load(std::memory_order_relaxed)),
            reinterpret_cast<void*>(last_gather_finish_arg1.load(std::memory_order_relaxed)),
            reinterpret_cast<void*>(last_sort_arg1.load(std::memory_order_relaxed))));
    }
    // Lever A2 (docs/CAMERA-LEVER-PLAN.md §3): persistent, unrestored,
    // no-cap camera clobber AFTER gather has already run for this frame
    // and immediately BEFORE the real draw. Reads render_view fresh from
    // arg1 (this frame's, not the cached prior-frame one). Comparison with
    // A1: if A1 moves the picture and A2 doesn't, +0xC44 is uploaded
    // before gather - upload runs pre-gather. If A2 moves it and A1
    // doesn't, upload runs between gather and draw. If both move it,
    // upload runs after our A2 write (unlikely but informative). If
    // neither moves it, +0xC44/+0xC84 are not read by rendering at all
    // (all prior "confirmations" of the offset confirmed correlation, not
    // causation).
    {
        const int mode = lever_a2_mode.load(std::memory_order_relaxed);
        const bool unified_active = g_stereo_offset.load(std::memory_order_relaxed) != 0.0f;
        // wrap::armed() joins this guard: Phase 3 head look is a reason to write
        // the camera even with no stereo offset and no lever mode armed.
        if (mode != static_cast<int>(LeverAMode::Off) || unified_active || wrap::armed()) {
            uintptr_t render_view_slot = 0, render_view = 0;
            if (safe_read_bytes(reinterpret_cast<uintptr_t>(arg1) + 0x40,
                    &render_view_slot, sizeof(render_view_slot)) &&
                render_view_slot &&
                safe_read_bytes(render_view_slot, &render_view, sizeof(render_view))) {
                apply_lever_a_mutation(render_view, mode);
            }
        }
    }
    inside_render_opaque = true;
    query_gate_seen_this_real_frame = false;
    bit31_probe_total_draws = 0;
    bit31_probe_flagged_draws = 0;
    scissor_captured_this_call = false;
    viewport_captured_this_call = false;
    representative_world_pipeline_this_call = VK_NULL_HANDLE;
    representative_world_pipeline_cmdbuf_this_call = VK_NULL_HANDLE;
    world_binding_captured_this_call = false;
    world_vertex_captured_this_call = false;
    world_index_captured_this_call = false;
    world_indirect_captured_this_call = false;
    current_render_opaque_pass_slot = -1;
    pass1_draw_count = 0;
    pass2_draw_count = 0;
    other_pass_draw_count = 0;
    log_world_vp_at("RenderOpaque pre-real");
    { FbTramp fbt_(FB_D_DOOM_RENDER_OPAQUE); real_doom_render_opaque(arg1); }
    log_world_vp_at("RenderOpaque post-real");
    // RealFrameQueryGateProbe (docs/STEREO-RESTART-PLAN.md Task 2): logs once
    // per real frame, not per query event, whether the query-gated batch
    // showed up at all this frame - bounded to kMaxRealFrameQueryGateProbeSamples
    // real frames (~200), independent of the extra-call mechanism entirely.
    // Extended with Bit31Probe's raw counts and ScissorViewportProbe's first-
    // seen rect (see their declarations above) so a capture answers not just
    // "did a query fire" but "how many draw commands actually had the gate
    // bit set" and "what scissor/viewport did this real call use."
    if (real_frame_query_gate_probe_armed.load(std::memory_order_relaxed)) {
        const uint32_t sample =
            real_frame_query_gate_probe_samples.fetch_add(1, std::memory_order_relaxed);
        if (sample < kMaxRealFrameQueryGateProbeSamples) {
            log::warn(std::format(
                "RealFrameQueryGateProbe sample={} queryGateSeen={} totalDraws={} bit31Count={} "
                "scissor=({},{} {}x{}) viewport=({:.1f},{:.1f} {:.1f}x{:.1f} depth {:.3f}..{:.3f})",
                sample, query_gate_seen_this_real_frame, bit31_probe_total_draws,
                bit31_probe_flagged_draws,
                captured_scissor.offset.x, captured_scissor.offset.y,
                captured_scissor.extent.width, captured_scissor.extent.height,
                captured_viewport.x, captured_viewport.y,
                captured_viewport.width, captured_viewport.height,
                captured_viewport.minDepth, captured_viewport.maxDepth));
        } else {
            real_frame_query_gate_probe_armed.store(false, std::memory_order_relaxed);
            log::info("RealFrameQueryGateProbe reached its sample cap; disarming automatically");
        }
    }
    // PassDrawCountProbe (docs/STEREO-RESTART-PLAN.md Task 3): settles
    // whether RenderOpaque's first internal pass (offscreen_targets[0],
    // Clear) draws any real content, or is a bare clear with zero draws -
    // docs/RE-NOTES.md's own "which iterations land in which pass isn't
    // mapped yet." Same HOME-armed window/sample count as RealFrameQueryGateProbe
    // above, logged as its own line for readability.
    if (real_frame_query_gate_probe_armed.load(std::memory_order_relaxed)) {
        const uint32_t sample = pass_draw_count_probe_samples.fetch_add(1, std::memory_order_relaxed);
        if (sample < kMaxRealFrameQueryGateProbeSamples) {
            log::warn(std::format(
                "PassDrawCountProbe sample={} pass1(Clear)={} pass2(Load)={} other={}",
                sample, pass1_draw_count, pass2_draw_count, other_pass_draw_count));
        }
    }

    // Hard invariant, independent of any toggle-state race: at most ONE
    // extra RenderOpaque call per real invocation, ever. F6/F7 and
    // StereoPreview are mutually exclusive by design (enforced when their
    // hotkeys arm/disarm each other), but that enforcement happens on the
    // main/present thread while this function runs on DOOM's own worker
    // thread - a key-press landing at exactly the wrong instant could
    // theoretically read stale state on one side. This local flag closes
    // that off directly: only "called twice" (proven safe, 60/60 F7
    // attempts) is possible here, never "called three times" (untested).
    bool extra_call_already_ran = false;

    // RenderViewClone Stage 1 (F2) - see the flag's declaration above for
    // the full reasoning. Deliberately does NOT redirect the render target
    // (inside_injected_extra_call stays false here) - this test is isolated
    // to just the render-view pointer swap, same "isolate what's risky"
    // discipline as every other experiment tonight. Mutually exclusive with
    // F6/F7/F3 (arm-time enforcement, see hotkey handlers) plus the same
    // extra_call_already_ran guard everything else uses.
    if (!extra_call_already_ran && render_view_clone_test_enabled.load(std::memory_order_relaxed)) {
        const uint32_t attempt = render_view_clone_test_count.load(std::memory_order_relaxed);
        if (attempt >= kMaxRenderViewCloneTestAttempts) {
            log::info("RenderViewClone test reached its sample cap; disabling automatically");
            render_view_clone_test_enabled.store(false, std::memory_order_relaxed);
        } else {
            render_view_clone_test_count.fetch_add(1, std::memory_order_relaxed);
            uintptr_t render_view_slot = 0, original_render_view = 0;
            bool swapped = false;
            if (safe_read_bytes(reinterpret_cast<uintptr_t>(arg1) + 0x40, &render_view_slot,
                    sizeof(render_view_slot)) &&
                render_view_slot != 0 &&
                safe_read_bytes(render_view_slot, &original_render_view, sizeof(original_render_view)) &&
                original_render_view != 0 &&
                safe_read_bytes(original_render_view, render_view_clone_buffer, sizeof(render_view_clone_buffer))) {
                const uintptr_t clone_address = reinterpret_cast<uintptr_t>(render_view_clone_buffer);
                swapped = safe_write_bytes(render_view_slot, &clone_address, sizeof(clone_address));
            }
            log::warn(std::format(
                "RenderViewClone attempt={} arg1={} renderViewSlot={} originalRenderView={} swapped={} "
                "(identical-content clone, no camera change - isolating pointer-swap risk from camera-change risk)",
                attempt, arg1, reinterpret_cast<void*>(render_view_slot),
                reinterpret_cast<void*>(original_render_view), swapped));
            extra_call_already_ran = true;
            const bool ok = safe_call_render_opaque(arg1, real_doom_render_opaque);
            bool restored = true;
            if (swapped) {
                restored = safe_write_bytes(render_view_slot, &original_render_view, sizeof(original_render_view));
            }
            if (ok) {
                log::warn(std::format("RenderViewClone attempt={} returned normally, restored={}", attempt, restored));
            } else {
                log::error(std::format(
                    "RenderViewClone attempt={} threw a structured exception (restored={}); disabling",
                    attempt, restored));
                render_view_clone_test_enabled.store(false, std::memory_order_relaxed);
            }
            if (!restored) {
                log::error("RenderViewClone: CRITICAL - failed to restore the original render-view pointer; "
                    "the shared slot may now point at our clone buffer. Disabling permanently, do not re-arm "
                    "this session - investigate before trying again.");
                render_view_clone_test_enabled.store(false, std::memory_order_relaxed);
            }
        }
    }

    // TrueStereo (F1) - Stage 2, see the flag's declaration above. Same
    // clone-and-swap mechanism as Stage 1 (F2), but the clone's camera
    // matrix pair is actually rotated before the swap, and the extra call
    // IS redirected into our offscreen target this time (inside_injected_
    // extra_call = true) so eye 1 gets a genuinely different, visible
    // image instead of a discarded duplicate draw into DOOM's real frame.
    if (!extra_call_already_ran && true_stereo_enabled.load(std::memory_order_relaxed)) {
        const uint32_t attempt = true_stereo_attempt_count.load(std::memory_order_relaxed);
        if (attempt >= kMaxTrueStereoAttempts) {
            log::info("TrueStereo reached its sample cap; disabling automatically");
            true_stereo_enabled.store(false, std::memory_order_relaxed);
        } else {
            true_stereo_attempt_count.fetch_add(1, std::memory_order_relaxed);
            uintptr_t render_view_slot = 0, original_render_view = 0;
            bool swapped = false;
            if (safe_read_bytes(reinterpret_cast<uintptr_t>(arg1) + 0x40, &render_view_slot,
                    sizeof(render_view_slot)) &&
                render_view_slot != 0 &&
                safe_read_bytes(render_view_slot, &original_render_view, sizeof(original_render_view)) &&
                original_render_view != 0 &&
                safe_read_bytes(original_render_view, render_view_clone_buffer, sizeof(render_view_clone_buffer))) {
                // Diagnostic: log the camera row before/after rotation on
                // the very first attempt only, to directly verify the
                // rotation math is actually changing the clone's bytes as
                // expected - rules out a bug in apply_yaw_rotation() itself
                // as the reason no visible effect has been seen live, vs.
                // DOOM deriving/caching the GPU-bound camera data somewhere
                // upstream of what this clone actually feeds.
                const bool log_this_attempt = attempt == 0;
                float before_row0[4]{}, before_row2[4]{};
                if (log_this_attempt) {
                    std::memcpy(before_row0, render_view_clone_buffer + 0xc44, sizeof(before_row0));
                    std::memcpy(before_row2, render_view_clone_buffer + 0xc44 + 32, sizeof(before_row2));
                }
                // Rotate both the current (+0xC44) and previous (+0xC84)
                // matrices in the CLONE identically - never DOOM's real
                // object - per FORBIDDEN-BOUNDARIES.md "Never assume" #3
                // (must set both consistently, not just the current value).
                apply_yaw_rotation(reinterpret_cast<float*>(render_view_clone_buffer + 0xc44),
                    kTrueStereoToeInRadians);
                apply_yaw_rotation(reinterpret_cast<float*>(render_view_clone_buffer + 0xc84),
                    kTrueStereoToeInRadians);
                if (log_this_attempt) {
                    float after_row0[4]{}, after_row2[4]{};
                    std::memcpy(after_row0, render_view_clone_buffer + 0xc44, sizeof(after_row0));
                    std::memcpy(after_row2, render_view_clone_buffer + 0xc44 + 32, sizeof(after_row2));
                    log::warn(std::format(
                        "TrueStereo clone rotation check: row0 before=({:.4f} {:.4f} {:.4f} {:.4f}) "
                        "after=({:.4f} {:.4f} {:.4f} {:.4f}) | row2 before=({:.4f} {:.4f} {:.4f} {:.4f}) "
                        "after=({:.4f} {:.4f} {:.4f} {:.4f})",
                        before_row0[0], before_row0[1], before_row0[2], before_row0[3],
                        after_row0[0], after_row0[1], after_row0[2], after_row0[3],
                        before_row2[0], before_row2[1], before_row2[2], before_row2[3],
                        after_row2[0], after_row2[1], after_row2[2], after_row2[3]));
                }
                const uintptr_t clone_address = reinterpret_cast<uintptr_t>(render_view_clone_buffer);
                swapped = safe_write_bytes(render_view_slot, &clone_address, sizeof(clone_address));
            }
            log::warn(std::format(
                "TrueStereo attempt={} arg1={} renderViewSlot={} originalRenderView={} swapped={} "
                "toeInRadians={} (redirected extra call - first real per-eye divergence attempt)",
                attempt, arg1, reinterpret_cast<void*>(render_view_slot),
                reinterpret_cast<void*>(original_render_view), swapped, kTrueStereoToeInRadians));
            true_stereo_rotated_addresses.clear();
            inside_injected_extra_call = true;
            extra_call_already_ran = true;
            const bool ok = safe_call_render_opaque(arg1, real_doom_render_opaque);
            inside_injected_extra_call = false;
            bool restored = true;
            if (swapped) {
                restored = safe_write_bytes(render_view_slot, &original_render_view, sizeof(original_render_view));
            }
            if (ok) {
                log::warn(std::format("TrueStereo attempt={} returned normally, restored={}", attempt, restored));
            } else {
                log::error(std::format(
                    "TrueStereo attempt={} threw a structured exception (restored={}); disabling",
                    attempt, restored));
                true_stereo_enabled.store(false, std::memory_order_relaxed);
            }
            if (!restored) {
                log::error("TrueStereo: CRITICAL - failed to restore the original render-view pointer; "
                    "the shared slot may now point at our clone buffer. Disabling permanently, do not re-arm "
                    "this session - investigate before trying again.");
                true_stereo_enabled.store(false, std::memory_order_relaxed);
            }
        }
    }

    if (render_opaque_double_call_enabled.load(std::memory_order_relaxed)) {
        const uint32_t attempt = render_opaque_double_call_count.load(std::memory_order_relaxed);
        if (attempt >= kMaxRenderOpaqueDoubleCalls) {
            log::info("RenderOpaque double-call smoke test reached its sample cap; disabling automatically");
            render_opaque_double_call_enabled.store(false, std::memory_order_relaxed);
        } else {
            render_opaque_double_call_count.fetch_add(1, std::memory_order_relaxed);
            // F6 composes here: if armed AND still under its own tighter cap,
            // this specific extra call's vkCmdBeginRenderPass gets redirected
            // into offscreen_target by hook_cmd_begin_render_pass below.
            const bool redirect_this_attempt = redirect_extra_call_enabled.load(std::memory_order_relaxed) &&
                redirect_attempt_count.load(std::memory_order_relaxed) < kMaxRedirectAttempts;
            log::warn(std::format(
                "RenderOpaque DOUBLE-CALL attempt={} arg1={} redirect={} (re-entrancy smoke test, no state changed between calls)",
                attempt, arg1, redirect_this_attempt));
            inside_injected_extra_call = redirect_this_attempt;
            extra_call_already_ran = true;
            const bool ok = safe_call_render_opaque(arg1, real_doom_render_opaque);
            inside_injected_extra_call = false;
            if (ok) {
                log::warn(std::format("RenderOpaque DOUBLE-CALL attempt={} returned normally", attempt));
            } else {
                log::error(std::format(
                    "RenderOpaque DOUBLE-CALL attempt={} threw a structured exception; disabling double-call",
                    attempt));
                render_opaque_double_call_enabled.store(false, std::memory_order_relaxed);
                redirect_extra_call_enabled.store(false, std::memory_order_relaxed);
            }
        }
    }

    // StereoDualEyeDump (INSERT) Phase 1 (STEREO-IMPLEMENTATION-PLAN.md v2,
    // 2026-08-11 third rewrite). Stages A-D (all logged under this
    // mechanism's history, see docs/HANDOFF-NEXT-SESSION.md) tried CPU
    // mutation and GPU-direct writes, with and without a full gather/cull/
    // sort reinvoke - all showed the same near-geometry-collapse signature
    // once the GPU write was active. Root cause per the v2 plan (§3, §3a,
    // §4): RenderWalkBSP's real visibility key is a separate BSP-leaf
    // object, not renderView - reinvoking gather/cull/sort was ALWAYS a
    // no-op for visibility at IPD scale, confirmed by this project's own
    // WalkBSPCullSourceProbe before any of this was tried. The actual cause
    // is depth: both eyes' RenderOpaque calls depth-tested (EQUAL, per
    // Phase 0's live confirmation) against DOOM's single, shared,
    // center-camera depth buffer, never RenderDepth'd per eye. Phase 1
    // fixes exactly that: drop the reinvoke entirely (both eyes share the
    // ONE real gather/cull/sort draw-command list, unmodified - no
    // pass-table/viewport-rect save-restore needed anymore), and instead
    // give each eye its own private depth image, populated by re-invoking
    // ONLY RenderDepth (not the whole pipeline) immediately before that
    // eye's RenderOpaque call. World-path only; SetupView_gui/
    // RenderGuiModels (weapon/HUD) still out of scope.
    if (!extra_call_already_ran && stereo_dual_eye_enabled.load(std::memory_order_relaxed) &&
        stereo_dual_eye_frame_counter.fetch_add(1, std::memory_order_relaxed) %
            kStereoDualEyeFrameGap == 0) {
        const uint32_t attempt = stereo_dual_eye_attempt_count.load(std::memory_order_relaxed);
        if (attempt >= kMaxStereoDualEyeAttempts) {
            log::info("StereoDualEyeDump reached its sample cap; disabling automatically");
            stereo_dual_eye_enabled.store(false, std::memory_order_relaxed);
        } else {
            stereo_dual_eye_attempt_count.fetch_add(1, std::memory_order_relaxed);
            const uintptr_t render_depth_ctx = last_render_depth_arg1.load(std::memory_order_relaxed);
            log::warn(std::format(
                "StereoDualEyeDump attempt={} arg1={} renderDepthCtx={} renderDepthPass={}",
                attempt, arg1, reinterpret_cast<void*>(render_depth_ctx),
                reinterpret_cast<void*>(render_depth_captured_pass.load(std::memory_order_relaxed))));
            if (!render_depth_ctx) {
                log::error(std::format(
                    "StereoDualEyeDump attempt={} RenderDepth has not run yet this session (no "
                    "captured arg1) - skipping this attempt; play a bit longer before arming",
                    attempt));
            } else {
                extra_call_already_ran = true;
                for (int eye = 0; eye < 2 && stereo_dual_eye_enabled.load(std::memory_order_relaxed); ++eye) {
                    const bool depth_ready = ensure_stereo_eye_depth_framebuffer_ready(eye);
                    // PassDrawCountProbe follow-up fix - see
                    // stereo_eye_pass1_framebuffers' declaration above.
                    const bool pass1_ready = ensure_stereo_eye_pass1_framebuffer_ready(eye);
                    log::warn(std::format(
                        "StereoDualEyeDump attempt={} eye={} halfIpd={:.2f} depthFramebufferReady={} "
                        "pass1FramebufferReady={}",
                        attempt, eye, kStereoDualEyeHalfIpd, depth_ready, pass1_ready));
                    bool ok = false;
                    if (depth_ready && pass1_ready) {
                        ok = true;
                        current_eye_render = eye;
                        // Fresh dedup for THIS eye's extra calls - see
                        // true_stereo_rotated_addresses' declaration
                        // (cleared per extra call so a rebound slot within
                        // one call doesn't get double-translated, but eye 1
                        // must not inherit eye 0's already-visited
                        // addresses either). Covers BOTH RenderDepth's and
                        // RenderOpaque's binds within this bracket.
                        true_stereo_rotated_addresses.clear();
                        stereo_dual_eye_candidate_addresses.clear();
                        stereo_dual_eye_targeted_write_done = false;
                        stereo_dual_eye_depth_write_done = false;
                        stereo_dual_eye_worldspace_128_seen = 0;
                        inside_injected_extra_call = true;
                        inside_extra_render_depth_reinvoke = true;
                        ok = ok && safe_call_render_opaque(
                            reinterpret_cast<void*>(render_depth_ctx), real_render_depth);
                        inside_extra_render_depth_reinvoke = false;
                        // Bit31Probe (docs/STEREO-RESTART-PLAN.md Task 2 follow-up) - see
                        // bit31_probe_total_draws' declaration above. Isolates JUST this
                        // eye's RenderOpaque call (RenderDepth's reinvocation above doesn't
                        // go through DOOM+0x1A37910 at all, so it can't pollute this count).
                        // ScissorViewportProbe's and WorldPipelineBindingProbe's
                        // resets also happen here, same reasoning.
                        bit31_probe_total_draws = 0;
                        bit31_probe_flagged_draws = 0;
                        scissor_captured_this_call = false;
                        viewport_captured_this_call = false;
                        representative_world_pipeline_this_call = VK_NULL_HANDLE;
                        representative_world_pipeline_cmdbuf_this_call = VK_NULL_HANDLE;
                        world_binding_captured_this_call = false;
                        world_vertex_captured_this_call = false;
                        world_index_captured_this_call = false;
                        world_indirect_captured_this_call = false;
                        ok = ok && safe_call_render_opaque(arg1, real_doom_render_opaque);
                        {
                            const uint32_t sample =
                                bit31_probe_extra_samples.fetch_add(1, std::memory_order_relaxed);
                            if (sample < kMaxBit31ProbeExtraSamples) {
                                log::warn(std::format(
                                    "Bit31Probe callKind=extra eye={} attempt={} totalDraws={} bit31Count={} "
                                    "scissor=({},{} {}x{}) viewport=({:.1f},{:.1f} {:.1f}x{:.1f} depth {:.3f}..{:.3f})",
                                    eye, attempt, bit31_probe_total_draws, bit31_probe_flagged_draws,
                                    captured_scissor.offset.x, captured_scissor.offset.y,
                                    captured_scissor.extent.width, captured_scissor.extent.height,
                                    captured_viewport.x, captured_viewport.y,
                                    captured_viewport.width, captured_viewport.height,
                                    captured_viewport.minDepth, captured_viewport.maxDepth));
                            }
                        }
                        inside_injected_extra_call = false;
                        current_eye_render = -1;
                    }
                    if (!ok) {
                        log::error(std::format(
                            "StereoDualEyeDump attempt={} eye={} threw a structured exception or its "
                            "depth framebuffer was never ready; disabling",
                            attempt, eye));
                        stereo_dual_eye_enabled.store(false, std::memory_order_relaxed);
                    }
                }
                log::warn(std::format("StereoDualEyeDump attempt={} both eyes done", attempt));
            }
        }
    }

    // UpstreamPipelineReinvoke Stage A (F9) - see its declaration above for
    // full reasoning. Mutually exclusive with F1/F2/F3/F6/F7 (enforced at
    // arm-time), and extra_call_already_ran closes the same race window as
    // everywhere else in this function.
    if (!extra_call_already_ran && upstream_reinvoke_enabled.load(std::memory_order_relaxed) &&
        upstream_reinvoke_frame_counter.fetch_add(1, std::memory_order_relaxed) %
            kUpstreamReinvokeFrameGap == 0) {
        const uint32_t attempt = upstream_reinvoke_attempt_count.load(std::memory_order_relaxed);
        if (attempt >= kMaxUpstreamReinvokeAttempts) {
            log::info("UpstreamPipelineReinvoke Stage A reached its sample cap; disabling automatically");
            upstream_reinvoke_enabled.store(false, std::memory_order_relaxed);
        } else {
            upstream_reinvoke_attempt_count.fetch_add(1, std::memory_order_relaxed);
            const uintptr_t gather_prepare_ctx = last_gather_prepare_arg1.load(std::memory_order_relaxed);
            const uintptr_t gather_world_surfaces_ctx =
                last_gather_world_surfaces_arg1.load(std::memory_order_relaxed);
            const uintptr_t cull_ctx = last_cull_arg1.load(std::memory_order_relaxed);
            const uintptr_t walk_bsp_ctx = last_walk_bsp_arg1.load(std::memory_order_relaxed);
            const uintptr_t gather_finish_ctx = last_gather_finish_arg1.load(std::memory_order_relaxed);
            const uintptr_t sort_ctx = last_sort_arg1.load(std::memory_order_relaxed);
            // Sibling Gather*Prepare jobs (see their trampolines' comment
            // above) - first live test without these produced a visually
            // SMALLER redraw (world geometry only), not corruption; adding
            // them so the re-gathered scene matches the real frame's.
            const uintptr_t gather_models_ctx = last_gather_models_arg1.load(std::memory_order_relaxed);
            const uintptr_t gather_decals_ctx = last_gather_decals_arg1.load(std::memory_order_relaxed);
            const uintptr_t gather_lights_ctx = last_gather_lights_arg1.load(std::memory_order_relaxed);
            const uintptr_t gather_add_always_ctx =
                last_gather_add_always_arg1.load(std::memory_order_relaxed);
            const bool have_all_contexts = gather_prepare_ctx && gather_world_surfaces_ctx &&
                cull_ctx && walk_bsp_ctx && gather_finish_ctx && sort_ctx && gather_models_ctx &&
                gather_decals_ctx && gather_lights_ctx && gather_add_always_ctx;

            uintptr_t pass_table_base = 0;
            const bool got_pass_table = have_all_contexts && safe_read_bytes(
                reinterpret_cast<uintptr_t>(arg1) + 0x130, &pass_table_base, sizeof(pass_table_base));
            // Viewport-rect region (see viewport_rect_save_buffer's
            // declaration above) - RenderGatherWorldSurfacesPrepare writes
            // into this same +0x18-relative object RenderOpaque later reads
            // for its viewport-rect selection. Saved/restored alongside the
            // pass-table so our detour can't leave it in a state the real
            // frame's next RenderOpaque call would trip over.
            uintptr_t viewport_obj = 0;
            const bool got_viewport_obj = got_pass_table && safe_read_bytes(
                reinterpret_cast<uintptr_t>(arg1) + 0x18, &viewport_obj, sizeof(viewport_obj));
            bool save_ok = got_viewport_obj;
            if (got_viewport_obj) {
                for (int slot = 0; slot < kPassSlotCount; ++slot) {
                    save_ok = save_ok && safe_read_bytes(
                        pass_table_base + slot * kPassSlotStride + 0x30000,
                        pass_table_save_buffer[slot], kPassSlotSaveSize);
                }
                save_ok = save_ok && safe_read_bytes(viewport_obj + kViewportRectOffset,
                    viewport_rect_save_buffer, kViewportRectSaveSize);
            }
            bool reset_ok = save_ok;
            if (save_ok) {
                for (int slot = 0; slot < kPassSlotCount; ++slot) {
                    const uint32_t zero = 0;
                    reset_ok = reset_ok && safe_write_bytes(
                        pass_table_base + slot * kPassSlotStride + 0x38000, &zero, sizeof(zero));
                }
            }
            log::warn(std::format(
                "UpstreamPipelineReinvoke attempt={} arg1={} passTable={} viewportObj={} "
                "haveAllContexts={} gotViewportObj={} saveOk={} resetOk={}",
                attempt, arg1, reinterpret_cast<void*>(pass_table_base),
                reinterpret_cast<void*>(viewport_obj), have_all_contexts, got_viewport_obj,
                save_ok, reset_ok));

            bool ok = false;
            if (reset_ok) {
                extra_call_already_ran = true;
                ok = true;
                ok = ok && safe_call_render_opaque(
                    reinterpret_cast<void*>(gather_prepare_ctx), real_render_gather_prepare);
                ok = ok && safe_call_render_opaque(
                    reinterpret_cast<void*>(gather_add_always_ctx), real_render_gather_add_always);
                ok = ok && safe_call_render_opaque(
                    reinterpret_cast<void*>(gather_world_surfaces_ctx),
                    real_render_gather_world_surfaces_prepare);
                ok = ok && safe_call_render_opaque(
                    reinterpret_cast<void*>(gather_models_ctx), real_render_gather_models_prepare);
                ok = ok && safe_call_render_opaque(
                    reinterpret_cast<void*>(gather_decals_ctx), real_render_gather_decals_prepare);
                ok = ok && safe_call_render_opaque(
                    reinterpret_cast<void*>(gather_lights_ctx), real_render_gather_lights_prepare);
                ok = ok && safe_call_render_opaque(reinterpret_cast<void*>(cull_ctx), real_render_cull);
                ok = ok && safe_call_render_opaque(
                    reinterpret_cast<void*>(walk_bsp_ctx), real_render_walk_bsp);
                ok = ok && safe_call_render_opaque(
                    reinterpret_cast<void*>(gather_finish_ctx), real_render_gather_finish);
                ok = ok && safe_call_render_opaque(reinterpret_cast<void*>(sort_ctx), real_render_sort);
                // Stage A: no render-view swap, no target redirect - draws
                // into DOOM's real frame with the SAME arg1/camera as the
                // real call, exactly like F7's original re-entrancy smoke
                // test, just preceded by a full re-gather instead of
                // reusing the real call's already-built draw list.
                ok = ok && safe_call_render_opaque(arg1, real_doom_render_opaque);
            }

            bool restore_ok = true;
            if (save_ok) {
                for (int slot = 0; slot < kPassSlotCount; ++slot) {
                    restore_ok = restore_ok && safe_write_bytes(
                        pass_table_base + slot * kPassSlotStride + 0x30000,
                        pass_table_save_buffer[slot], kPassSlotSaveSize);
                }
                restore_ok = restore_ok && safe_write_bytes(viewport_obj + kViewportRectOffset,
                    viewport_rect_save_buffer, kViewportRectSaveSize);
            }

            if (!reset_ok) {
                log::error(std::format(
                    "UpstreamPipelineReinvoke attempt={} skipped the re-invoke sequence entirely - "
                    "missing context, pass-table read, or reset failed; disabling", attempt));
                upstream_reinvoke_enabled.store(false, std::memory_order_relaxed);
            } else if (ok) {
                log::warn(std::format(
                    "UpstreamPipelineReinvoke attempt={} returned normally, restoreOk={}",
                    attempt, restore_ok));
            } else {
                log::error(std::format(
                    "UpstreamPipelineReinvoke attempt={} threw a structured exception somewhere in "
                    "the chain, restoreOk={}; disabling", attempt, restore_ok));
                upstream_reinvoke_enabled.store(false, std::memory_order_relaxed);
            }
            if (!restore_ok) {
                log::error("UpstreamPipelineReinvoke: CRITICAL - failed to restore the pass-table's "
                    "original contents; the real frame's draw list may now be corrupted. Disabling "
                    "permanently, do not re-arm this session - investigate before trying again.");
                upstream_reinvoke_enabled.store(false, std::memory_order_relaxed);
            }
        }
    }

    // ScopedPipelineReinvoke Stage A' (F4) - see its declaration above.
    // Mutually exclusive with F9 and every other extra-call mechanism
    // (arm-time), extra_call_already_ran closes the same race window.
    if (!extra_call_already_ran && scoped_reinvoke_enabled.load(std::memory_order_relaxed) &&
        scoped_reinvoke_frame_counter.fetch_add(1, std::memory_order_relaxed) %
            kScopedReinvokeFrameGap == 0) {
        const uint32_t attempt = scoped_reinvoke_attempt_count.load(std::memory_order_relaxed);
        if (attempt >= kMaxScopedReinvokeAttempts) {
            log::info("ScopedPipelineReinvoke reached its sample cap; disabling automatically");
            scoped_reinvoke_enabled.store(false, std::memory_order_relaxed);
        } else {
            scoped_reinvoke_attempt_count.fetch_add(1, std::memory_order_relaxed);
            // Only the four visibility-dependent jobs - NOT re-running any
            // Gather*Prepare/GatherAddAlways job, unlike F9. Their captured
            // arg1s (last_gather_prepare_arg1 etc.) are deliberately unused
            // here.
            const uintptr_t cull_ctx = last_cull_arg1.load(std::memory_order_relaxed);
            const uintptr_t walk_bsp_ctx = last_walk_bsp_arg1.load(std::memory_order_relaxed);
            const uintptr_t gather_finish_ctx = last_gather_finish_arg1.load(std::memory_order_relaxed);
            const uintptr_t sort_ctx = last_sort_arg1.load(std::memory_order_relaxed);
            const bool have_all_contexts = cull_ctx && walk_bsp_ctx && gather_finish_ctx && sort_ctx;

            uintptr_t pass_table_base = 0;
            const bool got_pass_table = have_all_contexts && safe_read_bytes(
                reinterpret_cast<uintptr_t>(arg1) + 0x130, &pass_table_base, sizeof(pass_table_base));
            uintptr_t viewport_obj = 0;
            const bool got_viewport_obj = got_pass_table && safe_read_bytes(
                reinterpret_cast<uintptr_t>(arg1) + 0x18, &viewport_obj, sizeof(viewport_obj));
            bool save_ok = got_viewport_obj;
            if (got_viewport_obj) {
                for (int slot = 0; slot < kPassSlotCount; ++slot) {
                    save_ok = save_ok && safe_read_bytes(
                        pass_table_base + slot * kPassSlotStride + 0x30000,
                        pass_table_save_buffer[slot], kPassSlotSaveSize);
                }
                save_ok = save_ok && safe_read_bytes(viewport_obj + kViewportRectOffset,
                    viewport_rect_save_buffer, kViewportRectSaveSize);
            }
            bool reset_ok = save_ok;
            if (save_ok) {
                for (int slot = 0; slot < kPassSlotCount; ++slot) {
                    const uint32_t zero = 0;
                    reset_ok = reset_ok && safe_write_bytes(
                        pass_table_base + slot * kPassSlotStride + 0x38000, &zero, sizeof(zero));
                }
            }
            log::warn(std::format(
                "ScopedPipelineReinvoke attempt={} arg1={} passTable={} viewportObj={} "
                "haveAllContexts={} gotViewportObj={} saveOk={} resetOk={} (Cull/WalkBSP/GatherFinish/"
                "Sort only - NO Gather*Prepare re-run)",
                attempt, arg1, reinterpret_cast<void*>(pass_table_base),
                reinterpret_cast<void*>(viewport_obj), have_all_contexts, got_viewport_obj,
                save_ok, reset_ok));

            bool ok = false;
            if (reset_ok) {
                extra_call_already_ran = true;
                ok = true;
                ok = ok && safe_call_render_opaque(reinterpret_cast<void*>(cull_ctx), real_render_cull);
                ok = ok && safe_call_render_opaque(
                    reinterpret_cast<void*>(walk_bsp_ctx), real_render_walk_bsp);
                ok = ok && safe_call_render_opaque(
                    reinterpret_cast<void*>(gather_finish_ctx), real_render_gather_finish);
                ok = ok && safe_call_render_opaque(reinterpret_cast<void*>(sort_ctx), real_render_sort);
                // Stage A': same as F9's Stage A - no render-view swap, no
                // target redirect, draws into DOOM's real frame with the
                // SAME arg1/camera as the real call. Validates safety AND
                // visual completeness (does skipping Prepare produce a
                // complete scene or a degraded one) before ever attempting
                // camera divergence.
                ok = ok && safe_call_render_opaque(arg1, real_doom_render_opaque);
            }

            bool restore_ok = true;
            if (save_ok) {
                for (int slot = 0; slot < kPassSlotCount; ++slot) {
                    restore_ok = restore_ok && safe_write_bytes(
                        pass_table_base + slot * kPassSlotStride + 0x30000,
                        pass_table_save_buffer[slot], kPassSlotSaveSize);
                }
                restore_ok = restore_ok && safe_write_bytes(viewport_obj + kViewportRectOffset,
                    viewport_rect_save_buffer, kViewportRectSaveSize);
            }

            if (!reset_ok) {
                log::error(std::format(
                    "ScopedPipelineReinvoke attempt={} skipped the re-invoke sequence entirely - "
                    "missing context, pass-table read, or reset failed; disabling", attempt));
                scoped_reinvoke_enabled.store(false, std::memory_order_relaxed);
            } else if (ok) {
                log::warn(std::format(
                    "ScopedPipelineReinvoke attempt={} returned normally, restoreOk={}",
                    attempt, restore_ok));
            } else {
                log::error(std::format(
                    "ScopedPipelineReinvoke attempt={} threw a structured exception somewhere in "
                    "the chain, restoreOk={}; disabling", attempt, restore_ok));
                scoped_reinvoke_enabled.store(false, std::memory_order_relaxed);
            }
            if (!restore_ok) {
                log::error("ScopedPipelineReinvoke: CRITICAL - failed to restore the pass-table's "
                    "original contents; the real frame's draw list may now be corrupted. Disabling "
                    "permanently, do not re-arm this session - investigate before trying again.");
                scoped_reinvoke_enabled.store(false, std::memory_order_relaxed);
            }
        }
    }

    // StereoPreview (F3) - see the flag's declaration above for the full
    // reasoning. Mutually exclusive with F6/F7 (enforced at arm-time), and
    // extra_call_already_ran above closes the race window that arm-time
    // enforcement alone can't (see its comment).
    if (!extra_call_already_ran && stereo_preview_enabled.load(std::memory_order_relaxed)) {
        const uint32_t attempt = stereo_preview_attempt_count.load(std::memory_order_relaxed);
        if (attempt >= kMaxStereoPreviewAttempts) {
            log::info("StereoPreview reached its sample cap; disabling automatically");
            stereo_preview_enabled.store(false, std::memory_order_relaxed);
        } else {
            stereo_preview_attempt_count.fetch_add(1, std::memory_order_relaxed);
            if (attempt % 500 == 0) {
                log::info(std::format("StereoPreview extra call attempt={}", attempt));
            }
            inside_injected_extra_call = true;
            const bool ok = safe_call_render_opaque(arg1, real_doom_render_opaque);
            inside_injected_extra_call = false;
            if (!ok) {
                log::error(std::format(
                    "StereoPreview extra call attempt={} threw a structured exception; disabling",
                    attempt));
                stereo_preview_enabled.store(false, std::memory_order_relaxed);
            }
        }
    }
    inside_render_opaque = false;
}

// --- Phase 2.4: offscreen redirect target (F6) -----------------------------
// Builds a VkFramebuffer we own, compatible with a live-captured RenderOpaque
// VkRenderPass, so the F7 extra call's output can be redirected somewhere
// that isn't DOOM's real frame. See docs/RE-NOTES.md and
// docs/HANDOFF-NEXT-SESSION.md for the reasoning; see the OffscreenTarget
// struct above for the risk note on this being a new class of failure mode
// (GPU-level, not CPU-exception-catchable).

PFN_vkGetDeviceProcAddr g_get_device_proc_addr{};

template <typename T>
T load_device_proc(VkDevice device, const char* name) {
    if (!g_get_device_proc_addr) {
        HMODULE vk = GetModuleHandleW(L"vulkan-1.dll");
        g_get_device_proc_addr = vk
            ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(vk, "vkGetDeviceProcAddr"))
            : nullptr;
    }
    return g_get_device_proc_addr
        ? reinterpret_cast<T>(g_get_device_proc_addr(device, name))
        : nullptr;
}

bool find_memory_type(VkPhysicalDeviceMemoryProperties& props, uint32_t type_bits,
    VkMemoryPropertyFlags required, uint32_t& out_index) {
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & required) == required) {
            out_index = i;
            return true;
        }
    }
    return false;
}

bool ensure_composite_holding_ready(VkExtent2D extent, VkFormat format) {
    if (composite_holding.ready.load(std::memory_order_relaxed)) return true;
    if (composite_holding.failed.load(std::memory_order_relaxed)) return false;
    const auto& vk = vulkan_state();
    if (!vk.device || !vk.physical_device) return false;

    auto create_image = load_device_proc<PFN_vkCreateImage>(vk.device, "vkCreateImage");
    auto get_reqs = load_device_proc<PFN_vkGetImageMemoryRequirements>(
        vk.device, "vkGetImageMemoryRequirements");
    auto alloc = load_device_proc<PFN_vkAllocateMemory>(vk.device, "vkAllocateMemory");
    auto bind = load_device_proc<PFN_vkBindImageMemory>(vk.device, "vkBindImageMemory");
    if (!create_image || !get_reqs || !alloc || !bind) {
        log::error("CompositeHolding: could not load required device procs");
        composite_holding.failed.store(true, std::memory_order_relaxed);
        return false;
    }

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {extent.width, extent.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (create_image(vk.device, &ici, nullptr, &composite_holding.image) != VK_SUCCESS) {
        log::error("CompositeHolding: vkCreateImage failed");
        composite_holding.failed.store(true, std::memory_order_relaxed);
        return false;
    }
    VkMemoryRequirements req{};
    get_reqs(vk.device, composite_holding.image, &req);
    VkPhysicalDeviceMemoryProperties mem_props{};
    if (auto get_mem_props = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
            kharvox::native::instanceProc("vkGetPhysicalDeviceMemoryProperties"))) {
        get_mem_props(vk.physical_device, &mem_props);
    }
    uint32_t type_index = UINT32_MAX;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    if (!find_memory_type(mem_props, req.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type_index)) {
        log::error("CompositeHolding: no device-local memory type");
        composite_holding.failed.store(true, std::memory_order_relaxed);
        return false;
    }
    mai.memoryTypeIndex = type_index;
    if (alloc(vk.device, &mai, nullptr, &composite_holding.memory) != VK_SUCCESS ||
        bind(vk.device, composite_holding.image, composite_holding.memory, 0) != VK_SUCCESS) {
        log::error("CompositeHolding: memory allocation/bind failed");
        composite_holding.failed.store(true, std::memory_order_relaxed);
        return false;
    }
    composite_holding.extent = extent;
    composite_holding.format = format;
    composite_holding.ready.store(true, std::memory_order_release);
    log::warn(std::format(
        "CompositeHolding ready image={} {}x{} format={}",
        reinterpret_cast<void*>(composite_holding.image),
        extent.width, extent.height, static_cast<int>(format)));
    return true;
}

// Records the blit into DOOM's live command buffer. Nothing is submitted
// here - it rides along with DOOM's own submission, which is what keeps this
// free of the vkQueueWaitIdle that made the PPM dumps cost 1-2 seconds.
void capture_composite_to_holding() {
    if (last_frame_root_cmd_buffer == VK_NULL_HANDLE ||
        last_frame_root_framebuffer == VK_NULL_HANDLE) return;

    VkImage src = VK_NULL_HANDLE;
    VkFormat src_format = VK_FORMAT_UNDEFINED;
    {
        std::scoped_lock lock(framebuffer_depth_view_mutex);
        auto fb = framebuffer_color0_views.find(last_frame_root_framebuffer);
        if (fb == framebuffer_color0_views.end()) return;
        auto im = image_view_to_image.find(fb->second);
        if (im == image_view_to_image.end()) return;
        src = im->second;
        // The composite's OWN format, for symmetric stereo (STEP 8).
        if (auto vi = image_view_infos.find(fb->second); vi != image_view_infos.end())
            src_format = vi->second.format;
    }
    if (!src) return;

    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    {
        std::unique_lock lock(swapchains_mutex);
        for (const auto& [sc, rec] : swapchains) { (void)sc; extent = rec.extent; format = rec.format; break; }
    }
    // ⚠ THE HOLDING IMAGE MUST MATCH THE MIRROR'S FORMAT, not the swapchain's
    // (2026-08-17, STEP 8).
    //
    // This used to take the SWAPCHAIN's format, which for full-frame mode was
    // fine because both eyes came from it. Symmetric stereo pairs this image
    // (eye 0) against a MIRROR (eye 1), and the mirror is format 122
    // (B10G11R11_UFLOAT). Converting eye 0 to the swapchain's 8-bit format
    // here would send the two eyes down DIFFERENT colour paths into the sRGB
    // XR swapchain - eye 0 through the raw sRGB-typed staging copy, eye 1
    // through a blit that applies an sRGB encode. The eyes would differ in
    // brightness for no reason but our own plumbing, which is the one thing
    // "near-identical quality in both eyes" cannot tolerate.
    //
    // Taking the composite's own format makes both eyes byte-identical in
    // kind, so whatever the XR blit does, it does to both equally.
    if (src_format != VK_FORMAT_UNDEFINED) format = src_format;
    if (!extent.width || !extent.height) return;
    if (!ensure_composite_holding_ready(extent, format)) return;

    const auto& vk = vulkan_state();
    auto cmd_barrier = load_device_proc<PFN_vkCmdPipelineBarrier>(vk.device, "vkCmdPipelineBarrier");
    auto cmd_blit = load_device_proc<PFN_vkCmdBlitImage>(vk.device, "vkCmdBlitImage");
    if (!cmd_barrier || !cmd_blit) return;

    auto barrier = [&](VkImage img, VkImageLayout from, VkImageLayout to,
                       VkAccessFlags srcA, VkAccessFlags dstA) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = srcA; b.dstAccessMask = dstA;
        b.oldLayout = from; b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        cmd_barrier(last_frame_root_cmd_buffer,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    };

    const auto holdingTiming=kharvox::native::beginGpuSpan(last_frame_root_cmd_buffer,"left-holding-copy",0,extent.width,extent.height,0);
    // DOOM's composite is left in COLOR_ATTACHMENT_OPTIMAL by the pass that
    // just wrote it; the holding image's previous contents are disposable, so
    // it transitions from UNDEFINED every frame.
    barrier(src, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    barrier(composite_holding.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT);

    VkImageBlit region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    region.srcOffsets[1] = {static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height), 1};
    region.dstOffsets[1] = region.srcOffsets[1];
    cmd_blit(last_frame_root_cmd_buffer,
        src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        composite_holding.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &region, VK_FILTER_NEAREST);

    // Hand both images back to the layouts their next users expect.
    barrier(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    barrier(composite_holding.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

    kharvox::native::endGpuSpan(holdingTiming);
    const uint32_t n = composite_blit_count.fetch_add(1, std::memory_order_relaxed);
    if (n < 5 || n % 500 == 0) {
        log::warn(std::format("CompositeHolding blit #{} src={} -> holding={}",
            n, reinterpret_cast<void*>(src),
            reinterpret_cast<void*>(composite_holding.image)));
    }
}

// Wipes DOOM's composite target to black between the two frame-root runs, so
// the second run starts from a clean slate instead of compositing on top of
// the first. Recorded inline into DOOM's own command buffer, same as the
// capture blit - no submit, no queue wait.
// ---------------------------------------------------------------------------
// GENERIC FRAMEBUFFER MIRRORING (2026-08-16)
//
// The census says a doubled run touches 7 distinct framebuffers, which is
// small enough to mirror all of them. For each one we build a compatible
// framebuffer over OUR OWN colour images, bound to DOOM's own VkRenderPass.
// The doubled call then renders entirely into our targets - isolated by
// construction, which is the property the full-frame approach could not have.
//
// Depth attachments are deliberately SHARED with DOOM rather than mirrored:
// reusing DOOM's real depth view is what made the original RenderOpaque
// capture produce a complete, correct scene (see docs/RE-NOTES.md
// "Depth-prepass hypothesis"). Private per-eye depth is a later change; one
// variable at a time.
struct MirrorTarget {
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    std::vector<VkImage> images;
    std::vector<VkDeviceMemory> memories;
    std::vector<VkImageView> views;
    // Depth/stencil images we own, kept apart from the colour list because
    // they need a different initial layout (DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    // initialLayout=3 in DOOM's own render passes) than the colour ones.
    std::vector<VkImage> depth_images;
    VkImage color0 = VK_NULL_HANDLE;
    VkFormat color0_format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    bool ok = false;
};
std::unordered_map<VkFramebuffer, MirrorTarget> mirror_targets;
// RESOURCE LIFETIME (2026-08-17). Guards mirror_targets, mirrored_views,
// mirror_shared_source_views and mirrored_view_refs.
//
// Until now these were touched only from the render thread inside the
// doubled call, so no lock existed. Destroy hooks change that: DOOM tears
// framebuffers and views down during level transitions and resolution
// changes, potentially from another thread, and a teardown racing a doubled
// call would be a use-after-free rather than merely a stale read.
//
// LOCK ORDER, wherever more than one is held:
//   mirror_mutex  ->  swapchains_mutex  ->  framebuffer_depth_view_mutex
// Never acquire mirror_mutex while already holding either of the others.
std::mutex mirror_mutex;
// OWNERSHIP. mirrored_views owns the mirror image/memory/view for a source
// view; a MirrorTarget owns ONLY its own VkFramebuffer. That split is forced
// by the aliasing design - one mirrored view is deliberately shared by
// several mirror framebuffers so DOOM's own sharing topology survives - so
// destroying a framebuffer must NOT destroy the views it points at. This
// refcount is how we know when the last user is gone: incremented for every
// mirror framebuffer that references a source view (created or cached),
// decremented when such a framebuffer is destroyed.
std::unordered_map<VkImageView, kharvox::native::MirrorFramebufferReferences> mirrored_view_refs;

// MIRRORED VIEW CACHE, keyed on DOOM's ORIGINAL VkImageView.
//
// DOOM aliases one image view across several framebuffers - the same depth
// view is view[0] of its depth-only framebuffers and view[3] of its
// 4-attachment G-buffer ones (verified in the live vkCreateFramebuffer log).
// Allocating a private mirror per framebuffer destroys that aliasing:
// RenderDepth's prepass lands in one private depth image and RenderOpaque
// then depth-tests against a different, empty one, so every fragment fails
// and eye 1 renders BLACK. That is exactly the "private depth came back
// black" result this project recorded before, and its real cause.
//
// Keying on the source view makes the mirror set reproduce DOOM's own
// sharing topology: same original view in, same mirrored view out.
// memory/size added 2026-08-17: mirrored_views is the OWNER of the mirror
// image, so it must carry the allocation needed to free it. It used to live
// only in MirrorTarget::memories, a per-framebuffer vector, which cannot
// express "this image is shared by three framebuffers, free it when the last
// one goes" and so could never have been freed correctly.
struct MirroredView {
    VkImage image;
    VkImageView view;
    VkFormat format;
    VkDeviceMemory memory;
    VkDeviceSize size;
    // Extent added 2026-08-17 for STEP 6.6's attribution lines. A gap or
    // fallback identity is only actionable if you can tell a 3818x2032
    // full-screen input apart from a 64x64 lookup table, and nothing else in
    // this file records an image's size against its handle.
    VkExtent2D extent;
    uint32_t mips{1}, layers{1};
};
std::unordered_map<VkImageView, MirroredView> mirrored_views;
// Source views we DELIBERATELY leave shared rather than mirroring - today
// just the light-space shadow atlas (extent > 8192, see the branch in
// ensure_mirror_framebuffer). Kept so DescriptorCensus can tell "shared on
// purpose, view-independent, leave alone" apart from "we have a mirror for
// this and a read of the original is therefore reading DOOM's unshifted
// data" - the two have opposite implications for STEP 6's redirection.
std::unordered_set<VkImageView> mirror_shared_source_views;
// Source VkImage -> mirror VkImage (2026-08-17, after the STEP 6 hang).
//
// Mirroring substituted framebuffers but never BARRIERS. DOOM writes its
// G-buffer and then transitions ITS OWN images COLOR_ATTACHMENT_OPTIMAL ->
// SHADER_READ_ONLY_OPTIMAL before the deferred pass samples them. Our mirror
// images are named in no barrier at all - they were transitioned once at
// creation and never again - so a clone binding a mirror view while
// declaring DOOM's SHADER_READ_ONLY_OPTIMAL had the shader sample an image
// that was in a different layout. That is undefined behaviour, and it hung
// the GPU on the first frame of the STEP 6 pilot.
//
// Keyed on image rather than view because barriers name images.
std::unordered_map<VkImage, VkImage> mirrored_images;
// CURRENT LAYOUT of each mirror image (2026-08-17, after eye 1 went black).
//
// The XR present blit hardcoded VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL as
// the mirror's layout. That was TRUE while mirrors received no barriers -
// they were transitioned once at creation and DOOM's render passes left
// them there. The moment barrier substitution started applying DOOM's real
// transitions to them, it stopped being true, and blitting from an image
// while declaring a layout it is not in is undefined - which is how eye 1
// went black.
//
// Two things move a mirror's layout and both are tracked:
//   - barriers we substitute            (hook_cmd_pipeline_barrier)
//   - render pass finalLayout           (the mirror block in
//                                        hook_cmd_begin_render_pass)
// Seeded at creation with what transition_mirror_images_to_color_attachment
// actually puts them in.
std::unordered_map<VkImage, VkImageLayout> mirror_image_layouts;
#include "../../../../NativeImageLayouts.inc"
// LAYOUT REWIND (2026-08-18). DOOM's OWN images' layouts, and a snapshot of
// them taken at the start of each real frame root.
//
// Why this is needed at all: the doubled call records into the SAME command
// buffer as the real one, immediately after it, so it inherits DOOM's images
// in their END-of-frame-root layouts rather than their start-of-frame-root
// ones. The doubled call then re-issues DOOM's own barriers, which assume the
// starting state - so every mirror is one frame-root out of step.
//
// It surfaced as a direct contradiction: the composite needed
// SHADER_READ_ONLY and the storage images needed GENERAL, from the same
// mirror set, at the same moment. MirrorLockstep resolved that in the
// composite's favour and thereby broke the storage writes (have=5 want=1 at
// passes 16 and 22) - one image cannot hold two layouts, and no per-image
// patch can fix that. Rewinding the whole set to the layout DOOM's images
// actually had when the real call began removes the contradiction instead of
// choosing a side.
std::unordered_map<VkImage, VkImageLayout> doom_image_layouts;
std::unordered_map<VkImage, VkImageLayout> frame_start_layouts;
std::atomic_uint32_t layout_rewind_barriers{0};
std::atomic_uint32_t layout_rewind_calls{0};
// REWIND ATTRIBUTION - see the counted loop in rewind_mirror_layouts.
std::atomic_uint32_t g_rw_considered{0};
std::atomic_uint32_t g_rw_no_snapshot{0};
std::atomic_uint32_t g_rw_already_ok{0};
std::atomic_uint32_t g_rw_undefined_target{0};
// Mirror images that a mirrored render pass has actually rendered into.
//
// Mirrors are created LAZILY, on first use, so the first doubled call
// creates them progressively - 49 of them, interleaved with the descriptor
// clones, all inside attempt 0. A mirror created at pass 45 has never been
// written and is still sitting in the layout we seeded at creation, while
// the descriptor we clone from DOOM declares SHADER_READ_ONLY_OPTIMAL.
// Sampling an image in a layout it is not in is undefined, and that is what
// hung the GPU on attempt 0 - twice.
//
// The obvious fix, emitting a transition barrier at bind time, is ILLEGAL:
// most binds happen inside a render pass, where an image layout transition
// requires a subpass self-dependency DOOM did not declare. So instead a
// mirror is only substituted once it is genuinely safe - written by a
// mirrored pass, and tracked as being in the layout the descriptor asks
// for. Anything else falls back to DOOM's own view for that binding, which
// costs parallax on that input and cannot hang.
std::unordered_set<VkImage> mirror_images_written;
// H2: per-frame, doubled-call-only. See snapshot_frame_start_layouts for why
// mirror_images_written cannot answer this. Guarded by mirror_mutex.
std::unordered_set<VkImage> h2_doubled_written_this_frame;
// ALIASED SAMPLING VIEWS (2026-08-17, STEP 6.7a).
//
// mirrored_views is keyed on the exact VkImageView DOOM used as a framebuffer
// ATTACHMENT. A sampling view over the same image is a DIFFERENT handle, so
// it misses that lookup and is left pointing at DOOM's real, unshifted image
// even though we hold a perfectly good mirror of it.
//
// Run 19 measured what that costs, with identity rather than counts: 67 such
// binds, EVERY ONE of them a full-screen 3818x2032 image, and 60 of the 67
// are ONE view handle bound at set 0 binding 4 - the primary G-buffer input
// of the lighting job (0x19AE9E0, 22 hits), the deferred chain (0x1A39930,
// 20 hits) and the final composite pass (0x186A2F0, 4 hits). That single
// view never appears as an attachment anywhere in the log. It is exactly the
// "one bind carries the whole image" case the review predicted, and dividing
// 67 by 21,542 total binds could never have shown it.
//
// Fix: key the substitution by the underlying IMAGE. On a gap, build (once,
// cached) a mirror-side view with the SAME create parameters as the sampling
// view, over the mirror image. That closes the entire alias class rather
// than one instance.
//
// Guarded by mirror_mutex, like every other mirror map, so the substitution
// path that already holds it needs no additional lock (Rule 9).
struct AliasMirrorView {
    VkImageView view;
    VkImage mirror_image;
    uint32_t generation;
};
std::unordered_map<VkImageView, AliasMirrorView> alias_mirror_views;
VkImageView descriptor_mirror_view(VkImageView source,const MirroredView& mirror,VkDevice device,uint32_t generation){
 auto vi=image_view_infos.find(source);if(vi==image_view_infos.end())kharvox::native::fail("descriptor source view metadata missing");
 const auto range=checked_mirror_range(mirror.image,vi->second.range);
 if(range.baseMipLevel==0&&range.levelCount==1&&range.baseArrayLayer==0&&range.layerCount==1)return mirror.view;
 auto cached=alias_mirror_views.find(source);
 if(cached!=alias_mirror_views.end()){if(cached->second.mirror_image!=mirror.image)kharvox::native::fail("alias view image changed before retirement");return cached->second.view;}
 VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};ci.flags=vi->second.flags;ci.image=mirror.image;ci.viewType=vi->second.view_type;ci.format=vi->second.format;ci.components=vi->second.components;ci.subresourceRange=range;
 VkImageView view{};if(real_create_image_view(device,&ci,nullptr,&view)!=VK_SUCCESS)kharvox::native::fail("full-range mirror descriptor view creation failed");
 alias_mirror_views[source]={view,mirror.image,generation};return view;
}
std::atomic_uint32_t alias_view_substitutions{0};
std::atomic_uint32_t alias_view_created{0};
std::atomic_uint32_t alias_view_failed{0};
std::atomic_uint32_t alias_view_gated{0};
std::atomic_uint32_t alias_view_no_info{0};

// P1 arm-time snapshot - see the block at kP1ParticleSimJobRva.
//
// Each lock is taken and RELEASED before the next. mirror_mutex ->
// framebuffer_depth_view_mutex is the documented order anyway, but nesting is
// avoided outright: the census records that adding a third lock on the render
// thread underneath the other two is what wedged run 17, and this runs on the
// render thread.
//
// The counts are logged unconditionally and are half of this instrument's
// positive control: a classifier built from EMPTY sets would file every read
// as OTH and report rdMIR=0 on both sides, which is exactly what "redirection
// is identical in both arms" looks like. Zero here means the run says nothing.
void p1_build_snapshot() {
    p1_mirror_images.clear();
    p1_source_images.clear();
    p1_mirror_views.clear();
    p1_view_image.clear();
    p1_volume_views.clear();
    p1_rva_count = 0;
    p1_rva_overflow = 0;
    p1_detail_lines.store(0, std::memory_order_relaxed);
    p1_light_sets_real_count = 0;
    p1_light_sets_doubled_count = 0;
    std::memset(p1_rva_keys, 0, sizeof(p1_rva_keys));
    std::memset(p1_real_asked_rva, 0, sizeof(p1_real_asked_rva));
    std::memset(p1_doubled_asked_rva, 0, sizeof(p1_doubled_asked_rva));
    std::memset(p1_doubled_bound_rva, 0, sizeof(p1_doubled_bound_rva));
    // Source views we hold a mirror for, resolved to images below.
    std::vector<VkImageView> source_views;
    {
        std::scoped_lock lock(mirror_mutex);
        source_views.reserve(mirrored_views.size());
        for (const auto& [src_view, mv] : mirrored_views) {
            source_views.push_back(src_view);
            if (mv.image != VK_NULL_HANDLE) p1_mirror_images.insert(mv.image);
            if (mv.view != VK_NULL_HANDLE) p1_mirror_views.insert(mv.view);
        }
        for (const auto& [src_img, mir_img] : mirrored_images) {
            if (src_img != VK_NULL_HANDLE) p1_source_images.insert(src_img);
            if (mir_img != VK_NULL_HANDLE) p1_mirror_images.insert(mir_img);
        }
        for (const auto& [src_view, alias] : alias_mirror_views) {
            (void)src_view;
            if (alias.mirror_image != VK_NULL_HANDLE) p1_mirror_images.insert(alias.mirror_image);
            if (alias.view != VK_NULL_HANDLE) p1_mirror_views.insert(alias.view);
        }
    }
    {
        std::scoped_lock lock(framebuffer_depth_view_mutex);
        p1_view_image.reserve(image_view_infos.size());
        for (const auto& [view, info] : image_view_infos) {
            p1_view_image[view] = info.image;
            if (info.view_type == VK_IMAGE_VIEW_TYPE_3D) p1_volume_views.insert(view);
        }
    }
    // Locks released. Resolve the mirrored-view keys to their source images so
    // the classifier can work by IMAGE - MISS-KEYING=2 reproduces in every run,
    // and a view-keyed test would file those reads as OTH.
    for (VkImageView v : source_views) {
        if (const auto it = p1_view_image.find(v); it != p1_view_image.end())
            p1_source_images.insert(it->second);
    }
    // A source image that is ALSO one of our mirrors cannot be classified, and
    // would silently bias the discriminator. Nothing should be in both; if
    // anything is, say so rather than pick a side.
    uint32_t both = 0;
    for (VkImage img : p1_source_images) if (p1_mirror_images.count(img)) ++both;
    log::warn(std::format(
        "P1 SNAPSHOT: mirrorImages={} mirrorViews={} sourceImages={} viewsResolved={} "
        "volumeViews={} inBoth={}. THE CLASSIFIER'S OWN CONTROL: any of the first four at zero "
        "means reads classify as OTH, rdMIR reads 0 on BOTH sides, and that is instrument "
        "failure - not 'redirection is identical in both arms'. mirrorViews is the field run 1 "
        "did not have: our mirror views are made with real_create_image_view and are absent "
        "from image_view_infos, so an image-only classifier calls every substituted read OTH. "
        "inBoth>0 means a handle is recorded as both a mirror and a source and the MIR/REAL "
        "split is unsafe.",
        p1_mirror_images.size(), p1_mirror_views.size(), p1_source_images.size(),
        p1_view_image.size(), p1_volume_views.size(), both));
}
// STEP 6.7d: stale per-set content records dropped at (re)allocation - see
// hook_allocate_descriptor_sets.
std::atomic_uint32_t stale_set_records_cleared{0};
// Bumped whenever the mirror set changes in a way that could make a cached
// clone's substitution decisions stale - a new mirror, or a mirror becoming
// written for the first time. Clones record the generation they were built
// at and are rebuilt when it moves, so substitution converges over the first
// frames instead of being frozen at whatever was safe on frame one.
std::atomic_uint32_t mirror_generation{0};

// MIRROR CONVERGENCE (2026-08-17). The permanent replacement for "wait 300
// frames and hope", which the plan has carried as an unbuilt TODO since the
// frame-0 hangs.
//
// mirror_generation moves on exactly the two events that make redirection
// unsafe: a new mirror appearing, and a mirror being written for the first
// time. So "generation has not moved for N consecutive doubled calls" IS the
// convergence condition, measured rather than assumed.
//
// The 300-frame constant was ~75x too conservative and hid its own basis.
// Measured from run 26's log: all 53 mirrors were created between
// 21:01:02.997 and 21:01:03.057 - 60 MILLISECONDS, about five frames - and
// redirection then waited a further 3.7 seconds for no reason anyone could
// state.
//
// The reason this matters beyond startup: a frame count can only ever
// protect the FIRST convergence. DOOM tears framebuffers down on level
// transitions and resolution changes, and the mirror set then rebuilds mid-
// session with no keypress and no scripted step to defer anything. A
// condition re-arms itself; a constant does not. Redirection now pauses
// automatically whenever the mirror set is in flux and resumes when it
// settles, so the worst case at a level change is a few frames of eye 1
// without parallax instead of a GPU hang.
constexpr uint32_t kMirrorStableCalls = 8;
std::atomic_uint32_t mirror_stable_calls{0};
std::atomic_uint32_t mirror_last_seen_generation{0};
std::atomic_bool mirror_converged_logged{false};

// ===================== MIRROR HARD RESET (2026-08-24) ======================
//
// THE FIX FOR "ONCE VR IS GONE THERE IS NO BRINGING IT BACK".
//
// The observed facts: at the cutscene the two eyes go byte-identical (mono),
// and a full teardown + re-arm - SHIFT+DOWN, three times, each reaching
// gradeRung=2 with the PROJECTION layer back and hundreds of draws at a
// doubled/real ratio of 1.00 - does NOT restore stereo. If re-arming every
// flag we own changes nothing, the broken thing is not a flag. It is a
// resource that the re-arm deliberately does not rebuild.
//
// And there is exactly one: THE MIRRORS. The fast-VR exit was written to leave
// them standing precisely so re-entry could be instant, and nothing anywhere
// rebuilds them once created. A cutscene is a scene transition; DOOM tears
// down and recreates framebuffers and image views across one. Our mirror maps
// are keyed on the OLD objects, so after the transition eye 1 renders through
// mirrors that no longer correspond to anything the game is drawing into - and
// no amount of re-arming can notice, because every flag is correct.
//
// WHAT THIS DOES, AND WHAT IT DELIBERATELY DOES NOT DO. It clears the lookup
// maps so the next doubled call creates FRESH mirrors for whatever framebuffers
// and views are current, bumps the generation so convergence has to be
// re-established before redirection resumes, and invalidates the redirect
// cache.
//
// It does NOT call vkDestroyImage on the old mirrors. That is intentional:
// they may still be bound to in-flight framebuffers, and destroying a bound
// image is how this project has hung the GPU before. The old images LEAK.
// That is a real cost - a few MB per reset - and it is the right trade for a
// key pressed rarely to recover a session that is otherwise unusable.
void mirror_hard_reset(const char* why) {
    size_t targets = 0, images = 0, views = 0;
    {
        std::scoped_lock lock(mirror_mutex);
        targets = mirror_targets.size();
        images = mirrored_images.size();
        views = mirrored_views.size();
        mirror_targets.clear();
        mirrored_images.clear();
        mirrored_views.clear();
        mirrored_view_refs.clear();
        mirror_shared_source_views.clear();
        alias_mirror_views.clear();
        // The cumulative "has this mirror ever been written" safety gate must
        // go with them: left populated it would vouch for images that no
        // longer exist, which is worse than not having it at all.
        mirror_images_written.clear();
        mirror_image_layouts.clear();
        frame_start_layouts.clear();
    }
    // Force re-convergence. Both halves, because mirrors_converged() reads the
    // count and update_mirror_convergence() compares against the last-seen
    // generation - resetting one without the other leaves them disagreeing.
    mirror_generation.fetch_add(1, std::memory_order_relaxed);
    mirror_stable_calls.store(0, std::memory_order_relaxed);
    mirror_converged_logged.store(false, std::memory_order_relaxed);
    invalidate_read_redirect_cache(why);
    log::warn(std::format(
        "MIRROR HARD RESET ({}): dropped {} framebuffer targets, {} image mirrors and {} view "
        "mirrors. Fresh ones are built for the CURRENT scene on the next doubled call, and "
        "redirection stays off until convergence is re-established. The old images are leaked "
        "rather than destroyed - they may still be bound - so this costs a few MB per use.",
        why, targets, images, views));
}

bool mirrors_converged() {
    return mirror_stable_calls.load(std::memory_order_relaxed) >= kMirrorStableCalls;
}

// Called once at the end of every doubled call, on the render thread.
void update_mirror_convergence(bool preparationOnly) {
    const uint32_t gen = mirror_generation.load(std::memory_order_relaxed);
    if (gen != mirror_last_seen_generation.exchange(gen, std::memory_order_relaxed)) {
        // The set moved. Any convergence we had is void - say so if we had
        // announced it, or a silent regression looks like a rendering fault.
        if (mirror_converged_logged.exchange(false, std::memory_order_relaxed)) {
            log::warn(std::format(
                "MirrorConvergence LOST: mirror set changed (generation now {}). Read "
                "redirection PAUSES until it settles again - expect eye 1 to lose parallax for "
                "a few frames rather than sample a mirror that is not ready.", gen));
        }
        mirror_stable_calls.store(0, std::memory_order_relaxed);
        return;
    }
    const uint32_t stable = mirror_stable_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (stable == kMirrorStableCalls &&
        !mirror_converged_logged.exchange(true, std::memory_order_relaxed)) {
        log::warn(std::format(
            "MirrorConvergence REACHED: generation {} unchanged for {} consecutive {} "
            "calls; allocation set stable (contents validated separately).", gen, kMirrorStableCalls,preparationOnly?"diagnostic preparation":"doubled"));
    }
}
// Attachment finalLayouts per render pass. Parsed for logging since forever
// (log_render_pass_attachments) but never stored, so nothing could know what
// layout a pass leaves its attachments in.
std::mutex render_pass_layouts_mutex;
std::unordered_map<VkRenderPass, std::vector<VkImageLayout>> render_pass_final_layouts;
// Entry layouts, recorded for the real call's begin/end delta - see the
// population site in hook_create_render_pass.
std::unordered_map<VkRenderPass, std::vector<VkImageLayout>> render_pass_initial_layouts;

// Current layout of a mirror image, defaulting to what creation leaves it
// in. Caller must hold mirror_mutex.
VkImageLayout mirror_layout_of(VkImage mirror) {
    if (auto it = mirror_image_layouts.find(mirror); it != mirror_image_layouts.end())
        return it->second;
    return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}

// LAYOUT REWIND (2026-08-18) - see doom_image_layouts for why this exists.
// Defined here, below the mirror maps it walks; declared above hook_frame_root.
void snapshot_frame_start_layouts() {
    std::scoped_lock lock(mirror_mutex);
    // H2 (2026-08-26 run 12 correction). mirror_images_written is a CUMULATIVE
    // session-wide safety gate - "has this mirror ever been written, so is it
    // safe to substitute" - and is never cleared. Reading it as "the doubled
    // call rendered this target THIS FRAME" was wrong, and made run 12's flag
    // inconclusive: every pair reported YES because every mirror had been
    // written at some point since launch.
    //
    // This set answers the question actually asked. Cleared at the start of
    // every real frame root and filled only while inside_frame_root_double, so
    // at present time it holds exactly the mirrors the doubled call rendered
    // into during the frame H1 censuses.
    h2_doubled_written_this_frame.clear();
    frame_start_layouts.clear();
    for (const auto& [src, mirror] : mirrored_images) {
        (void)mirror;
        if (auto it = doom_image_layouts.find(src); it != doom_image_layouts.end())
            frame_start_layouts[src] = it->second;
    }
}

void rewind_mirror_layouts(VkCommandBuffer command_buffer) {
    if (command_buffer == VK_NULL_HANDLE || !real_cmd_pipeline_barrier) return;
    std::vector<VkImageMemoryBarrier> rewind;
    {
        std::scoped_lock lock(mirror_mutex);
        for (const auto& [src, mirror] : mirrored_images) {
            // EVERY EXIT COUNTED (2026-09-03). This loop had three bare
            // `continue`s and the log printed only "N mirror(s) restored", so
            // "nothing needed rewinding" was indistinguishable from "no
            // start-of-frame layout was ever recorded for these images" - the
            // second being a broken snapshot masquerading as a healthy no-op.
            g_rw_considered.fetch_add(1, std::memory_order_relaxed);
            auto want = frame_start_layouts.find(src);
            if (want == frame_start_layouts.end()) {
                g_rw_no_snapshot.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            const VkImageLayout have = mirror_layout_of(mirror);
            if (have == want->second) {
                g_rw_already_ok.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (want->second == VK_IMAGE_LAYOUT_UNDEFINED) {
                g_rw_undefined_target.fetch_add(1, std::memory_order_relaxed);
                continue;  // nothing to restore to
            }
            // Depth mirrors need the depth aspect, colour ones the colour
            // aspect. Derived from the mirrored view's format rather than
            // assumed: a wrong aspect is an invalid barrier, not a cosmetic
            // slip.
            VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
            for (const auto& [view, mv] : mirrored_views) {
                (void)view;
                if (mv.image != mirror) continue;
                if (mv.format >= VK_FORMAT_D16_UNORM && mv.format <= VK_FORMAT_D32_SFLOAT_S8_UINT)
                    aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
                break;
            }
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.oldLayout = have;
            b.newLayout = want->second;
            b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = mirror;
            b.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
            append_mirror_transitions(rewind,b);
        }
    }
    if (rewind.empty()) return;
    real_cmd_pipeline_barrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
        static_cast<uint32_t>(rewind.size()), rewind.data());
    layout_rewind_barriers.fetch_add(static_cast<uint32_t>(rewind.size()),
                                     std::memory_order_relaxed);
    const uint32_t n = layout_rewind_calls.fetch_add(1, std::memory_order_relaxed);
    if (n < 5 || (n % 600) == 0) {
        log::warn(std::format(
            "LayoutRewind #{}: {} mirror(s) restored to their start-of-frame layouts before the "
            "doubled call (first: mirror {} {} -> {}). Without this the doubled call inherits "
            "END-of-frame-root layouts while re-issuing barriers that describe the START, which "
            "is why the composite and the storage images demanded contradictory layouts from the "
            "same mirror set.",
            n, rewind.size(), reinterpret_cast<void*>(rewind[0].image),
            static_cast<int>(rewind[0].oldLayout), static_cast<int>(rewind[0].newLayout)));
    }
}

// CHAIN WALK snapshot recording - see the ChainWalk block near the census
// globals for why this exists. Called from hook_cmd_end_render_pass, i.e.
// OUTSIDE any render pass, which is where an image layout transition is
// legal (inside one it would need a subpass self-dependency DOOM never
// declared - the same constraint that shaped the safety gate).
//
// Everything here is private: our own image, our own memory, barriers that
// put the mirror back in exactly the layout the render pass left it in. The
// only thing touched that DOOM owns is the command buffer, which receives a
// barrier/copy/barrier triple it would not otherwise have.
//
// Caller must hold chainwalk_mutex.
bool chainwalk_ensure_slot_image(ChainWalkSlot& slot) {
    if (slot.image != VK_NULL_HANDLE) return true;
    if (slot.create_failed) return false;
    const auto& vk = vulkan_state();
    auto create_image = load_device_proc<PFN_vkCreateImage>(vk.device, "vkCreateImage");
    auto get_reqs = load_device_proc<PFN_vkGetImageMemoryRequirements>(
        vk.device, "vkGetImageMemoryRequirements");
    auto allocate = load_device_proc<PFN_vkAllocateMemory>(vk.device, "vkAllocateMemory");
    auto bind = load_device_proc<PFN_vkBindImageMemory>(vk.device, "vkBindImageMemory");
    if (!vk.device || !create_image || !get_reqs || !allocate || !bind) {
        slot.create_failed = true;
        log::error("ChainWalk: required device procs unavailable; this slot will not snapshot");
        return false;
    }
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = slot.format;
    ici.extent = {slot.extent.width, slot.extent.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    // DST for the pass-boundary copy, SRC because dump_image_snapshot copies
    // out of it again into its host-visible staging image.
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkImage img = VK_NULL_HANDLE;
    if (create_image(vk.device, &ici, nullptr, &img) != VK_SUCCESS) {
        slot.create_failed = true;
        log::error(std::format(
            "ChainWalk: vkCreateImage failed for jobRva={:#x} {}x{} format={}",
            slot.rva, slot.extent.width, slot.extent.height, static_cast<int>(slot.format)));
        return false;
    }
    VkMemoryRequirements req{};
    get_reqs(vk.device, img, &req);
    VkPhysicalDeviceMemoryProperties mem_props{};
    if (auto gmp = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
            kharvox::native::instanceProc("vkGetPhysicalDeviceMemoryProperties")))
        gmp(vk.physical_device, &mem_props);
    uint32_t type_index = UINT32_MAX;
    if (!find_memory_type(mem_props, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                          type_index)) {
        slot.create_failed = true;
        log::error(std::format("ChainWalk: no device-local memory type for jobRva={:#x}", slot.rva));
        return false;
    }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type_index;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (allocate(vk.device, &mai, nullptr, &mem) != VK_SUCCESS ||
        bind(vk.device, img, mem, 0) != VK_SUCCESS) {
        slot.create_failed = true;
        log::error(std::format(
            "ChainWalk: allocate/bind failed for jobRva={:#x} size={} bytes", slot.rva, req.size));
        return false;
    }
    slot.image = img;
    slot.memory = mem;
    const uint64_t total = chainwalk_bytes_allocated.fetch_add(req.size,
                                                               std::memory_order_relaxed) + req.size;
    log::warn(std::format(
        "ChainWalk: snapshot image created for jobRva={:#x} {}x{} format={} image={} "
        "bytes={} totalChainWalkBytes={}",
        slot.rva, slot.extent.width, slot.extent.height, static_cast<int>(slot.format),
        reinterpret_cast<void*>(img), req.size, total));
    return true;
}

// REAL-CALL timeline copy. Same barrier/copy/restore discipline as
// chainwalk_record_snapshot - in particular the source is put back in exactly
// the layout the render pass guarantees, so the next pass's declared
// initialLayout stays true.
void real_timeline_record_snapshot(VkCommandBuffer command_buffer) {
    if (!pass_timeline_armed.load(std::memory_order_relaxed)) return;
    if (real_timeline_current_color == VK_NULL_HANDLE) return;
    if (real_timeline_current_cmd != command_buffer) return;
    if (!real_timeline_layout_known) {
        log::warn(std::format(
            "PassTimeline REAL pass={} SKIPPED - the render pass declared no final layout we "
            "recorded, and copying from a guessed layout is the class of bug that hung the GPU "
            "three times. No file, rather than a wrong one.", real_pass_seq));
        return;
    }
    uint32_t index = kChainWalkSlots;
    for (uint32_t s = 0; s < kTimelinePasses.size(); ++s)
        if (kTimelinePasses[s] == real_pass_seq) index = s;
    if (index >= kTimelinePasses.size()) return;
    const auto& vk = vulkan_state();
    auto cmd_barrier = load_device_proc<PFN_vkCmdPipelineBarrier>(vk.device, "vkCmdPipelineBarrier");
    auto cmd_copy = load_device_proc<PFN_vkCmdCopyImage>(vk.device, "vkCmdCopyImage");
    if (!cmd_barrier || !cmd_copy) return;

    std::scoped_lock lock(chainwalk_mutex);
    RealTimelineSlot& slot = real_timeline_slots[index];
    if (slot.image == VK_NULL_HANDLE && !slot.create_failed) {
        slot.extent = real_timeline_current_extent;
        slot.format = real_timeline_current_format;
        ChainWalkSlot tmp{};
        tmp.extent = slot.extent;
        tmp.format = slot.format;
        if (chainwalk_ensure_slot_image(tmp)) {
            slot.image = tmp.image;
            slot.memory = tmp.memory;
        } else {
            slot.create_failed = true;
            log::error(std::format("PassTimeline REAL slot={} image creation failed", index));
            return;
        }
    }
    if (slot.image == VK_NULL_HANDLE) return;
    if (slot.extent.width != real_timeline_current_extent.width ||
        slot.format != real_timeline_current_format) return;

    const VkImage src = real_timeline_current_color;
    const VkImageLayout layout = real_timeline_current_layout;
    VkImageMemoryBarrier to_src{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_src.oldLayout = layout;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.image = src;
    to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = slot.image;
    to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    std::array<VkImageMemoryBarrier, 2> pre{to_src, to_dst};
    cmd_barrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
        static_cast<uint32_t>(pre.size()), pre.data());
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {slot.extent.width, slot.extent.height, 1};
    cmd_copy(command_buffer, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        slot.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    VkImageMemoryBarrier restore{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    restore.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    restore.newLayout = layout;
    restore.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    restore.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    restore.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    restore.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    restore.image = src;
    restore.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    cmd_barrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &restore);
    slot.filled = true;
    slot.filled_pass = real_pass_seq;
    log::warn(std::format(
        "PassTimeline [gpu] REAL slot={} pass={} src={} {}x{} format={} srcLayout={}",
        index, real_pass_seq, reinterpret_cast<void*>(src), slot.extent.width,
        slot.extent.height, static_cast<int>(slot.format), static_cast<int>(layout)));
}

void chainwalk_record_snapshot(VkCommandBuffer command_buffer) {
    const uint32_t attempt = chainwalk_capture_attempt;
    if (attempt == UINT32_MAX) return;
    if (chainwalk_current_color == VK_NULL_HANDLE) return;
    if (chainwalk_current_cmd != command_buffer) {
        const uint32_t n = chainwalk_cmd_mismatches.fetch_add(1, std::memory_order_relaxed);
        if (n < 10) {
            log::error(std::format(
                "ChainWalk: vkCmdEndRenderPass on command buffer {} but the open pass we tracked "
                "was recorded into {} - interleaved render passes across command buffers. "
                "Snapshot SKIPPED rather than attributed to the wrong stage.",
                reinterpret_cast<void*>(command_buffer),
                reinterpret_cast<void*>(chainwalk_current_cmd)));
        }
        return;
    }
    // The RVA captured at BEGIN - see chainwalk_current_rva for why reading
    // doubled_current_job_rva here silently lost two of run 19's three
    // snapshots.
    const uintptr_t rva = chainwalk_current_rva;
    const uint32_t pass = doubled_pass_seq;
    const VkImage src = chainwalk_current_color;
    const VkFormat format = chainwalk_current_format;
    const VkExtent2D extent = chainwalk_current_extent;
    const VkImageLayout layout = chainwalk_current_layout;
    const auto& vk = vulkan_state();
    auto cmd_barrier = load_device_proc<PFN_vkCmdPipelineBarrier>(vk.device, "vkCmdPipelineBarrier");
    auto cmd_copy = load_device_proc<PFN_vkCmdCopyImage>(vk.device, "vkCmdCopyImage");
    if (!cmd_barrier || !cmd_copy) return;
    const bool timeline = pass_timeline_armed.load(std::memory_order_relaxed);
    std::scoped_lock lock(chainwalk_mutex);
    for (uint32_t s = 0; s < kChainWalkSlots; ++s) {
        ChainWalkSlot& slot = chainwalk_slots[s];
        if (timeline) {
            // PASS-KEYED. The whole point of the timeline is that these copies
            // execute in GPU ORDER at the pass boundaries, so the four dumps
            // are four moments in one frame - unlike the effects_stage dumps,
            // which are four DIFFERENT IMAGES all read at present time. That
            // distinction is what three runs of this session got wrong.
            if (pass != kTimelinePasses[s]) continue;
            if (!slot.observed) {
                slot.observed = true;
                slot.rva = rva;
                slot.observed_pass = pass;
                slot.extent = extent;
                slot.format = format;
            }
        } else if (!slot.observed || slot.rva != rva) {
            continue;
        }
        if (slot.extent.width != extent.width || slot.extent.height != extent.height ||
            slot.format != format) {
            // Last-write-wins within a slot is only sound while every write is
            // the same size and format. A mismatch is skipped and said aloud
            // rather than silently producing a snapshot of a bloom mip.
            const uint32_t n = chainwalk_copy_lines.fetch_add(1, std::memory_order_relaxed);
            if (n < 40) {
                log::warn(std::format(
                    "ChainWalk SKIP slot={} pass={} jobRva={:#x}: pass is {}x{} format={} but the "
                    "slot was observed as {}x{} format={}",
                    s, pass, rva, extent.width, extent.height, static_cast<int>(format),
                    slot.extent.width, slot.extent.height, static_cast<int>(slot.format)));
            }
            continue;
        }
        if (!chainwalk_ensure_slot_image(slot)) continue;
        VkImageMemoryBarrier to_src{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        to_src.oldLayout = layout;
        to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        to_src.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_src.image = src;
        to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        // UNDEFINED, not the previous attempt's layout: the whole image is
        // overwritten, so its old contents are worthless and this removes a
        // whole class of layout-tracking bug from a diagnostic.
        to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_dst.image = slot.image;
        to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        std::array<VkImageMemoryBarrier, 2> pre{to_src, to_dst};
        cmd_barrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
            static_cast<uint32_t>(pre.size()), pre.data());
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.extent = {extent.width, extent.height, 1};
        cmd_copy(command_buffer, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            slot.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        // Put the mirror back exactly where the render pass left it. Anything
        // else and the next pass's declared initialLayout is a lie - the same
        // failure mode that hung the GPU three times this stream.
        VkImageMemoryBarrier restore{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        restore.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        restore.newLayout = layout;
        restore.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        restore.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        restore.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        restore.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        restore.image = src;
        restore.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        cmd_barrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &restore);
        slot.filled_attempt = attempt;
        slot.filled_pass = pass;
        slot.filled_source = src;
        const uint32_t n = chainwalk_copy_lines.fetch_add(1, std::memory_order_relaxed);
        if (n < 40) {
            log::warn(std::format(
                "ChainWalk COPY slot={} attempt={} pass={} jobRva={:#x} src={} -> snapshot={} "
                "{}x{} format={} srcLayout={}",
                s, attempt, pass, rva, reinterpret_cast<void*>(src),
                reinterpret_cast<void*>(slot.image), extent.width, extent.height,
                static_cast<int>(format), static_cast<int>(layout)));
        }
    }
}

// Transitions freshly created mirror colour images from UNDEFINED to
// COLOR_ATTACHMENT_OPTIMAL. Lifted verbatim in shape from
// prepare_offscreen_target's own one-time transition, which has run live in
// this same hook for months.
// Transitions ONE image we own from UNDEFINED to a chosen layout, on its own
// transient command buffer. Modelled on
// transition_mirror_images_to_color_attachment below, which does the same for
// framebuffer mirrors at creation.
//
// WHY A STORAGE MIRROR NEEDS THIS AND A FRAMEBUFFER MIRROR DOES NOT
// (2026-08-18, run B). Framebuffer mirrors reach their layout because DOOM
// barriers its own attachments inside the doubled call and barrier
// substitution re-points those onto the mirror. Run B measured that this
// never happens for the two storage images: their mirrors sat at UNDEFINED
// for the whole run (`GateFallbackID … have=0 want=1`), so the safety gate
// could only ever refuse and the substitution was dead on arrival.
// barrier2Calls=0 rules out the obvious alternative - DOOM uses plain
// vkCmdPipelineBarrier, which IS substituted - so the conclusion is that
// DOOM issues no layout transition for these images inside the frame root
// at all. Nothing to piggyback on means we transition it or nobody does.
bool transition_owned_image_to(VkImage image, VkImageLayout layout,
                               VkAccessFlags dst_access) {
    const auto& vk = vulkan_state();
    if (!vk.device || !vk.queue || vk.queue_family == UINT32_MAX || image == VK_NULL_HANDLE)
        return false;
    auto create_command_pool = load_device_proc<PFN_vkCreateCommandPool>(vk.device, "vkCreateCommandPool");
    auto allocate_command_buffers = load_device_proc<PFN_vkAllocateCommandBuffers>(vk.device, "vkAllocateCommandBuffers");
    auto begin_command_buffer = load_device_proc<PFN_vkBeginCommandBuffer>(vk.device, "vkBeginCommandBuffer");
    auto cmd_pipeline_barrier = load_device_proc<PFN_vkCmdPipelineBarrier>(vk.device, "vkCmdPipelineBarrier");
    auto end_command_buffer = load_device_proc<PFN_vkEndCommandBuffer>(vk.device, "vkEndCommandBuffer");
    auto queue_submit = load_device_proc<PFN_vkQueueSubmit>(vk.device, "vkQueueSubmit");
    auto queue_wait_idle = load_device_proc<PFN_vkQueueWaitIdle>(vk.device, "vkQueueWaitIdle");
    auto free_command_buffers = load_device_proc<PFN_vkFreeCommandBuffers>(vk.device, "vkFreeCommandBuffers");
    auto destroy_command_pool = load_device_proc<PFN_vkDestroyCommandPool>(vk.device, "vkDestroyCommandPool");
    if (!create_command_pool || !allocate_command_buffers || !begin_command_buffer ||
        !cmd_pipeline_barrier || !end_command_buffer || !queue_submit || !queue_wait_idle ||
        !free_command_buffers || !destroy_command_pool) return false;

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = vk.queue_family;
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
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = layout;
        b.dstAccessMask = dst_access;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS,
                              0, VK_REMAINING_ARRAY_LAYERS};
        cmd_pipeline_barrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        end_command_buffer(command);
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        ok = queue_submit(vk.queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS;
        if (ok) queue_wait_idle(vk.queue);
        free_command_buffers(vk.device, pool, 1, &command);
    }
    destroy_command_pool(vk.device, pool, nullptr);
    return ok;
}

bool transition_mirror_images_to_color_attachment(const std::vector<VkImage>& images,
                                                  const std::vector<VkImage>& depth_images,
                                                  VkImageAspectFlags depth_aspect) {
    const auto& vk = vulkan_state();
    if (!vk.device || !vk.queue || vk.queue_family == UINT32_MAX) return false;
    auto create_command_pool = load_device_proc<PFN_vkCreateCommandPool>(vk.device, "vkCreateCommandPool");
    auto allocate_command_buffers = load_device_proc<PFN_vkAllocateCommandBuffers>(vk.device, "vkAllocateCommandBuffers");
    auto begin_command_buffer = load_device_proc<PFN_vkBeginCommandBuffer>(vk.device, "vkBeginCommandBuffer");
    auto cmd_pipeline_barrier = load_device_proc<PFN_vkCmdPipelineBarrier>(vk.device, "vkCmdPipelineBarrier");
    auto end_command_buffer = load_device_proc<PFN_vkEndCommandBuffer>(vk.device, "vkEndCommandBuffer");
    auto queue_submit = load_device_proc<PFN_vkQueueSubmit>(vk.device, "vkQueueSubmit");
    auto queue_wait_idle = load_device_proc<PFN_vkQueueWaitIdle>(vk.device, "vkQueueWaitIdle");
    auto free_command_buffers = load_device_proc<PFN_vkFreeCommandBuffers>(vk.device, "vkFreeCommandBuffers");
    auto destroy_command_pool = load_device_proc<PFN_vkDestroyCommandPool>(vk.device, "vkDestroyCommandPool");
    if (!create_command_pool || !allocate_command_buffers || !begin_command_buffer ||
        !cmd_pipeline_barrier || !end_command_buffer || !queue_submit || !queue_wait_idle ||
        !free_command_buffers || !destroy_command_pool) return false;

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = vk.queue_family;
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
        std::vector<VkImageMemoryBarrier> barriers;
        barriers.reserve(images.size());
        for (VkImage img : images) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            b.image = img;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
            barriers.push_back(b);
        }
        // Depth goes to DEPTH_STENCIL_ATTACHMENT_OPTIMAL - DOOM's own render
        // passes declare initialLayout=3 for their depth attachment (verified
        // in the live vkCreateRenderPass log), so a fresh UNDEFINED image
        // would be undefined behaviour exactly as it was for colour.
        for (VkImage img : depth_images) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            b.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            b.image = img;
            b.subresourceRange = {depth_aspect, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
            barriers.push_back(b);
        }
        cmd_pipeline_barrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, nullptr, 0, nullptr,
            static_cast<uint32_t>(barriers.size()), barriers.data());
        // ===== THE WHITE FLASHES (2026-08-30) ==============================
        //
        // A layout transition out of UNDEFINED does NOT initialise contents.
        // Vulkan says the data is undefined until something writes it, and
        // undefined reads as whatever was in that memory - frequently bright.
        // So a mirror that is sampled or presented before the doubled call
        // first writes it shows one frame of garbage.
        //
        // That fits every constraint the owner reported: the flashes exist
        // ONLY under PageUp (mirrors only exist when doubling), they appear
        // WHILE MOVING AROUND (mirrors are created lazily as new framebuffers
        // and render passes are encountered in new areas), and they survived
        // both the framerate fix and dblcompute=run because neither has
        // anything to do with image initialisation.
        //
        // So clear every new colour mirror to opaque black once, here, while
        // we already own a command buffer and are already paying a submit. A
        // black frame is invisible; an uninitialised one is a white flash.
        // Depth mirrors are deliberately left alone - depth is fully written
        // by the pass that uses it and clearing it would need its own
        // aspect-correct path for no benefit.
        if (!images.empty()) {
            auto cmd_clear_color =
                load_device_proc<PFN_vkCmdClearColorImage>(vk.device, "vkCmdClearColorImage");
            if (cmd_clear_color) {
                std::vector<VkImageMemoryBarrier> to_dst;
                to_dst.reserve(images.size());
                for (VkImage img : images) {
                    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    b.image = img;
                    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0,
                                          VK_REMAINING_ARRAY_LAYERS};
                    to_dst.push_back(b);
                }
                cmd_pipeline_barrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                    static_cast<uint32_t>(to_dst.size()), to_dst.data());
                const VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
                const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0,
                                                    VK_REMAINING_ARRAY_LAYERS};
                for (VkImage img : images)
                    cmd_clear_color(command, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    &black, 1, &range);
                // Back to the layout the caller promised, or DOOM begins a
                // render pass against an image in the wrong layout - which
                // would be a worse fault than the one being fixed.
                for (VkImageMemoryBarrier& b : to_dst) {
                    std::swap(b.oldLayout, b.newLayout);
                    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                }
                cmd_pipeline_barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr,
                    static_cast<uint32_t>(to_dst.size()), to_dst.data());
                g_mirror_cleared.fetch_add(images.size(), std::memory_order_relaxed);
            }
        }
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

// ⚠ CALLER MUST HOLD mirror_mutex, and must keep holding it for as long as
// it uses the returned pointer - it points into mirror_targets, which a
// destroy hook can erase. m.images/m.memories/m.views are bookkeeping of
// what this call created; the OWNER of those is mirrored_views (see the
// ownership note at mirrored_view_refs). Do not free them per-framebuffer.
const MirrorTarget* ensure_mirror_framebuffer(VkFramebuffer original) {
    auto existing = mirror_targets.find(original);
    if (existing != mirror_targets.end())
        return existing->second.ok ? &existing->second : nullptr;

    FramebufferInfo fi;
    {
        std::scoped_lock lock(framebuffer_depth_view_mutex);
        auto it = framebuffer_infos.find(original);
        if (it == framebuffer_infos.end()) {
            // Expected failure mode, and this codebase has hit it before:
            // DOOM creates its framebuffers at startup, BEFORE our DLL
            // injects, so our vkCreateFramebuffer hook never saw them and we
            // have no attachment list to mirror. Report it loudly rather than
            // failing silently - a silent null here is exactly why the first
            // mirroring test produced zero mirrors and looked like the old
            // path.
            const uint32_t n = mirror_miss_count.fetch_add(1, std::memory_order_relaxed);
            if (n < 5) {
                // NOTE: the mutex is ALREADY held by `lock` above. A second
                // scoped_lock on the same non-recursive std::mutex used to sit
                // here and would have self-deadlocked the render thread on the
                // first miss - a silent game freeze with no log line and no SEH
                // (a deadlock is not an exception), i.e. the worst possible
                // outcome for the very diagnostic this branch exists to print.
                log::error(std::format(
                    "MirrorFramebuffer MISS #{} framebuffer={} not in framebuffer_infos "
                    "(created before our hooks installed?). Known entries: framebuffers={} "
                    "imageViews={}",
                    n, reinterpret_cast<void*>(original),
                    framebuffer_infos.size(), image_view_infos.size()));
            }
            return nullptr;
        }
        fi = it->second;
    }
    MirrorTarget& m = mirror_targets[original];
    m.extent = {fi.width, fi.height};

    const auto& vk = vulkan_state();
    auto create_image = load_device_proc<PFN_vkCreateImage>(vk.device, "vkCreateImage");
    auto get_reqs = load_device_proc<PFN_vkGetImageMemoryRequirements>(vk.device, "vkGetImageMemoryRequirements");
    auto allocate = load_device_proc<PFN_vkAllocateMemory>(vk.device, "vkAllocateMemory");
    auto bind = load_device_proc<PFN_vkBindImageMemory>(vk.device, "vkBindImageMemory");
    auto create_view = load_device_proc<PFN_vkCreateImageView>(vk.device, "vkCreateImageView");
    auto create_fb = load_device_proc<PFN_vkCreateFramebuffer>(vk.device, "vkCreateFramebuffer");
    if (!create_image || !get_reqs || !allocate || !bind || !create_view || !create_fb) {
        m.ok = false;
        log::error("MirrorFramebuffer ABORT: a required device proc could not be loaded");
        return nullptr;
    }

    VkPhysicalDeviceMemoryProperties mem_props{};
    if (auto gmp = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
            kharvox::native::instanceProc("vkGetPhysicalDeviceMemoryProperties")))
        gmp(vk.physical_device, &mem_props);

    std::vector<VkImageView> attachments;
    attachments.reserve(fi.attachments.size());
    // Aspect of whichever depth attachment we mirror, needed for its layout
    // transition below. DOOM's depth here is D24_UNORM_S8_UINT (format 129),
    // so the stencil bit matters and must not be assumed away.
    VkImageAspectFlags mirror_depth_aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    for (VkImageView view : fi.attachments) {
        ImageViewInfo vi{};
        // DOOM's OWN usage for the image behind this attachment, read in the
        // SAME locked block as vi below - framebuffer_depth_view_mutex is not
        // recursive and guards both maps, and a second scoped_lock on it is a
        // deadlock this function has already been bitten by once.
        VkImageUsageFlags src_usage = 0;
        bool have_src_usage = false;
        CreatedImageInfo source_info{};
        {
            std::scoped_lock lock(framebuffer_depth_view_mutex);
            auto it = image_view_infos.find(view);
            if (it == image_view_infos.end()) {
                m.ok = false;
                // Rule 3: this used to return a silent nullptr, which falls
                // straight through to the old path and is indistinguishable
                // from "mirroring never ran" - exactly the ambiguity that has
                // cost this project four live runs.
                const uint32_t vn = mirror_view_miss_count.fetch_add(1, std::memory_order_relaxed);
                if (vn < 5) {
                    log::error(std::format(
                        "MirrorFramebuffer VIEW MISS #{} framebuffer={} attachment view={} not in "
                        "image_view_infos - cannot mirror this framebuffer, falling back to DOOM's "
                        "own target. Known imageViews={}",
                        vn, reinterpret_cast<void*>(original), reinterpret_cast<void*>(view),
                        image_view_infos.size()));
                }
                return nullptr;
            }
            vi = it->second;
            if (auto ci = created_image_infos.find(vi.image); ci != created_image_infos.end()) {
                src_usage = ci->second.usage;
                source_info = ci->second;
                have_src_usage = true;
            }
        }
        // PRIVATE PER-EYE DEPTH (2026-08-16). Depth used to be SHARED with
        // DOOM here, and the live bisection proved that is the single largest
        // cause of eye 0's damage: with the doubled call's RenderDepth
        // suppressed eye 0 improved sharply, and with ALL jobs suppressed it
        // was no better still - so RenderOpaque contributes nothing and shared
        // depth was doing all of it.
        //
        // The old "private depth came back black" result does not apply: it
        // predates the doubled call running its own RenderDepth, which is in
        // the job allow-list, so this buffer is populated by our own prepass.
        const bool is_depth =
            (vi.aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0;
        if (is_depth) mirror_depth_aspect = vi.aspect;
        // SHADOW ATLAS STAYS SHARED. DOOM's shadow atlas is 16384x16384
        // depth-only, so making depth private turned it into a 1 GiB
        // allocation (measured: totalMirrorBytes went 250 MB -> 1.56 GB).
        //
        // Sharing it is also correct rather than merely cheap: a shadow atlas
        // is rendered in LIGHT space, not eye space, so it does not depend on
        // which eye is being drawn and both eyes should use the same one.
        // Only the main view's depth - the buffer eye 0's deferred composite
        // reconstructs position from - has to be private.
        //
        // Keyed on extent because that is what actually distinguishes them
        // here: the main view is 3818x2032, the atlas 16384x16384. Logged, so
        // a future resolution change that breaks this assumption is visible
        // rather than silent.
        // KHARVOX owns private depth at every supported extent. The donor's
        // large-atlas sharing branch conflicts with Native's seed/readiness
        // contract and used to terminate on the 8192x16384 Ultra atlas.
        if (!kharvox::native::requested() && is_depth && (fi.width > 8192 || fi.height > 8192)) {
            const uint32_t n = mirror_shared_depth_count.fetch_add(1, std::memory_order_relaxed);
            if (n < 3) {
                log::warn(std::format(
                    "MirrorFramebuffer: sharing DOOM's depth for {}x{} framebuffer {} "
                    "(light-space shadow atlas, not eye-dependent) instead of allocating a "
                    "private copy - that would cost {} MB",
                    fi.width, fi.height, reinterpret_cast<void*>(original),
                    (static_cast<uint64_t>(fi.width) * fi.height * 4) / (1024 * 1024)));
            }
            mirror_shared_source_views.insert(view);
            // CONTAMINATION OF EYE 0, found 2026-08-18 from the owner's report
            // of "odd shadow effects appearing and disappearing when moving
            // the mouse" in the LEFT eye - which is DOOM's own frame and ought
            // to be pristine.
            //
            // Sharing this atlas is justified above as "not eye-dependent".
            // That is true of light-space shadow CONTENT and false of what
            // actually happens: the doubled call RE-RENDERS the atlas with a
            // shifted camera, and view-dependent culling makes it draw
            // different geometry. DrawParity measured exactly that - passes 3
            // and 5, 35 draws against 34 and 28638 indices against 26952 - and
            // that divergence was written off at the time as "the doubled call
            // working correctly". It was the contamination signature.
            //
            // Recorded so the draw hooks can suppress the doubled call's
            // writes into it. Eye 1 then simply uses DOOM's shadows, which IS
            // legitimate for light-space content.
            shared_shadow_framebuffer.store(reinterpret_cast<uintptr_t>(original),
                                            std::memory_order_relaxed);
            attachments.push_back(view);
            continue;
        }
        // Reuse the mirror already made for this source view, so DOOM's
        // aliasing survives. Already created means already transitioned, so
        // it must NOT be added to this target's transition lists again.
        if (auto cached = mirrored_views.find(view); cached != mirrored_views.end()) {
            attachments.push_back(cached->second.view);
            mirrored_view_refs[view].add();  // this framebuffer is now a user too
            if (!is_depth && !m.color0) {
                m.color0 = cached->second.image;
                m.color0_format = cached->second.format;
            }
            continue;
        }
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = vi.format;
        ici.extent = {fi.width, fi.height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = fi.layers ? fi.layers : 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        if(have_src_usage){
            if(source_info.type!=VK_IMAGE_TYPE_2D||source_info.layers!=1||vi.range.baseMipLevel||vi.range.baseArrayLayer)kharvox::native::fail("framebuffer mirror requires supported 2D source range");
            ici.extent=source_info.extent;ici.mipLevels=source_info.mips;ici.arrayLayers=source_info.layers;
        }
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        // MIRROR DOOM'S OWN USAGE (2026-08-22). THIS WAS THE BUG.
        //
        // The line below used to hardcode COLOR_ATTACHMENT|SAMPLED|TRANSFER_SRC
        // |TRANSFER_DST, with no VK_IMAGE_USAGE_STORAGE_BIT - which appeared
        // NOWHERE in this file. Every mirror this path built was therefore
        // incapable of being a storage image, and any compute dispatch in the
        // doubled call that bound one as a UAV had its store silently dropped.
        //
        // MEASURED, doom-capture 2026-08-22 (doomvr_frame16696.rdc, 0.0
        // separation), on DOOM's deferred-composite dispatch - shader
        // ResourceId::6659, RW ib_uavdeferredcomposite0/1:
        //
        //   composite0  eye 0 id 235    1210 gained / 632 lost / +15.3526
        //               eye 1 id 60493     0 gained /   0 lost /   0.0000
        //
        // Zero of 30 353 sampled points changed on eye 1 at a 16 px grid,
        // against 29 006 of the same points on eye 0 as the positive control.
        // RenderDoc's own creation flags then named the cause outright:
        //
        //   235   DOOM  ColorTarget, ShaderRead, ShaderReadWrite
        //   383   DOOM  ColorTarget, ShaderRead, ShaderReadWrite
        //   60559 OURS  ColorTarget, ShaderRead, ShaderReadWrite  <- storage path
        //   60493 OURS  ColorTarget, ShaderRead                   <- THIS path
        //
        // and it is systematic, not one unlucky image: 14 of DOOM's 46 full-res
        // textures carry ShaderReadWrite against 1 of our 27 - the one being
        // 60559, built by ensure_mirror_storage_image, which has always copied
        // DOOM's flags. The internal control is decisive: ONE dispatch, ONE
        // shader, TWO UAVs, and only the one from this path fails.
        //
        // So do what the storage path's own comment already says: "the mirror
        // has to be legal everywhere the original is legal, because DOOM's
        // barriers for the original get re-pointed onto it." Copy the source
        // image's usage rather than guessing a set.
        //
        // The hardcoded set stays as the FALLBACK, and only for images created
        // before our hooks installed, where created_image_infos has no entry.
        // Falling back silently is what let this hide for so long, so the miss
        // is counted and reported.
        // UNION, NOT REPLACE (2026-08-22, second revision - the first was a
        // regression risk and possibly a live one).
        //
        // The first version of this fix set usage = src_usage | transfers,
        // REPLACING the hardcoded set. That silently dropped any guarantee the
        // old code made which DOOM's own image happened not to have: an image
        // DOOM created without SAMPLED would now produce a mirror without
        // SAMPLED, and anything in the doubled call that samples it reads an
        // image that is not legally sampleable. Black patches on a surface,
        // varying with viewpoint, are exactly what that looks like - which is
        // the symptom the owner reported on a ceiling monitor right after the
        // first version shipped.
        //
        // The union keeps every guarantee the old code made AND adds whatever
        // DOOM had, so it is a strict superset of both and cannot regress in
        // either direction. The blanket set is already proven safe on this
        // game - it is what every colour mirror used before today.
        const VkImageUsageFlags guaranteed = is_depth
            ? (VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
            : (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        if (have_src_usage) {
            ici.usage = guaranteed | src_usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            // THE DIAGNOSTIC THAT SAYS WHETHER THE FIRST VERSION WAS A LIVE
            // REGRESSION, not just a theoretical one. These are the bits the
            // previous build would have LOST for this image. Unbounded by the
            // general cap below and reported separately, because the general
            // cap is 8 per category and 25 mirrors get built - which is why
            // the first pass could not see whether this ever happened.
            if (const VkImageUsageFlags lost = guaranteed & ~src_usage; lost != 0) {
                static std::atomic_uint32_t restored{0};
                const uint32_t r = restored.fetch_add(1, std::memory_order_relaxed);
                if (r < 20) {
                    log::warn(std::format(
                        "MirrorUsage RESTORED #{}: view={} {}x{} fmt={} srcUsage={:#x} lacks "
                        "{:#x} of the guaranteed set{}{}. READ THE TAGS, NOT THIS SENTENCE: "
                        "0x1/0x2 are the TRANSFER bits, which the 17:11 build re-added "
                        "explicitly, so losing only those means that build lost NOTHING. Only "
                        "a [SAMPLED] or [COLOR_ATTACHMENT] tag means it created a mirror that "
                        "could not legally be sampled or attached.",
                        r, reinterpret_cast<void*>(view), fi.width, fi.height,
                        static_cast<int>(vi.format), static_cast<unsigned>(src_usage),
                        static_cast<unsigned>(lost),
                        (lost & VK_IMAGE_USAGE_SAMPLED_BIT) ? " [SAMPLED]" : "",
                        (lost & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ? " [COLOR_ATTACHMENT]" : ""));
                }
            }
        } else {
            static std::atomic_uint32_t usage_fallbacks{0};
            const uint32_t uf = usage_fallbacks.fetch_add(1, std::memory_order_relaxed);
            if (uf < 5) {
                // EXTENT AND FORMAT ADDED 2026-08-22 (second revision). The
                // first version logged only the two handles, which cannot be
                // matched against anything - so when the owner reported a
                // ceiling monitor going partially black in eye 1, there was no
                // way to ask whether one of these two mirrors was the surface
                // feeding it. A diagnostic that cannot be joined to the symptom
                // it exists for is not finished.
                log::warn(std::format(
                    "MirrorFramebuffer USAGE FALLBACK #{} view={} image={} {}x{} fmt={} "
                    "depth={} - no created_image_infos entry, so DOOM's usage is unknown and "
                    "the old hardcoded set is used. THIS MIRROR CANNOT BE A STORAGE IMAGE: "
                    "any compute dispatch in the doubled call that binds it as a UAV will "
                    "have its store silently dropped - the SAME defect fixed everywhere else "
                    "today, surviving here. Created before our hooks installed?",
                    uf, reinterpret_cast<void*>(view), reinterpret_cast<void*>(vi.image),
                    fi.width, fi.height, static_cast<int>(vi.format), is_depth));
            }
            ici.usage = guaranteed;
        }
        // ENGAGEMENT, READ FROM THE INSTRUMENT AND NOT FROM THE EDIT. Counted
        // separately from creations so "the fix is in the build" and "the fix
        // reached this image" can never be read as one number.
        {
            static std::atomic_uint32_t storage_capable{0};
            static std::atomic_uint32_t storage_missing{0};
            const bool has_storage = (ici.usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0;
            const uint32_t sc = has_storage
                ? storage_capable.fetch_add(1, std::memory_order_relaxed)
                : storage_missing.fetch_add(1, std::memory_order_relaxed);
            // 40, not 8. The first build capped each category at 8 while ~25
            // mirrors get created, so the log could not show whether any
            // source image lacked a guaranteed usage bit - the exact question
            // the union above exists to answer. A cap hit is truncation, not a
            // measurement, and 8 was being hit.
            if (sc < 40) {
                log::warn(std::format(
                    "MirrorUsage: view={} {}x{} fmt={} srcUsage={:#x} mirrorUsage={:#x} "
                    "STORAGE={} (fromSource={}). storageCapable={} storageMissing={}. Before "
                    "2026-08-22 this was ALWAYS missing on this path and the doubled call's "
                    "UAV stores were dropped.",
                    reinterpret_cast<void*>(view), fi.width, fi.height,
                    static_cast<int>(vi.format), static_cast<unsigned>(src_usage),
                    static_cast<unsigned>(ici.usage), has_storage ? "YES" : "NO",
                    have_src_usage,
                    storage_capable.load(std::memory_order_relaxed),
                    storage_missing.load(std::memory_order_relaxed)));
            }
        }
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImage img = VK_NULL_HANDLE;
        if (create_image(vk.device, &ici, nullptr, &img) != VK_SUCCESS) {
            m.ok = false;
            log::error(std::format(
                "MirrorFramebuffer ABORT: vkCreateImage failed for framebuffer={} format={} {}x{}",
                reinterpret_cast<void*>(original), static_cast<int>(vi.format), fi.width, fi.height));
            return nullptr;
        }
        VkMemoryRequirements req{};
        get_reqs(vk.device, img, &req);
        uint32_t type_index = UINT32_MAX;
        if (!find_memory_type(mem_props, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type_index)) {
            m.ok = false;
            log::error(std::format(
                "MirrorFramebuffer ABORT: no device-local memory type for framebuffer={} "
                "(size={} bytes)", reinterpret_cast<void*>(original), req.size));
            return nullptr;
        }
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size; mai.memoryTypeIndex = type_index;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        if (allocate(vk.device, &mai, nullptr, &mem) != VK_SUCCESS ||
            bind(vk.device, img, mem, 0) != VK_SUCCESS) {
            m.ok = false;
            log::error(std::format(
                "MirrorFramebuffer ABORT: allocate/bind failed for framebuffer={} size={} bytes "
                "(out of GPU memory?)", reinterpret_cast<void*>(original), req.size));
            return nullptr;
        }
        mirror_bytes_allocated.fetch_add(req.size, std::memory_order_relaxed);
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = vi.format;
        vci.subresourceRange = {is_depth ? vi.aspect : VK_IMAGE_ASPECT_COLOR_BIT,
                                0, 1, 0, ici.arrayLayers};
        VkImageView nv = VK_NULL_HANDLE;
        if (create_view(vk.device, &vci, nullptr, &nv) != VK_SUCCESS) {
            m.ok = false;
            log::error(std::format(
                "MirrorFramebuffer ABORT: vkCreateImageView failed for framebuffer={}",
                reinterpret_cast<void*>(original)));
            return nullptr;
        }
        mirrored_views[view] = MirroredView{img, nv, vi.format, mem, req.size,
                                            VkExtent2D{fi.width, fi.height}, ici.mipLevels, ici.arrayLayers};
        mirrored_view_refs[view] = {1};
        // Barrier substitution needs source image -> mirror image; vi.image
        // is the source image this view was created over.
        if (vi.image != VK_NULL_HANDLE) mirrored_images[vi.image] = img;
        // Seeded to match transition_mirror_images_to_color_attachment, which
        // runs below for exactly these images.
        register_mirror_layouts(img,ici.mipLevels,ici.arrayLayers,is_depth
            ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
            : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,vi.format);
        // A brand-new mirror holds nothing and is not yet substitutable;
        // cached clones must re-evaluate now that it exists.
        mirror_generation.fetch_add(1, std::memory_order_relaxed);
        m.memories.push_back(mem); m.views.push_back(nv);
        if (is_depth) {
            m.depth_images.push_back(img);
        } else {
            m.images.push_back(img);
            if (!m.color0) { m.color0 = img; m.color0_format = vi.format; }
        }
        attachments.push_back(nv);
    }

    // One-time layout transition, same proven pattern as
    // prepare_offscreen_target (which is likewise called from inside
    // hook_cmd_begin_render_pass, so submitting here is not a new risk).
    //
    // NOT optional and NOT assumed: the live log for this exact set of 7
    // framebuffers shows every colour attachment declaring
    // initialLayout=2 (COLOR_ATTACHMENT_OPTIMAL). Fresh images start
    // UNDEFINED, so beginning DOOM's render pass against them untransitioned
    // is undefined behaviour with a real device-fault risk. Depth attachments
    // are DOOM's own and must not be touched - they are already in whatever
    // layout its passes leave them in.
    if (!m.images.empty() || !m.depth_images.empty()) {
        if (!transition_mirror_images_to_color_attachment(m.images, m.depth_images,
                                                          mirror_depth_aspect)) {
            m.ok = false;
            log::error(std::format(
                "MirrorFramebuffer ABORT: initial layout transition failed for framebuffer={} "
                "({} colour images) - refusing to hand DOOM a render pass over UNDEFINED images",
                reinterpret_cast<void*>(original), m.images.size()));
            return nullptr;
        }
    }

    VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbi.renderPass = fi.render_pass;
    fbi.attachmentCount = static_cast<uint32_t>(attachments.size());
    fbi.pAttachments = attachments.data();
    fbi.width = fi.width; fbi.height = fi.height; fbi.layers = fi.layers ? fi.layers : 1;
    if (create_fb(vk.device, &fbi, nullptr, &m.framebuffer) != VK_SUCCESS) {
        m.ok = false;
        log::error(std::format(
            "MirrorFramebuffer ABORT: vkCreateFramebuffer failed for original={} renderPass={} "
            "attachments={}", reinterpret_cast<void*>(original),
            reinterpret_cast<void*>(fi.render_pass), fbi.attachmentCount));
        return nullptr;
    }

    m.ok = true;
    const uint32_t n = mirror_created_count.fetch_add(1, std::memory_order_relaxed);
    log::warn(std::format(
        "MirrorFramebuffer #{} original={} -> mirror={} attachments={} colour={} {}x{} "
        "totalMirrorBytes={}",
        n, reinterpret_cast<void*>(original), reinterpret_cast<void*>(m.framebuffer),
        fbi.attachmentCount, m.images.size(), fi.width, fi.height,
        mirror_bytes_allocated.load(std::memory_order_relaxed)));
    return &m;
}
#endif  // DOOMVR_RESEARCH_TOOLS

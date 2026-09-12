// 03_camera.inc - camera: CamHunt, the view-origin solve, Lever A, safe read/write
// 4005 of these lines sit inside #ifdef DOOMVR_RESEARCH_TOOLS,
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

void apply_shift_to_world_vp_destinations(float offset) {
    vwc_entry(kVwsLeverADest);
    const uintptr_t doom_base =
        reinterpret_cast<uintptr_t>(GetModuleHandleW(L"DOOMx64vk.exe"));
    if (!doom_base) return;
    // CAMERASRC HUNT (2026-08-27), bit 6 of the done mask. The SOURCE the
    // SetupView copy loop reads has never been hunted by position - only the
    // destinations. If the true camera exists in CameraSrc too, the seam
    // rotation can move upstream of the copy (the purest othermods form:
    // rotate the source once, let the engine distribute it six times).
    // Read-only, same cadence and same one-shot latch as the dest hunts.
    if ((g_dch_done_mask.load(std::memory_order_relaxed) & (1u << 6)) == 0) {
        const uint32_t calls = g_dch_calls.load(std::memory_order_relaxed);
        if (calls % (kDchCooldown * 2) == 1 && wrap::armed()) {
            uintptr_t renderer = 0, camera_src = 0;
            if (safe_read_bytes(doom_base + 0x6FD5FB8, &renderer, sizeof(renderer)) &&
                renderer &&
                safe_read_bytes(renderer, &camera_src, sizeof(camera_src)) && camera_src &&
                camera_hunt("src", 6, camera_src)) {
                g_dch_done_mask.fetch_or(1u << 6, std::memory_order_relaxed);
            }
        }
    }
    // Slots at DOOM+0x6FD5FC0 through +0x6FD5FE8 (6 pointer slots, 8 bytes each).
    for (uintptr_t slot_off = 0x6FD5FC0; slot_off <= 0x6FD5FE8; slot_off += 8) {
        uintptr_t dest_obj = 0;
        if (!safe_read_bytes(doom_base + slot_off, &dest_obj, sizeof(dest_obj)) ||
            dest_obj == 0) continue;
        uintptr_t dest_data = 0;
        if (!safe_read_bytes(dest_obj, &dest_data, sizeof(dest_data)) ||
            dest_data == 0) continue;
        const size_t idx = static_cast<size_t>((slot_off - kWorldVpDestFirst) / 8);
        if (idx >= 6) continue;
        // DESTCAMHUNT - read-only, see its block above. At most one
        // destination per call, at most one per kDchCooldown calls, each
        // exactly once per session. The cooldown is checked before the mask so
        // two destinations can never hunt in the same frame.
        if (g_dch_done_mask.load(std::memory_order_relaxed) != 0x3F) {
            const uint32_t calls = g_dch_calls.fetch_add(1, std::memory_order_relaxed);
            const uint32_t mask = g_dch_done_mask.load(std::memory_order_relaxed);
            // ⚠ PERIODIC ALIASING, fixed 2026-08-26. This counter advances once
            // per (call, destination), so calls ≡ idx (mod 6). The old cooldown
            // of 90 is divisible by 6, which made `calls % cooldown == 0` true
            // ONLY when idx was 0 - so dest[0] was hunted and the other five
            // were unreachable for the whole session. The 18:08 run's
            // doneMask=0x01 after 120696 calls is that bug, not five silent
            // destinations. kDchCooldown is now coprime with 6.
            if (calls % kDchCooldown == 0 && (mask & (1u << idx)) == 0 &&
                wrap::armed() && camera_hunt("dest", idx, dest_data)) {
                // Marked done ONLY when the hunt actually ran and logged. A
                // refusal before gameplay must leave it open to retry - this
                // repo has twice latched a scan on a pre-gameplay zero and
                // then reported that zero for a whole session.
                g_dch_done_mask.fetch_or(1u << idx, std::memory_order_relaxed);
            }
        }
        alignas(16) float m[16]{};
        alignas(16) float m_before[16]{};
        const uintptr_t addrs[2]{dest_data + 0xC44, dest_data + 0xC84};
        for (int which = 0; which < 2; ++which) {
            if (!safe_read_bytes(addrs[which], m, sizeof(m))) continue;
            std::memcpy(m_before, m, sizeof(m));
            if (!inside_frame_root_double) {
                // DOOM's own value, before either call has touched it.
                std::memcpy(dest_clean[idx][which], m, sizeof(m));
                dest_clean_valid[idx][which] = true;
                if (idx == 0 && which == 0) {
                    // DRIFT GUARD. If the restore ever fails to land, the clean
                    // basis walks by the offset every frame and the world slides
                    // away. One representative destination is enough to catch it,
                    // and catching it in the log beats catching it in a headset.
                    const float d = m[3] - dest_clean_m3_last;
                    if (dest_clean_m3_last != 0.0f && (d > 50.0f || d < -50.0f)) {
                        const uint32_t w =
                            dest_drift_warnings.fetch_add(1, std::memory_order_relaxed);
                        if (w < 10) {
                            log::error(std::format(
                                "WorldVpDest DRIFT #{}: clean m3 jumped {:.3f} -> {:.3f} "
                                "(delta {:.3f}) between frames. The restore is not landing and "
                                "the basis is walking. Expect the world to slide.",
                                w, dest_clean_m3_last, m[3], d));
                        }
                    }
                    dest_clean_m3_last = m[3];
                }
            } else if (dest_clean_valid[idx][which]) {
                std::memcpy(m, dest_clean[idx][which], sizeof(m));
                dest_rebased.fetch_add(1, std::memory_order_relaxed);
            } else {
                dest_no_clean.fetch_add(1, std::memory_order_relaxed);
            }
            // PHASE 3. The six sibling copies must receive the SAME head
            // rotation renderView just got, for the same reason they receive
            // the same translation: different downstream jobs read different
            // destinations, and a camera that disagrees between them is the
            // mid-terrain glitch all over again - only now it would be the
            // whole world tearing as the head turns.
            //
            // Rotation is applied BEFORE the translation, matching the order
            // in apply_lever_a_mutation, so the per-eye lateral offset is
            // taken along the ROTATED right vector - the eye separation
            // follows the head, which is what makes stereo stay correct when
            // you look sideways.
            if (wrap::head_look_active() &&
                dest_fixed_write_enabled.load(std::memory_order_relaxed)) {
                // Counted now (2026-08-26): the verdict used to be discarded,
                // so six zero matrices silently refusing every frame printed
                // the same nothing as six real matrices rotating. The 13:43
                // run's props stayed welded WITH this path running - whether
                // these writes even landed on real matrices is exactly what
                // these three counters settle in the next run's log.
                dest_fixed_attempts.fetch_add(1, std::memory_order_relaxed);
                float hy = 0.0f, hp = 0.0f, hr = 0.0f;
                const bool have_angles = wrap::residual_head_angles(hy, hp, hr);
                // Captured BEFORE apply_head_rotation can touch m. These four
                // lengths ARE its gate: it refuses unless |row0| and |row1|
                // both exceed 1e-6, and unless the forward row it picks
                // (row3 in the projective layout, row2 in the affine one) is
                // non-zero as well. A site refusing every frame will show its
                // reason here as a zero.
                const float rl0 = wrap::row_length(m, 0);
                const float rl1 = wrap::row_length(m, 1);
                const float rl2 = wrap::row_length(m, 2);
                const float rl3 = wrap::row_length(m, 3);
                const float pre_row0[4]{m[0], m[1], m[2], m[3]};
                const bool applied =
                    have_angles &&
                    wrap::apply_head_rotation(m, hy, hp, hr, wrap::head_axes());
                if (applied)
                    dest_fixed_rotations.fetch_add(1, std::memory_order_relaxed);
                else
                    dest_fixed_refusals.fetch_add(1, std::memory_order_relaxed);
                // The per-site one-shot. See dest_fixed_sites_logged.
                if (dest_fixed_sites_logged.load(std::memory_order_relaxed) < 12 &&
                    !dest_fixed_site_logged[idx][which].load(std::memory_order_relaxed)) {
                    wrap::VpSnapshot gvp;
                    if (wrap::world_vp(gvp) && wrap::is_gameplay_view(gvp.m) &&
                        !dest_fixed_site_logged[idx][which].exchange(
                            true, std::memory_order_relaxed)) {
                        dest_fixed_sites_logged.fetch_add(1, std::memory_order_relaxed);
                        log::warn(std::format(
                            "DestFixedSite dest[{}] {} addr={:#x} verdict={} angles={} "
                            "rowLengths=({:.4f} {:.4f} {:.4f} {:.4f}) "
                            "row0=({:.4f} {:.4f} {:.4f} {:.4f})\n"
                            "READING IT: one line per destination per matrix, first gameplay "
                            "frame that reaches it, twelve maximum. destFixed refused EXACTLY "
                            "half of 205272 attempts in the 14:47 run and the aggregate could "
                            "not say which half. If every REFUSED line is a +0xC84 the previous "
                            "copies are dead and only the current ones carry a camera; if the "
                            "REFUSED lines are three whole destinations, three of the six slots "
                            "are not view objects at all. Either way the refusal reason is a "
                            "ZERO among the four row lengths - all-zero means the slot holds no "
                            "matrix, and a zero in only row2 AND row3 means it is neither "
                            "layout. angles=NO would mean the head pose was unavailable, which "
                            "is a different (wiring) fault entirely.",
                            idx, which == 0 ? "+0xC44(current)" : "+0xC84(previous)",
                            addrs[which], applied ? "ROTATED" : "REFUSED",
                            have_angles ? "yes" : "NO",
                            rl0, rl1, rl2, rl3,
                            pre_row0[0], pre_row0[1], pre_row0[2], pre_row0[3]));
                    }
                }
            }
            // CTRL+Insert. When OFF, this destination keeps DOOM's own bytes:
            // no rotation (already skipped above), no translation, no write.
            // The read-back, the clean-basis snapshot and the drift guard all
            // still run, so the OFF arm is still instrumented rather than
            // going dark - a disarmed path that also stops reporting is how a
            // toggle turns into an unmeasured run.
            if (!dest_fixed_write_enabled.load(std::memory_order_relaxed)) {
                dest_fixed_skipped.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            apply_translation(m, offset);
            safe_write_bytes(addrs[which], m, sizeof(m));
            vwc_write(kVwsLeverADest, addrs[which], offset, m_before, m);
        }
    }
}

// Put DOOM's own six destination copies back after the doubled call, so the
// next frame's real call starts from DOOM's value rather than from eye 1's.
// See the note above apply_shift_to_world_vp_destinations for why omitting
// this would let the basis accumulate without bound.
// ============================== LEVER O ==================================
// The camera POSITION, at CameraSrc+0x14b0, found by ViewOriginScan on
// 2026-08-23. Every constant buffer in the R18 capture - three graphics draws
// and the deferred-composite dispatch, both eyes, separate ring allocations -
// carries globalvieworigin unshifted, so eye 1 shades the whole frame from eye
// 0's camera while its geometry is transformed by a shifted matrix.
//
// HOW THE OFFSET WAS FOUND, and how strong it is. The scan solved the pre-shift
// VP for the camera position it encodes (solveResidual 0.00098 - the solve is
// exact) and reported the nearest float3 in each of 8 regions:
//     CameraSrc+0x14b0  (-17956.0410 -2569.0391 3160.8062)   distance 2.9992
//     renderView        nearest anything                     distance 4073.58
// The 3.0 is one frame of camera motion - the sample was taken while MOVING,
// which is the expected lag between the matrix and a stored copy. Same offset
// in CameraSrc and in all six destination copies, which is what a per-frame
// copy of one source struct should look like.
//
// STILL A CANDIDATE, NOT A CONFIRMED FIELD. One sample, one moment, and the
// distance was never measured with the camera parked. That is exactly what
// the origin-tracking probe below measures, and why this lever ships as an
// automatic A/B rather than as a standing fix.
//
// WRITES BOTH the source and the six destinations. Which of them the constant
// buffer is built from is not known; writing only the source loses if the copy
// already ran, writing only the destinations loses if it has not.
// kCameraSrcOriginOffset and the lever_o_* counters are declared next to the
// A/B driver in the render-view hook, which needs them earlier in the file.

// Saved originals for the restore. Slot 0 is CameraSrc, 1..6 the destinations.
alignas(16) float origin_clean[7][3]{};
uintptr_t origin_clean_addr[7]{};
bool origin_clean_valid[7]{};

// Collects CameraSrc and the six destination copies into addrs; returns count.
size_t collect_origin_sites(uintptr_t* addrs, size_t cap) {
    size_t n = 0;
    const uintptr_t doom_base =
        reinterpret_cast<uintptr_t>(GetModuleHandleW(L"DOOMx64vk.exe"));
    if (!doom_base) return 0;
    uintptr_t renderer = 0, camera_src = 0;
    if (safe_read_bytes(doom_base + 0x6FD5FB8, &renderer, sizeof(renderer)) &&
        renderer &&
        safe_read_bytes(renderer, &camera_src, sizeof(camera_src)) && camera_src &&
        n < cap) {
        addrs[n++] = camera_src + kCameraSrcOriginOffset;
    }
    for (uintptr_t slot = kWorldVpDestFirst; slot <= kWorldVpDestLast; slot += 8) {
        uintptr_t dest_obj = 0, dest_data = 0;
        if (!safe_read_bytes(doom_base + slot, &dest_obj, sizeof(dest_obj)) ||
            !dest_obj) continue;
        if (!safe_read_bytes(dest_obj, &dest_data, sizeof(dest_data)) ||
            !dest_data) continue;
        if (n < cap) addrs[n++] = dest_data + kCameraSrcOriginOffset;
    }
    return n;
}

void apply_origin_shift(const float* delta) {
    uintptr_t addrs[7]{};
    const size_t n = collect_origin_sites(addrs, 7);
    for (size_t i = 0; i < n; ++i) {
        float p[3]{};
        if (!safe_read_bytes(addrs[i], p, sizeof(p))) continue;
        // Snapshot BEFORE mutating so the restore hands DOOM back its own
        // value. Without this the position accumulates the offset every frame
        // and the camera walks away - the same failure the dest drift guard
        // exists to catch.
        std::memcpy(origin_clean[i], p, sizeof(p));
        origin_clean_addr[i] = addrs[i];
        origin_clean_valid[i] = true;
        for (int c = 0; c < 3; ++c) p[c] += delta[c];
        if (safe_write_bytes(addrs[i], p, sizeof(p)))
            lever_o_applied.fetch_add(1, std::memory_order_relaxed);
    }
    if (n == 0) lever_o_no_clean.fetch_add(1, std::memory_order_relaxed);
}

void restore_origin_shift() {
    for (size_t i = 0; i < 7; ++i) {
        if (!origin_clean_valid[i]) continue;
        if (safe_write_bytes(origin_clean_addr[i], origin_clean[i], sizeof(float) * 3))
            lever_o_restored.fetch_add(1, std::memory_order_relaxed);
        origin_clean_valid[i] = false;
    }
}

void restore_world_vp_destinations() {
    const uintptr_t doom_base =
        reinterpret_cast<uintptr_t>(GetModuleHandleW(L"DOOMx64vk.exe"));
    if (!doom_base) return;
    for (uintptr_t slot_off = kWorldVpDestFirst; slot_off <= kWorldVpDestLast; slot_off += 8) {
        const size_t idx = static_cast<size_t>((slot_off - kWorldVpDestFirst) / 8);
        if (idx >= 6) continue;
        uintptr_t dest_obj = 0;
        if (!safe_read_bytes(doom_base + slot_off, &dest_obj, sizeof(dest_obj)) ||
            dest_obj == 0) continue;
        uintptr_t dest_data = 0;
        if (!safe_read_bytes(dest_obj, &dest_data, sizeof(dest_data)) ||
            dest_data == 0) continue;
        const uintptr_t addrs[2]{dest_data + 0xC44, dest_data + 0xC84};
        for (int which = 0; which < 2; ++which) {
            if (!dest_clean_valid[idx][which]) continue;
            if (safe_write_bytes(addrs[which], dest_clean[idx][which],
                                 sizeof(dest_clean[idx][which]))) {
                dest_restored.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

// ======================= VIEW-ORIGIN SOURCE SCAN =========================
// WHY (2026-08-23). R18, ONE capture, BOTH eyes, doomvr_frame10201.rdc: every
// draw that paints the ceiling window - base geometry (eid 738/1064), the
// 18-index contributor (1483/2259), the 195-index draw (1791/2539) - AND the
// deferred-composite dispatch (844/1155) carry globalvieworigin
//
//     -17735.1230  -2852.0928  3160.2500  1.0
//
// byte-identical on both sides, at DIFFERENT buffer offsets (2263552 vs
// 2396416, 2314240 vs 2447104, 2328576 vs 2461440). Separate copies holding
// the same value: eye 1 shades the entire frame from eye 0 camera position
// while its own VP matrix says the camera moved. Cross-check on the same
// capture: row_i[3] == -dot(row_i.xyz, globalvieworigin) reproduces every
// printed [3] term to within print precision, so that vec3 IS the camera
// position the matrix encodes - not a coincidentally-similar field.
//
// One unshifted vec3 covers all three symptoms still open, because all three
// are view-VECTOR effects and not geometry:
//   * the hexagonal window pane - Fresnel/reflection/refraction are functions
//     of (viewOrigin - worldPos), so an origin locked to eye 0 gives both eyes
//     an identical sheen no matter how correctly the pane is transformed. The
//     owner described it as rendering without any stereo separation, like a
//     solid bitmap with no transparency.
//   * candle flames - view-oriented billboards face the view origin.
//   * sparks - particle sprites, same construction.
//
// WHY MODE 4 DID NOTHING. It patches ONE consumer, the compute dispatch copy.
// The graphics draws carry their own copies and were never touched. The owner
// reported "nothing", which is exactly what patching 1 of N consumers
// predicts. Patching the other N-1 at bind time is the same mistake N more
// times and costs per-draw work in the hot path, so this looks for the SOURCE.
//
// WHAT THIS IS. Discovery only. IT WRITES NOTHING. It solves the pre-shift VP
// for the camera position that matrix encodes, then looks for that float3
// stored as plain floats in the three places view state is known to live:
// renderView, CameraSrc, and the six sibling destination copies.
//
// SCOPE: the doubled (eye 1) call only, pre-mutation, 0x4000 bytes per region,
//   8 regions. Fires at most 3 times per process.
// SOURCE: renderView+0xC44 as apply_translation sees it (current_before), so
//   the solved origin is derived from the same bytes the lever shifts - not a
//   re-derivation that could disagree.
// POLARITY, STATED BEFORE THE RUN. HEALTHY = at least one ORIGIN hit at a
//   stable offset across the three fires. ZERO origin hits is a real answer,
//   not a failure: it says the camera position is not held as a plain float3
//   in these structures and the next move is the constant-buffer writer
//   instead. Either way the run is informative.
// POSITIVE CONTROL: the same pass also searches for the matrix OWN row0.xyz,
//   which is known to sit at +0xC44. If the control is not found in a region,
//   the scanner did not work there and any origin null for that region is
//   VOID, not a refutation.
// CAPS: 12 hits per region per vector, and a capped scan says so explicitly -
//   a report that stops exactly at the cap is truncation, not a total.
// WHICH THREE ROWS (2026-08-23). The first version always used rows 0,1,2 and
// the live probe then reported the distance to CameraSrc+0x14b0 pinned at
// exactly 3.000 across 107 of 136 samples - constant, not varying with camera
// speed, so not the frame lag it was first read as. Row 2 is the projection Z
// row and the captured projectionmatrixz is (0, 0, -1.001, -3.002): its ~3.0
// near-plane term is the obvious candidate for that bias. Row 3 is the W row,
// which carries the view basis without it. Both solves now run so the
// difference is measured rather than argued.
// Takes the three chosen rows already packed as 3 consecutive float4s.
bool solve_view_origin_packed(const float* m, float* out_p, float* out_residual);

bool solve_view_origin_rows(const float* m, const int* rows, float* out_p,
                            float* out_residual) {
    const int r0 = rows[0], r1 = rows[1], r2 = rows[2];
    const float mm[12]{m[r0*4],m[r0*4+1],m[r0*4+2],m[r0*4+3],
                       m[r1*4],m[r1*4+1],m[r1*4+2],m[r1*4+3],
                       m[r2*4],m[r2*4+1],m[r2*4+2],m[r2*4+3]};
    return solve_view_origin_packed(mm, out_p, out_residual);
}

bool solve_view_origin(const float* m, float* out_p, float* out_residual) {
    static constexpr int kRows012[3]{0, 1, 2};
    return solve_view_origin_rows(m, kRows012, out_p, out_residual);
}

bool solve_view_origin_packed(const float* m, float* out_p, float* out_residual) {
    // row_i[3] == -dot(row_i.xyz, p)  =>  M p = -b, solved by Cramer.
    const double a[3][3] = {{m[0], m[1], m[2]},
                            {m[4], m[5], m[6]},
                            {m[8], m[9], m[10]}};
    const double b[3] = {-static_cast<double>(m[3]),
                         -static_cast<double>(m[7]),
                         -static_cast<double>(m[11])};
    auto det3 = [](const double q[3][3]) {
        return q[0][0] * (q[1][1] * q[2][2] - q[1][2] * q[2][1]) -
               q[0][1] * (q[1][0] * q[2][2] - q[1][2] * q[2][0]) +
               q[0][2] * (q[1][0] * q[2][1] - q[1][1] * q[2][0]);
    };
    const double det = det3(a);
    // Rows 2 and 3 of these matrices are near-parallel (z/w of a reversed-Z
    // projection), which is why rows 0-2 are used and row 3 is ignored. A
    // near-zero determinant means this is not that layout; bail rather than
    // report a fabricated position.
    if (det < 1e-6 && det > -1e-6) return false;
    for (int c = 0; c < 3; ++c) {
        double t[3][3];
        std::memcpy(t, a, sizeof(t));
        for (int r = 0; r < 3; ++r) t[r][c] = b[r];
        out_p[c] = static_cast<float>(det3(t) / det);
    }
    if (!std::isfinite(out_p[0]) || !std::isfinite(out_p[1]) ||
        !std::isfinite(out_p[2]))
        return false;
    // Self-validation. Push the solved position back through the identity it
    // came from and report the worst row residual. Without this, a solve that
    // is quietly wrong by several units produces a search for a value that was
    // never stored, and the resulting zero looks exactly like "the field is not
    // here" - the confusable failure mode for this whole probe.
    float worst = 0.0f;
    for (int r = 0; r < 3; ++r) {
        const float* row = m + r * 4;
        const float back = row[3] + (row[0] * out_p[0] + row[1] * out_p[1] +
                                     row[2] * out_p[2]);
        const float mag = back < 0.0f ? -back : back;
        if (mag > worst) worst = mag;
    }
    if (out_residual) *out_residual = worst;
    return true;
}

// Returns hits found. `capped` is set when the cap stopped the search, so a
// count that equals the cap is never mistaken for a complete total.
size_t scan_region_for_vec3(const uint8_t* buf, size_t bytes, const float* want,
                            float tol, const char* region, const char* what,
                            bool* capped) {
    constexpr size_t kCap = 12;
    size_t hits = 0;
    *capped = false;
    // NEAREST TRACKING. A pass/fail threshold turns "the field is absent" and
    // "the field is present but my target value is slightly stale" into the
    // same zero. The nearest candidate and its distance separate them: a miss
    // by 6 units reads as one-frame camera lag, a miss by 20000 reads as
    // genuinely absent.
    double best_d2 = 1e300;
    size_t best_off = 0;
    float best_v[3]{};
    for (size_t off = 0; off + 12 <= bytes; off += 4) {
        const float* f = reinterpret_cast<const float*>(buf + off);
        if (!std::isfinite(f[0]) || !std::isfinite(f[1]) || !std::isfinite(f[2]))
            continue;
        const double dx = double(f[0]) - want[0];
        const double dy = double(f[1]) - want[1];
        const double dz = double(f[2]) - want[2];
        const double d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < best_d2) {
            best_d2 = d2;
            best_off = off;
            best_v[0] = f[0]; best_v[1] = f[1]; best_v[2] = f[2];
        }
        if (std::fabs(f[0] - want[0]) > tol) continue;
        if (std::fabs(f[1] - want[1]) > tol) continue;
        if (std::fabs(f[2] - want[2]) > tol) continue;
        float follow = 0.0f;
        const bool have_follow = (off + 16 <= bytes);
        if (have_follow) std::memcpy(&follow, buf + off + 12, sizeof(follow));
        log::warn(std::format(
            "ViewOriginScan {} {} hit offset={:#06x} value=({:.4f} {:.4f} {:.4f}) "
            "next={:.4f}{}",
            region, what, off, f[0], f[1], f[2], follow,
            (have_follow && follow == 1.0f)
                ? "  <- next==1.0, matches the constant-buffer float4 layout"
                : ""));
        if (++hits >= kCap) {
            *capped = true;
            log::warn(std::format(
                "ViewOriginScan {} {} TRUNCATED at the {} hit cap - this is a "
                "partial list, not a total.",
                region, what, kCap));
            break;
        }
    }
    if (best_d2 < 1e299) {
        log::warn(std::format(
            "ViewOriginScan {} {} nearest: offset={:#06x} value=({:.4f} {:.4f} {:.4f}) "
            "distance={:.4f}",
            region, what, best_off, best_v[0], best_v[1], best_v[2],
            std::sqrt(best_d2)));
    }
    return hits;
}

void log_view_origin_scan(uintptr_t render_view, const float* vp_before,
                          const float* solved_origin) {
    if (!render_view || !vp_before || !solved_origin) return;
    const float origin[3]{solved_origin[0], solved_origin[1], solved_origin[2]};
    // The control vector is row0.xyz, known to live at +0xC44 in renderView and
    // in each destination copy. Finding it proves the scanner reaches that
    // memory; not finding it voids that region origin result.
    const float control[3]{vp_before[0], vp_before[1], vp_before[2]};
    log::warn(std::format(
        "ViewOriginScan begin. Solved camera position from renderView+0xC44 "
        "(pre-shift) = ({:.4f} {:.4f} {:.4f}). Control vector (row0.xyz, expected "
        "at +0xC44) = ({:.4f} {:.4f} {:.4f}). Looking for the position as a plain "
        "float3; writes nothing.",
        origin[0], origin[1], origin[2], control[0], control[1], control[2]));

    struct Region { uintptr_t base; std::string name; };
    std::vector<Region> regions;
    regions.push_back({render_view, "renderView"});
    const uintptr_t doom_base =
        reinterpret_cast<uintptr_t>(GetModuleHandleW(L"DOOMx64vk.exe"));
    if (doom_base) {
        uintptr_t renderer = 0, camera_src = 0;
        if (safe_read_bytes(doom_base + 0x6FD5FB8, &renderer, sizeof(renderer)) &&
            renderer &&
            safe_read_bytes(renderer, &camera_src, sizeof(camera_src)) && camera_src) {
            regions.push_back({camera_src, "CameraSrc"});
        }
        for (uintptr_t slot = kWorldVpDestFirst; slot <= kWorldVpDestLast; slot += 8) {
            uintptr_t dest_obj = 0, dest_data = 0;
            if (!safe_read_bytes(doom_base + slot, &dest_obj, sizeof(dest_obj)) ||
                !dest_obj) continue;
            if (!safe_read_bytes(dest_obj, &dest_data, sizeof(dest_data)) ||
                !dest_data) continue;
            regions.push_back(
                {dest_data,
                 std::format("dest[{}]", (slot - kWorldVpDestFirst) / 8)});
        }
    }

    constexpr size_t kScanBytes = 0x8000;   // widened 2026-08-23; covers +0x3B00
    std::vector<uint8_t> buf(kScanBytes);
    size_t total_origin = 0, regions_scanned = 0, regions_controlled = 0;
    for (const Region& r : regions) {
        // Step down rather than give up. The window was widened to 0x8000 this
        // build; if a struct is smaller than that, a single all-or-nothing read
        // would fail and drop a region that scanned perfectly well at 0x4000
        // last run - turning eight good regions into none.
        size_t got = 0;
        for (const size_t want : {kScanBytes, size_t{0x4000}, size_t{0x2000}}) {
            if (safe_read_bytes(r.base, buf.data(), want)) { got = want; break; }
        }
        if (got == 0) {
            log::warn(std::format(
                "ViewOriginScan {}: unreadable at {} even at {:#x} bytes - region "
                "skipped, its result is neither positive nor negative.",
                r.name, reinterpret_cast<void*>(r.base), 0x2000));
            continue;
        }
        ++regions_scanned;
        bool capped_ctl = false, capped_org = false;
        const size_t ctl = scan_region_for_vec3(buf.data(), got, control,
                                                1e-4f, r.name.c_str(),
                                                "CONTROL", &capped_ctl);
        // Loose on purpose. Precision now comes from the nearest-candidate
        // line, not from this threshold; a tight threshold here only risks
        // converting a near-miss into a silent zero.
        const size_t org = scan_region_for_vec3(buf.data(), got, origin,
                                                25.0f, r.name.c_str(),
                                                "ORIGIN", &capped_org);
        if (ctl > 0) ++regions_controlled;
        total_origin += org;
        log::warn(std::format(
            "ViewOriginScan {} summary: scanned={:#x} control={} origin={}{}",
            r.name, got, ctl, org,
            ctl == 0 ? "  <- CONTROL NOT FOUND: this region origin result is "
                       "VOID, the scanner did not reach the expected bytes"
                     : ""));
    }
    log::warn(std::format(
        "ViewOriginScan end. regionsScanned={} regionsWithControl={} "
        "originHitsTotal={}. Control rate {}/{} - a zero origin count is only "
        "meaningful for regions whose control was found.",
        regions_scanned, regions_controlled, total_origin,
        regions_controlled, regions_scanned));
}

// A1 (in RenderGatherPrepare) and A2 (before RenderOpaque) both call
// apply_lever_a_mutation and are additive/non-mutex'd. WorldVpTrace
// (2026-08-11) proved renderView+0xC44 is stable across every upstream
// job between them - nothing else in the pipeline touches it - so if
// both fire in the unified-offset path, A2 would read A1's already-
// shifted value and shift it AGAIN, compounding. Guarded by this flag,
// cleared unconditionally at the top of RenderGatherPrepare (the known
// first stage each frame) and set by the first successful unified-path
// write. Legacy (non-unified) mode enum paths are unaffected since A1/A2
// were never simultaneously armed in that mode (hotkeys still write
// distinct atomics, but only one is normally toggled at a time in
// practice) - the guard only short-circuits the unified path, where
// double-application is a real bug, not the legacy path where it isn't
// exercised.
std::atomic_bool world_vp_shifted_this_frame{false};
// Lever A declined because the producer write owns the camera this run.
std::atomic_uint64_t g_lever_a_precull_suppressed{0};
// JITTER PROBE - see the block in apply_lever_a_mutation. [0]=real [1]=doubled.
std::atomic_uint64_t g_jit_applied[2]{};
std::atomic_uint64_t g_jit_flips[2]{};
std::atomic_uint64_t g_jit_double_apply[2]{};
std::atomic<float> g_jit_last_offset[2]{};
uint64_t g_jit_frame[2]{};
uint32_t g_jit_in_frame[2]{};
// Lever V declined for the same reason: viewParms+0x11F0 is built FROM the
// axis this thread rotates, so it arrives rotated with no help from the seam.
std::atomic_uint64_t g_lever_v_precull_suppressed{0};

bool apply_lever_a_mutation(uintptr_t render_view, int mode) {
    // Unified stereo offset (F5 cycle): applies the same translation to
    // renderView+0xC44/+0xC84 AND to all 6 destination struct copies
    // populated by SetupView_world's copy loop. Fixes the mid-terrain
    // glitch caused by writing to only one destination.
    const float unified = g_stereo_offset.load(std::memory_order_relaxed);
    // Census entry recorded BEFORE every early return below, so "this site
    // never ran" and "it ran and declined to write" are distinguishable in
    // the log rather than collapsing into the same zero.
    vwc_entry(kVwsLeverACur);
    vwc_entry(kVwsLeverAPrev);
    // wrap::armed() joins both guards below. Phase 3 needs this site to run
    // with NO stereo offset and NO lever mode - head rotation is a reason to
    // write the camera in its own right, and without this the whole mechanism
    // would be built, shipped and never once executed.
    // ===== PRECULL SUPPRESSES TWO HALVES OF THIS SITE - AND ONLY TWO ======
    //
    // ⚠ The first version of this returned from the whole function, AND THAT
    // WOULD HAVE COLLAPSED STEREO. This function does not only rotate: with a
    // non-zero g_stereo_offset it applies the per-eye lateral translation to
    // renderView+0xC44/+0xC84 AND to all six destination copies, it maintains
    // the render_view_restore_* snapshots the doubled frame-root call hands
    // back, and it drives lever_a_world_delta / apply_origin_shift. Under
    // PageUp - which is every headset run - g_stereo_offset is the eye
    // separation itself, so returning early here would have rendered both eyes
    // from the same point and the run would have been spent discovering that.
    //
    // So exactly two things stand down, and everything else runs untouched:
    //
    //  * THE ROTATION of current/previous. SetupView copies viewParms+0x1270
    //    into renderView+0xC44 (0x17B40B2, measured live: 2271 attempts, 2271
    //    match), so with the axis rotated at the producer, `current` is read
    //    back ALREADY rotated. Rotating it again here would double the turn
    //    rate. The translation that follows still applies - and it applies
    //    along the rotated right vector, which is exactly the ordering this
    //    function already documents for keeping the stereo baseline with the
    //    head.
    //  * THE PUBLISH. publish_reference_basis/publish_head_delta define what
    //    the whole codebase calls "the clean camera". Under precull, +0xC44 is
    //    the ROTATED camera, so publishing it here would hand every consumer -
    //    fwdfix, the freqHigh census, the corpus - a rotated matrix labelled
    //    clean, and would feed the head rotation back into the frame its own
    //    R is built in. The producer publishes instead, from a genuinely clean
    //    +0x1270 it reads before it rotates anything.
    //
    // The destination rotation inside apply_shift_to_world_vp_destinations is
    // separately gated on dest_fixed_write_enabled, which is false by default
    // and stays false here, so it cannot double-rotate either.
    //
    // Counted, not silent: a site that declines is a different finding from a
    // site that never ran, and this one is load-bearing in both modes.
    const bool precull_owns_rotation = wrap::precull_active() && wrap::armed();
    if (precull_owns_rotation)
        g_lever_a_precull_suppressed.fetch_add(1, std::memory_order_relaxed);
    const bool head_look = wrap::head_look_active();
    // ⚠ 2026-08-29: THIS EARLY RETURN USED TO STARVE EVERY HEADSET-FREE
    // CONSUMER OF THE CLEAN CAMERA, AND IT HAS NOW COST TWO RUNS.
    //
    // publish_head_delta's own comment records this defect being fixed INSIDE
    // that function: "The clean world VP and its inverse are head-INDEPENDENT
    // facts about renderView. Publishing them only when a head pose existed
    // silently starved every headset-free consumer: the 2026-08-28 19:00 flat
    // capture run died with noRef=753846." That fix was real but incomplete -
    // it removed the inner gate and left this outer one, so with headlook=off,
    // no stereo offset and no lever mode, the publish site is never REACHED at
    // all. The 20:44 flat run died the same way, noRef=820103, and took the
    // bodyfollow calibration down with it: clean_world_copy returned false on
    // every frame, so the measurement had no input.
    //
    // The publication is READ-ONLY and head-independent. It now happens
    // whenever wraparound is armed and a render view exists, and only the
    // WRITE half is gated on having something to write.
    const bool want_write =
        unified != 0.0f || mode != static_cast<int>(LeverAMode::Off) || head_look;
    if (!want_write && !wrap::armed()) return false;
    if (render_view == 0) return false;
    // Dedup widened for the same reason: A1 and A2 both firing would apply the
    // head rotation twice and double the turn rate.
    // wrap::armed() joins the condition so a publish-only frame takes the same
    // latch a writing frame would - otherwise A1 and A2 would BOTH publish
    // every frame, and the two can be looking at different render views.
    if ((unified != 0.0f || head_look || wrap::armed()) &&
        world_vp_shifted_this_frame.exchange(true, std::memory_order_relaxed)) {
        // Already shifted this frame by the other arm point - skip to
        // avoid compounding.
        vwc_deduped(kVwsLeverACur);
        vwc_deduped(kVwsLeverAPrev);
        return false;
    }
    if (render_view_layout_scan_pending.exchange(false, std::memory_order_relaxed)) {
        log_render_view_layout_scan(render_view);
    }
    alignas(16) float current[16]{};
    alignas(16) float previous[16]{};
    if (!safe_read_bytes(render_view + 0xC44, current, sizeof(current))) return false;
    if (!safe_read_bytes(render_view + 0xC84, previous, sizeof(previous))) return false;
    alignas(16) float current_before[16]{};
    alignas(16) float previous_before[16]{};
    std::memcpy(current_before, current, sizeof(current));
    // Publish renderView's CLEAN camera directions before we rotate anything.
    // Every other site is located by matching these, because only this matrix
    // has the projection-scaled layout my structural predicates assumed.
    // !precull_owns_rotation: under precull, `current` here is the ROTATED
    // camera and publishing it as "clean" would poison every consumer. The
    // producer hook publishes instead, from a genuinely clean +0x1270.
    if (wrap::armed() && !precull_owns_rotation) {
        wrap::publish_reference_basis(current);
        // A = VP_rot * inverse(VP_clean), from THIS frame's clean camera. The
        // only place both halves exist together.
        wrap::publish_head_delta(current);
    }
    // Publish-only frame: the clean camera is out, and there is nothing to
    // write. Counted, so "this site published but declined to write" stays
    // distinguishable from "this site never ran" - the whole reason the
    // vwc_entry calls at the top of this function exist.
    if (!want_write) {
        vwc_deduped(kVwsLeverACur);
        vwc_deduped(kVwsLeverAPrev);
        return false;
    }
    std::memcpy(previous_before, previous, sizeof(previous));
    // Snapshot BOTH matrices before mutating, so the doubled call can hand
    // DOOM back its own motion-vector history untouched. First writer wins:
    // a second SetupView within one doubled call must not overwrite the
    // genuine original with an already-shifted matrix.
    if (inside_frame_root_double && !render_view_restore_valid) {
        render_view_restore_addr = render_view;
        std::memcpy(render_view_restore_current, current, sizeof(current));
        std::memcpy(render_view_restore_previous, previous, sizeof(previous));
        render_view_restore_valid = true;
    }
    // ===== PHASE 3: DECOUPLED HEAD LOOK ==================================
    //
    // Applied HERE, on Lever A's path, and nowhere else. That choice is the
    // whole design:
    //
    //  * PERSISTENT AND UNRESTORED. apply_temporary_world_yaw did a scoped
    //    write/restore at the owning view and is permanently disabled for
    //    producing frame-to-frame jitter - the camera pair is consumed
    //    asynchronously by multiple render jobs, so restoring it under their
    //    feet is what jittered, not the address. This writes once per frame
    //    and leaves it, so every consumer sees one consistent camera.
    //
    //  * ALL SIX DESTINATIONS TOO, via apply_shift_to_world_vp_destinations.
    //    Writing only renderView was the mid-terrain glitch.
    //
    //  * BEFORE THE GATHER CHAIN when it fires from A1 at RenderGatherPrepare,
    //    so cull / walk_bsp / sort see the head-rotated camera. The edge
    //    pop-in that afflicts view-matrix-only VR mods does not arise here:
    //    we rotate before visibility is computed, not after.
    //
    //  * THE GUI/VIEWMODEL VIEW IS NOT TOUCHED. That is a different matrix
    //    (SetupView_gui -> (*(arg1+0x18))+0x11F0, "what every downstream
    //    GUI/viewmodel draw actually reads"). Leaving it alone is precisely
    //    what keeps the gun, hands and body aiming where the mouse points
    //    while the world camera turns away from them. The decoupling is free
    //    because the two view paths were already separate.
    //
    // BEFORE apply_translation, deliberately: the translation shifts along the
    // matrix's own right vector, so rotating first makes the per-eye lateral
    // offset follow the head. Translate-then-rotate would leave the stereo
    // baseline pinned to the body and collapse the separation as you look
    // sideways. The destination copies apply the same two in the same order.
    //
    // Both matrices get the SAME rotation: they are the current/previous pair
    // for temporal effects and motion vectors and must stay consistent.
    // ===== SLOT 3: LEVER A ROTATION ON/OFF - THE COMPOSITION TEST =======
    //
    // THE HALF-ANGLE RUN IDENTIFIED THE MECHANISM. At half rotation the room
    // objects went UNDER-rotated (they follow the head) while the GUN alone
    // kept counter-rotating - and counter-rotating at the right rate is what a
    // world-locked object does. So at half angle the gun was CORRECT and
    // everything else was wrong: the gun needs HALF what the rest needs, i.e.
    // its chain applies a head rotation TWICE where every other population
    // gets it once.
    //
    // The only population composed from BOTH rotated cameras would be one
    // built as renderView(Lever A) x guiView(Lever V) x model. Walls use Lever
    // A alone (1x, correct); room objects use Lever V alone (1x, correct); the
    // reticle uses neither (0x, follows the head). The weapon is the only
    // thing that would land at 2x.
    //
    // This tests exactly that, and nothing else in this thread predicts the
    // same outcome: with Lever A's rotation OFF and Lever V's ON, the WALLS
    // must break and follow the head, while the GUN must become correct -
    // dropping from two rotations to one. Any other combination refutes the
    // composition.
    // SLOT 2 (2026-08-27) gates the LEGACY derived-matrix rotations - this
    // renderView pair write and Lever V's GUI write - as one arm, so the
    // tactic run can flip old/new/both/neither live. Default ON: with no
    // keypress this build renders exactly like the previous one. NOTE the old
    // slot-2 label "Lever V head rotation ON/OFF (standing control)" was never
    // wired to anything - exp_on(1) had zero consumers before this edit.
    // !precull_owns_rotation: THIS is the half precull replaces. `current` was
    // read from renderView+0xC44, which DOOM itself has already filled from
    // the rotated viewParms+0x1270, so rotating it here would double the turn
    // rate. Everything after this block - the stereo translation, the six
    // destination copies, the restore snapshots, the write - still runs.
    // `|| config().precull`: in a precull build, slot 2 means PRECULL ON/OFF,
    // not "legacy rotations on/off". Without this clause the kill switch would
    // turn precull off and leave exp_on(1) false, so the legacy arm it is
    // supposed to restore would render with NO head rotation at all - a dead
    // camera that reads as the new site failing. The legacy rotation is still
    // suppressed while precull is on, by the flag below.
    if (head_look && (exp_on(1) || wrap::config().precull) && !precull_owns_rotation) {
        float head_yaw = 0.0f, head_pitch = 0.0f, head_roll = 0.0f;
        if (wrap::residual_head_angles(head_yaw, head_pitch, head_roll)) {
            // ===== THE PREVIOUS MATRIX GETS THE PREVIOUS ROTATION (2026-08-28)
            //
            // Until now +0xC84 was rotated with the CURRENT frame's head
            // angles. DOOM's temporal passes difference and BLEND current
            // against previous; handing both the same rotation corrupts
            // exactly those passes - and the 09:49 run measured the damage
            // numerically: over-rotated populations sit at FRACTIONAL slopes
            // (-1.38, -1.63), which only blended cameras produce, and the
            // remove-k correction's POST landed at -0.50 instead of -1.00
            // because integer surgery cannot fix a blend. The engine's
            // temporal math is owed a consistent pair: current rotation on
            // current, previous frame's rotation on previous.
            static uint64_t s_prev_frame = 0;
            static float s_prev_angles[3]{};
            static float s_frame_angles[3]{};
            static bool s_have_prev = false;
            const uint64_t frame_now = presented_frame.load(std::memory_order_relaxed);
            if (frame_now != s_prev_frame) {
                s_prev_angles[0] = s_frame_angles[0];
                s_prev_angles[1] = s_frame_angles[1];
                s_prev_angles[2] = s_frame_angles[2];
                s_have_prev = s_prev_frame != 0;
                s_prev_frame = frame_now;
                s_frame_angles[0] = head_yaw;
                s_frame_angles[1] = head_pitch;
                s_frame_angles[2] = head_roll;
            }
            const float py = s_have_prev ? s_prev_angles[0] : head_yaw;
            const float pp = s_have_prev ? s_prev_angles[1] : head_pitch;
            const float pr = s_have_prev ? s_prev_angles[2] : head_roll;
            const bool ok_current = wrap::apply_head_rotation(
                current, head_yaw, head_pitch, head_roll, wrap::head_axes());
            const bool ok_previous = wrap::apply_head_rotation(
                previous, py, pp, pr, wrap::head_axes());
            // Counted, not assumed. A zero here while armed means the content
            // never followed the head while the declared pose did - which is
            // exactly how the 23:18 run produced a screen welded to the face.
            if (ok_current && ok_previous) wrap::note_head_rotation_applied();
            else wrap::note_head_rotation_refused();
        } else {
            wrap::note_head_rotation_refused();
        }
    }
    if (unified != 0.0f) {
        apply_translation(current, unified);
        apply_translation(previous, unified);
        // Also shift all 6 sibling destination copies so every downstream
        // job - regardless of which destination it happens to read - sees
        // a consistently-shifted world VP. This is what kills the
        // mid-terrain glitch: previously only renderView (one of the 6)
        // was written.
        // Publish the world vector the camera just moved by, for the fog
        // repair. Taken from current_before, which is the matrix as
        // apply_translation saw it, so `right` here is the same `right` it
        // used - not a re-derivation that could disagree.
        // ===== JITTER PROBE (2026-08-30) ===================================
        // Owner: "both eyes have a very obvious jitter with PageUp engaged,
        // rock solid without." Both eyes rules out shared temporal history;
        // AA off rules out TAA; the destination rebase is landing (rebased
        // climbing, noClean=0, no DRIFT warnings). What is left is the stereo
        // offset itself landing inconsistently.
        //
        // world_vp_shifted_this_frame is cleared at RenderGatherPrepare, which
        // runs ONCE PER FRAME-ROOT ENTRY - so twice per frame under doubling.
        // If the real call does not reliably get the same sign every frame,
        // the whole scene steps back and forth by the IPD and BOTH eyes
        // jitter. This records the offset each pass actually applied, keyed by
        // which pass it was, so "the real call alternates" and "the real call
        // is stable" are distinguishable instead of inferred.
        {
            const int side = inside_frame_root_double ? 1 : 0;
            g_jit_applied[side].fetch_add(1, std::memory_order_relaxed);
            const float prev = g_jit_last_offset[side].exchange(unified, std::memory_order_relaxed);
            if (prev != 0.0f && prev != unified)
                g_jit_flips[side].fetch_add(1, std::memory_order_relaxed);
            // Per-frame ordinal: how many times THIS pass applied an offset in
            // one frame. Anything but 1 is the fault - two writes in one frame
            // on one side means the dedup latch was cleared under us.
            const uint64_t f = presented_frame.load(std::memory_order_relaxed);
            if (g_jit_frame[side] != f) { g_jit_frame[side] = f; g_jit_in_frame[side] = 0; }
            if (++g_jit_in_frame[side] > 1)
                g_jit_double_apply[side].fetch_add(1, std::memory_order_relaxed);
        }
        lever_a_world_delta[0] = current_before[0] * unified;
        lever_a_world_delta[1] = current_before[1] * unified;
        lever_a_world_delta[2] = current_before[2] * unified;
        lever_a_world_delta_doubled = inside_frame_root_double;
        // Lever O rides the same delta Lever A just used, in the doubled call
        // only - eye 0 is wrong in the same way and is deliberately left alone,
        // so any change the owner sees is eye 1 moving toward eye 0 rather than
        // the whole scene shifting.
        if (inside_frame_root_double &&
            lever_o_enabled.load(std::memory_order_relaxed)) {
            apply_origin_shift(lever_a_world_delta);
        }
        apply_shift_to_world_vp_destinations(unified);
    } else if (head_look) {
        // PHASE 3 WITH NO STEREO OFFSET. The six destination copies still need
        // the head rotation, or half the downstream jobs render the world from
        // the body's facing and half from the head's - the mid-terrain glitch
        // again, except now it would be the world tearing as the head turns.
        // A zero offset makes apply_translation a no-op; the rotation inside
        // is the point.
        apply_shift_to_world_vp_destinations(0.0f);
        lever_a_applied.fetch_add(1, std::memory_order_relaxed);
        // Symmetric with lever_v_clean_rebased, and for the same reason: the
        // census only arms on a toggle, so a run can go minutes with no record
        // of whether EITHER lever is still applying. Reported on PluginHealth
        // every 180 presents. If Lever A's delta keeps climbing while Lever
        // V's flatlines, the static world still has depth and only the objects
        // lost it - which is a completely different failure from the whole
        // scene going flat, and the two are indistinguishable by eye in a
        // headset.
        lever_a_applied.fetch_add(1, std::memory_order_relaxed);
    } else {
        switch (static_cast<LeverAMode>(mode)) {
            case LeverAMode::Yaw90Deg:
                apply_yaw_rotation(current, kLeverAYawRadians);
                apply_yaw_rotation(previous, kLeverAYawRadians);
                break;
            case LeverAMode::TranslateLateral5:
                apply_translation(current, kLeverATranslateOffsetSmall);
                apply_translation(previous, kLeverATranslateOffsetSmall);
                break;
            case LeverAMode::TranslateLateral100:
                apply_translation(current, kLeverATranslateOffsetLarge);
                apply_translation(previous, kLeverATranslateOffsetLarge);
                break;
            default: return false;
        }
    }
    safe_write_bytes(render_view + 0xC44, current, sizeof(current));
    safe_write_bytes(render_view + 0xC84, previous, sizeof(previous));
    // ===== DID OUR CAMERA WRITE SURVIVE? (2026-08-24) ====================
    //
    // The census proves the write LANDS - it reads back the shifted value
    // immediately. It cannot prove the value is still there when the GPU
    // consumes it. If DOOM's cinematic camera rewrites renderView after we
    // patch it, our write is reverted between here and the draw, and both eyes
    // render the same view - which is exactly the byte-identical pair measured
    // at the cutscene, with every counter we own still reporting health.
    //
    // "A write that reads back proves nothing" is a rule this project has paid
    // for before. This records the address and the value we left, so the
    // present hook can re-read it at end of frame and say whether it held.
    // Only the doubled call is tracked: it is the one whose divergence matters.
    if (inside_frame_root_double) {
        lever_a_persist_addr.store(render_view + 0xC44, std::memory_order_relaxed);
        lever_a_persist_wrote.store(current[12], std::memory_order_relaxed);
        lever_a_persist_valid.store(true, std::memory_order_relaxed);
    }
    vwc_write(kVwsLeverACur, render_view + 0xC44, unified, current_before, current);
    vwc_write(kVwsLeverAPrev, render_view + 0xC84, unified, previous_before, previous);
    return true;
}

// Shared shape heuristic for "does this 64-byte dynamic-uniform record look
// like the main world view/projection matrix" - extracted from the F10 CPU
// camera scan's inline check (originally what correlated renderView+0xC44)
// so TrueStereoGpuProbe below can reuse the exact same, already-validated
// thresholds instead of a second hand-copied set of magic numbers.
bool looks_like_world_camera_matrix(const float* f) {
    const float l0 = std::sqrt(f[0]*f[0] + f[1]*f[1] + f[2]*f[2]);
    const float l1 = std::sqrt(f[4]*f[4] + f[5]*f[5] + f[6]*f[6]);
    const float l2 = std::sqrt(f[8]*f[8] + f[9]*f[9] + f[10]*f[10]);
    const float l3 = std::sqrt(f[12]*f[12] + f[13]*f[13] + f[14]*f[14]);
    return l0 > 0.85f && l0 < 1.05f &&
           l1 > 1.65f && l1 < 1.90f &&
           l2 > 0.90f && l2 < 1.10f &&
           l3 > 0.90f && l3 < 1.10f;
}

bool safe_read_bytes(uintptr_t address, void* dest, size_t size) {
    if (!address) return false;
    __try {
        std::memcpy(dest, reinterpret_cast<const void*>(address), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool safe_write_bytes(uintptr_t address, const void* src, size_t size) {
    if (!address) return false;
    __try {
        std::memcpy(reinterpret_cast<void*>(address), src, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Sums every slot's draw-command count (+0x38000, same field
// AppendDrawCommandProbe reads) in one shot - a single read-only pass over
// 16 known-small integers, not a hook, so it costs nothing to call twice
// per WidenGatherFovProbe attempt.
int64_t sum_pass_table_counts(uintptr_t pass_table_base) {
    if (!pass_table_base) return -1;
    int64_t total = 0;
    for (int slot = 0; slot < kPassSlotCount; ++slot) {
        int32_t count = 0;
        if (safe_read_bytes(pass_table_base + slot * kPassSlotStride + 0x38000, &count, sizeof(count)))
            total += count;
    }
    return total;
}

// Isolated from hook_doom_render_opaque because MSVC forbids __try/__except
// in a function that also has C++ objects with destructors in scope (e.g.
// the std::string temporaries std::format produces) - "object unwinding"
// error C2712.
bool safe_call_render_opaque(void* arg1, DoomRenderOpaqueFn fn) {
    __try {
        fn(arg1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// UpstreamPipelineProbe: read-only diagnostic answering whether DOOM's
// upstream visibility pipeline (RenderGatherPrepare/RenderGatherWorld
// SurfacesPrepare/RenderCull/RenderWalkBSP/RenderGatherFinish/RenderSort -
// the jobs that run before RenderOpaque each frame and, per static
// analysis, build the exact draw-command list RenderOpaque's dispatch loop
// consumes) shares RenderOpaque's own arg1 context pointer, or uses
// separate job-specific contexts. This determines whether re-invoking them
// for TrueStereo's extra call (to make the diverged-camera eye also get a
// diverged visibility list, not just a diverged transform - the leading
// theory for why every camera-write variant tried so far produced
// identical severe corruption regardless of angle) can reuse the same
// arg1 we already have, or needs its own separately-discovered context.
// Answering this live instead of guessing from struct-layout differences
// alone - WalkBSP's own fields don't resemble RenderOpaque's at a glance,
// but that alone doesn't rule out a shared struct with many job-specific
// regions. Unconditional, bounded, matches the existing always-on
// "RenderOpaque sample=..." diagnostic already in this file - no hotkey
// needed, just launch and play normally.
constexpr uint32_t kMaxUpstreamProbeSamples = 30;
// WalkBSPCullSourceProbe (2026-08-09) - see hook_render_walk_bsp. 1 sample
// every 45 real frames (~0.5-0.75s apart, matching UpstreamPipelineReinvoke's
// SPREAD OUT pacing), 20 samples total - a ~10-15s automatic, always-on,
// read-only capture giving ample time to move the head between samples.
constexpr uint32_t kMaxWalkBspCullSourceProbeSamples = 20;
constexpr uint32_t kWalkBspCullSourceProbeFrameGap = 45;
std::atomic_uint32_t walk_bsp_cull_source_probe_sample_count{0};
std::atomic_uint32_t walk_bsp_cull_source_probe_frame_count{0};
std::atomic_uint32_t gather_prepare_sample_count{0};
std::atomic_uint32_t gather_world_surfaces_sample_count{0};
std::atomic_uint32_t cull_sample_count{0};
std::atomic_uint32_t walk_bsp_sample_count{0};
std::atomic_uint32_t gather_finish_sample_count{0};
std::atomic_uint32_t sort_sample_count{0};
std::atomic_uint32_t gather_models_sample_count{0};
std::atomic_uint32_t gather_decals_sample_count{0};
std::atomic_uint32_t gather_lights_sample_count{0};
std::atomic_uint32_t gather_add_always_sample_count{0};
// Updated on every call (not just the first kMaxUpstreamProbeSamples) so
// hook_doom_render_opaque's own comparison log always has each job's most
// recent arg1 from THIS frame to compare against, even after each job's
// own per-call logging has capped out.
std::atomic_uintptr_t last_gather_prepare_arg1{0};
std::atomic_uintptr_t last_gather_world_surfaces_arg1{0};
std::atomic_uintptr_t last_cull_arg1{0};
std::atomic_uintptr_t last_walk_bsp_arg1{0};
std::atomic_uintptr_t last_gather_finish_arg1{0};
std::atomic_uintptr_t last_sort_arg1{0};
std::atomic_uintptr_t last_gather_models_arg1{0};
std::atomic_uintptr_t last_gather_decals_arg1{0};
std::atomic_uintptr_t last_gather_lights_arg1{0};
std::atomic_uintptr_t last_gather_add_always_arg1{0};
// Phase 1 (STEREO-IMPLEMENTATION-PLAN.md v2): RenderDepth (DOOM+0x1A36C10).
// Not part of the original upstream job set this project instrumented -
// added specifically because RenderOpaque's framebuffer depth attachment is
// confirmed (RE-NOTES, live VkImageView cross-reference) to be RenderDepth's
// own output, loadOp=LOAD not CLEAR, and neither eye's RenderOpaque call has
// ever had its own private depth before now - both read DOOM's single,
// real, center-camera depth buffer, which Attempt 5's pixel evidence points
// to as the actual cause of near-geometry collapse (EQUAL depth compare
// against stale prepass data). inside_render_depth brackets only the REAL
// (non-redirected) invocation, same shape as inside_render_opaque, so
// hook_cmd_begin_render_pass can identify and capture RenderDepth's own
// render pass (separate from RenderOpaque's) exactly once.
std::atomic_uint32_t render_depth_sample_count{0};
std::atomic_uintptr_t last_render_depth_arg1{0};
thread_local bool inside_render_depth = false;

// WidenGatherFovProbe (F5, 2026-08-09): answers the key open question for
// the frustum-widening strategy - does the natural once-per-frame gather
// pipeline actually derive its visibility decisions from the SAME render-
// view object TrueStereo already knows how to clone and rotate, or from
// some separately-cached/independent frustum data computed elsewhere?
// Cheaper than trying to find and patch scattered internal cull thresholds
// (RenderGatherPrepare/RenderCull/RenderWalkBSP each reference different,
// only partially understood per-object test functions) - if rotating the
// SAME render-view before the real gather runs measurably changes what
// gets gathered (via AppendDrawCommandProbe's already-live counts), that
// confirms the connection and the render-view clone is the right lever to
// pull; if not, gather's frustum data comes from somewhere else entirely.
// Read-only from gameplay's perspective except for the deliberately large
// rotation applied to a CLONE (never DOOM's shared object, same discipline
// as TrueStereo) - swapped in only around the real RenderGatherPrepare
// call, restored immediately after, before Cull/WalkBSP/RenderOpaque run.
std::atomic_uintptr_t captured_render_view_slot{0};
// Same idea as captured_render_view_slot, but for the pass-table base
// (RenderOpaque's arg1+0x130 - see UpstreamPipelineReinvoke's identical
// read) - added 2026-08-09 so WidenGatherFovProbe can read real pass-slot
// counts directly around its own WalkBSP call instead of piggybacking on
// AppendDrawCommandProbe's per-call hook, which turned out to fire ~660
// times per WalkBSP call in a busy scene - logging every one of those
// during F5's armed window reproduced the exact self-inflicted logging-
// burst artifact this project already root-caused once (see RE-NOTES.md
// pitfall). A single before/after slot-count read costs one log line per
// attempt, not thousands.
std::atomic_uintptr_t captured_pass_table_base{0};
std::atomic_bool widen_gather_probe_enabled{false};
std::atomic_uint32_t widen_gather_probe_attempt_count{0};
constexpr uint32_t kMaxWidenGatherProbeAttempts = 20;
// RESULT at 1.2 rad (2026-08-09): object count for the dominant slot
// jumped from a few dozen to 100+ - confirms gather DOES derive visibility
// from this render-view - but visible icon/HUD artifacts. RESULT at 0.05
// rad (realistic toe-in magnitude): identical artifact, unchanged. RESULT
// moving the swap from RenderGatherPrepare to RenderWalkBSP: identical,
// unchanged. RESULT forcing DOOM+0x7061830's sync-vs-async job flag to
// synchronous for the swap window: identical, unchanged (syncFlagForced
// confirmed true in the log, so the write itself wasn't the issue).
// TEMPORARILY 0.0 - a pure byte-identical clone swap, no rotation at all,
// the same "control" test that proved every other mechanism this session
// clean at 0 deviation. If this is ALSO corrupted, the swap itself is
// unsafe during this phase regardless of content - a different, more
// fundamental issue than anything tried so far. If clean, the problem is
// specifically about what rotating produces, not the swap mechanism.
//
// RESULT at 0.0 rad: STILL corrupted, identical to every other variant.
// That rules out rotation entirely (already 3x ruled out) AND rules out
// content/correctness within the copied region - the only thing left that
// explains "any swap breaks it regardless of content" is the clone buffer
// itself being too small. 4096 bytes was sized for RenderOpaque's own
// camera-bind usage (the only region ever confirmed read, ending at
// 0xCC4) - exactly why Stage 1's same-sized clone was clean when scoped to
// RenderOpaque's re-entrant call. Gather-phase code may read fields
// further into the object that a 4096-byte clone never had valid data
// for, independent of rotation - RE-NOTES.md flagged "full render-view
// object size unknown, don't assume it's small" from early in the
// project. Raised drastically (over-copy, matching this project's
// established discipline) to test that directly.
//
// RESULT (2026-08-09, at 64KB, true no-op): STILL corrupted, byte-for-byte
// the same symptom, despite doing NOTHING. That proved the swap mechanism
// itself is clean and the artifact was never a render-view/gather bug at
// all - see RE-NOTES.md's "Methodology pitfall" entry: F5's OWN arm
// handler was resetting AppendDrawCommandProbe's sample-count budget,
// unlocking a 3000-line logging burst on this same worker thread. Removed
// that reset; the artifact disappeared completely, confirmed live. Gather-
// phase render-view manipulation is confirmed safe at the mechanism level.
//
// NEXT (2026-08-09): reintroducing the real clone-and-rotate swap (was
// reduced to a no-op purely to isolate the confound above) at a real toe-in
// angle instead of the 0.0 rad control - reusing TrueStereo's
// already-validated magnitude so the widened frustum covers the same span
// a diverged eye will actually need.
constexpr float kWidenGatherProbeRadians = kTrueStereoToeInRadians;
alignas(16) uint8_t widen_gather_probe_clone_buffer[65536];

void log_upstream_job_call(const char* name, void* arg1, std::atomic_uint32_t& sample_count,
    std::atomic_uintptr_t& last_arg1) {
    last_arg1.store(reinterpret_cast<uintptr_t>(arg1), std::memory_order_relaxed);
    const uint32_t sample = sample_count.fetch_add(1, std::memory_order_relaxed);
    if (sample >= kMaxUpstreamProbeSamples) return;
    log::info(std::format("UpstreamPipelineProbe job={} sample={} arg1={} thread={}",
        name, sample, arg1, GetCurrentThreadId()));
}

void __fastcall hook_render_gather_prepare(void* arg1) {
    FbDoom fbd_(FB_D_RENDER_GATHER_PREPARE);
    // FLOOR RUNG 2. Returns before the instrumentation as well as before the
    // real call - at the floor the point is that this ran not at all, and a
    // probe line saying otherwise would be the confusable result.
    if (doubled_skip_gather_now()) return;
    log_upstream_job_call("RenderGatherPrepare", arg1, gather_prepare_sample_count,
        last_gather_prepare_arg1);
    log_world_vp_at("RenderGatherPrepare");
    // VIEWSRC ordering marker. Bumped here and cleared at the PRESENT (not
    // here) so the producer probe can say whether it ran before or after A1
    // within the same frame. Unconditional - a marker gated on any arm state
    // would make "before" and "probe disarmed" the same reading.
    g_vs_a1_calls_this_frame.fetch_add(1, std::memory_order_relaxed);
    // RenderGatherPrepare is the pipeline's first stage each frame (see
    // job order in docs/RE-NOTES.md). Clear the once-per-frame dedup
    // guard here, unconditionally, before A1's own mutation block below
    // reads it.
    world_vp_shifted_this_frame.store(false, std::memory_order_relaxed);
    // The FREQHIGH per-frame clears used to live here - WRONG SITE, found
    // 2026-08-28 evening: gather-prepare runs once per frame-root INVOCATION
    // and the doubled call precedes the real one (gun8: pass1=+1.5uu=doubled,
    // pass2=-1.5=real), so this site cleared the tracked list MID-frame and
    // the submit-time writes only ever reached the second pass. All three
    // clears moved to the present hook - the actual frame boundary, after
    // every submit of the frame.
    // Lever A1 (docs/CAMERA-LEVER-PLAN.md §3): persistent, unrestored,
    // no-cap camera clobber BEFORE the gather chain runs. Uses
    // captured_render_view_slot (populated by hook_doom_render_opaque on
    // the previous frame - the slot address is renderer-lifetime stable;
    // only the render-view object it points to changes per frame). If A1
    // and A2 both fire, A2's later write overwrites A1's - a valid A/B
    // when only one is armed, useless when both are, so the hotkey
    // handlers mutually exclude them.
    {
        const int mode = lever_a1_mode.load(std::memory_order_relaxed);
        const bool unified_active = g_stereo_offset.load(std::memory_order_relaxed) != 0.0f;
        // wrap::armed() joins this guard: Phase 3 head look is a reason to write
        // the camera even with no stereo offset and no lever mode armed.
        if (mode != static_cast<int>(LeverAMode::Off) || unified_active || wrap::armed()) {
            const uintptr_t render_view_slot =
                captured_render_view_slot.load(std::memory_order_relaxed);
            uintptr_t render_view = 0;
            if (render_view_slot &&
                safe_read_bytes(render_view_slot, &render_view, sizeof(render_view))) {
                apply_lever_a_mutation(render_view, mode);
            }
        }
    }
    // AFTER Lever A1: this site should read rotated-once. A gather job that
    // reads clean here is running BEFORE this write in frame order (or on a
    // camera copy this write does not reach) - that is the gen-0 producer.
    sample_camera_state_at(CS_GatherPrepare);
    // WidenGatherFovProbe (F5) - MOVED BACK here from RenderWalkBSP
    // (2026-08-09). Originally lived here; moved away because a live test
    // at this location showed icon-row/HUD corruption, attributed at the
    // time to a "snapshot" theory (GatherPrepare caching camera fields into
    // its own internal state before our swap could take effect). That
    // theory was never actually confirmed - what really happened, root-
    // caused later the same session (see RE-NOTES.md's Methodology
    // pitfall), is that F5's own arm handler was resetting a shared
    // logging budget (AppendDrawCommandProbe's sample count), unlocking a
    // multi-thousand-line burst that itself produced the icon-row/HUD
    // symptom - independent of anything this swap did. Moving to WalkBSP
    // was chasing a phantom bug, and WalkBSP turned out to have ZERO
    // measurable effect on gather anyway (40/40 attempts, burst-free
    // before/after pass-table telemetry, passTableCountAfter ==
    // passTableCountBefore every single time, 2026-08-09 19:03) - almost
    // certainly because RenderCull, which runs BEFORE WalkBSP in the real
    // pipeline order (GatherPrepare -> GatherWorldSurfacesPrepare -> Cull
    // -> WalkBSP -> GatherFinish -> Sort), already does the actual
    // visibility/append work by the time WalkBSP runs. GatherPrepare is the
    // ONE location that ever showed a real, live-measured effect (1.2 rad:
    // object count jumped from dozens to 100+) - moving back now that the
    // confound explaining its apparent corruption is understood, fixed,
    // and replaced by burst-free telemetry.
    if (widen_gather_probe_enabled.load(std::memory_order_relaxed)) {
        const uint32_t attempt = widen_gather_probe_attempt_count.load(std::memory_order_relaxed);
        if (attempt >= kMaxWidenGatherProbeAttempts) {
            log::info("WidenGatherFovProbe reached its sample cap; disabling automatically");
            widen_gather_probe_enabled.store(false, std::memory_order_relaxed);
            { FbTramp fbt_(FB_D_RENDER_GATHER_PREPARE); real_render_gather_prepare(arg1); }
            return;
        }
        widen_gather_probe_attempt_count.fetch_add(1, std::memory_order_relaxed);
        // Same clone-and-swap discipline as TrueStereo (F1): never write
        // DOOM's shared render-view object, only a CLONE, swap the shared
        // slot to point at it, restore immediately after this call
        // returns. captured_render_view_slot/captured_pass_table_base come
        // from the PREVIOUS frame's hook_doom_render_opaque call (this
        // job's own arg1 isn't the render-view/pass-table-holding object).
        uintptr_t render_view_slot = captured_render_view_slot.load(std::memory_order_relaxed);
        uintptr_t original_render_view = 0;
        bool swapped = false;
        if (render_view_slot != 0 &&
            safe_read_bytes(render_view_slot, &original_render_view, sizeof(original_render_view)) &&
            original_render_view != 0 &&
            safe_read_bytes(original_render_view, widen_gather_probe_clone_buffer,
                sizeof(widen_gather_probe_clone_buffer))) {
            // Rotate both the current (+0xC44) and previous (+0xC84)
            // matrices in the CLONE identically - same two-matrix
            // discipline as TrueStereo, per FORBIDDEN-BOUNDARIES.md "Never
            // assume" #3.
            apply_yaw_rotation(reinterpret_cast<float*>(widen_gather_probe_clone_buffer + 0xc44),
                kWidenGatherProbeRadians);
            apply_yaw_rotation(reinterpret_cast<float*>(widen_gather_probe_clone_buffer + 0xc84),
                kWidenGatherProbeRadians);
            const uintptr_t clone_address = reinterpret_cast<uintptr_t>(widen_gather_probe_clone_buffer);
            swapped = safe_write_bytes(render_view_slot, &clone_address, sizeof(clone_address));
        }
        const uintptr_t pass_table_base = captured_pass_table_base.load(std::memory_order_relaxed);
        const int64_t count_before = sum_pass_table_counts(pass_table_base);
        log::warn(std::format(
            "WidenGatherFovProbe attempt={} renderViewSlot={} originalRenderView={} swapped={} "
            "toeInRadians={:.2f} passTableCountBefore={}",
            attempt, reinterpret_cast<void*>(render_view_slot),
            reinterpret_cast<void*>(original_render_view), swapped, kWidenGatherProbeRadians,
            count_before));
        { FbTramp fbt_(FB_D_RENDER_GATHER_PREPARE); real_render_gather_prepare(arg1); }
        const int64_t count_after = sum_pass_table_counts(pass_table_base);
        log::warn(std::format(
            "WidenGatherFovProbe attempt={} passTableCountAfter={} delta={}",
            attempt, count_after, count_after - count_before));
        if (swapped) {
            const bool restored = safe_write_bytes(render_view_slot, &original_render_view,
                sizeof(original_render_view));
            if (!restored) {
                log::error("WidenGatherFovProbe: CRITICAL - failed to restore the original "
                    "render-view pointer; the shared slot may now point at our clone buffer. "
                    "Disabling permanently, do not re-arm this session - investigate before "
                    "trying again.");
                widen_gather_probe_enabled.store(false, std::memory_order_relaxed);
            }
        }
        return;
    }
    { FbTramp fbt_(FB_D_RENDER_GATHER_PREPARE); real_render_gather_prepare(arg1); }
}
void __fastcall hook_render_gather_world_surfaces_prepare(void* arg1) {
    FbDoom fbd_(FB_D_RENDER_GATHER_WORLD_SURFACES_PREPARE);
    if (doubled_skip_gather_now()) return;
    log_upstream_job_call("RenderGatherWorldSurfacesPrepare", arg1,
        gather_world_surfaces_sample_count, last_gather_world_surfaces_arg1);
    log_world_vp_at("GatherWorldSurfaces");
    { FbTramp fbt_(FB_D_RENDER_GATHER_WORLD_SURFACES_PREPARE); real_render_gather_world_surfaces_prepare(arg1); }
}
void __fastcall hook_render_cull(void* arg1) {
    FbDoom fbd_(FB_D_RENDER_CULL);
    if (doubled_skip_gather_now()) return;
    log_upstream_job_call("RenderCull", arg1, cull_sample_count, last_cull_arg1);
    log_world_vp_at("RenderCull");
    { FbTramp fbt_(FB_D_RENDER_CULL); real_render_cull(arg1); }
}

// Every DOOM-memory read below goes through safe_read_bytes, which is
// itself SEH-guarded per read (see its definition) - the same protection
// every other read-only probe in this file relies on. No outer __try here:
// std::format requires object unwinding, which MSVC will not compile inside
// a __try, and there is nothing left to guard once the reads are safe.
// CvarStateDump (2026-08-16) - read-only, one-shot, fired from the PageUp
// arm handler.
//
// A FrameRootProbe run with `+set jobs_forceSingleThreaded 1` on the
// command line came back byte-identical to the run without it: both
// selectors still 1. That is two very different situations wearing the same
// face - "the cvar does not drive these selectors" versus "the cvar was
// never actually set" - and nothing logged so far can tell them apart. This
// dumps the evidence that can:
//
//   1. The process command line, which settles whether the launch option
//      reached DOOM at all.
//   2. The raw bytes of the `jobs_forceSingleThreaded` idCVar object
//      (DOOM+0x5841C80, from its registration site), plus any ASCII reachable
//      through its pointer-shaped fields - so its live value is readable
//      without having to guess idCVar's exact layout first.
//   3. The job-system struct around the flag at DOOM+0x7061830. The flag is
//      only ever COMPARED by rip-relative code, never written, so it is a
//      field written through a base pointer; seeing its neighbours is the
//      way to identify the struct.
void dump_cvar_and_job_system_state() {
    const uintptr_t doom_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"DOOMx64vk.exe"));
    if (!doom_base) {
        log::error("CvarStateDump: DOOMx64vk.exe module handle unavailable");
        return;
    }

    if (const wchar_t* cmd = GetCommandLineW()) {
        const int need = WideCharToMultiByte(CP_UTF8, 0, cmd, -1, nullptr, 0, nullptr, nullptr);
        std::string narrow(need > 0 ? need - 1 : 0, '\0');
        if (need > 0) {
            WideCharToMultiByte(CP_UTF8, 0, cmd, -1, narrow.data(), need, nullptr, nullptr);
        }
        log::warn(std::format("CvarStateDump commandLine=[{}]", narrow));
    } else {
        log::error("CvarStateDump: GetCommandLineW returned null");
    }

    // Helper: hexdump `count` bytes at an absolute address, and for each
    // 8-byte slot that looks like a readable pointer, print what it points at
    // if it is printable ASCII.
    auto dump_region = [&](const char* label, uintptr_t addr, size_t count) {
        std::string hex;
        hex.reserve(count * 3);
        for (size_t i = 0; i < count; ++i) {
            uint8_t b = 0;
            if (!safe_read_bytes(addr + i, &b, sizeof(b))) { hex += " ??"; continue; }
            char tmp[4];
            std::snprintf(tmp, sizeof(tmp), " %02X", b);
            hex += tmp;
        }
        log::warn(std::format("CvarStateDump {} @{:#x} bytes:{}", label, addr, hex));

        for (size_t off = 0; off + 8 <= count; off += 8) {
            uintptr_t p = 0;
            if (!safe_read_bytes(addr + off, &p, sizeof(p)) || p < 0x10000) continue;
            char text[64]{};
            bool printable = true, any = false;
            for (size_t i = 0; i < sizeof(text) - 1; ++i) {
                uint8_t c = 0;
                if (!safe_read_bytes(p + i, &c, sizeof(c))) { printable = false; break; }
                if (c == 0) break;
                if (c < 0x20 || c > 0x7e) { printable = false; break; }
                text[i] = static_cast<char>(c);
                any = true;
            }
            if (printable && any) {
                log::warn(std::format("CvarStateDump {} +{:#04x} -> {:#x} = \"{}\"",
                    label, off, p, text));
            }
        }
    };

    // RVAs from this session's static analysis. Same "RVAs are evidence, not
    // stable addresses" caveat as everything else in docs/RE-NOTES.md.
    // idCVar layout, decoded from the first live dump: +0x10 holds a pointer
    // to the current value STRING, which for short values points at an inline
    // buffer at +0x1c; +0x18 is the flags word. Confirmed by jobs_numThreads
    // reading "15" against a "-1" default (the engine resolves -1 to
    // hyperthreads-minus-one at init and writes the resolved value back), so
    // this field really is the live value and not the default.
    //
    // Every object address below is read directly off its own registration
    // site via the static cvar extraction, not guessed.
    struct CvarProbe { const char* name; uintptr_t rva; const char* expect; };
    static constexpr CvarProbe kCvarProbes[] = {
        // The cvar under test.
        {"r_useSMP  <== THE SWITCH", 0x5D48350, "default 2; 1=separate thread, 2=jobs"},
        {"jobs_forceSingleThreaded", 0x5841C80, "default 0 (measured inert)"},
        {"jobs_numThreads",          0x5841B40, "default -1"},
        // CONTROLS. These two are already on the user's Steam launch line in
        // the same `+cvar value` form. If they show their non-default values
        // and jobs_forceSingleThreaded does not, the command-line syntax
        // works and that cvar is special. If none of them took, the syntax
        // itself is wrong and `+set cvar value` is needed instead. Without
        // these the test cannot tell those two cases apart.
        {"com_skipIntroVideo (CONTROL)", 0x5D4DF90, "default 0, launch line says 1"},
        {"r_renderAPI (CONTROL)",        0x672ADA0, "default -1, launch line says -2"},
        // Finer-grained render-threading levers, for reference.
        {"r_threadedCommit",         0x6FD5C00, "default 0"},
        {"r_threadedUpdateInView",   0x6FD5CA0, "default 2"},
        {"stereoRender_separation",  0x5FB8870, "default 1.5"},
        {"stereoRender_swapEyes",    0x5FB8A50, "default 0"},
    };
    for (const auto& c : kCvarProbes) {
        const uintptr_t obj = doom_base + c.rva;
        uintptr_t value_ptr = 0;
        uint32_t flags = 0;
        char value[64]{};
        bool ok = safe_read_bytes(obj + 0x10, &value_ptr, sizeof(value_ptr));
        safe_read_bytes(obj + 0x18, &flags, sizeof(flags));
        if (ok && value_ptr) {
            for (size_t i = 0; i < sizeof(value) - 1; ++i) {
                uint8_t ch = 0;
                if (!safe_read_bytes(value_ptr + i, &ch, sizeof(ch)) || ch == 0) break;
                if (ch < 0x20 || ch > 0x7e) { value[i] = '?'; break; }
                value[i] = static_cast<char>(ch);
            }
        }
        log::warn(std::format("CvarStateDump cvar {:<32} value=\"{}\" flags={:#010x}  ({})",
            c.name, value, flags, c.expect));
    }
    dump_region("job-system struct", doom_base + 0x7061810, 0x50);
}

// ForceSingleThreadedToggle (PageDown, 2026-08-16).
//
// Sets `jobs_forceSingleThreaded` directly through its idCVar object rather
// than fighting Steam's command line, which cost two inconclusive runs. Safe
// to do: the cvar's own flags are 0x80000014 = CVAR_FLOAT | CVAR_NOCHEAT |
// bit31, decoded from DOOM's own CVAR_ enum value table. It carries none of
// CVAR_CHEAT (0x8), CVAR_ROM (0x8000, read-only), CVAR_INIT (0x4000,
// command-line-only) or CVAR_SHIPPINGDISABLED (0x400000) - an ordinary,
// unprotected, runtime-settable cvar.
//
// idCVar layout, decoded byte-by-byte from the first CvarStateDump rather
// than assumed (jobs_numThreads read "15"/15/15.0f against a "-1" default,
// which pins all three value fields at once):
//   +0x00  vtable
//   +0x10  pointer to the value STRING (points at the inline buffer at +0x1c
//          for short values)
//   +0x18  flags
//   +0x1c  inline value-string buffer
//   +0x30  integer value
//   +0x34  float value   <- the field the stereoRender_* consumer reads
//
// Fully reversible: pressing again writes "0"/0/0.0f back.
//
// EXPECTATION, stated in advance so a null result is not over-read: the job
// system's own flag at DOOM+0x7061830 is almost certainly derived when the
// job system initialises, so setting the cvar mid-run may well NOT flip it.
// If the cvar changes and the flag does not, that is informative, not a
// failure - it means the next step is finding the code that reads this cvar
// and writes that flag.
// RETARGETED 2026-08-16 to r_useSMP after jobs_forceSingleThreaded was
// measured inert for the render path.
//
// jobs_forceSingleThreaded was set successfully at runtime (value/int/float
// all confirmed written) and changed NOTHING: thread topology after the
// write was 2/21 frames "inline-looking", identical to the 2/21 baseline and
// exactly what chance predicts for ~11 worker threads. Its 8 read sites are
// all in the generic job-system core, and the render path's Enqueue does not
// consult it. Disproven, cleanly.
//
// The real switch, found by walking the write path of the flag itself:
//   JobDispatch reads   *(DOOM+0x7061830)
//   written by          DOOM+0x1A331E1, inside the setter DOOM+0x1A331A0,
//                       straight from its 4th argument (r9b)
//   called by           DOOM+0x186ECF0 (render-frame entry; mode in edx)
//   whose main-scene    DOOM+0x1558BF5:  cmp dword [0x145D48380], 2
//   call site does      DOOM+0x1558C10:  sete r9b
//
// 0x145D48380 is r_useSMP's integer field (cvar object 0x145D48350 + 0x30).
//   r_useSMP: "1 = run the renderer in a separate thread, 2 = run the
//              renderer in jobs"   default 2
// So syncFlag = (r_useSMP == 2), and r_useSMP != 2 makes every one of
// JobDispatch's 20 render-job dispatches take its inline branch. This is the
// structural analogue of BioShock's `1t`.
//
// IMPORTANT CAVEAT, and the reason a launch-time set is preferred over this
// hotkey: r_useSMP has NINETEEN read sites across the renderer. This is
// precisely the situation BioShock's own history warns about - their
// equivalent global poke crashed on level transitions because the global had
// other consumers. Flipping it mid-run may leave other subsystems holding
// assumptions from the old value. The safe first experiment is `+r_useSMP 1`
// on the command line so everything initialises consistently; this hotkey
// exists for A/B comparison within one session, not as the mechanism.
constexpr uintptr_t kUseSmpCvarRva = 0x5D48350;
std::atomic_bool force_single_threaded_applied{false};
std::atomic_uint32_t use_smp_reassert_count{0};
std::atomic_bool splitscreen_applied{false};

void log_job_system_flag(const char* when) {
    const uintptr_t doom_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"DOOMx64vk.exe"));
    uint8_t flag = 0xFF;
    if (doom_base) safe_read_bytes(doom_base + 0x7061830, &flag, sizeof(flag));
    log::warn(std::format("ForceSingleThreaded {} jobSystemFlag(DOOM+0x7061830)={}", when, flag));
}

void read_cvar_triplet(uintptr_t obj, char* str_out, size_t str_cap,
                       int32_t* int_out, float* float_out) {
    if (str_out && str_cap) {
        str_out[0] = '\0';
        uintptr_t vp = 0;
        if (safe_read_bytes(obj + 0x10, &vp, sizeof(vp)) && vp) {
            for (size_t i = 0; i + 1 < str_cap; ++i) {
                uint8_t ch = 0;
                if (!safe_read_bytes(vp + i, &ch, sizeof(ch)) || ch == 0) break;
                str_out[i] = (ch >= 0x20 && ch <= 0x7e) ? static_cast<char>(ch) : '?';
                str_out[i + 1] = '\0';
            }
        }
    }
    if (int_out) { *int_out = 0; safe_read_bytes(obj + 0x30, int_out, sizeof(*int_out)); }
    if (float_out) { *float_out = 0.0f; safe_read_bytes(obj + 0x34, float_out, sizeof(*float_out)); }
}

void toggle_force_single_threaded() {
    const uintptr_t doom_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"DOOMx64vk.exe"));
    if (!doom_base) {
        log::error("ForceSingleThreaded: DOOMx64vk.exe module handle unavailable");
        return;
    }
    const uintptr_t obj = doom_base + kUseSmpCvarRva;

    char before[32]{}; int32_t before_i = 0; float before_f = 0.0f;
    read_cvar_triplet(obj, before, sizeof(before), &before_i, &before_f);
    log::warn(std::format(
        "ForceSingleThreaded BEFORE r_useSMP cvarObj={:#x} value=\"{}\" int={} float={:.3f}",
        obj, before, before_i, before_f));
    log_job_system_flag("BEFORE");

    // 2 = "run the renderer in jobs" (DOOM's default, async).
    // 1 = "run the renderer in a separate thread" -> syncFlag becomes 0 and
    //     every render-job dispatch takes JobDispatch's inline branch.
    const bool turn_on = !force_single_threaded_applied.load(std::memory_order_relaxed);
    const char* text = turn_on ? "1" : "2";
    const int32_t iv = turn_on ? 1 : 2;
    const float fv = turn_on ? 1.0f : 2.0f;

    // Write the inline string buffer, then the numeric fields. The value
    // pointer at +0x10 already points at +0x1c for a value this short, so it
    // does not need rewriting - verified in the live dump (0x...1c9c == obj
    // + 0x1c). Deliberately not touching +0x10 for that reason.
    char buf5[2] = {text[0], '\0'};
    const bool ok_s = safe_write_bytes(obj + 0x1c, buf5, sizeof(buf5));
    const bool ok_i = safe_write_bytes(obj + 0x30, &iv, sizeof(iv));
    const bool ok_f = safe_write_bytes(obj + 0x34, &fv, sizeof(fv));

    char after[32]{}; int32_t after_i = 0; float after_f = 0.0f;
    read_cvar_triplet(obj, after, sizeof(after), &after_i, &after_f);
    log::warn(std::format(
        "ForceSingleThreaded AFTER  writeOk(s/i/f)={}/{}/{} value=\"{}\" int={} float={:.3f}",
        ok_s, ok_i, ok_f, after, after_i, after_f));
    log_job_system_flag("AFTER");

    force_single_threaded_applied.store(turn_on, std::memory_order_relaxed);
    log::warn(std::format(
        "PageDown: r_useSMP now {} (2=renderer in jobs/async, 1=separate thread/inline). If the cvar "
        "changed but the job-system flag did NOT follow, the flag is latched rather than re-read; "
        "it is computed per frame at DOOM+0x1558BF5, so it SHOULD follow. Press PageUp after.",
        turn_on ? "1 (INLINE)" : "2 (async, DOOM default)"));
}

// ---------------------------------------------------------------------------
// FrameRootDouble Stage 1 (Pause key, 2026-08-16)
//
// The first re-entry experiment in this project that targets the FRAME ROOT
// rather than something below the camera bake. Every prior attempt - Track
// A's extra RenderOpaque calls, Track B's queued job, Avenue 1's GPU write -
// re-entered at or under draw-submit, downstream of where DOOM bakes the
// camera into its draw commands. DOOM+0x17B7900 is upstream of all of it:
// its call tree contains SetupView (camera sample), the gather/cull/sort
// scene build, and RenderView (draw + composite). BioShock's rule for an SR
// root, satisfied exactly.
//
// This is only attemptable now because r_useSMP=1 makes the whole chain run
// inline on one thread (confirmed live: 21/21 frames, 1 thread, no queue
// handoff). Doubling it under the threaded regime would be re-entering a
// live job graph - the thing that has broken every previous attempt.
//
// STAGE 1 IS DELIBERATELY BORING: it calls the frame root a second time with
// the IDENTICAL arguments and no camera change whatsoever. Same discipline
// as RenderViewClone Stage 1 and UpstreamPipelineReinvoke Stage A - establish
// that a second invocation is survivable BEFORE adding divergence, so that a
// failure has exactly one possible cause. Stage 2 (applying the already-proven
// Lever W camera shift between the two calls, so the second call's own
// SetupView bakes a diverged camera) is NOT implemented here on purpose.
//
// Refuses to arm unless r_useSMP is actually 1 - doubling under the threaded
// regime is not a test this project should run by accident.
std::atomic_bool frame_root_double_enabled{false};
std::atomic_uint32_t frame_root_double_attempts{0};
// THE TWO-PART HITCH ON ENTERING VR (2026-08-24). Owner-reported, then found
// in the log with 143 ms between the two halves:
//
//   t+9   ms  XR LAYER -> PROJECTION      fullFrame=false, real=0.000 doubled=3.000
//   t+152 ms  grade applies               fullFrame=true,  real=-1.500 doubled=1.500
//
// The layer flipped to stereo the instant doubling armed, because
// override_eye1_only is just `frame_root_live`. But the state that makes that
// stereo CORRECT - symmetric camera offsets and the graded eye-1 mirror -
// arrives ~150 ms later, after mirror convergence and apply_graded_mode(2). So
// the headset showed a half-built pair first (eye 0 unshifted, eye 1 shifted a
// full 3.0 and ungraded) and then snapped to the real one. Two transitions
// where the owner should see one.
//
// This flag holds presentation on the flat quad for that window. The doubled
// call still runs throughout - it has to, since convergence is what we are
// waiting for - only its OUTPUT is withheld until the final state is ready.
// Then the layer flips once, straight into the correct configuration.
//
// ⚠ SET ONLY BY THE FAST TOGGLE. It defaults false and PageUp never touches
// it, so the scripted test's behaviour is unchanged. Do not repurpose it as a
// general "suppress stereo" switch: anything that leaves it true with the
// toggle idle would look exactly like VR silently failing to arm.
std::atomic_bool fast_vr_hold_presentation{false};
constexpr uint32_t kMaxFrameRootDoubleAttempts = 20;
// Stage 3 divergence magnitude. Deliberately far larger than DOOM's own
// half-separation (1.5 uu, per stereoRender_separation "world units from
// center to eyes") for the FIRST test: the question here is binary - does the
// doubled call render a different viewpoint at all - and a large offset makes
// that unmissable in the dumped image. Tune to 1.5 only once divergence is
// confirmed to work at all.
// A/B magnitude. Raised from 20 to 200 on 2026-08-16 to settle a specific
// doubt rather than to look nicer: at 20 uu the measured disparity was only
// ~74 px on the nearest geometry (~1.7 deg), which is smaller than a 20-unit
// lateral move ought to produce. That is consistent either with the row
// samples cross-correlating poorly over large flat regions, or with the
// offset being scaled/clamped somewhere - i.e. partial rather than full
// camera control. 200 uu makes the difference between those two cases
// obvious by eye, before anything larger gets built on the assumption.
// Raised to 2000 on 2026-08-16. At 200 uu the lit interior walls measured
// 0.16-0.34 mean abs luma difference - i.e. exactly unmoved - while
// mid-distance terrain moved a lot. Confirming Lever W applies (CameraSrc
// +0x3AC0 verifiably shifted at draw time) changed the rendered image not at
// all, so only renderView+0xC44 reaches the draws, and it evidently does not
// reach whatever draws the near interior surfaces.
//
// 2000 uu is far outside the room. If the interior walls are still pixel-
// identical at that magnitude they are definitively not consuming the matrix
// we shift, and the search moves to what they DO consume. If they finally
// distort or vanish, the shift does reach them and the earlier result was a
// magnitude/scale problem instead.
constexpr float kFrameRootDoubleIpd = 20.0f;
std::atomic<float> frame_root_double_ipd{kFrameRootDoubleIpd};
// ===== THE SPLIT TEST FOR THE TRANSIENT ARTIFACTS (2026-08-30) =============
//
// The white flashes and the flame flicker are transient, random, and appear
// ONLY under doubling and ONLY during movement. They cannot be caught by hand
// - the owner is right that reacting in time to hit a capture key is not a
// test - so instead of catching one, make the cause stop being random.
//
// Doubling changes TWO things at once: the frame is rendered twice, and the
// two passes use DIFFERENT cameras (+/- half the separation). Setting the
// separation to zero holds everything else identical - two passes, two
// mirrors, every code path the same - and removes only the camera difference.
//
//   artifacts VANISH  -> the cause is the two passes disagreeing about the
//                        camera while sharing per-frame state (light lists,
//                        emitter state). Whole family confirmed; the fix is
//                        per-pass state rather than shared.
//   artifacts PERSIST -> camera divergence is innocent and it is the doubling
//                        mechanism itself. Kills an entire branch, which is
//                        worth as much.
//
// Stereo depth is lost while this is set - that is the point of it being a
// diagnostic and not a setting to keep.
std::atomic<float> g_ipd_override{-1.0f};  // <0 = follow the scripted value; set from config at present
inline float effective_double_ipd() {
    const float o = g_ipd_override.load(std::memory_order_relaxed);
    return o >= 0.0f ? o : frame_root_double_ipd.load(std::memory_order_relaxed);
}
// ===== IPDHB - THE PER-PASS HEARTBEAT (2026-08-30 evening) =================
//
// The A/B is worthless unless the log proves WHAT EACH PASS ACTUALLY
// RENDERED WITH. Reading the keypress, or reading g_ipd_override at the top
// of the frame, would report the ARM LABEL and not the measurement: the
// value is consumed at two separate sites, on the real call and on the
// doubled call, and either one could fail to see it (the doubled call has
// three other paths - zero_offset, ab_zero_pass, null_control - that force
// its offset to 0 independently, and full_frame picks half vs full). So the
// offset each pass has ALREADY COMPUTED is recorded AT THAT SITE, after
// every branch, and printed once a second.
//
// READING IT, polarity fixed in advance:
//   p1n / p2n climbing together   - both passes running; the doubling is live.
//   p2n == 0 while p1n climbs     - PG UP was never pressed. Every ipd
//                                   judgement in the run is void, not null.
//   arm=ON and p1last/p2last both 0.000 - the override reached BOTH passes.
//                                   This is the middle approach's proof.
//   arm=ON but either last != 0   - the key toggled a flag that never
//                                   reached the pass. Broken probe, not a
//                                   refutation of the hypothesis.
//   refreshSup climbing while armed - the per-frame config refresh is
//                                   standing down as intended (see its site).
std::atomic_uint64_t g_ipdhb_p1_n{0};
std::atomic_uint64_t g_ipdhb_p1_zero{0};
std::atomic<float>   g_ipdhb_p1_last{-999.0f};
std::atomic_uint64_t g_ipdhb_p2_n{0};
std::atomic_uint64_t g_ipdhb_p2_zero{0};
std::atomic<float>   g_ipdhb_p2_last{-999.0f};
std::atomic_uint64_t g_ipd_refresh_applied{0};
std::atomic_uint64_t g_ipd_refresh_suppressed{0};
std::atomic_uint64_t g_ipd_toggles{0};
// Records the offset a pass has already computed. pass 1 = the real
// frame-root call, pass 2 = the injected doubled call. No branching of its
// own: whatever the site decided is what lands here.
inline void ipdhb_note(int pass, float offset) {
    const bool zero = std::fabs(offset) < 1e-6f;
    if (pass == 1) {
        g_ipdhb_p1_n.fetch_add(1, std::memory_order_relaxed);
        if (zero) g_ipdhb_p1_zero.fetch_add(1, std::memory_order_relaxed);
        g_ipdhb_p1_last.store(offset, std::memory_order_relaxed);
    } else {
        g_ipdhb_p2_n.fetch_add(1, std::memory_order_relaxed);
        if (zero) g_ipdhb_p2_zero.fetch_add(1, std::memory_order_relaxed);
        g_ipdhb_p2_last.store(offset, std::memory_order_relaxed);
    }
}
std::atomic_int frame_root_double_dump_label{0};
// A/B mode = the 20-attempt burst that writes the two comparison PPMs. Live
// mode = continuous, no dumps (each dump costs a vkQueueWaitIdle plus a 23 MB
// readback and file write, which is what produced the 1-2 s freeze), eye 1
// presented to the headset.
std::atomic_bool frame_root_double_ab{false};
std::atomic_bool frame_root_double_null_control{false};
std::atomic_uint32_t frame_root_double_cap{kMaxFrameRootDoubleAttempts};
// Live separation. stereoRender_separation is documented as "world units from
// center to eyes", i.e. already a half-IPD, so DOOM own full eye-to-eye is
// 2 * 1.5 = 3.0 uu. Eye 0 is DOOM real unshifted frame, so eye 1 carries the
// full separation to give the correct relative disparity between the pair.
constexpr float kFrameRootDoubleLiveIpd = 3.0f;  // full eye-to-eye; each eye gets +/- half
constexpr uint32_t kFrameRootDoubleLiveCap = 200000;
// Guards against our own second call re-entering this hook and recursing.

// ============ FRAME TIME BUDGET (2026-08-25) ================================
//
// WHY. The pause menu drops 80.0 -> 19.3 fps (12.5 -> 51.8 ms/frame) and
// recovers the instant it closes. Already eliminated, each from the 09:07 log:
// log volume (~80 lines in 11 s), the PPM dumps (all four at 09:08:4x),
// the XR span (XR FRAME SLOW never fired at 45+ ms/frame, and it triggers at
// 25), clone churn (`made` frozen at 2038 through the whole menu), descriptor
// traffic (updCalls=36), and the draw count itself - 987 draws at ratio 1.00
// at BOTH 19 fps and 80 fps. So ~39 ms/frame is unattributed, and guessing at
// it has no better odds than measuring it.
//
// WHAT THIS IS. A per-frame CPU time budget, accumulated in atomics and
// emitted once per second. The star field is `residual`: frame time minus
// everything we can name. A large residual means the cost is in DOOM's own
// work and none of ours - which is a real answer, and one that no amount of
// staring at our code would produce.
//
// ⚠ AN INSTRUMENT THAT COSTS TIME DISTORTS WHAT IT MEASURES. That is the whole
// hazard of timing a performance bug, so the probe is bounded by construction
// and PUBLISHES ITS OWN COST: the per-clock-read price is calibrated once at
// startup, and `probeMs` is the measured overhead this frame, printed on the
// same line so it can be subtracted rather than argued about.
//
// The high-rate hooks are split by `inside_frame_root_double` so their cost is
// not double-counted inside `doubled`. Only the `_real` halves enter the
// residual; the `_dbl` halves are already inside `doubled` and are printed as
// informational sub-costs.
// Scopes measured inside the present hook.
std::atomic_uint64_t fb_pre_ns{0};       // our work before xr().poll
std::atomic_uint64_t fb_xr_ns{0};        // xr().poll - the whole XR frame
std::atomic_uint64_t fb_present_ns{0};   // real_queue_present - compositor back-pressure
std::atomic_uint64_t fb_post_ns{0};      // our work after the present returns
// Scopes measured during DOOM's frame, outside the present hook.
std::atomic_uint64_t fb_doubled_ns{0};   // the whole injected second render
std::atomic_uint64_t fb_bind_real_ns{0}, fb_bind_dbl_ns{0};
std::atomic_uint64_t fb_barrier_real_ns{0}, fb_barrier_dbl_ns{0};
std::atomic_uint64_t fb_update_real_ns{0}, fb_update_dbl_ns{0};
std::atomic_uint64_t fb_bind_calls{0}, fb_barrier_calls{0}, fb_update_calls{0};
// The GPU-wait trio. These are the calls that BLOCK, and they were the hole in
// the first version of this instrument - all three landed in RESIDUAL, where
// "DOOM's own CPU work" and "the CPU waiting for the second render we injected"
// look identical. Split out, `gpuWait` answers the only question that matters:
// is the menu slow because of work we added to the GPU?
std::atomic_uint64_t fb_submit_ns{0}, fb_acquire_ns{0}, fb_fence_ns{0};
std::atomic_uint64_t fb_submit_calls{0}, fb_acquire_calls{0}, fb_fence_calls{0};
// THE LAST HOLE. We hook 26 vkCmd* entry points and the budget timed three of
// them, so 23 hooks - every draw, every pipeline bind, every viewport and
// render-pass call - landed in RESIDUAL indistinguishable from DOOM's own CPU
// work. One shared bucket collapses them into a single number, which is all
// that is needed to answer ours-or-theirs. If `otherHooks` is ~0 the menu cost
// is definitively DOOM's; if it is tens of ms it is ours and the next step is
// to bisect WITHIN this bucket, not to keep guessing.
std::atomic_uint64_t fb_other_real_ns{0}, fb_other_dbl_ns{0}, fb_other_calls{0};
// ---- GENERATED FROM THE add_hook REGISTRATION LIST, NOT HAND-PICKED ----
// 53 Vulkan exports. The previous version of this instrument timed 16 of
// them and I called it exhaustive; it was not, and the conclusion built on
// it was withdrawn. These ids are generated from the registration list so
// the two cannot drift, and install() cross-checks them at startup.
// Declared ahead of the hook tables below, which use them.
std::atomic_uint64_t fb_frames{0};
enum FbHookId {
    FB_H_ACQUIRE_NEXT_IMAGE,
    FB_H_ALLOCATE_DESCRIPTOR_SETS,
    FB_H_BIND_BUFFER_MEMORY,
    FB_H_CMD_BEGIN_QUERY,
    FB_H_CMD_BEGIN_RENDER_PASS,
    FB_H_CMD_BEGIN_RENDERING,
    FB_H_CMD_BEGIN_RENDERING_KHR,
    FB_H_CMD_BIND_DESCRIPTOR_SETS,
    FB_H_CMD_BIND_INDEX_BUFFER,
    FB_H_CMD_BIND_PIPELINE,
    FB_H_CMD_BIND_VERTEX_BUFFERS,
    FB_H_CMD_BLIT_IMAGE,
    FB_H_CMD_COPY_BUFFER_TO_IMAGE,
    FB_H_CMD_COPY_IMAGE,
    FB_H_CMD_DISPATCH,
    FB_H_CMD_DISPATCH_INDIRECT,
    FB_H_CMD_DRAW,
    FB_H_CMD_DRAW_INDEXED,
    FB_H_CMD_DRAW_INDEXED_INDIRECT,
    FB_H_CMD_DRAW_INDIRECT,
    FB_H_CMD_END_QUERY,
    FB_H_CMD_END_RENDER_PASS,
    FB_H_CMD_PIPELINE_BARRIER,
    FB_H_CMD_PUSH_DESCRIPTOR_SET,
    FB_H_CMD_RESET_QUERY_POOL,
    FB_H_CMD_RESOLVE_IMAGE,
    FB_H_CMD_SET_SCISSOR,
    FB_H_CMD_SET_VIEWPORT,
    FB_H_CMD_WAIT_EVENTS,
    FB_H_CREATE_BUFFER,
    FB_H_CREATE_DESCRIPTOR_UPDATE_TEMPLATE,
    FB_H_CREATE_DEVICE,
    FB_H_CREATE_FRAMEBUFFER,
    FB_H_CREATE_GRAPHICS_PIPELINES,
    FB_H_CREATE_IMAGE,
    FB_H_CREATE_IMAGE_VIEW,
    FB_H_CREATE_INSTANCE,
    FB_H_CREATE_RENDER_PASS,
    FB_H_CREATE_SWAPCHAIN,
    FB_H_DESTROY_FRAMEBUFFER,
    FB_H_DESTROY_IMAGE_VIEW,
    FB_H_DESTROY_SWAPCHAIN,
    FB_H_ENUMERATE_PHYSICAL_DEVICES,
    FB_H_FREE_MEMORY,
    FB_H_GET_DEVICE_QUEUE,
    FB_H_GET_SWAPCHAIN_IMAGES,
    FB_H_MAP_MEMORY,
    FB_H_QUEUE_PRESENT,
    FB_H_QUEUE_SUBMIT,
    FB_H_UNMAP_MEMORY,
    FB_H_UPDATE_DESCRIPTOR_SET_WITH_TEMPLATE,
    FB_H_UPDATE_DESCRIPTOR_SETS,
    FB_H_WAIT_FOR_FENCES,
    FB_H_DEVICE_WAIT_IDLE,
    FB_H_QUEUE_WAIT_IDLE,
    FB_H_GET_QUERY_POOL_RESULTS,
    FB_H_ALLOCATE_MEMORY,
    FB_H_FLUSH_MAPPED_MEMORY_RANGES,
    FB_H_ALLOCATE_COMMAND_BUFFERS,
    FB_H_FREE_COMMAND_BUFFERS,
    FB_H_BEGIN_COMMAND_BUFFER,
    FB_H_END_COMMAND_BUFFER,
    FB_H_RESET_FENCES,
    FB_H_CREATE_FENCE,
    FB_H_DESTROY_FENCE,
    FB_H_RESET_DESCRIPTOR_POOL,
    FB_H_CREATE_DESCRIPTOR_POOL,
    FB_H_DESTROY_DESCRIPTOR_POOL,
    FB_H_CREATE_COMPUTE_PIPELINES,
    FB_H_CREATE_SHADER_MODULE,
    FB_H_DESTROY_SHADER_MODULE,
    FB_H_DESTROY_IMAGE,
    FB_H_DESTROY_BUFFER,
    FB_H_DESTROY_PIPELINE,
    FB_H_CMD_COPY_BUFFER,
    FB_H_CMD_COPY_IMAGE_TO_BUFFER,
    FB_H_CMD_WRITE_TIMESTAMP,
    FB_H_CMD_SET_DEPTH_BIAS,
    FB_H_CMD_SET_STENCIL_COMPARE_MASK,
    FB_H_CMD_SET_STENCIL_REFERENCE,
    FB_H_BIND_IMAGE_MEMORY,
    FB_H_CREATE_COMMAND_POOL,
    FB_H_DESTROY_COMMAND_POOL,
    FB_H_CREATE_DESCRIPTOR_SET_LAYOUT,
    FB_H_DESTROY_DESCRIPTOR_SET_LAYOUT,
    FB_H_CREATE_PIPELINE_LAYOUT,
    FB_H_DESTROY_PIPELINE_LAYOUT,
    FB_H_CREATE_QUERY_POOL,
    FB_H_DESTROY_QUERY_POOL,
    FB_H_CREATE_SAMPLER,
    FB_H_CREATE_SEMAPHORE,
    FB_H_DESTROY_SEMAPHORE,
    FB_H_DESTROY_RENDER_PASS,
    FB_H_GET_BUFFER_MEMORY_REQUIREMENTS,
    FB_H_GET_IMAGE_MEMORY_REQUIREMENTS,
    FB_H_ENUMERATE_DEVICE_EXTENSION_PROPERTIES,
    FB_H_GET_PHYSICAL_DEVICE_FEATURES,
    FB_H_GET_PHYSICAL_DEVICE_FORMAT_PROPERTIES,
    FB_H_GET_PHYSICAL_DEVICE_MEMORY_PROPERTIES,
    FB_H_GET_PHYSICAL_DEVICE_PROPERTIES,
    FB_H_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_PROPERTIES,
    FB_H_GET_PHYSICAL_DEVICE_SURFACE_CAPABILITIES_KHR,
    FB_H_GET_PHYSICAL_DEVICE_SURFACE_FORMATS_KHR,
    FB_H_GET_PHYSICAL_DEVICE_SURFACE_PRESENT_MODES_KHR,
    FB_H_GET_PHYSICAL_DEVICE_SURFACE_SUPPORT_KHR,
    FB_H_CREATE_WIN32_SURFACE_KHR,
    // Added 2026-08-25 after the coverage audit caught them: my scan
    // regex was [a-zA-Z]* and silently excluded every export with a
    // DIGIT in its name, so the whole *2 / *2KHR family was invisible.
    FB_H_CMD_BEGIN_RENDER_PASS2,
    FB_H_CMD_BEGIN_RENDER_PASS2_KHR,
    FB_H_CMD_PIPELINE_BARRIER2,
    FB_H_CREATE_RENDER_PASS2,
    FB_H_CREATE_RENDER_PASS2_KHR,
    FB_H_COUNT
};
const char* const kFbHookExport[FB_H_COUNT] = {
    "vkAcquireNextImageKHR",
    "vkAllocateDescriptorSets",
    "vkBindBufferMemory",
    "vkCmdBeginQuery",
    "vkCmdBeginRenderPass",
    "vkCmdBeginRendering",
    "vkCmdBeginRenderingKHR",
    "vkCmdBindDescriptorSets",
    "vkCmdBindIndexBuffer",
    "vkCmdBindPipeline",
    "vkCmdBindVertexBuffers",
    "vkCmdBlitImage",
    "vkCmdCopyBufferToImage",
    "vkCmdCopyImage",
    "vkCmdDispatch",
    "vkCmdDispatchIndirect",
    "vkCmdDraw",
    "vkCmdDrawIndexed",
    "vkCmdDrawIndexedIndirect",
    "vkCmdDrawIndirect",
    "vkCmdEndQuery",
    "vkCmdEndRenderPass",
    "vkCmdPipelineBarrier",
    "vkCmdPushDescriptorSetKHR",
    "vkCmdResetQueryPool",
    "vkCmdResolveImage",
    "vkCmdSetScissor",
    "vkCmdSetViewport",
    "vkCmdWaitEvents",
    "vkCreateBuffer",
    "vkCreateDescriptorUpdateTemplate",
    "vkCreateDevice",
    "vkCreateFramebuffer",
    "vkCreateGraphicsPipelines",
    "vkCreateImage",
    "vkCreateImageView",
    "vkCreateInstance",
    "vkCreateRenderPass",
    "vkCreateSwapchainKHR",
    "vkDestroyFramebuffer",
    "vkDestroyImageView",
    "vkDestroySwapchainKHR",
    "vkEnumeratePhysicalDevices",
    "vkFreeMemory",
    "vkGetDeviceQueue",
    "vkGetSwapchainImagesKHR",
    "vkMapMemory",
    "vkQueuePresentKHR",
    "vkQueueSubmit",
    "vkUnmapMemory",
    "vkUpdateDescriptorSetWithTemplate",
    "vkUpdateDescriptorSets",
    "vkWaitForFences",
    "vkCmdBeginRenderPass2",
    "vkCmdBeginRenderPass2KHR",
    "vkCmdPipelineBarrier2",
    "vkCreateRenderPass2",
    "vkCreateRenderPass2KHR",
    "vkDeviceWaitIdle",
    "vkQueueWaitIdle",
    "vkGetQueryPoolResults",
    "vkAllocateMemory",
    "vkFlushMappedMemoryRanges",
    "vkAllocateCommandBuffers",
    "vkFreeCommandBuffers",
    "vkBeginCommandBuffer",
    "vkEndCommandBuffer",
    "vkResetFences",
    "vkCreateFence",
    "vkDestroyFence",
    "vkResetDescriptorPool",
    "vkCreateDescriptorPool",
    "vkDestroyDescriptorPool",
    "vkCreateComputePipelines",
    "vkCreateShaderModule",
    "vkDestroyShaderModule",
    "vkDestroyImage",
    "vkDestroyBuffer",
    "vkDestroyPipeline",
    "vkCmdCopyBuffer",
    "vkCmdCopyImageToBuffer",
    "vkCmdWriteTimestamp",
    "vkCmdSetDepthBias",
    "vkCmdSetStencilCompareMask",
    "vkCmdSetStencilReference",
    "vkBindImageMemory",
    "vkCreateCommandPool",
    "vkDestroyCommandPool",
    "vkCreateDescriptorSetLayout",
    "vkDestroyDescriptorSetLayout",
    "vkCreatePipelineLayout",
    "vkDestroyPipelineLayout",
    "vkCreateQueryPool",
    "vkDestroyQueryPool",
    "vkCreateSampler",
    "vkCreateSemaphore",
    "vkDestroySemaphore",
    "vkDestroyRenderPass",
    "vkGetBufferMemoryRequirements",
    "vkGetImageMemoryRequirements",
    "vkEnumerateDeviceExtensionProperties",
    "vkGetPhysicalDeviceFeatures",
    "vkGetPhysicalDeviceFormatProperties",
    "vkGetPhysicalDeviceMemoryProperties",
    "vkGetPhysicalDeviceProperties",
    "vkGetPhysicalDeviceQueueFamilyProperties",
    "vkGetPhysicalDeviceSurfaceCapabilitiesKHR",
    "vkGetPhysicalDeviceSurfaceFormatsKHR",
    "vkGetPhysicalDeviceSurfacePresentModesKHR",
    "vkGetPhysicalDeviceSurfaceSupportKHR",
    "vkCreateWin32SurfaceKHR",
};
// Dedicated-path hooks: already decomposed elsewhere, so adding them to the
// generic bucket would DOUBLE COUNT. vkQueuePresentKHR is split into
// pre/xrPoll/presentCall/post; the other three are GPUWAIT. Marked here so
// the coverage audit counts them as covered rather than missing. Generated
// in enum order, not hand-aligned.
const bool kFbDedicated[FB_H_COUNT] = {
    true,  // vkAcquireNextImageKHR
    false,  // vkAllocateDescriptorSets
    false,  // vkBindBufferMemory
    false,  // vkCmdBeginQuery
    false,  // vkCmdBeginRenderPass
    false,  // vkCmdBeginRendering
    false,  // vkCmdBeginRenderingKHR
    false,  // vkCmdBindDescriptorSets
    false,  // vkCmdBindIndexBuffer
    false,  // vkCmdBindPipeline
    false,  // vkCmdBindVertexBuffers
    false,  // vkCmdBlitImage
    false,  // vkCmdCopyBufferToImage
    false,  // vkCmdCopyImage
    false,  // vkCmdDispatch
    false,  // vkCmdDispatchIndirect
    false,  // vkCmdDraw
    false,  // vkCmdDrawIndexed
    false,  // vkCmdDrawIndexedIndirect
    false,  // vkCmdDrawIndirect
    false,  // vkCmdEndQuery
    false,  // vkCmdEndRenderPass
    false,  // vkCmdPipelineBarrier
    false,  // vkCmdPushDescriptorSetKHR
    false,  // vkCmdResetQueryPool
    false,  // vkCmdResolveImage
    false,  // vkCmdSetScissor
    false,  // vkCmdSetViewport
    false,  // vkCmdWaitEvents
    false,  // vkCreateBuffer
    false,  // vkCreateDescriptorUpdateTemplate
    false,  // vkCreateDevice
    false,  // vkCreateFramebuffer
    false,  // vkCreateGraphicsPipelines
    false,  // vkCreateImage
    false,  // vkCreateImageView
    false,  // vkCreateInstance
    false,  // vkCreateRenderPass
    false,  // vkCreateSwapchainKHR
    false,  // vkDestroyFramebuffer
    false,  // vkDestroyImageView
    false,  // vkDestroySwapchainKHR
    false,  // vkEnumeratePhysicalDevices
    false,  // vkFreeMemory
    false,  // vkGetDeviceQueue
    false,  // vkGetSwapchainImagesKHR
    false,  // vkMapMemory
    true,  // vkQueuePresentKHR
    true,  // vkQueueSubmit
    false,  // vkUnmapMemory
    false,  // vkUpdateDescriptorSetWithTemplate
    false,  // vkUpdateDescriptorSets
    true,  // vkWaitForFences
    false,  // vkCmdBeginRenderPass2
    false,  // vkCmdBeginRenderPass2KHR
    false,  // vkCmdPipelineBarrier2
    false,  // vkCreateRenderPass2
    false,  // vkCreateRenderPass2KHR
    false,  // vkDeviceWaitIdle
    false,  // vkQueueWaitIdle
    false,  // vkGetQueryPoolResults
    false,  // vkAllocateMemory
    false,  // vkFlushMappedMemoryRanges
    false,  // vkAllocateCommandBuffers
    false,  // vkFreeCommandBuffers
    false,  // vkBeginCommandBuffer
    false,  // vkEndCommandBuffer
    false,  // vkResetFences
    false,  // vkCreateFence
    false,  // vkDestroyFence
    false,  // vkResetDescriptorPool
    false,  // vkCreateDescriptorPool
    false,  // vkDestroyDescriptorPool
    false,  // vkCreateComputePipelines
    false,  // vkCreateShaderModule
    false,  // vkDestroyShaderModule
    false,  // vkDestroyImage
    false,  // vkDestroyBuffer
    false,  // vkDestroyPipeline
    false,  // vkCmdCopyBuffer
    false,  // vkCmdCopyImageToBuffer
    false,  // vkCmdWriteTimestamp
    false,  // vkCmdSetDepthBias
    false,  // vkCmdSetStencilCompareMask
    false,  // vkCmdSetStencilReference
    false,  // vkBindImageMemory
    false,  // vkCreateCommandPool
    false,  // vkDestroyCommandPool
    false,  // vkCreateDescriptorSetLayout
    false,  // vkDestroyDescriptorSetLayout
    false,  // vkCreatePipelineLayout
    false,  // vkDestroyPipelineLayout
    false,  // vkCreateQueryPool
    false,  // vkDestroyQueryPool
    false,  // vkCreateSampler
    false,  // vkCreateSemaphore
    false,  // vkDestroySemaphore
    false,  // vkDestroyRenderPass
    false,  // vkGetBufferMemoryRequirements
    false,  // vkGetImageMemoryRequirements
    false,  // vkEnumerateDeviceExtensionProperties
    false,  // vkGetPhysicalDeviceFeatures
    false,  // vkGetPhysicalDeviceFormatProperties
    false,  // vkGetPhysicalDeviceMemoryProperties
    false,  // vkGetPhysicalDeviceProperties
    false,  // vkGetPhysicalDeviceQueueFamilyProperties
    false,  // vkGetPhysicalDeviceSurfaceCapabilitiesKHR
    false,  // vkGetPhysicalDeviceSurfaceFormatsKHR
    false,  // vkGetPhysicalDeviceSurfacePresentModesKHR
    false,  // vkGetPhysicalDeviceSurfaceSupportKHR
    false,  // vkCreateWin32SurfaceKHR
};
std::atomic_uint64_t fb_hook_real_ns[FB_H_COUNT];
std::atomic_uint64_t fb_hook_dbl_ns[FB_H_COUNT];
std::atomic_uint64_t fb_hook_calls[FB_H_COUNT];
// Authoritative total: only the OUTERMOST hook contributes, so a hook that
// calls another hook cannot be counted twice. The per-hook buckets above are
// INCLUSIVE and may sum to more than this - they are for naming the culprit,
// never for the budget arithmetic.
std::atomic_uint64_t fb_all_real_ns{0}, fb_all_dbl_ns{0};
std::atomic_uint64_t fb_depth_leaks{0};
thread_local int fb_depth = 0;
thread_local uint64_t fb_outer_t0 = 0;

// "This hook has fired at least once, ever." Separate from fb_hook_calls,
// which is zeroed every window and so cannot answer "did it EVER run".
std::atomic_bool fb_hook_ever[FB_H_COUNT];

inline uint64_t fb_hook_enter(int id) {
    if (!kharvox::native::cpu::current.detailed) return 0;
    fb_hook_ever[id].store(true, std::memory_order_relaxed);
    fb_hook_calls[id].fetch_add(1, std::memory_order_relaxed);
    fb_clock_reads.fetch_add(2, std::memory_order_relaxed);
    const uint64_t t0 = fb_ns();
    if (fb_depth++ == 0) fb_outer_t0 = t0;
    return t0;
}
inline void fb_hook_exit(int id, uint64_t t0) {
    if (!t0) return;
    const uint64_t now = fb_ns();
    (inside_frame_root_double ? fb_hook_dbl_ns[id] : fb_hook_real_ns[id])
        .fetch_add(now - t0, std::memory_order_relaxed);
    if (--fb_depth <= 0) {
        fb_depth = 0;
        (inside_frame_root_double ? fb_all_dbl_ns : fb_all_real_ns)
            .fetch_add(now - fb_outer_t0, std::memory_order_relaxed);
    }
}
// RAII form. Used everywhere MSVC allows it; functions containing __try cannot
// hold an object with a destructor (C2712) and use the manual pair instead.
struct FbHook {
    kharvox::native::trace::Scope tracing;
    int id;
    uint64_t t0;
    explicit FbHook(int i) : tracing(kFbHookExport[i],uint64_t(i)),id(i), t0(fb_hook_enter(i)) {}
    ~FbHook() { fb_hook_exit(id, t0); }
};
thread_local uint64_t fb_doubled_start = 0;
// Calibrated once, on first use: the price of one fb_ns() call in ns. Timed
// over 20000 reads so a single scheduling hiccup cannot skew it.
uint64_t fb_clock_cost_ns() {
    static const uint64_t cost = [] {
        constexpr int kReads = 20000;
        const uint64_t t0 = fb_ns();
        volatile uint64_t sink = 0;
        for (int i = 0; i < kReads; ++i) sink = fb_ns();
        (void)sink;
        const uint64_t t1 = fb_ns();
        return (t1 - t0) / kReads;
    }();
    return cost;
}
// ============ END FRAME TIME BUDGET =========================================
// LEVER V RESTORE (2026-08-16). Lever V shifts the GUI/viewmodel view matrix
// at source_ctx+0x11F0, which is PERSISTENT renderer state that eye 0's
// weapon draw reads afterwards. g_stereo_offset is restored around the
// doubled call but the matrix bytes it caused to be written never were, so
// eye 0's gun kept eye 1's transform - the measured symptom being a blurry
// left-eye gun at 3.0 and 12.0 uu separation but NOT at 0.0, i.e. camera
// dependent, which the frame-root residual is not.
//
// This is the "scope the restore" that hook_setup_view_gui's own comment
// calls for, rather than disabling the lever and losing the near-geometry
// coverage it is the only source of.
thread_local uintptr_t lever_v_restore_addr = 0;
thread_local float lever_v_restore_matrix[16]{};
thread_local bool lever_v_restore_valid = false;
std::atomic_uint32_t lever_v_restore_count{0};

// THE CLEAN, PRE-REAL-CALL GUI VIEW (2026-08-22). See the use site in
// SetupView for the measurement this exists to fix: the doubled call was
// applying its shift on top of the REAL call's shift, which cancels to the
// centre instead of mirroring to the far side. The two counters live further
// up, beside vwc_leverv_restores, because the census header reports them and
// is defined above this point.
thread_local float lever_v_clean_matrix[16]{};
thread_local bool lever_v_clean_valid = false;

// ---------------------------------------------------------------------------
// FULL-FRAME architecture (2026-08-16). See
// docs/FRAME-ROOT-FINDINGS-2026-08-16.md §13 for the design and the
// CompositeTargetProbe evidence behind it.
//
// The composite holding image. Receives the REAL frame-root call's fully
// composited output (all lighting, post-process, effects) via a GPU blit
// recorded into DOOM's own command buffer. That content becomes eye 1.
// DEFAULT OFF as of 2026-08-16: the full-frame approach is disproven, see
// the FULL-FRAME CLOSURE note in docs/FRAME-ROOT-FINDINGS-2026-08-16.md.
// DOOM composite target is accumulated into rather than owned by a
// frame-root run, so doubling into DOOM live targets cannot produce two
// independent frames. Plain Pause returns to the redirect path, which is
// known to yield clean (if post-process-incomplete) images.
// OFF. The full-frame approach is now properly closed - see the FULL-FRAME
// CLOSURE (EARNED) note in docs/FRAME-ROOT-FINDINGS-2026-08-16.md §13a.
// Ordering and ping-pong parity were both ruled out by direct test; DOOM
// composite target is read-modify-write, so a doubled run cannot own it.
std::atomic_bool frame_root_full_frame{false};
// SYMMETRIC STEREO (STEP 8, 2026-08-17). Both eyes from the SAME pipeline
// stage, at +/- half the separation, so they are near-identical in quality
// by construction rather than by correction.
//
// The plan's version of STEP 8 was "run the doubled call twice into two
// mirror sets keyed (framebuffer, eye)" - a third full render per frame and
// roughly a second gigabyte of mirrors. That is not needed. We ALREADY
// render the frame root twice: DOOM's real call and our doubled one. The
// asymmetry was never the render, it was the SOURCE - eye 0 was taken from
// DOOM's finished swapchain image (graded, HUD, post-frame-root work) while
// eye 1 came from our mirror of the frame root's final composite.
//
// So: shift the REAL call by +half and take its own pre-final composite
// (the frame root's last internal target, isSwapchainImage=false - the same
// stage the mirror captures) for eye 0, and keep the doubled call at -half
// in its mirror for eye 1. Same stage, same format, same blit path, one
// separation apart. No extra render, no extra mirror set.
//
// Cost of honesty: NEITHER eye has DOOM's final grade or its HUD, because
// both are now pre-final. That is the deliberate trade - a consistent pair
// rather than a matched-quality illusion - and the HUD returns later as its
// own composition layer per docs/HUD-LAYER-DESIGN-2026-08-16.md.
std::atomic_bool symmetric_stereo_enabled{false};
std::atomic_uint32_t symmetric_log_count{0};
// Blit-only control: full-frame path with the doubled call skipped.
std::atomic_bool frame_root_blit_only{false};
// The green clear proved its point (eye 0 entirely green = nothing rewrote the
// composite after it). Off by default now; it makes the game unusable.
std::atomic_bool frame_root_clear_between{false};
struct CompositeHolding {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::atomic_bool ready{false};
    std::atomic_bool failed{false};
};
CompositeHolding composite_holding;
std::atomic_uint32_t composite_blit_count{0};

// Definitions live below find_memory_type (which they need); declared here.
bool ensure_composite_holding_ready(VkExtent2D extent, VkFormat format);
void capture_composite_to_holding();
void clear_composite_before_double();

bool safe_call_frame_root(void* a1, void* a2, DoomFrameRootFn fn) {
    __try {
        fn(a1, a2);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void snapshot_frame_start_layouts();
void rewind_mirror_layouts(VkCommandBuffer command_buffer);

void __fastcall hook_frame_root(void* arg1, void* arg2) {
    if(!kharvox::native::beginRoot()){real_frame_root(arg1,arg2);return;}
    kharvox::native::cpu::Scope profileRoot(kharvox::native::cpu::Root);
    struct OwnerRootEnd {~OwnerRootEnd(){kharvox::native::endRoot();}} ownerRootEnd;
    if(kharvox::native::leftEyeOnlyDiagnostic()&&!kharvox::native::stereoPreparationEnabled()) {
        // Before all right-eye snapshots, seeds, holding copies and replay.
        // The original scene still uses its normal LEFT camera and resolution.
        const float saved=g_stereo_offset.load(std::memory_order_relaxed);
        g_stereo_offset.store(-effective_double_ipd()*0.5f,std::memory_order_relaxed);
        world_vp_shifted_this_frame.store(false,std::memory_order_relaxed);
        last_frame_root_framebuffer=VK_NULL_HANDLE;
        last_frame_root_cmd_buffer=VK_NULL_HANDLE;
        frame_root_render_pass_count=0;doubled_render_pass_count=0;
        real_pass_seq=0;real_current_framebuffer=VK_NULL_HANDLE;real_current_job_rva=0;
        g_frame_root_serial.fetch_add(1,std::memory_order_relaxed);
        inside_real_frame_root=true;
        {kharvox::native::cpu::Scope original(kharvox::native::cpu::Original);real_frame_root(arg1,arg2);}
        inside_real_frame_root=false;
        g_stereo_offset.store(saved,std::memory_order_relaxed);
        return;
    }
    FbDoom fbd_(FB_D_FRAME_ROOT);
    // FULL-FRAME: the REAL call renders eye 1 (+half). Its fully composited
    // output is blitted to the holding image immediately after it returns.
    const bool full_frame = frame_root_full_frame.load(std::memory_order_relaxed) &&
                            frame_root_double_enabled.load(std::memory_order_relaxed) &&
                            !frame_root_double_ab.load(std::memory_order_relaxed);
    float saved_offset_for_real = 0.0f;
    // The offset the REAL call is actually given, hoisted to function scope so
    // the view-write census below can report what the real call's writes used
    // instead of the resting value g_stereo_offset has been put back to by
    // then. Stays 0.0 outside full-frame mode, where the real call is not
    // offset at all.
    float real_call_offset = 0.0f;
    if (full_frame) {
        saved_offset_for_real = g_stereo_offset.load(std::memory_order_relaxed);
        // STEREO SIGN (2026-08-18, review P4). The real call's composite
        // becomes EYE 0, and eye 0 is the LEFT eye - so it must be rendered
        // from the LEFT-shifted camera, i.e. -half. It was +half, which put
        // the right-eye viewpoint in the left eye and the left in the right:
        // PSEUDOSCOPIC. Depth reads inside-out, and it is the kind of wrong
        // that a still screenshot cannot show and a headset makes immediately
        // unpleasant. Paired with the doubled call's sign below; the two must
        // always be opposite.
        // EYE SWAP (2026-08-28, owner's control experiment). Normally the real
        // call renders -half and is presented as eye 0 (LEFT), and the doubled
        // call renders +half into eye 1 (RIGHT), which is the eye missing its
        // additive layer. Create C:\dev\doomvr-swapeyes.txt to swap BOTH the
        // camera signs here and the eye each image is submitted to (see
        // openxr_context.cpp's true_stereo branch), so the REAL call produces
        // the right eye and the DOUBLED call produces the left.
        //
        // WHAT IT DISCRIMINATES, and nothing in nine days has tested it:
        //   effects now missing in the LEFT eye  -> the defect follows the
        //       DOUBLED CALL. Everything measured so far is about the right
        //       thing, and presentation is exonerated.
        //   effects still missing in the RIGHT eye -> the defect follows the
        //       EYE SLOT, i.e. it is in the blit/XR/presentation path and not
        //       in the second render at all - which would invalidate the
        //       framing of the entire investigation.
        // Both signs flip together; getting only one would render a
        // pseudoscopic pair and prove nothing.
        real_call_offset = (stereo_swap_eyes_enabled() ? 1.0f : -1.0f) *
                           effective_double_ipd() * 0.5f;
        // IPDHB pass 1. Recorded AFTER the sign and the halving, i.e. the
        // number this pass is about to render with, not the arm label.
        ipdhb_note(1, real_call_offset);
        g_stereo_offset.store(real_call_offset, std::memory_order_relaxed);
        world_vp_shifted_this_frame.store(false, std::memory_order_relaxed);
    }

    // CompositeTargetProbe: bracket the REAL frame-root call so the last
    // render pass it records can be identified.
    last_frame_root_framebuffer = VK_NULL_HANDLE;
    last_frame_root_cmd_buffer = VK_NULL_HANDLE;
    frame_root_render_pass_count = 0;
    doubled_render_pass_count = 0;
    doubled_last_framebuffer = VK_NULL_HANDLE;
    // SAME-FRAME TAG (G2). One increment per REAL frame-root entry, before
    // the real call, so the doubled call that follows below records THIS
    // frame's number. Unconditional and log-only - it does not depend on any
    // arming flag, so the counter never has a gap that would make the
    // present-side comparison ambiguous.
    g_frame_root_serial.fetch_add(1, std::memory_order_relaxed);
    // DRAW PARITY. Latched into a thread_local here, ONCE per frame, before
    // the real call: the two calls being compared must be the two halves of
    // the SAME frame, and an atomic re-read part-way through could otherwise
    // start the table mid-frame with a real call already gone by. Cleared
    // after the emit below.
    real_pass_seq = 0;
    real_current_framebuffer = VK_NULL_HANDLE;
    real_current_job_rva = 0;
    // X1: the recording is per FRAME, not cumulative. A slot table carried over
    // from an earlier frame would replay offsets into a ring region DOOM has
    // since rewritten - the stale-handle class of error this file refuses
    // everywhere else. Cleared before the real call fills it again.
    x1_real_dyn.clear();
    x1_real_bind_seq = 0;
    x1_doubled_bind_seq = 0;
    // FROXEL FIX: cleared every frame. A frame where the real call binds no
    // volume must substitute nothing, not last frame's handle - which may by
    // then have been destroyed.
    real_lighting_volume_view = VK_NULL_HANDLE;
    // PER-FRAME by construction. The bind-time volume redirect may only ever
    // write a volume the REAL call bound in THIS frame - DOOM alternates two
    // per binding for temporal reprojection (VolumeRecord measured the swap),
    // and replaying the other one is how a repair becomes a corruption.
    for (auto& v : real_volume_by_binding) v = VK_NULL_HANDLE;
    for (auto& r : volume_job_records) r = VolumeJobRecord{};
    for (auto& r : real_volume_job_rva) r = 0;
    volume_redirects_this_frame.store(0, std::memory_order_relaxed);
    if (draw_parity_armed.load(std::memory_order_relaxed) &&
        !draw_parity_done.load(std::memory_order_relaxed)) {
        draw_parity_active = true;
        std::memset(draw_parity_real, 0, sizeof(draw_parity_real));
        std::memset(draw_parity_doubled, 0, sizeof(draw_parity_doubled));
    }
    // P1 READ CLASSIFICATION - latched exactly like DrawParity above, and for
    // the same reason: the two sides being compared must be the two halves of
    // ONE frame, and the real call has already started by the time anything
    // downstream could re-read the atomic. The snapshot is built here too, on
    // the render thread, before any pass runs.
    //
    // GATED ON A CONVERGED, REDIRECTING FRAME. The BOUND row only exists if
    // the substitution branch runs, and that branch requires
    // read_redirect_enabled && mirrors_converged(). 172 MirrorConvergence LOST
    // events were recorded in one session, each pausing redirection for eight
    // doubled calls; landing the armed frame inside one would produce an empty
    // BOUND side that reads exactly like "redirection resolved nothing" - the
    // strongest possible wrong answer. p1_armed stays set, so this simply
    // waits for the next converged frame instead of spending the arm.
    if (p1_armed.load(std::memory_order_relaxed) &&
        !p1_done.load(std::memory_order_relaxed) &&
        read_redirect_enabled.load(std::memory_order_relaxed) &&
        mirrors_converged()) {
        p1_active = true;
        std::memset(p1_real_asked, 0, sizeof(p1_real_asked));
        std::memset(p1_doubled_asked, 0, sizeof(p1_doubled_asked));
        std::memset(p1_doubled_bound, 0, sizeof(p1_doubled_bound));
        p1_build_snapshot();
    }
    // LAYOUT REWIND, half 1 - see snapshot_frame_start_layouts.
    if (mirror_enabled.load(std::memory_order_relaxed) &&
        frame_root_double_enabled.load(std::memory_order_relaxed) &&
        !skip_mirror_layout_ops.load(std::memory_order_relaxed)) {
        snapshot_frame_start_layouts();
    }
    kharvox::native::trace::setEye(0);
    inside_real_frame_root = true;
    { kharvox::native::cpu::Scope profileOriginal(kharvox::native::cpu::Original); FbTramp fbt_(FB_D_FRAME_ROOT); real_frame_root(arg1, arg2); }
    inside_real_frame_root = false;
    kharvox::native::trace::setEye(2);
    // During loading, FrameRoot may return with a closed command buffer or an
    // open render pass. Prove the injection boundary BEFORE holding-image blits,
    // mirror barriers or storage copies. No native eye pair is published for
    // such a frame; a later complete world frame can warm the mirrors safely.
    if (!kharvox::native::prepareDoubleBoundary(last_frame_root_cmd_buffer)) {
        if (full_frame) g_stereo_offset.store(saved_offset_for_real, std::memory_order_relaxed);
        return;
    }
    // POST-FRAME-ROOT COVERAGE PROBE (2026-08-17, run 22).
    //
    // Run 22 settled where eye 1's remaining quality gap comes from, on
    // three independent signals: DOOM's presented image carries a HUD the
    // pass-78 composite does not, its grade is materially different, and
    // CompositeTargetProbe reports isSwapchainImage=FALSE - the frame root's
    // last render pass writes an INTERNAL target, not the image that reaches
    // the monitor. So DOOM does more work after the frame root returns, and
    // that work is outside the doubled call by construction.
    //
    // This marks the window between the real frame root returning and
    // vkQueuePresentKHR so the passes and transfers in it can be identified.
    // Whatever writes the swapchain image in here is the missing coverage.
    //
    // ⚠ The window is opened at the END of this function, NOT here. Our own
    // doubled call runs below, in this same function, and its ~78 render
    // passes would otherwise be logged as "work DOOM does after the frame
    // root" - filling the 80-line budget with our own injection before a
    // single real post-frame-root pass was reached, and producing a list
    // that looked like data. See the store at the bottom of hook_frame_root.

    // FULL-FRAME: copy the real call's fully composited output into the
    // holding image. Recorded inline into DOOM's own command buffer - a GPU
    // blit, no submit, no queue wait.
    //
    // Under SYMMETRIC STEREO this composite is EYE 0 (+half), paired against
    // the doubled call's mirror as eye 1 (-half). The original full-frame
    // mode called it eye 1; the eye it feeds is decided at present time, not
    // here, so only the offset sign below is load-bearing.
    if (full_frame) {
        // SHIFT+END. This blit is recorded into DOOM's OWN command buffer and
        // carries layout transitions on DOOM's own images, every frame,
        // regardless of what the doubled call does - which makes it one of the
        // few remaining things that survives every floor rung. Judged flat, the
        // holding image is never read back, so skipping it costs nothing.
        if (kharvox::native::mirrorPreparationEnabled()&&!skip_composite_capture.load(std::memory_order_relaxed))
            capture_composite_to_holding();
        // The doubled run ACCUMULATES onto the first run's output rather than
        // replacing it - confirmed live: sparks visible twice in one eye,
        // mid-range lighting roughly doubled in brightness. Additive passes
        // (particles, bloom, transparency) composite on top of whatever is
        // already in the target.
        //
        // Now that the holding image safely carries eye 1, the composite can
        // be wiped before the second run so it starts from black. If the
        // doubling disappears, accumulation in this image was the whole
        // problem; if it persists, the accumulation lives in some other
        // buffer (a particle atlas or a history/TAA target) and this needs
        // widening rather than guessing again.
        if (!kharvox::native::leftEyeOnlyDiagnostic()&&frame_root_clear_between.load(std::memory_order_relaxed)) clear_composite_before_double();
        g_stereo_offset.store(saved_offset_for_real, std::memory_order_relaxed);
    }

    if (frame_root_double_enabled.load(std::memory_order_relaxed) &&
        composite_probe_lines.fetch_add(1, std::memory_order_relaxed)
            < kMaxCompositeProbeLines) {
        VkImageView view = VK_NULL_HANDLE;
        VkImage image = VK_NULL_HANDLE;
        {
            std::scoped_lock lock(framebuffer_depth_view_mutex);
            auto fb = framebuffer_color0_views.find(last_frame_root_framebuffer);
            if (fb != framebuffer_color0_views.end()) {
                view = fb->second;
                auto im = image_view_to_image.find(view);
                if (im != image_view_to_image.end()) image = im->second;
            }
        }
        // Is that image one of the swapchain images? If so, the composite
        // lands straight in the swapchain and eye 0 can be copied from a
        // handle we already track. If not, it is an internal target and the
        // copy has to source from there instead.
        bool is_swapchain = false;
        {
            std::unique_lock lock(swapchains_mutex);
            for (const auto& [sc, rec] : swapchains) {
                (void)sc;
                for (VkImage img : rec.images) if (img == image) { is_swapchain = true; break; }
                if (is_swapchain) break;
            }
        }
        log::warn(std::format(
            "CompositeTargetProbe renderPassesInFrameRoot={} lastFramebuffer={} color0View={} "
            "image={} isSwapchainImage={} cmdBuffer={}",
            frame_root_render_pass_count,
            reinterpret_cast<void*>(last_frame_root_framebuffer),
            reinterpret_cast<void*>(view), reinterpret_cast<void*>(image),
            is_swapchain, reinterpret_cast<void*>(last_frame_root_cmd_buffer)));
    }

    // BLIT-ONLY CONTROL. Runs the full-frame path with the doubled call
    // SKIPPED: eye 1 still comes from the holding image, eye 0 still from the
    // swapchain, but only ONE render happened. Both eyes should then show the
    // same clean, complete frame with no doubling, no double-brightness and
    // the HUD present in both.
    //
    // This separates the two candidate faults in the live result:
    //   - if blit-only is CLEAN, the blit and the presentation wiring are
    //     correct and the contamination comes purely from running the frame
    //     root twice into DOOM's own targets (additive passes accumulating).
    //   - if blit-only is ALREADY wrong, the fault is in the blit timing or
    //     the eye wiring and the doubling is not to blame.
    if (frame_root_blit_only.load(std::memory_order_relaxed)) return;

    if(kharvox::native::leftEyeOnlyDiagnostic()) {
        // The normal first-eye recording, immutable input completion and
        // holding copy above remain. Stop before incrementing/entering the
        // second root, after exercising its common resource preparation.
        if(kharvox::native::mirrorPreparationEnabled()) {
            kharvox::native::prepareLeftDiagnosticTargets();
            if(mirror_enabled.load(std::memory_order_relaxed)&&
               !skip_mirror_layout_ops.load(std::memory_order_relaxed))
                rewind_mirror_layouts(last_frame_root_cmd_buffer);
            kharvox::native::seedBuffers(last_frame_root_cmd_buffer);
            kharvox::native::seedFrameImages(last_frame_root_cmd_buffer);
        }
        kharvox::native::finishLeftDiagnosticPreparation();
        return;
    }

    // FrameRootDouble Stage 1 - see the declarations above. Runs strictly
    // AFTER the real call has completed, never around or instead of it, so
    // the frame DOOM actually presents is produced by the untouched original
    // invocation. Identical arguments, no camera change.
    if (!inside_frame_root_double &&
        frame_root_double_enabled.load(std::memory_order_relaxed)) {
        const uint32_t attempt = frame_root_double_attempts.load(std::memory_order_relaxed);
        if (attempt >= frame_root_double_cap.load(std::memory_order_relaxed)) {
            frame_root_double_enabled.store(false, std::memory_order_relaxed);
            log::warn("FrameRootDouble: attempt cap reached; disarmed automatically");
        } else {
            frame_root_double_attempts.fetch_add(1, std::memory_order_relaxed);
            // LAYOUT REWIND, half 2 - see rewind_mirror_layouts.
            if (mirror_enabled.load(std::memory_order_relaxed) &&
                !skip_mirror_layout_ops.load(std::memory_order_relaxed) &&
                last_frame_root_cmd_buffer != VK_NULL_HANDLE) {
                rewind_mirror_layouts(last_frame_root_cmd_buffer);
            }
            kharvox::native::seedBuffers(last_frame_root_cmd_buffer);
            kharvox::native::seedFrameImages(last_frame_root_cmd_buffer);
            inside_frame_root_double = true;
            fb_doubled_start = kharvox::native::cpu::current.detailed ? fb_ns() : 0;
            if (fb_doubled_start) fb_clock_reads.fetch_add(2, std::memory_order_relaxed);
            frame_root_stage.store(1, std::memory_order_relaxed);
            // DescriptorCensus: fresh pass context per doubled call, so
            // passSeq in the log restarts at 1 each frame and reads as
            // "the Nth render pass of this doubled call".
            doubled_pass_seq = 0;
            doubled_current_framebuffer = VK_NULL_HANDLE;
            doubled_current_fb_mirrored = false;
            doubled_current_job_rva = 0;
            if (descriptor_census_enabled.load(std::memory_order_relaxed)) {
                census_pass_totals.fill(CensusPassTotals{});
                census_pass_line_count.fill(0);
            }
            // ChainWalk (STEP 6.6). Observation and capture are mutually
            // exclusive by construction: observation runs on live doubled
            // calls before the A/B is armed and produces the pass map that
            // capture's slots are sized from.
            chainwalk_observing_this_call = chainwalk_observe.load(std::memory_order_relaxed);
            if (chainwalk_observing_this_call) {
                chainwalk_map.fill(ChainWalkPass{});
            }
            chainwalk_capture_attempt = UINT32_MAX;
            if (chainwalk_capture.load(std::memory_order_relaxed) &&
                chainwalk_ready.load(std::memory_order_acquire) &&
                frame_root_double_ab.load(std::memory_order_relaxed) && attempt <= 1) {
                chainwalk_capture_attempt = attempt;
            }
            // PASS TIMELINE (2026-08-19). Rides the same recorded-copy path,
            // keyed by PASS NUMBER instead of job RVA, and needs neither the
            // A/B attempt pair nor a prior observation pass - so one F12 press
            // arms it directly. See pass_timeline_armed.
            if (pass_timeline_armed.load(std::memory_order_relaxed))
                chainwalk_capture_attempt = 0;
            if (chainwalk_camera_armed.load(std::memory_order_relaxed)) {
                chainwalk_cams.fill(ChainWalkCam{});
            }
            chainwalk_current_color = VK_NULL_HANDLE;
            // Stage 2a: redirect the second call's RenderOpaque render passes
            // into our own offscreen framebuffers instead of DOOM's live one.
            // hook_cmd_begin_render_pass keys purely off this thread-local, and
            // the offscreen targets are the ones already proven to capture a
            // complete, correct DOOM frame (canyon, walls, floor, weapon, sky)
            // when combined with DOOM's real depth buffer.
            //
            // FULL-FRAME mode turns this OFF: there the doubled call must
            // render into DOOM's own targets so its fully composited output
            // reaches the swapchain and becomes eye 0. The redirect only
            // applies to the legacy G-buffer A/B path.
            // With generic mirroring on, mirroring is the ONLY redirect: every
            // framebuffer is swapped in hook_cmd_begin_render_pass. Leaving
            // the legacy offscreen redirect on as well put RenderOpaque into
            // offscreen_targets while the now-unfiltered remaining 15 jobs
            // wrote DOOM's real targets, and that combination faulted on the
            // first attempt (SEH caught it and auto-disarmed).
            inside_injected_extra_call =
                !full_frame && !mirror_enabled.load(std::memory_order_relaxed);
            // Stage 3: camera divergence, applied the only way this project
            // has ever got a camera change to actually reach rendered output.
            //
            // The frame root CALLS SetupView. Our SetupView hooks (Lever V/W)
            // already apply g_stereo_offset to the camera before the real
            // SetupView runs, and that lever is the proven "PERFECT!!!!"
            // whole-scene mechanism from 2026-08-11. So simply setting the
            // offset around the doubled call makes the doubled call's OWN
            // camera sample bake the shift - upstream of the gather-time
            // render-view bake that defeated every previous attempt. No
            // clone, no GPU uniform write, nothing fighting the bake.
            //
            // Safe to touch a global here: we are inline on one thread and
            // the real call has already fully returned.
            // Stage 3 A/B. One image proves nothing on its own - the whole
            // point is whether a camera shift CHANGES the rendered viewpoint,
            // and that needs two dumps from the same standing position. If
            // they come back identical, that is the Avenue-1 signature
            // reappearing one level up (complete-but-identical eyes), which
            // this project's plan names in advance as a stop-and-report.
            //
            // Attempt 0 renders with offset 0.0, attempt 1 with the full
            // offset, on adjacent frames, each dumped to its own file.
            const bool ab_mode = frame_root_double_ab.load(std::memory_order_relaxed);
            const bool ab_zero_pass = ab_mode && (attempt == 0);
            // FULL-FRAME: the doubled call's mirror becomes EYE 1, the RIGHT
            // eye, so it renders from the RIGHT-shifted camera at +half -
            // opposite the real call's -half above. Both signs flipped
            // together 2026-08-18 (review P4); they were inverted, which made
            // the pair pseudoscopic. In A/B mode the legacy semantics stand
            // (0 vs full offset) so the comparison dumps stay meaningful.
            const float this_offset =
                frame_root_zero_offset.load(std::memory_order_relaxed)
                    ? 0.0f
                : full_frame
                    // EYE SWAP - paired with the real call's sign above. See
                    // the comment there for what this experiment separates.
                    ? (stereo_swap_eyes_enabled() ? -1.0f : 1.0f) *
                          effective_double_ipd() * 0.5f
                : (ab_zero_pass ||
                   frame_root_double_null_control.load(std::memory_order_relaxed))
                    ? 0.0f
                    : effective_double_ipd();
            // IPDHB pass 2. Recorded AFTER every one of the four branches
            // that can force this to zero independently of the override, so
            // "both passes at 0" in the log cannot be a guess about which
            // branch won.
            ipdhb_note(2, this_offset);
            const float prev_offset = g_stereo_offset.load(std::memory_order_relaxed);
            g_stereo_offset.store(this_offset, std::memory_order_relaxed);
            // The per-frame guard has already been consumed by the real
            // call's own SetupView; clear it so the doubled call's SetupView
            // is allowed to apply the shift too.
            world_vp_shifted_this_frame.store(false, std::memory_order_relaxed);

            // The arm line reports frame_root_double_ipd, which is NOT the
            // offset actually used - zero-offset mode is applied here, at
            // this_offset, and was invisible in the log. A Ctrl+Pause run
            // therefore could not be told apart from a plain Pause run after
            // the fact. Print what the doubled call is really doing, once.
            if (attempt == 0) {
                log::warn(std::format(
                    "FrameRootDouble CONFIG attempt=0: effectiveOffset={:.2f} uu zeroOffset={} "
                    "ab={} nullControl={} fullFrame={} mirror={} unfiltered={} noJobs={}",
                    this_offset,
                    frame_root_zero_offset.load(std::memory_order_relaxed),
                    ab_mode,
                    frame_root_double_null_control.load(std::memory_order_relaxed),
                    full_frame,
                    mirror_enabled.load(std::memory_order_relaxed),
                    mirror_unfiltered.load(std::memory_order_relaxed),
                    doubled_dispatch_no_jobs.load(std::memory_order_relaxed)));
            }
            frame_root_stage.store(2, std::memory_order_relaxed);
            // FORCED SERIALISATION (2026-08-22). The next RenderDoc behaviour.
            //
            // Usage widening is now cleanly eliminated - ON with zero driver
            // refusals, complete coverage, clean baseline, judged on the haze
            // rather than on px>200. Test A eliminated "a layer is present"
            // (Khronos validation demonstrably live, 1470 messages, deficit
            // unmoved) and Test B eliminated "the game is slow". What is left
            // of what RenderDoc does is that it SERIALISES: it stops GPU work
            // overlapping, so everything the real call submitted has actually
            // COMPLETED before the doubled call reads it.
            //
            // That is the one candidate that can differ while every descriptor
            // we compare stays identical - the handles are the same, the
            // buffers are the same, and what is IN them at the moment of the
            // read is not.
            //
            // A full barrier rather than vkQueueWaitIdle: this is recorded into
            // DOOM's own command buffer, so it costs a pipeline drain at the
            // right point in the stream instead of a CPU stall, and it cannot
            // deadlock the way an idle-wait inside a render thread can.
            if (force_doubled_serialisation.load(std::memory_order_relaxed) &&
                last_frame_root_cmd_buffer != VK_NULL_HANDLE) {
                // real_cmd_pipeline_barrier, not a loaded proc: load_device_proc
                // is defined further down this file, and going through the REAL
                // pointer also avoids re-entering our own barrier hook, whose
                // mirror-substitution logic has no business rewriting a barrier
                // that exists purely to drain the pipeline.
                if (real_cmd_pipeline_barrier) {
                    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
                    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                    real_cmd_pipeline_barrier(last_frame_root_cmd_buffer,
                                              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                              0, 1, &mb, 0, nullptr, 0, nullptr);
                    const uint32_t n =
                        doubled_serialisations.fetch_add(1, std::memory_order_relaxed);
                    if (n < 3 || (n % 600) == 0)
                        log::warn(std::format(
                            "ForcedSerialisation #{}: full ALL_COMMANDS memory barrier recorded "
                            "before the doubled call, so everything the real call submitted is "
                            "complete and visible before eye 1 reads it. This is the RenderDoc "
                            "behaviour that remains after usage widening, the validation layer "
                            "and slowness were each eliminated.",
                            n));
                }
            }
            bool ok;
            kharvox::native::trace::setEye(1);
            { kharvox::native::cpu::Scope profileDoubled(kharvox::native::cpu::Doubled); ok = safe_call_frame_root(arg1, arg2, real_frame_root); }
            kharvox::native::trace::setEye(2);
            frame_root_stage.store(3, std::memory_order_relaxed);

            // Put DOOM's GUI/viewmodel matrix back exactly as it was before
            // the doubled call touched it. Without this, eye 0's weapon draw
            // inherits eye 1's transform.
            if (lever_v_restore_valid) {
                const bool restored = safe_write_bytes(
                    lever_v_restore_addr, lever_v_restore_matrix,
                    sizeof(lever_v_restore_matrix));
                vwc_leverv_restores.fetch_add(1, std::memory_order_relaxed);
                const uint32_t n = lever_v_restore_count.fetch_add(1, std::memory_order_relaxed);
                if (n < 3 || !restored) {
                    log::warn(std::format(
                        "LeverV restore #{}: GUI view matrix at {:#x} put back, ok={}",
                        n, lever_v_restore_addr, restored));
                }
                lever_v_restore_valid = false;
            }
            // The six dest copies, restored for the same reason and at the same
            // point. Unconditional: it is keyed on dest_clean_valid internally,
            // and a missed restore here is the one failure mode that compounds
            // frame over frame.
            restore_world_vp_destinations();
            // Lever O, restored at the same point and unconditionally, for the
            // same reason: it is keyed on origin_clean_valid internally, and a
            // missed restore compounds frame over frame.
            restore_origin_shift();
            // Hand back DOOM's motion-vector history. Without this the next
            // real frame differences eye 0's camera against eye 1's and
            // temporal reprojection smears the whole left eye.
            if (render_view_restore_valid) {
                const bool ok_cur = safe_write_bytes(
                    render_view_restore_addr + 0xC44, render_view_restore_current,
                    sizeof(render_view_restore_current));
                const bool ok_prev = safe_write_bytes(
                    render_view_restore_addr + 0xC84, render_view_restore_previous,
                    sizeof(render_view_restore_previous));
                vwc_renderview_restores.fetch_add(1, std::memory_order_relaxed);
                const uint32_t n = render_view_restore_count.fetch_add(1, std::memory_order_relaxed);
                if (n < 3 || !ok_cur || !ok_prev) {
                    log::warn(std::format(
                        "RenderView restore #{}: +0xC44 current ok={} +0xC84 previous ok={} "
                        "at {:#x}",
                        n, ok_cur, ok_prev, render_view_restore_addr));
                }
                render_view_restore_valid = false;
            }

            // VIEW-WRITE CENSUS. Emitted here, with both restores already
            // counted and the doubled call finished, so one block is one whole
            // frame carrying both sides. real_call_offset, NOT prev_offset:
            // prev_offset is read after the full-frame block has already put
            // g_stereo_offset back to its resting value, so it printed 0.000
            // for frames whose own rows recorded off=-6.000.
            vwc_emit_frame(real_call_offset, this_offset, full_frame,
                frame_root_double_ipd.load(std::memory_order_relaxed));

            g_stereo_offset.store(prev_offset, std::memory_order_relaxed);
            inside_injected_extra_call = false;
            // Closed BEFORE the flag clears, so the whole injected render is
            // inside the span and nothing after it leaks in.
            if (fb_doubled_start) {
                const uint64_t dbl_ns = fb_ns() - fb_doubled_start;
                fb_doubled_ns.fetch_add(dbl_ns, std::memory_order_relaxed);
                // ⚠ DOUBLE-COUNT FIX. This injected render happens INSIDE
                // hook_frame_root but OUTSIDE real_frame_root, so it would
                // otherwise appear both as `doubled` and inside frame_root's
                // overhead (span - trampoline). Charging it to frame_root's
                // trampoline bucket subtracts it from that overhead, leaving
                // `doubled` as its single home in the budget.
                // The REAL bucket, deliberately: hook_frame_root was entered
                // with inside_frame_root_double still false, so its span was
                // recorded there.
                fb_doom_tramp_ns[FB_D_FRAME_ROOT].fetch_add(dbl_ns, std::memory_order_relaxed);
                fb_doubled_start = 0;
            }
            inside_frame_root_double = false;

            // DRAW PARITY TABLE. Emitted here, after the doubled call has
            // returned and its flag is cleared, so nothing logged below can
            // be mistaken for work inside either call. Both halves of ONE
            // frame, side by side.
            if (draw_parity_active) {
                draw_parity_active = false;
                draw_parity_done.store(true, std::memory_order_relaxed);
                draw_parity_armed.store(false, std::memory_order_relaxed);
                emit_draw_parity_table(attempt);
            }
            // P1 READ TABLE. Same window, same reasoning, and it self-disarms
            // here so one press cannot leave it recording for the session.
            if (p1_active) {
                p1_active = false;
                p1_done.store(true, std::memory_order_relaxed);
                p1_armed.store(false, std::memory_order_relaxed);
                p1_emit_table(attempt);
            }
            // DeferredInputMap rides the same one-frame window and self-disarms
            // here, so a press cannot leave it logging forever. The COMPLETE
            // sentinel prints both sides' line counts unconditionally - a side
            // that logged NOTHING is the most important thing this probe can
            // report and is exactly what a missing line would hide.
            if (deferred_input_map_armed.exchange(false, std::memory_order_relaxed))
                deferred_input_budget.emit_complete("DeferredInputMap");
            // IndirectArgs disarms HERE (recording is finished for both calls)
            // but is EMITTED at present. The copies are only RECORDED at this
            // point - the GPU has not run them yet, so reading the mapped
            // buffer now would report the previous frame's contents, or zeros.
            // Disarm-here/read-there is the whole reason this is two flags.
            if (indirect_args_probe_armed.exchange(false, std::memory_order_relaxed)) {
                indirect_args_ready.store(true, std::memory_order_release);
                // Reported on the capture frame so the answer arrives with the
                // dumps rather than needing its own run. begins=0 means the
                // doubled call issues no queries and this hypothesis is dead;
                // begins>0 means it was replaying writes onto unreset query
                // slots every frame, which is undefined behaviour and the best
                // remaining explanation for draws that vanish with every input
                // identical.
                // DEPTH CONTENT is read AT PRESENT, not here - see
                // emit_depth_content_verdict(). This block runs on the render
                // thread the moment the doubled call returns, when the strip
                // copies have been RECORDED and not yet executed, so reading
                // the buffer here returns the memset zeros. That is precisely
                // what made the last two runs void. The same split already
                // exists for IndirectArgs; I documented it there and then read
                // DepthContent in the wrong place anyway.
                depth_verdict_pending.store(true, std::memory_order_release);
                // RELEVANCE TEST verdict - the thing that decides whether the
                // query hypothesis is worth another attempt at all.
                const uint64_t rg = p25_draws_gated[0].load(std::memory_order_relaxed);
                const uint64_t ru = p25_draws_ungated[0].load(std::memory_order_relaxed);
                const uint64_t dg = p25_draws_gated[1].load(std::memory_order_relaxed);
                const uint64_t du = p25_draws_ungated[1].load(std::memory_order_relaxed);
                log::warn(std::format(
                    "QueryRelevance [rec] pass-25 draws | REAL gated={} ungated={} | DOUBLED "
                    "gated={} ungated={} | {}",
                    rg, ru, dg, du,
                    (rg + dg) == 0
                        ? "ZERO GATED - the pass-25 effects draws are NOT inside any occlusion "
                          "query, so unreset query slots cannot be what discards them. The "
                          "hypothesis is IRRELEVANT and is dropped on evidence, not fatigue - no "
                          "door closed."
                        : "GATED DRAWS EXIST at pass 25 - the hypothesis is live and the reset "
                          "hazard is worth one more careful attempt."));
                const uint64_t redirected =
                    doubled_query_redirected.load(std::memory_order_relaxed);
                log::warn(std::format(
                    "DoubledQuery TOTAL begins={} ends={} redirected={} overflow={} | {}",
                    doubled_query_begins.load(std::memory_order_relaxed),
                    doubled_query_ends.load(std::memory_order_relaxed), redirected,
                    doubled_query_overflow.load(std::memory_order_relaxed),
                    redirected == 0
                        ? "REDIRECTED ZERO - the private pool never took a query, so the doubled "
                          "call is STILL writing DOOM's unreset slots and an unchanged headset "
                          "proves nothing this run."
                        : "The doubled call's queries now run on our own pool, reset at the start "
                          "of every doubled call, so none of them begin an unreset slot."));
            }
            // Same disarm-here/read-there split, and for the same reason: the
            // pass-boundary copies are recorded now and executed later.
            if (pass_timeline_armed.exchange(false, std::memory_order_relaxed))
                pass_timeline_ready.store(true, std::memory_order_release);
            timeline_beginend_armed.store(false, std::memory_order_relaxed);
            if (pass_state_parity_armed.exchange(false, std::memory_order_relaxed))
                pass_state_budget.emit_complete("PassState");

            // ReadRedirect (STEP 6) audit. Without this the run is judged
            // from the headset alone with no way to tell "redirection ran and
            // did not help" from "redirection never fired" - the ambiguity
            // that has cost this project whole runs before. First 5 doubled
            // calls, then every 500th.
            if (read_redirect_enabled.load(std::memory_order_relaxed) &&
                (attempt < 5 || attempt % 500 == 0)) {
                log::warn(std::format(
                    "ReadRedirect attempt={} substitutedBinds={} clones={} fallbacks={} "
                    "barrierImagesSubstituted={} barrierCallsRewritten={} "
                    "| noRecords={} templateSets={} templateMisses={} staleRecordsCleared={} "
                    "setRecordsRetained={} "
                    "(substitutedBinds=0 means the pilot job never bound a set carrying a "
                    "mirror-backed read; barrierImagesSubstituted=0 would mean DOOM issues no "
                    "image barriers on mirrored images, which would refute the hang diagnosis)\n"
                    "⚠ THE FOUR NEW FIELDS ARE THE CUTSCENE DIAGNOSIS, and they were unlogged "
                    "until 2026-08-24. If substitutedBinds FREEZES while attempt keeps climbing, "
                    "every set is being refused and eye 1 is reading DOOM's real images instead "
                    "of our mirrors - which IS stereo collapsing to mono. Then: noRecords "
                    "climbing says the refusal is missing content records; staleRecordsCleared "
                    "climbing at the same moment says vkAllocateDescriptorSets recycled the "
                    "handles and dropped those records; and templateMisses climbing says DOOM "
                    "re-populated them through an update template we could not decode, so they "
                    "never came back. Those three together name the whole failure.",
                    attempt, read_redirect_substitutions.load(std::memory_order_relaxed),
                    read_redirect_clones.load(std::memory_order_relaxed),
                    read_redirect_failures.load(std::memory_order_relaxed),
                    barrier_image_substitutions.load(std::memory_order_relaxed),
                    barrier_calls_rewritten.load(std::memory_order_relaxed),
                    rr_reason_no_records.load(std::memory_order_relaxed),
                    template_decode_sets.load(std::memory_order_relaxed),
                    template_decode_misses.load(std::memory_order_relaxed),
                    stale_set_records_cleared.load(std::memory_order_relaxed),
                    set_records_retained.load(std::memory_order_relaxed)));
                // ===== PER-WINDOW RATE DELTAS (2026-08-24, plan §2 iii) =====
                //
                // A TOTAL cannot show a path going quiet, and "did DOOM's
                // writes stop reaching us at the collapse?" is precisely a
                // question about a path going quiet. These are deltas since
                // this line's OWN last emission, so the window is whatever
                // gap the audit's 5-then-every-500 cadence produced - the
                // window length is published as attemptsInWindow so a delta
                // is never read without its denominator.
                //
                // Statics: this site runs only on the render thread at the end
                // of a doubled call, the same single thread the rest of this
                // block's bookkeeping assumes.
                static uint64_t w_attempt = 0, w_upd = 0, w_img = 0, w_buf = 0, w_cpy = 0,
                                w_alloc_c = 0, w_alloc_s = 0, w_stale_i = 0, w_stale_b = 0;
                const uint64_t c_attempt = attempt;
                const uint64_t c_upd = desc_update_calls.load(std::memory_order_relaxed);
                const uint64_t c_img = desc_update_img_records.load(std::memory_order_relaxed);
                const uint64_t c_buf = desc_update_buf_records.load(std::memory_order_relaxed);
                const uint64_t c_cpy = desc_copy_calls.load(std::memory_order_relaxed);
                const uint64_t c_alloc_c = desc_alloc_calls.load(std::memory_order_relaxed);
                const uint64_t c_alloc_s = desc_alloc_sets.load(std::memory_order_relaxed);
                const uint64_t c_stale_i = stale_img_records_cleared.load(std::memory_order_relaxed);
                const uint64_t c_stale_b = stale_buf_records_cleared.load(std::memory_order_relaxed);
                log::warn(std::format(
                    "ReadRedirectRates attemptsInWindow={} | updCalls={} updImgRec={} "
                    "updBufRec={} cpyCalls={} allocCalls={} allocSets={} staleImgCleared={} "
                    "staleBufCleared={} (ALL DELTAS since this line last printed) "
                    "| totals upd={} img={} buf={} cpy={} alloc={} sets={} staleImg={} "
                    "staleBuf={}\n"
                    "POLARITY, stated before the run: healthy = updCalls in the thousands per "
                    "window with updImgRec climbing. If updCalls goes to ~0 across the collapse "
                    "while allocCalls carries on, the loader-export route went quiet and §1(c) "
                    "of the plan is contradicted - stop and re-plan. If updCalls stays high "
                    "while the refused handles' rings show ALLOC with no *_WRITE after it, the "
                    "writes are missing both record branches and the WriteShape lines name the "
                    "field.",
                    c_attempt - w_attempt, c_upd - w_upd, c_img - w_img, c_buf - w_buf,
                    c_cpy - w_cpy, c_alloc_c - w_alloc_c, c_alloc_s - w_alloc_s,
                    c_stale_i - w_stale_i, c_stale_b - w_stale_b,
                    c_upd, c_img, c_buf, c_cpy, c_alloc_c, c_alloc_s, c_stale_i, c_stale_b));
                w_attempt = c_attempt; w_upd = c_upd; w_img = c_img; w_buf = c_buf;
                w_cpy = c_cpy; w_alloc_c = c_alloc_c; w_alloc_s = c_alloc_s;
                w_stale_i = c_stale_i; w_stale_b = c_stale_b;
                // Every cap's own state, on its own line, so a silent
                // truncation can never masquerade as a null.
                size_t ring_handles = 0;
                {
                    std::unique_lock lock(swapchains_mutex);
                    ring_handles = descriptor_set_rings.size();
                }
                log::warn(std::format(
                    "DescRingCaps ringHandles={}/{} ringSkipped={} eventsPerHandle={} "
                    "refusalRingLines={} refusalRingBudgetLeft={}/{} refusalRearms={} "
                    "attribUsed={}/{} shapeLines={} shapeBudgetLeft={}/{} shapeRearms={} "
                    "shapeHits={} recentRefusedSlots={} "
                    "(ringSkipped>0 means some handle has NO ring at all and a missing ring on "
                    "a refusal line is truncation; a budgetLeft equal to its cap alongside zero "
                    "lines is a broken re-arm, not a measurement)",
                    ring_handles, kDescRingMaxHandles,
                    desc_ring_skipped.load(std::memory_order_relaxed), kDescRingEvents,
                    refusal_ring_lines.load(std::memory_order_relaxed),
                    refusal_ring_budget.load(std::memory_order_relaxed), kRefusalRingRearmLines,
                    refusal_ring_rearms.load(std::memory_order_relaxed),
                    read_redirect_failures.load(std::memory_order_relaxed), kMaxAttributionLines,
                    shape_tap_lines.load(std::memory_order_relaxed),
                    shape_tap_budget.load(std::memory_order_relaxed), kShapeTapLines,
                    shape_tap_rearms.load(std::memory_order_relaxed),
                    shape_tap_hits.load(std::memory_order_relaxed), kRecentRefusedSets));
                // ===== CLONE POOL HEALTH - the fix's own positive control ====
                //
                // clonesLive is the number of pool slots actually in use. The
                // collapse in both prior runs was this number reaching 1024 and
                // staying there. queueNow is printed beside queuePeak because a
                // running maximum never comes back down - a single transient
                // would otherwise latch it and read as a steady state.
                const uint32_t retire_queue_now =
                    rr_retire_queue_depth.load(std::memory_order_relaxed);
                const uint64_t made = read_redirect_clones.load(std::memory_order_relaxed);
                const uint64_t freed = rr_clones_freed.load(std::memory_order_relaxed);
                if(kharvox::native::installed())kharvox::native::logDescriptorArenaStats();
                else log::warn(std::format(
                    "ClonePool clonesLive={}/1024 made={} retired={} freed={} queueNow={} "
                    "queuePeak={} allocFailures={} retireFrames={}\n"
                    "POLARITY, before the run: clonesLive must SETTLE well under 1024 and stop "
                    "growing; made-freed should track it. allocFailures MUST STAY 0 - it is the "
                    "falsifier, and any non-zero value means the pool still exhausted and the "
                    "leak diagnosis is wrong. queueNow in single digits is healthy; a queueNow "
                    "that grows without bound means the drain is not running.",
                    made > freed ? made - freed : 0, made,
                    rr_clones_retired.load(std::memory_order_relaxed), freed, retire_queue_now,
                    rr_retire_queue_peak.load(std::memory_order_relaxed),
                    rr_clone_alloc_failures.load(std::memory_order_relaxed),
                    kCloneRetireFrames));
            }
            // DescriptorCensus: this doubled call has now visited every pass
            // it is going to, so the table is complete. Emit it, then disarm
            // and restore the job filter - the owner never has to press
            // anything a second time and the game cannot be left unfiltered.
            // Emitted here, after inside_frame_root_double is cleared, so
            // nothing logged from inside the summary can re-enter the census.
            if (descriptor_census_enabled.exchange(false, std::memory_order_relaxed)) {
                emit_descriptor_census_summary();
                const bool restore =
                    descriptor_census_restore_unfiltered.load(std::memory_order_relaxed);
                mirror_unfiltered.store(restore, std::memory_order_relaxed);
                log::warn(std::format(
                    "DescriptorCensus: one complete doubled call censused; DISARMED and job "
                    "filter restored to unfiltered={}. Grep 'DescCensusPass' for the table "
                    "and 'DescCensus pass=' for the sampled per-bind detail.",
                    restore));
            }

            // ChainWalk (STEP 6.6) end-of-call work. Deliberately after
            // inside_frame_root_double is cleared, so nothing logged here can
            // re-enter the pass map or the camera scan.
            if (chainwalk_observing_this_call) {
                chainwalk_observing_this_call = false;
                chainwalk_observe.store(false, std::memory_order_relaxed);
                chainwalk_finish_observation(doubled_pass_seq);
            }
            if (chainwalk_camera_armed.load(std::memory_order_relaxed) &&
                frame_root_double_ab.load(std::memory_order_relaxed) && attempt <= 1) {
                chainwalk_emit_camera_classification(attempt, doubled_pass_seq, this_offset);
            }
            if (chainwalk_capture_attempt != UINT32_MAX && attempt >= 1) {
                // Both A/B attempts are recorded; stop touching DOOM's
                // command buffers. Self-disarming, like every other probe
                // here. The present-thread dump reads the slots, not this
                // flag, so attempt 1's PPMs are unaffected.
                chainwalk_capture.store(false, std::memory_order_relaxed);
                chainwalk_camera_armed.store(false, std::memory_order_relaxed);
                log::warn("ChainWalk: both A/B attempts captured; snapshot recording DISARMED");
            }
            chainwalk_capture_attempt = UINT32_MAX;

            // Convergence is evaluated once per doubled call, here, after
            // every mirror this call could create or first-write has been
            // accounted for.
            update_mirror_convergence();

            // Capture the diverged eye once, so the result is judged from the
            // actual picture rather than from a clean log. This project has
            // been fooled by a flawless log before (Round 3's pass-1 redirect
            // logged 10/10 perfect and was refuted by looking at the image).
            if (ab_mode && attempt <= 1) {
                frame_root_double_dump_label.store(ab_zero_pass ? 0 : 1,
                                                  std::memory_order_relaxed);
                offscreen_dump_done[1].store(false, std::memory_order_relaxed);
                offscreen_dump_pending[1].store(true, std::memory_order_relaxed);
                log::warn(std::format(
                    "FrameRootDouble A/B: attempt={} rendered with offset={:.1f} uu -> "
                    "dump {}", attempt, this_offset,
                    ab_zero_pass ? "frdouble_off0" : "frdouble_off20"));
            }
            if (ok) {
                // PING-PONG PARITY: compare where each run's LAST render pass
                // landed. Different framebuffers means DOOM's post-process
                // chain alternates targets, so the doubled run's final image
                // goes to the other buffer while DOOM's swapchain copy still
                // reads the first - which would explain eye 0 remaining green
                // even though the doubled call verifiably ran.
                if (attempt < 6) {
                    VkImage real_img = VK_NULL_HANDLE, dbl_img = VK_NULL_HANDLE;
                    {
                        std::scoped_lock lock(framebuffer_depth_view_mutex);
                        auto r = framebuffer_color0_views.find(last_frame_root_framebuffer);
                        if (r != framebuffer_color0_views.end()) {
                            auto i = image_view_to_image.find(r->second);
                            if (i != image_view_to_image.end()) real_img = i->second;
                        }
                        auto d = framebuffer_color0_views.find(doubled_last_framebuffer);
                        if (d != framebuffer_color0_views.end()) {
                            auto i = image_view_to_image.find(d->second);
                            if (i != image_view_to_image.end()) dbl_img = i->second;
                        }
                    }
                    log::warn(std::format(
                        "PingPongParity attempt={} realPasses={} realFb={} realImg={} | "
                        "doubledPasses={} doubledFb={} doubledImg={} | SAME={}",
                        attempt, frame_root_render_pass_count,
                        reinterpret_cast<void*>(last_frame_root_framebuffer),
                        reinterpret_cast<void*>(real_img),
                        doubled_render_pass_count,
                        reinterpret_cast<void*>(doubled_last_framebuffer),
                        reinterpret_cast<void*>(dbl_img),
                        real_img == dbl_img && real_img != VK_NULL_HANDLE));
                }
                // Report the framebuffer census once, after enough frames for
                // the set to stabilise. This is the number that decides
                // whether mirroring every pass is realistic.
                if (attempt == 30 && !doubled_fb_census_reported.exchange(true)) {
                    std::string detail;
                    int idx = 0;
                    for (VkFramebuffer fb : doubled_fb_census) {
                        VkImageView v = VK_NULL_HANDLE;
                        {
                            std::scoped_lock lock(framebuffer_depth_view_mutex);
                            auto it = framebuffer_color0_views.find(fb);
                            if (it != framebuffer_color0_views.end()) v = it->second;
                        }
                        detail += std::format(" [{}]{}{}", idx++,
                            reinterpret_cast<void*>(fb),
                            v ? "" : "(no colour0 record)");
                        if (idx >= 40) { detail += " ..."; break; }
                    }
                    log::warn(std::format(
                        "FramebufferCensus distinctFramebuffersPerDoubledRun={} "
                        "renderPassesPerRun={} ->{}",
                        doubled_fb_census.size(), doubled_render_pass_count, detail));
                }
                // BOUNDED 2026-08-22. This fired ONCE PER FRAME, unconditionally,
                // and log::write does a synchronous flush() under a global mutex
                // on the present thread. Measured during the headset run: ~86
                // log lines per second, of which 640 of 672 over an eight-second
                // window were this one line - 95% of all logging, ~80 forced
                // flushes a second into a 148 MB file, on the thread that
                // presents. That is a plausible source of the regular pauses the
                // owner reported in VR, and it costs nothing to remove.
                //
                // Same shape as the FinalStage DOUBLED bound a few thousand
                // lines below: keep the first few so a run still proves the
                // doubling happens at all, then one every 600 (~7.5 s at the
                // 80 fps this run measured) so it stays visible as a heartbeat.
                // The counter is reported so a gap in the sequence is legible
                // rather than looking like the doubling stopped.
                static std::atomic_uint32_t normal_returns{0};
                const uint32_t nr = normal_returns.fetch_add(1, std::memory_order_relaxed);
                if (nr < 5 || (nr % 600) == 0) {
                    log::warn(std::format(
                        "FrameRootDouble attempt={} returned normally (identical args, no camera "
                        "change) tid={} normalReturns={} (this line is throttled to the first 5 "
                        "then 1 in 600 - it used to print EVERY FRAME and was 95% of all log "
                        "traffic; the counter, not the line count, is the engagement measure)",
                        attempt, GetCurrentThreadId(), nr));
                }
            } else {
                // The stage breadcrumb and the switch dump were documented as
                // existing and did not: the old line printed `attempt` and
                // nothing else, so four faults were localised no further than
                // "somewhere in the doubled call". Stage 2 = faulted inside
                // the doubled frame root itself; stage 1 = faulted before it
                // was even entered.
                log::error(std::format(
                    "FrameRootDouble attempt={} threw a structured exception at stage={} "
                    "(1=flags set, 2=inside the doubled call, 3=returned); disarming. "
                    "switches: mirror={} unfiltered={} fullFrame={} zeroOffset={} ab={} "
                    "blitOnly={} injectedExtraCall={} eyeRender={} | mirrors={} misses={} "
                    "viewMisses={} renderPassesThisRun={}",
                    attempt, frame_root_stage.load(std::memory_order_relaxed),
                    mirror_enabled.load(std::memory_order_relaxed),
                    mirror_unfiltered.load(std::memory_order_relaxed),
                    frame_root_full_frame.load(std::memory_order_relaxed),
                    frame_root_zero_offset.load(std::memory_order_relaxed),
                    frame_root_double_ab.load(std::memory_order_relaxed),
                    frame_root_blit_only.load(std::memory_order_relaxed),
                    inside_injected_extra_call, current_eye_render,
                    mirror_created_count.load(std::memory_order_relaxed),
                    mirror_miss_count.load(std::memory_order_relaxed),
                    mirror_view_miss_count.load(std::memory_order_relaxed),
                    doubled_render_pass_count));
                frame_root_double_enabled.store(false, std::memory_order_relaxed);
            }
        }
    }
    // POST-FRAME-ROOT WINDOW OPENS HERE - after the real call AND after our
    // own doubled call, so it contains only work DOOM itself does once the
    // frame root is finished. Closed in hook_queue_present.
    post_frame_root_window = true;
}

// Stage 2b job filter. Stage 2a redirected the doubled call's RenderOpaque
// passes offscreen and the white flash SURVIVED - because the frame root
// dispatches 18 jobs, not one, and only RenderOpaque's two G-buffer passes
// are covered by the offscreen machinery. RenderPass x6, RenderPostProcess,
// RenderDeferredPasses and RenderParticleLightAtlas all ran a second time
// against their real targets. Doubling post-process and lighting composition
// is exactly a partial white blowout.
//
// A second EYE does not need any of that. It needs the world G-buffer:
// depth, then opaque, into our own target. Everything else in the second
// pass is duplicated work whose only effect is corrupting the visible frame.
//
// So during the doubled call, allow only the jobs the eye actually needs and
// skip the rest by simply not forwarding them. Skipping is safe here
// specifically because we are inline (r_useSMP=1): there is no queue entry,
// no group pending-count and no barrier bookkeeping attached to a dispatch
// we never make. Under the threaded regime this would desynchronise the job
// graph and must not be attempted.
//
// Job identity comes from the module-relative RVA of job_fn, which is exactly
// what the FrameRootProbe capture already decoded for all 18 sites.
constexpr uintptr_t kRenderDepthFnRva = 0x1A36C10;
constexpr uintptr_t kRenderOpaqueFnRva = 0x1A37CC0;
constexpr uintptr_t kResolveDepthFnRva = 0x17B3D30;
// STEP 6 PILOT (2026-08-17). The deferred-lighting block, identified from
// the census rather than guessed: its passes (25-46 of the doubled call)
// carry ~152 of the 240 mirror-backed sampled reads, pass 41 alone holding
// 104. Allowing this ONE job through the filter is what gives eye 1 its
// lighting; redirecting its G-buffer reads to our mirrors is what should
// let it keep the parallax. See the ReadRedirect block near
// hook_cmd_bind_descriptor_sets.
// ⚠ 2026-08-26: 0x1A39930 is registered as **"RenderPass"** - a generic pass
// runner, not deferred lighting. The comment block above is preserved for
// history but its "this ONE job gives eye 1 its lighting" claim was never
// checked against the binary. The job map now names the real one; see
// kP1DeferredPassesJobRva (0x1873830, "RenderDeferredPasses"). This constant
// has no code use - only the comment at the census RVA attribution mentions
// it - so it is left defined, renamed to what it actually is.
constexpr uintptr_t kRenderPassGenericFnRva = 0x1A39930;
// read_redirect_enabled and its counters live with the DescriptorCensus
// globals further up - hook_frame_root's audit line needs them and is
// defined before this point.

// EYE 1 SOURCE SELECTION (2026-08-17, after eye 1 came back HUD-only).
//
// doubled_last_mirror_color used to mean "the last colour-carrying mirror
// the doubled call touched, whatever it was". That was safe only while the
// doubled call ended at the G-buffer. Allowing the deferred-lighting job
// through extended the chain into DOOM's post/UI targets and the last
// colour mirror became an 8-bit LDR UI target - read from the log, not
// inferred: the presented image went from format=122 (B10G11R11, the HDR
// world colour) to format=37 (R8G8B8A8_UNORM) at the same 3818x2032, in a
// perfectly correct layout. The GUI passes run INLINE and are not filtered,
// so the doubled call drew the HUD into that mirror and nothing else. Hence
// "only the HUD in the right eye".
//
// Eye 1 must be the last colour target the WORLD jobs wrote - the ones we
// deliberately allow through the filter - not whatever pass came last.
// Inline work (jobRva 0: scene build, GUI) and any job we do not run are
// excluded by construction. Note this also restores the pre-pilot behaviour
// exactly when only depth/opaque run, since those RVAs are in this set.
// UNFILTERED changes what "eye 1" means. With the filter on, the doubled
// call renders only the world G-buffer and eye 1 is that. With it off the
// doubled call renders a COMPLETE frame - lighting, post, and its own HUD -
// into DOOM's real composite target's mirror, and eye 1 is that final
// composite, whichever pass wrote it last. So the job restriction applies
// only while filtered; unfiltered deliberately accepts any pass.
bool eye1_source_candidate(uintptr_t rva) {
    if (mirror_unfiltered.load(std::memory_order_relaxed)) return true;
    return rva == kRenderDepthFnRva || rva == kRenderOpaqueFnRva ||
           rva == kResolveDepthFnRva;
}

// is_world_job_rva is defined with the job RVA constants further down.

void __fastcall hook_job_dispatch(void* job_ctx, void* job_fn, void* job_arg, uint8_t wait_flag) {
    FbDoom fbd_(FB_D_JOB_DISPATCH);
    // BISECTION (2026-08-16). Eye 0's real symptom, once the eyes were
    // assigned correctly, is loss of TEXTURE DETAIL - solid outlines, missing
    // mid-range textures - with lighting and effects intact. Not a blowout and
    // not darkness. Zero camera offset did not help, so the damage is done by
    // the act of doubling rather than by camera divergence.
    //
    // That splits cleanly in two, and this switch separates them in one run.
    // With no jobs dispatched at all, the doubled call still runs the frame
    // root's own scaffolding - SetupView, gather, cull, sort - but renders
    // nothing whatsoever.
    //
    //   eye 0 textures RETURN  => damage is inside RenderDepth / ResolveDepth /
    //                             RenderOpaque; bisect those three next.
    //   eye 0 STILL broken     => damage is in the frame root's own gather/cull/
    //                             streaming-request path. No amount of render-
    //                             target isolation can fix that, and STEP 2
    //                             (private per-eye depth) would be a wasted run.
    //
    // Strictly less work than the current configuration, so it cannot be less
    // stable than the run that just completed 1647 attempts cleanly.
    if (inside_frame_root_double &&
        doubled_dispatch_no_jobs.load(std::memory_order_relaxed)) {
        const uint32_t n = doubled_jobs_skipped.fetch_add(1, std::memory_order_relaxed);
        if (n < 3 || n == 20) {
            log::warn(std::format(
                "DoubledJobBisect: skipping ALL jobs during the doubled call "
                "(skip #{}). Eye 1 will render NOTHING this run - that is the "
                "point of the test, judge eye 0 only.", n));
        }
        return;
    }
    // FULL-FRAME mode runs the doubled call UNFILTERED - it must produce a
    // complete composited frame, lighting and post included, because that
    // output IS eye 0. The filter below is the legacy G-buffer-only path,
    // kept for the A/B diagnostic where the doubled call's output is only
    // ever compared against itself.
    // Dropping this filter is INDEPENDENT of mirroring - see the switch
    // matrix at mirror_enabled's declaration. Mirroring alone changes one
    // thing from the known-good configuration; unfiltering as well changes
    // two, which faulted twice. The filter therefore stays on until
    // mirror_unfiltered is deliberately set.
    if (inside_frame_root_double &&
        !frame_root_full_frame.load(std::memory_order_relaxed) &&
        !mirror_unfiltered.load(std::memory_order_relaxed)) {
        const uintptr_t doom_base =
            reinterpret_cast<uintptr_t>(GetModuleHandleW(L"DOOMx64vk.exe"));
        if (doom_base) {
            const uintptr_t rva = reinterpret_cast<uintptr_t>(job_fn) - doom_base;
            // SHARED-DEPTH ISOLATION (2026-08-16). Eye 0's severe symptom is
            // loss of opaque/albedo content while depth-like blocks and the
            // full effects chain survive - the signature of DOOM's deferred
            // composite reading a depth buffer that no longer matches its
            // geometry. Mirrors deliberately SHARE DOOM's real depth, and the
            // doubled call's RenderDepth writes eye-1 geometry into it.
            //
            // Suppressing RenderDepth alone leaves DOOM's depth untouched
            // while the doubled call still renders opaque into mirrors.
            //   eye 0 detail RETURNS => shared depth confirmed; private
            //                           per-eye depth is the fix after all.
            //   eye 0 still broken   => depth is not the severe cause.
            //
            // This reopens what was closed as STEP 2. The no-jobs run proved
            // only that SOME damage is non-render (the mild lighting shifts);
            // RenderDepth never ran in it, so it could not have tested this.
            // ResolveDepth added 2026-08-16: suppressing RenderDepth alone
            // made eye 0 "definitely better but not all the way", and
            // ResolveDepth is the OTHER depth-touching job in the allow-list.
            // Leaving it running left a second writer to DOOM's shared depth.
            if ((rva == kRenderDepthFnRva || rva == kResolveDepthFnRva) &&
                !doubled_allow_render_depth.load(std::memory_order_relaxed)) {
                const uint32_t n = doubled_depth_skipped.fetch_add(1, std::memory_order_relaxed);
                if (n < 2) {
                    log::warn(std::format(
                        "SharedDepthIsolation: suppressing the doubled call's RenderDepth "
                        "(skip #{}) so DOOM's real depth buffer is never written. Eye 1 may "
                        "be wrong or empty without its prepass - judge EYE 0 only.", n));
                }
                return;
            }
            const bool needed = (rva == kRenderDepthFnRva) ||
                                (rva == kRenderOpaqueFnRva) ||
                                (rva == kResolveDepthFnRva);
            // 0x1A39930 was added here as the STEP 6 pilot target and has
            // been REMOVED. It was identified as "the deferred lighting
            // block" by having the most mirror-backed reads (~152 of 240),
            // and that criterion was wrong: the census shows it spans 19
            // passes and 0x186A2F0 spans 29, so both are post-process
            // CHAINS. The most mirror reads is simply where the post chain
            // re-reads previous targets, not where lighting happens. The
            // shadow-consuming lighting pass is the single pass 18,
            // jobRva 0x19AE9E0 (shared=393 shadow-atlas reads).
            // Allowing it made eye 1 WORSE: the doubled call composited a
            // post chain out of inputs the jobs we do not run never
            // produced, so its LDR output was black with only the inline
            // HUD in it. Filtered mode is therefore back to the known-good
            // depth+opaque set, and the real test is UNFILTERED.
            if (!needed) return;  // skip: not part of the world G-buffer
        }
    }
    // DescriptorCensus job attribution: label descriptor binds recorded
    // inside this job with its fn RVA. Save/restore rather than clear, since
    // jobs can dispatch jobs. Only covers JobDispatch-routed work - see the
    // warning at doubled_current_job_rva's declaration.
    // ⚠ NOT gated on descriptor_census_enabled. It was, and that made
    // STEP 6's redirection dead code: ReadRedirect keys on
    // doubled_current_job_rva == kDeferredLightingFnRva, so with the census
    // disarmed the RVA stayed 0 and the substitution could never fire once -
    // the exact "path that cannot execute" failure Rule 1 exists to catch.
    // Now maintained for every doubled dispatch; it is two loads and a
    // subtract.
    const uintptr_t census_prev_job_rva = doubled_current_job_rva;
    if (inside_frame_root_double) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"DOOMx64vk.exe"));
        doubled_current_job_rva = base ? reinterpret_cast<uintptr_t>(job_fn) - base : 0;
    }
    // DrawParity needs the same attribution for the REAL call, or its half of
    // the table has no job column to line up against. Same save/restore
    // discipline; jobs dispatch jobs.
    const uintptr_t real_prev_job_rva = real_current_job_rva;
    if (inside_real_frame_root) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"DOOMx64vk.exe"));
        real_current_job_rva = base ? reinterpret_cast<uintptr_t>(job_fn) - base : 0;
    }
    // ------------- G1: DO NOT RE-RUN A CONSUMING, VIEW-INDEPENDENT JOB ----
    // Runs 8b/9 changed the diagnosis. F1 restored the global bounds-check
    // count [0x705CA24] 400 times with read-back proof and the duplicate at
    // binding 2 persisted - because that count is only ONE of the things the
    // job drains. The static read of the loop tail shows the rest:
    //
    //   019AF277  mov [rdi+rsi+0x169], r13b   ; per-system flag  = 0
    //   019AF27F  mov [rdi+rsi+0x150], r13d   ; per-system COUNT = 0
    //   019AF28A  add rdi, 0x170              ; next system
    //   019AF2C4  mov [0x705CA24], r13d       ; global count     = 0
    //
    // and [rdi+rsi+0x150] is exactly what gates the binding loop at 0x19AF132.
    // GPU Particle Simulate is a CONSUMING job: it drains its inputs as it
    // runs. Re-invoking it after it has drained everything is a run with no
    // inputs, which binds the shared default object into both
    // gpuParticleVectorField1 and particles - and C2 showed there is ONE
    // context object shared by both eyes, so the doubled run does not merely
    // fail for eye 1, it OVERWRITES slots eye 1's rendering then reads.
    //
    // It is also view-independent: it is a simulation, and the job map shows
    // generation and rendering are separate jobs (ParticleGenJob 0x19B7D40,
    // ParticleGenCoopJob 0x19B7CC0, RenderParticleLightAtlas 0x1A3B0A0). The
    // frame has already simulated its particles by the time we re-invoke.
    //
    // So do not restore drained state - do not re-run the job. Same shape as
    // the existing SharedDepthIsolation suppression, and reversible.
    if (g1_suppress_particle_sim.load(std::memory_order_relaxed) && inside_frame_root_double) {
        static const uintptr_t g1_base =
            reinterpret_cast<uintptr_t>(GetModuleHandleW(L"DOOMx64vk.exe"));
        if (g1_base &&
            reinterpret_cast<uintptr_t>(job_fn) - g1_base == kP1ParticleSimJobRva) {
            const uint32_t n = g1_suppressed.fetch_add(1, std::memory_order_relaxed);
            if (n < 3) {
                log::warn(std::format(
                    "G1 SUPPRESS #{}: skipping the DOUBLED dispatch of GPU Particle Simulate "
                    "({:#x}). The frame already simulated its particles; re-running a drained "
                    "consuming job writes defaults into the SHARED context.", n, kP1ParticleSimJobRva));
            }
            real_current_job_rva = real_prev_job_rva;
            doubled_current_job_rva = census_prev_job_rva;
            return;
        }
    }

    // V1: the view index, at the deferred lighting job, both sides. See the
    // block beside v1_decode_jobarg. Scoped by job RVA, never by pass ordinal.
    // Captured BEFORE real_job_dispatch, because jobArg points into the
    // caller's live stack frame and is only guaranteed valid here.
    // ⚠ The gate is v1_armed OR c1_armed, never v1_armed alone. They arm on the
    // same press but self-test INDEPENDENTLY, and nesting C1 under V1's flag
    // would let a V1 self-test failure silently produce an empty C1 table that
    // reads exactly like "the jobs never dispatched".
    const bool v1_on = v1_armed.load(std::memory_order_relaxed);
    const bool c1_on = c1_armed.load(std::memory_order_relaxed);
    if ((v1_on || c1_on) && (inside_frame_root_double || inside_real_frame_root)) {
        static const uintptr_t v1_base =
            reinterpret_cast<uintptr_t>(GetModuleHandleW(L"DOOMx64vk.exe"));
        const uintptr_t v1_rva = v1_base
            ? reinterpret_cast<uintptr_t>(job_fn) - v1_base : 0;
        uint32_t v1_job = kV1Jobs;
        for (uint32_t j = 0; j < kV1Jobs; ++j) if (v1_rva == kV1JobRva[j]) { v1_job = j; break; }
        if (v1_job < kV1Jobs) {
            const uint32_t side = inside_frame_root_double ? 1u : 0u;
            if (v1_on) v1_seen[v1_job][side].fetch_add(1, std::memory_order_relaxed);
            const uint32_t slot = v1_on
                ? v1_kept[v1_job][side].load(std::memory_order_relaxed) : kV1PerSide;
            if (slot < kV1PerSide) {
                V1Rec rec{};
                if (v1_decode_jobarg(reinterpret_cast<uintptr_t>(job_arg), rec)) {
                    v1_decode_globals(v1_base, rec);
                }
                v1_recs[v1_job][side][slot] = rec;
                v1_kept[v1_job][side].store(slot + 1, std::memory_order_relaxed);
            }

            // D1. Only the particle job, only the doubled side, only while
            // explicitly enabled, and capped. Relative to what the engine just
            // computed: latch = jobArg[0x10] - lastRealDelta.
            // F1: save on the real side, restore on the doubled side. Scoped to
            // the particle job only - DeferredPasses reads this count as 0 on
            // both sides and is unaffected, which E1 confirmed.
            if (v1_job == 0) {
                if (side == 0) {
                    int32_t c = 0;
                    if (safe_read_bytes(v1_base + 0x705CA24, &c, sizeof(c)) && c > 0) {
                        f1_saved_count.store(c, std::memory_order_relaxed);
                    }
                } else if (f1_enabled.load(std::memory_order_relaxed)) {
                    const uint32_t n = f1_attempts.fetch_add(1, std::memory_order_relaxed);
                    if (n >= kF1MaxAttempts) {
                        f1_enabled.store(false, std::memory_order_relaxed);
                        log::warn("F1 reached its attempt cap; disabling automatically");
                    } else {
                        const int32_t c = f1_saved_count.load(std::memory_order_relaxed);
                        if (c <= 0) {
                            f1_no_saved.fetch_add(1, std::memory_order_relaxed);
                        } else if (safe_write_bytes(v1_base + 0x705CA24, &c, sizeof(c))) {
                            const uint32_t w =
                                f1_restores.fetch_add(1, std::memory_order_relaxed);
                            if (w < 3) {
                                int32_t rb = 0;
                                safe_read_bytes(v1_base + 0x705CA24, &rb, sizeof(rb));
                                log::warn(std::format(
                                    "F1 RESTORE #{}: wrote tblCount={} readback={} => the bounds "
                                    "check at 0x19AF14F will now admit idx 0..{}",
                                    w, c, rb, c - 1));
                            }
                        } else {
                            f1_failures.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
            }
            if (v1_job == 0 && side == 0) {
                // Learn the real delta from the real dispatch itself, the same
                // way the job does: new - old, both read here.
                uint32_t tick = 0, latch = 0;
                if (safe_read_bytes(reinterpret_cast<uintptr_t>(job_arg) + 0x10,
                        &tick, sizeof(tick)) &&
                    safe_read_bytes(v1_base + kV1FrameDeltaRva, &latch, sizeof(latch))) {
                    const uint32_t d = tick - latch;
                    if (latch != 0 && d != 0 && d < 1000) {
                        d1_last_real_delta.store(d, std::memory_order_relaxed);
                    }
                }
            } else if (v1_job == 0 && side == 1 &&
                       d1_enabled.load(std::memory_order_relaxed)) {
                const uint32_t n = d1_attempts.fetch_add(1, std::memory_order_relaxed);
                if (n >= kD1MaxAttempts) {
                    d1_enabled.store(false, std::memory_order_relaxed);
                    log::warn("D1 reached its attempt cap; disabling automatically");
                } else {
                    const uint32_t d = d1_last_real_delta.load(std::memory_order_relaxed);
                    uint32_t tick = 0;
                    if (d != 0 && safe_read_bytes(reinterpret_cast<uintptr_t>(job_arg) + 0x10,
                            &tick, sizeof(tick))) {
                        const uint32_t forced = tick - d;
                        if (safe_write_bytes(v1_base + kV1FrameDeltaRva,
                                &forced, sizeof(forced))) {
                            const uint32_t w =
                                d1_writes.fetch_add(1, std::memory_order_relaxed);
                            // Bounded proof the forcing actually took. V1's own
                            // record is captured BEFORE this write, so it will
                            // still show the unforced latch - without these
                            // lines a successful force is invisible.
                            if (w < 3) {
                                uint32_t readback = 0;
                                safe_read_bytes(v1_base + kV1FrameDeltaRva,
                                    &readback, sizeof(readback));
                                log::warn(std::format(
                                    "D1 FORCE #{}: tick={} lastRealDelta={} wroteLatch={} "
                                    "readback={} => the job will compute delta={} instead of 0",
                                    w, tick, d, forced, readback, tick - readback));
                            }
                        } else {
                            d1_write_failures.fetch_add(1, std::memory_order_relaxed);
                        }
                    } else {
                        d1_write_failures.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        }

        // C1: the shared context's slots, before and after this dispatch.
        if (v1_job < kV1Jobs && c1_armed.load(std::memory_order_relaxed)) {
            const uint32_t side = inside_frame_root_double ? 1u : 0u;
            const uint32_t rep = c1_taken[v1_job][side].load(std::memory_order_relaxed);
            if (rep < kC1Reps) {
                uintptr_t mid = 0, ctx = 0;
                if (safe_read_bytes(reinterpret_cast<uintptr_t>(job_arg), &mid, sizeof(mid)) &&
                    mid && safe_read_bytes(mid, &ctx, sizeof(ctx)) && ctx) {
                    c1_snapshot(v1_base, ctx, c1_pre[v1_job][side][rep]);
                    // C2 rides the same bracket: the FULL slot table, captured
                    // AFTER the dispatch, because binding 2 is written during
                    // the job (0x19AF250) and a pre-dispatch snapshot would
                    // show the reset state on both sides - which is exactly
                    // what C1 measured and why it saw nothing.
                    { FbTramp fbt_(FB_D_JOB_DISPATCH); real_job_dispatch(job_ctx, job_fn, job_arg, wait_flag); }
                    c1_snapshot(v1_base, ctx, c1_post[v1_job][side][rep]);
                    if (rep == 0 && c2_armed.load(std::memory_order_relaxed) &&
                        c2_taken[v1_job][side].load(std::memory_order_relaxed) == 0) {
                        c2_snapshot(ctx, c2_snap[v1_job][side]);
                        c2_taken[v1_job][side].store(1, std::memory_order_relaxed);
                    }
                    c1_taken[v1_job][side].store(rep + 1, std::memory_order_relaxed);
                    real_current_job_rva = real_prev_job_rva;
                    doubled_current_job_rva = census_prev_job_rva;
                    return;
                }
            }
        }
    }
    { FbTramp fbt_(FB_D_JOB_DISPATCH); real_job_dispatch(job_ctx, job_fn, job_arg, wait_flag); }
    real_current_job_rva = real_prev_job_rva;
    doubled_current_job_rva = census_prev_job_rva;
}
void __fastcall hook_render_walk_bsp(void* arg1) {
    FbDoom fbd_(FB_D_RENDER_WALK_BSP);
    if (doubled_skip_gather_now()) return;
    log_upstream_job_call("RenderWalkBSP", arg1, walk_bsp_sample_count, last_walk_bsp_arg1);
    // WalkBSPCullSourceProbe (2026-08-09, read-only): static decompile of
    // this function (DOOM+0x1A3BE50) shows its actual plane-side test reads
    // a position-shaped vector from *(arg1+8)+0x110/+0x114/+0x118 -
    // `dot(pos, planeNormal) + planeD`, the standard BSP camera-side-of-
    // plane formula - plus a separate AABB-shaped min/max bound at
    // +0x120/+0x124/+0x12C/+0x130, neither of which is +0xC44 (what
    // WidenGatherFovProbe has been rotating this whole session, confirmed
    // zero effect at two pipeline locations). First live sample: confirmed
    // *(arg1+8) is NOT the same object as the render-view (sameAsRenderView
    // =false), and the values looked suspiciously small/round
    // (pos=(1,0,0), a ~32-unit bounds box) for world-space camera data -
    // this project's own logged camera positions run in the thousands.
    // Sampling repeatedly now, spread over several seconds, to settle
    // definitively whether `pos` tracks head movement at all (camera-
    // derived) or is constant (unrelated scratch/per-node data, closing
    // this lead) - one sample can't distinguish those, several can.
    if (walk_bsp_cull_source_probe_sample_count.load(std::memory_order_relaxed) <
            kMaxWalkBspCullSourceProbeSamples &&
        walk_bsp_cull_source_probe_frame_count.fetch_add(1, std::memory_order_relaxed) %
                kWalkBspCullSourceProbeFrameGap ==
            0) {
        const uint32_t sample =
            walk_bsp_cull_source_probe_sample_count.fetch_add(1, std::memory_order_relaxed);
        uintptr_t indirect = 0;
        safe_read_bytes(reinterpret_cast<uintptr_t>(arg1) + 8, &indirect, sizeof(indirect));
        float pos[3]{NAN, NAN, NAN};
        float boundsMin[3]{NAN, NAN, NAN};
        float boundsMax[3]{NAN, NAN, NAN};
        if (indirect) {
            safe_read_bytes(indirect + 0x110, pos, sizeof(pos));
            safe_read_bytes(indirect + 0x120, boundsMin, sizeof(boundsMin));
            safe_read_bytes(indirect + 0x12C, boundsMax, sizeof(boundsMax));
        }
        const uintptr_t slot = captured_render_view_slot.load(std::memory_order_relaxed);
        uintptr_t current_render_view = 0;
        if (slot) safe_read_bytes(slot, &current_render_view, sizeof(current_render_view));
        log::warn(std::format(
            "WalkBSPCullSourceProbe sample={} arg1={} indirectAt0x8={} sameAsRenderView={} "
            "pos=({:.2f} {:.2f} {:.2f}) boundsMin=({:.2f} {:.2f} {:.2f}) "
            "boundsMax=({:.2f} {:.2f} {:.2f})",
            sample, arg1, reinterpret_cast<void*>(indirect), indirect == current_render_view,
            pos[0], pos[1], pos[2], boundsMin[0], boundsMin[1], boundsMin[2],
            boundsMax[0], boundsMax[1], boundsMax[2]));
    }
    log_world_vp_at("RenderWalkBSP");
    { FbTramp fbt_(FB_D_RENDER_WALK_BSP); real_render_walk_bsp(arg1); }
}
void __fastcall hook_render_gather_finish(void* arg1) {
    FbDoom fbd_(FB_D_RENDER_GATHER_FINISH);
    if (doubled_skip_gather_now()) return;
    log_upstream_job_call("RenderGatherFinish", arg1, gather_finish_sample_count,
        last_gather_finish_arg1);
    log_world_vp_at("RenderGatherFinish");
    { FbTramp fbt_(FB_D_RENDER_GATHER_FINISH); real_render_gather_finish(arg1); }
}
// WorldVpSourceProbe (2026-08-16). Read-only, bounded.
//
// The 200 uu A/B produced the documented "mid-terrain glitch" signature:
// per-depth-band difference peaks in the MIDDLE of the scene and falls to
// nothing at both the far sky (0.52) and the near floor/interior walls
// (0.16-0.43). That is partial camera application - some geometry reads a
// matrix we shifted, the rest reads one we did not.
//
// Suspected cause is ordering inside the frame root:
//     gather chain      <- Lever A shifts renderView AND all six destinations
//     SetupView(world)  <- RE-COPIES all six destinations from CameraSrc,
//                          overwriting Lever A's writes
//     RenderSort -> RenderView (the draws)
//
// Rather than guess which matrix survives, read them all at RenderSort -
// after SetupView has run, immediately before the draws. Logs the
// translation column of every candidate so the ones carrying the shift are
// directly visible. A 200 uu offset is unmistakable in these numbers.
std::atomic_uint32_t world_vp_source_probe_lines{0};
constexpr uint32_t kMaxWorldVpSourceProbeLines = 24;

void log_all_world_vp_sources(const char* when) {
    if (world_vp_source_probe_lines.fetch_add(1, std::memory_order_relaxed)
            >= kMaxWorldVpSourceProbeLines) return;
    const uintptr_t doom_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"DOOMx64vk.exe"));
    if (!doom_base) return;
    auto row0 = [&](uintptr_t addr, const char* label) {
        alignas(16) float m[16]{};
        if (!safe_read_bytes(addr, m, sizeof(m))) return;
        log::warn(std::format("WorldVpSource {} {:<22} @{:#x} row0=({:.3f} {:.3f} {:.3f} {:.4f})",
            when, label, addr, m[0], m[1], m[2], m[3]));
    };
    uintptr_t renderer = 0, camera_src = 0;
    if (safe_read_bytes(doom_base + kRendererPtrGlobalRva, &renderer, sizeof(renderer)) &&
        renderer && safe_read_bytes(renderer, &camera_src, sizeof(camera_src)) && camera_src) {
        row0(camera_src + kCameraSrcWorldVpOffset, "CameraSrc+0x3AC0");
        row0(camera_src + 0xC44, "CameraSrc+0xC44");
    }
    // renderView+0xC44/+0xC84 - the offset Lever A writes, and the one three
    // independent methods confirmed as the live world camera. Omitted from
    // the first version of this probe, which is why it came back with every
    // candidate unshifted while mid-range geometry demonstrably moved.
    const uintptr_t rv_slot = captured_render_view_slot.load(std::memory_order_relaxed);
    if (rv_slot) {
        uintptr_t render_view = 0;
        if (safe_read_bytes(rv_slot, &render_view, sizeof(render_view)) && render_view) {
            row0(render_view + 0xC44, "renderView+0xC44");
            row0(render_view + 0xC84, "renderView+0xC84");
        }
    }
    int idx = 0;
    for (uintptr_t slot_off = 0x6FD5FC0; slot_off <= 0x6FD5FE8; slot_off += 8, ++idx) {
        uintptr_t dest_obj = 0, dest_data = 0;
        if (!safe_read_bytes(doom_base + slot_off, &dest_obj, sizeof(dest_obj)) || !dest_obj) continue;
        if (!safe_read_bytes(dest_obj, &dest_data, sizeof(dest_data)) || !dest_data) continue;
        row0(dest_data + 0xC44, std::format("dest{}+0xC44", idx).c_str());
    }
}

void __fastcall hook_render_sort(void* arg1) {
    FbDoom fbd_(FB_D_RENDER_SORT);
    if (doubled_skip_gather_now()) return;
    if (inside_frame_root_double) log_all_world_vp_sources("DOUBLED");
    log_upstream_job_call("RenderSort", arg1, sort_sample_count, last_sort_arg1);
    log_world_vp_at("RenderSort");
    { FbTramp fbt_(FB_D_RENDER_SORT); real_render_sort(arg1); }
}
// Phase 1 (STEREO-IMPLEMENTATION-PLAN.md v2). Thin passthrough - the
// interesting behavior is in hook_cmd_begin_render_pass (capturing and,
// during a per-eye extra call, redirecting the vkCmdBeginRenderPass this
// real call issues), not here. inside_render_depth brackets only this REAL
// invocation (never the reinvoked one - that goes through real_render_depth
// directly via safe_call_render_opaque, bypassing this hook entirely) so
// the capture-once logic below can tell "this is RenderDepth's own render
// pass" apart from any other job's.

void __fastcall hook_render_depth(void* arg1) {
    FbDoom fbd_(FB_D_RENDER_DEPTH);
    log_upstream_job_call("RenderDepth", arg1, render_depth_sample_count, last_render_depth_arg1);
    inside_render_depth = true;
    { FbTramp fbt_(FB_D_RENDER_DEPTH); real_render_depth(arg1); }
    inside_render_depth = false;
}
void __fastcall hook_render_gather_models_prepare(void* arg1) {
    FbDoom fbd_(FB_D_RENDER_GATHER_MODELS_PREPARE);
    if (doubled_skip_gather_now()) return;
    log_upstream_job_call("RenderGatherModelsPrepare", arg1, gather_models_sample_count,
        last_gather_models_arg1);
    log_world_vp_at("RenderGatherModelsPrepare");
    sample_camera_state_at(CS_GatherModels);
    { FbTramp fbt_(FB_D_RENDER_GATHER_MODELS_PREPARE); real_render_gather_models_prepare(arg1); }
}
void __fastcall hook_render_gather_decals_prepare(void* arg1) {
    FbDoom fbd_(FB_D_RENDER_GATHER_DECALS_PREPARE);
    if (doubled_skip_gather_now()) return;
    log_upstream_job_call("RenderGatherDecalsPrepare", arg1, gather_decals_sample_count,
        last_gather_decals_arg1);
    log_world_vp_at("RenderGatherDecalsPrepare");
    sample_camera_state_at(CS_GatherDecals);
    { FbTramp fbt_(FB_D_RENDER_GATHER_DECALS_PREPARE); real_render_gather_decals_prepare(arg1); }
}
void __fastcall hook_render_gather_lights_prepare(void* arg1) {
    FbDoom fbd_(FB_D_RENDER_GATHER_LIGHTS_PREPARE);
    if (doubled_skip_gather_now()) return;
    log_upstream_job_call("RenderGatherLightsPrepare", arg1, gather_lights_sample_count,
        last_gather_lights_arg1);
    log_world_vp_at("RenderGatherLightsPrepare");
    sample_camera_state_at(CS_GatherLights);
    { FbTramp fbt_(FB_D_RENDER_GATHER_LIGHTS_PREPARE); real_render_gather_lights_prepare(arg1); }
}
void __fastcall hook_render_gather_add_always(void* arg1) {
    FbDoom fbd_(FB_D_RENDER_GATHER_ADD_ALWAYS);
    if (doubled_skip_gather_now()) return;
    log_upstream_job_call("RenderGatherAddAlways", arg1, gather_add_always_sample_count,
        last_gather_add_always_arg1);
    log_world_vp_at("RenderGatherAddAlways");
    sample_camera_state_at(CS_GatherAddAlways);
    { FbTramp fbt_(FB_D_RENDER_GATHER_ADD_ALWAYS); real_render_gather_add_always(arg1); }
}

// Lever G hook (2026-08-10). Fires every RenderGuiModels invocation. On the
// first armed frame after Lever G cycles into non-OFF, dumps the three
// candidate matrix offsets to the log so we can confirm the decompile
// before writing. Then applies the current mode's mutation destructively
// to ctx+0x11f0 and ctx+0x1130. On disarm the mutation stops and DOOM's
// next per-frame RenderView setup restores the real matrices, same
// no-persistent-damage story as Lever A.

// ===========================================================================
// GUI MODEL TRANSFORMS (2026-08-26) - THE LAST LINK IN THE WEAPON'S CHAIN
// ===========================================================================
//
// WHAT THE 21:00 RUN ELIMINATED, with a positive control. The bake patch
// verified PERFECTLY - `bakeMatch=yes relAB=0.000000` on 23393 of 23393
// attempts, operand order proj x view - so our multiply reproduces DOOM's own
// exactly and the seam was correctly identified. It then wrote a deliberately
// WRONG MVP (an extra 30 degrees of yaw) 1308 times, and the image did not
// change.
//
// So NOTHING VISIBLE READS renderer+0x4955A4 after SetupView_gui writes it.
// That is an elimination backed by a control, not another ambiguous null, and
// it fits the decompile: the per-model draw recomputes
// (ctx+0x1170) x (ctx+0x11F0) x model AT DRAW TIME, so the baked product is a
// cached value the draw path rebuilds from the two source matrices anyway.
//
// WHICH LEAVES EXACTLY ONE UNMEASURED TERM. In P x V x M:
//   P (ctx+0x1170)  static - ChainAngles measured a constant 180 deg at every
//                   head yaw, i.e. a sign convention, not head rotation.
//   V (ctx+0x11F0)  carries R exactly ONCE - ChainAngles ratio 0.998.
//   M (model)       NEVER MEASURED.
// One R in the chain gives 1x. The owner sees 2x. And CTRL+Up showed that
// removing Lever V's R welds the gun - so R reaches the weapon through a path
// that also collapses when V stops rotating. A model transform that DOOM
// rebuilds each frame from the (now rotated) camera does exactly that: the
// weapon's world placement follows the head, and then it is drawn through a
// head-rotated view. R twice.
//
// The HUD reticle corroborates the shape of this: it is the ONE thing that
// follows the head, i.e. the one thing on a genuinely unrotated path. Three
// populations at 0R (reticle), 1R (world, correct) and 2R (weapon).
//
// This scans the model array for transform-shaped blocks, reports the rotation
// each one carries against DOOM's clean camera basis, and - under a slot -
// writes a scoped correction that is restored the instant the real call
// returns, exactly like the gun-unshift swap above.
constexpr size_t kModelObjWindow = 0x400;   // per model OBJECT, not the pointer table
constexpr int kMaxModelObjects = 16;
constexpr int kMaxModelHits = 12;
struct ModelHit { uintptr_t addr; float saved[16]; };
thread_local ModelHit g_model_saved[kMaxModelHits];
thread_local int g_model_saved_count = 0;
std::atomic_uint32_t g_model_scan_lines{0};
std::atomic_uint64_t g_model_attempts{0};
std::atomic_uint64_t g_model_written{0};
std::atomic_uint64_t g_model_restored{0};
std::atomic_uint64_t g_model_no_array{0};
// SLOT 1 liveness: frames where the real RenderGuiModels call was skipped.
std::atomic_uint64_t g_gui_pass_skipped{0};
// SLOT 3 liveness: Lever V writes that carried the extra 30 deg probe yaw.
std::atomic_uint64_t g_leverv_extra{0};
constexpr uint32_t kMaxModelScanLines = 10;

// True when the upper-left 3x3 is orthonormal to a tolerance - a rigid
// transform, which is what a placement matrix is and what random floats are
// not. Deliberately tight: this predicate gates a WRITE.
bool is_rigid_3x3(const float* m) {
    for (int i = 0; i < 12; ++i) if (!std::isfinite(m[i])) return false;
    const float l0 = wrap::row_length(m, 0);
    const float l1 = wrap::row_length(m, 1);
    const float l2 = wrap::row_length(m, 2);
    if (std::fabs(l0 - 1.0f) > 0.02f) return false;
    if (std::fabs(l1 - 1.0f) > 0.02f) return false;
    if (std::fabs(l2 - 1.0f) > 0.02f) return false;
    auto dot = [&](int a, int b) {
        return m[a * 4] * m[b * 4] + m[a * 4 + 1] * m[b * 4 + 1] + m[a * 4 + 2] * m[b * 4 + 2];
    };
    if (std::fabs(dot(0, 1)) > 0.02f) return false;
    if (std::fabs(dot(0, 2)) > 0.02f) return false;
    if (std::fabs(dot(1, 2)) > 0.02f) return false;
    return true;
}

void gui_models_scan_and_patch(void* arg1) {
    g_model_saved_count = 0;
    g_model_attempts.fetch_add(1, std::memory_order_relaxed);
    if (!arg1) return;
    uintptr_t array_ptr = 0;
    int32_t count = -1;
    safe_read_bytes(reinterpret_cast<uintptr_t>(arg1) + 0x08, &array_ptr, sizeof(array_ptr));
    safe_read_bytes(reinterpret_cast<uintptr_t>(arg1) + 0x10, &count, sizeof(count));
    if (!array_ptr) { g_model_no_array.fetch_add(1, std::memory_order_relaxed); return; }

    // ⚠ THE ARRAY HOLDS POINTERS. The 21:34 run dumped
    // first4=0x209989145b0,0x2082e4832c0,... - four large heap addresses, with
    // count=8 - and the flat scan found ZERO rigid transforms because it was
    // scanning the pointer table itself. The transforms are one indirection
    // away. That run was VOID for this hypothesis, not a null.
    float rr[3], ru[3], rf[3];
    const bool have_basis = wrap::reference_basis(rr, ru, rf);
    float hy = 0.0f, hp = 0.0f, hr = 0.0f;
    const bool have_head = wrap::residual_head_angles(hy, hp, hr);

    uint64_t ptrs[kMaxModelObjects]{};
    const int n_models =
        (count > 0 && count < kMaxModelObjects) ? count : kMaxModelObjects;
    if (!safe_read_bytes(array_ptr, ptrs, sizeof(uint64_t) * n_models)) {
        g_model_no_array.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    static thread_local std::vector<unsigned char> buf;
    uintptr_t hits[kMaxModelHits]{};
    int found = 0;
    int objects_read = 0;
    std::string report;
    for (int mi = 0; mi < n_models && found < kMaxModelHits; ++mi) {
        const uintptr_t obj = static_cast<uintptr_t>(ptrs[mi]);
        // A heap pointer, not a small integer or a float pattern.
        if (obj < 0x10000 || (obj & 0x7) != 0) continue;
        buf.assign(kModelObjWindow, 0);
        if (!safe_read_bytes(obj, buf.data(), kModelObjWindow)) continue;
        ++objects_read;
        int in_this = 0;
        for (size_t off = 0; off + 64 <= kModelObjWindow && found < kMaxModelHits;
             off += sizeof(float)) {
            const float* m = reinterpret_cast<const float*>(buf.data() + off);
            if (!is_rigid_3x3(m)) continue;
            hits[found++] = obj + off;
            ++in_this;
            if (have_basis && have_head && std::fabs(hy) > 1e-4f) {
                float d = m[0] * rr[0] + m[1] * rr[1] + m[2] * rr[2];
                if (d > 1.0f) d = 1.0f;
                if (d < -1.0f) d = -1.0f;
                const float a = std::acos(d);
                report += std::format(" m{}+{:#06x}(deg {:.2f} ratio {:.3f})", mi,
                                      static_cast<unsigned>(off), a * 57.29578f,
                                      a / std::fabs(hy));
            } else {
                report += std::format(" m{}+{:#06x}", mi, static_cast<unsigned>(off));
            }
            // One per object is enough to characterise it and keeps the
            // budget from being eaten by the first model.
            if (in_this >= 2) break;
        }
    }

    if (have_head && std::fabs(hy) > 0.0873f &&
        g_model_scan_lines.fetch_add(1, std::memory_order_relaxed) < kMaxModelScanLines) {
        uint64_t q[4]{};
        safe_read_bytes(array_ptr, q, sizeof(q));
        log::warn(std::format(
            "GuiModels array={:#x} count={} objectsRead={} headYaw={:.2f} deg "
            "first4={:#x},{:#x},{:#x},{:#x} -> {} rigid transforms:{}\n"
            "READING IT: P x V x M is the weapon's MVP. P is static and V carries the head "
            "rotation exactly once, both already measured - M is the ONLY term never measured, "
            "and the gun moves at TWICE the world. ratio ~1.0 on any block below means that "
            "model transform ALSO carries the head rotation, which is the second R and the "
            "whole answer. ratio ~0.0 means the placements are clean and the doubling is not "
            "here either. first4 says whether this array holds POINTERS (large similar values) "
            "or inline transforms (floats) - if pointers, the transforms are one indirection "
            "away and this scan is looking at the wrong bytes, which is itself the finding. "
            "Capped at {} lines, only past 5 degrees of head yaw.",
            array_ptr, count, objects_read, hy * 57.29578f, q[0], q[1], q[2], q[3],
            found, found ? report : std::string(" NONE"), kMaxModelScanLines));
    }

    // ===== THE SCOPED CORRECTION, under a slot ============================
    // Slot 1 removes one head rotation from every rigid transform found.
    // Slot 3 is its POSITIVE CONTROL: an unmissable +30 degrees instead, so a
    // null is again attributable rather than ambiguous - the same discipline
    // that made the bake elimination stick.
    // RETIRED 2026-08-27: slots reassigned; the model-hit correction and its
    // +30 control stay disconnected pending the corpus verdict. Hard false.
    const bool ctrl = false;
    const bool fix = false;
    if ((!ctrl && !fix) || !have_head || !wrap::head_look_active()) return;
    for (int i = 0; i < found && g_model_saved_count < kMaxModelHits; ++i) {
        alignas(16) float m[16]{};
        if (!safe_read_bytes(hits[i], m, sizeof(m))) continue;
        if (!is_rigid_3x3(m)) continue;
        ModelHit& s = g_model_saved[g_model_saved_count];
        s.addr = hits[i];
        std::memcpy(s.saved, m, sizeof(m));
        if (ctrl) apply_yaw_rotation(m, 0.5236f);
        else      wrap::apply_head_rotation(m, -hy, -hp, -hr, wrap::head_axes());
        bool finite = true;
        for (int k = 0; k < 16; ++k) if (!std::isfinite(m[k])) { finite = false; break; }
        if (!finite) continue;
        if (safe_write_bytes(hits[i], m, sizeof(m))) {
            ++g_model_saved_count;
            g_model_written.fetch_add(1, std::memory_order_relaxed);
        }
    }
}
#endif  // DOOMVR_RESEARCH_TOOLS

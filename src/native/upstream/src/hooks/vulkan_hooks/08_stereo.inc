// 08_stereo.inc - dispatch fate, arm_frame_root_double, drive_scripted_test
// 2248 of these lines sit inside #ifdef DOOMVR_RESEARCH_TOOLS,
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

// Called from hook_cmd_dispatch / _indirect AFTER the suppression decision, so
// `executed` is the execution verdict and not a binding observation. Trap 7:
// ComputeProbe's line sits ABOVE the suppress-return and can never say this.
void dispatch_fate_record(bool indirect, bool suppressed, const DfSsboMatch& match,
                          uint32_t gx, uint32_t gy, uint32_t gz,
                          const std::vector<VkDescriptorSet>& bound_sets, bool is_draw = false) {
    const bool doubled = inside_frame_root_double;
    {
        const uint32_t serial = g_frame_root_serial.load(std::memory_order_relaxed);
        uint32_t expected = 0;
        if (!df_frame_latched.compare_exchange_strong(expected, serial,
                                                      std::memory_order_relaxed) &&
            expected != serial) {
            df_frames_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    // Collected first into a stack record under SEQUENTIAL, NON-NESTED lock
    // scopes - swapchains_mutex, then mirror_mutex, then
    // framebuffer_depth_view_mutex, then the leaf df mutexes. Run 17 wedged the
    // render thread by nesting three of these; Rule 9 is why this is written
    // out longhand instead of in one block.
    DfRecord tmp{};
    tmp.used = true;
    tmp.pass = doubled ? doubled_pass_seq : real_pass_seq;
    tmp.job_rva = doubled ? doubled_current_job_rva : real_current_job_rva;
    tmp.sets_bound = static_cast<uint32_t>(bound_sets.size());
    tmp.set0 = bound_sets.empty() ? VK_NULL_HANDLE : bound_sets.front();
    tmp.dispatches = 1;
    tmp.indirect = indirect ? 1u : 0u;
    tmp.executed = suppressed ? 0u : 1u;
    tmp.suppressed = suppressed ? 1u : 0u;
    tmp.would_suppress = match.found;
    tmp.ssbo_matches = match.matches;
    tmp.ssbo_binding = match.binding;
    tmp.ssbo_buffer = match.buffer;
    tmp.gx = gx; tmp.gy = gy; tmp.gz = gz;
    tmp.is_draw = is_draw;

    // 1. EVERY image binding this dispatch's recorded sets declare - inputs as
    //    well as storage outputs. Sorted so the row order is stable across
    //    runs: descriptor_image_sets is an unordered_map and an unstable order
    //    would make two runs' tables uncomparable by eye.
    struct Cand { VkDescriptorSet set; uint32_t binding; VkImageView view; VkImageLayout want;
                  VkDescriptorType type; };
    Cand cands[kDfMaxRw]{};
    uint32_t cand_count = 0;
    uint32_t cand_over = 0;
    {
        std::unique_lock lock(swapchains_mutex);
        for (VkDescriptorSet set : bound_sets) {
            const auto imgs = descriptor_image_sets.find(set);
            if (imgs != descriptor_image_sets.end()) {
                for (const auto& [binding, rec] : imgs->second) {
                    if (cand_count >= kDfMaxRw) { ++cand_over; continue; }
                    cands[cand_count++] = Cand{set, binding, rec.image_view, rec.layout, rec.type};
                }
            }
            // R7: the buffer half of the same set.
            const auto bufs = descriptor_sets.find(set);
            if (bufs == descriptor_sets.end()) continue;
            for (const auto& [binding, d] : bufs->second) {
                if (tmp.buf_count >= std::size(tmp.buf)) { ++tmp.buf_over; continue; }
                tmp.buf[tmp.buf_count++] =
                    DfRecord::DfBuf{binding, d.type, d.buffer, d.offset, d.range};
            }
        }
    }
    // R7: sorted, for the same reason the image bindings are - two runs' tables
    // have to be comparable by eye.
    for (uint32_t i = 1; i < tmp.buf_count; ++i) {
        const auto k = tmp.buf[i];
        int32_t j = static_cast<int32_t>(i) - 1;
        while (j >= 0 && tmp.buf[j].binding > k.binding) { tmp.buf[j + 1] = tmp.buf[j]; --j; }
        tmp.buf[j + 1] = k;
    }
    tmp.dyn_count = static_cast<uint32_t>(
        std::min<size_t>(last_graphics_dynamic_offsets.size(), std::size(tmp.dyn)));
    for (uint32_t i = 0; i < tmp.dyn_count; ++i) tmp.dyn[i] = last_graphics_dynamic_offsets[i];
    for (uint32_t i = 1; i < cand_count; ++i) {
        Cand k = cands[i];
        int32_t j = static_cast<int32_t>(i) - 1;
        while (j >= 0 && cands[j].binding > k.binding) { cands[j + 1] = cands[j]; --j; }
        cands[j + 1] = k;
    }
    // 1b. The clone this set currently resolves to, and what it carries per
    //     binding. This is the R2 fix: it reads the clone's CONTENTS, which
    //     survive the per-(set, generation) caching that made R1's verdict
    //     column read NOT-EVALUATED.
    //     R3: gated on the clone that was ACTUALLY BOUND at the preceding
    //     compute bind, not on one merely existing for this source set. That
    //     distinction is what R2's key-error control caught: both calls bind
    //     the same source handles, so "a clone exists" reported OUR-MIRROR on
    //     the real side too, where redirection never runs.
    VkDescriptorSet bound_clone = VK_NULL_HANDLE;
    if (auto bc = df_bound_clone_by_set.find(tmp.set0); bc != df_bound_clone_by_set.end())
        bound_clone = bc->second;
    DfCloneViews clone_views{};
    tmp.clone_found = bound_clone != VK_NULL_HANDLE && bound_clone != tmp.set0 &&
                      tmp.set0 != VK_NULL_HANDLE && df_lookup_clone_views(tmp.set0, clone_views) &&
                      clone_views.clone == bound_clone;
    tmp.clone = tmp.clone_found ? clone_views.clone : bound_clone;
    tmp.clone_generation = tmp.clone_found ? clone_views.generation : 0;
    tmp.mirror_generation_now = mirror_generation.load(std::memory_order_relaxed);
    if (!tmp.clone_found) clone_views = DfCloneViews{};
    // 2. Our mirror for each, and the layout we track it in.
    VkImage mirror_images[kDfMaxRw]{};
    VkExtent2D mirror_extents[kDfMaxRw]{};
    VkImageLayout mirror_haves[kDfMaxRw]{};
    // 2b. And, for the view the CLONE actually carries, which of our mirrors it
    //     is. mirrored_views is keyed by DOOM's SOURCE view, so the effective
    //     (mirror) view needs a reverse scan - bounded by the mirror
    //     population, ~50 entries, under a lock this thread already takes.
    VkImageView effective_views[kDfMaxRw]{};
    VkImage effective_images[kDfMaxRw]{};
    uint8_t bind_classes[kDfMaxRw]{};
    for (uint32_t i = 0; i < cand_count; ++i) {
        bind_classes[i] = kDfBindNoClone;
        if (!tmp.clone_found) continue;
        bool seen = false;
        for (const auto& [b, v] : clone_views.bindings) {
            if (b != cands[i].binding) continue;
            effective_views[i] = v;
            seen = true;
            break;
        }
        bind_classes[i] = seen ? kDfBindUnclassified : kDfBindMissing;
        if (seen && effective_views[i] == cands[i].view) bind_classes[i] = kDfBindDoomOwn;
    }
    {
        std::scoped_lock lock(mirror_mutex);
        for (uint32_t i = 0; i < cand_count; ++i) {
            auto mv = mirrored_views.find(cands[i].view);
            if (mv != mirrored_views.end()) {
                mirror_images[i] = mv->second.image;
                mirror_extents[i] = mv->second.extent;
                mirror_haves[i] = mirror_layout_of(mv->second.image);
            }
            if (bind_classes[i] != kDfBindUnclassified) continue;
            for (const auto& [sv, m] : mirrored_views) {
                (void)sv;
                if (m.view != effective_views[i]) continue;
                effective_images[i] = m.image;
                bind_classes[i] = kDfBindMirror;
                break;
            }
            if (bind_classes[i] == kDfBindMirror) continue;
            for (const auto& [sv, a] : alias_mirror_views) {
                (void)sv;
                if (a.view != effective_views[i]) continue;
                effective_images[i] = a.mirror_image;
                bind_classes[i] = kDfBindMirror;
                break;
            }
        }
    }
    // 3. DOOM's own image behind each view, so the row can be matched to an H1
    //    line by handle rather than by shape (two 3818x2032 f122 targets exist
    //    and run 18 could not tell them apart - handoff trap 1's cousin).
    VkImage doom_images[kDfMaxRw]{};
    VkExtent3D doom_extents[kDfMaxRw]{};
    VkFormat doom_formats[kDfMaxRw]{};
    {
        std::scoped_lock lock(framebuffer_depth_view_mutex);
        for (uint32_t i = 0; i < cand_count; ++i) {
            auto v = image_view_infos.find(cands[i].view);
            if (v == image_view_infos.end()) continue;
            doom_images[i] = v->second.image;
            auto c = created_image_infos.find(v->second.image);
            if (c == created_image_infos.end()) continue;
            doom_extents[i] = c->second.extent;
            doom_formats[i] = c->second.format;
        }
    }
    // 4. The verdict get_redirected_set reached for each binding.
    tmp.set_outcome = df_lookup_set_outcome(tmp.set0);
    for (uint32_t i = 0; i < cand_count; ++i) {
        DfRwBinding& r = tmp.rw[i];
        r.binding = cands[i].binding;
        r.type = cands[i].type;
        r.effective_view = effective_views[i];
        r.effective_image = effective_images[i];
        r.bind_class = bind_classes[i];
        r.doom_view = cands[i].view;
        r.doom_image = doom_images[i];
        r.mirror_image = mirror_images[i];
        r.mirror_extent = mirror_extents[i];
        r.image_extent = doom_extents[i];
        r.image_format = doom_formats[i];
        r.want = cands[i].want;
        r.have = mirror_haves[i];
        DfStorageOutcome o{};
        if (df_lookup_storage_outcome(cands[i].set, cands[i].binding, o)) {
            r.outcome = o.outcome;
            if (o.have != VK_IMAGE_LAYOUT_UNDEFINED) r.have = o.have;
        } else if (tmp.set_outcome == kDfSetRefused) {
            r.outcome = kDfSetRefused;
        } else if (mirror_images[i] == VK_NULL_HANDLE) {
            r.outcome = kDfNoMirror;
        } else {
            r.outcome = kDfUnknown;
        }
    }
    tmp.rw_count = cand_count;
    tmp.rw_over = cand_over;

    // 5. Merge. Deduped by (pass, first compute set) so pass 18's eleven
    //    dispatches collapse into one row carrying dispatches=11 - a count, not
    //    a truncation. Separate table and separate overflow counter per side:
    //    one shared cap lets the frequent side starve the rare one.
    DfRecord* table = doubled ? df_doubled : df_real;
    std::atomic_uint32_t& over = doubled ? df_doubled_over : df_real_over;
    std::scoped_lock lock(df_records_mutex);
    for (uint32_t i = 0; i < kDfMaxRecords; ++i) {
        if (!table[i].used) continue;
        if (table[i].pass != tmp.pass || table[i].set0 != tmp.set0 ||
            table[i].is_draw != tmp.is_draw) continue;
        table[i].dispatches += 1;
        table[i].indirect += tmp.indirect;
        table[i].executed += tmp.executed;
        table[i].suppressed += tmp.suppressed;
        // A row whose members disagree about the predicate would be a merge
        // artefact, so keep the OR and let rw/outcome stand from the first.
        table[i].would_suppress = table[i].would_suppress || tmp.would_suppress;
        if (table[i].gx != tmp.gx || table[i].gy != tmp.gy || table[i].gz != tmp.gz)
            table[i].group_mismatch = true;
        return;
    }
    for (uint32_t i = 0; i < kDfMaxRecords; ++i) {
        if (table[i].used) continue;
        table[i] = tmp;
        return;
    }
    over.fetch_add(1, std::memory_order_relaxed);
}

// R6: the SAME manifest, for the two GRAPHICS passes that write the saturated
// depth-downsample mirrors.
//
// R5 killed both halves of the clear hypothesis at once: DrawParity reports
// passes 15 and 16 as SAME (`DOUBLED draws=1 idx=3` against `REAL draws=1
// idx=3`), so the draws do run, and the clear value is
// `colorF=(0,0,0,1)` - BLACK, not 1.0, so a cleared-and-never-drawn R16_SFLOAT
// target would read 0.0 and not the 1.0 that is measured. The draw executes and
// computes 1.0 everywhere from a depth input measured at 100.6%.
//
// What a draw is GIVEN has never been printed. The dispatch manifest already
// answers exactly that question for compute; this points it at the two passes
// in question. Scoped to passes 15 and 16 by construction - not to a job, not
// to "graphics" - so it cannot cost more than four rows a frame.
void dispatch_fate_record_draw() {
    if (!df_armed.load(std::memory_order_relaxed)) return;
    if (!inside_frame_root_double && !inside_real_frame_root) return;
    const uint32_t pass = inside_frame_root_double ? doubled_pass_seq : real_pass_seq;
    if (pass != 15 && pass != 16) return;
    std::vector<VkDescriptorSet> gsets;
    gsets.reserve(last_bound_graphics_sets.size());
    for (const auto& [idx, s] : last_bound_graphics_sets) {
        (void)idx;
        if (s != VK_NULL_HANDLE) gsets.push_back(s);
    }
    if (gsets.empty()) return;
    // No SSBO predicate applies to a draw; the fields stay zero and the row is
    // labelled by its zero group counts and indirect=0.
    dispatch_fate_record(false, false, DfSsboMatch{}, 0, 0, 0, gsets, true);
}

// ARM-TIME SELF-TEST. An unarmed or mis-wired manifest prints an empty table,
// which reads exactly like "the doubled call issued no dispatches" - the
// failure this project has already paid for three times. So this pushes a
// synthetic SSBO-bearing set and a synthetic image-ONLY set through the REAL
// predicate (dispatch_ssbo_match, the same function the hooks call) and
// round-trips a storage verdict through the REAL recording and lookup helpers
// the manifest reads. Any mismatch refuses the arm.
//
// WHAT IT DOES NOT COVER, stated rather than implied: it does not drive
// get_redirected_set, which allocates a real clone from the descriptor pool.
// The substitution DECISION is therefore not exercised here - only the channel
// that carries its verdict to the manifest. A verdict that is never recorded
// shows up as NOT-EVALUATED in the table, not as a wrong answer.
bool df_selftest(std::string& detail) {
    const auto set_ssbo = reinterpret_cast<VkDescriptorSet>(static_cast<uintptr_t>(0xD15FA7E01ull));
    const auto set_image = reinterpret_cast<VkDescriptorSet>(static_cast<uintptr_t>(0xD15FA7E02ull));
    {
        std::unique_lock lock(swapchains_mutex);
        DescriptorBinding b{};
        b.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b.range = 64;
        descriptor_sets[set_ssbo][3] = b;
        ImageDescriptorBinding img{};
        img.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        img.count = 1;
        img.layout = VK_IMAGE_LAYOUT_GENERAL;
        descriptor_image_sets[set_image][8] = img;
    }
    // last_bound_compute_sets is thread_local and this runs on the scripted
    // test's thread, so the render thread's state cannot be disturbed. Saved
    // and restored anyway.
    const std::vector<VkDescriptorSet> saved = last_bound_compute_sets;
    last_bound_compute_sets.assign(1, set_ssbo);
    const DfSsboMatch m_ssbo = dispatch_ssbo_match();
    last_bound_compute_sets.assign(1, set_image);
    const DfSsboMatch m_image = dispatch_ssbo_match();
    last_bound_compute_sets = saved;
    {
        std::unique_lock lock(swapchains_mutex);
        descriptor_sets.erase(set_ssbo);
        descriptor_image_sets.erase(set_image);
    }
    df_record_storage_outcome(set_image, 8, kDfRefusedLayout, VK_IMAGE_LAYOUT_GENERAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_NULL_HANDLE);
    DfStorageOutcome back{};
    const bool round_trip = df_lookup_storage_outcome(set_image, 8, back) &&
                            back.outcome == kDfRefusedLayout &&
                            back.have == VK_IMAGE_LAYOUT_GENERAL &&
                            back.want == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    {
        std::scoped_lock lock(df_outcome_mutex);
        df_storage_outcomes.erase(std::make_pair(set_image, 8u));
    }
    // R2: the clone-contents channel is now the load-bearing one, so it gets a
    // round-trip of its own, including the miss case - a lookup that cannot
    // distinguish "no clone" from "clone with no entry for this binding" would
    // print NO-CLONE over a real substitution.
    const auto fake_clone = reinterpret_cast<VkDescriptorSet>(static_cast<uintptr_t>(0xD15FA7E03ull));
    const auto fake_view = reinterpret_cast<VkImageView>(static_cast<uintptr_t>(0xD15FA7E04ull));
    df_record_clone_views(set_image, fake_clone, 4242, {{8u, fake_view}});
    DfCloneViews cv{};
    const bool clone_hit = df_lookup_clone_views(set_image, cv) && cv.clone == fake_clone &&
                           cv.generation == 4242 && cv.bindings.size() == 1 &&
                           cv.bindings[0].first == 8u && cv.bindings[0].second == fake_view;
    DfCloneViews cv_miss{};
    const bool clone_miss = !df_lookup_clone_views(set_ssbo, cv_miss);
    {
        std::scoped_lock lock(df_clone_mutex);
        df_clone_effective.erase(set_image);
    }
    const bool ok = m_ssbo.found && m_ssbo.matches == 1 && m_ssbo.binding == 3 &&
                    !m_image.found && m_image.matches == 0 && round_trip &&
                    clone_hit && clone_miss;
    detail = std::format(
        "ssboSet->found={} matches={} binding={} (want found=1 matches=1 binding=3); "
        "imageOnlySet->found={} matches={} (want found=0 matches=0); verdictRoundTrip={}; "
        "cloneViewsRoundTrip={}; cloneViewsMissIsAMiss={}",
        m_ssbo.found, m_ssbo.matches, m_ssbo.binding, m_image.found, m_image.matches, round_trip,
        clone_hit, clone_miss);
    return ok;
}

void df_emit_manifest(const char* why) {
    if (df_emitted.exchange(true, std::memory_order_relaxed)) return;
    DfRecord doubled_copy[kDfMaxRecords];
    DfRecord real_copy[kDfMaxRecords];
    {
        std::scoped_lock lock(df_records_mutex);
        for (uint32_t i = 0; i < kDfMaxRecords; ++i) {
            doubled_copy[i] = df_doubled[i];
            real_copy[i] = df_real[i];
        }
    }
    uint32_t doubled_rows = 0, real_rows = 0, doubled_dispatches = 0, real_dispatches = 0;
    uint32_t doubled_executed = 0, doubled_suppressed = 0, would_suppress_dispatches = 0;
    uint32_t rw_substituted = 0, rw_refused = 0, rw_nomirror = 0, rw_unknown = 0;
    for (uint32_t i = 0; i < kDfMaxRecords; ++i) {
        if (doubled_copy[i].used) {
            ++doubled_rows;
            doubled_dispatches += doubled_copy[i].dispatches;
            doubled_executed += doubled_copy[i].executed;
            doubled_suppressed += doubled_copy[i].suppressed;
            if (doubled_copy[i].would_suppress)
                would_suppress_dispatches += doubled_copy[i].dispatches;
        }
        if (real_copy[i].used) { ++real_rows; real_dispatches += real_copy[i].dispatches; }
    }
    log::warn(std::format(
        "DispatchFate MANIFEST BEGINS ({}). selftest={} suppressLightCulling={} G1={} D1={} F1={} "
        "froxelFix={} | storageMirrors created={} substitutions={} layoutRefusals={} noInfo={} | "
        "doubledRows={} doubledDispatches={} realRows={} realDispatches={} "
        "doubledTableOverflow={} realTableOverflow={} verdictsDropped={} frameRootSerial={} "
        "dispatchesFromOtherFramesDropped={}.\n"
        "  READ THE CONTROL LINE BEFORE ANY ROW. A manifest with realDispatches=0 is BLIND, not "
        "a result.\n"
        "  R1 (15:21 run) ESTABLISHED, and this run re-carries as its own control: "
        "wouldSuppress=YES on exactly the eleven pass-18 rows at jobRva 0x19ae9e0 and NO on the "
        "pass-16/pass-22 rows; and the pass-22 dispatch at jobRva 0x1873830 - the lighting "
        "accumulation, two storage outputs - EXECUTES. Both should reproduce; if either flips, "
        "something changed under us and nothing else here may be read.\n"
        "  ⚠ WHAT R2 ADDS AND WHY. R1's per-binding verdict column read NOT-EVALUATED "
        "everywhere: get_redirected_set caches per (set, mirrorGeneration) and returns before its "
        "recording points, so the one fact the run existed to read had to be inferred from a "
        "CLONE line three seconds earlier. BOUND= now reads the clone's own contents instead. "
        "This run also prints EVERY image binding, inputs included - pass 22 binds eight, of "
        "which the census can classify seven, and the eighth (b2) resolves to a D24S8 depth "
        "mirror through the alias path while SSDO is depth-driven - and the workgroup counts, "
        "which no instrument has ever logged.",
        why, df_selftest_passed.load(std::memory_order_relaxed) ? "PASS" : "FAIL",
        suppress_doubled_light_culling.load(std::memory_order_relaxed) ? "ON" : "OFF",
        g1_suppress_particle_sim.load(std::memory_order_relaxed) ? "ON" : "OFF",
        d1_enabled.load(std::memory_order_relaxed) ? "ON" : "OFF",
        f1_enabled.load(std::memory_order_relaxed) ? "ON" : "OFF",
        kFixDoubledLightingVolume ? "ON" : "OFF",
        storage_mirror_created.load(std::memory_order_relaxed),
        storage_mirror_substitutions.load(std::memory_order_relaxed),
        storage_mirror_layout_refusals.load(std::memory_order_relaxed),
        storage_mirror_no_info.load(std::memory_order_relaxed),
        doubled_rows, doubled_dispatches, real_rows, real_dispatches,
        df_doubled_over.load(std::memory_order_relaxed),
        df_real_over.load(std::memory_order_relaxed),
        df_outcomes_dropped.load(std::memory_order_relaxed),
        df_frame_latched.load(std::memory_order_relaxed),
        df_frames_dropped.load(std::memory_order_relaxed)));

    for (int side = 0; side < 2; ++side) {
        const DfRecord* table = side == 0 ? doubled_copy : real_copy;
        const char* label = side == 0 ? "DOUBLED" : "REAL";
        for (uint32_t i = 0; i < kDfMaxRecords; ++i) {
            const DfRecord& r = table[i];
            if (!r.used) continue;
            log::warn(std::format(
                "DispatchFate {} {} pass={} jobRva={:#x} set={} setsBound={} dispatches={} "
                "indirect={} EXECUTED={} SUPPRESSED={} wouldSuppress={} groups={}x{}x{}{} "
                "ssboMatches={} ssboBinding={} ssboBuffer={} clone={} cloneGen={} mirrorGenNow={} "
                "setVerdict={} imageBindings={}{}",
                label, r.is_draw ? "DRAW " : "COMPUTE", r.pass, r.job_rva,
                reinterpret_cast<void*>(r.set0), r.sets_bound,
                r.dispatches, r.indirect, r.executed, r.suppressed,
                r.would_suppress ? "YES" : "NO", r.gx, r.gy, r.gz,
                r.group_mismatch ? " ⚠MERGED-ROWS-DISAGREED" : "",
                r.ssbo_matches, r.ssbo_binding, reinterpret_cast<void*>(r.ssbo_buffer),
                r.clone_found ? std::format("{}", reinterpret_cast<void*>(r.clone))
                              : std::string("NONE"),
                r.clone_generation, r.mirror_generation_now,
                df_subst_name(r.set_outcome), r.rw_count,
                r.rw_over ? std::format(" (+{} over the {}-slot cap)", r.rw_over, kDfMaxRw)
                          : std::string()));
            if (r.buf_count || r.dyn_count) {
                std::string bl;
                for (uint32_t k = 0; k < r.buf_count; ++k) {
                    bl += std::format(" [b{} type={} buffer={} offset={} range={}]",
                        r.buf[k].binding, static_cast<int>(r.buf[k].type),
                        reinterpret_cast<void*>(r.buf[k].buffer),
                        static_cast<unsigned long long>(r.buf[k].offset),
                        static_cast<unsigned long long>(r.buf[k].range));
                }
                std::string dl;
                for (uint32_t k = 0; k < r.dyn_count; ++k)
                    dl += std::format(" {}", r.dyn[k]);
                log::warn(std::format(
                    "DispatchFate {}   pass={} BUFFERS n={}{}{} | dynamicOffsets n={}:{}. "
                    "DOOM ring-allocates a constant slot per call and the redirect passes "
                    "dynamic offsets through UNTOUCHED, so a DOUBLED row whose offsets differ "
                    "from the REAL row of the same pass is reading a DIFFERENT slot - which is "
                    "correct by design, and makes the CONTENTS of that slot the next question, "
                    "not the binding.",
                    label, r.pass, r.buf_count,
                    r.buf_over ? std::format(" (+{} over cap)", r.buf_over) : std::string(),
                    bl, r.dyn_count, dl.empty() ? std::string(" none") : dl));
            }
            for (uint32_t k = 0; k < r.rw_count; ++k) {
                const DfRwBinding& w = r.rw[k];
                const bool is_output = w.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                if (side == 0 && is_output) {
                    if (w.bind_class == kDfBindMirror) ++rw_substituted;
                    else if (w.outcome == kDfRefusedLayout) ++rw_refused;
                    else if (w.bind_class == kDfBindDoomOwn || w.outcome == kDfNoMirror)
                        ++rw_nomirror;
                    else ++rw_unknown;
                }
                log::warn(std::format(
                    "DispatchFate {}   pass={} b{} {} type={} doomView={} doomImage={} {}x{} "
                    "fmt={} | BOUND={} effectiveView={} effectiveImage={} | mirrorWeHold={} {}x{} "
                    "verdict={} have={} want={}. Match doomImage/effectiveImage/mirrorWeHold "
                    "against the H1 lines of THIS SAME FRAME (they print src=/mir= handles) to "
                    "name the target; do not match by shape, two 3818x2032 f122 targets exist.",
                    label, r.pass, w.binding, is_output ? "OUTPUT" : "input",
                    static_cast<int>(w.type), reinterpret_cast<void*>(w.doom_view),
                    reinterpret_cast<void*>(w.doom_image), w.image_extent.width,
                    w.image_extent.height, static_cast<int>(w.image_format),
                    df_bind_class_name(w.bind_class),
                    reinterpret_cast<void*>(w.effective_view),
                    reinterpret_cast<void*>(w.effective_image),
                    reinterpret_cast<void*>(w.mirror_image), w.mirror_extent.width,
                    w.mirror_extent.height, df_subst_name(w.outcome),
                    static_cast<int>(w.have), static_cast<int>(w.want)));
            }
        }
    }
    // CONTROL, aimed at this probe's own confusable failure mode: an empty or
    // near-empty table reads exactly like "the doubled call does no compute".
    // The capture says the real call runs the twins of eids 6486/6607, so the
    // REAL side must show dispatches at these three jobs or the instrument is
    // blind and no row above may be read.
    bool saw_prepare = false, saw_particle = false, saw_deferred = false;
    uint32_t real_bindings = 0, real_mirror_bound = 0;
    for (uint32_t i = 0; i < kDfMaxRecords; ++i) {
        if (!real_copy[i].used) continue;
        if (real_copy[i].job_rva == 0x18733c0) saw_prepare = true;
        if (real_copy[i].job_rva == kP1ParticleSimJobRva) saw_particle = true;
        if (real_copy[i].job_rva == kP1DeferredPassesJobRva) saw_deferred = true;
        for (uint32_t k = 0; k < real_copy[i].rw_count; ++k) {
            ++real_bindings;
            if (real_copy[i].rw[k].bind_class == kDfBindMirror) ++real_mirror_bound;
        }
    }
    log::warn(std::format(
        "DispatchFate CONTROL: realDispatches={} realRows={} sawPrepare(0x18733c0)={} "
        "sawParticleSim({:#x})={} sawDeferredPasses({:#x})={} | realImageBindings={} "
        "realBoundToOurMirror={}.\n"
        "  BLINDNESS CHECK: the capture's working arm runs compute at all three jobs (eids 6486 "
        "and 6607 are vkCmdDispatch), so all three FALSE - or realDispatches=0 - means this "
        "manifest never saw the real call. Read no DOUBLED row in that state.\n"
        "  KEY-ERROR CHECK, aimed at THIS probe's own confusable failure mode: redirection runs "
        "only inside the doubled call, so the REAL call is NEVER given a clone. "
        "realBoundToOurMirror MUST be 0. Anything else means the clone lookup is keyed wrong and "
        "the DOUBLED side's OUR-MIRROR verdicts are the same error wearing a good answer's face.",
        real_dispatches, real_rows, saw_prepare, kP1ParticleSimJobRva, saw_particle,
        kP1DeferredPassesJobRva, saw_deferred, real_bindings, real_mirror_bound));
    log::warn(std::format(
        "DispatchFate COMPLETE. doubled: dispatches={} executed={} suppressed={} "
        "wouldBeSuppressedIfArmA={} | storage OUTPUT verdicts on the doubled side: "
        "SUBSTITUTED={} REFUSED-LAYOUT={} NO-MIRROR={} NOT-EVALUATED={}.\n"
        "  HOW TO READ IT, and note the tally now comes from BOUND= (what the clone carries), "
        "not from verdict= (what get_redirected_set decided). R1's verdict column read "
        "NOT-EVALUATED on every binding because the clone was cached from an earlier frame and "
        "the decision points were never re-entered; BOUND= is read out of the clone itself and "
        "does not care when it was built.\n"
        "  OUR-MIRROR on an OUTPUT means eye 1's copy receives that compute write - so if the "
        "target is still dim in H1 the fault is in what the shader was GIVEN, or in the grid it "
        "was dispatched over, not in where it wrote. DOOM-OWN or REFUSED-LAYOUT means the write "
        "went into DOOM's image: eye 1 never gets it AND eye 0 is written twice per frame.\n"
        "  THEN READ THE INPUT ROWS. R1 left one question: the pass-22 dispatch runs, both its "
        "outputs are ours, and uavSSDO still lands at 20.1% of DOOM's with its content confined "
        "to two of eight row bands and its peak 15x lower. An input row showing DOOM-OWN where "
        "the twin real row also shows DOOM-OWN is shared-on-purpose; an input that is ours but "
        "empty is the upstream fault; and groups= differing between the DOUBLED and REAL rows of "
        "the same pass would explain a partially-written target on its own.",
        doubled_dispatches, doubled_executed, doubled_suppressed, would_suppress_dispatches,
        rw_substituted, rw_refused, rw_nomirror, rw_unknown));
}

// R1: the match is computed only when someone needs it - the suppression path
// needs it inside the doubled call with the flag on, the manifest needs it on
// both sides while armed (one real call and one doubled call). Disarmed, this
// is two relaxed loads and the cost profile is exactly what it was before the
// split.
inline bool df_capture_wanted() {
    return df_armed.load(std::memory_order_relaxed) &&
           (inside_frame_root_double || inside_real_frame_root);
}

// FOG REPAIR, AT THE DISPATCH (2026-08-23). Sixth attempt, and the first that
// is not bolted onto the constant-repair subsystem.
//
// Five separate gates keyed on kRepairDoubledConstants blocked the bind-time
// version in turn - graphics-only bind point, the flag missing from the guard,
// the pass whitelist, a per-process dedupe, and a per-frame reset that never
// ran. Each was found one headset run at a time. Then the walk finally ran
// properly, examined 13 000 blocks in two windows, and still never matched:
// the sets it reaches are not the ones this dispatch uses.
//
// A dispatch knows exactly what IT has bound. That sidesteps the dedupe, the
// discovery pass, the per-frame cap, the bind-point filter and the reset by
// construction, and it scans a handful of sets rather than thousands of binds.
//
// Read-only until the signature matches. Logs what it sees either way, so a
// non-match names the values that failed instead of leaving another counter at
// zero.
void fog_repair_at_dispatch(uint32_t x, uint32_t y, uint32_t z) {
    if (!kFogRepairOnly.load(std::memory_order_relaxed)) return;
    if (!inside_frame_root_double) return;               // eye 1 only, deliberately
    if (last_bound_compute_sets.empty()) return;
    std::unique_lock lock(swapchains_mutex);
    uint32_t dyn_index = 0;
    for (size_t si = 0; si < last_bound_compute_sets.size(); ++si) {
        const auto it = descriptor_sets.find(last_bound_compute_sets[si]);
        if (it == descriptor_sets.end()) continue;
        std::vector<uint32_t> bindings;
        for (const auto& [b, d] : it->second) bindings.push_back(b);
        std::sort(bindings.begin(), bindings.end());
        for (uint32_t binding : bindings) {
            const auto& d = it->second.at(binding);
            const bool is_dynamic = d.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
                                    d.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
            uint32_t dyn = 0;
            if (is_dynamic) {
                if (dyn_index < last_bound_compute_dynoffsets.size())
                    dyn = last_bound_compute_dynoffsets[dyn_index];
                ++dyn_index;   // consumed in binding order whether or not we use it
            }
            if (d.type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
                d.type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) continue;
            const auto fb = buffer_bindings.find(d.buffer);
            if (fb == buffer_bindings.end()) continue;
            const auto fm = memory_mappings.find(fb->second.memory);
            if (fm == memory_mappings.end()) continue;
            const VkDeviceSize absolute = fb->second.offset + d.offset + dyn;
            if (absolute < fm->second.offset) continue;
            const VkDeviceSize host_offset = absolute - fm->second.offset;
            if (fm->second.size != VK_WHOLE_SIZE && host_offset + 256 > fm->second.size) continue;
            unsigned char* fb_ptr = fm->second.host + host_offset;
            const uint32_t ex = fog_blocks_examined.fetch_add(1, std::memory_order_relaxed);
            if (ex < 30) {
                float w[4]{}, head[4]{}, ow = 0.0f;
                std::memcpy(head, fb_ptr, sizeof(head));
                std::memcpy(w, fb_ptr + 64, sizeof(w));
                std::memcpy(&ow, fb_ptr + 108, sizeof(ow));
                log::warn(std::format(
                    "FogDispatch #{}: dim=({},{},{}) set={} binding={} off={} | head=({:.4f} "
                    "{:.4f} {:.4f} {:.4f}) +64=({:.5f} {:.5f} {:.5f} {:.5f}) +108={:.4f}",
                    ex, x, y, z, last_bound_compute_first_set + static_cast<uint32_t>(si),
                    binding, static_cast<uint64_t>(d.offset + dyn),
                    head[0], head[1], head[2], head[3], w[0], w[1], w[2], w[3], ow));
            }
            if (!looks_like_fog_block(fb_ptr, 256)) continue;
            fog_patch_blocks_seen.fetch_add(1, std::memory_order_relaxed);
            if (!lever_a_world_delta_doubled) {
                fog_patch_no_delta.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            float wpg_w[4], before_origin[3];
            std::memcpy(wpg_w, fb_ptr + 64, sizeof(wpg_w));
            std::memcpy(before_origin, fb_ptr + 96, sizeof(before_origin));
            for (int row = 0; row < 3; ++row) {
                float r4[4];
                std::memcpy(r4, fb_ptr + 16 + row * 16, sizeof(r4));
                for (int c = 0; c < 4; ++c) r4[c] += lever_a_world_delta[row] * wpg_w[c];
                std::memcpy(fb_ptr + 16 + row * 16, r4, sizeof(r4));
            }
            float origin[3];
            std::memcpy(origin, fb_ptr + 96, sizeof(origin));
            for (int c = 0; c < 3; ++c) origin[c] += lever_a_world_delta[c];
            std::memcpy(fb_ptr + 96, origin, sizeof(origin));
            const uint32_t n = fog_patch_applied.fetch_add(1, std::memory_order_relaxed);
            if (n < 6) {
                log::warn(std::format(
                    "FogRepair #{}: dim=({},{},{}) set={} binding={} patched at dispatch. "
                    "worldDelta=({:.3f} {:.3f} {:.3f}) | globalvieworigin ({:.2f} {:.2f} {:.2f})"
                    " -> ({:.2f} {:.2f} {:.2f}). EYE 1 ONLY.",
                    n, x, y, z, last_bound_compute_first_set + static_cast<uint32_t>(si),
                    binding, lever_a_world_delta[0], lever_a_world_delta[1],
                    lever_a_world_delta[2], before_origin[0], before_origin[1],
                    before_origin[2], origin[0], origin[1], origin[2]));
            }
        }
    }
}

void VKAPI_PTR hook_cmd_dispatch(VkCommandBuffer command_buffer,
    uint32_t x, uint32_t y, uint32_t z) {
    kharvox::native::noteCommand(command_buffer);
    kharvox::native::checkFinalCommand(command_buffer,"hook_cmd_dispatch");
    FbHook fb_(FB_H_CMD_DISPATCH);
    fog_repair_at_dispatch(x, y, z);
    DrawParitySlot* p = draw_parity_slot();
    if (p) ++p->dispatches;
    probe_dispatch_storage_images("dispatch");
    const bool capture = df_capture_wanted();
    DfSsboMatch match{};
    if (capture || (inside_frame_root_double &&
                    suppress_doubled_light_culling.load(std::memory_order_relaxed)))
        match = dispatch_ssbo_match();
    const bool suppressed = doubled_dispatch_writes_shared_buffer(match);
    if (capture) dispatch_fate_record(false, suppressed, match, x, y, z, last_bound_compute_sets);
    if (suppressed) {
        if (p) ++p->dispatches_suppressed;
        return;
    }
    real_cmd_dispatch(command_buffer, x, y, z);
}

void VKAPI_PTR hook_cmd_dispatch_indirect(VkCommandBuffer command_buffer,
    VkBuffer buffer, VkDeviceSize offset) {
    kharvox::native::noteCommand(command_buffer);
    kharvox::native::checkFinalCommand(command_buffer,"hook_cmd_dispatch_indirect");
    FbHook fb_(FB_H_CMD_DISPATCH_INDIRECT);
    DrawParitySlot* p = draw_parity_slot();
    if (p) ++p->dispatches;
    probe_dispatch_storage_images("dispatchIndirect");
    const bool capture = df_capture_wanted();
    DfSsboMatch match{};
    if (capture || (inside_frame_root_double &&
                    suppress_doubled_light_culling.load(std::memory_order_relaxed)))
        match = dispatch_ssbo_match();
    const bool suppressed = doubled_dispatch_writes_shared_buffer(match);
    // Indirect: the grid lives in a device buffer, so 0x0x0 here means
    // "unknown", not "empty". indirect=1 on the row says which it is.
    if (capture) dispatch_fate_record(true, suppressed, match, 0, 0, 0, last_bound_compute_sets);
    if (suppressed) {
        if (p) ++p->dispatches_suppressed;
        return;
    }
    real_cmd_dispatch_indirect(command_buffer, kharvox::native::bufferForEye(buffer), offset);
}


// Desk-1 (2026-08-29): the dynamic-binding slot array in the descriptor walk
// below is fixed-size so it costs no allocation inside swapchains_mutex. A set
// with more dynamic bindings than this spills to a heap vector, which is the
// OLD behaviour exactly - and says so, so "it never overflowed" stays a
// measurement rather than an assumption.
constexpr uint32_t kMaxDynamicBindingsPerSet = 32;
std::atomic_uint32_t g_bind_slot_spills{0};

void VKAPI_PTR hook_cmd_bind_descriptor_sets(VkCommandBuffer command_buffer,
    VkPipelineBindPoint bind_point, VkPipelineLayout layout, uint32_t first_set,
    uint32_t set_count, const VkDescriptorSet* sets, uint32_t dynamic_offset_count,
    const uint32_t* dynamic_offsets) {
    kharvox::native::noteCommand(command_buffer);
    FbHook fb_(FB_H_CMD_BIND_DESCRIPTOR_SETS);
    // =====================================================================
    // DESK-1 (2026-08-29) - THE ONE CHEAP GUARD, EVALUATED BEFORE ANY LOCK,
    // ANY MAP LOOKUP AND ANY ALLOCATION.
    //
    // ⚠ THIS IS A GUARD, NEVER A RETURN. An early `return` anywhere in this
    // function silently drops every descriptor bind that reaches it - the
    // file's standing warning, and the reason the two pre-emptions further
    // down each call real_cmd_bind_descriptor_sets before returning.
    //
    // WHY. FrameBudget measured 14.66 ms real / 12.40 ms doubled at 1730
    // calls/frame, ~8.5 us/call, in a frame whose GPUWAIT was 0.07 ms. The
    // per-call cost that is paid REGARDLESS of what is armed is what this
    // block exists to price once and skip:
    //
    //   * the X1 ring-slot RECORD below - a std::map node allocation plus a
    //     std::vector allocation on every call with dynamic offsets, while
    //     kX1ReplayRingSlots is a compile-time `false`, so nothing has ever
    //     read what it records;
    //   * the MatrixSnapshot vector - grown and freed per call, and consumed
    //     only by the F8 camera-capture tail;
    //   * the per-descriptor-set `bindings` vector inside the walk - a heap
    //     allocation per set, taken while swapchains_mutex is held.
    //
    // CORRECTION TO THE HANDOFF (§4). Its "11 scoped_lock acquisitions,
    // several re-taking swapchains_mutex" is a STATIC count of sites, not a
    // per-call count. In the shipped configuration (headlook=on, gunfix2,
    // fhcensus=on, fwdfix=on, everything else off) a GRAPHICS bind takes
    // swapchains_mutex exactly ONCE - the walk at the bottom. The one path
    // that takes it twice is a COMPUTE bind inside the doubled call with
    // storage mirroring on, and those two scopes are deliberately sequential
    // rather than nested: merging them would hold swapchains_mutex across
    // ensure_mirror_storage_image, which takes mirror_mutex and
    // framebuffer_depth_view_mutex underneath it. Three locks on the render
    // thread is what wedged run 17 (Rule 9). That pair is left alone on
    // purpose; no new acquisition is added anywhere.
    //
    // Every flag here is a relaxed atomic load or a cached config bool. The
    // config accessors are hoisted because wrap::config() is a function-local
    // static and each call pays its initialisation guard - they were being
    // re-read several times per descriptor record inside the locked walk.
    const bool x1_replay_on = x1_replay_ring_offsets.load(std::memory_order_relaxed);
    const bool camera_capture_on = camera_capture_active.load(std::memory_order_relaxed);
    const bool cfg_armed    = wrap::armed();
    const bool cfg_fhcensus = wrap::config_fhcensus();
    // ReadRedirect is the only producer of a non-null clone, so it is also the
    // only thing that can make the per-set "no clone" default write observable.
    const bool redirect_bookkeeping_wanted =
        read_redirect_enabled.load(std::memory_order_relaxed);
    // ---------------------------------------------------------------------
    // X1 - RING-SLOT REPLAY (2026-08-27). THE FIRST CAUSAL LEVER IN THIS
    // THREAD, and it is aimed at the one difference left standing.
    //
    // Seven measurement runs have now closed, each with its own control:
    // execution (13 dispatches, all EXECUTED), targets (every mirror written),
    // resources (every binding of the failing passes BOUND=OUR-MIRROR, depth
    // input measured at 100.6%), layouts (have=want everywhere, zero
    // refusals), workgroup grids (identical), draw parity (SAME from pass 11
    // on), suppression (never touched the lighting dispatch). DOOM never calls
    // vkCmdPushConstants. So for the depth-downsample draws that write a
    // uniform 1.0 where DOOM writes 0..0.23, exactly ONE input differs between
    // the two calls, and R7 printed it:
    //
    //   DOUBLED pass=15  buffer=0x…dd0fd0  dynamicOffsets 1827328 1827584
    //   REAL    pass=15  buffer=0x…dd0fd0  dynamicOffsets 1577984 1578240
    //
    // Same buffer, same ranges, DIFFERENT SLOT. DOOM ring-allocates a constant
    // slot per call, and this project's own 2026-08-09 TrueStereoGpuProbe work
    // already established that those GPU slots are NOT reached by CPU-side
    // render-view edits. Nothing we do fills the doubled call's slot; DOOM
    // fills it only if its own upload path re-runs for our re-invocation.
    //
    // THE LEVER: point the doubled call's binds at the REAL call's slots for
    // the same pass. Memory-safe by construction - same buffer handle, same
    // ranges, and offsets the real call itself just used - and reversible.
    //
    // EXPECTED COST IF IT WORKS: the real slot holds the REAL camera, so eye 1
    // becomes a graded copy of eye 0's VIEWPOINT and parallax is lost. That is
    // an acceptable, informative outcome and the project has judged it before:
    // effects returning with parallax destroyed localises the fault to the
    // slot contents, and the remaining work becomes "fill our own slot" rather
    // than "find the fault".
    //
    // POSITIVE CONTROL: x1_replayed must be non-zero. Zero means the replay
    // never fired and the run says nothing either way.
    //
    // ⚠ GUARDED ON x1_replay_on (Desk-1, 2026-08-29). The RECORD half used to
    // run unconditionally on every call inside the real frame root, paying a
    // std::map node allocation and a std::vector allocation each time. Its
    // ONLY consumer is the replay branch six lines below, and
    // x1_replay_ring_offsets is stored from kX1ReplayRingSlots, which is a
    // compile-time `false` - so the recording has never been read. Behaviour
    // is identical when the flag is off and identical when it is on; nothing
    // outside this block touches x1_real_dyn.
    const uint32_t* dyn_out = dynamic_offsets;
    std::vector<uint32_t> x1_storage;
    if (x1_replay_on && dynamic_offset_count && dynamic_offsets) {
        const auto key = [&](uint32_t pass, uint32_t seq) {
            return (static_cast<uint64_t>(pass) << 40) |
                   (static_cast<uint64_t>(bind_point) << 32) | seq;
        };
        if (inside_real_frame_root) {
            x1_real_dyn[key(real_pass_seq, x1_real_bind_seq++)]
                .assign(dynamic_offsets, dynamic_offsets + dynamic_offset_count);
        } else if (inside_frame_root_double) {
            const uint64_t k = key(doubled_pass_seq, x1_doubled_bind_seq++);
            if (x1_replay_ring_offsets.load(std::memory_order_relaxed)) {
                auto it = x1_real_dyn.find(k);
                if (it == x1_real_dyn.end()) {
                    x1_missing.fetch_add(1, std::memory_order_relaxed);
                } else if (it->second.size() != dynamic_offset_count) {
                    // Never substitute a mismatched count: the driver reads
                    // exactly as many as the layout declares, and a short array
                    // is undefined behaviour, not a diagnostic.
                    x1_count_mismatch.fetch_add(1, std::memory_order_relaxed);
                } else {
                    x1_storage = it->second;
                    dyn_out = x1_storage.data();
                    const uint32_t n = x1_replayed.fetch_add(1, std::memory_order_relaxed);
                    if (n < 6) {
                        log::warn(std::format(
                            "X1 ReplayRingSlot #{} pass={} bindPoint={} n={}: doubled {} {} -> "
                            "real {} {}. The doubled call now reads the REAL call's constant "
                            "slot for this bind.",
                            n, doubled_pass_seq, static_cast<int>(bind_point),
                            dynamic_offset_count, dynamic_offsets[0],
                            dynamic_offset_count > 1 ? dynamic_offsets[1] : 0u,
                            it->second[0], it->second.size() > 1 ? it->second[1] : 0u));
                    }
                }
            }
        }
    }
    // UNIFORM BLOCK DIFF - see capture_uniform_blocks. Graphics binds only:
    // the passes the stage table indicts are graphics, and including compute
    // would collide pass numbering between the two pipelines.
    // ⚠ ONLY WHILE DOUBLING IS ARMED - this is a CRASH FIX, not an
    // optimisation (2026-08-20, two crashes at menu load).
    //
    // repair_seen_binds / repair_real_blocks / uniform_diff_seen_* are plain
    // containers, justified by "recording is single-threaded, measured
    // threadID 13060". That measurement was taken AFTER the scripted test,
    // whose step 1 forces r_useSMP inline. Before PageUp, DOOM renders with
    // r_useSMP=2 - jobs and async, MANY threads - and the pre-filter insert
    // added at 13:20 sits OUTSIDE swapchains_mutex. Concurrent insertion into
    // an unordered_set is exactly the crash the owner saw, 8 s in, before
    // arming anything. My "single-threaded" premise was true only in the
    // window I had measured, and I applied it outside that window.
    //
    // frame_root_double_enabled is set at scripted-test step 2, strictly after
    // step 1 has forced inline. Gating on it makes the single-thread premise
    // actually hold, and costs nothing before arming.
    // VOLUME RESTORE - RECORD FROM THE BIND, NOT THE WRITE (2026-08-22).
    //
    // The first recorder listened only on vkUpdateDescriptorSets while
    // inside_real_frame_root, and came back with vrRecorded=0 against
    // vrDoubledWrites=36 and vrTemplateWrites=0. So the write path IS exercised
    // and is NOT templates - the volume-carrying sets are simply written
    // outside the real frame root, probably once rather than per frame. The
    // restore had nothing to restore from, which is why it read as a null.
    //
    // Binds are the reliable observation: whatever DOOM wrote and whenever, the
    // set it BINDS carries the volume, and both calls bind. So the real call's
    // binds are the record.
    //
    // NOT cleared per frame, deliberately, because the write that populated it
    // may never recur. ⚠ P1 measured that DOOM alternates TWO volumes for
    // temporal reprojection, so a handle held across a swap can be the wrong
    // one of the two - the log prints it whenever it changes so that is visible
    // rather than silent. For a causal test, the wrong-of-two is still a volume
    // and would still produce scattering.
    //
    // Behind the arm flag: this walks bound sets and would otherwise pay a map
    // lookup on a path that runs thousands of times a frame.
    if (volume_restore_enabled.load(std::memory_order_relaxed) &&
        !inside_frame_root_double && sets && set_count) {
        std::unique_lock lock(swapchains_mutex);
        for (uint32_t i = 0; i < set_count; ++i) {
            const auto imgs = descriptor_image_sets.find(sets[i]);
            if (imgs == descriptor_image_sets.end()) continue;
            for (const auto& [b, rec] : imgs->second) {
                if (b >= kVolumeRestoreBindings || rec.image_view == VK_NULL_HANDLE) continue;
                const auto vi = image_view_infos.find(rec.image_view);
                if (vi == image_view_infos.end() ||
                    vi->second.view_type != VK_IMAGE_VIEW_TYPE_3D) continue;
                if (real_current_job_rva == 0) {
                    vr_records_no_rva.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                VolumeJobRecord* slot = volume_job_slot(real_current_job_rva, true);
                if (!slot) continue;
                if (slot->view[b] != rec.image_view) {
                    if (vr_record_lines.fetch_add(1, std::memory_order_relaxed) < 8)
                        log::warn(std::format(
                            "VolumeRecord: job {:#x} binding {} volume view {} (was {}) - what "
                            "the doubled call at THIS job and binding will be given.",
                            real_current_job_rva, b, reinterpret_cast<void*>(rec.image_view),
                            reinterpret_cast<void*>(slot->view[b])));
                    slot->view[b] = rec.image_view;
                }
                vr_volumes_recorded.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    // ⚠ THE FOG REPAIR NEEDS THE **COMPUTE** BIND POINT (2026-08-23).
    //
    // This guard was GRAPHICS-only, and mode 4's target - the deferred
    // composite's freqLow_computeUniforms - is bound to a COMPUTE shader and
    // read by vkCmdDispatch. The repair path could never see it, and two
    // headset runs came back "does absolutely nothing" with fogExamined=0:
    // not a signature mismatch, not a failed patch - never reached. And
    // kFogRepairOnly was missing from the condition below, so it would not
    // have fired on a graphics bind either. Both were mine.
    //
    // Modes 1-3 keep their graphics-only scope. Only the fog repair widens the
    // bind point, and only while its own flag is set.
    const bool fog_repair_on = kFogRepairOnly.load(std::memory_order_relaxed);
    const bool bind_point_ok =
        bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS ||
        (fog_repair_on && bind_point == VK_PIPELINE_BIND_POINT_COMPUTE);
    if (bind_point_ok && sets && set_count &&
        frame_root_double_enabled.load(std::memory_order_relaxed) &&
        (kRepairDoubledConstants || fog_repair_on ||
         uniform_diff_capturing.load(std::memory_order_relaxed))) {
        // Pre-filter BEFORE the lock - see repair_seen_binds. Without this the
        // global mutex is taken ~2 700 times a frame and the game crawls.
        // ⚠ THE KEY MUST MATCH THE INNER DEDUPE, or the filter does nothing
        // (2026-08-20, second slow-motion report).
        //
        // This key used to include sets[0]. The inner dedupe key is
        // (pass, setIndex, binding) - no set HANDLE - so every per-material
        // descriptor set in a 405-draw pass passed this filter, took
        // swapchains_mutex, walked its bindings and copied up to 4 KB, only to
        // be thrown away by the inner check. Thousands of locked walks per
        // frame instead of the ~150 the inner dedupe actually keeps.
        //
        // Dropping the handle makes the two agree: whatever the first bind in
        // this (pass, firstSet, setCount) produces is exactly what the inner
        // dedupe would have kept anyway, so this is the same measurement at a
        // fraction of the cost.
        const bool dbl = inside_frame_root_double;
        // TARGETED (2026-08-20). The repair only ever fires on four blocks -
        // pass 18 bindings 10/11/13 and pass 24 binding 9, measured over five
        // runs - but the walk that finds them ran on every pass and cost the
        // owner four slow-motion runs. Skipping passes that have never carried
        // a repairable block restores the framerate and changes nothing about
        // what gets repaired. Widen this list if the pass numbering moves;
        // the ConstantRepair lines say which passes actually hit.
        // ⚠ NEVER `return` FROM HERE. This sits near the top of
        // hook_cmd_bind_descriptor_sets and the real vkCmdBindDescriptorSets
        // is called at the BOTTOM - an early return would silently drop every
        // descriptor bind in the frame. Guard with a condition, never a return.
        // ⚠ NOT A PASS WHITELIST. The first version of this filter listed
        // passes 18/22/24/27, taken from the runs where the repair had fired.
        // DOOM's pass NUMBERING SHIFTS BETWEEN RUNS - 15/19/20/21/33,
        // 17/21/22/23/28 and 18/22/24/27 have all been observed for the same
        // logical chain - so a pass whitelist silently skips the blocks it is
        // meant to cover. The 14:08 run repaired only pass 22 and none of the
        // others for exactly that reason.
        //
        // The robust criterion is the one that actually matters: does this
        // descriptor set contain a LARGE uniform block at all? Cached per set
        // handle, so it costs one walk the first time a set is ever seen and a
        // hash lookup for every bind after - independent of pass numbering.
        const uint32_t seq = dbl ? doubled_pass_seq : real_pass_seq;
        // Discovery frames walk everything; afterwards only the passes that
        // were actually seen carrying a large block. See the declarations.
        const bool interesting_pass =
            fog_repair_on ||
            !repair_discovery_done.load(std::memory_order_relaxed) ||
            repair_interesting_passes.find(seq) != repair_interesting_passes.end();
        uint64_t k = (static_cast<uint64_t>(seq) << 40) ^
                     (static_cast<uint64_t>(first_set) << 36) ^
                     (static_cast<uint64_t>(set_count) << 32);
        if (dbl) k |= 0x8000000000000000ull;
        if (!interesting_pass) {
            // Not a pass the repair has ever fired on - skip before the lock.
        } else if (repair_seen_binds.size() >= kMaxRepairBindsPerFrame) {
            // Rule 3: a silent cap would read as "nothing needed repairing".
            if (repair_cap_lines.fetch_add(1, std::memory_order_relaxed) < 3)
                log::error(std::format(
                    "ConstantRepair: hit the {} bind/frame cap - later binds this frame are NOT "
                    "covered, so an absent repair below is not evidence of a healthy block.",
                    kMaxRepairBindsPerFrame));
        } else if (repair_seen_binds.insert(k).second) {
            std::unique_lock lock(swapchains_mutex);
            capture_uniform_blocks(first_set, set_count, sets, dynamic_offset_count,
                                   dynamic_offsets, dbl);
        }
    }
    // Remember what compute has bound, so a dispatch can be asked what it is
    // about to write. Cheap and unconditional: a vector assign per compute
    // bind, and DOOM binds compute sets far less often than graphics ones.
    // FINAL PASS BINDINGS (2026-08-18). Why the doubled final pass graded the
    // WRONG eye. It produced a complete frame at eye 0's viewpoint, so its
    // composite read was never substituted, and there are three quite
    // different reasons that could be true - which this separates:
    //
    //   recorded=0            we never saw the set written, so cloning is
    //                         refused wholesale and NOTHING in it can be
    //                         substituted. (Leading candidate: the log shows
    //                         "no recorded contents for this set".)
    //   recorded>0 mirror=0   we can see the set, but the composite view it
    //                         samples is not one we hold a mirror for - the
    //                         redirect has nothing to swap TO.
    //   recorded>0 mirror>0   the mirror exists and the safety gate refused
    //                         it, in which case the gate's reason is the fix.
    //
    // Logged for the REAL invocation as well as the doubled one, because what
    // the pass actually reads is a property of the real one; the doubled one
    // only shows what we managed to change.
    if (sets && set_count && (inside_final_pass_real || inside_final_pass_double) &&
        (inside_final_pass_double ? final_pass_binding_lines_doubled : final_pass_binding_lines_real)
                .load(std::memory_order_relaxed) < kMaxFinalPassBindingLines) {
        for (uint32_t i = 0; i < set_count; ++i) {
            if (sets[i] == VK_NULL_HANDLE) continue;
            uint32_t recorded = 0, mirrored = 0, unknown = 0;
            std::string detail;
            {
                std::scoped_lock mirror_lock(mirror_mutex);
                std::unique_lock lock(swapchains_mutex);
                if (auto it = descriptor_image_sets.find(sets[i]);
                    it != descriptor_image_sets.end()) {
                    for (const auto& [binding, rec] : it->second) {
                        ++recorded;
                        const char* hit = census_classify_view(rec.image_view);
                        if (std::strcmp(hit, "mirror") == 0) ++mirrored; else ++unknown;
                        if (detail.size() < 260)
                            detail += std::format(" [b{} view={} {}]", binding,
                                reinterpret_cast<void*>(rec.image_view), hit);
                    }
                }
            }
            const uint32_t n =
                (inside_final_pass_double ? final_pass_binding_lines_doubled
                                          : final_pass_binding_lines_real)
                    .fetch_add(1, std::memory_order_relaxed);
            log::warn(std::format(
                "FinalPassBindings #{} call={} setIndex={} set={} recordedImageBindings={} "
                "mirrorBacked={} unknown={}{}",
                n, inside_final_pass_double ? "DOUBLED" : "real", first_set + i,
                reinterpret_cast<void*>(sets[i]), recorded, mirrored, unknown, detail));
        }
    }
    // FOG HUNT (2026-08-18). The effects are missing even though DrawParity
    // shows the doubled call runs the SAME draws and dispatches, and the two
    // storage images it writes are now correctly mirrored. So the fog work
    // executes and reads a view-dependent INPUT we do not mirror - which lives
    // in the census's large 'unknown' sampled-image bucket. Volumetric fog
    // uses a froxel volume, a 3D texture; that is the shape to look for.
    //
    // This enumerates every image the doubled call SAMPLES (not writes) and is
    // not mirror-backed, and logs the ones that are 3D or screen-resolution -
    // the froxel/scattering candidates - with full identity. Everything else
    // in that bucket (megatexture pages, small LUTs) is correctly ignored.
    // Armed and bounded; runs on both bind points because fog can be sampled
    // by graphics as well as compute.
    if (sets && set_count && inside_frame_root_double &&
        fog_hunt_armed.load(std::memory_order_relaxed) &&
        fog_hunt_lines.load(std::memory_order_relaxed) < kMaxFogHuntLines) {
        // TWO PHASES, NOT THREE NESTED LOCKS. Phase 1 collects the non-mirror
        // sampled views under mirror+swapchains (the order the census uses);
        // phase 2 resolves their identity under framebuffer_depth_view_mutex
        // ALONE, after the first two are released. Nesting a third lock here
        // is exactly what wedged the render thread in run 17 (Rule 9).
        struct FogCand { uint32_t binding; VkImageView view; };
        FogCand cands[24];
        uint32_t cand_count = 0;
        {
            std::scoped_lock mirror_lock(mirror_mutex);
            std::unique_lock lock(swapchains_mutex);
            for (uint32_t i = 0; i < set_count && cand_count < std::size(cands); ++i) {
                if (sets[i] == VK_NULL_HANDLE) continue;
                auto it = descriptor_image_sets.find(sets[i]);
                if (it == descriptor_image_sets.end()) continue;
                for (const auto& [binding, rec] : it->second) {
                    if (rec.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
                        rec.type != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) continue;
                    if (std::strcmp(census_classify_view(rec.image_view), "mirror") == 0) continue;
                    if (cand_count >= std::size(cands)) break;
                    cands[cand_count++] = FogCand{binding, rec.image_view};
                }
            }
        }
        const uint32_t pass = doubled_pass_seq;
        std::scoped_lock idlock(framebuffer_depth_view_mutex);
        for (uint32_t c = 0; c < cand_count; ++c) {
            VkImage img = VK_NULL_HANDLE;
            if (auto v = image_view_infos.find(cands[c].view); v != image_view_infos.end())
                img = v->second.image;
            if (img == VK_NULL_HANDLE) continue;
            auto ci = created_image_infos.find(img);
            if (ci == created_image_infos.end()) continue;
            const bool is_3d = ci->second.type == VK_IMAGE_TYPE_3D;
            const bool full_screen = ci->second.extent.width >= 1800 &&
                                     ci->second.extent.height >= 900;
            if (!is_3d && !full_screen) continue;  // not a fog/effects-shaped input
            if (fog_hunt_lines.fetch_add(1, std::memory_order_relaxed) >= kMaxFogHuntLines)
                break;
            log::warn(std::format(
                "FogHunt pass={} binding={} view={} image={} type={} extent={}x{}x{} "
                "format={} usage={:#x} - SAMPLED by the doubled call, NOT mirrored. A 3D "
                "texture here is a froxel fog/scattering volume; a full-screen one is a shared "
                "lighting or history buffer that every additive effect reads. Either is DOOM's "
                "real, view-wrong version.",
                pass, cands[c].binding, reinterpret_cast<void*>(cands[c].view),
                reinterpret_cast<void*>(img),
                is_3d ? "3D" : (ci->second.type == VK_IMAGE_TYPE_1D ? "1D" : "2D"),
                ci->second.extent.width, ci->second.extent.height, ci->second.extent.depth,
                static_cast<int>(ci->second.format),
                static_cast<unsigned>(ci->second.usage)));
        }
    }
    // Storage descriptors can be bound to graphics as well as compute. Observe
    // both on the real pass so the owner can seed them at a legal pass boundary.
    kharvox::native::observeStorageBindings(set_count,sets);
    if (bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS && sets) {
        // DeferredInputMap's source. Kept for both calls, unconditionally -
        // gating it on inside_frame_root_double would leave the REAL side of
        // the comparison empty, which is the defect this probe exists to
        // avoid repeating.
        for (uint32_t i = 0; i < set_count; ++i) {
            const uint32_t idx = first_set + i;
            auto it = std::find_if(last_bound_graphics_sets.begin(),
                last_bound_graphics_sets.end(),
                [idx](const auto& e) { return e.first == idx; });
            if (it != last_bound_graphics_sets.end()) it->second = sets[i];
            else last_bound_graphics_sets.emplace_back(idx, sets[i]);
            // R6, same default-then-overwrite as the compute branch.
            // ⚠ Desk-1 (2026-08-29): only while redirection can produce a
            // non-null clone, or while the map already holds one. This is a
            // std::map node write per set per call - 1730 calls a frame - and
            // its single reader treats "absent" and "present but null"
            // identically, so with read_redirect_enabled off and the map empty
            // the write is unobservable. The `.empty()` term keeps the reset
            // semantics exact if redirection is armed and later disarmed.
            if (sets[i] != VK_NULL_HANDLE && (redirect_bookkeeping_wanted ||
                                              !df_bound_clone_by_set.empty()))
                df_bound_clone_by_set[sets[i]] = VK_NULL_HANDLE;
        }
        // R7. Recorded unconditionally on both calls - gating it on the doubled
        // side would leave the comparison one-armed, which is the defect the
        // DeferredInputMap comment two lines up already warns about.
        last_graphics_dynamic_offsets.assign(
            dynamic_offsets, dynamic_offsets + (dynamic_offsets ? dynamic_offset_count : 0));
    }
    if (bind_point == VK_PIPELINE_BIND_POINT_COMPUTE && sets) {
        last_bound_compute_sets.assign(sets, sets + set_count);
        // The dynamic offsets that came with this bind, kept parallel to the
        // set handles (2026-08-23). Without them a later dispatch can find the
        // descriptor but not the BYTE RANGE it actually points at, and DOOM's
        // constants live at large per-allocation offsets into a shared ring -
        // measured at 242176 and 493056 for the two eyes' copies of one block.
        // Also record first_set, because a descriptor's dynamic offsets are
        // consumed in binding order across the sets of THIS bind only.
        last_bound_compute_dynoffsets.assign(
            dynamic_offsets ? dynamic_offsets : nullptr,
            dynamic_offsets ? dynamic_offsets + dynamic_offset_count : nullptr);
        last_bound_compute_first_set = first_set;
        // Default: nothing substituted. The ReadRedirect block below overwrites
        // this when it actually binds clones, and it returns immediately after
        // doing so - so a bind that falls through to the normal path leaves the
        // honest "no clone" here rather than last bind's answer.
        // ⚠ Desk-1 (2026-08-29): see the graphics branch - same gate, same
        // reason (a std::map write per set per call that nothing can observe
        // while redirection is off and the map is empty).
        for (uint32_t i = 0; i < set_count; ++i)
            if (sets[i] != VK_NULL_HANDLE && (redirect_bookkeeping_wanted ||
                                              !df_bound_clone_by_set.empty()))
                df_bound_clone_by_set[sets[i]] = VK_NULL_HANDLE;
        // STORAGE-IMAGE MIRRORING (2026-08-18, Run A / review P3). Create the
        // private image BEFORE the ReadRedirect block below, which is what
        // substitutes it - and above every early return in this function, for
        // the reason the census and the camera rider are also up here: three
        // probes have already died silently below those returns (review
        // §7.4), and a mirror that is never created cannot be substituted.
        //
        // Lazy, on first sight, exactly like framebuffer mirroring: the set of
        // storage images the doubled call writes is small (Run A: two) and
        // stable, so this allocates twice and then does nothing but two map
        // lookups per compute bind forever after.
        if (inside_frame_root_double && mirror_enabled.load(std::memory_order_relaxed) &&
            storage_mirror_enabled.load(std::memory_order_relaxed)) {
            // TWO SEPARATE, SEQUENTIAL LOCK SCOPES, NOT NESTED. The candidate
            // views are collected under swapchains_mutex and that lock is
            // RELEASED before mirror_mutex is taken - because
            // ensure_mirror_storage_image itself takes
            // framebuffer_depth_view_mutex underneath mirror_mutex, and doing
            // this in one nested scope would put three locks on the render
            // thread at once. Run 17 wedged the render thread doing exactly
            // that, which is why Rule 9 names it.
            VkImageView candidates[8]{};
            uint32_t candidate_count = 0;
            {
                std::unique_lock lock(swapchains_mutex);
                for (uint32_t i = 0; i < set_count && candidate_count < std::size(candidates);
                     ++i) {
                    if (sets[i] == VK_NULL_HANDLE) continue;
                    const auto imgs = descriptor_image_sets.find(sets[i]);
                    if (imgs == descriptor_image_sets.end()) continue;
                    for (const auto& [binding, rec] : imgs->second) {
                        (void)binding;
                        if (rec.type != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) continue;
                        if (rec.image_view == VK_NULL_HANDLE) continue;
                        if (candidate_count >= std::size(candidates)) break;
                        candidates[candidate_count++] = rec.image_view;
                    }
                }
            }
            std::vector<VkImageMemoryBarrier> to_general;
            if (candidate_count) {
                std::scoped_lock mirror_lock(mirror_mutex);
                for (uint32_t i = 0; i < candidate_count; ++i) {
                    if (!mirrored_views.count(candidates[i]))
                        ensure_mirror_storage_image(candidates[i]);
                    kharvox::native::seedStorageImage(command_buffer,candidates[i]);
                    // A STORAGE_IMAGE write needs GENERAL, and nothing else
                    // will put the mirror there. The rewind restores it to
                    // DOOM's start-of-frame layout, and DOOM issues no
                    // transition for these two images inside the frame root at
                    // all - which is how they ended up refused with have=5
                    // want=1 even after the rewind. That refusal then cascades:
                    // the mirror is only marked written when the storage
                    // substitution succeeds, so the later SAMPLED reads of the
                    // same image are refused too (the "not-written have=5
                    // want=5" class).
                    //
                    // Legal here specifically because this is a COMPUTE bind:
                    // compute work is outside a render pass, and a layout
                    // transition inside one would be invalid. Hence the
                    // bind-point gate on the enclosing block.
                    auto mv = mirrored_views.find(candidates[i]);
                    if (mv == mirrored_views.end()) continue;
                    const VkImageLayout have = mirror_layout_of(mv->second.image);
                    if (have == VK_IMAGE_LAYOUT_GENERAL) continue;
                    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                    b.oldLayout = have;
                    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
                    b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
                    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    b.image = mv->second.image;
                    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS,
                                          0, VK_REMAINING_ARRAY_LAYERS};
                    append_mirror_transitions(to_general,b);
                }
            }
            if (!to_general.empty() && real_cmd_pipeline_barrier) {
                real_cmd_pipeline_barrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                    static_cast<uint32_t>(to_general.size()), to_general.data());
                const uint32_t n = storage_to_general_barriers.fetch_add(
                    static_cast<uint32_t>(to_general.size()), std::memory_order_relaxed);
                if (n < 6) {
                    log::warn(std::format(
                        "StorageToGeneral: {} storage mirror(s) transitioned to GENERAL before a "
                        "compute bind (first: mirror {} {} -> 1). This is the layout a "
                        "STORAGE_IMAGE write declares; without it the substitution is refused and "
                        "the doubled call keeps writing DOOM's own image.",
                        to_general.size(), reinterpret_cast<void*>(to_general[0].image),
                        static_cast<int>(to_general[0].oldLayout)));
                }
            }
        }
    }
    // ChainWalk camera rider (STEP 6.6 item 3), FIRST - ahead of the
    // ReadRedirect block below, which returns early on every bind it
    // substitutes. Placed further down, the rider would have been skipped for
    // exactly the passes it exists to classify: the lighting and post passes
    // that read mirrors are precisely the ones that get substituted. That is
    // the "probe that cannot execute" failure this project has already paid
    // for twice, and it is invisible in the log because the probe simply
    // reports nothing.
    //
    // Costs one relaxed atomic load when disarmed, which is every
    // configuration except the two A/B attempts of a scripted run.
    if (chainwalk_camera_armed.load(std::memory_order_relaxed)) {
        chainwalk_scan_camera(first_set, set_count, sets, dynamic_offset_count, dynamic_offsets);
    }
    // DESCRIPTOR CENSUS - ALSO ABOVE THE READREDIRECT BLOCK, and for the same
    // reason (2026-08-17, run 24).
    //
    // The census used to sit below it, and the moment the scripted test began
    // enabling redirection BEFORE arming the census, the ReadRedirect block's
    // early return meant every substituted bind skipped the census entirely.
    // Run 24 reported passes=23 passesWithMirrorReads=0 mirror=0 against
    // 59,175 substituted binds - which reads as "this scene has no
    // mirror-backed reads at all" and is nothing of the sort. It is the same
    // defect that made the camera rider blind, in the same function, and I
    // had already fixed it once here without checking who else was downstream
    // of that return.
    //
    // Classifying DOOM's ORIGINAL sets, before any substitution, is also what
    // the census is FOR: it answers "what does this pass read", not "what did
    // we leave unsubstituted". Its numbers are now independent of whether
    // redirection happens to be on.
    if (descriptor_census_enabled.load(std::memory_order_relaxed) &&
        (inside_frame_root_double
             ? descriptor_census_lines.load(std::memory_order_relaxed) <
                   kMaxDescriptorCensusLines
             : descriptor_census_real_lines.load(std::memory_order_relaxed) <
                   kMaxDescriptorCensusRealLines)) {
        run_descriptor_census(command_buffer, first_set, set_count, sets,
            dynamic_offset_count, dynamic_offsets);
    }
    // THE FROXEL FIX, half 1: record the volume the REAL call binds at the
    // lighting job this frame. Runs only inside the real call, only at that
    // job's RVA, and only for one binding index, so it costs two thread_local
    // comparisons on every other bind in the frame.
    //
    // Two sequential lock scopes, never nested: descriptor_image_sets is under
    // swapchains_mutex and image_view_infos under framebuffer_depth_view_mutex,
    // and stacking a second lock on the render thread inside the bind hook is
    // what the census warns wedged run 17.
    if (kFixDoubledLightingVolume && inside_real_frame_root && sets && set_count &&
        real_current_job_rva == kP1ParticleSimJobRva) {
        VkImageView candidate = VK_NULL_HANDLE;
        {
            std::unique_lock lock(swapchains_mutex);
            for (uint32_t i = 0; i < set_count && candidate == VK_NULL_HANDLE; ++i) {
                const auto imgs = descriptor_image_sets.find(sets[i]);
                if (imgs == descriptor_image_sets.end()) continue;
                const auto b = imgs->second.find(kLightingVolumeBinding);
                if (b != imgs->second.end()) candidate = b->second.image_view;
            }
        }
        if (candidate != VK_NULL_HANDLE) {
            bool is_volume = false;
            {
                std::scoped_lock lock(framebuffer_depth_view_mutex);
                const auto vi = image_view_infos.find(candidate);
                is_volume = vi != image_view_infos.end() &&
                            vi->second.view_type == VK_IMAGE_VIEW_TYPE_3D;
            }
            // Only a 3D view is accepted. The whole point is that the doubled
            // call's binding 2 is NOT one, and copying a non-volume into it
            // would substitute nothing while reporting that it had.
            if (is_volume) real_lighting_volume_view = candidate;
        }
    }
    // P1 ASKED phase - what this pass's job handed us, before redirection
    // touches anything. Sits here, alongside the census and ahead of the
    // ReadRedirect block, because every bind reaches this point: the two
    // pre-emption early-returns further down are both BELOW the redirect
    // block, and the redirect block's own return is handled by the BOUND
    // record inside it. Gated only on p1_active, never on the census flag -
    // sharing a gate with another instrument is how FogHunt's budget was
    // silently spent by pass 12.
    p1_record(set_count, sets, false);
    // ReadRedirect (STEP 6 PILOT). Before anything else, and deliberately
    // ahead of every diagnostic early-return below: those returns exist to
    // skip LOGGING work, and a behavioural substitution must not inherit
    // their conditions.
    // Applies to the WHOLE doubled call, not one job. The pilot restricted
    // this to a single jobRva; with the filter off that restriction is
    // meaningless - every pass in the chain re-reads the previous target, and
    // the census says the classification is unambiguous everywhere
    // (biggestArray=1, noRecord=0). Any set with no mirror-backed binding is
    // cached as needing nothing and costs one lookup.
    // mirrors_converged() as well as the enable flag: the flag says the owner
    // wants redirection, the condition says the mirror set is currently in a
    // state where substituting is defined. A level transition rebuilds
    // mirrors mid-session with no keypress involved, so this is what makes
    // the frame-0 hang unrepeatable rather than merely avoided once.
    // Doubled FINAL PASS too (2026-08-18). This is what makes eye 1 a graded
    // copy of ITS OWN composite rather than a graded copy of eye 0: the pass
    // samples the frame root's composite, and that view is mirror-backed, so
    // redirection swaps it for the doubled call's own output.
    if ((inside_frame_root_double || inside_final_pass_double) &&
        read_redirect_enabled.load(std::memory_order_relaxed) &&
        sets && set_count) {
        std::array<VkDescriptorSet, 16> substituted{};
        bool any = false;
        if (set_count <= substituted.size()) {
            for (uint32_t i = 0; i < set_count; ++i) {
                substituted[i] = sets[i];
                if (sets[i] == VK_NULL_HANDLE) continue;
                const VkDescriptorSet clone =
                    get_redirected_set(vulkan_state().device, sets[i], first_set + i);
                if (clone != VK_NULL_HANDLE) { substituted[i] = clone; any = true; }
            }
            if (any) {
                read_redirect_substitutions.fetch_add(1, std::memory_order_relaxed);
                // R3/R6: record what is about to be bound - the fact the
                // manifest needs and the one its failed control proved it did
                // not have. Both bind points now, since the depth-downsample
                // passes are graphics.
                for (uint32_t i = 0; i < set_count; ++i)
                    if (sets[i] != VK_NULL_HANDLE)
                        df_bound_clone_by_set[sets[i]] = substituted[i];
                // P1 BOUND phase - the clones that are about to be bound in
                // place of DOOM's sets. This is the row that compares to the
                // capture's DOUBLED line, because a capture records what was
                // bound, not what was asked for.
                p1_record(set_count, substituted.data(), true);
                // Dynamic offsets are passed through untouched: the clone has
                // the same layout and the same buffer bindings, so the offsets
                // still address the same records.
                kharvox::native::bindNativeDescriptors(command_buffer, bind_point, layout, first_set,
                    set_count, sets, substituted.data(), dynamic_offset_count, dyn_out);
                return;
            }
        } else if (read_redirect_failures.fetch_add(1, std::memory_order_relaxed) < 10) {
            log::error(std::format(
                "ReadRedirect FALLBACK: {} sets in one bind exceeds the substitution buffer; "
                "binding all unmodified.", set_count));
        }
    }
    struct MatrixSnapshot {
        uint32_t set{};
        uint32_t binding{};
        uint32_t dynamic_offset{};
        uint32_t byte_count{};
        uint64_t descriptor_range{};
        VkPipeline pipeline{VK_NULL_HANDLE};
        uint64_t frame{};
        std::array<float, 16> values{};
    };
    // ⚠ COLLECTED ONLY WHEN F8 CAMERA CAPTURE IS ON (Desk-1, 2026-08-29).
    // The only consumer is the classification loop at the bottom of this
    // function, and that loop is already skipped when camera_capture_active
    // is false - so every push_back on a normal frame allocated, grew and
    // freed a vector of 112-byte records whose contents were then discarded.
    // The flag is read ONCE per call (camera_capture_on, hoisted at the top)
    // and used at all three sites, so a call can no longer collect under one
    // answer and be judged under another.
    std::vector<MatrixSnapshot> snapshots;
    if (camera_capture_on) snapshots.reserve(8);
    // TrueStereoGpuProbe window - see its declaration above. Computed once
    // here so both gates below (the fast-path skip and the inner per-
    // binding sample-count skip) can be widened identically without two
    // separate flags drifting out of sync.
    const bool true_stereo_probe_wanted =
        current_true_stereo_frame < kMaxTrueStereoGpuProbeFrames &&
        (true_stereo_gpu_probe_real_samples.load(std::memory_order_relaxed) <
             kMaxTrueStereoGpuProbeSamplesPerCallKind ||
         true_stereo_gpu_probe_extra_samples.load(std::memory_order_relaxed) <
             kMaxTrueStereoGpuProbeSamplesPerCallKind);
    // TrueStereoGpuWrite needs the detailed path for the ENTIRE TrueStereo
    // session (not just the probe's diagnostic window), but only while
    // actually inside our own injected extra call - the real call's binds
    // are untouched.
    const bool true_stereo_extra_active =
        inside_injected_extra_call && true_stereo_enabled.load(std::memory_order_relaxed);
    // StereoDualEyeDump Stage D (2026-08-11): three prior attempts (clone-
    // swap alone, in-place mutation + 4-job reinvoke, in-place mutation +
    // full 10-job reinvoke) all showed the SAME magnitude-independent
    // noise-level diff between eyes - confirmed via a 20x diagnostic
    // amplification that didn't scale the diff at all. Root cause: none of
    // them ever touch the GPU-mapped uniform buffer directly: this
    // project's own 2026-08-09 TrueStereoGpuProbe/TrueStereoGpuWrite work
    // already established DOOM's GPU camera data is a SEPARATE, per-call
    // ring-allocated slot that CPU-side render-view edits don't propagate
    // into - only a direct write into that slot (exactly what
    // TrueStereoGpuWrite already does, just for the old toe-in rotation)
    // reaches the image. Stage D reuses that exact proven write mechanism,
    // keyed on current_eye_render instead of true_stereo_enabled, with a
    // signed per-eye TRANSLATION instead of the old rotation - riding
    // alongside Stage C's already-validated full pipeline reinvoke (which
    // TrueStereoGpuWrite alone never had, and needs, to avoid the stale-
    // draw-list corruption that alone produced in 2026-08-09).
    const bool stereo_dual_eye_gpu_active =
        stereo_dual_eye_camera_write_enabled.load(std::memory_order_relaxed) &&
        inside_injected_extra_call && current_eye_render >= 0;
    const bool stereo_dual_eye_candidate_trace_active =
        !stereo_dual_eye_camera_write_enabled.load(std::memory_order_relaxed) &&
        inside_injected_extra_call && current_eye_render >= 0;
    // TrueStereoMatrixCapture - see its declaration above. Active for the
    // whole TrueStereo session, not just the probe's short window.
    const bool true_stereo_matrix_capture_wanted = current_true_stereo_frame != UINT32_MAX;
    // WorldPipelineBindingProbe - see representative_world_pipeline_this_call's
    // declaration above. Gated on real_frame_query_gate_probe_armed (HOME)
    // for the real call specifically - same human-triggered-capture rule
    // established for RealFrameQueryGateProbe this session, so this doesn't
    // silently log from every ordinary real frame starting at DLL load. The
    // extra call side doesn't need its own gate: it's already bounded by
    // INSERT arming StereoDualEyeDump (max 5 attempts x 2 eyes).
    const bool world_pipeline_binding_probe_wanted =
        representative_world_pipeline_this_call != VK_NULL_HANDLE && !world_binding_captured_this_call &&
        (inside_injected_extra_call ||
         real_frame_query_gate_probe_armed.load(std::memory_order_relaxed));
    // DescriptorCensus - see its declaration above. Must be part of the
    // fast-path guard below or the census would be silently skipped on every
    // bind, which is precisely the "path that cannot execute" failure this
    // project keeps re-committing.
    const bool descriptor_census_wanted =
        descriptor_census_enabled.load(std::memory_order_relaxed) &&
        (inside_frame_root_double
             ? descriptor_census_lines.load(std::memory_order_relaxed) <
                   kMaxDescriptorCensusLines
             : descriptor_census_real_lines.load(std::memory_order_relaxed) <
                   kMaxDescriptorCensusRealLines);
    // ⚠ wrap::armed() JOINS THIS GATE, and its absence is why three instruments
    // in a row logged zero.
    //
    // Everything below early-returns unless one of nine diagnostic flags is
    // armed. My GPU camera classifier was placed INSIDE that dead region and
    // reported "rotated=0 clean=0 other=0" for two runs - not a measurement, an
    // unreachable probe.
    //
    // It also has to be here for the WRITE. The comment forty lines above says
    // it outright, and has since 2026-08-09: "DOOM's GPU camera data is a
    // SEPARATE, per-call ring-allocated slot that CPU-side render-view edits
    // don't propagate into - only a direct write into that slot reaches the
    // image." Four CPU sites writing 4609/8446/864 times a run with no visual
    // change is that sentence being demonstrated. This hook is where the slot
    // is bound, so this is the only place the rotation can actually land.
    // Native does not use the donor's headlook/gun correction census. With
    // both census and GPU-camera inspection off, every dynamic-buffer record
    // below would stop at snapshot_wanted without reading or writing bytes.
    // Keep all diagnostic/translation guards and the actual native bind.
    const bool native_walk_unused = kharvox::native::requested() &&
        !cfg_fhcensus && !wrap::config_gpucam();
    if ((!cfg_armed || native_walk_unused) &&
        !camera_capture_on &&
        !view_shift_active.load(std::memory_order_relaxed) &&
        cpu_camera_scan_state.load(std::memory_order_relaxed) != 1 &&
        !true_stereo_probe_wanted && !true_stereo_extra_active &&
        !stereo_dual_eye_gpu_active &&
        !stereo_dual_eye_candidate_trace_active &&
        !true_stereo_matrix_capture_wanted &&
        !descriptor_census_wanted &&
        !world_pipeline_binding_probe_wanted) {
        kharvox::native::bindNativeDescriptors(command_buffer, bind_point, layout, first_set,
            set_count, sets, sets, dynamic_offset_count, dyn_out);
        return;
    }
    // Second pre-emption on the path to the census: camera capture's 1-in-10
    // frame sampling. Traced and neutralised rather than assumed inactive.
    if (camera_capture_on &&
        !descriptor_census_wanted &&
        (presented_frame.load(std::memory_order_relaxed) % 10) != 0) {
        kharvox::native::bindNativeDescriptors(command_buffer, bind_point, layout, first_set,
            set_count, sets, sets, dynamic_offset_count, dyn_out);
        return;
    }
    // The census call that used to be here has moved ABOVE the ReadRedirect
    // block - see the note there. descriptor_census_wanted survives only in
    // the two pre-emption guards above, where it still has to keep this
    // function from early-returning past the rest of the diagnostic work.
    // WorldPipelineBindingProbe: isolated from the camera-matrix-scanning
    // logic below on purpose - that logic is specific to dynamic uniform
    // buffer offsets and shouldn't need to know about this. Fires once per
    // RenderOpaque call, the first time the representative world-geometry
    // pipeline's command buffer binds descriptor sets, tagged real/extra/eye
    // so a HOME+INSERT paired capture can compare actual bound resource IDs
    // (not just flags/counts) for the same logical draw.
    if (world_pipeline_binding_probe_wanted && command_buffer == representative_world_pipeline_cmdbuf_this_call) {
        std::unique_lock lock(swapchains_mutex);
        world_binding_captured_this_call = true;
        std::string detail;
        for (uint32_t set_index = 0; sets && set_index < set_count; ++set_index) {
            detail += std::format(" set{}={}", first_set + set_index,
                reinterpret_cast<void*>(sets[set_index]));
            const auto found_buffers = descriptor_sets.find(sets[set_index]);
            const auto found_images = descriptor_image_sets.find(sets[set_index]);
            std::vector<uint32_t> binding_indices;
            if (found_buffers != descriptor_sets.end())
                for (const auto& [binding, unused] : found_buffers->second) binding_indices.push_back(binding);
            if (found_images != descriptor_image_sets.end())
                for (const auto& [binding, unused] : found_images->second) binding_indices.push_back(binding);
            std::sort(binding_indices.begin(), binding_indices.end());
            binding_indices.erase(std::unique(binding_indices.begin(), binding_indices.end()),
                binding_indices.end());
            for (uint32_t binding : binding_indices) {
                if (found_buffers != descriptor_sets.end()) {
                    const auto b = found_buffers->second.find(binding);
                    if (b != found_buffers->second.end()) {
                        detail += std::format("[b{}:buf={}]", binding,
                            reinterpret_cast<void*>(b->second.buffer));
                        continue;
                    }
                }
                if (found_images != descriptor_image_sets.end()) {
                    const auto b = found_images->second.find(binding);
                    if (b != found_images->second.end()) {
                        detail += std::format("[b{}:view={} samp={}]", binding,
                            reinterpret_cast<void*>(b->second.image_view),
                            reinterpret_cast<void*>(b->second.sampler));
                    }
                }
            }
        }
        log::warn(std::format(
            "WorldPipelineBindingProbe callKind={} eye={} pipeline={}{}",
            inside_injected_extra_call ? "extra" : "real", current_eye_render,
            reinterpret_cast<void*>(representative_world_pipeline_this_call), detail));
    }
    uint32_t dynamic_index = 0;
    {
        // ===== SHARED. THIS ONE LINE IS THE FRAMERATE FIX ==================
        //
        // ~1667 of these a frame, from DOOM's parallel job threads, each
        // holding the global mutex across this whole walk. That is what made
        // this hook cost 13-16 us against 0.04 us for CmdDrawIndexed next to
        // it, and it is why shaving work off the BODY only ever bought 15%:
        // the cost was never the body, it was the waiting.
        //
        // Shared is SOUND here, checked line by line over the whole scope
        // rather than assumed: this walk only READS descriptor_sets,
        // buffer_bindings and memory_mappings. Everything it writes is either
        // an atomic (captured_render_view, world_view_address, the census
        // counters), caller-local (dyn_spill, snapshot, snapshots - declared
        // in this function), mapped GPU memory at a per-record address, or
        // freqHigh census state, which now takes g_census_mutex of its own.
        // freqHigh census state, which takes g_census_mutex of its own.
        //
        // Writers - swapchain create/destroy, memory map/unmap, descriptor
        // update - still take it EXCLUSIVELY, so a mapping cannot be torn down
        // underneath this walk. That is why the lock is held across the whole
        // scope rather than being released after the lookups: shortening it
        // that way would have been faster still and would have raced an unmap.
        // ===== DBW: WHERE DO THE 10 us PER CALL ACTUALLY GO? ===============
        //
        // I have now guessed twice about this hook and been half right both
        // times: the census hoist bought 15% and the shared lock bought
        // another 25%, and it is STILL ~10 us a call against 0.04 us for
        // CmdDrawIndexed beside it. So this splits the call into the only
        // three things it can be - waiting for the lock, walking the sets,
        // and the volume of descriptors walked - and reports each per frame.
        // No more reasoning about it until the split says which one it is.
        const uint64_t dbw_t0 = fb_ns();
        std::shared_lock lock(swapchains_mutex);
        const uint64_t dbw_t1 = fb_ns();
        g_dbw_lock_ns.fetch_add(dbw_t1 - dbw_t0, std::memory_order_relaxed);
        g_dbw_calls.fetch_add(1, std::memory_order_relaxed);
        struct DbwBody {
            uint64_t t;
            ~DbwBody() { g_dbw_body_ns.fetch_add(fb_ns() - t, std::memory_order_relaxed); }
        } dbw_body_{dbw_t1};
        for (uint32_t set_index = 0; sets && set_index < set_count; ++set_index) {
            g_dbw_sets.fetch_add(1, std::memory_order_relaxed);
            const auto found_set = descriptor_sets.find(sets[set_index]);
            if (found_set == descriptor_sets.end()) continue;
            g_dbw_bindings.fetch_add(
                static_cast<uint32_t>(found_set->second.size()), std::memory_order_relaxed);
            // ⚠ NO HEAP ALLOCATION INSIDE THE LOCK (Desk-1, 2026-08-29).
            // `std::vector<uint32_t> bindings` was allocated and freed once
            // per descriptor set per call - at 1730 calls a frame that is
            // thousands of malloc/free pairs on the render thread, all of
            // them while the global swapchains_mutex is held, which is the
            // FORBIDDEN #11 class of cost this hook keeps re-growing.
            //
            // The number of DYNAMIC bindings in one set is bounded by the
            // device's maxDescriptorSetUniformBuffersDynamic (single digits
            // here), so a fixed array covers every real set; the spill path
            // reproduces the old behaviour exactly and counts itself.
            //
            // The DescriptorBinding POINTER travels with the binding number,
            // which also removes the second hash lookup that
            // `found_set->second.at(binding)` performed per record. Pointers
            // into the map's nodes are stable for the whole walk: nothing in
            // this loop inserts into or erases from descriptor_sets, and
            // swapchains_mutex is held throughout.
            struct DynSlot { uint32_t binding; const DescriptorBinding* desc; };
            std::array<DynSlot, kMaxDynamicBindingsPerSet> dyn_fixed{};
            std::vector<DynSlot> dyn_spill;  // empty => no allocation
            uint32_t dyn_count = 0;
            for (const auto& [binding, descriptor] : found_set->second) {
                if (descriptor.type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC &&
                    descriptor.type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)
                    continue;
                if (dyn_count < dyn_fixed.size() && dyn_spill.empty()) {
                    dyn_fixed[dyn_count] = DynSlot{binding, &descriptor};
                } else {
                    if (dyn_spill.empty()) {
                        if (g_bind_slot_spills.fetch_add(1, std::memory_order_relaxed) < 4)
                            log::warn(std::format(
                                "BindWalk: descriptor set has more than {} dynamic bindings; "
                                "spilling to the heap path (old behaviour, just slower).",
                                kMaxDynamicBindingsPerSet));
                        dyn_spill.assign(dyn_fixed.begin(),
                                         dyn_fixed.begin() + dyn_count);
                    }
                    dyn_spill.push_back(DynSlot{binding, &descriptor});
                }
                ++dyn_count;
            }
            DynSlot* dyn_slots = dyn_spill.empty() ? dyn_fixed.data() : dyn_spill.data();
            std::sort(dyn_slots, dyn_slots + dyn_count,
                      [](const DynSlot& a, const DynSlot& b) { return a.binding < b.binding; });
            for (uint32_t dyn_i = 0; dyn_i < dyn_count; ++dyn_i) {
                if (dynamic_index >= dynamic_offset_count || !dynamic_offsets) break;
                const uint32_t binding = dyn_slots[dyn_i].binding;
                const uint32_t dynamic_offset = dynamic_offsets[dynamic_index++];
                const auto& descriptor = *dyn_slots[dyn_i].desc;
                // ⚠ THE SEPCENSUS INTAKE DEPENDED ON AN ACCIDENT (2026-08-26).
                //
                // Past 512 F8 samples this clause skips every record unless
                // one of the listed probes is armed - and the only one live
                // during stereo is true_stereo_extra_active, which is true
                // ONLY INSIDE OUR OWN INJECTED EXTRA CALL. A census running
                // under that gate would see eye 1's binds and never eye 0's,
                // and would report maxPairSep ~ 0 whatever the real
                // separation was. That is this probe's most alarming result,
                // arriving as an artefact.
                //
                // It has been invisible because camera_sample_count only
                // increments on the F8 capture path, and F8 was never pressed
                // in the 16:14 run - it read 0 all session, so the gate was
                // open by luck. `wrap::armed()` makes that explicit: while
                // wraparound is armed nothing is dropped for sample-count
                // reasons. Everything downstream that WRITES is gated on its
                // own flag, so this widens a read-only intake only.
                // Upper bound raised 256 -> 4096 so LARGE uniform blocks reach the
                // wide sweep below. The narrow path still only reads the first
                // 64 bytes; the sweep handles the rest.
                // Ceiling 4096 -> 65536: blocks larger than the old limit were
                // SKIPPED ENTIRELY, so a weapon matrix inside a big per-object
                // block was invisible to every measurement in this thread. The
                // wide sweep still only reads the first kWideSweepBytes of one,
                // which is bounded work, but the block is no longer discarded.
                // FWDFIX intake. MUST sit before the filter below: that
                // filter drops everything under 64 bytes, and globalviewfwd
                // lives in a 32-byte block - the same blind spot that made
                // the 16-byte vanish test silently measure nothing.
                // The compute reconstruction block (windowpostoglobal at +16).
                if (descriptor.range < 64 || descriptor.range > 65536 ||
                    (!cfg_armed && camera_sample_count.load() >= 512 &&
                     cpu_camera_scan_state.load(std::memory_order_relaxed) != 1 &&
                     !true_stereo_probe_wanted && !true_stereo_extra_active &&
                     !stereo_dual_eye_gpu_active &&
                     !stereo_dual_eye_candidate_trace_active &&
                     !true_stereo_matrix_capture_wanted)) continue;
                const auto found_buffer = buffer_bindings.find(descriptor.buffer);
                if (found_buffer == buffer_bindings.end()) continue;
                const auto found_mapping = memory_mappings.find(found_buffer->second.memory);
                if (found_mapping == memory_mappings.end()) continue;
                const VkDeviceSize absolute = found_buffer->second.offset + descriptor.offset + dynamic_offset;
                if (absolute < found_mapping->second.offset) continue;
                const VkDeviceSize host_offset = absolute - found_mapping->second.offset;
                if (found_mapping->second.size != VK_WHOLE_SIZE &&
                    host_offset + 64 > found_mapping->second.size) continue;
                // FREQHIGH CENSUS intake (script §1.2). Read-only. The check
                // above only guarantees 64 readable bytes; the extra range
                // check here is REQUIRED before reading a full 160/128-byte
                // record. Declared range IS the layout discriminator.
                if (cfg_fhcensus &&
                    (descriptor.range == 160 || descriptor.range == 128) &&
                    (found_mapping->second.size == VK_WHOLE_SIZE ||
                     host_offset + descriptor.range <= found_mapping->second.size)) {
                    freqhigh_census_note(
                        reinterpret_cast<const float*>(found_mapping->second.host + host_offset),
                        static_cast<uint32_t>(descriptor.range),
                        found_mapping->second.host + host_offset);
                } else if (cfg_fhcensus &&
                           (descriptor.range == 208 || descriptor.range == 496) &&
                           (found_mapping->second.size == VK_WHOLE_SIZE ||
                            host_offset + descriptor.range <= found_mapping->second.size)) {
                    // The viewmodel family's sibling layouts (prepass depth +
                    // forward variant) - see freqhigh_sibling_note.
                    freqhigh_sibling_note(
                        reinterpret_cast<const float*>(found_mapping->second.host + host_offset),
                        static_cast<uint32_t>(descriptor.range),
                        found_mapping->second.host + host_offset);
                }
                // ===== THE FRAMERATE. Measured 2026-08-30 ==================
                //
                // Below this line is a 64-byte memcpy OUT OF MAPPED GPU
                // MEMORY, per descriptor, per bind call - about 3,000 times a
                // frame. Host-visible Vulkan memory on a discrete GPU is
                // write-combined and UNCACHED: reading it costs microseconds,
                // not nanoseconds. The DBW split measured the walk at 15.30 us
                // per call for ONE set of 3.8 bindings - ~4 us each - while
                // the lock was 0.03 us and the real driver call 0.07 us. This
                // read is the 4 us.
                //
                // And it ran on EVERY descriptor because of the gate above:
                // `!cfg_armed && camera_sample_count >= 512 && ...`. That
                // 512-sample cutoff is DISABLED whenever wraparound is armed,
                // so arming wraparound switched on an uncapped per-descriptor
                // uncached read - which is exactly the owner's report that the
                // framerate has been wrong "since we first enabled true
                // wraparound VR", and why it never tracked scene complexity.
                //
                // Everything downstream of here is a DIAGNOSTIC:
                // classify_gpu_camera (GPUCAM), the camera capture and
                // the true-stereo probes. None of it is
                // production. The production consumer - the freqHigh census
                // that gunfix2 depends on - reads its OWN ranges above this
                // point and is untouched, which is why this is safe to skip.
                // wrap::publish_world_vp is not here either (it lives at the
                // render-view sampler), so world_vp() and every
                // is_gameplay_view gate keep working.
                const bool snapshot_wanted =
                    camera_capture_on ||
                    cpu_camera_scan_state.load(std::memory_order_relaxed) == 1 ||
                    true_stereo_probe_wanted || true_stereo_extra_active ||
                    stereo_dual_eye_gpu_active ||
                    stereo_dual_eye_candidate_trace_active ||
                    true_stereo_matrix_capture_wanted ||
                    wrap::config_gpucam();
                if (!snapshot_wanted) {
                    g_dbw_snapshot_skipped.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                g_dbw_snapshot_read.fetch_add(1, std::memory_order_relaxed);
                MatrixSnapshot snapshot;
                snapshot.set = first_set + set_index;
                snapshot.binding = binding;
                snapshot.dynamic_offset = dynamic_offset;
                snapshot.byte_count = 64;
                snapshot.descriptor_range = static_cast<uint64_t>(descriptor.range);
                snapshot.frame = presented_frame.load(std::memory_order_relaxed);
                const auto found_pipeline = bound_graphics_pipelines.find(command_buffer);
                if (found_pipeline != bound_graphics_pipelines.end())
                    snapshot.pipeline = found_pipeline->second;
                const uint64_t sample_key =
                    static_cast<uint64_t>(reinterpret_cast<uintptr_t>(snapshot.pipeline)) ^
                    (static_cast<uint64_t>(binding) << 56) ^
                    (snapshot.descriptor_range << 40);
                if (camera_capture_on &&
                    !sampled_camera_pipeline_bindings.insert(sample_key).second)
                    continue;
                std::memcpy(snapshot.values.data(),
                    found_mapping->second.host + host_offset, snapshot.byte_count);
                // ===== DOES OUR ROTATION REACH THE GPU AT ALL? =============
                //
                // Four write sites now write every frame - renderView=3299,
                // guiView=3299, cameraSrc=3169 - and the picture is unchanged.
                // Every one of those counters proves a CPU write LANDED. None
                // of them can see what the GPU actually reads.
                //
                // This is that measurement, and it is the one I should have
                // built five runs ago. It classifies every camera-shaped
                // uniform the GPU is ACTUALLY BOUND, at the moment of binding,
                // against two references: DOOM's clean camera basis, and the
                // head-rotated basis we wrote. The three counts answer it
                // outright:
                //
                //   rotated  > 0, clean == 0  -> the GPU sees our rotation.
                //                                The camera is not the problem.
                //   clean    > 0, rotated == 0 -> the GPU is fed from a source
                //                                upstream of every site we
                //                                write. More CPU sites will
                //                                never help.
                //   both     > 0              -> SPLIT. This is the symptom:
                //                                some draws rotated, some not,
                //                                and the pipeline that binds
                //                                the clean one names the pass
                //                                that will not move.
                //
                // Read-only, and bounded by construction: it runs only while
                // wraparound is armed, only on 64-byte camera-shaped records,
                // and it does arithmetic on bytes already copied above.
                // ⚠ NOT looks_like_world_camera_matrix. That predicate bounds
                // row 1 to 1.65..1.90 and the gameplay camera at slider 130
                // reads 0.8290 - it is FOV-coupled, which this project already
                // proved once and which I then used as this probe's own gate.
                // It returned zero for every camera and the probe logged
                // rotated=0 clean=0 other=0: a dead instrument, not a result.
                // wide_world_vp_predicate is scale-free.
                if (cfg_armed) {
                    // POSITIVE CONTROL, before any predicate. If this stays at
                    // zero the probe is unreachable, which is exactly what the
                    // last two runs could not distinguish from "no cameras".
                    g_gpu_cam_attempts.fetch_add(1, std::memory_order_relaxed);
                    // The 2x CORRECTION now lives INSIDE classify_gpu_camera, not at a
                    // separate call site. The 10:32 run had it here as its own `if` and
                    // its examined counter stayed at 0 all session while the census 28
                    // lines up counted 560 2x records a second - two call sites, same
                    // guards, one firing. Rather than spend another of the owner's runs
                    // finding out why, the fix moved to where the measurement that
                    // already works lives: the census computes the exact predicate the
                    // correction needs, so they cannot disagree about whether they ran.
                    if (wrap::wide_world_vp_predicate(snapshot.values.data()))
                        classify_gpu_camera(snapshot.values.data(), snapshot.pipeline,
                                            found_mapping->second.host + host_offset,
                                            snapshot.set, snapshot.binding);
                    // ===== THE WIDE SWEEP (2026-08-26) ====================
                    //
                    // WHY. The 17:06 run eliminated the entire narrow census
                    // surface for the weapon. It worked - it found a -1.66
                    // population, corrected it to -1.02 with Lever V's own
                    // delta, and the correct -0.99 population was untouched -
                    // so the instrument and the correction machinery are both
                    // sound. But inBand=1, the whole baseline list ran from
                    // -1.66 to +0.42 with NOTHING near -2, and the gun did not
                    // change. No record this census sees rotates at the rate
                    // the gun visibly does.
                    //
                    // Push constants were ruled out in August (the hook ran 48
                    // seconds of play and DOOM never called vkCmdPushConstants
                    // once). So the gap is in the intake itself: the census
                    // only ever looked at the FIRST 64 bytes of descriptors
                    // whose range is 64..256. A per-object matrix living at a
                    // non-zero offset inside a larger uniform block has been
                    // invisible to every measurement in this thread.
                    //
                    // This sweeps those larger blocks at 64-byte strides, and
                    // keys each sub-matrix separately so its rate is fitted on
                    // its own rather than blended with its neighbours. Bounded:
                    // only descriptors bigger than the narrow path's limit,
                    // capped at kWideSweepBytes, and only while armed.
                    // ⚠ STRIDED. Raising the descriptor ceiling to 65536 sent
                    // vastly more binds through this sweep, and each one walks
                    // up to sixteen sub-matrices through the predicate, the
                    // classifier and the rate table. Combined with the triple
                    // table scan that is what made the game unplayable.
                    //
                    // Sampling one bind in eight costs nothing in coverage
                    // over a thirty-second sweep - the same keys are bound
                    // thousands of times - and cuts the sweep's cost by 8x.
                    // Coverage is provable rather than assumed: wideExam still
                    // counts what was looked at.
                    static std::atomic_uint32_t wide_stride{0};
                    if (descriptor.range > 256 &&
                        (wide_stride.fetch_add(1, std::memory_order_relaxed) & 7u) == 0u) {
                        const uint64_t span =
                            (std::min)(static_cast<uint64_t>(descriptor.range),
                                       static_cast<uint64_t>(kWideSweepBytes));
                        for (uint64_t off = 64; off + 64 <= span; off += 64) {
                            if (found_mapping->second.size != VK_WHOLE_SIZE &&
                                host_offset + off + 64 > found_mapping->second.size) break;
                            const float* sub = reinterpret_cast<const float*>(
                                found_mapping->second.host + host_offset + off);
                            g_wide_examined.fetch_add(1, std::memory_order_relaxed);
                            if (!wrap::wide_world_vp_predicate(sub)) continue;
                            g_wide_matched.fetch_add(1, std::memory_order_relaxed);
                            classify_gpu_camera(
                                sub, snapshot.pipeline,
                                found_mapping->second.host + host_offset + off,
                                snapshot.set,
                                snapshot.binding + 1000u + static_cast<uint32_t>(off / 64));
                        }
                    }

                    // ===== THE DIRECT GPU RING-SLOT WRITE: REMOVED =========
                    //
                    // Build B (2026-08-26). The A*MVP re-base into the
                    // per-invocation ring slot lived here, gated on
                    // `mvp=on` in the switch file. The 14:47 run measured
                    // `GPUCAM ... WRITES=0` for the whole session: it never
                    // rewrote a single record. It is deleted rather than left
                    // switched off, because a dormant write site is exactly
                    // the thing this project keeps rediscovering as an armed
                    // confound six runs later.
                    //
                    // The CLASSIFIER above stays. It is read-only, it is what
                    // produced clean=0 / WRITES=0 / other=29253, and `other`
                    // is where the remaining welded population (class 3)
                    // lives - Build C's target.
                    //
                    // `mvp=` in the switch file is now INERT and says so at
                    // arm time; do not resurrect this without re-reading
                    // FORBIDDEN-BOUNDARIES item 1.

                }
                // TrueStereoMatrixCapture - see its declaration above. Real
                // call only, one line per frame, full 4 rows (+ the second
                // 64 bytes if present) - pure read, no write.
                if (true_stereo_matrix_capture_wanted && !inside_injected_extra_call &&
                    binding == 0 && looks_like_world_camera_matrix(snapshot.values.data()) &&
                    true_stereo_matrix_capture_last_frame.exchange(
                        current_true_stereo_frame, std::memory_order_relaxed) !=
                        current_true_stereo_frame) {
                    const float* f = snapshot.values.data();
                    char second_half[160] = "";
                    if (descriptor.range >= 128 &&
                        (found_mapping->second.size == VK_WHOLE_SIZE ||
                         host_offset + 128 <= found_mapping->second.size)) {
                        std::array<float, 16> second{};
                        if (safe_read_bytes(reinterpret_cast<uintptr_t>(
                                found_mapping->second.host + host_offset + 64),
                                second.data(), 64)) {
                            std::snprintf(second_half, sizeof(second_half),
                                " second_row0=(%.4f %.4f %.4f %.4f) second_row1=(%.4f %.4f %.4f %.4f) "
                                "second_row2=(%.4f %.4f %.4f %.4f) second_row3=(%.4f %.4f %.4f %.4f)",
                                second[0], second[1], second[2], second[3],
                                second[4], second[5], second[6], second[7],
                                second[8], second[9], second[10], second[11],
                                second[12], second[13], second[14], second[15]);
                        }
                    }
                    log::warn(std::format(
                        "TrueStereoMatrixCapture frame={} dynamicOffset={} range={} "
                        "row0=({:.4f} {:.4f} {:.4f} {:.4f}) row1=({:.4f} {:.4f} {:.4f} {:.4f}) "
                        "row2=({:.4f} {:.4f} {:.4f} {:.4f}) row3=({:.4f} {:.4f} {:.4f} {:.4f}){}",
                        current_true_stereo_frame, dynamic_offset, descriptor.range,
                        f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7],
                        f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15], second_half));
                }
                // Restricted to binding==0 && camera-shaped - the exact
                // signature F10 already validated as meaningful (see
                // kMaxTrueStereoGpuProbeSamplesPerCallKind's comment above -
                // an unfiltered first attempt logged 400 lines, all from one
                // real call, without ever reaching the extra call).
                auto& probe_counter = inside_injected_extra_call
                    ? true_stereo_gpu_probe_extra_samples : true_stereo_gpu_probe_real_samples;
                if (true_stereo_probe_wanted && binding == 0 &&
                    looks_like_world_camera_matrix(snapshot.values.data()) &&
                    probe_counter.fetch_add(1, std::memory_order_relaxed) <
                        kMaxTrueStereoGpuProbeSamplesPerCallKind) {
                    const float* f = snapshot.values.data();
                    log::warn(std::format(
                        "TrueStereoGpuProbe frame={} callKind={} thread={} activeRenderView={} "
                        "cmdBuf={} buffer={} memory={} dynamicOffset={} absoluteHostOffset={} "
                        "pipeline={} set={} binding={} range={} cameraShaped={} "
                        "row0=({:.4f} {:.4f} {:.4f} {:.4f}) row2=({:.4f} {:.4f} {:.4f} {:.4f})",
                        current_true_stereo_frame, inside_injected_extra_call ? "extra" : "real",
                        GetCurrentThreadId(), reinterpret_cast<void*>(active_render_view),
                        reinterpret_cast<void*>(command_buffer),
                        reinterpret_cast<void*>(descriptor.buffer),
                        reinterpret_cast<void*>(found_buffer->second.memory), dynamic_offset,
                        static_cast<uint64_t>(host_offset),
                        reinterpret_cast<void*>(snapshot.pipeline), snapshot.set, binding,
                        descriptor.range, looks_like_world_camera_matrix(f),
                        f[0], f[1], f[2], f[3], f[8], f[9], f[10], f[11]));
                }
                // TrueStereoGpuWrite - see its declaration above. Writes
                // straight into this GPU-mapped host pointer, never DOOM's
                // shared render-view object, only for binding==0 camera-
                // shaped records seen during our OWN injected extra call.
                if (true_stereo_extra_active && binding == 0 &&
                    looks_like_world_camera_matrix(snapshot.values.data()) &&
                    true_stereo_rotated_addresses.insert(
                        reinterpret_cast<uintptr_t>(found_mapping->second.host + host_offset)).second) {
                    unsigned char* target = found_mapping->second.host + host_offset;
                    std::array<float, 16> translated_current;
                    std::memcpy(translated_current.data(), snapshot.values.data(), 64);
                    const float translation_offset =
                        g_true_stereo_translation_offset.load(std::memory_order_relaxed);
                    apply_translation(translated_current.data(), translation_offset);
                    const bool wrote_current = safe_write_bytes(
                        reinterpret_cast<uintptr_t>(target), translated_current.data(), 64);
                    // Second 64 bytes (present when range>=128): NOT
                    // touched. Live TrueStereoMatrixCapture data
                    // (2026-08-09) showed this half holding an axis-aligned
                    // identity-like transform with large, round world-
                    // coordinate offsets (e.g. -10176, -2624, 9540) -
                    // looks like a bounding-box/region volume, not a
                    // "current/previous camera pair" (that assumption was
                    // only ever confirmed for the CPU render-view object,
                    // never for this GPU record). Corruption was identical
                    // in severity at both 20 deg and 1.4 deg toe-in -
                    // proportional-mismatch theories (stale visibility
                    // frustum) predict graceful degradation at small
                    // angles; this didn't degrade, it stayed all-or-
                    // nothing, which fits "clobbering an unrelated
                    // bounding/clip field" (binary include/exclude) far
                    // better than "frustum slightly off." Leaving this
                    // half untouched to test that directly.
                    const bool wrote_previous = true;
                    const bool write_ok = wrote_current && wrote_previous;
                    const uint32_t logged =
                        true_stereo_gpu_write_log_count.fetch_add(1, std::memory_order_relaxed);
                    if (!write_ok || logged < kMaxTrueStereoGpuWriteLogLines) {
                        log::warn(std::format(
                            "TrueStereoGpuWrite frame={} buffer={} dynamicOffset={} range={} "
                            "rightOffset={} wroteCurrent={} wrotePrevious={}",
                            current_true_stereo_frame, reinterpret_cast<void*>(descriptor.buffer),
                            dynamic_offset, descriptor.range, translation_offset,
                            wrote_current, wrote_previous));
                    }
                }
                // StereoDualEyeDump Stage D - see stereo_dual_eye_gpu_active's
                // declaration above. Same write mechanism as TrueStereoGpuWrite,
                // same dedup set (true_stereo_rotated_addresses; mutually
                // exclusive with F1, and cleared per extra call - see
                // hook_doom_render_opaque), signed per-eye TRANSLATION instead
                // of the old fixed rotation/translation.
                // Matched RenderDepth record for Candidate C. The paired
                // read-only trace found exactly one 64-byte, world-camera-
                // shaped binding-0 record in RenderDepth. Write it before
                // the depth pass emits the eye's depth image, at the same
                // native offset as Candidate C below.
                if (stereo_dual_eye_gpu_active && inside_extra_render_depth_reinvoke &&
                    !stereo_dual_eye_depth_write_done && binding == 0 &&
                    snapshot.descriptor_range == 64 &&
                    std::fabs(snapshot.values[15]) > 1000.0f &&
                    looks_like_world_camera_matrix(snapshot.values.data()) &&
                    true_stereo_rotated_addresses.insert(
                        reinterpret_cast<uintptr_t>(found_mapping->second.host + host_offset)).second) {
                    std::array<float, 16> translated_current;
                    std::memcpy(translated_current.data(), snapshot.values.data(), 64);
                    const float signed_offset =
                        current_eye_render == 0 ? -kStereoDualEyeHalfIpd : kStereoDualEyeHalfIpd;
                    apply_translation(translated_current.data(), signed_offset);
                    const bool wrote_ok = safe_write_bytes(
                        reinterpret_cast<uintptr_t>(found_mapping->second.host + host_offset),
                        translated_current.data(), 64);
                    stereo_dual_eye_depth_write_done = wrote_ok;
                    log::warn(std::format(
                        "StereoDualEyeGpuWrite stage=RenderDepth eye={} buffer={} dynamicOffset={} range={} "
                        "signedOffset={:.2f} wroteOk={}",
                        current_eye_render, reinterpret_cast<void*>(descriptor.buffer),
                        dynamic_offset, descriptor.range, signed_offset, wrote_ok));
                }
                // Candidate C (Avenue 1, 2026-08-13): the 64-byte world
                // pipeline record and then the first world-scale 128-byte
                // record were both safely written with byte-identical eye
                // images. The trace's remaining independent candidate is the
                // SECOND world-scale 128-byte record, bound on the next
                // pipeline. Count only world-scale 128-byte records and write
                // exactly ordinal 1, once per eye.
                if (stereo_dual_eye_gpu_active && !inside_extra_render_depth_reinvoke &&
                    snapshot.descriptor_range == 128 &&
                    std::fabs(snapshot.values[15]) > 1000.0f &&
                    looks_like_world_camera_matrix(snapshot.values.data())) {
                    const uint32_t candidate_ordinal = stereo_dual_eye_worldspace_128_seen++;
                    if (!stereo_dual_eye_targeted_write_done && candidate_ordinal == 1 &&
                    true_stereo_rotated_addresses.insert(
                        reinterpret_cast<uintptr_t>(found_mapping->second.host + host_offset)).second) {
                    unsigned char* target = found_mapping->second.host + host_offset;
                    std::array<float, 16> translated_current;
                    std::memcpy(translated_current.data(), snapshot.values.data(), 64);
                    const float signed_offset =
                        current_eye_render == 0 ? -kStereoDualEyeHalfIpd : kStereoDualEyeHalfIpd;
                    apply_translation(translated_current.data(), signed_offset);
                    const bool wrote_ok = safe_write_bytes(
                        reinterpret_cast<uintptr_t>(target), translated_current.data(), 64);
                    stereo_dual_eye_targeted_write_done = wrote_ok;
                    const uint32_t logged =
                        stereo_dual_eye_gpu_write_log_count.fetch_add(1, std::memory_order_relaxed);
                    if (!wrote_ok || logged < kMaxStereoDualEyeGpuWriteLogLines) {
                        log::warn(std::format(
                            "StereoDualEyeGpuWrite stage=RenderOpaque eye={} buffer={} dynamicOffset={} range={} "
                            "signedOffset={:.2f} wroteOk={}",
                            current_eye_render, reinterpret_cast<void*>(descriptor.buffer),
                            dynamic_offset, descriptor.range, signed_offset, wrote_ok));
                    }
                    }
                }
                // The no-camera-write INSERT control renders an intact image.
                // Trace RenderDepth and RenderOpaque separately: Candidate C
                // affects the image in opaque, but its depth was produced
                // beforehand with the center camera. This identifies a
                // matching depth-stage input before a paired write is tried.
                if (stereo_dual_eye_candidate_trace_active && binding == 0 &&
                    looks_like_world_camera_matrix(snapshot.values.data()) &&
                    stereo_dual_eye_candidate_addresses.insert(
                        reinterpret_cast<uintptr_t>(found_mapping->second.host + host_offset)).second &&
                    stereo_dual_eye_candidate_log_count.fetch_add(
                        1, std::memory_order_relaxed) <
                        kMaxStereoDualEyeCandidateLogLines) {
                    log::warn(std::format(
                        "StereoDualEyeCandidate eye={} stage={} cmdBuf={} pipeline={} worldPipeline={} "
                        "isRepresentative={} set={} binding={} buffer={} dynamicOffset={} "
                        "hostOffset={} range={} row3=({:.4f} {:.4f} {:.4f} {:.4f})",
                        current_eye_render,
                        inside_extra_render_depth_reinvoke ? "RenderDepth" : "RenderOpaque",
                        reinterpret_cast<void*>(command_buffer),
                        reinterpret_cast<void*>(snapshot.pipeline),
                        reinterpret_cast<void*>(representative_world_pipeline_this_call),
                        snapshot.pipeline == representative_world_pipeline_this_call,
                        snapshot.set, snapshot.binding,
                        reinterpret_cast<void*>(descriptor.buffer), snapshot.dynamic_offset,
                        static_cast<uint64_t>(host_offset), snapshot.descriptor_range,
                        snapshot.values[12], snapshot.values[13], snapshot.values[14],
                        snapshot.values[15]));
                }
                if (cpu_camera_scan_state.load(std::memory_order_relaxed) == 1 &&
                    binding == 0) {
                    const float* f = snapshot.values.data();
                    const bool main_camera_shape = looks_like_world_camera_matrix(f);
                    if (main_camera_shape && !camera_bind_stack_captured.exchange(true))
                        log_doom_camera_bind_stack();
                    uint32_t expected = 1;
                    if (main_camera_shape && cpu_camera_scan_state.compare_exchange_strong(expected, 2)) {
                        captured_render_view.store(active_render_view, std::memory_order_relaxed);
                        const size_t local_offset = find_matrix_in_render_view(
                            active_render_view, snapshot.values.data());
                        if (local_offset != SIZE_MAX) {
                            world_view_address.store(active_render_view, std::memory_order_relaxed);
                            world_view_matrix_offset.store(local_offset, std::memory_order_relaxed);
                        }
                        char correlation[224];
                        std::snprintf(correlation, sizeof(correlation),
                            "F10 exact camera bind correlation renderView=%p viewToken=%p pipeline=%p dynamicOffset=%u localMatrixOffset=%s0x%zX",
                            reinterpret_cast<void*>(active_render_view),
                            reinterpret_cast<void*>(active_render_view ? active_render_view + 0x110 : 0),
                            reinterpret_cast<void*>(snapshot.pipeline), dynamic_offset,
                            local_offset == SIZE_MAX ? "NOT_FOUND " : "", local_offset);
                        log::info(correlation);
                        auto* job = new CpuCameraScanJob;
                        job->matrix = snapshot.values;
                        HANDLE thread = CreateThread(nullptr, 0, cpu_camera_scan_worker, job, 0, nullptr);
                        if (thread) {
                            CloseHandle(thread);
                            log::info("F10 captured validated camera matrix; background scan launched");
                        } else {
                            delete job;
                            cpu_camera_scan_state = 0;
                            log::error("F10 could not start CPU camera scan thread");
                        }
                    }
                }
                // Live uniform writes are permanently disabled. The 64+128 test
                // caused a GPU hang because some larger records are material or
                // color-pass data rather than writable camera storage.
                if (camera_capture_on) snapshots.push_back(snapshot);
            }
        }
    }
    if (!camera_capture_on) {
        // TRAMPOLINE TIMED SEPARATELY. FbHook measures the WHOLE span, driver
        // call included, so 11.9 us/call has never distinguished "our work"
        // from "what vkCmdBindDescriptorSets costs anyway". Those have
        // completely different fixes and I have been quoting the sum as if it
        // were ours.
        const uint64_t tramp_t0 = fb_ns();
        kharvox::native::bindNativeDescriptors(command_buffer, bind_point, layout, first_set,
            set_count, sets, sets, dynamic_offset_count, dyn_out);
        g_dbw_tramp_ns.fetch_add(fb_ns() - tramp_t0, std::memory_order_relaxed);
        return;
    }
    for (const auto& snapshot : snapshots) {
        const float* f = snapshot.values.data();
        const uint32_t float_count = snapshot.byte_count / sizeof(float);
        constexpr float flip_identity[16] = {
            1,0,0,0, 0,-1,0,0, 0,0,1,0, 0,0,0,1};
        bool is_flip_identity = true;
        bool is_zero = true;
        bool is_finite = true;
        for (uint32_t element = 0; element < float_count; ++element) {
            is_flip_identity &= std::fabs(f[element] - flip_identity[element]) <= 0.0001f;
            is_zero &= std::fabs(f[element]) <= 0.000001f;
            is_finite &= std::isfinite(f[element]);
        }
        const bool is_ui_orthographic =
            std::fabs(f[0]) < 0.01f && std::fabs(f[5]) < 0.01f &&
            std::fabs(std::fabs(f[3]) - 1.0f) < 0.01f &&
            std::fabs(std::fabs(f[7]) - 1.0f) < 0.01f &&
            std::fabs(f[15] - 1.0f) < 0.01f;
        const bool is_known_projection =
            std::fabs(std::fabs(f[1]) - 1.639f) < 0.01f &&
            std::fabs(std::fabs(f[6]) - 3.079f) < 0.01f &&
            std::fabs(f[8] + 1.0f) < 0.01f;
        if (!is_finite || is_zero || is_flip_identity || is_ui_orthographic ||
            is_known_projection) continue;
        const uint32_t sample = camera_sample_count.fetch_add(1);
        if (sample >= 512) {
            camera_capture_active = false;
            continue;
        }
        char text[512];
        std::snprintf(text, sizeof(text),
            "F8 camera sample=%u frame=%llu pipeline=%p range=%llu bytes=%u set=%u binding=%u offset=%u "
            "[%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | "
            "%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]",
            sample, static_cast<unsigned long long>(snapshot.frame),
            reinterpret_cast<void*>(snapshot.pipeline),
            static_cast<unsigned long long>(snapshot.descriptor_range), snapshot.byte_count,
            snapshot.set, snapshot.binding,
            snapshot.dynamic_offset,
            f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7],
            f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
        log::info(text);
    }
    kharvox::native::bindNativeDescriptors(command_buffer, bind_point, layout, first_set,
        set_count, sets, sets, dynamic_offset_count, dyn_out);
}
#endif  // DOOMVR_RESEARCH_TOOLS

// ---------------------------------------------------------------------------
// SCRIPTED TEST (2026-08-17).
//
// Written because the test procedures had grown to five and six keypresses
// in a fixed order, to be performed by someone wearing a headset who cannot
// see the keyboard. That is a bad instrument: a mistimed or missed press
// produces a result that looks like data. Runs 14 and 15 both cost a run to
// procedure problems rather than to the thing under test.
//
// So the sequence lives here instead. ONE keypress runs the whole
// experiment, including the r_useSMP step, with the waits measured in
// frames rather than in the owner's patience. Every transition logs, so the
// log alone reconstructs exactly what state the game was in at each moment.
//
// Add new experiments as steps here rather than as instructions to a human.
std::atomic_bool scripted_test_active{false};
std::atomic_uint32_t scripted_test_step{0};
std::atomic_uint32_t scripted_test_frames_in_step{0};
// Frame-root serial sampled at the start of a readiness check, and how many
// times the check has had to wait - see step 0.
std::atomic_uint32_t scripted_test_serial_mark{0};
std::atomic_uint32_t scripted_test_waits{0};
// Which sequence PageUp runs. TRUE (default) = the STEP 8 symmetric stereo
// VIEWING mode: converge and stay live, no captures, no freeze. FALSE = the
// diagnostic sequence (census, chain walk, A/B captures) that localised the
// parallax failure. The viewing mode is the default because the diagnostic
// work is done; the measurement path is kept whole rather than deleted, so
// re-running it is a one-line change and not an archaeology exercise.
std::atomic_bool scripted_test_symmetric{true};
// RUN A (2026-08-18, review P2). TRUE = the short diagnostic: converge,
// DrawParity for one frame, the descriptor census for one doubled call (it
// is what gates ComputeProbe and therefore the StorageImageID identity
// lines), summaries, stop. No chain walk, no A/B, NO PPMs and no long
// freeze.
//
// It takes precedence over scripted_test_symmetric, and it has to: that
// flag has NO toggle anywhere in this file - no hotkey, no cvar, nothing -
// so with it hard-true the diagnostic sequence was UNREACHABLE and any new
// step added to it would have been built, committed, and silently never
// executed. That is the same class of error as P0's stale binary: a fix
// sitting switched off looks exactly like a fix that was never written.
// Set this false to get the symmetric viewing mode back.
//
// 2026-08-18: Run A is DONE - it completed in 3.6 s and its table answered
// P3 (full draw/dispatch parity; the loss is in CONTENTS). Back to false so
// PageUp is the viewing mode again, now carrying the corrected stereo sign.
std::atomic_bool scripted_test_run_a{false};

void arm_frame_root_double(bool ab) {
    // FORCE THE MEASUREMENT STATE HERE, DO NOT ASK FOR IT (2026-08-19).
    //
    // The owner cannot read a log or verify a toggle from inside a headset, so
    // "press F11 and check it says ON" is not a workable instruction and never
    // was. Anything a run depends on is set in code at arm time, where it is
    // deterministic regardless of what any previous press left behind.
    //
    // suppress_doubled_light_culling defaults true, but a toggle earlier in
    // the SAME process leaves it false, and that difference silently changes
    // what a capture means. Forced here so the run always STARTS in arm A.
    //
    // ⚠ THE CLAIM THAT USED TO BE IN THIS COMMENT WAS WITHDRAWN (2026-08-21).
    // It read: "Measured both ways this session (pass-25 delta +0.0002 ON,
    // +0.0001 OFF - no difference)", and that sentence is what removed light
    // culling from the candidate list. It cannot carry the weight:
    //   * +0.0002 and +0.0001 are not two outcomes. They are two readings of
    //     an instrument that detected nothing in EITHER arm. A null result in
    //     both arms of an A/B is instrument failure, never elimination.
    //   * it was read at pass 25. The zeros this hypothesis is about were
    //     measured at passes 18/22/24 (13:06 run, binding 10/2/6).
    // The suppression is therefore back under test, both arms driven from
    // step 35 by the LightCullAB block, with a positive control attached.
    suppress_doubled_light_culling.store(true, std::memory_order_relaxed);
    // The A/B only becomes live at step 35, where the instruments it labels
    // are armed. Cleared here so an arm that never reaches step 35 cannot flip
    // the suppression behind a measurement that is not recording.
    light_cull_ab_active.store(false, std::memory_order_relaxed);
    // SHADOW-ATLAS SUPPRESSION RESTORED TO ON (2026-08-20).
    //
    // It was forced OFF on 2026-08-19 for one experiment, on the premise that
    // the RenderDoc capture which showed the doubled call producing FULL
    // effects had been armed with Pause (FrameRootDouble Stage 1) and was
    // therefore UNSUPPRESSED - making shadow suppression the one untested
    // difference between that capture and the effect-less XR runs.
    //
    // THAT PREMISE IS FALSE, and the capture's own log says so. The 08:50 run
    // was a PAGEUP arm, and at 08:50:27 it logged "shadow-atlas suppression
    // is ON (suppressedShadowDraws so far=64092)". The configuration in which
    // the mirror demonstrably carried the full spark shower, fire, haze and
    // correct parallax had this suppression ON, not off. Forcing it OFF moved
    // the build AWAY from the only known-good configuration.
    //
    // The experiment has now been run twice with it OFF (10:44 and 11:03
    // 2026-08-20) and is a clean negative: the like-for-like pair
    // finalstage_eye0_control_N vs finalstage_eye1_graded_N shows eye 1's
    // bright population pinned at ~16 000 px>200 across four samples spanning
    // 15 s while DOOM's own swings 18 000 -> 76 000 with the sparks. Eye 1
    // has no dynamic additive layer at all with this off.
    //
    // So: back to ON. It is also the declared default at the definition site,
    // the shipping configuration, and the state that keeps EYE 0's shadows
    // correct - the doubled call re-rendering the shared 16384x16384 atlas
    // with a shifted camera is what caused the "odd shadow effects when
    // moving the mouse" report, and that cost is now paid for nothing.
    //
    // Forced rather than toggled, per the standing rule that the owner cannot
    // verify a keypress from inside a headset. FORCED OFF as of 2026-08-28 -
    // this line is why flipping the declaration's default alone would have done
    // nothing, and it is exactly the trap of an arm-time force silently
    // overriding a default. See suppress_doubled_shadow_draws for the live A/B
    // that made it S3's cause rather than its cure.
    suppress_doubled_shadow_draws.store(false, std::memory_order_relaxed);
    // TEST B. Forced here, at arm time, with the same reasoning as everything
    // else in this function: the owner cannot verify a toggle from outside the
    // game, so the run must START in the state it is meant to measure. Armed
    // at step 2 rather than at the press, so menu navigation and the step 0
    // readiness gate still run at full speed.
    present_throttle_armed.store(kPresentThrottleEnabled, std::memory_order_relaxed);
    log::warn(std::format(
        "ArmState: PRESENT THROTTLE is {} ({} ms sleep after every present, ~{:.1f} fps "
        "ceiling). THE QUESTION: RenderDoc both loads a layer AND drops the game to a crawl, "
        "and nobody has separated the two. Test A eliminated the layer half - the Khronos "
        "validation layer was demonstrably live (1470 messages) and eye 1's deficit did not "
        "move - but it only cost ~28% of the frame rate, so it never tested 'slow'. This does. "
        "PRIMARY = eye1 vs real px>128 at the first HDR stage, and the final-stage meanLuma "
        "ratio. Read 'PresentThrottle' FIRST: if it does not report hundreds of ms per frame, "
        "the throttle did not take and nothing below it means anything.",
        kPresentThrottleEnabled ? "ON" : "OFF", kPresentThrottleMs,
        1000.0 / static_cast<double>(kPresentThrottleMs)));
    log::warn(std::format(
        "ArmState: RenderDocParity usage widening is {}. This run is testing whether the "
        "RenderDoc layer's habit of adding TRANSFER_SRC|TRANSFER_DST to every image and buffer "
        "is what makes the doubled call render its additive layer - measured discriminator, "
        "layer loaded vs not, same build.",
        renderdoc_parity_usage_enabled() ? "ON (every image and buffer widened)" : "OFF"));
    // RENDERDOC IN-APP API, armed here so the capture key is rebound and the
    // attachment control is stated before anything is judged. Both outcomes
    // are logged: a helper that only spoke when the layer was present could
    // not distinguish "no layer" from "the check never ran".
    rdc_configure_if_present("arm");
    if (g_rdc_api.load(std::memory_order_relaxed) == nullptr) {
        log::warn(std::format(
            "RenderDocAPI (arm): NOT PRESENT - renderdoc.dll is not loaded in this process "
            "(resolveAttempts={}). Normal and expected for an ordinary run. FOR A CAPTURE "
            "SESSION IT MEANS THE LAYER NEVER ATTACHED: the capture key was not rebound, no "
            "capture can be taken, and nothing this run measures is the RenderDoc arm. The "
            "resolve is retried every 180 presents, so a later 'RenderDocAPI (retry): PRESENT' "
            "line supersedes this one.",
            g_rdc_resolve_attempts.load(std::memory_order_relaxed)));
    }
    log::warn("ArmState: light-culling suppression FORCED ON; shadow-atlas suppression FORCED "
              "OFF at arm time as of 2026-08-28. The suppression was S3's CAUSE, not its cure: "
              "with it off the shadow sliding stops, A/B'd live in the headset against eye 0 = "
              "DOOM's own presented frame. Suppressing draws never suppressed the render pass, "
              "so the doubled call kept entering DOOM's shared shadow pass (~44 blocked draws a "
              "frame, 2.3M in one session) and leaving the atlas wiped rather than refilled. "
              "The 2026-08-20 note that restored it predates the removal of the corrupting "
              "render-surface hook at DOOM+0x1A367E0 and is on a corrupted baseline. If eye 1's "
              "effects regress, that trade is the first thing to re-examine - SHIFT+F7 A/Bs it.");
    // GRADE-GAP LADDER FORCED TO RUNG 0 AT ARM TIME (2026-08-29), for the same
    // reason as everything else in this function: a second PageUp in the same
    // process must not inherit the rung the previous run was left on. Step 37
    // raises it to 2 later, from code, so the run still starts in the
    // configuration it is meant to measure.
    graded_mode.store(0, std::memory_order_relaxed);
    final_pass_double_armed.store(false, std::memory_order_relaxed);
    graded_stereo_enabled.store(false, std::memory_order_relaxed);
    baseline_pair_burst.store(0, std::memory_order_relaxed);
    kRepairDoubledConstants.store(false, std::memory_order_relaxed);
    // Forced off at arm time like every other toggle, so a second PageUp in
    // the same process cannot inherit mode 4 from the previous run.
    kFogRepairOnly.store(false, std::memory_order_relaxed);
    fog_patch_applied.store(0, std::memory_order_relaxed);
    fog_patch_blocks_seen.store(0, std::memory_order_relaxed);
    fog_patch_no_delta.store(0, std::memory_order_relaxed);
    constant_repair_mode.store(0, std::memory_order_relaxed);
    kFixDoubledLightingVolume.store(false, std::memory_order_relaxed);
    volume_restore_enabled.store(false, std::memory_order_relaxed);
    force_doubled_serialisation.store(false, std::memory_order_relaxed);
    lighting_volume_substitutions.store(0, std::memory_order_relaxed);
    monitor_eye1_direct.store(false, std::memory_order_relaxed);
    monitor_eye1_passes.store(0, std::memory_order_relaxed);
    frame_root_zero_offset.store(false, std::memory_order_relaxed);
    frame_root_blit_only.store(false, std::memory_order_relaxed);
    frame_root_double_null_control.store(false, std::memory_order_relaxed);
    frame_root_double_ab.store(ab, std::memory_order_relaxed);
    frame_root_double_ipd.store(ab ? kFrameRootDoubleIpd : kFrameRootDoubleLiveIpd,
                                std::memory_order_relaxed);
    frame_root_double_cap.store(ab ? kMaxFrameRootDoubleAttempts : kFrameRootDoubleLiveCap,
                                std::memory_order_relaxed);
    frame_root_double_attempts.store(0, std::memory_order_relaxed);
    mirror_serial_log_count.store(0, std::memory_order_relaxed);
    redirect_extra_call_enabled.store(true, std::memory_order_relaxed);
    redirect_attempt_count.store(0, std::memory_order_relaxed);
    frame_root_double_enabled.store(true, std::memory_order_relaxed);
}

// THE ONLY WRITER of graded_mode, final_pass_double_armed and
// graded_stereo_enabled on the step-37 path and on SHIFT+F4 (2026-08-29).
// Legacy step 35 still sets the two booleans itself, deliberately: it arms the
// whole instrument battery this function force-disarms, so routing it through
// here would silently undo its own purpose. Step 35 stays unreachable from the
// symmetric path.
//
// Every rung forces the WHOLE state in code, so a run starts in the
// configuration it is meant to measure and no earlier press can leave a flag
// somewhere the log does not say. It also force-DISARMS step 35's instrument
// battery on every call: the census, P1, the PPM series and the effects-stage
// dumps are what freeze the game for seconds at a time and what confounded
// the runs this step exists to replace. Arming the grade must never drag them
// along, and asserting that here means it cannot happen by accident either.
//
// Nothing here is a measurement. The verification is deliberately separate -
// see the GradedArmCheck block in scripted step 38 - because a press is not
// evidence that a mode engaged.
void apply_graded_mode(uint32_t mode, const char* why) {
    if (mode > 2) mode = 0;
    graded_mode.store(mode, std::memory_order_relaxed);
    final_pass_double_armed.store(mode >= 1, std::memory_order_relaxed);
    graded_stereo_enabled.store(mode >= 2, std::memory_order_relaxed);

    // STEP 35'S BATTERY, FORCED OFF. Listed individually rather than behind a
    // single flag because they are armed individually, and a group switch that
    // misses one is exactly how a "log-only" run ended up dumping 26
    // full-resolution readbacks per sample.
    final_pass_dump_pending.store(false, std::memory_order_relaxed);
    final_pass_dump_ready.store(false, std::memory_order_relaxed);
    final_pass_dump_rearm_at.store(0, std::memory_order_relaxed);
    effects_series_rearm_at.store(0, std::memory_order_relaxed);
    effects_stage_dump_armed.store(false, std::memory_order_relaxed);
    effects_stage_dump_ready.store(false, std::memory_order_relaxed);
    fog_hunt_armed.store(false, std::memory_order_relaxed);
    storage_buffer_probe_armed.store(false, std::memory_order_relaxed);
    uniform_diff_armed.store(false, std::memory_order_relaxed);
    light_cull_ab_active.store(false, std::memory_order_relaxed);
    pass_state_parity_armed.store(false, std::memory_order_relaxed);
    pass_timeline_armed.store(false, std::memory_order_relaxed);
    // The census owns the job filter while it runs, so disarming it without
    // putting mirror_unfiltered back would leave the filter down for the rest
    // of the run - a different render, silently.
    if (descriptor_census_enabled.exchange(false, std::memory_order_relaxed)) {
        mirror_unfiltered.store(
            descriptor_census_restore_unfiltered.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        log::warn("GradedMode: DescriptorCensus was armed and has been force-disarmed; the job "
                  "filter is restored. Nothing in this step wants it.");
    }

    // Counters zeroed so "did it engage SINCE THIS ARM" is answerable. A
    // total that was already non-zero before the press cannot answer it, and
    // that is the whole class of mistake this project keeps paying for.
    final_pass_doubles.store(0, std::memory_order_relaxed);
    final_pass_refused_no_mirror.store(0, std::memory_order_relaxed);
    // ENTRIES counted separately from WRITES: final_stage_calls is the hook's
    // own liveness (the real call), final_pass_doubles is the second
    // invocation. hookLive with doubles=0 is a refusal; both zero is a dead
    // hook. They must never be read as one number.
    final_stage_calls.store(0, std::memory_order_relaxed);
    baseline_pair_burst.store(8, std::memory_order_relaxed);

    static const char* kRung[] = {
        "0 OFF (POSITIVE CONTROL / ROLLBACK) - the final pass runs ONCE, exactly as in the "
        "confirmed eye-0 config. Eye 1 keeps its ungraded frame-root mirror (fmt 122). Eye 0 "
        "MUST be clean here; if it is not, the run is void rather than informative",
        "1 DOUBLE ONLY - the final pass is re-invoked into our mirror, but eye 1 is still "
        "presented the UNGRADED mirror. Eye 1 should look identical to rung 0. ⚠ NOT A CLEAN "
        "SINGLE-VARIABLE STEP: final_pass_double_armed ALSO enables the MirrorLockstep barrier "
        "append (vkCmdPipelineBarrier), so rung 0 -> 1 adds re-entry AND extra barriers on our "
        "mirrors. Both are 'what the second invocation costs', which is the question, but do "
        "not report an eye-0 change here as re-entry alone",
        "2 GRADED (THE STATE UNDER TEST) - re-invoked AND presented: eye 1 = the doubled final "
        "stage's mirror, graded + HUD, fmt 44. Expect BaselinePair toneMatch=true. An eye-0 "
        "change that appears ONLY here is presentation/layout, not re-entry"};
    vwc_arm(std::format("graded mode {}", mode).c_str());
    log::warn(std::format(
        "GradedMode -> {} ({}). ArmState forced here: finalStageHookInstalled={} "
        "mirrorsConverged={} floorRung={} shadowSuppression={} lightCullSuppression={} "
        "gunUnshift={} symmetricStereo={} | xrPresentPathHits={} finalPassMirror={}. "
        "⚠ xrPresentPathHits=0 means the HEADSET SUBMISSION PATH HAS NEVER RUN, and "
        "graded stereo is read ONLY there - a run in that state cannot judge this mode at all, "
        "in either direction. SHIFT+F4 cycles the rungs live.",
        mode, kRung[mode], final_stage_hook_installed.load(std::memory_order_relaxed),
        mirrors_converged(), doubled_floor_level.load(std::memory_order_relaxed),
        suppress_doubled_shadow_draws.load(std::memory_order_relaxed)
            ? "ON"
            : "OFF (shipping, load-bearing for eye 0 shadows)",
        suppress_doubled_light_culling.load(std::memory_order_relaxed) ? "ON" : "OFF",
        gun_unshift_enabled.load(std::memory_order_relaxed),
        symmetric_stereo_enabled.load(std::memory_order_relaxed),
        xr_present_path_hits.load(std::memory_order_relaxed),
        reinterpret_cast<void*>(final_pass_mirror_color)));
}

// Returns true while the test is still running.
void drive_scripted_test() {
    if (!scripted_test_active.load(std::memory_order_relaxed)) return;
    const uint32_t step = scripted_test_step.load(std::memory_order_relaxed);
    const uint32_t frames = scripted_test_frames_in_step.fetch_add(1, std::memory_order_relaxed);
    // ELAPSED TIME IN THIS STEP, because a FRAME COUNT is not a delay when the
    // frame rate is the thing that changed (2026-08-20).
    //
    // Step 35 waits for 240 frames after step 4. With a diagnostic build
    // running at ~11 fps that is 22 seconds, on top of the ~18 s the earlier
    // steps take - so a 25-second run reached step 4 and stopped there, armed
    // nothing, dumped nothing, and looked exactly like a failed fix. That cost
    // a run and a good deal of the owner's patience.
    //
    // Steps that are settle delays now satisfy EITHER the frame count OR a
    // wall-clock equivalent at 60 Hz, so a slow build waits the same number of
    // SECONDS a fast one does.
    static uint32_t step_started_ms = 0;
    static uint32_t last_step_seen = UINT32_MAX;
    const uint32_t now_ms = GetTickCount();
    if (step != last_step_seen) { last_step_seen = step; step_started_ms = now_ms; }
    const uint32_t elapsed_ms = now_ms - step_started_ms;
    const auto settled = [&](uint32_t want_frames) {
        return frames >= want_frames || elapsed_ms >= (want_frames * 1000u) / 60u;
    };
    const auto advance = [&](uint32_t next) {
        scripted_test_step.store(next, std::memory_order_relaxed);
        scripted_test_frames_in_step.store(0, std::memory_order_relaxed);
        last_step_seen = UINT32_MAX;  // force the timer to restart on the next step
    };
    switch (step) {
    case 0: {
        // READINESS GATE. Nothing touches r_useSMP or arms doubling until
        // the per-view frame root is demonstrably running once per present -
        // i.e. the world is actually being rendered.
        //
        // Without this, a press in a menu, on a loading screen, or during a
        // level transition would force the render pipeline inline and arm
        // doubling against a world that is not rendering. The owner asked
        // for this explicitly and was right to: the press stays theirs, but
        // WHEN the sequence acts is now decided by evidence rather than by
        // whatever happened to be on screen at the moment of the press.
        //
        // g_frame_root_serial increments once per REAL per-view frame root
        // entry (the same counter the same-frame tag uses), so serial
        // advancing in step with presents means world rendering is live.
        const uint32_t serial = g_frame_root_serial.load(std::memory_order_relaxed);
        if (frames == 0) {
            scripted_test_serial_mark.store(serial, std::memory_order_relaxed);
            log::warn("ScriptedTest step 0: waiting for the world to render before touching "
                      "anything (safe to sit in a menu or a load screen here)");
            break;
        }
        if (frames >= 60) {
            const uint32_t advanced =
                serial - scripted_test_serial_mark.load(std::memory_order_relaxed);
            if (advanced >= 50) {
                log::warn(std::format(
                    "ScriptedTest step 0: world is rendering ({} frame-root entries in {} "
                    "presents) - starting", advanced, frames));
                advance(1);
            } else {
                // Not rendering yet. Re-mark and keep waiting, quietly.
                scripted_test_serial_mark.store(serial, std::memory_order_relaxed);
                scripted_test_frames_in_step.store(1, std::memory_order_relaxed);
                const uint32_t waited =
                    scripted_test_waits.fetch_add(1, std::memory_order_relaxed);
                if (waited % 10 == 0) {
                    log::warn(std::format(
                        "ScriptedTest step 0: still waiting - only {} frame-root entries in {} "
                        "presents. In a menu or loading? The test will start itself once the "
                        "world is rendering.", advanced, frames));
                }
            }
        }
        break;
    }
    case 1: {
        // Put the render pipeline inline. Doubling refuses to arm otherwise,
        // and this is what PageDown used to be for.
        const uintptr_t doom_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"DOOMx64vk.exe"));
        int32_t use_smp = -1;
        if (doom_base) safe_read_bytes(doom_base + kUseSmpCvarRva + 0x30, &use_smp, sizeof(use_smp));
        if (use_smp == 2) toggle_force_single_threaded();
        log::warn(std::format("ScriptedTest step 1: r_useSMP was {}, forcing inline", use_smp));
        // P1W ARMS HERE, roughly ten seconds ahead of the frame P1 measures.
        // Run 4 armed it at step 35 and the table emitted 23 ms later, so the
        // window was a few milliseconds and it captured nothing - a blind
        // probe that would have read as "no write observed" for every set.
        // Costs one relaxed atomic load per descriptor write until then.
        {
            std::scoped_lock lock(p1_write_mutex);
            p1_writes.clear();
            p1_write_keys.clear();
        }
        p1_writes_dropped.store(0, std::memory_order_relaxed);
        p1_write_calls.store(0, std::memory_order_relaxed);
        p1_write_in_range.store(0, std::memory_order_relaxed);
        p1_write_probe_armed.store(true, std::memory_order_relaxed);
        // V1 ARMS HERE, on the same press, for the same reason P1W does: the
        // capture must already be running when the doubled dispatch happens.
        // It REFUSES TO ARM if the decoder cannot round-trip a synthetic
        // record and reject two invalid ones - an unarmed probe prints empty
        // rows that read exactly like "the job never dispatched".
        {
            for (uint32_t j = 0; j < kV1Jobs; ++j) {
                for (uint32_t s = 0; s < kV1Sides; ++s) {
                    v1_kept[j][s].store(0, std::memory_order_relaxed);
                    v1_seen[j][s].store(0, std::memory_order_relaxed);
                }
            }
            v1_emitted.store(false, std::memory_order_relaxed);
            std::string v1_detail;
            const bool v1_ok = v1_selftest(v1_detail);
            v1_selftest_passed.store(v1_ok, std::memory_order_relaxed);
            v1_armed.store(v1_ok, std::memory_order_relaxed);
            // C1 arms with V1, same press, same reasoning.
            for (uint32_t j = 0; j < kV1Jobs; ++j)
                for (uint32_t s = 0; s < kV1Sides; ++s)
                    c1_taken[j][s].store(0, std::memory_order_relaxed);
            std::string c1_detail;
            const bool c1_ok = c1_selftest(c1_detail);
            c1_selftest_passed.store(c1_ok, std::memory_order_relaxed);
            c1_armed.store(c1_ok, std::memory_order_relaxed);
            log::warn(std::format(
                "C1 SELFTEST {}: {}. {}", c1_ok ? "PASS" : "FAIL", c1_detail,
                c1_ok ? "C1 ARMED at step 1 - snapshotting the shared context's declaration "
                        "slots before AND after the first dispatches of both jobs, both sides."
                      : "C1 NOT ARMED - the resolver failed its own round-trip, so no C1 rows "
                        "will appear and their absence means nothing."));
            // W1 ARMS HERE, with P1W, because it rides p1_capture_write and
            // must be recording before the frame whose descriptors P1 reads.
            {
                std::scoped_lock lock(w1_mutex);
                for (uint32_t i = 0; i < kW1MaxChains; ++i) w1_chains[i] = W1Chain{};
            }
            w1_count.store(0, std::memory_order_relaxed);
            w1_seen.store(0, std::memory_order_relaxed);
            w1_dropped.store(0, std::memory_order_relaxed);
            w1_backtrace_failures.store(0, std::memory_order_relaxed);
            for (uint32_t s = 0; s < kV1Sides; ++s) c3_view[s].store(0, std::memory_order_relaxed);
            w1_armed.store(true, std::memory_order_relaxed);
            log::warn("W1 ARMED at step 1 - stack backtrace at every binding-2 descriptor write, "
                      "deduped by chain, both sides. Read 'W1WriteSite' AFTER 'P1W'.");
            // C2 rides C1's dispatch bracket and needs C1 armed to fire.
            for (uint32_t j = 0; j < kV1Jobs; ++j)
                for (uint32_t s = 0; s < kV1Sides; ++s)
                    c2_taken[j][s].store(0, std::memory_order_relaxed);
            c2_armed.store(c1_ok, std::memory_order_relaxed);
            log::warn(std::format(
                "C2 {} - full context slot table ({} slots) captured AFTER the first dispatch of "
                "each job on each side, then diffed and named from DOOM's own declaration list.",
                c1_ok ? "ARMED at step 1" : "NOT ARMED (C1's self-test failed; C2 rides its "
                                            "dispatch bracket and cannot fire without it)",
                kC2MaxSlots));
            // F1 ARMED (2026-08-26 run 9). This is the CAUSAL test of the
            // mechanism E1 confirmed. C2/C3/P1W stay armed deliberately: they
            // are the in-band pass/fail (slot 1056 should stop matching slot
            // 1057 on the doubled side, and P1W's doubled binding 2 should
            // become VOL), and every one of them has a baseline from runs 7-8
            // in the same scene. The fog metric is the secondary judge.
            // G1 ARMED, F1 OFF (2026-08-26 run 10). F1's single-dword restore
            // is superseded: the job drains per-system state too, and restoring
            // all of it would be memory surgery across an array. Suppressing
            // the redundant re-invocation is the smaller and more honest
            // intervention. Running F1 alongside would confound both.
            // ⚠ G1 TURNED OFF (2026-08-26 run 18). It was left armed from run 10
            // as a "correctness fix that moves no metric", and H1/C2/M1 were
            // then built and read UNDERNEATH it for six runs. That reasoning was
            // wrong: not moving the FOG metric does not mean not moving the
            // PER-TARGET census. Run 17 made it obvious - every gpuParticle*
            // slot (328-353) reads zero on the doubled side because G1 stops the
            // job that writes them, and a bloom pyramid fed by bright particles
            // is exactly the kind of thing suppressing particle simulation would
            // empty. The pyramid emptiness of runs 11-16 has therefore NEVER
            // been measured without our own intervention running.
            //
            // The symptom itself predates G1 (dark30 ~111% in runs 4-9), so the
            // deficit is real. The per-target map needs a clean baseline.
            g1_suppress_particle_sim.store(false, std::memory_order_relaxed);
            g1_suppressed.store(0, std::memory_order_relaxed);
            // H1 arms here and fires once, on the present that takes the
            // final-stage snapshot. G1 stays ON underneath it as a correctness
            // fix; run 10 showed it moves no metric, so it cannot confound the
            // per-target map.
            h1_done.store(0, std::memory_order_relaxed);
            h1_pending.store(false, std::memory_order_relaxed);
            h1_armed.store(true, std::memory_order_relaxed);
            log::warn("H1 ARMED at step 1 - per-target census of EVERY mirror/source pair, once, "
                      "on the present thread. It WILL hitch for several seconds when it fires.");
            log::warn(std::format(
                "G1 IS {} FOR THIS RUN. {}",
                g1_suppress_particle_sim.load(std::memory_order_relaxed) ? "ON" : "OFF",
                g1_suppress_particle_sim.load(std::memory_order_relaxed)
                    ? "CAUSAL: the doubled dispatch of GPU Particle Simulate is SUPPRESSED. "
                      "EXPECTED SIDE EFFECTS: C1/C2/C3 ride that dispatch so their ParticleSim "
                      "doubled rows are absent, AND every gpuParticle* context slot reads zero "
                      "on the doubled side because the job that writes them did not run. Neither "
                      "is a finding."
                    : "BASELINE: nothing is suppressed. Runs 11-16 built and read H1/C2/M1 with "
                      "G1 armed underneath them, so this is the first CLEAN per-target map. If "
                      "the HDR pyramid is still empty here, the deficit is real; if it fills, "
                      "runs 11-16 were measuring our own intervention."));
            f1_enabled.store(false, std::memory_order_relaxed);
            f1_attempts.store(0, std::memory_order_relaxed);
            f1_restores.store(0, std::memory_order_relaxed);
            f1_failures.store(0, std::memory_order_relaxed);
            f1_no_saved.store(0, std::memory_order_relaxed);
            f1_saved_count.store(0, std::memory_order_relaxed);
            log::warn(std::format("F1 IS {} FOR THIS RUN.",
                f1_enabled.load(std::memory_order_relaxed)
                    ? "ON - restoring [0x705CA24] before each doubled dispatch"
                    : "OFF - superseded by G1; run 9 proved the single-dword restore "
                      "insufficient"));
            // D1 RETURNED TO OFF (2026-08-26 run 6). Run 5 forced the timestep
            // on 400 dispatches with read-back proof and moved no metric, so
            // leaving it on would put an ineffective behavioural change under
            // W1's diagnostic without buying anything.
            d1_enabled.store(false, std::memory_order_relaxed);
            d1_attempts.store(0, std::memory_order_relaxed);
            d1_writes.store(0, std::memory_order_relaxed);
            d1_write_failures.store(0, std::memory_order_relaxed);
            d1_last_real_delta.store(0, std::memory_order_relaxed);
            log::warn(std::format("D1 IS {} FOR THIS RUN.",
                d1_enabled.load(std::memory_order_relaxed) ? "ON - this is a CAUSAL run"
                                                           : "OFF - this is a LOG-ONLY run"));
            log::warn(std::format(
                "V1 SELFTEST {}: {}. {}", v1_ok ? "PASS" : "FAIL", v1_detail,
                v1_ok ? "V1 ARMED at step 1 - capturing jobArg at the deferred lighting job "
                        "(RVA 0x19AE9E0) on both sides until the P1 table emits."
                      : "V1 NOT ARMED - the decoder failed its own round-trip, so no V1 rows "
                        "will appear and their absence means nothing."));
        }
        log::warn("P1W ARMED at step 1 - capturing image descriptor writes until the P1 table "
                  "emits. Read 'P1W CAPTURE' first: calls=0 means the write hooks never fired in "
                  "this window at all, which is a different answer from 'they fired and nothing "
                  "matched' and Run 4 could not tell the two apart.");
        advance(2);
        break;
    }
    case 2:
        // Give the cvar write a few frames to take effect before arming.
        if (frames >= 30) {
            arm_frame_root_double(false);
            log::warn("ScriptedTest step 2: live doubling ARMED (unfiltered, redirect still off) "
                      "- letting mirrors be created and written");
            advance(3);
        }
        break;
    case 3:
        // The wait that matters: every mirror must exist and have been
        // rendered into before redirection is enabled, because enabling it
        // during the lazy-creation window is what hung the GPU three times.
        //
        // Now waits on the CONDITION rather than on 300 frames. Measured,
        // that wait was ~75x longer than convergence takes (all 53 mirrors in
        // 60 ms) and it protected only the first convergence. The timeout
        // survives purely so a failure to converge is reported instead of
        // hanging the sequence.
        if (mirrors_converged() || frames >= 600) {
            if (!mirrors_converged()) {
                // Rule 3. Enabling anyway would re-create the frame-0 hang;
                // the substitution gate below also refuses, so this is a
                // report, not a silent downgrade.
                log::error(std::format(
                    "ScriptedTest step 3: mirrors did NOT converge in {} frames "
                    "(generation still moving). Redirection is enabled but its own convergence "
                    "gate will keep refusing, so eye 1 will render WITHOUT redirection. Grep "
                    "'MirrorConvergence'.", frames));
            }
            read_redirect_enabled.store(true, std::memory_order_relaxed);
            invalidate_read_redirect_cache("scripted test enabling redirection");
            log::warn(std::format(
                "ScriptedTest step 3: read redirection ENABLED after {} frames of live doubling "
                "(mirrorGeneration={})", frames,
                mirror_generation.load(std::memory_order_relaxed)));
            // DrawParity (2026-08-18, review P2) runs FIRST on the diagnostic
            // path - it is the run's existential measurement and it wants a
            // frame with redirection on and no other instrument perturbing
            // it. The symmetric viewing path skips it: that mode exists to be
            // looked at, not measured.
            // CallerSpecificity rides in front of both paths (2026-08-18). It
            // is log-only, costs about a second, and answers the question that
            // the refuted FinalPass hook made urgent: which caller, if any, is
            // unique to the swapchain-writing pass. Doing it here means the
            // owner's ordinary PageUp collects it without a dedicated run.
            advance(34);
        }
        break;
    case 35:
        // FINAL STAGE DOUBLING. Armed a few seconds AFTER symmetric stereo has
        // gone live, deliberately: the owner sees the known-good stereo first,
        // so if the picture changes when this arms, the change is attributable
        // to this and to nothing else.
        //
        // NOT wired to presentation in this build. Eye 1 still shows its
        // ungraded composite; the doubled pass's output is captured to a PPM
        // instead. Judging a new render path from the picture BEFORE routing it
        // to a headset is the cheap order to do this in - if the second
        // invocation produces a black or corrupt frame, that costs a file
        // rather than the owner's eyes, and this project has been fooled by a
        // clean log often enough to want the image first.
        if (settled(240)) {
            if (!final_stage_hook_installed.load(std::memory_order_relaxed)) {
                // Rule 3. Without this, "no grade in the PPM" would be
                // indistinguishable from "the hook was never installed" - the
                // exact ambiguity that made the last run unreadable.
                log::error("ScriptedTest step 35: final-stage hook was NOT installed, so the "
                           "doubling is NOT armed. Read 'CallerSpecificity'/'FinalStage HOOK' "
                           "for why. No PPM will be written; do not read its absence as the "
                           "doubling having produced nothing.");
                advance(99);
                break;
            }
            // PHASE-LOCKED TO THE UNIFORM SAMPLES (2026-08-21). This used to
            // fire the first PPM pair immediately, which put the image series
            // ~90 presents ahead of the uniform series and let a PPM pair and
            // a uniform sample with the same index sit in DIFFERENT arms of
            // the A/B. Both series now start at present+90 and both step by
            // 150, so sample N of one is the same moment as sample N of the
            // other. The arm is also written into each artifact's log line, so
            // a drift would be visible rather than silent.
            final_pass_dump_series.store(0, std::memory_order_relaxed);
            // VISUAL-ONLY MODE (2026-08-28). Create C:\dev\doomvr-visual.txt to
            // skip the whole readback chain: the PPM pairs here and, through
            // them, the H1 per-target census (26 full-resolution image
            // readbacks per sample, which is the multi-second freeze the owner
            // hit the first time the picture was worth looking at).
            //
            // The point is that LOOKING and MEASURING now need different runs.
            // Everything else - doubling, mirroring, redirection, stereo
            // presentation - is untouched, so this is the same render with the
            // instruments silent.
            if (!diagnostics_readbacks_enabled()) {
                log::warn("VisualOnly: C:\\dev\\doomvr-visual.txt present - PPM dumps and the H1 "
                          "census are SKIPPED for this run. Stereo is unaffected. Delete the file "
                          "to get measurements back.");
            } else {
                final_pass_dump_rearm_at.store(
                    presented_frame.load(std::memory_order_relaxed) + 90,
                    std::memory_order_relaxed);
            }
            // DESCRIPTOR CENSUS, armed here for the first time (2026-08-20).
            //
            // THE ONE UNMEASURED LINK. Everything else in the chain is
            // confirmed: the doubled call DRAWS the effects (DrawParity,
            // identical counts), into MIRRORED colour and depth targets, from
            // CORRECT inputs (identical froxel-volume handles), with REPAIRED
            // constants - and its additive composite is still 47 against
            // DOOM's 11 490. What has never been measured is what those
            // effects passes actually SAMPLE: whether each input resolves to
            // one of our mirrors, to DOOM's real image, or to nothing we have
            // a record of.
            //
            // The census answers exactly that, per pass, and has never run in
            // a single log this session - it was only reachable from the dead
            // SHIFT+PageUp branch and from a scripted step the PageUp path
            // skips. It is log-only: no image dumps, no readbacks. It
            // self-disarms after ONE complete doubled call and restores the
            // job filter itself, so it costs one frame.
            //
            // Read 'DescCensusPass' for the per-pass table. For the effects
            // passes, mirror=0 with shared/unknown high means the doubled call
            // is compositing DOOM's real images rather than its own, and
            // noRecord high means we never saw those sets written and cannot
            // redirect them at all.
            descriptor_census_restore_unfiltered.store(
                mirror_unfiltered.load(std::memory_order_relaxed), std::memory_order_relaxed);
            descriptor_census_lines.store(0, std::memory_order_relaxed);
            descriptor_census_real_lines.store(0, std::memory_order_relaxed);
            descriptor_census_mirror_lines.store(0, std::memory_order_relaxed);
            census_miss_lines.store(0, std::memory_order_relaxed);
            census_miss_keying.store(0, std::memory_order_relaxed);
            census_miss_nomirror.store(0, std::memory_order_relaxed);
            census_miss_noimage.store(0, std::memory_order_relaxed);
            census_miss_seen.clear();
            mirror_unfiltered.store(true, std::memory_order_relaxed);
            descriptor_census_enabled.store(true, std::memory_order_relaxed);
            // P1 READ CLASSIFICATION (2026-08-23) - the missing half of the
            // working-arm comparison. Armed here because the job filter is
            // already down for the census (the lighting job is exactly what
            // the filter skips) and because redirection has been live and
            // converged for seconds by this point. One frame, log-only, self-
            // disarming; it waits for a converged frame rather than spending
            // the arm inside a MirrorConvergence LOST window.
            p1_done.store(false, std::memory_order_relaxed);
            p1_armed.store(true, std::memory_order_relaxed);
            log::warn(std::format(
                "P1 ARMED for one converged frame. THE QUESTION: at the deferred lighting job "
                "({:#x}) the WORKING arm's doubled call reads EIGHT mirror-chain images where "
                "the real call reads their real-chain counterparts (rdMIR=8 rdREAL=1 against "
                "rdMIR=0 rdREAL=9). No broken-arm equivalent has ever been measured and none "
                "can come from a capture, because taking one requires RenderDoc and with "
                "RenderDoc loaded eye 1 is correct. Grep 'P1ReadTable', then read 'P1 CONTROL' "
                "BEFORE the verdict: the real side must show rdMIR=0 with rdREAL>0 or the "
                "classifier is broken and the rows mean nothing.",
                kP1ParticleSimJobRva));
            // EFFECTS-STAGE AUTO SERIES OFF BY DEFAULT (2026-08-20).
            //
            // It is the reason the last two runs hitched badly. Each sample
            // dumps ten FULL-RESOLUTION images, and each dump drains the queue
            // and writes a 23 MB PPM; four samples is 40 dumps, ~1 GB of
            // mid-frame disk writes, on top of the finalstage series' 12. The
            // owner reported "tons of hitches", and they were right.
            //
            // The stage tables it produces are already captured for both
            // configurations (11:23 without RenderDoc, 12:25 with), so this
            // costs nothing to leave off. F2 still arms it on demand for one
            // sample when a fresh stage table is actually wanted.
            if (kEffectsAutoSeriesEnabled) {
                effects_series_rearm_at.store(
                    presented_frame.load(std::memory_order_relaxed) + 60,
                    std::memory_order_relaxed);
            }
            // UNIFORM DIFF AND THE LIGHT-CULLING A/B ARE BOTH OFF FOR THIS RUN
            // (2026-08-21). Their question is answered - see
            // docs/RESULT-2026-08-21-LIGHT-CULLING-REFUTED.md: HOLEStotal=0 in
            // both arms, byte-identical across the flip, with four positive
            // controls passing. Leaving them armed would cost a slow captured
            // frame per sample and put a second variable in a run that exists
            // to measure ONE thing.
            //
            // The machinery is kept whole rather than deleted - re-arming it
            // is these three lines - because the LightBlocks population
            // measure is the instrument that found the 16-float record stride,
            // and it is the right tool for any future question about what the
            // two calls' constant blocks contain.
            uniform_diff_armed.store(false, std::memory_order_relaxed);
            // ⚠ THE LIGHT-CULLING A/B IS OFF FROM 2026-08-27, AND THIS IS A
            // CORRECTION, NOT A PREFERENCE.
            //
            // It was armed here and flipped suppression to OFF from inside the
            // effects-stage dump, roughly THREE SECONDS after this step. The
            // FinalStage snapshot - the thing that sets h1_pending and
            // therefore triggers the entire H1 per-target census - lands about
            // 5.6 s after this step. So for the whole 08-26 session:
            //
            //   * every H1 census row, including uavSSDO's 20.1% and the
            //     "unwritten" 59x31 and 2x2 targets, and
            //   * every final-stage band number, i.e. the 78.6-82.1% meanLuma
            //     and 110.6-111.3% dark30 that six runs called "untreated"
            //
            // were measured with suppress_doubled_light_culling ALREADY OFF.
            // That is not an inference: the snapshot line reads the live atomic
            // and every one of the last six runs logs "FinalStage SNAPSHOT
            // series=0 suppressLightCulling=OFF".
            //
            // Two consequences. First, a run that flips an arm three seconds
            // before its own primary measurement is not an A/B, it is one arm
            // with a misleading label - disarmed here so the whole run sits in
            // ONE state. Second, the state it sat in was OFF, so OFF is what
            // this run pins: it is the configuration every historical band
            // number belongs to, and R2 has to be judged against a baseline
            // taken in the same configuration or the comparison is worthless.
            //
            // Forced in code at arm time, per the standing rule - the owner
            // cannot verify a toggle from inside a headset.
            light_cull_ab_active.store(false, std::memory_order_relaxed);
            light_cull_ab_flips.store(0, std::memory_order_relaxed);
            // ===== PIN FLIPPED TO **ON** (2026-08-25) =====================
            //
            // This used to pin the suppression OFF for the whole run. That was
            // a MEASUREMENT-COMPARABILITY decision from the 08-26 H1/band
            // analysis - historical numbers had accidentally been taken with it
            // off, so the step pinned the run to match them. It was never a
            // rendering decision, and it has been costing the picture ever
            // since.
            //
            // WHAT IT COST, and the owner reported it themselves on 2026-08-21:
            // sparks in the first room render flat with a compressed, warped
            // trajectory. The gate is doubled_dispatch_writes_shared_buffer, so
            // OFF lets the doubled call re-run every compute dispatch carrying
            // a storage buffer - GPU PARTICLE SIMULATION AND EMITTER STATE, not
            // just light lists - and rewrite DOOM's SHARED buffers. The F1
            // handler says it outright: "expect eye 0's sparks to degrade -
            // that is the known trade".
            // Preserved as a positive control in
            // RESULT-2026-08-21-LIGHT-CULLING-REFUTED.md §2:
            //   "the owner's screen | sparks visibly warped during arm B"
            //
            // AND IT BOUGHT NOTHING. That same document refutes the
            // light-culling hypothesis with four passing controls, verdict "do
            // not re-open without new evidence". OFF gives eye 1 no effects
            // back, so the pin was pure cost.
            //
            // Forced explicitly rather than simply deleted, per the arm-time
            // discipline: a toggle earlier in the same process (F1) would
            // otherwise leave the run in a state nobody chose.
            //
            // ⚠ The historical 08-26 band and H1 per-target numbers belong to
            // the OFF configuration. They are no longer like-for-like with runs
            // after this change - re-baseline before comparing against them.
            suppress_doubled_light_culling.store(true, std::memory_order_relaxed);
            log::warn(std::format(
                "ArmState: LIGHT-CULLING SUPPRESSION PINNED **ON** FOR THE WHOLE RUN (changed "
                "2026-08-25; it used to pin OFF), and the LightCullAB flip is DISARMED. ON is "
                "the shipping default and the state in which eye 0's sparks are CORRECT. Pinning "
                "it OFF was a measurement-comparability choice for the 08-26 H1/band analysis, "
                "and it cost the owner warped, trajectory-compressed sparks on every PG UP run "
                "while buying eye 1 nothing - the light-culling hypothesis was refuted with four "
                "controls. suppressedDispatches={} at this moment and it SHOULD climb from here; "
                "if it stays frozen the pin did not take. ⚠ The 08-26 band/H1 numbers were taken "
                "with this OFF and are no longer like-for-like.",
                suppressed_light_culling_dispatches.load(std::memory_order_relaxed)));
            // R1 DISPATCH FATE MANIFEST. Armed here, next to the census, for
            // the same two reasons the census is armed here: redirection has
            // been live and converged for seconds, and the job filter is down.
            // One real call and one doubled call, latched to a single frame
            // root serial, then self-disarming. REFUSES TO ARM if its own
            // self-test does not round-trip - an empty manifest reads exactly
            // like "the doubled call issues no dispatches", which is the
            // silent-probe failure this project has already paid for.
            // X1 - RING-SLOT REPLAY, ARMED. THIS RUN IS CAUSAL.
            //
            // Full reasoning at the top of hook_cmd_bind_descriptor_sets. In
            // one line: seven runs closed execution, targets, resources,
            // layouts, grids, parity and suppression, and the only input left
            // differing between the real and doubled calls is WHICH constant
            // slot of DOOM's per-call ring each one reads. This points the
            // doubled call at the real call's slots.
            x1_replayed.store(0, std::memory_order_relaxed);
            x1_missing.store(0, std::memory_order_relaxed);
            x1_count_mismatch.store(0, std::memory_order_relaxed);
            x1_replay_ring_offsets.store(kX1ReplayRingSlots, std::memory_order_relaxed);
            log::warn(std::format(
                "X1 RING-SLOT REPLAY is {}. THIS IS A CAUSAL RUN{}\n"
                "  WHAT TO LOOK FOR WITH YOUR OWN EYES: fog, distant fire, sparks and haze "
                "returning to the RIGHT eye. That is the primary judge this time, not a ratio.\n"
                "  EXPECTED COST IF IT WORKS: the real call's slot carries the REAL camera, so "
                "eye 1 may lose its parallax and look like a copy of eye 0's viewpoint. Effects "
                "back + parallax gone is a WIN, not a failure: it localises the fault to the "
                "slot CONTENTS and the remaining work becomes filling our own slot correctly.\n"
                "  POSITIVE CONTROL: 'X1 Summary' must report replayed>0. Zero means the lever "
                "never fired and the run says nothing either way.\n"
                "  SAFETY: same buffer handle, same ranges, offsets the real call itself used "
                "microseconds earlier - so this cannot address memory DOOM has not already "
                "bound. Counts are checked before every substitution and a mismatch refuses.",
                kX1ReplayRingSlots ? "ON" : "OFF",
                kX1ReplayRingSlots ? "" : " - compiled OFF, so this run is log-only."));
            // R5 (2026-08-27). DRAW PARITY, armed on the LIVE path for the
            // first time.
            //
            // R4 refuted the depth hypothesis with its own instrument: eye 1's
            // depth mirror reads 100.6% of DOOM's (mean .8700 vs .8647), and it
            // is the input to passes 15/16 AND to pass 22's b2. So the depth
            // downsample chain reads a correct depth and still writes a mirror
            // that is min=max=mean=1.0000 EXACTLY.
            //
            // An exact uniform value across a whole target is the signature of
            // a CLEAR, not of a computed reduction. The mirror framebuffer is
            // begun (fbMirrored=true), DOOM's clear value applies, and then
            // either the draw does not happen or it covers nothing. DrawParity
            // is the instrument that separates those: it tabulates draws,
            // vertices and indices per pass for the REAL and DOUBLED calls of
            // the SAME frame, and it has only ever run on the diagnostic path
            // (steps 30/31), which the symmetric/graded path skips - so it has
            // never been read for passes 15/16 at all.
            //
            // One frame, log-only, self-latching and self-disarming.
            draw_parity_done.store(false, std::memory_order_relaxed);
            draw_parity_armed.store(true, std::memory_order_relaxed);
            log::warn(
                "DrawParity ARMED for one frame on the LIVE path (new - steps 30/31 are skipped "
                "in graded stereo, so this table has never been read for the depth-downsample "
                "passes). THE QUESTION: passes 15 and 16 of jobRva 0x18733c0 write eye 1's "
                "R16_SFLOAT depth-downsample mirrors as a UNIFORM 1.0 while reading a depth "
                "mirror measured at 100.6% of DOOM's. Compare the DOUBLED and REAL rows AT PASS "
                "15 AND 16: equal draw counts means the draws ran and computed that value, so "
                "the fault is in what they were given or in the shader's constants; DOUBLED "
                "draws=0 with REAL draws>0 means the target is only ever CLEARED, and a uniform "
                "1.0 is simply DOOM's clear value showing through.");
            {
                df_reset_tables();
                std::string df_detail;
                const bool df_ok = df_selftest(df_detail);
                df_selftest_passed.store(df_ok, std::memory_order_relaxed);
                df_armed.store(df_ok, std::memory_order_relaxed);
                log::warn(std::format(
                    "DispatchFate SELFTEST {}: {}. {}", df_ok ? "PASS" : "FAIL", df_detail,
                    df_ok ? "DispatchFate ARMED for ONE frame - per-dispatch execution verdict "
                            "(taken AFTER the suppress-return, unlike ComputeProbe), the "
                            "predicate's kill set printed whether or not it is acting, and the "
                            "per-binding storage-image substitution outcome read back from "
                            "get_redirected_set. Grep 'DispatchFate' and read the CONTROL line "
                            "first."
                          : "DispatchFate NOT ARMED - the predicate or the verdict channel "
                            "failed its own round-trip, so NO 'DispatchFate' rows will appear "
                            "and their absence means nothing about the game."));
            }
            // PIPELINE PARITY, armed BEFORE the A/B flips anything so it
            // measures the shipping configuration - the one the symptom was
            // measured in. One frame, log-only.
            pipeline_parity_at.store(
                presented_frame.load(std::memory_order_relaxed) + 30,
                std::memory_order_relaxed);
            pipeline_parity_armed.store(true, std::memory_order_relaxed);
            log::warn(
                "PipelineParity ARMED for one frame. THE LAST UNTESTED CATEGORY: everything "
                "eye 1's lighting pass is GIVEN is now verified correct - identical draws, "
                "constants, redirected descriptors, all attachments mirrored, DOOM's own render "
                "pass and clear values, full barrier coverage - and it still accumulates 21% of "
                "the bright light while the same code under RenderDoc accumulates 117%. Blend "
                "state lives on the PIPELINE OBJECT, so identical pipeline sets per pass means "
                "identical blend factors and write masks BY CONSTRUCTION. Grep 'PipelineParity'. "
                "DIFFERING=0 on every pass kills the last category and the answer is not in what "
                "eye 1 draws with at all.");
            log::warn(std::format(
                "StageBracketAB ARMED: {} stage samples, one per arm. Sample 0 = suppression ON "
                "(shipping), sample 1 = OFF, flipped in code between them. PRIMARY = eye1's p15 "
                "dark30 against DOOM's at the SAME pass. Broken-arm baseline measured 16:23: "
                "real 32386, eye1 441083 - a 13.6x gap at the FIRST full-res HDR stage, which is "
                "where the additive layer is already missing. Arm B collapsing eye1 toward ~32k "
                "confirms the doubled call's own light culling is what its lighting lacked; "
                "unchanged at ~441k refutes it. ⚠ px>200 IS BLIND HERE - it reads 0 against 0 at "
                "p15, which is why the 15:10 A/B could not answer this.",
                kEffectsSeries));
            log::warn(std::format(
                "SymptomRun ARMED: {} samples, {} presents apart (~{} s at 60 Hz, ~{} s at the "
                "~43 fps these runs actually measure). THE QUESTION: does eye 1 still lose the "
                "additive layer at all? PRIMARY = 'BrightCensus' lines, one per dump, computed "
                "in-process with the same definition as tools/ppmbright.ps1 -Threshold 200. Read "
                "eye1 against the eye0 CONTROL OF THE SAME SAMPLE, never across samples. "
                "VERDICT RULE, fixed now: the control must SWING across the series - if it never "
                "does, the run is void and proves nothing either way, exactly like a spark test "
                "with no sparks on screen. Given a swinging control: eye 1 tracking it means the "
                "additive layer is present and the pinned ~16000 ceiling is gone; eye 1 flat "
                "while the control swings means it is still missing. Check bbox200 too - eye 1 "
                "matching the control's population AND its bounding box would mean eye 1 is a "
                "copy of eye 0's viewpoint, effects restored and parallax destroyed. "
                "Light-culling suppression is ON (shipping); UniformDiff and the A/B are OFF, "
                "their question is answered.",
                kFinalPassDumpSeries, kFinalPassDumpGapFrames,
                (kFinalPassDumpSeries * kFinalPassDumpGapFrames) / 60,
                (kFinalPassDumpSeries * kFinalPassDumpGapFrames) / 43));
            final_pass_double_armed.store(true, std::memory_order_relaxed);
            // FOG/EFFECTS HUNT armed alongside graded stereo. The owner
            // confirmed the missing layers are many - fog, distant fire, wall
            // sparks - all additive, all vanishing together, which points at a
            // shared input or a shared effects target rather than per-effect
            // breakage. This surfaces the unmirrored full-screen/3D inputs the
            // doubled call samples; ShadowIdentity (cap raised) shows where the
            // effects passes output.
            fog_hunt_lines.store(0, std::memory_order_relaxed);
            fog_hunt_armed.store(true, std::memory_order_relaxed);
            // Storage-buffer probe: the particle-simulation-state hypothesis
            // from the owner's weapon-bounce observation.
            storage_buffer_probe_lines.store(0, std::memory_order_relaxed);
            { std::scoped_lock lk(storage_buffer_seen_mutex); storage_buffer_seen.clear(); }
            storage_buffer_probe_armed.store(true, std::memory_order_relaxed);
            // Effects-stage dump is NOT auto-armed: the sparks are
            // intermittent, so it is driven by F12 when they are on screen.
            shadow_identity_lines.store(0, std::memory_order_relaxed);
            graded_stereo_enabled.store(true, std::memory_order_relaxed);
            log::warn(std::format(
                "ScriptedTest step 35: shadow-atlas suppression is {} (suppressedShadowDraws so far={}) - the doubled call will "
                "{} draw into DOOM's shared 16384x16384 shadow atlas. suppressedShadowDraws "
                "climbing means eye 0's shadows are DOOM's own again.",
                suppress_doubled_shadow_draws.load(std::memory_order_relaxed) ? "ON" : "OFF",
                suppressed_shadow_draws.load(std::memory_order_relaxed),
                suppress_doubled_shadow_draws.load(std::memory_order_relaxed) ? "NOT" : "still"));
            log::warn(std::format(
                "ScriptedTest step 35: LIGHT-CULLING suppression is {} (suppressedDispatches so "
                "far={}). The doubled call will {} rewrite DOOM's five shared light-culling "
                "storage buffers. ⚠ THIS IS NOW THE STATE FOR THE WHOLE RUN - the LightCullAB "
                "flip is disarmed above, so unlike every 08-26 run this label still describes "
                "the configuration at the moment H1 and the final-stage band are measured.",
                suppress_doubled_light_culling.load(std::memory_order_relaxed) ? "ON" : "OFF",
                suppressed_light_culling_dispatches.load(std::memory_order_relaxed),
                suppress_doubled_light_culling.load(std::memory_order_relaxed) ? "NOT" : "still"));
            log::warn("ScriptedTest step 35: GRADED STEREO LIVE. The final stage runs a SECOND "
                      "time per frame with its reads redirected to eye 1's composite, and BOTH "
                      "EYES are now post-grade: eye 0 is DOOM's own finished frame at -half, "
                      "eye 1 is the doubled grade at +half. Both carry the HUD, both are fmt 44. "
                      "THE PICTURE WILL CHANGE HERE - that is the point. Expect the two eyes to "
                      "differ in WEATHER: eye 1 has no volumetric fog or fire, which is the "
                      "known unsolved problem, not a presentation fault. Judge the GRADE and the "
                      "DEPTH. PageUp stops it.");
            advance(99);
        }
        break;
    case 37:
        // ===== STEP 1 OF THE GRADE-GAP PLAN: CLOSE THE TONE GAP =====
        //
        // A DECOUPLING, NOT A BUILD. Everything this needs already exists and
        // has already run: hook_final_stage doubles DOOM's grade+HUD pass into
        // a mirror, and hook_queue_present already routes eye 1 to that mirror
        // when graded_stereo_enabled is set. The only reason it has been
        // unreachable is that it was armed inside step 35, alongside a census,
        // P1 and a PPM series that freeze the game - and the test parks at 99.
        //
        // WHY THIS COMES FIRST, and it is not because it is easier. BaselinePair
        // measures eye0 fmt=44 against eye1 fmt=122 with geometryMatch=true:
        // identical extents, so parallax is not at fault, and one eye is DOOM's
        // graded LDR image while the other is raw HDR world colour. Until both
        // eyes sit at the same pipeline stage, "eye 1 has no effects" is a
        // CONFOUNDED observation - a contrast grade crushes low-contrast haze
        // and moves every bright value, so fog and fire can appear to vanish
        // for a reason that is purely tonal. That confound has already cost
        // this project a measurement, and it is the same class of error as
        // comparing a frame-last-write ratio against a lighting stage.
        //
        // Waits first, deliberately: the owner gets several seconds of the
        // CONFIRMED eye-0 pair before anything changes, so if the picture moves
        // when this arms, the change is attributable to this arm alone.
        if (settled(240)) {
            if (!final_stage_hook_installed.load(std::memory_order_relaxed)) {
                // Rule 3. Without this, "eye 1 still is not graded" would be
                // indistinguishable from "the hook was never installed" - the
                // exact ambiguity that made an earlier run unreadable. The
                // hook is derived from THIS run's own containment measurement
                // in step 36, so it can genuinely fail to install.
                log::error("ScriptedTest step 37: the final-stage hook was NOT installed, so the "
                           "grade gap CANNOT be closed this run. Read 'CallerSpecificity' and "
                           "'Containment' for why it was not derived. Nothing is armed; the run "
                           "stays in the confirmed eye-0 configuration, which is the correct "
                           "fallback. Do NOT read eye 1's ungraded picture as a result.");
                advance(99);
                break;
            }
            if (!mirrors_converged()) {
                // The doubling refuses without converged mirrors anyway (see
                // hook_final_stage's pre-flight), so arming here would produce
                // a silent no-op that looks exactly like a mode that did
                // nothing to eye 1.
                log::error("ScriptedTest step 37: mirrors have NOT converged, so the doubled "
                           "final pass would refuse its pre-flight every frame and eye 1 would "
                           "silently keep its ungraded composite. Not arming. Grep "
                           "'MirrorConvergence'.");
                advance(99);
                break;
            }
            apply_graded_mode(2, "graded stereo go-live");
            log::warn(
                "ScriptedTest step 37: GRADE GAP CLOSING. The final stage runs a SECOND time per "
                "frame into our mirror, and eye 1 is now presented THAT - graded, HUD, fmt 44 - "
                "while eye 0 stays DOOM's own finished frame. BOTH EYES SHOULD NOW BE fmt 44 "
                "with toneMatch=true; that is the exit criterion and it is on the BaselinePair "
                "line, not in the picture.\n"
                "WHAT TO JUDGE, in this order:\n"
                "  1. EYE 0 FIRST, AND IT OUTRANKS EVERYTHING HERE. Viewmodel drift, motion "
                "flicker, sliding shadows, brightness against the monitor. Any of those "
                "returning is a REGRESSION, not a trade - press SHIFT+F4 to walk the mode back "
                "down and say which rung it stops at.\n"
                "  2. EYE 1's GRADE and HUD. Is it graded like eye 0? Is the HUD there?\n"
                "  3. EYE 1's EFFECTS, and only now: fog, fire, sparks, glow. This is the first "
                "time that question has ever been asked of a LIKE-FOR-LIKE pair. ZERO or DIM is "
                "the single most discriminating thing you can report - zero rules out parameter "
                "and quality explanations, dim does not.\n"
                "SHIFT+F4 cycles: 2 GRADED -> 0 OFF (rollback/positive control) -> 1 DOUBLE ONLY "
                "-> 2. Rung 0 is the confirmed eye-0 config, so it is the first press if "
                "anything looks wrong. PageUp still stops everything.");
            advance(38);
        }
        break;
    case 38:
        // VERIFY THE MODE ENGAGED, FROM THE INSTRUMENT AND NOT FROM THE PRESS.
        //
        // Separate from the arm on purpose. apply_graded_mode sets flags; it
        // cannot know whether hook_final_stage then ran, whether its pre-flight
        // refused, or whether the XR submission path executed at all. Each of
        // those failures produces an eye 1 that looks exactly like "the grade
        // did not help", and this project has already spent a headset run on
        // precisely that ambiguity.
        if (settled(180)) {
            const bool hooked = final_stage_hook_installed.load(std::memory_order_relaxed);
            const uint32_t entries = final_stage_calls.load(std::memory_order_relaxed);
            const uint32_t doubles = final_pass_doubles.load(std::memory_order_relaxed);
            const uint32_t refused = final_pass_refused_no_mirror.load(std::memory_order_relaxed);
            const uint32_t xr_hits = xr_present_path_hits.load(std::memory_order_relaxed);
            log::warn(std::format(
                "GradedArmCheck mode={} hookInstalled={} | ENTRIES (real final-stage calls "
                "since arm) = {} | WRITES (second invocations) = {} | preflightRefusals = {} | "
                "mirror={} {}x{} fmt={} | xrPresentPathHits={}. Entries and writes are counted "
                "SEPARATELY and must never be read as one number: entries>0 with writes=0 is a "
                "refusal, both zero is a dead hook.",
                graded_mode.load(std::memory_order_relaxed), hooked, entries, doubles, refused,
                reinterpret_cast<void*>(final_pass_mirror_color),
                final_pass_mirror_extent.width, final_pass_mirror_extent.height,
                static_cast<int>(final_pass_mirror_format), xr_hits));
            bool usable = true;
            if (entries == 0) {
                usable = false;
                log::error("GradedArmCheck: the final-stage hook is installed but its target "
                           "NEVER RAN in this window. The doubling cannot have happened; eye 1 "
                           "is still its ungraded composite. Nothing about the grade or the "
                           "effects can be read from this run.");
            } else if (doubles == 0) {
                usable = false;
                log::error(std::format(
                    "GradedArmCheck: {} real final-stage calls and ZERO second invocations "
                    "({} pre-flight refusals). The mode is armed and doing nothing - eye 1 keeps "
                    "its ungraded composite. Refusals>0 means no mirror existed for the final "
                    "pass's framebuffer; refusals=0 means mirror_enabled or mirrors_converged "
                    "gated it. Do NOT read eye 1 as a graded result.", entries, refused));
            } else if (final_pass_mirror_color == VK_NULL_HANDLE) {
                usable = false;
                log::error("GradedArmCheck: the second invocation ran but produced NO mirror "
                           "colour, so its output went somewhere we do not own. Eye 1 falls back "
                           "to the ungraded mirror and any image from this run is "
                           "unattributable.");
            }
            if (xr_hits == 0) {
                // NOT AUTOMATICALLY VOID ANY MORE (2026-08-29). Until the
                // monitor eye-1 view existed, a headset-free run genuinely
                // could not judge this mode: graded_stereo_enabled is read
                // only in the XR submission path, and that trap voided a run
                // on 2026-08-28. SHIFT+F5 changes the situation by putting eye
                // 1 on the monitor through DOOM's own final pass, which is not
                // XR-gated - so the run is void only if BOTH judging surfaces
                // are absent. Saying "void" when a valid surface is available
                // would be its own kind of wrong.
                log::warn("GradedArmCheck: xrPresentPathHits=0 - the headset submission path has "
                          "never run, so nothing can be judged THROUGH THE HEADSET this run. "
                          "graded_stereo_enabled is read only there. This is a FLAT RUN: use "
                          "SHIFT+F5 (monitor eye-1 view) to put eye 1 on the screen through "
                          "DOOM's own final pass, which is not XR-gated. Do NOT judge eye 1 from "
                          "the monitor WITHOUT that mode - the monitor otherwise shows DOOM's "
                          "own presented frame, which is eye 0, and reading it as eye 1 is the "
                          "trap that voided 2026-08-28.");
            }
            if (usable) {
                log::warn(std::format(
                    "GradedArmCheck: ENGAGED - {} second invocations over {} real calls, mirror "
                    "live. Judging surface: {}. The exit criterion is on the BaselinePair line: "
                    "both eyes fmt 44 and toneMatch=true. Read that, then judge EYE 0, then eye "
                    "1's grade/HUD, then eye 1's effects as a LIKE-FOR-LIKE pair.",
                    doubles, entries,
                    xr_hits ? "HEADSET (xrPresentPathHits climbing) - judge eye 1 in the headset"
                            : "FLAT RUN - step 39 arms the monitor eye-1 view in ~3 s and the "
                              "screen becomes EYE 1; SHIFT+F5 flips back to eye 0"));
            }
            advance(39);
        }
        break;
    case 39:
        // MONITOR EYE-1 VIEW, ARMED AUTOMATICALLY ON A FLAT RUN (2026-08-29).
        //
        // The owner asked for this to be what PageUp leaves them in, and it is
        // the right default: it was confirmed against the headset - SHIFT+F5
        // showed "exactly what I was seeing in my right eye" - so the monitor
        // is now a VALIDATED stand-in for the right eye on content questions,
        // and content questions are most of them.
        //
        // WHY IT STILL WAITS THREE SECONDS INSTEAD OF ARMING WITH THE GRADE.
        // Not ceremony - the mode's own positive control is a TRANSITION.
        // Eye 1 renders at +half separation, so the viewpoint visibly shifts
        // when this engages; armed from the start there is nothing to shift
        // against and the owner would have to press SHIFT+F5 twice to learn
        // whether it took. Arming it late means every run demonstrates its own
        // control, unprompted, and gives a few seconds of eye 0 to check the
        // three fixed symptoms against first.
        if (settled(180)) {
            if (xr().ready()) {
                // EYE 0 OUTRANKS EYE 1. In this mode the doubled final pass
                // overwrites the presented frame, which in a headset run IS
                // eye 0 - the confirmed reference. Same refusal as SHIFT+F5,
                // stated here so a headset run says plainly why the monitor is
                // not showing eye 1 rather than looking broken.
                log::warn("ScriptedTest step 39: monitor eye-1 view NOT armed - the headset path "
                          "is live, and this mode overwrites the presented frame, which in a "
                          "headset run is EYE 0. Judge eye 1 through the headset as usual. This "
                          "is a refusal by design, not a failure.");
                advance(99);
                break;
            }
            monitor_eye1_direct.store(true, std::memory_order_relaxed);
            monitor_eye1_passes.store(0, std::memory_order_relaxed);
            vwc_arm("monitor eye-1 view auto-armed (flat run)");
            log::warn(
                "ScriptedTest step 39: MONITOR EYE-1 VIEW LIVE. THE SCREEN IS NOW EYE 1.\n"
                "THE VIEWPOINT SHOULD HAVE JUST SHIFTED. That shift is this mode's positive "
                "control and it is the only thing that proves the doubled final pass graded EYE "
                "1's composite rather than DOOM's real one. If the picture did NOT move, the "
                "mode is VOID - it is not evidence that the eyes match. Cross-check "
                "'passesRedirected' on the next 'ComputeSuppression' line.\n"
                "SHIFT+F5 flips back to eye 0 and is how you check the reference: viewmodel "
                "drift, motion flicker, sliding shadows, brightness. Do that before judging eye "
                "1, every run.\n"
                "F1 A/Bs the compute suppression - the standing intervention that blocks EVERY "
                "shared-SSBO dispatch in the doubled call, which is what GPU particles, emitter "
                "state and light lists are. PageUp stops everything.");
            advance(99);
        }
        break;
    case 34:
        // CALLER SPECIFICITY. Observe every render pass for a short window,
        // then report which callers are unique to the swapchain-writing pass.
        if (frames == 0) {
            caller_specificity_walks.store(0, std::memory_order_relaxed);
            caller_specificity_armed.store(true, std::memory_order_relaxed);
            log::warn("ScriptedTest step 34: CallerSpecificity ARMED (log-only, ~1 s). Grep "
                      "'CallerSpecificity' for the DOOM functions that run for the "
                      "swapchain-writing final pass and for NO other render pass - the only "
                      "safe doubling targets for the grade+HUD stage.");
            break;
        }
        if (frames >= 90) {
            caller_specificity_armed.store(false, std::memory_order_relaxed);
            emit_caller_specificity();
            advance(36);
        }
        break;
    case 36:
        // CONTAINMENT. Which candidate contains the grade pass but NOT the
        // queue submit. Log-only; nothing is hooked or re-invoked, so this
        // window cannot crash the way the last run did.
        if (frames == 0) {
            containment_armed.store(true, std::memory_order_relaxed);
            log::warn("ScriptedTest step 36: CONTAINMENT ARMED (log-only, ~1 s). Grep "
                      "'Containment' for which candidate encloses the grade+HUD render pass "
                      "WITHOUT enclosing vkQueueSubmit/vkQueuePresentKHR. Re-invoking one that "
                      "encloses the submit is what crashed the last run.");
            break;
        }
        if (frames >= 90) {
            containment_armed.store(false, std::memory_order_relaxed);
            emit_containment();
            advance(scripted_test_symmetric.load(std::memory_order_relaxed) &&
                            !scripted_test_run_a.load(std::memory_order_relaxed)
                        ? 4 : 30);
        }
        break;
    case 30:
        // DRAW PARITY. Arms for exactly ONE frame; the frame-root hook
        // latches it, tabulates the real call and the doubled call of that
        // same frame, emits the table and disarms itself.
        if (frames >= 60) {
            draw_parity_done.store(false, std::memory_order_relaxed);
            draw_parity_armed.store(true, std::memory_order_relaxed);
            log::warn("ScriptedTest step 30: DrawParity ARMED for one frame. Grep 'DrawParity' "
                      "for the per-pass table comparing the REAL call and the DOUBLED call of "
                      "the SAME frame - draws, indirect draws, vertices, indices and dispatches. "
                      "This is the measurement that decides whether the doubled call loses the "
                      "effects layers as missing DRAWS, divergent COMPUTE, or identical work on "
                      "different CONTENTS.");
            advance(31);
        }
        break;
    case 31:
        if (draw_parity_done.load(std::memory_order_relaxed) || frames >= 300) {
            if (!draw_parity_done.load(std::memory_order_relaxed)) {
                // Rule 3: a table that never emitted must not look like a
                // table of zeroes.
                draw_parity_armed.store(false, std::memory_order_relaxed);
                log::error("ScriptedTest step 31: DrawParity did NOT emit within 300 frames. "
                           "No table was produced - the doubled call did not run while armed. "
                           "Do not read the absence as parity.");
            }
            advance(4);
        }
        break;
    case 4:
        // STEP 8 — SYMMETRIC STEREO, the live viewing mode.
        //
        // This branch exists because what the owner asked for is not a
        // measurement: it is "show me true stereo parallax in both eyes with
        // near-identical quality". So this path takes no captures, freezes
        // nothing, writes no PPMs, and does not self-disarm - it converges
        // and then STAYS live so the headset can be looked around in.
        //
        // Everything before this step is unchanged and already did the work:
        // pipeline inline, doubling armed, mirrors converged, read
        // redirection on. All that is added here is WHERE each eye comes
        // from and which way its camera is offset.
        if (scripted_test_symmetric.load(std::memory_order_relaxed) &&
            !scripted_test_run_a.load(std::memory_order_relaxed)) {
            frame_root_full_frame.store(true, std::memory_order_relaxed);
            // EYE 0 = DOOM'S PRESENTED FRAME, THE MONITOR IMAGE (2026-08-28).
            //
            // symmetric_stereo_enabled used to be set here, which pointed eye 0
            // at the composite holding image - the frame root's pre-grade,
            // pre-HUD output, captured before the doubled call. That is a
            // DIFFERENT image from the one on the monitor, and the headset run
            // proved the difference matters: the gun that S1's fix locks in
            // DOOM's own frame still stepped in the headset, and neither eye
            // was graded.
            //
            // It also invalidated the plan's own premise that "eye 0 is DOOM's
            // own frame", which is what made the flat screen a valid stand-in.
            // Left as it was, every future flat-screen judgement would keep
            // being made on an image the headset does not show.
            //
            // With this false, eye 0 falls through to the presented swapchain
            // image - byte-for-byte what the monitor displays, graded, HUD and
            // all, rendered from the -half camera so it is genuinely the left
            // eye. Eye 1 is untouched: it comes from its own mirror override,
            // which this flag does not gate. The pair is deliberately
            // mismatched until eye 1 is worked, which is the point - eye 0
            // becomes the fixed baseline to judge eye 1 against.
            //
            // SHIFT+END toggles back to the holding image for comparison.
            symmetric_stereo_enabled.store(false, std::memory_order_relaxed);
            symmetric_log_count.store(0, std::memory_order_relaxed);
            // Step 1 of the EYE 0 CONTAMINATION plan arms itself here rather
            // than on a key, so the state it records is forced in code at arm
            // time and the run needs no extra press to be valid. It re-arms on
            // every separation change below, which is what turns the census
            // into a per-separation table.
            vwc_arm("symmetric-stereo go-live");
            log::warn(std::format(
                "StorageMirror STATE at go-live: enabled={} created={} substitutions={} "
                "layoutRefusals={} noInfo={}. created=0 means NOTHING was mirrored and this run "
                "is identical to the previous one - do NOT read that as 'the fix did not help'. "
                "substitutions=0 with created>0 means the mirrors exist but the safety gate "
                "never let one through, and layoutRefusals says whether the layout half is why.",
                storage_mirror_enabled.load(std::memory_order_relaxed),
                storage_mirror_created.load(std::memory_order_relaxed),
                storage_mirror_substitutions.load(std::memory_order_relaxed),
                storage_mirror_layout_refusals.load(std::memory_order_relaxed),
                storage_mirror_no_info.load(std::memory_order_relaxed)));
            log::warn(std::format(
                "ScriptedTest step 4: SYMMETRIC STEREO LIVE, with the stereo sign CORRECTED "
                "(review P4 - it was inverted, i.e. pseudoscopic). The real call now renders at "
                "-{:.2f} uu and its own pre-final composite becomes EYE 0, the LEFT eye; the "
                "doubled call renders at +{:.2f} uu into its mirror and stays EYE 1, the RIGHT "
                "eye. Both eyes are the same "
                "pipeline stage in the same format, so they should match in quality - and "
                "NEITHER has DOOM's final grade or HUD, which is the deliberate trade. The "
                "monitor now shows the LEFT eye, not a centred view. Nothing self-disarms: "
                "look around. Press PageUp again to stop. Grep 'SymmetricStereo'.",
                frame_root_double_ipd.load(std::memory_order_relaxed) * 0.5f,
                frame_root_double_ipd.load(std::memory_order_relaxed) * 0.5f));
            // ON TO STEP 37, THE GRADE GAP (2026-08-29). NOT back to step 35.
            //
            // This used to park at 99 because step 35 arms the final-stage
            // doubling AND the descriptor census AND P1 AND the PPM series -
            // three interventions and a battery of freezes landing in the
            // middle of the window the owner is judging the gun in. That is
            // still true and step 35 is still off.
            //
            // Step 37 takes ONLY the part that is under test: the doubled
            // final pass and the graded presentation. It also waits, so the
            // owner sees the confirmed eye-0 pair FIRST and any change in the
            // picture is attributable to that arm and to nothing else - the
            // same reasoning step 35's own header gives for arming late.
            advance(37);
            break;
        }
        // STEP 6.5 rider (2026-08-17). The generality run needs the
        // descriptor census - biggestArray and noRecord are the two
        // load-bearing conclusions behind STEP 6's whole mechanism and have
        // only ever been measured in one room - and the census was still a
        // separate SHIFT+PageUp keypress. Rule 8: it becomes a step.
        //
        // Given its own doubled call rather than sharing the ChainWalk
        // observation frame, so neither instrument perturbs the other. It
        // self-disarms after exactly one complete doubled call and restores
        // the job filter by itself.
        if (frames >= 120) {
            descriptor_census_restore_unfiltered.store(
                mirror_unfiltered.load(std::memory_order_relaxed), std::memory_order_relaxed);
            descriptor_census_lines.store(0, std::memory_order_relaxed);
            descriptor_census_real_lines.store(0, std::memory_order_relaxed);
            descriptor_census_mirror_lines.store(0, std::memory_order_relaxed);
            census_miss_lines.store(0, std::memory_order_relaxed);
            census_miss_keying.store(0, std::memory_order_relaxed);
            census_miss_nomirror.store(0, std::memory_order_relaxed);
            census_miss_noimage.store(0, std::memory_order_relaxed);
            census_miss_seen.clear();
            mirror_unfiltered.store(true, std::memory_order_relaxed);
            descriptor_census_enabled.store(true, std::memory_order_relaxed);
            log::warn("ScriptedTest step 4: DescriptorCensus ARMED for one doubled call - "
                      "grep 'DescCensusPass TOTAL' for biggestArray/noRecord, the two "
                      "conclusions STEP 6's mechanism rests on.");
            advance(5);
        }
        break;
    case 5:
        // The census self-disarms at the end of the doubled call it censused.
        if (!descriptor_census_enabled.load(std::memory_order_relaxed) || frames >= 300) {
            if (descriptor_census_enabled.exchange(false, std::memory_order_relaxed)) {
                // Rule 3: never let a timeout look like a completed census.
                log::error("ScriptedTest step 5: the census did NOT complete within 300 frames "
                           "and was force-disarmed. Any DescCensusPass table below is PARTIAL.");
            }
            // RUN A stops here: everything past this point is the chain walk
            // and the ten-PPM A/B freeze, which answer nothing this run asks.
            advance(scripted_test_run_a.load(std::memory_order_relaxed) ? 32 : 6);
        }
        break;
    case 32:
        // RUN A TERMINAL. Summaries, then stop - no captures, no freeze.
        log::warn(std::format(
            "ScriptedTest RUN A COMPLETE. Read in this order:\n"
            "  1. 'MirrorConvergence' - REACHED/LOST. The gate committed at 21:26 inside "
            "728e90b had NEVER executed before this run (the deployed DLL was built 21:12). "
            "This run is its shakedown; if it never reached, everything below ran WITHOUT "
            "redirection and must be read that way.\n"
            "  2. 'DrawParity' - the per-pass table, REAL call vs DOUBLED call of the same "
            "frame. The existential measurement.\n"
            "  3. 'StorageImageID' - what the doubled call writes into DOOM's own "
            "intermediates with no mirror, now with extent and format.\n"
            "  4. 'DescCensusPass TOTAL' - noRecord, against run 26's 5.\n"
            "  redirect substitutedBinds={} clones={} fallbacks={} wholeSetRefusals={} "
            "staleRecordsCleared={}.",
            read_redirect_substitutions.load(std::memory_order_relaxed),
            read_redirect_clones.load(std::memory_order_relaxed),
            read_redirect_failures.load(std::memory_order_relaxed),
            read_redirect_failures.load(std::memory_order_relaxed),
            stale_set_records_cleared.load(std::memory_order_relaxed)));
        log::warn(std::format(
            "RUN A COPY-DESCRIPTOR SUMMARY (review §4): copiesSeen={} "
            "bufferRecordsPropagated={} imageRecordsPropagated={}. copiesSeen=0 means DOOM "
            "issues NO vkCopyDescriptorSet at all - the propagation added this build provably "
            "did nothing this run, the build stays log-only in effect, and 'noRecord contents "
            "unknowable by construction' still needs a different explanation. copiesSeen>0 "
            "means the hole the review named was real.",
            descriptor_copies_seen.load(std::memory_order_relaxed),
            descriptor_copy_records_propagated.load(std::memory_order_relaxed),
            descriptor_copy_image_records_propagated.load(std::memory_order_relaxed)));
        log::warn(std::format(
            "RUN A STORAGE-IMAGE IDENTITY SUMMARY (review P1/§3): identityLines={} (cap {}). "
            "Zero lines while ComputeProbe reports unmirrored>0 would mean the view has no "
            "recorded create parameters at all, and the identity has to be logged from the "
            "creation call itself instead.",
            storage_identity_lines.load(std::memory_order_relaxed),
            kMaxStorageIdentityLines));
        // MAKE THE END VISIBLE. Run A's first outing "never stopped" from the
        // owner's seat: it finished in 3.6 s, but doubling stays armed under
        // the live cap of 200000 attempts, so nothing on screen changed and
        // there was no way to tell. A log-only test that leaves the renderer
        // modified indefinitely is not self-disarming in any sense the person
        // wearing the headset can use. Put the game back the way it was.
        frame_root_double_enabled.store(false, std::memory_order_relaxed);
        symmetric_stereo_enabled.store(false, std::memory_order_relaxed);
        frame_root_full_frame.store(false, std::memory_order_relaxed);
        read_redirect_enabled.store(false, std::memory_order_relaxed);
        g_stereo_offset.store(0.0f, std::memory_order_relaxed);
        log::warn("RUN A: doubling DISARMED, redirection off, camera re-centred. The game is "
                  "back to normal - that visible change IS the end-of-test signal.");
        scripted_test_active.store(false, std::memory_order_relaxed);
        break;
    case 6:
        // STEP 6.6. Let redirection settle, then OBSERVE one doubled call.
        // Observation is log-only: it records the pass map and picks, for
        // each target job RVA, the last pass that renders into a mirrored
        // colour attachment at the main view's extent. Capture cannot be
        // armed before this, because a snapshot image's extent and format
        // have to be known before the first copy is recorded and the image
        // cannot be safely re-created mid-call.
        if (frames >= 120) {
            chainwalk_observe.store(true, std::memory_order_relaxed);
            // Rides along, log-only, same window: what DOOM does AFTER the
            // frame root returns is what eye 1 structurally cannot have, and
            // run 22 showed that gap is real (HUD present on the monitor and
            // absent from our composite, different grade, and the frame
            // root's last pass writes an internal target rather than the
            // swapchain). Self-limits at its line cap.
            post_frame_root_lines.store(0, std::memory_order_relaxed);
            post_frame_root_probe_armed.store(true, std::memory_order_relaxed);
            log::warn("ScriptedTest step 6: ChainWalk OBSERVATION armed - one doubled call is "
                      "mapped pass by pass (log-only). Grep 'ChainWalkMap'. PostFrameRoot probe "
                      "armed alongside it - grep 'PostFrameRoot' for the work DOOM does after "
                      "the frame root returns, which is what eye 1 is missing, and for the "
                      "retRva that identifies the function to double.");
            advance(7);
        }
        break;
    case 7:
        // Observation finishes inside the very next doubled call, so this is
        // normally one or two frames. The timeout exists so a failure to
        // observe costs the snapshots, not the whole run: the identity lines
        // and the camera classification are still worth having.
        if (chainwalk_ready.load(std::memory_order_acquire) ||
            !chainwalk_observe.load(std::memory_order_relaxed) || frames >= 300) {
            const bool ready = chainwalk_ready.load(std::memory_order_acquire);
            chainwalk_observe.store(false, std::memory_order_relaxed);
            chainwalk_capture.store(ready, std::memory_order_relaxed);
            chainwalk_camera_armed.store(true, std::memory_order_relaxed);
            arm_frame_root_double(true);
            if (ready) {
                log::warn("ScriptedTest step 7: A/B capture ARMED at 20 uu with redirection ON, "
                          "ChainWalk snapshots ON and the camera rider ON. Expect a LONGER freeze "
                          "than previous runs - up to ten PPMs are written, not two.");
            } else {
                // Rule 3. Without this the run looks like a normal A/B and the
                // missing chain PPMs read as "the stages produced nothing".
                log::error("ScriptedTest step 7: ChainWalk observation did NOT arm any slot - "
                           "proceeding with the A/B and the camera rider ONLY. No chain_*.ppm "
                           "will be written. Read 'ChainWalkMap' and 'ChainWalkSlot' to see why "
                           "before treating this run as a measurement of the chain.");
            }
            advance(8);
        }
        break;
    case 8:
        // A/B self-disarms at its 20-attempt cap.
        if (!frame_root_double_enabled.load(std::memory_order_relaxed) || frames >= 1200) {
            log::warn(std::format(
                "ScriptedTest COMPLETE. redirect substitutedBinds={} clones={} fallbacks={} "
                "barrierImagesSubstituted={} VIEW-KEYING-GAP={} (gapSkipped={}). The gap counts sampled "
                "bindings whose VIEW is not in mirrored_views but whose underlying IMAGE is "
                "mirrored - reads we could redirect and currently do not. Large => that is why "
                "the composite carries no parallax while the G-buffer does.",
                read_redirect_substitutions.load(std::memory_order_relaxed),
                read_redirect_clones.load(std::memory_order_relaxed),
                read_redirect_failures.load(std::memory_order_relaxed),
                barrier_image_substitutions.load(std::memory_order_relaxed),
                read_redirect_view_gap.load(std::memory_order_relaxed),
                read_redirect_gap_skipped.load(std::memory_order_relaxed)));
            log::warn(std::format(
                "ScriptedTest STEP 6.6 SUMMARY: viewKeyingGaps={} ('ViewGapID'), "
                "safetyGateRefusals={} ('GateFallbackID'), wholeSetRefusals={} "
                "('SetRefusedID'), chainWalkCopies={} ('ChainWalk COPY'/'ChainWalk DUMP'), "
                "chainWalkBytes={}, identity cap {} lines each.\n"
                "  - safetyGateRefusals is NEW: the per-binding !written/layout-mismatch "
                "refusal was never counted before, only sampled as refusedMirrors= inside CLONE "
                "lines. The number previously reported as '46 safety-gate fallbacks' was in fact "
                "wholeSetRefusals, a different and coarser thing.\n"
                "  - wholeSetRefusals cost EVERY mirror-backed read in the set, not one binding: "
                "cloning is all-or-nothing. Read each line's mirrorBacked= and extents.\n"
                "  - All three are per distinct SET EVALUATION, not per bind - "
                "get_redirected_set caches its verdict per (set, mirrorGeneration) - so each "
                "identity line names a set that then binds unredirected for the rest of the run.",
                read_redirect_view_gap.load(std::memory_order_relaxed),
                read_redirect_gate_refusals.load(std::memory_order_relaxed),
                read_redirect_failures.load(std::memory_order_relaxed),
                chainwalk_copy_lines.load(std::memory_order_relaxed),
                chainwalk_bytes_allocated.load(std::memory_order_relaxed),
                kMaxAttributionLines));
            log::warn(std::format(
                "ScriptedTest STEP 6.7a SUMMARY (alias-view substitution): aliasViewsCreated={} "
                "aliasSubstitutions={} gatedByLayoutOrUnwritten={} createFailed={} "
                "noRecordedViewParams={}. aliasSubstitutions is the number this step exists to "
                "make non-zero: run 19 had 67 aliased full-screen binds and substituted NONE of "
                "them. aliasViewsCreated should be small (run 19 saw exactly TWO distinct "
                "aliased views carrying all 67 hits). If aliasSubstitutions is 0 while "
                "viewKeyingGaps is not, the fix did not fire and the images say nothing.",
                alias_view_created.load(std::memory_order_relaxed),
                alias_view_substitutions.load(std::memory_order_relaxed),
                alias_view_gated.load(std::memory_order_relaxed),
                alias_view_failed.load(std::memory_order_relaxed),
                alias_view_no_info.load(std::memory_order_relaxed)));
            log::warn(std::format(
                "ScriptedTest STEP 6.7d SUMMARY (stale set records): staleRecordsCleared={} "
                "wholeSetRefusals={}. Run 20 had 36 whole-set refusals, and every full-extent "
                "pass of the post chain (35, 38, 39, 64) was one of them - each discarding 2-5 "
                "full-screen mirror-backed reads, pass 35's including the G-buffer image itself. "
                "If staleRecordsCleared is 0 the fix never fired. If it is non-zero and "
                "wholeSetRefusals has NOT fallen, the collision has a second source and the "
                "SetRefusedID reasons will name it.",
                stale_set_records_cleared.load(std::memory_order_relaxed),
                read_redirect_failures.load(std::memory_order_relaxed)));
            log::warn(std::format(
                "ScriptedTest COPY-DESCRIPTOR SUMMARY (review §4): copiesSeen={} "
                "bufferRecordsPropagated={} imageRecordsPropagated={}. copiesSeen=0 means DOOM "
                "issues NO vkCopyDescriptorSet at all - the propagation added this build "
                "provably did nothing this run, the build stays log-only in effect, and "
                "'noRecord contents unknowable by construction' still needs a different "
                "explanation. copiesSeen>0 means the hole the review named was real and the "
                "noRecord sets on the deferred chain may now be resolvable; compare "
                "DescCensusPass TOTAL noRecord against run 26's 5.",
                descriptor_copies_seen.load(std::memory_order_relaxed),
                descriptor_copy_records_propagated.load(std::memory_order_relaxed),
                descriptor_copy_image_records_propagated.load(std::memory_order_relaxed)));
            log::warn(std::format(
                "ScriptedTest STORAGE-IMAGE IDENTITY SUMMARY (review P1/§3): "
                "identityLines={} (cap {}). Grep 'StorageImageID' for the extent, format and "
                "usage of every storage image the doubled call wrote WITHOUT a mirror - the "
                "contamination path into eye 0 that appears nowhere in the plan or any commit "
                "message. Zero lines with ComputeProbe unmirrored>0 would mean the view has no "
                "create record at all and the identity must be logged at creation instead.",
                storage_identity_lines.load(std::memory_order_relaxed),
                kMaxStorageIdentityLines));
            post_frame_root_probe_armed.store(false, std::memory_order_relaxed);
            log::warn(std::format(
                "PostFrameRoot TOTAL loggedEvents={} (cap {}). These are the render passes and "
                "transfers DOOM performs AFTER the frame root returns - work the doubled call "
                "never does, so eye 1 cannot have it. The one with isSwapchainImage/"
                "dstIsSwapchainImage=true is the final grade/HUD step that makes DOOM's frame "
                "differ from our composite. Zero here would mean the gap is somewhere else "
                "entirely and this diagnosis is wrong.",
                post_frame_root_lines.load(std::memory_order_relaxed),
                kMaxPostFrameRootLines));
            // V1 FALLBACK EMIT. Idempotent; a no-op when p1_emit_table already
            // ran. Here so that a run whose P1 frame never converged still
            // prints the capture instead of discarding it silently.
            v1_emit_table();
            scripted_test_active.store(false, std::memory_order_relaxed);
        }
        break;
    case 99:
        // PARKED: symmetric stereo is live and stays that way. Deliberately
        // NOT the default case below, which disarms - the driver has to stay
        // active so a second PageUp is read as "stop" rather than as a fresh
        // arm, and so the mode can be turned off without restarting DOOM.
        break;
    default:
        scripted_test_active.store(false, std::memory_order_relaxed);
        break;
    }
}

// ===================== CUTSCENE DETECTOR (2026-08-24) =======================
//
// WHAT IT MEASURES: the rate at which DOOM calls vkQueuePresentKHR, in windows
// of 60 presents. Nothing else - no game state is read, no memory is probed.
//
// WHY THIS SIGNAL. In the 11:46 run the present cadence held 71-80 fps through
// gameplay (headset-driven; 80 is the HMD refresh) and then, at the cutscene,
// went to EXACTLY 60.0 and stayed pinned there:
//
//     11:48:30.599   80.0 fps   gameplay
//     11:48:31.425   72.5 fps   transition
//     11:48:32.426   60.0 fps   locked
//     11:48:32.523   the owner's CUTSCENE MARKER #1
//
// 60 is a DOOM cinematic cadence, not a headset one, and the edge landed 1.9 s
// BEFORE the human marker - which is human reaction time, not detector lag.
//
// ⚠ SCOPE, AND THE LIMIT OF IT, STATED BEFORE ANY RUN. This rule is derived
// from EXACTLY ONE cutscene in ONE run. The owner has already said future
// cutscenes may not behave the same. So the detector is built in two separable
// halves, and only the first is trusted:
//   * IT ALWAYS REPORTS THE RAW EDGE - "PRESENT CADENCE 80.0 -> 60.0 fps".
//     That is a measurement and it stands on its own.
//   * IT SEPARATELY LABELS a drop to ~60 from something materially higher as a
//     CUTSCENE CANDIDATE. That label is the provisional half. If the label is
//     wrong on some future cutscene the raw edge is still correct and still
//     useful, which is the whole reason the two are not fused.
// Cross-check it against the UP-arrow marker: two independent instruments on
// the same event, one automatic and one human.
//
// LATENCY, BY CONSTRUCTION: a change must hold for a second 60-present window
// before it is confirmed, so the report lands 1-2 windows (~1-2 s at these
// rates) after the edge. That is deliberate - the 72.5 fps transition window
// above would otherwise be reported as its own bogus regime. The CONFIRMED
// timestamp is therefore later than the true edge; do not read it as the edge.
//
// COST: one integer increment per present, and a tick compare once per 60.
// DENSE CAPTURE AROUND A MOMENT (2026-08-24).
//
// WHY THIS EXISTS. The 19:14 run caught the event and told us almost nothing
// about it, because every instrument samples on a slow clock: the view-write
// census every 30 s, BaselinePair and Eye1Source every 500 presents, the
// SameFrameTag every 500 after the first 10. Marker #3 landed at 19:16:57 with
// the nearest census 8 s BEFORE it and the next 18 s AFTER. The event is a
// moment and the coverage was a heartbeat, so the one frame that matters was
// never recorded by anything.
//
// This re-arms the three probes that answer "what was the plugin doing at this
// instant" so they emit at the boundary instead of whenever their counter next
// comes round. All log-only, all bounded by their own existing budgets - the
// census is 3 frames, the baseline burst 8 lines, the same-frame tag 10 - so
// this cannot flood a headset run.
//
// Called from the UP marker AND from the cadence detector's edges, so the
// interesting window is densely covered whether or not anyone pressed a key.
// AUTOMATIC PIXEL CAPTURE (2026-08-24). The owner should not have to press a
// key at the right instant to produce the evidence, and should not have to
// prove what they already reported. This arms the like-for-like PPM pair from
// code at the three moments that matter:
//   * ~20 s after VR goes live, while it is known good  -> the BASELINE
//   * the cadence detector's CUTSCENE ENTER edge         -> the transition
//   * ~6 s after that edge                               -> the settled state
// Each costs one freeze. Three per run is the budget; it does not re-arm.
// force (2026-08-31 night): bypass the doomvr-visual.txt suppression for a
// SINGLE, explicitly key-armed dump. The switch file exists to stop the five
// AUTOMATIC arming sites from freezing the game 1.5 s at a time, and it has
// been in place since 2026-08-21 - meaning this machinery has been inert for
// ten days. Removing the file would re-arm all five; forcing one keypress
// leaves the owner.s setting doing exactly what they set it to do.
void arm_pixel_pair(const char* why, bool force = false) {
    // ⚠ HONOUR doomvr-visual.txt HERE (2026-08-25). It did not, and the gap was
    // measured twice: two PPM pairs per run costing 1.49 s and 1.50 s of hard
    // freeze, landing ~20 s after VR goes live - the "two distinctive second
    // plus pauses" in the opening sequence. Confirmed independently by the
    // FrameBudget line, where the same two windows read pre=22.21 ms over 67
    // frames and pre=18.73 ms over 80.
    //
    // The switch file's own promise is "PPM dumps and the H1 census are
    // SKIPPED for this run", but diagnostics_readbacks_enabled() had exactly
    // ONE caller - inside the scripted test, guarding only the series re-arm -
    // while final_pass_dump_pending was armed from five places, none of which
    // checked it. So on the fast-VR (DOWN) path the file was completely inert
    // and the owner reasonably believed it was suppressing these.
    //
    // Reversible the same way it always was: delete C:\dev\doomvr-visual.txt
    // and the captures come back.
    if (!force && !diagnostics_readbacks_enabled()) {
        log::warn(std::format(
            "PixelPair ({}): SKIPPED - doomvr-visual.txt is present, so readbacks are off for "
            "this run. This is the switch file doing what it says, not a failure. Each pair "
            "costs ~1.5 s of freeze; delete the file to get captures back.",
            why));
        return;
    }
    if (final_pass_mirror_color == VK_NULL_HANDLE) {
        log::error(std::format("PixelPair ({}): REFUSED - no final-stage mirror exists yet, so "
                               "there is no eye-1 image to dump. Not a capture, and not a null.",
                               why));
        return;
    }
    final_pass_dump_pending.store(true, std::memory_order_relaxed);
    log::warn(std::format(
        "PixelPair ARMED ({}). The next completed final-stage double writes "
        "finalstage_eye0_control_N.ppm and finalstage_eye1_graded_N.ppm. One freeze. "
        "eye0 is DOOM's presented swapchain image, eye1 is our doubled mirror - two different "
        "objects, so byte-identical output means the two eyes genuinely carry the same picture.",
        why));
}

void cutscene_dense_burst(const char* why) {
    vwc_arm(why);                                              // 3 frames of write census
    baseline_pair_burst.store(8, std::memory_order_relaxed);   // 8 eye0/eye1 pair lines
    mirror_serial_log_count.store(0, std::memory_order_relaxed);  // 10 SameFrameTag lines
    // DESCRIPTOR RING BUDGETS (2026-08-24, plan §2 ii/iv). Re-armed HERE, at
    // the point BOTH the cadence detector's edges and the UP marker already
    // call, rather than only at the cadence edge - a flat run may never
    // produce the 80 -> 60 fps edge the detector was calibrated on, and a
    // budget that never re-arms would print nothing in exactly the window
    // that matters. Log budgets only; nothing about the frame changes.
    // Force a FrameBudget sample at this exact moment (UP marker or a
    // cadence edge), so the transition has a clean window boundary.
    fb_force_emit.store(true, std::memory_order_relaxed);
    refusal_ring_budget.store(kRefusalRingRearmLines, std::memory_order_relaxed);
    refusal_ring_rearms.fetch_add(1, std::memory_order_relaxed);
    shape_tap_budget.store(kShapeTapLines, std::memory_order_relaxed);
    shape_tap_rearms.fetch_add(1, std::memory_order_relaxed);
    log::warn(std::format(
        "DescRing REARMED ({}): SetRefusedID ring lines = {}, WriteShape lines = {}. "
        "Both budgets were reset to full at this moment; if the collapse window then prints "
        "ZERO of either, the re-arm is broken and that is instrument failure, not a null.",
        why, kRefusalRingRearmLines, kShapeTapLines));
}

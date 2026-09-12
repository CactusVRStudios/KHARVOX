// 07_descriptors.inc - descriptor updates, the descriptor census, uniform diff, P1
// 4067 of these lines sit inside #ifdef DOOMVR_RESEARCH_TOOLS,
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

// 4096, not 1024: the first run showed several blocks sitting at exactly the
// old 1024 cap, which means they were truncated and the hash covered only
// their opening bytes. A block that "differs" might have differed only in the
// part we could see, and one that matched might have diverged past the cut.
constexpr uint32_t kUniformDiffMaxBytes = 4096;
// ⚠ CAPTURE EXACTLY ONE FRAME PER SAMPLE (2026-08-20, learned by wasting a
// run). The first version gated only on `armed` and so ran on EVERY graphics
// descriptor bind for hundreds of frames. DOOM issues ~2 700 of those per
// frame; with a global lock, a heap allocation and a map insert on each, the
// game dropped to one frame every five seconds and the 150-present spacing
// meant no report ever emitted. Cost has to be bounded by CONSTRUCTION, not
// by hoping the work is small.
//
// So: `capturing` is true for a single frame, the hook tests that one atomic
// before touching any lock, and the per-frame record count is capped.
std::atomic_bool uniform_diff_capturing{false};
std::atomic_uint32_t uniform_diff_next_at{0};
// ⚠ SEPARATE BUDGETS AND DEDUPE (2026-08-20, second wasted run).
//
// The first bounded version used ONE shared cap of 3000. The real call binds
// so much more often than the doubled one that it consumed the whole budget
// before the doubled call ran: records=3005, realOnly=45, doubledOnly=0,
// identical=0, DIFFERENT=0. A comparison probe with a shared budget lets the
// frequent side starve the rare one - a lesson this project had already
// written down and I re-learned the expensive way.
//
// And 3005 records for 45 distinct keys says the same bindings are rebound
// thousands of times per frame. Deduping on (pass,set,binding) collapses the
// work by ~60x AND makes the cap irrelevant, which fixes the framerate at the
// same time as the starvation: the early-out happens before any hashing.
//
// First write per key per frame wins. DrawParity has already proven both
// calls issue the same draws in the same order, so first-seen is the same
// logical block on both sides.
std::unordered_set<uint64_t> uniform_diff_seen_real;
std::unordered_set<uint64_t> uniform_diff_seen_doubled;
std::atomic_uint32_t uniform_diff_records_real{0};
std::atomic_uint32_t uniform_diff_records_doubled{0};
constexpr uint32_t kUniformDiffMaxRecordsPerSide = 3000;
constexpr uint32_t kUniformDiffGapFrames = 150;

// CONSTANT REPAIR (2026-08-20). The fix the uniform diff earned.
//
// MEASURED: in the large shared constant blocks bound by passes 18, 22, 24 and
// 27, a contiguous region around float 300-320 is EXACTLY ZERO in the doubled
// call and populated in the real one - identically across all four samples:
//
//   f304 REAL -5645.45117  DOUBLED 0.0     f305 REAL -2885.32764  DOUBLED 0.0
//   f306 REAL  8726.18945  DOUBLED 0.0     f315 REAL     1.00000  DOUBLED 0.0
//
// (-5645, -2885, 8726) is a world position and f315=1 is a matrix w. 64 of the
// 131 differing floats are zero on the doubled side. This is not a shifted
// camera, it is UNINITIALISED memory: the doubled call's constant allocation
// is never fully written, so its lighting and fog read a zero eye position and
// a degenerate matrix. Geometry survives because it uses the small binding-0
// block, which IS correctly shifted - which is exactly the observed symptom,
// correct scene with correct parallax and no volumetrics or additive light.
//
// It also explains RenderDoc, and retires it as a mystery: with the layer
// loaded that host-visible memory holds the previous frame's valid values
// instead of zeros. RenderDoc never fixed anything; it changed what
// uninitialised memory happened to contain.
//
// THE REPAIR IS DELIBERATELY CONSERVATIVE: copy the real call's float into the
// doubled call's block ONLY where the doubled float is exactly zero and the
// real one is not. A value the doubling intentionally changed is non-zero, so
// it is never touched, and the stereo shift cannot be undone by this. It also
// refuses to write when both sides resolve to the same host address, which
// would mean patching the real call's own data.
// OFF (2026-08-20). The finding it rests on is REAL and is written up below -
// the doubled call reads uninitialised zeros in the shared lighting constants,
// and filling them makes the mirror respond to scene lighting for the first
// time (px>200 broke its pinned ~16 000 ceiling twice, at 178 632 and
// 191 949). But it does NOT restore fog or fire, and the walk needed to find
// the blocks cost five slow-motion runs and, in the last two, prevented the
// owner from reaching any fire at all - so the probe was blocking its own
// measurement.
//
// Turn back on only with a cheaper way to locate the blocks. Two filters have
// already failed: a pass-number whitelist (DOOM's pass numbering shifts
// between runs) and a per-descriptor-set cache (DOOM appears to allocate fresh
// sets per frame, so the cache never warms and every bind takes the lock).
// The next thing to try is recording the block ADDRESSES once while armed and
// matching on those, which depends on neither.
// OFF for the template-decode test. It is what made every recent run slow
// motion, it is not the fix (lighting responds, fog and fire do not), and
// leaving it on would confound the one variable this run is testing. The
// constants finding stands and is written up; bring the repair back once the
// redirection gap is closed.
// REOPENED 2026-08-29, ON A KEY, FOR THE REASON ITS OWN NOTE GIVES.
//
// The negative above is not safe to rely on. It says so itself: "in the last
// two [runs], prevented the owner from reaching any fire at all - so the probe
// was blocking its own measurement." A test in which the phenomenon cannot be
// reached does not return a negative, it returns nothing. It is also
// 2026-08-20 - the corrupted pre-render-surface-hook baseline.
//
// The perf problem was never the repair, it was the ARMING: a compile-time
// constant meant the whole traversal to the fire happened at the slow rate.
// Runtime-settable and bound to SHIFT+F10, the owner walks to the fog at full
// speed and arms it standing in front of the phenomenon. That is the entire
// difference, and it is what makes the old verdict re-testable.
//
// WHAT IT WILL AND WILL NOT REPAIR, given the 0.0-separation UniformDiff:
//   4096 B blocks (the -1/8 eight-step sequence)  -> LARGE rule, copied whole,
//                                                    REPAIRED
//   208 B blocks  (sub-pixel jitter, ~1e-4)       -> small rule, doubled side
//                                                    is non-zero, NOT repaired
//   16 B blocks   (2048-multiple tile offsets)    -> same, NOT repaired
// So this is a single-variable test of the large-block family alone, which is
// the cleanest of the three and the one whose eight-step index looks most like
// a temporal sequence. A null here does NOT clear the other two.
//
// std::atomic_bool converts to bool implicitly, so every existing use site
// keeps working unchanged.
std::atomic_bool kRepairDoubledConstants{false};
// 0 OFF, 1 WHOLE-BLOCK (the 2026-08-20 behaviour, kept as the A/B), 2 SURGICAL
// (only the shadow-atlas tile floats the 0.0-separation dump names). See the
// three-mode block in capture_uniform_blocks.
std::atomic_uint32_t constant_repair_mode{0};
struct RepairBlock {
    std::vector<unsigned char> data;
    const unsigned char* host = nullptr;
};
std::unordered_map<uint64_t, RepairBlock> repair_real_blocks;
// ⚠ PRE-FILTER, CHECKED BEFORE ANY LOCK (2026-08-20, third perf mistake).
//
// The repair gate is `kRepairDoubledConstants || capturing`, i.e. always true,
// so capture_uniform_blocks took swapchains_mutex on EVERY graphics descriptor
// bind - ~2 700 per frame - and the game went to slow motion. Same error as
// the first probe, reached through a different gate.
//
// Recording is single-threaded (measured, threadID 13060), so this plain set
// is safe. Keyed on the bind itself, so a repeat bind of the same sets in the
// same pass costs one hash lookup and nothing else.
std::unordered_set<uint64_t> repair_seen_binds;
// Hard ceiling on locked walks per frame. The pre-filter should keep this
// far below the cap; the cap exists so a future key mistake degrades the
// measurement instead of the framerate.
constexpr size_t kMaxRepairBindsPerFrame = 400;
// Per-descriptor-set answer to "could this set ever need repairing?", so the
// filter does not depend on DOOM's pass numbering, which moves between runs.
// Populated once per set handle and then read-only in the hot path. Not
// cleared per frame - a set's layout does not change.
std::unordered_map<VkDescriptorSet, bool> set_large_uniform_cache;
// SELF-DISCOVERING PASS FILTER. The hardcoded pass whitelist was FAST (14:08)
// and wrong (numbering shifts between runs). The per-set cache was correct and
// SLOW (DOOM allocates fresh sets per frame, so it never warmed). This is
// both: spend the first two armed frames walking everything, record which
// passes actually carry a large uniform block, then restrict to those for the
// rest of the run. One slow frame at arm time, full speed after, and the
// numbering is learned rather than assumed.
std::unordered_set<uint32_t> repair_interesting_passes;
// EVERY SIGNATURE ANY REPAIR MODE CAN ACT ON, IN ONE PLACE (2026-08-29).
//
// This exists because the same filter voided two rungs in a row. Discovery
// closes after two frames and then permanently restricts the repair to the
// passes it recorded; if a mode's target signature is not recorded here, that
// mode silently writes nothing and reads as a clean negative.
//
// It happened first for the 16-byte tile blocks (excluded by a >=1024 test),
// and then AGAIN for the 208-byte matrix after that fix was applied to the
// 16-byte case only. Two rungs, one root cause, because the discovery
// condition and the per-mode whitelists were written in two places and drifted.
//
// So the condition lives here and is called from BOTH sides. Adding a family
// to a mode without adding it here cannot silently void the rung any more -
// and the ConstantRepair arm line reports floatsWritten as a RATE, which is
// what caught it both times.
inline bool repair_signature_interesting(uint32_t bytes, uint32_t binding) {
    return bytes >= 1024 ||                        // mode 1 whole-block, mode 2 tile arrays
           (bytes == 16 && binding == 0) ||        // mode 2 tile offsets
           (bytes == 208 && binding == 0);         // mode 3 reprojection matrix
    // Mode 4 is deliberately ABSENT here. Adding `bytes == 256` widened this
    // whitelist to a very common block size and dragged the whole
    // capture-and-hash walk in with it - the game crawled. Mode 4 runs on its
    // own fast path in capture_uniform_blocks and never touches this discovery
    // set, because it does not need the real call's blocks at all.
}

// MODE 4's SIGNATURE. Size alone is not enough - other 256-byte blocks exist.
// freqLow_computeUniforms is identified by windowpostoglobalw, floats 16..19,
// which is (0, 0, -a, +a) with a ~ 1/3: a reconstruction row whose xy terms are
// zero because window x/y do not contribute to the perspective divisor.
// Measured on doomvr_frame71015: 0.00000 0.00000 -0.33317 0.33333.
// ⚠ DO NOT TEST bytes == 256. The first version did, and the block was never
// once examined in a three-minute run (fogSeen stayed 0). RenderDoc reports
// byteSize=256 from SHADER REFLECTION - the size the shader declares - but
// Vulkan binds a RANGE, and DOOM binds a large window into its constant ring
// with a dynamic offset pointing at the block. This file's own bounds comment
// records that range being capped at 4096. So the bound length is never 256,
// and a size test can only ever fail.
//
// The dynamic offset already places the pointer at the block's first byte, so
// the signature is read at fixed offsets from there and only a MINIMUM length
// is required.
//
// Signature, from doomvr_frame71015 (values 0.00000 0.00000 -0.33317 0.33333):
//   windowpostoglobalw (+64) is (0, 0, -a, +a) - its xy terms are exactly zero
//   because window x/y do not contribute to the perspective divisor, and its
//   z/w very nearly negate. globalvieworigin.w (+108) is exactly 1.0, being a
//   homogeneous point. Together that is specific enough not to collide with an
//   arbitrary block that happens to be 256 bytes into a shared ring.
inline bool looks_like_fog_block(const unsigned char* p, uint32_t bytes) {
    if (bytes < 256) return false;
    float w[4], origin_w;
    std::memcpy(w, p + 64, sizeof(w));            // windowpostoglobalw
    std::memcpy(&origin_w, p + 108, sizeof(origin_w));   // globalvieworigin.w
    if (!(w[0] == 0.0f && w[1] == 0.0f)) return false;
    if (!(std::isfinite(w[2]) && std::isfinite(w[3]))) return false;
    if (!(w[2] < -0.01f && w[3] > 0.01f)) return false;
    const float sum = w[2] + w[3];
    if (sum > 0.02f || sum < -0.02f) return false;       // z and w must near-negate
    return origin_w == 1.0f;
}
std::atomic_bool repair_discovery_done{false};
std::atomic_uint32_t repair_discovery_frames{0};
std::atomic_uint32_t repair_cap_lines{0};
std::atomic_uint32_t repair_floats_written{0};
std::atomic_uint32_t repair_blocks_touched{0};
std::atomic_uint32_t repair_log_lines{0};

uint64_t fnv1a(const unsigned char* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// Caller must hold swapchains_mutex (buffer_bindings / memory_mappings /
// descriptor_sets all live under it).
void capture_uniform_blocks(uint32_t first_set, uint32_t set_count,
    const VkDescriptorSet* sets, uint32_t dynamic_offset_count,
    const uint32_t* dynamic_offsets, bool doubled) {
    const bool capturing = uniform_diff_capturing.load(std::memory_order_relaxed);
    const bool fog_only = kFogRepairOnly.load(std::memory_order_relaxed);
    if ((!capturing && !kRepairDoubledConstants && !fog_only) || !sets) return;
    // Mode 4 only ever patches the DOUBLED side. Returning here keeps the walk
    // off the real call's binds entirely, which is half the frame's binds.
    // In fog-only mode this function has NOTHING to do: the patch moved to
    // fog_repair_at_dispatch, which sees exactly the sets its dispatch bound.
    // Returning here also keeps the slow record-building path - a copy, a hash
    // and a map insert per block - out of the frame entirely.
    if (fog_only && !capturing && !kRepairDoubledConstants) return;
    auto& records = doubled ? uniform_diff_records_doubled : uniform_diff_records_real;
    auto& seen = doubled ? uniform_diff_seen_doubled : uniform_diff_seen_real;
    if (capturing && records.load(std::memory_order_relaxed) >= kUniformDiffMaxRecordsPerSide)
        return;
    const uint32_t pass = doubled ? doubled_pass_seq : real_pass_seq;
    uint32_t dynamic_index = 0;
    // Collected locally and merged once per bind: taking the diff mutex per
    // BINDING is what made the first version unusable.
    std::vector<std::pair<std::tuple<uint32_t, uint32_t, uint32_t>, UniformBlockRecord>> batch;
    std::vector<uint32_t> bindings;
    for (uint32_t si = 0; si < set_count; ++si) {
        const auto bufs = descriptor_sets.find(sets[si]);
        if (bufs == descriptor_sets.end()) continue;
        bindings.clear();
        for (const auto& [binding, d] : bufs->second) bindings.push_back(binding);
        std::sort(bindings.begin(), bindings.end());
        for (uint32_t binding : bindings) {
            const auto& d = bufs->second.at(binding);
            const bool is_dynamic = d.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
                                    d.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
            uint32_t dynamic_offset = 0;
            // The dynamic offsets are consumed in binding order across the
            // whole bind - the same walk the camera scan uses. Consume it even
            // when the block is not one we record, or every later binding
            // pairs with the wrong offset.
            if (is_dynamic) {
                if (dynamic_index >= dynamic_offset_count || !dynamic_offsets) break;
                dynamic_offset = dynamic_offsets[dynamic_index++];
            }
            if (d.type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
                d.type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) continue;
            // DEDUPE FIRST, before any map lookup or hashing. This is the
            // early-out that makes the probe affordable: the same bindings are
            // rebound thousands of times per frame and only the first is kept.
            // Recording is single-threaded (DOOM records, submits and presents
            // on one thread - measured, threadID 13060), so this unsynchronised
            // set is safe.
            const uint64_t packed = (static_cast<uint64_t>(pass) << 32) |
                                    (static_cast<uint64_t>(first_set + si) << 16) | binding;
            // ⚠ NOT IN FOG MODE. `seen` is cleared only when a capture is armed,
            // so in fog-only mode it would let the patch fire once per
            // (pass,set,binding) per PROCESS and never again - while DOOM rewrites
            // these constants every frame. A once-ever patch is indistinguishable
            // from no patch, which is exactly the failure being fixed here.
            if (!fog_only && !seen.insert(packed).second) continue;
            const auto fb = buffer_bindings.find(d.buffer);
            if (fb == buffer_bindings.end()) continue;
            const auto fm = memory_mappings.find(fb->second.memory);
            if (fm == memory_mappings.end()) continue;  // not host-mapped; unreadable
            const VkDeviceSize absolute = fb->second.offset + d.offset + dynamic_offset;
            if (absolute < fm->second.offset) continue;
            const VkDeviceSize host_offset = absolute - fm->second.offset;
            // ⚠ BOUNDS, PROPERLY (2026-08-20 - this crashed the game at menu
            // load). The old check was `if (mapping.size != VK_WHOLE_SIZE &&
            // host_offset + len > mapping.size) continue;` - which does
            // NOTHING when the mapping was created with VK_WHOLE_SIZE, and
            // most are. That was survivable at the old 1024-byte cap and became
            // an out-of-bounds read the moment the cap went to 4096.
            //
            // The buffer's own size is the authority: never read past the end
            // of the BUFFER, regardless of what the mapping claims or what
            // d.range says. A descriptor's range may legitimately be
            // VK_WHOLE_SIZE, which is not a length.
            VkDeviceSize len = d.range == VK_WHOLE_SIZE ? kUniformDiffMaxBytes : d.range;
            if (len > kUniformDiffMaxBytes) len = kUniformDiffMaxBytes;
            const auto fbuf = buffers.find(d.buffer);
            if (fbuf == buffers.end()) continue;  // unknown size: refuse, never guess
            const VkDeviceSize into_buffer = d.offset + dynamic_offset;
            if (into_buffer >= fbuf->second.size) continue;
            const VkDeviceSize available = fbuf->second.size - into_buffer;
            if (len > available) len = available;
            if (len < 4) continue;
            if (fm->second.size != VK_WHOLE_SIZE && host_offset + len > fm->second.size) continue;
            UniformBlockRecord r;
            r.bytes = static_cast<uint32_t>(len);
            r.hash = fnv1a(fm->second.host + host_offset, static_cast<size_t>(len));
            std::memcpy(r.head.data(), fm->second.host + host_offset,
                        std::min<size_t>(sizeof(r.head), static_cast<size_t>(len)));
            unsigned char* block = fm->second.host + host_offset;
            r.data.assign(block, block + static_cast<size_t>(len));
            // Learn the pass numbering for this run - see repair_interesting_passes.
            //
            // ⚠ THE ≥1024 CONDITION SILENTLY VOIDED SURGICAL MODE'S FIRST RUN
            // (2026-08-29). Discovery walks every pass for two frames and then
            // permanently restricts the repair to the passes recorded here. It
            // recorded only passes carrying a LARGE block - but the shadow-atlas
            // tile offsets surgical mode targets live in 16-BYTE blocks, at
            // passes 2 and 3. So they were repaired during the two discovery
            // frames and excluded for ever afterwards.
            //
            // The owner saw exactly that and described it precisely: "a brief
            // change in shadows in the canyon but it reverted quickly". The
            // change was the discovery window; the revert was this filter
            // closing. floatsWritten confirmed it - 11, then +4 per 180
            // presents, i.e. essentially not firing.
            //
            // The whitelist must therefore admit every signature any repair
            // MODE can act on, not just the large-block one. Keyed on binding
            // and size, never on the pass number, which moved between the two
            // runs today (pass 4 became pass 3).
            if (repair_signature_interesting(r.bytes, binding))
                repair_interesting_passes.insert(pass);
            if (kRepairDoubledConstants) {
                if (!doubled) {
                    RepairBlock& rb = repair_real_blocks[packed];
                    rb.data = r.data;
                    rb.host = block;
                } else if (const auto rit = repair_real_blocks.find(packed);
                           rit != repair_real_blocks.end() && rit->second.host != block) {
                    const size_t floats = std::min(rit->second.data.size(), r.data.size()) / 4;
                    uint32_t wrote = 0;
                    // THREE MODES (2026-08-29). Mode 1 is the original
                    // whole-block copy; mode 2 is SURGICAL and is what the
                    // 0.0-separation dump actually justifies.
                    //
                    // WHY SURGICAL EXISTS. Mode 1 was run on 2026-08-29 and
                    // engaged visibly - the viewmodel HAND DISAPPEARED - and
                    // changed nothing about the effects. The hand is the tell:
                    // copying a whole block keyed by (pass,set,binding) writes
                    // the real call's data over the doubled call's, and when
                    // DOOM has ring-allocated a different slot for that key the
                    // two are DIFFERENT OBJECTS. That is the same failure X1
                    // hit ("geometry flashing... a per-object constant slot
                    // bound to the wrong object"). So mode 1 repairs some
                    // floats and corrupts others, which is no way to test a
                    // hypothesis.
                    //
                    // Mode 2 writes ONLY the floats the dump names, at
                    // signatures keyed by BINDING AND BLOCK SIZE - never by
                    // pass number, which DOOM shifts between runs:
                    //
                    //   16 B, binding 0        -> f0, f1
                    //        values in {0, +/-2048, +/-4096, +/-6144}: an (x,y)
                    //        pixel offset in multiples of 2048, and the shadow
                    //        atlas is 16384 = 8 x 2048.
                    //   4096 B, binding 9 / 13 -> f18, f19, f38, f39
                    //        values strictly in {0.625, 0.750, 0.875} - eighths,
                    //        i.e. the same tile index normalised to [0,1].
                    //
                    // Both families are therefore SHADOW-ATLAS TILE ALLOCATION,
                    // and the doubled call is being handed different tiles than
                    // the real call - a per-call allocator cursor, which is the
                    // consume-once shape. Non-finite reals are already skipped
                    // below, which excludes the NaN-vs-NaN entries that are
                    // comparison artefacts rather than differences.
                    const uint32_t mode = constant_repair_mode.load(std::memory_order_relaxed);
                    const bool whole = mode == 1 && r.bytes >= 1024;
                    const bool surgical = mode == 2 || mode == 3;
                    // Mode 2 = tiles only. Mode 3 = the 208-byte matrix only.
                    // Kept disjoint so each rung is a single variable; mode 2's
                    // negative stays reproducible while mode 3 is judged.
                    const bool surg_tile16 = mode == 2 && r.bytes == 16 && binding == 0;
                    const bool surg_tile4k = mode == 2 && r.bytes == 4096 &&
                                             (binding == 9 || binding == 13);
                    // MODE 3 - THE LAST UNTESTED CONSTANT FAMILY (2026-08-29).
                    //
                    // 208-byte block at binding 0, passes 11 and 13. The dump
                    // shows differing floats at f36,37,38 / f40,41,42 /
                    // f44,45,46 / f48,49,50 - i.e. FOUR vec4 rows with the w
                    // component identical in each. That is a 4x4 MATRIX
                    // occupying the last 16 floats of the block, differing only
                    // in its xyz terms, by ~1e-4.
                    //
                    // A matrix that differs by 1e-4 between two calls rendering
                    // the IDENTICAL camera is either accumulated float noise or
                    // a REPROJECTION matrix carrying frame-to-frame motion -
                    // and volumetric fog is reprojected. That is why it is
                    // worth a rung even though the deltas are tiny.
                    //
                    // Written as f36..f51 whole: f39/f43/f47/f51 are already
                    // identical, so including them is a no-op and keeps the
                    // matrix internally consistent rather than half-updated.
                    //
                    // ⚠ ITS OWN CONTROL, VISIBLE: if this IS the per-pass
                    // camera, copying it collapses eye 1's parallax toward eye
                    // 0 at those passes. If the viewpoint visibly flattens, the
                    // rung is VOID for the fog question - we overwrote the
                    // camera, not a reprojection term.
                    const bool surg_mat208 = mode == 3 && r.bytes == 208 && binding == 0;
                    if (surgical && !surg_tile16 && !surg_tile4k && !surg_mat208) continue;
                    for (size_t fi = 0; fi < floats; ++fi) {
                        if (surg_tile16 && fi > 1) break;
                        if (surg_tile4k && fi != 18 && fi != 19 && fi != 38 && fi != 39) continue;
                        if (surg_mat208 && (fi < 36 || fi > 51)) continue;
                        float dv = 0.0f, rv = 0.0f;
                        std::memcpy(&dv, block + fi * 4, 4);
                        std::memcpy(&rv, rit->second.data.data() + fi * 4, 4);
                        if (dv == rv || !std::isfinite(rv)) continue;
                        if (!whole && !surgical && !(dv == 0.0f && rv != 0.0f)) continue;
                        std::memcpy(block + fi * 4, &rv, 4);
                        ++wrote;
                    }
                    if (wrote) {
                        repair_floats_written.fetch_add(wrote, std::memory_order_relaxed);
                        repair_blocks_touched.fetch_add(1, std::memory_order_relaxed);
                        if (repair_log_lines.fetch_add(1, std::memory_order_relaxed) < 8) {
                            log::warn(std::format(
                                "ConstantRepair[mode {}]: pass={} set={} binding={} bytes={} "
                                "wrote {} floats from the real call's block. {}",
                                mode, pass, first_set + si, binding, r.bytes, wrote,
                                mode == 2
                                    ? "SURGICAL: shadow-atlas tile floats only (16 B -> f0,f1; "
                                      "4096 B -> f18,f19,f38,f39). Nothing else in the block is "
                                      "touched, so a viewmodel that breaks here means the "
                                      "whitelist is wrong."
                                    : "WHOLE-BLOCK: zero here was uninitialised constant memory, "
                                      "not a stereo shift."));
                        }
                    }
                }
            }
            if (!capturing) continue;
            batch.emplace_back(std::tuple<uint32_t, uint32_t, uint32_t>{pass, first_set + si,
                                                                       binding}, r);
        }
    }
    if (batch.empty()) return;
    records.fetch_add(static_cast<uint32_t>(batch.size()), std::memory_order_relaxed);
    std::scoped_lock lk(uniform_diff_mutex);
    auto& target = doubled ? uniform_diff_doubled : uniform_diff_real;
    for (auto& [key, rec] : batch) target[key] = rec;
}

// Emitted on the present thread once both sides of a frame are collected.
void emit_uniform_diff_table() {
    std::scoped_lock lk(uniform_diff_mutex);
    uint32_t same = 0, diff = 0, real_only = 0, doubled_only = 0;
    std::string lines;
    for (const auto& [key, rr] : uniform_diff_real) {
        const auto it = uniform_diff_doubled.find(key);
        if (it == uniform_diff_doubled.end()) { ++real_only; continue; }
        const auto& dr = it->second;
        if (rr.hash == dr.hash && rr.bytes == dr.bytes) { ++same; continue; }
        ++diff;
        // WHERE the block diverges, in floats. This is the number the first
        // report could not give: a camera block differing at float 12..15 is
        // the expected shift, while a block differing at float 40 is not.
        size_t first_diff = SIZE_MAX;
        const size_t common = std::min(rr.data.size(), dr.data.size());
        for (size_t b = 0; b < common; ++b)
            if (rr.data[b] != dr.data[b]) { first_diff = b; break; }
        if (first_diff != SIZE_MAX) {
            const size_t fi = first_diff / 4;
            float rv = 0.0f, dv = 0.0f;
            if ((fi + 1) * 4 <= rr.data.size()) std::memcpy(&rv, rr.data.data() + fi * 4, 4);
            if ((fi + 1) * 4 <= dr.data.size()) std::memcpy(&dv, dr.data.data() + fi * 4, 4);
            lines += std::format(
                "\n  pass={:<3} set={} binding={:<2} bytes={} firstDiffFloat={} REAL={:.4f} "
                "DOUBLED={:.4f} delta={:.4f}",
                std::get<0>(key), std::get<1>(key), std::get<2>(key), rr.bytes,
                fi, rv, dv, dv - rv);
        }
    }
    for (const auto& [key, dr] : uniform_diff_doubled) {
        (void)dr;
        if (uniform_diff_real.find(key) == uniform_diff_real.end()) ++doubled_only;
    }
    // ---- LIGHTBLOCKS: the primary measurement for the light-culling A/B ----
    //
    // ⚠ WHY THIS EXISTS AND firstDiffFloat DOES NOT ANSWER IT.
    //
    // The table above prints ONE float per block - the first byte position at
    // which the two sides disagree - and it prints NOTHING AT ALL for a block
    // whose two sides are identical. Both of those are fatal here:
    //
    //  * "firstDiffFloat=301 REAL=-0.0000 DOUBLED=-0.0000" is what the last
    //    run reported for every large block, and it is compatible with the
    //    doubled side being fully populated OR entirely zero from f302 on.
    //    The 13:06 run's "firstDiffFloat=304 REAL=-5645.4512 DOUBLED=0.0000"
    //    is the same instrument catching the same condition only because the
    //    first disagreement happened to land on a big number that day.
    //  * A block that is identical on both sides is skipped entirely, so
    //    "the zeros are gone" and "the block was never compared" print the
    //    same way: not at all.
    //
    // What the hypothesis actually claims is a POPULATION property: the
    // doubled call's lighting constants contain holes where the real call has
    // real values. So count the holes, on every large block, whether or not
    // the block differs, and report the total. That number is comparable
    // across samples and across arms, which is what an A/B needs.
    //
    // ⚠ SHAPE, NOT PASS NUMBER. DOOM's pass numbering shifts between runs -
    // 15/19/20/21/33, 17/21/22/23/28, 18/22/24/27 and 16/20/22/27 have all
    // been observed for the same logical chain - so this selects by the one
    // property that does not move: a uniform block of >= 1024 bytes. That is
    // the shared lighting/fog/additive constant block in every run observed.
    uint32_t big_blocks = 0, holes_total = 0, antiholes_total = 0;
    uint32_t zeros_real_total = 0, zeros_doubled_total = 0, floats_total = 0;
    std::string big_lines;
    for (const auto& [key, rr] : uniform_diff_real) {
        if (rr.bytes < 1024) continue;
        const auto it = uniform_diff_doubled.find(key);
        if (it == uniform_diff_doubled.end()) continue;
        const auto& dr = it->second;
        const size_t n = std::min(rr.data.size(), dr.data.size()) / 4;
        if (n == 0) continue;
        ++big_blocks;
        uint32_t holes = 0, antiholes = 0, zr = 0, zd = 0;
        float max_abs_real = 0.0f, max_abs_doubled = 0.0f;
        for (size_t fi = 0; fi < n; ++fi) {
            float rv = 0.0f, dv = 0.0f;
            std::memcpy(&rv, rr.data.data() + fi * 4, 4);
            std::memcpy(&dv, dr.data.data() + fi * 4, 4);
            if (rv == 0.0f) ++zr;
            if (dv == 0.0f) ++zd;
            // A HOLE is the exact condition the analysis names: the real call
            // has a value there and the doubled call has an exact zero.
            if (rv != 0.0f && dv == 0.0f) ++holes;
            if (rv == 0.0f && dv != 0.0f) ++antiholes;
            if (std::isfinite(rv) && std::fabs(rv) > max_abs_real) max_abs_real = std::fabs(rv);
            if (std::isfinite(dv) && std::fabs(dv) > max_abs_doubled)
                max_abs_doubled = std::fabs(dv);
        }
        holes_total += holes;
        antiholes_total += antiholes;
        zeros_real_total += zr;
        zeros_doubled_total += zd;
        floats_total += static_cast<uint32_t>(n);
        if (big_lines.size() < 1400)
            big_lines += std::format(
                "\n  pass={:<3} set={} binding={:<2} bytes={} floats={} HOLES={} antiHoles={} "
                "zerosReal={} zerosDoubled={} maxAbsReal={:.3f} maxAbsDoubled={:.3f}",
                std::get<0>(key), std::get<1>(key), std::get<2>(key), rr.bytes, n, holes,
                antiholes, zr, zd, max_abs_real, max_abs_doubled);
    }
    const bool arm_on = light_cull_sample_arm_on.load(std::memory_order_relaxed);
    const uint64_t suppressed_delta = light_cull_sample_suppressed.load(std::memory_order_relaxed);
    log::warn(std::format(
        "LightBlocks sample={} suppressLightCulling={} suppressedDispatchesThisSample={} "
        "largeBlocks={} floats={} HOLEStotal={} antiHolesTotal={} zerosReal={} zerosDoubled={}. "
        "A HOLE is a float the REAL call has and the DOUBLED call reads as exact zero - the "
        "condition the light-culling analysis predicts. READ IT LIKE THIS: HOLES high with "
        "suppression ON and much lower with it OFF confirms the suppression is the source; "
        "HOLES essentially unchanged across the two arms refutes it. "
        "⚠ POSITIVE CONTROL FIRST: suppressedDispatchesThisSample must be >0 in the ON arm and "
        "exactly 0 in the OFF arm. Zero in BOTH arms means the flag never reached the dispatch "
        "path and this sample measured nothing - that is instrument failure, not a result. "
        "largeBlocks=0 likewise means nothing was compared, not that the blocks were clean.{}",
        uniform_diff_reports.load(std::memory_order_relaxed), arm_on ? "ON" : "OFF",
        suppressed_delta, big_blocks, floats_total, holes_total, antiholes_total,
        zeros_real_total, zeros_doubled_total, big_lines));
    // SEPARATION STAMPED ON THE TABLE (2026-08-29). Read the arm label AT the
    // measurement: without this the table cannot be interpreted at all.
    //
    // At a non-zero separation the camera blocks are SUPPOSED to differ - that
    // is Lever A doing its job - so "DIFFERENT=14" needs every entry
    // hand-classified as camera-shaped or not, which is guesswork. At ipd=0.00
    // the two calls render the identical camera and EVERY constant should be
    // byte-identical, so any block that still differs is an anomaly BY
    // CONSTRUCTION and needs no interpretation. The first reading of this table
    // was taken at 3.0 without the value being recorded anywhere on it.
    log::warn(std::format(
        "UniformDiff: sample={} ipd={:.2f} uu zeroOffset={} suppressLightCulling={} "
        "recordsReal={} recordsDoubled={} "
        "(cap {} EACH) identical={} DIFFERENT={} "
        "realOnly={} doubledOnly={}. Either count at the cap means that side was truncated and "
        "absences are not evidence; doubledOnly=0 with realOnly high means the doubled call never "
        "recorded at all. Same draws and same "
        "bound images are already proven, so a block that differs here is the doubled call "
        "reading different constants. ⚠ INTERPRETATION DEPENDS ON ipd: at ipd>0 the camera "
        "blocks SHOULD differ and only the non-camera ones are leads; AT ipd=0.00 NOTHING should "
        "differ and every entry below is an anomaly.{}",
        uniform_diff_reports.load(std::memory_order_relaxed),
        frame_root_double_ipd.load(std::memory_order_relaxed),
        frame_root_zero_offset.load(std::memory_order_relaxed),
        arm_on ? "ON" : "OFF",
        uniform_diff_records_real.load(std::memory_order_relaxed),
        uniform_diff_records_doubled.load(std::memory_order_relaxed),
        kUniformDiffMaxRecordsPerSide, same, diff, real_only, doubled_only, lines));
    // FULL DUMP TO DISK. Every question I might ask next - which float, is it
    // a matrix row, does the delta match the stereo offset - is answerable
    // from this file without spending another of the owner's runs.
    const auto filename=std::format("doomvr_uniform_diff_{}.txt",
                    uniform_diff_reports.load(std::memory_order_relaxed));
    const auto out=std::filesystem::u8path(kharvox::logPathA(filename.c_str()));
    if (std::FILE* f = _wfopen(out.c_str(), L"w")) {
        std::fprintf(f, "# UniformDiff full dump. identical=%u different=%u realOnly=%u\n",
                     same, diff, real_only);
        std::fprintf(f, "# floats printed for every key present on BOTH sides that differs.\n");
        // The A/B arm this sample was taken in. Without it the four dumps are
        // indistinguishable on disk and the offline read has to trust the
        // filename ordering, which is exactly the kind of implicit labelling
        // that has cost this project runs before.
        std::fprintf(f,
            "# suppressLightCulling=%s suppressedDispatchesThisSample=%llu largeBlocks=%u "
            "HOLEStotal=%u zerosDoubled=%u zerosReal=%u\n",
            arm_on ? "ON" : "OFF", static_cast<unsigned long long>(suppressed_delta),
            big_blocks, holes_total, zeros_doubled_total, zeros_real_total);
        for (const auto& [key, rr] : uniform_diff_real) {
            const auto it = uniform_diff_doubled.find(key);
            if (it == uniform_diff_doubled.end()) continue;
            const auto& dr = it->second;
            // Large blocks are dumped whether or not they differ. An IDENTICAL
            // large block is a result here - it says the doubled call's
            // lighting constants match the real call's - and skipping it would
            // make "clean" and "never compared" look the same on disk, which
            // is the reading error this whole re-opening exists to correct.
            if (rr.bytes < 1024 && rr.hash == dr.hash && rr.bytes == dr.bytes) continue;
            std::fprintf(f, "\npass=%u set=%u binding=%u bytesReal=%u bytesDoubled=%u\n",
                         std::get<0>(key), std::get<1>(key), std::get<2>(key),
                         rr.bytes, dr.bytes);
            const size_t n = std::min(rr.data.size(), dr.data.size()) / 4;
            for (size_t i = 0; i < n; ++i) {
                float a = 0.0f, b = 0.0f;
                std::memcpy(&a, rr.data.data() + i * 4, 4);
                std::memcpy(&b, dr.data.data() + i * 4, 4);
                std::fprintf(f, "  f%-4zu REAL %14.5f  DOUBLED %14.5f %s\n",
                             i, a, b, a == b ? "" : "  <-- DIFF");
            }
        }
        std::fclose(f);
        log::warn(std::format("UniformDiff: full float dump written to {}",
                              out.string()));
    } else {
        log::error("UniformDiff: could not open the dump file; only the summary above exists.");
    }
    uniform_diff_real.clear();
    uniform_diff_doubled.clear();
}

// Emitted on the present thread once the captured frame is complete.
void emit_pipeline_parity() {
    uint32_t passes = 0, same = 0, differing = 0, real_only_passes = 0, doubled_only_passes = 0;
    std::string lines;
    std::set<uint32_t> all_passes;
    for (const auto& [p, s] : pipeline_parity_real) { (void)s; all_passes.insert(p); }
    for (const auto& [p, s] : pipeline_parity_doubled) { (void)s; all_passes.insert(p); }
    for (uint32_t p : all_passes) {
        const auto ri = pipeline_parity_real.find(p);
        const auto di = pipeline_parity_doubled.find(p);
        const bool has_r = ri != pipeline_parity_real.end();
        const bool has_d = di != pipeline_parity_doubled.end();
        ++passes;
        if (has_r && !has_d) { ++real_only_passes; continue; }
        if (!has_r && has_d) { ++doubled_only_passes; continue; }
        if (ri->second == di->second) { ++same; continue; }
        ++differing;
        // Name the actual difference, both directions. A pipeline the REAL
        // call binds and the doubled one does not is a draw eye 1 is doing
        // with different state - which is the whole point of this probe.
        // Blend state printed HERE rather than left as a handle to look up in
        // the ColorBlendProbe lines: the whole point of naming the pipeline is
        // its blend state, and a two-step lookup across a 480 MB log is how a
        // finding gets lost.
        const auto describe = [&](VkPipeline v) {
            std::unique_lock lock(swapchains_mutex);
            const auto it = pipeline_depth_stencil_info.find(v);
            if (it == pipeline_depth_stencil_info.end())
                return std::format(" {}(noRecord)", reinterpret_cast<void*>(v));
            const auto& i = it->second;
            if (!i.has_color_blend_state)
                return std::format(" {}(noBlendState)", reinterpret_cast<void*>(v));
            return std::format(" {}(blendEn={} src={} dst={} op={})",
                               reinterpret_cast<void*>(v), i.blend_enable[0],
                               static_cast<int>(i.src_color_blend_factor),
                               static_cast<int>(i.dst_color_blend_factor),
                               static_cast<int>(i.color_blend_op));
        };
        std::string only_r, only_d;
        for (VkPipeline v : ri->second)
            if (!di->second.count(v)) only_r += describe(v);
        for (VkPipeline v : di->second)
            if (!ri->second.count(v)) only_d += describe(v);
        if (lines.size() < 1600)
            lines += std::format("\n  pass={:<3} realPipes={} doubledPipes={} realOnly=[{}] "
                                 "doubledOnly=[{}]",
                                 p, ri->second.size(), di->second.size(), only_r, only_d);
    }
    log::warn(std::format(
        "PipelineParity: passes={} identical={} DIFFERING={} realOnlyPasses={} "
        "doubledOnlyPasses={} (records real={} doubled={}, cap {} EACH).\n"
        "  Blend state is a property of the PIPELINE OBJECT, captured at creation. So "
        "identical pipeline sets on a pass means eye 1 draws that pass with exactly the same "
        "blend factors, blend op and colour write mask as DOOM - by construction, not by "
        "inference - and the 'eye 1 blends differently' hypothesis is dead for that pass.\n"
        "  DIFFERING>0 names the passes where the two calls bind different pipelines, and the "
        "realOnly/doubledOnly handles are the specific ones to look up in the ColorBlendProbe "
        "lines.\n"
        "  ⚠ passes=0 or both record counts 0 is INSTRUMENT FAILURE, not parity: it means the "
        "capture frame never armed. Either count AT the cap means that side was truncated and "
        "an absence below is not evidence.{}",
        passes, same, differing, real_only_passes, doubled_only_passes,
        pipeline_parity_seen_real.size(), pipeline_parity_seen_doubled.size(),
        kMaxPipelineParityRecords, lines));
    pipeline_parity_real.clear();
    pipeline_parity_doubled.clear();
}

void run_descriptor_census(VkCommandBuffer command_buffer, uint32_t first_set,
    uint32_t set_count, const VkDescriptorSet* sets, uint32_t dynamic_offset_count,
    const uint32_t* dynamic_offsets) {
    if (!sets || set_count == 0) return;
    const bool doubled = inside_frame_root_double;
    std::string detail;
    std::string camera_detail;
    uint32_t mirror_hits = 0, shared_hits = 0, unknown_hits = 0, array_max = 0;
    // MISS PATH collection buffer - see kMaxCensusMissLines. Fixed size, so a
    // pass binding hundreds of unknown views costs a bounded amount here; the
    // rollup counters above still see every one of them through unknown_hits.
    VkImageView miss_views[24]{};
    uint32_t miss_count = 0;
    uint32_t no_record_hits = 0;
    // Lock order: mirror_mutex before swapchains_mutex (see mirror_mutex).
    // census_classify_view reads mirrored_views/mirror_shared_source_views,
    // which a destroy hook can now mutate from another thread.
    {
        std::scoped_lock mirror_lock(mirror_mutex);
        std::unique_lock lock(swapchains_mutex);
        uint32_t dynamic_index = 0;
        for (uint32_t si = 0; si < set_count; ++si) {
            const VkDescriptorSet set = sets[si];
            detail += std::format(" set{}={}", first_set + si, reinterpret_cast<void*>(set));
            // --- sampled images: the classification the census exists for ---
            const auto imgs = descriptor_image_sets.find(set);
            // Rule 3. Our record of a set's contents comes only from
            // hook_update_descriptor_sets. A set written through
            // vkUpdateDescriptorSetWithTemplate (unhooked) or populated
            // before our hooks installed has NO entry here, and would
            // otherwise render as "this set binds no sampled images" - a
            // silent blind spot indistinguishable from a real negative, and
            // it would under-count exactly the redirection work STEP 6 has to
            // do. Mark it instead.
            if (imgs == descriptor_image_sets.end() &&
                descriptor_sets.find(set) == descriptor_sets.end()) {
                detail += "[NO-RECORD]";
                ++no_record_hits;
            }
            if (imgs != descriptor_image_sets.end()) {
                std::vector<uint32_t> bindings;
                bindings.reserve(imgs->second.size());
                for (const auto& [binding, unused] : imgs->second) bindings.push_back(binding);
                std::sort(bindings.begin(), bindings.end());
                for (uint32_t binding : bindings) {
                    const auto& rec = imgs->second.at(binding);
                    if (rec.count > array_max) array_max = rec.count;
                    const char* hit = census_classify_view(rec.image_view);
                    if (std::strcmp(hit, "mirror") == 0) ++mirror_hits;
                    else if (std::strcmp(hit, "shared") == 0) ++shared_hits;
                    else if (std::strcmp(hit, "unknown") == 0) {
                        ++unknown_hits;
                        // MISS PATH phase 1: collect only. Resolving the view
                        // to an image needs framebuffer_depth_view_mutex and
                        // the mirror lookup needs mirror_mutex - taking either
                        // HERE would put a third lock on the render thread
                        // underneath the two already held, which is exactly
                        // what wedged the render thread in run 17. Phases 2
                        // and 3 run below, after these locks are released.
                        if (miss_count < std::size(miss_views))
                            miss_views[miss_count++] = rec.image_view;
                    }
                    detail += std::format("[b{}:view={} hit={}", binding,
                        reinterpret_cast<void*>(rec.image_view), hit);
                    // Arrays: report the count and classify the leading
                    // elements we captured, so "one big indexed array" is
                    // visible as such instead of masquerading as one image.
                    if (rec.count > 1) {
                    kharvox::native::fail("descriptor arrays require complete element capture");
                        detail += std::format(" count={}", rec.count);
                        for (uint32_t e = 1; e < rec.count && e < rec.views.size(); ++e) {
                            const char* eh = census_classify_view(rec.views[e]);
                            if (std::strcmp(eh, "mirror") == 0) ++mirror_hits;
                            detail += std::format(" e{}={}", e, eh);
                        }
                    }
                    detail += "]";
                }
            }
            // --- dynamic uniforms: claim 12(c), does the bound camera record
            // carry the shift? Same binding-order walk over dynamic_offsets
            // the matrix scan in hook_cmd_bind_descriptor_sets uses, so the
            // offsets line up with the same bindings.
            const auto bufs = descriptor_sets.find(set);
            if (bufs == descriptor_sets.end()) continue;
            std::vector<uint32_t> dyn;
            for (const auto& [binding, d] : bufs->second) {
                if (d.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
                    d.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)
                    dyn.push_back(binding);
            }
            std::sort(dyn.begin(), dyn.end());
            for (uint32_t binding : dyn) {
                if (dynamic_index >= dynamic_offset_count || !dynamic_offsets) break;
                const uint32_t dynamic_offset = dynamic_offsets[dynamic_index++];
                const auto& d = bufs->second.at(binding);
                if (d.range < 64 || d.range > 256) continue;  // camera-shaped only
                const auto fb = buffer_bindings.find(d.buffer);
                if (fb == buffer_bindings.end()) continue;
                const auto fm = memory_mappings.find(fb->second.memory);
                if (fm == memory_mappings.end()) continue;
                const VkDeviceSize absolute = fb->second.offset + d.offset + dynamic_offset;
                if (absolute < fm->second.offset) continue;
                const VkDeviceSize host_offset = absolute - fm->second.offset;
                if (fm->second.size != VK_WHOLE_SIZE && host_offset + 64 > fm->second.size) continue;
                std::array<float, 16> v{};
                std::memcpy(v.data(), fm->second.host + host_offset, 64);
                if (!looks_like_world_camera_matrix(v.data())) continue;
                // 2026-08-17 RUN 2 FIX. This printed v[12..15] as "row3" and
                // called it the translation row. It is not. The live values
                // have the shape (0.9999 0.0046 0.0092 10114.93): a unit
                // basis vector followed by a large scalar, and v[15] != 1,
                // so each ROW here is (axis.xyz, offset) and the translation
                // lives in the fourth COLUMN - v[3], v[7], v[11], v[15].
                // A lateral shift would appear in the column, which the old
                // line did not print, so RUN 2's "real and doubled look
                // identical" reading was inconclusive, NOT a negative.
                // Print the translation column and keep row3 for continuity.
                camera_detail += std::format(
                    " [cam set{} b{} dynOff={} range={} trans=({:.4f} {:.4f} {:.4f} {:.4f}) "
                    "row3=({:.4f} {:.4f} {:.4f} {:.4f})]",
                    first_set + si, binding, dynamic_offset,
                    static_cast<uint64_t>(d.range), v[3], v[7], v[11], v[15],
                    v[12], v[13], v[14], v[15]);
            }
        }
    }
    // ---- MISS PATH phases 2 and 3, with both census locks now RELEASED.
    //
    // Phase 2 resolves view -> image under framebuffer_depth_view_mutex alone;
    // phase 3 asks mirrored_images whether a mirror exists for that image,
    // under mirror_mutex alone. Sequential scopes, never nested - the census
    // above holds mirror_mutex + swapchains_mutex, and a third lock taken
    // underneath them is the run-17 wedge.
    if (doubled && miss_count && census_miss_seen.size() < kMaxCensusMissResolve) {
        struct MissRec { VkImageView view; VkImage image; VkExtent3D extent; int fmt; int type; };
        MissRec recs[std::size(miss_views)]{};
        uint32_t nrec = 0;
        {
            std::scoped_lock idlock(framebuffer_depth_view_mutex);
            for (uint32_t i = 0; i < miss_count; ++i) {
                // Dedupe across the whole census run: the same view is bound
                // by many draws, and one line per distinct view is the whole
                // budget's worth of information.
                if (!census_miss_seen.insert(miss_views[i]).second) continue;
                MissRec r{miss_views[i], VK_NULL_HANDLE, {}, 0, -1};
                if (auto v = image_view_to_image.find(miss_views[i]);
                    v != image_view_to_image.end())
                    r.image = v->second;
                if (r.image != VK_NULL_HANDLE) {
                    if (auto ci = created_image_infos.find(r.image);
                        ci != created_image_infos.end()) {
                        r.extent = ci->second.extent;
                        r.fmt = static_cast<int>(ci->second.format);
                        r.type = static_cast<int>(ci->second.type);
                    }
                }
                recs[nrec++] = r;
            }
        }
        if (nrec) {
            std::scoped_lock mlock(mirror_mutex);
            for (uint32_t i = 0; i < nrec; ++i) {
                const MissRec& r = recs[i];
                const char* verdict;
                if (r.image == VK_NULL_HANDLE) {
                    census_miss_noimage.fetch_add(1, std::memory_order_relaxed);
                    verdict = "MISS-NOIMAGE";
                } else if (mirrored_images.find(r.image) != mirrored_images.end()) {
                    census_miss_keying.fetch_add(1, std::memory_order_relaxed);
                    verdict = "MISS-KEYING";
                } else {
                    census_miss_nomirror.fetch_add(1, std::memory_order_relaxed);
                    verdict = "MISS-NOMIRROR";
                }
                if (census_miss_lines.fetch_add(1, std::memory_order_relaxed) <
                    kMaxCensusMissLines) {
                    log::warn(std::format(
                        "CensusMiss pass={} {} view={} image={} extent={}x{}x{} fmt={} type={}. "
                        "MISS-KEYING = we DO hold a mirror for this image, filed under a "
                        "different view handle, so the fix is to resolve mirrors by IMAGE. "
                        "MISS-NOMIRROR = no mirror exists for the image at all. MISS-NOIMAGE = "
                        "we could not resolve the view to an image, which is a third bug.",
                        doubled_pass_seq, verdict,
                        reinterpret_cast<void*>(r.view), reinterpret_cast<void*>(r.image),
                        r.extent.width, r.extent.height, r.extent.depth, r.fmt, r.type));
                }
            }
        }
    }
    if (doubled) {
        // Accumulate for EVERY bind, unconditionally and without logging, so
        // the per-pass summary is a true total even where detail lines are
        // sampled away. This is what makes the table trustworthy.
        const uint32_t slot = doubled_pass_seq < kCensusPassSlots
                                  ? doubled_pass_seq : kCensusPassSlots - 1;
        CensusPassTotals& t = census_pass_totals[slot];
        ++t.binds;
        t.mirror += mirror_hits;
        t.shared += shared_hits;
        t.unknown += unknown_hits;
        t.no_record += no_record_hits;
        if (array_max > t.max_array) t.max_array = array_max;
        t.fb = doubled_current_framebuffer;
        t.fb_mirrored = doubled_current_fb_mirrored;
        if (doubled_current_job_rva) t.job_rva = doubled_current_job_rva;

        // Detail line, sampled. A line carrying a mirror hit is the actual
        // signal - which pass reads which of our mirrored views - so it gets
        // its own budget rather than competing with per-object noise.
        bool want_line = false;
        if (mirror_hits > 0) {
            want_line = descriptor_census_mirror_lines.fetch_add(1, std::memory_order_relaxed) <
                        kMaxCensusMirrorLines;
        }
        if (!want_line && census_pass_line_count[slot] < kMaxCensusLinesPerPass) {
            ++census_pass_line_count[slot];
            want_line = true;
        }
        if (want_line &&
            descriptor_census_lines.fetch_add(1, std::memory_order_relaxed) <
                kMaxDescriptorCensusLines) {
            log::warn(std::format(
                "DescCensus pass={} fb={} fbMirrored={} jobRva={:#x} pipe={} "
                "mirror={} shared={} unknown={} maxArray={}{}{}",
                doubled_pass_seq, reinterpret_cast<void*>(doubled_current_framebuffer),
                doubled_current_fb_mirrored, doubled_current_job_rva,
                reinterpret_cast<void*>(command_buffer),
                mirror_hits, shared_hits, unknown_hits, array_max, detail, camera_detail));
        }
        return;
    }
    // Real call: camera records only, tiny budget. Without this the doubled
    // call's row3 is a number with nothing to compare against.
    if (camera_detail.empty()) return;
    const uint32_t rn = descriptor_census_real_lines.fetch_add(1, std::memory_order_relaxed);
    if (rn < kMaxDescriptorCensusRealLines) {
        log::warn(std::format("DescCensusReal #{} pipe={}{}", rn,
            reinterpret_cast<void*>(command_buffer), camera_detail));
    }
}

// P1 classification and recording - see the block at kP1ParticleSimJobRva.
//
// Reads only the arm-time snapshots, so it holds no mirror lock and no view
// lock. swapchains_mutex is taken for descriptor_image_sets, the same single
// lock the census already takes for the same map.
uint8_t p1_classify(VkImageView view) {
    // VOL first: the froxel volumes are shared by BOTH sides by design
    // (rd3D=2 on both sides of the working table), so letting one fall into
    // MIR or REAL would move the discriminator for a resource that is not
    // part of the question.
    if (p1_volume_views.count(view) != 0) return kP1Vol;
    // BY HANDLE FIRST, and this is not an optimisation. Our mirror and alias
    // views are created with real_create_image_view, so image_view_infos has
    // no entry for them and the image lookup below cannot resolve them at all.
    // Run 1 reported rdMIR=0 on every bound row for exactly this reason.
    if (p1_mirror_views.count(view) != 0) return kP1Mir;
    const auto it = p1_view_image.find(view);
    if (it == p1_view_image.end()) return kP1Oth;
    if (p1_mirror_images.count(it->second) != 0) return kP1Mir;
    if (p1_source_images.count(it->second) != 0) return kP1Real;
    return kP1Oth;
}

// Find-or-add in the shared job-RVA registry. Render thread only, and only
// while p1_active, so no lock: the same single-threaded assumption
// ensure_mirror_framebuffer and census_classify_view already make.
uint32_t p1_rva_slot(uintptr_t rva) {
    for (uint32_t i = 0; i < p1_rva_count; ++i) if (p1_rva_keys[i] == rva) return i;
    if (p1_rva_count >= kP1RvaSlots) { ++p1_rva_overflow; return kP1RvaSlots - 1; }
    p1_rva_keys[p1_rva_count] = rva;
    return p1_rva_count++;
}

void p1_note(P1Slot& s, VkImageView view) {
    if (view == VK_NULL_HANDLE) return;
    // Per-side dedupe, over the views this slot actually stored. Past
    // kP1SlotViews the dedupe can no longer see earlier handles, so those are
    // counted into overflow rather than into the class totals - an inflated
    // rdREAL from repeated binds of one view would read exactly like a real
    // one otherwise.
    const uint32_t stored = s.distinct < kP1SlotViews ? s.distinct : kP1SlotViews;
    for (uint32_t i = 0; i < stored; ++i) if (s.views[i] == view) return;
    if (s.distinct >= kP1SlotViews) { ++s.overflow; ++s.distinct; return; }
    const uint8_t c = p1_classify(view);
    s.views[s.distinct] = view;
    s.cls[s.distinct] = c;
    ++s.cls_count[c];
    ++s.distinct;
}

void p1_record(uint32_t set_count, const VkDescriptorSet* sets, bool bound_phase) {
    if (!p1_active || !sets || set_count == 0) return;
    P1Slot* pass_table = nullptr;
    P1Slot* rva_table = nullptr;
    uint32_t seq = 0;
    uintptr_t rva = 0;
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (inside_frame_root_double) {
        pass_table = bound_phase ? p1_doubled_bound : p1_doubled_asked;
        rva_table = bound_phase ? p1_doubled_bound_rva : p1_doubled_asked_rva;
        seq = doubled_pass_seq;
        rva = doubled_current_job_rva;
        fb = doubled_current_framebuffer;
    } else if (inside_real_frame_root) {
        // The real call is never substituted, so it has no BOUND phase. Its
        // ASKED row IS what it bound.
        if (bound_phase) return;
        pass_table = p1_real_asked;
        rva_table = p1_real_asked_rva;
        seq = real_pass_seq;
        rva = real_current_job_rva;
        fb = real_current_framebuffer;
    } else {
        return;
    }
    P1Slot& sp = pass_table[seq < kP1PassSlots ? seq : kP1PassSlots - 1];
    ++sp.binds;
    // LAST writer wins on the pass row, matching DescCensusPass. First-writer
    // -wins is what latched RenderOpaque onto run 1's pass-16 asked row while
    // the lighting job's own binds were arriving in the same ordinal.
    sp.job_rva = rva;
    sp.fb = fb;
    P1Slot& sr = rva_table[p1_rva_slot(rva)];
    ++sr.binds;
    sr.job_rva = rva;
    if (!sr.fb) sr.fb = fb;
    // Both rows get every view. Dedupe is per row, so the RVA row sees the
    // job's whole read set across the ordinals it spans and the pass row stays
    // readable as context.
    const auto note = [&sp, &sr](VkImageView v) { p1_note(sp, v); p1_note(sr, v); };
    // Walks one of DOOM's own sets out of descriptor_image_sets. p1_note reads
    // only the arm-time snapshots, so nothing here needs the mirror or view
    // lock - swapchains_mutex alone, the same single lock the census takes for
    // the same map.
    // Called with swapchains_mutex already held, once per p1_record rather
    // than once per set: this runs on a path that executes ~2700 times in the
    // armed frame and there is no reason to pay an acquire per descriptor set.
    const auto note_original_set = [&note, &sp, &sr, rva](VkDescriptorSet set) {
        ++sp.sets_seen; ++sr.sets_seen;
        // Remember the lighting job's sets, per side, so P1W can replay the
        // captured writes against exactly the sets that carry the divergence.
        if (rva == kP1ParticleSimJobRva) {
            VkDescriptorSet* list = inside_frame_root_double ? p1_light_sets_doubled
                                                             : p1_light_sets_real;
            uint32_t& count = inside_frame_root_double ? p1_light_sets_doubled_count
                                                       : p1_light_sets_real_count;
            bool known = false;
            for (uint32_t i = 0; i < count; ++i) if (list[i] == set) { known = true; break; }
            if (!known && count < kP1MaxLightSets) list[count++] = set;
        }
        const auto imgs = descriptor_image_sets.find(set);
        if (imgs == descriptor_image_sets.end()) {
            // We have no content record for this set. Counted, not ignored:
            // an unseen set contributes zero views and is indistinguishable
            // from a set that binds none.
            if (descriptor_sets.find(set) == descriptor_sets.end()) {
                ++sp.no_record; ++sr.no_record;
            }
            return;
        }
        for (const auto& [binding, rec] : imgs->second) {
            note(rec.image_view);
            // Per-binding detail for the lighting job only, so a divergence
            // can be read as "which set, which binding" rather than as a
            // count. Bounded: this job binds ~12 sets in the armed frame.
            if (rva == kP1ParticleSimJobRva &&
                p1_detail_lines.fetch_add(1, std::memory_order_relaxed) < kP1MaxDetailLines) {
                log::warn(std::format(
                    "P1Binding side={} set={} binding={} view={} class={} count={} type={}",
                    inside_frame_root_double ? "DOUBLED" : "REAL",
                    reinterpret_cast<void*>(set), binding,
                    reinterpret_cast<void*>(rec.image_view),
                    p1_class_name(p1_classify(rec.image_view)), rec.count,
                    static_cast<int>(rec.type)));
            }
            for (uint32_t e = 1; e < rec.count && e < rec.views.size(); ++e)
                note(rec.views[e]);
        }
    };
    if (!bound_phase) {
        std::unique_lock lock(swapchains_mutex);
        for (uint32_t i = 0; i < set_count; ++i) note_original_set(sets[i]);
        return;
    }
    // BOUND: `sets` is the substituted array. An entry that was cloned carries
    // the clone handle, whose contents live only in p1_clone_views - the clone
    // is written with real_update_descriptor_sets, deliberately not our hook,
    // so descriptor_image_sets has no record of it.
    //
    // An entry that was NOT cloned is DOOM's original handle, unchanged, and
    // its reads are just as much part of what this pass binds. Skipping those
    // would under-count rdREAL on the bound side and make "redirection left
    // real-chain reads in place" - the finding this instrument exists to be
    // able to see - look like a smaller number than it is.
    std::array<VkDescriptorSet, 16> passthrough{};
    uint32_t passthrough_count = 0;
    {
        std::scoped_lock lock(p1_clone_mutex);
        for (uint32_t i = 0; i < set_count && i < passthrough.size(); ++i) {
            const auto it = p1_clone_views.find(sets[i]);
            if (it != p1_clone_views.end()) {
                for (VkImageView v : it->second) note(v);
            } else {
                passthrough[passthrough_count++] = sets[i];
            }
        }
    }
    // p1_clone_mutex released before swapchains_mutex is taken, so the two
    // never nest and this adds no edge to the lock order.
    if (passthrough_count == 0) return;
    std::unique_lock lock(swapchains_mutex);
    for (uint32_t i = 0; i < passthrough_count; ++i) note_original_set(passthrough[i]);
}

const char* p1_class_name(uint8_t c) {
    switch (c) {
    case kP1Mir: return "MIR";
    case kP1Real: return "REAL";
    case kP1Vol: return "VOL";
    default: return "OTH";
    }
}

void p1_emit_row(const char* side, const char* phase, uint32_t pass, const P1Slot& s) {
    log::warn(std::format(
        "P1Read side={:7} phase={:6} pass={:3} binds={:5} distinct={:4} rdMIR={:3} rdREAL={:3} "
        "rdVOL={:3} rdOTH={:4} overflow={:3} sets={:5} noRecord={:4} jobRva={:#x} fb={}",
        side, phase, pass, s.binds, s.distinct, s.cls_count[kP1Mir], s.cls_count[kP1Real],
        s.cls_count[kP1Vol], s.cls_count[kP1Oth], s.overflow, s.sets_seen, s.no_record,
        s.job_rva, reinterpret_cast<void*>(s.fb)));
}

void p1_emit_handles(const char* side, const char* phase, uint32_t pass, const P1Slot& s) {
    // Raw handles, as the capture tool prints them, so a substitution can be
    // followed one-for-one rather than inferred from a count. Only for passes
    // at the lighting job - printing all 96 would be thousands of lines.
    std::string mir, real, vol;
    const uint32_t stored = s.distinct < kP1SlotViews ? s.distinct : kP1SlotViews;
    for (uint32_t i = 0; i < stored; ++i) {
        if (s.cls[i] == kP1Oth) continue;   // content textures, LUTs - not the question
        std::string& into = s.cls[i] == kP1Mir ? mir : (s.cls[i] == kP1Real ? real : vol);
        into += std::format(" {}", reinterpret_cast<void*>(s.views[i]));
    }
    log::warn(std::format("P1Handles side={} phase={} pass={} MIR=[{} ] REAL=[{} ] VOL=[{} ]",
        side, phase, pass, mir, real, vol));
}

void c1_emit_table();
void w1_emit_table();
void c2_emit_table();
void c3_emit_table(const std::unordered_map<int32_t, std::vector<std::string>>& names);
void m1_emit_matrices(const std::unordered_map<int32_t, std::vector<std::string>>& names);
std::atomic_bool v1_emitted{false};

// Idempotent. Called from p1_emit_table (the primary path) AND from the
// scripted test's end/abort, because P1 only emits on a CONVERGED frame: a run
// that never converges would otherwise throw away a capture that succeeded.
void v1_emit_table() {
    if (v1_emitted.exchange(true, std::memory_order_relaxed)) return;
    v1_armed.store(false, std::memory_order_relaxed);
    log::warn(std::format(
        "V1ViewIndex BEGINS. selftest={}. TWO jobs, separate budgets ({}/side each): job0={} "
        "({:#x}) job1={} ({:#x}). THE FIRST CONTROL: selftest=FAIL means the decoder never armed "
        "and every row below is absent BY CONSTRUCTION, not by measurement. A job with "
        "realDispatches=0 never went through JobDispatch on the real side in this window and has "
        "no baseline - that is blindness, not a null result.",
        v1_selftest_passed.load(std::memory_order_relaxed) ? "PASS" : "FAIL", kV1PerSide,
        kV1JobName[0], kV1JobRva[0], kV1JobName[1], kV1JobRva[1]));

    for (uint32_t j = 0; j < kV1Jobs; ++j) {
        const uint32_t rs = v1_seen[j][0].load(std::memory_order_relaxed);
        const uint32_t ds = v1_seen[j][1].load(std::memory_order_relaxed);
        const uint32_t rk = v1_kept[j][0].load(std::memory_order_relaxed);
        const uint32_t dk = v1_kept[j][1].load(std::memory_order_relaxed);
        log::warn(std::format(
            "V1 JOB {} ({:#x}): realDispatches={} kept={} doubledDispatches={} kept={}",
            kV1JobName[j], kV1JobRva[j], rs, rk, ds, dk));
        for (uint32_t s = 0; s < kV1Sides; ++s) {
            const uint32_t n = v1_kept[j][s].load(std::memory_order_relaxed);
            for (uint32_t i = 0; i < n && i < kV1PerSide; ++i) {
                const V1Rec& r = v1_recs[j][s][i];
                log::warn(std::format(
                    "V1 {} {} #{} ok={:#x} jobArg={:#x} mid={:#x} viewCtx={:#x} ctxLive={} "
                    "earlyOut={} pred[9A0]={} pred[9F40]={} queueFam={} latch[705CA28]={} "
                    "arg08={:#x} arg10={:#x} arg18={:#x} plus20={:#x} perViewArr={:#x} "
                    "E1[tblPtr={:#x} tblCount={} c20={}]",
                    kV1JobName[j], kV1SideName[s], i, r.ok, r.job_arg, r.mid, r.view_ctx,
                    (r.ok & kV1OkCtxLive) ? "yes" : "NO", r.early_out, r.pred_g9a0,
                    r.pred_g9f40, r.queue_family, r.frame_delta,
                    r.arg08, r.arg10, r.arg18,
                    static_cast<uint32_t>(r.view_index), r.per_view_arr,
                    r.tbl_ptr, r.tbl_count, r.tbl_c20));
            }
        }

        // LAYOUT CONTROL, corrected. The 2026-08-26 run compared the doubled
        // side's FIRST record against the real side's FIRST, but with hundreds
        // of dispatches in the window those are nowhere near consecutive, so it
        // could never line up and reported a false failure.
        //
        // The right form is WITHIN ONE SIDE, BETWEEN CONSECUTIVE RECORDS:
        // 0x19AEA52 stores jobArg[0x10]'s low dword into [0x705CA28] as the job
        // runs, and each record reads that latch BEFORE its own dispatch, so
        //     latch(#N) == arg10(#N-1)
        // must hold for every adjacent pair. It held 11/11 for the particle job
        // on 2026-08-26. NOTE this control is only meaningful for the job that
        // actually writes [0x705CA28]; for the other it is expected to FAIL and
        // that failure says nothing about that job's layout.
        for (uint32_t s = 0; s < kV1Sides; ++s) {
            const uint32_t n = v1_kept[j][s].load(std::memory_order_relaxed);
            uint32_t pairs = 0, hits = 0;
            for (uint32_t i = 1; i < n && i < kV1PerSide; ++i) {
                ++pairs;
                if (static_cast<uint32_t>(v1_recs[j][s][i].frame_delta) ==
                    static_cast<uint32_t>(v1_recs[j][s][i - 1].arg10)) ++hits;
            }
            if (pairs) {
                log::warn(std::format(
                    "V1 LAYOUT CONTROL {} {}: latch(#N)==arg10(#N-1) on {}/{} adjacent pairs. "
                    "All pairs matching means jobArg[0x10] is confirmed to be what 0x19AEA52 "
                    "latches for THIS job. Zero matching is expected for a job that does not "
                    "write that latch and must not be read as a layout failure.",
                    kV1JobName[j], kV1SideName[s], hits, pairs));
            }
        }

        // Live filter and cross-side comparison, per job.
        const V1Rec* fr = nullptr;
        const V1Rec* fd = nullptr;
        uint32_t early[kV1Sides] = {0, 0};
        for (uint32_t s = 0; s < kV1Sides; ++s) {
            const uint32_t n = v1_kept[j][s].load(std::memory_order_relaxed);
            for (uint32_t i = 0; i < n && i < kV1PerSide; ++i) {
                const V1Rec& r = v1_recs[j][s][i];
                if (r.early_out != 0) { ++early[s]; continue; }
                if (!(r.ok & kV1OkCtxLive)) continue;
                if (s == 0 && !fr) fr = &r;
                if (s == 1 && !fd) fd = &r;
            }
        }
        log::warn(std::format(
            "V1 LIVE FILTER {}: realEarlyOut={} doubledEarlyOut={} firstLiveReal={} "
            "firstLiveDoubled={}. An early-out dispatch binds NOTHING ([0x705C400] non-zero "
            "returns the job at its entry); a record without ctxLive is stack garbage shaped "
            "like data. Both are excluded.",
            kV1JobName[j], early[0], early[1], fr ? "yes" : "NO", fd ? "yes" : "NO"));
        if (fr && fd) {
            log::warn(std::format(
                "V1 CONTEXT {}: real viewCtx={:#x} mid={:#x} | doubled viewCtx={:#x} mid={:#x} | "
                "sameCtx={} sameMid={} | predicate: pred[9A0]={} pred[9F40]={} queueFam={} => "
                "path={}. READING: sameCtx=true means BOTH dispatches bind out of the SAME "
                "context object, so nothing about the binding can differ except what MUTATES "
                "that object between the two dispatches. path=B is the no-copy path and is what "
                "both arms were shown to take.",
                kV1JobName[j], fr->view_ctx, fr->mid, fd->view_ctx, fd->mid,
                fr->view_ctx == fd->view_ctx, fr->mid == fd->mid,
                fr->pred_g9a0, fr->pred_g9f40, fr->queue_family,
                (fr->pred_g9a0 != 0 && fr->pred_g9f40 == 0 && fr->queue_family >= 0) ? "A" : "B"));
            // E1 VERDICT. The bounds check at 0x19AF14F is `idx >= [0x705CA24]`
            // -> take the DEFAULT object. If the doubled dispatch sees a
            // smaller (or zero) count than the real one, every index fails and
            // BOTH declarations take that default - the duplicate C3 measured.
            log::warn(std::format(
                "E1 BOUNDS {}: real tblPtr={:#x} tblCount={} c20={} | doubled tblPtr={:#x} "
                "tblCount={} c20={} | countDiffers={} doubledCountZero={}. READING: the loop at "
                "0x19AF143 takes the DEFAULT object [[0x3848D98]+0x10] whenever idx >= tblCount "
                "or idx < 0, and 0x19AF2C4 ZEROES tblCount at the end of every run. "
                "doubledCountZero=true means every index failed on the doubled side and BOTH "
                "gpuParticleVectorField1 and particles took the default - exactly the duplicate "
                "at binding 2. countDiffers=false means the bounds check is NOT the "
                "discriminator and the index VALUES are, so the next probe reads the per-system "
                "index arrays instead.",
                kV1JobName[j], fr->tbl_ptr, fr->tbl_count, fr->tbl_c20,
                fd->tbl_ptr, fd->tbl_count, fd->tbl_c20,
                fr->tbl_count != fd->tbl_count, fd->tbl_count == 0));
        } else {
            log::warn(std::format(
                "V1 CONTEXT {}: WITHHELD - one or both sides produced no live record. This is "
                "instrument failure for this job, not a finding.", kV1JobName[j]));
        }
    }
    log::warn("V1ViewIndex COMPLETE.");
    c1_emit_table();
}

void c1_emit_table() {
    c1_armed.store(false, std::memory_order_relaxed);
    log::warn(std::format(
        "C1Slots BEGINS. selftest={} decls={} reps={}/side. THE FIRST CONTROL: selftest=FAIL "
        "means the resolver never armed and every row below is absent BY CONSTRUCTION. A decl "
        "printed as UNRESOLVED had a null global - that is NOT the same as a slot holding "
        "nothing, and must never be read as one.",
        c1_selftest_passed.load(std::memory_order_relaxed) ? "PASS" : "FAIL",
        kC1DeclCount, kC1Reps));

    for (uint32_t j = 0; j < kV1Jobs; ++j) {
        const uint32_t nr = c1_taken[j][0].load(std::memory_order_relaxed);
        const uint32_t nd = c1_taken[j][1].load(std::memory_order_relaxed);
        log::warn(std::format("C1 JOB {} ({:#x}): realSnaps={} doubledSnaps={}",
            kV1JobName[j], kV1JobRva[j], nr, nd));
        if (!nr || !nd) {
            log::warn(std::format(
                "C1 {}: WITHHELD - one side produced no snapshot. Instrument failure for this "
                "job UNLESS G1 is ON, in which case the doubled dispatch was deliberately "
                "suppressed and this absence is the intervention working. Check 'G1 STATE'. "
                "job, not a finding.", kV1JobName[j]));
            continue;
        }
        const C1Snap& rpre = c1_pre[j][0][0];
        const C1Snap& rpost = c1_post[j][0][0];
        const C1Snap& dpre = c1_pre[j][1][0];
        const C1Snap& dpost = c1_post[j][1][0];
        log::warn(std::format(
            "C1 {} ctx: realPre={:#x} doubledPre={:#x} sameCtx={}",
            kV1JobName[j], rpre.ctx, dpre.ctx, rpre.ctx == dpre.ctx));
        uint32_t diff_pre = 0, diff_post = 0, resolved = 0;
        for (uint32_t i = 0; i < kC1DeclCount; ++i) {
            const C1Slot& a = rpre.d[i];
            const C1Slot& b = rpost.d[i];
            const C1Slot& c = dpre.d[i];
            const C1Slot& e = dpost.d[i];
            if (!(a.ok & kC1OkDecl) && !(c.ok & kC1OkDecl)) {
                log::warn(std::format("C1 {} {:<26} UNRESOLVED (decl global null both sides)",
                    kV1JobName[j], kC1Decls[i].name));
                continue;
            }
            ++resolved;
            const bool same_pre = (a.ok & kC1OkHandle) && (c.ok & kC1OkHandle) &&
                                  a.lo == c.lo && a.hi == c.hi;
            const bool same_post = (b.ok & kC1OkHandle) && (e.ok & kC1OkHandle) &&
                                   b.lo == e.lo && b.hi == e.hi;
            if ((a.ok & kC1OkHandle) && (c.ok & kC1OkHandle) && !same_pre) ++diff_pre;
            if ((b.ok & kC1OkHandle) && (e.ok & kC1OkHandle) && !same_post) ++diff_post;
            log::warn(std::format(
                "C1 {} {:<26} slot={} | REALpre {:#018x}/{:#018x} idx={} -> post {:#018x} idx={} "
                "| DBLpre {:#018x}/{:#018x} idx={} -> post {:#018x} idx={} | samePre={} samePost={}",
                kV1JobName[j], kC1Decls[i].name, a.slot,
                a.lo, a.hi, a.idx, b.lo, b.idx,
                c.lo, c.hi, c.idx, e.lo, e.idx,
                same_pre, same_post));
        }
        log::warn(std::format(
            "C1 VERDICT {}: resolvedDecls={} differingBeforeDispatch={} differingAfter={}. "
            "READING: a decl differing in the BEFORE pair was already different when the doubled "
            "dispatch started - written by something else between the two dispatches, and that "
            "writer is the target. A decl differing only in the AFTER pair was rewritten by the "
            "job ITSELF, and the branch inside 0x19AE9E0 that chose differently is the target. "
            "resolvedDecls=0 means every global was null and this job binds none of them - "
            "blindness, not a null result.",
            kV1JobName[j], resolved, diff_pre, diff_post));
    }
    log::warn(std::format(
        "G1 STATE: enabled={} suppressedDoubledDispatches={}. THE CONTROL: suppressed=0 with "
        "enabled=ON means the job was never actually skipped and the run tests nothing. A large "
        "count with the fog ratio unmoved is a genuine null for this intervention.",
        g1_suppress_particle_sim.load(std::memory_order_relaxed) ? "ON" : "OFF",
        g1_suppressed.load(std::memory_order_relaxed)));
    log::warn(std::format(
        "F1 STATE: enabled={} attempts={} restores={} writeFailures={} noSavedCount={} "
        "savedCount={}. THE CONTROL: restores=0 with enabled=ON means the count was NEVER put "
        "back and the run tests nothing. noSavedCount>0 means the real side never presented a "
        "positive count to copy, which is a different failure and points at the arming window, "
        "not at the hypothesis.",
        f1_enabled.load(std::memory_order_relaxed) ? "ON" : "OFF",
        f1_attempts.load(std::memory_order_relaxed),
        f1_restores.load(std::memory_order_relaxed),
        f1_failures.load(std::memory_order_relaxed),
        f1_no_saved.load(std::memory_order_relaxed),
        f1_saved_count.load(std::memory_order_relaxed)));
    log::warn(std::format(
        "D1 STATE: enabled={} attempts={} writes={} writeFailures={} lastRealDeltaMs={}. THE "
        "CONTROL: writes=0 with enabled=ON means the forced timestep NEVER APPLIED and the run "
        "tests nothing. lastRealDeltaMs=0 means the real side never yielded a delta to copy.",
        d1_enabled.load(std::memory_order_relaxed) ? "ON" : "OFF",
        d1_attempts.load(std::memory_order_relaxed),
        d1_writes.load(std::memory_order_relaxed),
        d1_write_failures.load(std::memory_order_relaxed),
        d1_last_real_delta.load(std::memory_order_relaxed)));
    log::warn("C1Slots COMPLETE.");
    w1_emit_table();
}

void w1_emit_table() {
    w1_armed.store(false, std::memory_order_relaxed);
    const uintptr_t doom = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"DOOMx64vk.exe"));
    const uintptr_t ours = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"doomvr.dll"));
    const uint32_t n = w1_count.load(std::memory_order_relaxed);
    const uint32_t seen = w1_seen.load(std::memory_order_relaxed);
    log::warn(std::format(
        "W1WriteSite BEGINS. binding2Writes={} distinctChains={} dropped={} backtraceFailures={}. "
        "THE CONTROL: binding2Writes>0 with distinctChains=0 means RtlCaptureStackBackTrace "
        "itself failed and this table is instrument failure, NOT 'nothing wrote binding 2'. "
        "dropped>0 means the chain table saturated and an absent chain may simply not have fitted.",
        seen, n, w1_dropped.load(std::memory_order_relaxed),
        w1_backtrace_failures.load(std::memory_order_relaxed)));
    if (seen > 0 && n == 0) {
        log::warn("W1 WITHHELD - writes were counted but no chain was captured. Instrument "
                  "failure; nothing below may be read as a result.");
        log::warn("W1WriteSite COMPLETE.");
        return;
    }
    std::scoped_lock lock(w1_mutex);
    for (uint32_t i = 0; i < n && i < kW1MaxChains; ++i) {
        const W1Chain& c = w1_chains[i];
        std::string chain;
        uint32_t doom_frames = 0, our_frames = 0;
        for (uint32_t f = 0; f < c.frames; ++f) {
            if (!chain.empty()) chain += " <- ";
            const uintptr_t a = c.frame[f];
            // Attribute each frame. A chain lying entirely inside doomvr.dll
            // means WE wrote this descriptor, which inverts the reading.
            if (doom && a > doom && a - doom < 0x14000000) {
                chain += std::format("DOOM+{:#x}", a - doom);
                ++doom_frames;
            } else if (ours && a > ours && a - ours < 0x2000000) {
                chain += std::format("OURS+{:#x}", a - ours);
                ++our_frames;
            } else {
                chain += std::format("?{:#x}", a);
            }
        }
        log::warn(std::format(
            "W1 chain#{} side={} hits={} view={} doomFrames={} ourFrames={} | {}",
            i, c.side ? "DOUBLED" : "REAL   ", c.hits,
            reinterpret_cast<void*>(c.view), doom_frames, our_frames, chain));
    }
    log::warn(
        "W1 READING: compare the REAL chain that wrote the VOL view against the DOUBLED chain "
        "that wrote the OTH view. If the two chains are the SAME, one code path produced both "
        "and the difference is in its inputs - resolve the deepest DOOM frame with "
        "'pescan extent/dis' and read the branch there. If the chains DIFFER, the doubled call "
        "reaches a different builder entirely and the first frame where they diverge names it. "
        "A chain with ourFrames>0 at the deepest end is OUR write, not DOOM's.");
    log::warn("W1WriteSite COMPLETE.");
    c2_emit_table();
}

void c2_emit_table() {
    c2_armed.store(false, std::memory_order_relaxed);
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"DOOMx64vk.exe"));
    std::unordered_map<int32_t, std::vector<std::string>> names;
    bool clean = false;
    const uint32_t nodes = c2_build_name_map(base, names, clean);
    log::warn(std::format(
        "C2SlotDiff BEGINS. declListNodes={} terminatedCleanly={} namedSlots={} (cap {} nodes). "
        "THE CONTROL: terminatedCleanly=false means the walk hit its cap and the map is "
        "TRUNCATED - an unnamed slot below may simply have fallen off the end, and must not be "
        "read as 'no declaration owns it'.",
        nodes, clean, names.size(), kC2MaxNodes));

    for (uint32_t j = 0; j < kV1Jobs; ++j) {
        const uint32_t rt = c2_taken[j][0].load(std::memory_order_relaxed);
        const uint32_t dt = c2_taken[j][1].load(std::memory_order_relaxed);
        if (!rt || !dt) {
            log::warn(std::format(
                "C2 {}: WITHHELD - realSnap={} doubledSnap={}. Instrument failure for this job "
                "UNLESS G1 is ON, in which case the doubled dispatch was deliberately suppressed "
                "and this absence is expected. Check 'G1 STATE'.",
                kV1JobName[j], rt, dt));
            continue;
        }
        const C2Snap& r = c2_snap[j][0];
        const C2Snap& d = c2_snap[j][1];
        uint32_t both_readable = 0, differ = 0, both_zero = 0, reported = 0;
        std::string diff_list;
        for (uint32_t s = 0; s < kC2MaxSlots; ++s) {
            if (!r.ok[s] || !d.ok[s]) continue;
            ++both_readable;
            const bool zero = (r.lo[s] | r.hi[s] | d.lo[s] | d.hi[s]) == 0;
            if (zero) { ++both_zero; continue; }
            if (r.lo[s] == d.lo[s] && r.hi[s] == d.hi[s]) continue;
            ++differ;
            if (reported < kC2MaxReport) {
                ++reported;
                // see c2_name_of: ambiguous slots render as <AMBIGUOUS:...>.
                log::warn(std::format(
                    "C2 {} SLOT {} name={} | REAL {:#018x}/{:#018x} | DBL {:#018x}/{:#018x}",
                    kV1JobName[j], s,
                    c2_name_of(names, static_cast<int32_t>(s)),
                    r.lo[s], r.hi[s], d.lo[s], d.hi[s]));
            }
        }
        log::warn(std::format(
            "C2 VERDICT {}: ctxReal={:#x} ctxDoubled={:#x} sameCtx={} slotsReadableBoth={} "
            "bothZero={} DIFFERING={} reported={}. READING: DIFFERING=0 with a large "
            "slotsReadableBoth means the whole binding table is identical after the dispatch and "
            "binding 2's divergence is NOT in this context at all - look at the descriptor set "
            "build instead. Any named differing slot IS the input W1 proved one builder consumed "
            "differently; bothZero is dead space and evidence of nothing.",
            kV1JobName[j], r.ctx, d.ctx, r.ctx == d.ctx, both_readable, both_zero,
            differ, reported));
    }
    log::warn("C2SlotDiff COMPLETE.");
    c3_emit_table(names);
}

// ---------------------- C3: WHICH SLOT OWNS BINDING 2 --------------------
// C2 found 215 differing slots, but the VkImageView written at binding 2 is
// not the value stored in a slot - DOOM keeps its own resource object there and
// the view is derived from it. So: take the binding-2 view handle recorded per
// side, walk every slot with a plausible resource pointer, read a bounded
// window of that object, and report which slot's object CONTAINS the handle.
// That maps binding 2 to a named declaration without guessing.
//
// BOUNDED: kC3MaxScans objects, kC3Window bytes each, all guarded.
//
// CONTROLS:
//   * if either side's view handle was never recorded, the scan is skipped and
//     says so - a silent zero-match table would read like "no slot owns it";
//   * matches are reported per side, so a slot matching only ONE side is
//     distinguished from one matching both (which would mean the window caught
//     something incidental, not the binding);
//   * scansPerformed is reported, so a table that ran out of budget cannot look
//     like a table that found nothing.

void c3_emit_table(const std::unordered_map<int32_t, std::vector<std::string>>& names) {
    const uintptr_t vr = c3_view[0].load(std::memory_order_relaxed);
    const uintptr_t vd = c3_view[1].load(std::memory_order_relaxed);
    log::warn(std::format(
        "C3SlotOwner BEGINS. binding2ViewReal={:#x} binding2ViewDoubled={:#x} window={}B "
        "maxScans={}. THE CONTROL: a zero view handle means W1 never recorded that side and the "
        "scan below cannot speak for it.",
        vr, vd, kC3Window, kC3MaxScans));
    if (!vr && !vd) {
        log::warn("C3 WITHHELD - neither binding-2 view was recorded.");
        log::warn("C3SlotOwner COMPLETE.");
        return;
    }
    for (uint32_t j = 0; j < kV1Jobs; ++j) {
        if (!c2_taken[j][0].load(std::memory_order_relaxed) ||
            !c2_taken[j][1].load(std::memory_order_relaxed)) continue;
        const C2Snap& r = c2_snap[j][0];
        const C2Snap& d = c2_snap[j][1];
        uint32_t scans = 0, hits = 0;
        for (uint32_t s = 0; s < kC2MaxSlots && scans < kC3MaxScans; ++s) {
            for (uint32_t side = 0; side < kV1Sides; ++side) {
                const uintptr_t p = side ? d.lo[s] : r.lo[s];
                const uintptr_t want = side ? vd : vr;
                if (!want) continue;
                // Only plausible heap pointers, never floats or small ints.
                if (p < 0x10000 || (p & 0x7) != 0 || p > 0x7FFFFFFFFFFFull) continue;
                ++scans;
                uint8_t buf[kC3Window]{};
                if (!safe_read_bytes(p, buf, sizeof(buf))) continue;
                for (uint32_t off = 0; off + 8 <= sizeof(buf); off += 8) {
                    uintptr_t v = 0;
                    std::memcpy(&v, buf + off, sizeof(v));
                    if (v != want) continue;
                    ++hits;
                    // see c2_name_of: ambiguous slots render as <AMBIGUOUS:...>.
                    log::warn(std::format(
                        "C3 {} MATCH slot={} name={} side={} objectPtr={:#x} +{:#x} == "
                        "binding2View {:#x} | otherSideSlotPtr={:#x} slotDiffers={}",
                        kV1JobName[j], s,
                        c2_name_of(names, static_cast<int32_t>(s)),
                        side ? "DOUBLED" : "REAL", p, off, want,
                        side ? r.lo[s] : d.lo[s], r.lo[s] != d.lo[s]));
                    break;
                }
            }
        }
        log::warn(std::format(
            "C3 VERDICT {}: scansPerformed={} matches={}. READING: a slot matching on BOTH sides "
            "with slotDiffers=true is binding 2's declaration, and its name is the answer. "
            "matches=0 with scansPerformed large means binding 2's resource object does not hold "
            "the view within {}B of its base - widen the window rather than concluding.",
            kV1JobName[j], scans, hits, kC3Window));
    }
    log::warn("C3SlotOwner COMPLETE.");
    m1_emit_matrices(names);
}

// ---------------------- M1: WHAT IS IN THE MATRIX SLOTS ------------------
// Run 7 recorded, and never explained, that C2 slot 2248 `matricesPrevious`
// holds a NULL resource pointer on the REAL side and a real pointer on the
// DOUBLED side. Run 14 measured the `motionBlurParms` target (59x31 R16G16)
// SIGN-FLIPPED between the eyes - eye 0 non-negative, eye 1 non-positive, equal
// magnitude - which is exactly what a mismatched previous-frame transform
// produces, because for two eyes the camera delta is opposite in sign.
//
// M1 reads the actual bytes rather than reasoning about them further. For each
// matrix-ish slot it dumps the slot record and a window of the object it points
// at, on BOTH sides, as hex AND as floats. The question it answers offline:
// **is eye 1's "previous" transform eye 0's CURRENT one?**
//
// Deliberately dumps raw bytes instead of trying to parse a matrix out of a
// resource wrapper. The wrapper's layout is not established, and inventing an
// offset would be exactly the kind of guess that has cost this investigation
// runs. Raw hex plus floats lets the structure be read rather than assumed.
//
// CONTROLS:
//   * slots are chosen by NAME ONLY where the name is unambiguous, plus the
//     three slots C2 measured directly (370, 2248, 2249) which are included
//     regardless of what the name map says about them - the map is
//     corroborating, not an authority (run 16);
//   * every read is guarded, and an unreadable object prints as such rather
//     than as zeros;
//   * both sides are printed even when identical, so "no difference" is a
//     visible result rather than an absent one.
constexpr uint32_t kM1MaxSlots = 8;
constexpr uint32_t kM1Window = 128;

void m1_emit_matrices(const std::unordered_map<int32_t, std::vector<std::string>>& names) {
    std::vector<int32_t> slots{370, 2248, 2249};
    for (const auto& kv : names) {
        if (slots.size() >= kM1MaxSlots) break;
        if (kv.second.size() != 1) continue;  // ambiguous names are not names
        std::string low = kv.second.front();
        std::transform(low.begin(), low.end(), low.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (low.find("matri") == std::string::npos) continue;
        if (std::find(slots.begin(), slots.end(), kv.first) == slots.end())
            slots.push_back(kv.first);
    }
    log::warn(std::format(
        "M1Matrices BEGINS. slots={} window={}B. Reads the slot record and a raw window of the "
        "object it points at, both sides, as hex and float. THE QUESTION: is eye 1's PREVIOUS "
        "transform eye 0's CURRENT one? THE CONTROL: 'unreadable' means the object could not be "
        "read and that line says nothing - it is not a zero matrix.",
        slots.size(), kM1Window));

    for (uint32_t j = 0; j < kV1Jobs; ++j) {
        if (!c2_taken[j][0].load(std::memory_order_relaxed) ||
            !c2_taken[j][1].load(std::memory_order_relaxed)) continue;
        for (int32_t slot : slots) {
            if (slot < 0 || slot >= static_cast<int32_t>(kC2MaxSlots)) continue;
            for (uint32_t side = 0; side < kV1Sides; ++side) {
                const C2Snap& sn = c2_snap[j][side];
                const uintptr_t obj = sn.lo[slot];
                log::warn(std::format(
                    "M1 {} {} slot={} name={} record={:#018x}/{:#018x}",
                    kV1JobName[j], side ? "DOUBLED" : "REAL   ", slot,
                    c2_name_of(names, slot), sn.lo[slot], sn.hi[slot]));
                if (obj < 0x10000 || (obj & 0x7) != 0 || obj > 0x7FFFFFFFFFFFull) {
                    log::warn("M1     object pointer not plausible - nothing read");
                    continue;
                }
                uint8_t buf[kM1Window]{};
                if (!safe_read_bytes(obj, buf, sizeof(buf))) {
                    log::warn("M1     unreadable - this line says nothing, it is not zeros");
                    continue;
                }
                for (uint32_t off = 0; off < kM1Window; off += 32) {
                    std::string hex, flt;
                    for (uint32_t k = 0; k < 32; k += 4) {
                        uint32_t u = 0;
                        float f = 0.0f;
                        std::memcpy(&u, buf + off + k, sizeof(u));
                        std::memcpy(&f, buf + off + k, sizeof(f));
                        hex += std::format("{:08x} ", u);
                        flt += std::isfinite(f) ? std::format("{:11.5g} ", f)
                                                : std::string("        nan ");
                    }
                    log::warn(std::format("M1     +{:03x} {}| {}", off, hex, flt));
                }
            }
        }
        break;  // one job's snapshot is enough - the context is shared
    }
    log::warn("M1Matrices COMPLETE.");
}

void p1_emit_table(uint32_t attempt) {
    log::warn(std::format(
        "P1ReadTable attempt={} BEGINS. THE MISSING HALF of the working-arm comparison. The "
        "working arm (doom-capture-2, tools/rdc_eye1_reads.py) at the deferred lighting pass "
        "reads: real rdMIR=0 rdREAL=9, DOUBLED rdMIR=8 rdREAL=1 - eight mirror-chain images "
        "substituted one-for-one. ASKED = the sets DOOM's job handed us; BOUND = the clone "
        "redirection bound instead. The difference between them IS the redirection. Compare "
        "the DOUBLED/bound row at jobRva={:#x} against the REAL/asked row at the SAME rva - "
        "never by pass number, they move between runs. ⚠ Do NOT compare any of this to "
        "DescCensusPass 'mirror': that bucket counts DOOM source views we hold a mirror FOR, "
        "the opposite polarity, on a different denominator.",
        attempt, kP1ParticleSimJobRva));
    // PER-JOB TABLE FIRST - this is the one that answers the question. Keyed
    // by job RVA, because a render-pass ordinal can carry binds from two jobs
    // and run 1 proved this one does.
    uint32_t real_all_mir = 0, real_all_real = 0;
    uint32_t bound_all_mir = 0;
    uint32_t real_light_mir = 0, real_light_real = 0, real_light_seen = 0;
    uint32_t dbl_asked_real = 0, dbl_bound_mir = 0, dbl_bound_real = 0, dbl_light_seen = 0;
    for (uint32_t i = 0; i < p1_rva_count; ++i) {
        const uintptr_t key = p1_rva_keys[i];
        const P1Slot& r = p1_real_asked_rva[i];
        const P1Slot& a = p1_doubled_asked_rva[i];
        const P1Slot& b = p1_doubled_bound_rva[i];
        // is_light keeps the star on the particle job (where every prior P1/P1W
        // result was measured, under the wrong name). is_defer stars the REAL
        // deferred lighting job, which has never been measured - the RVA tables
        // already record every job, so this only makes its rows findable.
        const bool is_light = key == kP1ParticleSimJobRva;
        const bool is_defer = key == kP1DeferredPassesJobRva;
        const char* rtag = is_light ? "REAL*" : (is_defer ? "REAL+" : "REAL");
        const char* dtag = is_light ? "DOUBLD*" : (is_defer ? "DOUBLD+" : "DOUBLED");
        if (r.binds) p1_emit_row(rtag, "askedJ", i, r);
        if (a.binds) p1_emit_row(dtag, "askedJ", i, a);
        if (b.binds) p1_emit_row(dtag, "boundJ", i, b);
        real_all_mir += r.cls_count[kP1Mir];
        real_all_real += r.cls_count[kP1Real];
        bound_all_mir += b.cls_count[kP1Mir];
        // Handles for every job that reads anything from either chain - the
        // whole point is to follow a substitution one-for-one, and restricting
        // that to one RVA is what run 1 got wrong.
        if (r.cls_count[kP1Mir] + r.cls_count[kP1Real] > 0) p1_emit_handles("REAL", "askedJ", i, r);
        if (a.cls_count[kP1Mir] + a.cls_count[kP1Real] > 0) p1_emit_handles("DOUBLED", "askedJ", i, a);
        if (b.cls_count[kP1Mir] + b.cls_count[kP1Real] > 0) p1_emit_handles("DOUBLED", "boundJ", i, b);
        if (is_light) {
            real_light_seen += r.binds ? 1 : 0;
            dbl_light_seen += (a.binds || b.binds) ? 1 : 0;
            real_light_mir += r.cls_count[kP1Mir];
            real_light_real += r.cls_count[kP1Real];
            dbl_asked_real += a.cls_count[kP1Real];
            dbl_bound_mir += b.cls_count[kP1Mir];
            dbl_bound_real += b.cls_count[kP1Real];
        }
        // VOLUME PARITY, every job. Run 2 measured rdVOL=2 on the real side of
        // the lighting job and rdVOL=0 on the doubled side, with every other
        // job matching exactly - and the working arm's capture shows rd3D=2 on
        // BOTH sides. The froxel volumes are what carry the volumetrics eye 1
        // is missing, so this is either the finding or blindness, and the
        // noRecord counts are what tell the two apart.
        if (r.binds && a.binds && r.cls_count[kP1Vol] != a.cls_count[kP1Vol]) {
            log::warn(std::format(
                "P1 VOLUME DIVERGENCE at jobRva={:#x}: real binds {} 3D view(s), the doubled "
                "call binds {}. realSets={} realNoRecord={} doubledSets={} doubledNoRecord={}. "
                "⚠ READ noRecord FIRST: a descriptor set we hold no content record for "
                "contributes ZERO views and is indistinguishable from a set that binds none. "
                "Both noRecord at 0 means the doubled call genuinely does not sample these "
                "volumes; doubledNoRecord>0 means this instrument cannot see what it binds and "
                "the divergence is unproven. The froxel volumes carry the volumetric fog eye 1 "
                "lacks, and the working arm reads rd3D=2 on BOTH sides.",
                key, r.cls_count[kP1Vol], a.cls_count[kP1Vol],
                r.sets_seen, r.no_record, a.sets_seen, a.no_record));
        }
    }
    log::warn(std::format(
        "P1 JOB REGISTRY: {} distinct job RVAs this frame, overflow={}. Rows marked with * are "
        "the configured lighting job {:#x}. If its real-side row shows a read population "
        "unlike the capture's rdREAL=9, the RVA may simply not be this build's lighting job - "
        "identify it by SIGNATURE from the rows above, not by the constant.",
        p1_rva_count, p1_rva_overflow, kP1ParticleSimJobRva));
    // Per-pass rows, as context only. Kept because they show WHERE in the
    // frame each job's binds land, which the per-job table cannot.
    for (uint32_t p = 0; p < kP1PassSlots; ++p) {
        if (p1_real_asked[p].binds) p1_emit_row("REAL", "asked", p, p1_real_asked[p]);
    }
    for (uint32_t p = 0; p < kP1PassSlots; ++p) {
        if (p1_doubled_asked[p].binds) p1_emit_row("DOUBLED", "asked", p, p1_doubled_asked[p]);
        if (p1_doubled_bound[p].binds) p1_emit_row("DOUBLED", "bound", p, p1_doubled_bound[p]);
    }
    // THE POSITIVE CONTROL, checked in code and stated as a verdict rather
    // than left for the reader.
    //
    // It is NOT "reproduce rdc_eye1_reads' 9". That number comes from a
    // different tool with a different definition of a read-only image, and
    // demanding it would make a working classifier look broken. The control
    // that IS self-contained: the REAL call never reads our mirrors - nothing
    // substitutes its descriptors - so rdMIR on the real side must be 0, and
    // rdREAL must be NON-ZERO or the classifier cannot see the real chain at
    // all. Both together prove it can tell the two chains apart.
    // CONTROL A - the real call is never substituted, so it must never be seen
    // reading one of our mirrors, on ANY job.
    const bool ctrl_a = real_all_mir == 0;
    // CONTROL B - it must nonetheless see the real chain somewhere, or it
    // cannot tell the two chains apart at all.
    const bool ctrl_b = real_all_real > 0;
    // CONTROL C - THE ONE RUN 1 DID NOT HAVE, and the one that would have
    // named its bug on sight. ReadRedirect counts its own substitutions
    // independently. If it substituted and the bound side still shows rdMIR=0
    // everywhere, the classifier cannot recognise our own mirrors - which is
    // exactly what happened when mirror views were matched only by image.
    const uint32_t substitutions = read_redirect_substitutions.load(std::memory_order_relaxed);
    const bool ctrl_c = substitutions == 0 || bound_all_mir > 0;
    const bool control_ok = ctrl_a && ctrl_b && ctrl_c;
    log::warn(std::format(
        "P1 CONTROL: A realRdMIR={} (must be 0) {} | B realRdREAL={} (must be >0) {} | "
        "C readRedirectSubstitutions={} boundRdMIR={} (substitutions>0 demands boundRdMIR>0) {} "
        "-> {}. A failing: the classifier calls DOOM's own images mirrors and every row is "
        "void. B failing: it cannot see the real chain, so a doubled rdMIR=0 would be "
        "instrument failure rather than a finding. C failing: redirection demonstrably ran and "
        "the instrument cannot see its output - run 1 failed exactly here, silently, because it "
        "had no C.",
        real_all_mir, ctrl_a ? "PASS" : "FAIL",
        real_all_real, ctrl_b ? "PASS" : "FAIL",
        substitutions, bound_all_mir, ctrl_c ? "PASS" : "FAIL",
        control_ok ? "PASS" : "FAIL - draw no conclusion from the rows above"));
    if (real_light_seen == 0 || dbl_light_seen == 0) {
        log::warn(std::format(
            "P1 NOTE: the configured lighting job {:#x} produced no rows on {} side this frame. "
            "That is NOT the same as a failed control - it means the constant does not name a "
            "job that binds descriptors here. Identify the lighting job by signature from the "
            "P1Read *J rows (the capture's real side reads rdREAL=9 with rd3D=2) and re-scope.",
            kP1ParticleSimJobRva,
            real_light_seen == 0 ? (dbl_light_seen == 0 ? "EITHER" : "the real") : "the doubled"));
    }
    log::warn(std::format(
        "P1 VERDICT: at jobRva={:#x} the DOUBLED call ASKED for rdREAL={} and BOUND rdMIR={} "
        "rdREAL={} over {} pass(es). READING: bound rdMIR high with rdREAL collapsing toward 0 "
        "means redirection resolves the same chain the working arm does, our redirect logic is "
        "NOT the difference, and only memory CONTENTS remain. Bound rdMIR~0 with rdREAL still "
        "high means the doubled lighting pass is reading DOOM's real images in this arm where "
        "the working arm reads eight mirrors - the bug is in our redirect resolution and the "
        "failing views are named on the P1Handles lines above.",
        kP1ParticleSimJobRva, dbl_asked_real, dbl_bound_mir, dbl_bound_real, dbl_light_seen));
    log::warn(std::format(
        "P1 WHOLE-CALL ROLLUP, across every job: the real call read rdMIR={} rdREAL={}; the "
        "doubled call BOUND rdMIR={}. This is the scope-independent version of the verdict and "
        "does not depend on {:#x} being the right constant. The doubled call binding a large "
        "mirror population means redirection resolves; binding none while "
        "readRedirectSubstitutions is non-zero means it does not.",
        real_all_mir, real_all_real, bound_all_mir, kP1ParticleSimJobRva));
    // ------------------------------------------------- P1W: WRITE ORIGIN --
    // Replay the captured descriptor writes against the lighting job's sets.
    // The question this settles: did DOOM write binding 2 of the doubled
    // call's sets with a non-volume view, or is our RECORD of those sets wrong?
    p1_write_probe_armed.store(false, std::memory_order_relaxed);
    {
        std::scoped_lock lock(p1_write_mutex);
        uint32_t vol_writes = 0;
        for (const auto& w : p1_writes) if (p1_volume_views.count(w.view)) ++vol_writes;
        log::warn(std::format(
            "P1W CAPTURE: calls={} inRange={} distinct={} volumeWrites={} dropped={}. THE "
            "CONTROLS, in order: calls=0 means the write hooks NEVER FIRED in this window - "
            "DOOM does not write image descriptors here at all and these sets are populated "
            "earlier or by a path we do not hook, so the probe cannot speak. calls>0 with "
            "volumeWrites=0 means it saw writes but never saw a volume bound anywhere, which is "
            "still blindness for this question. dropped>0 means the capture saturated and an "
            "absent write may simply not have fitted.",
            p1_write_calls.load(std::memory_order_relaxed),
            p1_write_in_range.load(std::memory_order_relaxed),
            p1_writes.size(), vol_writes,
            p1_writes_dropped.load(std::memory_order_relaxed)));
        const auto replay = [&](const char* side, const VkDescriptorSet* list, uint32_t count) {
            for (uint32_t i = 0; i < count; ++i) {
                bool found = false;
                for (const auto& w : p1_writes) {
                    if (w.set != list[i] || w.binding != 2) continue;
                    found = true;
                    log::warn(std::format(
                        "P1W side={} set={} binding=2 OBSERVED WRITE view={} class={}",
                        side, reinterpret_cast<void*>(list[i]),
                        reinterpret_cast<void*>(w.view), p1_class_name(p1_classify(w.view))));
                }
                if (!found) {
                    log::warn(std::format(
                        "P1W side={} set={} binding=2 NO WRITE OBSERVED in this window - our "
                        "record of it did not come from a write we saw. Copy, stale entry from a "
                        "recycled handle, or a write that predates the arm.",
                        side, reinterpret_cast<void*>(list[i])));
                }
            }
        };
        replay("REAL", p1_light_sets_real, p1_light_sets_real_count);
        replay("DOUBLED", p1_light_sets_doubled, p1_light_sets_doubled_count);
        log::warn(std::format(
            "P1W VERDICT INPUTS: lightingSets real={} doubled={}. READING: the DOUBLED sets "
            "showing an OBSERVED WRITE of a non-VOL view at binding 2 means DOOM wrote them that "
            "way when we re-invoked the job, and the missing volumetrics have a mechanism. The "
            "DOUBLED sets showing NO WRITE OBSERVED while the REAL sets show one means our "
            "RECORD is the thing that differs, not DOOM's behaviour, and nothing may be built on "
            "the divergence until that is resolved.",
            p1_light_sets_real_count, p1_light_sets_doubled_count));
        p1_writes.clear();
        p1_writes.shrink_to_fit();
        p1_write_keys.clear();
    }
    log::warn(std::format(
        "FroxelFix STATE: enabled={} substitutions={} missing={} rebuilds={} "
        "realVolumeThisFrame={}. THE CONTROL, and it decides whether a null result means "
        "anything: substitutions=0 means binding {} of the doubled call's lighting sets was "
        "never rewritten, so eye 1 is UNCHANGED and this run tests nothing. missing>0 means the "
        "real call bound no 3D volume we could see, so there was nothing to copy. rebuilds "
        "climbing is correct and expected - DOOM alternates two volumes and a cached clone must "
        "not outlive the swap.",
        kFixDoubledLightingVolume ? "ON" : "OFF",
        lighting_volume_substitutions.load(std::memory_order_relaxed),
        lighting_volume_missing.load(std::memory_order_relaxed),
        lighting_volume_rebuilds.load(std::memory_order_relaxed),
        reinterpret_cast<void*>(real_lighting_volume_view), kLightingVolumeBinding));
    log::warn("P1ReadTable COMPLETE.");
    v1_emit_table();
}

// CHAIN WALK camera rider (STEP 6.6 item 3). The same dynamic-uniform walk
// run_descriptor_census does, reduced to the one question hypotheses 5 and 6
// turn on: for each pass, is the camera record it binds the SHIFTED one?
//
// Split out rather than folded into the census because the census is heavy
// (it classifies every sampled view, holds mirror_mutex, and carries a line
// budget that has starved itself before) and because this must run during
// the A/B capture, where the census would emit thousands of lines. Takes
// swapchains_mutex only - no view classification, so mirror_mutex is not
// needed and the lock order is not extended.
void chainwalk_scan_camera(uint32_t first_set, uint32_t set_count, const VkDescriptorSet* sets,
    uint32_t dynamic_offset_count, const uint32_t* dynamic_offsets) {
    if (!sets || set_count == 0) return;
    const bool doubled = inside_frame_root_double;
    bool found = false;
    float fx = 0.0f, fy = 0.0f, fz = 0.0f;
    uint32_t records = 0;
    // Distinct translations seen in THIS bind call, in order.
    std::array<float, kChainWalkCamRecords> lx{}, ly{}, lz{};
    uint32_t local_distinct = 0;
    {
        std::unique_lock lock(swapchains_mutex);
        uint32_t dynamic_index = 0;
        for (uint32_t si = 0; si < set_count; ++si) {
            const auto bufs = descriptor_sets.find(sets[si]);
            if (bufs == descriptor_sets.end()) continue;
            std::vector<uint32_t> dyn;
            for (const auto& [binding, d] : bufs->second) {
                if (d.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
                    d.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)
                    dyn.push_back(binding);
            }
            std::sort(dyn.begin(), dyn.end());
            for (uint32_t binding : dyn) {
                if (dynamic_index >= dynamic_offset_count || !dynamic_offsets) break;
                const uint32_t dynamic_offset = dynamic_offsets[dynamic_index++];
                const auto& d = bufs->second.at(binding);
                if (d.range < 64 || d.range > 256) continue;  // camera-shaped only
                const auto fb = buffer_bindings.find(d.buffer);
                if (fb == buffer_bindings.end()) continue;
                const auto fm = memory_mappings.find(fb->second.memory);
                if (fm == memory_mappings.end()) continue;
                const VkDeviceSize absolute = fb->second.offset + d.offset + dynamic_offset;
                if (absolute < fm->second.offset) continue;
                const VkDeviceSize host_offset = absolute - fm->second.offset;
                if (fm->second.size != VK_WHOLE_SIZE && host_offset + 64 > fm->second.size) continue;
                std::array<float, 16> v{};
                std::memcpy(v.data(), fm->second.host + host_offset, 64);
                if (!looks_like_world_camera_matrix(v.data())) continue;
                // The TRANSLATION COLUMN, v[3]/v[7]/v[11] - not v[12..15].
                // Run 2 printed the latter, called it the translation row, and
                // read "no shift reaches the uniform" off a number that was
                // never the translation at all. Each row here is
                // (axis.xyz, offset), so the translation is the fourth column.
                fx = v[3]; fy = v[7]; fz = v[11];
                found = true;
                ++records;
                bool duplicate = false;
                for (uint32_t i = 0; i < local_distinct; ++i) {
                    if (lx[i] == fx && ly[i] == fy && lz[i] == fz) { duplicate = true; break; }
                }
                if (!duplicate && local_distinct < kChainWalkCamRecords) {
                    lx[local_distinct] = fx;
                    ly[local_distinct] = fy;
                    lz[local_distinct] = fz;
                    ++local_distinct;
                }
            }
        }
    }
    (void)first_set;
    if (!found) return;
    if (!doubled) {
        // Reference for this frame: the real call's own camera record.
        chainwalk_real_cam_x.store(fx, std::memory_order_relaxed);
        chainwalk_real_cam_y.store(fy, std::memory_order_relaxed);
        chainwalk_real_cam_z.store(fz, std::memory_order_relaxed);
        chainwalk_real_cam_seen.store(true, std::memory_order_relaxed);
        return;
    }
    const uint32_t slot = doubled_pass_seq < kCensusPassSlots
                              ? doubled_pass_seq : kCensusPassSlots - 1;
    ChainWalkCam& c = chainwalk_cams[slot];
    c.seen = true;
    c.records += records;
    c.rva = doubled_current_job_rva;
    // Merge this bind's distinct records into the pass's, still deduplicated.
    // A pass binds its sets many times; without dedup the four slots would
    // fill with copies of the first record and the second camera - the one
    // the whole question turns on - would never be recorded.
    for (uint32_t i = 0; i < local_distinct && c.distinct < kChainWalkCamRecords; ++i) {
        bool duplicate = false;
        for (uint32_t j = 0; j < c.distinct; ++j) {
            if (c.x[j] == lx[i] && c.y[j] == ly[i] && c.z[j] == lz[i]) { duplicate = true; break; }
        }
        if (duplicate) continue;
        c.x[c.distinct] = lx[i];
        c.y[c.distinct] = ly[i];
        c.z[c.distinct] = lz[i];
        ++c.distinct;
    }
}

// The STEP 5 deliverable: one complete per-pass table from one doubled
// call, emitted when that call ends. Then the census disarms itself and
// puts the job filter back - a complete table needs exactly one doubled
// call, so there is no reason to keep the game unfiltered any longer.
void emit_descriptor_census_summary() {
    uint32_t total_binds = 0, total_mirror = 0, total_shared = 0, total_unknown = 0;
    uint32_t total_no_record = 0, biggest_array = 0, passes = 0, mirror_passes = 0;
    log::warn("DescCensusPass TABLE begins. Columns: pass binds mirror shared unknown "
              "noRecord maxArray fbMirrored jobRva fb. 'mirror' counts sampled image "
              "views we hold a private mirror for - those reads are seeing DOOM's "
              "UNSHIFTED content and are what STEP 6 must redirect.");
    for (uint32_t p = 0; p < kCensusPassSlots; ++p) {
        const CensusPassTotals& t = census_pass_totals[p];
        if (!t.binds) continue;
        ++passes;
        if (t.mirror) ++mirror_passes;
        total_binds += t.binds;
        total_mirror += t.mirror;
        total_shared += t.shared;
        total_unknown += t.unknown;
        total_no_record += t.no_record;
        if (t.max_array > biggest_array) biggest_array = t.max_array;
        log::warn(std::format(
            "DescCensusPass pass={:3} binds={:5} mirror={:4} shared={:4} unknown={:5} "
            "noRecord={:3} maxArray={:3} fbMirrored={} jobRva={:#x} fb={}",
            p, t.binds, t.mirror, t.shared, t.unknown, t.no_record, t.max_array,
            t.fb_mirrored, t.job_rva, reinterpret_cast<void*>(t.fb)));
    }
    log::warn(std::format(
        "ComputeProbe TOTAL dispatches={} storageImageWrites mirrored={} unmirrored={}. "
        "unmirrored>0 means the doubled call writes storage images we hold NO mirror for - "
        "i.e. straight into DOOM's own intermediates. That would explain run 10 (eye 1's "
        "composite rebuilt from DOOM's unshifted compute output, so redirecting SAMPLED views "
        "could never restore its parallax) and is simultaneously a contamination path into "
        "eye 0. dispatches=0 means the doubled call runs no compute at all and the hypothesis "
        "is REFUTED.",
        doubled_dispatch_count.load(std::memory_order_relaxed),
        doubled_storage_mirrored.load(std::memory_order_relaxed),
        doubled_storage_unmirrored.load(std::memory_order_relaxed)));
    log::warn(std::format(
        "CoverageProbe TOTAL pushDescriptorCalls={} pushImageViewsWithMirror={} without={} "
        "barrier2Calls={} waitEventsWithImageBarriers={} copyBufferToImageCalls={} "
        "updateWithTemplateCallsAllTime={}. pushDescriptorCalls>0 is the important one: those "
        "name image views INLINE, so the census cannot see them and ReadRedirect cannot "
        "substitute them - a pass using them reads DOOM's unshifted images no matter what we "
        "do to descriptor sets. All zero means the remaining image-naming entry points are "
        "unused and the cause of run 10 is somewhere else entirely.",
        doubled_push_descriptor_count.load(std::memory_order_relaxed),
        doubled_push_images_mirrored.load(std::memory_order_relaxed),
        doubled_push_images_unmirrored.load(std::memory_order_relaxed),
        doubled_barrier2_count.load(std::memory_order_relaxed),
        doubled_wait_events_count.load(std::memory_order_relaxed),
        doubled_copy_buffer_to_image_count.load(std::memory_order_relaxed),
        update_template_count.load(std::memory_order_relaxed)));
    log::warn(std::format(
        "TransferProbe TOTAL imageTransfers={} srcNotMirrored={} dstNotMirrored={}. "
        "srcNotMirrored>0 means the doubled call READS DOOM's real unshifted image and carries "
        "that content forward - enough on its own to flatten eye 1. dstNotMirrored>0 means it "
        "WRITES DOOM's real image a second time per frame - contamination of eye 0. "
        "imageTransfers=0 refutes this one.",
        doubled_transfer_count.load(std::memory_order_relaxed),
        doubled_transfer_src_unmirrored.load(std::memory_order_relaxed),
        doubled_transfer_dst_unmirrored.load(std::memory_order_relaxed)));
    log::warn(std::format(
        "DescCensusPass TOTAL passes={} passesWithMirrorReads={} binds={} mirror={} "
        "shared={} unknown={} noRecord={} biggestArray={}. biggestArray=1 means "
        "conventional per-pass descriptor sets with NO bindless indexed array, so "
        "clone-and-substitute is viable; anything large would force per-resource "
        "classification instead. noRecord>0 means sets we cannot see into (write "
        "templates or pre-injection) and the mirror count is a LOWER BOUND.",
        passes, mirror_passes, total_binds, total_mirror, total_shared, total_unknown,
        total_no_record, biggest_array));
    // THE HEADLINE FOR THIS RUN. total_unknown has been the project's largest
    // unexplained bucket (4099) and this is the first time it is broken down.
    const uint32_t mk = census_miss_keying.load(std::memory_order_relaxed);
    const uint32_t mn = census_miss_nomirror.load(std::memory_order_relaxed);
    const uint32_t mi = census_miss_noimage.load(std::memory_order_relaxed);
    log::warn(std::format(
        "CensusMiss TOTAL distinctUnknownViewsResolved={} of a {} resolve budget "
        "MISS-KEYING={} MISS-NOMIRROR={} MISS-NOIMAGE={} (detail lines {} of cap {}). "
        "At the resolve budget the counts are a capped SAMPLE of distinct views - the "
        "ratio is still meaningful, the absolute totals are not.\n"
        "  MISS-KEYING>0 CONFIRMS the view-vs-image keying gap: we hold a mirror for the "
        "image, filed under a different VkImageView, so redirection has something to swap to "
        "and simply cannot find it. Fix = resolve mirrors by IMAGE, not by view handle.\n"
        "  MISS-NOMIRROR dominant instead means the mirror was never created for those "
        "targets and the fix is upstream, in which images become mirrored attachments.\n"
        "  ALL THREE ZERO with unknown={} is INSTRUMENT FAILURE, not a clean result: it "
        "would mean the census counted unknown views that the miss path never saw, and "
        "nothing below may be concluded from it.",
        mk + mn + mi, kMaxCensusMissResolve, mk, mn, mi,
        census_miss_lines.load(std::memory_order_relaxed), kMaxCensusMissLines,
        total_unknown));
}

// ---------------------------------------------------------------------------
// ReadRedirect (2026-08-17, STEP 6 PILOT).
//
// Framebuffer mirroring substitutes what a pass WRITES. This substitutes
// what a pass READS. The census proved the need: 54 of 69 passes in the
// doubled call sample image views we hold private mirrors for, so those
// passes reconstitute eye 1's image out of DOOM's UNSHIFTED content - which
// is exactly why dropping the job filter has always given eye 1 lighting
// and cost it the parallax (claim 8).
//
// Mechanism: when the pilot job binds a descriptor set whose sampled-image
// bindings include mirrored views, bind a CLONE of that set with only those
// bindings re-pointed at the mirror. DOOM's own set is never modified -
// mutating it in place would corrupt the real call too, since descriptor
// sets are read at GPU execution time and the real frame has not executed
// yet when we record the doubled call.
//
// Bindings for the clone come from what we OBSERVED being written
// (descriptor_sets / descriptor_image_sets), not from the layout. That is
// deliberate: it means we can only ever write descriptors we have a real
// recorded value for, and any gap becomes a refusal to clone rather than a
// set containing undefined descriptors. Cloning is all-or-nothing.
// State lives further up, beside hook_allocate_descriptor_sets, because the
// allocate/update hooks that maintain it are defined before this point.

// ---------------------------------------------------------------------------
// R1 - THE DISPATCH FATE MANIFEST (2026-08-27).
//
// docs/PLAN-2026-08-27-NEXT-SESSION.md §3 R1, REWEIGHTED by the offline
// reconciliation of the 08-26 log (docs/RESULT-2026-08-27-ARM-B-RECONCILIATION
// .md). Two things the plan asked this run to establish are already answered by
// the log we have, and are recorded here so this instrument does not spend a
// press re-asking them:
//
//   1. EVERY H1 census and EVERY final-stage band number of the 08-26 session
//      was taken with suppress_doubled_light_culling ALREADY OFF. The
//      LightCullAB block flips it ~3 s after step 35 and the FinalStage
//      snapshot (which is what sets h1_pending) is ~5.6 s after step 35. The
//      arm label is read from the live atomic in the snapshot line itself, so
//      this is the state, not an inference: every one of the last six runs
//      logs "FinalStage SNAPSHOT series=0 suppressLightCulling=OFF".
//      The "untreated" band 78.6-82.1% / 110.6-111.3% is therefore the
//      SUPPRESSION-OFF band, and uavSSDO's 20.1% was measured with nothing
//      suppressed.
//   2. Suppression removes EXACTLY 11 dispatches per doubled call: the counter
//      moves 4526->5219 = 693 between arming and the flip, over exactly 63
//      doubled calls, in two independent runs (693/63 = 11.00). ComputeProbe
//      shows the doubled call issues 13 dispatches - one at pass 16
//      (jobRva 0x18733c0, one storage image), ELEVEN at pass 18
//      (jobRva 0x19ae9e0, GPU Particle Simulate, no storage images), one at
//      pass 22 (jobRva 0x1873830, RenderDeferredPasses, TWO storage images).
//      11 = the particle set exactly. The lighting-accumulation dispatch has
//      never been in the kill set.
//
// That is consistent with the capture: eid 8341's ReadWriteBuffers 557/559 are
// PIXEL-stage bindings, and last_bound_compute_sets only ever holds sets bound
// at VK_PIPELINE_BIND_POINT_COMPUTE - so the predicate cannot see them.
//
// Both claims are PREDICTIONS this manifest re-measures in band, in one frame,
// and either confirms or destroys. They are stated in the arming line before
// the run so the result cannot be read backwards.
//
// WHAT IS ACTUALLY OPEN, and what this therefore measures:
//
//   * EXECUTION, not binding. ComputeProbe fires ABOVE the suppress-return
//     (handoff trap 7), so "storageMirrored=2 b8/b9 hit=mirror" proves the
//     doubled call BOUND two mirror-backed storage images at pass 22 and says
//     nothing about whether the dispatch ran. This records the verdict AFTER
//     the decision, per dispatch.
//   * WHERE THE WRITE LANDED. hit=mirror is the census classifier - "we hold a
//     mirror for the view DOOM bound" - which is a different question from
//     "the clone we actually bound carries our mirror". Only the second one
//     decides whether eye 1's HDR accumulation and uavSSDO receive the compute
//     write or DOOM's real images do. get_redirected_set now records its
//     per-binding storage verdict, and this reads it back at the dispatch.
//   * THE KILL SET, made visible. Plan trap 8: the predicate's reach grows
//     with descriptor bookkeeping, silently. wouldSuppress is evaluated and
//     logged for every dispatch REGARDLESS of the flag, so the kill set is a
//     printed fact in every future run rather than an inference from a counter.
//
// BOUNDED BY CONSTRUCTION: fixed record tables, one per side, deduped by
// (pass, first compute set) so pass 18's eleven dispatches collapse to one row
// carrying dispatches=11. Armed for exactly ONE real call and ONE doubled call
// - it disarms where the descriptor census disarms, at the end of the doubled
// call - and refuses to arm at all unless its self-test round-trips.
//
// CONTROL, fixed in advance: the REAL side has its own table and its own
// budget (a shared cap lets the frequent side starve the rare one). The real
// call must show dispatches at the twins of the capture's eids 6486/6607 -
// i.e. at jobRva 0x18733c0, 0x19ae9e0 and 0x1873830. A manifest that finds no
// real-side dispatch is BLIND, not a result, and says so in its own COMPLETE
// line.
enum DfSubst : uint8_t {
    kDfUnknown = 0,       // never evaluated - the set was never put through the clone path
    kDfNoMirror = 1,      // no mirror held for this view: the write goes to DOOM's own image
    kDfSubstituted = 2,   // the clone binds OUR mirror: the write lands in eye 1's copy
    kDfRefusedLayout = 3, // safety gate refused: DOOM's image again, have/want say why
    kDfSetRefused = 4,    // the WHOLE set was refused, so every binding in it is DOOM's
    kDfSetNoRedirect = 5, // the set was examined and needed no substitution at all
};

const char* df_subst_name(uint8_t o) {
    switch (o) {
    case kDfNoMirror: return "NO-MIRROR(writes DOOM's own)";
    case kDfSubstituted: return "SUBSTITUTED(writes OUR mirror)";
    case kDfRefusedLayout: return "REFUSED-LAYOUT(writes DOOM's own)";
    case kDfSetRefused: return "WHOLE-SET-REFUSED(writes DOOM's own)";
    case kDfSetNoRedirect: return "SET-NEEDED-NO-REDIRECT";
    default: return "NOT-EVALUATED";
    }
}

struct DfStorageOutcome {
    uint8_t outcome{kDfUnknown};
    VkImageLayout have{VK_IMAGE_LAYOUT_UNDEFINED};
    VkImageLayout want{VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage mirror_image{VK_NULL_HANDLE};
};
// Written by get_redirected_set AFTER its locks are released (the gap_lines /
// gate_lines convention this file already uses - log and bookkeeping I/O must
// not happen under mirror_mutex, Rule 9 / the run-17 lesson), read by the
// dispatch hooks. Keyed per (set, binding); the verdict is the one that built
// the clone currently in use, which is exactly what the dispatch binds.
std::mutex df_outcome_mutex;
std::map<std::pair<VkDescriptorSet, uint32_t>, DfStorageOutcome> df_storage_outcomes;
std::map<VkDescriptorSet, uint8_t> df_set_outcomes;
constexpr size_t kDfMaxOutcomes = 256;
std::atomic_uint32_t df_outcomes_dropped{0};

void df_record_storage_outcome(VkDescriptorSet set, uint32_t binding, uint8_t outcome,
                               VkImageLayout have, VkImageLayout want, VkImage mirror) {
    std::scoped_lock lock(df_outcome_mutex);
    const auto key = std::make_pair(set, binding);
    if (df_storage_outcomes.size() >= kDfMaxOutcomes && !df_storage_outcomes.count(key)) {
        df_outcomes_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    df_storage_outcomes[key] = DfStorageOutcome{outcome, have, want, mirror};
}

void df_record_set_outcome(VkDescriptorSet set, uint8_t outcome) {
    std::scoped_lock lock(df_outcome_mutex);
    if (df_set_outcomes.size() >= kDfMaxOutcomes && !df_set_outcomes.count(set)) {
        df_outcomes_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    df_set_outcomes[set] = outcome;
}

// R2 (2026-08-27, after R1). READ THE CLONE, NOT THE VERDICT.
//
// R1's OUTCOME column came back NOT-EVALUATED on all three storage bindings,
// and that was MY defect, not the game's: get_redirected_set returns a cached
// clone before it reaches any recording point, and the clone for pass 22's set
// had been built three seconds earlier at generation 94. The verdict had to be
// reconstructed from a `ReadRedirect CLONE #14 ... refusedMirrors=0` line
// elsewhere in the same log - an inference from three lines, for the single
// fact the run existed to read directly.
//
// So stop asking what get_redirected_set DECIDED and read what the clone
// CONTAINS. These are the effective per-binding views actually written into
// the clone - `images[i].view` is post-substitution by construction, the same
// handle the GPU sees - recorded once when the clone is built and valid for as
// long as the clone is. Keyed by the SOURCE set, which is what a dispatch has
// in last_bound_compute_sets, and carrying the generation so a stale entry is
// visible rather than silently believed.
struct DfCloneViews {
    uint32_t generation{0};
    VkDescriptorSet clone{VK_NULL_HANDLE};
    std::vector<std::pair<uint32_t, VkImageView>> bindings;
};
std::mutex df_clone_mutex;
std::map<VkDescriptorSet, DfCloneViews> df_clone_effective;

void df_record_clone_views(VkDescriptorSet src, VkDescriptorSet clone, uint32_t generation,
                           std::vector<std::pair<uint32_t, VkImageView>> bindings) {
    std::scoped_lock lock(df_clone_mutex);
    if (df_clone_effective.size() >= kDfMaxOutcomes && !df_clone_effective.count(src)) {
        df_outcomes_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    df_clone_effective[src] = DfCloneViews{generation, clone, std::move(bindings)};
}

bool df_lookup_clone_views(VkDescriptorSet src, DfCloneViews& out) {
    std::scoped_lock lock(df_clone_mutex);
    auto it = df_clone_effective.find(src);
    if (it == df_clone_effective.end()) return false;
    out = it->second;
    return true;
}

bool df_lookup_storage_outcome(VkDescriptorSet set, uint32_t binding, DfStorageOutcome& out) {
    std::scoped_lock lock(df_outcome_mutex);
    auto it = df_storage_outcomes.find(std::make_pair(set, binding));
    if (it == df_storage_outcomes.end()) return false;
    out = it->second;
    return true;
}

uint8_t df_lookup_set_outcome(VkDescriptorSet set) {
    std::scoped_lock lock(df_outcome_mutex);
    auto it = df_set_outcomes.find(set);
    return it == df_set_outcomes.end() ? kDfUnknown : it->second;
}

// Everything cached here points at mirror image views. When a mirror dies
// the clones referencing it are dangling, so they all go.
void invalidate_read_redirect_cache(const char* why) {
    std::scoped_lock lock(read_redirect_mutex);
    if (redirected_sets.empty() && sets_needing_no_redirect.empty()) return;
    log::warn(std::format(
        "ReadRedirect: dropping {} cached clone(s) and {} no-redirect marks ({})",
        redirected_sets.size(), sets_needing_no_redirect.size(), why));
    // ⚠ THIS IS THE BIGGEST SINGLE LEAK SITE - it dropped EVERY live clone at
    // once. The comment that used to stand here read: "The sets themselves are
    // left allocated in our pool; it is reset only on device teardown. Bounded
    // by the number of distinct sets DOOM uses."
    //
    // That bound does not exist. The 23:04 run measured DOOM allocating one
    // set per call, ~1200 per 6-second window, with staleBufCleared ~= allocSets
    // - it recycles handles continuously, so distinct handles over time are
    // unbounded and the pool (maxSets=1024) is guaranteed to run out. Every
    // clone dropped here is now RETIRED and freed once the GPU is past it.
    for (auto& [src, rec] : redirected_sets) {
        (void)src;
        retire_clone(rec.clone);
    }
    redirected_sets.clear();
    sets_needing_no_redirect.clear();
}

bool ensure_read_redirect_pool(VkDevice device) {
    if (read_redirect_pool != VK_NULL_HANDLE) return true;
    if (read_redirect_pool_failed) return false;
    auto create_pool = load_device_proc<PFN_vkCreateDescriptorPool>(device, "vkCreateDescriptorPool");
    if (!create_pool) {
        read_redirect_pool_failed = true;
        log::error("ReadRedirect: vkCreateDescriptorPool unavailable; redirection disabled");
        return false;
    }
    const std::array<VkDescriptorPoolSize, 8> sizes{{
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8192},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4096},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 2048},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2048},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2048},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1024},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4096}}};
    VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    info.maxSets = 1024;
    // FREE_DESCRIPTOR_SET so a clone invalidated by a mirror-generation
    // change can be recycled instead of leaking a pool slot per rebuild.
    info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    info.poolSizeCount = static_cast<uint32_t>(sizes.size());
    info.pPoolSizes = sizes.data();
    if (create_pool(device, &info, nullptr, &read_redirect_pool) != VK_SUCCESS) {
        read_redirect_pool = VK_NULL_HANDLE;
        read_redirect_pool_failed = true;
        log::error("ReadRedirect: descriptor pool creation FAILED; redirection disabled, the "
                   "doubled call will read DOOM's unshifted images");
        return false;
    }
    // Captured so the retirement drain can free without a device to hand. The
    // pool is per-device and created once, so this cannot go stale beneath it.
    read_redirect_device = device;
    log::warn("ReadRedirect: descriptor pool created (maxSets=1024)");
    return true;
}

// Returns a clone with mirror-backed sampled images re-pointed, or
// VK_NULL_HANDLE meaning "bind the original unchanged". Every reason for the
// latter is logged (Rule 3) - a silent fallback here would look exactly like
// a redirection that ran and did not help.
VkDescriptorSet get_redirected_set(VkDevice device, VkDescriptorSet src, uint32_t set_index) {
    // Empty layouts legally have no writes. Only a captured, genuinely empty
    // layout can bypass cloning without proving descriptor contents.
    if(kharvox::native::isKnownEmptySet(src)) {
        static uint32_t noted{};
        if(noted++<4)log::info(std::format("Native empty descriptor layout: set={} index={} (no resources to redirect)",reinterpret_cast<void*>(src),set_index));
        return VK_NULL_HANDLE;
    }
    const uint32_t generation = mirror_generation.load(std::memory_order_relaxed);
    VkDescriptorSet stale_clone = VK_NULL_HANDLE;
    // BIND-TIME VOLUME REDIRECT (2026-08-22). Decided BEFORE the early-outs.
    //
    // P1: at pass 5 the real call's sets bind 2 froxel volumes and the doubled
    // call's bind none, on the ASKED side. CloneAudit: volume-carrying sets are
    // never cloned - they take the sets_needing_no_redirect path below and pass
    // through untouched. And vr_writes_doubled=0: the doubled call writes NO
    // image descriptors at all, it only RE-BINDS sets the real call wrote. So
    // there is no write to patch, and a set that needs its volume put back will
    // never get a clone unless we ask for one here.
    //
    // This asks. If the source set holds a NON-3D view at a binding where the
    // real call of this frame bound a 3D volume, the set must be cloned even
    // though nothing else about it needs redirecting, and the volume written
    // in. Off by default; the flag is the same one SHIFT+F11 toggles.
    bool needs_volume_fix = false;
    if (volume_restore_enabled.load(std::memory_order_relaxed)) {
        std::unique_lock lock(swapchains_mutex);
        const VolumeJobRecord* jslot = volume_job_slot(doubled_current_job_rva, false);
        if (jslot) {
        if (auto imgs0 = descriptor_image_sets.find(src); imgs0 != descriptor_image_sets.end()) {
            for (const auto& [b, rec0] : imgs0->second) {
                if (b >= kVolumeRestoreBindings) continue;
                if (jslot->view[b] == VK_NULL_HANDLE) continue;
                if (rec0.image_view == jslot->view[b]) continue;
                const auto vi = image_view_infos.find(rec0.image_view);
                const bool is_3d = vi != image_view_infos.end() &&
                                   vi->second.view_type == VK_IMAGE_VIEW_TYPE_3D;
                if (!is_3d) { needs_volume_fix = true; break; }
            }
        }
        }
    }
    {
        std::scoped_lock lock(read_redirect_mutex);
        if (auto it = redirected_sets.find(src); it != redirected_sets.end()) {
            // FROXEL FIX staleness. A clone built against the OTHER volume of
            // the temporal pair would bind last frame's froxels on every frame
            // the pair swaps - a half-correct result that would read as a
            // half-working fix. Treated exactly like a mirror-generation
            // change: rebuild and recycle.
            //
            // The bind-time redirect keys on the same alternation. VolumeRecord
            // measured it directly: bindings 2 and 3 swap between two handles
            // frame to frame, so a cached clone must be rebuilt whenever the
            // volume it was built against is no longer the one the real call
            // bound this frame.
            const VkImageView vol_key =
                volume_restore_enabled.load(std::memory_order_relaxed)
                    ? (volume_job_slot(doubled_current_job_rva, false)
                           ? volume_job_slot(doubled_current_job_rva, false)
                                 ->view[kVolumeRestorePrimaryBinding]
                           : VK_NULL_HANDLE)
                    : real_lighting_volume_view;
            const bool volume_stale =
                ((kFixDoubledLightingVolume &&
                  doubled_current_job_rva == kP1ParticleSimJobRva) ||
                 volume_restore_enabled.load(std::memory_order_relaxed)) &&
                it->second.volume_used != vol_key;
            if (volume_stale) lighting_volume_rebuilds.fetch_add(1, std::memory_order_relaxed);
            if (it->second.generation == generation && !volume_stale) return it->second.clone;
            // Mirrors changed under it; rebuild and recycle the old one.
            stale_clone = it->second.clone;
            redirected_sets.erase(it);
        } else if (auto nr = sets_needing_no_redirect.find(src);
                   nr != sets_needing_no_redirect.end()) {
            // A set that needs its volume restored must NOT take this
            // pass-through, even though nothing else about it needs
            // redirecting. This is the whole reason needs_volume_fix is
            // computed above the lock.
            if (nr->second == generation && !needs_volume_fix) return VK_NULL_HANDLE;
            sets_needing_no_redirect.erase(nr);
        }
    }
    if (stale_clone != VK_NULL_HANDLE) {
        {std::scoped_lock lock(read_redirect_mutex);retire_clone(stale_clone);}
        // P1: drop the freed clone's record with it. The handle can be
        // recycled by the pool, and a stale entry would attribute one set's
        // views to another - the recycled-handle failure this file already
        // refuses sets over elsewhere.
        std::scoped_lock lock(p1_clone_mutex);
        p1_clone_views.erase(stale_clone);
    }
    // Snapshot what we know about this set, and resolve mirror views, with
    // the mirror lock held only for the lookup itself.
    {
        std::scoped_lock lock(read_redirect_mutex);
        if (sets_with_untracked_bindings.count(src)) {
            kharvox::native::fail("untracked descriptor type in native clone");
            const uint32_t f = read_redirect_failures.fetch_add(1, std::memory_order_relaxed);
            if (f < kMaxAttributionLines) {
                // STEP 6.6 identity, same shape as the other refusal paths.
                // No mirrorBacked count is available here: this returns before
                // the set's contents are walked, by design - we cannot see
                // into it at all, which is the reason for the refusal.
                log::warn(std::format(
                    "SetRefusedID #{} pass={} jobRva={:#x} set={} setIndex={} mirrorBacked=? "
                    "reason=untracked-descriptor-type",
                    f, doubled_pass_seq, doubled_current_job_rva,
                    reinterpret_cast<void*>(src), set_index));
            }
            if (f < 10) {
                log::error(std::format(
                    "ReadRedirect FALLBACK: set {} carries a descriptor type we do not record "
                    "(texel buffer / inline uniform / acceleration structure). Cloning it would "
                    "leave that binding UNDEFINED, so it is bound unmodified. This pass reads "
                    "DOOM's unshifted images.", reinterpret_cast<void*>(src)));
            }
            sets_needing_no_redirect[src] = generation;
            df_record_set_outcome(src, kDfSetRefused);
            return VK_NULL_HANDLE;
        }
    }
    struct ImgWrite { uint32_t binding; VkDescriptorType type; VkImageView view; VkSampler sampler;
                      VkImageLayout layout; };
    struct BufWrite { uint32_t binding; VkDescriptorType type; VkBuffer buffer; VkDeviceSize offset, range; };
    std::vector<ImgWrite> images;
    // CloneDrop census - the asked bindings, recorded BEFORE any skip or break
    // in the loop below, so a binding lost to an early exit is still counted.
    struct AskedBinding { uint32_t binding; int type; uint32_t count; VkImageView view; };
    std::vector<AskedBinding> clone_asked;
    std::vector<BufWrite> buffers;
    bool any_mirror = false, unclonable = false;
    std::string reason;
    uint32_t refused = 0;
    std::string refused_examples;
    // STEP 6.6 attribution. Built under the locks, EMITTED after they are
    // released - log.cpp flushes to disk per line and the render thread must
    // not do file I/O while holding mirror_mutex (Rule 9, the run-17 lesson).
    std::vector<std::string> gap_lines;
    std::vector<std::string> gate_lines;
    // R1 DISPATCH FATE. Same convention as gap_lines/gate_lines above and for
    // the same reason: collected under the locks, published after they are
    // released. df_record_storage_outcome takes its own mutex, and taking a
    // third lock underneath mirror_mutex and swapchains_mutex is what wedged
    // the render thread in run 17.
    struct DfPending { VkDescriptorSet set; uint32_t binding; uint8_t outcome;
                       VkImageLayout have, want; VkImage mirror; };
    std::vector<DfPending> df_pending;
    // What a WHOLE-SET refusal would have cost. Cloning is all-or-nothing, so
    // when a set is refused every mirror-backed sampled image in it reads
    // DOOM's unshifted content - not one binding, the lot. Run 18's log shows
    // all ten sampled ReadRedirect FALLBACK lines were exactly this, every one
    // of them the "binding appears as BOTH an image and a buffer" case, and
    // none of them named its pass, its job, or what it was carrying. Counting
    // 46 of those against 21,542 binds is the same wrong denominator as the
    // gap: the question is whether ONE of them is a full-screen pass's
    // primary input, and only the extents answer it.
    uint32_t mirror_backed = 0;
    std::string mirror_backed_detail;
    // Event-ring snapshot (plan §2 i/ii). Taken as a POD copy under the lock
    // that already guards the ring and FORMATTED after release - Rule 9. Costs
    // one map lookup per set EVALUATION (cached per set+generation), not per
    // bind.
    DescRing ring_snapshot{};
    bool have_ring = false;
    {
        std::scoped_lock mirror_lock(mirror_mutex);
        std::unique_lock lock(swapchains_mutex);
        if (auto r = descriptor_set_rings.find(src); r != descriptor_set_rings.end()) {
            ring_snapshot = r->second;
            have_ring = true;
        }
        const auto imgs = descriptor_image_sets.find(src);
        const auto bufs = descriptor_sets.find(src);
        if (imgs == descriptor_image_sets.end() && bufs == descriptor_sets.end()) {
            unclonable = true;
            reason = "no recorded contents for this set";
        }
        if (imgs != descriptor_image_sets.end()) {
            for (const auto& [binding, rec] : imgs->second) {
                clone_asked.push_back({binding, static_cast<int>(rec.type), rec.count,
                                       rec.image_view});
                if (rec.count > 1) {
                    kharvox::native::fail("descriptor arrays require complete element capture");
                    unclonable = true;
                    reason = std::format("binding {} is an array of {} (pilot handles count==1 "
                                         "only; the census measured maxArray=1 everywhere)",
                                         binding, rec.count);
                    break;
                }
                VkImageView use = rec.image_view;
                // MEASURE THE VIEW-KEYING GAP (2026-08-17, after run 16).
                //
                // mirrored_views is keyed on the exact VkImageView DOOM used
                // as a framebuffer ATTACHMENT. But an engine routinely
                // creates a SEPARATE view to sample the same image - other
                // mip range, other format, other aspect - and that is a
                // different handle. Such a binding misses the lookup, gets
                // classified "unknown", and is left pointing at DOOM's real
                // image even though we hold a perfectly good mirror of it.
                //
                // That would explain run 16 exactly: the G-buffer carries a
                // massive shift (weapon crosses the screen at 20 uu) while
                // the composite carries none, despite 18,845 substitutions -
                // because the substitutions that matter were never
                // recognised as substitutable. The census counted 6,483
                // "unknown" sampled views against 796 mirror hits.
                //
                // Counted before it is fixed. This session has repeatedly
                // paid for fixes that were not measured first.
                if (mirrored_views.find(rec.image_view) == mirrored_views.end() &&
                    mirror_shared_source_views.count(rec.image_view) == 0) {
                    // try_lock, NOT scoped_lock. This runs with mirror_mutex
                    // and swapchains_mutex already held, and taking a third
                    // lock underneath them is how run 17 died 13 ms after
                    // redirection was enabled - the same sequence run 16
                    // survived, with the same mirrorGeneration, differing
                    // only by this measurement. A diagnostic counter must
                    // not be able to wedge the render thread.
                    //
                    // Consequence: the count is a LOWER BOUND, since a
                    // contended moment skips a binding. That is fine for the
                    // question being asked, which is "large or near zero".
                    VkImage src_image = VK_NULL_HANDLE;
                    ImageViewInfo src_view_info{};
                    bool have_view_info = false;
                    {
                        std::unique_lock<std::mutex> fb(framebuffer_depth_view_mutex,
                                                        std::try_to_lock);
                        if (fb.owns_lock()) {
                            if (auto vi = image_view_to_image.find(rec.image_view);
                                vi != image_view_to_image.end())
                                src_image = vi->second;
                            // STEP 6.7a needs the sampling view's full create
                            // parameters, and this is the only lock under which
                            // they are readable. Taken in the SAME try_lock, so
                            // the fix inherits the run-17 safety property
                            // instead of adding a second chance to wedge.
                            if (auto vf = image_view_infos.find(rec.image_view);
                                vf != image_view_infos.end()) {
                                src_view_info = vf->second;
                                have_view_info = true;
                            }
                        } else {
                            read_redirect_gap_skipped.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    if (src_image != VK_NULL_HANDLE) {
                        if (auto mi = mirrored_images.find(src_image);
                            mi != mirrored_images.end()) {
                            const uint32_t n =
                                read_redirect_view_gap.fetch_add(1, std::memory_order_relaxed);
                            // IDENTITY, not just the count (STEP 6.6 item 1).
                            // The extent is the whole point: one 3818x2032
                            // bind here is a full-screen pass's primary input
                            // and can carry the entire image, while a hundred
                            // small ones carry nothing. Counting could never
                            // tell those apart, which is why 76/21542 was
                            // read as noise.
                            VkExtent2D ext{};
                            for (const auto& [sv, mv] : mirrored_views) {
                                if (mv.image == mi->second) { ext = mv.extent; break; }
                            }
                            if (n < kMaxAttributionLines) {
                                gap_lines.push_back(std::format(
                                    "ViewGapID #{} pass={} jobRva={:#x} set={} binding={} "
                                    "view={} image={} -> mirrorImage={} {}x{} type={} "
                                    "declaredLayout={}",
                                    n, doubled_pass_seq, doubled_current_job_rva, set_index,
                                    binding, reinterpret_cast<void*>(rec.image_view),
                                    reinterpret_cast<void*>(src_image),
                                    reinterpret_cast<void*>(mi->second),
                                    ext.width, ext.height, static_cast<int>(rec.type),
                                    static_cast<int>(rec.layout)));
                            }
                            // ---- STEP 6.7a: CLOSE THE GAP, don't just count it.
                            //
                            // Same safety gate as the attachment-view path
                            // below, for the same reason: substituting a mirror
                            // that has not been rendered into, or that is not in
                            // the layout this descriptor declares, is undefined
                            // and is what hung the GPU three times. Falling back
                            // costs parallax on one input; it cannot hang.
                            const VkImage mirror_image = mi->second;
                            const bool written =
                                mirror_images_written.count(mirror_image) != 0;
                            const VkImageLayout have = mirror_layout_of(mirror_image);
                            if (!have_view_info) {
                                // Rule 3: the one silent path left here, made
                                // loud. Without the create parameters we cannot
                                // build a matching view at all.
                                if (alias_view_no_info.fetch_add(
                                        1, std::memory_order_relaxed) < 10) {
                                    gap_lines.push_back(std::format(
                                        "AliasView NO-INFO pass={} view={} - no recorded "
                                        "vkCreateImageView parameters (created before our hook, "
                                        "or the try_lock was contended). Binding DOOM's view.",
                                        doubled_pass_seq,
                                        reinterpret_cast<void*>(rec.image_view)));
                                }
                            } else if (!written || have != rec.layout) {
                                alias_view_gated.fetch_add(1, std::memory_order_relaxed);
                                if (gate_lines.size() < 4) {
                                    gate_lines.push_back(std::format(
                                        "AliasView GATED pass={} jobRva={:#x} set={} binding={} "
                                        "mirrorImage={} written={} have={} want={}",
                                        doubled_pass_seq, doubled_current_job_rva, set_index,
                                        binding, reinterpret_cast<void*>(mirror_image),
                                        written, static_cast<int>(have),
                                        static_cast<int>(rec.layout)));
                                }
                            } else {
                                VkImageView alias = VK_NULL_HANDLE;
                                auto cached = alias_mirror_views.find(rec.image_view);
                                if (cached != alias_mirror_views.end() &&
                                    cached->second.mirror_image == mirror_image) {
                                    alias = cached->second.view;
                                } else {
                                    // Rebuild. Destroying the stale view here is
                                    // safe only because a generation change also
                                    // invalidates every descriptor clone that
                                    // referenced it (invalidate_read_redirect_
                                    // cache), so nothing recorded still points
                                    // at it.
                                    // real_*, NOT load_device_proc. Both
                                    // vkCreateImageView and vkDestroyImageView
                                    // are HOOKED, so a proc-address call here
                                    // re-enters our own hooks - and
                                    // hook_destroy_image_view takes
                                    // mirror_mutex, which this code is already
                                    // holding. That is a self-deadlock on a
                                    // non-recursive mutex: a silent freeze with
                                    // no log line and no exception, the exact
                                    // shape of run 17. It would also file our
                                    // private alias view into the maps that
                                    // describe DOOM's views.
                                    if (cached != alias_mirror_views.end()) {
                                        kharvox::native::fail("alias view mirror changed before safe retirement");
                                    }
                                    VkImageViewCreateInfo vci{
                                        VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                                    vci.flags = src_view_info.flags;
                                    vci.image = mirror_image;
                                    vci.viewType = src_view_info.view_type;
                                    vci.format = src_view_info.format;
                                    vci.components = src_view_info.components;
                                    vci.subresourceRange = src_view_info.range;
                                    VkImageView nv = VK_NULL_HANDLE;
                                    // real_create_image_view for the same two
                                    // reasons as the destroy above: the hook
                                    // takes framebuffer_depth_view_mutex
                                    // unconditionally beneath the two locks we
                                    // already hold (the third-lock hazard the
                                    // try_lock above exists to avoid), and it
                                    // would record our private view as though
                                    // DOOM had created it.
                                    if (real_create_image_view &&
                                        real_create_image_view(device, &vci, nullptr, &nv) ==
                                            VK_SUCCESS &&
                                        nv != VK_NULL_HANDLE) {
                                        alias_mirror_views[rec.image_view] =
                                            AliasMirrorView{nv, mirror_image, generation};
                                        alias = nv;
                                        const uint32_t c = alias_view_created.fetch_add(
                                            1, std::memory_order_relaxed);
                                        gap_lines.push_back(std::format(
                                            "AliasView CREATED #{} srcView={} -> mirrorView={} "
                                            "over mirrorImage={} format={} viewType={} mips={}+{} "
                                            "layers={}+{} gen={}",
                                            c, reinterpret_cast<void*>(rec.image_view),
                                            reinterpret_cast<void*>(nv),
                                            reinterpret_cast<void*>(mirror_image),
                                            static_cast<int>(src_view_info.format),
                                            static_cast<int>(src_view_info.view_type),
                                            src_view_info.range.baseMipLevel,
                                            src_view_info.range.levelCount,
                                            src_view_info.range.baseArrayLayer,
                                            src_view_info.range.layerCount, generation));
                                    } else {
                                        // The mirror is one mip, one layer, at
                                        // the framebuffer's extent. A sampling
                                        // view asking for more cannot be built
                                        // over it, and that must be visible.
                                        if (alias_view_failed.fetch_add(
                                                1, std::memory_order_relaxed) < 10) {
                                            gap_lines.push_back(std::format(
                                                "AliasView CREATE FAILED srcView={} "
                                                "mirrorImage={} format={} viewType={} mips={}+{} "
                                                "layers={}+{} - binding DOOM's unshifted view",
                                                reinterpret_cast<void*>(rec.image_view),
                                                reinterpret_cast<void*>(mirror_image),
                                                static_cast<int>(src_view_info.format),
                                                static_cast<int>(src_view_info.view_type),
                                                src_view_info.range.baseMipLevel,
                                                src_view_info.range.levelCount,
                                                src_view_info.range.baseArrayLayer,
                                                src_view_info.range.layerCount));
                                        }
                                    }
                                }
                                if (alias != VK_NULL_HANDLE) {
                                    use = alias;
                                    any_mirror = true;
                                    alias_view_substitutions.fetch_add(
                                        1, std::memory_order_relaxed);
                                }
                            }
                        }
                    }
                }
                if (auto m = mirrored_views.find(rec.image_view); m != mirrored_views.end()) {
                    ++mirror_backed;
                    if (mirror_backed_detail.size() < 400) {
                        mirror_backed_detail += std::format(
                            " [b{} view={} mirrorImage={} {}x{}]", binding,
                            reinterpret_cast<void*>(rec.image_view),
                            reinterpret_cast<void*>(m->second.image),
                            m->second.extent.width, m->second.extent.height);
                    }
                    // SAFETY GATE. Substitute only a mirror that has actually
                    // been rendered into by a mirrored pass AND that we track
                    // as being in the layout this descriptor declares.
                    // Anything else is either empty or in an unknown layout,
                    // and sampling it is undefined - the cause of both
                    // attempt-0 GPU hangs. Falling back to DOOM's own view
                    // costs parallax on this one input; it cannot hang.
                    const bool written = mirror_images_written.count(m->second.image) != 0;
                    const VkImageLayout have = mirror_layout_of(m->second.image);
                    // STORAGE IMAGES ARE OUTPUTS (2026-08-18, review P3).
                    // The "written" half of this gate exists because SAMPLING
                    // an empty mirror is undefined - it is what hung attempt 0
                    // twice. A STORAGE_IMAGE binding is the opposite case: the
                    // shader WRITES it, so "not yet written" is its normal
                    // first state and refusing on that ground would make the
                    // storage mirror permanently unreachable - built, shipped
                    // and never substituted once. The LAYOUT half still
                    // applies in full, and is what keeps this safe.
                    const bool is_storage = rec.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                    if ((written || is_storage) && have == rec.layout) {
                        use = descriptor_mirror_view(rec.image_view,m->second,device,generation);
                        any_mirror = true;
                        if (inside_final_pass_double) {
                            final_pass_redirects.fetch_add(1, std::memory_order_relaxed);
                            // WHICH binding, and to WHICH mirror image. The run
                            // that measured zero parallax also reported a
                            // non-zero substitution count, and those two cannot
                            // both be about the composite - so the identity of
                            // what was swapped is the missing fact, not the
                            // count of swaps.
                            if (final_pass_subst_lines.fetch_add(1, std::memory_order_relaxed) <
                                kMaxFinalPassBindingLines) {
                                log::warn(std::format(
                                    "FinalPassSubst binding={} type={} srcView={} -> "
                                    "mirrorView={} mirrorImage={} {}x{}",
                                    binding, static_cast<int>(rec.type),
                                    reinterpret_cast<void*>(rec.image_view),
                                    reinterpret_cast<void*>(m->second.view),
                                    reinterpret_cast<void*>(m->second.image),
                                    m->second.extent.width, m->second.extent.height));
                            }
                        }
                        if (is_storage) {
                            storage_mirror_substitutions.fetch_add(1, std::memory_order_relaxed);
                            // Once the doubled call's compute has written this
                            // mirror, the LATER passes that sample the same
                            // source view may be redirected to it too - which
                            // is the entire point, since those are the passes
                            // that consume the effects. Marked at bind time
                            // rather than after the dispatch executes: within
                            // one command buffer the sampling passes are
                            // recorded after this bind, so the ordering the
                            // mark asserts is guaranteed by submission order.
                            mirror_images_written.insert(m->second.image);
                            if (inside_frame_root_double)
                                h2_doubled_written_this_frame.insert(m->second.image);
                            // R1: the verdict that decides where the compute
                            // write lands. Published below, outside the locks.
                            df_pending.push_back(DfPending{src, binding, kDfSubstituted,
                                                           have, rec.layout, m->second.image});
                        }
                    } else {
                        if (is_storage) {
                            storage_mirror_layout_refusals.fetch_add(1, std::memory_order_relaxed);
                            df_pending.push_back(DfPending{src, binding, kDfRefusedLayout,
                                                           have, rec.layout, m->second.image});
                        }
                        ++refused;
                        read_redirect_gate_refusals.fetch_add(1, std::memory_order_relaxed);
                        if (refused < 4) {
                            refused_examples += std::format(
                                " [b{} written={} have={} want={}]", binding, written,
                                static_cast<int>(have), static_cast<int>(rec.layout));
                        }
                        // IDENTITY for the safety gate (STEP 6.6 item 1). The
                        // gate's own comment says what a refusal costs -
                        // "parallax on this one input" - and 46 of them per
                        // run was dismissed by dividing by 21,542 binds. Same
                        // wrong denominator as the gap. The reason field
                        // separates the two possible repairs: not-written
                        // means the write-marking is starving the gate,
                        // layout means the layout tracking is.
                        const uint32_t n =
                            attribution_gate_lines.fetch_add(1, std::memory_order_relaxed);
                        if (n < kMaxAttributionLines) {
                            gate_lines.push_back(std::format(
                                "GateFallbackID #{} pass={} jobRva={:#x} set={} binding={} "
                                "view={} mirrorImage={} {}x{} type={} reason={} have={} want={}",
                                n, doubled_pass_seq, doubled_current_job_rva, set_index, binding,
                                reinterpret_cast<void*>(rec.image_view),
                                reinterpret_cast<void*>(m->second.image),
                                m->second.extent.width, m->second.extent.height,
                                static_cast<int>(rec.type),
                                // The layout half is checked FIRST here, and
                                // must be: run B printed "not-written" for
                                // refusals that were actually layout
                                // mismatches (have=0 want=1 on a storage
                                // image, where not-written is not even a
                                // refusal reason any more), and that sent the
                                // investigation at the wrong half of the gate.
                                have != rec.layout
                                    ? "layout-mismatch"
                                    : "not-written",
                                static_cast<int>(have), static_cast<int>(rec.layout)));
                        }
                    }
                } else if (rec.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
                    // R1: a storage OUTPUT for which mirrored_views holds
                    // nothing. `use` may still have been redirected by the
                    // alias path above; any_mirror records that, and if it did
                    // not fire this dispatch writes straight into DOOM's own
                    // image - the contamination path StorageImageID names.
                    df_pending.push_back(DfPending{src, binding,
                                                   use != rec.image_view ? kDfSubstituted
                                                                         : kDfNoMirror,
                                                   VK_IMAGE_LAYOUT_UNDEFINED, rec.layout,
                                                   VK_NULL_HANDLE});
                }
                // THE FROXEL FIX - see kFixDoubledLightingVolume. Applied
                // LAST, after every mirror/alias decision above, because it is
                // about a resource DOOM bound wrongly rather than about which
                // chain a correct binding should resolve to. Scoped by RVA and
                // by binding index, and refuses unless the real call of THIS
                // frame actually bound a volume there.
                if (kFixDoubledLightingVolume &&
                    doubled_current_job_rva == kP1ParticleSimJobRva &&
                    binding == kLightingVolumeBinding) {
                    if (real_lighting_volume_view != VK_NULL_HANDLE &&
                        use != real_lighting_volume_view) {
                        use = real_lighting_volume_view;
                        lighting_volume_substitutions.fetch_add(1, std::memory_order_relaxed);
                    } else if (real_lighting_volume_view == VK_NULL_HANDLE) {
                        // The real call bound no volume we could see this
                        // frame. Counted, never guessed: substituting a stale
                        // handle from an earlier frame is how a fix turns into
                        // a corruption.
                        lighting_volume_missing.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        // ALREADY CORRECT - the doubled call is binding the
                        // same volume view the real call did, so there is
                        // nothing to substitute.
                        //
                        // THIS CASE USED TO LOG NOTHING (fixed 2026-08-29), and
                        // that silence cost a run: the fix was armed, produced
                        // no substitutions and no line, and "no change" was
                        // indistinguishable from "the fix did nothing" when the
                        // truth was "there was nothing to fix". A no-op must
                        // say it is a no-op, or it reads as a refutation.
                        const uint32_t n = lighting_volume_already_ok.fetch_add(
                            1, std::memory_order_relaxed);
                        if (n < 3 || (n % 2000) == 0) {
                            log::warn(std::format(
                                "FroxelFix NO-OP #{}: binding {} ALREADY holds the real call's "
                                "volume view {} - the divergence this fix exists to repair is "
                                "NOT PRESENT. Arming it changes nothing by construction, so a "
                                "null result here says nothing about the froxel hypothesis. "
                                "P1's binding-2 divergence was measured on the corrupted "
                                "pre-2026-08-28 baseline and does not reproduce.",
                                n, binding,
                                reinterpret_cast<void*>(real_lighting_volume_view)));
                        }
                    }
                }
                // CLONE ARRAY TRUNCATION - MEASURE FIRST (2026-08-29).
                //
                // The clone below writes descriptorCount=1 per binding with
                // dstArrayElement=0, so for any binding DOOM declared as an
                // ARRAY, elements 1..N-1 of the clone are NEVER WRITTEN and are
                // undefined when the shader samples them. ImageDescriptorBinding's
                // own comment flags this as review risk 1, the "bindless-shaped
                // fallback... which would need per-resource classification
                // rather than a whole-set clone", and nothing has ever sized it
                // on the redirected sets specifically.
                //
                // It would fit every observation left standing: each recorded
                // input is correct because we record and report ELEMENT 0; the
                // froxel volume is byte-identical because it IS element 0; and
                // whatever the array elements contribute is simply absent -
                // zero rather than dim, with element-0 lighting intact.
                //
                // Counted, not assumed, and log-only: no descriptor is written
                // differently by this. maxArray=1 closes the whole idea for the
                // cost of one line.
                if (rec.count > 1) {
                    kharvox::native::fail("descriptor arrays require complete element capture");
                    clone_array_bindings.fetch_add(1, std::memory_order_relaxed);
                    clone_elements_dropped.fetch_add(rec.count - 1, std::memory_order_relaxed);
                    uint32_t prev = clone_max_array.load(std::memory_order_relaxed);
                    while (rec.count > prev &&
                           !clone_max_array.compare_exchange_weak(prev, rec.count,
                                                                  std::memory_order_relaxed)) {}
                    if (clone_array_lines.fetch_add(1, std::memory_order_relaxed) < 8) {
                        log::warn(std::format(
                            "CloneArray: binding {} is an ARRAY of {} (type={}), but the clone "
                            "writes only element 0 - elements 1..{} are UNDEFINED when the "
                            "doubled call samples them. This is review risk 1, measured at last.",
                            binding, rec.count, static_cast<int>(rec.type), rec.count - 1));
                    }
                }
                // VOLUME SUBSTITUTION CENSUS (2026-08-22). The reconciliation.
                //
                // P1 says the doubled call binds 0 of the 2 froxel volumes the
                // real call binds, with noRecord=0 on both sides. CloneDrop
                // says no binding is ever dropped. Both are sound, and they
                // reconcile only one way: the clone does not LOSE the volume
                // bindings, it SUBSTITUTES them - writing a mirror view in
                // place of DOOM's 3D volume, which then classifies as MIR and
                // leaves VOL at zero.
                //
                // If true it is a straightforward bug with a straightforward
                // fix. The froxel volumes are shared by design - they report
                // mirrorHeld=false imageMirrored=false and both calls are
                // supposed to read DOOM's own. Swapping a VK_IMAGE_VIEW_TYPE_3D
                // binding for a 2D mirror view is not a redirect, it is a type
                // mismatch: the sampler reads something that is not the volume
                // and the scattering comes out as nothing. No volumetrics, and
                // analytic lighting on the genuinely-mirrored 2D reads carries
                // on untouched - which is the split in the pictures.
                //
                // Named, not inferred: this logs the binding, both view
                // handles, and the view TYPE of each, so "3D replaced by 2D"
                // is visible rather than argued.
                if (use != rec.image_view &&
                    volume_subst_lines.load(std::memory_order_relaxed) < kMaxVolumeSubstLines) {
                    VkImageViewType asked_type = VK_IMAGE_VIEW_TYPE_2D;
                    VkImageViewType used_type = VK_IMAGE_VIEW_TYPE_2D;
                    bool asked_known = false, used_known = false;
                    if (auto a = image_view_infos.find(rec.image_view);
                        a != image_view_infos.end()) { asked_type = a->second.view_type; asked_known = true; }
                    if (auto u = image_view_infos.find(use);
                        u != image_view_infos.end()) { used_type = u->second.view_type; used_known = true; }
                    if (asked_known && asked_type == VK_IMAGE_VIEW_TYPE_3D) {
                        volume_subst_lines.fetch_add(1, std::memory_order_relaxed);
                        volume_substitutions_seen.fetch_add(1, std::memory_order_relaxed);
                        log::error(std::format(
                            "VolumeSubst: binding {} asked for a 3D VOLUME view {} but the clone "
                            "writes {} (viewType={}{}). A 3D sampler bound to a non-3D view does "
                            "not read the volume - this is the froxel scattering eye 1 lacks.",
                            binding, reinterpret_cast<void*>(rec.image_view),
                            reinterpret_cast<void*>(use), static_cast<int>(used_type),
                            used_known ? "" : ", NOT IN image_view_infos - one of our own "
                                              "mirror/alias views, which bypass that hook"));
                    }
                }
                // BIND-TIME VOLUME REDIRECT - the write. Applied last, after
                // every other resolution, so it cannot be undone by them.
                //
                // Only where the real call of THIS frame bound a 3D volume and
                // this set holds something that is not one. The per-frame
                // record is why the temporal pair cannot be crossed: it is
                // cleared at frame-root entry and refilled from the real call's
                // own binds, so the volume written here is the one the real
                // call used in this very frame.
                // ⚠ SCOPED BY JOB RVA AS WELL AS BINDING. The first version
                // keyed on binding index alone and blacked the screen: it wrote
                // volumes into binding 2 of every set holding a non-3D view
                // there, and a 3D view in a slot whose layout declares a 2D
                // sampler is an invalid descriptor. Requiring the SAME JOB the
                // real call bound that volume at turns an index collision into
                // an identity.
                const VolumeJobRecord* vslot =
                    volume_restore_enabled.load(std::memory_order_relaxed)
                        ? volume_job_slot(doubled_current_job_rva, false) : nullptr;
                if (vslot && binding < kVolumeRestoreBindings &&
                    vslot->view[binding] != VK_NULL_HANDLE &&
                    use != vslot->view[binding]) {
                    const auto vi = image_view_infos.find(use);
                    const bool use_is_3d = vi != image_view_infos.end() &&
                                           vi->second.view_type == VK_IMAGE_VIEW_TYPE_3D;
                    const uint32_t n_frame =
                        volume_redirects_this_frame.load(std::memory_order_relaxed);
                    if (!use_is_3d && n_frame >= kMaxVolumeRedirectsPerFrame) {
                        volume_restore_enabled.store(false, std::memory_order_relaxed);
                        log::error(std::format(
                            "VolumeRedirect SELF-DISARMED: {} substitutions in one frame exceeds "
                            "the {} cap. With the per-(job,binding) record this should be the "
                            "number of DISTINCT sets at the job, so hitting it again means the "
                            "clone cache is still thrashing rather than that the scoping is too "
                            "broad. Turned OFF in code; the picture returns without a keypress.",
                            n_frame, kMaxVolumeRedirectsPerFrame));
                    } else if (!use_is_3d) {
                        use = vslot->view[binding];
                        volume_redirects_this_frame.fetch_add(1, std::memory_order_relaxed);
                        volume_restores.fetch_add(1, std::memory_order_relaxed);
                        if (volume_restore_lines.fetch_add(1, std::memory_order_relaxed) < 8)
                            log::warn(std::format(
                                "VolumeRedirect: set {} binding {} at job {:#x} held a non-3D "
                                "view; the clone now binds the real call's volume {}.",
                                reinterpret_cast<void*>(src), binding, doubled_current_job_rva,
                                reinterpret_cast<void*>(use)));
                    }
                }
                if(rec.type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE&&use==rec.image_view)kharvox::native::fail(std::format("unmirrored storage image binding: set={} b={} view={} wantLayout={} pass={} job={:#x}",reinterpret_cast<void*>(src),binding,reinterpret_cast<void*>(rec.image_view),int(rec.layout),doubled_pass_seq,doubled_current_job_rva).c_str());
                if(use==rec.image_view&&mirrors_converged()){
                    bool knownMirror=mirrored_views.count(rec.image_view)!=0;
                    auto sourceImage=image_view_to_image.find(rec.image_view);
                    if(sourceImage!=image_view_to_image.end())knownMirror|=mirrored_images.count(sourceImage->second)!=0;
                    if(knownMirror)kharvox::native::fail(std::format("ready mirror descriptor failed layout/initialization gate: set={} b={} view={} want={} pass={} final={}",reinterpret_cast<void*>(src),binding,reinterpret_cast<void*>(rec.image_view),int(rec.layout),doubled_pass_seq,inside_final_pass_double).c_str());
                }
                images.push_back({binding, rec.type, use, rec.sampler, rec.layout});
            }
        }
        if (!unclonable && bufs != descriptor_sets.end()) {
            for (const auto& [binding, d] : bufs->second) {
                // A binding index present in BOTH maps would make us emit two
                // writes for it, one of them with a type the layout does not
                // declare - invalid usage, and a plausible second cause of a
                // GPU hang. Records are never pruned, so a recycled set
                // handle carrying stale entries is exactly how this happens.
                // Refuse the whole set rather than write something invalid.
                bool collides = false;
                for (const auto& im : images) if (im.binding == binding) { collides = true; break; }
                if (collides) {
                    unclonable = true;
                    reason = std::format("binding {} appears as BOTH an image and a buffer in our "
                                         "records (stale entry from a recycled set handle?)",
                                         binding);
                    break;
                }
                const VkBuffer eyeBuffer=kharvox::native::bufferForEye(d.buffer);
                if((d.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER||d.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)&&eyeBuffer==d.buffer)kharvox::native::fail("unseeded writable storage buffer");
                any_mirror |= eyeBuffer!=d.buffer;
                buffers.push_back({binding, d.type, eyeBuffer, d.offset, d.range});
            }
        }
    }
    // Locks released. Emit the attribution here, ahead of EVERY return below,
    // so a set that is refused for some other reason still names the gaps and
    // fallbacks it contained.
    for (const std::string& line : gap_lines) log::warn(line);
    for (const std::string& line : gate_lines) log::warn(line);
    // R1 DISPATCH FATE, published here for the same reason the lines above
    // are: ahead of every return, so a set refused further down still leaves
    // its per-binding storage verdicts behind. The SET-level verdict is
    // written at each return so "the whole set was refused" is distinguishable
    // from "this binding was refused".
    for (const DfPending& p : df_pending)
        df_record_storage_outcome(p.set, p.binding, p.outcome, p.have, p.want, p.mirror);
    if (unclonable) {
        kharvox::native::fail(std::format("descriptor cannot be cloned completely: set={} setIndex={} pass={} jobRva={:#x}: {}",
            reinterpret_cast<void*>(src), set_index, doubled_pass_seq, doubled_current_job_rva, reason+"; layout="+kharvox::native::describeSetLayout(src)).c_str());
        const uint32_t f = read_redirect_failures.fetch_add(1, std::memory_order_relaxed);
        // Classified UNCAPPED, unlike the line below. See the counters'
        // declaration for why inferring this from the log was not possible.
        if (reason.rfind("no recorded", 0) == 0) {
            rr_reason_no_records.fetch_add(1, std::memory_order_relaxed);
        } else if (reason.find("is an array of") != std::string::npos) {
            rr_reason_array_binding.fetch_add(1, std::memory_order_relaxed);
        } else if (reason.find("BOTH an image and a buffer") != std::string::npos) {
            rr_reason_type_conflict.fetch_add(1, std::memory_order_relaxed);
        } else {
            rr_reason_other.fetch_add(1, std::memory_order_relaxed);
        }
        if (f < 10) {
            log::error(std::format(
                "ReadRedirect FALLBACK: set {} bound UNMODIFIED - {}. This pass will read "
                "DOOM's unshifted images for that binding.",
                reinterpret_cast<void*>(src), reason));
        }
        // STEP 6.6 identity. Cloning is all-or-nothing, so this refusal costs
        // EVERY mirror-backed read in the set, and mirrorBacked/extents say
        // whether that is one full-screen input or nothing at all.
        //
        // ⚠ WHEN reason IS "no recorded contents", mirrorBacked=0 IS NOT A
        // MEASUREMENT. There is nothing to walk, so the zero is guaranteed by
        // construction and says nothing about what the set carries. allocFrame
        // and updateFrame are what make that case answerable: updateFrame <
        // allocFrame (or none) means DOOM bound a handle it had not written
        // since 6.7d cleared it, and the clear is reaching too far.
        uint64_t alloc_frame = 0, update_frame = 0;
        bool have_alloc = false, have_update = false;
        {
            std::scoped_lock lock(read_redirect_mutex);
            if (auto a = descriptor_set_alloc_frame.find(src);
                a != descriptor_set_alloc_frame.end()) { alloc_frame = a->second; have_alloc = true; }
            if (auto u = descriptor_set_update_frame.find(src);
                u != descriptor_set_update_frame.end()) { update_frame = u->second; have_update = true; }
        }
        // Publish the handle to the write-shape tap BEFORE deciding whether
        // this line prints: the tap must arm on every refusal, not only on the
        // ones that fit inside a logging budget.
        note_refused_set(src);
        // TWO budgets, and both are printed on the line. `f` is the shared
        // attribution budget (kMaxAttributionLines across every site) which
        // the 21:09 run exhausted at #185, BEFORE the collapse. The second is
        // re-armed at each cutscene edge / UP marker, so the collapse window
        // prints fresh lines. A collapse window with ringBudget still at its
        // full re-armed value AND no lines is a broken re-arm, not a null.
        bool print_line = f < kMaxAttributionLines;
        bool from_rearm = false;
        if (!print_line) {
            uint32_t b = refusal_ring_budget.load(std::memory_order_relaxed);
            while (b > 0 && !refusal_ring_budget.compare_exchange_weak(
                                b, b - 1, std::memory_order_relaxed)) {}
            if (b > 0) { print_line = true; from_rearm = true; }
        }
        if (print_line) {
            refusal_ring_lines.fetch_add(1, std::memory_order_relaxed);
            log::warn(std::format(
                "SetRefusedID #{} pass={} jobRva={:#x} set={} setIndex={} mirrorBacked={} "
                "allocFrame={} updateFrame={} boundWithoutUpdateSinceAlloc={} reason={}{} "
                "| ring={} budget={} attrib={}/{} ringBudgetLeft={} rearms={}",
                f, doubled_pass_seq, doubled_current_job_rva,
                reinterpret_cast<void*>(src), set_index, mirror_backed,
                have_alloc ? std::to_string(alloc_frame) : "never-seen",
                have_update ? std::to_string(update_frame) : "never-seen",
                (have_alloc && (!have_update || update_frame < alloc_frame)),
                reason, mirror_backed_detail,
                have_ring ? desc_ring_format(ring_snapshot) : "NO-RING-ENTRY",
                from_rearm ? "cutscene-rearm" : "attribution",
                f, kMaxAttributionLines,
                refusal_ring_budget.load(std::memory_order_relaxed),
                refusal_ring_rearms.load(std::memory_order_relaxed)));
        }
        df_record_set_outcome(src, kDfSetRefused);
        std::scoped_lock lock(read_redirect_mutex);
        sets_needing_no_redirect[src] = generation;
        return VK_NULL_HANDLE;
    }
    if (!any_mirror) {
        df_record_set_outcome(src, kDfSetNoRedirect);
        std::scoped_lock lock(read_redirect_mutex);
        sets_needing_no_redirect[src] = generation;
        return VK_NULL_HANDLE;
    }
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    {
        std::scoped_lock lock(read_redirect_mutex);
        if (auto it = descriptor_set_layouts.find(src); it != descriptor_set_layouts.end())
            set_layout = it->second;
    }
    if (set_layout == VK_NULL_HANDLE) {
        kharvox::native::fail("native descriptor layout missing");
        const uint32_t f = read_redirect_failures.fetch_add(1, std::memory_order_relaxed);
        if (f < 10) {
            log::error(std::format(
                "ReadRedirect FALLBACK: set {} has mirror-backed reads but NO recorded layout "
                "(allocated before our vkAllocateDescriptorSets hook installed?) - cannot clone, "
                "binding it unmodified.", reinterpret_cast<void*>(src)));
        }
        if (f < kMaxAttributionLines) {
            log::warn(std::format(
                "SetRefusedID #{} pass={} jobRva={:#x} set={} setIndex={} mirrorBacked={} "
                "reason=no-recorded-layout{}",
                f, doubled_pass_seq, doubled_current_job_rva,
                reinterpret_cast<void*>(src), set_index, mirror_backed, mirror_backed_detail));
        }
        df_record_set_outcome(src, kDfSetRefused);
        std::scoped_lock lock(read_redirect_mutex);
        sets_needing_no_redirect[src] = generation;
        return VK_NULL_HANDLE;
    }
    kharvox::native::validateLayout(set_layout);
    VkDescriptorSet clone = VK_NULL_HANDLE;
    VkResult cloneAllocationResult{};
    if(kharvox::native::installed())cloneAllocationResult=kharvox::native::allocateRedirectedDescriptor(device,set_layout,&clone);
    else {
        if (!ensure_read_redirect_pool(device)) {kharvox::native::fail("native descriptor pool unavailable");return VK_NULL_HANDLE;}
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool=read_redirect_pool;alloc.descriptorSetCount=1;alloc.pSetLayouts=&set_layout;
        cloneAllocationResult=real_allocate_descriptor_sets(device,&alloc,&clone);
    }
    if (cloneAllocationResult != VK_SUCCESS ||
        clone == VK_NULL_HANDLE) {
        {std::scoped_lock lock(read_redirect_mutex);log::error(std::format("Native clone allocation failure result={} liveClones={} retiredClones={} layout={}",int(cloneAllocationResult),redirected_sets.size(),retired_clones.size(),reinterpret_cast<void*>(set_layout)));}
        kharvox::native::fail("native descriptor clone allocation failed");
        // THE FALSIFIER, counted separately and UNCAPPED (2026-08-24). This
        // site is where both collapses actually happened, and until now it fed
        // only the shared read_redirect_failures - so 271 of the 21:09 run's
        // 274 collapse-window refusals had no attributable reason and the
        // failure was diagnosed as "no recorded contents", which it was not.
        const uint32_t pf = rr_clone_alloc_failures.fetch_add(1, std::memory_order_relaxed);
        read_redirect_failures.fetch_add(1, std::memory_order_relaxed);
        // Its own budget, NOT the shared 10-line one that was spent minutes
        // before the collapse in both runs. First 5, then every 500th, so a
        // sustained exhaustion stays visible without flooding.
        if (pf < 5 || (pf % 500) == 0) {
            log::error(std::format(
                "ReadRedirect CLONE ALLOC FAILED #{} for set {} - the read-redirect pool "
                "(maxSets=1024) is exhausted. This set is now marked no-redirect for "
                "generation {}, and with the generation static that is PERMANENT: eye 1 stops "
                "being redirected and both eyes show the same picture. If this line appears at "
                "all, the clone-retirement fix did NOT hold.",
                pf, reinterpret_cast<void*>(src), generation));
        }
        std::scoped_lock lock(read_redirect_mutex);
        sets_needing_no_redirect[src] = generation;
        return VK_NULL_HANDLE;
    }
    // CLONE COMPLETENESS CENSUS (2026-08-22). The clone is dropping bindings.
    //
    // P1 caught it at pass 8, both phases side by side:
    //   REAL    askedJ  REAL=[2 views]  VOL=[2 volumes]
    //   DOUBLED askedJ  REAL=[2 views]  VOL=[2 volumes]   <- same ask
    //   DOUBLED boundJ  MIR=[2 views]   REAL=[] VOL=[]    <- volumes gone
    //
    // Four asked bindings, two written. The froxel volume slots are left
    // UNWRITTEN in the clone, so the shader samples undefined descriptors where
    // the scattering volumes should be - no volumetrics, while the mirrored 2D
    // reads carry analytic lighting through untouched. That is the deficit
    // exactly, and the working arm reads rd3D=2 on both sides.
    //
    // This names WHICH bindings are lost and why, rather than inferring it.
    // Counted at the point the writes are assembled, so it measures what is
    // actually sent to vkUpdateDescriptorSets, not what we intended to send.
    if (clone_completeness_lines.load(std::memory_order_relaxed) < kMaxCloneCompletenessLines) {
        const size_t asked = clone_asked.size();
        const size_t wrote = images.size();
        if (asked != wrote) {
            kharvox::native::fail("incomplete descriptor clone image writes");
            clone_completeness_lines.fetch_add(1, std::memory_order_relaxed);
            clone_bindings_dropped.fetch_add(static_cast<uint32_t>(asked - wrote),
                                             std::memory_order_relaxed);
            std::string missing;
            for (const auto& a : clone_asked) {
                bool found = false;
                for (const auto& im : images) if (im.binding == a.binding) { found = true; break; }
                if (!found)
                    missing += std::format(" binding{}(type={} count={} view={})", a.binding,
                                           a.type, a.count, reinterpret_cast<void*>(a.view));
            }
            log::error(std::format(
                "CloneDrop: source set {} has {} image bindings, the clone writes {}. MISSING:{} "
                "- these slots are UNWRITTEN in the clone and undefined when the doubled call "
                "samples them.",
                reinterpret_cast<void*>(src), asked, wrote,
                missing.empty() ? std::string(" (none identified)") : missing));
        }
    }
    std::vector<VkDescriptorImageInfo> image_infos(images.size());
    std::vector<VkDescriptorBufferInfo> buffer_infos(buffers.size());
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(images.size() + buffers.size());
    for (size_t i = 0; i < images.size(); ++i) {
        // DOOM's own declared layout, not a guess - see ImageDescriptorBinding::layout.
        image_infos[i] = {images[i].sampler, images[i].view, images[i].layout};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = clone; w.dstBinding = images[i].binding; w.descriptorCount = 1;
        w.descriptorType = images[i].type; w.pImageInfo = &image_infos[i];
        writes.push_back(w);
    }
    for (size_t i = 0; i < buffers.size(); ++i) {
        buffer_infos[i] = {buffers[i].buffer, buffers[i].offset, buffers[i].range};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = clone; w.dstBinding = buffers[i].binding; w.descriptorCount = 1;
        w.descriptorType = buffers[i].type; w.pBufferInfo = &buffer_infos[i];
        writes.push_back(w);
    }
    // real_ rather than the hook: this is OUR set, and letting our own
    // tracking record it would put clone handles in the maps that describe
    // DOOM's sets.
    real_update_descriptor_sets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    {
        std::scoped_lock lock(read_redirect_mutex);
        redirected_sets[src] = RedirectedSet{
            clone, generation,
            volume_restore_enabled.load(std::memory_order_relaxed)
                ? (volume_job_slot(doubled_current_job_rva, false)
                      ? volume_job_slot(doubled_current_job_rva, false)
                            ->view[kVolumeRestorePrimaryBinding]
                      : VK_NULL_HANDLE)
                : real_lighting_volume_view};
    }
    df_record_set_outcome(src, kDfSubstituted);
    // P1: the clone's EFFECTIVE image views, recorded here because nothing
    // else can see them. `images[i].view` is post-substitution by
    // construction - it is the handle actually written into the clone - so
    // this is what the GPU reads, which is the same thing a capture records.
    // Kept unconditionally rather than behind p1_active: clones are cached per
    // (set, mirrorGeneration) and are usually built BEFORE the armed frame, so
    // gating on the arm would leave the BOUND side empty for exactly the sets
    // that matter. Bounded by the clone population, one small vector each.
    {
        std::vector<VkImageView> effective;
        effective.reserve(images.size());
        for (const auto& im : images) effective.push_back(im.view);
        std::scoped_lock lock(p1_clone_mutex);
        p1_clone_views[clone] = std::move(effective);
    }
    // R2: the same handles, but keyed BY BINDING and BY SOURCE SET. P1's copy
    // is positional and indexed by clone, which a dispatch cannot use: it has
    // the source set, and it needs to ask about binding 8 specifically.
    {
        std::vector<std::pair<uint32_t, VkImageView>> by_binding;
        by_binding.reserve(images.size());
        for (const auto& im : images) by_binding.emplace_back(im.binding, im.view);
        df_record_clone_views(src, clone, generation, std::move(by_binding));
    }
    const uint32_t n = read_redirect_clones.fetch_add(1, std::memory_order_relaxed);
    if (n < 40) {
        log::warn(std::format(
            "ReadRedirect CLONE #{}: set {} -> {} ({} image bindings, {} buffer bindings, "
            "layout {}, gen {}, refusedMirrors={}{})",
            n, reinterpret_cast<void*>(src), reinterpret_cast<void*>(clone),
            images.size(), buffers.size(), reinterpret_cast<void*>(set_layout),
            generation, refused, refused_examples));
    }
    return clone;
}

// COMPUTE PROBE (2026-08-17, STEP 6 RESULT follow-up).
//
// Testing the hypothesis that survived run 10: framebuffer mirroring can
// only mirror images written as framebuffer ATTACHMENTS, so anything DOOM's
// lighting/post chain writes from a COMPUTE shader through a storage image
// has no framebuffer, is never mirrored, and is therefore written by the
// doubled call straight into DOOM'S OWN image. If that is happening it
// explains run 10 exactly - eye 1's composite is rebuilt from DOOM's real,
// unshifted compute output, so no substitution of SAMPLED views could ever
// restore its parallax - and it is simultaneously a contamination path into
// eye 0, since the doubled call would be overwriting DOOM's real
// intermediates every frame.
//
// Log-only. Rides on the DescriptorCensus arm (SHIFT+PageUp), which already
// self-disarms after exactly one complete doubled call.
//
// Classification is the same three-way tag the census uses:
//   mirror     - we hold a mirror for this storage image
//   unknown    - we do NOT, so the doubled call writes DOOM's real image
void probe_dispatch_storage_images(const char* kind) {
    // POST-FRAME-ROOT COVERAGE PROBE. A final grade is as likely to be a
    // compute shader as a render pass, and if it were, a probe that watched
    // only render passes and transfers would report "nothing writes the
    // swapchain" - a confident, wrong answer. Counted here so the window's
    // description is complete rather than partial.
    if (post_frame_root_window && !inside_frame_root_double && !inside_real_frame_root &&
        post_frame_root_probe_armed.load(std::memory_order_relaxed) &&
        post_frame_root_lines.load(std::memory_order_relaxed) < kMaxPostFrameRootLines) {
        const uint32_t n = post_frame_root_lines.fetch_add(1, std::memory_order_relaxed);
        log::warn(std::format("PostFrameRoot #{} {} (compute)", n, kind));
    }
    if (!inside_frame_root_double) return;
    // STORAGE-BUFFER PROBE (2026-08-18). The owner's decisive clue: sparks in
    // eye 0 - DOOM's OWN frame - are smaller, cover less area, and no longer
    // bounce off the weapon once doubling is on. That is a stateful particle
    // SIMULATION being corrupted, and simulation state lives in a storage
    // BUFFER, not an image. We mirror images and framebuffers; we have never
    // mirrored a buffer, so any buffer the doubled call's compute writes is
    // DOOM's real one, advanced a second time per frame with a shifted camera
    // (wrong depth -> no weapon bounce) and left corrupted for the next
    // frame's real call (cumulative shrink). This enumerates the STORAGE
    // BUFFERS the doubled call's compute binds - the thing the image-only
    // ComputeProbe never looked at. Runs on its own arm so it does not need
    // the census.
    if (storage_buffer_probe_armed.load(std::memory_order_relaxed) &&
        storage_buffer_probe_lines.load(std::memory_order_relaxed) <
            kMaxStorageBufferProbeLines) {
        struct BufId { VkDescriptorSet set; uint32_t binding; VkBuffer buffer; VkDeviceSize range; };
        BufId bufs[12];
        uint32_t bc = 0;
        {
            std::unique_lock lock(swapchains_mutex);
            for (VkDescriptorSet set : last_bound_compute_sets) {
                auto it = descriptor_sets.find(set);
                if (it == descriptor_sets.end()) continue;
                for (const auto& [binding, d] : it->second) {
                    if (d.type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
                        d.type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) continue;
                    if (bc >= std::size(bufs)) break;
                    bufs[bc++] = BufId{set, binding, d.buffer, d.range};
                }
            }
        }
        for (uint32_t i = 0; i < bc; ++i) {
            // DEDUPE BY BUFFER HANDLE. The first run spent its entire 40-line
            // budget on pass 16 repeating across three descriptor sets, so the
            // inventory was truncated before any later pass - and a later pass
            // is exactly where a particle simulation would live. One line per
            // distinct buffer makes the budget cover the whole frame.
            {
                std::scoped_lock lk(storage_buffer_seen_mutex);
                if (!storage_buffer_seen.insert(bufs[i].buffer).second) continue;
            }
            if (storage_buffer_probe_lines.fetch_add(1, std::memory_order_relaxed) >=
                kMaxStorageBufferProbeLines) break;
            // Usage flags decide whether mirroring this buffer is even safe.
            // A buffer that is ALSO an indirect-args or vertex/index source is
            // read through vkCmdDrawIndirect / vkCmdBindVertexBuffers, which
            // name it by HANDLE and bypass descriptor substitution entirely -
            // so substituting only the descriptor would have the doubled call
            // writing our mirror while still reading DOOM's real args. That
            // desync is the hazard to know about BEFORE building the mirror,
            // not after.
            VkDeviceSize size = 0;
            VkBufferUsageFlags usage = 0;
            {
                std::unique_lock lock(swapchains_mutex);
                if (auto b = buffers.find(bufs[i].buffer); b != buffers.end()) {
                    size = b->second.size;
                    usage = b->second.usage;
                }
            }
            const bool indirect = (usage & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT) != 0;
            const bool vtx_idx = (usage & (VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                           VK_BUFFER_USAGE_INDEX_BUFFER_BIT)) != 0;
            log::warn(std::format(
                "StorageBufferProbe pass={} binding={} buffer={} range={} size={} usage={:#x} "
                "indirect={} vertexOrIndex={} - the doubled call's compute WRITES this storage "
                "buffer and we hold no mirror, so it is DOOM's real state, advanced twice per "
                "frame. indirect/vertexOrIndex=true means it is ALSO read by handle outside "
                "descriptors, so mirroring the descriptor alone would desync it.",
                doubled_pass_seq, bufs[i].binding, reinterpret_cast<void*>(bufs[i].buffer),
                static_cast<unsigned long long>(bufs[i].range),
                static_cast<unsigned long long>(size), static_cast<unsigned>(usage),
                indirect, vtx_idx));
        }
    }
    if (!descriptor_census_enabled.load(std::memory_order_relaxed)) return;
    doubled_dispatch_count.fetch_add(1, std::memory_order_relaxed);
    std::string detail;
    uint32_t mirrored = 0, unmirrored = 0;
    // Review P1: the unmirrored views, carried OUT of the locked block so
    // their identity can be resolved without taking a third lock underneath
    // mirror_mutex and swapchains_mutex. Run 17 wedged the render thread
    // doing exactly that; Rule 9 exists because of it.
    struct PendingId { VkDescriptorSet set; uint32_t binding; VkImageView view; };
    PendingId unmirrored_ids[8]{};
    uint32_t unmirrored_id_count = 0;
    {
        std::scoped_lock mirror_lock(mirror_mutex);
        std::unique_lock lock(swapchains_mutex);
        for (VkDescriptorSet set : last_bound_compute_sets) {
            const auto imgs = descriptor_image_sets.find(set);
            if (imgs == descriptor_image_sets.end()) continue;
            for (const auto& [binding, rec] : imgs->second) {
                if (rec.type != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) continue;
                const char* hit = census_classify_view(rec.image_view);
                if (std::strcmp(hit, "mirror") == 0) {
                    ++mirrored;
                } else {
                    ++unmirrored;
                    if (unmirrored_id_count < std::size(unmirrored_ids))
                        unmirrored_ids[unmirrored_id_count++] =
                            PendingId{set, binding, rec.image_view};
                }
                if (detail.size() < 300) {
                    detail += std::format(" [set{} b{} view={} hit={}]",
                        reinterpret_cast<void*>(set), binding,
                        reinterpret_cast<void*>(rec.image_view), hit);
                }
            }
        }
    }
    // IDENTITY (2026-08-18, review P1). What the doubled call is scribbling
    // into DOOM's own intermediates, named rather than merely counted: the
    // backing VkImage, its extent, format and usage. P1 could not answer
    // this offline because nothing in this process had ever recorded an
    // image's extent - the whole reason vkCreateImage is now hooked.
    for (uint32_t i = 0; i < unmirrored_id_count; ++i) {
        if (storage_identity_lines.load(std::memory_order_relaxed) >= kMaxStorageIdentityLines)
            break;
        VkImage img = VK_NULL_HANDLE;
        ImageViewInfo vinfo{};
        bool have_view = false;
        CreatedImageInfo iinfo{};
        bool have_image = false;
        {
            std::scoped_lock lock(framebuffer_depth_view_mutex);
            if (auto v = image_view_infos.find(unmirrored_ids[i].view);
                v != image_view_infos.end()) {
                vinfo = v->second;
                img = v->second.image;
                have_view = true;
            }
            if (img != VK_NULL_HANDLE) {
                if (auto c = created_image_infos.find(img); c != created_image_infos.end()) {
                    iinfo = c->second;
                    have_image = true;
                }
            }
        }
        const uint32_t n = storage_identity_lines.fetch_add(1, std::memory_order_relaxed);
        log::warn(std::format(
            "StorageImageID #{} pass={} jobRva={:#x} set={} b{} view={} image={} "
            "viewInfoKnown={} viewFormat={} viewType={} mips={}+{} layers={}+{} | "
            "imageInfoKnown={} extent={}x{}x{} imageFormat={} usage={:#x} imageMips={} "
            "imageLayers={}. This image has NO MIRROR: the doubled call's compute wrote it "
            "straight into DOOM's own intermediate, which is a contamination path into eye 0 "
            "as well as a reason eye 1 is incomplete (review §3).",
            n, doubled_pass_seq, doubled_current_job_rva,
            reinterpret_cast<void*>(unmirrored_ids[i].set), unmirrored_ids[i].binding,
            reinterpret_cast<void*>(unmirrored_ids[i].view), reinterpret_cast<void*>(img),
            have_view, static_cast<int>(vinfo.format), static_cast<int>(vinfo.view_type),
            vinfo.range.baseMipLevel, vinfo.range.levelCount,
            vinfo.range.baseArrayLayer, vinfo.range.layerCount,
            have_image, iinfo.extent.width, iinfo.extent.height, iinfo.extent.depth,
            static_cast<int>(iinfo.format), static_cast<unsigned>(iinfo.usage),
            iinfo.mips, iinfo.layers));
    }
    doubled_storage_mirrored.fetch_add(mirrored, std::memory_order_relaxed);
    doubled_storage_unmirrored.fetch_add(unmirrored, std::memory_order_relaxed);
    const uint32_t n = doubled_dispatch_lines.fetch_add(1, std::memory_order_relaxed);
    if (n < 60) {
        log::warn(std::format(
            "ComputeProbe {} #{} pass={} jobRva={:#x} computeSetsBound={} storageMirrored={} "
            "storageUnmirrored={}{}",
            kind, n, doubled_pass_seq, doubled_current_job_rva,
            last_bound_compute_sets.size(), mirrored, unmirrored, detail));
    }
}

// TRANSFER PROBE (2026-08-17). vkCmdCopyImage / vkCmdBlitImage /
// vkCmdResolveImage name their source and destination images DIRECTLY, and
// none of them was ever hooked. Inside the doubled call that means:
//
//   src not mirrored => the doubled call READS DOOM's real, unshifted image
//                       and carries that content forward into the chain,
//                       which is enough on its own to flatten eye 1.
//   dst not mirrored => the doubled call WRITES DOOM's real image, a second
//                       time per frame - contamination of eye 0, and the
//                       same class of hole barrier substitution just closed.
//
// Exactly the gap that produced the barrier bug: mirroring substitutes
// framebuffers, and every Vulkan command that names an image by handle
// bypasses it. Log-only, on the same census arm.
void probe_image_transfer(const char* kind, VkImage src, VkImage dst) {
    // POST-FRAME-ROOT COVERAGE PROBE. DOOM's final grade could as easily be a
    // blit or a resolve as a render pass, and the transfer hooks are the only
    // place those name their images. Checked FIRST, before the doubled-call
    // gate below, or this window would never be reached.
    if (post_frame_root_window && !inside_frame_root_double && !inside_real_frame_root &&
        post_frame_root_probe_armed.load(std::memory_order_relaxed) &&
        post_frame_root_lines.load(std::memory_order_relaxed) < kMaxPostFrameRootLines) {
        bool dst_is_swapchain = false;
        {
            std::unique_lock lock(swapchains_mutex);
            for (const auto& [sc, rec] : swapchains) {
                (void)sc;
                for (VkImage s : rec.images) if (s == dst) { dst_is_swapchain = true; break; }
                if (dst_is_swapchain) break;
            }
        }
        const uint32_t n = post_frame_root_lines.fetch_add(1, std::memory_order_relaxed);
        log::warn(std::format(
            "PostFrameRoot #{} {} src={} dst={} dstIsSwapchainImage={}",
            n, kind, reinterpret_cast<void*>(src), reinterpret_cast<void*>(dst),
            dst_is_swapchain));
    }
    if (!inside_frame_root_double) return;
    if (!descriptor_census_enabled.load(std::memory_order_relaxed)) return;
    doubled_transfer_count.fetch_add(1, std::memory_order_relaxed);
    bool src_mirrored = false, dst_mirrored = false;
    {
        std::scoped_lock lock(mirror_mutex);
        src_mirrored = mirrored_images.find(src) != mirrored_images.end();
        dst_mirrored = mirrored_images.find(dst) != mirrored_images.end();
    }
    if (!src_mirrored) doubled_transfer_src_unmirrored.fetch_add(1, std::memory_order_relaxed);
    if (!dst_mirrored) doubled_transfer_dst_unmirrored.fetch_add(1, std::memory_order_relaxed);
    const uint32_t n = doubled_transfer_lines.fetch_add(1, std::memory_order_relaxed);
    if (n < 40) {
        log::warn(std::format(
            "TransferProbe {} #{} pass={} jobRva={:#x} src={} srcHasMirror={} dst={} "
            "dstHasMirror={}",
            kind, n, doubled_pass_seq, doubled_current_job_rva,
            reinterpret_cast<void*>(src), src_mirrored,
            reinterpret_cast<void*>(dst), dst_mirrored));
    }
}

// COVERAGE PROBE hooks - see the block at real_cmd_push_descriptor_set.
void VKAPI_PTR hook_cmd_push_descriptor_set(VkCommandBuffer command_buffer,
    VkPipelineBindPoint bind_point, VkPipelineLayout layout, uint32_t set,
    uint32_t write_count, const VkWriteDescriptorSet* writes) {
    kharvox::native::noteCommand(command_buffer);
    kharvox::native::checkFinalCommand(command_buffer,"hook_cmd_push_descriptor_set");
    FbHook fb_(FB_H_CMD_PUSH_DESCRIPTOR_SET);
    if (inside_frame_root_double && descriptor_census_enabled.load(std::memory_order_relaxed)) {
        doubled_push_descriptor_count.fetch_add(1, std::memory_order_relaxed);
        uint32_t mirrored = 0, unmirrored = 0;
        {
            std::scoped_lock lock(mirror_mutex);
            for (uint32_t i = 0; writes && i < write_count; ++i) {
                if (!writes[i].pImageInfo) continue;
                for (uint32_t e = 0; e < writes[i].descriptorCount; ++e) {
                    const VkImageView v = writes[i].pImageInfo[e].imageView;
                    if (v == VK_NULL_HANDLE) continue;
                    if (mirrored_views.find(v) != mirrored_views.end()) ++mirrored;
                    else ++unmirrored;
                }
            }
        }
        doubled_push_images_mirrored.fetch_add(mirrored, std::memory_order_relaxed);
        doubled_push_images_unmirrored.fetch_add(unmirrored, std::memory_order_relaxed);
        const uint32_t n = doubled_push_descriptor_count.load(std::memory_order_relaxed);
        if (n <= 20) {
            log::warn(std::format(
                "CoverageProbe pushDescriptor #{} pass={} jobRva={:#x} set={} writes={} "
                "imageViewsWithMirror={} without={}",
                n, doubled_pass_seq, doubled_current_job_rva, set, write_count,
                mirrored, unmirrored));
        }
    }
    real_cmd_push_descriptor_set(command_buffer, bind_point, layout, set, write_count, writes);
}

void VKAPI_PTR hook_cmd_pipeline_barrier2(VkCommandBuffer command_buffer,
    const VkDependencyInfo* info) {
    kharvox::native::noteCommand(command_buffer);
    kharvox::native::checkFinalCommand(command_buffer,"hook_cmd_pipeline_barrier2");
    FbHook fb_(FB_H_CMD_PIPELINE_BARRIER2);
    if(inside_frame_root_double||inside_final_pass_double)kharvox::native::fail("Vulkan synchronization2 needs native layout support");
    if (inside_frame_root_double) doubled_barrier2_count.fetch_add(1, std::memory_order_relaxed);
    real_cmd_pipeline_barrier2(command_buffer, info);
}

void VKAPI_PTR hook_cmd_wait_events(VkCommandBuffer command_buffer, uint32_t event_count,
    const VkEvent* events, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage,
    uint32_t memory_count, const VkMemoryBarrier* memories,
    uint32_t buffer_count, const VkBufferMemoryBarrier* buffers,
    uint32_t image_count, const VkImageMemoryBarrier* images) {
    kharvox::native::noteCommand(command_buffer);
    kharvox::native::checkFinalCommand(command_buffer,"hook_cmd_wait_events");
    if(inside_frame_root_double)kharvox::native::fail("event barriers in doubled frame unsupported");
    FbHook fb_(FB_H_CMD_WAIT_EVENTS);
    if (inside_frame_root_double && image_count)
        doubled_wait_events_count.fetch_add(1, std::memory_order_relaxed);
    real_cmd_wait_events(command_buffer, event_count, events, src_stage, dst_stage,
        memory_count, memories, buffer_count, buffers, image_count, images);
}

void VKAPI_PTR hook_cmd_copy_buffer_to_image(VkCommandBuffer command_buffer, VkBuffer src,
    VkImage dst, VkImageLayout dst_layout, uint32_t region_count,
    const VkBufferImageCopy* regions) {
    kharvox::native::noteCommand(command_buffer);
    kharvox::native::checkFinalCommand(command_buffer,"hook_cmd_copy_buffer_to_image");
    FbHook fb_(FB_H_CMD_COPY_BUFFER_TO_IMAGE);
    if (inside_frame_root_double)
        doubled_copy_buffer_to_image_count.fetch_add(1, std::memory_order_relaxed);
    real_cmd_copy_buffer_to_image(command_buffer, src, dst, dst_layout, region_count, regions);
}

// Captures the entry layout every template update needs to be decodable. This
// was never hooked, which is why the payload was undecodable and every
// template-written set was excluded from redirection.
VkResult VKAPI_PTR hook_create_descriptor_update_template(VkDevice device,
    const VkDescriptorUpdateTemplateCreateInfo* info, const VkAllocationCallbacks* allocator,
    VkDescriptorUpdateTemplate* tmpl) {
    FbHook fb_(FB_H_CREATE_DESCRIPTOR_UPDATE_TEMPLATE);
    const VkResult result = real_create_descriptor_update_template(device, info, allocator, tmpl);
    if (result == VK_SUCCESS && info && tmpl && info->pDescriptorUpdateEntries) {
        std::vector<VkDescriptorUpdateTemplateEntry> entries(
            info->pDescriptorUpdateEntries,
            info->pDescriptorUpdateEntries + info->descriptorUpdateEntryCount);
        std::scoped_lock lock(descriptor_template_mutex);
        descriptor_update_templates[*tmpl] = std::move(entries);
        static std::atomic_uint32_t logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 4) {
            log::warn(std::format(
                "DescriptorTemplate captured: {} entries, type={}. Without this the template "
                "payload is undecodable and every set DOOM writes through it is excluded from "
                "redirection - which is why pass 22 read mirror=0.",
                info->descriptorUpdateEntryCount, static_cast<int>(info->templateType)));
        }
    }
    return result;
}

void VKAPI_PTR hook_update_descriptor_set_with_template(VkDevice device, VkDescriptorSet set,
    VkDescriptorUpdateTemplate tmpl, const void* data) {
    FbHook fb_(FB_H_UPDATE_DESCRIPTOR_SET_WITH_TEMPLATE);
    // DECODED NOW (2026-08-20) - see descriptor_update_templates for why this
    // is the root cause of the missing effects layer.
    //
    // The template's entry list, captured at vkCreateDescriptorUpdateTemplate,
    // is exactly the layout this needs: for each entry, descriptorCount image
    // infos sit at data + offset + j*stride. Decoding them records the sampled
    // views the same way hook_update_descriptor_sets does, so the sets DOOM
    // writes by template become redirectable instead of being excluded.
    update_template_count.fetch_add(1, std::memory_order_relaxed);
    std::vector<VkDescriptorUpdateTemplateEntry> entries;
    {
        std::scoped_lock lock(descriptor_template_mutex);
        if (const auto it = descriptor_update_templates.find(tmpl);
            it != descriptor_update_templates.end())
            entries = it->second;
    }
    bool decoded = false, complete = true;
    if (!entries.empty() && data) {
        const auto* base = static_cast<const unsigned char*>(data);
        std::unique_lock lock(swapchains_mutex);
        for (const auto& e : entries) {
            const bool is_image = e.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                                  e.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
                                  e.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
                                  e.descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
            if(e.descriptorCount!=1 || e.dstArrayElement!=0){complete=false;continue;}
            const bool is_buffer=e.descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER||e.descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC||e.descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER||e.descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
            if(is_buffer){VkDescriptorBufferInfo b{};std::memcpy(&b,base+e.offset,sizeof(b));descriptor_sets[set][e.dstBinding]={b.buffer,b.offset,b.range,e.descriptorType};decoded=true;continue;}
            if(!is_image){complete=false;continue;}
            ImageDescriptorBinding rec{};
            rec.type = e.descriptorType;
            rec.count = e.descriptorCount;
            // TEMPLATE PATH COUNTER - the control that says whether DOOM writes
            // image descriptors through templates at all. If this climbs while
            // vr_volumes_recorded stays 0, the vkUpdateDescriptorSets hook the
            // volume restore lives in is simply not the path DOOM uses.
            vr_template_writes.fetch_add(1, std::memory_order_relaxed);
            for (uint32_t j = 0; j < e.descriptorCount; ++j) {
                VkDescriptorImageInfo info{};
                std::memcpy(&info, base + e.offset + static_cast<size_t>(j) * e.stride,
                            sizeof(info));
                if (j == 0) { rec.image_view = info.imageView; rec.sampler = info.sampler; }
                // views is a fixed array holding the leading elements only -
                // same convention as hook_update_descriptor_sets.
                if (j < rec.views.size()) rec.views[j] = info.imageView;
            }
            descriptor_image_sets[set][e.dstBinding] = rec;
            // P1W: the template path writes the same records; both are captured
            // or the probe would report "no write observed" for every set DOOM
            // populates through a template.
            p1_capture_write(set, e.dstBinding, rec.image_view);
            template_decode_images.fetch_add(e.descriptorCount, std::memory_order_relaxed);
            decoded = true;
        }
    }
    {
        std::scoped_lock lock(read_redirect_mutex);
        // Only refuse the set when the payload really could not be read. A
        // decoded set is as well understood as one written by
        // vkUpdateDescriptorSets, so it must NOT be marked untracked - that
        // mark is what excluded pass 22 from redirection.
        if (!decoded || !complete) {
            sets_with_untracked_bindings.insert(set);
            template_decode_misses.fetch_add(1, std::memory_order_relaxed);
        } else {
            template_decode_sets.fetch_add(1, std::memory_order_relaxed);
        }
        drop_redirected_set(set);
        sets_needing_no_redirect.erase(set);
        // ⚠ THE MISSING STAMP - this is the cutscene bug (2026-08-24).
        //
        // hook_update_descriptor_sets stamps descriptor_set_update_frame on
        // every write, and the COPY path stamps it too - with a comment saying
        // that omitting it produced "run 26's allocFrame=668 updateFrame=355
        // boundWithoutUpdateSinceAlloc=true signature with no record". This,
        // the THIRD update path, never got the same fix.
        //
        // Consequence: any set DOOM populates BY TEMPLATE is never recorded as
        // updated. While the set's handle keeps its old allocation that is
        // harmless, because updateFrame is merely stale rather than wrong. But
        // a scene transition - a cutscene - frees and reallocates descriptor
        // sets, and vkAllocateDescriptorSets hands back RECYCLED handles. The
        // set is then stamped with a NEW allocFrame while its updateFrame is
        // still whatever the handle's previous life left behind, so
        // allocFrame > updateFrame forever and the redirect's own safety check
        // refuses it - correctly, on the information it has.
        //
        // That refusal is what "VR never comes back" is. Eye 1 stops being
        // handed our mirrored images and reads DOOM's real ones instead, so
        // both eyes render the same picture: stereo collapses to mono. It
        // survives re-arming because the gap lives in a map keyed by set
        // handle that no re-arm touches, and it survives a mirror rebuild
        // because the mirrors were never the problem.
        //
        // Measured signature, 20:58 run: substitutedBinds FROZEN at 1367893
        // across three consecutive samples while fallbacks jumped 10 -> 787.
        descriptor_set_update_frame[set] =
            presented_frame.load(std::memory_order_relaxed);
    }
    real_update_descriptor_set_with_template(device, set, tmpl, data);
}

void VKAPI_PTR hook_cmd_copy_image(VkCommandBuffer command_buffer,
    VkImage src, VkImageLayout src_layout, VkImage dst, VkImageLayout dst_layout,
    uint32_t region_count, const VkImageCopy* regions) {
    kharvox::native::noteCommand(command_buffer);
    kharvox::native::checkFinalCommand(command_buffer,"hook_cmd_copy_image");
    FbHook fb_(FB_H_CMD_COPY_IMAGE);
    probe_image_transfer("copy", src, dst);
    real_cmd_copy_image(command_buffer, src, src_layout, dst, dst_layout, region_count, regions);
}

void VKAPI_PTR hook_cmd_blit_image(VkCommandBuffer command_buffer,
    VkImage src, VkImageLayout src_layout, VkImage dst, VkImageLayout dst_layout,
    uint32_t region_count, const VkImageBlit* regions, VkFilter filter) {
    kharvox::native::noteCommand(command_buffer);
    kharvox::native::checkFinalCommand(command_buffer,"hook_cmd_blit_image");
    FbHook fb_(FB_H_CMD_BLIT_IMAGE);
    probe_image_transfer("blit", src, dst);
    real_cmd_blit_image(command_buffer, src, src_layout, dst, dst_layout, region_count,
        regions, filter);
}

void VKAPI_PTR hook_cmd_resolve_image(VkCommandBuffer command_buffer,
    VkImage src, VkImageLayout src_layout, VkImage dst, VkImageLayout dst_layout,
    uint32_t region_count, const VkImageResolve* regions) {
    kharvox::native::noteCommand(command_buffer);
    kharvox::native::checkFinalCommand(command_buffer,"hook_cmd_resolve_image");
    FbHook fb_(FB_H_CMD_RESOLVE_IMAGE);
    probe_image_transfer("resolve", src, dst);
    real_cmd_resolve_image(command_buffer, src, src_layout, dst, dst_layout, region_count, regions);
}

// Would this doubled-call dispatch write one of DOOM's shared storage buffers?
//
// Measured at pass 18 (the lighting pass, 11 dispatches): five storage buffers
// - light lists, cluster grids, and a 20 KB INDIRECT-args buffer - all DOOM's
// own, all rewritten by the doubled call from a shifted camera. That corrupts
// DOOM's real light culling, which is why eye 0's sparks shrank and lost their
// weapon interaction, and why eye 1's glow/fire/fog draw from wrong lighting
// state.
//
// SUPPRESS rather than mirror, deliberately. One of the five is also read by
// HANDLE through vkCmdDispatchIndirect, which bypasses descriptor substitution
// entirely - so mirroring the descriptors would leave the doubled call writing
// our copy while reading DOOM's real args, a desync. Suppression has neither
// problem, follows the shadow-atlas precedent that already works, and leaves
// eye 1 using DOOM's light culling, which at 1.5 uu of separation is correct
// to well under a cluster.
//
// Gated on the set carrying a STORAGE_BUFFER, so the storage-IMAGE dispatches
// at passes 16 and 22 - which we now mirror correctly - are untouched.
//
// R1 SPLIT (2026-08-27). The predicate is now two pieces: the MATCH, which
// walks the recorded compute sets and is free of every gate, and the
// SUPPRESSION, which is the match plus the flags. They are split because the
// manifest has to print the kill set in a run where suppression is OFF -
// "which dispatches WOULD this predicate kill" is the fact that grows silently
// with our descriptor bookkeeping (plan trap 8) and it must be visible in
// every run, not inferred from a counter afterwards.
struct DfSsboMatch {
    bool found{false};
    uint32_t matches{0};
    uint32_t binding{0};
    VkBuffer buffer{VK_NULL_HANDLE};
    VkDescriptorSet set{VK_NULL_HANDLE};
};

DfSsboMatch dispatch_ssbo_match() {
    DfSsboMatch m{};
    std::unique_lock lock(swapchains_mutex);
    for (VkDescriptorSet set : last_bound_compute_sets) {
        auto it = descriptor_sets.find(set);
        if (it == descriptor_sets.end()) continue;
        for (const auto& [binding, d] : it->second) {
            if (d.type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
                d.type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) continue;
            ++m.matches;
            if (!m.found) {
                m.found = true;
                m.binding = binding;
                m.buffer = d.buffer;
                m.set = set;
            }
        }
    }
    return m;
}

inline bool doubled_dispatch_writes_shared_buffer(const DfSsboMatch& m) {
    if (!inside_frame_root_double) return false;
    if (!suppress_doubled_light_culling.load(std::memory_order_relaxed)) return false;
    if (m.found) suppressed_light_culling_dispatches.fetch_add(1, std::memory_order_relaxed);
    return m.found;
}

// ---------------------------------------------------------------------------
// R1 MANIFEST TABLE - see the DfSubst block above get_redirected_set for what
// this exists to answer and which two questions it deliberately does NOT
// re-ask.
// PER SIDE, SEPARATE BUDGETS, AND DELIBERATELY LARGER THAN THE WORK.
// The doubled call issues 13 dispatches. The dedupe key is (pass, first
// compute set), so if pass 18's eleven particle dispatches happen to bind
// eleven DISTINCT sets they occupy eleven rows, and a 12-slot table would
// overflow exactly as the pass-22 lighting dispatch - the last one, and the
// only one this run is really about - arrived. A cap that drops the row you
// came for reads identically to "that dispatch never happened". 32 cannot
// overflow at 13 dispatches however the sets fall.
constexpr uint32_t kDfMaxRecords = 32;
// R2: EVERY image binding of the dispatch's set, not just the storage
// outputs. R1 established that the pass-22 lighting dispatch runs and that its
// two outputs land in our mirrors - and that one of them (uavSSDO, compute-only
// and therefore the clean readout) still comes out at 20.1% of DOOM's, with its
// content spatially confined and its peak 15x lower. That is a question about
// what the shader was GIVEN, so the INPUTS have to be in the table: pass 22
// binds eight images, of which the census can only classify seven - b2 is
// `hit=unknown` and reachable only through the alias path, it resolves to a
// D24S8 depth mirror, and SSDO is depth-driven.
constexpr uint32_t kDfMaxRw = 12;

// How the binding was ACTUALLY resolved at bind time, read out of the clone.
enum DfBindClass : uint8_t {
    kDfBindNoClone = 0,   // no live clone for this set: DOOM's own set was bound unmodified
    kDfBindDoomOwn = 1,   // clone exists and carries DOOM's OWN view here - not substituted
    kDfBindMirror = 2,    // clone carries one of OUR mirror views - reads/writes eye 1's copy
    kDfBindUnclassified = 3, // clone carries a view that is neither: report it, do not guess
    kDfBindMissing = 4,   // clone exists but has no entry for this binding at all
};

const char* df_bind_class_name(uint8_t c) {
    switch (c) {
    case kDfBindDoomOwn: return "DOOM-OWN(not substituted)";
    case kDfBindMirror: return "OUR-MIRROR";
    case kDfBindUnclassified: return "UNCLASSIFIED-VIEW";
    case kDfBindMissing: return "ABSENT-FROM-CLONE";
    default: return "NO-CLONE(DOOM's set bound as-is)";
    }
}

struct DfRwBinding {
    uint32_t binding;
    VkDescriptorType type;
    VkImageView doom_view;
    VkImage doom_image;
    VkImage mirror_image;
    VkExtent2D mirror_extent;
    VkExtent3D image_extent;
    VkFormat image_format;
    uint8_t outcome;
    VkImageLayout have;
    VkImageLayout want;
    // R2: what the clone actually carries here.
    VkImageView effective_view;
    VkImage effective_image;
    uint8_t bind_class;
};

struct DfRecord {
    bool used;
    uint32_t pass;
    uintptr_t job_rva;
    VkDescriptorSet set0;
    uint32_t sets_bound;
    uint32_t dispatches;
    uint32_t indirect;
    uint32_t executed;
    uint32_t suppressed;
    bool would_suppress;
    uint32_t ssbo_matches;
    uint32_t ssbo_binding;
    VkBuffer ssbo_buffer;
    uint8_t set_outcome;
    // R2: the workgroup counts, both sides. Free - they are the hook's own
    // arguments and no instrument has ever logged them. A doubled call
    // dispatching a smaller grid than the real one writes only part of its
    // target and leaves the rest at whatever the mirror held, which is exactly
    // what uavSSDO's mirror looks like (content confined to rows 1-2 and cols
    // 2-5 of eight bands, everything else 100% dark). If the grids match, that
    // explanation is dead and the fault is in the inputs or the shader's data.
    uint32_t gx, gy, gz;
    bool group_mismatch;   // two dispatches merged into this row disagreed
    // R6: pass 16 carries BOTH a draw and a dispatch. Without this in the
    // dedupe key they would merge into one row whenever the graphics and
    // compute binds happen to share a set handle, and the counts would be
    // uninterpretable - the same class of quiet wrong answer as a cap hit
    // exactly at its count.
    bool is_draw;
    // R2: the live clone this set resolves to, so "was DOOM's set redirected
    // at all" is a printed fact rather than an inference from a CLONE line
    // three seconds earlier in the log.
    VkDescriptorSet clone;
    uint32_t clone_generation;
    uint32_t mirror_generation_now;
    bool clone_found;
    uint32_t rw_count;
    uint32_t rw_over;
    DfRwBinding rw[kDfMaxRw];
    // R7: the BUFFER bindings and the dynamic offsets. With one image binding
    // and no push constants, these are everything else the shader is given.
    struct DfBuf { uint32_t binding; VkDescriptorType type; VkBuffer buffer;
                   VkDeviceSize offset; VkDeviceSize range; };
    uint32_t buf_count;
    uint32_t buf_over;
    DfBuf buf[6];
    uint32_t dyn_count;
    uint32_t dyn[8];
};

std::mutex df_records_mutex;
DfRecord df_doubled[kDfMaxRecords]{};
DfRecord df_real[kDfMaxRecords]{};
std::atomic_uint32_t df_doubled_over{0};
std::atomic_uint32_t df_real_over{0};
std::atomic_bool df_armed{false};
std::atomic_bool df_selftest_passed{false};
std::atomic_bool df_emitted{false};
// ONE FRAME, LATCHED BY THE FRAME ROOT'S OWN SERIAL. The disarm happens on the
// present thread, which is one or two frames behind the render thread, so
// without this the table would silently accumulate two frames' dispatches into
// one row and the counts would be a number nobody could interpret. The first
// record wins the latch; every later frame is dropped and counted.
std::atomic_uint32_t df_frame_latched{0};
std::atomic_uint32_t df_frames_dropped{0};

void df_reset_tables() {
    std::scoped_lock lock(df_records_mutex);
    for (uint32_t i = 0; i < kDfMaxRecords; ++i) { df_doubled[i] = DfRecord{}; df_real[i] = DfRecord{}; }
    df_doubled_over.store(0, std::memory_order_relaxed);
    df_real_over.store(0, std::memory_order_relaxed);
    {
        std::scoped_lock lk(df_outcome_mutex);
        df_storage_outcomes.clear();
        df_set_outcomes.clear();
    }
    // NOT df_clone_effective: it is populated when a clone is BUILT, which for
    // the sets this run cares about happened seconds before the arm. Clearing
    // it here would recreate exactly the blindness R2 exists to remove.
    df_outcomes_dropped.store(0, std::memory_order_relaxed);
    df_frame_latched.store(0, std::memory_order_relaxed);
    df_frames_dropped.store(0, std::memory_order_relaxed);
    df_emitted.store(false, std::memory_order_relaxed);
}
#endif  // DOOMVR_RESEARCH_TOOLS

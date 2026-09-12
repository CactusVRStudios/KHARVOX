// 02_probes.inc - read-only probes: V1/C1/C2/W1, the frame budget, the freqHigh census
// 3916 of these lines sit inside #ifdef DOOMVR_RESEARCH_TOOLS,
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

// -------------------------------- C1: THE SHARED CONTEXT'S SLOTS ----------
// docs/RESULT-2026-08-26-RUN2-DELTA-ZERO-AND-DEFERRED-CLEAN.md §3. Run 2
// established that the real and the doubled dispatch bind out of ONE AND THE
// SAME context object (`sameCtx=true`, path=B, no copy step) - so the doubled
// call's binding-2 dummy, which P1W run 5 showed DOOM itself writes, has to be
// something that writes that slot in the SHARED context between the two
// dispatches. C1 reads the slots directly.
//
// For each declaration global: `decl = *[base+rva]`, `slot = decl->[0x68]`,
// and the slot's record is 16 bytes at `ctx + (slot+7)*16` with an int16 index
// at `ctx + slot*16 + 0x78` (the same record's second half - see
// RESULT-2026-08-26-STATIC-PERVIEW-CONTEXT.md §3).
//
// SNAPSHOT BEFORE **AND AFTER** each dispatch. Before-vs-before across the two
// sides says whether the doubled call already inherits a different slot; the
// after-snapshots say whether the job itself rewrote it. Both are needed: a
// difference visible only in the after pair is the job's own doing, one already
// present in the before pair is somebody else's.
//
// BOUNDED BY CONSTRUCTION: only the first kC1Reps dispatches per (job, side)
// are captured, so this is at most 2*2*2*2*12 slot reads for the whole run
// regardless of how many hundred times the jobs dispatch.
//
// CONTROLS, each aimed at a specific confusable failure:
//   * a decl global that is still null (constructed lazily) is reported
//     UNRESOLVED, never as slot 0 with a zero handle - which would read
//     exactly like "the engine bound nothing there";
//   * `slot` is range-checked; an out-of-range slot is the finding, not a
//     licence to read at it;
//   * every read is guarded and its success recorded per decl;
//   * the arm-time self-test drives a synthetic context and a synthetic decl
//     through the same resolver, and additionally requires it to REJECT a null
//     decl pointer and an out-of-range slot.
bool safe_write_bytes(uintptr_t address, const void* src, size_t size);

constexpr uint32_t kC1Reps = 2;
struct C1Decl { uintptr_t rva; const char* name; };
// Names verified by dumping each record's +0x10 name-string pointer. The four
// particle ones and viewNormalMap round-tripped exactly; the rest of the
// 0x384xxxx family did not resolve cleanly, so they carry their RVA only and
// MUST NOT be quoted by name.
const C1Decl kC1Decls[] = {
    { 0x570B780, "particleIndirectDrawArgs" },
    { 0x570B7A0, "particleRandom" },
    { 0x570B7C0, "particleVectorFields" },
    { 0x570B7E0, "particle_570B7E0?" },
    { 0x3845458, "viewNormalMap" },
    { 0x3845478, "decl_3845478?" },
    { 0x3844838, "decl_3844838?" },
    { 0x3844858, "decl_3844858?" },
    { 0x3844878, "decl_3844878?" },
    { 0x3844898, "decl_3844898?" },
    { 0x3844F38, "decl_3844F38?" },
    { 0x38445D8, "decl_38445D8?" },
};
constexpr uint32_t kC1DeclCount = sizeof(kC1Decls) / sizeof(kC1Decls[0]);

enum : uint32_t {
    kC1OkDecl   = 1u << 0,  // the decl global held a non-null pointer
    kC1OkSlot   = 1u << 1,  // decl->[0x68] read and in range
    kC1OkHandle = 1u << 2,  // the 16-byte resource record read
    kC1OkIndex  = 1u << 3,  // the int16 index read
};

struct C1Slot { uint32_t ok; int32_t slot; uint64_t lo, hi; int32_t idx; };
struct C1Snap { uintptr_t ctx; C1Slot d[kC1DeclCount]; };

std::atomic_bool c1_armed{false};
std::atomic_bool c1_selftest_passed{false};
C1Snap c1_pre[kV1Jobs][kV1Sides][kC1Reps]{};
C1Snap c1_post[kV1Jobs][kV1Sides][kC1Reps]{};
std::atomic_uint32_t c1_taken[kV1Jobs][kV1Sides]{};

// THE PRODUCTION PATH. Base and ctx are parameters so the self-test can hand
// it synthetic ones.
void c1_snapshot(uintptr_t base, uintptr_t ctx, C1Snap& out) {
    out.ctx = ctx;
    for (uint32_t i = 0; i < kC1DeclCount; ++i) {
        C1Slot& s = out.d[i];
        s = C1Slot{};
        if (!base || !ctx) continue;
        uintptr_t decl = 0;
        if (!safe_read_bytes(base + kC1Decls[i].rva, &decl, sizeof(decl)) || !decl) continue;
        s.ok |= kC1OkDecl;
        int32_t slot = 0;
        if (!safe_read_bytes(decl + 0x68, &slot, sizeof(slot))) continue;
        s.slot = slot;
        // Range check. The context's slot table is indexed (slot+7)*16; a wild
        // slot would read megabytes away and is a finding, not a value.
        if (slot < 0 || slot > 4096) continue;
        s.ok |= kC1OkSlot;
        uint64_t rec[2]{};
        if (safe_read_bytes(ctx + (static_cast<size_t>(slot) + 7) * 16, rec, sizeof(rec))) {
            s.lo = rec[0];
            s.hi = rec[1];
            s.ok |= kC1OkHandle;
        }
        int16_t idx = 0;
        if (safe_read_bytes(ctx + static_cast<size_t>(slot) * 16 + 0x78, &idx, sizeof(idx))) {
            s.idx = idx;
            s.ok |= kC1OkIndex;
        }
    }
}

alignas(16) uint8_t c1_fake_ctx[0x20000]{};
alignas(16) uint8_t c1_fake_decl[0x80]{};
uintptr_t c1_fake_decl_ptr = 0;
uintptr_t c1_fake_decl_null = 0;

bool c1_selftest(std::string& detail) {
    // Drive the resolver with a synthetic module base so kC1Decls[0].rva lands
    // on our own decl-pointer cell - no 117 MB of fake image needed.
    // ⚠ THE INDEX LIVES INSIDE THE HANDLE RECORD. (slot+7)*16 == slot*16+0x70,
    // and 0x18F6930 writes its int16 at slot*16+0x78 - byte 8 of the very same
    // 16 bytes that 0x18F6D50's `movups` writes. Run 3's self-test planted a
    // full 16-byte handle AND an index and then failed its own round-trip,
    // because the index had overwritten bytes 8-9 of the handle. That was
    // contradictory TEST DATA, not a resolver fault (ok=0xf: all four reads
    // succeeded). kPlantHi therefore carries kPlantIdx in its low 16 bits, so
    // the two plants agree instead of fighting.
    constexpr int32_t kPlantSlot = 3;
    constexpr uint64_t kPlantLo = 0x1122334455667788ull;
    constexpr int16_t kPlantIdx = 0x5A5;
    constexpr uint64_t kPlantHi = 0x99AABBCCDDEE0000ull | static_cast<uint16_t>(kPlantIdx);
    std::memset(c1_fake_ctx, 0, sizeof(c1_fake_ctx));
    std::memset(c1_fake_decl, 0, sizeof(c1_fake_decl));
    std::memcpy(c1_fake_decl + 0x68, &kPlantSlot, sizeof(kPlantSlot));
    c1_fake_decl_ptr = reinterpret_cast<uintptr_t>(c1_fake_decl);
    const uintptr_t ctx = reinterpret_cast<uintptr_t>(c1_fake_ctx);
    std::memcpy(c1_fake_ctx + (kPlantSlot + 7) * 16, &kPlantLo, sizeof(kPlantLo));
    std::memcpy(c1_fake_ctx + (kPlantSlot + 7) * 16 + 8, &kPlantHi, sizeof(kPlantHi));
    std::memcpy(c1_fake_ctx + kPlantSlot * 16 + 0x78, &kPlantIdx, sizeof(kPlantIdx));

    const uintptr_t fake_base =
        reinterpret_cast<uintptr_t>(&c1_fake_decl_ptr) - kC1Decls[0].rva;
    C1Snap snap{};
    c1_snapshot(fake_base, ctx, snap);
    const C1Slot& g = snap.d[0];
    const bool round =
        (g.ok == (kC1OkDecl | kC1OkSlot | kC1OkHandle | kC1OkIndex)) &&
        g.slot == kPlantSlot && g.lo == kPlantLo && g.hi == kPlantHi &&
        g.idx == kPlantIdx;

    // Control 1: a NULL decl global must report UNRESOLVED, not slot 0.
    const uintptr_t null_base =
        reinterpret_cast<uintptr_t>(&c1_fake_decl_null) - kC1Decls[0].rva;
    C1Snap nsnap{};
    c1_snapshot(null_base, ctx, nsnap);
    const bool null_ok = nsnap.d[0].ok == 0 && nsnap.d[0].lo == 0;

    // Control 2: an out-of-range slot must be refused, not read at.
    const int32_t wild = 999999;
    std::memcpy(c1_fake_decl + 0x68, &wild, sizeof(wild));
    C1Snap wsnap{};
    c1_snapshot(fake_base, ctx, wsnap);
    const bool wild_ok = (wsnap.d[0].ok & kC1OkDecl) && !(wsnap.d[0].ok & kC1OkSlot) &&
                         !(wsnap.d[0].ok & kC1OkHandle) && wsnap.d[0].slot == wild;
    std::memcpy(c1_fake_decl + 0x68, &kPlantSlot, sizeof(kPlantSlot));

    detail = std::format("roundTrip={} nullDeclUnresolved={} wildSlotRefused={} ok={:#x}",
        round, null_ok, wild_ok, g.ok);
    return round && null_ok && wild_ok;
}

// ------------------------------------ D1: FORCE THE DOUBLED TIMESTEP ------
// OFF BY DEFAULT AND NOT ARMED BY THE C1 PRESS. Run 2 measured the doubled
// particle dispatch computing delta = 0 on 12 of 12 records, because the real
// dispatch wrote [0x705CA28] moments earlier with the SAME jobArg pointer and
// the doubled dispatch then subtracts that value from itself. D1 writes the
// latch back by the last REAL delta immediately before the doubled dispatch,
// so the job computes the same non-zero timestep the real call did.
//
// PER THE STANDING RULE, the write is RELATIVE TO WHAT THE ENGINE JUST
// COMPUTED: latch = jobArg[0x10] - lastRealDelta, both read at that moment.
// Never an absolute rebuilt from another clock.
//
// Self-cleaning: the job's own 0x19AEA53 stores jobArg[0x10] into the latch as
// it runs, so no restore is needed and no state outlives the dispatch.
// [0x705CA28] has exactly two references image-wide, both inside this job -
// there is no other consumer to disturb and no integrity gate over it.
// ---------------------- C2: WHICH SLOT DIFFERS, AND WHAT IS IT CALLED ----
// W1 (run 6) resolved the write site: binding 2 is written by ONE builder on
// both sides - `call 0x1893CA0` at 0x19AF250, reached because [rbp+0x98]==0
// (PATH B), returning to the observed frame DOOM+0x19AF255. Same code path,
// real writes a VOL, doubled writes an OTH. So the difference is in its INPUT,
// and C1 could not see it because C1 watched only the twelve PATH A decls.
//
// C2 stops guessing which declaration matters and diffs the WHOLE context slot
// table, then names the slots that differ by walking DOOM's own declaration
// registration list.
//
// THE LIST. Each declaration is a uniform 0x20-byte record:
//     +0x00 decl pointer   +0x08 shared registrar   +0x10 name string
//     +0x18 next
// and the 0x241xxx constructor stubs chain them onto a head at 0x5D78E70,
// storing `record+0x10`. So walking is: p = [0x5D78E70]; name = [p];
// next = [p+8]; decl = [p-0x10]; slot = decl->[0x68]. That yields a live
// slot -> name map, which turns "slot 2185 differs" into a name.
//
// BOUNDED BY CONSTRUCTION: one snapshot per (job, side), kC2MaxSlots slots,
// the walk capped at kC2MaxNodes nodes, the reported difference list capped.
//
// CONTROLS:
//   * the diff reports how many slots were READABLE, not just how many
//     differed - an unreadable tail would otherwise read as "all identical";
//   * a slot that is all-zero on BOTH sides is counted separately, because
//     "identical" over dead space is not evidence of anything;
//   * the name walk reports its node count and whether it terminated cleanly,
//     so a truncated map cannot masquerade as "slot not found";
//   * a differing slot whose name does not resolve is still reported, by index.
constexpr uint32_t kC2MaxSlots = 4096;
constexpr uint32_t kC2MaxNodes = 8192;
constexpr uint32_t kC2MaxReport = 40;
// Shared by C3 (which slot owns binding 2) and K1 (which slot owns each H1
// target). Declared here rather than beside c3_emit_table because K1 runs from
// hook_queue_present, thousands of lines earlier in the file.
constexpr uint32_t kC3MaxScans = 1200;
constexpr uint32_t kC3Window = 512;
constexpr uintptr_t kC2DeclListHeadRva = 0x5D78E70;

struct C2Snap {
    uintptr_t ctx;
    uint32_t readable;
    uint64_t lo[kC2MaxSlots];
    uint64_t hi[kC2MaxSlots];
    bool ok[kC2MaxSlots];
};
std::atomic_bool c2_armed{false};
C2Snap c2_snap[kV1Jobs][kV1Sides]{};
std::atomic_uint32_t c2_taken[kV1Jobs][kV1Sides]{};

void c2_snapshot(uintptr_t ctx, C2Snap& out) {
    out.ctx = ctx;
    out.readable = 0;
    for (uint32_t s = 0; s < kC2MaxSlots; ++s) {
        uint64_t rec[2]{};
        if (safe_read_bytes(ctx + (static_cast<size_t>(s) + 7) * 16, rec, sizeof(rec))) {
            out.lo[s] = rec[0];
            out.hi[s] = rec[1];
            out.ok[s] = true;
            ++out.readable;
        } else {
            out.lo[s] = 0;
            out.hi[s] = 0;
            out.ok[s] = false;
        }
    }
}

// Live slot -> declaration-name map. Returns nodes walked; sets `clean` when
// the list terminated on a null next rather than on the cap.
//
// ⚠ SLOTS COLLIDE, AND THE FIRST WRITER USED TO WIN SILENTLY. Run 15 named a
// 3818x2032 R16G16 target `AMD_VK_EXTENSIONS`, a depth target `viewNormalMap`,
// and a 954x508 colour target `numMultisamples` - all plainly wrong, because
// several declarations report the same `[0x68]` and the map kept whichever the
// walk reached first. A name that might be one of several is not a name, and
// this investigation has already lost weeks to a confident wrong label.
//
// Every claimant is now collected. A slot with one claimant is a name; a slot
// with several is reported as `a|b|c` and must be treated as UNRESOLVED.
uint32_t c2_build_name_map(uintptr_t base,
    std::unordered_map<int32_t, std::vector<std::string>>& out, bool& clean) {
    clean = false;
    out.clear();
    if (!base) return 0;
    uintptr_t p = 0;
    if (!safe_read_bytes(base + kC2DeclListHeadRva, &p, sizeof(p))) return 0;
    uint32_t n = 0;
    while (p && n < kC2MaxNodes) {
        ++n;
        uintptr_t next = 0, name_ptr = 0, decl = 0;
        if (!safe_read_bytes(p + 8, &next, sizeof(next))) break;
        if (safe_read_bytes(p, &name_ptr, sizeof(name_ptr)) &&
            safe_read_bytes(p - 0x10, &decl, sizeof(decl)) && decl && name_ptr) {
            int32_t slot = 0;
            if (safe_read_bytes(decl + 0x68, &slot, sizeof(slot)) &&
                slot >= 0 && slot < static_cast<int32_t>(kC2MaxSlots)) {
                char nm[96]{};
                if (safe_read_bytes(name_ptr, nm, sizeof(nm) - 1)) {
                    nm[sizeof(nm) - 1] = '\0';
                    // Only accept plausible ASCII names; anything else means
                    // the record shape is not what we think and the entry must
                    // not be trusted into the map.
                    bool okname = nm[0] != '\0';
                    for (size_t i = 0; nm[i] && i < sizeof(nm) - 1; ++i)
                        if (static_cast<unsigned char>(nm[i]) < 0x20 ||
                            static_cast<unsigned char>(nm[i]) > 0x7E) { okname = false; break; }
                    if (okname) {
                        auto& v = out[slot];
                        if (std::find(v.begin(), v.end(), nm) == v.end())
                            v.emplace_back(nm);
                    }
                }
            }
        }
        if (!next) { clean = true; break; }
        p = next;
    }
    return n;
}

// One claimant is a name. Several is not, and is rendered so it cannot be
// quoted as one. Zero is "<unnamed>", which is also not a name.
std::string c2_name_of(const std::unordered_map<int32_t, std::vector<std::string>>& names,
                       int32_t slot) {
    auto it = names.find(slot);
    if (it == names.end() || it->second.empty()) return "<unnamed>";
    if (it->second.size() == 1) return it->second.front();
    std::string s = "<AMBIGUOUS:";
    for (size_t i = 0; i < it->second.size() && i < 6; ++i) {
        if (i) s += "|";
        s += it->second[i];
    }
    if (it->second.size() > 6) s += "|...";
    return s + ">";
}

// -------------------------------- W1: WHO WRITES BINDING 2 ---------------
// The plan's original highest-value instrument, never run, and now sharply
// targeted. Runs 4 and 5 established that the context's twelve declaration
// slots are byte-identical on both sides while the descriptor sets DOOM writes
// differ at binding 2 (REAL=VOL, DOUBLED=OTH). So binding 2's source is a
// declaration outside the twelve-decl PATH A list, and the divergence is in
// whatever builds the sets. A stack backtrace at the moment of the write names
// that builder.
//
// Cheap by construction: there is exactly ONE vkUpdateDescriptorSets wrapper
// (0x1A7BBB0) and one flush family (0x1893B30/0x1893CA0), so there are very
// few distinct chains to resolve.
//
// BOUNDED: only binding 2, deduped by chain hash, hard cap kW1MaxChains.
// RtlCaptureStackBackTrace with a small depth is a few hundred ns and only
// runs while P1W is armed and only for one binding.
//
// CONTROLS, aimed at this probe's own confusable failures:
//   * chainsCaptured=0 while P1W counts binding-2 writes means the backtrace
//     itself failed - a different answer from "no writes happened", and the
//     emit refuses to interpret in that case;
//   * every frame is classified DOOM / OURS / other-module. A chain that lies
//     entirely inside doomvr.dll means WE wrote that descriptor, not DOOM,
//     and would invert the reading of the whole table;
//   * the view handle is recorded with each chain, so a chain can be tied to
//     the VOL write or the OTH write rather than guessed at.
constexpr uint32_t kW1MaxChains = 24;
constexpr uint32_t kW1Depth = 12;
struct W1Chain {
    uint64_t hash;
    uint32_t side;      // 0 real, 1 doubled
    uint32_t hits;
    VkImageView view;
    uint32_t frames;
    uintptr_t frame[kW1Depth];
};
std::atomic_bool w1_armed{false};
std::mutex w1_mutex;
W1Chain w1_chains[kW1MaxChains]{};
std::atomic_uint32_t w1_count{0};
std::atomic_uint32_t w1_seen{0};
std::atomic_uint32_t w1_dropped{0};
std::atomic_uint32_t w1_backtrace_failures{0};

// C3 needs the actual binding-2 view handles to search for. Recorded here
// because this is the only place they are known per side.
std::atomic_uintptr_t c3_view[kV1Sides]{};

void w1_capture(uint32_t binding, VkImageView view) {
    if (binding != 2 || !w1_armed.load(std::memory_order_relaxed)) return;
    {
        const uint32_t s = inside_frame_root_double ? 1u : 0u;
        uintptr_t expected = 0;
        c3_view[s].compare_exchange_strong(expected, reinterpret_cast<uintptr_t>(view),
            std::memory_order_relaxed);
    }
    w1_seen.fetch_add(1, std::memory_order_relaxed);
    void* raw[kW1Depth]{};
    const uint32_t n = RtlCaptureStackBackTrace(1, kW1Depth, raw, nullptr);
    if (n == 0) { w1_backtrace_failures.fetch_add(1, std::memory_order_relaxed); return; }
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < n; ++i) {
        h ^= reinterpret_cast<uintptr_t>(raw[i]);
        h *= 1099511628211ull;
    }
    const uint32_t side = inside_frame_root_double ? 1u : 0u;
    h = h * 31 + side;
    std::scoped_lock lock(w1_mutex);
    const uint32_t have = w1_count.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < have; ++i) {
        if (w1_chains[i].hash == h) { ++w1_chains[i].hits; return; }
    }
    if (have >= kW1MaxChains) { w1_dropped.fetch_add(1, std::memory_order_relaxed); return; }
    W1Chain& c = w1_chains[have];
    c.hash = h;
    c.side = side;
    c.hits = 1;
    c.view = view;
    c.frames = n;
    for (uint32_t i = 0; i < n; ++i) c.frame[i] = reinterpret_cast<uintptr_t>(raw[i]);
    w1_count.store(have + 1, std::memory_order_relaxed);
}

// ------------------------- F1: RESTORE THE BOUNDS-CHECK COUNT ------------
// Run 8 (E1) measured it: at the particle job the REAL dispatch reads
// [0x705CA24] = 9 and the DOUBLED dispatch reads 0, same table pointer, same
// sibling count. The job zeroes it on its way out (`mov [0x705CA24], r13d` at
// 0x19AF2C4 with r13d zeroed at 0x19AEC89), so our re-invocation runs with an
// empty table, every index fails `idx >= count` at 0x19AF14F, and BOTH
// gpuParticleVectorField1 and particles take the one shared default object
// [[0x3848D98]+0x10] - which is exactly the duplicate at binding 2 that C3
// identified by handle.
//
// F1 saves the count the engine itself had before the real dispatch and puts
// that same value back before the doubled one. Per the standing rule the write
// is the value THE ENGINE JUST COMPUTED, not a constant and not a figure
// rebuilt from another clock: whatever it read as 9, we restore as 9.
//
// Self-cleaning: 0x19AF2C4 zeroes it again at the end of the doubled run, so
// nothing outlives the dispatch. [0x705CA24] is renderer+0x14 with ONE runtime
// writer and ONE reader, both inside this job, and no checksum over it.
//
// The table POINTER [0x705CA18] is unchanged between the two dispatches
// (measured), so the entries the restored count admits are the same entries the
// real dispatch resolved moments earlier.
//
// IN-BAND READOUT: C2/C3/P1W stay armed. If this works, slot 1056 stops
// matching slot 1057 on the doubled side and P1W's doubled binding 2 becomes
// VOL. That is a far sharper pass/fail than the fog metric, which is the
// secondary judge.
std::atomic_bool g1_suppress_particle_sim{false};
std::atomic_uint32_t g1_suppressed{0};

std::atomic_bool f1_enabled{false};
std::atomic_int32_t f1_saved_count{0};
std::atomic_uint32_t f1_attempts{0};
std::atomic_uint32_t f1_restores{0};
std::atomic_uint32_t f1_failures{0};
std::atomic_uint32_t f1_no_saved{0};
constexpr uint32_t kF1MaxAttempts = 400;

std::atomic_bool d1_enabled{false};
std::atomic_uint32_t d1_attempts{0};
std::atomic_uint32_t d1_last_real_delta{0};
std::atomic_uint32_t d1_writes{0};
std::atomic_uint32_t d1_write_failures{0};
constexpr uint32_t kD1MaxAttempts = 400;

// Bit31Probe (docs/STEREO-RESTART-PLAN.md Task 2 follow-up, 2026-08-11):
// static RE of fcn.141a37b80/func_0x141893680/func_0x141894620/RenderDepth's
// fcn.141a36c10 found no code-level reason the real and extra RenderOpaque
// calls should see different query-gating outcomes - the gate is bit 31 of
// each draw command's +0x2c field, baked in once at gather time and shared,
// unmutated, between both traversals. This counts, per RenderOpaque call
// bracket (real or extra - see the reset/log sites in hook_doom_render_opaque
// below), how many draw commands DOOM+0x1A37910 actually receives and how
// many have that bit set - the raw data static analysis says should be
// identical, checked live instead of assumed.
thread_local uint32_t bit31_probe_total_draws = 0;
thread_local uint32_t bit31_probe_flagged_draws = 0;

// PassDrawCountProbe (docs/STEREO-RESTART-PLAN.md Task 3, 2026-08-11) - see
// the tracking logic in hook_cmd_begin_render_pass/hook_cmd_end_render_pass
// for the full reasoning. -1 = not currently inside either of RenderOpaque's
// two known internal passes (or they haven't been captured yet).
thread_local int current_render_opaque_pass_slot = -1;
thread_local uint32_t pass1_draw_count = 0;
thread_local uint32_t pass2_draw_count = 0;
thread_local uint32_t other_pass_draw_count = 0;

// ScissorViewportProbe (docs/STEREO-RESTART-PLAN.md Task 3) - captures only
// the FIRST scissor/viewport rect seen per RenderOpaque call (real or extra),
// reset alongside the other per-call probes in hook_doom_render_opaque and
// the StereoDualEyeDump extra-call site. A prior finding (before these hooks
// were removed for crashing) already showed the real call uses a full-extent
// rect; this exists to check whether the extra call's redirected target ever
// disagrees.
thread_local bool scissor_captured_this_call = false;
thread_local VkRect2D captured_scissor{};
thread_local bool viewport_captured_this_call = false;
thread_local VkViewport captured_viewport{};

// WorldPipelineBindingProbe (docs/STEREO-RESTART-PLAN.md Task 3, new
// candidate) - see ImageDescriptorBinding's declaration above for the full
// reasoning. representative_world_pipeline_this_call is chosen in
// hook_cmd_bind_pipeline as the FIRST pipeline bound this RenderOpaque call
// with depthTestEnable && depthWriteEnable (a plausible signature for
// opaque world geometry, distinct from sky/decal passes that read but don't
// write depth - same heuristic PipelineDepthStencilProbe's data already
// suggested). hook_cmd_bind_descriptor_sets then captures the actual bound
// resource IDs the FIRST time that exact (pipeline, command buffer) pair's
// descriptor sets are bound, once per call.
thread_local VkPipeline representative_world_pipeline_this_call = VK_NULL_HANDLE;
thread_local VkCommandBuffer representative_world_pipeline_cmdbuf_this_call = VK_NULL_HANDLE;
thread_local bool world_binding_captured_this_call = false;
// WorldVertexIndexProbe (docs/STEREO-RESTART-PLAN.md Task 3, last
// candidate) - see CmdBindVertexBuffersFn's declaration above for the full
// reasoning. Reuses WorldPipelineBindingProbe's existing representative-
// pipeline selection (don't re-derive) - captures the first vertex-buffer
// bind, index-buffer bind, and indirect-draw-parameter buffer seen on that
// same (pipeline, command buffer) pair, independently, since their relative
// order isn't guaranteed.
thread_local bool world_vertex_captured_this_call = false;
thread_local bool world_index_captured_this_call = false;
thread_local bool world_indirect_captured_this_call = false;

// Defined further down, next to the lever they diagnose; declared here so the
// scan can fire from the render-view hook, which is the one site that runs
// whether or not OpenXR came up.
bool looks_like_world_camera_matrix(const float* f);
void log_view_origin_scan(uintptr_t render_view, const float* vp_before,
                          const float* solved_origin);
bool solve_view_origin(const float* m, float* out_p, float* out_residual);

// Self-arming for log_view_origin_scan. NOT on a hotkey, deliberately: the
// scan is a one-shot read of 8 regions, so it is the cheap thing, and gating a
// cheap thing behind a keypress only creates a way for the run to come back
// with the probe switched off. Three fires, spaced, so an early one at a
// loading screen cannot be the only sample and a stable offset can be told
// from a coincidence. Counted, so "never fired" is visible in the log rather
// than looking identical to "fired and found nothing".
std::atomic_uint32_t origin_scan_gate{0};
std::atomic_uint32_t origin_scan_fires{0};
// Counted BEFORE the world-camera predicate, so "the hook never ran" and "the
// hook ran but nothing looked like a world view" cannot collapse into the same
// zero. Reported on every PluginHealth line.
std::atomic_uint32_t origin_scan_views_seen{0};
// Per-frame doom_render_view call count, feeding the menu detector. Reset every
// present; see wrap::publish_gui_draw_rate.
std::atomic_uint64_t g_render_view_calls_this_frame{0};
// Consecutive world views with a stationary camera. Reported so a run that
// never fires shows WHY - the owner kept walking - rather than looking broken.
std::atomic_uint32_t origin_scan_still_streak{0};
// Origin-tracking probe: distance between CameraSrc+0x14b0 and the position
// solved from the matrix. min is carried alongside max because a running max
// alone never comes back down and one transient would latch it.
std::atomic_uint32_t origin_track_samples{0};
std::atomic<float> origin_track_last{0.0f};
std::atomic<float> origin_track_max{0.0f};
std::atomic<float> origin_track_min{1e30f};
// The same distance measured against a rows-0,1,3 solve, to test whether the
// constant 3.0 is the projection near-plane term rather than a real offset.
std::atomic<float> origin_track_last013{0.0f};
std::atomic<float> origin_track_resid013{0.0f};
// Lever O state, declared here because the A/B driver in the render-view hook
// runs long before the lever itself is defined.
std::atomic_bool lever_o_enabled{false};
std::atomic_uint32_t lever_o_applied{0};
std::atomic_uint32_t lever_o_restored{0};
std::atomic_uint32_t lever_o_no_clean{0};
size_t collect_origin_sites(uintptr_t* addrs, size_t cap);
bool solve_view_origin_rows(const float* m, const int* rows, float* out_p,
                            float* out_residual);
constexpr size_t kCameraSrcOriginOffset = 0x14b0;

// ViewOriginScan fires from the render-view hook, NOT from the doubled call.
//
// 2026-08-23: it was first hung off apply_lever_a_mutation's unified!=0 branch,
// which only executes inside the frame-root double. The run came back with 44
// xrGetSystem failures, leverAapplied=0 and zero scan lines - no OpenXR means
// no eye 1, means no doubled call, means the probe could not fire. That was an
// arming error, not a result.
//
// The scan only needs to LOCATE the camera-position field, and that field
// exists in the real call exactly as it does in the doubled one, so nothing
// about the question requires eye 1 or a headset. The render-view hook runs
// every frame regardless of XR state and regardless of the stereo offset.
//
// ONE fire site, deliberately: a second detector on the other side of the
// stereo branch would double-fire the moment the mode flips.
//
// Its own function rather than inline in the hook: that hook uses __try, and
// MSVC refuses __try in a frame that also needs object unwinding, which
// std::format introduces.
void maybe_fire_origin_scan(uintptr_t view) {
    if (view == 0) return;
    const uint32_t vp_seen = origin_scan_views_seen.fetch_add(1, std::memory_order_relaxed);

    // ===== WRAPAROUND: LIVE PROJECTION SCALE =============================
    //
    // This is deliberately NOT a new reader and NOT a new DOOM hook. It rides
    // the sampler that already exists here: the same +0xC44 read, the same
    // looks_like_world_camera_matrix predicate that already filters GUI and
    // shadow views out of the count below. The wraparound path needs DOOM's
    // live projection scale so the FOV it declares tracks the in-game FOV
    // slider instead of a constant baked in when the slider was somewhere
    // else - and the constant is the whole "zoomed in and blurry" class.
    //
    // COST IS BOUNDED BY CONSTRUCTION. This hook runs ~40,000 times a second
    // (see the origin-scan comment below), so the body is entered once every
    // 256 calls - about 150 publishes a second, three orders of magnitude
    // more than the once-per-frame the consumer needs. Everything inside is
    // one 64-byte guarded read, four square roots and a memcpy.
    //
    // It runs whether or not wraparound is armed, because Phase 0 must print
    // these numbers on EVERY session including the headset-free diagnostic
    // one, and because a sample count of zero is what proves the sampler is
    // dead rather than the camera being still.
    //
    // NOTE THE PREDICATE IS PASSED IN, NOT USED AS THE GATE.
    // looks_like_world_camera_matrix bounds row 1's length to 1.65..1.90, and
    // RUN 1 asks the owner to put DOOM's FOV slider at MAXIMUM. If row 1's
    // length IS the FOV term, the slider can push it out of that band, the
    // predicate would reject every world view, and the run would come back
    // with samples=0 looking like a dead instrument - a five-minute run lost
    // to our own gate. wrap::publish_world_vp applies its own wider structural
    // predicate and RECORDS the strict verdict alongside, so a strict count
    // that stops climbing while the wide count keeps going is a finding
    // rather than a failure.
    wrap::note_hook_call();
    // Per-FRAME render-view count, sampled and reset in the present hook. This
    // is the menu detector's input: ~70 calls/frame in gameplay, ~493 at the
    // menu, measured in this repo before the detector existed.
    g_render_view_calls_this_frame.fetch_add(1, std::memory_order_relaxed);
    if ((vp_seen & 0xFFu) == 0) {
        alignas(16) float wrap_vp[16]{};
        if (safe_read_bytes(view + 0xC44, wrap_vp, sizeof(wrap_vp))) {
            wrap::publish_world_vp(wrap_vp, looks_like_world_camera_matrix(wrap_vp));
        } else {
            // COUNTED, NOT SWALLOWED. RUN 1's only instrument here was
            // worldViews, a single counter standing for "the hook never ran",
            // "the read failed" and "the predicate said no" all at once - and
            // it read zero, so the run could not say which. Never again.
            wrap::note_vp_read_failure();
        }
    }

    if (origin_scan_fires.load(std::memory_order_relaxed) >= 3) return;
    alignas(16) float vp[16]{};
    if (!safe_read_bytes(view + 0xC44, vp, sizeof(vp))) return;
    // Gated on the world-camera predicate so the count advances only on world
    // views - GUI and shadow views also pass through this hook and would
    // otherwise burn the gate before a world view is ever seen.
    if (!looks_like_world_camera_matrix(vp)) return;
    origin_scan_gate.fetch_add(1, std::memory_order_relaxed);

    float p[3]{};
    float residual = 0.0f;
    if (!solve_view_origin(vp, p, &residual)) return;

    // ORIGIN TRACKING PROBE + LEVER O A/B DRIVER.
    //
    // Cost is bounded by construction: this hook runs ~40,000 times a second,
    // so the body below is entered once every 4096 calls (~10/s) and does two
    // pointer reads and one 12-byte read. Everything else is arithmetic on
    // values already in hand.
    //
    // What it measures: how far CameraSrc+0x14b0 sits from the position solved
    // from the matrix, over many frames. If that distance stays within a frame
    // of camera motion and collapses toward zero when the camera is parked, the
    // field tracks the camera and the offset is right. If it wanders without
    // bound, 0x14b0 is something else that happened to be nearby once.
    if ((origin_scan_views_seen.load(std::memory_order_relaxed) & 0xFFF) == 0) {
        uintptr_t addrs[7]{};
        if (collect_origin_sites(addrs, 7) > 0) {
            float stored[3]{};
            if (safe_read_bytes(addrs[0], stored, sizeof(stored))) {
                const double dx = double(stored[0]) - p[0];
                const double dy = double(stored[1]) - p[1];
                const double dz = double(stored[2]) - p[2];
                const float d = static_cast<float>(std::sqrt(dx * dx + dy * dy + dz * dz));
                // The same distance, but against a position solved from rows
                // 0,1,3 instead of 0,1,2. If the constant 3.0 is the projection
                // near-plane term leaking out of row 2, this one collapses to
                // ~0 and CameraSrc+0x14b0 is confirmed as the camera position
                // outright. If both sit at 3.0, the offset is real and belongs
                // to the field, which is a different fact and changes what
                // Lever O should be writing.
                static constexpr int kRows013[3]{0, 1, 3};
                float p13[3]{};
                float resid13 = 0.0f;
                if (solve_view_origin_rows(vp, kRows013, p13, &resid13)) {
                    const double ex = double(stored[0]) - p13[0];
                    const double ey = double(stored[1]) - p13[1];
                    const double ez = double(stored[2]) - p13[2];
                    origin_track_last013.store(
                        static_cast<float>(std::sqrt(ex * ex + ey * ey + ez * ez)),
                        std::memory_order_relaxed);
                    origin_track_resid013.store(resid13, std::memory_order_relaxed);
                }
                origin_track_samples.fetch_add(1, std::memory_order_relaxed);
                origin_track_last.store(d, std::memory_order_relaxed);
                float prev = origin_track_max.load(std::memory_order_relaxed);
                while (d > prev &&
                       !origin_track_max.compare_exchange_weak(prev, d,
                                                               std::memory_order_relaxed)) {}
                // A running max never comes back down, so a single transient
                // would latch it forever. The min is carried alongside, and
                // the parked-camera case is what the min is there to catch.
                float pmin = origin_track_min.load(std::memory_order_relaxed);
                while (d < pmin &&
                       !origin_track_min.compare_exchange_weak(pmin, d,
                                                               std::memory_order_relaxed)) {}
            }
        }

        // AUTOMATIC A/B. No hotkey: every SHIFT+F-key is already bound, and
        // overloading one is what produced two of mode 4's arming failures.
        // The arm state is logged at every transition and on every health
        // line, so the arm is read from the instrument, never from a press.
        const uint64_t now_ms = GetTickCount64();
        static uint64_t ab_start_ms = 0;
        if (ab_start_ms == 0) ab_start_ms = now_ms;
        // DISARMED 2026-08-23 after the 16:18 run. Lever O engaged cleanly
        // (applied=9009, restored=9009, A/B tracking the arm state exactly) and
        // produced no visible change, so it is a settled negative rather than a
        // pending experiment. Leaving the A/B cycling would put a standing
        // intervention underneath whatever diagnostic comes next - the failure
        // recorded in disarm-interventions-before-new-diagnostics. The lever
        // and its counters stay in place; only the driver is off. Flip this to
        // true to re-run the A/B unchanged.
        constexpr bool kLeverOAutoAB = false;
        // 20 s of untouched warm-up first, so the tracking numbers above are
        // measured against DOOM's own value before anything writes to it.
        if (kLeverOAutoAB && now_ms - ab_start_ms > 20000) {
            const bool want = (((now_ms - ab_start_ms - 20000) / 10000) & 1ULL) != 0;
            if (want != lever_o_enabled.exchange(want, std::memory_order_relaxed)) {
                log::warn(std::format(
                    "LeverO A/B -> {} (10 s periods). Writes CameraSrc+{:#x} and the six "
                    "destination copies, EYE 1 ONLY. applied={} restored={} noSites={} | "
                    "originTrack samples={} last={:.3f} min={:.3f} max={:.3f}",
                    want ? "ON" : "OFF", kCameraSrcOriginOffset,
                    lever_o_applied.load(std::memory_order_relaxed),
                    lever_o_restored.load(std::memory_order_relaxed),
                    lever_o_no_clean.load(std::memory_order_relaxed),
                    origin_track_samples.load(std::memory_order_relaxed),
                    origin_track_last.load(std::memory_order_relaxed),
                    origin_track_min.load(std::memory_order_relaxed),
                    origin_track_max.load(std::memory_order_relaxed)));
            }
        }
    }

    // STATIONARY GATE (2026-08-23, second attempt).
    //
    // The first attempt spaced its three fires by a count of world views. That
    // count advances ~10,000 times a SECOND - the run logged viewsSeen=1036667
    // over 100 s - so 120/900/2700 all elapsed inside 67 ms and the three
    // "samples" were one sample with one camera position. Wall clock now, not
    // an event count.
    //
    // More importantly, the value searched for is solved from the matrix at
    // THIS instant, while any stored copy may be a frame behind. At DOOM
    // movement speed one frame is several world units, which on its own
    // exceeds the match tolerance and manufactures a null. Firing only while
    // the camera is parked removes that confound entirely, so a miss is a real
    // miss rather than a timing artefact.
    static float last_p[3]{};
    static bool last_valid = false;
    static uint32_t still_streak = 0;
    static uint64_t last_fire_ms = 0;
    static uint64_t first_seen_ms = 0;
    const uint64_t now = GetTickCount64();
    if (first_seen_ms == 0) first_seen_ms = now;
    if (last_valid) {
        const float dx = p[0] - last_p[0], dy = p[1] - last_p[1],
                    dz = p[2] - last_p[2];
        still_streak = (dx * dx + dy * dy + dz * dz) < 0.0025f ? still_streak + 1 : 0;
    }
    last_p[0] = p[0]; last_p[1] = p[1]; last_p[2] = p[2];
    last_valid = true;
    origin_scan_still_streak.store(still_streak, std::memory_order_relaxed);

    const bool parked = still_streak >= 2000;   // ~0.2 s of world views
    // FALLBACK so the run cannot come back empty if the owner never stands
    // still. Labelled, because a moving sample cannot support the same
    // conclusion as a parked one.
    const bool timed_out = (now - first_seen_ms) > 45000;
    if (!parked && !timed_out) return;
    if (last_fire_ms != 0 && (now - last_fire_ms) < 8000) return;
    last_fire_ms = now;

    log::warn(std::format(
        "ViewOriginScan fire {} of 3. camera={} stillStreak={} solveResidual={:.5f} "
        "(residual is how far the solved position misses reproducing the matrix "
        "own translation terms; large means the search target itself is wrong).",
        origin_scan_fires.fetch_add(1, std::memory_order_relaxed) + 1,
        parked ? "PARKED" : "MOVING - weaker evidence, timed fallback",
        still_streak, residual));
    log_view_origin_scan(view, vp, p);
}

// ============ FRAME TIME BUDGET - CORE (2026-08-25) ========================
// Declared HERE, ahead of the first DOOM-side hook at hook_doom_render_view,
// because those hooks need it and they precede the Vulkan hooks in this file.
using fb_clock = std::chrono::steady_clock;
inline uint64_t fb_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        fb_clock::now().time_since_epoch()).count());
}
std::atomic_uint64_t fb_clock_reads{0};
// Forces the FrameBudget line to emit on the next present instead of waiting
// out its 1-second window. Set from cutscene_dense_burst, which BOTH the UP
// marker and the cadence detector's edges already call - so the exact moment
// the owner marks, and every cadence transition, gets its own budget sample
// with a clean window boundary either side. Without this a 1-second average
// smears the transition being investigated across the frames before it.
std::atomic_bool fb_force_emit{false};
// Guards against our own second call re-entering the frame root and recursing.
// Moved up from its old position so the budget can split every measurement by
// whether it happened inside the injected render.
thread_local bool inside_frame_root_double = false;

// ---- DOOM-SIDE HOOK OVERHEAD --------------------------------------------
// 19 installed DOOM hooks. Timing their FULL span would be meaningless - it
// would swallow DOOM's own rendering - so each records span AND the time
// spent inside its trampoline, and the reported figure is OVERHEAD = span -
// trampoline, i.e. our code only. hook_job_dispatch runs per JOB and was
// entirely unmeasured until now.
enum FbDoomId {
    FB_D_RENDER_GATHER_PREPARE,
    FB_D_RENDER_GATHER_WORLD_SURFACES_PREPARE,
    FB_D_RENDER_CULL,
    FB_D_RENDER_WALK_BSP,
    FB_D_RENDER_GATHER_FINISH,
    FB_D_RENDER_SORT,
    FB_D_RENDER_GATHER_MODELS_PREPARE,
    FB_D_RENDER_GATHER_DECALS_PREPARE,
    FB_D_RENDER_GATHER_LIGHTS_PREPARE,
    FB_D_RENDER_GATHER_ADD_ALWAYS,
    FB_D_RENDER_GUI_MODELS,
    FB_D_SETUP_VIEW_GUI,
    FB_D_SETUP_VIEW_WORLD,
    FB_D_BUILD_VIEW_MATRICES,
    FB_D_CULL_LIGHTS,
    FB_D_RENDER_DEPTH,
    FB_D_FRAME_ROOT,
    FB_D_JOB_DISPATCH,
    FB_D_DOOM_RENDER_VIEW,
    FB_D_DOOM_RENDER_OPAQUE,
    FB_D_FINAL_STAGE,
    FB_D_COUNT
};
const char* const kFbDoomName[FB_D_COUNT] = {
    "render_gather_prepare",
    "render_gather_world_surfaces_prepare",
    "render_cull",
    "render_walk_bsp",
    "render_gather_finish",
    "render_sort",
    "render_gather_models_prepare",
    "render_gather_decals_prepare",
    "render_gather_lights_prepare",
    "render_gather_add_always",
    "render_gui_models",
    "setup_view_gui",
    "setup_view_world",
    "build_view_matrices",
    "cull_lights",
    "render_depth",
    "frame_root",
    "job_dispatch",
    "doom_render_view",
    "doom_render_opaque",
    "final_stage",
};
std::atomic_uint64_t fb_doom_span_ns[FB_D_COUNT];
std::atomic_uint64_t fb_doom_tramp_ns[FB_D_COUNT];
std::atomic_uint64_t fb_doom_span_dbl_ns[FB_D_COUNT];
std::atomic_uint64_t fb_doom_tramp_dbl_ns[FB_D_COUNT];
std::atomic_uint64_t fb_doom_calls[FB_D_COUNT];

inline uint64_t fb_doom_enter(int id) {
    if (!kharvox::native::cpu::current.detailed) return 0;
    fb_doom_calls[id].fetch_add(1, std::memory_order_relaxed);
    fb_clock_reads.fetch_add(2, std::memory_order_relaxed);
    return fb_ns();
}
inline void fb_doom_exit(int id, uint64_t t0) {
    if (!t0) return;
    (inside_frame_root_double ? fb_doom_span_dbl_ns[id] : fb_doom_span_ns[id])
        .fetch_add(fb_ns() - t0, std::memory_order_relaxed);
}
inline void fb_tramp_exit(int id, uint64_t t0) {
    if (!t0) return;
    (inside_frame_root_double ? fb_doom_tramp_dbl_ns[id] : fb_doom_tramp_ns[id])
        .fetch_add(fb_ns() - t0, std::memory_order_relaxed);
}
struct FbDoom {
    kharvox::native::trace::Scope tracing;
    int id;
    uint64_t t0;
    explicit FbDoom(int i) : tracing(kFbDoomName[i],uint64_t(i)),id(i), t0(fb_doom_enter(i)) {}
    ~FbDoom() { fb_doom_exit(id, t0); }
};
// Wraps a trampoline call so DOOM's own work is subtracted back out.
struct FbTramp {
    kharvox::native::trace::Scope tracing;
    int id;
    uint64_t t0;
    explicit FbTramp(int i) : tracing(kFbDoomName[i],uint64_t(i)),id(i), t0(kharvox::native::cpu::current.detailed ? fb_ns() : 0) {
        if (t0) fb_clock_reads.fetch_add(2, std::memory_order_relaxed);
    }
    ~FbTramp() { fb_tramp_exit(id, t0); }
};
// ============ END FRAME TIME BUDGET - CORE =================================
void __fastcall hook_doom_render_view(void* renderer, void* draw_command) {
    const uint64_t fbd_t0 = fb_doom_enter(FB_D_DOOM_RENDER_VIEW);  // manual: this frame uses __try (C2712)
    // DOOM+0x1A37910 receives the draw command in RDX. Its first pointer is the
    // render-view object later used throughout this exact camera bind path.
    // Capture addresses only; never modify DOOM-owned state here.
    uintptr_t view = 0;
    if (draw_command) safe_read_pointer(draw_command, view);
    if (inside_render_opaque) {
        ++bit31_probe_total_draws;
        uint32_t packed = 0;
        if (safe_read_bytes(reinterpret_cast<uintptr_t>(draw_command) + 0x2c, &packed, sizeof(packed)) &&
            (packed & 0x80000000u) != 0) {
            ++bit31_probe_flagged_draws;
        }
        // PassDrawCountProbe - see current_render_opaque_pass_slot's
        // declaration above.
        if (current_render_opaque_pass_slot == 0) ++pass1_draw_count;
        else if (current_render_opaque_pass_slot == 1) ++pass2_draw_count;
        else ++other_pass_draw_count;
    }
    const uintptr_t previous_view = active_render_view;
    active_render_view = view;
    maybe_fire_origin_scan(view);
    std::array<float, 16> original{};
    float* modified = nullptr;
    apply_temporary_world_yaw(view, original, modified);
    const uint64_t fbt_t0 = kharvox::native::cpu::current.detailed ? fb_ns() : 0;
    real_doom_render_view(renderer, draw_command);
    fb_tramp_exit(FB_D_DOOM_RENDER_VIEW, fbt_t0);
    if (modified) {
        __try { std::memcpy(modified, original.data(), 64); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    // Single fall-through exit; no return can bypass this.
    fb_doom_exit(FB_D_DOOM_RENDER_VIEW, fbd_t0);
    active_render_view = previous_view;
}

void __fastcall hook_doom_render_surface(void* renderer, void* render_view,
    void* surface, uintptr_t flags) {
    // DOOM+0x1A367E0 is the sibling surface path seen in the latest trace.
    // RDX is used directly as its render-view structure.
    const uintptr_t previous_view = active_render_view;
    active_render_view = reinterpret_cast<uintptr_t>(render_view);
    std::array<float, 16> original{};
    float* modified = nullptr;
    apply_temporary_world_yaw(active_render_view, original, modified);
    real_doom_render_surface(renderer, render_view, surface, flags);
    if (modified) {
        __try { std::memcpy(modified, original.data(), 64); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    active_render_view = previous_view;
}

std::atomic_uint32_t render_opaque_sample_count{0};
constexpr uint32_t kMaxRenderOpaqueSamples = 300;

// Re-entrancy smoke test (Phase 2.4): off by default, armed with F7. When
// enabled, hook_doom_render_opaque calls the real job body a SECOND time
// immediately after the first, with no state changed between calls, purely
// to observe whether DOOM's "RenderOpaque" job tolerates running twice per
// scheduled invocation. Self-limits to kMaxRenderOpaqueDoubleCalls attempts
// and disables itself immediately on any caught exception - see
// docs/RE-NOTES.md open question 1 before ever extending this to actually
// changing camera/target state between the two calls.
std::atomic_bool render_opaque_double_call_enabled{false};
std::atomic_uint32_t render_opaque_double_call_count{0};
constexpr uint32_t kMaxRenderOpaqueDoubleCalls = 60;

// Phase 2.4 camera-divergence groundwork, Stage 1 (F2). Does NOT touch
// DOOM's shared render-view object at all - see docs/FORBIDDEN-BOUNDARIES.md
// item 2, a standing "Never re-enable" after a real prior failure writing
// directly into renderView+0xC44/+0xC84's shared bytes, which are read
// asynchronously by multiple other render jobs during the same frame.
// Instead: clone the FULL render-view object's bytes into memory WE own,
// then temporarily redirect the render-view SLOT pointer (the value at
// *(arg1+0x40), which itself points to the current render-view object) to
// point at our clone instead, only for the scoped duration of our own
// extra RenderOpaque call, restoring it immediately after - we never
// write a single byte into the shared object itself, only into our own
// clone and into the slot's own pointer value.
//
// Stage 1 (this): the clone is a byte-identical copy - no camera change
// at all. This isolates "does redirecting the pointer cause any
// instability" (which would mean something else reads this slot
// concurrently, and the whole approach is unsafe) from "does an actual
// camera change cause instability" (Stage 2, a separate question, not
// attempted until Stage 1 is confirmed clean). Identical content makes
// Stage 1 safe by construction from the "other readers see something
// wrong" angle: a complete, faithful copy is indistinguishable from the
// original to any reader, at any time, regardless of the swap's timing -
// the only things that can go wrong are (a) the clone being incomplete
// (mitigated by over-copying generously, see the buffer size below) or
// (b) the restore write itself failing, which is treated as a critical
// event below (attempts stop immediately, loudly logged).
std::atomic_bool render_view_clone_test_enabled{false};
std::atomic_uint32_t render_view_clone_test_count{0};
constexpr uint32_t kMaxRenderViewCloneTestAttempts = 60;
// Reused every attempt, never freed - avoids any dangling-pointer window
// between attempts. Generously oversized vs. the largest confirmed offset
// (+0xC84, a 64-byte matrix ending at 0xCC4/3268 bytes) since the
// render-view object's true full size is still unconfirmed (see
// docs/RE-NOTES.md "Full render-view object size/lifetime - still
// unknown") - better to over-copy than risk missing a field something
// else depends on.
alignas(16) uint8_t render_view_clone_buffer[4096];

// TrueStereo (F1) - Stage 2: the first real per-eye divergence attempt.
// Only proceed with this after Stage 1 (F2) has been confirmed clean live
// - see docs/RE-NOTES.md. Same clone-and-swap mechanism as Stage 1
// (never writes into DOOM's shared render-view object), but now the
// clone's copy of the camera matrix pair is actually rotated (toe-in
// yaw) before the swap, and the extra call is ALSO redirected into our
// offscreen target (so eye 1 gets a genuinely different, visible image,
// not just a discarded duplicate draw into DOOM's real frame).
// KEY OWNERSHIP (2026-08-19). One handler per key, no exceptions.
//
// F11 and F12 each had three claimants - this file, openxr_context.cpp, and
// DOOM itself - all using GetAsyncKeyState's consume-on-read &1 bit from
// different threads, so presses landed at random. The log shows "Look
// injection" firing 103 times against this file's F11 handler's 7. Modifiers
// do NOT fix it: the rival handlers do not check modifiers, so CTRL+F11 still
// toggled head-tracking.
//
// Setting this false retires the superseded extra-call stereo modes
// (TrueStereo on F1, RenderViewClone on F2) and RELEASES their keys, which the
// live probes then take as sole owners. The bodies are kept, dead, so the
// mechanisms can be revived - but a revived one must claim a key nothing else
// uses, not share.
constexpr bool kLegacyStereoModesEnabled = false;
std::atomic_bool true_stereo_enabled{false};
std::atomic_uint32_t true_stereo_attempt_count{0};
// First live test (60-attempt cap, ~1 second) came back clean - zero
// exceptions/crashes, eye 1 reported as looking "perfect." Extended here
// the same way StereoPreview's cap was extended after ITS first clean
// test: now something to actually evaluate for depth/comfort over time,
// not a blink-and-miss-it flash.
constexpr uint32_t kMaxTrueStereoAttempts = 15000;
// Magnitude reused directly from AlternateEye's already-validated toe-in
// bias (confirmed to produce genuine stereo depth perception when applied
// to real content) rather than inventing a new untested value. Sign is a
// best-guess carried from AlternateEye's own eye-1 convention - the two
// mechanisms (mouse-delta yaw injection vs. direct matrix rotation)
// aren't guaranteed to share the same sign convention, so if depth feels
// inverted/uncomfortable rather than absent, flipping this sign is the
// first thing to try, not evidence the whole approach is wrong.
//
// TEMPORARY DIAGNOSTIC VALUE, not the real target: first live test at
// 0.025 rad (1.4deg, AlternateEye's proven magnitude) showed no visible
// difference between eyes at all - mechanically confirmed applying
// correctly every time (7230/7230 swapped=true, restored=true, zero
// exceptions), so either 1.4deg is too subtle to notice comparing two
// separately-rendered images side by side (not a controlled A/B), or the
// rotation axis/sign from apply_yaw_rotation() isn't doing what's
// intended. Cranked way up to get an unambiguous signal either way - if
// even this shows nothing, the mechanism itself needs investigating, not
// just the magnitude. Dial back down once any effect is confirmed.
//
// TEMPORARILY 0.0 (2026-08-09): three different TrueStereoGpuWrite fixes
// (bounds check, consistent current/previous, no-recompound guard)
// produced VISUALLY IDENTICAL smearing/missing-geometry across all three
// live tests - strong evidence the corruption isn't caused by what value
// we rotate to, so the rotation magnitude itself isn't the variable to
// keep tuning. Zero here makes the write a pure read-back-and-write-the-
// same-bytes no-op: if the corruption persists at 0 deviation, it's a
// write-timing/sync issue independent of the matrix's content; if it
// disappears, this IS the right matrix and the bug is specifically in how
// non-zero rotation is computed/applied.
//
// RESULT (2026-08-09): 0.0 rad came back CLEAN; 0.35 rad and 0.025 rad
// (AlternateEye's proven comfortable magnitude) produced IDENTICAL severe
// corruption - not proportional to angle at all. That rules out a
// proportional-mismatch theory (e.g. stale-visibility frustum slightly
// off), which would degrade gracefully at small angles, and points at a
// binary/threshold cause instead - most likely the second-64-byte write
// (see the write site below) clobbering an unrelated bounding/clip field.
// Back to 0.35 for an unmistakable visual signal now that the second-half
// write has been removed - either this comes back clean or it doesn't,
// no need to squint at a subtle angle to tell.
constexpr float kTrueStereoToeInRadians = 0.35f;

// TrueStereoGpuWrite switched from rotation to TRANSLATION (2026-08-09,
// user direction): real binocular stereo comes from shifting the eye
// POSITION sideways by the interpupillary distance, not rotating the view
// direction - toe-in rotation was never how this was supposed to work long
// term, it was only ever a quick-to-validate stand-in reusing AlternateEye's
// proven magnitude. See apply_translation() below.
//
// RESULT at 64.0 units (2026-08-09): reached the renderer (write confirmed
// every attempt) but produced the same severe smearing/missing-geometry
// corruption already seen with rotation - initially read as "translation
// hits the same stale-visibility wall as rotation." Reconsidered after
// WalkBSPCullSourceProbe (see hook_render_walk_bsp) live-sampled the actual
// per-node cull-reject reference the BSP walk uses: it's a per-leaf
// position+bounds pair, held constant across ~7s of head movement and only
// jumping when crossing into a new cell, with each cell's bounds forming a
// tight ~32-unit box. 64 units is bigger than an entire leaf - large enough
// to plausibly walk the write into a different leaf's cull data than what
// gather actually ran against, which would produce exactly this kind of
// corruption independent of whether translation is otherwise the right fix.
// id Software's own dormant stereoRender_separation cvar in this binary
// defaults to 1.5 ("world units from center to eyes" - see docs/RE-NOTES.md)
// - tiny next to a 32-unit leaf, unlikely to ever cross a cell boundary.
// Dialing down to that real-world-scale reference value to test whether the
// corruption was specifically a too-large-a-jump artifact, not a
// fundamental translation-vs-stale-visibility problem.
//
// RESULT at 1.5 units (2026-08-09): visibly less smeary than 64 units, but
// not clean - a real, graded response to magnitude, unlike rotation (which
// showed IDENTICAL severe corruption at both 20deg and 1.4deg - flat, not
// proportional). That itself is informative: this really does look like a
// "how much does eye 1's visible set differ from what gather selected"
// problem that scales with distance, not rotation's separate binary-
// threshold bug. Going smaller again to see whether the trend continues
// toward clean (good - means a small-enough real IPD value works) or
// plateaus at a nonzero floor (would mean a separate, magnitude-independent
// corruption source, e.g. the still-untouched "second 64 bytes" possible
// bounding/clip volume).
// Was a constexpr 0.5f; became runtime-variable 2026-08-09 for Phase 0's
// single-run magnitude sweep (deleted plan; kept as a same-run sweep harness). Default
// preserves the pre-Phase-0 behaviour of a fixed 0.5-unit offset for any
// TrueStereo test outside the sweep. Written from the main/present thread
// (F1 arm and the post-dump sweep advance) and read from DOOM's worker
// thread in hook_cmd_bind_descriptor_sets - std::atomic<float> so the read
// gets a coherent value without a mutex on the hot path.
std::atomic<float> g_true_stereo_translation_offset{0.5f};

// Phase 0 sweep state (deleted plan; kept as a same-run sweep harness). Owned entirely
// by the main/present thread - the F1 arm handler starts a sweep; the
// per-slot-1 dump handler in hook_queue_present_khr advances it after each
// dump completes. The atomic offset above is the only cross-thread
// interface; sweep bookkeeping never touches the worker thread.
constexpr std::array<float, 4> kPhase0SweepOffsets{0.0f, 0.1f, 0.5f, 1.5f};
bool g_phase0_sweep_active = false;
size_t g_phase0_sweep_index = 0;

// TrueStereoGpuProbe: read-only diagnostic riding along on TrueStereo (F1)
// live tests, no new hotkey needed. Root-caused (see docs/RE-NOTES.md,
// "RenderViewClone Stage 2 / TrueStereo") that the clone's rotated camera
// bytes genuinely change but the render is unaffected - real evidence
// DOOM's GPU-consumed camera data is uploaded to a uniform buffer once per
// frame, upstream of RenderOpaque, not read fresh from the CPU render-view
// object. This probe answers the next open question empirically: does the
// real call's camera-shaped dynamic-uniform binding (buffer + dynamic
// offset) match the extra call's, in the SAME frame? If they match, both
// eyes are reading one shared slot (write-then-restore there would hit the
// exact same async-timing hazard FORBIDDEN-BOUNDARIES.md item 2 already
// burned us on). If they differ (e.g. a ring-allocated slot), a scoped
// write into just the extra call's own slot, sandwiched between the two
// synchronous RenderOpaque calls in one hook_doom_render_opaque invocation,
// would be safe by construction.
//
// First live test (2026-08-09): a single real RenderOpaque call issues far
// more qualifying dynamic-uniform binds per frame than expected (hundreds+
// - DOOM rebinds per draw command, not once per pass) - a shared 400-sample
// global cap was exhausted entirely inside frame 0's REAL call alone,
// before the extra call ever ran, yielding zero extra-call data. Fixed two
// ways: (1) filtered to binding==0 && looks_like_world_camera_matrix(),
// the exact signature F10 already validated as meaningful (cut the same
// frame's matches from 400 to 93, all of which were already binding==0);
// (2) separate per-callKind sample budgets so the extra call is guaranteed
// its own quota regardless of how much the real call logs. Bounded on two
// independent axes (frame count AND per-callKind logged-line budget) -
// never runs unconditionally in the render hot path, per
// FORBIDDEN-BOUNDARIES.md item 11.
constexpr uint32_t kMaxTrueStereoGpuProbeFrames = 3;
std::atomic_uint32_t true_stereo_gpu_probe_real_samples{0};
std::atomic_uint32_t true_stereo_gpu_probe_extra_samples{0};
constexpr uint32_t kMaxTrueStereoGpuProbeSamplesPerCallKind = 200;

// TrueStereoMatrixCapture: pure read-only, whole-session (up to
// kMaxTrueStereoAttempts frames, ~4 min), one line per frame - meant to be
// run while deliberately turning your head (left/right/circular) so the
// logged matrix can be correlated against known motion to actually
// determine this record's layout (which row is right/up/forward, what
// index [3] tracks) instead of guessing at another write. REAL call only
// (the extra call is redundant here while kTrueStereoToeInRadians is 0).
// Logs all 4 rows of the "current" half plus, when the record has room,
// the second 64 bytes too (never confirmed to be a "previous" matrix for
// this GPU record specifically, only for the CPU render-view object - this
// capture is partly how we'd confirm or rule that out). Deduplicated to
// one line per frame (the value doesn't change within a frame - confirmed
// by the earlier probe) to keep a multi-minute capture's log volume sane.
std::atomic_uint32_t true_stereo_matrix_capture_last_frame{UINT32_MAX};

// TrueStereoGpuWrite: the actual fix, informed by the probe above. Second
// live test proved two things together: the extra call's binding==0
// camera-shaped record sits at a DIFFERENT (buffer, dynamicOffset) than
// the real call's in the same frame (a private ring-allocated slot, not a
// shared one), and its bytes were still the unrotated original - nothing
// was writing rotation into it. So: write the toe-in-rotated matrix
// directly into that GPU-mapped host pointer, for every camera-shaped
// binding==0 record seen during the extra call, for the whole TrueStereo
// session (not just the probe's diagnostic window) - never into DOOM's
// shared render-view object, never into the real call's slot, and no
// restore needed since the probe's evidence says nothing else reads this
// specific slot. Logging capped, but write failures always log regardless
// of the cap - a failed write here is safety-relevant the same way a
// failed clone-pointer restore is elsewhere in this file.
std::atomic_uint32_t true_stereo_gpu_write_log_count{0};
constexpr uint32_t kMaxTrueStereoGpuWriteLogLines = 20;
// StereoDualEyeDump Stage D's own GPU write log budget - separate counter
// from TrueStereoGpuWrite's (mutually exclusive mechanisms, but kept
// distinct rather than shared so each gets its own visible sample window).
std::atomic_uint32_t stereo_dual_eye_gpu_write_log_count{0};
constexpr uint32_t kMaxStereoDualEyeGpuWriteLogLines = 20;
// Avenue 1 follow-up: classify every camera-shaped dynamic-uniform record
// encountered by the already-safe no-camera-write INSERT control.  This
// gives a precise (pipeline, set, offset, range) identity before another
// record is ever modified.
std::atomic_uint32_t stereo_dual_eye_candidate_log_count{0};
constexpr uint32_t kMaxStereoDualEyeCandidateLogLines = 24;

// UpstreamPipelineReinvoke (F9, repurposed - see its old meaning's comment
// a few lines down, already dead code before this session started) - Stage
// A of the "move further up the stack" plan. Every TrueStereoGpuWrite
// variant tried produced identical severe corruption at any nonzero angle,
// completely independent of exactly what was written - strong evidence
// RenderOpaque's draw-command list (built once per real frame by an
// upstream Gather/Cull/WalkBSP/Sort pipeline, confirmed via radare2 static
// analysis + the AppendDrawCommandProbe/UpstreamPipelineProbe live
// diagnostics) is what's actually stale, not the camera transform. Fix
// needs the WHOLE upstream chain re-run for eye 1, not just RenderOpaque
// again. AppendDrawCommandProbe confirmed the append helper writes
// directly into the SAME 16-slot, 0x38008-stride table RenderOpaque reads
// (three observed pass-table addresses were exactly 2 and 11 slots apart),
// and that each slot's +0x38000 count resets to 0 every real frame
// (wherever that happens, it's upstream of our capture point) - so a clean
// re-run needs US to zero it ourselves before re-invoking gather, then
// restore the real frame's original counts/entries afterward so nothing
// later in the real frame sees our detour.
//
// Stage A (this): re-run the WHOLE chain - RenderGatherPrepare ->
// RenderGatherWorldSurfacesPrepare -> RenderCull -> RenderWalkBSP ->
// RenderGatherFinish -> RenderSort -> RenderOpaque - using each job's own
// arg1 already captured this same frame by UpstreamPipelineProbe, with
// ZERO camera divergence (byte-identical render-view, no clone/rotation at
// all) and NO target redirection (draws into DOOM's real framebuffer, same
// "isolate what's risky" discipline as F7's original RenderOpaque-only
// re-entrancy smoke test). Only tests whether save/reset/re-invoke/restore
// around the whole pipeline is safe on its own - camera divergence and
// target redirection are separate, later stages, only attempted once this
// is confirmed clean, mirroring Stage 1/Stage 2's validation order.
std::atomic_bool upstream_reinvoke_enabled{false};
std::atomic_uint32_t upstream_reinvoke_attempt_count{0};
// Deliberately much tighter than TrueStereo's 15000 or even F7's 60 - this
// re-invokes TEN previously-never-directly-called functions per attempt,
// each with real internal state and object-list iteration, genuinely new
// and unvalidated territory. Widen only after this cap comes back clean.
constexpr uint32_t kMaxUpstreamReinvokeAttempts = 20;
// Live test (2026-08-09): two different, well-targeted content fixes
// (viewport-rect save/restore, then adding all 4 missing sibling Gather
// jobs) both engaged perfectly (haveAllContexts/saveOk/resetOk/restoreOk
// all true, zero exceptions) and had ZERO effect on the shrink - same
// smaller-square-with-black-borders symptom, unchanged. That rules out a
// data/content bug in our mechanism. Remaining variable not yet tested:
// all 20 attempts fire back-to-back in ~250ms, each duplicating the
// entire visibility pipeline + RenderOpaque - a massive frame-time spike
// that could plausibly trigger the VR runtime's own reprojection/motion-
// smoothing fallback (external to DOOM entirely, unfixable via DOOM
// memory). Spreading attempts out tests this directly: only actually run
// the detour every kUpstreamReinvokeFrameGap real frames instead of every
// one, so the SAME 20 attempts span several seconds instead of a quarter
// second - no change to what each attempt does, only when.
std::atomic_uint32_t upstream_reinvoke_frame_counter{0};
constexpr uint32_t kUpstreamReinvokeFrameGap = 45;
// One slot's used region: array (0x30000..0x38000) + count (0x38000..
// 0x38004) rounds up to the same 0x8008-byte chunk AppendDrawCommandProbe
// showed slots are spaced by. Saving/restoring this whole chunk per slot
// (not just the count) means the restore is exact regardless of how many
// entries this frame's real gather wrote - no need to know the count
// beforehand to size the copy.
constexpr size_t kPassSlotSaveSize = 0x8008;
constexpr int kPassSlotCount = 16;
constexpr size_t kPassSlotStride = 0x38008;
alignas(16) uint8_t pass_table_save_buffer[kPassSlotCount][kPassSlotSaveSize];

// Live test (2026-08-09): Stage A ran clean by every metric this file logs
// (saveOk/resetOk/restoreOk all true, no exceptions, no crash across
// several 20-attempt bursts) - but visually, the window briefly shrank to
// a smaller square each time, recovering after roughly a second. Root-
// caused via radare2: RenderOpaque's viewport-rect selection (arg1+0x18,
// offsets +0x14e8/+0x14ec/+0x14f0/+0x14f4 form a rect, falls back to a
// second candidate rect at +0x14d8 if the first one is degenerate - x1<=x0
// or y1<=y0) reads state that RenderGatherWorldSurfacesPrepare itself
// writes (confirmed: the exact same +0x14c8/+0x14e8/+0x14d8 offset pattern
// appears six times in a row inside that job's own address range, an
// accumulation loop across sub-categories - almost certainly a bounding
// rect built up over the visible geometry gathered this pass). Re-running
// that job for our extra call - without saving/restoring this small
// region the way the pass-table already is - very plausibly leaves it
// transiently degenerate, tripping RenderOpaque's fallback to the smaller
// rect B. Fixed below: save/restore this ~48-byte region (generously
// rounded up, matching the project's "over-copy rather than risk missing
// a field" convention) around the SAME re-invoke window as the pass-table.
// Not yet live-tested - written and built while the user was away from
// the headset; next session's first F9 test validates whether this
// actually eliminates the shrink.
constexpr uintptr_t kViewportRectOffset = 0x14c8;
constexpr size_t kViewportRectSaveSize = 0x100;
alignas(16) uint8_t viewport_rect_save_buffer[kViewportRectSaveSize];

// ScopedPipelineReinvoke (F4, 2026-08-11) - the "not yet tried" idea flagged
// in docs/HANDOFF-NEXT-SESSION.md's Stage A writeup: Stage A (F9) re-runs
// ALL 10 jobs and is confirmed too expensive for real-time (~562ms/attempt
// flash, matching the spread-out attempt cadence exactly - a real per-
// attempt cost, not a logging confound). This tests whether skipping the
// six Gather*Prepare/GatherAddAlways jobs - which build the CANDIDATE
// object/surface list, plausibly camera-independent at IPD scale (a few
// world units, nowhere near the ~32-unit BSP leaf cells that would make a
// candidate actually enter/leave scope) - and re-running only the
// genuinely camera-dependent visibility stages (RenderCull, RenderWalkBSP,
// RenderGatherFinish, RenderSort) is both correct (does WalkBSP's append
// helper work at all without a fresh Prepare pass in the SAME re-invoke,
// or does it depend on transient state Prepare sets up that only survives
// within one call chain) and cheap enough for real-time. Genuinely
// untested in both respects - unlike F9, which had already validated
// "does re-invoking survive at all" before HANDOFF's cost finding. Stage
// A' here: zero camera divergence, no target redirect, draws into DOOM's
// real frame - same "isolate what's risky" discipline as F9's own first
// test, validating safety AND scope-correctness before ever attempting
// camera divergence on top. Reuses pass_table_save_buffer/
// viewport_rect_save_buffer - mutually exclusive with F9 (arm-time
// disable + extra_call_already_ran), never concurrent.
std::atomic_bool scoped_reinvoke_enabled{false};
std::atomic_uint32_t scoped_reinvoke_attempt_count{0};
constexpr uint32_t kMaxScopedReinvokeAttempts = 10;
std::atomic_uint32_t scoped_reinvoke_frame_counter{0};
constexpr uint32_t kScopedReinvokeFrameGap = 45;

// Rotates a 4x4 matrix (16 floats, row-major: row0=[0..3], row1=[4..7],
// row2=[8..11], row3=[12..15]) by angle_radians in the row0/row2 plane.
// Formula reused directly from the disabled apply_temporary_world_yaw()
// (see below) rather than re-derived - that function's rotation math was
// never itself live-tested (F9 was disabled before reaching a live test
// with this specific formula), so this IS new, unvalidated math, applied
// here only to our own clone buffer (never DOOM's shared object) where a
// wrong result can only look visually wrong, not corrupt anything.
void apply_yaw_rotation(float* matrix, float angle_radians) {
    const float c = std::cos(angle_radians);
    const float s = std::sin(angle_radians);
    const std::array<float, 4> row0{matrix[0], matrix[1], matrix[2], matrix[3]};
    const std::array<float, 4> row2{matrix[8], matrix[9], matrix[10], matrix[11]};
    // Blends all 4 components, including [3]. Live head-movement capture
    // (2026-08-09, TrueStereoMatrixCapture) confirmed [3] tracks
    // -dot(basisVector, cameraPosition) - it swings by tens of units from
    // pure rotation alone (zero actual movement), exactly what a standard
    // view matrix's translation term does when its basis rotates. That
    // means blending it via c*row0[3]+s*row2[3] is mathematically the
    // CORRECT way to update it for a basis-only rotation about a fixed
    // camera position - the earlier "only rotate 0-2" change was a
    // regression, not a fix (retested: identical corruption either way,
    // confirming the corruption was never about this formula's
    // correctness - see kTrueStereoToeInRadians's comment).
    for (int i = 0; i < 4; ++i) {
        matrix[i] = c * row0[i] + s * row2[i];
        matrix[8 + i] = -s * row0[i] + c * row2[i];
    }
}

// Physically-correct camera TRANSLATION (2026-08-09, user direction: real
// stereo shifts eye POSITION sideways, it doesn't rotate the view - toe-in
// was only ever a fast stand-in). Reuses the same row semantics
// apply_yaw_rotation() above already validated live via head-movement
// capture: row0=right, row1=forward, row2/row3=up pair, and every row's
// [3] component is -dot(rowXYZ, cameraPosition) - the standard view-matrix
// translation term. Shifting the camera by offset vector T therefore only
// requires subtracting dot(rowXYZ, T) from each row's [3] - the basis
// vectors (every row's xyz) are left completely untouched, unlike rotation,
// which touched xyz AND [3] on two rows. right_offset is along the
// UNROTATED matrix's own row0 (its actual right vector), not world X, so
// this works regardless of which way the camera is currently facing.
// CTRL+END. OFF by default - see the block inside apply_translation for the
// measurement that reopened this and for why it must be judged from a capture
// before it is judged in a headset.
// DEFAULT FLIPPED TO ON, 2026-08-26, on an owner-confirmed A/B with a positive
// control: normalised=0 before the first press, 1158 and climbing after, across
// two ON/OFF cycles, and the owner reported "some of the in room objects that
// were moving around and looking detached now look correct".
//
// THE MECHANISM, which fits the symptom exactly: unnormalised, each site
// displaced by its OWN row length - renderView 0.946x of commanded, the six
// destination copies 0.866x, Lever V 1.000x. Layers drawn from different sites
// therefore carried DIFFERENT effective separations, so they fused at
// inconsistent depths and read as detached from the room. Normalising makes
// every site displace by the commanded amount, so they agree.
//
// The standing "judge this from a capture first" caution in the key handler
// predates a live judgement existing; there is one now. CTRL+End still flips it.
std::atomic_bool translation_normalise{true};
std::atomic_uint32_t translation_normalised_count{0};
std::atomic_uint32_t translation_degenerate_count{0};

void apply_translation(float* matrix, float right_offset) {
    // NORMALISE THE RIGHT VECTOR (2026-08-22). THIS WAS A REAL BUG AND IT IS
    // MEASURED, NOT INFERRED.
    //
    // The row-0 term below is r[3] -= dot(right, right) * right_offset, so the
    // shift a matrix actually receives is scaled by the SQUARED LENGTH of its
    // own first row. That is only equal to right_offset when the first row is
    // a unit vector, and the three sites this is called on have three
    // different scales. From the view-write census, one frame at
    // 'separation 3.0 uu', offsetUsed real=-1.500 doubled=+1.500:
    //
    //   site                        m3 delta   => |right|^2   effective sep
    //   LeverV ctx+0x11F0            1.500        1.000          3.00 uu
    //   LeverA renderView+0xC44      1.343        0.895          2.69 uu
    //   LeverA dest[6] +0xC44/+0xC84 4.500        3.000          9.00 uu
    //
    // So the six sibling world-view copies at DOOM+0x6FD5FC0..FE8 were being
    // separated 3.35x more than the world around them. The owner saw exactly
    // that and described it precisely: health packs and destructible barrels
    // rendering "at a DIFFERENT amount than everything else", reading as an
    // optical illusion of the object sitting underground. Objects drawn
    // through those copies were at the wrong depth relative to the floor they
    // stand on, and no amount of adjusting the requested IPD could fix it -
    // every value was being multiplied by a different constant per site.
    //
    // Normalising makes every site shift by exactly right_offset world units,
    // which is what the requested separation has always claimed to mean.
    //
    // ⚠ THIS ALSO CHANGES THE WORLD. renderView was getting 0.895x, so a
    // requested 3.0 uu was really 2.69 uu. It is now 3.00. Expect slightly
    // MORE world separation than every previous build, and judge comfort
    // accordingly - that is a real perceptual change, not a side effect to
    // wave through.
    //
    // Degenerate rows are left alone rather than normalised to a guess: a
    // zero-length right vector means this is not the matrix layout assumed
    // here, and shifting it by an invented axis would be worse than not
    // shifting it. Counted so a silent skip cannot masquerade as a shift.
    // REVERTED 2026-08-22 18:1x. Normalising made it VISIBLY WORSE in the
    // headset, so the original form is restored unchanged.
    //
    // WHY THE REASONING WAS WRONG, recorded so it is not repeated. The census
    // reports the delta of ONE MATRIX COMPONENT, m3. I compared that delta
    // across three different matrices - 1.500, 1.343, 4.500 - and read the
    // spread as three different physical separations. It is not: m3 is in each
    // matrix's OWN units, and a matrix whose rows carry a scale produces a
    // proportionally larger component delta for the SAME world-space shift.
    // Comparing a raw component across matrices of different scale is the same
    // class of error as comparing px>200 across two tone curves.
    //
    // So |right|^2 = 3.0 on the dest copies is evidence that those matrices
    // are scaled, NOT evidence that they are shifted 3x too far - and the
    // unnormalised form may well be exactly right for them, which is what the
    // headset then said.
    //
    // The pickup/barrel depth mismatch is REAL and still unexplained. What is
    // now also known is that it cannot be settled from m3 deltas. It needs a
    // measurement in a common space - the same world point projected through
    // each matrix - or the identification of which draws actually read these
    // six slots.
    // ======================= REOPENED 2026-08-23, WITH THE MEASUREMENT THE
    // ======================= REVERT NOTE ABOVE SAYS IS REQUIRED.
    //
    // That note ends: "It needs a measurement in a common space - the same
    // world point projected through each matrix - or the identification of
    // which draws actually read these six slots." R22 is that measurement.
    //
    // R22 on doomvr_frame10201.rdc, the ceiling window, all four layers, in
    // WORLD UNITS and scale-invariantly (|t| = |delta row_r[3]| / |row_r.xyz|,
    // so no matrix's own scale enters the number - which is precisely the
    // error that produced both the original fix and its revert):
    //
    //   base geometry 738/1064   row 0   11.354 uu
    //   contributor   1483/2259  row 0   12.000 uu
    //   DIVERGENT     1791/2539  row 1   11.354 uu
    //   composite dispatch                0.000 uu  (all constants identical)
    //
    // commanded separation: 12.0.
    //
    // THE 12.000 DRAW IS NOT ONE OF OURS. There is exactly one matrix-shift
    // implementation in this file - this function - and it is unnormalised.
    // A matrix DOOM rebuilds from the genuinely-moved camera gets
    // delta = -|row.xyz| * offset, i.e. a true `offset` world units. A matrix
    // WE patch here gets delta = -|row.xyz|^2 * offset, i.e. |row.xyz| * offset
    // world units. So every site this function touches is SHORT by exactly its
    // own row length, and DOOM's own matrices are the ones that are correct:
    //
    //   renderView +0xC44   |right| 0.946  ->  0.946x commanded
    //   dest copies x6      |right| 1.732  ->  0.866x commanded
    //   Lever V ctx+0x11F0  |right| 1.000  ->  1.000x commanded (unaffected)
    //
    // Those reproduce the 2026-08-22 census numbers exactly (1.343/0.895 and
    // 4.500/3.000), so this is the same phenomenon that census saw - read in a
    // common space this time instead of from raw m3 deltas.
    //
    // WHY IT MATTERS FOR THE WINDOW. The owner reports everything meant to sit
    // BEHIND the window appearing IN FRONT of it, with each eye correct alone -
    // a pure disparity error. A baseline must be IDENTICAL for every layer;
    // depth alone is what makes disparity differ. Here the layer behind
    // (contributor, 12.000, DOOM-built) carries a LARGER baseline than the
    // layer in front of it (DIVERGENT, 11.354, ours), which inverts their
    // apparent order regardless of their depths.
    //
    // ⚠ THIS IS THE CHANGE THAT WAS REVERTED ON FEEL. It is therefore OFF by
    // default and on CTRL+END, and it must be judged OFFLINE FIRST: arm it,
    // take a capture, re-run R22, and every layer should read 12.000. Only
    // then is a headset judgement worth spending. The 2026-08-22 attempt had
    // no such prediction to check against, which is why "worse" could not be
    // separated from "more separation than you were used to" - normalising
    // RAISES effective world separation at every site above, by up to 15% on
    // the six destination copies.
    const float right[3]{matrix[0], matrix[1], matrix[2]};
    float scale = right_offset;
    if (translation_normalise.load(std::memory_order_relaxed)) {
        const float len = std::sqrt(right[0] * right[0] + right[1] * right[1] +
                                    right[2] * right[2]);
        // A degenerate row is left alone rather than normalised to a guess: a
        // zero-length right vector means this is not the layout assumed here,
        // and shifting along an invented axis is worse than not shifting.
        // Counted, so a silent skip cannot masquerade as a shift.
        if (len > 1e-6f) {
            scale = right_offset / len;
            translation_normalised_count.fetch_add(1, std::memory_order_relaxed);
        } else {
            translation_degenerate_count.fetch_add(1, std::memory_order_relaxed);
        }
    }
    const float t[3]{right[0] * scale, right[1] * scale, right[2] * scale};
    for (int row = 0; row < 4; ++row) {
        float* r = matrix + row * 4;
        r[3] -= r[0] * t[0] + r[1] * t[1] + r[2] * t[2];
    }
}

// Lever A (docs/CAMERA-LEVER-PLAN.md §3). Two arm points, one hotkey each:
//   A1 fires at the top of hook_render_gather_prepare - BEFORE the gather
//      chain reads renderView+0xC44/+0xC84.
//   A2 fires in hook_doom_render_opaque immediately before real_doom_render_
//      opaque - AFTER gather, right BEFORE the draws that consume the camera.
// Both clobber DOOM's OWN render-view object (renderView+0xC44 for current
// view matrix, +0xC84 for previous) destructively, every frame, no restore,
// no attempt cap. That is the point: prior mechanisms wrote to a private
// slot on an injected extra call and could never affect what you see;
// persistent writes to the engine's own object let DOOM's next upload run
// against our data. When the hotkey is toggled off, DOOM's next per-frame
// camera compute overwrites our clobber back to real player values, so no
// permanent state damage. FORBIDDEN-BOUNDARIES.md #2 forbids this on the
// grounds of jitter - the plan explicitly overrides that; jitter is a PASS
// (visible change is exactly the goal), not a failure.
// Forward decls - definitions live further down. Same signatures used
// across the file. Grouped here so every Lever A helper below can call
// them regardless of source order.
bool safe_read_bytes(uintptr_t address, void* dest, size_t size);
bool safe_write_bytes(uintptr_t address, const void* src, size_t size);
// Forced render resolution - defined with the Win32 surface hook far below,
// called from the present path so a late injection still gets the resize.
void apply_forced_render_resolution(const char* why);
extern std::atomic<HWND> g_doom_hwnd;
extern std::atomic_bool g_render_resize_done;
bool looks_like_world_camera_matrix(const float* f);
bool looks_like_pure_view_matrix(const float* f);
// ReadRedirect (STEP 6) - defined beside hook_cmd_bind_descriptor_sets, but
// called from the destroy hooks ~1000 lines earlier.
void invalidate_read_redirect_cache(const char* why);
// Mirror convergence - defined with the mirror state further down, used by
// hook_frame_root above it.
bool mirrors_converged();
// Defined with the mirror bookkeeping far below; declared here because the
// ReadRedirect summary reports it, and that summary sits above the definition.
extern std::atomic_uint32_t stale_set_records_cleared;
std::atomic_uint32_t set_records_retained{0};
void update_mirror_convergence(bool preparationOnly=false);
void log_world_vp_at(const char* location);
enum CamSite : int { CS_GatherPrepare, CS_GatherModels, CS_GatherDecals,
               CS_GatherLights, CS_GatherAddAlways, CS_SetupWorldEnter,
               CS_SetupWorldExit, CS_COUNT };
void sample_camera_state_at(CamSite site);

// ===== VIEWSRC (2026-08-29, Step 2 of the pre-cull camera producer thread) ==
//
// READ-ONLY probe on DOOM+0x1830B80 - the function static analysis names as
// the PRE-CULL CAMERA PRODUCER (docs/RESULT-2026-08-29-STEP1-PRECULL-CAMERA-
// PRODUCER-FOUND.md). It builds, from viewParms+0x60 (vieworg) and +0x6C
// (viewaxis), the whole matrix block the renderer then lives on:
//
//   +0x11B0 inverse projection  \  the two matrices ComputeClustersFrustums
//   +0x1230 inverse view        /  multiplies to bin the froxel light lists
//   +0x1270 proj * view         -> SetupView COPIES this into renderView+0xC44
//
// So Lever A's target is a LEAF of this object, and the cluster camera is a
// SIBLING leaf. That is the EFF12 result explained; this probe is what turns
// the static claim into a measured one before any headset run is asked for.
//
// Every counter below is incremented unconditionally at its site, including
// on every early return, so "the probe never ran" can never be confused with
// "the probe ran and declined" - the failure mode that has cost this project
// runs before. Logging is capped per frame AND in total, and gated on a
// gameplay view so the capped lines cannot all be spent on the menu.
//
// POLARITY, STATED BEFORE THE RUN:
//   callsTotal == 0                -> INSTRUMENT FAILURE (hook not installed
//                                     or address wrong), NOT a null result.
//   callsTotal > 0, vp1270 CLEAN   -> separate-copy CONFIRMED: the producer
//                                     never sees the head rotation.
//   callsTotal > 0, vp1270 rot1    -> ordering problem instead: something
//                                     already rotated this object.
//   beforeA1 == callsTotal         -> producer runs before A1, as step 1 says.
//   c44Match climbing              -> renderView+0xC44 IS a copy of +0x1270,
//                                     proven live rather than from disassembly.
std::atomic_uint64_t g_vs_calls{0};            // hook entered (every call)
std::atomic_uint64_t g_vs_null_arg{0};         // early return: viewParms == 0
std::atomic_uint64_t g_vs_read_fail{0};        // early return: axis/vp read failed
std::atomic_uint64_t g_vs_no_ref{0};           // no clean reference basis published
std::atomic_uint64_t g_vs_no_head{0};          // no head rotation available
std::atomic_uint64_t g_vs_not_gameplay{0};     // ran, but not a gameplay view
std::atomic_uint64_t g_vs_state[4]{};          // +0x1270 vs clean|rot1|rot2|other
std::atomic_uint64_t g_vs_identity_view{0};    // srcFlag +0xD8 != 0 -> view = identity
std::atomic_uint64_t g_vs_alt_scale{0};        // altScale arg was 1
std::atomic_uint64_t g_vs_before_a1{0};        // A1 had not yet run this frame
std::atomic_uint64_t g_vs_after_a1{0};         // A1 had already run this frame
std::atomic_uint64_t g_vs_axis_same_as_prev{0};// axis identical to the previous call
std::atomic_uint64_t g_vs_axis_diff_in_frame{0};
std::atomic_uint64_t g_vs_frames{0};           // frames with >= 1 call
std::atomic_uint32_t g_vs_calls_this_frame{0};
std::atomic_uint32_t g_vs_calls_last_frame{0};
std::atomic_uint32_t g_vs_a1_calls_this_frame{0};
std::atomic_uint32_t g_vs_lines{0};            // capped log lines emitted
std::atomic_uint64_t g_vs_axis_hash{0};        // last call's axis hash
std::atomic_uint64_t g_vs_axis_hash_frame0{0}; // first hash of this frame
std::atomic_uint64_t g_vs_axis_hash_prevframe{0};
std::atomic_uint64_t g_vs_frame_pipelined{0};  // frame-N first hash == frame-N-1's
// Proof of the copy relationship, measured in hook_setup_view_gui.
std::atomic_uint64_t g_vs_c44_attempts{0};
std::atomic_uint64_t g_vs_c44_match{0};
std::atomic_uint64_t g_vs_c44_mismatch{0};
constexpr uint32_t kViewSrcLinesPerFrame = 6;
constexpr uint32_t kViewSrcLinesTotal = 240;
// Object identity between the producer and the clustered pipeline. A tiny
// fixed ring of the viewParms pointers the producer built, and a matching
// census taken at ComputeClustersFrustums entry on *(jobArg+0x38). No
// allocation, no lock, fixed size - it runs on DOOM's own threads.
constexpr uint32_t kViewSrcRing = 16;
std::atomic_uintptr_t g_vs_known_parms[kViewSrcRing]{};
std::atomic_uint32_t g_vs_known_next{0};
std::atomic_uint64_t g_ccf_calls{0};
std::atomic_uint64_t g_ccf_null_arg{0};
std::atomic_uint64_t g_ccf_read_fail{0};
std::atomic_uint64_t g_ccf_same_object{0};   // *(jobArg+0x38) was built by the producer
std::atomic_uint64_t g_ccf_other_object{0};  // it was not - a second camera copy exists
std::atomic_uint64_t g_ccf_after_a1{0};
std::atomic_uint64_t g_ccf_state[4]{};       // its +0x1270 vs clean|rot1|rot2|other
std::atomic_uint32_t g_ccf_lines{0};
constexpr uint32_t kCcfLinesTotal = 40;
void viewsrc_remember_view_parms(uintptr_t vp);
// ===== PRECULL: the producer WRITE (2026-08-30, step 3) ====================
// Engagement and refusal census. Every refusal has its own reason, because
// "the write site never ran" and "it ran and declined" are different findings
// and a shared counter collapses them.
std::atomic_uint64_t g_pc_attempts{0};      // armed and reached the write block
std::atomic_uint64_t g_pc_written{0};       // axis actually written
std::atomic_uint64_t g_pc_no_head{0};       // no head rotation published
std::atomic_uint64_t g_pc_no_basis{0};      // clean basis unavailable
std::atomic_uint64_t g_pc_axis_bad{0};      // entry axis not orthonormal - refused
std::atomic_uint64_t g_pc_post_bad{0};      // our OWN output failed its POST check
std::atomic_uint64_t g_pc_double_guard{0};  // entry axis was one we had written
std::atomic_uint64_t g_pc_write_fail{0};    // safe_write_bytes said no
std::atomic_uint64_t g_pc_readback_bad{0};  // wrote, but the bytes did not stick
std::atomic_uint64_t g_pc_angle_bad{0};     // rotated by the wrong amount
std::atomic_uint64_t g_pc_published{0};     // clean basis + delta published from here
std::atomic_uint64_t g_pc_post_state[4]{};  // rebuilt +0x1270 vs clean|rot1|rot2|other
std::atomic_uint32_t g_pc_lines{0};
constexpr uint32_t kPrecullLinesTotal = 60;
// Anti-double-apply. Two of the three calls per frame land on the SAME object
// pointer (measured, step 2). Each is preceded by its own refresh copy from
// the game record at the call site, so a clean axis is what we should always
// see - but that is the call site's property, not something this write may
// assume. If an entry axis is one we produced, we refuse and count it.
constexpr uint32_t kPrecullWrittenRing = 8;
std::atomic_uint64_t g_pc_written_hash[kPrecullWrittenRing]{};
std::atomic_uint32_t g_pc_written_next{0};
// ⚠ THE ONE UNVERIFIED LINK IN THE PUBLISH PATH, MADE MEASURABLE.
// The producer publishes the clean camera from the object it builds with
// altScale==0. SetupView_gui then copies THAT object's +0x1270 into
// renderView+0xC44. Step 2 saw two distinct viewParms per frame (one built
// twice with alt=0, one with alt=1), so "the alt=0 object is the one
// SetupView uses" is an inference, not a measurement - and the whole publish
// is only correct if it holds. SetupView_gui now compares its own object
// against the pointer we published from and counts the verdict, so a wrong
// guess announces itself in the log instead of silently mislabelling the
// clean camera for a whole run.
std::atomic_uintptr_t g_pc_publish_src{0};
std::atomic_uint64_t g_pc_publish_obj_match{0};
std::atomic_uint64_t g_pc_publish_obj_mismatch{0};
bool ensure_stereo_eye_target_ready(int eye);
bool ensure_stereo_eye_depth_framebuffer_ready(int eye);
bool ensure_stereo_eye_pass1_framebuffer_ready(int eye);
bool ensure_real_call_redirect_target_ready();
bool ensure_real_call_redirect_pass1_ready();
bool ensure_real_call_redirect_depth_ready();

enum class LeverAMode : int {
    Off = 0,
    Yaw90Deg = 1,           // 90 deg yaw about the up axis - absurd on purpose
    TranslateLateral5 = 2,  // +5uu - distinguishes "off-screen at large" from
                            //         "on a different matrix" for near geometry
    TranslateLateral100 = 3 // +100 world units along the current right vector
};

// Unified stereo-offset control (2026-08-11). Both Lever A (world VP via
// renderView+0xC44/+0xC84) and Lever V (GUI-pass view via
// *(SetupView_arg1+0x18)+0x11F0) apply this same lateral translation
// every frame when it's non-zero. One hotkey (F5) cycles through the
// magnitude sequence below; F10/F12 legacy handlers are no longer used.
// This is the "one knob shifts the whole scene" control we needed to
// confirm coverage: if all layers move by the same amount, we know
// Lever A + Lever V between them span the entire rendered scene.
constexpr std::array<float, 5> kStereoOffsetSequence{0.0f, 2.0f, 5.0f, 20.0f, 100.0f};
std::atomic<float> g_stereo_offset{0.0f};
std::atomic_int g_stereo_offset_index{0};
constexpr int kLeverAModeCount = 4;
std::atomic_int lever_a1_mode{static_cast<int>(LeverAMode::Off)};
std::atomic_int lever_a2_mode{static_cast<int>(LeverAMode::Off)};
constexpr float kLeverAYawRadians = 1.5707963f;    // 90 deg
constexpr float kLeverATranslateOffsetSmall = 5.0f;
constexpr float kLeverATranslateOffsetLarge = 100.0f;

const char* lever_a_mode_name(int mode) {
    switch (static_cast<LeverAMode>(mode)) {
        case LeverAMode::Off: return "OFF";
        case LeverAMode::Yaw90Deg: return "YAW-90DEG";
        case LeverAMode::TranslateLateral5: return "TRANSLATE-+5UU";
        case LeverAMode::TranslateLateral100: return "TRANSLATE-+100UU";
    }
    return "UNKNOWN";
}

// Lever G (2026-08-10, replaces the failed Lever B). Decompile of
// RenderGuiModels + its per-model draw + fcn.141ade630 (matrix builder)
// showed the GUI/viewmodel MVP is built as (ctx+0x1170) * (ctx+0x11f0) *
// model_local, where ctx = *(RenderGuiModels_arg1 + 0x28). So the view
// matrix that positions the weapon/hands lives at ctx+0x11f0, with an
// alternate at ctx+0x1130 for one of the sub-modes - completely separate
// memory from the world renderView object Lever A touches, which is why
// Lever A never moved the weapon. Lever G hooks RenderGuiModels
// (DOOM+0x17b35c0), reads arg1+0x28, and clobbers ctx+0x11f0 and
// ctx+0x1130 the same way Lever A clobbers renderView+0xC44/+0xC84.
// Reuses LeverAMode's enum: the modes and magnitudes are the same.
// (Lever B mutation code in hook_cmd_bind_descriptor_sets is being
// removed - decompile also showed DOOM does not deliver per-draw matrices
// via vkCmdBindDescriptorSets or vkCmdPushConstants, so that whole path
// was structurally incapable of moving the weapon.)
// lever_g_mode (v1-v4) is retired in v5 - replaced by lever_g_slot_index
// declared further down. Keeping the symbol removed to make sure any
// stale references become build errors, not silent no-ops.

// RenderGuiModels' fixed job-context address (from radare2 disasm of the
// RegisterJob thunk at 0x1402271a0): the third lea in that thunk stores
// this into rcx as the job's arg1. Kept only as a validation sanity check;
// the hook reads arg1 directly, so this address doesn't have to survive a
// game patch to keep working - if it drifts, the sanity check just warns.
constexpr uintptr_t kRenderGuiModelsExpectedContextRva = 0x66d8978;

// Lever G v5 (2026-08-11). SINGLE-SLOT CYCLE.
//
// v4's blanket-clobber froze DOOM: the loose shape check accepted ~50
// false positives (junk data whose row lengths happened to hit ~1),
// mutating them corrupted unrelated state. This version:
//
//  1. Uses the TIGHTER shape check (orthogonality-required) that rejects
//     those false positives. Expect ~15-25 real slots, not 100+.
//  2. Only mutates ONE cached slot per frame, selected by
//     lever_g_slot_index. F8 press advances the index; -1 = OFF; wraps
//     back to -1 after the last slot. Fixed mutation: YAW-90DEG (largest
//     possible visual signal).
//
// One boot = one press per slot = per-boot proof of which slot (if any)
// drives the weapon. Isolates the culprit even if some slots freeze
// DOOM (kill DOOM, resume from the next index after unfreezing).
struct LeverGSlot {
    uintptr_t base;
    size_t offset;
    const char* ctx_label;  // "ctx28" or "ctx30"
    const char* kind;       // "VP" or "VIEW"
};
std::vector<LeverGSlot> lever_g_slots;
std::mutex lever_g_slots_mutex;
std::atomic_bool lever_g_slots_populated{false};
std::atomic_int lever_g_slot_index{-1};  // -1 = OFF, else slot to clobber
constexpr float kLeverGSlotYawRadians = 1.5707963f;  // 90 deg

void populate_lever_g_slots_from_scan(uintptr_t base, const char* label,
    std::vector<LeverGSlot>& out) {
    if (!base) return;
    constexpr size_t kScanBytes = 0x8000;
    std::vector<uint8_t> buf(kScanBytes);
    if (!safe_read_bytes(base, buf.data(), kScanBytes)) return;
    for (size_t off = 0; off + 64 <= kScanBytes; off += 4) {
        const float* f = reinterpret_cast<const float*>(buf.data() + off);
        bool finite = true;
        for (int i = 0; i < 16; ++i) {
            if (!std::isfinite(f[i])) { finite = false; break; }
        }
        if (!finite) continue;
        const char* kind = nullptr;
        if (looks_like_world_camera_matrix(f)) kind = "VP";
        else if (looks_like_pure_view_matrix(f)) kind = "VIEW";
        if (!kind) continue;
        out.push_back({base, off, label, kind});
        if (out.size() >= 64) return;
    }
}

// Populates lever_g_slots from ctx28/ctx30 the first time it runs after
// arming, logs each one so the user can correlate slot indices to
// (ctx, offset, kind). Called from apply_lever_g_mutation on the worker
// thread; guarded by lever_g_slots_populated.
void populate_and_log_lever_g_slots(uintptr_t ctx28, uintptr_t ctx30) {
    lever_g_slots.clear();
    populate_lever_g_slots_from_scan(ctx28, "ctx28", lever_g_slots);
    populate_lever_g_slots_from_scan(ctx30, "ctx30", lever_g_slots);
    log::warn(std::format(
        "LeverG cache: {} slot(s) from ctx28={:#x} ctx30={:#x}",
        lever_g_slots.size(), ctx28, ctx30));
    for (size_t i = 0; i < lever_g_slots.size(); ++i) {
        const auto& s = lever_g_slots[i];
        log::warn(std::format(
            "LeverG slot[{}] {} offset={:#06x} kind={} addr={:#x}",
            i, s.ctx_label, s.offset, s.kind, s.base + s.offset));
    }
}

bool apply_lever_g_mutation(void* arg1, int /*unused_legacy_mode*/) {
    const int idx = lever_g_slot_index.load(std::memory_order_relaxed);
    if (idx < 0 || !arg1) return false;
    uintptr_t ctx28 = 0, ctx30 = 0;
    safe_read_bytes(reinterpret_cast<uintptr_t>(arg1) + 0x28, &ctx28, sizeof(ctx28));
    safe_read_bytes(reinterpret_cast<uintptr_t>(arg1) + 0x30, &ctx30, sizeof(ctx30));
    if (!lever_g_slots_populated.load(std::memory_order_acquire)) {
        std::scoped_lock lock(lever_g_slots_mutex);
        if (!lever_g_slots_populated.load(std::memory_order_relaxed)) {
            populate_and_log_lever_g_slots(ctx28, ctx30);
            lever_g_slots_populated.store(true, std::memory_order_release);
        }
    }
    if (static_cast<size_t>(idx) >= lever_g_slots.size()) return false;
    const auto& slot = lever_g_slots[idx];
    alignas(16) float m[16]{};
    if (!safe_read_bytes(slot.base + slot.offset, m, sizeof(m))) return false;
    apply_yaw_rotation(m, kLeverGSlotYawRadians);
    safe_write_bytes(slot.base + slot.offset, m, sizeof(m));
    return true;
}

// One-shot probe per Lever G arm: dumps arg1's structure and scans both
// arg1+0x28's target and arg1+0x30's target for any camera-shape 4x4
// matrices. First armed-frame read of +0x11f0/+0x1130/+0x1170 came back
// as all-zeros - meaning either arg3 isn't the one with the view matrix,
// or the arg1 struct field is different from what the decompile
// suggested. This wider probe answers it: if a camera-shape matrix exists
// anywhere in either object's first 32 KB, we'll see the offset and can
// re-target Lever G's write. If none exists, the ctx isn't the right
// object at all and we hunt elsewhere.
std::atomic_bool lever_g_probe_pending{false};

// Pure view/model shape: 3 basis rows (row0/row1/row2 xyz) each with
// length ~1 AND roughly orthogonal to each other. The orthogonality
// check kills false positives like row0=row1=row2=(0,0,1,0) that the
// length-only check accepts but which are junk data, not real matrices.
// A 102-slot blanket clobber earlier this session froze DOOM by writing
// into ~50 such false positives; this check drops them.
bool looks_like_pure_view_matrix(const float* f) {
    const float r0x = f[0], r0y = f[1], r0z = f[2];
    const float r1x = f[4], r1y = f[5], r1z = f[6];
    const float r2x = f[8], r2y = f[9], r2z = f[10];
    const float l0 = std::sqrt(r0x*r0x + r0y*r0y + r0z*r0z);
    const float l1 = std::sqrt(r1x*r1x + r1y*r1y + r1z*r1z);
    const float l2 = std::sqrt(r2x*r2x + r2y*r2y + r2z*r2z);
    if (!(l0 > 0.85f && l0 < 1.15f)) return false;
    if (!(l1 > 0.85f && l1 < 1.15f)) return false;
    if (!(l2 > 0.85f && l2 < 1.15f)) return false;
    // Dot products between unit-length basis rows: orthogonal means ~0.
    const float d01 = std::abs((r0x*r1x + r0y*r1y + r0z*r1z) / (l0 * l1));
    const float d02 = std::abs((r0x*r2x + r0y*r2y + r0z*r2z) / (l0 * l2));
    const float d12 = std::abs((r1x*r2x + r1y*r2y + r1z*r2z) / (l1 * l2));
    return d01 < 0.15f && d02 < 0.15f && d12 < 0.15f;
}

void scan_object_for_camera_matrices(uintptr_t base, const char* label) {
    if (!base) {
        log::warn(std::format("LeverGProbe scan {}: base=NULL", label));
        return;
    }
    constexpr size_t kScanBytes = 0x8000;  // 32 KB
    std::vector<uint8_t> buf(kScanBytes);
    if (!safe_read_bytes(base, buf.data(), kScanBytes)) {
        log::warn(std::format(
            "LeverGProbe scan {}: could not read {} bytes at {:#x}",
            label, kScanBytes, base));
        return;
    }
    log::warn(std::format(
        "LeverGProbe scan {} begin base={:#x} scanBytes={:#x}",
        label, base, kScanBytes));
    size_t hits = 0;
    for (size_t off = 0; off + 64 <= kScanBytes; off += 4) {
        const float* f = reinterpret_cast<const float*>(buf.data() + off);
        bool finite = true;
        for (int i = 0; i < 16; ++i) {
            if (!std::isfinite(f[i])) { finite = false; break; }
        }
        if (!finite) continue;
        // Tag which shape family it belongs to. VP has the FOV bake
        // (row1 length ~1.78); pure view/model has row1 length ~1. A slot
        // that passes VP shape is not tested again for pure-view.
        const char* kind = nullptr;
        if (looks_like_world_camera_matrix(f)) kind = "VP";
        else if (looks_like_pure_view_matrix(f)) kind = "VIEW";
        if (!kind) continue;
        log::warn(std::format(
            "LeverGProbe scan {} hit offset={:#06x} kind={}  row0=({:.4f} {:.4f} {:.4f} {:.4f})  "
            "row1=({:.4f} {:.4f} {:.4f} {:.4f})  row2=({:.4f} {:.4f} {:.4f} {:.4f})  "
            "row3=({:.4f} {:.4f} {:.4f} {:.4f})",
            label, off, kind, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7],
            f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]));
        if (++hits >= 64) {
            log::warn(std::format("LeverGProbe scan {}: 64 hits, capping", label));
            break;
        }
    }
    log::warn(std::format("LeverGProbe scan {} end hits={}", label, hits));
}

// Dumps a specific offset raw as a 16-float block, regardless of shape.
// Use for offsets the decompile called out that the shape scan missed —
// answers "is there anything matrix-like here at all" for that exact
// address.
void dump_raw_matrix(uintptr_t base, size_t offset, const char* label) {
    if (!base) return;
    alignas(16) float m[16]{};
    if (!safe_read_bytes(base + offset, m, sizeof(m))) {
        log::warn(std::format(
            "LeverGProbe raw {} offset={:#06x}: unreadable", label, offset));
        return;
    }
    log::warn(std::format(
        "LeverGProbe raw {} offset={:#06x}  row0=({:.4f} {:.4f} {:.4f} {:.4f})  "
        "row1=({:.4f} {:.4f} {:.4f} {:.4f})  row2=({:.4f} {:.4f} {:.4f} {:.4f})  "
        "row3=({:.4f} {:.4f} {:.4f} {:.4f})",
        label, offset, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
        m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]));
}


// ===========================================================================
// THE GUI MVP BAKE (2026-08-26) - VERIFY FIRST, PATCH ONLY ON A MATCH
// ===========================================================================
//
// WHAT THE EVIDENCE SAYS, in order:
//
//  * CTRL+Up (19:29): with Lever V's rotation OFF the gun welds to the head,
//    with it ON the gun swings at ~2x. So Lever V's R reaches the weapon, and
//    it reaches it TWICE - one application of R to a view produces 1x.
//  * CTRL+Delete (18:29): swapping the UNROTATED matrix into ctx+0x11F0 for
//    the duration of the weapon draw changed nothing. So the weapon does not
//    read that address at draw time.
//  * The decompile (see the Lever V target note at the top of this file):
//        fcn.1402c5840(ctx+0x1170, ctx+0x11F0, renderer+0x4955A4)
//                      [projection]  [view]      [output MVP]
//    SetupView_gui BAKES the MVP, and the bake happens INSIDE the trampoline -
//    after Lever V has already rotated the view. Every draw-time swap is
//    therefore too late by construction, which is exactly the CTRL+Delete
//    null.
//
// So the weapon's R enters through the BAKE. This is the seam plan section 8
// named as the fallback, and it now has a mechanism behind it rather than
// being a guess.
//
// ⚠ WHY THIS VERIFIES BEFORE IT WRITES. Reproducing DOOM's multiply means
// assuming its operand order and row/column convention. Guessing wrong writes
// a plausible-looking wrong matrix, and a wrong matrix here looks like "the
// fix did not work" rather than like a broken probe - the confusable failure
// mode. So every frame this recomputes projection x rotated_view and compares
// it against the bake DOOM JUST WROTE. bakeMatch=yes means the convention is
// right and the patch below is meaningful; bakeMatch=no means it is not, the
// patch is REFUSED, and the log says so instead of silently corrupting the
// weapon's MVP.
//
// The patch itself, when armed and only when verified: rewrite the bake as
// projection x CLEAN view. That removes exactly one R from the weapon's path
// - 2x becomes 1x - while leaving the draw-time matrix, Lever A, and every
// world site untouched.
//
// Idempotent by construction: recomputed from the fresh inputs DOOM baked this
// frame, never rotated in place twice. That is the A-squared compounding
// warning from plan section 8, honoured.

// ===========================================================================
// EXPERIMENT SLOTS (2026-08-26) - FIXED KEYS, REUSED MEANINGS
// ===========================================================================
//
// Owner's standing request, and it is now the rule: keybinds do NOT move.
// Four chords are permanently the experiment slots. When a hypothesis is
// retired its slot is REUSED by the next one rather than a new chord being
// invented, so there is never a growing list to memorise - only four keys
// whose current meaning the log states outright.
//
// The SLOTMAP line prints at arm time and once a second alongside
// HeadWriteSites, so the meaning for THIS build is always in the run's own
// log and can never be mixed up with an earlier build's.
//
// RETIRED so far, and why - do not re-bind these without new evidence:
//   destFixed writes    - measured inert with a positive control (attempts
//                         froze, skipped climbed, no visual change). Default
//                         OFF, permanently.
//   gun base RAW/rotated- the weapon does not read ctx+0x11F0 at draw time;
//                         the bake has already consumed it. Dead by
//                         construction, not by null.
struct ExpSlot {
    const char* chord;
    const char* what;   // reassigned at slot-map time when a build changes a role
    std::atomic_bool on;
};
// REASSIGNED 2026-08-27 for the tactic run (plan §0b). Retired meanings whose
// consumers were DISCONNECTED in the same edit (a reassigned slot with a live
// old consumer is the half-migrated-indirection defect from handoff §5.3):
//   half-angle Lever V, remove-k rotations, RenderGuiModels pass skip,
//   +30deg Lever-V probe, model-hit correction - all now hard false.
// TEST 2 (2026-08-28): both fixes aimed by the 09:17 corpus + census data,
// individually toggleable so ONE run attributes both. Anchor/slew retired
// (projected content is ruled out permanently - owner).
// REASSIGNED 2026-08-28 evening (freqHigh script §2.2): slot 1 is now the
// POSITIVE CONTROL - +30 deg world yaw written through the proven freqHigh
// operators into exactly the classified viewmodel records (arms + gun, both
// passes, <=4 records/frame). Its old UNWELD consumer is hard-disconnected
// in the same edit. Pressing the key again is the kill switch.
// ===== SLOT 1 REASSIGNED 2026-08-30 (evening): THE IPD SPLIT TEST ==========
//
// SHIFT+Up now drives g_ipd_override ONLY. ON = both passes render the SAME
// camera (ipd 0); OFF = the configured/scripted separation is back. Nothing
// else moves with the key.
//
// THE OLD CONSUMERS ARE HARD-DISCONNECTED IN THIS SAME EDIT, which is the
// house rule: a reassigned slot with a live old consumer is the
// half-migrated-indirection defect. They now read g_legacy_fixfamily, an
// atomic set ONCE from the switch file and never touched by a key, so their
// behaviour for any given config is byte-for-byte what it was - only the
// keypress is gone. Audited in code before the reassignment, not from docs:
//   ssfix3_note / cullfix_note / fwdfix_note - DELETED 2026-09-03.
//   FHCTRL / FHCTRL2 write paths            - DELETED 2026-09-03.
//   the bodyfollow kill inside exp_toggle   - bodyfollow=off, and removed.
// Nothing behavioural reads g_legacy_fixfamily any more: after the ssfix /
// fwdfix deletion it is exactly `bodyfollow != Off`, and its only two
// remaining readers are arm= labels in the FHCENSUS logs. It goes with
// bodyfollow.
std::atomic_bool g_legacy_fixfamily{false};
inline bool legacy_fixfamily_on() {
    return g_legacy_fixfamily.load(std::memory_order_relaxed);
}
ExpSlot g_exp_slots[4] = {
    // SLOT 1 IS UNASSIGNED as of 2026-09-03. Its last three meanings
    // (job filter, LUMFIX, STEPLADDER) are all deleted, so NOTHING is
    // wired to this chord: SHIFT+Up reaches no handler at all. The slot
    // is kept in the table because the four chords are fixed by design -
    // reassign it by writing a handler AND this label in the same edit.
    {"SHIFT+Up",    "UNASSIGNED - no handler behind this chord", {false}},
    // REASSIGNED 2026-08-30 in a precull build: this slot is the PRECULL A/B
    // and the run's safety exit. ON = producer write at DOOM+0x1830B80 owns
    // the camera; OFF = the legacy seam rotations (Lever A + Lever V) come
    // back, i.e. the shipped build. In a non-precull build the old meaning is
    // unchanged - the role is decided once from the switch file, so there is
    // no live old consumer behind a reassigned key.
    {"CTRL+Up",     "PRECULL A/B: producer write ON <-> legacy seam rotation (Lever A + V)", {true}},
    {"CTRL+Insert", "GUNFIX2 kill switch: viewmodel world-projection substitution on/off", {false}},
    // Reduced to one meaning 2026-09-03: BLANKFAM and the SYNTHYAW angle
    // ladder both went, leaving the head-look runtime toggle as the whole
    // of this chord.
    {"CTRL+Delete", "HEADLOOK toggle: park/unpark the head-decoupling experiment", {false}},
};
inline bool exp_on(int i) { return g_exp_slots[i].on.load(std::memory_order_relaxed); }

bool exp_toggle(int i) {
    const bool now = !g_exp_slots[i].on.load(std::memory_order_relaxed);
    g_exp_slots[i].on.store(now, std::memory_order_relaxed);
    // DISCONNECTED 2026-08-30 (evening) with the slot-1 reassignment: this key
    // used to ALSO kill the bodyfollow transfer, which would make one press
    // change two behaviours in a bodyfollow build - the half-migrated defect
    // the slot map warns about. bodyfollow=off in this config, so it was
    // already a no-op; it is now gone by construction rather than by luck.
    // Re-earn it a slot with new evidence if bodyfollow comes back.
    log::warn(std::format(
        "SLOT {} [{}] -> {} : {}\n"
        "READING IT: the four experiment slots are FIXED KEYS whose meaning changes per build "
        "- grep SLOTMAP for what each one means in this run. Never assume a chord does what it "
        "did in an earlier session.",
        i + 1, g_exp_slots[i].chord, now ? "ON" : "OFF", g_exp_slots[i].what));
    return now;
}
void log_slot_map() {
    // One-time sync: slot 3's display must match the CONFIG-armed anchor
    // state, or the map would show "off" over a live anchor and the first
    // press would read as a no-op - the check-the-toggle-is-armed defect.
    static bool anchor_synced = false;
    if (!anchor_synced) {
        anchor_synced = true;
        // Slot 3 initializes from the switch file (gunfix2=worldp), so the
        // FIX can be armed with zero keypresses and a dead key can never
        // again turn a test into a no-op; pressing CTRL+Insert kills it
        // live. The SLOTMAP line is the receipt. (derotate= no longer feeds
        // this slot - its GPUFIX consumer was disconnected the same edit.)
        // ===== SLOT 1 REASSIGNED WHEN BODYFOLLOW IS ARMED (2026-08-29) =====
        //
        // The Run-3 build needs a kill switch on the head/body transfer, and
        // the plan reassigns slot 1 for it: once the engine's own view turns,
        // that family is gone (deleted 2026-09-03) - so one press dropping
        // BOTH is the intended behaviour, not a collision.
        //
        // ⚠ The role is decided ONCE, from the config, so a build with
        // bodyfollow=off (FLAT Run 1) keeps slot 1 exactly as it was: same
        // consumers, same initial state, no behavioural change at all. The
        // SLOTMAP line prints whichever role is live, so the run's own log
        // says what the key does in THIS build.
        // ===== SLOT 1 IS THE IPD SPLIT TEST (2026-08-30 evening) ===========
        //
        // The expression that used to initialise this SLOT still initialises
        // g_legacy_fixfamily - same bits, same consumers, no key.
        //
        // ===== SLOT 1 REASSIGNED 2026-08-31: PREVFIX =======================
        // The ipd split test it held is BANKED: the 23:47 run pressed it once
        // (IPDHB toggles=1, 3997/3997 passes at offset 0.000) and both defects
        // persisted, which retired camera divergence as the cause. The ipd
        // override stays available from the switch file (`ipd=`) and is now
        // driven ONLY from there - no key touches it, so the reassignment
        // leaves no live old consumer behind this chord.
        //
        const bool bf_armed = wrap::bodyfollow_configured() != wrap::BodyFollow::Off;
        g_legacy_fixfamily.store(bf_armed, std::memory_order_relaxed);
        g_exp_slots[0].on.store(false, std::memory_order_relaxed);
        g_exp_slots[2].on.store(wrap::config().gunfix2_worldp,
                                std::memory_order_relaxed);
    }
    log::warn(std::format(
        "SLOTMAP  1 [{}] {} = {}  |  2 [{}] {} = {}  |  3 [{}] {} = {}  |  4 [{}] {} = {}",
        g_exp_slots[0].chord, exp_on(0) ? "ON " : "off", g_exp_slots[0].what,
        g_exp_slots[1].chord, exp_on(1) ? "ON " : "off", g_exp_slots[1].what,
        g_exp_slots[2].chord, exp_on(2) ? "ON " : "off", g_exp_slots[2].what,
        g_exp_slots[3].chord, exp_on(3) ? "ON " : "off", g_exp_slots[3].what));
}
constexpr uintptr_t kGuiBakeMvpOffset = 0x4955A4;
thread_local uintptr_t g_bake_ctx = 0;
thread_local float g_bake_view_clean[16]{};
thread_local float g_bake_view_rot[16]{};
thread_local bool g_bake_have_inputs = false;
std::atomic_uint64_t g_bake_attempts{0};
std::atomic_uint64_t g_bake_verified{0};
std::atomic_uint64_t g_bake_mismatch{0};
std::atomic_uint64_t g_bake_patched{0};
std::atomic_uint64_t g_bake_no_inputs{0};
std::atomic<float> g_bake_worst_diff{0.0f};
std::atomic_uint32_t g_bake_log_lines{0};
constexpr uint32_t kMaxBakeLogLines = 12;

// ⚠ EVERY EXIT IS COUNTED (2026-08-26, second version). The first version had
// FOUR silent early returns, and the run that armed it recorded
// attempts=7135 verified=0 mismatch=0 patched=0 noInputs=0 with not one dump
// line: the function ran seven thousand times and fell into an unattributed
// hole every single time. The owner spent a headset run on a probe that could
// not report its own failure. Never again in this file: an early return with
// no counter is the defect, not the symptom.
std::atomic_uint64_t g_bake_no_renderer{0};
std::atomic_uint64_t g_bake_no_proj{0};
std::atomic_uint64_t g_bake_no_mvp{0};
std::atomic_uint32_t g_bake_diag_lines{0};
constexpr uint32_t kMaxBakeDiagLines = 8;

void gui_bake_verify_and_patch(void* arg1) {
    // POSITIVE CONTROL, before any predicate.
    g_bake_attempts.fetch_add(1, std::memory_order_relaxed);
    if (!g_bake_have_inputs || !g_bake_ctx) {
        g_bake_no_inputs.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_bake_have_inputs = false;   // one patch per bake, never a stale reuse

    // TWO CANDIDATE RENDERERS, because the decompile's third operand is
    // `arg1_00 + 0x4955A4` where arg1_00 came from SetupView's OWN argument -
    // not necessarily the process-global renderer this file uses elsewhere.
    // The first version assumed the global. Try the argument-derived one
    // first, fall back to the global, and log both so a wrong guess is
    // visible instead of silent.
    uintptr_t rend_arg = 0;
    if (arg1) safe_read_bytes(reinterpret_cast<uintptr_t>(arg1), &rend_arg, sizeof(rend_arg));
    uintptr_t rend_global = 0;
    const uintptr_t doom_base =
        reinterpret_cast<uintptr_t>(GetModuleHandleW(L"DOOMx64vk.exe"));
    if (doom_base)
        safe_read_bytes(doom_base + kRendererPtrGlobalRva, &rend_global, sizeof(rend_global));

    alignas(16) float proj[16]{};
    const bool proj_ok = safe_read_bytes(g_bake_ctx + 0x1170, proj, sizeof(proj));

    uintptr_t mvp_addr = 0;
    alignas(16) float baked[16]{};
    bool mvp_ok = false;
    for (uintptr_t r : {rend_arg, rend_global}) {
        if (!r) continue;
        const uintptr_t a = r + kGuiBakeMvpOffset;
        if (safe_read_bytes(a, baked, sizeof(baked))) { mvp_addr = a; mvp_ok = true; break; }
    }

    // ONE DIAGNOSTIC LINE THAT NAMES WHICHEVER STEP FAILED. Bounded, and it
    // fires on the FIRST few calls whatever the outcome, so a run can never
    // again come back with nothing but a frozen counter.
    if (g_bake_diag_lines.fetch_add(1, std::memory_order_relaxed) < kMaxBakeDiagLines) {
        log::warn(std::format(
            "GuiBakeDiag ctx={:#x} projRead={} rendArg={:#x} rendGlobal={:#x} mvpAddr={:#x} "
            "mvpRead={}\n"
            "READING IT: this fires on the first {} calls REGARDLESS of outcome - it exists "
            "because the previous version returned silently and burned a run. projRead=false "
            "means ctx+0x1170 is not readable and the projection operand assumption is wrong. "
            "Both renderers zero, or mvpRead=false, means renderer+0x4955A4 is not where this "
            "build keeps the baked GUI MVP and the whole seam needs relocating rather than "
            "re-signing.",
            g_bake_ctx, proj_ok, rend_arg, rend_global, mvp_addr, mvp_ok, kMaxBakeDiagLines));
    }

    if (!rend_arg && !rend_global) { g_bake_no_renderer.fetch_add(1, std::memory_order_relaxed); return; }
    if (!proj_ok) { g_bake_no_proj.fetch_add(1, std::memory_order_relaxed); return; }
    if (!mvp_ok)  { g_bake_no_mvp.fetch_add(1, std::memory_order_relaxed); return; }

    // Reproduce DOOM's own multiply from the operands it used, BOTH orders,
    // so the convention is measured rather than assumed.
    alignas(16) float check_ab[16]{}, check_ba[16]{};
    wrap::mat4_multiply(proj, g_bake_view_rot, check_ab);
    wrap::mat4_multiply(g_bake_view_rot, proj, check_ba);
    float scale = 1.0f;
    for (int i = 0; i < 16; ++i) scale = (std::max)(scale, std::fabs(baked[i]));
    auto rel_diff = [&](const float* c) {
        float worst = 0.0f;
        for (int i = 0; i < 16; ++i) {
            if (!std::isfinite(c[i]) || !std::isfinite(baked[i])) return 1e30f;
            const float d = std::fabs(c[i] - baked[i]);
            if (d > worst) worst = d;
        }
        return worst / scale;
    };
    const float rel_ab = rel_diff(check_ab);
    const float rel_ba = rel_diff(check_ba);
    const bool ab = rel_ab <= rel_ba;
    const float rel = ab ? rel_ab : rel_ba;
    const bool match = rel <= 1e-3f;

    float wd = g_bake_worst_diff.load(std::memory_order_relaxed);
    while (rel > wd && !g_bake_worst_diff.compare_exchange_weak(
                           wd, rel, std::memory_order_relaxed)) {}
    if (match) g_bake_verified.fetch_add(1, std::memory_order_relaxed);
    else       g_bake_mismatch.fetch_add(1, std::memory_order_relaxed);

    if (g_bake_log_lines.fetch_add(1, std::memory_order_relaxed) < kMaxBakeLogLines) {
        log::warn(std::format(
            "GuiBake ctx={:#x} mvp={:#x} bakeMatch={} order={} relAB={:.6f} relBA={:.6f}\n"
            "READING IT: both operand orders are tried against the MVP SetupView_gui JUST "
            "wrote, so the convention is measured, not guessed. bakeMatch=yes means one of "
            "them reproduces DOOM's own multiply and the patch below is meaningful. NO means "
            "NEITHER order reproduces it - the bake is not a plain product of these two "
            "operands, the patch is REFUSED rather than writing a plausible wrong matrix, and "
            "the seam needs re-deriving before anything else is tried here.",
            g_bake_ctx, mvp_addr, match ? "yes" : "NO", ab ? "projxview" : "viewxproj",
            rel_ab, rel_ba));
    }


    // ===== SEAM RETIRED 2026-08-26 =====================================
    // The patch below is DISARMED PERMANENTLY. Its positive control wrote a
    // deliberately wrong MVP here 1308 times, with the multiply verified
    // against DOOM's own to relAB=0.000000 on 23393 of 23393 attempts, and
    // the image did not change. Nothing visible reads renderer+0x4955A4
    // after SetupView_gui writes it - the per-model draw rebuilds the product
    // from the two source matrices anyway. The VERIFY stays, because it is a
    // standing proof that our operand convention is right and it costs
    // nothing; the WRITE does not come back without new evidence.
}
void log_render_gui_context_matrices(uintptr_t arg1) {
    // Dump arg1's key fields per the decompile:
    //   arg1+0x0  -> renderer pointer
    //   arg1+0x8  -> model array
    //   arg1+0x10 -> model count (int)
    //   arg1+0x28 -> arg3 for per-model draw (my first ctx guess)
    //   arg1+0x30 -> arg4 for per-model draw (alt ctx candidate)
    uintptr_t f_renderer = 0, f_models = 0, f_ctx28 = 0, f_ctx30 = 0;
    int32_t f_count = -1;
    safe_read_bytes(arg1 + 0x00, &f_renderer, sizeof(f_renderer));
    safe_read_bytes(arg1 + 0x08, &f_models, sizeof(f_models));
    safe_read_bytes(arg1 + 0x10, &f_count, sizeof(f_count));
    safe_read_bytes(arg1 + 0x28, &f_ctx28, sizeof(f_ctx28));
    safe_read_bytes(arg1 + 0x30, &f_ctx30, sizeof(f_ctx30));
    log::warn(std::format(
        "LeverGProbe arg1={:#x}  renderer={:#x}  models={:#x}  count={}  "
        "ctx28={:#x}  ctx30={:#x}",
        arg1, f_renderer, f_models, f_count, f_ctx28, f_ctx30));
    // Scan both candidate contexts. If count==0, RenderGuiModels didn't
    // draw anything this call and the ctx fields may be stale/unused; the
    // scan still tells us whether they've EVER held a matrix.
    scan_object_for_camera_matrices(f_ctx28, "ctx28");
    scan_object_for_camera_matrices(f_ctx30, "ctx30");
    // Raw dumps of the exact offsets the decompile of fcn.141ade630 called
    // out. Yesterday's shape scan filtered these out because they're pure
    // view matrices (row1 length ~1.0), not VP (row1 length ~1.78). Now
    // scan_object also runs the pure-view check and tags results as VP or
    // VIEW; these raw dumps are a belt-and-suspenders on the exact
    // decompile-referenced offsets.
    dump_raw_matrix(f_ctx30, 0x11B0, "ctx30");
    dump_raw_matrix(f_ctx30, 0x11F0, "ctx30");
    dump_raw_matrix(f_ctx30, 0x1230, "ctx30");
    dump_raw_matrix(f_ctx30, 0x1130, "ctx30");
}
// +0x2000 and log every 16-float region that passes the world-camera shape
// check. Answers the "where do the other cameras live" question directly on
// the object we already have. One-shot per arm cycle; bounded log volume.
std::atomic_bool render_view_layout_scan_pending{false};

void log_render_view_layout_scan(uintptr_t render_view) {
    if (!render_view) return;
    constexpr size_t kScanBytes = 0x2000;
    alignas(16) uint8_t buffer[kScanBytes]{};
    if (!safe_read_bytes(render_view, buffer, kScanBytes)) {
        log::warn(std::format(
            "RenderViewLayoutScan: could not read {} bytes at renderView={}",
            kScanBytes, reinterpret_cast<void*>(render_view)));
        return;
    }
    log::warn(std::format(
        "RenderViewLayoutScan begin renderView={} scanBytes={:#x}",
        reinterpret_cast<void*>(render_view), kScanBytes));
    size_t hits = 0;
    constexpr size_t kMatrixBytes = 64;
    constexpr size_t kStep = 4;
    for (size_t off = 0; off + kMatrixBytes <= kScanBytes; off += kStep) {
        const float* f = reinterpret_cast<const float*>(buffer + off);
        bool finite = true;
        for (int i = 0; i < 16; ++i) {
            if (!std::isfinite(f[i])) { finite = false; break; }
        }
        if (!finite) continue;
        if (!looks_like_world_camera_matrix(f)) continue;
        log::warn(std::format(
            "RenderViewLayoutScan hit offset={:#06x}  row0=({:.4f} {:.4f} {:.4f} {:.4f})  "
            "row1=({:.4f} {:.4f} {:.4f} {:.4f})  row2=({:.4f} {:.4f} {:.4f} {:.4f})  "
            "row3=({:.4f} {:.4f} {:.4f} {:.4f})",
            off, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7],
            f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]));
        if (++hits >= 32) {
            log::warn("RenderViewLayoutScan: 32 hits, capping");
            break;
        }
    }
    log::warn(std::format("RenderViewLayoutScan end hits={}", hits));
}

// Reads renderView+0xC44 and +0xC84, applies the current mode's mutation to
// both matrices identically, and writes them back destructively. Not for
// use on the injected extra call - this is the REAL call's data and stays
// that way. Returns false only if the reads failed (bad render_view
// pointer); write success is not the interesting signal (see the plan's §1
// - "wroteCurrent=true" was never the missing evidence anyway).
// Writes an offset-shifted copy of the world VP at renderView+0xC44/+0xC84
// PLUS all 6 sibling destination copies at *(*[0x146fd5fc0..0x146fd5fe8]) +
// same offsets. The WorldVpTrace probe showed the value at renderView+0xC44
// is stable across every downstream job of one frame - meaning the glitch
// isn't temporal (nobody stomps our write within a frame). It's spatial:
// SetupView_world's copy loop populates 6 destinations from a single
// CameraSrc source, downstream jobs read from different destinations, and
// Lever A only mutated ONE of them. Writing to all 6 kills the mismatch.
// RENDER-VIEW RESTORE (2026-08-16). renderView+0xC44 is the current view
// matrix and +0xC84 the PREVIOUS one - the pair DOOM differences to build
// motion vectors. The shift below clobbers both destructively with no
// restore, so after the doubled call DOOM's "previous view" is eye 1's
// shifted matrix. The next real frame then computes a camera delta the size
// of the eye separation and smears eye 0 with temporal reprojection.
//
// That matches the measurement exactly: blur, not displacement; monocular in
// the left eye; absent at 0.0 separation and worse at 12.0 than at 3.0.
// Defined here because this write site precedes inside_frame_root_double's
// definition in this translation unit.
extern thread_local bool inside_frame_root_double;
thread_local uintptr_t render_view_restore_addr = 0;
thread_local float render_view_restore_current[16]{};
thread_local float render_view_restore_previous[16]{};
thread_local bool render_view_restore_valid = false;
std::atomic_uint32_t render_view_restore_count{0};

// ---------------------------------------------------------------------------
// GUN UNSHIFT (2026-08-28). Declared here rather than beside
// hook_setup_view_gui because the view-write census below prints its state in
// every frame header, and that census is defined ~2000 lines above the hook.
//
// TWO REFUTED PREDECESSORS, both closed by flat-screen tests, both kept in this
// note because between them they fix the location of the fix:
//
//  1. VIEWMODEL LOCK - decline Lever V's +0x11F0 write entirely in live
//     symmetric stereo. It locked the gun AND stopped the interior walls
//     moving with the separation; only the distant canyon still shifted, from
//     Lever A. So the 2026-08-16 measurement stands on an uncorrupted
//     baseline: +0x11F0 really does carry near world geometry.
//  2. POST-BAKE RESTORE - keep the write, then put the clean matrix back the
//     instant real_setup_view_gui returns, on the theory that the MVP bake had
//     already consumed the shifted copy for the walls. Behaved IDENTICALLY to
//     the lock: walls and gun both stopped moving.
//
// (2) is the informative one. Restoring immediately after SetupView returns is
// indistinguishable from never writing, so SetupView's own execution does not
// durably consume the shifted value into anything the walls later draw from.
// BOTH consumers read +0x11F0 AFTER SetupView has returned, and no restore
// scoped to that call can separate them. The seam below is the theory that
// predicted otherwise; it is recorded because it was wrong:
//
//   * SetupView_gui BAKES the MVP from it into renderer+0x4955A4, inside the
//     real function call - this is what the interior walls draw from;
//   * the per-model GUI draw REBUILDS its own MVP later, at draw time, as
//     (ctx+0x1170) * (ctx+0x11f0) * model_local - this is what the weapon
//     draws from. See the Lever G decompile note above.
//
// What actually separates the two consumers is WHICH JOB is running.
// RenderGuiModels (DOOM+0x17b35c0) is "not on the world visibility pipeline;
// renders viewmodel/HUD 3D via its own per-context view matrix" - see
// real_render_gui_models above - while the walls are drawn by RenderOpaque.
// Disjoint windows over the same matrix. So the unshift is scoped to the
// CONSUMER: put the clean matrix in for the duration of RenderGuiModels' call
// and the shifted one straight back afterwards. The weapon draws unshifted;
// every draw outside that window still sees the shift, walls included.
//
// EXACT ADDRESS, NOT A GUESSED CONTEXT. Lever G's notes offer two candidate
// context objects for this job (*(arg1+0x28) and *(arg1+0x30)) and the raw
// dumps disagree with the decompile about which one carries +0x11F0. Rather
// than pick, Lever V republishes the address it actually wrote - the census
// already printed it as 0x18c9e7d5210 - together with both the clean and the
// shifted matrix, so the swap cannot miss the object. Both candidate ctx
// addresses are logged anyway: if the gun still moves, that tells us at once
// whether the weapon's job even uses the same object, which is the difference
// between "wrong address" and "wrong job".
//
// EXACT INVERSE, FOR FREE. apply_translation only ever modifies each row's [3]
// element, and reads the right vector from m[0..2], which it does not change.
// So clean and shifted differ in four floats and swapping between them is
// lossless in both directions - no inverse has to be computed, and no rounding
// accumulates across frames.
//
// THE RACE, NAMED RATHER THAN ASSUMED AWAY. RenderGuiModels is a registered
// job like the others in this table, so it may run on a worker thread
// concurrently with the world draws that want the shifted matrix. If it does,
// the swap window is a real hazard and the walls would flicker. The probe logs
// its thread id against the render thread's for exactly this reason; the
// window is kept to the single call to make it as small as it can be.
//
// Scoped to LIVE symmetric stereo, like both predecessors, so A/B mode - where
// eye 0 is an offscreen dump nobody looks at - keeps the behaviour its
// measurements were taken under. Live A/B on SHIFT+HOME.
//
// MEASURED, run of 09:42-09:43 (see the census rows, and the flat-screen
// observation that goes with them). At 12.0 uu separation, one frame:
//
//   real call    ctx+0x11F0  off=-6.0   m3 -2624.000 -> -2618.000
//   doubled call ctx+0x11F0  off=+6.0   m3 -2618.000 -> -2624.000
//
// Both sides write the SAME address, and the doubled call's "before" is the
// real call's "after". So the doubled call applies its +half ON TOP of the
// real call's -half and lands exactly back on DOOM's own unshifted matrix.
// That is the whole eye-to-eye asymmetry the plan asked about:
//
//   * eye 1's viewmodel is drawn from the ORIGINAL matrix, which is why the
//     right eye's gun does not move at any separation;
//   * eye 0's viewmodel is drawn from base - half, which scales with the
//     separation, which is why the left eye's gun has three positions.
//
// The screen displacement predicted from -off = +ipd/2 is +1.5 at 3.0, +6.0 at
// 12.0 and 0.0 at 0.0, so the 3->12->0->3 cycle should read right-a-little,
// left-a-lot, right-a-little. That is exactly what the owner saw, magnitudes
// included. S1 is the real call's own Lever V shift.
//
// There is a second defect in the same numbers. Far geometry gets its
// separation from Lever A alone (-half in eye 0, +half in eye 1, total 2*half).
// Near geometry, which Lever V also covers, gets Lever A's -half PLUS Lever V's
// -half in eye 0 and Lever A's +half plus Lever V's ZERO in eye 1 - a total of
// 3*half, applied asymmetrically. Near geometry is currently carrying 1.5x the
// disparity of far geometry, all of the excess in one eye.
//
// Locking the viewmodel means declining Lever V's +0x11F0 write on BOTH sides
// in live symmetric stereo, which fixes the gun and the near/far disparity
// mismatch together, and leaves both eyes' parallax to Lever A. It must be both
// sides: suppressing only the real call would leave the doubled call applying
// +half to an unshifted matrix and simply move the moving gun to eye 1.
//
// WHAT THIS COSTS, and it is a real cost, not a rhetorical one. The 2026-08-16
// measurement recorded interior walls changing 0.0% of pixels with Lever V
// suppressed against 94.4% with it active, and that is the reason the
// suppression was hardcoded off with a comment saying to scope the restore
// instead. Two things make it worth re-testing rather than obeying: that
// measurement was taken in A/B mode on a baseline since found to be corrupted
// by the render-surface hook at DOOM+0x1A367E0, and eye 1 today has correct
// world parallax while receiving no net Lever V shift at all - which is a
// standing demonstration that Lever A alone carries the world.
//
// So the run this is built for judges TWO things, not one, and the second is
// the one that can refute it: the gun must stop moving, AND the world must
// still show parallax. Live-toggleable on SHIFT+HOME so both states are
// comparable from one standing position rather than across a relaunch.
// A/B mode is untouched - it compares offscreen dumps where eye 0 does not
// matter, and that is where the 94.4% figure came from.
std::atomic_bool gun_unshift_enabled{true};
// CTRL+Delete. false = publish the head-rotated base (every run to date);
// true = publish DOOM's raw unrotated matrix. See the key handler for the
// measurement that motivates it and for the three predicted outcomes.
std::atomic_bool gun_base_raw{false};
// CTRL+Insert. The destFixed +0xC44/+0xC84 rotation AND translation. Default
// true = the behaviour every previous run was measured under.
// DEFAULT FLIPPED TO OFF, 2026-08-26, on a measured null WITH a positive
// control: across two ON-/OFF cycles the attempts counter froze and `skipped`
// climbed, proving the writes really stopped, and the owner saw no change.
// These writes land on offsets that hold no camera in any of the six
// destinations (CamHunt found the real ones at 0x1450/0x1460/0x3AC0,
// 0x2250, 0x1eb0, 0x3b00 - never 0xC44/0xC84), so what they were doing was
// rotating and translating garbage into live engine objects ~127000 times a
// run. Inert is not harmless: it is a standing confound in every future run.
// CTRL+Insert restores the old behaviour if a later result needs it back.
std::atomic_bool dest_fixed_write_enabled{false};
// Phase 3 liveness for the near-geometry / viewmodel view. A zero here while
// the world is rotating is the gun-welded-to-your-face symptom, and it is
// invisible from every counter on Lever A.
std::atomic_uint64_t gui_head_rotations{0};
std::atomic_uint64_t gui_head_refusals{0};
// RAW attempts, incremented BEFORE any predicate (handoff 2026-08-26 §8's
// mandatory rule): guiAttempts frozen means the site is unreachable, which is
// a different failure from guiApplied=0 with attempts climbing.
std::atomic_uint64_t gui_head_attempts{0};
// Refusals split by cause, because they demand different fixes: norot = the
// world rotation was unavailable (no published basis or head angles yet - a
// startup transient if small, a wiring break if climbing), shape = the matrix
// at ctx+0x11F0 failed the affine-orthonormal gate (layout changed - the
// first few are dumped in full).
std::atomic_uint64_t gui_head_norot{0};
std::atomic_uint64_t gui_head_shape_refusals{0};
// CTRL+Up. Lever V rotation forced Off at runtime. Its own counter so the OFF
// arm is visibly running rather than looking like a dead site.
std::atomic_bool lever_v_rot_enabled{true};
std::atomic_uint64_t gui_head_rot_suppressed{0};
// SLOT 3 liveness: Lever A rotations suppressed for the composition test.
std::atomic_uint64_t g_lever_a_rot_suppressed{0};
// FIXED-OFFSET destination rotation liveness (the +0xC44/+0xC84 writes inside
// apply_shift_to_world_vp_destinations). That call previously DISCARDED
// apply_head_rotation's verdict, so "rotating real matrices every frame" and
// "silently refusing six zero matrices every frame" printed identical logs -
// the exact §8 failure class. attempts increments before the predicate.
std::atomic_uint64_t dest_fixed_attempts{0};
std::atomic_uint64_t dest_fixed_rotations{0};
std::atomic_uint64_t dest_fixed_refusals{0};
// Writes skipped because CTRL+Insert disarmed this path. Its own counter so
// the OFF arm is visible in the log rather than looking like a dead site.
std::atomic_uint64_t dest_fixed_skipped{0};
// BUILD B (2026-08-26): NAME THE HALF-SPLIT.
//
// The 14:47 run measured destFixed=102636/205272 - refused EXACTLY half. The
// pass touches 12 matrices (6 destinations x current+previous), so six of the
// twelve sites refuse on every single frame and the aggregate cannot say which
// six. "Exactly half" has two very different readings - one of each
// current/previous pair (the +0xC84 previous copies are zeros), or three whole
// destinations dead - and they point at different next moves.
//
// One line per (destination, matrix) site, ONCE, in gameplay, carrying the
// four row lengths that ARE apply_head_rotation's gate. Bounded by
// construction at 12 lines per session, and the gameplay check is only
// evaluated while sites remain unlogged, so the steady state costs one relaxed
// atomic load.
std::atomic_uint32_t dest_fixed_sites_logged{0};
std::atomic_bool dest_fixed_site_logged[6][2]{};

// Declared up here, above the view-write census, because the census header
// reports both every frame and is defined further down this file than either
// natural home would be.
//
// xr_present_path_hits: incremented in the present hook's XR branch. ZERO means
// the headset submission path never ran, so the monitor is showing DOOM's own
// presented swapchain image and nothing that only affects XR submission can
// explain anything seen in that run. It exists because a test was built and
// judged that this would have voided in advance - see the SHIFT+END handler.
std::atomic_uint32_t xr_present_path_hits{0};
// skip_composite_capture: SHIFT+END. capture_composite_to_holding records a GPU
// blit plus layout transitions into DOOM's OWN command buffer every frame while
// full-frame mode is on, independently of anything the doubled call does and of
// every floor rung. In a headset-free run nothing reads the holding image, so
// skipping it is free; with a headset attached it would freeze eye 0.
std::atomic_bool skip_composite_capture{false};
// SHIFT+PageDown state 2. snapshot_frame_start_layouts and rewind_mirror_layouts
// record image-layout barriers into DOOM's OWN command buffer around the real
// call, every frame, regardless of what the doubled call does or which floor
// rung is set - which is the same "survives everything" profile S3 has. Skipped
// surgically rather than by turning mirroring off wholesale: clearing
// mirror_enabled would also stop framebuffer substitution, sending the doubled
// call's draws into DOOM's live targets, which is known to double brightness
// and would wreck the very judgement this is for.
std::atomic_bool skip_mirror_layout_ops{false};
// Forward declaration only - the definition stays with the rest of the
// frame-root doubling state further down, but the census header reports it.
extern std::atomic_bool frame_root_blit_only;
// Counted per emitted census frame, so the header reports whether the swap
// actually ran on this frame rather than only that it was switched on. A frame
// where it is enabled and ran zero times is the single most useful thing this
// can report, and it is exactly what a missing log line would hide.
std::atomic_uint32_t vwc_gun_unshifts{0};

// Published by Lever V immediately after its write, consumed by
// hook_render_gui_models. Not thread_local: the two hooks may be on different
// threads, which is the whole reason the thread ids are logged.
// ONLY THE REAL CALL PUBLISHES, and that is load-bearing. The doubled call's
// pre-write value is not DOOM's base - it is the real call's ALREADY-SHIFTED
// matrix, because the real call's mutation is never restored. The first build
// of this let the doubled call publish too, and the log shows what that did:
//
//   real  clean=-2624.000 (base)          -> eye 0's gun drawn at base, correct
//   dbl   clean=-2622.500 (base - half)   -> eye 1's gun drawn at base - half
//
// which locked eye 0's gun by moving eye 1's, a straight regression in an eye
// this task was told not to touch. Publishing only from the real call gives
// both sides DOOM's own base matrix, which is what a view-space viewmodel
// wants in both eyes anyway. On the doubled side writing it is a no-op, since
// the doubled call's own +half has already landed the matrix back on base.
std::mutex gun_unshift_mutex;
uintptr_t gun_unshift_addr = 0;
alignas(16) float gun_unshift_clean[16]{};
float gun_unshift_offset = 0.0f;
uint32_t gun_unshift_seq = 0;   // increments on every publish
bool gun_unshift_valid = false;

void gun_unshift_publish(uintptr_t addr, float offset, const float* clean) {
    std::scoped_lock lock(gun_unshift_mutex);
    gun_unshift_addr = addr;
    gun_unshift_offset = offset;
    std::memcpy(gun_unshift_clean, clean, sizeof(gun_unshift_clean));
    ++gun_unshift_seq;
    gun_unshift_valid = true;
}

// Lever V declined to write this call, so nothing is standing to undo. Cleared
// rather than left stale: a stale publish would have RenderGuiModels swapping in
// a matrix from an earlier frame, which is the class of error that looks like a
// working fix until the camera moves.
void gun_unshift_invalidate() {
    std::scoped_lock lock(gun_unshift_mutex);
    gun_unshift_valid = false;
}

// Bounded per census arm, separate budget per side - a shared cap would let the
// real call's fires spend the whole quota before the doubled call logged once.
ParityBudget gun_unshift_budget{6};
// GUNWINDOW angle measurement - see the block in hook_render_gui_models. Its
// OWN budget: one shared cap between two samplers lets the frequent side
// starve the rare one, which this project has already paid for once.
std::atomic_uint32_t g_gunang_attempts{0};
std::atomic_uint32_t g_gunang_lines{0};
constexpr uint32_t kMaxGunAngleLines = 24;
// CHAINANGLES - see hook_render_gui_models. Own budget again: this samples the
// same rare condition GunWindow does and sharing a cap would starve one.
std::atomic_uint32_t g_chain_attempts{0};
std::atomic_uint32_t g_chain_lines{0};
constexpr uint32_t kMaxChainLines = 16;

// ---------------------------------------------------------------------------
// VIEW-WRITE CENSUS (2026-08-28). Step 1 of
// docs/PLAN-2026-08-28-EYE0-CONTAMINATION.md, and it is LOG ONLY: it changes
// no behaviour, installs no hook and mutates no engine memory. It records,
// per frame and per CALL SIDE, every write this DLL makes to the view
// matrices eye 0's own frame depends on, and whether a restore followed:
//
//   * Lever V -> source_ctx+0x11F0        GUI/viewmodel view. S1's suspect:
//                                         a viewmodel is drawn in view space
//                                         and must not move with the camera.
//   * Lever A -> renderView+0xC44/+0xC84  current and previous world view -
//                                         the pair DOOM differences to build
//                                         motion vectors. S2's suspect.
//   * Lever A -> the six sibling destination copies at DOOM+0x6FD5FC0..FE8,
//                each +0xC44 and +0xC84. Written by
//                apply_shift_to_world_vp_destinations and, unlike renderView,
//                covered by NO restore at all.
//   * Lever W -> CameraSrc+0x3AC0, the bounded WorldVpSource write probe.
//
// WHAT MAKES A ZERO READABLE. Every site records three counts, not one:
//   e  entries - the code path ran at all
//   w  writes  - it actually mutated memory
//   dd deduped - it ran and world_vp_shifted_this_frame short-circuited it
// "w=0" means three completely different things depending on e and dd, and
// reading it as "this site does not shift" while e is also 0 - the hook never
// ran - would close an avenue on instrument failure rather than on evidence.
// That is this probe's own confusable failure mode, and e is the control for
// it.
//
// SIDE CLASSIFICATION IS ITSELF SUSPECT, so it is measured instead of
// assumed. inside_frame_root_double and inside_real_frame_root are BOTH
// thread_local. Any of these sites that runs on a job worker thread rather
// than the render thread sees both flags false, and would otherwise be filed
// silently under "real". Those writes go into a third UNFLAGGED column with
// their thread id. A nonzero UNFLAGGED count is a finding in its own right:
// it means the restore snapshot guarded by inside_frame_root_double was never
// taken for that write, which is one of the plan's own candidates.
enum : int {
    kVwsLeverVGui = 0,
    kVwsLeverACur,
    kVwsLeverAPrev,
    kVwsLeverADest,
    kVwsLeverWSrc,
    kVwsCount
};
constexpr const char* kVwsSiteName[kVwsCount] = {
    "LeverV ctx+0x11F0 viewmodel",
    "LeverA renderView+0xC44 cur",
    "LeverA renderView+0xC84 prev",
    "LeverA dest[6]+0xC44/+0xC84",
    "LeverW CameraSrc+0x3AC0",
};
constexpr int kVwcSideReal = 0;
constexpr int kVwcSideDoubled = 1;
constexpr int kVwcSideUnflagged = 2;
constexpr const char* kVwcSideName[3] = {"real", "dbl", "UNFLAGGED"};

struct ViewWriteRow {
    uint32_t entries = 0;
    uint32_t writes = 0;
    uint32_t deduped = 0;
    float offset = 0.0f;    // g_stereo_offset the write was made with
    float before3 = 0.0f;   // matrix[3] before the translation
    float after3 = 0.0f;    // matrix[3] after it
    uintptr_t first_addr = 0;
    uintptr_t addrs[3]{};   // distinct destinations seen, first three
    uint32_t stored = 0;    // how many of addrs[] are filled
    uint32_t distinct = 0;  // distinct destinations seen; APPROXIMATE above 3
    uint32_t tid = 0;
};

std::mutex view_write_census_mutex;
ViewWriteRow view_write_census[kVwsCount][3];
char view_write_census_why[80] = "unarmed";
std::atomic_bool view_write_census_armed{false};
std::atomic_uint32_t view_write_census_frames_left{0};
std::atomic_uint32_t view_write_census_arm_serial{0};
// Three frames per arm, not one: candidate 5 in the plan is cross-frame
// accumulation, which a single-frame snapshot cannot show. Six lines a frame,
// so one arm costs eighteen log lines.
constexpr uint32_t kViewWriteCensusFrames = 3;
// Restores observed since the last emit. Counted at the restore sites in
// hook_frame_root, so "a restore followed" is recorded rather than inferred.
std::atomic_uint32_t vwc_leverv_restores{0};
// Engagement counters for the 2026-08-22 Lever V rebase. Declared here, above
// the census header that reports them, rather than beside the thread_locals
// they belong with further down - same reason xr_present_path_hits is up here.
// Counted SEPARATELY so "the rebase ran" and "the rebase had no clean basis"
// can never be read as one number.
std::atomic_uint32_t lever_v_clean_rebased{0};
std::atomic_uint32_t lever_v_clean_missing{0};
std::atomic_uint32_t vwc_renderview_restores{0};

int vwc_side() {
    if (inside_frame_root_double) return kVwcSideDoubled;
    if (inside_real_frame_root) return kVwcSideReal;
    return kVwcSideUnflagged;
}

void vwc_entry(int site) {
    if (!view_write_census_armed.load(std::memory_order_relaxed)) return;
    std::scoped_lock lock(view_write_census_mutex);
    ViewWriteRow& r = view_write_census[site][vwc_side()];
    ++r.entries;
    r.tid = GetCurrentThreadId();
}

void vwc_deduped(int site) {
    if (!view_write_census_armed.load(std::memory_order_relaxed)) return;
    std::scoped_lock lock(view_write_census_mutex);
    ++view_write_census[site][vwc_side()].deduped;
}

void vwc_write(int site, uintptr_t addr, float offset, const float* before, const float* after) {
    if (!view_write_census_armed.load(std::memory_order_relaxed)) return;
    std::scoped_lock lock(view_write_census_mutex);
    ViewWriteRow& r = view_write_census[site][vwc_side()];
    ++r.writes;
    r.offset = offset;
    r.before3 = before[3];
    r.after3 = after[3];
    r.tid = GetCurrentThreadId();
    if (r.first_addr == 0) r.first_addr = addr;
    bool seen = false;
    for (uint32_t i = 0; i < r.stored; ++i) {
        if (r.addrs[i] == addr) { seen = true; break; }
    }
    if (!seen) {
        ++r.distinct;
        if (r.stored < 3) r.addrs[r.stored++] = addr;
    }
}

void vwc_arm(const char* why) {
    {
        std::scoped_lock lock(view_write_census_mutex);
        for (auto& site : view_write_census)
            for (auto& row : site) row = ViewWriteRow{};
        std::snprintf(view_write_census_why, sizeof(view_write_census_why), "%s", why);
    }
    vwc_leverv_restores.store(0, std::memory_order_relaxed);
    vwc_renderview_restores.store(0, std::memory_order_relaxed);
    vwc_gun_unshifts.store(0, std::memory_order_relaxed);
    gun_unshift_budget.reset();
    view_write_census_frames_left.store(kViewWriteCensusFrames, std::memory_order_relaxed);
    const uint32_t arm = view_write_census_arm_serial.fetch_add(1, std::memory_order_relaxed) + 1;
    view_write_census_armed.store(true, std::memory_order_relaxed);
    log::warn(std::format(
        "ViewWriteCensus ARM #{} why='{}' frames={}. Log only - nothing is mutated by this probe.",
        arm, why, kViewWriteCensusFrames));
}

// Emitted from hook_frame_root AFTER the doubled call and its restores have
// run, so one block is one whole frame with both sides in it. The offsets are
// passed in rather than re-read: g_stereo_offset has already been put back by
// the time this runs, and re-reading it would print the resting value while
// claiming it was the value the writes used. The first version of this header
// did exactly that - it passed prev_offset as the real call's offset and
// printed real=0.000 for a frame whose own rows recorded off=-6.000. The rows
// were right and the header was wrong; it now takes the value the real call
// was actually given.
void vwc_emit_frame(float expected_real, float expected_doubled, bool full_frame, float ipd) {
    if (!view_write_census_armed.load(std::memory_order_relaxed)) return;
    ViewWriteRow snap[kVwsCount][3];
    char why[sizeof(view_write_census_why)];
    {
        std::scoped_lock lock(view_write_census_mutex);
        std::memcpy(snap, view_write_census, sizeof(snap));
        std::memcpy(why, view_write_census_why, sizeof(why));
        for (auto& site : view_write_census)
            for (auto& row : site) row = ViewWriteRow{};
    }
    const uint32_t arm = view_write_census_arm_serial.load(std::memory_order_relaxed);
    const uint32_t serial = g_frame_root_serial.load(std::memory_order_relaxed);
    const uint32_t lv = vwc_leverv_restores.exchange(0, std::memory_order_relaxed);
    const uint32_t rv = vwc_renderview_restores.exchange(0, std::memory_order_relaxed);
    const uint32_t gu = vwc_gun_unshifts.exchange(0, std::memory_order_relaxed);
    // SHADOW-ATLAS SUPPRESSION, REPORTED WHERE IT CAN ACTUALLY BE SEEN
    // (2026-08-28). suppress_doubled_shadow_draws has defaulted to true since
    // 2026-08-20 and its counter was only ever printed by scripted step 35 -
    // which this branch of the work parks out of the run. So an armed fix has
    // been running for every one of these runs with no evidence in any log that
    // it suppresses a single draw. Armed is not the same as firing, and a
    // shadow symptom is exactly when that difference matters. Printed
    // cumulatively: a number that does not climb between frames means the
    // doubled call is still writing DOOM's atlas, whatever the flag says.
    const uint64_t shadow_suppressed =
        suppressed_shadow_draws.load(std::memory_order_relaxed);
    const uintptr_t shadow_target =
        shared_shadow_framebuffer.load(std::memory_order_relaxed);
    log::warn(std::format(
        "VWC arm#{} f={} shadowSuppression={} suppressedShadowDrawsTotal={} atlasFb={:#x} "
        "compositeCapture={} blitOnly={} layoutOps={} xrPresentPathHits={} | a shadow total "
        "that does not climb between "
        "frames means the doubled call is still drawing into DOOM's atlas, and atlasFb=0 means "
        "the atlas was never identified. xrPresentPathHits=0 means the HEADSET path never ran, "
        "so the monitor is showing DOOM's own presented frame and NOTHING that only affects XR "
        "submission can explain anything seen in this run",
        arm, serial, suppress_doubled_shadow_draws.load(std::memory_order_relaxed),
        shadow_suppressed, shadow_target,
        !skip_composite_capture.load(std::memory_order_relaxed),
        frame_root_blit_only.load(std::memory_order_relaxed),
        !skip_mirror_layout_ops.load(std::memory_order_relaxed),
        xr_present_path_hits.load(std::memory_order_relaxed)));
    log::warn(std::format(
        "VWC arm#{} f={} why='{}' ipd={:.2f} fullFrame={} gunUnshift={} "
        "offsetUsed real={:.3f} doubled={:.3f} | thisFrame gunUnshiftSwaps={} leverVrestore={} "
        "renderViewRestore={} destCopies=0 (there is NO restore for the six dest copies - that "
        "zero is by construction, not a measurement) | leverVrebased={} leverVnoClean={}. "
        "gunUnshift=true with swaps=0 means RenderGuiModels never ran in this frame, which is a "
        "finding, not a null result. leverVrebased CLIMBING is how you know the 2026-08-22 "
        "rebase engaged; leverVnoClean CLIMBING means it did not and eye 1's GUI view is back "
        "to cancelling to the centre. Read the LeverV row below: with the rebase working, dbl "
        "must start from the CENTRE and land on the FAR side, not return to where the real "
        "call started",
        arm, serial, why, ipd, full_frame,
        gun_unshift_enabled.load(std::memory_order_relaxed),
        expected_real, expected_doubled, gu, lv, rv,
        lever_v_clean_rebased.load(std::memory_order_relaxed),
        lever_v_clean_missing.load(std::memory_order_relaxed)));
    for (int s = 0; s < kVwsCount; ++s) {
        std::string line = std::format("VWC arm#{} f={} {:<29}", arm, serial, kVwsSiteName[s]);
        for (int side = 0; side < 3; ++side) {
            const ViewWriteRow& r = snap[s][side];
            line += std::format(
                " {}[e={} w={} dd={} off={:.3f} m3={:.3f}->{:.3f} addr={:#x} distinct={}{} tid={}]",
                kVwcSideName[side], r.entries, r.writes, r.deduped, r.offset,
                r.before3, r.after3, r.first_addr, r.distinct,
                r.distinct > 3 ? "+" : "", r.tid);
        }
        log::warn(line);
    }
    if (view_write_census_frames_left.fetch_sub(1, std::memory_order_relaxed) <= 1) {
        view_write_census_armed.store(false, std::memory_order_relaxed);
        log::warn(std::format(
            "ViewWriteCensus COMPLETE arm#{} frames={} why='{}' | READING GUIDE: "
            "e=entries (the path ran), w=writes (it mutated memory), dd=deduped "
            "(it ran and world_vp_shifted_this_frame short-circuited it). w=0 WITH e=0 means "
            "the hook never ran and is NOT evidence that the site is inactive. Any nonzero "
            "count in the UNFLAGGED column means that site runs on a thread where "
            "inside_frame_root_double is false, so its real/doubled split is unreliable AND "
            "the restore snapshot guarded by that flag was never taken. distinct>1 on LeverV "
            "means the single-address restore covers only one of several destinations.",
            arm, kViewWriteCensusFrames, why));
    }
}

// ---------------------------------------------------------------------------
// DOUBLED-CALL FLOOR (2026-08-28). Its SHIFT+INSERT cycle was deleted 2026-09-03
// (it could reach rung 4, which HANGS). The level is now fixed at 1.
//
// THE FIRST LADDER IS SPENT, and it landed. Its three rungs were: no dispatched
// jobs, then also no gather/cull/sort, then also no SetupView. The census
// confirmed each rung engaged rather than being taken on trust:
//
//   rung 1   dbl LeverA e=1, LeverV/W e=1   flicker YES
//   rung 2   dbl LeverA e=0, LeverV/W e=1   flicker YES
//   rung 3   dbl LeverA e=0, LeverV/W e=0   flicker NO
//
// A clean single-variable delta between rungs 2 and 3: the doubled call's
// SETUPVIEW is what damages DOOM's next real frame. Not the dispatched jobs,
// not the gather/cull/sort family, not the camera offset (the flicker survives
// 0.0 separation, where no lever writes anything at all).
//
// Rung 3 turned BOTH SetupView variants off together, so gui versus world is
// still open. The rungs below split them, with jobs and gather left running
// normally so the answer is not entangled with a floor state.
//
// Defined here rather than beside doubled_floor_level because they need
// inside_frame_root_double, whose extern declaration is above this point.
bool doubled_skip_setup_world_now() {
    if (!inside_frame_root_double) return false;
    const int l = doubled_floor_level.load(std::memory_order_relaxed);
    return l == 1 || l == 3 || l == 4;
}
bool doubled_skip_setup_gui_now() {
    if (!inside_frame_root_double) return false;
    const int l = doubled_floor_level.load(std::memory_order_relaxed);
    return l == 2 || l == 3 || l == 4;
}
// Only rung 4, the retained floor - see the handler for why it is kept.
bool doubled_skip_gather_now() {
    return inside_frame_root_double &&
           doubled_floor_level.load(std::memory_order_relaxed) == 4;
}

// CLEAN BASIS FOR THE SIX DEST COPIES (2026-08-23). Same defect, same shape
// and same fix as the Lever V rebase - see lever_v_clean_matrix.
//
// The loop below was read -> apply_translation -> write with no clean basis and
// NO RESTORE, so the doubled call shifted whatever the real call had already
// shifted. Census at 'separation 12.0 uu':
//
//     LeverA dest[6]  real -17.000 -> 1.000     dbl 1.000 -> -17.000
//
// Eye 0 ends at +18, eye 1 ends back on the unshifted value: HALF the correct
// disparity, so everything drawn through these copies appears about twice as
// far away as it should and reads as sunk into the floor. That is the
// first-room symptom - health boxes, barrels, sparks, monitors, candle flames.
//
// ⚠ THE RESTORE IS NOT OPTIONAL HERE, unlike Lever V's. Lever V is repopulated
// by DOOM every frame so its basis cannot drift; whether DOOM repopulates these
// six is NOT established. Without a restore, rebasing would make each frame's
// real call start from the previous frame's doubled output and the shift would
// accumulate without bound - a far worse failure than the one being fixed. The
// restore writes the clean value back after the doubled frame root returns, so
// the next frame starts from DOOM's own value either way.
thread_local float dest_clean[6][2][16]{};
thread_local bool dest_clean_valid[6][2]{};
thread_local float dest_clean_m3_last = 0.0f;
std::atomic_uint32_t dest_rebased{0};
std::atomic_uint32_t dest_no_clean{0};
std::atomic_uint32_t dest_restored{0};
std::atomic_uint32_t dest_drift_warnings{0};

constexpr uintptr_t kWorldVpDestFirst = 0x6FD5FC0;
constexpr uintptr_t kWorldVpDestLast = 0x6FD5FE8;

// ===== REMOVED 2026-08-26, BUILD B ==========================================
//
// Four shotgun-era head-rotation sites lived here and were deleted in one
// build, whose SINGLE behavioural variable is these removals. The 14:47 run
// (vrot=world) measured every one of them, and the numbers are the reason:
//
//   rotate_camera_src_for_head  - 9342 writes/run into CameraSrc, and the
//       image was unchanged across every build that had it. Kept only as the
//       prime suspect for the weapon's ~2x counter-rotation: it is the one
//       removed site that plausibly feeds the viewmodel's placement.
//   scan_and_rotate_basis3      - basis3=0. Never fired, in any run.
//   rotate_world_vp_destinations_for_head (the SCAN-based dest rotation)
//                               - destCopies=0, destOffsetsFound=0. The scan
//       never found anything after it was gameplay-gated; before that it had
//       latched onto a LOADING matrix, which is the confound recorded in
//       PLAN-2026-08-26 section 3c.
//   the direct GPU ring-slot camera write (mvp rebase, at the descriptor
//       bind) - GPUCAM WRITES=0 for the whole run.
//
// WHAT REMAINS, deliberately and exactly: Lever A (renderView+0xC44/+0xC84),
// the FIXED-offset destination rotation inside
// apply_shift_to_world_vp_destinations ("destFixed"), and Lever V (the GUI
// view at ctx+0x11F0, armed by vrot=). Nothing else rotates a camera.
//
// The GPU CLASSIFIER (attempts/rotated/clean/other) is NOT removed - it is
// read-only, it is the instrument that produced the retirements above, and it
// is how this build's result gets read.

// DOOM's frame aspect, set from the present hook alongside wrap's copy. The
// world-view test is a ratio against it.
std::atomic<float> doom_frame_aspect{0.0f};

// ===== GPU CAMERA CLASSIFIER - see the call site in BindDescriptorSets =====
//
// Compares a camera-shaped uniform the GPU is being bound against DOOM's clean
// basis and against the head-rotated basis, by direction. Counts only; the
// per-second line in the present hook prints them.
std::atomic_uint64_t g_gpu_cam_rotated{0};
std::atomic_uint64_t g_gpu_cam_clean{0};
std::atomic_uint64_t g_gpu_cam_other{0};
std::atomic_uint64_t g_gpu_cam_attempts{0};
// g_gpu_cam_writes / g_gpu_cam_rejects / g_mvp_written removed with the direct
// ring-slot write in Build B (2026-08-26) - see the tombstone at the bind site.
// This classifier is now purely read-only.
std::atomic_uintptr_t g_gpu_cam_clean_pipeline{0};
std::atomic_uintptr_t g_gpu_cam_rotated_pipeline{0};
// The 2x census - see the block at the end of classify_gpu_camera.


// ===========================================================================
// PER-PIPELINE ROTATION RATE (2026-08-26) - THE METRIC THAT NEEDS NO JUDGEMENT
// ===========================================================================
//
// WHY THE PREVIOUS ONE WAS WRONG, and why this one cannot be. The ratio census
// divided a record's ABSOLUTE angle-from-clean by the head yaw. For a
// per-object matrix that absolute angle includes the OBJECT'S OWN orientation,
// so an object standing at 10 degrees read "2x" at 5 degrees of head yaw by
// pure coincidence. Arming a fix on it un-rotated innocent room geometry.
//
// A static orientation contributes ZERO to a RATE. So track, per pipeline, how
// the record's facing CHANGES against how the head's yaw CHANGES, and fit the
// slope through the origin:
//
//     slope = sum(da * dy) / sum(dy * dy)
//
//   slope ~ 0  -> does not follow the head at all (the HUD reticle)
//   slope ~ 1  -> exactly one head rotation. Correct, world-locked.
//   slope ~ 2  -> TWO. The over-rotated population - the gun.
//
// AND THE POINT: this is read straight out of the log. It does not ask the
// owner to judge relative motion in a headset and it does not depend on how
// that judgement is worded. Every run from here reports numbers I can act on
// by myself.
//
// Bounded by construction: a fixed 16-slot table keyed on pipeline handle, no
// allocation, updated only when the head actually moved.
// 16 -> 256. The 11:40 run reported overflow=974217: the table filled with the
// first sixteen pipelines it ever saw and dropped every later one, so the eight
// slopes it printed were the EARLIEST pipelines, not a survey. A cap hit that
// hard is truncation, and the sample was biased toward whatever binds first.
// How far into a large uniform block the wide sweep looks. 1 KB is 16 candidate
// matrices per descriptor - enough for a per-object block, bounded by design.
constexpr uint32_t kWideSweepBytes = 1024;
std::atomic_uint64_t g_wide_examined{0};
std::atomic_uint64_t g_wide_matched{0};

// Signed angle of the record's right axis about the reference up axis.
inline float signed_facing(const float* m, float l0,
                           const float* r, const float* u) {
    const float x = m[0] / l0, y = m[1] / l0, z = m[2] / l0;
    const float dot_r = x * r[0] + y * r[1] + z * r[2];
    const float cx = r[1] * z - r[2] * y;
    const float cy = r[2] * x - r[0] * z;
    const float cz = r[0] * y - r[1] * x;
    const float dot_u = cx * u[0] + cy * u[1] + cz * u[2];
    return std::atan2(dot_u, dot_r);
}

// Called from classify_gpu_camera, which already holds swapchains_mutex, so
// the table needs no lock of its own.
// ⚠ PER (PIPELINE, SET, BINDING), not per pipeline. 2026-08-26.
//
// The 13:03 run armed a per-PIPELINE correction and the screen "freaked out".
// That is the same error as the ratio census, one level up: a single pipeline
// binds many different matrices - world camera, shadow views, per-object MVPs -
// so its fitted slope is an AVERAGE over a heterogeneous set, and correcting
// every record by that average corrupts the ones that were already right.
//
// The set and binding split those apart, so each slope describes one kind of
// record rather than a blend of several.


// ===== THE GUI-VIEW DELTA (2026-08-26) =====================================
//
// WHY THIS EXISTS. Three corrections in a row scattered POST instead of
// converging, and the reason was the same each time: they removed
// renderView's rotation delta (A = VP_rot * inverse(VP_clean), built from the
// WORLD camera) from records that are not built on the world camera at all.
// The 16:40 run then refuted the composition theory outright - with Lever A's
// rotation off, the over-rotated key went to +3.04 rather than halving to
// -1.00 - so renderView is not in that chain and its delta was never the right
// thing to remove.
//
// Lever V has BOTH of its own matrices in hand at the moment it writes:
// m_before (DOOM's clean GUI view) and m_head (the same view with exactly one
// head rotation applied). Their ratio is that site's own delta, and removing
// it is a well-posed operation instead of a guess about which camera a record
// belongs to.
//
// The clean GUI view itself, kept alongside the delta so captures can record
// the exact input a correction would have had to work from.
// Guarded by a seq counter rather than a mutex: the bind hook only ever reads,
// and a torn read is harmless because it is checked for invertibility before
// use and the value is refreshed every frame.
std::mutex g_gui_delta_mutex;
alignas(16) float g_gui_delta[16]{};
alignas(16) float g_gui_clean[16]{};
bool g_gui_delta_valid = false;

void publish_gui_delta(const float* clean, const float* rotated) {
    alignas(16) float inv[16]{};
    if (!wrap::mat4_inverse(clean, inv)) return;
    alignas(16) float d[16]{};
    wrap::mat4_multiply(rotated, inv, d);
    for (int i = 0; i < 16; ++i) if (!std::isfinite(d[i])) return;
    std::scoped_lock lock(g_gui_delta_mutex);
    std::memcpy(g_gui_delta, d, sizeof(g_gui_delta));
    std::memcpy(g_gui_clean, clean, sizeof(g_gui_clean));
    g_gui_delta_valid = true;
}


bool gui_delta_inverse(float* out16) {
    alignas(16) float d[16]{};
    {
        std::scoped_lock lock(g_gui_delta_mutex);
        if (!g_gui_delta_valid) return false;
        std::memcpy(d, g_gui_delta, sizeof(d));
    }
    if (!wrap::mat4_inverse(d, out16)) return false;
    for (int i = 0; i < 16; ++i) if (!std::isfinite(out16[i])) return false;
    return true;
}
// ===== THE CAPTURE CORPUS (2026-08-26) =====================================
//
// WHY THIS EXISTS. Owner, verbatim: "why does EVERY run you ask me to do
// result in you saying - oh there was a bug in my test". The answer is that
// every run has carried a code path that had never executed before, so it
// asked two questions at once - is the model right, and does the new code work
// - and a defect in the second destroys the answer to the first.
//
// This breaks that loop. It dumps REAL records from the over-rotated band,
// with the head yaw and the clean GUI view that were live at the time, in a
// form tests/wraparound_math_test.cpp can replay. Once a corpus exists, any
// future correction can be proved to produce the target slope OFFLINE, at no
// cost to the owner, before it is ever armed in a headset.
//
// One capture run pays for every iteration after it.
// ⚠ STRATIFIED BY SLOPE, AND NEVER GATED ON THE HYPOTHESIS.
//
// Two ways this could have silently missed the very thing it exists to record,
// both now designed out:
//   * A flat first-N cap fills with whatever binds first. If the weapon binds
//     late, or rarely, it is never in the corpus. Buckets fix that: every
//     population present gets slots reserved for it whatever its rate and
//     whatever order it arrives in.
//   * The first version was called from INSIDE the correction's band gate, so
//     a band that excluded the gun would have excluded it from the capture as
//     well - the capture would have confirmed my own filter rather than
//     recording the truth. It is now called unconditionally for every record
//     with a determined slope.

// ===== FREQHIGH CENSUS (2026-08-28) =======================================
// Phase 1 of docs/HANDOFF-2026-08-28-FREQHIGH-IMPLEMENTATION-SCRIPT.md.
// READ-ONLY census of freqHigh_vertexUniforms records - the gun's actual
// transform surface (skinned 160-byte layout, mvp at +16; rigid 128-byte,
// mvp at +0). Answers ONE question: how many head rotations do the viewmodel
// records' implied VP (X = mvp*inv(model)) and model matrix each carry?
// That pair picks the Phase-2 correction mode. All math is header functions
// covered by tests/freqhigh_record_test.cpp (84 checks, real capture
// fixtures). Called from the descriptor walk under swapchains_mutex, so the
// accumulator structs need no lock of their own (the same lock-free rule the deleted PipeRate table used).
namespace fh = kharvoxnative::freqhigh;

// EVERY early return counts (standing rule: silent returns are forbidden).
std::atomic_uint64_t g_fh_seen160{0};        // 160-byte records offered
std::atomic_uint64_t g_fh_seen128{0};        // 128-byte records offered
std::atomic_uint64_t g_fh_parse_fail{0};     // parse() refused (never, by construction)
std::atomic_uint64_t g_fh_no_ref{0};         // clean world VP / its origin unavailable
std::atomic_uint64_t g_fh_not_rigid{0};      // model 3x3 not orthonormal (gore props)
std::atomic_uint64_t g_fh_fwd_not_unit{0};   // X row3 not unit (not P*V shaped)
std::atomic_uint64_t g_fh_origin_unsolved{0};// X's camera origin Cramer solve failed
std::atomic_uint64_t g_fh_origin_mismatch{0};// solved origin >8 from our reference
std::atomic_uint64_t g_fh_far_eye{0};        // model origin >100 from the eye (monsters)
std::atomic_uint64_t g_fh_not_slab{0};       // depth slab absent or a != 0.100 (world)
std::atomic_uint64_t g_fh_viewmodel{0};      // passed the full five-gate classifier
std::atomic_uint64_t g_fh_dedup{0};          // same record pointer again this frame
std::atomic_uint64_t g_fh_no_head{0};        // viewmodel seen but no head pose yet
std::atomic_uint64_t g_fh_small_yaw{0};      // viewmodel seen, |yaw| under 5 deg
std::atomic_uint64_t g_fh_no_basis{0};       // viewmodel seen, reference basis absent

// Running means for the heartbeat. Written only under swapchains_mutex;
// racy-read from the present thread like every other per-second line.
std::atomic<double> g_fh_gun_eyedist_sum{0.0};
std::atomic_uint64_t g_fh_gun_eyedist_n{0};
std::atomic<double> g_fh_slab_a_sum{0.0};
std::atomic_uint64_t g_fh_slab_a_n{0};

// Slope accumulators, the deleted PipeRate census's delta-fit pattern (+-pi wrap,
// same dy gates). Indexed [side][which][metric]:
//   side   0 = REAL pass, 1 = DOUBLED pass (inside_frame_root_double)
//   which  0 = arms (eye_dist < 5), 1 = gun (5 <= eye_dist < 100)
//   metric 0 = V (X's camera forward), 1 = M (model forward, column 0)
struct FhSlope {
    float last_angle{0.0f};
    float last_yaw{0.0f};
    bool have_last{false};
    double sum_ay{0.0};
    double sum_yy{0.0};
    uint32_t samples{0};
};
FhSlope g_fh_slope[2][2][2]{};

void fh_slope_note(FhSlope& s, float angle, float yaw) {
    if (s.have_last) {
        float da = angle - s.last_angle;
        while (da > 3.14159265f)  da -= 6.28318531f;
        while (da < -3.14159265f) da += 6.28318531f;
        float dy = yaw - s.last_yaw;
        while (dy > 3.14159265f)  dy -= 6.28318531f;
        while (dy < -3.14159265f) dy += 6.28318531f;
        // Same gates the PipeRate census used: head actually moved, no
        // ambiguous wrap, no torn-frame outlier.
        if (std::fabs(dy) > 0.0035f && std::fabs(dy) < 0.5f && std::fabs(da) < 1.5f) {
            s.sum_ay += static_cast<double>(da) * dy;
            s.sum_yy += static_cast<double>(dy) * dy;
            ++s.samples;
        }
    }
    s.last_angle = angle;
    s.last_yaw = yaw;
    s.have_last = true;
}

float fh_slope_value(const FhSlope& s) {
    if (s.samples < 8 || !(s.sum_yy > 0.0))
        return std::numeric_limits<float>::quiet_NaN();
    return static_cast<float>(s.sum_ay / s.sum_yy);
}

// WORLD REFERENCE (2026-08-28, post-Phase-1). Phase 1 measured the gun's
// V at +1.00 in THIS instrument's convention while PIPERATE's world
// population reads -1.00 in ITS convention (signed_facing on the right
// axis vs signed_yaw on the forward axis - the signs are NOT comparable
// across the two instruments). The correction magnitude in script §2.1 is
// (V_gun - V_world), so V_world must be measured by the SAME function on
// the SAME axis against the SAME basis. Baked world geometry's 128-byte
// record has model == identity, so its mvp IS the world view-projection:
// feed its forward through the identical signed_yaw path. Gate mirrors
// rdc_gun5_verify's world-VP finder (identity model) plus the world's own
// depth-slab signature a~1.0 (excludes HUD ortho and any light view;
// prepass/shadow consumers of THIS record don't exist per the captures).
std::atomic_uint64_t g_fh_worldref{0};       // world-VP records recognised
FhSlope g_fh_world_v[2]{};                   // [side] - V slope only, no model
// The latest world VP per side, latched in the worldref branch (under
// swapchains_mutex) every time an identity-model world record binds - the
// substitution target for gunfix2=worldp. Hundreds bind per pass, so this is
// at most one pass stale when read at submit time. Guarded by the same
// swapchains_mutex on write; reads at submit take g_fh_tracked_mutex only,
// so a copy is taken under a tiny seqlock instead of sharing the big lock.
std::atomic_uint32_t g_fh_wvp_seq[2]{};
float g_fh_wvp[2][16]{};
std::atomic_bool g_fh_wvp_valid[2]{};

// THE PROJECTION-SWAP CONSTANT C = VP_world * inv(VP_weapon) = P_world *
// inv(P_weapon): the camera V cancels (gun9 Q2: sibling mvps equal forward
// mvps bit-for-bit; both projections are camera-free), so C is
// frame-invariant - no per-record model matrix, no lag, and the SAME C on
// the color and depth records keeps them byte-coherent. It only changes
// when the scene/FOV changes, so it is refreshed whenever a SAME-FRAME
// weapon-VP + world-VP pair exists for a side and the staleness between
// refreshes is invisible. mvp' = C * mvp.
std::atomic_uint32_t g_fh_c_seq[2]{};
float g_fh_c[2][16]{};
std::atomic_bool g_fh_c_valid[2]{};
// The weapon VP (X of a classified forward record) with the frame it was
// latched in, so C is only computed from a same-frame pair.
float g_fh_xw[2][16]{};
uint64_t g_fh_xw_frame[2]{};
std::atomic_uint64_t g_gf2_c_refreshes{0};

bool fh_read_c(int side, float out[16]) {
    if (!g_fh_c_valid[side].load(std::memory_order_relaxed)) return false;
    for (int a = 0; a < 8; ++a) {
        const uint32_t before = g_fh_c_seq[side].load(std::memory_order_acquire);
        if (before & 1u) continue;
        std::memcpy(out, g_fh_c[side], 64);
        if (g_fh_c_seq[side].load(std::memory_order_acquire) == before)
            return true;
    }
    return false;
}




void fh_latch_world_vp(int side, const float X[16]) {
    const uint32_t s = g_fh_wvp_seq[side].load(std::memory_order_relaxed);
    g_fh_wvp_seq[side].store(s + 1, std::memory_order_release);
    std::memcpy(g_fh_wvp[side], X, 64);
    g_fh_wvp_seq[side].store(s + 2, std::memory_order_release);
    g_fh_wvp_valid[side].store(true, std::memory_order_relaxed);
}

bool fh_read_world_vp(int side, float out[16]) {
    if (!g_fh_wvp_valid[side].load(std::memory_order_relaxed)) return false;
    for (int a = 0; a < 8; ++a) {
        const uint32_t before = g_fh_wvp_seq[side].load(std::memory_order_acquire);
        if (before & 1u) continue;
        std::memcpy(out, g_fh_wvp[side], 64);
        if (g_fh_wvp_seq[side].load(std::memory_order_acquire) == before)
            return true;
    }
    return false;
}

// FHCTRL - the positive control (script §2.2), armed by slot 1 in-run.
// +30 deg world yaw through rotate_model_about_pivot + recompose_mvp into
// the classified viewmodel records. Every outcome counted; POST re-parses
// the written bytes. meanDelta doubles as a SIGN CALIBRATION: the written
// rotation is +30 deg right-handed about the reference up axis, so the
// POST-minus-PRE model yaw (+0.524 or -0.524) pins which physical rotation
// direction this census's signed_yaw calls positive.
std::atomic_uint64_t g_fh_ctrl_applied{0};
std::atomic_uint64_t g_fh_ctrl_refused{0};
std::atomic_uint64_t g_fh_ctrl_post_ok{0};   // written bytes still classify as viewmodel
std::atomic_uint64_t g_fh_ctrl_post_bad{0};
std::atomic<double> g_fh_ctrl_delta_sum{0.0};
std::atomic_uint64_t g_fh_ctrl_delta_n{0};

// ===== SUBMIT-TIME LAYER (2026-08-28 evening) ==============================
// The bind-time control PROVED (applied=896 postOk=896 meanDelta=-0.524, gun
// black but unmoved): the GPU consumes this record's modelmatrix as we wrote
// it, but NOT its mvpmatrix - a late CPU writer recomposes the MVP field
// (only) between our bind-time write and GPU consumption. So the classified
// records are re-examined at vkQueueSubmit: diff the bytes against bind time
// (catches the late write in the act, names WHICH field), census the FINAL
// mvp (what rotation the picture actually gets), and optionally re-apply the
// control there, mvp-only (slot 1) - last-writer-wins is the fix's shape.
struct FhTracked {
    unsigned char* host{nullptr};
    uint32_t range{0};        // 160 or 128
    uint8_t side{0};          // 0 REAL, 1 DBL
    uint8_t which{0};         // 0 arms, 1 gun
    bool sibling{false};      // 208/496 layout: mvp-only handling, no parse
    float last_seen[40]{};    // first 160 bytes at bind, then per submit check
    float our_mvp[16]{};      // the mvp ctrl2 last wrote here
    bool ctrl2_written{false};
};
// 24: the measured per-frame population is 2 passes x (2 forward-160 +
// 1 forward-496 + 3 prepass-208) = 12 (gun9) plus up to 2 passes x
// (1 ss-fragment + 1 ss-compute) = 4 more, and a cap hit exactly is
// truncation.
constexpr uint32_t kFhTrackedMax = 24;
FhTracked g_fh_tracked[kFhTrackedMax];
std::atomic_uint32_t g_fh_tracked_n{0};      // cleared at gather-prepare
// LEAF mutex for the tracked table. The submit hook must NOT take
// swapchains_mutex: the mod's own present-time copy submits re-enter
// hook_queue_submit while holding it (std::mutex, non-recursive = deadlock).
// Order is always swapchains_mutex -> g_fh_tracked_mutex, never reversed.
std::mutex g_fh_tracked_mutex;

std::atomic_uint64_t g_fh_sub_checks{0};
std::atomic_uint64_t g_fh_sub_mvp_changed{0};    // mvp differs from last look
std::atomic_uint64_t g_fh_sub_model_changed{0};  // modelmatrix differs
std::atomic_uint64_t g_fh_sub_other_changed{0};  // skin flag / ambient differ
std::atomic_uint64_t g_fh_sub_reparse_fail{0};   // final bytes no longer classify
FhSlope g_fh_sub_slope[2][2][2]{};               // [side][arms/gun][V/M], SUBMIT state
std::atomic_uint32_t g_fh_sub_dumped{0};         // FHSUBDUMP cap, mirrors FHDUMP

std::atomic_uint64_t g_fh_ctrl2_applied{0};
std::atomic_uint64_t g_fh_ctrl2_refused{0};
std::atomic_uint64_t g_fh_ctrl2_post_ok{0};
std::atomic_uint64_t g_fh_ctrl2_post_bad{0};

// GUNFIX2 = the world-projection substitution (gunfix2=worldp + slot 3).
// mvp' = VP_world[side] * model - the gun becomes an ordinary world object:
// correct angular size, walls-rate motion under head turns, real depth.
std::atomic_uint64_t g_gf2_applied{0};       // forward records rewritten
std::atomic_uint64_t g_gf2_refused{0};       // non-finite result, never written
std::atomic_uint64_t g_gf2_no_wvp{0};        // no world VP latched for the side
std::atomic_uint64_t g_gf2_post_ok{0};       // read-back == intended bytes
std::atomic_uint64_t g_gf2_post_bad{0};
std::atomic_uint64_t g_gf2_pre_applied{0};   // prepass siblings rewritten
std::atomic_uint64_t g_gf2_pre_unmatched{0}; // prepass record, no fwd match
std::atomic_uint32_t g_gf2_dumped{0};        // FHFIXDUMP cap
// The engine rewrote the mvp AFTER a ctrl2 write (detected at the next
// check). clobbered climbing with the gun unmoved = the writer runs after
// even the last submit, and the watchpoint round is next.
std::atomic_uint64_t g_fh_ctrl2_clobbered{0};

// +30deg RIGHT-HANDED rotation about the reference up axis (Rodrigues).
// Shared by both controls so their signs are comparable.
bool fh_build_r30(float R9[9]) {
    float cr[3], cu[3], cf[3];
    if (!wrap::reference_basis(cr, cu, cf)) return false;
    constexpr float t = 0.5235988f;
    const float c = std::cos(t), s = std::sin(t);
    const float ux = cu[0], uy = cu[1], uz = cu[2];
    R9[0] = c + ux * ux * (1 - c);      R9[1] = ux * uy * (1 - c) - uz * s; R9[2] = ux * uz * (1 - c) + uy * s;
    R9[3] = uy * ux * (1 - c) + uz * s; R9[4] = c + uy * uy * (1 - c);      R9[5] = uy * uz * (1 - c) - ux * s;
    R9[6] = uz * ux * (1 - c) - uy * s; R9[7] = uz * uy * (1 - c) + ux * s; R9[8] = c + uz * uz * (1 - c);
    return true;
}
// One world sample per side per frame, so the world accumulator advances at
// the same cadence as the gun's (a cross-population slope comparison should
// sample both at the same frame positions). Cleared with the dedupe below.
bool g_fh_worldref_seen[2]{};

// Per-frame dedupe: one sample per record per frame, whatever its bind
// count (~10 consumer draws share one gun record). Entries written only
// under swapchains_mutex; the count is atomic so the unconditional clear in
// hook_render_gather_prepare (frame start, before any gameplay bind) needs
// no lock. 16 slots is 4x the expected population (2 records x 2 passes).
constexpr int kFhDedupSlots = 16;
// ===== THE CENSUS'S OWN LOCK (2026-08-30) ==================================
// Until today every one of these globals was serialised for free, because the
// descriptor walk that writes them held swapchains_mutex exclusively for its
// whole duration. That exclusivity is what cost the framerate and it is now a
// shared_lock, so the census state needs protecting on its own account -
// otherwise the perf fix would have quietly introduced a data race on the
// dedup ring and the world-reference latches.
//
// It is a SHORT hold (a scan of a few slots) taken ~270 times a frame, against
// 1667 whole-walk holds before, so it does not reintroduce what was removed.
// Lock order is always g_census_mutex -> g_fh_tracked_mutex, never the
// reverse; the submit-time block takes g_fh_tracked_mutex alone and calls no
// note function, so there is no path that could invert it.
// DBW: the descriptor-bind walk's own split - lock wait vs body vs volume.
std::atomic_uint64_t g_dbw_lock_ns{0};
std::atomic_uint64_t g_dbw_body_ns{0};
std::atomic_uint64_t g_dbw_calls{0};
std::atomic_uint64_t g_dbw_sets{0};
std::atomic_uint64_t g_dbw_bindings{0};
// Time inside the REAL vkCmdBindDescriptorSets. FbHook times the whole span,
// driver call included, so every "11.9 us per call" figure quoted so far has
// been ours PLUS the driver. Those have completely different fixes.
std::atomic_uint64_t g_dbw_tramp_ns{0};
std::atomic_uint64_t g_dbw_snapshot_read{0};
std::atomic_uint64_t g_dbw_snapshot_skipped{0};
std::mutex g_census_mutex;
uintptr_t g_fh_seen_ptrs[kFhDedupSlots]{};
std::atomic_uint32_t g_fh_seen_count{0};

// FIRST-8 RAW DUMP - the offline cross-check corpus. Gameplay-gated (the
// standing first-N rule: an ungated cap samples the loading screen).
std::atomic_uint32_t g_fh_dumped{0};
constexpr uint32_t kFhMaxDumps = 8;

// ===== THE CLEAN-CAMERA REFERENCE, MEMOISED (2026-08-30) ===================
//
// WHY THIS EXISTS, with the arithmetic that found it. The 09:31 headset run
// measured hook_cmd_bind_descriptor_sets at 11.40 + 9.38 ms PER FRAME across
// the real and doubled calls - 20.8 ms of a 31-45 ms frame, and it is our
// code, not DOOM's. In the same run FHCENSUS reported seen160=247,337 and
// seen128=1,438,324 over 5,691 frames: 1.69 MILLION records, ~296 per frame,
// each one calling clean_world_copy (a seqlock read with two acquire loads and
// a 64-byte memcpy) and then solve_cam_origin (Cramer's rule, FOUR 3x3
// determinants in double precision, plus three 3x3 memcpys).
//
// Both depend ONLY on the clean world VP, which is published exactly ONCE PER
// FRAME. So a per-frame value was being recomputed ~296 times per frame, on
// every thread, inside the hottest hook in the build. This is the per-item
// helper priced wrongly when the loop widened.
//
// The cache is keyed on wrap::clean_world_seq(), the publish sequence itself,
// so it is behaviour-identical BY CONSTRUCTION: same input, same output, just
// not recomputed. It is NOT a discovery filter - no record is skipped and no
// gate is changed - so gunfix2's engagement cannot move. Its own proof is
// arithmetic and appears on the FHCENSUS line: refSolved should track FRAMES
// while refServed tracks RECORDS, and the ratio between them is the work
// removed. thread_local because the hook runs on DOOM's job threads and a
// shared cache would just move the contention rather than remove it.
struct FhCleanRef {
    uint32_t seq{UINT32_MAX};
    bool ok{false};
    alignas(16) float cw[16]{};
    float ref[3]{};
};
thread_local FhCleanRef tl_fh_clean_ref;
std::atomic_uint64_t g_fh_ref_solved{0};   // the expensive path actually ran
std::atomic_uint64_t g_fh_ref_served{0};   // answered from the per-frame cache
std::atomic_uint64_t g_fh_ref_ns{0};       // ns spent inside the census, per frame window
std::atomic_uint64_t g_fh_perf_frames{0};  // frames in that same window - the divisor

bool fh_clean_ref(float cw_out[16], float ref_out[3]) {
    const uint32_t s = wrap::clean_world_seq();
    // Recompute on a new publish, and also whenever the last attempt failed -
    // a failed read must never be latched for the rest of the frame, which is
    // how a pre-gameplay zero gets reported for a whole session.
    if (tl_fh_clean_ref.seq != s || !tl_fh_clean_ref.ok) {
        alignas(16) float cw[16];
        float ref[3];
        if (wrap::clean_world_copy(cw) && fh::solve_cam_origin(cw, ref)) {
            std::memcpy(tl_fh_clean_ref.cw, cw, sizeof(cw));
            std::memcpy(tl_fh_clean_ref.ref, ref, sizeof(ref));
            tl_fh_clean_ref.seq = s;
            tl_fh_clean_ref.ok = true;
        } else {
            tl_fh_clean_ref.ok = false;
        }
        g_fh_ref_solved.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_fh_ref_served.fetch_add(1, std::memory_order_relaxed);
    }
    if (!tl_fh_clean_ref.ok) return false;
    std::memcpy(cw_out, tl_fh_clean_ref.cw, sizeof(float) * 16);
    std::memcpy(ref_out, tl_fh_clean_ref.ref, sizeof(float) * 3);
    return true;
}

void freqhigh_census_note(const float* data, uint32_t byte_range,
                          unsigned char* host) {
    // See g_census_mutex: the descriptor walk that calls this now holds only a
    // SHARED lock, so this state is no longer serialised for free.
    std::scoped_lock census_lock(g_census_mutex);
    // Times the whole census body, so the 20.8 ms/frame attribution is
    // MEASURED in the next run rather than asserted from this reasoning.
    const uint64_t fh_t0 = fb_ns();
    struct FhTimer {
        uint64_t t0;
        ~FhTimer() { g_fh_ref_ns.fetch_add(fb_ns() - t0, std::memory_order_relaxed); }
    } fh_timer_{fh_t0};
    if (byte_range == 160) g_fh_seen160.fetch_add(1, std::memory_order_relaxed);
    else                   g_fh_seen128.fetch_add(1, std::memory_order_relaxed);

    fh::Record r;
    if (!fh::parse(data, byte_range, r)) {
        g_fh_parse_fail.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Reference origin: OUR clean world VP's solved camera. Self-contained -
    // no CamHunt, no globalvieworigin (that one is centre-eye and carries our
    // half-IPD shift; the script forbids it as a precision reference).
    alignas(16) float cw[16];
    float ref[3];
    // Was: clean_world_copy + solve_cam_origin, per record, ~296 times a frame
    // for a value that changes once a frame. See fh_clean_ref above.
    if (!fh_clean_ref(cw, ref)) {
        g_fh_no_ref.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const fh::Verdict v = fh::validate(r, ref, 100.0f);
    // WORLD REFERENCE branch: identity-model rigid record = the world VP
    // itself. Sampled BEFORE the viewmodel ladder (it would die at near_eye
    // there and pollute farEye's meaning as the monster counter).
    if (r.kind == fh::Kind::Rigid128) {
        const float* m = r.model;
        const bool identity =
            std::fabs(m[0] - 1.0f) < 1e-3f && std::fabs(m[5] - 1.0f) < 1e-3f &&
            std::fabs(m[10] - 1.0f) < 1e-3f && std::fabs(m[1]) < 1e-3f &&
            std::fabs(m[2]) < 1e-3f && std::fabs(m[4]) < 1e-3f &&
            std::fabs(m[6]) < 1e-3f && std::fabs(m[8]) < 1e-3f &&
            std::fabs(m[9]) < 1e-3f && std::fabs(m[3]) < 1e-3f &&
            std::fabs(m[7]) < 1e-3f && std::fabs(m[11]) < 1e-3f;
        if (identity && v.rigid && v.fwd_unit && v.origin_solved &&
            v.origin_matches && v.slab.ok &&
            std::fabs(v.slab.a - 1.0f) < 0.05f) {
            g_fh_worldref.fetch_add(1, std::memory_order_relaxed);
            const int wside = inside_frame_root_double ? 1 : 0;
            fh_latch_world_vp(wside, v.X);
            // Refresh C from a SAME-FRAME (weaponVP, worldVP) pair. World
            // records bind after the forward viewmodel draws in each pass
            // (gun9 Q3), so by the time we are here the pass's X_w is
            // usually already latched for this frame. Runs under
            // swapchains_mutex like the rest of the intake.
            if (g_fh_xw_frame[wside] ==
                presented_frame.load(std::memory_order_relaxed)) {
                alignas(16) float inv_xw[16], C[16];
                if (wrap::mat4_inverse(g_fh_xw[wside], inv_xw)) {
                    wrap::mat4_multiply(v.X, inv_xw, C);
                    bool finite = true;
                    for (int i = 0; finite && i < 16; ++i)
                        finite = std::isfinite(C[i]);
                    if (finite) {
                        const uint32_t s =
                            g_fh_c_seq[wside].load(std::memory_order_relaxed);
                        g_fh_c_seq[wside].store(s + 1, std::memory_order_release);
                        std::memcpy(g_fh_c[wside], C, 64);
                        g_fh_c_seq[wside].store(s + 2, std::memory_order_release);
                        g_fh_c_valid[wside].store(true, std::memory_order_relaxed);
                        g_gf2_c_refreshes.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                // One refresh per frame per side is plenty; drop the tag so
                // the hundreds of later world records this frame skip the
                // inverse.
                g_fh_xw_frame[wside] = UINT64_MAX;
            }
            if (!g_fh_worldref_seen[wside]) {
                float hy = 0.0f, hp = 0.0f, hr = 0.0f;
                float cr[3], cu[3], cf[3], wf[3];
                if (wrap::residual_head_angles(hy, hp, hr) &&
                    wrap::reference_basis(cr, cu, cf) &&
                    std::fabs(hy) > 0.0873f && fh::vp_forward(v.X, wf)) {
                    g_fh_worldref_seen[wside] = true;
                    fh_slope_note(g_fh_world_v[wside],
                                  fh::signed_yaw(wf, cr, cf), hy);
                }
            }
            return;
        }
    }
    // Count each failing gate, first gate that fails wins the counter.
    if (!v.rigid) { g_fh_not_rigid.fetch_add(1, std::memory_order_relaxed); return; }
    if (!v.fwd_unit) { g_fh_fwd_not_unit.fetch_add(1, std::memory_order_relaxed); return; }
    if (!v.origin_solved) { g_fh_origin_unsolved.fetch_add(1, std::memory_order_relaxed); return; }
    if (!v.origin_matches) { g_fh_origin_mismatch.fetch_add(1, std::memory_order_relaxed); return; }
    if (!v.near_eye) { g_fh_far_eye.fetch_add(1, std::memory_order_relaxed); return; }
    if (!fh::is_viewmodel(v)) { g_fh_not_slab.fetch_add(1, std::memory_order_relaxed); return; }
    g_fh_viewmodel.fetch_add(1, std::memory_order_relaxed);
    // Latch this side's WEAPON VP with its frame, for the C refresh in the
    // worldref branch (C = VP_world * inv(VP_weapon), camera cancels).
    {
        const int xside = inside_frame_root_double ? 1 : 0;
        std::memcpy(g_fh_xw[xside], v.X, 64);
        g_fh_xw_frame[xside] = presented_frame.load(std::memory_order_relaxed);
    }

    // Dedupe AFTER classification, so seen/viewmodel keep their meaning as
    // raw offer counts and dedup counts exactly the multi-draw re-binds.
    const uintptr_t key = reinterpret_cast<uintptr_t>(host);
    const uint32_t n = g_fh_seen_count.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < n && i < kFhDedupSlots; ++i)
        if (g_fh_seen_ptrs[i] == key) {
            g_fh_dedup.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    if (n < kFhDedupSlots) {
        g_fh_seen_ptrs[n] = key;
        g_fh_seen_count.store(n + 1, std::memory_order_relaxed);
    }

    // Heartbeat means.
    const bool is_gun = v.eye_dist >= 5.0f;
    if (is_gun) {
        g_fh_gun_eyedist_sum.store(
            g_fh_gun_eyedist_sum.load(std::memory_order_relaxed) + v.eye_dist,
            std::memory_order_relaxed);
        g_fh_gun_eyedist_n.fetch_add(1, std::memory_order_relaxed);
    }
    g_fh_slab_a_sum.store(
        g_fh_slab_a_sum.load(std::memory_order_relaxed) + v.slab.a,
        std::memory_order_relaxed);
    g_fh_slab_a_n.fetch_add(1, std::memory_order_relaxed);

    // Track this record for the submit-time layer (bytes now, re-examined at
    // every vkQueueSubmit this frame). Leaf mutex on top of the intake's
    // swapchains_mutex; the count is cleared at gather-prepare with the
    // dedupe.
    {
        std::scoped_lock tracked_lock(g_fh_tracked_mutex);
        const uint32_t tn = g_fh_tracked_n.load(std::memory_order_relaxed);
        if (tn < kFhTrackedMax) {
            FhTracked& t = g_fh_tracked[tn];
            t.host = host;
            t.range = byte_range;
            t.side = inside_frame_root_double ? 1 : 0;
            t.which = is_gun ? 1 : 0;
            t.sibling = false;
            std::memcpy(t.last_seen, data, byte_range);
            t.ctrl2_written = false;
            g_fh_tracked_n.store(tn + 1, std::memory_order_relaxed);
        }
    }

    // Facing metrics -> slope accumulators.
    float hy = 0.0f, hp = 0.0f, hr = 0.0f;
    if (!wrap::residual_head_angles(hy, hp, hr)) {
        g_fh_no_head.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    float cr[3], cu[3], cf[3];
    if (!wrap::reference_basis(cr, cu, cf)) {
        g_fh_no_basis.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // FIRST-8 RAW DUMP before the yaw gate, so the corpus also holds
    // at-rest records (the offline fixtures were captured at rest too).
    // Gameplay-gated per the standing first-N rule.
    {
        wrap::VpSnapshot gvp;
        if (g_fh_dumped.load(std::memory_order_relaxed) < kFhMaxDumps &&
            wrap::world_vp(gvp) && wrap::is_gameplay_view(gvp.m) &&
            g_fh_dumped.fetch_add(1, std::memory_order_relaxed) < kFhMaxDumps) {
            float xf[3] = {0, 0, 0}, mf[3];
            fh::vp_forward(v.X, xf);
            fh::model_forward(r.model, mf);
            std::string raw;
            const uint32_t nfloats = byte_range / 4;
            for (uint32_t i = 0; i < nfloats; ++i)
                raw += std::format("{}{:.6f}", i ? " " : "", data[i]);
            log::warn(std::format(
                "FHDUMP kind={} side={} eyeDist={:.2f} slabA={:.4f} slabB={:.4f} "
                "vpYaw={:.4f} mYaw={:.4f} headYaw={:.4f} raw=[{}]",
                byte_range, inside_frame_root_double ? "DBL" : "REAL",
                v.eye_dist, v.slab.a, v.slab.b,
                fh::signed_yaw(xf, cr, cf), fh::signed_yaw(mf, cr, cf), hy, raw));
        }
    }
    // ===== FHCTRL - the BIND-TIME positive control: DISCONNECTED ==========
    // Its one run (18:04) delivered the verdict and it is banked: applied=896
    // refused=0 postOk=896 meanDelta=-0.524, gun BLACK (modelmatrix consumed)
    // but UNMOVED (mvpmatrix not consumed as written) -> a late CPU writer
    // recomposes the MVP field after bind time. Slot 1 now arms the
    // SUBMIT-TIME control in hook_queue_submit instead; this block is hard
    // false so a reassigned slot cannot re-arm a retired write site.
    // Same 5-degree engagement threshold as PIPERATE: below it the head is
    // at rest and a delta sample is noise.
    if (std::fabs(hy) <= 0.0873f) {
        g_fh_small_yaw.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const int side = inside_frame_root_double ? 1 : 0;
    const int which = is_gun ? 1 : 0;
    float xf[3], mf[3];
    if (fh::vp_forward(v.X, xf))
        fh_slope_note(g_fh_slope[side][which][0], fh::signed_yaw(xf, cr, cf), hy);
    fh::model_forward(r.model, mf);
    fh_slope_note(g_fh_slope[side][which][1], fh::signed_yaw(mf, cr, cf), hy);
}

// SIBLING intake: the viewmodel family's OTHER layouts (gun9): 208-byte
// depth-prepass records and the 496-byte forward variant, mvp at +16, no
// modelmatrix our Record models. Classified by the crushed depth slab on the
// RAW mvp (a=0.100 - the relation survives right-multiplication by the model
// matrix, and gun9 Q1 measured it at zero false positives across all 1328
// records of the frame: every slab-0.10 record IS the viewmodel family).
// Tracked for the submit-time C correction; no census sampling.
std::atomic_uint64_t g_fh_sib_seen{0};       // 208/496 records offered
std::atomic_uint64_t g_fh_sib_viewmodel{0};  // passed the slab gate

void freqhigh_sibling_note(const float* data, uint32_t byte_range,
                           unsigned char* host) {
    std::scoped_lock census_lock(g_census_mutex);  // see g_census_mutex
    g_fh_sib_seen.fetch_add(1, std::memory_order_relaxed);
    const fh::Slab sl = fh::depth_slab(data + 4);
    if (!(sl.ok && std::fabs(sl.a - 0.1f) < 0.02f)) return;
    g_fh_sib_viewmodel.fetch_add(1, std::memory_order_relaxed);
    // Same per-frame dedupe as the forward records.
    const uintptr_t key = reinterpret_cast<uintptr_t>(host);
    const uint32_t n = g_fh_seen_count.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < n && i < kFhDedupSlots; ++i)
        if (g_fh_seen_ptrs[i] == key) {
            g_fh_dedup.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    if (n < kFhDedupSlots) {
        g_fh_seen_ptrs[n] = key;
        g_fh_seen_count.store(n + 1, std::memory_order_relaxed);
    }
    std::scoped_lock tracked_lock(g_fh_tracked_mutex);
    const uint32_t tn = g_fh_tracked_n.load(std::memory_order_relaxed);
    if (tn < kFhTrackedMax) {
        FhTracked& t = g_fh_tracked[tn];
        t.host = host;
        t.range = byte_range;
        t.side = inside_frame_root_double ? 1 : 0;
        t.which = 1;
        t.sibling = true;
        std::memcpy(t.last_seen, data, (std::min)(byte_range, 160u));
        t.ctrl2_written = false;
        g_fh_tracked_n.store(tn + 1, std::memory_order_relaxed);
    }
}


void classify_gpu_camera(const float* m, void* pipeline, unsigned char* host,
                         uint32_t set, uint32_t binding) {
    float r[3], u[3], f[3];
    if (!wrap::reference_basis(r, u, f)) return;
    const float l0 = wrap::row_length(m, 0), l1 = wrap::row_length(m, 1);
    const float l2 = wrap::row_length(m, 2), l3 = wrap::row_length(m, 3);
    const bool affine = !(l3 > 1e-6f);
    const float lf = affine ? l2 : l3;
    if (!(l0 > 1e-6f) || !(l1 > 1e-6f) || !(lf > 1e-6f)) return;
    const int fwd_row = affine ? 2 : 3;

    // The clean basis is the reference. The rotated basis is that same
    // reference put through the exact rotation we apply, so the comparison is
    // against what we WROTE rather than against an idea of it.
    float hy = 0.0f, hp = 0.0f, hr = 0.0f;
    const bool have_head = wrap::residual_head_angles(hy, hp, hr);
    float ref_rot[16] = {r[0], r[1], r[2], 0.0f,
                         u[0], u[1], u[2], 0.0f,
                         f[0], f[1], f[2], 0.0f,
                         f[0], f[1], f[2], 0.0f};
    if (have_head) wrap::apply_head_rotation(ref_rot, hy, hp, hr, wrap::head_axes());

    auto dot3 = [&](int row, const float* v, float len) {
        return (m[row * 4 + 0] / len) * v[0] + (m[row * 4 + 1] / len) * v[1] +
               (m[row * 4 + 2] / len) * v[2];
    };
    const float clean_score =
        dot3(0, r, l0) + dot3(1, u, l1) + dot3(fwd_row, f, lf);
    float rr[3] = {ref_rot[0], ref_rot[1], ref_rot[2]};
    float ru[3] = {ref_rot[4], ref_rot[5], ref_rot[6]};
    float rf[3] = {ref_rot[12], ref_rot[13], ref_rot[14]};
    const float rot_score =
        dot3(0, rr, l0) + dot3(1, ru, l1) + dot3(fwd_row, rf, lf);

    // 2.997 is all three axes agreeing to within ~2.5 degrees each.
    if (rot_score > 2.997f) {
        g_gpu_cam_rotated.fetch_add(1, std::memory_order_relaxed);
        g_gpu_cam_rotated_pipeline.store(reinterpret_cast<uintptr_t>(pipeline),
                                         std::memory_order_relaxed);
    } else if (clean_score > 2.997f) {
        g_gpu_cam_clean.fetch_add(1, std::memory_order_relaxed);
        g_gpu_cam_clean_pipeline.store(reinterpret_cast<uintptr_t>(pipeline),
                                       std::memory_order_relaxed);
    } else {
        g_gpu_cam_other.fetch_add(1, std::memory_order_relaxed);
    }

}

// ===========================================================================
// DESTCAMHUNT (2026-08-26) - WHERE IS THE DESTINATION'S REAL CAMERA?
// ===========================================================================
//
// The 16:14 run proved destFixed writes NON-CAMERAS: every +0xC44 has zero
// row0/row1, every +0xC84 has row0=(1,1,1,1). The repo's 2026-08-23
// control-vector scan put the destination world VP at 0x3b00, "no match at
// 0xC44 at all", and that has never been acted on.
//
// READ-ONLY, and it locates by POSITION, not by direction. Every previous scan
// here matched on basis direction, which is why one latched onto a LOADING
// matrix; a matrix is accepted here only if solve_view_origin returns a
// position within kDchRadius of the position renderView's own live world VP
// encodes. A loading camera is thousands of units away and cannot pass.
//
// Bounded by construction: ONE destination per call at most, one destination
// per kDchCooldown calls, each one a SINGLE 32 KB read followed by pure
// arithmetic - never a read per candidate, which at 8192 candidates would be
// the classic per-item-syscall stall.
constexpr size_t kDchWindow = 0x8000;
// TIGHTENED 300 -> 50 (2026-08-26). The GuiCtxHunt lines came back with eight
// "hits" at d=149..259 uu and r0len of 3634, 3167 and 0.73 - accidental solves,
// not cameras. A radius that admits a quarter of a room admits anything.
constexpr float kDchRadius = 50.0f;        // uu from renderView's own camera
constexpr float kDchMaxResidual = 2.0f;
constexpr uint32_t kDchCooldown = 91;      // calls between hunts; COPRIME WITH 6 - see the call site
constexpr int kDchMaxHits = 8;
std::atomic_uint32_t g_dch_done_mask{0};
std::atomic_uint32_t g_dch_calls{0};
std::atomic_uint32_t g_dch_hunts{0};
std::atomic_uint32_t g_dch_no_ref{0};
// GUICTXHUNT: bit 0 = ctx28, 1 = ctx30, 2 = arg1. See hook_render_gui_models.
std::atomic_uint32_t g_gui_ctx_hunt_mask{0};

bool camera_hunt(const char* label, size_t idx, uintptr_t dest_data) {
    // renderView's own live world VP is the reference. Without it there is
    // nothing to measure against and the hunt must not run blind.
    wrap::VpSnapshot vp;
    if (!wrap::world_vp(vp) || !wrap::is_gameplay_view(vp.m)) return false;
    float ref[3]{};
    float ref_resid = 0.0f;
    if (!solve_view_origin(vp.m, ref, &ref_resid) || !(ref_resid < kDchMaxResidual)) {
        g_dch_no_ref.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    g_dch_hunts.fetch_add(1, std::memory_order_relaxed);

    static thread_local std::vector<unsigned char> buf;
    buf.assign(kDchWindow, 0);
    size_t have = 0;
    if (safe_read_bytes(dest_data, buf.data(), kDchWindow)) {
        have = kDchWindow;
    } else {
        // Fall back to pages, so a short mapping still yields what it has
        // rather than reporting a null that means "unreadable".
        for (size_t off = 0; off < kDchWindow; off += 0x1000) {
            if (!safe_read_bytes(dest_data + off, buf.data() + off, 0x1000)) break;
            have = off + 0x1000;
        }
    }

    std::string hits;
    int found = 0;
    int examined = 0;
    for (size_t off = 0; off + 64 <= have && found < kDchMaxHits; off += sizeof(float)) {
        const float* m = reinterpret_cast<const float*>(buf.data() + off);
        bool finite = true;
        for (int k = 0; k < 12; ++k)
            if (!std::isfinite(m[k])) { finite = false; break; }
        if (!finite) continue;
        const float l0 = wrap::row_length(m, 0);
        // A real view row is unit-ish or projection-scaled; 3634 is neither.
        if (!(l0 > 0.05f) || !(l0 < 50.0f)) continue;
        ++examined;
        float p[3]{};
        float resid = 0.0f;
        if (!solve_view_origin(m, p, &resid) || !(resid < kDchMaxResidual)) continue;
        const float dx = p[0] - ref[0], dy = p[1] - ref[1], dz = p[2] - ref[2];
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!(dist < kDchRadius)) continue;
        hits += std::format(" {:#06x}(d={:.2f} res={:.4f} r0len={:.4f})",
                            static_cast<unsigned>(off), dist, resid, l0);
        ++found;
    }
    log::warn(std::format(
        "CamHunt {}[{}] data={:#x} readable={:#x} of {:#x} examined={} "
        "refPos=({:.1f} {:.1f} {:.1f}) refResid={:.5f} -> {} camera matrices "
        "within {:.0f} uu:{}\n"
        "READING IT: READ-ONLY, nothing is written. These are the offsets in this "
        "destination object holding a matrix that encodes THIS FRAME'S camera position, "
        "solved and self-validated - not a direction match, which is what latched onto a "
        "loading matrix in 2026-08-23. destFixed currently writes +0xC44 and +0xC84 and the "
        "16:14 run proved both hold non-cameras (zero rows, and rows of ones). If 0x3b00 "
        "appears below, the 2026-08-23 scan was right and re-pointing destFixed there is the "
        "fix. If NOTHING appears, this destination holds no world camera at all and destFixed "
        "should be DELETED rather than re-aimed. examined=0 with readable>0 would mean the "
        "pre-filter is wrong - instrument failure, not a null.",
        label, idx, dest_data, have, kDchWindow, examined, ref[0], ref[1], ref[2], ref_resid,
        found, kDchRadius, found ? hits : std::string(" NONE")));
    return true;
}


#endif  // DOOMVR_RESEARCH_TOOLS

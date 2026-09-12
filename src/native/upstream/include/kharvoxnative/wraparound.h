#pragma once
#include "kharvoxnative/wraparound_math.h"

#include <cstdint>
#include <string>

// ===========================================================================
// TRUE WRAPAROUND VR - runtime side. See docs/PLAN-2026-08-25-TRUE-WRAPAROUND-VR.md.
//
// EVERYTHING HERE IS INERT UNLESS C:\dev\doomvr-wraparound.txt EXISTS.
// With the file absent, the only thing this module does is publish the live
// projection scale and print the Phase 0 line. Nothing else runs, nothing
// downstream of DOOM's renderer is touched, and the virtual-screen path is
// byte-for-byte what it is today. That is the regression arm and it is not
// negotiable.
//
// FILE-based, never environment-based: DOOM inherits Steam's environment, not
// the injector's, and an env var has already silently voided one bisect run.
// ===========================================================================

namespace kharvoxnative::wrap {

// How the GUI/viewmodel view receives the head rotation - see Config::vrot.
enum class VrotMode { Off, World, Local };
const char* vrot_mode_name(VrotMode m);

// bodyfollow=off|on|calibrate - the decoupled head/body transfer. See the
// Config field for what each mode does and wraparound_math.h for the
// invariant. Default Off, and Off is inert by construction.
enum class BodyFollow { Off, On, Calibrate };
const char* bodyfollow_name(BodyFollow m);

// ---------------------------------------------------------------------------
// Configuration. Parsed ONCE, from the switch file's own contents, so the
// tuning surface needs no new INI machinery and no console writes (the convar
// write path crashes - standing rule). An empty file means all defaults.
//
//   scalesrc=row1len|elem1|elem6|elem5|fixed   which +0xC44 term is the scale
//   scale=<float>                              value used when scalesrc=fixed
//   pose=located|shared                        Phase 2-v1 pose declaration
//
// A key that is not recognised is LOGGED, not ignored: a typo in a tuning file
// that silently does nothing is how a run gets spent on the default arm while
// the notes say otherwise.
// ---------------------------------------------------------------------------
struct Config {
    bool armed{false};
    ScaleSource scale_source{ScaleSource::Row1OverRow3};
    float fixed_scale{kLegacyProjectionScale};
    // Located: each eye declares its own located position - the compositor's
    // reprojection is then near-identity, which is the minimum-artefact
    // choice. Shared: both eyes declare eye 0's position, which is what
    // today's working virtual-screen arm does (both eyes get one fixed_view)
    // and where "stereo depth as today" comes from. One line in the switch
    // file moves between them with no rebuild, because which is right depends
    // on whether the compositor's stereo baseline fights the baseline already
    // baked into the content - and that is judged in the headset, not here.
    bool pose_shared{false};
    // fov   = crop to what the headset can actually SHOW, filling the eye.
    // pixel = crop to the eye buffer's pixel aspect - the plan's literal
    //         "aspect-preserving" - which on THIS headset leaves black edges,
    //         because RUN 1b measured its buffer aspect (4608/4224 = 1.0909)
    //         and its located FOV aspect (tan55/tan45 = 1.4281) disagreeing by
    //         31%. Both keep declaration == content by construction; they
    //         differ only in coverage and in sampling density per axis.
    bool crop_to_fov{true};
    // headaxes=<3 chars of + or -> for yaw, pitch, roll. NOT a calibration
    // knob: the OpenXR-to-DOOM axis mapping has never been established in this
    // repo, and the basis measured out of +0xC44 is left-handed in the
    // (right, up, forward) sense - right x up came out as -forward on every
    // census world view. So each axis is ONE BIT, settled by a ten-second
    // look-left / look-up / tilt check, not a magnitude to tune.
    HeadAxes head_axes;
    // anchor=content|head (2026-08-27) - WHICH ORIENTATION the submitted
    // projection declares to the compositor.
    //   head    = today's behaviour: declare the located head pose. Content
    //             that does not rotate with the head welds to the face; every
    //             population defect is a DIFFERENTIAL against this choice.
    //   content = declare the LOCAL-space identity orientation - the session's
    //             reference facing, i.e. where the body aims. The whole
    //             coherent frame is then world-locked AS A UNIT by the
    //             compositor and the head looks around WITHIN it. No DOOM-side
    //             camera is touched at all; the ~20 differential defects
    //             cannot exist by construction. The cost is content edges:
    //             at slider 130 the content (132x101 deg) exceeds the headset
    //             (110x90) by ~+/-11 deg yaw and ~+/-5 deg pitch of slack
    //             before black shows. Run with headlook=off (or CTRL+Delete):
    //             the content must stay body-aimed for the anchor to be true.
    bool anchor_content{false};
    // overscan=on|off (2026-08-28, default ON). Submit the FULL rendered
    // frame (132x101 deg at slider 130) instead of cropping to the headset's
    // located FOV. The compositor timewarps every image between render and
    // display; with a zero-margin crop each head motion flashes black past
    // the trailing edge - the constant flicker the owner read as "a projected
    // screen in front of me" even in the in-engine mode. The margin makes
    // timewarp invisible. Costs nothing: the pixels were already rendered.
    bool overscan{true};
    // fhcensus=on (2026-08-28): READ-ONLY census of freqHigh_vertexUniforms
    // records (the gun's real transform surface - docs/REVIEW-2026-08-28-GUN-
    // TRANSFORM-FOUND.md). Measures, at the CORRECT offsets, how many head
    // rotations the viewmodel records' implied VP and model matrix each
    // carry. Phase 1 of docs/HANDOFF-2026-08-28-FREQHIGH-IMPLEMENTATION-
    // SCRIPT.md; its numbers pick the Phase-2 correction mode. Default off.
    bool fhcensus{false};
    // precull=on (2026-08-30): THE PRE-CULL CAMERA PRODUCER WRITE. Rotates the
    // head into viewParms+0x6C (the idMat3 viewaxis) at DOOM+0x1830B80 entry -
    // the one function every camera matrix in the renderer descends from.
    // Measured, not inferred: STEP 2 showed that function runs 6816/6816 times
    // BEFORE Lever A, that its viewParms+0x1270 is the CLEAN camera while
    // renderView+0xC44 at the same moment reads rot1 under the same
    // classifier, and that +0xC44 is a byte-for-byte copy of +0x1270
    // (2271 attempts, 2271 match, 0 mismatch). So the seam rotates ONE leaf of
    // this object and leaves +0x11B0 / +0x1230 / +0x12B0 - the matrices the
    // froxel light binning is built from - clean. That is the EFF12 defect.
    // This REPLACES the seam writes: when it is on, Lever A's rotation AND
    // publish and Lever V's +0x11F0 rotation are all suppressed, because every
    // one of their targets is now derived from an already-rotated axis.
    // CTRL+Up is the live kill switch and swaps back to the legacy seam mode.
    bool precull{false};
    // gpucam=on|off (2026-08-30, default OFF). Re-arms the per-descriptor
    // 64-byte read out of MAPPED GPU MEMORY that feeds the GPUCAM and
    // SepCensus diagnostics. That memory is write-combined and uncached, so
    // each read costs microseconds; done per descriptor per bind call it was
    // ~4 us of a 15.30 us walk (against a 0.03 us lock and a 0.07 us driver
    // call) and it is why arming wraparound cost two thirds of the framerate.
    // Those two census lines read zero unless this is on. That is the trade,
    // and it is the right way round: a diagnostic must not cost the frame.
    bool gpucam{false};
    // renderw / renderh (2026-08-30). THE RENDER RESOLUTION, FORCED FROM CODE.
    // DOOM renders at its swapchain size, and its swapchain size is its window
    // client area - so resizing the window IS setting the render resolution.
    // Measured 2026-08-30: DOOM was rendering 3818x2032 (a 3840x2160 desktop
    // minus borders and title bar) into two 4032x3648 eye buffers, which is
    // 20.2 px/deg vertical against a 36.2 px/deg panel - a 1.79x stretch, and
    // the reason the image looks soft. Setting these to the eye size fixes it
    // WITHOUT asking anyone to edit DOOM's video settings or Steam launch
    // options; a VR mod should own its own render size.
    // 0 = leave DOOM's window alone (the old behaviour).
    int render_w{0};
    int render_h{0};
    // ipd=<float> (2026-08-30). Overrides the doubled-call stereo separation.
    // ipd=0 makes both passes render the SAME view while keeping every other
    // part of doubling identical - the split test for the transient white
    // flashes and flame flicker. Negative = follow the scripted value.
    float ipd_override{-1.0f};
    // gunfix2=worldp (2026-08-28 evening): THE weapon-projection substitution
    // (docs/RESULT-2026-08-28-FREQHIGH-CAUSAL-CHAIN.md). The gun renders
    // through its own ~70deg projection pasted into the 132deg wraparound
    // frame - 1.5-1.9x the world's angular scale - so it slides 1.5-1.9x the
    // walls' rate under head turns AND draws that much oversized. worldp
    // rewrites each classified viewmodel record to mvp' = VP_world * model
    // (the gun becomes an ordinary world object; Halo-MCC-VR's shipped fix),
    // prepass siblings included. Requires fhcensus=on (it IS the classifier).
    // Default off; slot 3 is the live kill switch.
    bool gunfix2_worldp{false};
    // BARFIX (2026-08-31 night). Inside the doubled call, APPEND a parallel
    // barrier for our mirror instead of RE-POINTING DOOM.s own barrier at it.
    // STEPLADDER put all three defects at scripted step 2 (live doubling armed)
    // and BarrierSubstitute fires 17 ms later, re-pointing only SOME barriers
    // per call - every one of those is a layout transition DOOM.s own image
    // never receives. Barriers are emitted whether or not a job draws, which is
    // why suppressing all 13 jobs, the final pass, both, and the frame rate all
    // nulled. Default off.
    bool barfix{false};
    // RPFIX (2026-08-31 night). MEASURED FIRST: the render-pass census returned
    // fellThrough=3728 withClears=3728 - EVERY render pass the doubled call
    // began on DOOM.s OWN framebuffer carries clears, full screen size, about
    // once per displayed frame. Redirect those to a mirror. Default off.
    bool rpfix{false};
    // The slew dial (2026-08-28). The owner's requirement is NO VISIBLE EDGES
    // in any direction; the game renders 132x101 deg per frame, so the only
    // way to satisfy it is the body following the head fast enough that the
    // window is always under the gaze. slewdead is the decoupled-glance zone
    // in DEGREES (inside it the head is free and the gun never moves);
    // slewrate is the catch-up fraction per frame outside it. Hard defaults
    // (2 deg, 0.12) make edges unreachable in normal play at the cost of the
    // gun following sustained head motion - the shipped-seated-mod trade.
    // Soft values (7 / 0.05) give wider decoupling but visible edges on big
    // turns. These keys exist because the 07:07/07:3x runs demanded exactly
    // this trade be tunable, not because a magnitude wanted guessing.
    // 1:1 defaults (2026-08-28, after "THIS IS NOT VR"): zero deadzone, fast
    // catch-up - the camera follows the head like a native VR title and the
    // reprojection window can never be seen. Raise slewdead to re-introduce
    // decoupled glances at the price of visible edges on big turns.
    float slew_dead_deg{0.0f};
    float slew_rate{0.30f};
    // vrot=world|local|off - HOW the GUI/viewmodel view (Lever V's ctx+0x11F0)
    // receives the head rotation. OFF by default: absent from the file, the
    // GUI site only counts its attempts and writes nothing new, so an armed
    // build with no vrot key behaves exactly like the previous build.
    //   world = apply_world_rotation_affine_view with the SAME world-space
    //           rotation renderView's rows undergo (head_world_rotation).
    //           Convention-free: never interprets the view's row signs, so it
    //           needs no new axis calibration beyond headaxes. RECOMMENDED.
    //   local = the old own-rows mix (apply_head_rotation's affine branch),
    //           kept as the fallback arm if the world frame is somehow wrong.
    VrotMode vrot{VrotMode::Off};
    // headlook=off parks the ENTIRE head-decoupling experiment (Lever A's and
    // Lever V's head rotation) while leaving wraparound, the stereo path and
    // the framing fully armed. Added 2026-08-26 because there was no way to
    // run the working true-stereo build WITHOUT the head-look work: the only
    // off switch was deleting the switch file, which disarms everything.
    bool head_look{true};
    // ===== bodyfollow (2026-08-29, thread A) ==============================
    // The decoupled head/body transfer. See BodyTransferState in
    // wraparound_math.h for the invariant this implements and why it cannot
    // oscillate; see docs/PLAN-2026-08-29-NIGHT-BODYFOLLOW-AND-BINDHOOK.md
    // for the ladder that sizes its two numbers.
    //
    //   off       DEFAULT. The seam subtracts a reference that nothing ever
    //             advances, i.e. zero, so every path behaves exactly as it did
    //             before this key existed. Inert by construction.
    //   on        Head-look free inside bfdead; past it the over-deadzone
    //             excess is injected into DOOM's own view at bfrate and the
    //             reference is advanced by exactly what was sent.
    //   calibrate FLAT/MONITOR ONLY. Scripted pixel bursts with the committed
    //             yaw measured off the clean camera, to turn bfpxrad from a
    //             feel-tuned guess into a measurement. Injects nothing else.
    BodyFollow bodyfollow{BodyFollow::Off};
    // Deadzone, degrees of head yaw that stay entirely at the seam. The
    // artifact level scales with this, because the artifacts scale with the
    // residual. FLAT Run 1's 5-deg-vs-15-deg amplitude curve sizes it; 8 is a
    // placeholder that says so rather than a value anything measured.
    float bf_dead_deg{8.0f};
    // Transfer rate cap, degrees per second of body turn.
    float bf_rate_deg_per_sec{120.0f};
    // Mouse pixels per radian of committed view rotation. 3000 is the
    // look-injection era's "tuned by feel" constant and is UNCALIBRATED - it
    // is the default only so an unset key reproduces today's injection
    // behaviour. bodyfollow=calibrate measures the real number.
    float bf_pixels_per_radian{3000.0f};
    std::string parse_notes;   // echoed into the Phase 0 line
};
const Config& config();
inline bool armed() { return config().armed; }

// ---------------------------------------------------------------------------
// Live projection scale, published by the EXISTING renderView+0xC44 sampler in
// vulkan_hooks.cpp. No new reader, no new hook, no new DOOM-side address.
// Bounded by construction to roughly 150 publishes/second.
//
// TWO CHANNELS, ON PURPOSE. looks_like_world_camera_matrix() - the project's
// own live-validated predicate - bounds row 1's length to 1.65..1.90. RUN 1
// asks the owner to move DOOM's FOV slider to MAXIMUM. If row 1's length IS
// the FOV term, the slider can push it straight out of that band and the
// predicate would then reject every world view: the sampler would fall silent
// and the run would come back with samples=0, looking like a dead instrument.
// That would be a five-minute run lost to my own gate.
//
// So the strict verdict is passed in and RECORDED rather than used as the gate,
// alongside a wider structural predicate that leaves row 1 free. If the strict
// count collapses while the wide count stays healthy, that is not a failure -
// it is the answer to which term the slider moves.
// ---------------------------------------------------------------------------
void publish_world_vp(const float m[16], bool passed_strict_predicate);
// Called even when the read FAILED, so "the hook never ran" and "the hook ran
// and the matrix was unreadable" are different numbers. RUN 1 could not tell
// those apart: one counter, worldViews, covered both, and it read zero.
void note_vp_read_failure();
// Counters, so no single number can hide a silent site.
struct SamplerCounts {
    uint64_t hook_calls{0};     // every non-null render view reaching the hook
    uint64_t attempts{0};       // the 1-in-256 subset we actually read
    uint64_t read_failures{0};
    uint64_t wide_pass{0};
    uint64_t strict_pass{0};
    uint64_t census_lines{0};
};
SamplerCounts sampler_counts();
void note_hook_call();
// wide_world_vp_predicate lives in wraparound_math.h so the offline test can
// exercise it: rows 0, 2 and 3 gate; row 1 - the term the FOV slider may move
// - is only required to be a plausible scale.

struct VpSnapshot {
    float m[16]{};
    float row_len[4]{};
    uint64_t samples{0};        // samples in whichever channel this came from
    uint64_t strict_samples{0};
    uint64_t wide_samples{0};
    bool from_strict{false};
};
// false when NEITHER channel has ever fired. A false here is INSTRUMENT
// FAILURE, not "DOOM has no camera" - the self-test treats it as an abort.
bool world_vp(VpSnapshot& out);
bool sampler_compiled_in();

// Once a minute, armed or not: current scale, both channel counts, and the
// row-1 length range seen SINCE THE LAST LINE (not all-time - an all-time
// extremum is a ratchet that one transient latches for good). This is what
// lets RUN 1 see the scale MOVE when the slider moves, without a second run.
void maybe_emit_scale_heartbeat();

// Scale for THIS frame, latched once per present so both eyes cannot use
// different values. Returns the legacy constant and sets was_live=false if the
// live read is missing or outside the plausible band - and that fallback logs
// itself the first time and then once a minute, never silently.
void begin_frame();
// bodyfollow=calibrate. Driven from the PRESENT hook, not the XR poll, because
// the XR poll does not run on a headset-free session and this mode is
// FLAT/MONITOR only. `xr_ready` is passed in by the caller (the only place
// that knows it) and a true value REFUSES the mode loudly rather than letting
// scripted bursts fight a real head. Self-gated on the config; a no-op in
// every other mode.
void tick_body_calibration(bool xr_ready);
// One heartbeat line for the transfer: mode, deadzone/rate/calibration in use,
// the live residual, the last committed step, the reference, and every gate
// reason as its own counter. Empty string when bodyfollow was never configured,
// so an unrelated run's log does not grow a line about a mode it is not in.
std::string bodyfollow_report();
float frame_scale(bool* was_live);

// ---------------------------------------------------------------------------
// What each eye actually blitted and declared, published by render_eye so the
// engagement line can compare the two rather than restate one of them.
// ---------------------------------------------------------------------------
void publish_framing(uint32_t eye, const Framing& f);
bool last_framing(uint32_t eye, Framing& out);

// ---------------------------------------------------------------------------
// Phase 0. One line per session, armed or not. Fed from both sides: the DOOM
// side (swapchain extent, live +0xC44 and every scale candidate) and the XR
// side (per-eye recommendedImageRect, per-eye located XrFovf). Whichever half
// is missing is NAMED as missing rather than omitted.
// ---------------------------------------------------------------------------
struct XrFacts {
    bool have{false};
    int32_t eye_w[2]{}, eye_h[2]{};
    float fov_l[2]{}, fov_r[2]{}, fov_u[2]{}, fov_d[2]{};
    uint32_t blit_src_w{0}, blit_src_h{0};
};
void publish_xr_facts(const XrFacts& f);
// DOOM's frame aspect, published from the present hook. The world-view test is
// a ratio against this; without it the sampler falls back to a 16:9..21:9 band.
void publish_frame_aspect(float aspect);
// Whether DOOM's camera is actually following the head this session. The
// DECLARED pose tracks the head unconditionally; if the CONTENT does not, the
// compositor welds the image to the head and you get a screen strapped to your
// face. That is a precondition, not a subtlety to be discovered in a headset,
// and the self-test now aborts on it.
void publish_look_injection(bool on);

// ---------------------------------------------------------------------------
// PHASE 3 - DECOUPLED HEAD LOOK.
//
// The XR side publishes the head's yaw/pitch/roll every frame; Lever A1's
// once-per-frame write in hook_render_gather_prepare rotates DOOM's world
// view-projection by it. The GUI/viewmodel view is a different matrix and is
// deliberately left alone, which is what keeps the gun, hands and body aiming
// where the mouse points while the world camera turns away from them.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// MENU STATE. While DOOM's menu is up the image must stay a WORLD-FIXED screen
// you can look around, not a head-tracked view - the menu is drawn in DOOM's
// screen space, so a head-tracked pose welds it to your face by construction.
// Fed from the present hook with doom_render_view's calls-per-frame, whose menu
// signature (~70 -> ~493) was measured in this repo before this feature
// existed. Every transition logs.
// ---------------------------------------------------------------------------
void publish_gui_draw_rate(uint64_t render_view_calls_this_frame);
bool menu_is_open();
// armed AND not in the menu. The single predicate every head-rotation site
// uses, so the menu cannot suspend one of them and not another.
bool head_look_active();
void set_head_look_runtime(bool on);
bool head_look_runtime();

// ---------------------------------------------------------------------------
// THE REFERENCE BASIS - renderView's CLEAN camera directions, published every
// frame before we rotate it.
//
// RUN 8 killed the structural search. HeadWriteSites read
// "renderView=3147 guiView=0 (refused 3147) cameraSrc=0 destCopies=0" with
// cameraSrcOffsets=0 and destOffsetsFound=0 - so of four write sites, ONE was
// writing and three matched nothing at all, and the GUI site refused every
// single call. My predicates assume a projection-scaled layout (|row1|/|row0|
// == the frame aspect) and only renderView+0xC44 has it. A plain VIEW matrix
// has unit rows and a ratio of 1.0, so it can never match - which is why three
// sites came back empty and one refused.
//
// So: match by DIRECTION instead. This is the method the repo's own 2026-08-23
// control-vector scan used, and it is the method that actually found 0x3b00.
// It works whether the target is projection-scaled or orthonormal, because a
// direction does not care about scale.
// ---------------------------------------------------------------------------
void publish_reference_basis(const float* clean_world_vp);
// A = VP_rot * inverse(VP_clean), recomputed each frame. Left-multiplying any
// matrix built on the clean camera re-bases it onto the head-rotated one -
// including per-object MVPs whose model transform we cannot see. false until a
// clean matrix has been published and inverted.
void publish_head_delta(const float* clean_world_vp);
bool head_delta(float* out16);
// inverse(VP_clean) for this frame, so the bind hook can recover a record's
// model transform and prove the record was built on OUR camera.
bool clean_vp_inverse(float* out16);
// The clean (pre-rotation) world VP itself, published in the same seqlock as
// the delta. For the capture corpus: offline factorisation needs the world
// family's generation-0 base, not just its inverse.
bool clean_world_copy(float* out16);
// The publish sequence behind clean_world_copy / head_delta / clean_vp_inverse.
// It changes exactly once per publish, i.e. once per frame, so a caller that
// derives something expensive FROM the clean camera can key a cache on it and
bool config_barfix();
bool config_rpfix();
// stop recomputing a per-frame value per record. Odd = a publish is in flight.
uint32_t clean_world_seq();
// Unit right/up/forward from renderView's clean matrix. false until published.
bool reference_basis(float right[3], float up[3], float forward[3]);
// Does this matrix hold the same camera directions as renderView, at any scale
// and in either layout? Tolerance is a dot product, so it is scale-free.
bool matches_reference_basis(const float* m);

// The 3x3 world-space rotation the head applied to renderView THIS frame:
// built from the published clean reference basis, the published head angles
// and the configured headaxes flips, via head_world_rotation_from_basis. This
// is what Lever V's world-mode rotation consumes, so the GUI view receives
// exactly the rotation the world camera received - one rigid world by
// construction. false until both the basis and the angles have been published
// (callers count that as a refusal, never skip silently).
bool head_world_rotation(float R[9]);
bool head_world_rotation_scaled(float R[9], float scale);
inline VrotMode vrot_mode() { return config().vrot; }

// The live anchor decision: config().anchor_content unless a runtime override
// is set (slot 3 / CTRL+Insert flips it live, same pattern as CTRL+Delete's
// head-look park). -1 = follow config, 0 = force head, 1 = force content.
bool anchor_content_active();
void set_anchor_content_runtime(int v);
bool config_fhcensus();
bool config_gunfix2_worldp();
bool config_gpucam();
int config_render_w();
int config_render_h();
float config_ipd_override();
// The pre-cull producer write. Config-armed, and CTRL+Up flips it live: false
// here means the legacy seam rotation (Lever A + Lever V) is in charge, which
// is exactly the shipped behaviour, so one press is a complete safety exit.
bool precull_active();
void set_precull_runtime(int v);
// The panel's orientation in XR space: DOOM's body facing as a delta from the
// facing captured when the anchor was (re)armed, mapped through headaxes.
// false until the reference basis has been published. Closed-loop: mouse and
// slew turns both move the panel with the content, so calibration error
// cannot accumulate between them.
bool body_anchor_angles(float& xr_yaw, float& xr_pitch);
// Slot 1: the slew - body follows sustained head offsets past a deadzone so
// the look-around window recenters under the gaze. OFF by default.
bool slew_active();
void set_slew_runtime(bool on);

void publish_head_angles(float yaw, float pitch, float roll, bool valid);
// Returns false when there is no usable head pose yet, so the camera is left
// exactly as DOOM built it rather than rotated by a stale or zero pose.
bool head_angles(float& yaw, float& pitch, float& roll);

// ===== THE HEAD/BODY TRANSFER REFERENCE (2026-08-29, thread A) =============
//
// ⚠ EVERY SEAM CONSUMER MUST USE residual_head_angles(), NOT head_angles().
// Lever A rotates renderView and Lever V rotates the GUI/viewmodel view; if
// one subtracted the reference and the other did not, the two would diverge by
// the transferred amount and the gun would swim against the world. That is the
// clock-split failure this project has already paid for once. head_angles()
// stays public only for the pose declaration and for the transfer itself,
// which need the ABSOLUTE head pose.
bool residual_head_angles(float& yaw, float& pitch, float& roll);
// The open-loop reference, in the same convention as head_angles().
float recenter_ref_yaw();
float recenter_ref_pitch();
// Advance by EXACTLY the amount committed to the body this frame. No other
// caller may move it - see the checkbook note in wraparound_math.h.
void advance_recenter_ref(float committed_yaw, float committed_pitch);
// World change / explicit recenter. Logs `why`.
void reset_recenter_ref(const char* why);
// Per-window telemetry from the transfer, published by the XR side so the
// heartbeat can print residual/committed/gate counts in one line.
void note_transfer(float injected_px_x, float injected_px_y,
                   float committed_yaw, float committed_pitch);
void note_transfer_gated(int reason);   // see kTransferGateReasons
inline constexpr int kTransferGateReasons = 5;
//   0 = bodyfollow off        1 = no look-injection file
//   2 = not a gameplay view   3 = no head pose
//   4 = calibration mode holds the injector
// The LIVE mode: the configured mode unless the kill switch (experiment slot
// 1, SHIFT+Up, in a build where bodyfollow is armed) has been pressed.
BodyFollow bodyfollow_mode();
// The CONFIGURED mode, ignoring the kill switch. Used to decide slot 1's role
// once at arm time, so a press cannot change what the key means.
BodyFollow bodyfollow_configured();
// ⚠ The kill deliberately does NOT reset the recenter reference. Resetting it
// would snap the world by however much had already been transferred; leaving
// it means the body simply stops following and the view stays exactly where
// the wearer is looking.
void set_bodyfollow_runtime(bool on);
bool bodyfollow_runtime();
float config_bf_dead_deg();
float config_bf_rate_deg_per_sec();
float config_bf_pixels_per_radian();
const HeadAxes& head_axes();
// LIVENESS. The declared pose tracks the head unconditionally; if this write is
// not landing, the content does not follow and the image welds to the head -
// the exact 23:18 failure. Counted, and the self-test aborts on zero.
void note_head_rotation_applied();
void note_head_rotation_refused();
uint64_t head_rotations_applied();
uint64_t head_rotations_refused();
// Whether DOOM's camera is actually following the head this session. The
// declared pose tracks the head unconditionally; if the CONTENT does not, the
// image is welded to the head. That is not a subtlety to be discovered in the
// headset - it is a precondition, and the self-test aborts on it.
void publish_look_injection(bool on);
void maybe_emit_phase0(uint64_t presents, uint32_t doom_src_w, uint32_t doom_src_h);

// ---------------------------------------------------------------------------
// Self-tests. Both follow the INSTRUMENT SELF-TEST pattern: log PASS, or log
// "***ABORT THIS RUN***" so a broken build costs thirty seconds instead of a
// five-minute run.
// ---------------------------------------------------------------------------
// Pure math, run at load. Identical assertions to tests/wraparound_math_test.cpp.
bool run_math_self_test(std::string* report);
void startup_self_test();
// Liveness, arm state, and declared-vs-content agreement. Fires once.
void present_self_test(uint64_t presents);

// Engagement, once per second while armed. Reads the fov ACTUALLY SUBMITTED
// back out of the projection view, so match=NO means the blit and the
// declaration really did diverge - a build defect, and it says so.
void note_submitted_fov(uint32_t eye, float angle_l, float angle_r,
                        float angle_u, float angle_d);
void maybe_emit_engagement(bool look_injection_on, bool xr_live);

}  // namespace kharvoxnative::wrap

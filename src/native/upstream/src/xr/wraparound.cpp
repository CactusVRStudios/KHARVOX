#include "kharvoxnative/wraparound.h"
#include "kharvoxnative/log.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <limits>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>

// SendInput for bodyfollow=calibrate. This module is otherwise platform-free;
// the calibration bursts have to originate on a tick that runs headset-free,
// and the present hook is the only one that does - see tick_body_calibration.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windef.h defines `near` and `far` as empty macros (16-bit segment relics).
// This file has a self-test lambda literally called `near`, and the wraparound
// math is full of ordinary identifiers; undefining them here keeps the include
// from silently rewriting code that has nothing to do with Win32.
#undef near
#undef far

namespace kharvoxnative::wrap {
namespace {

constexpr const char* kSwitchPath = "C:\\dev\\doomvr-wraparound.txt";

// ---------------------------------------------------------------------------
// CONFIG
// ---------------------------------------------------------------------------
std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' ||
                     s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

Config parse_config() {
    Config c;
    FILE* f = nullptr;
    if (fopen_s(&f, kSwitchPath, "r") != 0 || !f) {
        c.parse_notes = "switch file ABSENT - virtual-screen mode, nothing below Phase 0 runs";
        return c;
    }
    c.armed = true;
    char line[256];
    int keys = 0;
    while (std::fgets(line, sizeof(line), f)) {
        const std::string raw = trim(line);
        if (raw.empty() || raw[0] == '#' || raw[0] == ';') continue;
        const size_t eq = raw.find('=');
        if (eq == std::string::npos) {
            c.parse_notes += std::format(" [IGNORED, no '=': '{}']", raw);
            continue;
        }
        const std::string key = lower(trim(raw.substr(0, eq)));
        const std::string val = lower(trim(raw.substr(eq + 1)));
        ++keys;
        if (key == "scalesrc") {
            if (val == "row1/row3" || val == "row1norm")
                                       c.scale_source = ScaleSource::Row1OverRow3;
            else if (val == "row0/row3" || val == "row0norm")
                                       c.scale_source = ScaleSource::Row0OverRow3;
            else if (val == "row1len") c.scale_source = ScaleSource::Row1Len;
            else if (val == "elem1")   c.scale_source = ScaleSource::Elem1;
            else if (val == "elem6")   c.scale_source = ScaleSource::Elem6;
            else if (val == "elem5")   c.scale_source = ScaleSource::Elem5;
            else if (val == "fixed")   c.scale_source = ScaleSource::Fixed;
            else c.parse_notes += std::format(" [BAD scalesrc '{}', keeping {}]",
                                              val, scale_source_name(c.scale_source));
        } else if (key == "scale") {
            try {
                const float v = std::stof(val);
                if (scale_plausible(v)) c.fixed_scale = v;
                else c.parse_notes += std::format(
                    " [scale={} outside {}..{}, keeping {:.4f}]",
                    val, kScaleMin, kScaleMax, c.fixed_scale);
            } catch (...) {
                c.parse_notes += std::format(" [scale='{}' is not a number]", val);
            }
        } else if (key == "headaxes") {
            // Three characters, one per axis: yaw, pitch, roll. '+' or '-'.
            if (val.size() == 3 &&
                (val[0] == '+' || val[0] == '-') &&
                (val[1] == '+' || val[1] == '-') &&
                (val[2] == '+' || val[2] == '-')) {
                c.head_axes.flip_yaw = val[0] == '-';
                c.head_axes.flip_pitch = val[1] == '-';
                c.head_axes.flip_roll = val[2] == '-';
            } else {
                c.parse_notes += std::format(
                    " [BAD headaxes '{}' - expected three of + or -, e.g. headaxes=+-+ ; "
                    "keeping {}{}{}]", val,
                    c.head_axes.flip_yaw ? '-' : '+', c.head_axes.flip_pitch ? '-' : '+',
                    c.head_axes.flip_roll ? '-' : '+');
            }
        } else if (key == "slewdead") {
            try {
                const float v = std::stof(val);
                if (v >= 0.0f && v <= 45.0f) c.slew_dead_deg = v;
                else c.parse_notes += std::format(" [slewdead={} outside 0..45]", val);
            } catch (...) {
                c.parse_notes += std::format(" [slewdead='{}' is not a number]", val);
            }
        } else if (key == "slewrate") {
            try {
                const float v = std::stof(val);
                if (v >= 0.0f && v <= 0.5f) c.slew_rate = v;
                else c.parse_notes += std::format(" [slewrate={} outside 0..0.5]", val);
            } catch (...) {
                c.parse_notes += std::format(" [slewrate='{}' is not a number]", val);
            }
        } else if (key == "fhcensus") {
            if (val == "on")       c.fhcensus = true;
            else if (val == "off") c.fhcensus = false;
            else c.parse_notes += std::format(" [BAD fhcensus '{}']", val);
        } else if (key == "gunfix2") {
            if (val == "worldp")   c.gunfix2_worldp = true;
            else if (val == "off") c.gunfix2_worldp = false;
            else c.parse_notes += std::format(" [BAD gunfix2 '{}']", val);
        } else if (key == "barfix") {
            if (val == "on")       c.barfix = true;
            else if (val == "off") c.barfix = false;
            else c.parse_notes += std::format(" [BAD barfix '{}']", val);
        } else if (key == "rpfix") {
            if (val == "on")       c.rpfix = true;
            else if (val == "off") c.rpfix = false;
            else c.parse_notes += std::format(" [BAD rpfix '{}']", val);
        } else if (key == "renderw" || key == "renderh") {
            int v = 0;
            for (char ch : val) {
                if (ch < '0' || ch > '9') { v = -1; break; }
                v = v * 10 + (ch - '0');
                if (v > 16384) { v = -1; break; }
            }
            if (v < 0) c.parse_notes += std::format(" [BAD {}]", key);
            else if (key == "renderw") c.render_w = v;
            else c.render_h = v;
        } else if (key == "ipd") {
            try { c.ipd_override = std::stof(val); }
            catch (...) { c.parse_notes += " [BAD ipd]"; }
        } else if (key == "gpucam") {
            if (val == "on")       c.gpucam = true;
            else if (val == "off") c.gpucam = false;
            else c.parse_notes += std::format(" [BAD gpucam]");
        } else if (key == "precull") {
            if (val == "on")       c.precull = true;
            else if (val == "off") c.precull = false;
            else c.parse_notes += std::format(" [BAD precull '{}']", val);
        } else if (key == "overscan") {
            if (val == "on")       c.overscan = true;
            else if (val == "off") c.overscan = false;
            else c.parse_notes += std::format(" [BAD overscan '{}', keeping {}]",
                                              val, c.overscan ? "on" : "off");
        } else if (key == "anchor") {
            if (val == "content")   c.anchor_content = true;
            else if (val == "head") c.anchor_content = false;
            else c.parse_notes += std::format(" [BAD anchor '{}', keeping {}]",
                                              val, c.anchor_content ? "content" : "head");
        } else if (key == "headlook") {
            if (val == "on") c.head_look = true;
            else if (val == "off") {
                c.head_look = false;
                c.parse_notes +=
                    " [headlook=OFF - the head-decoupling experiment is PARKED. Stereo, "
                    "framing and wraparound stay armed; Lever A and Lever V simply do not "
                    "rotate. This is the 16:9 true-stereo build without the head work.]";
            } else c.parse_notes += std::format(" [BAD headlook '{}', keeping {}]",
                                                val, c.head_look ? "on" : "off");
        } else if (key == "bodyfollow") {
            if (val == "off")            c.bodyfollow = BodyFollow::Off;
            else if (val == "on")        c.bodyfollow = BodyFollow::On;
            else if (val == "calibrate") c.bodyfollow = BodyFollow::Calibrate;
            else c.parse_notes += std::format(" [BAD bodyfollow '{}', keeping {}]",
                                              val, bodyfollow_name(c.bodyfollow));
        } else if (key == "bfdead") {
            try {
                const float v = std::stof(val);
                if (v >= 0.0f && v <= 45.0f) c.bf_dead_deg = v;
                else c.parse_notes += std::format(" [bfdead={} outside 0..45]", val);
            } catch (...) {
                c.parse_notes += std::format(" [bfdead='{}' is not a number]", val);
            }
        } else if (key == "bfrate") {
            try {
                const float v = std::stof(val);
                if (v > 0.0f && v <= 720.0f) c.bf_rate_deg_per_sec = v;
                else c.parse_notes += std::format(" [bfrate={} outside 0..720]", val);
            } catch (...) {
                c.parse_notes += std::format(" [bfrate='{}' is not a number]", val);
            }
        } else if (key == "bfpxrad") {
            try {
                const float v = std::stof(val);
                // A plausibility band, not a tuning range: outside it the value
                // is a typo or a wrong unit, and a silent accept would put the
                // whole transfer on the wrong scale for a whole run.
                if (v >= 100.0f && v <= 100000.0f) c.bf_pixels_per_radian = v;
                else c.parse_notes += std::format(" [bfpxrad={} outside 100..100000]", val);
            } catch (...) {
                c.parse_notes += std::format(" [bfpxrad='{}' is not a number]", val);
            }
        } else if (key == "vrot") {
            if (val == "world")      c.vrot = VrotMode::World;
            else if (val == "local") c.vrot = VrotMode::Local;
            else if (val == "off")   c.vrot = VrotMode::Off;
            else c.parse_notes += std::format(" [BAD vrot '{}', keeping {}]",
                                              val, vrot_mode_name(c.vrot));
        } else if (key == "crop") {
            if (val == "fov")        c.crop_to_fov = true;
            else if (val == "pixel") c.crop_to_fov = false;
            else c.parse_notes += std::format(" [BAD crop '{}', keeping {}]",
                                              val, c.crop_to_fov ? "fov" : "pixel");
        } else if (key == "pose") {
            if (val == "located")     c.pose_shared = false;
            else if (val == "shared") c.pose_shared = true;
            else c.parse_notes += std::format(" [BAD pose '{}', keeping {}]",
                                              val, c.pose_shared ? "shared" : "located");
        } else {
            // NOT silently ignored. A typo that quietly does nothing is how a
            // run gets spent on the default arm while the notes say otherwise.
            c.parse_notes += std::format(" [UNRECOGNISED key '{}']", key);
        }
    }
    std::fclose(f);
    if (keys == 0 && c.parse_notes.empty()) c.parse_notes = "no keys, all defaults";
    return c;
}

// ---------------------------------------------------------------------------
// LIVE WORLD-VP PUBLICATION - seqlock.
//
// Written from DOOM's render threads (inside the existing render-view hook),
// read from the present thread. A seqlock rather than 16 independent atomics
// because the Phase 0 line prints the whole matrix and a torn matrix would be
// read as a measurement.
// ---------------------------------------------------------------------------
struct VpChannel {
    std::atomic_uint32_t seq{0};
    float m[16]{};
    std::atomic_uint64_t samples{0};
};
VpChannel g_vp_strict;   // passed looks_like_world_camera_matrix
VpChannel g_vp_wide;     // passed the wider structural predicate only

// Row-1 length range SINCE THE LAST HEARTBEAT. Reset every line, deliberately:
// a running all-time extremum never comes back down and one transient latches
// it for the rest of the session.
std::atomic<float> g_row1_min{0.0f};
std::atomic<float> g_row1_max{0.0f};
std::atomic_bool g_row1_have{false};
// The wide-channel sample count at the moment the strict predicate last
// matched. See the freshness test in world_vp().
std::atomic_uint64_t g_strict_at_wide{0};

// ONE COUNTER PER OUTCOME. RUN 1 had a single number - worldViews - standing
// for "the hook never ran", "the read failed" and "the predicate said no", and
// it read zero. That is three very different runs wearing the same face.
std::atomic_uint64_t g_hook_calls{0};
std::atomic_uint64_t g_attempts{0};
std::atomic_uint64_t g_read_failures{0};
std::atomic_uint32_t g_census_lines{0};
// DOOM's frame aspect, published from the present hook; 0 = not yet known.
std::atomic<float> g_frame_aspect{0.0f};
std::atomic_bool g_look_injection{false};
std::atomic_bool g_look_injection_known{false};

// Phase 3 head pose, published by the XR side, consumed by Lever A1.
std::atomic<float> g_head_yaw{0.0f};
std::atomic<float> g_head_pitch{0.0f};
std::atomic<float> g_head_roll{0.0f};
std::atomic_bool g_head_valid{false};
std::atomic_uint64_t g_head_applied{0};
std::atomic_uint64_t g_head_refused{0};
std::atomic<float> g_ref_right[3]{};
std::atomic<float> g_ref_up[3]{};
std::atomic<float> g_ref_fwd[3]{};
std::atomic_bool g_ref_valid{false};
float g_delta[16]{};
float g_clean_inv[16]{};
float g_clean_world[16]{};
std::atomic_uint32_t g_delta_seq{0};
std::atomic_bool g_delta_valid{false};
// The clean world VP / inverse are published even with no head pose (they are
// head-independent); the delta alone stays gated on g_delta_valid.
std::atomic_bool g_clean_valid{false};
std::atomic_bool g_menu_open{false};
std::atomic_uint32_t g_menu_transitions{0};

// ===== THE HEAD/BODY TRANSFER REFERENCE (2026-08-29, thread A) =============
// The open-loop reference of the invariant in wraparound_math.h. Written ONLY
// by advance_recenter_ref (by exactly the amount committed to the body) and by
// reset_recenter_ref. Zero until something commits, which is what makes
// bodyfollow=off inert: the subtraction is then a subtraction of nothing.
std::atomic<float> g_ref_yaw_transfer{0.0f};
std::atomic<float> g_ref_pitch_transfer{0.0f};
std::atomic_uint64_t g_transfer_commits{0};
std::atomic_uint64_t g_transfer_resets{0};
std::atomic<float> g_transfer_px_x{0.0f};
std::atomic<float> g_transfer_px_y{0.0f};
std::atomic<float> g_transfer_deg_yaw{0.0f};
std::atomic<float> g_transfer_deg_pitch{0.0f};
// ONE COUNTER PER GATE REASON. A single "gated" number standing for five very
// different runs is the defect this project keeps rediscovering.
std::atomic_uint64_t g_transfer_gates[kTransferGateReasons]{};

void channel_store(VpChannel& ch, const float* m) {
    const uint32_t s = ch.seq.load(std::memory_order_relaxed);
    ch.seq.store(s + 1, std::memory_order_release);   // odd = writing
    std::memcpy(ch.m, m, sizeof(ch.m));
    ch.seq.store(s + 2, std::memory_order_release);   // even = stable
    ch.samples.fetch_add(1, std::memory_order_relaxed);
}

bool channel_load(const VpChannel& ch, float* out) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint32_t before = ch.seq.load(std::memory_order_acquire);
        if (before & 1u) continue;
        std::memcpy(out, ch.m, sizeof(float) * 16);
        if (ch.seq.load(std::memory_order_acquire) != before) continue;
        return true;
    }
    return false;
}

// Latched once per present; both eyes must use the same value.
std::atomic<float> g_frame_scale{kLegacyProjectionScale};
std::atomic_bool g_frame_scale_live{false};
std::atomic_uint64_t g_fallback_count{0};

// Per-eye framing actually used by the blit, and the fov actually submitted.
std::atomic_uint32_t g_framing_seq[2]{};
Framing g_framing[2]{};
std::atomic<float> g_submitted[2][4]{};
std::atomic_bool g_submitted_have[2]{};

// XR facts for the Phase 0 line.
std::atomic_uint32_t g_xr_seq{0};
XrFacts g_xr_facts{};

std::atomic_bool g_phase0_emitted_without_xr{false};
std::atomic_bool g_phase0_emitted_with_xr{false};

std::string fmt_matrix(const float* m) {
    std::string s;
    for (int r = 0; r < 4; ++r) {
        s += std::format("{}({:.4f} {:.4f} {:.4f} {:.4f})", r ? " | " : "",
                         m[r * 4 + 0], m[r * 4 + 1], m[r * 4 + 2], m[r * 4 + 3]);
    }
    return s;
}

}  // namespace

bool config_fhcensus() { return config().fhcensus; }
bool config_gunfix2_worldp() { return config().gunfix2_worldp; }
bool config_barfix() { return config().barfix; }
bool config_rpfix() { return config().rpfix; }
// gpucam=on re-arms the per-descriptor GPU-memory snapshot that feeds the
// GPUCAM and SepCensus diagnostics. DEFAULT OFF because that read is uncached
// GPU memory done ~3000 times a frame and it was the framerate (2026-08-30).
bool config_gpucam() { return config().gpucam; }
int config_render_w() { return config().render_w; }
int config_render_h() { return config().render_h; }
float config_ipd_override() { return config().ipd_override; }
// -1 = follow config, 0 = force off, 1 = force on. Same pattern as the anchor
// override, and for the same reason: the run's arm state must be readable at
// the measurement, not inferred from which key was pressed when.
std::atomic_int g_precull_runtime{-1};
bool precull_active() {
    const int rt = g_precull_runtime.load(std::memory_order_relaxed);
    return rt < 0 ? config().precull : rt != 0;
}
void set_precull_runtime(int v) { g_precull_runtime.store(v, std::memory_order_relaxed); }

std::atomic_bool g_bodyfollow_runtime{true};
BodyFollow bodyfollow_configured() { return config().bodyfollow; }
BodyFollow bodyfollow_mode() {
    if (!g_bodyfollow_runtime.load(std::memory_order_relaxed)) return BodyFollow::Off;
    return config().bodyfollow;
}
void set_bodyfollow_runtime(bool on) {
    g_bodyfollow_runtime.store(on, std::memory_order_relaxed);
}
bool bodyfollow_runtime() { return g_bodyfollow_runtime.load(std::memory_order_relaxed); }
float config_bf_dead_deg() { return config().bf_dead_deg; }
float config_bf_rate_deg_per_sec() { return config().bf_rate_deg_per_sec; }
float config_bf_pixels_per_radian() { return config().bf_pixels_per_radian; }

const char* bodyfollow_name(BodyFollow m) {
    switch (m) {
        case BodyFollow::Off:       return "off";
        case BodyFollow::On:        return "on";
        case BodyFollow::Calibrate: return "calibrate";
    }
    return "unrecognised";
}

const char* vrot_mode_name(VrotMode m) {
    switch (m) {
        case VrotMode::Off:   return "off";
        case VrotMode::World: return "world";
        case VrotMode::Local: return "local";
    }
    return "unrecognised";
}

const Config& config() {
    static const Config c = [] { Config c; c.armed=true; c.head_look=false;
        c.precull=false; c.gunfix2_worldp=false; c.fhcensus=false;
        c.barfix=true; c.rpfix=true; c.render_w=0; c.render_h=0;
        return c; }();
    return c;
}

bool sampler_compiled_in() {
#ifdef DOOMVR_RESEARCH_TOOLS
    return true;
#else
    return false;
#endif
}


void publish_gui_draw_rate(uint64_t render_view_calls_this_frame) {
    // Measured signature, not a guess: doom_render_view runs ~70 calls/frame in
    // gameplay and ~493 at the menu (STATE-2026-08-25-MENU-STUTTER-SHELVED.md).
    // The threshold sits in the middle of that gap, with hysteresis so a frame
    // straddling it cannot flap the layer between QUAD and PROJECTION.
    constexpr uint64_t kMenuOn = 250;
    constexpr uint64_t kMenuOff = 160;
    const bool was = g_menu_open.load(std::memory_order_relaxed);
    const bool now = was ? (render_view_calls_this_frame > kMenuOff)
                         : (render_view_calls_this_frame > kMenuOn);
    if (now != was) {
        g_menu_open.store(now, std::memory_order_relaxed);
        const uint32_t n = g_menu_transitions.fetch_add(1, std::memory_order_relaxed);
        if (n < 40) {
            log::warn(std::format(
                "MenuState {} -> {} at {} doom_render_view calls/frame (on>{} off<={}). {}"
                " Every transition logs, so a mis-detection is a line in the log rather than a "
                "mystery in the headset.",
                was ? "MENU" : "GAMEPLAY", now ? "MENU" : "GAMEPLAY",
                render_view_calls_this_frame, kMenuOn, kMenuOff,
                now ? "Dropping to the world-fixed QUAD screen and suspending head rotation of "
                      "DOOM's camera, so the menu is a screen you can look around instead of one "
                      "welded to your face."
                    : "Back to head-tracked wraparound."));
        }
    }
}

bool menu_is_open() { return g_menu_open.load(std::memory_order_relaxed); }

// ⚠ THE MENU DETECTOR NO LONGER GATES ANYTHING. It is log-only.
//
// The 09:26 run: ONE transition, "GAMEPLAY -> MENU at 259", at 09:28:09, and it
// never came back. The off-threshold was 160 and this scene's ordinary gameplay
// runs above it - the "~70 calls/frame" figure I built the thresholds from was
// measured in a different scene in a different session. So the detector latched
// into MENU and switched head rotation off for the entire rest of the run.
// That is the owner's "head movement doing nothing... not affecting the game at
// all", and it was my heuristic, not the mechanism: the same run recorded
// headRotApplied=1017 refused=0 with a correct 110x90 declaration BEFORE the
// latch.
//
// A detector that can silently disable the whole feature is worse than no
// detector. It stays as an instrument so the real per-scene rates get recorded,
// and it drives nothing until there is a signal that is not a guess.
// Runtime override so the same park can be done live from a key without a
// relaunch - see set_head_look_runtime.
std::atomic_bool g_head_look_runtime{true};
bool head_look_active() {
    // anchor=content and DOOM-side head rotation are mutually exclusive BY
    // DEFINITION (2026-08-27): the content anchor requires the frame to stay
    // body-aimed - the pose label absorbs the whole head pose. One mode, one
    // switch; no run can arm both halves and produce the differential mess.
    return armed() && config().head_look &&
           g_head_look_runtime.load(std::memory_order_relaxed) &&
           !anchor_content_active();
}
void set_head_look_runtime(bool on) {
    g_head_look_runtime.store(on, std::memory_order_relaxed);
}
bool head_look_runtime() { return g_head_look_runtime.load(std::memory_order_relaxed); }

// -1 = follow config; 0/1 force. Slot 3 writes it; the submit path reads it.
static std::atomic_int g_anchor_override{-1};
bool anchor_content_active() {
    const int v = g_anchor_override.load(std::memory_order_relaxed);
    if (v >= 0) return v != 0;
    return config().anchor_content;
}

// ===== THE BODY ANCHOR (2026-08-27, second build) ==========================
// The panel's orientation tracks DOOM'S OWN BODY FACING, read from the clean
// reference basis, as a DELTA from the facing captured when the anchor was
// (re)armed. Closed-loop by construction: however the body turns - mouse or
// slew - the panel follows the CONTENT's real facing, so a calibration error
// cannot accumulate between panel and image. Mouse turning then reads as
// SELF-rotation (world holds still around you - true VR turning semantics),
// and head look samples the world-locked frame freely.
static std::atomic_bool g_anchor_epoch_valid{false};
static std::atomic<float> g_anchor_epoch_yaw{0.0f};
static std::atomic<float> g_anchor_epoch_pitch{0.0f};
// The HEAD's facing at the arming epoch, baked into the panel's orientation
// so the window spawns dead ahead of wherever the wearer is actually looking.
// Without this the panel sits at LOCAL-space identity - the runtime's room
// forward - which the 07:07 run showed was 75 degrees away from the owner's
// natural facing: they were parked at the window's far edge from the first
// frame. Recentring is the (re)arming gesture: CTRL+Insert off/on.
static std::atomic<float> g_anchor_epoch_head_yaw{0.0f};
static std::atomic<float> g_anchor_epoch_head_pitch{0.0f};

void set_anchor_content_runtime(int v) {
    g_anchor_override.store(v, std::memory_order_relaxed);
    // Re-arming re-captures the epoch: the panel starts at identity again,
    // aligned with wherever the body aims at that moment.
    g_anchor_epoch_valid.store(false, std::memory_order_relaxed);
}

// DOOM body yaw/pitch from the clean basis's forward vector. DOOM convention:
// X forward, Z up (measured throughout this repo's camera work).
static bool doom_body_yaw_pitch(float& yaw, float& pitch) {
    float r[3], u[3], f[3];
    if (!reference_basis(r, u, f)) return false;
    yaw = std::atan2(f[1], f[0]);
    pitch = std::asin(std::clamp(f[2], -1.0f, 1.0f));
    return std::isfinite(yaw) && std::isfinite(pitch);
}

// Slot 1 bridge: the slew's on/off lives here so the XR side can read it and
// the vulkan-side keypress handler can write it (the lever_v_rot_enabled
// pattern). Default OFF - the base anchor state is judged without it.
// Default OFF again (2026-08-28): the 1:1 slew went unstable - a feedback
// loop through SendInput with the render pipeline's latency inside it. The
// whole anchor/slew arm is PARKED (anchor=head in the switch file); this stays
// so a stray SHIFT+Up cannot re-arm chaos mid-run.
static std::atomic_bool g_slew_enabled{false};
bool slew_active() { return g_slew_enabled.load(std::memory_order_relaxed); }
void set_slew_runtime(bool on) { g_slew_enabled.store(on, std::memory_order_relaxed); }

bool body_anchor_angles(float& xr_yaw, float& xr_pitch) {
    float by = 0.0f, bp = 0.0f;
    if (!doom_body_yaw_pitch(by, bp)) return false;
    if (!g_anchor_epoch_valid.load(std::memory_order_relaxed)) {
        // The epoch latches only when the HEAD pose is also live, so the
        // panel can be centred on the wearer's actual facing. Until both
        // halves exist this returns false and the caller falls back to
        // identity - a visible-but-recoverable startup frame, never a latch
        // on a zero.
        float ehy = 0.0f, ehp = 0.0f, ehr = 0.0f;
        if (!head_angles(ehy, ehp, ehr)) return false;
        g_anchor_epoch_yaw.store(by, std::memory_order_relaxed);
        g_anchor_epoch_pitch.store(bp, std::memory_order_relaxed);
        g_anchor_epoch_head_yaw.store(ehy, std::memory_order_relaxed);
        g_anchor_epoch_head_pitch.store(ehp, std::memory_order_relaxed);
        g_anchor_epoch_valid.store(true, std::memory_order_relaxed);
        log::warn(std::format(
            "AnchorEpoch captured: panel centred at headYaw={:.1f} headPitch={:.1f} deg, "
            "bodyYaw={:.1f} bodyPitch={:.1f} deg. Re-centre any time: CTRL+Insert twice.",
            ehy * 57.2958f, ehp * 57.2958f, by * 57.2958f, bp * 57.2958f));
    }
    float dy = by - g_anchor_epoch_yaw.load(std::memory_order_relaxed);
    while (dy > 3.14159265f) dy -= 6.28318531f;
    while (dy < -3.14159265f) dy += 6.28318531f;
    const float dp = bp - g_anchor_epoch_pitch.load(std::memory_order_relaxed);
    // DOOM-delta -> XR angles through the SAME headaxes bits the head->DOOM
    // direction uses (they encode the one axis relation this repo has
    // measured). If a sign is wrong here it shows as the world spinning at 2x
    // under a mouse turn instead of holding still - a ten-second check, the
    // same class as headaxes itself.
    const HeadAxes& ax = head_axes();
    // Panel = (where the head looked at the epoch) + (how far the body has
    // turned since). The first term is the recentring fix; the second is the
    // closed-loop body tracking.
    xr_yaw = g_anchor_epoch_head_yaw.load(std::memory_order_relaxed) +
             (ax.flip_yaw ? -dy : dy);
    xr_pitch = g_anchor_epoch_head_pitch.load(std::memory_order_relaxed) +
               (ax.flip_pitch ? -dp : dp);
    return true;
}

void publish_reference_basis(const float* m) {
    if (!m) return;
    const float l0 = row_length(m, 0), l1 = row_length(m, 1), l3 = row_length(m, 3);
    if (!(l0 > 1e-6f) || !(l1 > 1e-6f) || !(l3 > 1e-6f)) return;
    for (int i = 0; i < 3; ++i) {
        g_ref_right[i].store(m[0 + i] / l0, std::memory_order_relaxed);
        g_ref_up[i].store(m[4 + i] / l1, std::memory_order_relaxed);
        g_ref_fwd[i].store(m[12 + i] / l3, std::memory_order_relaxed);
    }
    g_ref_valid.store(true, std::memory_order_relaxed);
}

bool reference_basis(float right[3], float up[3], float forward[3]) {
    if (!g_ref_valid.load(std::memory_order_relaxed)) return false;
    for (int i = 0; i < 3; ++i) {
        right[i] = g_ref_right[i].load(std::memory_order_relaxed);
        up[i] = g_ref_up[i].load(std::memory_order_relaxed);
        forward[i] = g_ref_fwd[i].load(std::memory_order_relaxed);
    }
    return true;
}

void publish_head_delta(const float* clean) {
    if (!clean) {
        g_delta_valid.store(false, std::memory_order_relaxed);
        return;
    }
    // The clean world VP and its inverse are head-INDEPENDENT facts about
    // renderView. Publishing them only when a head pose existed silently
    // starved every headset-free consumer: the 2026-08-28 19:00 flat capture
    // run died with noRef=753846 because the freqHigh census's reference
    // routes through here. Only the DELTA itself needs the head.
    float hy = 0.0f, hp = 0.0f, hr = 0.0f;
    float A[16];
    // ⚠ RESIDUAL, NOT ABSOLUTE (2026-08-29, thread A). The seam rotates by
    // head - recenterRef, so an injected body turn is not applied twice. With
    // bodyfollow=off nothing ever advances the reference and this is a
    // subtraction of zero - bit-identical to the pre-transfer build.
    const bool have_delta = head_look_active() && residual_head_angles(hy, hp, hr) &&
                            compute_head_delta(clean, hy, hp, hr, head_axes(), A);
    const uint32_t s = g_delta_seq.load(std::memory_order_relaxed);
    g_delta_seq.store(s + 1, std::memory_order_release);
    if (have_delta) std::memcpy(g_delta, A, sizeof(g_delta));
    mat4_inverse(clean, g_clean_inv);
    // The clean world VP itself, for the capture corpus: corpus_check factors
    // records against BOTH camera families offline, and the world family needs
    // the un-rotated matrix as its generation-0 base.
    std::memcpy(g_clean_world, clean, sizeof(g_clean_world));
    g_delta_seq.store(s + 2, std::memory_order_release);
    g_clean_valid.store(true, std::memory_order_relaxed);
    g_delta_valid.store(have_delta, std::memory_order_relaxed);
}

uint32_t clean_world_seq() { return g_delta_seq.load(std::memory_order_acquire); }
bool clean_world_copy(float* out16) {
    if (!out16 || !g_clean_valid.load(std::memory_order_relaxed)) return false;
    for (int a = 0; a < 8; ++a) {
        const uint32_t before = g_delta_seq.load(std::memory_order_acquire);
        if (before & 1u) continue;
        std::memcpy(out16, g_clean_world, sizeof(float) * 16);
        if (g_delta_seq.load(std::memory_order_acquire) == before) return true;
    }
    return false;
}

bool clean_vp_inverse(float* out16) {
    if (!out16 || !g_clean_valid.load(std::memory_order_relaxed)) return false;
    for (int a = 0; a < 8; ++a) {
        const uint32_t before = g_delta_seq.load(std::memory_order_acquire);
        if (before & 1u) continue;
        std::memcpy(out16, g_clean_inv, sizeof(float) * 16);
        if (g_delta_seq.load(std::memory_order_acquire) == before) return true;
    }
    return false;
}

bool head_delta(float* out16) {
    if (!out16 || !g_delta_valid.load(std::memory_order_relaxed)) return false;
    for (int a = 0; a < 8; ++a) {
        const uint32_t before = g_delta_seq.load(std::memory_order_acquire);
        if (before & 1u) continue;
        std::memcpy(out16, g_delta, sizeof(float) * 16);
        if (g_delta_seq.load(std::memory_order_acquire) == before) return true;
    }
    return false;
}

bool matches_reference_basis(const float* m) {
    float r[3], u[3], f[3];
    if (!m || !reference_basis(r, u, f)) return false;
    for (int i = 0; i < 16; ++i) if (!std::isfinite(m[i])) return false;
    const float l0 = row_length(m, 0), l1 = row_length(m, 1);
    if (!(l0 > 1e-6f) || !(l1 > 1e-6f)) return false;
    // Dot products of unit directions - scale-free, so this matches a plain
    // view matrix and a projection-scaled one alike. Both axes must agree, or
    // an unrelated matrix that happens to share one direction slips through.
    // ⚠ ALL THREE AXES, and forward is taken from whichever row holds it.
    //
    // Two axes were not enough. The same GUI matrix logged
    // matchesReferenceBasis=false and then =true on consecutive lines with
    // IDENTICAL values - a fixed axis-swizzle matrix that happened to align
    // with the live camera as it turned past. A coincidence like that latches
    // a wrong offset permanently, and I have been writing four CameraSrc
    // offsets and two per destination that were found this way.
    const float l2 = row_length(m, 2), l3 = row_length(m, 3);
    const bool affine = !(l3 > 1e-6f);
    const float lf = affine ? l2 : l3;
    if (!(lf > 1e-6f)) return false;
    const int fwd_row = affine ? 2 : 3;
    float d0 = 0.0f, d1 = 0.0f, d2 = 0.0f;
    for (int i = 0; i < 3; ++i) {
        d0 += (m[0 + i] / l0) * r[i];
        d1 += (m[4 + i] / l1) * u[i];
        d2 += (m[fwd_row * 4 + i] / lf) * f[i];
    }
    // ~2.5 degrees per axis. All three must agree: a single shared direction
    // is a coincidence, three simultaneously is the same camera.
    return d0 > 0.999f && d1 > 0.999f && d2 > 0.999f;
}

void publish_head_angles(float yaw, float pitch, float roll, bool valid) {
    if (!valid || !std::isfinite(yaw) || !std::isfinite(pitch) || !std::isfinite(roll)) {
        g_head_valid.store(false, std::memory_order_relaxed);
        return;
    }
    g_head_yaw.store(yaw, std::memory_order_relaxed);
    g_head_pitch.store(pitch, std::memory_order_relaxed);
    g_head_roll.store(roll, std::memory_order_relaxed);
    g_head_valid.store(true, std::memory_order_relaxed);
}

bool head_angles(float& yaw, float& pitch, float& roll) {
    if (!g_head_valid.load(std::memory_order_relaxed)) return false;
    yaw = g_head_yaw.load(std::memory_order_relaxed);
    pitch = g_head_pitch.load(std::memory_order_relaxed);
    roll = g_head_roll.load(std::memory_order_relaxed);
    return true;
}

float recenter_ref_yaw() { return g_ref_yaw_transfer.load(std::memory_order_relaxed); }
float recenter_ref_pitch() { return g_ref_pitch_transfer.load(std::memory_order_relaxed); }

// ⚠ THE SEAM'S ANGLES. Every consumer that ROTATES DOOM's camera must call
// this, never head_angles() - see the header's warning. Roll passes through
// untouched: DOOM's input path has no roll axis, so roll can never be
// transferred and always stays entirely at the seam.
bool residual_head_angles(float& yaw, float& pitch, float& roll) {
    if (!head_angles(yaw, pitch, roll)) return false;
    yaw = wrap_pi(yaw - g_ref_yaw_transfer.load(std::memory_order_relaxed));
    pitch -= g_ref_pitch_transfer.load(std::memory_order_relaxed);
    return std::isfinite(yaw) && std::isfinite(pitch);
}

void advance_recenter_ref(float committed_yaw, float committed_pitch) {
    if (!std::isfinite(committed_yaw) || !std::isfinite(committed_pitch)) return;
    if (committed_yaw == 0.0f && committed_pitch == 0.0f) return;
    g_ref_yaw_transfer.store(
        wrap_pi(g_ref_yaw_transfer.load(std::memory_order_relaxed) + committed_yaw),
        std::memory_order_relaxed);
    g_ref_pitch_transfer.store(
        g_ref_pitch_transfer.load(std::memory_order_relaxed) + committed_pitch,
        std::memory_order_relaxed);
    g_transfer_commits.fetch_add(1, std::memory_order_relaxed);
}

void reset_recenter_ref(const char* why) {
    g_ref_yaw_transfer.store(0.0f, std::memory_order_relaxed);
    g_ref_pitch_transfer.store(0.0f, std::memory_order_relaxed);
    const uint64_t n = g_transfer_resets.fetch_add(1, std::memory_order_relaxed);
    if (n < 20)
        log::warn(std::format("BodyFollow: recenter reference reset ({}). "
                              "Head and body are coincident again.", why ? why : "unstated"));
}

void note_transfer(float injected_px_x, float injected_px_y,
                   float committed_yaw, float committed_pitch) {
    g_transfer_px_x.store(injected_px_x, std::memory_order_relaxed);
    g_transfer_px_y.store(injected_px_y, std::memory_order_relaxed);
    g_transfer_deg_yaw.store(committed_yaw * 57.29577951f, std::memory_order_relaxed);
    g_transfer_deg_pitch.store(committed_pitch * 57.29577951f, std::memory_order_relaxed);
}

void note_transfer_gated(int reason) {
    if (reason < 0 || reason >= kTransferGateReasons) return;
    g_transfer_gates[reason].fetch_add(1, std::memory_order_relaxed);
}


const HeadAxes& head_axes() { return config().head_axes; }

bool head_world_rotation(float R[9]) { return head_world_rotation_scaled(R, 1.0f); }

// SCALE exists for the half-angle experiment (slot 1). The World path builds R
// in here from the PUBLISHED angles, so scaling a caller's local copy of hy
// does nothing at all - a fact that would have cost a run had it shipped.
bool head_world_rotation_scaled(float R[9], float scale) {
    if (!R) return false;
    float r[3], u[3], f[3];
    if (!reference_basis(r, u, f)) return false;
    float hy = 0.0f, hp = 0.0f, hr = 0.0f;
    // ⚠ RESIDUAL, for the same reason publish_head_delta uses it: Lever V must
    // receive exactly the rotation Lever A received, or the gun swims against
    // the world by the transferred amount.
    if (!residual_head_angles(hy, hp, hr)) return false;
    hy *= scale; hp *= scale; hr *= scale;
    // One-frame staleness note: the basis is published at Lever A time
    // (hook_render_gather_prepare); a consumer that runs earlier in the frame
    // sees last frame's clean basis. The basis moves at body-turn rates, so
    // the error is one frame of body motion applied to the ROTATION FRAME
    // only - not to the rotation amount - and decays to zero when the body is
    // still. Accepted; recorded here so it is not re-derived.
    return head_world_rotation_from_basis(r, u, f, hy, hp, hr, head_axes(), R);
}

void note_head_rotation_applied() {
    g_head_applied.fetch_add(1, std::memory_order_relaxed);
}
void note_head_rotation_refused() {
    g_head_refused.fetch_add(1, std::memory_order_relaxed);
}
uint64_t head_rotations_applied() {
    return g_head_applied.load(std::memory_order_relaxed);
}
uint64_t head_rotations_refused() {
    return g_head_refused.load(std::memory_order_relaxed);
}

void publish_look_injection(bool on) {
    g_look_injection.store(on, std::memory_order_relaxed);
    g_look_injection_known.store(true, std::memory_order_relaxed);
}

void publish_frame_aspect(float aspect) {
    if (aspect > 0.5f && aspect < 6.0f) g_frame_aspect.store(aspect, std::memory_order_relaxed);
}

void note_hook_call() {
    g_hook_calls.fetch_add(1, std::memory_order_relaxed);
}

void note_vp_read_failure() {
    g_attempts.fetch_add(1, std::memory_order_relaxed);
    g_read_failures.fetch_add(1, std::memory_order_relaxed);
}

SamplerCounts sampler_counts() {
    SamplerCounts c;
    c.hook_calls = g_hook_calls.load(std::memory_order_relaxed);
    c.attempts = g_attempts.load(std::memory_order_relaxed);
    c.read_failures = g_read_failures.load(std::memory_order_relaxed);
    c.wide_pass = g_vp_wide.samples.load(std::memory_order_relaxed);
    c.strict_pass = g_vp_strict.samples.load(std::memory_order_relaxed);
    c.census_lines = g_census_lines.load(std::memory_order_relaxed);
    return c;
}

namespace {
// ===========================================================================
// THE CENSUS - RUN 1's LESSON, BUILT IN.
//
// RUN 1 came back with worldViews=0 out of 987,318 views because a predicate
// calibrated at FOV 90 rejected every matrix at FOV 130. The run could not say
// WHY, because the only instrument was the predicate itself: a gate that
// answers "no" tells you nothing about what it was looking at.
//
// So this logs the matrices REGARDLESS of any predicate, deduplicated by their
// row-length signature and hard-budgeted. It is not a check; it is the raw
// distribution. Whatever predicate we end up wanting gets built from these
// lines rather than from a constant that was true at one slider setting.
//
// Cost is bounded three ways: it only runs on the 1-in-256 sampled subset, it
// takes the lock only while budget remains, and the budget is 48 lines for the
// whole session.
constexpr uint32_t kCensusBudget = 48;
constexpr size_t kCensusSignatures = 32;

std::mutex g_census_mutex;
std::vector<uint64_t> g_census_seen;

uint64_t row_signature(const float* m) {
    // Row lengths to 2 decimal places, packed. Two views whose lengths agree to
    // 1% are the same kind of view for this purpose.
    uint64_t sig = 0;
    for (int r = 0; r < 4; ++r) {
        const float l = row_length(m, r);
        const uint64_t q = static_cast<uint64_t>(
            std::lround(std::min(std::max(l, 0.0f), 600.0f) * 100.0f)) & 0xFFFFu;
        sig = (sig << 16) | q;
    }
    return sig;
}

void maybe_census(const float* m, bool strict, bool wide) {
    if (g_census_lines.load(std::memory_order_relaxed) >= kCensusBudget) return;
    {
        std::scoped_lock lock(g_census_mutex);
        if (g_census_lines.load(std::memory_order_relaxed) >= kCensusBudget) return;
        const uint64_t sig = row_signature(m);
        for (uint64_t seen : g_census_seen) if (seen == sig) return;
        if (g_census_seen.size() >= kCensusSignatures) return;
        g_census_seen.push_back(sig);
        g_census_lines.fetch_add(1, std::memory_order_relaxed);
    }
    log::warn(std::format(
        "VpCensus #{} strict={} wide={} rowLengths=({:.4f} {:.4f} {:.4f} {:.4f}) "
        "ratios l1/l0={:.4f} l0/l2={:.4f} | matrix={}\n"
        "READING IT: one line per DISTINCT row-length signature, no predicate applied. "
        "At FOV 90 the archive puts row 0 near 0.946 and row 1 in 1.65..1.90. Whichever "
        "rows MOVE when the FOV slider moves carry the projection; a row that holds still "
        "across 90 and 130 does not. l1/l0 should equal DOOM's frame aspect (1.879) if rows "
        "0 and 1 are the horizontal and vertical projection terms.",
        g_census_lines.load(std::memory_order_relaxed), strict, wide,
        row_length(m, 0), row_length(m, 1), row_length(m, 2), row_length(m, 3),
        row_length(m, 0) > 0.0f ? row_length(m, 1) / row_length(m, 0) : 0.0f,
        row_length(m, 2) > 0.0f ? row_length(m, 0) / row_length(m, 2) : 0.0f,
        fmt_matrix(m)));
}
}  // namespace

void publish_world_vp(const float m[16], bool passed_strict_predicate) {
    g_attempts.fetch_add(1, std::memory_order_relaxed);
    // CENSUS FIRST, AND UNGATED. Whatever the predicate decides, the raw
    // matrix is on the record - so a rejection can never again come back as a
    // silent zero with no way to tell what was rejected.
    //
    // WORLD-VIEW TEST, CORRECTED BY MEASUREMENT (RUN 1b). |row1|/|row0| equals
    // DOOM's frame aspect for world views and nothing else, and it is a ratio
    // of two terms that scale together - so unlike both previous predicates it
    // cannot move when the FOV slider moves. That is what killed RUN 1.
    const bool wide_ok = wide_world_vp_predicate(m) &&
                         is_world_view(m, g_frame_aspect.load(std::memory_order_relaxed));
    maybe_census(m, passed_strict_predicate, wide_ok);
    if (!wide_ok) return;
    if (passed_strict_predicate) {
        channel_store(g_vp_strict, m);
        g_strict_at_wide.store(g_vp_wide.samples.load(std::memory_order_relaxed),
                               std::memory_order_relaxed);
    }
    channel_store(g_vp_wide, m);

    const float l1 = row_length(m, 1);
    if (!g_row1_have.exchange(true, std::memory_order_relaxed)) {
        g_row1_min.store(l1, std::memory_order_relaxed);
        g_row1_max.store(l1, std::memory_order_relaxed);
    } else {
        float lo = g_row1_min.load(std::memory_order_relaxed);
        while (l1 < lo && !g_row1_min.compare_exchange_weak(lo, l1,
                                std::memory_order_relaxed)) {}
        float hi = g_row1_max.load(std::memory_order_relaxed);
        while (l1 > hi && !g_row1_max.compare_exchange_weak(hi, l1,
                                std::memory_order_relaxed)) {}
    }
}

bool world_vp(VpSnapshot& out) {
    const uint64_t strict = g_vp_strict.samples.load(std::memory_order_relaxed);
    const uint64_t wide = g_vp_wide.samples.load(std::memory_order_relaxed);
    out.strict_samples = strict;
    out.wide_samples = wide;
    // Prefer the strict channel: it is the project's own validated world-view
    // predicate and it excludes GUI and shadow views. Fall back to the wide
    // channel only when strict has never fired or has gone silent - which is
    // itself a finding, not a failure, and the Phase 0 line says which was used.
    //
    // FRESHNESS, NOT MERE EXISTENCE. If the strict predicate matched during
    // the menu and then stopped once the FOV slider moved, preferring "strict
    // has samples" would pin us to a matrix captured minutes ago and the
    // declared FOV would silently stop tracking the slider - a stale value
    // that reads back perfectly and proves nothing. Strict wins only if it
    // fired within the last 64 wide publishes, roughly the last half second.
    constexpr uint64_t kStrictFreshWindow = 64;
    const bool strict_fresh =
        strict > 0 && (wide - g_strict_at_wide.load(std::memory_order_relaxed))
                          <= kStrictFreshWindow;
    const VpChannel& ch = strict_fresh ? g_vp_strict : g_vp_wide;
    out.from_strict = strict_fresh;
    out.samples = out.from_strict ? strict : wide;
    if (out.samples == 0) return false;
    if (!channel_load(ch, out.m)) return false;
    for (int r = 0; r < 4; ++r) out.row_len[r] = row_length(out.m, r);
    return true;
}

// ===========================================================================
// bodyfollow=calibrate - TURNING kPixelsPerRadian INTO A MEASUREMENT
// ===========================================================================
//
// WHY IT LIVES HERE AND NOT IN THE XR POLL. The transfer itself belongs in
// update_look_injection, where the head pose is. Calibration does not: it is
// FLAT/MONITOR only, and on a headset-free session xrLocateViews never
// succeeds, so update_look_injection is never called at all. A calibration
// mode written there would have been unreachable on exactly the runs it is
// for - the "an unreachable probe looks exactly like a broken one" failure.
// This runs off the PRESENT hook, which runs with or without a headset.
//
// WHAT IT MEASURES. It sends a burst of a known number of mouse pixels during
// a quiet window and measures how far DOOM's OWN camera turned, read from
// clean_world_copy - the un-rotated camera, so the seam, headlook and synthyaw
// cannot contaminate it. deg-per-pixel is the ratio, reported as a mean with
// its spread across bursts.
//
// THE SIGNED STEP is taken in the camera's own right/forward plane:
//   step = atan2(dot(f_now, r_prev), dot(f_now, f_prev))
// which needs no world up-axis and is exactly the yaw for a yaw-only turn.
//
// REJECTION. A burst whose pre-window was not quiet, or whose settle window
// does not come to rest, overlapped player input and is thrown away with its
// reason counted. Bursts alternate sign so the view ends where it started and
// the mode cannot walk the player around the level.
namespace {

constexpr int   kCalQuietFrames   = 20;     // consecutive still frames required
constexpr float kCalQuietEps      = 0.0006f;// rad/frame - about 0.034 deg
constexpr int   kCalSettleFrames  = 14;
constexpr int   kCalTailFrames    = 4;      // of the settle window, must be still
constexpr int   kCalCooldown      = 30;
constexpr int   kCalBurstPx       = 300;
constexpr int   kCalWantSamples   = 12;
constexpr int   kCalMaxAttempts   = 60;     // stop rather than pester forever

enum class CalPhase { Quiet, Settle, Cooldown, Done, Refused };
CalPhase g_cal_phase = CalPhase::Quiet;
int   g_cal_quiet_run = 0;
int   g_cal_settle_i = 0;
int   g_cal_cool_i = 0;
int   g_cal_sign = 1;
int   g_cal_attempts = 0;
float g_cal_accum = 0.0f;
float g_cal_tail_max = 0.0f;
float g_cal_samples[kCalWantSamples]{};
int   g_cal_n = 0;
// ⚠ EDGE-TRIGGERED, not per-frame. Counting every noisy frame would make this
// read in the thousands during ordinary play and say nothing about how many
// bursts were actually lost - the aggregate-counter defect.
int   g_cal_rej_noisy_pre = 0;
bool  g_cal_noise_latch = false;
int   g_cal_rej_no_rest = 0;
int   g_cal_rej_no_basis = 0;
// A run where the player never stands still would otherwise print NOTHING and
// look exactly like a build where the mode never armed. This heartbeat makes
// the difference visible while the run is still happening.
int   g_cal_frames = 0;
bool  g_cal_have_prev = false;
float g_cal_prev_r[3]{};
float g_cal_prev_f[3]{};

// Unit right and forward out of the clean (un-rotated) world VP, in whichever
// layout it is in - same row selection apply_head_rotation uses, so the two
// cannot drift apart.
bool clean_basis_rf(float r[3], float f[3]) {
    float m[16];
    if (!clean_world_copy(m)) return false;
    const float l0 = row_length(m, 0);
    const float l2 = row_length(m, 2), l3 = row_length(m, 3);
    const bool affine = !(l3 > 1e-6f) || !std::isfinite(l3);
    const int fwd_row = affine ? 2 : 3;
    const float lf = affine ? l2 : l3;
    if (!(l0 > 1e-6f) || !(lf > 1e-6f)) return false;
    for (int i = 0; i < 3; ++i) {
        r[i] = m[0 + i] / l0;
        f[i] = m[fwd_row * 4 + i] / lf;
    }
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(r[i]) || !std::isfinite(f[i])) return false;
    return true;
}

void cal_send(int dx) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &input, sizeof(INPUT));
}

void cal_report(const char* when) {
    if (g_cal_n <= 0) {
        log::warn(std::format(
            "BFCAL {}: NO ACCEPTED BURSTS. attempts={} rejects: noisyPre={} noRest={} "
            "noBasis={}. A null here is instrument failure, not a calibration - the "
            "rejects say which half.",
            when, g_cal_attempts, g_cal_rej_noisy_pre, g_cal_rej_no_rest,
            g_cal_rej_no_basis));
        return;
    }
    float sum = 0.0f, lo = g_cal_samples[0], hi = g_cal_samples[0];
    for (int i = 0; i < g_cal_n; ++i) {
        sum += g_cal_samples[i];
        if (g_cal_samples[i] < lo) lo = g_cal_samples[i];
        if (g_cal_samples[i] > hi) hi = g_cal_samples[i];
    }
    const float mean = sum / static_cast<float>(g_cal_n);
    float var = 0.0f;
    for (int i = 0; i < g_cal_n; ++i) {
        const float d = g_cal_samples[i] - mean;
        var += d * d;
    }
    const float sd = g_cal_n > 1 ? std::sqrt(var / static_cast<float>(g_cal_n - 1)) : 0.0f;
    const float spread_pct = mean != 0.0f ? 100.0f * (hi - lo) / std::fabs(mean) : 0.0f;
    // The number the next run puts in the switch file. deg/px -> px/rad.
    const float px_per_rad = mean != 0.0f ? (57.29577951f / mean) : 0.0f;
    log::warn(std::format(
        "BFCAL {}: n={} accepted of {} attempts | degPerPixel mean={:.6f} sd={:.6f} "
        "min={:.6f} max={:.6f} spread={:.2f}%\n"
        "  => bfpxrad={:.1f}   (the switch-file value for the next run; the standing "
        "'tuned by feel' constant is 3000.0)\n"
        "  rejects: noisyPre={} noRest={} noBasis={} | burst={} px, quiet>={} frames "
        "at <{:.4f} rad/frame, settle={} frames.\n"
        "  POLARITY: healthy is spread of a few percent. A large spread means the "
        "bursts overlapped player input or the settle window is too short - the "
        "reject counters distinguish those.",
        when, g_cal_n, g_cal_attempts, mean, sd, lo, hi, spread_pct, px_per_rad,
        g_cal_rej_noisy_pre, g_cal_rej_no_rest, g_cal_rej_no_basis,
        kCalBurstPx, kCalQuietFrames, kCalQuietEps, kCalSettleFrames));
}

}  // namespace

void tick_body_calibration(bool xr_ready) {
    if (config().bodyfollow != BodyFollow::Calibrate) return;
    if (g_cal_phase == CalPhase::Done || g_cal_phase == CalPhase::Refused) return;
    // LIVENESS, printed the first time this actually RUNS - not at
    // registration. A mode that is configured but never reached looks
    // identical to one that found nothing, and this line is the difference.
    //
    // ⚠ THE LOOKINJECT FILE IS DELIBERATELY NOT CONSULTED HERE, and that is a
    // considered deviation from the plan, not an oversight. It is the master
    // arm for the TRANSFER, read via look_injection_enabled_ in the XR poll -
    // and the XR poll does not run on a headset-free session, which is the
    // only kind of session this mode is allowed in. Gating on a flag that can
    // never be published would make the mode silently inert. `bodyfollow=
    // calibrate` in the switch file is the explicit arm, and it says here, in
    // the run's own log, that synthetic mouse deltas are being sent.
    {
        static bool announced = false;
        if (!announced) {
            announced = true;
            log::warn(std::format(
                "BFCAL LIVE: bodyfollow=calibrate is running (headset-free). This mode SENDS "
                "SYNTHETIC MOUSE DELTAS - bursts of {} px, alternating sign so the view returns "
                "to where it started - during quiet windows only, and only in gameplay. It is "
                "armed by the switch file alone; C:\\dev\\doomvr-lookinject.txt gates the "
                "TRANSFER, which cannot run without a headset, so it is not consulted. "
                "Measuring deg-per-pixel off DOOM's own un-rotated camera.",
                kCalBurstPx));
        }
    }
    if (xr_ready) {
        g_cal_phase = CalPhase::Refused;
        log::error("BFCAL REFUSED: bodyfollow=calibrate is FLAT/MONITOR only and a "
                   "headset session is live. Scripted bursts would fight the real head "
                   "pose and the measurement would be meaningless. Nothing was injected.");
        return;
    }
    // Never inject into a menu or a loading screen - the same gate the
    // transfer uses, and the reason every mouse delta here is gated at all.
    VpSnapshot vp;
    if (!world_vp(vp) || !is_gameplay_view(vp.m)) {
        note_transfer_gated(2);
        g_cal_quiet_run = 0;
        g_cal_have_prev = false;
        return;
    }
    float r[3], f[3];
    if (!clean_basis_rf(r, f)) {
        ++g_cal_rej_no_basis;
        g_cal_have_prev = false;
        return;
    }
    float step = 0.0f;
    if (g_cal_have_prev) {
        float df = 0.0f, dr = 0.0f;
        for (int i = 0; i < 3; ++i) {
            df += f[i] * g_cal_prev_f[i];
            dr += f[i] * g_cal_prev_r[i];
        }
        step = std::atan2(dr, df);
        if (!std::isfinite(step)) step = 0.0f;
    }
    for (int i = 0; i < 3; ++i) { g_cal_prev_r[i] = r[i]; g_cal_prev_f[i] = f[i]; }
    if (!g_cal_have_prev) { g_cal_have_prev = true; return; }

    // PROGRESS HEARTBEAT. Roughly every ten seconds at 60 fps, so a run in
    // which nothing is ever quiet enough to fire says so while it is still
    // running instead of ending in silence.
    if ((++g_cal_frames % 600) == 0)
        log::warn(std::format(
            "BFCAL progress: phase={} accepted={}/{} attempts={} quietRun={}/{} "
            "lastStep={:.5f} rad | rejects noisyPre={} noRest={} noBasis={}. "
            "Bursts fire only after {} consecutive still frames - stand still for "
            "a couple of seconds at a time.",
            static_cast<int>(g_cal_phase), g_cal_n, kCalWantSamples, g_cal_attempts,
            g_cal_quiet_run, kCalQuietFrames, step, g_cal_rej_noisy_pre,
            g_cal_rej_no_rest, g_cal_rej_no_basis, kCalQuietFrames));

    switch (g_cal_phase) {
        case CalPhase::Quiet: {
            if (std::fabs(step) < kCalQuietEps) ++g_cal_quiet_run;
            else g_cal_quiet_run = 0;
            if (g_cal_quiet_run < kCalQuietFrames) return;
            if (g_cal_attempts >= kCalMaxAttempts) {
                g_cal_phase = CalPhase::Done;
                cal_report("attempt cap");
                return;
            }
            ++g_cal_attempts;
            g_cal_accum = 0.0f;
            g_cal_tail_max = 0.0f;
            g_cal_settle_i = 0;
            cal_send(kCalBurstPx * g_cal_sign);
            note_transfer(static_cast<float>(kCalBurstPx * g_cal_sign), 0.0f, 0.0f, 0.0f);
            g_cal_phase = CalPhase::Settle;
            return;
        }
        case CalPhase::Settle: {
            g_cal_accum += step;
            ++g_cal_settle_i;
            if (g_cal_settle_i > kCalSettleFrames - kCalTailFrames) {
                const float a = std::fabs(step);
                if (a > g_cal_tail_max) g_cal_tail_max = a;
            }
            if (g_cal_settle_i < kCalSettleFrames) return;
            if (g_cal_tail_max >= kCalQuietEps) {
                // Still moving when the window closed: either the burst is
                // longer than the window or the player was turning. Rejected
                // rather than folded into the mean.
                ++g_cal_rej_no_rest;
            } else {
                const float deg = std::fabs(g_cal_accum) * 57.29577951f;
                const float per_px = deg / static_cast<float>(kCalBurstPx);
                if (std::isfinite(per_px) && per_px > 0.0f && g_cal_n < kCalWantSamples)
                    g_cal_samples[g_cal_n++] = per_px;
                log::warn(std::format(
                    "BFCAL burst {}: {} px -> {:.4f} deg ({:.6f} deg/px), tailMax={:.6f} rad. "
                    "n={}/{}",
                    g_cal_attempts, kCalBurstPx * g_cal_sign, g_cal_accum * 57.29577951f,
                    per_px, g_cal_tail_max, g_cal_n, kCalWantSamples));
            }
            g_cal_sign = -g_cal_sign;   // return the view to where it started
            g_cal_quiet_run = 0;
            g_cal_cool_i = 0;
            g_cal_phase = (g_cal_n >= kCalWantSamples) ? CalPhase::Done : CalPhase::Cooldown;
            if (g_cal_phase == CalPhase::Done) cal_report("complete");
            return;
        }
        case CalPhase::Cooldown: {
            if (std::fabs(step) >= kCalQuietEps) {
                // Player input during the cooldown: the NEXT pre-window would
                // have been contaminated. Counted ONCE per contiguous episode
                // (see the latch) so this reads as "how many quiet windows the
                // player interrupted", not "how many frames had motion".
                if (!g_cal_noise_latch) { g_cal_noise_latch = true; ++g_cal_rej_noisy_pre; }
                g_cal_cool_i = 0;
                return;
            }
            g_cal_noise_latch = false;
            if (++g_cal_cool_i < kCalCooldown) return;
            g_cal_quiet_run = 0;
            g_cal_phase = CalPhase::Quiet;
            return;
        }
        default:
            return;
    }
}

std::string bodyfollow_report() {
    if (config().bodyfollow == BodyFollow::Off && !g_transfer_commits.load(std::memory_order_relaxed))
        return {};
    float hy = 0.0f, hp = 0.0f, hr = 0.0f;
    const bool have_head = head_angles(hy, hp, hr);
    const float ref_y = g_ref_yaw_transfer.load(std::memory_order_relaxed);
    const float ref_p = g_ref_pitch_transfer.load(std::memory_order_relaxed);
    const float resid_y = have_head ? wrap_pi(hy - ref_y) : 0.0f;
    const float resid_p = have_head ? (hp - ref_p) : 0.0f;
    constexpr float kR2D = 57.29577951f;
    return std::format(
        "BODYFOLLOW mode={} live={} | dead={:.1f} deg rate={:.0f} deg/s pxrad={:.1f} | "
        "head={} residual=({:.2f}, {:.2f}) deg  ref=({:.2f}, {:.2f}) deg  "
        "lastCommit=({:.3f}, {:.3f}) deg from ({:.0f}, {:.0f}) px  commits={}\n"
        "  gated: off={} noFile={} notGameplay={} noPose={} calibHolds={}\n"
        "  calib: phase={} accepted={} attempts={} quietRun={} noBasis={} noRest={} "
        "noisyPre={} ticks={}   <- noBasis climbing = NO CLEAN CAMERA, the "
        "measurement has no input and the run is void\n"
        "  READING IT: residual bounded by the deadzone is the whole point - it is what "
        "the artifacts scale with. commits climbing while residual stays near the "
        "deadzone is a healthy transfer. residual growing past the deadzone with "
        "commits=0 means a gate is holding, and the five counters say which.",
        bodyfollow_name(config().bodyfollow),
        g_bodyfollow_runtime.load(std::memory_order_relaxed) ? "ARMED" : "KILLED(slot 1)",
        config().bf_dead_deg, config().bf_rate_deg_per_sec, config().bf_pixels_per_radian,
        have_head ? "yes" : "NO",
        resid_y * kR2D, resid_p * kR2D, ref_y * kR2D, ref_p * kR2D,
        g_transfer_deg_yaw.load(std::memory_order_relaxed),
        g_transfer_deg_pitch.load(std::memory_order_relaxed),
        g_transfer_px_x.load(std::memory_order_relaxed),
        g_transfer_px_y.load(std::memory_order_relaxed),
        g_transfer_commits.load(std::memory_order_relaxed),
        g_transfer_gates[0].load(std::memory_order_relaxed),
        g_transfer_gates[1].load(std::memory_order_relaxed),
        g_transfer_gates[2].load(std::memory_order_relaxed),
        g_transfer_gates[3].load(std::memory_order_relaxed),
        g_transfer_gates[4].load(std::memory_order_relaxed),
        static_cast<int>(g_cal_phase), g_cal_n, g_cal_attempts, g_cal_quiet_run,
        g_cal_rej_no_basis, g_cal_rej_no_rest, g_cal_rej_noisy_pre, g_cal_frames);
}
void begin_frame() {
    VpSnapshot vp;
    float chosen = kLegacyProjectionScale;
    bool live = false;
    if (config().scale_source == ScaleSource::Fixed) {
        chosen = config().fixed_scale;
        live = true;   // configured on purpose, not a fallback
    } else if (world_vp(vp)) {
        const float candidate =
            scale_candidate(vp.m, config().scale_source, config().fixed_scale);
        if (scale_plausible(candidate)) {
            chosen = candidate;
            live = true;
        }
    }
    g_frame_scale.store(chosen, std::memory_order_relaxed);
    g_frame_scale_live.store(live, std::memory_order_relaxed);

    if (!live) {
        // A REAL FALLBACK, AND IT SAYS SO. Silent degradation to the legacy
        // constant would look exactly like a working live read while the FOV
        // slider did nothing - which is the one thing RUN 1 exists to see.
        //
        // ERROR while armed (it changes the picture the owner is judging),
        // WARN while unarmed (nothing is using the value; the scale heartbeat
        // is the diagnostic there). Rate-limited to roughly one a minute so a
        // dead sampler cannot flood a five-minute log.
        const uint64_t n = g_fallback_count.fetch_add(1, std::memory_order_relaxed);
        if (n == 0 || (n % 3600) == 0) {
            const std::string text = std::format(
                "Wraparound scale FALLBACK #{}: no plausible live value from +0xC44 via '{}' "
                "(strictSamples={} wideSamples={} samplerCompiledIn={}). Using the legacy "
                "constant {:.4f}, so the declared FOV will NOT track DOOM's FOV slider. "
                "Real fallback, not a no-op.",
                n + 1, scale_source_name(config().scale_source),
                g_vp_strict.samples.load(std::memory_order_relaxed),
                g_vp_wide.samples.load(std::memory_order_relaxed), sampler_compiled_in(),
                kLegacyProjectionScale);
            if (config().armed) log::error(text); else log::warn(text);
        }
    }
}

float frame_scale(bool* was_live) {
    if (was_live) *was_live = g_frame_scale_live.load(std::memory_order_relaxed);
    return g_frame_scale.load(std::memory_order_relaxed);
}

void publish_framing(uint32_t eye, const Framing& f) {
    if (eye > 1) return;
    const uint32_t s = g_framing_seq[eye].load(std::memory_order_relaxed);
    g_framing_seq[eye].store(s + 1, std::memory_order_release);
    g_framing[eye] = f;
    g_framing_seq[eye].store(s + 2, std::memory_order_release);
}

bool last_framing(uint32_t eye, Framing& out) {
    if (eye > 1) return false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint32_t before = g_framing_seq[eye].load(std::memory_order_acquire);
        if (before & 1u) continue;
        out = g_framing[eye];
        if (g_framing_seq[eye].load(std::memory_order_acquire) != before) continue;
        return out.valid;
    }
    return false;
}

void note_submitted_fov(uint32_t eye, float l, float r, float u, float d) {
    if (eye > 1) return;
    g_submitted[eye][0].store(l, std::memory_order_relaxed);
    g_submitted[eye][1].store(r, std::memory_order_relaxed);
    g_submitted[eye][2].store(u, std::memory_order_relaxed);
    g_submitted[eye][3].store(d, std::memory_order_relaxed);
    g_submitted_have[eye].store(true, std::memory_order_relaxed);
}

void publish_xr_facts(const XrFacts& f) {
    const uint32_t s = g_xr_seq.load(std::memory_order_relaxed);
    g_xr_seq.store(s + 1, std::memory_order_release);
    g_xr_facts = f;
    g_xr_seq.store(s + 2, std::memory_order_release);
}

namespace {
bool read_xr_facts(XrFacts& out) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint32_t before = g_xr_seq.load(std::memory_order_acquire);
        if (before & 1u) continue;
        out = g_xr_facts;
        if (g_xr_seq.load(std::memory_order_acquire) != before) continue;
        return out.have;
    }
    return false;
}

void emit_phase0(uint32_t doom_src_w, uint32_t doom_src_h) {
    VpSnapshot vp;
    const bool have_vp = world_vp(vp);

    std::string candidates;
    if (have_vp) {
        for (int i = 0; i < static_cast<int>(ScaleSource::Count); ++i) {
            const auto s = static_cast<ScaleSource>(i);
            if (s == ScaleSource::Fixed) continue;
            const float v = scale_candidate(vp.m, s, config().fixed_scale);
            candidates += std::format(
                " {}={:.4f}(fovY={:.2f}deg{})", scale_source_name(s), v,
                scale_plausible(v) ? to_degrees(fov_from_scale(v)) : 0.0f,
                scale_plausible(v) ? "" : ", IMPLAUSIBLE");
        }
    } else {
        candidates = " NONE - the +0xC44 sampler has not published a single world-view matrix";
    }

    XrFacts xr;
    const bool have_xr = read_xr_facts(xr);
    std::string xr_text;
    if (have_xr) {
        for (int e = 0; e < 2; ++e) {
            xr_text += std::format(
                " eye{}: recommendedImageRect={}x{} (aspect {:.4f}) locatedFov L={:.2f} R={:.2f} "
                "U={:.2f} D={:.2f} deg -> {:.2f}x{:.2f} deg HxV;",
                e, xr.eye_w[e], xr.eye_h[e],
                xr.eye_h[e] ? static_cast<float>(xr.eye_w[e]) / static_cast<float>(xr.eye_h[e]) : 0.0f,
                to_degrees(xr.fov_l[e]), to_degrees(xr.fov_r[e]),
                to_degrees(xr.fov_u[e]), to_degrees(xr.fov_d[e]),
                to_degrees(xr.fov_r[e] - xr.fov_l[e]), to_degrees(xr.fov_u[e] - xr.fov_d[e]));
        }
    } else {
        xr_text = " NONE - no XR session located a view yet (expected on a headset-free run; "
                  "this half arrives on the next headset run and this line re-prints then)";
    }

    // The blit pair. Source is whatever is actually being displayed; on a
    // headset-free run there is no destination, and that is stated rather
    // than filled in with a plausible-looking zero.
    const uint32_t src_w = have_xr && xr.blit_src_w ? xr.blit_src_w : doom_src_w;
    const uint32_t src_h = have_xr && xr.blit_src_h ? xr.blit_src_h : doom_src_h;
    const float src_aspect = src_h ? static_cast<float>(src_w) / static_cast<float>(src_h) : 0.0f;
    std::string blit_text;
    if (have_xr && src_w && src_h && xr.eye_w[0] && xr.eye_h[0]) {
        const float dst_aspect =
            static_cast<float>(xr.eye_w[0]) / static_cast<float>(xr.eye_h[0]);
        const Framing f = compute_framing(
            static_cast<int32_t>(src_w), static_cast<int32_t>(src_h),
            xr.eye_w[0], xr.eye_h[0], frame_scale(nullptr));
        blit_text = std::format(
            "src={}x{} (aspect {:.4f}) -> dst={}x{} (aspect {:.4f}), stretch factor {:.4f}. "
            "Aspect-preserving crop WOULD be {}x{} at ({},{}) giving {:.2f}x{:.2f} deg HxV",
            src_w, src_h, src_aspect, xr.eye_w[0], xr.eye_h[0], dst_aspect,
            dst_aspect > 0.0f ? src_aspect / dst_aspect : 0.0f,
            f.crop_w, f.crop_h, f.crop_x, f.crop_y,
            to_degrees(f.cropped_fov_x), to_degrees(f.cropped_fov_y));
    } else {
        blit_text = std::format(
            "src={}x{} (aspect {:.4f}) -> dst=NONE (no XR swapchain this session)",
            src_w, src_h, src_aspect);
    }

    log::warn(std::format(
        "Phase0 wraparound={} | scalesrc={} pose={} | "
        "+0xC44 strictSamples={} wideSamples={} channel={} samplerCompiledIn={} "
        "rowLengths=({:.4f} {:.4f} {:.4f} {:.4f}) "
        "matrix={} | scaleCandidates:{} | ACTIVE scale={:.4f} live={} -> contentFovY={:.2f} deg "
        "| XR:{} | BLIT: {}\n"
        "READING IT: SETTLED BY RUN 1b (2026-08-25, 22:53), no longer a default. "
        "|row0|/|row3| is DOOM's HORIZONTAL projection term and |row1|/|row3| the VERTICAL "
        "one; their ratio is the frame aspect 1.8789 for every world view and for nothing "
        "else. Normalising by |row3| matters: the matrix carries an arbitrary per-view scale "
        "(eight different values in one session) and the RAW row lengths are that scale times "
        "the projection term. Measured, at two slider settings: slider 90 -> 0.9462/1.7778 "
        "(93.2 x 58.7 deg), slider 130 -> 0.4412/0.8290 (132.4 x 100.7 deg). The ratio "
        "0.9462/0.4412 = 2.1446 is tan(65)/tan(45) = 2.1445, so the slider IS the horizontal "
        "FOV, defined at 16:9 and rescaled to the frame's 1.8789 (the constant 0.9462 = "
        "1.7778/1.8789). wideSamples=0 means the sampler never fired and every number here is "
        "void. strictSamples=0 with wideSamples climbing is EXPECTED above ~FOV 100: "
        "looks_like_world_camera_matrix is FOV-coupled and stops matching. The WraparoundScale "
        "HEARTBEAT repeats these numbers once a minute.",
        config().armed ? "ARMED" : "OFF",
        scale_source_name(config().scale_source),
        config().pose_shared ? "shared" : "located",
        have_vp ? vp.strict_samples : 0, have_vp ? vp.wide_samples : 0,
        have_vp ? (vp.from_strict ? "strict" : "wide") : "none",
        sampler_compiled_in(),
        have_vp ? vp.row_len[0] : 0.0f, have_vp ? vp.row_len[1] : 0.0f,
        have_vp ? vp.row_len[2] : 0.0f, have_vp ? vp.row_len[3] : 0.0f,
        have_vp ? fmt_matrix(vp.m) : std::string("UNAVAILABLE"),
        candidates,
        frame_scale(nullptr), g_frame_scale_live.load(std::memory_order_relaxed),
        to_degrees(fov_from_scale(frame_scale(nullptr))),
        xr_text, blit_text));
}
}  // namespace

void maybe_emit_phase0(uint64_t presents, uint32_t doom_src_w, uint32_t doom_src_h) {
    // 300 presents is ~5 s of play - the same moment the existing INSTRUMENT
    // SELF-TEST fires, deliberately, so the two land together and a bad run
    // is abandoned once.
    if (presents < 300) return;
    const bool xr_now = g_xr_seq.load(std::memory_order_relaxed) != 0;
    if (!g_phase0_emitted_without_xr.exchange(true, std::memory_order_relaxed)) {
        emit_phase0(doom_src_w, doom_src_h);
        if (xr_now) g_phase0_emitted_with_xr.store(true, std::memory_order_relaxed);
        return;
    }
    // XR arrived after the first line: re-print once, so the XR half is never
    // simply missing from a session that had a headset.
    if (xr_now && !g_phase0_emitted_with_xr.exchange(true, std::memory_order_relaxed)) {
        emit_phase0(doom_src_w, doom_src_h);
    }
}

// ---------------------------------------------------------------------------
// SELF-TESTS
// ---------------------------------------------------------------------------
bool run_math_self_test(std::string* report) {
    int failures = 0, checks = 0;
    std::string detail;
    auto ck = [&](bool ok, const char* what) {
        ++checks;
        if (!ok) { ++failures; detail += std::format(" [{}]", what); }
    };
    auto near = [&](float a, float b, float tol, const char* what) {
        ++checks;
        if (!(std::fabs(a - b) <= tol)) {
            ++failures;
            detail += std::format(" [{}: {:.6f} vs {:.6f}]", what, a, b);
        }
    };

    // The legacy formula must still produce the virtual screen's FOV, or
    // "file absent = today" is no longer true.
    near(to_degrees(fov_from_scale(kLegacyProjectionScale)), 62.7771f, 0.01f, "legacyFov");

    // Identity: matching aspects must crop nothing and change no angle.
    {
        const Framing f = compute_framing(1920, 1080, 1280, 720, 1.639f);
        ck(f.valid && f.crop_w == 1920 && f.crop_h == 1080 && f.crop_x == 0 && f.crop_y == 0,
           "identityNoCrop");
        near(f.cropped_fov_y, f.content_fov_y, 1e-5f, "identityFovY");
        near(f.cropped_fov_x, f.content_fov_x, 1e-5f, "identityFovX");
    }

    // The property the phase turns on: declared angles carry the DEST aspect.
    {
        const int32_t src[][2] = {{3818, 2032}, {1280, 1600}, {1024, 1024}};
        const int32_t dst[][2] = {{2064, 2272}, {2000, 1000}, {1024, 1024}};
        for (const auto& s : src) {
            for (const auto& d : dst) {
                for (float sc : {1.0f, 1.639f, 3.079f}) {
                    const Framing f = compute_framing(s[0], s[1], d[0], d[1], sc);
                    ck(f.valid, "sweepValid");
                    const float ratio = std::tan(f.cropped_fov_x * 0.5f) /
                                        std::tan(f.cropped_fov_y * 0.5f);
                    near(ratio / (static_cast<float>(d[0]) / static_cast<float>(d[1])),
                         1.0f, 0.006f, "sweepAspect");
                    near(f.angle_up - f.angle_down, f.cropped_fov_y, 1e-6f, "sweepDeclY");
                    near(f.angle_right - f.angle_left, f.cropped_fov_x, 1e-6f, "sweepDeclX");
                    ck(f.crop_x >= 0 && f.crop_y >= 0 && f.crop_x + f.crop_w <= s[0] &&
                       f.crop_y + f.crop_h <= s[1], "sweepInside");
                    ck(f.cropped_fov_x <= f.content_fov_x + 1e-5f &&
                       f.cropped_fov_y <= f.content_fov_y + 1e-5f, "sweepNeverWidens");
                }
            }
        }
    }

    // The FOV-matched crop, against RUN 1b's measured headset: DOOM 3818x2032
    // at slider 130 into a 110x90 deg eye must come out AT 110x90, and at
    // slider 90 - where the content is narrower than the eye - must clamp and
    // declare the content's own 58.7 deg rather than invent coverage.
    {
        const float kDeg = 3.14159265358979323846f / 180.0f;
        const Framing wide = compute_framing(3818, 2032, 4608, 4224, 0.8290f,
                                             110.0f * kDeg, 90.0f * kDeg);
        ck(wide.valid, "fovCropValid");
        near(to_degrees(wide.cropped_fov_x), 110.0f, 0.2f, "fovCropFillsX");
        near(to_degrees(wide.cropped_fov_y), 90.0f, 0.2f, "fovCropFillsY");
        near(wide.angle_up - wide.angle_down, wide.cropped_fov_y, 1e-6f, "fovCropTied");
        ck(wide.crop_w <= 3818 && wide.crop_h <= 2032, "fovCropInside");
        const Framing narrow = compute_framing(3818, 2032, 4608, 4224, 1.7778f,
                                               110.0f * kDeg, 90.0f * kDeg);
        ck(narrow.crop_h == 2032, "fovCropClampsHeight");
        near(to_degrees(narrow.cropped_fov_y), 58.72f, 0.2f, "fovCropDeclaresContent");
        // The destination rect must be the crop written 1:1 when it fits, and
        // must never exceed either the crop or the swapchain. Submitting a
        // larger imageRect than we wrote would have the runtime sample pixels
        // that were never touched.
        ck(wide.dst_w == wide.crop_w && wide.dst_h == wide.crop_h, "dstRectOneToOne");
        ck(wide.dst_w <= 4608 && wide.dst_h <= 4224, "dstRectFitsSwapchain");
        const Framing tiny = compute_framing(3818, 2032, 1024, 512, 0.8290f,
                                             110.0f * kDeg, 90.0f * kDeg);
        ck(tiny.dst_w > 0 && tiny.dst_h > 0 && tiny.dst_w <= 1024 && tiny.dst_h <= 512,
           "dstRectClamps");
        near(tiny.cropped_fov_y, wide.cropped_fov_y, 1e-5f, "dstRectDoesNotMoveGeometry");
    }

    ck(!compute_framing(0, 1080, 100, 100, 1.639f).valid, "degenSrcW");
    ck(!compute_framing(1920, 1080, 100, 0, 1.639f).valid, "degenDstH");
    ck(!compute_framing(1920, 1080, 100, 100, 0.0f).valid, "degenScale");
    ck(!compute_framing(1920, 1080, 100, 100,
                        std::numeric_limits<float>::quiet_NaN()).valid, "degenNaN");

    // Pose round-trip in the SAME convention update_look_injection uses.
    for (int yi = -17; yi <= 17; ++yi) {
        for (int pi = -8; pi <= 8; ++pi) {
            const float yaw = static_cast<float>(yi) * 0.18f;
            const float pitch = static_cast<float>(pi) * 0.17f;
            const Quat q = quat_from_yaw_pitch(yaw, pitch);
            float ry = 0.0f, rp = 0.0f;
            ck(yaw_pitch_from_quat(q, ry, rp), "poseFinite");
            float dy = ry - yaw;
            while (dy > 3.14159265f) dy -= 6.28318531f;
            while (dy < -3.14159265f) dy += 6.28318531f;
            near(dy, 0.0f, 2e-4f, "poseYaw");
            near(rp, pitch, 2e-4f, "posePitch");
            near(std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w), 1.0f,
                 1e-5f, "poseUnit");
        }
    }

    // strip_roll preserves the forward direction and is idempotent.
    {
        auto mul = [](const Quat& a, const Quat& b) {
            return Quat{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
        };
        for (int yi = -5; yi <= 5; ++yi) {
            for (int pi = -3; pi <= 3; ++pi) {
                for (int ri = -4; ri <= 4; ++ri) {
                    const Quat base = quat_from_yaw_pitch(static_cast<float>(yi) * 0.5f,
                                                          static_cast<float>(pi) * 0.35f);
                    const float roll = static_cast<float>(ri) * 0.25f;
                    const Quat rolled =
                        mul(base, Quat{0.0f, 0.0f, std::sin(roll * 0.5f), std::cos(roll * 0.5f)});
                    const Quat s = strip_roll(rolled);
                    float bx, by, bz, sx, sy, sz;
                    forward_from_quat(base, bx, by, bz);
                    forward_from_quat(s, sx, sy, sz);
                    near(sx, bx, 3e-4f, "rollFwdX");
                    near(sy, by, 3e-4f, "rollFwdY");
                    near(sz, bz, 3e-4f, "rollFwdZ");
                    const Quat t = strip_roll(s);
                    near(t.x, s.x, 1e-5f, "rollIdemX");
                    near(t.w, s.w, 1e-5f, "rollIdemW");
                }
            }
        }
    }

    // Scale candidates must be wired to distinct terms.
    {
        float m[16] = {1.0f, 1.639f, 0.0f, 0.0f,
                       0.0f, 3.0f,   4.0f, 0.0f,
                       0.0f, 0.0f,   1.0f, 0.0f,
                       0.0f, 0.0f,   0.0f, 1.0f};
        near(scale_candidate(m, ScaleSource::Row1Len, 0.0f), 5.0f, 1e-5f, "candRow1");
        near(scale_candidate(m, ScaleSource::Elem1, 0.0f), 1.639f, 1e-5f, "candElem1");
        near(scale_candidate(m, ScaleSource::Elem6, 0.0f), 4.0f, 1e-5f, "candElem6");
        near(scale_candidate(m, ScaleSource::Elem5, 0.0f), 3.0f, 1e-5f, "candElem5");
        ck(scale_plausible(1.639f) && !scale_plausible(0.0f) && !scale_plausible(1e9f) &&
           !scale_plausible(std::numeric_limits<float>::quiet_NaN()), "band");
    }

    // THE RUN 1 REGRESSION CASE. Archived ViewOriginScan lines put row 0's
    // length at ~0.946 at FOV 90; the move to 130 scales the projection terms
    // by tan(45)/tan(65) = 0.466, taking it to ~0.441. The first wide predicate
    // carried an l0 > 0.85 floor inherited from the FOV-90 build and rejected
    // every matrix, so RUN 1 came back with zero samples. This runs INSIDE the
    // DLL as well as offline, because it is the exact defect that cost a run.
    {
        float m[16] = {0.441f, 0.0f,   0.0f, 1234.5f,
                       0.0f,   0.875f, 0.0f, -67.25f,
                       0.0f,   0.0f,   1.0f, 42.0f,
                       0.0f,   1.0f,   0.0f, 1.0f};
        ck(wide_world_vp_predicate(m), "run1Fov130NotRejected");
        float zero[16]{};
        ck(!wide_world_vp_predicate(zero), "run1AllZeroRejected");
    }

    // RUN 1b's MEASURED VALUES. Session 2026-08-25 22:53, off the wire: the
    // same world view at eight different overall matrix scales must normalise
    // to one pair of projection terms, and the non-world views in that same
    // session must be rejected. If a later change breaks the normalisation or
    // the world-view test, this fires before the owner spends a run.
    {
        const float aspect = 3818.0f / 2032.0f;
        auto build = [](float l0, float l1, float l3, float* m) {
            for (int i = 0; i < 16; ++i) m[i] = 0.0f;
            m[0] = l0; m[3] = -91.77f;
            m[6] = l1; m[7] = 838.02f;
            m[9] = 1.0f; m[11] = -243.08f;
            m[13] = l3; m[15] = -240.0f;
        };
        const float world[][3] = {
            {0.4412f, 0.8290f, 1.0000f}, {0.5294f, 0.9948f, 1.2000f},
            {0.6662f, 1.2517f, 1.5099f}, {0.6579f, 1.2362f, 1.4912f},
            {0.3188f, 0.5991f, 0.7226f}, {0.3479f, 0.6537f, 0.7886f},
            {0.5736f, 1.0777f, 1.3000f}, {0.3821f, 0.7180f, 0.8661f}};
        for (const auto& r : world) {
            float m[16];
            build(r[0], r[1], r[2], m);
            ck(is_world_view(m, aspect), "run1bWorldViewRecognised");
            near(scale_candidate(m, ScaleSource::Row0OverRow3, 0.0f), 0.4412f, 5e-4f,
                 "run1bSx130");
            near(scale_candidate(m, ScaleSource::Row1OverRow3, 0.0f), 0.8290f, 5e-4f,
                 "run1bSy130");
        }
        {   // FOV 90, from the heartbeat line.
            float m[16];
            build(0.9462f, 1.7778f, 1.0000f, m);
            ck(is_world_view(m, aspect), "run1bFov90Recognised");
            near(scale_candidate(m, ScaleSource::Row0OverRow3, 0.0f), 0.9462f, 5e-4f,
                 "run1bSx90");
        }
        for (float bad : {1.6967f, 2.2544f, 2.5740f, 3.4590f}) {
            float m[16];
            build(0.5f, 0.5f * bad, 1.0f, m);
            ck(!is_world_view(m, aspect), "run1bNonWorldRejected");
        }
        {   // A zero w-row must yield 0, not infinity, so the fallback fires.
            float m[16];
            build(0.4412f, 0.8290f, 0.0f, m);
            near(scale_candidate(m, ScaleSource::Row1OverRow3, 0.0f), 0.0f, 1e-6f,
                 "run1bZeroRow3");
        }
    }

    // LEVER V WORLD-FRAME ROTATION (2026-08-26). The two properties whose
    // failure would waste the next run: R reproduces the world camera's own
    // rotation, and the affine apply holds the encoded camera position fixed.
    // The full sweep lives in tests/wraparound_math_test.cpp section 8h.
    {
        const HeadAxes id{};
        float cr[3]{0.0f, -1.0f, 0.0f}, cu[3]{0.0f, 0.0f, 1.0f}, cf[3]{1.0f, 0.0f, 0.0f};
        float mr[3]{cr[0], cr[1], cr[2]}, mu[3]{cu[0], cu[1], cu[2]}, mf[3]{cf[0], cf[1], cf[2]};
        ck(apply_head_rotation_basis(mr, mu, mf, 0.5f, -0.3f, 0.1f, id), "wfrMix");
        float R[9];
        ck(head_world_rotation_from_basis(cr, cu, cf, 0.5f, -0.3f, 0.1f, id, R), "wfrBuild");
        float t[3];
        for (int i = 0; i < 3; ++i)
            t[i] = R[i * 3 + 0] * cf[0] + R[i * 3 + 1] * cf[1] + R[i * 3 + 2] * cf[2];
        near(t[0], mf[0], 1e-4f, "wfrFwdX");
        near(t[1], mf[1], 1e-4f, "wfrFwdY");
        near(t[2], mf[2], 1e-4f, "wfrFwdZ");
        // The affine apply: gameplay-scale camera position stays the fixed point.
        const float cam[3]{-6135.5f, -2654.6f, 11405.3f};
        float v[16]{};
        const float* rows[3]{cr, cu, cf};
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) v[i * 4 + j] = rows[i][j];
            v[i * 4 + 3] = -(rows[i][0] * cam[0] + rows[i][1] * cam[1] + rows[i][2] * cam[2]);
        }
        v[15] = 1.0f;
        ck(apply_world_rotation_affine_view(v, R), "wfrApply");
        for (int i = 0; i < 3; ++i) {
            const float a = v[i * 4 + 0] * cam[0] + v[i * 4 + 1] * cam[1] +
                            v[i * 4 + 2] * cam[2] + v[i * 4 + 3];
            near(a, 0.0f, 1e-2f, "wfrEyeFixed");
        }
        // The projective renderView layout must refuse - this primitive is for
        // the affine GUI site only, and a silent wrong-layout write is the
        // class of defect that cost four builds.
        float proj[16]{-0.3116f, -0.3123f, 0.0f, -6135.5f,
                       -0.0018f,  0.0018f, 0.829f, -2654.6f,
                        0.7082f, -0.7065f, 0.0031f, 11405.3f,
                        0.7079f, -0.7063f, 0.0031f, 11404.1f};
        ck(!apply_world_rotation_affine_view(proj, R), "wfrProjRefused");
    }

    // THE HEAD/BODY TRANSFER INVARIANT (2026-08-29, thread A). The full
    // property sweep lives in tests/wraparound_math_test.cpp; this is the load
    // -bearing core of it, run INSIDE the DLL so a build whose transfer math
    // differs from the tested math says so in the first second of the log
    // rather than in the headset. Camera invariance under a committed
    // transfer, at exact calibration, is the whole stability argument.
    {
        BodyTransferState st;
        float game_yaw = 0.31f;
        const float head = 1.2f;
        const float camera0 = game_yaw + wrap_pi(head - st.ref_yaw);
        for (int frame = 0; frame < 300; ++frame) {
            const BodyTransferStep s =
                body_transfer_step(st, head, 0.0f, 0.14f, 2.0f, 1.0f / 90.0f);
            game_yaw += s.want_yaw;
            body_transfer_commit(st, s.want_yaw, 0.0f);
            near(game_yaw + wrap_pi(head - st.ref_yaw), camera0, 2e-4f, "bfInvariant");
        }
        near(std::fabs(wrap_pi(head - st.ref_yaw)), 0.14f, 1e-3f, "bfSettlesAtBand");
        BodyTransferState idle;
        const BodyTransferStep in_band =
            body_transfer_step(idle, 0.10f, 0.0f, 0.20f, 2.0f, 1.0f / 90.0f);
        near(in_band.want_yaw, 0.0f, 1e-9f, "bfInBandCommitsNothing");
        near(deadzone_excess(0.20f, 0.20f), 0.0f, 1e-7f, "bfNoJumpAtEdge");
        near(wrap_pi(6.28318531f + 0.25f), 0.25f, 1e-5f, "bfWrapPi");
    }

    if (report) {
        *report = failures == 0
            ? std::format("{} checks, 0 failures", checks)
            : std::format("{} checks, {} FAILURES:{}", checks, failures, detail);
    }
    return failures == 0;
}

void startup_self_test() {
    static bool done = false;
    if (done) return;
    done = true;
    (void)config();   // forces the config line to print on every session
    std::string report;
    if (run_math_self_test(&report)) {
        log::warn(std::format(
            "WRAPAROUND MATH SELF-TEST: PASS - {}. The crop rectangle and the declared FOV come "
            "out of one function and agree across the whole sweep; the pose convention "
            "round-trips against the one look injection uses. Same assertions as "
            "tests/wraparound_math_test.cpp.", report));
    } else {
        log::error(std::format(
            "WRAPAROUND MATH SELF-TEST: ***FAIL - ABORT THIS RUN***. {}. The declared FOV and "
            "the blitted region cannot be trusted to agree. Do NOT play on - quit now and say "
            "so; the build is broken and the run would be wasted.", report));
    }
}

void present_self_test(uint64_t presents) {
    static bool done = false;
    if (done || presents < 300) return;
    done = true;

    const Config& c = config();
    const uint64_t samples = g_vp_wide.samples.load(std::memory_order_relaxed);
    std::string problems;

    // LIVENESS, BY STAGE. RUN 1 proved one counter is not enough: worldViews
    // read zero and could have meant the hook was dead, the read failed, or the
    // predicate rejected - three different runs wearing the same face. Each
    // stage now has its own number and its own sentence.
    const SamplerCounts sc = sampler_counts();
    if (sc.hook_calls == 0) {
        problems += std::format(
            " [RENDER-VIEW HOOK NEVER RAN in {} presents (compiledIn={}). Nothing downstream "
            "of it can have a value. This is the hook, not the predicate.]",
            presents, sampler_compiled_in());
    } else if (sc.attempts == 0) {
        problems += std::format(
            " [hook ran {} times but the 1-in-256 sampling gate never opened - the gate itself "
            "is broken, not the matrix.]", sc.hook_calls);
    } else if (sc.read_failures == sc.attempts) {
        problems += std::format(
            " [+0xC44 UNREADABLE on all {} attempts across {} hook calls. The address or the "
            "object identity is wrong in this build - not a predicate problem.]",
            sc.attempts, sc.hook_calls);
    } else if (samples == 0) {
        problems += std::format(
            " [{} reads SUCCEEDED and every one was rejected by the wide predicate "
            "({} read failures of {} attempts). The matrices are on the record as VpCensus "
            "lines - read those; do NOT re-derive a predicate from a constant.]",
            sc.attempts - sc.read_failures, sc.read_failures, sc.attempts);
    }

    // ARM STATE READ FROM THE FILE AT THE MEASUREMENT, not from the flag we
    // set at startup - a run whose arm label is asserted rather than checked
    // is how six runs got confounded before.
    {
        FILE* f = nullptr;
        const bool file_now = fopen_s(&f, kSwitchPath, "r") == 0 && f;
        if (f) std::fclose(f);
        if (file_now != c.armed) {
            problems += std::format(
                " [switch file is {} NOW but the session armed as {} - the file changed after "
                "launch; this run's arm label is unreliable]",
                file_now ? "PRESENT" : "ABSENT", c.armed ? "ARMED" : "OFF");
        }
    }

    if (!c.armed) {
        // The regression arm. Nothing below Phase 0 may have executed, and the
        // proof is that no eye ever published a framing.
        Framing f;
        const bool any = last_framing(0, f) || last_framing(1, f);
        if (any) {
            problems += " [NOT ARMED, yet an eye published a wraparound framing - the switch "
                        "file is not actually gating the new path. This is the regression arm "
                        "failing.]";
        }
        if (problems.empty()) {
            log::warn(std::format(
                "WRAPAROUND SELF-TEST: PASS (arm=OFF) - {} +0xC44 samples, no wraparound framing "
                "was ever produced, so the virtual-screen path is intact. Phase 0's line is the "
                "only thing this build added to an unarmed session.", samples));
        }
    } else if (g_xr_seq.load(std::memory_order_relaxed) == 0) {
        // ARMED, BUT THERE IS NO HEADSET IN THIS SESSION. The wraparound path
        // cannot engage without a located view, so "not engaged" here is the
        // expected state, not a defect - calling ABORT on it would train the
        // owner to ignore the abort. Reported at WARN so the arm label on this
        // session is still unambiguous in the log.
        log::warn(std::format(
            "WRAPAROUND SELF-TEST: NOT APPLICABLE (arm=ON, no XR session) - {} +0xC44 samples "
            "and no located view in {} presents, so nothing wraparound-specific has run. This "
            "is a headset-free session with the switch file present. Phase 0's DOOM-side half "
            "is still valid; its XR half is not, and the line says so.{}",
            samples, presents, problems));
        if (!problems.empty()) {
            log::error(std::format(
                "WRAPAROUND SELF-TEST: ***ABORT THIS RUN***.{}", problems));
        }
        return;
    } else {
        // ARMED. Declaration must equal content on both eyes, read back from
        // the fov we actually submitted rather than from the framing struct.
        for (uint32_t eye = 0; eye < 2; ++eye) {
            Framing f;
            if (!last_framing(eye, f)) {
                problems += std::format(
                    " [eye {} never produced a wraparound framing in {} presents - armed but "
                    "not engaged]", eye, presents);
                continue;
            }
            if (!g_submitted_have[eye].load(std::memory_order_relaxed)) {
                problems += std::format(" [eye {} never submitted a projection view]", eye);
                continue;
            }
            const float du = g_submitted[eye][2].load(std::memory_order_relaxed);
            const float dd = g_submitted[eye][3].load(std::memory_order_relaxed);
            const float dr = g_submitted[eye][1].load(std::memory_order_relaxed);
            const float dl = g_submitted[eye][0].load(std::memory_order_relaxed);
            if (std::fabs((du - dd) - f.cropped_fov_y) > 1e-4f ||
                std::fabs((dr - dl) - f.cropped_fov_x) > 1e-4f) {
                problems += std::format(
                    " [eye {} DECLARATION/CONTENT MISMATCH: submitted {:.4f}x{:.4f} deg but the "
                    "blitted crop {}x{} subtends {:.4f}x{:.4f} deg]",
                    eye, to_degrees(dr - dl), to_degrees(du - dd), f.crop_w, f.crop_h,
                    to_degrees(f.cropped_fov_x), to_degrees(f.cropped_fov_y));
            }
        }
        // ===== THE TWO PRECONDITIONS THAT COST THE 23:18 RUN ==============
        //
        // Both were true of that run, neither was checked, and together they
        // produced exactly what the owner saw: "a 16x9 screen PINNED to my
        // head". Encoded here so the same run cannot be spent twice.
        // PHASE 3 INVERTS THIS. Lever A1 now rotates DOOM's world camera by the
        // head directly, so look injection must be OFF: it turns the PLAYER via
        // synthetic mouse deltas, which would rotate the body and the gun with
        // the head AND double-apply the rotation on top of the matrix write.
        // ⚠ NO LONGER UNCONDITIONAL (2026-08-29). The pairing is illegal only
        // while bodyfollow is OFF. That is the whole point of the transfer:
        // the seam rotates by head - recenterRef, and recenterRef advances by
        // exactly what the injection committed, so the two compose instead of
        // double-applying. Leaving this as a blanket abort would have fired
        // ***ABORT THIS RUN*** on the very configuration the transfer was
        // built to make legal - a stale guard turning a valid run into a
        // wasted one.
        if (g_look_injection.load(std::memory_order_relaxed) &&
            bodyfollow_mode() == BodyFollow::Off) {
            problems +=
                " [LOOK INJECTION IS ON while head look is ARMED and bodyfollow is OFF. Both are "
                "rotating the view: the matrix write turns the camera, and look injection turns "
                "the PLAYER via synthetic mouse deltas. You would get double the turn rate AND "
                "the gun swinging with your head. Either delete "
                "C:\\dev\\doomvr-lookinject.txt, or set bodyfollow=on so the recenter-reference "
                "subtraction makes the two compose.]";
        }
        // LIVENESS OF THE WRITE ITSELF. The declared pose tracks the head
        // unconditionally. If the rotation is not landing, the content does not
        // follow and the image welds to the head - the exact 23:18 failure, and
        // it is invisible from any counter upstream of the write.
        {
            const uint64_t applied = head_rotations_applied();
            const uint64_t refused = head_rotations_refused();
            if (applied == 0) {
                problems += std::format(
                    " [HEAD ROTATION NEVER APPLIED in {} presents (refused={}). Lever A1's write "
                    "is not reaching DOOM's world camera, so the world will NOT follow your head "
                    "while the declared pose does - a screen welded to your face. refused>0 means "
                    "the site ran and declined (no head pose, or the matrix was not the expected "
                    "layout); refused=0 means the site never ran at all.]",
                    presents, refused);
            } else if (refused > applied) {
                problems += std::format(
                    " [HEAD ROTATION MOSTLY REFUSED: applied={} refused={}. The camera is "
                    "following your head only intermittently, which reads as stutter or "
                    "snapping rather than as a clean failure.]", applied, refused);
            }
        }
        // Content narrower than the headset cannot fill it. No crop, no pose
        // and no declaration can invent field of view - it will be a floating
        // rectangle, and it will be reported as "still a screen".
        XrFacts xr;
        if (read_xr_facts(xr)) {
            Framing fc;
            if (last_framing(0, fc)) {
                const float head_x = xr.fov_r[0] - xr.fov_l[0];
                const float head_y = xr.fov_u[0] - xr.fov_d[0];
                if (fc.cropped_fov_x < head_x - 0.02f || fc.cropped_fov_y < head_y - 0.02f) {
                    problems += std::format(
                        " [CONTENT IS NARROWER THAN THE HEADSET: {:.1f}x{:.1f} deg of content "
                        "into a {:.1f}x{:.1f} deg eye. It will appear as a floating rectangle "
                        "with empty space around it, NOT as wraparound, and no amount of "
                        "cropping or pose work can change that - we cannot invent field of "
                        "view. RAISE DOOM's in-game FOV slider (measured: slider 90 gives "
                        "93x59 deg, slider 130 gives 132x101 deg).]",
                        to_degrees(fc.cropped_fov_x), to_degrees(fc.cropped_fov_y),
                        to_degrees(head_x), to_degrees(head_y));
                }
            }
        }

        if (problems.empty()) {
            Framing f0;
            last_framing(0, f0);
            log::warn(std::format(
                "WRAPAROUND SELF-TEST: PASS (arm=ON) - {} +0xC44 samples, both eyes engaged, and "
                "the fov submitted to the compositor equals the fov the blitted crop subtends on "
                "both eyes. Eye 0 crop {}x{} at ({},{}) -> {:.2f}x{:.2f} deg HxV, scale {:.4f} "
                "live={}. This run is worth playing out.",
                samples, f0.crop_w, f0.crop_h, f0.crop_x, f0.crop_y,
                to_degrees(f0.cropped_fov_x), to_degrees(f0.cropped_fov_y), f0.scale,
                g_frame_scale_live.load(std::memory_order_relaxed)));
        }
    }

    if (!problems.empty()) {
        log::error(std::format(
            "WRAPAROUND SELF-TEST: ***FAIL - ABORT THIS RUN***.{}\n"
            "Do NOT play on - quit now and say so; the build is broken and the run would be "
            "wasted. Five minutes of play cannot answer a question the instrument cannot ask.",
            problems));
    }
}

void maybe_emit_scale_heartbeat() {
    // ONE LINE A MINUTE, ARMED OR NOT. This is what turns RUN 1 from "one
    // snapshot of the scale" into "did the scale MOVE when the slider moved",
    // at zero extra run cost - the owner can change the FOV slider mid-session
    // and the next line records it.
    using clock = std::chrono::steady_clock;
    static clock::time_point last{};
    const auto now = clock::now();
    if (last.time_since_epoch().count() != 0 && now - last < std::chrono::seconds(60)) return;
    last = now;

    VpSnapshot vp;
    const bool have = world_vp(vp);
    const bool had_range = g_row1_have.exchange(false, std::memory_order_relaxed);
    const float lo = g_row1_min.load(std::memory_order_relaxed);
    const float hi = g_row1_max.load(std::memory_order_relaxed);

    const SamplerCounts sc = sampler_counts();
    if (!have) {
        log::error(std::format(
            "WraparoundScale HEARTBEAT: NO SAMPLE. hookCalls={} attempts={} readFailures={} "
            "widePass={} strictPass={} censusLines={} samplerCompiledIn={}. Every FOV number "
            "this build prints is the legacy constant {:.4f} wearing a live label. WHICH STAGE "
            "FAILED is the whole question and these five numbers separate it: hookCalls=0 means "
            "the hook is dead; readFailures==attempts means +0xC44 is wrong; widePass=0 with "
            "successful reads means the matrices exist and were rejected - and in that case the "
            "VpCensus lines above hold the actual matrices.",
            sc.hook_calls, sc.attempts, sc.read_failures, sc.wide_pass, sc.strict_pass,
            sc.census_lines, sampler_compiled_in(), kLegacyProjectionScale));
        return;
    }

    bool live = false;
    const float active = frame_scale(&live);
    const float sx = scale_candidate(vp.m, ScaleSource::Row0OverRow3, 0.0f);
    const float sy = scale_candidate(vp.m, ScaleSource::Row1OverRow3, 0.0f);
    // Invert the relation measured in RUN 1b so the line reads back DOOM's own
    // slider number. If it stops matching the slider, the relation changed and
    // this is the line that says so - rather than a wrong FOV reaching the
    // headset with nothing to compare it against.
    const float aspect = g_frame_aspect.load(std::memory_order_relaxed);
    float implied_slider = 0.0f;
    if (sx > 0.0f && aspect > 0.0f) {
        const float ref = sx * aspect / (16.0f / 9.0f);   // back to the 16:9 reference
        if (ref > 0.0f) implied_slider = to_degrees(2.0f * std::atan(1.0f / ref));
    }
    log::warn(std::format(
        "WraparoundScale HEARTBEAT: active={:.4f} live={} via '{}' (channel={}) -> "
        "contentFovY={:.2f} deg | strictSamples={} wideSamples={} | row1len now={:.4f} "
        "range this window {} | NORMALISED sx=row0/row3={:.4f} sy=row1/row3={:.4f} -> "
        "{:.2f} x {:.2f} deg HxV, impliedSlider={:.1f} | rows=({:.4f} {:.4f} {:.4f} {:.4f}) "
        "l1/l0={:.4f} vs frameAspect={:.4f}\n"
        "READING IT: sx and sy are the projection terms NORMALISED by |row3|; the raw row "
        "lengths carry an arbitrary per-view scale and mean nothing alone. impliedSlider "
        "inverts the measured relation sx = (1/tan(slider/2)) * (16/9)/frameAspect - if it "
        "does not read back the number on DOOM's FOV slider, that relation has changed and "
        "nothing downstream of it should be trusted. strictSamples stalling while wideSamples "
        "climbs is EXPECTED above ~FOV 100 and is not a fault.",
        active, live, scale_source_name(config().scale_source),
        vp.from_strict ? "strict" : "wide",
        to_degrees(fov_from_scale(active)),
        vp.strict_samples, vp.wide_samples, vp.row_len[1],
        had_range ? std::format("{:.4f}..{:.4f} (span {:.4f})", lo, hi, hi - lo)
                  : std::string("none - no publish since the last line"),
        sx, sy,
        sx > 0.0f ? to_degrees(fov_from_scale(sx)) : 0.0f,
        sy > 0.0f ? to_degrees(fov_from_scale(sy)) : 0.0f,
        implied_slider,
        vp.row_len[0], vp.row_len[1], vp.row_len[2], vp.row_len[3],
        vp.row_len[0] > 0.0f ? vp.row_len[1] / vp.row_len[0] : 0.0f,
        g_frame_aspect.load(std::memory_order_relaxed)));
}

void maybe_emit_engagement(bool look_injection_on, bool xr_live) {
    if (!config().armed) return;
    using clock = std::chrono::steady_clock;
    static clock::time_point last{};
    const auto now = clock::now();
    if (last.time_since_epoch().count() != 0 &&
        now - last < std::chrono::seconds(1)) return;
    last = now;

    Framing f0, f1;
    const bool have0 = last_framing(0, f0);
    const bool have1 = last_framing(1, f1);
    if (!have0 && !have1) {
        log::error(std::format(
            "Wraparound ARMED but NEITHER EYE has produced a framing (xrLive={}). The switch "
            "file is on and the new path is not running - engagement is zero, and a zero "
            "counter is never permission to read the rest of this run as a result.", xr_live));
        return;
    }
    // ===== PER-EYE REPORT (2026-08-29) =====================================
    //
    // This line used to pick ONE eye - `have0 ? f0 : f1` - and print it, with
    // bothEyes= saying only that each had produced *a* framing, never that the
    // two agreed. 44 of 44 lines in the 17:09 run reported eye 0 and not one
    // reported eye 1. Under true stereo the doubled eye is framed from
    // override_source_extent, a DIFFERENT image from DOOM's frame, and
    // openxr_context.cpp:1147 makes that extent the FOV basis for the path -
    // so the surface most likely to be wrong was the one surface never
    // measured. Both eyes are now printed and explicitly compared.
    if (have0 && have1) {
        const bool src_same = f0.src_w == f1.src_w && f0.src_h == f1.src_h;
        const bool crop_same = f0.crop_w == f1.crop_w && f0.crop_h == f1.crop_h &&
                               f0.crop_x == f1.crop_x && f0.crop_y == f1.crop_y;
        const bool dst_same = f0.dst_w == f1.dst_w && f0.dst_h == f1.dst_h;
        const bool fov_same =
            std::fabs(f0.cropped_fov_x - f1.cropped_fov_x) <= 1e-4f &&
            std::fabs(f0.cropped_fov_y - f1.cropped_fov_y) <= 1e-4f;
        log::warn(std::format(
            "WraparoundEyes  eye0 src={}x{} crop={}x{}@({},{}) dst={}x{} "
            "target={}x{} contentFov={:.2f}x{:.2f} croppedFov={:.2f}x{:.2f}\n"
            "                eye1 src={}x{} crop={}x{}@({},{}) dst={}x{} "
            "target={}x{} contentFov={:.2f}x{:.2f} croppedFov={:.2f}x{:.2f}\n"
            "                srcMatch={} cropMatch={} dstMatch={} fovMatch={}\n"
            "READING IT: the two eyes are framed from DIFFERENT source images "
            "under true stereo - eye 0 from DOOM's frame, the doubled eye from "
            "the mirror. srcMatch=NO means the mirror is not the same size as "
            "DOOM's frame, and every downstream number for that eye describes a "
            "different rectangle: that is a BUILD DEFECT and the run is void "
            "for stereo judgement. All four MATCH=YES is the healthy state. "
            "This line existing at all is new - before 2026-08-29 eye 1's "
            "framing was never printed, so its absence was not evidence.",
            f0.src_w, f0.src_h, f0.crop_w, f0.crop_h, f0.crop_x, f0.crop_y,
            f0.dst_w, f0.dst_h, f0.target_w, f0.target_h,
            to_degrees(f0.content_fov_x), to_degrees(f0.content_fov_y),
            to_degrees(f0.cropped_fov_x), to_degrees(f0.cropped_fov_y),
            f1.src_w, f1.src_h, f1.crop_w, f1.crop_h, f1.crop_x, f1.crop_y,
            f1.dst_w, f1.dst_h, f1.target_w, f1.target_h,
            to_degrees(f1.content_fov_x), to_degrees(f1.content_fov_y),
            to_degrees(f1.cropped_fov_x), to_degrees(f1.cropped_fov_y),
            src_same ? "YES" : "***NO***", crop_same ? "YES" : "***NO***",
            dst_same ? "YES" : "***NO***", fov_same ? "YES" : "***NO***"));
    } else {
        log::error(std::format(
            "WraparoundEyes: ONLY EYE {} HAS A FRAMING (have0={} have1={}). The "
            "other eye is being submitted without going through compute_framing, "
            "so its blit rectangle and its declared field of view were never "
            "derived together. Stereo judgement from this run is void.",
            have0 ? 0 : 1, have0, have1));
    }

    const Framing& f = have0 ? f0 : f1;
    const uint32_t eye = have0 ? 0u : 1u;
    const bool submitted = g_submitted_have[eye].load(std::memory_order_relaxed);
    const float du = g_submitted[eye][2].load(std::memory_order_relaxed);
    const float dd = g_submitted[eye][3].load(std::memory_order_relaxed);
    const float declared_y = submitted ? (du - dd) : 0.0f;
    const bool match = submitted && std::fabs(declared_y - f.cropped_fov_y) <= 1e-4f;

    bool live = false;
    const float scale = frame_scale(&live);
    log::warn(std::format(
        "WraparoundEngage headYaw={:.1f} headPitch={:.1f} headRoll={:.1f} deg | "
        "headRotApplied={} headRotRefused={} axes={}{}{} | "
        "eye={} declaredFovY={:.4f} contentFovY={:.4f} match={} "
        "poseSource={} lookInjection={} | scale={:.4f} scaleLive={} crop={}x{}@({},{}) "
        "dst={}x{} scaleFactor={:.3f}x{:.3f} "
        "declaredFovX={:.4f} contentFovX={:.4f} bothEyes={} | {}",
        to_degrees(g_head_yaw.load(std::memory_order_relaxed)),
        to_degrees(g_head_pitch.load(std::memory_order_relaxed)),
        to_degrees(g_head_roll.load(std::memory_order_relaxed)),
        head_rotations_applied(), head_rotations_refused(),
        config().head_axes.flip_yaw ? '-' : '+',
        config().head_axes.flip_pitch ? '-' : '+',
        config().head_axes.flip_roll ? '-' : '+',
        eye, to_degrees(declared_y), to_degrees(f.cropped_fov_y), match ? "YES" : "NO",
        config().pose_shared ? "shared" : "located",
        look_injection_on ? "on" : "off", scale, live,
        f.crop_w, f.crop_h, f.crop_x, f.crop_y,
        f.dst_w, f.dst_h,
        f.crop_w > 0 ? static_cast<float>(f.dst_w) / static_cast<float>(f.crop_w) : 0.0f,
        f.crop_h > 0 ? static_cast<float>(f.dst_h) / static_cast<float>(f.crop_h) : 0.0f,
        to_degrees(g_submitted[eye][1].load(std::memory_order_relaxed) -
                   g_submitted[eye][0].load(std::memory_order_relaxed)),
        to_degrees(f.cropped_fov_x), have0 && have1,
        match ? "declaration and content agree"
              : "***match=NO IS A BUILD DEFECT*** - the compositor is being told a field of "
                "view the blitted pixels do not subtend. Any judgement of magnification or "
                "world-lock from this run is void."));
}

}  // namespace kharvoxnative::wrap

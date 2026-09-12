// Offline unit test for include/doomvr/wraparound_math.h.
//
// Compiles the math header ON ITS OWN - no Windows, no Vulkan, no OpenXR - so
// every property below is checked in seconds on the build machine instead of
// in a five-minute headset run. This exists because three runs were lost to
// build defects that offline checks would have caught
// (docs/HANDOFF-2026-08-25-IMPLEMENTATION-AGENT-EXIT.md section 6).
//
// Build:
//   cl /nologo /std:c++20 /EHsc /I include tests\wraparound_math_test.cpp
// Run: exit code 0 = all pass. Any failure prints the case and returns 1.
//
// The SAME assertions run inside the DLL at startup (wrap::run_math_self_test),
// so a build that somehow ships with different math announces itself in the
// log within the first second rather than in the headset.

#include "kharvoxnative/wraparound_math.h"

#include <cstdio>
#include <cmath>
#include <array>
#include <limits>
#include <string>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

void close_to(float a, float b, float tol, const std::string& what) {
    ++g_checks;
    if (!(std::fabs(a - b) <= tol)) {
        ++g_failures;
        std::printf("FAIL: %s (got %.9f expected %.9f tol %.9f)\n",
                    what.c_str(), a, b, tol);
    }
}

}  // namespace

int main() {
    using namespace kharvoxnative::wrap;

    // ---------------------------------------------------------------------
    // 1. The legacy formula is preserved exactly.
    //    openxr_context.cpp used 2*atan(1/1.639) = 62.79 deg vertical. If this
    //    drifts, "wraparound absent = today" is no longer true.
    // ---------------------------------------------------------------------
    close_to(to_degrees(fov_from_scale(kLegacyProjectionScale)), 62.7771f, 0.01f,
             "legacy scale 1.639 -> 62.79 deg vertical");
    close_to(scale_from_fov(fov_from_scale(2.5f)), 2.5f, 1e-4f,
             "scale_from_fov inverts fov_from_scale");

    // ---------------------------------------------------------------------
    // 2. IDENTITY CASE. Source aspect == dest aspect must produce NO crop and
    //    a declared FOV identical to the content FOV. If this ever fails, the
    //    crop path is injecting a change where it must be a no-op.
    // ---------------------------------------------------------------------
    {
        const Framing f = compute_framing(1920, 1080, 1280, 720, 1.639f);
        check(f.valid, "identity: valid");
        check(f.crop_w == 1920 && f.crop_h == 1080, "identity: no crop");
        check(f.crop_x == 0 && f.crop_y == 0, "identity: crop at origin");
        close_to(f.cropped_fov_x, f.content_fov_x, 1e-5f, "identity: fov_x unchanged");
        close_to(f.cropped_fov_y, f.content_fov_y, 1e-5f, "identity: fov_y unchanged");
    }

    // ---------------------------------------------------------------------
    // 3. THE REAL CASE. DOOM 3818x2032 (1.879:1) into a roughly square eye.
    //    2064x2272 is a representative Quest-class recommendedImageRect; the
    //    live values come from the Phase 0 line, and the properties asserted
    //    here hold for any pair.
    // ---------------------------------------------------------------------
    {
        const Framing f = compute_framing(3818, 2032, 2064, 2272, 1.639f);
        check(f.valid, "real: valid");
        check(f.crop_h == 2032, "real: source is wider, so height is kept whole");
        check(f.crop_w < 3818, "real: width is cropped");
        check(f.crop_w > 0 && f.crop_x >= 0 && f.crop_x + f.crop_w <= 3818,
              "real: crop stays inside the source");
        check(f.crop_x == (3818 - f.crop_w) / 2, "real: crop is centred");
        close_to(f.cropped_fov_y, f.content_fov_y, 1e-5f,
                 "real: vertical FOV survives a horizontal-only crop");
        check(f.cropped_fov_x < f.content_fov_x, "real: horizontal FOV is reduced");
    }

    // ---------------------------------------------------------------------
    // 4. THE PROPERTY THE WHOLE PHASE TURNS ON: after the crop, the declared
    //    angles must have the DEST rect's aspect in tangent space. That is
    //    exactly "square pixels, no stretch, no magnification change". Checked
    //    across a sweep so it is a property, not one lucky pair.
    // ---------------------------------------------------------------------
    {
        const int32_t sources[][2] = {{3818, 2032}, {1920, 1080}, {2560, 1440},
                                      {1024, 1024}, {1280, 1600}, {800, 3000}};
        const int32_t dests[][2] = {{2064, 2272}, {1832, 1920}, {1440, 1584},
                                    {1024, 1024}, {2000, 1000}, {512, 2048}};
        for (const auto& s : sources) {
            for (const auto& d : dests) {
                for (float scale : {0.5f, 1.0f, 1.639f, 3.079f, 6.0f}) {
                    const Framing f = compute_framing(s[0], s[1], d[0], d[1], scale);
                    check(f.valid, "sweep: valid");
                    const float ratio = std::tan(f.cropped_fov_x * 0.5f) /
                                        std::tan(f.cropped_fov_y * 0.5f);
                    const float dst_aspect =
                        static_cast<float>(d[0]) / static_cast<float>(d[1]);
                    // 0.6% tolerance: crop_w/crop_h are integers, so the crop
                    // aspect can only match the dest aspect to within one
                    // pixel of rounding. Tightening this below the rounding
                    // floor would make the test fail on correct code.
                    close_to(ratio / dst_aspect, 1.0f, 0.006f,
                             "sweep: declared angles carry the DEST aspect");
                    // A crop can only ever REMOVE field of view.
                    check(f.cropped_fov_x <= f.content_fov_x + 1e-5f,
                          "sweep: crop never widens fov_x");
                    check(f.cropped_fov_y <= f.content_fov_y + 1e-5f,
                          "sweep: crop never widens fov_y");
                    // Declaration is read back from the SUBMITTED angles, the
                    // way the engagement line reads it, not from the internal
                    // field - so this is not a value compared against itself.
                    close_to(f.angle_up - f.angle_down, f.cropped_fov_y, 1e-6f,
                             "sweep: submitted angles equal the crop FOV (y)");
                    close_to(f.angle_right - f.angle_left, f.cropped_fov_x, 1e-6f,
                             "sweep: submitted angles equal the crop FOV (x)");
                    check(f.angle_left < 0.0f && f.angle_right > 0.0f &&
                          f.angle_up > 0.0f && f.angle_down < 0.0f,
                          "sweep: OpenXR angle sign convention");
                    check(f.crop_x >= 0 && f.crop_y >= 0 &&
                          f.crop_x + f.crop_w <= s[0] &&
                          f.crop_y + f.crop_h <= s[1],
                          "sweep: crop inside source");
                }
            }
        }
    }

    // ---------------------------------------------------------------------
    // 5. Degenerate inputs return invalid rather than NaN angles or a crop
    //    outside the image. A zero extent happens for real: DOOM's swapchain
    //    is empty for the first frames.
    // ---------------------------------------------------------------------
    check(!compute_framing(0, 1080, 100, 100, 1.639f).valid, "degenerate: zero src w");
    check(!compute_framing(1920, 0, 100, 100, 1.639f).valid, "degenerate: zero src h");
    check(!compute_framing(1920, 1080, 0, 100, 1.639f).valid, "degenerate: zero dst w");
    check(!compute_framing(1920, 1080, 100, 0, 1.639f).valid, "degenerate: zero dst h");
    check(!compute_framing(1920, 1080, 100, 100, 0.0f).valid, "degenerate: zero scale");
    check(!compute_framing(1920, 1080, 100, 100, -1.0f).valid, "degenerate: negative scale");
    check(!compute_framing(1920, 1080, 100, 100,
                           std::numeric_limits<float>::quiet_NaN()).valid,
          "degenerate: NaN scale");

    // ---------------------------------------------------------------------
    // 6. POSE. Round-trip yaw/pitch through the quaternion builder using the
    //    SAME extraction update_look_injection uses. This is what stops the
    //    submitted pose being in a different convention from the content.
    // ---------------------------------------------------------------------
    {
        for (int yi = -17; yi <= 17; ++yi) {
            for (int pi = -8; pi <= 8; ++pi) {
                const float yaw = static_cast<float>(yi) * 0.18f;    // ~ +-3.06 rad
                const float pitch = static_cast<float>(pi) * 0.17f;  // ~ +-1.36 rad
                const Quat q = quat_from_yaw_pitch(yaw, pitch);
                float ry = 0.0f, rp = 0.0f;
                check(yaw_pitch_from_quat(q, ry, rp), "pose: extraction finite");
                // atan2 wraps at +-pi; compare the wrapped difference.
                float dy = ry - yaw;
                while (dy > 3.14159265f) dy -= 6.28318531f;
                while (dy < -3.14159265f) dy += 6.28318531f;
                close_to(dy, 0.0f, 2e-4f, "pose: yaw round-trips");
                close_to(rp, pitch, 2e-4f, "pose: pitch round-trips");
                // Unit quaternion, or the compositor rejects it.
                close_to(std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w), 1.0f,
                         1e-5f, "pose: unit quaternion");
            }
        }
    }

    // ---------------------------------------------------------------------
    // 7. strip_roll: preserves yaw/pitch, is idempotent, and is a no-op on a
    //    pose that already has no roll. Built by composing a real roll about
    //    the forward axis onto a yaw/pitch pose - not by hand-writing a
    //    quaternion and hoping it contains roll.
    // ---------------------------------------------------------------------
    {
        auto mul = [](const Quat& a, const Quat& b) {
            return Quat{a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
                        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
                        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
                        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z};
        };
        for (int yi = -5; yi <= 5; ++yi) {
            for (int pi = -3; pi <= 3; ++pi) {
                for (int ri = -4; ri <= 4; ++ri) {
                    const float yaw = static_cast<float>(yi) * 0.5f;
                    const float pitch = static_cast<float>(pi) * 0.35f;
                    const float roll = static_cast<float>(ri) * 0.25f;
                    const Quat base = quat_from_yaw_pitch(yaw, pitch);
                    // Roll is about the view's own -Z, so post-multiply.
                    const Quat rollq{0.0f, 0.0f, std::sin(roll * 0.5f),
                                     std::cos(roll * 0.5f)};
                    const Quat rolled = mul(base, rollq);
                    const Quat stripped = strip_roll(rolled);
                    // Rolling about the forward axis must not move forward.
                    float bx, by, bz, sx, sy, sz;
                    forward_from_quat(base, bx, by, bz);
                    forward_from_quat(stripped, sx, sy, sz);
                    close_to(sx, bx, 3e-4f, "roll: forward x preserved");
                    close_to(sy, by, 3e-4f, "roll: forward y preserved");
                    close_to(sz, bz, 3e-4f, "roll: forward z preserved");
                    // Idempotent: stripping twice equals stripping once.
                    const Quat twice = strip_roll(stripped);
                    close_to(twice.x, stripped.x, 1e-5f, "roll: idempotent x");
                    close_to(twice.y, stripped.y, 1e-5f, "roll: idempotent y");
                    close_to(twice.z, stripped.z, 1e-5f, "roll: idempotent z");
                    close_to(twice.w, stripped.w, 1e-5f, "roll: idempotent w");
                }
            }
        }
    }

    // ---------------------------------------------------------------------
    // 8. Scale-candidate extraction and the plausibility band. A live read
    //    outside the band must be REJECTED, so the fallback announces itself
    //    instead of a wild FOV reaching the headset silently.
    // ---------------------------------------------------------------------
    {
        // Row 1 has length 5 (3,4,0); m[1] = 1.639; m[6] = 4; m[5] = 4... build
        // a matrix whose four candidates are all distinct so a wiring mistake
        // between them cannot pass.
        float m[16] = {
            1.0f, 1.639f, 0.0f, 0.0f,
            0.0f, 3.0f,   4.0f, 0.0f,
            0.0f, 0.0f,   1.0f, 0.0f,
            0.0f, 0.0f,   0.0f, 1.0f};
        close_to(scale_candidate(m, ScaleSource::Row1Len, 0.0f), 5.0f, 1e-5f,
                 "candidates: row1len");
        close_to(scale_candidate(m, ScaleSource::Elem1, 0.0f), 1.639f, 1e-5f,
                 "candidates: elem1");
        close_to(scale_candidate(m, ScaleSource::Elem6, 0.0f), 4.0f, 1e-5f,
                 "candidates: elem6");
        close_to(scale_candidate(m, ScaleSource::Elem5, 0.0f), 3.0f, 1e-5f,
                 "candidates: elem5");
        close_to(scale_candidate(m, ScaleSource::Fixed, 1.639f), 1.639f, 1e-5f,
                 "candidates: fixed ignores the matrix");
        // Sign must not matter - a mirrored basis still has a positive scale.
        m[1] = -1.639f;
        close_to(scale_candidate(m, ScaleSource::Elem1, 0.0f), 1.639f, 1e-5f,
                 "candidates: elem1 is magnitude");

        check(scale_plausible(1.639f), "band: 1.639 accepted");
        check(scale_plausible(kScaleMin), "band: lower edge accepted");
        check(scale_plausible(kScaleMax), "band: upper edge accepted");
        check(!scale_plausible(0.0f), "band: zero rejected");
        check(!scale_plausible(-1.639f), "band: negative rejected");
        check(!scale_plausible(1e9f), "band: absurd rejected");
        check(!scale_plausible(std::numeric_limits<float>::quiet_NaN()),
              "band: NaN rejected");
        check(!scale_plausible(std::numeric_limits<float>::infinity()),
              "band: infinity rejected");
    }

    // ---------------------------------------------------------------------
    // 8b. THE GATE THAT NEARLY COST RUN 1.
    //
    // looks_like_world_camera_matrix bounds row 1's length to 1.65..1.90.
    // RUN 1 puts DOOM's FOV slider at MAXIMUM. If row 1's length is the FOV
    // term it can leave that band, and a sampler gated on the strict predicate
    // would go silent and report zero - which reads as a dead instrument, not
    // as a finding. wide_world_vp_predicate must still accept those matrices.
    // ---------------------------------------------------------------------
    {
        // A plausible world view-projection: rows 0, 2, 3 unit-length, row 1
        // scaled by whatever the FOV term is. Built by scaling row 1 of an
        // identity basis, so only the one term varies.
        auto make = [](float row1_scale) {
            std::array<float, 16> m{1.0f, 0.0f, 0.0f, 1234.5f,
                                    0.0f, 1.0f, 0.0f, -67.25f,
                                    0.0f, 0.0f, 1.0f, 42.0f,
                                    0.0f, 1.0f, 0.0f, 1.0f};
            m[4] *= row1_scale; m[5] *= row1_scale; m[6] *= row1_scale;
            return m;
        };
        for (float s : {0.10f, 0.45f, 0.90f, 1.20f, 1.639f, 1.75f, 1.879f,
                        2.60f, 5.00f, 7.90f, 20.0f, 49.0f}) {
            const auto m = make(s);
            check(wide_world_vp_predicate(m.data()),
                  "wide predicate accepts row1len " + std::to_string(s));
            close_to(scale_candidate(m.data(), ScaleSource::Row1Len, 0.0f), s, 1e-4f,
                     "wide predicate case reports the row1 length back");
        }

        // THE RUN 1 REGRESSION CASE, from measurement, not from theory.
        //
        // Archived ViewOriginScan lines put row 0's length at ~0.946 at FOV 90.
        // Moving the slider 90 -> 130 scales the projection terms by
        // tan(45)/tan(65) = 0.466, taking row 0 to ~0.441. My first wide
        // predicate had an l0 > 0.85 floor inherited from the FOV-90 build, so
        // it rejected every matrix and RUN 1 came back with zero samples. This
        // case fails on that predicate and passes on the corrected one.
        {
            auto m = make(1.879f * 0.466f);   // row 1 at FOV 130
            m[0] = 0.441f; m[1] = 0.0f; m[2] = 0.0f;   // row 0 at FOV 130
            check(wide_world_vp_predicate(m.data()),
                  "RUN 1 case: FOV-130 matrix must NOT be rejected");
        }
        // A whole matrix scaled by the same factor - the case where every
        // projection row moves together - must also survive.
        for (float k : {0.3f, 0.466f, 1.0f, 2.15f, 4.0f}) {
            auto m = make(1.879f);
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) m[r * 4 + c] *= k;
            check(wide_world_vp_predicate(m.data()),
                  "uniformly scaled matrix survives k=" + std::to_string(k));
        }

        // It still rejects what cannot be a view matrix at ANY FOV: a
        // collapsed row carries no direction, and NaN is not a measurement.
        {
            auto m = make(1.639f);
            m[0] = 0.0f; m[1] = 0.0f; m[2] = 0.0f;      // row 0 collapsed
            check(!wide_world_vp_predicate(m.data()), "wide predicate rejects collapsed row0");
        }
        {
            auto m = make(1.639f);
            m[8] = 400.0f;                               // row 2 absurd
            check(!wide_world_vp_predicate(m.data()), "wide predicate rejects absurd row2");
        }
        {
            auto m = make(1.639f);
            m[13] = std::numeric_limits<float>::quiet_NaN();
            check(!wide_world_vp_predicate(m.data()), "wide predicate rejects NaN");
        }
        {
            std::array<float, 16> m{};                   // all zero
            check(!wide_world_vp_predicate(m.data()), "wide predicate rejects an all-zero matrix");
        }
    }

    // ---------------------------------------------------------------------
    // 8c. RUN 1b's MEASURED VALUES, as regression tests.
    //
    // Session 2026-08-25 22:53. Every gameplay world view, at eight different
    // overall matrix scales and two FOV slider settings. These are numbers off
    // the wire, not derived - if a future change breaks the normalisation or
    // the world-view test, these fail.
    // ---------------------------------------------------------------------
    {
        const float kFrameAspect = 3818.0f / 2032.0f;   // 1.87894

        // Build a matrix with given row lengths, arbitrary directions.
        auto build = [](float l0, float l1, float l2, float l3) {
            std::array<float, 16> m{l0, 0.0f, 0.0f, -91.77f,
                                    0.0f, 0.0f, l1,  838.02f,
                                    0.0f, l2,  0.0f, -243.08f,
                                    0.0f, l3,  0.0f, -240.00f};
            return m;
        };

        // FOV 130, the eight (l0,l1,l3) triples logged as VpCensus world views.
        const float f130[][3] = {
            {0.4412f, 0.8290f, 1.0000f}, {0.5294f, 0.9948f, 1.2000f},
            {0.6662f, 1.2517f, 1.5099f}, {0.6579f, 1.2362f, 1.4912f},
            {0.3188f, 0.5991f, 0.7226f}, {0.3479f, 0.6537f, 0.7886f},
            {0.5736f, 1.0777f, 1.3000f}, {0.3821f, 0.7180f, 0.8661f}};
        for (const auto& r : f130) {
            const auto m = build(r[0], r[1], 1.0f, r[2]);
            check(is_world_view(m.data(), kFrameAspect),
                  "RUN 1b: FOV-130 world view is recognised");
            // Normalisation must collapse all eight to the same pair.
            close_to(scale_candidate(m.data(), ScaleSource::Row0OverRow3, 0.0f),
                     0.4412f, 5e-4f, "RUN 1b: sx normalises to 0.4412 at FOV 130");
            close_to(scale_candidate(m.data(), ScaleSource::Row1OverRow3, 0.0f),
                     0.8290f, 5e-4f, "RUN 1b: sy normalises to 0.8290 at FOV 130");
        }
        // The raw row length does NOT collapse - that is why it was wrong.
        check(std::fabs(1.2517f - 0.8290f) > 0.1f,
              "RUN 1b: raw |row1| varies across views, so it cannot be the term");

        // FOV 90, from the heartbeat: rows=(0.9462 1.7778 1.0005 1.0000).
        {
            const auto m = build(0.9462f, 1.7778f, 1.0005f, 1.0000f);
            check(is_world_view(m.data(), kFrameAspect),
                  "RUN 1b: FOV-90 world view is recognised");
            close_to(scale_candidate(m.data(), ScaleSource::Row0OverRow3, 0.0f),
                     0.9462f, 5e-4f, "RUN 1b: sx = 0.9462 at FOV 90");
            close_to(to_degrees(fov_from_scale(0.9462f)), 93.17f, 0.05f,
                     "RUN 1b: FOV 90 slider -> 93.2 deg horizontal at this frame aspect");
            close_to(to_degrees(fov_from_scale(0.8290f)), 100.68f, 0.05f,
                     "RUN 1b: FOV 130 slider -> 100.7 deg vertical");
            close_to(to_degrees(fov_from_scale(0.4412f)), 132.39f, 0.05f,
                     "RUN 1b: FOV 130 slider -> 132.4 deg horizontal");
        }

        // The slider relation, confirmed at both settings to four decimals:
        //   sx = (1/tan(slider/2)) * (16/9) / frameAspect
        auto predict_sx = [&](float slider_deg) {
            const float half = slider_deg * 0.5f * (3.14159265358979323846f / 180.0f);
            return (1.0f / std::tan(half)) * (16.0f / 9.0f) / kFrameAspect;
        };
        close_to(predict_sx(90.0f), 0.9462f, 5e-4f, "RUN 1b: slider 90 predicts sx 0.9462");
        close_to(predict_sx(130.0f), 0.4412f, 5e-4f, "RUN 1b: slider 130 predicts sx 0.4412");

        // Non-world views seen in the same session must be rejected. These are
        // the actual l1/l0 ratios logged alongside the world views.
        for (float bad_ratio : {1.6967f, 2.2261f, 2.2544f, 2.2645f, 2.2753f, 2.2900f,
                                2.3390f, 2.4432f, 2.4528f, 2.5213f, 2.5712f, 2.5740f,
                                2.7500f, 2.7585f, 3.4590f}) {
            const auto m = build(0.5f, 0.5f * bad_ratio, 1.0f, 1.0f);
            check(!is_world_view(m.data(), kFrameAspect),
                  "RUN 1b: non-world view l1/l0=" + std::to_string(bad_ratio) + " rejected");
        }
        // And the world ratio is accepted at every overall scale.
        for (float k : {0.3f, 0.7226f, 1.0f, 1.2f, 1.5099f, 4.0f}) {
            const auto m = build(0.4412f * k, 0.8290f * k, k, k);
            check(is_world_view(m.data(), kFrameAspect),
                  "RUN 1b: world ratio survives overall scale " + std::to_string(k));
        }
        // Degenerate row 3 must not divide by zero into a bogus scale.
        {
            const auto m = build(0.4412f, 0.8290f, 1.0f, 0.0f);
            close_to(scale_candidate(m.data(), ScaleSource::Row1OverRow3, 0.0f), 0.0f,
                     1e-6f, "RUN 1b: zero row3 yields 0, not infinity");
            check(!scale_plausible(scale_candidate(m.data(), ScaleSource::Row1OverRow3, 0.0f)),
                  "RUN 1b: zero row3 is rejected as implausible, so the fallback fires");
        }
    }

    // ---------------------------------------------------------------------
    // 8d. THE FOV-MATCHED CROP, against RUN 1b's measured headset.
    //     recommendedImageRect 4608x4224, located FOV 110x90 deg, DOOM
    //     3818x2032 at slider 130 (sy = 0.8290 -> 132.4 x 100.7 deg).
    // ---------------------------------------------------------------------
    {
        const float kDeg = 3.14159265358979323846f / 180.0f;
        const float hx = 110.0f * kDeg, hy = 90.0f * kDeg;
        const Framing f = compute_framing(3818, 2032, 4608, 4224, 0.8290f, hx, hy);
        check(f.valid, "fovcrop: valid");
        // The whole point: the declared angles come out AT the headset's FOV,
        // so the eye is filled rather than letterboxed.
        close_to(to_degrees(f.cropped_fov_x), 110.0f, 0.2f, "fovcrop: fills 110 deg wide");
        close_to(to_degrees(f.cropped_fov_y), 90.0f, 0.2f, "fovcrop: fills 90 deg tall");
        check(f.crop_w <= 3818 && f.crop_h <= 2032, "fovcrop: inside the source");
        check(f.crop_x == (3818 - f.crop_w) / 2 && f.crop_y == (2032 - f.crop_h) / 2,
              "fovcrop: centred");
        // Still tied by construction - the submitted angles equal the crop.
        close_to(f.angle_up - f.angle_down, f.cropped_fov_y, 1e-6f, "fovcrop: tied y");
        close_to(f.angle_right - f.angle_left, f.cropped_fov_x, 1e-6f, "fovcrop: tied x");

        // At slider 90 the content (93.2 x 58.7) is NARROWER than the headset
        // vertically, so the crop must clamp and simply declare less. We
        // cannot invent field of view, and pretending to would be the exact
        // declaration/content divergence this whole phase exists to prevent.
        const Framing n = compute_framing(3818, 2032, 4608, 4224, 1.7778f, hx, hy);
        check(n.valid, "fovcrop narrow: valid");
        check(n.crop_h == 2032, "fovcrop narrow: keeps the full height");
        close_to(to_degrees(n.cropped_fov_y), 58.72f, 0.2f,
                 "fovcrop narrow: declares the content's 58.7 deg, not the headset's 90");
        check(to_degrees(n.cropped_fov_x) <= 110.0f + 0.2f,
              "fovcrop narrow: never declares more than the headset");
        close_to(n.angle_up - n.angle_down, n.cropped_fov_y, 1e-6f, "fovcrop narrow: tied");

        // The pixel-aspect mode still works and is genuinely different here.
        const Framing p = compute_framing(3818, 2032, 4608, 4224, 0.8290f);
        check(p.valid, "pixelcrop: valid");
        check(p.crop_w != f.crop_w || p.crop_h != f.crop_h,
              "pixelcrop: differs from the FOV crop on this headset");
        close_to(std::tan(p.cropped_fov_x * 0.5f) / std::tan(p.cropped_fov_y * 0.5f),
                 4608.0f / 4224.0f, 0.006f, "pixelcrop: carries the buffer aspect");
        close_to(p.angle_up - p.angle_down, p.cropped_fov_y, 1e-6f, "pixelcrop: tied");

        // ------------------------------------------------------------------
        // DESTINATION RECT. The crop is 2406x1684; the swapchain is 4608x4224.
        // Upscaling into the whole swapchain would resample the same pixels
        // twice - once by our bilinear blit, once by the runtime's distortion.
        // The destination must therefore be the crop, 1:1, submitted as a
        // sub-rectangle.
        // ------------------------------------------------------------------
        check(f.dst_w == f.crop_w && f.dst_h == f.crop_h,
              "dstrect: crop smaller than the swapchain is written 1:1");
        check(f.dst_w <= 4608 && f.dst_h <= 4224, "dstrect: inside the swapchain");
        // A crop LARGER than the swapchain must clamp - we cannot write outside it.
        const Framing big = compute_framing(3818, 2032, 1024, 512, 0.8290f, hx, hy);
        check(big.dst_w <= 1024 && big.dst_h <= 512, "dstrect: clamps to the swapchain");
        check(big.dst_w > 0 && big.dst_h > 0, "dstrect: never degenerate");
        // Shrinking the destination must NOT touch the declared geometry:
        // sampling density is not field of view.
        const Framing small_dst = compute_framing(3818, 2032, 900, 700, 0.8290f, hx, hy);
        const Framing large_dst = compute_framing(3818, 2032, 9000, 7000, 0.8290f, hx, hy);
        close_to(small_dst.cropped_fov_x, large_dst.cropped_fov_x, 1e-5f,
                 "dstrect: declared fov_x is independent of the destination size");
        close_to(small_dst.cropped_fov_y, large_dst.cropped_fov_y, 1e-5f,
                 "dstrect: declared fov_y is independent of the destination size");
        check(small_dst.crop_w == large_dst.crop_w && small_dst.crop_h == large_dst.crop_h,
              "dstrect: the crop itself is independent of the destination size");
    }

    // The destination invariant must hold across the whole sweep, in both crop
    // modes: never larger than the crop, never larger than the swapchain,
    // never zero.
    {
        const float kDeg = 3.14159265358979323846f / 180.0f;
        const int32_t srcs[][2] = {{3818, 2032}, {2904, 2032}, {1024, 1024}, {800, 3000}};
        const int32_t dsts[][2] = {{4608, 4224}, {2064, 2272}, {512, 512}, {6000, 6000}};
        for (const auto& s : srcs) {
            for (const auto& d : dsts) {
                for (float sc : {0.4412f, 0.8290f, 1.7778f, 3.079f}) {
                    for (int mode = 0; mode < 2; ++mode) {
                        const Framing g = mode
                            ? compute_framing(s[0], s[1], d[0], d[1], sc,
                                              110.0f * kDeg, 90.0f * kDeg)
                            : compute_framing(s[0], s[1], d[0], d[1], sc);
                        check(g.valid, "dstsweep: valid");
                        check(g.dst_w > 0 && g.dst_h > 0, "dstsweep: non-degenerate");
                        check(g.dst_w <= g.crop_w && g.dst_h <= g.crop_h,
                              "dstsweep: destination never exceeds the crop");
                        check(g.dst_w <= d[0] && g.dst_h <= d[1],
                              "dstsweep: destination fits the swapchain");
                    }
                }
            }
        }
    }

    // ---------------------------------------------------------------------
    // 8e. PHASE 3 - apply_head_rotation.
    //
    // Built from a REAL measured world view (VpCensus #2, slider 130) rather
    // than a synthetic identity, so the invariants are asserted against the
    // matrix layout DOOM actually produces.
    // ---------------------------------------------------------------------
    {
        auto fresh = [] {
            return std::array<float, 16>{
                -0.3116f, -0.3123f, 0.0000f, -6135.4849f,   // right  * 0.4412
                -0.0018f,  0.0018f, 0.8290f, -2654.6396f,   // up     * 0.8290
                 0.7082f, -0.7065f, 0.0031f, 11405.2588f,   // depth  ~1.0004
                 0.7079f, -0.7063f, 0.0031f, 11404.0820f};  // w      ~1.0000
        };
        const HeadAxes id{};

        // Identity rotation is a no-op.
        {
            auto m = fresh();
            const auto before = m;
            check(apply_head_rotation(m.data(), 0.0f, 0.0f, 0.0f, id), "head: identity ok");
            for (int i = 0; i < 16; ++i)
                close_to(m[i], before[i], 2e-3f, "head: identity leaves the matrix alone");
        }

        // THE INVARIANT THAT MATTERS: row lengths - and therefore the FOV
        // terms - must not move. This is exactly what apply_yaw_rotation gets
        // wrong, so it is asserted across a wide sweep of angles.
        {
            const auto base = fresh();
            const float sx0 = row_length(base.data(), 0) / row_length(base.data(), 3);
            const float sy0 = row_length(base.data(), 1) / row_length(base.data(), 3);
            for (int yi = -6; yi <= 6; ++yi) {
                for (int pi = -4; pi <= 4; ++pi) {
                    for (int ri = -2; ri <= 2; ++ri) {
                        auto m = fresh();
                        const float y = static_cast<float>(yi) * 0.45f;
                        const float p = static_cast<float>(pi) * 0.30f;
                        const float r = static_cast<float>(ri) * 0.25f;
                        check(apply_head_rotation(m.data(), y, p, r, id), "head: sweep ok");
                        close_to(row_length(m.data(), 0), row_length(base.data(), 0), 1e-4f,
                                 "head: |row0| preserved");
                        close_to(row_length(m.data(), 1), row_length(base.data(), 1), 1e-4f,
                                 "head: |row1| preserved");
                        close_to(row_length(m.data(), 3), row_length(base.data(), 3), 1e-4f,
                                 "head: |row3| preserved");
                        // The projection terms themselves - what the FOV is made of.
                        close_to(row_length(m.data(), 0) / row_length(m.data(), 3), sx0,
                                 1e-4f, "head: sx invariant under rotation");
                        close_to(row_length(m.data(), 1) / row_length(m.data(), 3), sy0,
                                 1e-4f, "head: sy invariant under rotation");
                        // The basis must stay orthonormal - no drift.
                        auto dotxyz = [&](int ra, int rb) {
                            return (m[ra*4+0]*m[rb*4+0] + m[ra*4+1]*m[rb*4+1] +
                                    m[ra*4+2]*m[rb*4+2]) /
                                   (row_length(m.data(), ra) * row_length(m.data(), rb));
                        };
                        close_to(dotxyz(0, 1), 0.0f, 2e-3f, "head: right stays perp to up");
                        close_to(dotxyz(0, 3), 0.0f, 2e-3f, "head: right stays perp to fwd");
                        close_to(dotxyz(1, 3), 0.0f, 2e-3f, "head: up stays perp to fwd");
                        // Depth-vs-w difference survives untouched.
                        for (int i = 0; i < 4; ++i)
                            close_to(m[8+i] - m[12+i], base[8+i] - base[12+i], 2e-3f,
                                     "head: depth-vs-w delta preserved");
                    }
                }
            }
        }

        // ANCHOR: for a yaw-only rotation the DIRECTIONS must match
        // apply_yaw_rotation's live-proven coefficient pattern
        //     right' = c*right + s*fwd      fwd' = -s*right + c*fwd
        // Directions, not raw rows - that function does not preserve length,
        // which is the defect this one exists to avoid. Matching directions
        // pins the axis and sign convention to code already validated in the
        // headset; matching rows would import the bug.
        {
            for (int yi = -6; yi <= 6; ++yi) {
                const float y = static_cast<float>(yi) * 0.4f;
                auto m = fresh();
                const auto base = fresh();
                check(apply_head_rotation(m.data(), y, 0.0f, 0.0f, id), "anchor: ok");
                const float c = std::cos(y), s = std::sin(y);
                const float l0 = row_length(base.data(), 0), l3 = row_length(base.data(), 3);
                for (int i = 0; i < 3; ++i) {
                    const float u0 = base[0+i] / l0, u3 = base[12+i] / l3;
                    close_to(m[0+i] / l0, c * u0 + s * u3, 1e-4f,
                             "anchor: right matches apply_yaw_rotation's pattern");
                    close_to(m[12+i] / l3, -s * u0 + c * u3, 1e-4f,
                             "anchor: forward matches apply_yaw_rotation's pattern");
                }
                // Yaw must not move the up axis at all.
                for (int i = 0; i < 3; ++i)
                    close_to(m[4+i], base[4+i], 1e-4f, "anchor: yaw leaves up alone");
            }
        }

        // Sign flips are exact inverses, and flipping twice is identity.
        {
            auto a = fresh(), b = fresh();
            HeadAxes flip; flip.flip_yaw = true;
            apply_head_rotation(a.data(), 0.6f, 0.0f, 0.0f, id);
            apply_head_rotation(b.data(), -0.6f, 0.0f, 0.0f, flip);
            for (int i = 0; i < 16; ++i)
                close_to(a[i], b[i], 2e-3f, "head: flip_yaw negates the angle");
        }

        // Degenerate and non-finite inputs are refused, matrix untouched.
        {
            auto m = fresh();
            const auto before = m;
            m[0] = m[1] = m[2] = 0.0f;            // collapsed right row
            check(!apply_head_rotation(m.data(), 0.5f, 0.0f, 0.0f, id),
                  "head: collapsed row refused");
            auto n = fresh();
            const auto n_before = n;
            check(!apply_head_rotation(n.data(), std::numeric_limits<float>::quiet_NaN(),
                                       0.0f, 0.0f, id), "head: NaN angle refused");
            for (int i = 0; i < 16; ++i)
                close_to(n[i], n_before[i], 1e-6f, "head: refusal leaves the matrix alone");
            (void)before;
        }
        check(!apply_head_rotation(nullptr, 0.0f, 0.0f, 0.0f, id), "head: null refused");

        // ==================================================================
        // THE HEAD DELTA. A = VP_rot * inverse(VP_clean) must satisfy
        //     A * (VP_clean * M) == VP_rot * M
        // for ARBITRARY model transforms M. This is the whole justification
        // for writing A * MVP into GPU memory for objects whose model
        // transform we cannot see, so it is asserted against real rotations,
        // real translations and non-uniform scales - not just identity.
        // ==================================================================
        {
            const auto vp = fresh();          // real gameplay world VP
            for (int yi = -3; yi <= 3; ++yi) {
                for (int pi = -2; pi <= 2; ++pi) {
                    const float y = static_cast<float>(yi) * 0.5f;
                    const float p = static_cast<float>(pi) * 0.35f;
                    float A[16];
                    check(compute_head_delta(vp.data(), y, p, 0.0f, id, A),
                          "delta: computed");
                    // VP_rot, built the same way the delta does internally.
                    auto vp_rot = vp;
                    check(apply_head_rotation(vp_rot.data(), y, p, 0.0f, id),
                          "delta: vp_rot ok");
                    // Identity model transform: A*VP must equal VP_rot. This
                    // is the static-world case.
                    float aVP[16];
                    mat4_multiply(A, vp.data(), aVP);
                    for (int i = 0; i < 16; ++i)
                        close_to(aVP[i] / (std::fabs(vp_rot[i]) > 1.0f ? std::fabs(vp_rot[i]) : 1.0f),
                                 vp_rot[i] / (std::fabs(vp_rot[i]) > 1.0f ? std::fabs(vp_rot[i]) : 1.0f),
                                 2e-3f, "delta: A*VP == VP_rot");
                    // Arbitrary model transforms - the dynamic-object case.
                    const float models[][16] = {
                        {1,0,0,137.5f, 0,1,0,-42.25f, 0,0,1,900.0f, 0,0,0,1},
                        {0,-1,0,12.0f, 1,0,0,7.5f, 0,0,1,-3.0f, 0,0,0,1},
                        {2.5f,0,0,-500.0f, 0,0.5f,0,60.0f, 0,0,1.75f,20.0f, 0,0,0,1},
                        {0.8f,0.6f,0,10.0f, -0.6f,0.8f,0,-10.0f, 0,0,1,5.0f, 0,0,0,1},
                    };
                    for (const auto& M : models) {
                        float mvp[16], lhs[16], rhs[16];
                        mat4_multiply(vp.data(), M, mvp);       // MVP = VP * M
                        mat4_multiply(A, mvp, lhs);             // A * MVP
                        mat4_multiply(vp_rot.data(), M, rhs);   // VP_rot * M
                        for (int i = 0; i < 16; ++i) {
                            const float scale =
                                std::fabs(rhs[i]) > 1.0f ? std::fabs(rhs[i]) : 1.0f;
                            close_to(lhs[i] / scale, rhs[i] / scale, 3e-3f,
                                     "delta: A*MVP == VP_rot*M for an arbitrary model");
                        }
                    }
                }
            }
            // A zero head rotation must give the identity, or every frame with
            // the head still would drift the whole scene.
            {
                float A[16];
                check(compute_head_delta(vp.data(), 0.0f, 0.0f, 0.0f, id, A),
                      "delta: identity computed");
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 4; ++c)
                        close_to(A[r * 4 + c], r == c ? 1.0f : 0.0f, 3e-3f,
                                 "delta: zero rotation is the identity matrix");
            }
            // A singular matrix must be refused, not silently produce NaNs in
            // GPU memory.
            {
                float singular[16]{};
                float A[16];
                check(!compute_head_delta(singular, 0.3f, 0.0f, 0.0f, id, A),
                      "delta: singular VP refused");
            }
            // is_built_on_clean_vp must ACCEPT records built on our camera and
            // REJECT everything else. The loose "not affine last row" gate
            // accepted shadow views built on a light's camera and made the
            // scene unstable; this is the exact test that replaces it.
            {
                float inv[16];
                check(mat4_inverse(vp.data(), inv), "gate: VP invertible");
                const float models[][16] = {
                    {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1},
                    {1,0,0,137.5f, 0,1,0,-42.25f, 0,0,1,900.0f, 0,0,0,1},
                    {0,-1,0,12.0f, 1,0,0,7.5f, 0,0,1,-3.0f, 0,0,0,1},
                    {2.5f,0,0,-500.0f, 0,0.5f,0,60.0f, 0,0,1.75f,20.0f, 0,0,0,1},
                };
                for (const auto& M : models) {
                    float mvp[16];
                    mat4_multiply(vp.data(), M, mvp);
                    check(is_built_on_clean_vp(mvp, inv),
                          "gate: ACCEPTS a record built on our camera");
                }
                // ⚠ WHAT THIS GATE CANNOT DO, established by this test.
                //
                // A record built on a ROTATED version of our own camera passes.
                // That is not a bug in the test - it is a fact: inverse(VP) *
                // VP_rot == V^-1 * R * V, a rigid world-space transform, which
                // is indistinguishable from a model transform. Rotating the
                // camera and counter-rotating every model are the same
                // operation. Nothing keyed on this identity can separate them.
                //
                // It does not matter. Such records are our own doubled call's,
                // and re-basing them is near-harmless. The records that MUST be
                // rejected are ones built on a different PROJECTION - shadow
                // maps from a light, with their own FOV, position and near/far.
                // Those this test does reject, and those were what made the
                // scene unstable.
                // ⚠ AND IT CANNOT REJECT A DIFFERENT PROJECTION EITHER.
                //
                // I asserted it could. The test said otherwise, twice, and the
                // test is right: inverse(VP_clean) * VP_other is affine for the
                // whole family VP_other = VP_clean * (any affine), which
                // includes cameras that differ by scale and position. So this
                // gate proves "built on our camera OR on something affinely
                // related to it" - NOT "built on our camera".
                //
                // Recorded as a limitation rather than papered over, because
                // the difference is exactly what decides whether shadow passes
                // get corrupted. The gate is a partial filter; the thing that
                // actually fixes the instability is the once-per-slot-per-frame
                // rule in the bind hook, since A*R compounding to A^n is what
                // "completely unstable" was.
                {
                    auto other = vp;
                    other[0] *= 3.1f; other[1] *= 3.1f; other[2] *= 3.1f;
                    other[4] *= 0.4f; other[5] *= 0.4f; other[6] *= 0.4f;
                    float mvp[16];
                    mat4_multiply(other.data(), models[1], mvp);
                    const bool accepted = is_built_on_clean_vp(mvp, inv);
                    check(accepted || !accepted,   // documents, does not assert
                          "gate: an affinely-related camera is NOT separable");
                }
                // A HUD orthographic matrix must be rejected.
                {
                    const float ortho[16] = {0.001f,0,0,-1, 0,0.002f,0,-1,
                                             0,0,-1,0, 0,0,0,1};
                    check(!is_built_on_clean_vp(ortho, inv),
                          "gate: REJECTS a HUD orthographic matrix");
                }
                // Garbage must be rejected, not written into GPU memory.
                {
                    float junk[16];
                    for (int i = 0; i < 16; ++i) junk[i] = static_cast<float>(i) * 3.7f - 11.0f;
                    check(!is_built_on_clean_vp(junk, inv), "gate: REJECTS junk");
                }
            }

            // Round-trip the inverse itself.
            {
                float inv[16], prod[16];
                check(mat4_inverse(vp.data(), inv), "delta: VP is invertible");
                mat4_multiply(vp.data(), inv, prod);
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 4; ++c)
                        close_to(prod[r * 4 + c], r == c ? 1.0f : 0.0f, 2e-3f,
                                 "delta: VP * inverse(VP) == I");
            }
        }

        // ==================================================================
        // THE AFFINE LAYOUT - the defect that cost four builds.
        //
        // This is the GUI/viewmodel view EXACTLY as logged:
        //   rowLengths=(1.0000 1.0000 1.0000 0.0000)
        // Row 3 is the homogeneous (0,0,0,1), so forward is in ROW 2. The old
        // code demanded a non-degenerate row 3 and refused 3308 of 3308 calls,
        // silently, for four builds running.
        // ==================================================================
        {
            auto gui = [] {
                return std::array<float, 16>{
                    -0.0000f, 1.0000f, -0.0000f,  0.0000f,
                     0.0000f, 0.0000f,  1.0000f, -0.0000f,
                     1.0000f, -0.0000f, -0.0000f, -1.0000f,
                     0.0000f, 0.0000f,  0.0000f,  1.0000f};
            };
            auto m = gui();
            check(apply_head_rotation(m.data(), 0.4f, 0.0f, 0.0f, id),
                  "affine: the real GUI matrix is ACCEPTED, not refused");
            // Row 3 is structural and must come out untouched.
            close_to(m[12], 0.0f, 1e-6f, "affine: row3 x untouched");
            close_to(m[13], 0.0f, 1e-6f, "affine: row3 y untouched");
            close_to(m[14], 0.0f, 1e-6f, "affine: row3 z untouched");
            close_to(m[15], 1.0f, 1e-6f, "affine: row3 w stays 1");
            // The basis rows keep unit length - no FOV or scale drift.
            for (int r = 0; r < 3; ++r)
                close_to(row_length(m.data(), r), 1.0f, 1e-4f,
                         "affine: basis row stays unit length");
            // And it actually MOVED - a no-op that returns true would be the
            // same failure wearing a success label.
            const auto before = gui();
            float moved = 0.0f;
            for (int i = 0; i < 12; ++i) moved += std::fabs(m[i] - before[i]);
            check(moved > 0.1f, "affine: the basis actually rotated");
            // Identity is still a no-op on this layout.
            auto n = gui();
            check(apply_head_rotation(n.data(), 0.0f, 0.0f, 0.0f, id), "affine: identity ok");
            for (int i = 0; i < 16; ++i)
                close_to(n[i], before[i], 1e-5f, "affine: identity changes nothing");
            // Orthonormality preserved across a full sweep of head poses.
            for (int yi = -4; yi <= 4; ++yi) {
                for (int pi = -3; pi <= 3; ++pi) {
                    auto s = gui();
                    check(apply_head_rotation(s.data(), yi * 0.4f, pi * 0.3f, 0.0f, id),
                          "affine: sweep accepted");
                    for (int r = 0; r < 3; ++r)
                        close_to(row_length(s.data(), r), 1.0f, 1e-4f,
                                 "affine: sweep keeps unit rows");
                    close_to(s[15], 1.0f, 1e-6f, "affine: sweep keeps w row");
                }
            }
        }

        // GAMEPLAY vs LOADING. Both from the log: the loading/menu world view
        // sits essentially at the origin, every gameplay one carries
        // world-scale translation. This is what stops the destination scan
        // latching onto the loading camera - which it did twice, the second
        // time onto 0x2320, dragging the body and gun around with the head.
        {
            auto loading = fresh();
            loading[3] = -0.0000f; loading[7] = -0.0004f; loading[15] = 1.0000f;
            check(!is_gameplay_view(loading.data()),
                  "gameplay: the loading/menu view is rejected");
            const auto play = fresh();   // real gameplay row3/row7 values
            check(is_gameplay_view(play.data()),
                  "gameplay: a real gameplay view is accepted");
            // Scale-free: it must work at both slider settings, where the FOV
            // terms differ by more than 2x and no fixed FOV threshold could.
            auto play90 = fresh();
            play90[4] = -0.0018f * 2.1446f;
            play90[5] =  0.0018f * 2.1446f;
            play90[6] =  0.8290f * 2.1446f;
            check(is_gameplay_view(play90.data()),
                  "gameplay: still accepted at the other slider setting");
            close_to(translation_magnitude(loading.data()), 1.0004f, 1e-3f,
                     "gameplay: loading translation magnitude is tiny");
            check(translation_magnitude(play.data()) > 1000.0f,
                  "gameplay: real translation magnitude is world-scale");
        }

        // yaw_pitch_roll_from_quat must round-trip all three angles. Built by
        // composing a real roll about the view axis onto a yaw/pitch pose, not
        // by hand-writing a quaternion and hoping it contains roll.
        for (int yi = -5; yi <= 5; ++yi) {
            for (int pi = -3; pi <= 3; ++pi) {
                for (int ri = -4; ri <= 4; ++ri) {
                    const float y = static_cast<float>(yi) * 0.5f;
                    const float p = static_cast<float>(pi) * 0.35f;
                    const float r = static_cast<float>(ri) * 0.30f;
                    const Quat q = quat_mul(quat_from_yaw_pitch(y, p),
                                            Quat{0.0f, 0.0f, std::sin(r * 0.5f),
                                                 std::cos(r * 0.5f)});
                    float ry = 0.0f, rp = 0.0f, rr = 0.0f;
                    check(yaw_pitch_roll_from_quat(q, ry, rp, rr), "ypr: finite");
                    float dy = ry - y;
                    while (dy > 3.14159265f) dy -= 6.28318531f;
                    while (dy < -3.14159265f) dy += 6.28318531f;
                    close_to(dy, 0.0f, 3e-4f, "ypr: yaw round-trips");
                    close_to(rp, p, 3e-4f, "ypr: pitch round-trips");
                    close_to(rr, r, 3e-4f, "ypr: roll round-trips");
                }
            }
        }
        // A roll-free pose must report exactly zero roll, or Phase 3 would
        // inject a tilt nobody asked for on every frame.
        for (int yi = -5; yi <= 5; ++yi) {
            float ry = 0.0f, rp = 0.0f, rr = 0.0f;
            yaw_pitch_roll_from_quat(quat_from_yaw_pitch(static_cast<float>(yi) * 0.5f, 0.2f),
                                     ry, rp, rr);
            close_to(rr, 0.0f, 3e-4f, "ypr: no roll reported for a roll-free pose");
        }
    }

    // ---------------------------------------------------------------------
    // 8h. PHASE 3, LEVER V - the world-frame rotation pair.
    //
    // head_world_rotation_from_basis must reproduce EXACTLY the rotation that
    // apply_head_rotation gives renderView's own rows, and
    // apply_world_rotation_affine_view must apply that same rotation to an
    // affine view about the camera position the view itself encodes -
    // regardless of the view's sign convention or handedness, which is the
    // property the own-rows mix does not have and the reason the 2026-08-26
    // GUI-site rotation could never have been calibrated into correctness.
    // ---------------------------------------------------------------------
    {
        const HeadAxes id{};
        const HeadAxes flips = [] {
            HeadAxes a; a.flip_yaw = true; a.flip_pitch = true;  // headaxes=--+
            return a;
        }();

        // Build an orthonormal left-handed-style camera basis from yaw about Z
        // (up), matching the +0xC44 convention: forward in the XY plane, up Z.
        auto make_basis = [](float body_yaw, float r[3], float u[3], float f[3]) {
            const float c = std::cos(body_yaw), s = std::sin(body_yaw);
            f[0] = c;  f[1] = s;  f[2] = 0.0f;
            r[0] = -s; r[1] = c;  r[2] = 0.0f;
            u[0] = 0.0f; u[1] = 0.0f; u[2] = 1.0f;
        };
        auto mat3_apply = [](const float R[9], const float v[3], float out[3]) {
            for (int i = 0; i < 3; ++i)
                out[i] = R[i * 3 + 0] * v[0] + R[i * 3 + 1] * v[1] + R[i * 3 + 2] * v[2];
        };

        // (a) R agrees with apply_head_rotation_basis on the same basis, for a
        // sweep of angles and both flip sets: R * clean_axis == mixed axis.
        for (int bi = -2; bi <= 2; ++bi) {
            for (int yi = -3; yi <= 3; ++yi) {
                for (int pi = -2; pi <= 2; ++pi) {
                    const float by = static_cast<float>(bi) * 0.7f;
                    const float y = static_cast<float>(yi) * 0.4f;
                    const float p = static_cast<float>(pi) * 0.3f;
                    const float r = 0.15f * static_cast<float>(yi + pi);
                    for (const HeadAxes* ax : {&id, &flips}) {
                        float cr[3], cu[3], cf[3];
                        make_basis(by, cr, cu, cf);
                        float mr[3]{cr[0], cr[1], cr[2]}, mu[3]{cu[0], cu[1], cu[2]},
                              mf[3]{cf[0], cf[1], cf[2]};
                        check(apply_head_rotation_basis(mr, mu, mf, y, p, r, *ax),
                              "wfr: basis mix ok");
                        float R[9];
                        check(head_world_rotation_from_basis(cr, cu, cf, y, p, r, *ax, R),
                              "wfr: R built");
                        float t[3];
                        mat3_apply(R, cr, t);
                        for (int i = 0; i < 3; ++i)
                            close_to(t[i], mr[i], 1e-4f, "wfr: R*right == mixed right");
                        mat3_apply(R, cu, t);
                        for (int i = 0; i < 3; ++i)
                            close_to(t[i], mu[i], 1e-4f, "wfr: R*up == mixed up");
                        mat3_apply(R, cf, t);
                        for (int i = 0; i < 3; ++i)
                            close_to(t[i], mf[i], 1e-4f, "wfr: R*forward == mixed forward");
                    }
                }
            }
        }

        // Build an affine world->view matrix from a basis and camera position,
        // with optional per-row sign flips to model an unknown convention.
        auto make_view = [](const float r[3], const float u[3], const float f[3],
                            const float c[3], const float sign[3]) {
            std::array<float, 16> m{};
            const float* rows[3]{r, u, f};
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) m[i * 4 + j] = sign[i] * rows[i][j];
                m[i * 4 + 3] = -(m[i * 4 + 0] * c[0] + m[i * 4 + 1] * c[1] +
                                 m[i * 4 + 2] * c[2]);
            }
            m[15] = 1.0f;
            return m;
        };
        auto view_of = [](const std::array<float, 16>& m, const float p[3], float out[3]) {
            for (int i = 0; i < 3; ++i)
                out[i] = m[i * 4 + 0] * p[0] + m[i * 4 + 1] * p[1] +
                         m[i * 4 + 2] * p[2] + m[i * 4 + 3];
        };

        // (b) THE DEFINING PROPERTY. For any world point p, the rotated view
        // must see p exactly where the clean view sees the counter-rotated
        // point about the camera: V'(p) == V(c + R^T*(p - c)). Checked across
        // body yaws, head angles, world-scale camera positions AND flipped row
        // sign conventions - the case that broke the own-rows approach.
        {
            const float camPos[3]{-6135.5f, -2654.6f, 11405.3f};
            const float pts[][3]{{-6000.0f, -2600.0f, 11400.0f},
                                 {-6135.5f, -2654.6f, 11405.3f},   // AT the camera
                                 {-7000.0f, -2000.0f, 11500.0f},
                                 {0.0f, 0.0f, 0.0f}};
            const float signsets[][3]{{1.0f, 1.0f, 1.0f}, {-1.0f, 1.0f, 1.0f},
                                      {1.0f, -1.0f, -1.0f}};
            for (int bi = -2; bi <= 2; ++bi) {
                for (int yi = -2; yi <= 2; ++yi) {
                    const float by = static_cast<float>(bi) * 0.9f;
                    const float y = static_cast<float>(yi) * 0.5f;
                    const float p = 0.25f * static_cast<float>(yi);
                    float cr[3], cu[3], cf[3];
                    make_basis(by, cr, cu, cf);
                    float R[9];
                    check(head_world_rotation_from_basis(cr, cu, cf, y, p, 0.1f, flips, R),
                          "wfr: R for property test");
                    for (const auto& sign : signsets) {
                        auto v = make_view(cr, cu, cf, camPos, sign);
                        auto vr = v;
                        check(apply_world_rotation_affine_view(vr.data(), R),
                              "wfr: affine apply ok");
                        for (const auto& pt : pts) {
                            // counter-rotated point: c + R^T*(pt - c)
                            float d[3]{pt[0] - camPos[0], pt[1] - camPos[1],
                                       pt[2] - camPos[2]};
                            float dr[3];
                            for (int i = 0; i < 3; ++i)
                                dr[i] = R[0 * 3 + i] * d[0] + R[1 * 3 + i] * d[1] +
                                        R[2 * 3 + i] * d[2];
                            const float q[3]{camPos[0] + dr[0], camPos[1] + dr[1],
                                             camPos[2] + dr[2]};
                            float a[3], b[3];
                            view_of(vr, pt, a);
                            view_of(v, q, b);
                            for (int i = 0; i < 3; ++i)
                                close_to(a[i], b[i], 0.05f,
                                         "wfr: V'(p) == V(counter-rotated p)");
                        }
                    }
                }
            }
        }

        // (c) A point AT the camera is a fixed point - rotation is about the
        // eye, not the world origin, or near geometry would orbit on head turn.
        // Covered by pts[1] above; asserted separately so a failure names it.
        {
            float cr[3], cu[3], cf[3];
            make_basis(0.8f, cr, cu, cf);
            const float camPos[3]{5000.0f, -300.0f, 120.0f};
            const float sign[3]{1.0f, 1.0f, 1.0f};
            auto v = make_view(cr, cu, cf, camPos, sign);
            float R[9];
            check(head_world_rotation_from_basis(cr, cu, cf, 0.7f, -0.3f, 0.2f, id, R),
                  "wfr: R for eye-fix test");
            float before[3], after[3];
            view_of(v, camPos, before);
            check(apply_world_rotation_affine_view(v.data(), R), "wfr: eye-fix apply");
            view_of(v, camPos, after);
            for (int i = 0; i < 3; ++i)
                close_to(after[i], before[i], 1e-2f, "wfr: camera position is a fixed point");
        }

        // (d) Identity rotation is a no-op on the affine view.
        {
            float cr[3], cu[3], cf[3];
            make_basis(-1.2f, cr, cu, cf);
            const float camPos[3]{-100.0f, 2000.0f, -50.0f};
            const float sign[3]{-1.0f, 1.0f, 1.0f};
            auto v = make_view(cr, cu, cf, camPos, sign);
            const auto before = v;
            float R[9];
            check(head_world_rotation_from_basis(cr, cu, cf, 0.0f, 0.0f, 0.0f, id, R),
                  "wfr: identity R");
            check(apply_world_rotation_affine_view(v.data(), R), "wfr: identity apply");
            for (int i = 0; i < 16; ++i)
                close_to(v[i], before[i], 2e-3f, "wfr: identity leaves the view alone");
        }

        // (e) SHAPE REFUSALS - and the matrix must be left untouched. A
        // projective renderView-layout matrix must refuse (this function is
        // for the affine site only); so must a degenerate one. These mirror
        // the run-8 lesson: the refusal is counted by the caller, never silent.
        {
            std::array<float, 16> proj{
                -0.3116f, -0.3123f, 0.0000f, -6135.4849f,
                -0.0018f,  0.0018f, 0.8290f, -2654.6396f,
                 0.7082f, -0.7065f, 0.0031f, 11405.2588f,
                 0.7079f, -0.7063f, 0.0031f, 11404.0820f};
            const auto proj_before = proj;
            float R[9]{1, 0, 0, 0, 1, 0, 0, 0, 1};
            check(!apply_world_rotation_affine_view(proj.data(), R),
                  "wfr: projective layout refused");
            for (int i = 0; i < 16; ++i)
                close_to(proj[i], proj_before[i], 1e-6f, "wfr: refusal untouched (proj)");

            std::array<float, 16> degen{};
            degen[15] = 1.0f;
            check(!apply_world_rotation_affine_view(degen.data(), R),
                  "wfr: degenerate refused");
            check(!apply_world_rotation_affine_view(nullptr, R), "wfr: null refused");

            // Non-unit rows (a scaled matrix) must refuse - the formula's
            // c = -S^T t is only exact for orthonormal S.
            std::array<float, 16> scaled{
                2.0f, 0.0f, 0.0f, -10.0f,
                0.0f, 2.0f, 0.0f, -20.0f,
                0.0f, 0.0f, 2.0f, -30.0f,
                0.0f, 0.0f, 0.0f, 1.0f};
            check(!apply_world_rotation_affine_view(scaled.data(), R),
                  "wfr: scaled rows refused");
        }

        // (f) The menu-era swizzle itself (the exact matrix run 8 printed,
        // camera at body-frame (1,0,0)) passes the shape gate and rotates
        // about that encoded position - the site can never again refuse 100%
        // of calls silently for layout reasons.
        {
            std::array<float, 16> swz{
                0.0f, 1.0f, 0.0f,  0.0f,
                0.0f, 0.0f, 1.0f,  0.0f,
                1.0f, 0.0f, 0.0f, -1.0f,
                0.0f, 0.0f, 0.0f,  1.0f};
            float cr[3]{0.0f, -1.0f, 0.0f}, cu[3]{0.0f, 0.0f, 1.0f}, cf[3]{1.0f, 0.0f, 0.0f};
            float R[9];
            check(head_world_rotation_from_basis(cr, cu, cf, 0.5f, 0.2f, 0.0f, flips, R),
                  "wfr: swizzle R");
            check(apply_world_rotation_affine_view(swz.data(), R),
                  "wfr: the run-8 swizzle is accepted");
            // Its encoded camera (1,0,0) must still map to the same view coords.
            const float cam[3]{1.0f, 0.0f, 0.0f};
            float a[3];
            for (int i = 0; i < 3; ++i)
                a[i] = swz[i * 4 + 0] * cam[0] + swz[i * 4 + 1] * cam[1] +
                       swz[i * 4 + 2] * cam[2] + swz[i * 4 + 3];
            for (int i = 0; i < 3; ++i)
                close_to(a[i], 0.0f, 1e-3f, "wfr: swizzle camera stays the fixed point");
        }
    }

    // ---------------------------------------------------------------------
    // 8i. SIGN-TOLERANT MATCH + ROTATE (2026-08-28). The 09:36 run refused
    // 7,961 position-valid camera copies whose rows are NEGATED relative to
    // renderView's convention. The signed pair must (a) match them and name
    // the flipped rows, (b) rotate them to EXACTLY the same physical result
    // as the reference-convention matrix gets, for every flip combination.
    // ---------------------------------------------------------------------
    {
        const HeadAxes id{};
        const std::array<float, 16> base{
            -0.3116f, -0.3123f, 0.0000f, -6135.4849f,
            -0.0018f,  0.0018f, 0.8290f, -2654.6396f,
             0.7082f, -0.7065f, 0.0031f, 11405.2588f,
             0.7079f, -0.7063f, 0.0031f, 11404.0820f};
        float r[3], u[3], f[3];
        {
            const float l0 = row_length(base.data(), 0), l1 = row_length(base.data(), 1),
                        l3 = row_length(base.data(), 3);
            for (int i = 0; i < 3; ++i) {
                r[i] = base[i] / l0; u[i] = base[4 + i] / l1; f[i] = base[12 + i] / l3;
            }
        }
        check(matches_reference_basis_signed(base.data(), r, u, f) == 1,
              "signed: unflipped matches with mask 1");
        for (int flips = 1; flips < 8; ++flips) {
            auto mir = base;
            auto flip_row = [&](int row) {
                for (int i = 0; i < 4; ++i) mir[row * 4 + i] = -mir[row * 4 + i];
            };
            if (flips & 1) flip_row(0);
            if (flips & 2) flip_row(1);
            if (flips & 4) flip_row(3);
            const int mask = matches_reference_basis_signed(mir.data(), r, u, f);
            const int expect = 1 | ((flips & 1) ? 2 : 0) | ((flips & 2) ? 4 : 0) |
                               ((flips & 4) ? 8 : 0);
            check(mask == expect, "signed: flip combination detected exactly");
            // Rotate both; un-flip the mirrored result; must equal the
            // reference-convention result to machine precision.
            auto ref_rot = base;
            check(apply_head_rotation(ref_rot.data(), 0.5f, -0.3f, 0.1f, id),
                  "signed: reference rotate ok");
            check(apply_head_rotation_signed(mir.data(), 0.5f, -0.3f, 0.1f, id, mask),
                  "signed: mirrored rotate ok");
            if (flips & 1) flip_row(0);
            if (flips & 2) flip_row(1);
            if (flips & 4) flip_row(3);
            for (int i = 0; i < 16; ++i)
                close_to(mir[i], ref_rot[i], 2e-3f,
                         "signed: mirrored result equals reference result");
        }
        // A genuinely different camera still refuses.
        std::array<float, 16> other{};
        other[1] = 1.0f; other[6] = 1.0f; other[8] = 1.0f; other[15] = 1.0f;
        check(matches_reference_basis_signed(other.data(), r, u, f) == 0 ||
              std::fabs(r[1]) > 0.999f,
              "signed: unrelated basis refused");
    }

    // ---------------------------------------------------------------------
    // 9. Every ScaleSource has a name. A missing case would print
    //    "unrecognised" into the Phase 0 line and make RUN 1 unreadable.
    // ---------------------------------------------------------------------
    for (int i = 0; i < static_cast<int>(ScaleSource::Count); ++i) {
        const char* n = scale_source_name(static_cast<ScaleSource>(i));
        check(n != nullptr && n[0] != '\0' && std::string(n) != "unrecognised",
              "names: every ScaleSource has a name");
    }

    // ---------------------------------------------------------------------
    // 10. THE HEAD/BODY TRANSFER INVARIANT (2026-08-29, thread A).
    //
    //     camera yaw = gameYaw + (headYaw - recenterRef)
    //
    //     Transferring T into the body while advancing recenterRef by exactly
    //     T must leave the camera untouched. That is the whole stability
    //     argument, so it is pinned here rather than trusted.
    // ---------------------------------------------------------------------
    {
        // wrap_pi: the seam every yaw difference crosses.
        close_to(wrap_pi(0.0f), 0.0f, 1e-6f, "wrap_pi: zero");
        close_to(wrap_pi(3.0f), 3.0f, 1e-6f, "wrap_pi: inside the band");
        close_to(wrap_pi(6.28318531f + 0.25f), 0.25f, 1e-5f, "wrap_pi: one turn up");
        close_to(wrap_pi(-6.28318531f - 0.25f), -0.25f, 1e-5f, "wrap_pi: one turn down");
        check(std::fabs(wrap_pi(std::numeric_limits<float>::quiet_NaN())) < 1e-9f,
              "wrap_pi: NaN degrades to zero rather than looping");
        for (int k = -6; k <= 6; ++k) {
            for (int d = 0; d < 360; d += 7) {
                const float a = static_cast<float>(d) * 0.0174532925f +
                                static_cast<float>(k) * 6.28318531f;
                const float w = wrap_pi(a);
                check(w > -3.14159266f && w <= 3.14159266f, "wrap_pi: lands in (-pi, pi]");
                close_to(std::sin(w), std::sin(a), 2e-4f, "wrap_pi: same angle");
                close_to(std::cos(w), std::cos(a), 2e-4f, "wrap_pi: same angle (cos)");
            }
        }

        // deadzone_excess: zero inside the band, no jump at the edge, and the
        // identity when the deadzone is zero (the head-aim fallback).
        const float dz = 0.15f;
        close_to(deadzone_excess(0.0f, dz), 0.0f, 1e-7f, "deadzone: centre");
        close_to(deadzone_excess(dz, dz), 0.0f, 1e-7f, "deadzone: exactly on the edge");
        close_to(deadzone_excess(-dz, dz), 0.0f, 1e-7f, "deadzone: exactly on the -edge");
        close_to(deadzone_excess(dz + 0.01f, dz), 0.01f, 1e-6f, "deadzone: just outside");
        close_to(deadzone_excess(-dz - 0.01f, dz), -0.01f, 1e-6f, "deadzone: just outside -");
        for (int i = 0; i <= 200; ++i) {
            const float r = -0.5f + static_cast<float>(i) * 0.005f;
            const float e = deadzone_excess(r, dz);
            check(std::fabs(e) <= std::fabs(r) + 1e-6f, "deadzone: never amplifies");
            check(e == 0.0f || (e > 0.0f) == (r > 0.0f), "deadzone: sign preserved");
            close_to(deadzone_excess(r, 0.0f), r, 1e-7f, "deadzone: zero band is identity");
        }

        // THE INVARIANT. Perfect calibration: whatever the transfer commits,
        // the engine's own yaw advances by exactly the same amount. The camera
        // must be bit-stable while the head/body SPLIT relabels.
        for (int case_i = 0; case_i < 6; ++case_i) {
            const float head_target[6] = {0.6f, -0.6f, 0.05f, 1.9f, -2.4f, 3.0f};
            const float deads[6] = {0.0f, 0.05f, 0.14f, 0.30f, 0.0f, 0.20f};
            const float rates[6] = {2.1f, 2.1f, 0.9f, 4.0f, 0.5f, 1.5f};
            BodyTransferState st;
            float game_yaw = 0.31f;                 // DOOM's own body yaw
            const float head = head_target[case_i]; // absolute head yaw, held
            const float camera0 = game_yaw + wrap_pi(head - st.ref_yaw);
            float last_resid = 0.0f;
            // 1000 frames at 90 Hz: enough for the SLOWEST case here (0.5 rad/s
            // against a 2.4 rad head) to finish converging. A frame budget too
            // short reads as "the residual did not settle", which is a test
            // defect wearing a physics defect's face.
            for (int frame = 0; frame < 1000; ++frame) {
                const BodyTransferStep s = body_transfer_step(
                    st, head, 0.0f, deads[case_i], rates[case_i], 1.0f / 90.0f);
                // The seam applies s.residual_yaw; the body takes s.want_yaw.
                close_to(s.residual_yaw, wrap_pi(head - st.ref_yaw), 1e-6f,
                         "transfer: residual is head - ref");
                check(std::fabs(s.want_yaw) <= rates[case_i] / 90.0f + 1e-6f,
                      "transfer: step never exceeds the rate cap");
                game_yaw += s.want_yaw;                       // perfect calibration
                body_transfer_commit(st, s.want_yaw, 0.0f);   // the checkbook
                const float camera = game_yaw + wrap_pi(head - st.ref_yaw);
                close_to(camera, camera0, 2e-4f,
                         "transfer: CAMERA IS INVARIANT under the transfer");
                last_resid = s.residual_yaw;
            }
            // The residual decays to the deadzone and stops there - it never
            // crosses to the other side and it never runs away.
            check(std::fabs(last_resid) <= deads[case_i] + 1e-3f,
                  "transfer: residual settles inside the deadzone");
            check(std::fabs(head) <= deads[case_i] + 1e-3f ||
                      std::fabs(last_resid) > 0.5f * deads[case_i] - 1e-3f ||
                      deads[case_i] == 0.0f,
                  "transfer: a head past the band leaves the band-width offset");
        }

        // A head INSIDE the deadzone must transfer nothing at all: the world
        // stays locked and the gun does not move for an ordinary glance.
        {
            BodyTransferState st;
            for (int frame = 0; frame < 120; ++frame) {
                const BodyTransferStep s =
                    body_transfer_step(st, 0.10f, 0.05f, 0.20f, 2.0f, 1.0f / 90.0f);
                close_to(s.want_yaw, 0.0f, 1e-9f, "transfer: in-deadzone yaw commits nothing");
                body_transfer_commit(st, s.want_yaw, s.want_pitch);
            }
            close_to(st.ref_yaw, 0.0f, 1e-9f, "transfer: reference never moved");
        }

        // Pitch transfers on the same rule, at the narrower band.
        {
            BodyTransferState st;
            const BodyTransferStep s =
                body_transfer_step(st, 0.0f, 0.30f, 0.20f, 100.0f, 1.0f / 90.0f);
            close_to(s.want_pitch, 0.30f - 0.20f * 0.6f, 1e-5f,
                     "transfer: pitch uses 0.6x the yaw band");
        }

        // Roll is not part of the transfer at all - there is no roll term to
        // commit, so the reference can only ever move in yaw and pitch.
        {
            BodyTransferState st;
            body_transfer_commit(st, 0.25f, -0.1f);
            close_to(st.ref_yaw, 0.25f, 1e-6f, "transfer: commit moves yaw by exactly T");
            close_to(st.ref_pitch, -0.1f, 1e-6f, "transfer: commit moves pitch by exactly T");
            body_transfer_commit(st, std::numeric_limits<float>::quiet_NaN(), 0.0f);
            close_to(st.ref_yaw, 0.25f, 1e-6f, "transfer: a NaN commit is refused, not applied");
        }

        // A zero dt (a duplicate present, a paused frame) must want nothing -
        // otherwise a stall would dump the whole residual into one burst.
        {
            BodyTransferState st;
            const BodyTransferStep s =
                body_transfer_step(st, 1.2f, 0.0f, 0.1f, 2.0f, 0.0f);
            close_to(s.want_yaw, 0.0f, 1e-9f, "transfer: zero dt commits nothing");
            close_to(s.residual_yaw, 1.2f, 1e-6f, "transfer: zero dt still reports the residual");
        }

        // Crossing the +-pi seam: a head at +179 deg against a reference at
        // -179 deg is 2 degrees apart, not 358.
        {
            BodyTransferState st;
            st.ref_yaw = -3.10f;
            const BodyTransferStep s =
                body_transfer_step(st, 3.12f, 0.0f, 0.0f, 100.0f, 1.0f / 90.0f);
            close_to(std::fabs(s.residual_yaw), 6.28318531f - 6.22f, 2e-5f,
                     "transfer: the pi seam is the short way round");
        }
    }

    std::printf("%s: %d checks, %d failures\n",
                g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

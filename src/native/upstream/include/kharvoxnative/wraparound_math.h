#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// ===========================================================================
// TRUE WRAPAROUND VR - PURE MATH. No Windows, no Vulkan, no OpenXR, no state.
//
// Everything in this header is a pure function of its arguments, deliberately,
// so tests/wraparound_math_test.cpp can compile it ON ITS OWN and assert the
// properties offline - before a build ever reaches the owner's headset. Three
// runs were lost previously to defects that an offline check would have caught
// in seconds (docs/HANDOFF-2026-08-25-IMPLEMENTATION-AGENT-EXIT.md section 6).
//
// THE ONE RULE THIS HEADER EXISTS TO ENFORCE: compute_framing() returns the
// crop rectangle AND the declared half-angles from the SAME inputs in the SAME
// call. There is no way for a caller to blit one rectangle and declare a
// different field of view, because it never gets to choose them separately.
// ===========================================================================

namespace kharvoxnative::wrap {

struct Framing {
    // Crop rectangle in source pixels, relative to the EYE's source rect (the
    // caller adds its own side-by-side base x). Centred.
    int32_t crop_x{0}, crop_y{0}, crop_w{0}, crop_h{0};
    // What gets submitted as XrCompositionLayerProjectionView::fov.
    float angle_left{0.0f}, angle_right{0.0f}, angle_up{0.0f}, angle_down{0.0f};
    // The FULL source rect's field of view - what DOOM actually rendered.
    float content_fov_x{0.0f}, content_fov_y{0.0f};
    // The CROP rect's field of view. Derived from the crop rectangle, so
    // comparing this against (angle_up - angle_down) compares the blit against
    // the declaration rather than a value against itself.
    float cropped_fov_x{0.0f}, cropped_fov_y{0.0f};
    // DESTINATION RECT inside the eye swapchain - the blit's dstOffsets AND the
    // submitted subImage.imageRect, which must be the same rectangle or the
    // runtime samples pixels we never wrote.
    //
    // NEVER LARGER THAN THE CROP. Blowing a 2406x1684 crop up to a 4608x4224
    // swapchain and handing that to the runtime means TWO resamples of the same
    // information: our bilinear upscale, then the runtime's distortion sampler.
    // Writing the crop 1:1 into a sub-rectangle and telling the runtime that is
    // the image leaves exactly one resample, of the same pixels. Strictly
    // sharper, and it stops us paying blit bandwidth to invent detail.
    int32_t dst_w{0}, dst_h{0};
    float scale{0.0f};
    bool valid{false};
    // THE INPUTS THIS FRAMING WAS COMPUTED FROM (2026-08-29, eye-1 gap).
    //
    // Under true stereo the two eyes are framed from DIFFERENT sources: eye 0
    // from DOOM's own frame, the doubled eye from override_source_* (one of
    // three mirror images depending on state), and openxr_context.cpp:1147
    // switches the FOV basis to override_source_extent for that path. Nothing
    // recorded either extent, and the engagement line only ever reported ONE
    // eye - so a per-eye source mismatch was unmeasurable by construction.
    // Carried here so the two eyes can be compared against each other rather
    // than each against itself.
    int32_t src_w{0}, src_h{0};
    int32_t target_w{0}, target_h{0};
};

// DOOM's projection scale term -> vertical field of view. This is EXACTLY the
// formula openxr_context.cpp has always used with the hardcoded 1.639; only
// the input changes from a constant to a live read. Keeping the formula
// identical is what makes "wraparound absent = today, byte for byte" checkable
// rather than merely intended.
inline float fov_from_scale(float scale) {
    return 2.0f * std::atan(1.0f / scale);
}

// Inverse, for the Phase 0 log line: state the scale a measured FOV implies.
inline float scale_from_fov(float fov) {
    return 1.0f / std::tan(fov * 0.5f);
}

inline float to_degrees(float radians) {
    return radians * (180.0f / 3.14159265358979323846f);
}

// ASPECT-PRESERVING CROP, NOT A STRETCH.
//
// The old blit scaled the whole source rect onto the whole eye rect. DOOM's
// frame is ~1.879:1 and an eye rect is roughly square, so that stretch is a
// large horizontal squeeze - and the FOV declared alongside it described the
// UNCROPPED image, so declaration and content disagreed by exactly that
// factor. This crops to the eye's aspect instead and derives the declared
// angles from the crop, so the two agree by construction at any source size,
// any eye size and any FOV-slider setting.
// target_fov_x/y > 0 selects the FOV-MATCHED crop: keep as much of each axis
// as the headset's own frustum can show, so the eye is filled. Zero (or
// negative) selects the pixel-aspect crop.
//
// WHY THE CHOICE EXISTS - measured, not speculative. RUN 1b logged this
// headset as recommendedImageRect 4608x4224 (pixel aspect 1.0909) with a
// located FOV of 110x90 deg (tangent aspect 1.4281). Those disagree by 31%,
// so "crop to the buffer aspect" and "crop to what the eye can see" are
// genuinely different rectangles here and one of them leaves black edges.
// Either way the DECLARED angles come from the crop actually blitted, so the
// geometry is right in both; only coverage and sampling density differ.
inline Framing compute_framing(int32_t src_w, int32_t src_h,
                               int32_t dst_w, int32_t dst_h, float scale,
                               float target_fov_x = 0.0f, float target_fov_y = 0.0f) {
    Framing f;
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return f;
    if (!(scale > 0.0f) || !std::isfinite(scale)) return f;

    const double src_aspect = static_cast<double>(src_w) / static_cast<double>(src_h);
    const double dst_aspect = static_cast<double>(dst_w) / static_cast<double>(dst_h);

    f.content_fov_y = fov_from_scale(scale);
    f.content_fov_x = 2.0f * std::atan(
        std::tan(f.content_fov_y * 0.5f) * static_cast<float>(src_aspect));

    int32_t cw = src_w, ch = src_h;
    const bool fov_matched = target_fov_x > 0.0f && target_fov_y > 0.0f &&
                             std::isfinite(target_fov_x) && std::isfinite(target_fov_y);
    if (fov_matched) {
        // Keep the fraction of each axis whose tangent the headset can show.
        // Clamped to 1: when the content is NARROWER than the headset we keep
        // everything and simply declare less - we cannot invent field of view.
        const float fx = std::min(1.0f, std::tan(target_fov_x * 0.5f) /
                                        std::tan(f.content_fov_x * 0.5f));
        const float fy = std::min(1.0f, std::tan(target_fov_y * 0.5f) /
                                        std::tan(f.content_fov_y * 0.5f));
        cw = static_cast<int32_t>(std::lround(static_cast<double>(src_w) * fx));
        ch = static_cast<int32_t>(std::lround(static_cast<double>(src_h) * fy));
        if (cw > src_w) cw = src_w;
        if (ch > src_h) ch = src_h;
        if (cw < 1) cw = 1;
        if (ch < 1) ch = 1;
    } else if (src_aspect > dst_aspect) {
        cw = static_cast<int32_t>(std::lround(static_cast<double>(src_h) * dst_aspect));
        if (cw > src_w) cw = src_w;
        if (cw < 1) cw = 1;
    } else if (src_aspect < dst_aspect) {
        ch = static_cast<int32_t>(std::lround(static_cast<double>(src_w) / dst_aspect));
        if (ch > src_h) ch = src_h;
        if (ch < 1) ch = 1;
    }
    f.crop_w = cw;
    f.crop_h = ch;
    f.crop_x = (src_w - cw) / 2;
    f.crop_y = (src_h - ch) / 2;

    const float half_x = std::atan(std::tan(f.content_fov_x * 0.5f) *
                                   (static_cast<float>(cw) / static_cast<float>(src_w)));
    const float half_y = std::atan(std::tan(f.content_fov_y * 0.5f) *
                                   (static_cast<float>(ch) / static_cast<float>(src_h)));
    f.angle_left = -half_x;
    f.angle_right = half_x;
    f.angle_up = half_y;
    f.angle_down = -half_y;
    f.cropped_fov_x = 2.0f * half_x;
    f.cropped_fov_y = 2.0f * half_y;
    // 1:1 where the crop fits, downscale only where it genuinely exceeds the
    // swapchain. The declared angles are NOT touched by this - they describe
    // the crop, and shrinking the destination changes sampling density, never
    // geometry.
    f.dst_w = cw < dst_w ? cw : dst_w;
    f.dst_h = ch < dst_h ? ch : dst_h;
    f.scale = scale;
    f.src_w = src_w;
    f.src_h = src_h;
    f.target_w = dst_w;
    f.target_h = dst_h;
    f.valid = true;
    return f;
}

// ===========================================================================
// POSE - yaw/pitch only, roll stripped.
//
// The convention here is NOT a fresh choice: it is copied term for term from
// OpenXRContext::update_look_injection, which is what actually drives DOOM's
// camera. Declaring a pose in a different convention from the one that
// produced the content is the "declare what the runtime located, not what was
// rendered" mistake this phase exists to avoid.
//
//   forward = local -Z rotated by q;  yaw = atan2(fx, -fz);  pitch = asin(fy)
//
// Roll is dropped because DOOM's input path has no roll axis, so no roll was
// ever rendered. Declaring the runtime's roll would tell the compositor the
// image is tilted when it is not. The cost is the known v1 artefact: on head
// roll the compositor rotates our un-rolled image and black wedges appear at
// the corners. That is stated up front, not discovered in the headset.
// ===========================================================================

struct Quat { float x{0.0f}, y{0.0f}, z{0.0f}, w{1.0f}; };

inline void forward_from_quat(const Quat& q, float& fx, float& fy, float& fz) {
    fx = -2.0f * (q.x * q.z + q.y * q.w);
    fy =  2.0f * (q.x * q.w - q.y * q.z);
    fz =  2.0f * (q.x * q.x + q.y * q.y) - 1.0f;
}

inline bool yaw_pitch_from_quat(const Quat& q, float& yaw, float& pitch) {
    float fx = 0.0f, fy = 0.0f, fz = 0.0f;
    forward_from_quat(q, fx, fy, fz);
    if (fy > 1.0f) fy = 1.0f;
    if (fy < -1.0f) fy = -1.0f;
    yaw = std::atan2(fx, -fz);
    pitch = std::asin(fy);
    return std::isfinite(yaw) && std::isfinite(pitch);
}

// Ry(-yaw) * Rx(pitch) in the convention above. The sign on yaw is not a
// guess: rotating -Z about +Y by theta gives fx = -sin(theta), fz = -cos(theta),
// so atan2(fx, -fz) = -theta. The offline test round-trips this rather than
// trusting the algebra.
inline Quat quat_from_yaw_pitch(float yaw, float pitch) {
    const float hy = -yaw * 0.5f;
    const float hp = pitch * 0.5f;
    const float sy = std::sin(hy), cy = std::cos(hy);
    const float sp = std::sin(hp), cp = std::cos(hp);
    return Quat{cy * sp, cp * sy, -sp * sy, cy * cp};
}

inline Quat quat_mul(const Quat& a, const Quat& b) {
    return Quat{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

inline Quat quat_conj(const Quat& q) { return Quat{-q.x, -q.y, -q.z, q.w}; }

// Full yaw/pitch/roll in the SAME convention as yaw_pitch_from_quat, with roll
// recovered as whatever rotation about the view axis is left once yaw and pitch
// are removed. Phase 3 needs all three: we are the camera now, so roll is
// rendered rather than stripped, and the pose we declare is the pose we wrote.
inline bool yaw_pitch_roll_from_quat(const Quat& q, float& yaw, float& pitch, float& roll) {
    if (!yaw_pitch_from_quat(q, yaw, pitch)) return false;
    // Residual = what is left after the yaw/pitch part is taken out. Its
    // remaining freedom is a rotation about the local Z (view) axis.
    const Quat residual = quat_mul(quat_conj(quat_from_yaw_pitch(yaw, pitch)), q);
    roll = 2.0f * std::atan2(residual.z, residual.w);
    if (roll > 3.14159265358979323846f) roll -= 2.0f * 3.14159265358979323846f;
    if (roll < -3.14159265358979323846f) roll += 2.0f * 3.14159265358979323846f;
    return std::isfinite(roll);
}

inline Quat strip_roll(const Quat& q) {
    float yaw = 0.0f, pitch = 0.0f;
    if (!yaw_pitch_from_quat(q, yaw, pitch)) return Quat{};
    return quat_from_yaw_pitch(yaw, pitch);
}

// ===========================================================================
// WHICH NUMBER IN renderView+0xC44 IS THE PROJECTION SCALE?
//
// Not settled, deliberately. The repo carries incompatible readings of "1.639"
// and none has been checked live against the FOV slider:
//
//   * docs/RE-NOTES.md:508 logs it as camera[1] - element 1 of the flat array.
//   * looks_like_world_camera_matrix() (vulkan_hooks.cpp) treats the matrix as
//     four rows of which row 1 is the ONLY non-unit-length one, and bounds that
//     length to 1.65..1.90 - a band that EXCLUDES 1.639.
//   * The descriptor-scan predicate near vulkan_hooks.cpp:27398 pairs
//     f[1]=1.639 with f[6]=3.079, whose ratio 1.879 is exactly DOOM's frame
//     aspect - which would make f[1] the HORIZONTAL term, not the vertical one.
//
// Deciding between those offline would be an assertion. RUN 1 decides it
// instead: the Phase 0 line prints every candidate's live value with the FOV
// each implies, the owner moves the in-game FOV slider to maximum, and the
// candidate that MOVED is the projection scale. Costs nothing extra.
// ===========================================================================

enum class ScaleSource : int {
    Row1OverRow3 = 0,  // |row1| / |row3|  -- the VERTICAL term. THE ANSWER.
    Row0OverRow3 = 1,  // |row0| / |row3|  -- the HORIZONTAL term
    Row1Len      = 2,  // raw |row1| - unnormalised, kept only for the log
    Elem1        = 3,  // m[1]  - docs/RE-NOTES.md:508's reading
    Elem6        = 4,  // m[6]  - the descriptor-scan pair's other half
    Elem5        = 5,  // m[5]  - the textbook Y term
    Fixed        = 6,  // ignore the matrix; use the configured constant
    Count        = 7,
};

inline const char* scale_source_name(ScaleSource s) {
    switch (s) {
        case ScaleSource::Row1OverRow3: return "row1/row3";
        case ScaleSource::Row0OverRow3: return "row0/row3";
        case ScaleSource::Row1Len:      return "row1len";
        case ScaleSource::Elem1:        return "elem1";
        case ScaleSource::Elem6:        return "elem6";
        case ScaleSource::Elem5:        return "elem5";
        case ScaleSource::Fixed:        return "fixed";
        default:                        return "unrecognised";
    }
}

inline float row_length(const float* m, int row) {
    const float a = m[row * 4 + 0], b = m[row * 4 + 1], c = m[row * 4 + 2];
    return std::sqrt(a * a + b * b + c * c);
}

// NORMALISE BY ROW 3. This is the correction RUN 1b bought.
//
// The +0xC44 matrix carries an arbitrary per-view overall scale: across the
// gameplay world views in the 22:53 session, |row3| was 1.0000, 1.2000, 1.5099,
// 1.4912, 0.7226, 0.7886, 1.3000 and 0.8661 - eight different values in one
// session. The RAW row lengths therefore mean nothing on their own, and the
// build that read |row1| directly was reading "the projection term times
// whatever this view's scale happened to be". Divided by |row3| all eight
// collapse to the SAME pair, to four decimal places.
inline float normalised_row(const float* m, int row) {
    const float w = row_length(m, 3);
    if (!(w > 1e-6f) || !std::isfinite(w)) return 0.0f;
    return row_length(m, row) / w;
}

inline float scale_candidate(const float* m, ScaleSource s, float fixed_value) {
    switch (s) {
        case ScaleSource::Row1OverRow3: return normalised_row(m, 1);
        case ScaleSource::Row0OverRow3: return normalised_row(m, 0);
        case ScaleSource::Row1Len:      return row_length(m, 1);
        case ScaleSource::Elem1:        return std::fabs(m[1]);
        case ScaleSource::Elem6:        return std::fabs(m[6]);
        case ScaleSource::Elem5:        return std::fabs(m[5]);
        case ScaleSource::Fixed:        return fixed_value;
        default:                        return fixed_value;
    }
}

// ===========================================================================
// PHASE 3 - HEAD ROTATION INTO DOOM'S WORLD VIEW-PROJECTION MATRIX.
//
// Rotates the camera basis stored in the matrix by the head's yaw/pitch/roll,
// expressed in the camera's OWN frame - so it composes on top of whatever the
// mouse has the body pointing at, exactly like a neck on a torso. The GUI /
// viewmodel view is a different matrix entirely and is deliberately not
// touched, which is what leaves the gun, hands and body aiming where the mouse
// points while the world camera turns away from them.
//
// ⚠ WHY THIS IS NOT apply_yaw_rotation.
//
// That function mixes row 0 and row 2 directly:
//     row0' = c*row0 + s*row2      row2' = -s*row0 + c*row2
// The coefficient pattern is right - it is reproduced below and the offline
// test anchors against it - but the rows have DIFFERENT LENGTHS. Measured, at
// slider 130: |row0| = 0.4412, |row2| = 1.0004. Mixing them changes the result's
// length, and those lengths ARE the projection terms: at a 90 degree yaw row 0
// would come out at 1.0004 instead of 0.4412 and the horizontal FOV would more
// than halve. Harmless for a one-shot "did the picture move" A/B at a fixed 90
// degrees, which is all Yaw90Deg was ever used for. Fatal for continuous head
// tracking, where it would breathe the FOV with every head turn.
//
// So this normalises each row to a unit basis vector, rotates the basis, and
// restores each row's ORIGINAL length. Row lengths - and therefore the field of
// view - are invariant by construction, which the offline test asserts directly.
//
// The 4th component travels with its row. That is correct and confirmed by live
// capture: row[3] = -dot(basis, cameraPosition), which is linear in the basis,
// so it transforms with the same coefficients. See apply_yaw_rotation's own
// comment - it swings by tens of units under pure rotation with no movement.
//
// Row 2 is the depth row and row 3 the w row; they are near-parallel but not
// equal (measured 1.0004 vs 1.0000). Row 2 is carried by its DELTA against row
// 3 rather than rotated independently, so the depth-vs-w difference the
// projection encodes survives untouched.
// ===========================================================================

// The OpenXR-to-DOOM axis mapping has never been established in this repo, and
// the basis measured out of +0xC44 is left-handed in the (right, up, forward)
// sense: right x up came out as -forward on every census world view. So the
// direction each axis turns is ONE BIT per axis, not a magnitude to tune, and
// it is settled by a ten-second look-left/look-up check rather than derived
// from a convention nobody has written down.
struct HeadAxes {
    bool flip_yaw{false};
    bool flip_pitch{false};
    bool flip_roll{false};
};

namespace detail {
// One Givens rotation in the plane of two orthonormal 4-vectors. Exactly
// apply_yaw_rotation's coefficient pattern, applied to a NORMALISED pair, so
// orthonormality is preserved to machine precision however many are composed.
inline void mix_rows(float* a, float* b, float angle) {
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    for (int i = 0; i < 4; ++i) {
        const float ai = a[i], bi = b[i];
        a[i] = c * ai + s * bi;
        b[i] = -s * ai + c * bi;
    }
}
}  // namespace detail

// Returns false and leaves the matrix untouched if it is not the expected
// layout. A degenerate row means this is not a world view-projection matrix,
// and rotating an invented basis is worse than not rotating.
inline bool apply_head_rotation(float* m, float yaw, float pitch, float roll,
                                const HeadAxes& axes) {
    if (!m) return false;
    if (!std::isfinite(yaw) || !std::isfinite(pitch) || !std::isfinite(roll)) return false;
    const float l0 = row_length(m, 0);   // right
    const float l1 = row_length(m, 1);   // up
    const float l2 = row_length(m, 2);
    const float l3 = row_length(m, 3);
    if (!(l0 > 1e-6f) || !(l1 > 1e-6f)) return false;
    if (!std::isfinite(l0) || !std::isfinite(l1)) return false;

    // ===== TWO LAYOUTS, ONE PATH ==========================================
    //
    // PROJECTION layout (renderView+0xC44): row3 is the w row and carries
    // forward; row2 is the depth row, near-parallel to it but not equal.
    //
    // AFFINE layout (the GUI / viewmodel view): row3 is the homogeneous row
    // (0,0,0,1) and forward lives in ROW 2. Measured, from the run that made
    // this necessary:
    //   rowLengths=(1.0000 1.0000 1.0000 0.0000)  row3=(0.0000 0 0 1.0000)
    //
    // That zero-length row 3 is why this function refused 3308 calls out of
    // 3308 at the GUI site - across FOUR builds it never rotated that matrix
    // once, while the log line said only "refused". Lever V is the site this
    // repo measured as covering the interior walls (0.0% -> 94.4% of pixels)
    // and the gun (0.5% -> 94.9%) - exactly the geometry that will not move.
    const bool affine = !(l3 > 1e-6f) || !std::isfinite(l3);
    const int fwd_row = affine ? 2 : 3;
    const float lf = affine ? l2 : l3;
    if (!(lf > 1e-6f) || !std::isfinite(lf)) return false;

    float n0[4], n1[4], nf[4], old_row2[4], old_rowf[4];
    for (int i = 0; i < 4; ++i) {
        n0[i] = m[0 + i] / l0;
        n1[i] = m[4 + i] / l1;
        nf[i] = m[fwd_row * 4 + i] / lf;
        old_row2[i] = m[8 + i];
        old_rowf[i] = m[fwd_row * 4 + i];
    }
    float* const n3 = nf;
    const float l3_out = lf;

    // Yaw about up mixes right and forward; pitch about right mixes up and
    // forward; roll about forward mixes right and up. Applied in that order so
    // each is taken in the frame the previous one left behind - a neck, not
    // three independent world-space rotations.
    detail::mix_rows(n0, n3, axes.flip_yaw ? -yaw : yaw);
    detail::mix_rows(n1, n3, axes.flip_pitch ? -pitch : pitch);
    detail::mix_rows(n0, n1, axes.flip_roll ? -roll : roll);

    for (int i = 0; i < 4; ++i) {
        m[0 + i] = n0[i] * l0;
        m[4 + i] = n1[i] * l1;
        const float rotated_fwd = n3[i] * l3_out;
        if (affine) {
            // Forward IS row 2 here, and row 3 is the homogeneous (0,0,0,1).
            // Writing row 3 would destroy the matrix.
            m[8 + i] = rotated_fwd;
        } else {
            m[12 + i] = rotated_fwd;
            // Carry the depth row by its delta against the w row, so the
            // projection's depth-vs-w difference survives untouched.
            m[8 + i] = old_row2[i] + (rotated_fwd - old_rowf[i]);
        }
    }
    return true;
}

// Rotate a bare 3x3 camera basis - three unit vectors, no projection, no
// translation, no homogeneous row.
//
// WHY THIS EXISTS: the owner's decisive observation was "EVERYTHING that isn't
// part of the base room is moving with the head". Static BSP world geometry
// follows the rotated camera; every dynamic thing - props, models, decals,
// effects, the weapon - does not. Those are gathered by different jobs, and
// this engine already stores the camera POSITION as a bare vector separate
// from any matrix (CameraSrc+0x14b0, found by this repo's own origin scan).
// A bare ORIENTATION beside it is the natural companion, and every scan I have
// written looked for 16-float matrices and was structurally blind to it.
//
// Same Givens mixes as apply_head_rotation, so the convention cannot drift
// between the two.
inline bool apply_head_rotation_basis(float* right, float* up, float* forward,
                                      float yaw, float pitch, float roll,
                                      const HeadAxes& axes) {
    if (!right || !up || !forward) return false;
    if (!std::isfinite(yaw) || !std::isfinite(pitch) || !std::isfinite(roll)) return false;
    float n0[4] = {right[0], right[1], right[2], 0.0f};
    float n1[4] = {up[0], up[1], up[2], 0.0f};
    float n3[4] = {forward[0], forward[1], forward[2], 0.0f};
    detail::mix_rows(n0, n3, axes.flip_yaw ? -yaw : yaw);
    detail::mix_rows(n1, n3, axes.flip_pitch ? -pitch : pitch);
    detail::mix_rows(n0, n1, axes.flip_roll ? -roll : roll);
    for (int i = 0; i < 3; ++i) {
        right[i] = n0[i];
        up[i] = n1[i];
        forward[i] = n3[i];
    }
    return true;
}

// ===========================================================================
// THE WORLD-FRAME ROTATION PAIR (2026-08-26, Phase 3 / Lever V).
//
// WHY THESE EXIST. apply_head_rotation mixes a matrix's OWN rows, which is a
// rotation about the matrix's own axes. That is correct for renderView and the
// six destination copies - their rows ARE the body camera's axes - and it is
// wrong for any matrix whose rows are not (a menu-era identity view, or a view
// with a flipped sign convention): mixing those rows rotates about the wrong
// axes, or in the mirrored direction, and no per-axis flip config can repair a
// frame error.
//
// The pair below removes the assumption. head_world_rotation_from_basis builds
// the ACTUAL world-space rotation R that renderView's rows undergo under the
// same Givens mixes and the same HeadAxes flips - so it is calibrated by the
// same headaxes bits that already hold for Lever A, and no new sign key is
// needed. apply_world_rotation_affine_view then applies R to an affine
// world->view matrix about the camera position that matrix itself encodes,
// WITHOUT interpreting which row is which axis or what sign it carries: the
// formula is S' = R applied to each axis row, t' = -S'*c with c = -S^T*t,
// which is convention-free for any orthonormal S, mirrored conventions
// included.
// ===========================================================================

// Build the 3x3 world-space rotation (row-major, R[i*3+j]) that takes the
// clean camera basis to the head-rotated one. Inputs are re-orthonormalised
// (preserving their handedness) so the output is a rotation to machine
// precision even when the published basis carries small projection residue.
inline bool head_world_rotation_from_basis(const float right[3], const float up[3],
                                           const float forward[3],
                                           float yaw, float pitch, float roll,
                                           const HeadAxes& axes, float R[9]) {
    if (!right || !up || !forward || !R) return false;
    if (!std::isfinite(yaw) || !std::isfinite(pitch) || !std::isfinite(roll)) return false;
    float f[3]{forward[0], forward[1], forward[2]};
    float r[3]{right[0], right[1], right[2]};
    float u[3]{up[0], up[1], up[2]};
    auto norm3 = [](float* v) -> bool {
        const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (!(l > 1e-6f) || !std::isfinite(l)) return false;
        v[0] /= l; v[1] /= l; v[2] /= l;
        return true;
    };
    auto dot3 = [](const float* a, const float* b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    // Gram-Schmidt against forward first (it is the direction the whole scheme
    // pivots on), then right, then up - keeping the INPUT's handedness rather
    // than imposing one: the +0xC44 basis measured left-handed in the
    // (right, up, forward) sense and that is not this function's business.
    if (!norm3(f)) return false;
    const float rf = dot3(r, f);
    for (int i = 0; i < 3; ++i) r[i] -= rf * f[i];
    if (!norm3(r)) return false;
    const float uf = dot3(u, f), ur = dot3(u, r);
    for (int i = 0; i < 3; ++i) u[i] -= uf * f[i] + ur * r[i];
    if (!norm3(u)) return false;
    float r2[3]{r[0], r[1], r[2]};
    float u2[3]{u[0], u[1], u[2]};
    float f2[3]{f[0], f[1], f[2]};
    if (!apply_head_rotation_basis(r2, u2, f2, yaw, pitch, roll, axes)) return false;
    // R = sum_k new_k (outer) old_k. Orthonormal inputs make R * old_k == new_k
    // exactly, so R is by construction the same rotation renderView's own rows
    // receive from apply_head_rotation under the same angles and flips.
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            R[i * 3 + j] = r2[i] * r[j] + u2[i] * u[j] + f2[i] * f[j];
    for (int i = 0; i < 9; ++i) if (!std::isfinite(R[i])) return false;
    return true;
}

// Apply a world-space rotation R to an affine world->view matrix: rows 0..2
// hold the view axes with each row's [3] the standard -dot(axis, cameraPos)
// translation term, row 3 the homogeneous (0,0,0,1). The camera is rotated
// about the position the matrix itself encodes, so a world point AT the camera
// maps to identical view coordinates before and after.
//
// Returns false - matrix untouched - unless the layout verifies: homogeneous
// row 3, unit-length rows 0..2, pairwise-orthogonal rows 0..2. Any handedness
// and any sign convention pass; a projective matrix (renderView), a degenerate
// one, or garbage does not. Callers must count the refusal - a silent decline
// at this site already cost four builds (handoff 2026-08-26 section 8).
inline bool apply_world_rotation_affine_view(float* m, const float R[9]) {
    if (!m || !R) return false;
    for (int i = 0; i < 16; ++i) if (!std::isfinite(m[i])) return false;
    for (int i = 0; i < 9; ++i) if (!std::isfinite(R[i])) return false;
    if (!(std::fabs(m[12]) < 1e-3f && std::fabs(m[13]) < 1e-3f &&
          std::fabs(m[14]) < 1e-3f && std::fabs(m[15] - 1.0f) < 1e-3f)) return false;
    // Unit rows within 2% and pairwise orthogonal within ~1.5 degrees. The
    // gameplay GUI view measured |right| = 1.000 exactly (R22, 2026-08-23), so
    // 2% is a layout gate, not a tuning band.
    const float* rows[3]{m + 0, m + 4, m + 8};
    for (int i = 0; i < 3; ++i) {
        const float l2 = rows[i][0] * rows[i][0] + rows[i][1] * rows[i][1] +
                         rows[i][2] * rows[i][2];
        if (!(l2 > 0.96f && l2 < 1.04f)) return false;
    }
    for (int i = 0; i < 3; ++i)
        for (int j = i + 1; j < 3; ++j) {
            const float d = rows[i][0] * rows[j][0] + rows[i][1] * rows[j][1] +
                            rows[i][2] * rows[j][2];
            if (!(std::fabs(d) < 0.026f)) return false;
        }
    // Camera position in the matrix's own input space: c = -S^T * t. Exact for
    // orthonormal S of either handedness.
    float c[3];
    for (int j = 0; j < 3; ++j)
        c[j] = -(m[0 + j] * m[3] + m[4 + j] * m[7] + m[8 + j] * m[11]);
    // Each axis row rotates by R; the translation terms are rebuilt from the
    // rotated axes and the SAME camera position, which is what "rotate about
    // the eye" means for a view matrix.
    float out[12];
    for (int i = 0; i < 3; ++i) {
        const float* row = rows[i];
        for (int j = 0; j < 3; ++j)
            out[i * 4 + j] = R[j * 3 + 0] * row[0] + R[j * 3 + 1] * row[1] +
                             R[j * 3 + 2] * row[2];
        out[i * 4 + 3] = 0.0f;
    }
    for (int i = 0; i < 3; ++i)
        out[i * 4 + 3] = -(out[i * 4 + 0] * c[0] + out[i * 4 + 1] * c[1] +
                           out[i * 4 + 2] * c[2]);
    for (int i = 0; i < 12; ++i) if (!std::isfinite(out[i])) return false;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) m[i * 4 + j] = out[i * 4 + j];
    return true;
}

// ===== SIGN-TOLERANT CAMERA MATCHING (2026-08-28) ==========================
// The 09:36 run refused 7,961 position-valid camera copies as wrongDir: same
// camera, MIRRORED row convention (some rows negated relative to renderView's).
// A sign-flipped row carries the same camera; a direction gate that demands
// +1 dots is blind to it, and the own-rows rotation mix INVERTS the angle for
// a flipped row. This pair fixes both: match with |dot|, report the signs,
// and rotate with the signs compensated (flip in, rotate, flip back) so a
// mirrored matrix receives exactly the same physical rotation.
//
// Returns 0 = no match; else a bitmask: bit0 set = matched, bits 1..3 = row
// 0/1/fwd was NEGATED. rows are compared per the same affine/projective
// layout rules as matches_reference_basis.
inline int matches_reference_basis_signed(const float* m, const float r[3],
                                          const float u[3], const float f[3]) {
    if (!m) return 0;
    for (int i = 0; i < 16; ++i) if (!std::isfinite(m[i])) return 0;
    const float l0 = row_length(m, 0), l1 = row_length(m, 1);
    if (!(l0 > 1e-6f) || !(l1 > 1e-6f)) return 0;
    const float l2 = row_length(m, 2), l3 = row_length(m, 3);
    const bool affine = !(l3 > 1e-6f);
    const float lf = affine ? l2 : l3;
    if (!(lf > 1e-6f)) return 0;
    const int fwd_row = affine ? 2 : 3;
    float d0 = 0.0f, d1 = 0.0f, d2 = 0.0f;
    for (int i = 0; i < 3; ++i) {
        d0 += (m[0 + i] / l0) * r[i];
        d1 += (m[4 + i] / l1) * u[i];
        d2 += (m[fwd_row * 4 + i] / lf) * f[i];
    }
    if (!(std::fabs(d0) > 0.999f && std::fabs(d1) > 0.999f && std::fabs(d2) > 0.999f))
        return 0;
    int mask = 1;
    if (d0 < 0.0f) mask |= 2;
    if (d1 < 0.0f) mask |= 4;
    if (d2 < 0.0f) mask |= 8;
    return mask;
}

// Rotate a camera matrix whose rows may be sign-flipped relative to the
// reference convention: un-flip the flagged rows, apply the standard
// rotation, flip them back. Exact - the flip is its own inverse.
inline bool apply_head_rotation_signed(float* m, float yaw, float pitch, float roll,
                                       const HeadAxes& axes, int sign_mask) {
    if (!m || !(sign_mask & 1)) return false;
    const float l3 = row_length(m, 3);
    const bool affine = !(l3 > 1e-6f);
    const int fwd_row = affine ? 2 : 3;
    auto flip_row = [&](int row) {
        for (int i = 0; i < 4; ++i) m[row * 4 + i] = -m[row * 4 + i];
    };
    if (sign_mask & 2) flip_row(0);
    if (sign_mask & 4) flip_row(1);
    if (sign_mask & 8) flip_row(fwd_row);
    const bool ok = apply_head_rotation(m, yaw, pitch, roll, axes);
    if (sign_mask & 2) flip_row(0);
    if (sign_mask & 4) flip_row(1);
    if (sign_mask & 8) flip_row(fwd_row);
    return ok;
}

// Do nine consecutive floats form an orthonormal basis matching a reference?
inline bool basis3_matches(const float* m, const float* r, const float* u, const float* f) {
    for (int i = 0; i < 9; ++i) if (!std::isfinite(m[i])) return false;
    float d[3] = {0.0f, 0.0f, 0.0f};
    const float* ref[3] = {r, u, f};
    for (int row = 0; row < 3; ++row) {
        const float len = std::sqrt(m[row * 3 + 0] * m[row * 3 + 0] +
                                    m[row * 3 + 1] * m[row * 3 + 1] +
                                    m[row * 3 + 2] * m[row * 3 + 2]);
        if (!(len > 0.98f) || !(len < 1.02f)) return false;   // must be unit
        for (int i = 0; i < 3; ++i) d[row] += (m[row * 3 + i] / len) * ref[row][i];
    }
    // All three axes, ~2.5 degrees each. Two was not enough - a fixed axis
    // swizzle matched two of them as the camera turned past.
    return d[0] > 0.999f && d[1] > 0.999f && d[2] > 0.999f;
}

// ===========================================================================
// THE HEAD DELTA - how to rotate a matrix you cannot decompose.
//
// Dynamic geometry is drawn with a per-object MVP: the camera composed with
// that object's own model transform. Its basis is NOT the camera basis, so
// every basis-matching test in this file misses it, and it is why "everything
// that isn't part of the base room" ignores the rotation while static BSP
// world geometry (identity model transform, so MVP == VP) follows it.
//
// You cannot pull V out of P*V*M to rotate it. You do not have to:
//
//     A = VP_rot * inverse(VP_clean)
//     A * MVP = VP_rot * inverse(VP_clean) * VP_clean * M = VP_rot * M
//
// One 4x4, computed once per frame from two matrices we already hold - the
// clean world view-projection and the same matrix after apply_head_rotation.
// Left-multiplying ANY matrix built on the clean camera re-bases it onto the
// rotated one, whatever model transform is baked into it. No decomposition, no
// knowledge of P, no knowledge of M.
//
// The offline test asserts exactly the identity above against random model
// transforms, which is what makes this safe to write into GPU memory.
// ===========================================================================

// DOUBLE INTERNALLY, and that is not fastidiousness.
//
// These matrices carry translations of order 1e4 alongside basis terms of order
// 1, so the inverse is badly conditioned in float. Computed in single
// precision the identity A*(VP*M) == VP_rot*M held only to ~0.6% - which on a
// camera matrix is a visible skew, and would have been shipped as "close
// enough". In double it is exact to float rounding.
inline void mat4_multiply(const float* a, const float* b, float* out) {
    double t[16];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            double s = 0.0;
            for (int k = 0; k < 4; ++k)
                s += static_cast<double>(a[r * 4 + k]) * static_cast<double>(b[k * 4 + c]);
            t[r * 4 + c] = s;
        }
    }
    for (int i = 0; i < 16; ++i) out[i] = static_cast<float>(t[i]);
}

// Full general 4x4 inverse. These matrices are projective - the last row is not
// (0,0,0,1) - so an affine shortcut would be wrong.
inline bool mat4_inverse(const float* src, float* out) {
    double m[16];
    for (int i = 0; i < 16; ++i) m[i] = static_cast<double>(src[i]);
    double inv[16];
    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];

    double det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (!std::isfinite(det) || std::fabs(det) < 1e-30) return false;
    det = 1.0 / det;
    for (int i = 0; i < 16; ++i) {
        const double v = inv[i] * det;
        if (!std::isfinite(v)) return false;
        out[i] = static_cast<float>(v);
    }
    return true;
}

// IS THIS RECORD ACTUALLY BUILT ON OUR CAMERA?
//
// A * R is only correct when R == VP_clean * M. For anything else it is
// garbage, and the first attempt gated merely on "the last row is not
// (0,0,0,1)" - which accepts every projective matrix in the frame, including
// shadow-map views built on a LIGHT's camera. The owner's word was
// "completely unstable".
//
// There is an exact test, not a heuristic. If R = VP_clean * M with M affine
// (as every model transform is), then inverse(VP_clean) * R == M, whose last
// row is (0,0,0,1) by definition. So recover M and check its last row. A
// record built on any other camera fails this, because the recovered matrix is
// then inverse(VP_clean) * VP_other * M, which has no reason to be affine.
inline bool is_built_on_clean_vp(const float* record, const float* inv_clean,
                                 float tol = 1e-3f) {
    float m[16];
    mat4_multiply(inv_clean, record, m);
    for (int i = 0; i < 16; ++i) if (!std::isfinite(m[i])) return false;
    // Scale-invariant: the recovered M may carry an overall factor, so compare
    // the last row against (0,0,0,w) with the row's own w.
    const float w = m[15];
    if (!(std::fabs(w) > 1e-6f)) return false;
    return std::fabs(m[12] / w) < tol &&
           std::fabs(m[13] / w) < tol &&
           std::fabs(m[14] / w) < tol;
}


// A = VP_rot * inverse(VP_clean). Returns false if the clean matrix is not
// invertible, in which case the caller must leave every record alone.
inline bool compute_head_delta(const float* clean_vp, float yaw, float pitch, float roll,
                               const HeadAxes& axes, float* out_delta) {
    float rot[16];
    for (int i = 0; i < 16; ++i) rot[i] = clean_vp[i];
    if (!apply_head_rotation(rot, yaw, pitch, roll, axes)) return false;
    float inv[16];
    if (!mat4_inverse(clean_vp, inv)) return false;
    mat4_multiply(rot, inv, out_delta);
    for (int i = 0; i < 16; ++i) if (!std::isfinite(out_delta[i])) return false;
    return true;
}

// ===========================================================================
// WHICH VIEWS ARE WORLD VIEWS - measured, and FOV-independent.
//
// |row1|/|row0| equals DOOM's frame aspect for every world view and for
// nothing else. Measured in the 22:53 session: every gameplay world view read
// 1.8789, which is 3818/2032 to five figures, at BOTH slider settings and at
// every one of those eight overall scales. The other views in the same session
// read 1.6967, 2.2261, 2.2544, 2.2645, 2.2753, 2.2900, 2.3390, 2.4432, 2.4528,
// 2.5213, 2.5712, 2.5740, 2.7500, 2.7585 and 3.4590 - shadow and cube views,
// well clear of the band.
//
// This is the predicate the previous two attempts should have used. It is a
// RATIO of two terms that scale together, so it cannot move when the FOV
// slider moves - which is exactly how the last one died.
// GAMEPLAY vs LOADING/MENU, and it is not a threshold on the FOV.
//
// The translation column separates them cleanly and scale-free. Measured:
// the loading/menu world view carries (-0.0000, -0.0004, -2.0008) - essentially
// at the origin - while every gameplay world view carries world-scale values
// like (-6135, -2654, 11405). The FOV cannot do this job: at slider 130
// gameplay reads sy=0.8290 and at slider 90 it reads 1.7778, so any fixed
// number would be wrong at one of them.
//
// This exists because the destination scan latched during LOADING twice: once
// finding nothing (liveSy=1.6390, the fallback constant) and once latching onto
// a matrix at 0x2320 that matched the loading view's sy=3.0793 - which is not
// the gameplay world camera at all, and rotating it dragged the body and gun
// around with the head.
inline constexpr float kGameplayTranslationMin = 100.0f;

inline float translation_magnitude(const float* m) {
    const float a = std::fabs(m[3]), b = std::fabs(m[7]), c = std::fabs(m[15]);
    if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(c)) return 0.0f;
    return a + b + c;
}

inline bool is_gameplay_view(const float* m) {
    return translation_magnitude(m) > kGameplayTranslationMin;
}

inline bool is_world_view(const float* m, float frame_aspect) {
    const float l0 = row_length(m, 0);
    const float l1 = row_length(m, 1);
    if (!(l0 > 1e-6f) || !std::isfinite(l0) || !std::isfinite(l1)) return false;
    const float ratio = l1 / l0;
    // Fall back to a band spanning 16:9 through 21:9 if the caller has not
    // published a live aspect yet (before DOOM's first present).
    if (!(frame_aspect > 0.5f) || !std::isfinite(frame_aspect))
        return ratio > 1.70f && ratio < 1.95f;
    return std::fabs(ratio - frame_aspect) <= frame_aspect * 0.02f;
}

// The value openxr_context.cpp has hardcoded since the virtual-screen era.
// Kept as the fallback so a failed live read degrades to today's picture
// rather than to a wild FOV - and the fallback ALWAYS announces itself.
inline constexpr float kLegacyProjectionScale = 1.639f;
// A live read outside this band is rejected. 0.3 is ~146 deg, 8.0 is ~14 deg;
// anything beyond is a mis-identified field, not a FOV setting.
inline constexpr float kScaleMin = 0.30f;
inline constexpr float kScaleMax = 8.00f;

inline bool scale_plausible(float s) {
    return std::isfinite(s) && s >= kScaleMin && s <= kScaleMax;
}

// THE GATE FOR THE PHASE 0 SAMPLER - deliberately NOT
// looks_like_world_camera_matrix, and CORRECTED AFTER RUN 1 (2026-08-25).
//
// RUN 1 MEASURED THIS. The owner set DOOM's FOV slider to 130 (default 90).
// Across the previous FIFTEEN sessions in doomvr.log, ViewOriginScanStatus
// reports worldViews of 1.19-1.39 MILLION. In the 130 session it reported
// worldViews=0 out of 987,318 views. The project's strict predicate is
// FOV-COUPLED and stops matching entirely at 130.
//
// My first wide predicate died with it, and for a reason I can name: it still
// gated rows 0, 2 and 3 on bands taken from a FOV-90 build. Archived
// ViewOriginScan lines put row 0's length at ~0.946 at FOV 90; scaling by
// tan(45)/tan(65) = 0.466 for the move to 130 puts it near 0.44, straight
// through the l0 > 0.85 floor. I assumed those rows were FOV-independent.
// They are not, and assuming it cost a run.
//
// So this gates ONLY on what cannot encode a field of view: finite, not
// degenerate, and every row length inside a band so generous that no FOV
// setting can leave it. Separating world views from GUI and shadow views is
// then done by the CENSUS - which logs what it actually sees rather than
// deciding in advance what it ought to be.
inline constexpr float kRowLenFloor = 0.05f;
inline constexpr float kRowLenCeil  = 50.0f;

inline bool wide_world_vp_predicate(const float m[16]) {
    for (int i = 0; i < 16; ++i) if (!std::isfinite(m[i])) return false;
    for (int r = 0; r < 4; ++r) {
        const float l = row_length(m, r);
        if (!(l >= kRowLenFloor && l <= kRowLenCeil)) return false;
    }
    return true;
}

// ===========================================================================
// THE DECOUPLED HEAD/BODY TRANSFER (2026-08-29, thread A)
// ===========================================================================
//
// THE INVARIANT THIS EXISTS TO PRESERVE - read it before touching anything
// below. Concept taken from the shipped BioShock VR body module (M7.5 yaw
// transfer); no code and no constant of theirs is reproduced here.
//
//     camera yaw = gameYaw + (headYaw - recenterRef)
//
// The seam (Lever A / Lever V) rotates DOOM's clean camera by the RESIDUAL,
// headYaw - recenterRef, not by the absolute head yaw. Past a deadzone a
// rate-limited transfer T is injected into the BODY (DOOM's own view, via the
// look-injection mouse path) and recenterRef is advanced by exactly the same
// T. The camera is then invariant under the transfer:
//
//     (gameYaw + T) + (headYaw - (recenterRef + T)) = gameYaw + headYaw - recenterRef
//
// ⚠ ZERO LOOP GAIN, AND THAT IS THE WHOLE STABILITY ARGUMENT. The reference
// advances by the amount COMMITTED, open-loop. It never tracks the camera, the
// measured body yaw, or any other output of the system. anchor=content+slew
// was the opposite structure - its epoch tracked DOOM's own yaw through
// pipeline latency - and it oscillated. Do not add an outer loop on a measured
// yaw here without re-reading that history.
//
// WHAT "COMMITTED" MEANS HERE, AND HOW IT DIFFERS FROM THE REFERENCE DESIGN.
// BioShock writes the body yaw field directly, so its committed amount is an
// exact integer and the invariant is a theorem. We drive DOOM's own input, so
// the committed amount is what we SENT (whole mouse pixels, converted at the
// calibrated pixels-per-radian), not what the engine necessarily applied. A
// calibration error therefore appears as slow world drift, bounded by the
// error times the transfer, and continuously re-absorbed by the deadzone. The
// two sides of the bookkeeping are still the same number, so no oscillation is
// possible; only a slow offset. kPixelsPerRadian = 3000 is UNCALIBRATED
// ("tuned by feel"), which is why bodyfollow=calibrate exists.
//
// PITCH transfers exactly like yaw. ROLL cannot be injected - DOOM's input
// path has no roll axis - so roll always stays entirely at the seam.

inline constexpr float kPiF = 3.14159265358979323846f;

// Wrap to (-pi, pi]. Used on every yaw difference: a head that crosses the
// +-pi seam must not read as a full turn of residual.
inline float wrap_pi(float a) {
    if (!std::isfinite(a)) return 0.0f;
    while (a > kPiF) a -= 2.0f * kPiF;
    while (a < -kPiF) a += 2.0f * kPiF;
    return a;
}

// How far past the deadzone the residual sits, signed, with NO jump at the
// band edge (the excess starts at zero exactly on the edge). A zero deadzone
// degenerates to the identity - that is the fully-coupled head-aim fallback
// the ladder names, expressed as a config value rather than a second build.
inline float deadzone_excess(float residual, float deadzone) {
    if (!std::isfinite(residual)) return 0.0f;
    if (!(deadzone > 0.0f) || !std::isfinite(deadzone)) return residual;
    if (residual > deadzone) return residual - deadzone;
    if (residual < -deadzone) return residual + deadzone;
    return 0.0f;
}

// The open-loop reference. Advanced ONLY by committed transfers, and reset
// only on a world change or an explicit recenter.
struct BodyTransferState {
    float ref_yaw{0.0f};
    float ref_pitch{0.0f};
};

// What one frame of the transfer wants. `residual_*` is what the SEAM must
// rotate by this frame; `want_*` is what the transfer would like to inject.
// The caller quantises want_* to whole mouse pixels and commits the quantised
// amount - see body_transfer_commit.
struct BodyTransferStep {
    float residual_yaw{0.0f};
    float residual_pitch{0.0f};
    float want_yaw{0.0f};
    float want_pitch{0.0f};
};

inline BodyTransferStep body_transfer_step(const BodyTransferState& s,
                                           float head_yaw, float head_pitch,
                                           float deadzone_rad, float rate_rad_per_sec,
                                           float dt) {
    BodyTransferStep out;
    if (!std::isfinite(head_yaw) || !std::isfinite(head_pitch)) return out;
    out.residual_yaw = wrap_pi(head_yaw - s.ref_yaw);
    out.residual_pitch = head_pitch - s.ref_pitch;
    if (!std::isfinite(dt) || dt <= 0.0f) return out;
    // Pitch uses a proportionally smaller band than yaw for the same reason
    // the slew did: the vertical look range is much shorter than the
    // horizontal one, so an equal band would swallow it whole.
    const float dead_yaw = deadzone_rad;
    const float dead_pitch = deadzone_rad * 0.6f;
    float wy = deadzone_excess(out.residual_yaw, dead_yaw);
    float wp = deadzone_excess(out.residual_pitch, dead_pitch);
    if (std::isfinite(rate_rad_per_sec) && rate_rad_per_sec > 0.0f) {
        const float cap = rate_rad_per_sec * dt;
        if (wy > cap) wy = cap;
        if (wy < -cap) wy = -cap;
        if (wp > cap) wp = cap;
        if (wp < -cap) wp = -cap;
    }
    out.want_yaw = wy;
    out.want_pitch = wp;
    return out;
}

// ⚠ THE CHECKBOOK. Advance the reference by EXACTLY what was committed - no
// more, no less. The caller must pass the amount it actually sent, after pixel
// quantisation, or the two halves of the invariant drift apart.
inline void body_transfer_commit(BodyTransferState& s, float commit_yaw, float commit_pitch) {
    if (std::isfinite(commit_yaw)) s.ref_yaw = wrap_pi(s.ref_yaw + commit_yaw);
    if (std::isfinite(commit_pitch)) s.ref_pitch += commit_pitch;
}

}  // namespace kharvoxnative::wrap

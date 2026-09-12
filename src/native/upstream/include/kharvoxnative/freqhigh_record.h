// Pure math for DOOM's per-draw freqHigh_vertexUniforms records - the surface
// that drives the GUN/viewmodel (docs/REVIEW-2026-08-28-GUN-TRANSFORM-FOUND.md).
//
// Ground truth (RenderDoc reflection + buffer reads, capture
// doomvr_frame13385.rdc, verified across captures by tools/rdc_gun5_verify.py):
//
//   SKINNED layout, 160 bytes (the gun, hands, monsters - anything skinned):
//     +0   hardwareskinning   vec4 flags (1,0,0,0 on the gun)
//     +16  mvpmatrix          4 rows; clip = mvp * [modelPos;1]
//     +80  modelmatrix        3 rows; world = model * [modelPos;1]
//     +128 skinoffsets, +144 gpuambientparms
//   RIGID layout, 128 bytes (world geometry, props):
//     +0   mvpmatrix          4 rows
//     +64  modelmatrix        3 rows (IDENTITY on baked world geometry)
//     +112 mvpmatrixdeterminantsign
//
//   The record algebra: mvp = P * V * model, where P*V (the "implied VP",
//   X = mvp * inverse(model)) has a UNIT forward row (row 3 xyz), solves to
//   the camera origin published in freqLow.globalvieworigin, and for the gun
//   carries its own weapon projection with a crushed depth slab
//   row2 = a*row3 + (0,0,0,b), a~0.1 b~-0.3.
//
// Everything here is offline-testable (tests/freqhigh_record_test.cpp) and
// has no Windows/Vulkan dependencies. Storage is row-major float[16], rows
// as stored in the record; products use wrap::mat4_multiply's convention
// (standard row-major matrix product, column-vector points).

#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

#include "kharvoxnative/wraparound_math.h"

namespace kharvoxnative::freqhigh {

enum class Kind : uint8_t { None = 0, Skinned160, Rigid128 };

struct Record {
    Kind kind{Kind::None};
    float skin_flag{0.0f};  // hardwareskinning.x (skinned layout only)
    float mvp[16]{};        // clip  = mvp   * [modelPos;1]
    float model[16]{};      // world = model * [modelPos;1]; last row 0,0,0,1
};

// `data` = the record bytes at the descriptor's dynamic offset;
// `byte_range` = the DECLARED descriptor range (160 or 128). Anything else
// is Kind::None. Parsing never validates values - see validate().
inline bool parse(const float* data, uint32_t byte_range, Record& out) {
    out = Record{};
    if (data == nullptr) return false;
    const float* mvp = nullptr;
    const float* mdl = nullptr;
    if (byte_range == 160) {
        out.kind = Kind::Skinned160;
        out.skin_flag = data[0];
        mvp = data + 4;
        mdl = data + 20;
    } else if (byte_range == 128) {
        out.kind = Kind::Rigid128;
        mvp = data;
        mdl = data + 16;
    } else {
        return false;
    }
    std::memcpy(out.mvp, mvp, 64);
    std::memcpy(out.model, mdl, 48);
    out.model[12] = 0.0f; out.model[13] = 0.0f;
    out.model[14] = 0.0f; out.model[15] = 1.0f;
    return true;
}

// Max deviation of the model's 3x3 from orthonormal (row norms and pairwise
// dots). The gun measures < 1e-3; 0.02 is the acceptance bound.
inline float rigidity_error(const float model[16]) {
    float worst = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const float* r = model + i * 4;
        const float n = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
        const float e = std::fabs(n - 1.0f);
        if (e > worst) worst = e;
    }
    for (int i = 0; i < 3; ++i)
        for (int j = i + 1; j < 3; ++j) {
            const float* a = model + i * 4;
            const float* b = model + j * 4;
            const float d =
                std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2]);
            if (d > worst) worst = d;
        }
    return worst;
}

// inverse of a RIGID [R|t; 0 1]: [R^T | -R^T t]. Caller checks rigidity.
inline void rigid_inverse(const float m[16], float out[16]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            out[i * 4 + j] = m[j * 4 + i];
    for (int i = 0; i < 3; ++i)
        out[i * 4 + 3] = -(out[i * 4 + 0] * m[3] + out[i * 4 + 1] * m[7] +
                           out[i * 4 + 2] * m[11]);
    out[12] = 0.0f; out[13] = 0.0f; out[14] = 0.0f; out[15] = 1.0f;
}

// X = mvp * inverse(model): the record's implied view-projection (P*V).
inline bool implied_vp(const Record& r, float X[16],
                       float max_rigidity = 0.02f) {
    if (r.kind == Kind::None) return false;
    if (rigidity_error(r.model) > max_rigidity) return false;
    float inv[16];
    rigid_inverse(r.model, inv);
    wrap::mat4_multiply(r.mvp, inv, X);
    return true;
}

// The camera centre maps to clip (0,0,*,0): rows x, y, w of the VP dotted
// with [C;1] are zero. 3x3 Cramer solve.
// Solved in DOUBLE: world coordinates run to thousands of units and a
// float32 Cramer solve missed by several units on the real fixtures.
inline bool solve_cam_origin(const float vp[16], float out[3]) {
    const double a[3][3] = {{vp[0], vp[1], vp[2]},
                            {vp[4], vp[5], vp[6]},
                            {vp[12], vp[13], vp[14]}};
    const double b[3] = {-vp[3], -vp[7], -vp[15]};
    const double det =
        a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
        a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
        a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (std::fabs(det) < 1e-9) return false;
    auto rep = [&](int col) {
        double m[3][3];
        std::memcpy(m, a, sizeof(m));
        for (int i = 0; i < 3; ++i) m[i][col] = b[i];
        return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
               m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
               m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    };
    out[0] = static_cast<float>(rep(0) / det);
    out[1] = static_cast<float>(rep(1) / det);
    out[2] = static_cast<float>(rep(2) / det);
    return true;
}

// Depth-slab structure of a VP: row2 = a*row3 + (0,0,0,b). On the gun's
// weapon projection a~0.1, b~-0.3; on the world a~1, b~-3. `ok` requires the
// xyz residual under 5e-3 (relative to a unit forward row).
struct Slab { bool ok{false}; float a{0.0f}, b{0.0f}; };
inline Slab depth_slab(const float vp[16]) {
    Slab s;
    int pivot = -1;
    for (int i = 0; i < 3; ++i)
        if (std::fabs(vp[12 + i]) > 0.05f) { pivot = i; break; }
    if (pivot < 0) return s;
    s.a = vp[8 + pivot] / vp[12 + pivot];
    float resid = 0.0f;
    for (int i = 0; i < 3; ++i)
        resid = std::max(resid, std::fabs(vp[8 + i] - s.a * vp[12 + i]));
    s.b = vp[11] - s.a * vp[15];
    s.ok = resid < 5e-3f;
    return s;
}

// ---- identification gate for the live instrument and the fix ------------
struct Verdict {
    bool parsed{false};
    bool rigid{false};
    bool fwd_unit{false};      // |X row3 xyz| within 1e-3 of 1
    bool origin_solved{false}; // solve_cam_origin succeeded
    bool origin_matches{false};// solved origin within 8.0 of view_origin
    bool near_eye{false};      // model translation within near_max of eye
    float eye_dist{-1.0f};
    float fwd_len{0.0f};
    float X[16]{};             // valid when rigid
    Slab slab{};               // computed when rigid
};

// `view_origin` = freqLow.globalvieworigin (or the mod's own solved camera
// position). A record is THE VIEWMODEL iff every flag is set. Monsters are
// skinned too - near_eye is what separates the gun/hands from them.
inline Verdict validate(const Record& r, const float view_origin[3],
                        float near_max = 100.0f) {
    Verdict v;
    if (r.kind == Kind::None) return v;
    v.parsed = true;
    if (rigidity_error(r.model) > 0.02f) return v;
    v.rigid = true;
    float inv[16];
    rigid_inverse(r.model, inv);
    wrap::mat4_multiply(r.mvp, inv, v.X);
    v.fwd_len = std::sqrt(v.X[12] * v.X[12] + v.X[13] * v.X[13] +
                          v.X[14] * v.X[14]);
    v.fwd_unit = std::fabs(v.fwd_len - 1.0f) < 1e-3f;
    v.slab = depth_slab(v.X);
    float c[3];
    v.origin_solved = solve_cam_origin(v.X, c);
    if (v.origin_solved && view_origin != nullptr) {
        const float dx = c[0] - view_origin[0];
        const float dy = c[1] - view_origin[1];
        const float dz = c[2] - view_origin[2];
        // 8.0: measured across captures, the VP's camera sits at
        // globalvieworigin PLUS OUR OWN half-IPD eye shift (+-1.5 at ipd 3,
        // +-6.0 at ipd 12 - rdc_gun5_verify.py, all three captures). The
        // gate is a sanity guard, not the discriminator; near_eye and the
        // depth slab do the discriminating. The STRICT invariant is that
        // two same-pass records' SOLVED origins agree (same V) - the
        // offline test asserts that at 0.1.
        v.origin_matches = std::sqrt(dx * dx + dy * dy + dz * dz) < 8.0f;
    }
    if (view_origin != nullptr) {
        const float dx = r.model[3] - view_origin[0];
        const float dy = r.model[7] - view_origin[1];
        const float dz = r.model[11] - view_origin[2];
        v.eye_dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        v.near_eye = v.eye_dist < near_max;
    }
    return v;
}

// THE VIEWMODEL CLASSIFIER. Measured across three captures
// (rdc_gun5_verify.py): every gun/arms record - and ONLY those - is a
// skinned 160-byte block whose implied VP carries the crushed weapon depth
// slab (a = 0.100 +- tiny in every sample; the projection SCALES vary by
// scene, so they are deliberately not part of the signature) and whose
// model origin is within arm's reach of the eye. Exactly two such records
// per eye pass: the arms (eye_dist ~0) and the gun (eye_dist ~23-29).
inline bool is_viewmodel(const Verdict& v) {
    return v.parsed && v.rigid && v.fwd_unit && v.origin_solved &&
           v.origin_matches && v.near_eye && v.slab.ok &&
           std::fabs(v.slab.a - 0.1f) < 0.02f;
}

// ---- facing metrics for the slope census (scale-free, signed) -----------
// Signed yaw of a direction v against a clean basis: atan2(v.right, v.fwd).
// Delta-fit consecutive samples like note_pipe_rate_slot does (handle the
// +-pi wrap in the caller).
inline float signed_yaw(const float v[3], const float clean_right[3],
                        const float clean_fwd[3]) {
    const float x = v[0] * clean_right[0] + v[1] * clean_right[1] +
                    v[2] * clean_right[2];
    const float z = v[0] * clean_fwd[0] + v[1] * clean_fwd[1] +
                    v[2] * clean_fwd[2];
    return std::atan2(x, z);
}

// X's camera forward = row3 xyz (unit by INV-2), normalised defensively.
inline bool vp_forward(const float X[16], float fwd[3]) {
    const float n = std::sqrt(X[12] * X[12] + X[13] * X[13] + X[14] * X[14]);
    if (!(n > 1e-6f)) return false;
    fwd[0] = X[12] / n; fwd[1] = X[13] / n; fwd[2] = X[14] / n;
    return true;
}

// The gun model's own forward = the world image of model-space +X (measured:
// it points along the view forward when the gun is at rest) = COLUMN 0.
inline void model_forward(const float model[16], float fwd[3]) {
    fwd[0] = model[0]; fwd[1] = model[4]; fwd[2] = model[8];
}

// ---- Phase-2 correction operators (pure; wiring stays OFF until the
// ----  Phase-1 diagnostic picks the mode) --------------------------------

// model' = T(pivot) * R * T(-pivot) * model : rotate the object's WORLD
// placement by R (3x3 row-major, world space) about `pivot` (the camera
// position, so the gun orbits the eye, not the map origin).
inline void rotate_model_about_pivot(const float model[16], const float R9[9],
                                     const float pivot[3], float out[16]) {
    float rot4[16] = {R9[0], R9[1], R9[2], 0.0f,
                      R9[3], R9[4], R9[5], 0.0f,
                      R9[6], R9[7], R9[8], 0.0f,
                      0.0f, 0.0f, 0.0f, 1.0f};
    // fold the pivot into the rotation's translation column:
    // T(p)*R*T(-p) has translation p - R*p.
    for (int i = 0; i < 3; ++i)
        rot4[i * 4 + 3] = pivot[i] - (R9[i * 3 + 0] * pivot[0] +
                                      R9[i * 3 + 1] * pivot[1] +
                                      R9[i * 3 + 2] * pivot[2]);
    wrap::mat4_multiply(rot4, model, out);
}

// mvp' = mvp * inv(model_old) * model_new. EXACT: whatever P and V the
// engine baked stay untouched; only the model factor is swapped. Both model
// matrices must be rigid.
inline bool recompose_mvp(const float mvp[16], const float model_old[16],
                          const float model_new[16], float out[16]) {
    if (rigidity_error(model_old) > 0.02f) return false;
    float inv[16], x[16];
    rigid_inverse(model_old, inv);
    wrap::mat4_multiply(mvp, inv, x);
    wrap::mat4_multiply(x, model_new, out);
    return true;
}

// Rotates the CAMERA of a view-projection by R (world space) while keeping
// its position: X' = X * T(c) * R^T * T(-c) with c = X's own solved origin.
// (Rotating the world by R^T about the camera IS rotating the camera by R.
// Sign is PINNED by the offline test: with Z-up world yaw +10 deg and a
// camera looking down +X, the new forward moves toward +Y.)
inline bool rotate_vp_camera(const float X[16], const float R9[9],
                             float out[16]) {
    float c[3];
    if (!solve_cam_origin(X, c)) return false;
    // transpose of R, plus the pivot folded into the translation column
    float rot4[16] = {R9[0], R9[3], R9[6], 0.0f,
                      R9[1], R9[4], R9[7], 0.0f,
                      R9[2], R9[5], R9[8], 0.0f,
                      0.0f, 0.0f, 0.0f, 1.0f};
    for (int i = 0; i < 3; ++i)
        rot4[i * 4 + 3] = c[i] - (rot4[i * 4 + 0] * c[0] +
                                  rot4[i * 4 + 1] * c[1] +
                                  rot4[i * 4 + 2] * c[2]);
    wrap::mat4_multiply(X, rot4, out);
    return true;
}

// Write a corrected record's mvp (+ optionally model) back into a raw
// buffer laid out per `kind`. Returns bytes 4-aligned span [first,last)
// touched, for logging; writes nothing on Kind::None.
inline bool store(const Record& r, float* data) {
    if (data == nullptr) return false;
    if (r.kind == Kind::Skinned160) {
        std::memcpy(data + 4, r.mvp, 64);
        std::memcpy(data + 20, r.model, 12 * sizeof(float));
        return true;
    }
    if (r.kind == Kind::Rigid128) {
        std::memcpy(data, r.mvp, 64);
        std::memcpy(data + 16, r.model, 12 * sizeof(float));
        return true;
    }
    return false;
}

}  // namespace kharvoxnative::freqhigh

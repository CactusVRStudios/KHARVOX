// Offline unit test for include/doomvr/freqhigh_record.h.
//
// Fixtures are REAL bytes from capture doomvr_frame13385.rdc (extracted by
// tools/rdc_gun4_matrices.py, 2026-08-28): the gun's skinned 160-byte
// freqHigh record at forward-pass draw eid 2554, and the world's rigid
// 128-byte record at eid 2657. The synthetic section then proves the
// Phase-2 correction operators are EXACT algebra, so a live null can never
// again be blamed on the operator.
//
// Build (from repo root, inside vcvars64):
//   cl /nologo /std:c++20 /EHsc /W4 /I include tests\freqhigh_record_test.cpp
// Run: exit 0 = all pass.

#include "kharvoxnative/freqhigh_record.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace fh = kharvoxnative::freqhigh;

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
        std::printf("FAIL: %s (got %f, want %f +-%f)\n", what.c_str(),
                    static_cast<double>(a), static_cast<double>(b),
                    static_cast<double>(tol));
    }
}

// ---- fixtures: capture doomvr_frame13385.rdc --------------------------

// gun, skinned 160-byte record (eid 2554; also bound by 2574 and the depth
// prepass - one record, many consumers).
const float kGun160[40] = {
    1.0f, 0.0f, 0.0f, 0.0f,                                    // hardwareskinning
    -0.060112f, -1.429685f, -0.036655f, 13.425830f,            // mvpx
    0.067394f, -0.064820f, 2.417705f, -24.367762f,             // mvpy
    0.099876f, -0.004125f, -0.002895f, 1.633911f,              // mvpz
    0.998729f, -0.041250f, -0.028946f, 19.338562f,             // mvpw
    0.323818f, 0.921474f, 0.214542f, 4818.284180f,             // modelx
    -0.765185f, 0.388435f, -0.513430f, 871.226074f,            // modely
    -0.556447f, 0.002094f, 0.830880f, 343.830811f,             // modelz
    0.0f, 0.0f, 0.0f, 0.0f,                                    // skinoffsets
    1.0f, 2.5f, 0.0f, 0.0f,                                    // gpuambientparms
};

// world geometry, rigid 128-byte record (eid 2657); modelmatrix = identity,
// so its mvp IS the world view-projection.
const float kWorld128[32] = {
    -0.888872f, -0.324234f, 0.000000f, 4574.118652f,           // mvpx
    0.353270f, -0.968473f, 1.448363f, -1374.293579f,           // mvpy
    0.279195f, -0.765400f, -0.579896f, -462.680054f,           // mvpz
    0.279186f, -0.765374f, -0.579877f, -459.664612f,           // mvpw
    1.0f, 0.0f, 0.0f, 0.0f,                                    // modelx
    0.0f, 1.0f, 0.0f, 0.0f,                                    // modely
    0.0f, 0.0f, 1.0f, 0.0f,                                    // modelz
    -1.0f, -1.0f, -1.0f, -1.0f,                                // determinantsign
};

// freqLow.globalvieworigin at the same draw (identical for gun and world).
const float kViewOrigin[3] = {4822.289062f, 883.241089f, 363.250000f};

void fixture_tests() {
    fh::Record gun, world;
    check(fh::parse(kGun160, 160, gun), "parse gun 160");
    check(gun.kind == fh::Kind::Skinned160, "gun kind");
    close_to(gun.skin_flag, 1.0f, 1e-6f, "gun skin flag");
    check(fh::parse(kWorld128, 128, world), "parse world 128");
    check(world.kind == fh::Kind::Rigid128, "world kind");
    fh::Record bad;
    check(!fh::parse(kGun160, 96, bad), "reject unknown range");

    // gun invariants (INV-1..5 of rdc_gun5_verify.py, same numbers)
    check(fh::rigidity_error(gun.model) < 0.02f, "gun model rigid");
    fh::Verdict gv = fh::validate(gun, kViewOrigin);
    check(gv.parsed && gv.rigid, "gun verdict parsed+rigid");
    check(gv.fwd_unit, "gun X forward unit");
    check(gv.origin_solved, "gun X origin solved");
    check(gv.origin_matches, "gun X origin == globalvieworigin");
    check(gv.near_eye, "gun near-eye");
    close_to(gv.eye_dist, 23.18f, 0.2f, "gun eye distance");
    fh::Slab gs = fh::depth_slab(gv.X);
    check(gs.ok, "gun depth slab structure");
    close_to(gs.a, 0.1f, 0.02f, "gun slab a (crushed depth)");
    close_to(gs.b, -0.3f, 0.05f, "gun slab b");
    check(fh::is_viewmodel(gv), "gun classifies as viewmodel");

    // world invariants
    fh::Verdict wv = fh::validate(world, kViewOrigin);
    check(wv.rigid && wv.fwd_unit && wv.origin_solved, "world verdict");
    check(wv.origin_matches, "world VP origin == globalvieworigin");
    check(!wv.near_eye, "world record is NOT near-eye");
    fh::Slab ws = fh::depth_slab(wv.X);
    check(ws.ok, "world slab structure");
    close_to(ws.a, 1.0f, 0.01f, "world slab a (normal depth)");
    check(!fh::is_viewmodel(wv), "world does NOT classify as viewmodel");

    // INV-3: gun and world share ONE camera orientation.
    float gf[3], wf[3];
    check(fh::vp_forward(gv.X, gf), "gun fwd");
    check(fh::vp_forward(wv.X, wf), "world fwd");
    close_to(gf[0] * wf[0] + gf[1] * wf[1] + gf[2] * wf[2], 1.0f, 1e-3f,
             "gun/world forward agree (same V)");
    // ... and ONE camera position: the two SOLVED origins must coincide
    // (globalvieworigin itself sits ~1.4 units away - eye vs render camera).
    float gc[3], wc[3];
    check(fh::solve_cam_origin(gv.X, gc) && fh::solve_cam_origin(wv.X, wc),
          "both origins solve");
    close_to(std::sqrt((gc[0] - wc[0]) * (gc[0] - wc[0]) +
                       (gc[1] - wc[1]) * (gc[1] - wc[1]) +
                       (gc[2] - wc[2]) * (gc[2] - wc[2])),
             0.0f, 0.1f, "gun/world solved origins coincide (same V)");

    // round-trip: recompose with the SAME model must reproduce the mvp.
    float rt[16];
    check(fh::recompose_mvp(gun.mvp, gun.model, gun.model, rt),
          "round-trip recompose");
    for (int i = 0; i < 16; ++i)
        close_to(rt[i], gun.mvp[i], 1e-3f * (1.0f + std::fabs(gun.mvp[i])),
                 "round-trip mvp[" + std::to_string(i) + "]");

    // store() writes back to the exact declared offsets.
    float buf[40];
    std::memcpy(buf, kGun160, sizeof(kGun160));
    fh::Record gun2 = gun;
    gun2.mvp[3] += 5.0f;
    check(fh::store(gun2, buf), "store gun");
    close_to(buf[7], kGun160[7] + 5.0f, 1e-6f, "store hits mvp@+16");
    close_to(buf[0], 1.0f, 1e-6f, "store leaves skinning flag");
    close_to(buf[36], 1.0f, 1e-6f, "store leaves gpuambientparms");
}

// ---- synthetic algebra: the Phase-2 operators are exact ---------------

void yaw_r9(float deg, float R[9]) {
    const float a = deg * 3.14159265f / 180.0f;
    const float c = std::cos(a), s = std::sin(a);
    // Z-up world yaw
    R[0] = c; R[1] = -s; R[2] = 0.0f;
    R[3] = s; R[4] = c;  R[5] = 0.0f;
    R[6] = 0.0f; R[7] = 0.0f; R[8] = 1.0f;
}

void synthetic_tests() {
    // camera at c, forward +X, right -Y, up +Z (Z-up like DOOM).
    const float c[3] = {10.0f, 5.0f, 3.0f};
    const float fwd[3] = {1.0f, 0.0f, 0.0f};
    const float right[3] = {0.0f, -1.0f, 0.0f};
    const float up[3] = {0.0f, 0.0f, 1.0f};
    // V rows: right, up, fwd with t = -R*c
    float V[16] = {right[0], right[1], right[2], 0.0f,
                   up[0], up[1], up[2], 0.0f,
                   fwd[0], fwd[1], fwd[2], 0.0f,
                   0.0f, 0.0f, 0.0f, 1.0f};
    for (int i = 0; i < 3; ++i)
        V[i * 4 + 3] = -(V[i * 4 + 0] * c[0] + V[i * 4 + 1] * c[1] +
                         V[i * 4 + 2] * c[2]);
    // weapon-style P: x=1.4*right, y=2.4*up, w=fwd, z=0.1*w-0.3
    const float P[16] = {1.4f, 0, 0, 0,
                         0, 2.4f, 0, 0,
                         0, 0, 0.1f, -0.3f,
                         0, 0, 1.0f, 0};
    float X[16];
    kharvoxnative::wrap::mat4_multiply(P, V, X);
    // model: 30 units ahead of the camera, rotated 25 deg
    float R0[9];
    yaw_r9(25.0f, R0);
    float M[16] = {R0[0], R0[1], R0[2], c[0] + 30.0f * fwd[0],
                   R0[3], R0[4], R0[5], c[1] + 30.0f * fwd[1],
                   R0[6], R0[7], R0[8], c[2] + 30.0f * fwd[2],
                   0, 0, 0, 1.0f};
    fh::Record r;
    r.kind = fh::Kind::Skinned160;
    kharvoxnative::wrap::mat4_multiply(X, M, r.mvp);
    std::memcpy(r.model, M, sizeof(M));

    fh::Verdict v = fh::validate(r, c, 100.0f);
    check(v.rigid && v.fwd_unit && v.origin_solved && v.origin_matches &&
          v.near_eye, "synthetic verdict all-pass");
    close_to(v.eye_dist, 30.0f, 1e-3f, "synthetic eye distance");
    fh::Slab s = fh::depth_slab(v.X);
    check(s.ok, "synthetic slab");
    close_to(s.a, 0.1f, 1e-4f, "synthetic slab a");
    close_to(s.b, -0.3f, 1e-4f, "synthetic slab b");

    // rotate_model_about_pivot: orbit radius preserved; rotation composed.
    float R10[9];
    yaw_r9(10.0f, R10);
    float M2[16];
    fh::rotate_model_about_pivot(M, R10, c, M2);
    const float t2[3] = {M2[3] - c[0], M2[7] - c[1], M2[11] - c[2]};
    close_to(std::sqrt(t2[0] * t2[0] + t2[1] * t2[1] + t2[2] * t2[2]),
             30.0f, 1e-3f, "orbit radius preserved");
    // expected new offset = R10 * (30,0,0)
    close_to(t2[0], 30.0f * std::cos(10.0f * 3.14159265f / 180.0f), 1e-3f,
             "orbit x");
    close_to(t2[1], 30.0f * std::sin(10.0f * 3.14159265f / 180.0f), 1e-3f,
             "orbit y");
    check(fh::rigidity_error(M2) < 1e-3f, "rotated model still rigid");

    // recompose_mvp: EXACTLY X * M2.
    float want[16], got[16];
    kharvoxnative::wrap::mat4_multiply(X, M2, want);
    check(fh::recompose_mvp(r.mvp, r.model, M2, got), "recompose ok");
    for (int i = 0; i < 16; ++i)
        close_to(got[i], want[i], 1e-3f * (1.0f + std::fabs(want[i])),
                 "recompose[" + std::to_string(i) + "]");

    // rotate_vp_camera: origin fixed, forward turns by exactly 10 deg.
    float X2[16];
    check(fh::rotate_vp_camera(X, R10, X2), "rotate_vp_camera ok");
    float c2[3];
    check(fh::solve_cam_origin(X2, c2), "rotated VP origin solves");
    close_to(c2[0], c[0], 1e-2f, "vp origin x fixed");
    close_to(c2[1], c[1], 1e-2f, "vp origin y fixed");
    close_to(c2[2], c[2], 1e-2f, "vp origin z fixed");
    float f1[3], f2[3];
    check(fh::vp_forward(X, f1) && fh::vp_forward(X2, f2), "fwds");
    const float dot = f1[0] * f2[0] + f1[1] * f2[1] + f1[2] * f2[2];
    close_to(std::acos(dot) * 180.0f / 3.14159265f, 10.0f, 0.05f,
             "vp forward turned exactly 10 deg");
    // SIGN PIN: with world yaw +10 (counter-clockwise seen from +Z), the
    // camera forward (was +X) moves toward +Y.
    check(f2[1] > 0.0f, "vp rotation sign: +yaw turns fwd toward +Y");

    // signed_yaw counts rotations linearly: k applications = k * 10 deg.
    float mf[3];
    fh::model_forward(M, mf);
    const float y0 = fh::signed_yaw(mf, right, fwd);
    fh::model_forward(M2, mf);
    const float y1 = fh::signed_yaw(mf, right, fwd);
    float M3[16];
    fh::rotate_model_about_pivot(M2, R10, c, M3);
    fh::model_forward(M3, mf);
    const float y2 = fh::signed_yaw(mf, right, fwd);
    close_to((y1 - y0) * 180.0f / 3.14159265f,
             (y2 - y1) * 180.0f / 3.14159265f, 0.05f,
             "signed_yaw delta is linear in applied rotation");
}

}  // namespace

int main() {
    fixture_tests();
    synthetic_tests();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

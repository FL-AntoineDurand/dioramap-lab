// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// core/tests/v7_beam_incidence_test.c
//
// Implementation tests for v7-beam-incidence. Each test asserts a property
// the algorithm spec claims to hold. Inputs are hand-crafted, minimal,
// deterministic. Tolerances are tight (binary pass/fail).
//
// Spec source: bench-v2 lib/pipeline/steps/beam-incidence.js
// (meta.version=5). bench-v2 has no dedicated beam-incidence.test.js;
// the closest invariants live in test/geometry.test.js (dot/cross/
// normalize/angleBetweenNormals) and the algorithm itself. We port the
// dot/normalize-style geometric invariants that map onto beam-incidence's
// algorithm, plus algorithmic invariants stated in the JS source:
//   - default beam = straight-down along up-axis for non-strip points
//   - output is a unit vector per point
//   - per-strip beam: bh = sin(α)·ct, b_up = -cos(α) — so α=0 → straight
//     down, +α and -α reflect through the flight axis
//   - up_axis=2 (z-up) swaps the default vector
//
// Implementation reference: bench-v2 lib/pipeline/steps/beam-incidence.js
// + v3 steps/v7/v7-beam-incidence-impl.c.
//
// We include the v7-beam-incidence-impl.c source directly to access its
// file-static helpers (horiz_axes, build_beam_dirs, etc.). The test is
// then a pure compile unit that links against dioramap_steps_normals + m.
//
// V3-vs-V2 note: v3 introduces an `up_axis` field on v7_beam_params_t
// (1 = y-up bench frame, 2 = z-up LAZ frame). v2 always assumed y-up.
// So the v3 beam-direction representation is frame-configurable; tests
// exercise both frames where it matters.

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE   700        // so <math.h> exposes M_PI / M_SQRT1_2

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Fallbacks in case the system <math.h> still doesn't expose them.
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_SQRT1_2
#define M_SQRT1_2 0.70710678118654752440
#endif

// Pull in the impl as part of this TU so we can call its statics.
#include "v7-beam-incidence-impl.c"

// ─── harness ────────────────────────────────────────────────────────
static int g_checks = 0;
static int g_failed = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        g_checks++;                                                                                \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                        \
            g_failed++;                                                                            \
        }                                                                                          \
    } while (0)

#define CHECK_CLOSE(a, b, eps)                                                                     \
    do {                                                                                           \
        double _a = (a), _b = (b);                                                                 \
        g_checks++;                                                                                \
        if (fabs(_a - _b) > (eps)) {                                                               \
            fprintf(stderr, "FAIL %s:%d: %s ≈ %s (got %.12g, want %.12g, eps=%g)\n",               \
                    __FILE__, __LINE__, #a, #b, _a, _b, (eps));                                    \
            g_failed++;                                                                            \
        }                                                                                          \
    } while (0)

// ─── helpers ────────────────────────────────────────────────────────

// Tiny deterministic PRNG (Numerical Recipes LCG) — used only to add
// noise that breaks the PCA degeneracy in iso-angle bands.  A perfectly
// linear band has eigval2=0, which the algorithm rejects (< 1e-10).
static uint32_t lcg_state = 0xC0FFEE42u;
static double lcg_unit(void) {
    lcg_state = lcg_state * 1103515245u + 12345u;
    return (double)((lcg_state >> 16) & 0x7fff) / 32768.0 - 0.5;
}
static void lcg_seed(uint32_t s) { lcg_state = s; }

// Build a synthetic strip simulating an aircraft flying along +x at fixed
// altitude `alt`, scanning across the +z direction (y-up frame).  Points
// per scan angle are placed on the ground at y=0, x distributed along
// flight, z = alt·tan(angle) so the geometry matches a real cross-track
// laser sweep.  All points belong to a single PSID and are listed in
// building_indices.
//
// Tiny noise (0.01 m) is added to x and z so iso-angle bands have a
// non-degenerate minor PCA axis (the algorithm rejects e2 < 1e-10).
// Noise is small enough that the recovered flight direction is still
// +x to within 1e-3 in dot product.
//
// On return, positions/scan_angles/psids/building_indices are filled.
// Caller pre-allocates everything with capacity n_x*n_angles.
static void build_y_up_strip(double*   positions,
                              float*    scan_angles,
                              uint16_t* psids,
                              uint32_t* building_indices,
                              size_t    n_x,
                              size_t    n_angles,
                              const double* angles_deg,
                              uint16_t  psid,
                              double    alt) {
    lcg_seed(0xC0FFEE42u);
    size_t k = 0;
    for (size_t ix = 0; ix < n_x; ix++) {
        for (size_t ia = 0; ia < n_angles; ia++) {
            const double angle_rad = angles_deg[ia] * M_PI / 180.0;
            const double x = (double)ix * 1.0;            // 1m spacing along flight
            const double z = alt * tan(angle_rad);        // ground hit cross-track
            positions[k * 3 + 0] = x + lcg_unit() * 0.01;
            positions[k * 3 + 1] = 0.0;                   // ground at y=0
            positions[k * 3 + 2] = z + lcg_unit() * 0.01;
            scan_angles[k]       = (float)angles_deg[ia];
            psids[k]             = psid;
            building_indices[k]  = (uint32_t)k;
            k++;
        }
    }
}

// Length of a 3-vector (Float32).
static double vec3f_len(const float* v) {
    return sqrt((double)v[0] * v[0] + (double)v[1] * v[1] + (double)v[2] * v[2]);
}

// ─── horiz_axes: frame selection ────────────────────────────────────

static void test_horiz_axes_y_up(void) {
    // Spec: y-up (up_axis=1) → horizontal axes are (x, z) = (0, 2).
    int h0 = -1, h1 = -1;
    horiz_axes(1, &h0, &h1);
    CHECK(h0 == 0);
    CHECK(h1 == 2);
}

static void test_horiz_axes_z_up(void) {
    // Spec: z-up (up_axis=2, LAZ frame) → horizontal axes are (x, y) = (0, 1).
    int h0 = -1, h1 = -1;
    horiz_axes(2, &h0, &h1);
    CHECK(h0 == 0);
    CHECK(h1 == 1);
}

// ─── default beam direction ────────────────────────────────────────

static void test_default_beam_y_up_straight_down(void) {
    // Spec: points with no strip fit get the default vector pointing
    // straight down along -y in the y-up frame.  This is the contract
    // for the "consumer reads this for every point" guarantee.
    const size_t n_pts = 5;
    double   positions[15]   = {0};
    float    scan_angles[5]  = {0};
    uint16_t psids[5]        = {0};
    float    out[15];

    v7_beam_params_t p; v7_beam_params_default(&p);
    p.up_axis = 1;
    // No building_indices → no strips → all points get default.
    int rc = v7_beam_compute(positions, scan_angles, psids,
                              NULL, NULL, n_pts,
                              NULL, 0, &p, out);
    CHECK(rc == 0);
    for (size_t i = 0; i < n_pts; i++) {
        CHECK_CLOSE(out[i * 3 + 0],  0.0, 1e-7);
        CHECK_CLOSE(out[i * 3 + 1], -1.0, 1e-7);
        CHECK_CLOSE(out[i * 3 + 2],  0.0, 1e-7);
    }
}

static void test_default_beam_z_up_straight_down(void) {
    // Spec: with up_axis=2 (LAZ z-up), the default flips to (0, 0, -1).
    // V3-only invariant; v2 didn't have this.
    const size_t n_pts = 3;
    double   positions[9]    = {0};
    float    scan_angles[3]  = {0};
    uint16_t psids[3]        = {0};
    float    out[9];

    v7_beam_params_t p; v7_beam_params_default(&p);
    p.up_axis = 2;
    int rc = v7_beam_compute(positions, scan_angles, psids,
                              NULL, NULL, n_pts,
                              NULL, 0, &p, out);
    CHECK(rc == 0);
    for (size_t i = 0; i < n_pts; i++) {
        CHECK_CLOSE(out[i * 3 + 0],  0.0, 1e-7);
        CHECK_CLOSE(out[i * 3 + 1],  0.0, 1e-7);
        CHECK_CLOSE(out[i * 3 + 2], -1.0, 1e-7);
    }
}

static void test_small_strip_falls_back_to_default(void) {
    // Spec: a strip with fewer than min_points_per_strip members is
    // skipped at Phase 2a — its points keep the default straight-down
    // vector.  Default min is 30; we feed 5.
    const size_t n_pts = 5;
    double   positions[15];
    float    scan_angles[5];
    uint16_t psids[5];
    uint32_t buildidx[5];
    float    out[15];
    for (size_t i = 0; i < n_pts; i++) {
        positions[i * 3 + 0] = (double)i;
        positions[i * 3 + 1] = 0.0;
        positions[i * 3 + 2] = 0.0;
        scan_angles[i]       = (float)i;   // some variety
        psids[i]             = 7;
        buildidx[i]          = (uint32_t)i;
    }

    v7_beam_params_t p; v7_beam_params_default(&p);
    p.up_axis = 1;
    int rc = v7_beam_compute(positions, scan_angles, psids,
                              NULL, NULL, n_pts,
                              buildidx, n_pts, &p, out);
    CHECK(rc == 0);
    for (size_t i = 0; i < n_pts; i++) {
        CHECK_CLOSE(out[i * 3 + 0],  0.0, 1e-7);
        CHECK_CLOSE(out[i * 3 + 1], -1.0, 1e-7);
        CHECK_CLOSE(out[i * 3 + 2],  0.0, 1e-7);
    }
}

// ─── per-point beam direction: unit vector + geometric content ─────

static void test_painted_beams_are_unit_vectors(void) {
    // Spec: every beam direction emitted by build_beam_dirs is normalized
    // (bench-v2 line 287-291: `len = hypot(bx, by, bz); beamDirs[o]=bx/len; ...`).
    // We synthesize a strip large enough to clear Phase 2a, run the
    // pipeline, and verify ||beam|| == 1 for every point in the strip.
    // n_x=20 makes edge_count_clamped (20) match one angle's worth of
    // points exactly, so the PCA edge bands stay 1D along flight.
    const size_t n_x = 20, n_angles = 9;
    const size_t n_pts = n_x * n_angles;   // 180 points → ≥30
    double angles_deg[9] = { -20, -15, -10, -5, 0, 5, 10, 15, 20 };

    double*   positions   = (double*)malloc(sizeof(double) * 3 * n_pts);
    float*    scan_angles = (float*)malloc(sizeof(float) * n_pts);
    uint16_t* psids       = (uint16_t*)malloc(sizeof(uint16_t) * n_pts);
    uint32_t* buildidx    = (uint32_t*)malloc(sizeof(uint32_t) * n_pts);
    float*    out         = (float*)malloc(sizeof(float) * 3 * n_pts);

    build_y_up_strip(positions, scan_angles, psids, buildidx,
                     n_x, n_angles, angles_deg, /*psid*/ 42, /*alt*/ 100.0);

    v7_beam_params_t p; v7_beam_params_default(&p);
    p.up_axis = 1;
    int rc = v7_beam_compute(positions, scan_angles, psids,
                              NULL, NULL, n_pts,
                              buildidx, n_pts, &p, out);
    CHECK(rc == 0);

    for (size_t i = 0; i < n_pts; i++) {
        const double L = vec3f_len(out + i * 3);
        CHECK_CLOSE(L, 1.0, 1e-5);
    }

    free(positions); free(scan_angles); free(psids); free(buildidx); free(out);
}

static void test_zero_angle_beam_points_straight_down(void) {
    // Spec: at scan_angle=0, sin(α)=0 → horizontal component is the
    // multi-return correction only (=0 when no return arrays passed),
    // and b_up = -cos(0) = -1.  So the beam at α=0 must equal (0, -1, 0)
    // exactly (within FP error).  This is a hard geometric anchor.
    const size_t n_x = 20, n_angles = 9;
    const size_t n_pts = n_x * n_angles;
    double angles_deg[9] = { -20, -15, -10, -5, 0, 5, 10, 15, 20 };

    double*   positions   = (double*)malloc(sizeof(double) * 3 * n_pts);
    float*    scan_angles = (float*)malloc(sizeof(float) * n_pts);
    uint16_t* psids       = (uint16_t*)malloc(sizeof(uint16_t) * n_pts);
    uint32_t* buildidx    = (uint32_t*)malloc(sizeof(uint32_t) * n_pts);
    float*    out         = (float*)malloc(sizeof(float) * 3 * n_pts);

    build_y_up_strip(positions, scan_angles, psids, buildidx,
                     n_x, n_angles, angles_deg, /*psid*/ 5, /*alt*/ 100.0);

    v7_beam_params_t p; v7_beam_params_default(&p);
    p.up_axis = 1;
    int rc = v7_beam_compute(positions, scan_angles, psids,
                              NULL, NULL, n_pts,
                              buildidx, n_pts, &p, out);
    CHECK(rc == 0);

    // Find all points with angle exactly 0; they should be straight down.
    // Tolerance is a bit relaxed because the recovered ct direction has
    // sub-1° PCA noise from the 0.01m position jitter; sin(small_err)
    // shows up in the horizontal component even at α=0.
    int n_checked = 0;
    for (size_t i = 0; i < n_pts; i++) {
        if (scan_angles[i] != 0.0f) continue;
        CHECK_CLOSE(out[i * 3 + 0],  0.0, 1e-5);
        CHECK_CLOSE(out[i * 3 + 1], -1.0, 1e-5);
        CHECK_CLOSE(out[i * 3 + 2],  0.0, 1e-5);
        n_checked++;
    }
    CHECK(n_checked == (int)n_x);  // we expect 20 such points

    free(positions); free(scan_angles); free(psids); free(buildidx); free(out);
}

static void test_beam_up_component_matches_cos_alpha(void) {
    // Spec (beam-incidence.js Phase 3b): b_up = -cos(α) before normalize.
    // After normalize: out_up = -cos(α) / hypot(sin α, cos α) = -cos(α)
    // (the cross-track is a unit vector and orthogonal to up, so the
    // total beam length is hypot(sin, cos)=1).  Therefore the y component
    // must equal exactly -cos(α_rad) for every painted point in y-up.
    const size_t n_x = 20, n_angles = 5;
    const size_t n_pts = n_x * n_angles;
    double angles_deg[5] = { -30, -15, 0, 15, 30 };

    double*   positions   = (double*)malloc(sizeof(double) * 3 * n_pts);
    float*    scan_angles = (float*)malloc(sizeof(float) * n_pts);
    uint16_t* psids       = (uint16_t*)malloc(sizeof(uint16_t) * n_pts);
    uint32_t* buildidx    = (uint32_t*)malloc(sizeof(uint32_t) * n_pts);
    float*    out         = (float*)malloc(sizeof(float) * 3 * n_pts);

    build_y_up_strip(positions, scan_angles, psids, buildidx,
                     n_x, n_angles, angles_deg, /*psid*/ 1, /*alt*/ 100.0);

    v7_beam_params_t p; v7_beam_params_default(&p);
    p.up_axis = 1;
    int rc = v7_beam_compute(positions, scan_angles, psids,
                              NULL, NULL, n_pts,
                              buildidx, n_pts, &p, out);
    CHECK(rc == 0);

    for (size_t i = 0; i < n_pts; i++) {
        const double a_rad = (double)scan_angles[i] * M_PI / 180.0;
        const double expected_uy = -cos(a_rad);
        CHECK_CLOSE((double)out[i * 3 + 1], expected_uy, 1e-5);
    }

    free(positions); free(scan_angles); free(psids); free(buildidx); free(out);
}

static void test_beam_angle_symmetry(void) {
    // Spec: within a single strip, +α and -α produce beams that are
    // mirror images across the flight axis.  Concretely: same b_up
    // (cos is even), and opposite-signed horizontal components.
    // Equivalent to: dot(beam(+α), beam(-α)) = cos²α - sin²α = cos(2α).
    //
    // We use a perfectly symmetric strip so the recovered flight
    // direction lands cleanly on +x, and pick a point at +15° + one
    // at -15°.  After normalization the dot equals cos(30°) = √3/2.
    const size_t n_x = 20, n_angles = 5;
    const size_t n_pts = n_x * n_angles;
    double angles_deg[5] = { -15, -7.5, 0, 7.5, 15 };

    double*   positions   = (double*)malloc(sizeof(double) * 3 * n_pts);
    float*    scan_angles = (float*)malloc(sizeof(float) * n_pts);
    uint16_t* psids       = (uint16_t*)malloc(sizeof(uint16_t) * n_pts);
    uint32_t* buildidx    = (uint32_t*)malloc(sizeof(uint32_t) * n_pts);
    float*    out         = (float*)malloc(sizeof(float) * 3 * n_pts);

    build_y_up_strip(positions, scan_angles, psids, buildidx,
                     n_x, n_angles, angles_deg, /*psid*/ 9, /*alt*/ 100.0);

    v7_beam_params_t p; v7_beam_params_default(&p);
    p.up_axis = 1;
    int rc = v7_beam_compute(positions, scan_angles, psids,
                              NULL, NULL, n_pts,
                              buildidx, n_pts, &p, out);
    CHECK(rc == 0);

    // Pick the first point with angle == +15 and the first with -15.
    int i_plus = -1, i_minus = -1;
    for (size_t i = 0; i < n_pts; i++) {
        if (i_plus  == -1 && scan_angles[i] ==  15.0f) i_plus  = (int)i;
        if (i_minus == -1 && scan_angles[i] == -15.0f) i_minus = (int)i;
    }
    CHECK(i_plus >= 0 && i_minus >= 0);

    const float* bp = out + i_plus * 3;
    const float* bm = out + i_minus * 3;
    // Dot product:
    const double d = (double)bp[0]*bm[0] + (double)bp[1]*bm[1] + (double)bp[2]*bm[2];
    // Expected: cos(30°) = sqrt(3)/2 ≈ 0.8660254
    CHECK_CLOSE(d, sqrt(3.0) / 2.0, 1e-4);

    // Up components are equal (b_up depends on cos α which is even):
    CHECK_CLOSE((double)bp[1], (double)bm[1], 1e-5);

    // Horizontal components are opposite-signed:
    CHECK((double)bp[0] * bm[0] + (double)bp[2] * bm[2] < 0);

    free(positions); free(scan_angles); free(psids); free(buildidx); free(out);
}

static void test_cross_track_perpendicular_to_flight(void) {
    // Spec: cross_track = perpendicular(-flight_h1, +flight_h0) in the
    // horizontal plane.  Therefore dot(flight, ct) == 0 in (h0, h1).
    // Our synthetic strip has flight = +x; ct must be along ±z.
    // We probe the painted output: pick a point at +α and extract the
    // ct direction by normalizing the horizontal beam projection
    // (bh = sin(α)·ct, with α≠0).
    const size_t n_x = 20, n_angles = 5;
    const size_t n_pts = n_x * n_angles;
    double angles_deg[5] = { -20, -10, 0, 10, 20 };

    double*   positions   = (double*)malloc(sizeof(double) * 3 * n_pts);
    float*    scan_angles = (float*)malloc(sizeof(float) * n_pts);
    uint16_t* psids       = (uint16_t*)malloc(sizeof(uint16_t) * n_pts);
    uint32_t* buildidx    = (uint32_t*)malloc(sizeof(uint32_t) * n_pts);
    float*    out         = (float*)malloc(sizeof(float) * 3 * n_pts);

    build_y_up_strip(positions, scan_angles, psids, buildidx,
                     n_x, n_angles, angles_deg, /*psid*/ 11, /*alt*/ 100.0);

    v7_beam_params_t p; v7_beam_params_default(&p);
    p.up_axis = 1;
    int rc = v7_beam_compute(positions, scan_angles, psids,
                              NULL, NULL, n_pts,
                              buildidx, n_pts, &p, out);
    CHECK(rc == 0);

    // Find any point at α=+20°. Its horizontal beam = sin(20°)·ct.
    // We extract ct by normalizing the horizontal projection of the beam.
    int i20 = -1;
    for (size_t i = 0; i < n_pts; i++) {
        if (scan_angles[i] == 20.0f) { i20 = (int)i; break; }
    }
    CHECK(i20 >= 0);
    const float* b = out + i20 * 3;
    const double bh0 = b[0], bh1 = b[2];
    const double hL  = sqrt(bh0 * bh0 + bh1 * bh1);
    CHECK(hL > 1e-6);
    const double ct_x = bh0 / hL;
    const double ct_z = bh1 / hL;

    // Flight direction is +x (the strip was built with x along flight).
    // dot(flight, ct) = 1·ct_x + 0·ct_z = ct_x.
    // For a perfectly symmetric strip we expect ct_x ≈ 0, ct_z ≈ ±1.
    CHECK_CLOSE(ct_x, 0.0, 5e-2);     // small slack: PCA isn't exact on 9 bands
    CHECK_CLOSE(fabs(ct_z), 1.0, 5e-2);

    free(positions); free(scan_angles); free(psids); free(buildidx); free(out);
}

// ─── error paths ────────────────────────────────────────────────────

static void test_null_inputs_return_error(void) {
    // Spec (header line "Returns 0 on success, -1 on alloc failure"; impl
    // line "if (!positions || !scan_angles || !psids || !out_beam_dirs
    // || !params) return -1"): null required pointers → -1 with no
    // memory writes.
    v7_beam_params_t p; v7_beam_params_default(&p);
    float out[3];
    double pos[3] = {0};
    float  ang[1] = {0};
    uint16_t ps[1] = {0};

    CHECK(v7_beam_compute(NULL, ang,  ps,   NULL, NULL, 1, NULL, 0, &p,   out) == -1);
    CHECK(v7_beam_compute(pos,  NULL, ps,   NULL, NULL, 1, NULL, 0, &p,   out) == -1);
    CHECK(v7_beam_compute(pos,  ang,  NULL, NULL, NULL, 1, NULL, 0, &p,   out) == -1);
    CHECK(v7_beam_compute(pos,  ang,  ps,   NULL, NULL, 1, NULL, 0, &p,   NULL) == -1);
    CHECK(v7_beam_compute(pos,  ang,  ps,   NULL, NULL, 1, NULL, 0, NULL, out) == -1);
}

// ─── parameter defaults ─────────────────────────────────────────────

static void test_default_params_match_spec(void) {
    // Spec: bench-v2 documents these defaults; v3 mirrors them in
    // v7_beam_params_default.  Lock them down so a silent change in
    // the C port surfaces here.
    v7_beam_params_t p;
    v7_beam_params_default(&p);
    CHECK(p.min_points_per_strip == 30);
    CHECK_CLOSE(p.min_band_elongation, 3.0, 1e-12);
    CHECK(p.mr_min_pairs == 5);
    CHECK_CLOSE(p.mr_max_angle_jitter, 0.1, 1e-12);
    CHECK_CLOSE(p.mr_min_pair_dist, 0.05, 1e-12);
    CHECK(p.up_axis == 1);  // y-up default (bench frame)
}

// ─── main ───────────────────────────────────────────────────────────

int main(void) {
    test_horiz_axes_y_up();
    test_horiz_axes_z_up();

    test_default_beam_y_up_straight_down();
    test_default_beam_z_up_straight_down();
    test_small_strip_falls_back_to_default();

    test_painted_beams_are_unit_vectors();
    test_zero_angle_beam_points_straight_down();
    test_beam_up_component_matches_cos_alpha();
    test_beam_angle_symmetry();
    test_cross_track_perpendicular_to_flight();

    test_null_inputs_return_error();
    test_default_params_match_spec();

    printf("%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}

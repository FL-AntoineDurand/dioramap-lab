// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// core/tests/v7_normal_cascade_seed_test.c
//
// Implementation tests for v7-normal-cascade-seed. Each test asserts an
// invariant the algorithm spec claims to hold. Inputs are hand-crafted,
// minimal, deterministic. Tolerances are tight (binary pass/fail).
//
// Spec source: bench-v2 lib/pipeline/steps/normal-cascade-seed.js
// (the JS source is the spec; v3's implementation is a direct port).
//
// bench-v2 has no single normal-cascade-seed.test.js — the criteria
// the algorithm relies on are exercised indirectly via
// test/ridge-and-alignment.test.js, test/ridge-knn-density.test.js,
// and test/edge-detection.test.js (ridge/normal helpers shared with
// this step's curvature-gate inputs). Most of the invariants encoded
// here come from the algorithm spec itself, since this step is a v3
// invention in its current consolidated form (v7's port drops several
// v2 artifacts; see the impl header for the diff).
//
// Implementation reference: v3 steps/v7/v7-normal-cascade-seed-impl.c.
//
// We include the v7-normal-cascade-seed-impl.c source directly so the
// test can call into the public compute() entry point without linking
// against dioramap_steps_v7 (which would duplicate the impl's symbols).
// The plane-fit helper lives in a separate TU (v7-plane-fit.c) that is
// also pulled in directly here for the same reason.

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE   700        // so <math.h> exposes M_PI

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Pull the impls into this TU. v7-plane-fit.c first because the seed
// impl calls v7_plane_fit_lsq / v7_azimuth_from_normal.
#include "v7-plane-fit.c"
#include "v7-normal-cascade-seed-impl.c"

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

// ─── input builders ─────────────────────────────────────────────────

// Build a w×d grid of points on the plane y = a*x + b*z + c (y-up).
// Returns the number of points written to `pts` (interleaved F64×3).
// Caller must size pts >= w*d*3.
static size_t build_grid_plane_yup(double* pts, int w, int d, double dx, double dz,
                                   double a, double b, double c) {
    size_t k = 0;
    for (int i = 0; i < w; i++) {
        for (int j = 0; j < d; j++) {
            const double x = i * dx;
            const double z = j * dz;
            const double y = a * x + b * z + c;
            pts[k * 3 + 0] = x;
            pts[k * 3 + 1] = y;
            pts[k * 3 + 2] = z;
            k++;
        }
    }
    return k;
}

// Build an indices array [0..n) for a "single segment owns every point" setup.
static void fill_identity_indices(uint32_t* idx, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) idx[i] = i;
}

// Fill an F32 array with 1.0. v7_seed_compute now REQUIRES non-NULL
// point_weights (see v7-plane-fit.h); tests pass an explicit uniform-1.0
// array so the "unweighted-equivalent" intent is visible at the call
// site (no silent NULL fallback).
static void fill_uniform_weights(float* w, size_t n) {
    for (size_t i = 0; i < n; i++) w[i] = 1.0f;
}

// ─── tests ──────────────────────────────────────────────────────────

static void test_zero_segments_emit_no_planes(void) {
    // Spec: n_segments == 0 → output has n_planes == 0 and owner all -1.
    // (Early-return path inside v7_seed_compute.) The argument-validation
    // gate forbids `segments == NULL` outright, so pass an empty-but-real
    // sentinel slot.
    double pts[9] = {0, 0, 0,  1, 0, 0,  0, 0, 1};
    float weights[3] = { 1.0f, 1.0f, 1.0f };
    v7_seed_input_segment_t sentinel = { .source_segment_id = 0, .pt_indices = NULL, .n_pts = 0 };
    v7_seed_params_t p; v7_seed_params_default(&p);
    v7_seed_outputs_t out; v7_seed_outputs_init(&out);
    int rc = v7_seed_compute(pts, NULL, weights, 3, &sentinel, 0, &p, &out);
    CHECK(rc == 0);
    CHECK(out.n_planes == 0);
    CHECK(out.n_pts == 3);
    for (size_t i = 0; i < 3; i++) CHECK(out.owner[i] == -1);
    v7_seed_outputs_free(&out);
}

static void test_degenerate_segment_skipped(void) {
    // Spec: a segment with < 3 points cannot LSQ-fit a plane; it must be
    // dropped, leaving an empty output but a sane owner array (all -1).
    double pts[6] = {0, 0, 0,  1, 0, 0};
    uint32_t idx[2] = {0, 1};
    float weights[2] = { 1.0f, 1.0f };
    v7_seed_input_segment_t seg = { .source_segment_id = 0, .pt_indices = idx, .n_pts = 2 };
    v7_seed_params_t p; v7_seed_params_default(&p);
    v7_seed_outputs_t out; v7_seed_outputs_init(&out);
    int rc = v7_seed_compute(pts, NULL, weights, 2, &seg, 1, &p, &out);
    CHECK(rc == 0);
    CHECK(out.n_planes == 0);
    CHECK(out.owner[0] == -1 && out.owner[1] == -1);
    v7_seed_outputs_free(&out);
}

static void test_single_flat_segment_emits_one_horizontal_plane(void) {
    // Spec: a 5×5 grid on y = 5 (perfectly flat, y-up) yields exactly
    // 1 seed plane with normal ≈ (0, ±1, 0), tilt ≈ 0°, and every input
    // point in the inlier set (no outliers to reject).
    enum { N = 25 };
    double pts[N * 3];
    size_t n = build_grid_plane_yup(pts, 5, 5, 0.5, 0.5, 0.0, 0.0, 5.0);
    CHECK(n == N);
    uint32_t idx[N]; fill_identity_indices(idx, N);
    float weights[N]; fill_uniform_weights(weights, N);
    v7_seed_input_segment_t seg = { .source_segment_id = 7, .pt_indices = idx, .n_pts = N };

    v7_seed_params_t p; v7_seed_params_default(&p);
    p.max_seed_curvature = 0.0;        // disable curvature gate (no curvatures provided)
    v7_seed_outputs_t out; v7_seed_outputs_init(&out);
    int rc = v7_seed_compute(pts, NULL, weights, N, &seg, 1, &p, &out);
    CHECK(rc == 0);
    CHECK(out.n_planes == 1);
    if (out.n_planes == 1) {
        CHECK_CLOSE(fabs(out.planes[0].hdr.normal[1]), 1.0, 1e-6);
        CHECK_CLOSE(out.planes[0].hdr.normal[0], 0.0, 1e-6);
        CHECK_CLOSE(out.planes[0].hdr.normal[2], 0.0, 1e-6);
        CHECK_CLOSE(out.planes[0].hdr.tilt, 0.0, 1e-4);
        CHECK(out.planes[0].hdr.inlier_count == (uint32_t)N);
        CHECK(out.planes[0].hdr.source_segment_id == 7);   // traceability preserved
        // d = -n·centroid = -(0)(*)-(1)(5)-(0)(*) = -5  (or +5 with negative normal,
        // which v2 forbids since normal is flipped "up").
        CHECK_CLOSE(out.planes[0].hdr.d, -5.0, 1e-6);
    }
    v7_seed_outputs_free(&out);
}

static void test_owner_array_assigns_inliers_only(void) {
    // Spec: out->owner[gi] = plane_index for every gi in seed_indices of
    // a surviving plane; -1 otherwise. With a single segment covering pts
    // [0..N), the owner array should be exactly { 0, 0, …, 0 }.
    enum { N = 16 };
    double pts[N * 3];
    build_grid_plane_yup(pts, 4, 4, 0.5, 0.5, 0.0, 0.0, 3.0);
    uint32_t idx[N]; fill_identity_indices(idx, N);
    float weights[N]; fill_uniform_weights(weights, N);
    v7_seed_input_segment_t seg = { .source_segment_id = 0, .pt_indices = idx, .n_pts = N };

    v7_seed_params_t p; v7_seed_params_default(&p);
    p.max_seed_curvature = 0.0;
    v7_seed_outputs_t out; v7_seed_outputs_init(&out);
    v7_seed_compute(pts, NULL, weights, N, &seg, 1, &p, &out);
    CHECK(out.n_planes == 1);
    for (size_t i = 0; i < N; i++) CHECK(out.owner[i] == 0);
    v7_seed_outputs_free(&out);
}

static void test_outlier_rejection_drops_far_point(void) {
    // Spec: points with |signed_distance_to_plane| > dist_threshold are
    // dropped from the inlier set. Layout: 25 flat points at y=5 plus
    // one wild outlier at y=5+10. Default threshold is 0.09 m; the
    // outlier sits 10 m off the plane, must be dropped.
    enum { FLAT = 25, N = FLAT + 1 };
    double pts[N * 3];
    build_grid_plane_yup(pts, 5, 5, 0.5, 0.5, 0.0, 0.0, 5.0);
    // Outlier well above the threshold (0.09 m) but small enough not to
    // wreck the initial LSQ fit's normal direction (otherwise the fit
    // tilts and the "good" flat points start getting rejected too).
    pts[FLAT * 3 + 0] = 1.0;
    pts[FLAT * 3 + 1] = 5.5;
    pts[FLAT * 3 + 2] = 1.0;
    uint32_t idx[N]; fill_identity_indices(idx, N);
    float weights[N]; fill_uniform_weights(weights, N);
    v7_seed_input_segment_t seg = { .source_segment_id = 0, .pt_indices = idx, .n_pts = N };

    v7_seed_params_t p; v7_seed_params_default(&p);
    p.max_seed_curvature = 0.0;
    v7_seed_outputs_t out; v7_seed_outputs_init(&out);
    v7_seed_compute(pts, NULL, weights, N, &seg, 1, &p, &out);
    CHECK(out.n_planes == 1);
    if (out.n_planes == 1) {
        CHECK(out.planes[0].hdr.inlier_count == FLAT);
        // Outlier sits at gi=FLAT → must be unassigned in owner.
        CHECK(out.owner[FLAT] == -1);
        // Every flat point owned by plane 0.
        for (uint32_t i = 0; i < FLAT; i++) CHECK(out.owner[i] == 0);
    }
    v7_seed_outputs_free(&out);
}

static void test_segments_sorted_by_area_desc(void) {
    // Spec: segments are processed in descending order of sum-of-weights
    // (== n_pts when no point_weights). The largest segment must be
    // emitted as plane 0.
    //
    // Layout: TWO non-overlapping flat segments. Small (3×3=9 pts) at
    // y=2 covering indices [0..8]; large (4×4=16 pts) at y=5 covering
    // indices [9..24]. We input small FIRST so order ≠ area; the output
    // must still place the larger one at index 0.
    enum { SMALL = 9, LARGE = 16, N = SMALL + LARGE };
    double pts[N * 3];
    // Small at y=2, in the X<0 quadrant so coordinates don't overlap.
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) {
        const size_t k = (size_t)(i * 3 + j);
        pts[k * 3 + 0] = -10.0 - i;        // X far away from the other segment
        pts[k * 3 + 1] = 2.0;
        pts[k * 3 + 2] = (double)j;
    }
    // Large at y=5 in the X>0 quadrant.
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) {
        const size_t k = SMALL + (size_t)(i * 4 + j);
        pts[k * 3 + 0] = (double)i;
        pts[k * 3 + 1] = 5.0;
        pts[k * 3 + 2] = (double)j;
    }
    uint32_t small_idx[SMALL], large_idx[LARGE];
    for (uint32_t i = 0; i < SMALL; i++) small_idx[i] = i;
    for (uint32_t i = 0; i < LARGE; i++) large_idx[i] = SMALL + i;
    float weights[N]; fill_uniform_weights(weights, N);

    v7_seed_input_segment_t segs[2] = {
        { .source_segment_id = 11, .pt_indices = small_idx, .n_pts = SMALL },
        { .source_segment_id = 22, .pt_indices = large_idx, .n_pts = LARGE },
    };
    v7_seed_params_t p; v7_seed_params_default(&p);
    p.max_seed_curvature = 0.0;
    v7_seed_outputs_t out; v7_seed_outputs_init(&out);
    v7_seed_compute(pts, NULL, weights, N, segs, 2, &p, &out);
    CHECK(out.n_planes == 2);
    if (out.n_planes == 2) {
        // Plane 0 == the larger one (segment id 22).
        CHECK(out.planes[0].hdr.source_segment_id == 22);
        CHECK(out.planes[0].hdr.inlier_count == LARGE);
        // Plane 1 == the smaller one (segment id 11).
        CHECK(out.planes[1].hdr.source_segment_id == 11);
        CHECK(out.planes[1].hdr.inlier_count == SMALL);
        // Owner consistency: the large segment's pts get owner 0, small gets 1.
        for (uint32_t i = 0; i < SMALL; i++) CHECK(out.owner[i] == 1);
        for (uint32_t i = 0; i < LARGE; i++) CHECK(out.owner[SMALL + i] == 0);
    }
    v7_seed_outputs_free(&out);
}

static void test_equal_area_tiebreaker_smaller_id_first(void) {
    // Spec: when two segments have identical sort weight, the tiebreaker
    // is source_segment_id ASC (seg_sort_cmp in the impl). Setup:
    // two 4×4 flat segments at different y-heights, identical sizes.
    enum { K = 16, N = K + K };
    double pts[N * 3];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) {
        const size_t a = (size_t)(i * 4 + j);
        pts[a * 3 + 0] = (double)i;
        pts[a * 3 + 1] = 1.0;
        pts[a * 3 + 2] = (double)j;
    }
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) {
        const size_t b = K + (size_t)(i * 4 + j);
        pts[b * 3 + 0] = 10.0 + i;
        pts[b * 3 + 1] = 5.0;
        pts[b * 3 + 2] = (double)j;
    }
    uint32_t aidx[K], bidx[K];
    for (uint32_t i = 0; i < K; i++) { aidx[i] = i; bidx[i] = K + i; }
    float weights[N]; fill_uniform_weights(weights, N);

    // Input order intentionally reversed (id=99 first, id=3 second). The
    // sort must reorder by source_segment_id ASC since weights are equal.
    v7_seed_input_segment_t segs[2] = {
        { .source_segment_id = 99, .pt_indices = aidx, .n_pts = K },
        { .source_segment_id =  3, .pt_indices = bidx, .n_pts = K },
    };
    v7_seed_params_t p; v7_seed_params_default(&p);
    p.max_seed_curvature = 0.0;
    v7_seed_outputs_t out; v7_seed_outputs_init(&out);
    v7_seed_compute(pts, NULL, weights, N, segs, 2, &p, &out);
    CHECK(out.n_planes == 2);
    if (out.n_planes == 2) {
        // Smaller id wins the tie → emitted first.
        CHECK(out.planes[0].hdr.source_segment_id == 3);
        CHECK(out.planes[1].hdr.source_segment_id == 99);
    }
    v7_seed_outputs_free(&out);
}

static void test_curvature_gate_skips_high_curv_non_wall(void) {
    // Spec: for segments with tilt < 70° (non-wall) and weighted-mean
    // curvature > max_seed_curvature, the segment is skipped (treated
    // as a ridge/valley transition, downstream GRM absorbs it).
    //
    // Setup: 5×5 grid at y=5 (tilt = 0°, definitely non-wall), with all
    // curvatures pinned at 0.1 (>> default threshold 0.03). Result must
    // be 0 planes emitted.
    enum { N = 25 };
    double pts[N * 3];
    build_grid_plane_yup(pts, 5, 5, 0.5, 0.5, 0.0, 0.0, 5.0);
    uint32_t idx[N]; fill_identity_indices(idx, N);
    float weights[N]; fill_uniform_weights(weights, N);
    v7_seed_input_segment_t seg = { .source_segment_id = 0, .pt_indices = idx, .n_pts = N };

    float curv[N];
    for (size_t i = 0; i < N; i++) curv[i] = 0.10f;   // 0.10 > 0.03 = default threshold

    v7_seed_params_t p; v7_seed_params_default(&p);
    CHECK(p.max_seed_curvature > 0.0);                 // sanity: gate is enabled by default
    v7_seed_outputs_t out; v7_seed_outputs_init(&out);
    int rc = v7_seed_compute(pts, curv, weights, N, &seg, 1, &p, &out);
    CHECK(rc == 0);
    CHECK(out.n_planes == 0);
    for (size_t i = 0; i < N; i++) CHECK(out.owner[i] == -1);
    v7_seed_outputs_free(&out);
}

static void test_curvature_gate_exempts_walls(void) {
    // Spec: walls (tilt > wall_tilt_deg = 70°) are exempt from the
    // curvature gate, since high curvature on grazing LiDAR is a normal
    // measurement artifact, not a ridge.
    //
    // Setup: vertical 5×5 grid in the XZ plane at x=0, parallel to the
    // y axis (i.e. y varies, x and z form the surface). The normal is
    // (±1, 0, 0) → tilt = 90°. Set curvatures high; we expect a plane
    // to be emitted regardless.
    enum { N = 25 };
    double pts[N * 3];
    for (int i = 0; i < 5; i++) for (int j = 0; j < 5; j++) {
        const size_t k = (size_t)(i * 5 + j);
        pts[k * 3 + 0] = 0.0;                  // x = 0 → vertical wall
        pts[k * 3 + 1] = (double)i * 0.5;      // y varies
        pts[k * 3 + 2] = (double)j * 0.5;      // z varies
    }
    uint32_t idx[N]; fill_identity_indices(idx, N);
    float weights[N]; fill_uniform_weights(weights, N);
    v7_seed_input_segment_t seg = { .source_segment_id = 5, .pt_indices = idx, .n_pts = N };

    float curv[N];
    for (size_t i = 0; i < N; i++) curv[i] = 0.5f;     // grossly above threshold

    v7_seed_params_t p; v7_seed_params_default(&p);
    v7_seed_outputs_t out; v7_seed_outputs_init(&out);
    v7_seed_compute(pts, curv, weights, N, &seg, 1, &p, &out);
    CHECK(out.n_planes == 1);
    if (out.n_planes == 1) {
        // Tilt ≈ 90° — confirms gate's wall-exemption branch was taken.
        CHECK(out.planes[0].hdr.tilt > 85.0);
        CHECK_CLOSE(fabs(out.planes[0].hdr.normal[0]), 1.0, 1e-6);
    }
    v7_seed_outputs_free(&out);
}

static void test_z_up_axis_orients_normal_along_z(void) {
    // Spec: with up_axis = 2 (z-up, LAZ convention), the seed plane's
    // normal is flipped so that normal[2] >= 0. Setup: flat z=5 patch.
    enum { N = 25 };
    double pts[N * 3];
    for (int i = 0; i < 5; i++) for (int j = 0; j < 5; j++) {
        const size_t k = (size_t)(i * 5 + j);
        pts[k * 3 + 0] = (double)i * 0.5;
        pts[k * 3 + 1] = (double)j * 0.5;
        pts[k * 3 + 2] = 5.0;
    }
    uint32_t idx[N]; fill_identity_indices(idx, N);
    float weights[N]; fill_uniform_weights(weights, N);
    v7_seed_input_segment_t seg = { .source_segment_id = 0, .pt_indices = idx, .n_pts = N };

    v7_seed_params_t p; v7_seed_params_default(&p);
    p.up_axis = 2;                       // z-up
    p.max_seed_curvature = 0.0;
    v7_seed_outputs_t out; v7_seed_outputs_init(&out);
    v7_seed_compute(pts, NULL, weights, N, &seg, 1, &p, &out);
    CHECK(out.n_planes == 1);
    if (out.n_planes == 1) {
        CHECK(out.planes[0].hdr.normal[2] >= 0.0);
        CHECK_CLOSE(fabs(out.planes[0].hdr.normal[2]), 1.0, 1e-6);
        CHECK_CLOSE(out.planes[0].hdr.tilt, 0.0, 1e-4);
    }
    v7_seed_outputs_free(&out);
}

static void test_point_weights_change_sort_key(void) {
    // Spec: when point-weights is provided, the area-sort key becomes
    // sum-of-weights instead of raw n_pts. A weighted-up small segment
    // can rank ahead of a count-wise larger one.
    //
    // Layout: 9-pt segment (heavy weights w=10), 16-pt segment (w=1).
    // Sum weights: 9*10 = 90 vs 16*1 = 16. The "small" segment wins.
    enum { SMALL = 9, LARGE = 16, N = SMALL + LARGE };
    double pts[N * 3];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) {
        const size_t k = (size_t)(i * 3 + j);
        pts[k * 3 + 0] = (double)i;
        pts[k * 3 + 1] = 2.0;
        pts[k * 3 + 2] = (double)j;
    }
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) {
        const size_t k = SMALL + (size_t)(i * 4 + j);
        pts[k * 3 + 0] = 10.0 + i;
        pts[k * 3 + 1] = 5.0;
        pts[k * 3 + 2] = (double)j;
    }
    uint32_t small_idx[SMALL], large_idx[LARGE];
    for (uint32_t i = 0; i < SMALL; i++) small_idx[i] = i;
    for (uint32_t i = 0; i < LARGE; i++) large_idx[i] = SMALL + i;

    float weights[N];
    for (uint32_t i = 0; i < SMALL; i++) weights[i] = 10.0f;
    for (uint32_t i = 0; i < LARGE; i++) weights[SMALL + i] = 1.0f;

    v7_seed_input_segment_t segs[2] = {
        { .source_segment_id =  1, .pt_indices = small_idx, .n_pts = SMALL },
        { .source_segment_id =  2, .pt_indices = large_idx, .n_pts = LARGE },
    };
    v7_seed_params_t p; v7_seed_params_default(&p);
    p.max_seed_curvature = 0.0;
    v7_seed_outputs_t out; v7_seed_outputs_init(&out);
    v7_seed_compute(pts, NULL, weights, N, segs, 2, &p, &out);
    CHECK(out.n_planes == 2);
    if (out.n_planes == 2) {
        // The 9-pt segment, weighted 10× per point, wins the sort.
        CHECK(out.planes[0].hdr.source_segment_id == 1);
        CHECK(out.planes[0].hdr.inlier_count == SMALL);
        CHECK(out.planes[1].hdr.source_segment_id == 2);
    }
    v7_seed_outputs_free(&out);
}

static void test_tilt_recovered_for_sloped_plane(void) {
    // Spec: tilt = acos(|n[up_axis]|) in degrees. A 30° pitched plane
    // (y = x·tan(30°) + 5) has a normal at 30° off vertical → tilt ≈ 30°.
    // Also: the normal is in the XY plane (nz ≈ 0). The d field equals
    // -n·centroid by construction.
    enum { N = 25 };
    double pts[N * 3];
    const double tan30 = tan(30.0 * M_PI / 180.0);
    build_grid_plane_yup(pts, 5, 5, 0.5, 0.5, tan30, 0.0, 5.0);
    uint32_t idx[N]; fill_identity_indices(idx, N);
    float weights[N]; fill_uniform_weights(weights, N);
    v7_seed_input_segment_t seg = { .source_segment_id = 0, .pt_indices = idx, .n_pts = N };

    v7_seed_params_t p; v7_seed_params_default(&p);
    p.max_seed_curvature = 0.0;
    v7_seed_outputs_t out; v7_seed_outputs_init(&out);
    v7_seed_compute(pts, NULL, weights, N, &seg, 1, &p, &out);
    CHECK(out.n_planes == 1);
    if (out.n_planes == 1) {
        CHECK_CLOSE(out.planes[0].hdr.tilt, 30.0, 0.5);     // bench-v2-grade tolerance
        CHECK_CLOSE(out.planes[0].hdr.normal[2], 0.0, 1e-6);
        CHECK(out.planes[0].hdr.normal[1] > 0.0);            // oriented up (y-up)
    }
    v7_seed_outputs_free(&out);
}

// ─── main ───────────────────────────────────────────────────────────

int main(void) {
    test_zero_segments_emit_no_planes();
    test_degenerate_segment_skipped();
    test_single_flat_segment_emits_one_horizontal_plane();
    test_owner_array_assigns_inliers_only();
    test_outlier_rejection_drops_far_point();
    test_segments_sorted_by_area_desc();
    test_equal_area_tiebreaker_smaller_id_first();
    test_curvature_gate_skips_high_curv_non_wall();
    test_curvature_gate_exempts_walls();
    test_z_up_axis_orients_normal_along_z();
    test_point_weights_change_sort_key();
    test_tilt_recovered_for_sloped_plane();

    printf("%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}

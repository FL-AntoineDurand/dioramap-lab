// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// core/tests/v7_grow_refit_merge_test.c
//
// Implementation tests for v7-grow-refit-merge. Each test asserts an
// invariant the algorithm spec claims to hold. Inputs are hand-crafted,
// minimal, deterministic. Tolerances are tight (binary pass/fail).
//
// Spec source: bench-v2 lib/pipeline/steps/grow-refit-merge.js
// (meta.version=8/9; v3's implementation is a direct port).
//
// bench-v2 has no single grow-refit-merge.test.js — the criteria the
// algorithm relies on are exercised indirectly via
// test/plane-extractor.test.js (single-flat-plane → 1 plane,
// gable → 2 planes, opposite azimuths), test/shape-fitting.test.js,
// and test/pca.test.js (the LSQ plane-fit primitive). Most of the
// invariants encoded here are stated in the algorithm header and the
// V7-TEST-STRATEGY.md sketch row for this step.
//
// Implementation reference: v3 steps/v7/v7-grow-refit-merge-impl.c.
//
// We include the v7-grow-refit-merge-impl.c source directly so the
// test stays a single TU. v7-plane-fit.c is pulled in the same way
// (the test executable only links dioramap_steps_normals, which
// provides the kd-tree + PCA primitives but NOT the v7-plane-fit
// symbols — those live in dioramap_steps_v7 which the test does not
// link to avoid duplicate-symbol errors on the impl).

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE   700        // so <math.h> exposes M_PI

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Pull the impls into this TU. v7-plane-fit.c first — the grm impl
// calls v7_plane_fit_lsq / v7_azimuth_from_normal.
#include "v7-plane-fit.c"
#include "v7-grow-refit-merge-impl.c"

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
// Writes into pts (interleaved F64×3) and returns count written.
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

static void fill_identity_indices_u32(uint32_t* idx, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) idx[i] = i;
}

// Fill an F32 array with 1.0. v7_grm_compute now REQUIRES non-NULL
// point_weights (see v7-plane-fit.h); tests pass an explicit uniform-1.0
// array so the "unweighted-equivalent" intent is visible at the call
// site (no silent NULL fallback).
static void fill_uniform_weights(float* w, size_t n) {
    for (size_t i = 0; i < n; i++) w[i] = 1.0f;
}

// Per-test params helper: defaults, but with the search-grow scale + density
// chosen so a 0.5 m grid spacing always finds its 4 immediate neighbours.
// density=25 pts/m² gives radius = 3·sqrt(1)/sqrt(25) = 0.6 m, capped at 1.0.
static void params_for_tests(v7_grm_params_t* p) {
    v7_grm_params_default(p);
    p->density             = 25.0;
    p->search_radius_scale = 3.0;
    p->search_radius_max   = 1.0;
    p->dist_threshold      = 0.12;
    p->max_iterations      = 5;
}

// ─── tests ──────────────────────────────────────────────────────────

// "Empty input": n_pool == 0 (no building pts) returns 0 planes, no
// crash. Spec invariant: early-exit path allocates owner/ridge/residual
// arrays sized to n_pts and fills with sentinels (-1, 0, +Inf).
//
// pool_indices is required non-NULL by the arg-validation gate; we pass
// a valid pointer to a length-0 logical pool (impl reads only the first
// n_pool entries, which is zero here).
static void test_empty_input_handled(void) {
    double pts[9] = {0, 0, 0,  1, 0, 0,  0, 0, 1};
    kd_tree_t tree; memset(&tree, 0, sizeof(tree));
    CHECK(kd_build(&tree, pts, 3) == 0);

    uint32_t empty_pool[1] = { 0 };           // valid ptr, n_pool=0 → no reads
    float weights[3] = { 1.0f, 1.0f, 1.0f };

    v7_grm_params_t p; params_for_tests(&p);
    v7_grm_outputs_t out; v7_grm_outputs_init(&out);
    int rc = v7_grm_compute(pts, weights, NULL, 3,
                            empty_pool, 0,      // n_pool = 0 → early exit
                            &tree, NULL, 0,     // K = 0
                            &p, &out);
    CHECK(rc == 0);
    CHECK(out.n_pts == 3);
    CHECK(out.n_planes == 0);
    CHECK(out.owner != NULL);
    CHECK(out.residual != NULL);
    CHECK(out.ridge_mask != NULL);
    for (int i = 0; i < 3; i++) {
        CHECK(out.owner[i] == -1);
        // INFINITY sentinel: residual init pattern documented in
        // docs/quality/FRAMA-C-FALSE-POSITIVES.md (silenced via
        // -warn-special-float nan; this is the load-bearing read).
        CHECK(isinf(out.residual[i]) && out.residual[i] > 0);
        CHECK(out.ridge_mask[i] == 0);
    }
    CHECK_CLOSE(out.distance_threshold, p.dist_threshold, 1e-12);

    v7_grm_outputs_free(&out);
    kd_free(&tree);
}

// "Seed-only": valid n_pool but K=0 (no seeds) → no planes; owner all -1.
// Spec invariant: early-return when there's nothing to grow from.
static void test_seed_only_no_seeds_no_regions(void) {
    enum { N = 9 };
    double pts[N * 3];
    build_grid_plane_yup(pts, 3, 3, 0.5, 0.5, 0.0, 0.0, 5.0);
    uint32_t pool[N]; fill_identity_indices_u32(pool, N);
    float weights[N]; fill_uniform_weights(weights, N);

    kd_tree_t tree; memset(&tree, 0, sizeof(tree));
    CHECK(kd_build(&tree, pts, N) == 0);

    v7_grm_params_t p; params_for_tests(&p);
    v7_grm_outputs_t out; v7_grm_outputs_init(&out);
    int rc = v7_grm_compute(pts, weights, NULL, N,
                            pool, N,
                            &tree, NULL, 0,        // K = 0 → 0 regions
                            &p, &out);
    CHECK(rc == 0);
    CHECK(out.n_planes == 0);
    for (int i = 0; i < N; i++) {
        CHECK(out.owner[i] == -1);
        CHECK(isinf(out.residual[i]) && out.residual[i] > 0);
    }
    v7_grm_outputs_free(&out);
    kd_free(&tree);
}

// "100 coplanar points + 1 seed → 1 region, normal & d recovered".
// Spec invariant: GRM on a single clean plane converges to that plane's
// equation. The seed indices cover only a fraction of the pool — grow
// must claim the rest within distThreshold.
static void test_single_plane_recovered_from_partial_seed(void) {
    // 10×10 grid (100 pts) on y = 5, spacing 0.5 m. With density=25 and
    // radius_scale=3, growth radius = 0.6 m which reaches all 4 grid
    // neighbours each iteration; the wavefront fully fills in within
    // max_iterations=5.
    enum { N = 100 };
    double pts[N * 3];
    size_t got = build_grid_plane_yup(pts, 10, 10, 0.5, 0.5, 0.0, 0.0, 5.0);
    CHECK(got == N);
    uint32_t pool[N]; fill_identity_indices_u32(pool, N);
    float weights[N]; fill_uniform_weights(weights, N);

    kd_tree_t tree; memset(&tree, 0, sizeof(tree));
    CHECK(kd_build(&tree, pts, N) == 0);

    // Seed = central 3×3 cluster (indices 33,34,35,43,44,45,53,54,55).
    uint32_t seed_idx[9];
    int k = 0;
    for (int i = 3; i < 6; i++) for (int j = 3; j < 6; j++) seed_idx[k++] = (uint32_t)(i * 10 + j);

    v7_grm_input_seed_t seed = {
        .normal = { 0.0, 1.0, 0.0 },
        .d      = -5.0,
        .seed_indices = seed_idx,
        .n_seeds = 9,
        .is_ghost = false,
        .ghost_void_id = 0,
        .seed_tilt = 0.0,
    };

    v7_grm_params_t p; params_for_tests(&p);
    // Use more iterations so the diffusion-limited wavefront fully fills
    // the 10×10 grid from the central 3×3 seed (radius 0.6m only catches
    // NSEW neighbours, so reaching every corner takes ~7 iters from the
    // center).
    p.max_iterations = 15;
    v7_grm_outputs_t out; v7_grm_outputs_init(&out);
    int rc = v7_grm_compute(pts, weights, NULL, N,
                            pool, N,
                            &tree, &seed, 1,
                            &p, &out);
    CHECK(rc == 0);
    CHECK(out.n_planes == 1);
    if (out.n_planes == 1) {
        const v7_grm_plane_t* pl = &out.planes[0];
        // Plane equation recovered within tight tolerance:
        CHECK_CLOSE(fabs(pl->hdr.normal[1]), 1.0, 1e-6);
        CHECK_CLOSE(pl->hdr.normal[0], 0.0, 1e-6);
        CHECK_CLOSE(pl->hdr.normal[2], 0.0, 1e-6);
        CHECK_CLOSE(pl->hdr.d, -5.0, 1e-6);
        // Tilt ≈ 0° for a y-aligned normal.
        CHECK_CLOSE(pl->hdr.tilt, 0.0, 1e-4);
        // All 100 points should end up claimed once the wavefront fills.
        CHECK(pl->hdr.inlier_count == (uint32_t)N);
        CHECK(pl->hdr.tombstoned == 0);
    }

    v7_grm_outputs_free(&out);
    kd_free(&tree);
}

// "Owner array partitions input": each point belongs to at most one region,
// and assigned points have owner in [0, n_planes). Spec invariant for the
// Int16 owner artifact.
static void test_owner_partition(void) {
    enum { N = 100 };
    double pts[N * 3];
    build_grid_plane_yup(pts, 10, 10, 0.5, 0.5, 0.0, 0.0, 5.0);
    uint32_t pool[N]; fill_identity_indices_u32(pool, N);
    float weights[N]; fill_uniform_weights(weights, N);

    kd_tree_t tree; memset(&tree, 0, sizeof(tree));
    CHECK(kd_build(&tree, pts, N) == 0);

    uint32_t seed_idx[9];
    int k = 0;
    for (int i = 3; i < 6; i++) for (int j = 3; j < 6; j++) seed_idx[k++] = (uint32_t)(i * 10 + j);
    v7_grm_input_seed_t seed = {
        .normal = { 0.0, 1.0, 0.0 },
        .d      = -5.0,
        .seed_indices = seed_idx,
        .n_seeds = 9,
        .is_ghost = false,
    };

    v7_grm_params_t p; params_for_tests(&p);
    v7_grm_outputs_t out; v7_grm_outputs_init(&out);
    v7_grm_compute(pts, weights, NULL, N, pool, N, &tree, &seed, 1, &p, &out);

    for (int i = 0; i < N; i++) {
        int16_t o = out.owner[i];
        CHECK(o == -1 || (o >= 0 && o < (int16_t)out.n_planes));
    }

    v7_grm_outputs_free(&out);
    kd_free(&tree);
}

// "Residual bound for assigned points": each assigned point's residual must
// be ≤ dist_threshold (the gate enforced by grow + min-residual resolve).
// Spec invariant tied to params.dist_threshold.
static void test_assigned_residuals_within_threshold(void) {
    enum { N = 100 };
    double pts[N * 3];
    build_grid_plane_yup(pts, 10, 10, 0.5, 0.5, 0.0, 0.0, 5.0);
    uint32_t pool[N]; fill_identity_indices_u32(pool, N);
    float weights[N]; fill_uniform_weights(weights, N);

    kd_tree_t tree; memset(&tree, 0, sizeof(tree));
    CHECK(kd_build(&tree, pts, N) == 0);

    uint32_t seed_idx[9];
    int k = 0;
    for (int i = 3; i < 6; i++) for (int j = 3; j < 6; j++) seed_idx[k++] = (uint32_t)(i * 10 + j);
    v7_grm_input_seed_t seed = {
        .normal = { 0.0, 1.0, 0.0 }, .d = -5.0,
        .seed_indices = seed_idx, .n_seeds = 9,
    };

    v7_grm_params_t p; params_for_tests(&p);
    v7_grm_outputs_t out; v7_grm_outputs_init(&out);
    v7_grm_compute(pts, weights, NULL, N, pool, N, &tree, &seed, 1, &p, &out);

    for (int i = 0; i < N; i++) {
        if (out.owner[i] >= 0) {
            CHECK(out.residual[i] <= (float)p.dist_threshold + 1e-6f);
        } else {
            // Unassigned MUST hold the INFINITY sentinel.
            CHECK(isinf(out.residual[i]) && out.residual[i] > 0);
        }
    }

    v7_grm_outputs_free(&out);
    kd_free(&tree);
}

// "Two distinct planes, two seeds → 2 regions with the right normals".
// Spec invariant: GRM keeps non-coplanar regions apart (no spurious merge).
static void test_two_planes_two_seeds(void) {
    // Plane A: 10×10 flat at y=5 covering x∈[0,4.5], z∈[0,4.5] (indices 0..99).
    // Plane B: 10×10 flat at y=10 covering x∈[10,14.5], z∈[0,4.5] (indices 100..199).
    // Different normals would be more discriminating, but parallel-different-d
    // is the harder case (tests both Stage-2 offset gating + the merge
    // anti-merge: offset 5m >> merge_offset_max=0.24m).
    enum { NSIDE = 100, N = NSIDE * 2 };
    double pts[N * 3];
    build_grid_plane_yup(pts, 10, 10, 0.5, 0.5, 0.0, 0.0, 5.0);
    double* B = pts + NSIDE * 3;
    for (int i = 0; i < 10; i++) for (int j = 0; j < 10; j++) {
        const size_t kk = (size_t)(i * 10 + j);
        B[kk * 3 + 0] = 10.0 + i * 0.5;
        B[kk * 3 + 1] = 10.0;
        B[kk * 3 + 2] = j * 0.5;
    }
    uint32_t pool[N]; fill_identity_indices_u32(pool, N);
    float weights[N]; fill_uniform_weights(weights, N);

    kd_tree_t tree; memset(&tree, 0, sizeof(tree));
    CHECK(kd_build(&tree, pts, N) == 0);

    uint32_t seedA[9], seedB[9];
    int k = 0;
    for (int i = 3; i < 6; i++) for (int j = 3; j < 6; j++) {
        seedA[k] = (uint32_t)(i * 10 + j);
        seedB[k] = (uint32_t)(NSIDE + i * 10 + j);
        k++;
    }
    v7_grm_input_seed_t seeds[2] = {
        { .normal = { 0, 1, 0 }, .d = -5.0,  .seed_indices = seedA, .n_seeds = 9 },
        { .normal = { 0, 1, 0 }, .d = -10.0, .seed_indices = seedB, .n_seeds = 9 },
    };

    v7_grm_params_t p; params_for_tests(&p);
    v7_grm_outputs_t out; v7_grm_outputs_init(&out);
    v7_grm_compute(pts, weights, NULL, N, pool, N, &tree, seeds, 2, &p, &out);

    // Expect 2 active (non-tombstoned) planes.
    size_t alive = 0;
    for (size_t i = 0; i < out.n_planes; i++) if (!out.planes[i].hdr.tombstoned) alive++;
    CHECK(alive == 2);

    // Each plane carries its seed's d (≈ -5 or -10) within ε.
    int found_d5 = 0, found_d10 = 0;
    for (size_t i = 0; i < out.n_planes; i++) {
        const v7_grm_plane_t* pl = &out.planes[i];
        if (pl->hdr.tombstoned) continue;
        CHECK_CLOSE(fabs(pl->hdr.normal[1]), 1.0, 1e-6);
        if (fabs(pl->hdr.d - (-5.0)) < 1e-3) found_d5 = 1;
        if (fabs(pl->hdr.d - (-10.0)) < 1e-3) found_d10 = 1;
    }
    CHECK(found_d5 == 1);
    CHECK(found_d10 == 1);

    // Plane A's points should own Plane A (and not B's), and vice-versa.
    // We don't know which slot is A/B, but cross-counts must be 0.
    int16_t a_owner = -2, b_owner = -2;
    for (int i = 0; i < NSIDE; i++) {
        if (out.owner[i] >= 0) { a_owner = out.owner[i]; break; }
    }
    for (int i = NSIDE; i < N; i++) {
        if (out.owner[i] >= 0) { b_owner = out.owner[i]; break; }
    }
    CHECK(a_owner >= 0 && b_owner >= 0 && a_owner != b_owner);
    int wrong = 0;
    for (int i = 0; i < NSIDE; i++) if (out.owner[i] == b_owner) wrong++;
    for (int i = NSIDE; i < N; i++) if (out.owner[i] == a_owner) wrong++;
    CHECK(wrong == 0);

    v7_grm_outputs_free(&out);
    kd_free(&tree);
}

// "Coplanar seeds merge into 1 region". Two seeds sharing the same plane
// (same normal, same d, overlapping point pool) must merge per the
// angle + offset + connexity + RMS gate.
static void test_coplanar_seeds_merge(void) {
    // 10×10 grid at y=5. Two seeds, each 3×3, in the SAME plane but at
    // different corners. Their grown regions must touch (the grid is
    // contiguous) and the merge stage must unify them.
    enum { N = 100 };
    double pts[N * 3];
    build_grid_plane_yup(pts, 10, 10, 0.5, 0.5, 0.0, 0.0, 5.0);
    uint32_t pool[N]; fill_identity_indices_u32(pool, N);
    float weights[N]; fill_uniform_weights(weights, N);

    kd_tree_t tree; memset(&tree, 0, sizeof(tree));
    CHECK(kd_build(&tree, pts, N) == 0);

    uint32_t seedA[9], seedB[9];
    int k = 0;
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) {
        seedA[k] = (uint32_t)(i * 10 + j);                 // top-left 3×3
        seedB[k] = (uint32_t)((i + 7) * 10 + (j + 7));     // bottom-right 3×3
        k++;
    }
    v7_grm_input_seed_t seeds[2] = {
        { .normal = { 0, 1, 0 }, .d = -5.0, .seed_indices = seedA, .n_seeds = 9 },
        { .normal = { 0, 1, 0 }, .d = -5.0, .seed_indices = seedB, .n_seeds = 9 },
    };

    v7_grm_params_t p; params_for_tests(&p);
    v7_grm_outputs_t out; v7_grm_outputs_init(&out);
    v7_grm_compute(pts, weights, NULL, N, pool, N, &tree, seeds, 2, &p, &out);

    // Plane slots still 2 (K is preserved), but only ONE should remain
    // alive — the other must be tombstoned by the merge stage.
    size_t alive = 0, tombstoned = 0;
    for (size_t i = 0; i < out.n_planes; i++) {
        if (out.planes[i].hdr.tombstoned) tombstoned++;
        else alive++;
    }
    CHECK(alive == 1);
    CHECK(tombstoned == 1);

    // All assigned points share the surviving plane id.
    int16_t alive_id = -1;
    for (size_t i = 0; i < out.n_planes; i++) {
        if (!out.planes[i].hdr.tombstoned) { alive_id = (int16_t)i; break; }
    }
    CHECK(alive_id >= 0);
    for (int i = 0; i < N; i++) {
        if (out.owner[i] >= 0) CHECK(out.owner[i] == alive_id);
    }

    v7_grm_outputs_free(&out);
    kd_free(&tree);
}

// "distance_threshold output echoes input param" — load-bearing for
// downstream WSD trust weighting (boundary-polygons + boundary-regularize).
static void test_distance_threshold_echoed(void) {
    double pts[9] = {0, 0, 0,  1, 0, 0,  0, 0, 1};
    kd_tree_t tree; memset(&tree, 0, sizeof(tree));
    CHECK(kd_build(&tree, pts, 3) == 0);

    uint32_t empty_pool[1] = { 0 };
    float weights[3] = { 1.0f, 1.0f, 1.0f };

    v7_grm_params_t p; params_for_tests(&p);
    p.dist_threshold = 0.17;        // arbitrary, distinct from default
    v7_grm_outputs_t out; v7_grm_outputs_init(&out);
    v7_grm_compute(pts, weights, NULL, 3, empty_pool, 0, &tree, NULL, 0, &p, &out);
    CHECK_CLOSE(out.distance_threshold, 0.17, 1e-12);

    v7_grm_outputs_free(&out);
    kd_free(&tree);
}

// "Ridge mask flags points adjacent to a different owner". Spec invariant:
// the ridge-candidates artifact is the natural ridge/valley detector seed.
static void test_ridge_mask_at_plane_boundary(void) {
    // 200 points: 10×10 at y=5 covering x∈[0,4.5] (indices 0..99) +
    // 10×10 ABUTTING at x∈[5.0,9.5], y=10 (different plane). The
    // ridge_mask should fire on at least the column of plane-A points
    // at x=4.5 (their neighbours include plane-B points at x=5.0).
    //
    // Place B exactly 0.5 m away in x and 5 m up in y. With the kd-tree
    // metric being 3D, points at the boundary of A are within 1.0 m of B
    // (sqrt(0.5² + 5²) ≈ 5.025 > 1.0 → too far). Make B closer in y.
    enum { NSIDE = 100, N = NSIDE * 2 };
    double pts[N * 3];
    build_grid_plane_yup(pts, 10, 10, 0.5, 0.5, 0.0, 0.0, 5.0);
    double* B = pts + NSIDE * 3;
    // B is tilted (a small step) so it's still a *different* plane, but
    // touching A's edge: B's y = 5 at its left edge, rising to 5+4.5*tan
    // away from A. Use a 30° tilt around z (so normal differs by 30°).
    const double tan30 = tan(30.0 * M_PI / 180.0);
    for (int i = 0; i < 10; i++) for (int j = 0; j < 10; j++) {
        const size_t kk = (size_t)(i * 10 + j);
        const double bx = 5.0 + i * 0.5;     // x∈[5.0,9.5]
        const double bz = j * 0.5;
        B[kk * 3 + 0] = bx;
        B[kk * 3 + 1] = 5.0 + (bx - 5.0) * tan30;  // y starts at 5 (touches A's edge)
        B[kk * 3 + 2] = bz;
    }
    uint32_t pool[N]; fill_identity_indices_u32(pool, N);
    float weights[N]; fill_uniform_weights(weights, N);

    kd_tree_t tree; memset(&tree, 0, sizeof(tree));
    CHECK(kd_build(&tree, pts, N) == 0);

    // Seeds in the interior of each plane (avoid the boundary col so the
    // initial RMS is clean).
    uint32_t seedA[9], seedB[9];
    int k = 0;
    for (int i = 1; i < 4; i++) for (int j = 1; j < 4; j++) {
        seedA[k] = (uint32_t)(i * 10 + j);
        seedB[k] = (uint32_t)(NSIDE + i * 10 + j);
        k++;
    }
    v7_grm_input_seed_t seeds[2] = {
        { .normal = { 0, 1, 0 }, .d = -5.0, .seed_indices = seedA, .n_seeds = 9 },
        // Plane B's analytic normal: tilted 30° in xy. y = 5 + (x-5)·tan30
        // → x·tan30 - y + (5 - 5·tan30) = 0 → normal (tan30, -1, 0)
        // normalised; oriented up → ny > 0.
        { .normal = { -sin(30.0 * M_PI / 180.0), cos(30.0 * M_PI / 180.0), 0 },
          // n·p + d = 0 with p in plane: -sin30·5 + cos30·5 + d = 0 → d = sin30·5 - cos30·5
          .d = sin(30.0 * M_PI / 180.0) * 5.0 - cos(30.0 * M_PI / 180.0) * 5.0,
          .seed_indices = seedB, .n_seeds = 9 },
    };

    v7_grm_params_t p; params_for_tests(&p);
    p.max_iterations = 15;        // give the wavefront room to reach the boundary
    v7_grm_outputs_t out; v7_grm_outputs_init(&out);
    v7_grm_compute(pts, weights, NULL, N, pool, N, &tree, seeds, 2, &p, &out);

    // Some assigned points must carry ridge_mask=1 (the boundary cols).
    int n_ridge_flagged = 0;
    int n_assigned = 0;
    int seen_owner_a = 0, seen_owner_b = 0;
    for (int i = 0; i < N; i++) {
        if (out.owner[i] >= 0) {
            n_assigned++;
            if (out.ridge_mask[i] != 0) n_ridge_flagged++;
            if (i < NSIDE && out.owner[i] >= 0) seen_owner_a = 1;
            if (i >= NSIDE && out.owner[i] >= 0) seen_owner_b = 1;
        } else {
            // Unassigned points keep ridge_mask = 0 (calloc'd, never set).
            CHECK(out.ridge_mask[i] == 0);
        }
    }
    CHECK(n_assigned > 0);
    CHECK(seen_owner_a && seen_owner_b);
    CHECK(n_ridge_flagged > 0);

    v7_grm_outputs_free(&out);
    kd_free(&tree);
}

// "Plane records carry tilt + azimuth derived from normal". Spec invariant
// for the seed-planes-out / plane-equations-out artifact shape.
static void test_plane_record_tilt_and_azimuth(void) {
    // 30° pitched plane (y = x·tan30 + 5). Expect tilt ≈ 30° on the
    // surviving record.
    enum { N = 100 };
    double pts[N * 3];
    const double tan30 = tan(30.0 * M_PI / 180.0);
    build_grid_plane_yup(pts, 10, 10, 0.5, 0.5, tan30, 0.0, 5.0);
    uint32_t pool[N]; fill_identity_indices_u32(pool, N);
    float weights[N]; fill_uniform_weights(weights, N);

    kd_tree_t tree; memset(&tree, 0, sizeof(tree));
    CHECK(kd_build(&tree, pts, N) == 0);

    uint32_t seed_idx[9];
    int k = 0;
    for (int i = 3; i < 6; i++) for (int j = 3; j < 6; j++) seed_idx[k++] = (uint32_t)(i * 10 + j);
    // Build a normal consistent with the plane: y = x·tan30 + 5 ⇒
    // x·tan30 - y + 5 = 0 → normal (tan30, -1, 0) normalised, flipped up.
    const double snorm = sqrt(tan30 * tan30 + 1.0);
    v7_grm_input_seed_t seed = {
        .normal = { -tan30 / snorm, 1.0 / snorm, 0.0 },
        // (-tan30/s)·x + (1/s)·y + d = 0; at any plane point y=x·tan30+5 → x·tan30 - y + 5 = 0 → multiply by 1/s.
        .d      = -5.0 / snorm,
        .seed_indices = seed_idx,
        .n_seeds = 9,
    };

    v7_grm_params_t p; params_for_tests(&p);
    v7_grm_outputs_t out; v7_grm_outputs_init(&out);
    v7_grm_compute(pts, weights, NULL, N, pool, N, &tree, &seed, 1, &p, &out);

    CHECK(out.n_planes == 1);
    if (out.n_planes == 1) {
        const v7_grm_plane_record_t* h = &out.planes[0].hdr;
        // Tilt should be ≈ 30° (within sub-degree from final refit precision).
        CHECK_CLOSE(h->tilt, 30.0, 0.5);
        // Azimuth: for up_axis=1, atan2(nx, nz) — with nz=0, nx<0 →
        // atan2 returns -π/2 → 270° in [0,360). Just sanity-check the range.
        CHECK(h->azimuth >= 0.0 && h->azimuth < 360.0);
        // source_seed_index echoes the input seed slot (always 0 here).
        CHECK(h->source_seed_index == 0);
        CHECK(h->is_ghost == 0);
        CHECK(h->tombstoned == 0);
    }

    v7_grm_outputs_free(&out);
    kd_free(&tree);
}

// "Outlier point gets rejected from inlier set". Spec invariant: a point
// > dist_threshold from the seed plane should never be claimed (grow's
// distance gate); after final resolve, it stays unassigned with residual
// = INFINITY.
static void test_outlier_unassigned_with_inf_residual(void) {
    // 5×5 flat grid + 1 wild point well outside the plane.
    enum { FLAT = 25, N = FLAT + 1 };
    double pts[N * 3];
    build_grid_plane_yup(pts, 5, 5, 0.5, 0.5, 0.0, 0.0, 5.0);
    // Outlier: 10 m above the plane, geometrically far from any grid pt
    // too (so it never enters the radius-search frontier from the inliers).
    pts[FLAT * 3 + 0] = 100.0;
    pts[FLAT * 3 + 1] = 100.0;
    pts[FLAT * 3 + 2] = 100.0;
    uint32_t pool[N]; fill_identity_indices_u32(pool, N);
    float weights[N]; fill_uniform_weights(weights, N);

    kd_tree_t tree; memset(&tree, 0, sizeof(tree));
    CHECK(kd_build(&tree, pts, N) == 0);

    uint32_t seed_idx[9];
    int k = 0;
    for (int i = 1; i < 4; i++) for (int j = 1; j < 4; j++) seed_idx[k++] = (uint32_t)(i * 5 + j);
    v7_grm_input_seed_t seed = {
        .normal = { 0, 1, 0 }, .d = -5.0,
        .seed_indices = seed_idx, .n_seeds = 9,
    };

    v7_grm_params_t p; params_for_tests(&p);
    v7_grm_outputs_t out; v7_grm_outputs_init(&out);
    v7_grm_compute(pts, weights, NULL, N, pool, N, &tree, &seed, 1, &p, &out);

    CHECK(out.owner[FLAT] == -1);
    CHECK(isinf(out.residual[FLAT]) && out.residual[FLAT] > 0);
    // All flat points end up assigned.
    int assigned = 0;
    for (int i = 0; i < FLAT; i++) if (out.owner[i] >= 0) assigned++;
    CHECK(assigned == FLAT);

    v7_grm_outputs_free(&out);
    kd_free(&tree);
}

// "Tombstoned records remain in out->planes but have inlier_count == 0".
// Spec invariant: the slot is preserved so downstream consumers can map
// back to the input seed array by index; the tombstone flag distinguishes
// dead planes from alive ones with no inliers (which shouldn't happen).
static void test_tombstoned_records_kept_with_flag(void) {
    enum { N = 100 };
    double pts[N * 3];
    build_grid_plane_yup(pts, 10, 10, 0.5, 0.5, 0.0, 0.0, 5.0);
    uint32_t pool[N]; fill_identity_indices_u32(pool, N);
    float weights[N]; fill_uniform_weights(weights, N);

    kd_tree_t tree; memset(&tree, 0, sizeof(tree));
    CHECK(kd_build(&tree, pts, N) == 0);

    uint32_t seedA[9], seedB[9];
    int k = 0;
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) {
        seedA[k] = (uint32_t)(i * 10 + j);
        seedB[k] = (uint32_t)((i + 7) * 10 + (j + 7));
        k++;
    }
    v7_grm_input_seed_t seeds[2] = {
        { .normal = { 0, 1, 0 }, .d = -5.0, .seed_indices = seedA, .n_seeds = 9 },
        { .normal = { 0, 1, 0 }, .d = -5.0, .seed_indices = seedB, .n_seeds = 9 },
    };

    v7_grm_params_t p; params_for_tests(&p);
    v7_grm_outputs_t out; v7_grm_outputs_init(&out);
    v7_grm_compute(pts, weights, NULL, N, pool, N, &tree, seeds, 2, &p, &out);

    // K=2 → n_planes=2; one tombstoned by the coplanar merge.
    CHECK(out.n_planes == 2);
    int n_tomb = 0, n_live_assigned = 0;
    for (size_t i = 0; i < out.n_planes; i++) {
        if (out.planes[i].hdr.tombstoned) {
            CHECK(out.planes[i].hdr.inlier_count == 0);
            n_tomb++;
        } else {
            CHECK(out.planes[i].hdr.inlier_count > 0);
            n_live_assigned++;
        }
    }
    CHECK(n_tomb == 1);
    CHECK(n_live_assigned == 1);

    v7_grm_outputs_free(&out);
    kd_free(&tree);
}

// ─── main ───────────────────────────────────────────────────────────

int main(void) {
    test_empty_input_handled();
    test_seed_only_no_seeds_no_regions();
    test_single_plane_recovered_from_partial_seed();
    test_owner_partition();
    test_assigned_residuals_within_threshold();
    test_two_planes_two_seeds();
    test_coplanar_seeds_merge();
    test_distance_threshold_echoed();
    test_ridge_mask_at_plane_boundary();
    test_plane_record_tilt_and_azimuth();
    test_outlier_unassigned_with_inf_residual();
    test_tombstoned_records_kept_with_flag();

    printf("%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}

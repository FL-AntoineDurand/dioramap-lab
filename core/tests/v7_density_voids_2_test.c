// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// core/tests/v7_density_voids_2_test.c
//
// Implementation tests for v7-density-voids-2. Each test asserts a spec
// invariant of the algorithm in bench-v2 lib/pipeline/steps/density-
// voids-2.js (meta.version=3). Inputs are hand-crafted, minimal, and
// deterministic. Tolerances are tight where geometry is exact.
//
// The bench-v2 unit-test suite does not have a `density-voids-2.test.js`
// — invariants below are derived directly from the JS algorithm spec
// (rasterize → occupancy mask → core cells → flood fill → prune small →
// rim dilation → RANSAC fit → min_normal_y filter → ghost emit).
//
// We include the impl source directly to keep the test in a single TU
// (mirrors v7_tile_signals_test.c and v7_hemisphere_segment_test.c).
// The impl pulls in v7-plane-fit + v7-ransac-plane (file statics — fine
// to inline at TU scope) plus pca (resolved via dioramap_steps_normals
// at link time). Don't link dioramap_steps_v7 — that'd duplicate the
// impl's public symbols.

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE   700

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Pull in the impl and its sibling helpers as part of this TU.
#include "v7-plane-fit.c"
#include "v7-ransac-plane.c"
#include "v7-density-voids-2-impl.c"
#include "v7-mulberry32.h"

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

// ─── synthetic-grid helpers ────────────────────────────────────────
//
// We build datasets directly in y-up bench frame (up_axis=1). The
// rasterizer uses (x, z) for the horizontal axes; the up axis (y) is
// unused for occupancy and only matters for the RANSAC fit.
//
// Cell size is forced to 1.0 m via params.cell_size to make the
// geometry of voids/rim/inner candidates exact and easy to reason
// about — no dependency on the (density → cell_size) heuristic.

// Push `n_per_cell` points uniformly into a single cell centred at
// (cx, cy, cz). Position jitter is intentionally tiny (1e-4) so every
// sample falls inside the cell.
static void push_cell(double* positions, size_t* pn,
                      double cx, double cy, double cz, int n_per_cell,
                      double cell_size, v7_rng_t* rng) {
    for (int k = 0; k < n_per_cell; k++) {
        double jx = (v7_rng_next(rng) - 0.5) * (cell_size * 0.5);
        double jz = (v7_rng_next(rng) - 0.5) * (cell_size * 0.5);
        positions[(*pn) * 3 + 0] = cx + jx;
        positions[(*pn) * 3 + 1] = cy;
        positions[(*pn) * 3 + 2] = cz + jz;
        (*pn)++;
    }
}

// Build a dense COLS×ROWS grid of cells at y=cy. `missing` is a list of
// (col, row) pairs (in grid coords) where NO points are emitted; passing
// a length-0 list builds a fully dense grid. Returns the actual number
// of points produced.
typedef struct cell_idx { int c; int r; } cell_idx_t;

static size_t build_grid_with_holes(double* positions,
                                     int cols, int rows,
                                     double cell_size,
                                     double x0, double cy, double z0,
                                     int pts_per_cell,
                                     const cell_idx_t* missing, size_t n_missing,
                                     v7_rng_t* rng) {
    size_t pn = 0;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int skip = 0;
            for (size_t m = 0; m < n_missing; m++) {
                if (missing[m].c == c && missing[m].r == r) { skip = 1; break; }
            }
            if (skip) continue;
            double cx = x0 + ((double)c + 0.5) * cell_size;
            double cz = z0 + ((double)r + 0.5) * cell_size;
            push_cell(positions, &pn, cx, cy, cz, pts_per_cell, cell_size, rng);
        }
    }
    return pn;
}

// Param preset for these tests: force cell_size=1 m, disable point-normal
// filter by relaxing roof-tilt band, drop the 2D viz blob unless a test
// flips it back on. Up axis stays at 1 (bench frame: rasterize on (x,z),
// up = y).
static void params_test(v7_dv2_params_t* p) {
    v7_dv2_params_default(p);
    p->cell_size = 1.0;
    p->roof_tilt_min = 0.0;
    p->roof_tilt_max = 1.0;
    p->emit_viz_artifacts = false;
}

// Build a default pool covering [0, n_pts).
static uint32_t* identity_pool(size_t n_pts) {
    uint32_t* pool = (uint32_t*)malloc(n_pts * sizeof(uint32_t));
    if (!pool) return NULL;
    for (size_t i = 0; i < n_pts; i++) pool[i] = (uint32_t)i;
    return pool;
}

// Build a uniform-1.0 weights array of length n_pts. v7_dv2_compute now
// requires non-NULL point_weights (see v7-plane-fit.h); tests pass
// explicit uniform weights so the "unweighted-equivalent" intent is
// visible at the call site (no silent NULL fallback).
static float* uniform_weights(size_t n_pts) {
    if (n_pts == 0) return NULL;
    float* w = (float*)malloc(n_pts * sizeof(float));
    if (!w) return NULL;
    for (size_t i = 0; i < n_pts; i++) w[i] = 1.0f;
    return w;
}

// ─── tests ──────────────────────────────────────────────────────────

// Spec: empty pool returns cleanly with no ghost planes and no viz.
static void test_empty_pool_no_crash(void) {
    v7_dv2_params_t p; params_test(&p);
    v7_dv2_outputs_t out; v7_dv2_outputs_init(&out);

    // Pass at least one position so the (positions != NULL) guard passes;
    // n_pool=0 short-circuits before any position is dereferenced.
    double pos[3] = {0, 0, 0};
    uint32_t pool[1] = {0};
    float w[1] = { 1.0f };
    CHECK(v7_dv2_compute(pos, NULL, w, 1, pool, 0, &p, &out) == 0);
    CHECK(out.n_ghost_planes == 0);
    CHECK(out.viz_2d == NULL);

    v7_dv2_outputs_free(&out);
}

// Spec: single point in the pool → bounds collapse to one cell → no
// voids → no ghost planes. No crash on a degenerate raster.
static void test_single_point_no_voids(void) {
    v7_dv2_params_t p; params_test(&p);
    v7_dv2_outputs_t out; v7_dv2_outputs_init(&out);

    double pos[3] = {0, 5, 0};
    uint32_t pool[1] = {0};
    float w[1] = { 1.0f };
    CHECK(v7_dv2_compute(pos, NULL, w, 1, pool, 1, &p, &out) == 0);
    CHECK(out.n_ghost_planes == 0);

    v7_dv2_outputs_free(&out);
}

// Spec: fully dense uniform grid → no empty cells → no voids → no ghost
// planes emitted. Establishes the trivial detection floor.
static void test_uniform_dense_grid_zero_voids(void) {
    const int COLS = 10, ROWS = 10;
    const double cell_size = 1.0;
    const int per_cell = 4;
    const size_t MAX = (size_t)COLS * (size_t)ROWS * (size_t)per_cell;

    double* positions = (double*)malloc(MAX * 3 * sizeof(double));
    CHECK(positions != NULL);

    v7_rng_t rng; v7_rng_init(&rng, 1);
    size_t n_pts = build_grid_with_holes(positions, COLS, ROWS, cell_size,
                                          0.0, 5.0, 0.0, per_cell,
                                          NULL, 0, &rng);
    uint32_t* pool = identity_pool(n_pts);
    float* weights = uniform_weights(n_pts);

    v7_dv2_params_t p; params_test(&p);
    v7_dv2_outputs_t out; v7_dv2_outputs_init(&out);
    CHECK(v7_dv2_compute(positions, NULL, weights, n_pts, pool, n_pts, &p, &out) == 0);
    CHECK(out.n_ghost_planes == 0);

    v7_dv2_outputs_free(&out);
    free(positions); free(pool); free(weights);
}

// Spec: a single missing cell is below `min_void_cells` (default 8),
// so even though one cell is empty, the connected component gets
// pruned. Result: 0 ghost planes.
static void test_one_missing_cell_pruned_by_min_void_cells(void) {
    const int COLS = 10, ROWS = 10;
    const double cell_size = 1.0;
    const int per_cell = 4;
    const size_t MAX = (size_t)COLS * (size_t)ROWS * (size_t)per_cell;

    double* positions = (double*)malloc(MAX * 3 * sizeof(double));
    CHECK(positions != NULL);

    v7_rng_t rng; v7_rng_init(&rng, 2);
    const cell_idx_t missing[] = {{4, 4}};
    size_t n_pts = build_grid_with_holes(positions, COLS, ROWS, cell_size,
                                          0.0, 5.0, 0.0, per_cell,
                                          missing, 1, &rng);
    uint32_t* pool = identity_pool(n_pts);
    float* weights = uniform_weights(n_pts);

    v7_dv2_params_t p; params_test(&p);
    v7_dv2_outputs_t out; v7_dv2_outputs_init(&out);
    CHECK(v7_dv2_compute(positions, NULL, weights, n_pts, pool, n_pts, &p, &out) == 0);
    // 1 empty cell → component_size = 1 < min_void_cells (default 8) →
    // pruned at step 5. No ghost planes survive.
    CHECK(out.n_ghost_planes == 0);

    v7_dv2_outputs_free(&out);
    free(positions); free(pool); free(weights);
}

// Spec: a 6×6 hole inside an otherwise dense grid is large enough to
// pass min_void_cells. RANSAC fits a near-horizontal plane through the
// surrounding rim points. Exactly one ghost plane is emitted, its normal
// aligned with ±y (the up axis), and its hull lies on the fit plane.
static void test_large_hole_one_ghost_plane(void) {
    const int COLS = 20, ROWS = 20;
    const double cell_size = 1.0;
    const double cy = 5.0;
    const int per_cell = 4;
    const size_t MAX = (size_t)COLS * (size_t)ROWS * (size_t)per_cell;

    double* positions = (double*)malloc(MAX * 3 * sizeof(double));
    CHECK(positions != NULL);

    // Carve a 6×6 hole at columns/rows [7..12].
    cell_idx_t missing[36];
    size_t mn = 0;
    for (int r = 7; r <= 12; r++) for (int c = 7; c <= 12; c++) {
        missing[mn].c = c; missing[mn].r = r; mn++;
    }
    v7_rng_t rng; v7_rng_init(&rng, 3);
    size_t n_pts = build_grid_with_holes(positions, COLS, ROWS, cell_size,
                                          0.0, cy, 0.0, per_cell,
                                          missing, mn, &rng);
    uint32_t* pool = identity_pool(n_pts);
    float* weights = uniform_weights(n_pts);

    v7_dv2_params_t p; params_test(&p);
    v7_dv2_outputs_t out; v7_dv2_outputs_init(&out);
    CHECK(v7_dv2_compute(positions, NULL, weights, n_pts, pool, n_pts, &p, &out) == 0);
    CHECK(out.n_ghost_planes == 1);

    if (out.n_ghost_planes == 1) {
        const v7_dv2_ghost_plane_t* g = &out.ghost_planes[0];
        // Plane is horizontal at y=cy. Normal should be ±y.
        CHECK_CLOSE(fabs(g->normal[1]), 1.0, 1e-3);
        CHECK_CLOSE(g->normal[0], 0.0, 1e-3);
        CHECK_CLOSE(g->normal[2], 0.0, 1e-3);
        // n·p + d = 0 on y=cy → d ≈ -cy * n_y ≈ ∓cy.
        CHECK_CLOSE(fabs(g->d), cy, 1e-3);
        // Hull is a proper polygon: ≥ 3 verts, finite, on the plane.
        CHECK(g->hull_vert_count >= 3);
        for (uint32_t v = 0; v < g->hull_vert_count; v++) {
            double x = g->hull_xyz[3 * v + 0];
            double y = g->hull_xyz[3 * v + 1];
            double z = g->hull_xyz[3 * v + 2];
            CHECK(isfinite(x) && isfinite(y) && isfinite(z));
            // Plane equation: |n·p + d| ≈ 0.
            double sd = g->normal[0] * x + g->normal[1] * y + g->normal[2] * z + g->d;
            CHECK_CLOSE(sd, 0.0, 1e-6);
        }
        // Centroid also on the plane.
        {
            double sd = g->normal[0] * g->centroid[0]
                      + g->normal[1] * g->centroid[1]
                      + g->normal[2] * g->centroid[2] + g->d;
            CHECK_CLOSE(sd, 0.0, 1e-6);
        }
        // Area = void_cell_count × cell_size². For a 6×6 missing block at
        // cell_size=1m the algorithm labels 32 cells (not 36): the four
        // corners of the hole each have only 3 empty 8-neighbours and
        // fail the core_min_empty=6 test, AND the BFS only expands from
        // core cells (so the corners stay unlabeled). 4×4 core interior
        // (16) + 4 edges × 4 non-core (16) = 32.
        CHECK_CLOSE(g->area, 32.0, 1e-9);
        // inlier_count is consistent with inlier_indices NOT NULL.
        CHECK(g->inlier_count >= (uint32_t)p.min_inliers);
        CHECK(g->inlier_indices != NULL);
        // inlier indices all live in [0, n_pts).
        for (uint32_t k = 0; k < g->inlier_count; k++) {
            CHECK(g->inlier_indices[k] < (uint32_t)n_pts);
        }
        // void_id is a valid 0-based id (first void is 0).
        CHECK(g->void_id == 0);
    }

    v7_dv2_outputs_free(&out);
    free(positions); free(pool); free(weights);
}

// Spec: outliers inside a void cell are tolerated. The algorithm
// classifies points in occupied-deficient void cells as "interior
// survivors" and folds them into the inner candidate pool — but the
// surrounding rim provides far more candidates, so RANSAC still
// converges to the surrounding plane. A handful of outliers cannot
// derail detection.
static void test_outlier_inside_void_still_detected(void) {
    const int COLS = 20, ROWS = 20;
    const double cell_size = 1.0;
    const double cy = 5.0;
    const int per_cell = 4;
    const size_t MAX = (size_t)COLS * (size_t)ROWS * (size_t)per_cell + 4;

    double* positions = (double*)malloc(MAX * 3 * sizeof(double));
    CHECK(positions != NULL);

    cell_idx_t missing[36];
    size_t mn = 0;
    for (int r = 7; r <= 12; r++) for (int c = 7; c <= 12; c++) {
        missing[mn].c = c; missing[mn].r = r; mn++;
    }
    v7_rng_t rng; v7_rng_init(&rng, 4);
    size_t n_pts = build_grid_with_holes(positions, COLS, ROWS, cell_size,
                                          0.0, cy, 0.0, per_cell,
                                          missing, mn, &rng);

    // Add 1 outlier high above the roof, in the middle of the void.
    positions[n_pts * 3 + 0] = 9.5;
    positions[n_pts * 3 + 1] = 20.0;
    positions[n_pts * 3 + 2] = 9.5;
    n_pts++;

    uint32_t* pool = identity_pool(n_pts);
    float* weights = uniform_weights(n_pts);

    v7_dv2_params_t p; params_test(&p);
    v7_dv2_outputs_t out; v7_dv2_outputs_init(&out);
    CHECK(v7_dv2_compute(positions, NULL, weights, n_pts, pool, n_pts, &p, &out) == 0);
    // Detection still produces exactly one ghost: outlier was a single
    // interior point (below occupied_min_count=2, so it doesn't make
    // its cell occupied; it becomes an "interior survivor" inner
    // candidate, but RANSAC majority-vote on the rim still wins).
    CHECK(out.n_ghost_planes == 1);
    if (out.n_ghost_planes == 1) {
        CHECK_CLOSE(fabs(out.ghost_planes[0].normal[1]), 1.0, 1e-2);
    }

    v7_dv2_outputs_free(&out);
    free(positions); free(pool); free(weights);
}

// Spec: the `min_normal_y` filter drops ghost fits whose fit normal has
// |n_up| below the threshold. We construct a near-vertical fit by
// raising `min_normal_y` above the actual horizontal-plane normal's
// up-component (which is ≈ 1.0) — any value > 1 should reject the only
// candidate. Demonstrates the filter is wired and load-bearing.
static void test_min_normal_y_filter_rejects_fit(void) {
    const int COLS = 20, ROWS = 20;
    const double cell_size = 1.0;
    const int per_cell = 4;
    const size_t MAX = (size_t)COLS * (size_t)ROWS * (size_t)per_cell;

    double* positions = (double*)malloc(MAX * 3 * sizeof(double));
    CHECK(positions != NULL);

    cell_idx_t missing[36];
    size_t mn = 0;
    for (int r = 7; r <= 12; r++) for (int c = 7; c <= 12; c++) {
        missing[mn].c = c; missing[mn].r = r; mn++;
    }
    v7_rng_t rng; v7_rng_init(&rng, 5);
    size_t n_pts = build_grid_with_holes(positions, COLS, ROWS, cell_size,
                                          0.0, 5.0, 0.0, per_cell,
                                          missing, mn, &rng);
    uint32_t* pool = identity_pool(n_pts);
    float* weights = uniform_weights(n_pts);

    v7_dv2_params_t p; params_test(&p);
    p.min_normal_y = 1.5;       // impossible for any unit normal → drop everything
    v7_dv2_outputs_t out; v7_dv2_outputs_init(&out);
    CHECK(v7_dv2_compute(positions, NULL, weights, n_pts, pool, n_pts, &p, &out) == 0);
    CHECK(out.n_ghost_planes == 0);

    v7_dv2_outputs_free(&out);
    free(positions); free(pool); free(weights);
}

// Spec: the per-point normal pre-filter excludes candidate points whose
// |n_up| is outside the [roofTiltMin, roofTiltMax] band. Setting both
// bounds to a near-zero window keeps wall-like points out of the
// candidate pool — RANSAC has nothing to fit and no ghost is emitted.
static void test_per_point_normal_filter_drops_all_candidates(void) {
    const int COLS = 20, ROWS = 20;
    const double cell_size = 1.0;
    const int per_cell = 4;
    const size_t MAX = (size_t)COLS * (size_t)ROWS * (size_t)per_cell;

    double* positions = (double*)malloc(MAX * 3 * sizeof(double));
    float*  normals   = (float*) malloc(MAX * 3 * sizeof(float));
    CHECK(positions != NULL); CHECK(normals != NULL);

    cell_idx_t missing[36];
    size_t mn = 0;
    for (int r = 7; r <= 12; r++) for (int c = 7; c <= 12; c++) {
        missing[mn].c = c; missing[mn].r = r; mn++;
    }
    v7_rng_t rng; v7_rng_init(&rng, 6);
    size_t n_pts = build_grid_with_holes(positions, COLS, ROWS, cell_size,
                                          0.0, 5.0, 0.0, per_cell,
                                          missing, mn, &rng);
    // All normals are perfectly up: n_y = 1.0. Setting roof_tilt_max to
    // 0.1 means EVERY point gets filtered out (|n_y|=1 > 0.1).
    for (size_t i = 0; i < n_pts; i++) {
        normals[i * 3 + 0] = 0.0f;
        normals[i * 3 + 1] = 1.0f;
        normals[i * 3 + 2] = 0.0f;
    }
    uint32_t* pool = identity_pool(n_pts);
    float* weights = uniform_weights(n_pts);

    v7_dv2_params_t p; params_test(&p);
    p.roof_tilt_min = 0.0;
    p.roof_tilt_max = 0.1;       // drops everything with |n_y| > 0.1
    v7_dv2_outputs_t out; v7_dv2_outputs_init(&out);
    CHECK(v7_dv2_compute(positions, normals, weights, n_pts, pool, n_pts, &p, &out) == 0);
    CHECK(out.n_ghost_planes == 0);

    v7_dv2_outputs_free(&out);
    free(positions); free(normals); free(pool); free(weights);
}

// Spec: when `emit_viz_artifacts` is false, the 2D viz blob is not
// allocated (load-bearing for production runs that don't ship the
// renderer-side debug payload).
static void test_emit_viz_artifacts_false_yields_null_viz(void) {
    const int COLS = 20, ROWS = 20;
    const double cell_size = 1.0;
    const int per_cell = 4;
    const size_t MAX = (size_t)COLS * (size_t)ROWS * (size_t)per_cell;

    double* positions = (double*)malloc(MAX * 3 * sizeof(double));
    CHECK(positions != NULL);

    cell_idx_t missing[36];
    size_t mn = 0;
    for (int r = 7; r <= 12; r++) for (int c = 7; c <= 12; c++) {
        missing[mn].c = c; missing[mn].r = r; mn++;
    }
    v7_rng_t rng; v7_rng_init(&rng, 7);
    size_t n_pts = build_grid_with_holes(positions, COLS, ROWS, cell_size,
                                          0.0, 5.0, 0.0, per_cell,
                                          missing, mn, &rng);
    uint32_t* pool = identity_pool(n_pts);
    float* weights = uniform_weights(n_pts);

    v7_dv2_params_t p; params_test(&p);
    p.emit_viz_artifacts = false;
    v7_dv2_outputs_t out; v7_dv2_outputs_init(&out);
    CHECK(v7_dv2_compute(positions, NULL, weights, n_pts, pool, n_pts, &p, &out) == 0);
    CHECK(out.viz_2d == NULL);

    v7_dv2_outputs_free(&out);
    free(positions); free(pool); free(weights);
}

// Spec converse: when `emit_viz_artifacts` is true, the 2D blob is
// populated and self-consistent. cell_size + cols + rows + cell counts
// match the rasterizer input, void summaries are dense in [0, n_voids).
static void test_emit_viz_artifacts_true_blob_consistent(void) {
    const int COLS = 20, ROWS = 20;
    const double cell_size = 1.0;
    const int per_cell = 4;
    const size_t MAX = (size_t)COLS * (size_t)ROWS * (size_t)per_cell;

    double* positions = (double*)malloc(MAX * 3 * sizeof(double));
    CHECK(positions != NULL);

    cell_idx_t missing[36];
    size_t mn = 0;
    for (int r = 7; r <= 12; r++) for (int c = 7; c <= 12; c++) {
        missing[mn].c = c; missing[mn].r = r; mn++;
    }
    v7_rng_t rng; v7_rng_init(&rng, 8);
    size_t n_pts = build_grid_with_holes(positions, COLS, ROWS, cell_size,
                                          0.0, 5.0, 0.0, per_cell,
                                          missing, mn, &rng);
    uint32_t* pool = identity_pool(n_pts);
    float* weights = uniform_weights(n_pts);

    v7_dv2_params_t p; params_test(&p);
    p.emit_viz_artifacts = true;
    v7_dv2_outputs_t out; v7_dv2_outputs_init(&out);
    CHECK(v7_dv2_compute(positions, NULL, weights, n_pts, pool, n_pts, &p, &out) == 0);
    CHECK(out.viz_2d != NULL);
    if (out.viz_2d) {
        const v7_dv2_2d_t* v = out.viz_2d;
        CHECK_CLOSE(v->cell_size, 1.0, 1e-12);
        CHECK(v->cols >= 1 && v->rows >= 1);
        CHECK(v->n_points == (uint32_t)n_pts);
        CHECK(v->n_voids >= 1);
        // Every cell's counts is non-negative and void_ids in [-1, n_voids).
        const size_t nc = (size_t)v->cols * (size_t)v->rows;
        for (size_t k = 0; k < nc; k++) {
            CHECK(v->counts[k] >= 0);
            CHECK(v->void_ids[k] >= -1);
            CHECK(v->void_ids[k] < (int16_t)v->n_voids);
        }
        // rim_mask is binary {0, 1}.
        for (size_t k = 0; k < nc; k++) CHECK(v->rim_mask[k] <= 1);
        // Sum of counts equals n_points.
        size_t sum = 0;
        for (size_t k = 0; k < nc; k++) sum += (size_t)v->counts[k];
        CHECK(sum == n_pts);
    }

    v7_dv2_outputs_free(&out);
    free(positions); free(pool); free(weights);
}

// Spec: determinism. Same seed + same input → identical ghost-plane
// output. Mulberry32 is bit-stable and rasterization + flood fill +
// RANSAC sampling are all seeded; rerunning with the SAME inputs must
// reproduce the SAME plane (count, normal, d, inlier_count, area).
static void test_repeatability_same_seed(void) {
    const int COLS = 20, ROWS = 20;
    const double cell_size = 1.0;
    const int per_cell = 4;
    const size_t MAX = (size_t)COLS * (size_t)ROWS * (size_t)per_cell;

    double* positions = (double*)malloc(MAX * 3 * sizeof(double));
    CHECK(positions != NULL);

    cell_idx_t missing[36];
    size_t mn = 0;
    for (int r = 7; r <= 12; r++) for (int c = 7; c <= 12; c++) {
        missing[mn].c = c; missing[mn].r = r; mn++;
    }
    v7_rng_t rng; v7_rng_init(&rng, 9);
    size_t n_pts = build_grid_with_holes(positions, COLS, ROWS, cell_size,
                                          0.0, 5.0, 0.0, per_cell,
                                          missing, mn, &rng);
    uint32_t* pool = identity_pool(n_pts);
    float* weights = uniform_weights(n_pts);

    v7_dv2_params_t p; params_test(&p);
    v7_dv2_outputs_t out1; v7_dv2_outputs_init(&out1);
    v7_dv2_outputs_t out2; v7_dv2_outputs_init(&out2);
    CHECK(v7_dv2_compute(positions, NULL, weights, n_pts, pool, n_pts, &p, &out1) == 0);
    CHECK(v7_dv2_compute(positions, NULL, weights, n_pts, pool, n_pts, &p, &out2) == 0);
    CHECK(out1.n_ghost_planes == out2.n_ghost_planes);
    if (out1.n_ghost_planes == out2.n_ghost_planes && out1.n_ghost_planes >= 1) {
        const v7_dv2_ghost_plane_t* a = &out1.ghost_planes[0];
        const v7_dv2_ghost_plane_t* b = &out2.ghost_planes[0];
        CHECK_CLOSE(a->normal[0], b->normal[0], 1e-12);
        CHECK_CLOSE(a->normal[1], b->normal[1], 1e-12);
        CHECK_CLOSE(a->normal[2], b->normal[2], 1e-12);
        CHECK_CLOSE(a->d, b->d, 1e-12);
        CHECK_CLOSE(a->area, b->area, 1e-12);
        CHECK(a->inlier_count == b->inlier_count);
        CHECK(a->hull_vert_count == b->hull_vert_count);
        CHECK(a->void_id == b->void_id);
    }

    v7_dv2_outputs_free(&out1);
    v7_dv2_outputs_free(&out2);
    free(positions); free(pool); free(weights);
}

// Spec: two well-separated holes in the same flat plane produce two
// independent connected components → two voids → two ghost planes.
// Both fit normals must align with ±y; void_ids are distinct.
static void test_two_separate_holes_two_ghosts(void) {
    const int COLS = 30, ROWS = 20;
    const double cell_size = 1.0;
    const double cy = 5.0;
    const int per_cell = 4;
    const size_t MAX = (size_t)COLS * (size_t)ROWS * (size_t)per_cell;

    double* positions = (double*)malloc(MAX * 3 * sizeof(double));
    CHECK(positions != NULL);

    // Two 5×5 holes far apart in the (c, r) lattice — separated by
    // several occupied rows so they don't merge via the 4-conn flood.
    cell_idx_t missing[50];
    size_t mn = 0;
    for (int r = 3; r <= 7; r++) for (int c = 3; c <= 7; c++) {
        missing[mn].c = c; missing[mn].r = r; mn++;
    }
    for (int r = 12; r <= 16; r++) for (int c = 20; c <= 24; c++) {
        missing[mn].c = c; missing[mn].r = r; mn++;
    }
    v7_rng_t rng; v7_rng_init(&rng, 10);
    size_t n_pts = build_grid_with_holes(positions, COLS, ROWS, cell_size,
                                          0.0, cy, 0.0, per_cell,
                                          missing, mn, &rng);
    uint32_t* pool = identity_pool(n_pts);
    float* weights = uniform_weights(n_pts);

    v7_dv2_params_t p; params_test(&p);
    v7_dv2_outputs_t out; v7_dv2_outputs_init(&out);
    CHECK(v7_dv2_compute(positions, NULL, weights, n_pts, pool, n_pts, &p, &out) == 0);
    CHECK(out.n_ghost_planes == 2);
    if (out.n_ghost_planes == 2) {
        for (size_t i = 0; i < 2; i++) {
            CHECK_CLOSE(fabs(out.ghost_planes[i].normal[1]), 1.0, 1e-3);
        }
        // void_ids are distinct.
        CHECK(out.ghost_planes[0].void_id != out.ghost_planes[1].void_id);
    }

    v7_dv2_outputs_free(&out);
    free(positions); free(pool); free(weights);
}

// Spec: a hole large enough to pass flood-fill, but with too few rim
// candidates for `min_fit_pts`, produces 0 ghost planes. We achieve
// this by raising min_fit_pts above the available inner candidate
// count.
static void test_min_fit_pts_gates_ransac(void) {
    const int COLS = 20, ROWS = 20;
    const double cell_size = 1.0;
    const int per_cell = 4;
    const size_t MAX = (size_t)COLS * (size_t)ROWS * (size_t)per_cell;

    double* positions = (double*)malloc(MAX * 3 * sizeof(double));
    CHECK(positions != NULL);

    cell_idx_t missing[36];
    size_t mn = 0;
    for (int r = 7; r <= 12; r++) for (int c = 7; c <= 12; c++) {
        missing[mn].c = c; missing[mn].r = r; mn++;
    }
    v7_rng_t rng; v7_rng_init(&rng, 11);
    size_t n_pts = build_grid_with_holes(positions, COLS, ROWS, cell_size,
                                          0.0, 5.0, 0.0, per_cell,
                                          missing, mn, &rng);
    uint32_t* pool = identity_pool(n_pts);
    float* weights = uniform_weights(n_pts);

    v7_dv2_params_t p; params_test(&p);
    p.min_fit_pts = 100000;     // impossibly high → RANSAC never tried
    v7_dv2_outputs_t out; v7_dv2_outputs_init(&out);
    CHECK(v7_dv2_compute(positions, NULL, weights, n_pts, pool, n_pts, &p, &out) == 0);
    CHECK(out.n_ghost_planes == 0);

    v7_dv2_outputs_free(&out);
    free(positions); free(pool); free(weights);
}

// ─── main ───────────────────────────────────────────────────────────

int main(void) {
    test_empty_pool_no_crash();
    test_single_point_no_voids();
    test_uniform_dense_grid_zero_voids();
    test_one_missing_cell_pruned_by_min_void_cells();
    test_large_hole_one_ghost_plane();
    test_outlier_inside_void_still_detected();
    test_min_normal_y_filter_rejects_fit();
    test_per_point_normal_filter_drops_all_candidates();
    test_emit_viz_artifacts_false_yields_null_viz();
    test_emit_viz_artifacts_true_blob_consistent();
    test_repeatability_same_seed();
    test_two_separate_holes_two_ghosts();
    test_min_fit_pts_gates_ransac();

    printf("%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}

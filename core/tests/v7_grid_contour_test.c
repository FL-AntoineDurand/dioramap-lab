// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// core/tests/v7_grid_contour_test.c
//
// Implementation tests for v7-grid-contour.c — the density-weighted
// marching-squares contour extractor (faithful port of bench-v2
// computeGridContour / computeGridContourAdaptive).
//
// White-box convention (docs/quality/V7-TEST-STRATEGY.md §3.3.1): the
// impl .c is #included directly; the foreach links dioramap_steps_normals
// + m. Assertions are analytic — exact on metadata/defaults/caps, and
// geometric (area/bbox/closed-loop) on the contour output.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "v7-grid-contour.c"

// ─── harness ────────────────────────────────────────────────────────
static int g_checks = 0; static int g_failed = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failed++; } \
} while (0)

#define CHECK_CLOSE(a, b, eps) do { \
    double _a = (a), _b = (b); \
    g_checks++; \
    if (fabs(_a - _b) > (eps)) { \
        fprintf(stderr, "FAIL %s:%d: %s ≈ %s (got %.12g, want %.12g, eps=%g)\n", \
            __FILE__, __LINE__, #a, #b, _a, _b, (eps)); \
        g_failed++; \
    } \
} while (0)

// ─── helpers ────────────────────────────────────────────────────────

// Fill a regular grid of unit-weight points over [u0,u1]×[v0,v1] at the
// given step. Caller frees.
static v7_gc_point_t* fill_grid(double u0, double u1, double v0, double v1,
                                double step, size_t* out_n) {
    size_t nu = (size_t)((u1 - u0) / step) + 1;
    size_t nv = (size_t)((v1 - v0) / step) + 1;
    size_t n = nu * nv;
    v7_gc_point_t* p = (v7_gc_point_t*)malloc(n * sizeof(*p));
    size_t k = 0;
    for (size_t i = 0; i < nu; i++)
        for (size_t j = 0; j < nv; j++) {
            p[k].u = u0 + (double)i * step;
            p[k].v = v0 + (double)j * step;
            p[k].weight = 1.0;
            k++;
        }
    *out_n = n;
    return p;
}

// Signed area (shoelace) of a closed loop stored as interleaved (u,v).
static double loop_area_abs(const v7_gc_loop_t* L) {
    if (!L || L->n_pts < 3) return 0.0;
    double a = 0.0;
    for (uint32_t i = 0; i < L->n_pts; i++) {
        uint32_t j = (i + 1) % L->n_pts;
        a += L->pts[i*2] * L->pts[j*2+1] - L->pts[j*2] * L->pts[i*2+1];
    }
    return fabs(a) * 0.5;
}

static void loop_bbox(const v7_gc_loop_t* L, double bb[4]) {
    bb[0] = bb[2] = L->pts[0];      // min_u, max_u
    bb[1] = bb[3] = L->pts[1];      // min_v, max_v
    for (uint32_t i = 1; i < L->n_pts; i++) {
        double u = L->pts[i*2], v = L->pts[i*2+1];
        if (u < bb[0]) bb[0] = u;
        if (u > bb[2]) bb[2] = u;
        if (v < bb[1]) bb[1] = v;
        if (v > bb[3]) bb[3] = v;
    }
}

// ─── tests ──────────────────────────────────────────────────────────

static void test_opts_default(void) {
    v7_gc_opts_t o;
    v7_gc_opts_default(&o);
    CHECK_CLOSE(o.threshold_ratio, 0.1, 0.0);
    CHECK(o.icm_iterations == 10);
    CHECK(o.margin_cells == 4);
    CHECK(o.shrink == false);
}

static void test_result_init_zeroes(void) {
    v7_gc_result_t r;
    // Poison, then init must zero/NULL everything.
    memset(&r, 0xAB, sizeof(r));
    v7_gc_result_init(&r);
    CHECK(r.n_cols == 0 && r.n_rows == 0);
    CHECK(r.density == NULL && r.smoothed == NULL && r.binary == NULL);
    CHECK(r.density_nearby == NULL && r.smoothed_nearby == NULL);
    CHECK(r.outer.pts == NULL && r.outer.n_pts == 0);
    CHECK(r.holes == NULL && r.n_holes == 0);
}

// Grid dimensions follow margin = cell_size·margin_cells:
//   g_min = bounds.min - margin;  n = ceil((span + 2·margin)/cell) + 1.
// For [0,10]², cell=1, margin_cells=4: margin=4, g_min=-4,
//   n = ceil((10 + 8)/1) + 1 = 19.
static void test_grid_dims_and_threshold(void) {
    size_t n; v7_gc_point_t* pts = fill_grid(0, 10, 0, 10, 0.5, &n);
    v7_gc_bounds_t b = { 0, 10, 0, 10 };
    v7_gc_result_t r;
    int rc = v7_gc_compute(pts, n, NULL, 0, 1.0, &b, NULL, &r);
    CHECK(rc == 0);
    CHECK(r.n_cols == 19);
    CHECK(r.n_rows == 19);
    CHECK_CLOSE(r.cell_size, 1.0, 0.0);
    CHECK_CLOSE(r.g_min_u, -4.0, 1e-12);
    CHECK_CLOSE(r.g_min_v, -4.0, 1e-12);
    // threshold == max_density · threshold_ratio (default 0.1).
    CHECK(r.max_density > 0.0);
    CHECK_CLOSE(r.threshold, r.max_density * 0.1, 1e-12);
    v7_gc_result_free(&r);
    free(pts);
}

// A grid that would exceed the 500k-cell safety cap is a silent no-op (+1).
static void test_safety_cap_oversized(void) {
    v7_gc_bounds_t b = { 0, 1000, 0, 1000 };
    v7_gc_result_t r;
    int rc = v7_gc_compute(NULL, 0, NULL, 0, 0.1, &b, NULL, &r);  // ~1e8 cells
    CHECK(rc == 1);
    CHECK(r.outer.n_pts == 0);
    CHECK(r.density == NULL);   // nothing allocated on the cap path
    v7_gc_result_free(&r);
}

// Empty input: valid grid, but no density anywhere → no loop, clean 0.
static void test_empty_input_no_loop(void) {
    v7_gc_bounds_t b = { 0, 10, 0, 10 };
    v7_gc_result_t r;
    int rc = v7_gc_compute(NULL, 0, NULL, 0, 1.0, &b, NULL, &r);
    CHECK(rc == 0);
    CHECK_CLOSE(r.max_density, 0.0, 0.0);
    CHECK(r.outer.n_pts == 0);
    CHECK(r.n_holes == 0);
    v7_gc_result_free(&r);
}

// A dense filled square yields a single closed outer loop, no holes, whose
// bounding box brackets the input square (within a few cells) and whose
// enclosed area is most of the 100 m² square.
static void test_dense_square_one_loop(void) {
    size_t n; v7_gc_point_t* pts = fill_grid(0, 10, 0, 10, 0.5, &n);
    v7_gc_bounds_t b = { 0, 10, 0, 10 };
    v7_gc_result_t r;
    int rc = v7_gc_compute(pts, n, NULL, 0, 1.0, &b, NULL, &r);
    CHECK(rc == 0);
    CHECK(r.outer.n_pts >= 4);     // a real polygon, not a sliver
    CHECK(r.n_holes == 0);         // solid fill → no interior holes
    if (r.outer.n_pts >= 4) {
        // The 3×3 Gaussian smooth + low (0.1) threshold push the contour
        // ~1–2 cells OUTWARD, so it encloses the input square plus a thin
        // collar — area ≳ 100 (the input) and well under a runaway. Measured
        // ≈162.5 for this fixture (bbox ≈ [-1.5,11.5]²).
        double area = loop_area_abs(&r.outer);
        CHECK(area > 90.0 && area < 256.0);
        double bb[4]; loop_bbox(&r.outer, bb);
        CHECK(bb[0] <= 1.0 && bb[2] >= 9.0);   // u brackets [0,10]
        CHECK(bb[1] <= 1.0 && bb[3] >= 9.0);   // v brackets [0,10]
    }
    v7_gc_result_free(&r);
    free(pts);
}

// Adaptive selection runs a sweep of cell sizes and returns a usable
// contour plus the per-trial diagnostics.
static void test_adaptive_picks_trial(void) {
    size_t n; v7_gc_point_t* pts = fill_grid(0, 10, 0, 10, 0.5, &n);
    v7_gc_bounds_t b = { 0, 10, 0, 10 };
    v7_gc_result_t r;
    v7_gc_trial_t* trials = NULL; uint32_t n_trials = 0;
    int rc = v7_gc_compute_adaptive(pts, n, NULL, 0, 1.0, &b, NULL,
                                     &r, &trials, &n_trials);
    CHECK(rc == 0);
    CHECK(r.outer.n_pts >= 4);
    CHECK(n_trials > 0);
    CHECK(trials != NULL);
    if (trials) {
        // The selected result's cell size must be one of the trials.
        int found = 0;
        for (uint32_t i = 0; i < n_trials; i++)
            if (fabs(trials[i].cell_size - r.cell_size) < 1e-9) found = 1;
        CHECK(found == 1);
    }
    free(trials);
    v7_gc_result_free(&r);
    free(pts);
}

int main(void) {
    test_opts_default();
    test_result_init_zeroes();
    test_grid_dims_and_threshold();
    test_safety_cap_oversized();
    test_empty_input_no_loop();
    test_dense_square_one_loop();
    test_adaptive_picks_trial();
    printf("%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}

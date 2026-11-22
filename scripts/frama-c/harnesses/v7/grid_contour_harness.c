// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Per-impl Frama-C harness for v7-grid-contour.c.
//
// Drives v7_gc_compute directly with bounded, finite, non-deterministic
// point coordinates. Like the seed / grow-refit-merge harnesses, the
// structural parameters (bounds, cell size, margin, ICM iteration count)
// are FIXED to keep the grid small and the analysis tractable — the RTE
// obligations (grid indexing, rasterise index math, realloc sizes, the
// chain hashmap probe) are the same code regardless of grid size; a 5×5
// grid exercises every branch the production 4-margin grid does.
//
// Inputs constrained to a finite box (real callers always pass finite UV
// coordinates), which discharges the ¬is_NaN obligations on the reads.
//
// Build: not built by CMake. Driven via:
//   scripts/frama-c/audit-module.sh grid_contour
//
// See docs/quality/README.md.

#include "v7-grid-contour.h"

#include <__fc_builtin.h>     // Frama_C_double_interval
#include <stddef.h>
#include <stdlib.h>

#define N_PTS 4

static void fill_finite(v7_gc_point_t* p, int n) {
    for (int i = 0; i < n; i++) {
        p[i].u      = Frama_C_double_interval(0.0, 2.0);
        p[i].v      = Frama_C_double_interval(0.0, 2.0);
        p[i].weight = Frama_C_double_interval(0.0, 10.0);
    }
}

int main(void) {
    v7_gc_point_t points[N_PTS];
    v7_gc_point_t nearby[N_PTS];
    fill_finite(points, N_PTS);
    fill_finite(nearby, N_PTS);

    // Fixed small grid: bounds 2×2, cell 1, margin 1 → 5×5 = 25 cells.
    v7_gc_bounds_t bounds = { 0.0, 2.0, 0.0, 2.0 };

    v7_gc_opts_t opts;
    v7_gc_opts_default(&opts);
    opts.margin_cells   = 1;   // shrink the grid (default 4) for tractability
    opts.icm_iterations = 2;   // 2 passes exercise the morphology loop body

    // Own-points-only path.
    v7_gc_result_t out;
    (void)v7_gc_compute(points, N_PTS, NULL, 0, 1.0, &bounds, &opts, &out);
    v7_gc_result_free(&out);

    // Competing-density path (nearby_points non-NULL): hits the
    // `own > threshold && own > nearby` binarisation + ICM tiebreak branch.
    v7_gc_result_t out2;
    (void)v7_gc_compute(points, N_PTS, nearby, N_PTS, 1.0, &bounds, &opts, &out2);
    v7_gc_result_free(&out2);

    // Empty-input path (no density anywhere → no loop, clean return).
    v7_gc_result_t out3;
    (void)v7_gc_compute(NULL, 0, NULL, 0, 1.0, &bounds, &opts, &out3);
    v7_gc_result_free(&out3);

    return 0;
}

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-grid-contour — density-weighted grid topology extraction.
//
// Faithful port of `computeGridContour` + `computeGridContourAdaptive`
// in lidar-algos-bench-v2/lib/pipeline/steps/boundary-edges/signals.js
// (the production-ready marching-squares utility extracted from the
// March 2026 boundary-bench tournament).
//
// Pipeline per call:
//   1. Rasterize: accumulate per-point weight (Voronoi area) into cells.
//   2. Smooth: 3×3 Gaussian, 2 passes, center=4 / edge=2 / corner=1.
//   3. Binarize: threshold at thresholdRatio · maxDensity.
//   4. ICM morphology: N iterations, neighbour-count rule with smoothed
//      tiebreak. Cleans salt-and-pepper noise.
//   5. Marching squares: 16-case lookup, edge-midpoint vertices.
//   6. Chain: adjacency via rounded-coord hash → closed loops.
//   7. (optional) Shrink inward by 0.5·cellSize. SKIPPED here by default
//      because boundary-polygons-2 wants raw cell-edge topology (no
//      edge-precision recovery — that's regularization's job).
//
// Output: outer = longest closed loop; holes = remaining closed loops.

#ifndef V3_STEPS_V7_GRID_CONTOUR_H
#define V3_STEPS_V7_GRID_CONTOUR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct v7_gc_point {
    double u, v;
    double weight;   // 1.0 if no per-point weight available
} v7_gc_point_t;

typedef struct v7_gc_bounds {
    double min_u, max_u;
    double min_v, max_v;
} v7_gc_bounds_t;

typedef struct v7_gc_opts {
    double threshold_ratio;   // default 0.1
    int    icm_iterations;    // default 10
    int    margin_cells;      // default 4
    bool   shrink;            // default false in C (bp2's use case)
} v7_gc_opts_t;

void v7_gc_opts_default(v7_gc_opts_t* o);

// A closed loop: array of (u, v) f64 pairs. Caller frees `pts`.
typedef struct v7_gc_loop {
    double* pts;       // 2 doubles per vertex (u, v interleaved)
    uint32_t n_pts;
} v7_gc_loop_t;

typedef struct v7_gc_result {
    // Grid metadata.
    uint32_t n_cols, n_rows;
    double   cell_size;
    double   g_min_u, g_min_v;
    double   max_density, threshold;
    // Grids — caller frees.
    double*  density;          // [n_cols * n_rows] own-points rasterised
    double*  smoothed;         // [n_cols * n_rows] post-Gaussian
    uint8_t* binary;           // [n_cols * n_rows] — 0 or 1
    // Nearby grids — NULL if no nearby_points were supplied.
    double*  density_nearby;   // [n_cols * n_rows] nearby-points rasterised
    double*  smoothed_nearby;  // [n_cols * n_rows] post-Gaussian
    // Loops — caller frees `outer.pts` and each holes[k].pts.
    v7_gc_loop_t  outer;       // .n_pts = 0 if no usable loop
    v7_gc_loop_t* holes;       // [n_holes]; NULL if 0
    uint32_t      n_holes;
} v7_gc_result_t;

void v7_gc_result_init(v7_gc_result_t* r);
void v7_gc_result_free(v7_gc_result_t* r);

// Returns 0 on success, +1 if the grid would exceed the 500k-cell safety
// cap (silently no-op), -1 on allocation failure.
//
// When `nearby_points` is non-NULL and `n_nearby > 0`, a parallel "competing"
// density grid is built from those points (typically per-plane nearby
// points — projections of OTHER planes' points onto this plane). The
// binarisation becomes `own > threshold AND own > nearby` per cell; the
// ICM tiebreak uses the same contest. Sharpens the boundary at
// shared-edge zones. When NULL, behaviour is unchanged.
int v7_gc_compute(const v7_gc_point_t* points, size_t n_points,
                   const v7_gc_point_t* nearby_points, size_t n_nearby,
                   double cell_size,
                   const v7_gc_bounds_t* bounds,
                   const v7_gc_opts_t* opts,
                   v7_gc_result_t* out);

// Adaptive cell size selection. Runs `compute` at 0.5·spacing → 2·spacing
// in 0.1·spacing steps; picks the finest cell size where no subsequent
// trial drops outer-contour length by more than 5%. Returns the result
// of the selected trial, or +1 if no trial produced a usable contour.
typedef struct v7_gc_trial {
    double   mult;         // 0.5 .. 2.0 step 0.1
    double   cell_size;
    double   contour_len;
    uint32_t n_outer;
    uint32_t n_regions;
} v7_gc_trial_t;

int v7_gc_compute_adaptive(const v7_gc_point_t* points, size_t n_points,
                            const v7_gc_point_t* nearby_points, size_t n_nearby,
                            double spacing,
                            const v7_gc_bounds_t* bounds,
                            const v7_gc_opts_t* opts,
                            v7_gc_result_t* out,
                            v7_gc_trial_t** out_trials,
                            uint32_t* out_n_trials);

#ifdef __cplusplus
}
#endif
#endif

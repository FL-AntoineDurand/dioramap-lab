// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-density-voids-2 — pure compute. Direct port of bench-v2
// lib/pipeline/steps/density-voids-2.js (meta.version=3).
//
// Detects "ghost planes" — roof regions whose points dropped out
// (specular dropout on south-facing slate is the canonical case). The
// algorithm is purely geometric: find low-density patches in the XZ
// projection, RANSAC-fit a plane through the rim points around each
// patch.  Emitted ghost planes feed grow-refit-merge as additional
// seeds.
//
// Per the V7 port policy + the post-port reassessments in
// PHASE-5-V7-PORT.md, this port:
//   - Drops `lidar-density` + `lidar-z-noise` reads. Both are
//     parameterised directly: `density` (default 25 pts/m², dahouet
//     uses 30.475) drives `cell_size = max(0.3, 2/√density)` when
//     `cell_size = 0` is passed; `dist_threshold` defaults to 0.09
//     (= 4.5 × 0.02 σ_z, identical to seed step's distThreshold —
//     v2 derives the same way).
//   - Emit-vs-skip the bulky 2D viz blob is gated by
//     `emit_viz_artifacts` (mirrors v7-hemisphere-segment).
//   - Axis is parameterised: `up_axis` (1 = y-up bench frame for
//     byte-validation; 2 = z-up LAZ frame for production). Affects
//     rasterization (uses the two non-up components) + normal-y
//     filtering (uses n[up_axis]) + hull lift.

#ifndef V3_STEPS_V7_DENSITY_VOIDS_2_IMPL_H
#define V3_STEPS_V7_DENSITY_VOIDS_2_IMPL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct v7_dv2_params {
    // Cell-size + distance scale.
    double cell_size;          // 0 → derive from density: max(0.3, 2/√density)
    double density;            // pts/m². Used only if cell_size==0.
    double dist_threshold;     // RANSAC inlier threshold (m). Default 0.09.

    // Detection knobs.
    int32_t occupied_min_count;  // default 2
    int32_t core_min_empty;      // default 6 (of 8 neighbours)
    int32_t min_void_cells;      // default 8 — discard tiny voids
    int32_t rim_dilation;        // default 2 (Chebyshev cells around void)

    // RANSAC.
    int32_t min_fit_pts;         // default 10
    int32_t ransac_iterations;   // default 200
    int32_t min_inliers;         // default 6
    int32_t seed;                // default 42

    // Filters.
    double min_normal_y;         // default 0.15 — drop near-vertical fits
    double roof_tilt_min;        // default 0.20 — exclude wall-like points
    double roof_tilt_max;        // default 0.98 — exclude near-flat ground

    // Output gating + frame.
    int32_t up_axis;             // 1 = y-up, 2 = z-up
    bool    emit_viz_artifacts;  // skip the 2D viz blob in production runs
} v7_dv2_params_t;

void v7_dv2_params_default(v7_dv2_params_t* p);

// ── Output: ghost plane (load-bearing) ──

typedef struct v7_dv2_ghost_plane {
    double normal[3];
    double d;
    double tilt;
    double azimuth;
    double rms;
    double centroid[3];
    double area;                // void cell count × cellSize²
    uint32_t void_id;
    uint32_t inlier_count;
    uint32_t hull_vert_count;
    uint32_t* inlier_indices;   // owned; local indices into the building's pool
    double*  hull_xyz;          // owned; 3D verts (length 3 × hull_vert_count)
} v7_dv2_ghost_plane_t;

// ── Output: 2D viz blob (skipped when emit_viz_artifacts=false) ──

typedef struct v7_dv2_void_summary {
    uint32_t cell_count;
    uint32_t rim_point_count;
    uint32_t unowned_rim_point_count;
    uint32_t interior_survivor_count;
    double area;
    double cx;
    double cz;                  // h0/h1 of the centroid in the active frame
    bool has_fit;
    // Mirrors of the fit fields for the per-void summary panel.
    double fit_normal[3];
    double fit_d;
    double fit_rms;
    double fit_tilt;
    double fit_azimuth;
    uint32_t fit_n_candidates;
    uint32_t fit_n_inliers;
} v7_dv2_void_summary_t;

typedef struct v7_dv2_2d {
    double bounds[4];           // h0_min, h0_max, h1_min, h1_max
    double cell_size;
    int32_t cols;
    int32_t rows;
    int32_t rim_dilation;
    uint32_t n_points;          // total building points

    int32_t* counts;            // length cols*rows (Int32)
    int16_t* void_ids;          // length cols*rows (Int16, -1 = not in void)
    uint8_t* rim_mask;          // length cols*rows

    v7_dv2_void_summary_t* voids;
    uint32_t n_voids;

    // Rim-point arrays — one entry per (rim or interior-survivor) point.
    double*  rim_xy;            // length 2 × n_rim
    int16_t* rim_void_ids;      // length n_rim
    uint8_t* rim_inlier;        // length n_rim
    uint8_t* rim_survivor;      // length n_rim (1 = interior survivor, 0 = rim)
    uint8_t* rim_dist;          // length n_rim (Chebyshev dist from void cell)
    uint8_t* rim_filtered;      // length n_rim (1 if dropped by normal pre-filter)
    uint32_t n_rim;
} v7_dv2_2d_t;

typedef struct v7_dv2_outputs {
    v7_dv2_ghost_plane_t* ghost_planes;  // owned; each owns its inlier_indices/hull
    size_t n_ghost_planes;

    v7_dv2_2d_t* viz_2d;                 // NULL when emit_viz_artifacts=false
} v7_dv2_outputs_t;

void v7_dv2_outputs_init(v7_dv2_outputs_t* out);
void v7_dv2_outputs_free(v7_dv2_outputs_t* out);

// Compute. Inputs:
//   positions:     F64×3·n_pts (interleaved). Frame must match params->up_axis.
//   normals:       F32×3·n_pts global. Required when roof_tilt_min/max > 0;
//                  pass NULL to disable the per-point normal pre-filter.
//   pool_indices:  U32×n_pool — global indices of points in the building
//                  (= bench-v2's `building-indices` artifact). When called
//                  per-building, these are local-to-building global slots
//                  that resolve into `positions` (no remapping needed).
//                  Pass `[0..n_pts)` for tile-wide.
// Returns 0 on success, -1 on alloc failure.
int v7_dv2_compute(const double* positions,
                   const float* normals,
                   const float* point_weights,    // REQUIRED (no NULL fallback)
                   size_t n_pts,
                   const uint32_t* pool_indices,
                   size_t n_pool,
                   const v7_dv2_params_t* params,
                   v7_dv2_outputs_t* out);

#ifdef __cplusplus
}
#endif
#endif

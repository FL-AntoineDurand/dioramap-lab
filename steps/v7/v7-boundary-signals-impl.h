// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-boundary-signals — pure compute. Phase B + Phase C of bench-v2
// boundary-signals.js (meta.version=4), split out per V7 audit. Phase
// A + class filter live in v7-boundary-topology; this step consumes
// that step's TLV and adds the heavy signal layers + corner pool.
//
// SCOPE: building-local. Inputs are building-local positions + per-
// building bi-directions (beam dirs). Outputs are per-plane signal
// grids, nearby projections, and a unified corner pool (RI ridge-
// intersection corners re-derived from upstream's RIDGE_LINES +
// concave/convex anchors detected by Phase C's OA-gradient local-
// maxima scan, each carrying dirsByPlane for downstream consumers).
//
// Phase B — Nearby gathering:
//   For each surviving plane g:
//     project group points → (u, v) on plane basis
//     build a bucket index over (u, v) for fast point-in-disc tests
//   Tile-wide pass: for each tile point p with beam direction b:
//     For each surviving plane g (with normal n, d):
//       ray-cast: t = -(n·p + d) / (n·b)   (skip if t >= 0 or |denom|<ε)
//       hit = p + t·b; project to (u, v) on g's basis
//       if (u, v) within NEARBY_RADIUS of g's group: keep as nearby item
//   Output: per-plane filtered nearby list with positions + signed-
//   distance + source point index.
//
// Phase C — Per-plane signal grids:
//   For each surviving plane g:
//     computeSignalGrids on the group point projection (groupField):
//       cell-by-cell OA (open-angle) + CoM-distance + outward direction
//       + best-left/right neighbour offsets
//     computeSignalGrids on the |sd|>0.20m-filtered nearby points
//       (nearbyField, same shape as groupField)
//     balance field — blends group-CoM and nearby-CoM with point-count
//       trust weighting + opposing-direction reward
//     corner synthesis — find OA-gradient local maxima (span ≥150°,
//       5×5 neighbourhood), emit concave/convex anchor at each peak
//       with two outgoing edge directions, dedup within 0.4m
//
// RI re-derivation:
//   Walks the upstream RIDGE_LINES section, runs the same closest-
//   approach + plane-group-sharing + margin test that bench-v2
//   detectCorners() does, dedups within cornerMergeThreshold.

#ifndef V3_STEPS_V7_BOUNDARY_SIGNALS_IMPL_H
#define V3_STEPS_V7_BOUNDARY_SIGNALS_IMPL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Params ──────────────────────────────────────────────────────────

typedef struct v7_bs_params {
    double nearby_radius;           // default 2.0 m  (Phase B disc radius)
    double plane_dist_filter;       // default 0.20 m (Phase C nearby
                                    //   filter: keep items with |sd|>=this)
    double corner_dist_threshold;   // default 1.0 m  (RI closest-approach gate)
    double corner_merge_threshold;  // default 0.25 m (RI dedup radius)
    double anchor_dedup_radius;     // default 0.4 m  (concave/convex dedup)
    double anchor_valid_radius;     // default 0.5 m  (anchor validation
                                    //   "no group within R" / "no nearby within R")
    double anchor_balance_min;      // default 0.25  (anchor validation balance gate)
    double grid_cell_size;          // default 0.10 m (signal grid cell)
    double grid_eval_radius;        // default 1.0 m  (signal grid disc radius)
    double angular_span_min_deg;    // default 150.0  (corner synthesis OA-gradient threshold)
    int32_t local_max_half_window;  // default 2      (5×5 local-maxima window)
    int32_t min_plane_pts;          // default 6      (must match topology)
    int32_t up_axis;                // 1 = y-up (bench), 2 = z-up (v3 prod)
    double  lidar_density;          // pts/m² for the operating point set.
                                    //   Default is NAN. When compute_contour
                                    //   is true (default), this MUST be a
                                    //   valid positive number or the contour
                                    //   site aborts. Set per-bldg in the
                                    //   step wrapper from bldg-thresholds.
    bool    compute_contour;        // default true — opt out (set false) in
                                    //   benches that don't supply lidar_density.
                                    //   When false, no PLANE_CONTOURS data
                                    //   is emitted and lidar_density is
                                    //   unused.
} v7_bs_params_t;

void v7_bs_params_default(v7_bs_params_t* p);

// ── Inputs (per-plane subset taken from upstream PLANES) ──

typedef struct v7_bs_input_plane {
    uint32_t id;                    // upstream plane id
    bool     rejected;              // upstream rejected flag — skip Phase B/C
    double   normal[3];
    double   d;
    double   basis_e1[3];
    double   basis_e2[3];
    // group point indices come in via group_indices_per_plane.
} v7_bs_input_plane_t;

// One upstream ridge-line record needed for RI re-derivation.
typedef struct v7_bs_input_ridge_line {
    uint32_t plane_a;
    uint32_t plane_b;
    bool     has_line;
    double   line_point[3];
    double   line_direction[3];
    float    t_min_a, t_max_a, t_min_b, t_max_b;
} v7_bs_input_ridge_line_t;

// ── Outputs ────────────────────────────────────────────────────────

typedef struct v7_bs_corner_dir {
    uint32_t plane_id;
    float    dir_a[3];
    float    dir_b[3];
} v7_bs_corner_dir_t;

// CoM-direction blended bisector cache. One unit vec3 per (corner, plane),
// derived from groupField.outU/V minus nearbyField.outU/V at the anchor's
// grid cell. This is the value v2 boundary-polygons recomputes in D.4 —
// cached here so v3 v7-boundary-polygons can skip the recompute entirely
// and v7-boundary-regularize can reuse it for nudge direction. Emitted
// only for concave/convex anchors; RI corners skip.
typedef struct v7_bs_corner_com_bisector {
    uint32_t plane_id;
    float    com_bisector[3];
} v7_bs_corner_com_bisector_t;

// One pool corner. Variable-length on wire because of planeIds + dirs;
// here we keep them as separate small heap allocations.
typedef struct v7_bs_corner {
    uint32_t id;                    // assigned at pool creation
    uint8_t  source;                // 1 ridge-intersection, 4 concave, 5 convex
                                    //   (see ROOF-GRAPH-FORMAT.md §5.3)
    double   point[3];              // tile-world (wrapper shifts; impl produces local)
    uint16_t n_merged;              // RI dedup multiplicity
    char*    name;                  // owned UTF-8 (NUL-term), NULL if not assigned
                                    //   — bench-v2 nameFor(): RI<n>, P<g>C<n>,
                                    //     P<g>X<n>. Surfaced in JS via
                                    //     RG_CORNER_HAS_NAME flag bit (0x01).
    // owned arrays:
    uint32_t* plane_ids;
    uint32_t  n_plane_ids;
    uint32_t* ridge_ids;            // RI corners only (indices into upstream RIDGES)
    uint32_t  n_ridge_ids;
    // dirsByPlane: for concave/convex corners, two outgoing edge
    // directions in tile-world (rotation-invariant — wrapper does
    // NOT need to transform). Empty for RI corners.
    v7_bs_corner_dir_t* dirs;
    uint32_t  n_dirs;
    // comBisectorByPlane: cached CoM-direction blended bisector per
    // (corner, plane). Same population pattern as dirs — emitted only
    // for concave/convex anchors, empty for RI corners.
    v7_bs_corner_com_bisector_t* com_bisectors;
    uint32_t  n_com_bisectors;
} v7_bs_corner_t;

// One nearby projection (filtered |sd|>=PLANE_DIST_FILTER). Mirrors
// one record in NEARBY_PROJECTIONS section.
typedef struct v7_bs_nearby_item {
    double   position[3];           // hit point on plane
    float    signed_dist;
    uint32_t src_idx;               // tile-wide source point index
                                    //   (we keep building-local idx here;
                                    //   wrapper converts to global)
} v7_bs_nearby_item_t;

// Per-plane nearby projections.
typedef struct v7_bs_nearby_per_plane {
    uint32_t plane_id;
    v7_bs_nearby_item_t* items;     // owned
    uint32_t n_items;
} v7_bs_nearby_per_plane_t;

// Signal grid field — one of {group, nearby} per plane. Indices into
// the cell arrays are row*nCols + col. NaN signals "no data" (cell
// had no points within eval-radius). All Float32 unless typed.
typedef struct v7_bs_grid_field {
    uint32_t n_cols, n_rows;
    float    cell_size;
    float    g_min_u, g_min_v;
    // Cell arrays (each length nRows*nCols). All owned.
    float*   oa_field;              // open angle in radians
    float*   oa_span_field;         // post-pass directional gradient
    float*   com_dist_field;        // CoM-to-cell distance
    uint16_t* pt_count_field;       // count of points in disc
    float*   out_u;                 // outward unit direction (u-component)
    float*   out_v;                 // outward unit direction (v-component)
    float*   best_left_field;
    float*   best_right_field;
    float*   node_x;                // cell centre on the plane (3D world / scene)
    float*   node_y;
    float*   node_z;
} v7_bs_grid_field_t;

void v7_bs_grid_field_free(v7_bs_grid_field_t* f);

// Per-plane grid-fill topology — added 2026-05-22 for bp2's topology
// source. Adaptive marching-squares on density-weighted group rasterise.
// See v7-grid-contour.h for the algorithm. Cell geometry can differ
// from group_field (adaptive picks its own cell size).
typedef struct v7_bs_plane_contour_loop {
    float*   pts;        // 2 floats per vertex (u, v interleaved); owned
    uint32_t n_pts;
} v7_bs_plane_contour_loop_t;

typedef struct v7_bs_plane_contour {
    bool     has_grids;
    bool     has_contour;
    bool     has_nearby_grids;   // density_nearby / smoothed_nearby present
    uint32_t n_cols, n_rows;
    float    cell_size;
    float    g_min_u, g_min_v;
    float    max_density, threshold;
    // Inspection grids — emitted as f32, cast from internal f64. Owned.
    float*   density;       // [n_cols * n_rows] own-points rasterised
    float*   smoothed;      // post-Gaussian
    uint8_t* binary;        // post-ICM mask (own>threshold AND own>nearby)
    // Nearby (competing) grids — NULL when no nearby points were available.
    float*   density_nearby;
    float*   smoothed_nearby;
    // Loops — outer first, then holes. Owned.
    v7_bs_plane_contour_loop_t  outer;
    v7_bs_plane_contour_loop_t* holes;
    uint32_t                    n_holes;
} v7_bs_plane_contour_t;

void v7_bs_plane_contour_free(v7_bs_plane_contour_t* c);

// Per-plane signal grid bundle.
typedef struct v7_bs_plane_grid {
    uint32_t plane_id;
    bool     has_group_field;
    bool     has_nearby_field;
    bool     has_bal_field;
    v7_bs_grid_field_t group_field;
    v7_bs_grid_field_t nearby_field;
    float*   bal_field;             // length nRows*nCols (matches groupField geometry)
    v7_bs_plane_contour_t contour;  // grid-fill topology (see above)
} v7_bs_plane_grid_t;

void v7_bs_plane_grid_free(v7_bs_plane_grid_t* g);

typedef struct v7_bs_outputs {
    // Pool of corners (RI first, then concave/convex). IDs are
    // assigned in this order.
    v7_bs_corner_t* corners;
    size_t          n_corners;
    // Per-plane nearby projections + signal grids.
    v7_bs_nearby_per_plane_t* nearby;
    size_t                    n_nearby;       // == n_planes_alive
    v7_bs_plane_grid_t* grids;
    size_t              n_grids;              // == n_planes_alive
} v7_bs_outputs_t;

void v7_bs_outputs_init(v7_bs_outputs_t* out);
void v7_bs_outputs_free(v7_bs_outputs_t* out);

// ── Compute ─────────────────────────────────────────────────────────
//
// Inputs are BUILDING-LOCAL. Both `positions` (the building's points)
// and `beam_dirs` (per-bldg copies of bi-directions) live in the
// shifted-local frame matching what build-building-kd-trees produced;
// the wrapper does the shift before calling.
//
//   positions       F64×3·n_pts_local
//   beam_dirs       F32×3·n_pts_local  (NULL OK — Phase B skipped entirely)
//   owner           int16_t × n_pts_local  (-1 unassigned)
//   planes          v7_bs_input_plane_t × n_planes (in upstream order)
//   ridge_lines     v7_bs_input_ridge_line_t × n_ridge_lines
//   params, out
//
// Returns 0 on success, -1 on alloc failure.

// Per-bldg dv2 void hull (subset of dv2-ghost-planes record fields
// the contour algorithm needs). hull_xyz is BUILDING-LOCAL F64 coords
// of the lifted convex hull (n_hull_verts × 3 doubles).
typedef struct v7_bs_void_hull {
    uint32_t      void_id;
    uint32_t      n_hull_verts;
    const double* hull_xyz;       // borrowed; lifetime = caller's dv2 blob
} v7_bs_void_hull_t;

// Per-plane void membership (which voids each plane owns). Borrowed
// from the caller's parse of RG_SECT_PLANE_VOIDS.
typedef struct v7_bs_plane_voids {
    uint32_t        plane_id;
    uint32_t        n_void_ids;
    const uint32_t* void_ids;     // borrowed
} v7_bs_plane_voids_t;

int v7_bs_compute(const double* positions,
                  const float*  beam_dirs,
                  const float*  point_weights,    // Voronoi area per point (nullable → uniform 1.0)
                  size_t n_pts_local,
                  const int16_t* owner,
                  const v7_bs_input_plane_t* planes, size_t n_planes,
                  const v7_bs_input_ridge_line_t* ridge_lines, size_t n_ridge_lines,
                  // Void-aware grid-fill contour (optional — both NULL/0
                  // disables, contour runs from own + nearby only):
                  const v7_bs_void_hull_t*   voids,        size_t n_voids,
                  const v7_bs_plane_voids_t* plane_voids,  size_t n_plane_voids,
                  const v7_bs_params_t* params,
                  v7_bs_outputs_t* out);

#ifdef __cplusplus
}
#endif
#endif

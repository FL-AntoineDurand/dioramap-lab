// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-boundary-regularize — pure compute, R.0 + R.0b + R.1-single-corner subset.
//
// Port of bench-v2 boundary-regularize.js (meta.version=35). Coordinate-
// descent boundary regularizer. Active phases:
//   R.0   DOF tagging (per-corner DOF + freedom axes)
//   R.0b  polygon cleanup (unused / redundant corners removed)
//   R.1   single-corner coordinate descent — 5.7g.1
//     * WSD (two-sided wrong-side distance)
//     * angle (snap to canonical {45, 90, 135, 180}°)
//     * neighbor-angle
//     * horizontal-edge
//     * pairwise horiz-coplanar / vert-coplanar / collinear (R.7) — 5.7g.4
//   R.2   proximity merge (incremental, during R.1) — 5.7g.2
//   R.3   collinear removal (incremental, during R.1) — 5.7g.2
//   joint two-corner moves with mulberry32 PRNG — 5.7g.3
//
// Phases NOT yet ported:
//   * MR-fit cost term (needs MR points input artifact) — 5.7g.4.b
//
// Coordinate convention: BUILDING-LOCAL y-up — same as classify +
// wall-detect. The step wrapper handles z-up ↔ y-up swap at TLV
// boundaries (see v7-boundary-classify.c axis_zup_to_yup_d).

#ifndef V3_STEPS_V7_BOUNDARY_REGULARIZE_IMPL_H
#define V3_STEPS_V7_BOUNDARY_REGULARIZE_IMPL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "v7-boundary-corners-impl.h"
#include "v7-boundary-merge-impl.h"
#include "v7-boundary-classify-impl.h"

#ifdef __cplusplus
extern "C" {
#endif

// ── Params ────────────────────────────────────────────────────────
//
// Defaults match bench-v2 (boundary-regularize.js L36–66) for the
// terms in scope; out-of-scope params (mrFit*, hcop*, vcop*, col*,
// joint*) are not declared yet.

typedef struct v7_br_params {
    // Optimizer config
    int32_t max_iterations;             // 1000
    double  nudge_range_m;              // 0.15
    double  nudge_step_m;               // 0.03
    double  angle_snap_thresh_deg;      // 20

    // Cost term toggles (use_*) + lambdas + sigmas
    bool    use_wsd_cost;               // true
    double  lambda_wsd;                 // 1.0
    double  wsd_sigma_m;                // 1.0 (linear scale, NOT Gaussian)

    bool    use_angle_cost;             // true
    double  lambda_angle;               // 2.0
    double  angle_sigma_deg;            // 5

    bool    use_neighbor_angle_cost;    // true
    double  lambda_neighbor_angle;      // 2.0

    bool    use_horizontal_edge_cost;   // true
    double  lambda_horizontal_edge;     // 1.0
    double  horizontal_edge_sigma_deg;  // 15

    // Trust weighting on WSD points (per-point weight from residual)
    bool    use_trust_weighting;        // true

    // Canonical angle target set: false → {45,90,135,180}, true → +{30,60,120,150}
    bool    use_extended_angles;        // false

    // R.2: proximity-merge after each accepted R.1 move.
    bool    use_proximity_merge;        // true
    double  proximity_merge_dist_m;     // 0.30

    // R.3: drop single-plane corners whose polygon angle is within
    // threshold of 180° (redundant midpoints).
    bool    use_collinear_removal;      // true
    double  collinear_removal_threshold_deg; // 10

    // Joint two-corner moves: at end of each R.1 iter, sample random
    // joint offsets on every polygon edge with both endpoints
    // optimisable. seed=0 → non-reproducible (uses libc rand), nonzero
    // seed enables mulberry32 PRNG for byte-stable runs.
    bool    use_joint_moves;             // true
    int32_t joint_samples_per_edge;     // 30
    uint32_t joint_move_seed;            // 0

    // R.4: ridge edge-merge. After each accepted R.1 move, the chase
    // unifies CROSS-PLANE corner pairs that both lie on a shared interior
    // ridge into one shared corner (planes_dyn = {A,B}, DOF→1). This
    // emergently recovers the shared edges that boundary-merge used to
    // build. `ridge_merge_dist_m` is the max 3D gap between the two
    // corners; `ridge_merge_coplanar_m` is the max perpendicular distance
    // from a corner to the ridge line for it to count as "on the ridge".
    bool    use_ridge_merge;             // true
    double  ridge_merge_dist_m;          // 0.50
    double  ridge_merge_coplanar_m;      // 0.30

    // R.7: pairwise coplanarity / collinearity (edge-pair) terms.
    bool    use_horiz_coplanar_cost;     // true
    double  lambda_horiz_coplanar;       // 1.0
    double  horiz_coplanar_sigma_m;      // 0.10
    bool    use_vert_coplanar_cost;      // true
    double  lambda_vert_coplanar;        // 1.0
    double  vert_coplanar_sigma_m;       // 0.10
    bool    use_collinear_cost;          // true
    double  lambda_collinear;            // 1.0
    double  collinear_sigma_m;           // 0.10

    // WSD normalization (default false). When true, each plane's WSD coverage
    // term is divided by its owner mass Σw → weighted-mean misfit distance
    // (intensive, size-invariant) instead of the extensive Σw·d. Fixes the
    // cross-plane / cross-term size-coupling (a big plane's coverage dwarfing
    // small planes and all other terms). A PARAM (not an env var) so on/off get
    // distinct cache keys. See journal 2026-06-29-corner-slide-runaway-and-wsd-
    // coverage-audit.
    bool    wsd_normalize;               // false
    bool    corner_insert;               // true (#25 in-loop corner insertion)

    // MARGINAL compactness guard (default 0 = off). Adds lambda_compact ·
    // compactness(plane) [perim²/(4π·area), scale-invariant, unbounded, NO target]
    // to each of a corner's planes. It is a REGULARIZER, not an objective: the
    // optimizer's accept-if-cost-drops rule then rejects a move that WORSENS a
    // plane's compactness without a compensating WSD gain (the "drift into empty
    // space for nothing" class — id=136/P6C3+P0CP0, where foreign_in is silent).
    // A legit thin/L roof is NOT rounded: no move reduces its compactness without
    // uncovering points (WSD rises), so its shape is preserved. Modest weight —
    // real WSD trades (coverage) always dominate; only ~zero-WSD drift is blocked.
    double  lambda_compact;              // 0.0 (off) — weighted form (scale-coupled, kept off)

    // GATED marginal-compactness rule (the scale-free form, default off). Vetoes a
    // corner move that worsens a plane's compactness unless it improves THAT plane's
    // WSD by more than compact_gate_eps (absolute) — "don't wreck a plane's shape for
    // nothing on WSD", per-plane, applied in BOTH the single-corner and joint accept
    // paths. Retracts DOF-1 slide/over-extension drift (id=136 spike, wall over-reach)
    // that WSD's foreign_in can't see because it drifts into empty space. See
    // [[dahouet-plane-taxonomy]] for why its side-effects land only on false planes.
    bool    compact_gate;                // false
    double  compact_gate_eps;            // 0.05 (WSD units; ~one point moving ~1m)

    // Cost-shape saturator. 0=gaussian (1 - exp(-x²/2σ²)), 1=huber,
    // 2=linear, 3=quadratic-clamp. Matches v2 enum.
    int32_t cost_shape;                  // 0 = gaussian

    // R.1 acceptance strategy:
    //   0 = GLOBAL_BEST     — v2 semantics: per iter, scan all
    //                          (corner, candidate) pairs (plus joint
    //                          pairs if enabled), accept the SINGLE
    //                          best-improvement winner. R.2/R.3 chase
    //                          runs after the one accepted move.
    //   1 = GREEDY_PER_CORNER — for each corner, accept its local best
    //                          immediately and proceed to the next.
    //                          Faster wall-clock convergence; may
    //                          oscillate when cross-corner cost terms
    //                          (R.7 pairwise, neighbor angle) shift
    //                          the cost surface mid-iter.
    int32_t r1_acceptance_strategy;      // 0 = GLOBAL_BEST (default, matches v2)

    int32_t up_axis;                    // 1 = y-up (impl is hardcoded y-up)
} v7_br_params_t;

void v7_br_params_default(v7_br_params_t* p);

// ── Inputs ────────────────────────────────────────────────────────

// PLANES, CORNERS, EDGES typedef'd against earlier port types since the
// payload shapes are bit-identical.
typedef v7_bc_input_plane_t           v7_br_input_plane_t;
typedef v7_bm_input_corner_t          v7_br_input_corner_t;
typedef v7_bm_input_edge_t            v7_br_input_edge_t;
typedef v7_bm_input_polygon_path_t    v7_br_input_polygon_path_t;

// One nearby-projection record (foreign-in WSD). The signed-dist /
// src-point-idx pair lets the cost fn apply per-point trust weighting
// from the upstream point-plane residual buffer.
typedef struct v7_br_input_nearby_item {
    double   position[3];   // building-local
    float    signed_dist;
    uint32_t src_idx;       // index into positions/residual
} v7_br_input_nearby_item_t;

typedef struct v7_br_input_nearby_per_plane {
    uint32_t                       plane_id;
    v7_br_input_nearby_item_t*     items;
    uint32_t                       n_items;
} v7_br_input_nearby_per_plane_t;

// One ridge with clipped endpoint geometry — R.4 (ridge edge-merge)
// gates cross-plane corner pairs onto the crease line. `from`/`to` are
// BUILDING-LOCAL y-up (the wrapper applies the z-up→y-up swap), so they
// live in the same frame as corner positions. id/plane_a/plane_b mirror
// v7_bclass_input_ridge_t.
typedef struct v7_br_input_ridge {
    uint32_t id;
    uint32_t plane_a;
    uint32_t plane_b;
    uint8_t  type;        // on-disk rg_ridge_record_t.type: 1=ridge 2=hip
                          // 3=valley 4=coplanar 5=step. make-shared (I1, §9)
                          // allow-lists {ridge,hip,valley} only; step=roof-wall
                          // and coplanar=degenerate are excluded.
    double   from[3];     // building-local y-up
    double   to[3];       // building-local y-up
} v7_br_input_ridge_t;

// ── Outputs ───────────────────────────────────────────────────────

// One per-corner DOF record (R.0 output).
//
// `alive` reflects survival through R.0b / R.2 / R.3 — false means the
// corner was dropped (unused, redundant midpoint, proximity-merged, or
// collinear-removed). Consumers should typically filter to alive=true
// before comparing against v2's post-regularize roofGraph.corners.
typedef struct v7_br_dof_record {
    uint32_t corner_id;
    uint8_t  dof;                  // 0 / 1 / 2
    bool     has_axis_a;
    bool     has_axis_b;
    bool     alive;
    double   axis_a[3];            // ridge dir (DOF=1) or plane-tangent-1 (DOF=2)
    double   axis_b[3];            // plane-tangent-2 (DOF=2 only)
} v7_br_dof_record_t;

// Updated corner position after R.1.
typedef struct v7_br_corner_pos {
    uint32_t corner_id;
    double   point[3];             // building-local, y-up
} v7_br_corner_pos_t;

// Post-merge plane set for one corner (R.2/R.4 union). Lets the wrapper
// re-emit CORNERS with the grown plane_ids so downstream sees the shared
// (n_plane_ids>=2) corners that R.4 created on interior ridges. Parallel
// to corner_positions / corners_in.
typedef struct v7_br_corner_planes {
    uint32_t  corner_id;
    uint32_t* plane_ids;           // owned; NULL if n==0
    uint32_t  n_plane_ids;
} v7_br_corner_planes_t;

// Per-corner trajectory of accepted positions (for viz / debug). One
// record per corner that moved.
typedef struct v7_br_trajectory_step {
    double point[3];
} v7_br_trajectory_step_t;

typedef struct v7_br_trajectory {
    uint32_t                  corner_id;
    v7_br_trajectory_step_t*  steps;
    uint32_t                  n_steps;
} v7_br_trajectory_t;

// Post-R.0b updated polygon path (only emitted if changed by R.0b).
typedef struct v7_br_polygon_path {
    uint32_t  plane_id;
    uint32_t* corner_ids;
    uint32_t  n_corners;
} v7_br_polygon_path_t;

typedef struct v7_br_stats {
    uint32_t n_corners_total;
    uint32_t dof0;
    uint32_t dof1;
    uint32_t dof2;
    uint32_t unused_removed;
    uint32_t redundant_ridge_removed; // LEGACY alias of n_drop_ridge — retained only
                                      // for the iso-equivalence.py JSON consumer (only-bench)
    uint32_t r1_iterations;
    uint32_t r1_total_moves;
    uint32_t r1_corners_optimisable;
    uint32_t r2_merged_count;         // LEGACY alias of n_merge_corner (same-plane part) — iso-equivalence.py
    uint32_t r3_collinear_removed;    // LEGACY alias of n_drop_colinear — iso-equivalence.py
    uint32_t r1_joint_moves;
    uint32_t xplane_merged;           // MERGE_CORNER_XPLANE (cross-plane weld, T5) firings
    uint32_t ridges_with_xplane_merge;// distinct ridges with >=1 cross-plane weld
    uint32_t shared_corners;          // alive corners with planes_of_n>=2 at end

    // Make-shared per-op counters — the §4.0 five-op vocabulary, ALL now built:
    //   n_merge_corner  ← MERGE_CORNER     (R.2 same-plane + cross-plane weld T5)
    //   n_split_share   ← SPLIT_TO_SHARE_CORNER (T7)
    //   n_drop_ridge    ← DROP_REDUNDANT_ON_RIDGE (T6: pre-loop + in-loop)
    //   n_drop_colinear ← DROP_COLINEAR    (R.3)
    uint32_t n_merge_corner;
    uint32_t n_split_share;
    uint32_t n_drop_ridge;
    uint32_t n_drop_colinear;

    // Read-only make-shared PREVIEW tally (the post-loop census, §9 M1 guard):
    //   n_would_merge ← cond-B would-share (a B corner within τ_partner on R) that
    //                   MERGE didn't already consume. Mutates nothing — a yield preview.
    // (n_split_share above is the REAL T7 op count, no longer a preview alias.)
    uint32_t n_would_merge;
} v7_br_stats_t;

typedef struct v7_br_outputs {
    v7_br_dof_record_t*    dof;                size_t n_dof;
    v7_br_corner_pos_t*    corner_positions;   size_t n_corner_positions;
    v7_br_corner_planes_t* corner_planes;      size_t n_corner_planes;  // post-merge plane sets, parallel to corner_positions
    v7_br_polygon_path_t*  polygon_paths;      size_t n_polygon_paths;  // only changed paths
    v7_br_trajectory_t*    trajectories;       size_t n_trajectories;   // only moved corners
    v7_br_stats_t          stats;
} v7_br_outputs_t;

void v7_br_outputs_init(v7_br_outputs_t* o);
void v7_br_outputs_free(v7_br_outputs_t* o);

// ── Compute ───────────────────────────────────────────────────────
//
// All coordinates BUILDING-LOCAL y-up. positions is per-bldg F32×3·N
// y-up; owner is per-bldg I16×N. nearby is one entry per plane id
// present (planes without nearby items just don't have a record).
//
// ACSL (audit, EVA-only). Same nullable-when-count-0 shape as the other v7
// compute entries — and the impl already codes the matching defensive
// guards (`n_X > 0 && !X → -1`). point_plane_residual is genuinely optional
// (NULL ⇒ trust=1). owner / positions are read-only inputs. Determinism is
// preserved by params->joint_move_seed feeding the mulberry32 PRNG (no
// global / time state). result ∈ {0,-1}.
/*@
  requires \valid_read(params);
  requires \valid(out);
  requires n_planes        == 0 || \valid_read(planes        + (0 .. n_planes - 1));
  requires n_corners       == 0 || \valid_read(corners_in    + (0 .. n_corners - 1));
  requires n_edges         == 0 || \valid_read(edges         + (0 .. n_edges - 1));
  requires n_polygon_paths == 0 || \valid_read(polygon_paths + (0 .. n_polygon_paths - 1));
  requires n_ridges        == 0 || \valid_read(ridges        + (0 .. n_ridges - 1));
  requires n_nearby        == 0 || \valid_read(nearby        + (0 .. n_nearby - 1));
  requires n_points        == 0 || \valid_read(positions     + (0 .. 3 * n_points - 1));
  requires n_points        == 0 || \valid_read(owner         + (0 .. n_points - 1));
  requires point_plane_residual == \null || n_points == 0
                              || \valid_read(point_plane_residual + (0 .. n_points - 1));
  assigns *out;
  ensures \result == 0 || \result == -1;
*/
int v7_br_compute(
    const v7_br_input_plane_t* planes, size_t n_planes,
    const v7_br_input_corner_t* corners_in, size_t n_corners,
    const v7_br_input_edge_t* edges, size_t n_edges,
    const v7_br_input_polygon_path_t* polygon_paths, size_t n_polygon_paths,
    const v7_br_input_ridge_t* ridges, size_t n_ridges,
    const v7_br_input_nearby_per_plane_t* nearby, size_t n_nearby,
    const float* positions, size_t n_points,
    const int16_t* owner,
    const float* point_plane_residual,  // may be NULL → trust=1 everywhere
    double point_plane_dist_threshold,
    const v7_br_params_t* params,
    v7_br_outputs_t* out);

#ifdef __cplusplus
}
#endif
#endif

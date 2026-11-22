// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-boundary-topology — pure compute. Phase A of bench-v2
// boundary-signals.js (meta.version=4), split out per V7 audit:
//
//   - adjacency graph (kNN cross-plane contact counting)
//   - ridge-line fits (per pair: intersect grm-equations, project
//     group points for tMin/tMax, weak-overlap + sparse-contact
//     filter, edge-type classify, ridge-elevation trim, side signs)
//   - class filter (plane rejection: gvw>50% with ridge-neighbor
//     rescue when any non-rejected neighbour exists)
//   - basis derivation per surviving plane
//
// SCOPE: building-local. positions are F64×3·n_pts_local; owner is
// per-bldg Int16 (-1 unassigned); seed_planes are post-GRM plane
// equations (trusted — we do NOT re-fit, audit policy "minimize
// duplication"). The wrapper concatenates per-building outputs
// into one ARRAY_INLINE-of-TLV artifact for the tile.
//
// What this DOESN'T do (deferred to v7-boundary-signals):
//   - per-plane signal grids (OA / CoM / balance)
//   - nearby projections (beam-project tile points → plane)
//   - corner pool (RI corners + concave/convex candidates)
//
// Vestigial v2 reads dropped per audit:
//   - curvatures-k32, linearity   (reserved-only params in
//     boundary-edges/ridges.js — never read in body)
//   - boundary-candidates-all/candidates (destructured but never
//     referenced)
//   - point-weights for plane fit  (we trust grm-seed-planes
//     equations, no re-fit)
//   - kd-tree (tile-wide) — v3 uses building-kd-trees per-bldg

#ifndef V3_STEPS_V7_BOUNDARY_TOPOLOGY_IMPL_H
#define V3_STEPS_V7_BOUNDARY_TOPOLOGY_IMPL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kd_tree.h"

#ifdef __cplusplus
extern "C" {
#endif

// ── Params ──────────────────────────────────────────────────────────

typedef struct v7_topo_params {
    double adjacency_radius;         // default 0.5 m (kNN proximity radius)
    int32_t min_contact_pts;         // default 3   (min cross-group contacts to keep pair)
    double class_filter_threshold;   // default 0.50 (gvw rejection fraction)
    int32_t min_plane_pts;           // default 6 (planes with fewer pts ignored)
    double spacing;                  // average point spacing (m); 1/sqrt(density)
    int32_t up_axis;                 // 1 = y-up (bench), 2 = z-up (v3 prod LAZ)
} v7_topo_params_t;

void v7_topo_params_default(v7_topo_params_t* p);

// ── Inputs ──────────────────────────────────────────────────────────

// One seed plane as the topology step sees it (post-GRM authoritative).
typedef struct v7_topo_input_seed {
    double normal[3];               // unit; oriented up along params.up_axis
    double d;                       // n·p + d = 0
    bool   tombstoned;              // true if dissolved during GRM merge
} v7_topo_input_seed_t;

// ── Outputs (mirror RG_SECT_PLANES / RIDGES / RIDGE_LINES) ──────────

typedef struct v7_topo_plane {
    uint32_t id;                    // grm plane group id
    bool     rejected;
    uint32_t rejection_reason;      // 0=none, 1=gvw>50%, 2=class-filter,
                                    // 3=low-nPts, 4=tombstoned
    double   centroid[3];           // weighted centroid of group points (unit weights)
    double   normal[3];             // copied from input seed
    double   d;                     // copied from input seed
    uint32_t n_pts;
    double   basis_e1[3];
    double   basis_e2[3];
    bool     has_basis;
    uint16_t tilt_deg;              // acos(|n_up|) * 180/π, rounded
    // class stats (only set when classifications available, else all zero)
    bool     has_class_stats;
    uint32_t cs_total, cs_building, cs_ground;
    uint32_t cs_low_veg, cs_med_veg, cs_high_veg, cs_water, cs_other;
} v7_topo_plane_t;

typedef struct v7_topo_ridge {
    uint32_t id;
    uint32_t plane_a;
    uint32_t plane_b;
    uint8_t  type;                  // see ROOF-GRAPH-FORMAT.md §5.2:
                                    //   1=ridge, 2=hip, 3=valley,
                                    //   4=coplanar, 5=step, 6=eave
    double   from[3];               // clipped start point
    double   to[3];                 // clipped end point
    double   length;
    uint32_t contact_count;
    float    overlap_ratio;
} v7_topo_ridge_t;

typedef struct v7_topo_ridge_line {
    uint32_t ridge_id;
    bool     has_line;
    // Per-plane interior-oriented side axes for boundary-corners' D.2b
    // wrong-side filter. side_vec_a is perpendicular to the ridge, in plane
    // A's surface, oriented INTO A's body; side_vec_b ditto for plane B. Each
    // is valid iff its has_side_* bit is set (cross product non-degenerate AND
    // a definite interior side was sampled). The kill test is sign(side_vec_P ·
    // (corner − line_point)) < 0. Replaces the old single side_vec (A's frame
    // only) + side_sign_a/b — see 2026-06-11-d2b-side-vec-wrong-plane-frame.
    bool     has_side_a, has_side_b;
    double   line_point[3];
    double   line_direction[3];
    float    t_min_a, t_max_a, t_min_b, t_max_b;
    float    side_vec_a[3], side_vec_b[3];
} v7_topo_ridge_line_t;

typedef struct v7_topo_outputs {
    v7_topo_plane_t*       planes;
    size_t                 n_planes;
    v7_topo_ridge_t*       ridges;
    size_t                 n_ridges;
    v7_topo_ridge_line_t*  ridge_lines;   // length = n_ridges, parallel
} v7_topo_outputs_t;

void v7_topo_outputs_init(v7_topo_outputs_t* out);
void v7_topo_outputs_free(v7_topo_outputs_t* out);

// ── Compute ─────────────────────────────────────────────────────────
//
// Inputs are building-local. `tree` MUST be a kd-tree built over the
// SAME `positions` array, length n_pts_local.
//
//   positions       F64×3·n_pts_local                          (building-local)
//   owner           int16_t × n_pts_local                       (grm-owner; -1 = unassigned)
//   classes_local   uint8_t × n_pts_local  (NULL → skip class filter)
//   seeds           v7_topo_input_seed_t × n_seeds              (post-GRM)
//   tree            kd_tree_t                                   (over `positions`)
//
// Returns 0 on success, -1 on alloc failure. Caller frees via
// v7_topo_outputs_free.

int v7_topo_compute(const double* positions,
                    size_t n_pts_local,
                    const int16_t* owner,
                    const uint8_t* classes_local,
                    const v7_topo_input_seed_t* seeds,
                    size_t n_seeds,
                    const kd_tree_t* tree,
                    const v7_topo_params_t* params,
                    v7_topo_outputs_t* out);

#ifdef __cplusplus
}
#endif
#endif

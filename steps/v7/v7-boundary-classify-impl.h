// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-boundary-classify — pure compute. Port of bench-v2
// boundary-classify.js (Phase F perimeter edge classification, extracted
// from the legacy boundary-3d monolith).
//
// Operates on the post-merge roof-graph. Topology and corner positions
// are fixed; this step assigns each edge a perimeter tag + coverage
// detail + outward XZ vector, and records new plane-pair adjacencies
// discovered via eave-sharing.
//
// Phases:
//   A   Build roofPlaneById + poolById maps.
//   B   Per non-wall plane: signed-area winding + XZ centroid (used
//       to disambiguate outward-direction from edge perpendiculars).
//   C   Collect edge geometry: 3D dir, XZ dir, midpoint, length.
//   D   For each edge:
//         - wall-only          → tag = wall
//         - multi-plane all-roof → tag = interior-shared
//         - shares ridge between two roof planes → tag = interior-shared
//         - else: candidate-loop over other-plane edges
//             3D collinear+close → coverage type 'shared'
//             else XZ collinear+close → 'overhang-top' / 'overhang-bottom'
//           Merge coverages, tag (shared > overhang-top > overhang-bottom),
//           record eave-shared adjacencies for shared coverages.
//   E   Deduplicate new adjacencies against existing ridges.
//   F   Level-transition detection: perimeter edges that share a wall
//       plane with 2+ different roof planes → retag as level-transition.
//   G   PIP fallback for overhangs: for remaining 'perimeter' edges,
//       walk along the edge, probe outward at 15cm offset in 10cm
//       steps, check XZ containment in other-plane polygons; if
//       ≥COVERAGE_THRESHOLD probes are covered → overhang-top/bottom.
//
// Outputs are sized by edge count; per-edge `coverages[]` is owned.

#ifndef V3_STEPS_V7_BOUNDARY_CLASSIFY_IMPL_H
#define V3_STEPS_V7_BOUNDARY_CLASSIFY_IMPL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "v7-boundary-corners-impl.h"  // input plane/corner typedefs
#include "v7-boundary-merge-impl.h"    // input edge / polygon_path typedefs

#ifdef __cplusplus
extern "C" {
#endif

// ── Tags ──────────────────────────────────────────────────────────
//
// On-wire codes match ROOF-GRAPH-FORMAT.md §5 (PERIMETER_TAGS.tag).

typedef enum {
    V7_BCLASS_TAG_UNSET            = 0,  // no entry — reader sees absent edge
    V7_BCLASS_TAG_PERIMETER        = 1,
    V7_BCLASS_TAG_SHARED           = 2,
    V7_BCLASS_TAG_OVERHANG_TOP     = 3,
    V7_BCLASS_TAG_OVERHANG_BOTTOM  = 4,
    V7_BCLASS_TAG_INTERIOR_SHARED  = 5,
    V7_BCLASS_TAG_WALL             = 6,
    V7_BCLASS_TAG_LEVEL_TRANSITION = 7,
} v7_bclass_tag_t;

// ── Params ────────────────────────────────────────────────────────
//
// Defaults mirror bench-v2 constants (boundary-classify.js L55–61).

typedef struct v7_bclass_params {
    double perim_dist_3d;            // 0.30 m — max 3D edge-edge dist for "shared"
    double perim_dist_xz;            // 0.30 m — max XZ midpoint dist for "overhang"
    double perim_collinear_min_deg;  // 15°    — collinearity gate
    double perim_min_overlap_m;      // 0.30 m — min coverage length to count
    double wall_tilt_min_deg;        // 72°    — wall classification cutoff
    double pip_probe_offset_m;       // 0.15 m
    double pip_step_len_m;           // 0.10 m
    double pip_coverage_threshold;   // 0.50
    double outward_side_eps;         // 0.05 — F-on-inward-side rejection
    double min_edge_len_3d;          // 0.01 m — degenerate-edge cutoff
    double min_edge_len_xz;          // 0.01 m — degenerate-XZ cutoff
    int32_t up_axis;                 // 1 = y-up, 2 = z-up (v3 default)
} v7_bclass_params_t;

void v7_bclass_params_default(v7_bclass_params_t* p);

// ── Inputs (typedef'd against existing port types) ────────────────

typedef v7_bc_input_plane_t      v7_bclass_input_plane_t;
typedef v7_bm_input_corner_t     v7_bclass_input_corner_t;
typedef v7_bm_input_edge_t       v7_bclass_input_edge_t;
typedef v7_bm_input_polygon_path_t v7_bclass_input_polygon_path_t;

// One ridge node (id + plane_a + plane_b) — needed for the
// "both corners share a ridge between two roof planes → interior-shared"
// gate. Other ridge fields aren't consulted by classify.
typedef struct v7_bclass_input_ridge {
    uint32_t id;
    uint32_t plane_a;
    uint32_t plane_b;
} v7_bclass_input_ridge_t;

// ── Outputs ───────────────────────────────────────────────────────

// One coverage interval along an edge's t-axis (0..1).
typedef struct v7_bclass_coverage {
    float    t0, t1;
    uint8_t  type;            // v7_bclass_tag_t (shared / overhang-* only)
    uint32_t other_plane_id;
    uint32_t other_edge_id;
} v7_bclass_coverage_t;

// One per-edge classification record. Parallel to input edges[].
// `tag == UNSET` means the edge was skipped (degenerate geometry).
typedef struct v7_bclass_edge {
    uint32_t edge_id;
    uint8_t  tag;             // v7_bclass_tag_t
    bool     has_outward_xz;
    float    outward_xz[2];   // outward_x, outward_z (only valid when has_outward_xz)
    // Coverage detail. Empty for tags that didn't run the pair-matcher
    // (wall / interior-shared) or for perimeter edges with no candidates.
    v7_bclass_coverage_t* coverages;
    uint32_t n_coverages;
    float    covered_fraction;
    // PIP-overhang detail (set by Phase G; absent => has_pip_overhang=false).
    bool     has_pip_overhang;
    float    pip_coverage;
    uint32_t pip_other_plane_id;
} v7_bclass_edge_t;

// Plane-pair adjacency synthesized by classify (eave-shared, etc).
typedef struct v7_bclass_new_adjacency {
    uint32_t plane_a;
    uint32_t plane_b;
    uint8_t  type;            // 1 = eave-shared
    uint32_t edge_id_a;
    uint32_t edge_id_b;
} v7_bclass_new_adjacency_t;

typedef struct v7_bclass_stats {
    uint32_t perimeter;
    uint32_t shared;
    uint32_t overhang_top;
    uint32_t overhang_bottom;
    uint32_t interior_shared;
    uint32_t wall;
    uint32_t level_transition;
    uint32_t overhang_top_pip;
    uint32_t overhang_bottom_pip;
    uint32_t new_adjacencies_after_dedup;
} v7_bclass_stats_t;

typedef struct v7_bclass_outputs {
    v7_bclass_edge_t*           edges;            // parallel to input edges[]
    size_t                       n_edges;
    v7_bclass_new_adjacency_t*  new_adjacencies;
    size_t                       n_new_adjacencies;
    v7_bclass_stats_t            stats;
} v7_bclass_outputs_t;

void v7_bclass_outputs_init(v7_bclass_outputs_t* o);
void v7_bclass_outputs_free(v7_bclass_outputs_t* o);

// ── Compute ───────────────────────────────────────────────────────
//
// All coordinates BUILDING-LOCAL. No tile-wide point reads; the step
// works purely on the post-merge roof-graph.
//
// `corners` is the unified pool (post-merge). `polygon_paths` carry
// per-plane corner-id cycles; classify uses them only for winding +
// centroid + PIP polygon-containment.
//
// ACSL (audit, EVA-only). Same shape as v7_bc_compute: each
// `n_X == 0 || \valid_read(X + (0..n_X-1))` allows a NULL pointer when its
// count is 0; up_axis ∈ {1,2}; result ∈ {0,-1}. The impl additionally
// fail-returns -1 on any NULL planes/corners/edges/params/out, so those
// stay safe even below the count-0 clause.
/*@
  requires \valid_read(params);
  requires \valid(out);
  requires params->up_axis == 1 || params->up_axis == 2;
  requires n_planes        == 0 || \valid_read(planes        + (0 .. n_planes - 1));
  requires n_corners       == 0 || \valid_read(corners       + (0 .. n_corners - 1));
  requires n_edges         == 0 || \valid_read(edges         + (0 .. n_edges - 1));
  requires n_polygon_paths == 0 || \valid_read(polygon_paths + (0 .. n_polygon_paths - 1));
  requires n_ridges        == 0 || \valid_read(ridges        + (0 .. n_ridges - 1));
  assigns *out;
  ensures \result == 0 || \result == -1;
*/
int v7_bclass_compute(const v7_bclass_input_plane_t* planes, size_t n_planes,
                       const v7_bclass_input_corner_t* corners, size_t n_corners,
                       const v7_bclass_input_edge_t* edges, size_t n_edges,
                       const v7_bclass_input_polygon_path_t* polygon_paths, size_t n_polygon_paths,
                       const v7_bclass_input_ridge_t* ridges, size_t n_ridges,
                       const v7_bclass_params_t* params,
                       v7_bclass_outputs_t* out);

#ifdef __cplusplus
}
#endif
#endif

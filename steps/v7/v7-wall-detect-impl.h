// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-wall-detect — pure compute. Port of bench-v2 wall-detect.js
// (meta.version=4). Detects building walls behind perimeter and
// level-transition edges using:
//   1) beam-geometry overhang lower bound (per-point beam back-projection),
//   2) existing wall plane merge (nearby tilt ≥ 72° planes),
//   3) new wall-plane emission with 4 corners + polygon path,
//   4) unassigned-point grab (owner === -1 + perp-dist gate).
//
// Reads the post-classify roof-graph for edge.outwardXZ + perimeter tag;
// no v3 spec change — `plane.source` field from v2 is dropped (audit
// confirmed dead — written nowhere except wall-detect itself, read by
// nobody in v2 main path).
//
// MUTATING step: re-emits PLANES (update merged + append new), CORNERS
// (append 4 per wall edge), POLYGON_PATHS (replace/append per wall).
// Also mutates the owner array (separate artifact, not in roof-graph).

#ifndef V3_STEPS_V7_WALL_DETECT_IMPL_H
#define V3_STEPS_V7_WALL_DETECT_IMPL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "v7-boundary-corners-impl.h"  // v7_bc_input_plane_t etc
#include "v7-boundary-merge-impl.h"    // edge typedef
#include "v7-boundary-classify-impl.h" // perimeter tag enum + ridge typedef

#ifdef __cplusplus
extern "C" {
#endif

// ── Params ────────────────────────────────────────────────────────
//
// Defaults mirror bench-v2 constants (wall-detect.js L49–64).

typedef struct v7_wd_params {
    double search_width_m;         // 2.0  — half-width of xz strip
    double search_length_buffer_m; // 1.0  — extension along edge dir
    double overhang_cap_m;         // 1.5  — hard cap on wall inset
    double merge_angle_deg;        // 10   — max angle for plane merge
    double merge_offset_m;         // 0.30 — UNUSED in v2 algorithm (kept for parity)
    double point_match_dist_m;     // 0 == "use ppThresh" (consumer interprets)
    double min_beam_downness;      // 0.05 — min |by| for usable witness beam
    double y_top_buffer_m;         // 0.10 — yTop offset for "below" test
    double wall_tilt_min_deg;      // 72   — tilt threshold for existing-wall filter
    double search_outward_tolerance_m; // 0.10 — slight-outward slack for plane merge
    double overhang_cap_search_buffer_m; // 0.30 — extra inwardDist allowance on merge
    double point_match_y_pad_m;     // 0.10 — y range pad in point-grab phase
    double point_match_xz_pad_m;    // 0.05 — xz bbox pad in point-grab phase
    double point_in_footprint_eps;  // 0.05 — P_p ≤ this rejects point as witness
    double min_edge_len_3d;         // 0.10 — drop edges below this
    double min_edge_len_xz;         // 0.01 — drop edges with no horiz component
    double min_existing_wall_normal_xz; // 0.5 — existing-wall filter (normal must be vertical)
    int32_t up_axis;               // 1 = y-up (v2 default), 2 = z-up
} v7_wd_params_t;

void v7_wd_params_default(v7_wd_params_t* p);

// ── Inputs ────────────────────────────────────────────────────────

typedef v7_bc_input_plane_t       v7_wd_input_plane_t;
typedef v7_bm_input_corner_t      v7_wd_input_corner_t;
typedef v7_bm_input_edge_t        v7_wd_input_edge_t;
typedef v7_bclass_edge_t          v7_wd_input_perim_tag_t;

// ── Outputs ───────────────────────────────────────────────────────

// One wall plane (new or merged). For merged planes, `existing_plane_id`
// is set to the merged plane's id; the wrapper updates that plane's
// centroid/d/polygonPath in the output TLV. For new planes,
// `existing_plane_id == UINT32_MAX` and a fresh id has been allocated.
typedef struct v7_wd_wall_plane {
    uint32_t id;                  // assigned id (existing or fresh)
    uint32_t existing_plane_id;   // UINT32_MAX if new, else the merged plane id
    double   centroid[3];         // building-local
    double   normal[3];           // unit, building-local; ny = 0 for walls
    double   d;                   // building-local plane equation
    double   tilt_deg;            // 90 for walls
    double   area_m2;             // wall-width × wall-height
    uint32_t polygon_corner_ids[4]; // top_a, top_b, bot_b, bot_a in winding order
    // Provenance for the wrapper / debug:
    uint32_t source_edge_id;
    uint8_t  source_perim_tag;    // V7_BCLASS_TAG_PERIMETER or _LEVEL_TRANSITION
    bool     is_merged;
} v7_wd_wall_plane_t;

// One newly-allocated wall corner (4 per wall plane).
typedef struct v7_wd_wall_corner {
    uint32_t id;
    uint32_t plane_id;   // points back at the wall plane id
    double   point[3];   // building-local
    char     name[16];   // "W{plane}_{topA|topB|botA|botB}" zero-padded
} v7_wd_wall_corner_t;

// Owner mutations (one entry per point that got reassigned by point-grab).
// Compact representation — most points won't be touched.
typedef struct v7_wd_owner_change {
    uint32_t point_idx;
    int32_t  old_owner;   // typically -1 (unassigned)
    int32_t  new_owner;   // wall plane id
} v7_wd_owner_change_t;

// Per-edge diagnostics (mirrors v2's debug[] array).
typedef struct v7_wd_debug_entry {
    uint32_t edge_id;
    uint8_t  tag;                 // perimeter / level-transition
    bool     skipped;             // true if edge was dropped (geometry / no outwardXZ / etc.)
    double   delta_min_m;
    enum {
        V7_WD_DELTA_NAIVE       = 0,
        V7_WD_DELTA_BEAM_BOUND  = 1,
        V7_WD_DELTA_EXISTING_WALL = 2,
    } delta_source;
    uint32_t witness_count;
    uint32_t merged_with_plane_id;   // UINT32_MAX if not merged
    uint32_t new_plane_id;           // UINT32_MAX if merged
    uint32_t points_grabbed;
    double   y_top;
    double   y_bottom;
} v7_wd_debug_entry_t;

// Optional 4-edge polygon outline per wall (viz-only).
typedef struct v7_wd_wall_viz_edge {
    double from[3];
    double to[3];
    uint32_t plane_id;
    uint8_t  source_perim_tag;
} v7_wd_wall_viz_edge_t;

typedef struct v7_wd_outputs {
    v7_wd_wall_plane_t*   wall_planes;        size_t n_wall_planes;
    v7_wd_wall_corner_t*  wall_corners;       size_t n_wall_corners;
    v7_wd_owner_change_t* owner_changes;      size_t n_owner_changes;
    v7_wd_debug_entry_t*  debug;              size_t n_debug;
    v7_wd_wall_viz_edge_t* viz_edges;         size_t n_viz_edges;
    // Aggregate stats
    uint32_t n_walls_created;
    uint32_t n_walls_merged;
    uint32_t n_edges_skipped;
    uint32_t n_points_grabbed;
} v7_wd_outputs_t;

void v7_wd_outputs_init(v7_wd_outputs_t* o);
void v7_wd_outputs_free(v7_wd_outputs_t* o);

// ── Compute ───────────────────────────────────────────────────────
//
// Coordinates BUILDING-LOCAL. `owner_inout` is modified in place.
// `beam_directions` may be NULL — falls back to nadir (0,-1,0) per point.
//
// `perim_tags` is the per-edge classification overlay (same indexing
// as `edges`; tag UNSET means no entry — edge is skipped). The new
// outwardXZ vector lives on perim_tags[i].outward_xz when
// has_outward_xz is set; the wall-detect compute requires it for
// perimeter + level-transition edges.
//
// ACSL (audit, EVA-only). Same nullable-when-count-0 shape as the other
// v7 compute entries; up_axis ∈ {1,2}; result ∈ {0,-1}. beam_directions
// is genuinely optional (NULL ⇒ nadir fallback). owner_inout is the one
// in/out buffer — read AND written over [0, n_points). The impl
// fail-returns -1 on any other NULL pointer, so those stay safe below
// the count-0 clauses.
/*@
  requires \valid_read(params);
  requires \valid(out);
  requires params->up_axis == 1 || params->up_axis == 2;
  requires n_planes     == 0 || \valid_read(planes     + (0 .. n_planes - 1));
  requires n_corners    == 0 || \valid_read(corners    + (0 .. n_corners - 1));
  requires n_edges      == 0 || \valid_read(edges      + (0 .. n_edges - 1));
  requires n_perim_tags == 0 || \valid_read(perim_tags + (0 .. n_perim_tags - 1));
  requires n_points     == 0 || \valid_read(positions  + (0 .. 3 * n_points - 1));
  requires n_points     == 0 || \valid(owner_inout + (0 .. n_points - 1));
  requires beam_directions == \null || n_points == 0
                           || \valid_read(beam_directions + (0 .. 3 * n_points - 1));
  assigns *out;
  assigns owner_inout[0 .. n_points - 1];
  ensures \result == 0 || \result == -1;
*/
int v7_wd_compute(
    const v7_wd_input_plane_t* planes, size_t n_planes,
    const v7_wd_input_corner_t* corners, size_t n_corners,
    const v7_wd_input_edge_t* edges, size_t n_edges,
    const v7_wd_input_perim_tag_t* perim_tags, size_t n_perim_tags,
    const float* positions, size_t n_points,
    const float* beam_directions,  // may be NULL
    int32_t* owner_inout,
    double point_plane_dist_threshold,
    const v7_wd_params_t* params,
    v7_wd_outputs_t* out);

#ifdef __cplusplus
}
#endif
#endif

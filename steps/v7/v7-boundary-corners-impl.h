// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-boundary-corners — pure compute. Port of bench-v2 boundary-
// corners.js, Phase D of the legacy boundary-3d pipeline. Reads the
// roof-graph emitted by v7-boundary-signals (PLANES + RIDGES + RIDGE_
// LINES + CORNERS (pre-pool RI + concave/convex with dirsByPlane) +
// PLANE_GRIDS) and produces:
//
//   - The unified corner pool (RI + ridge-end + concave + convex +
//     coplanar — wall-bbox enters as concave). Each pool corner
//     carries planeIds, ridgeIds, dirsByPlane, name, n_merged.
//
//   - The per-corner member decomposition (inline `members[]` block
//     inside the CORNERS section — §4.6), so boundary-regularize can
//     re-derive degrees of freedom on each anchor that contributed
//     to the pool entry (the v2 authority/axis machinery). Each
//     member carries its origin plane_id so boundary-polygons D.3
//     NMS can pick the per-plane anchor position (v2 planeAnchors[g]
//     semantics).
//
// SCOPE: building-local frame, like v7-boundary-signals-impl. The
// wrapper unshifts upstream tile-world coords into building-local
// before calling here, and re-shifts emitted pool coords back to
// tile-world afterwards.
//
// Pass 1 phases (per-plane loop, in this order):
//   D.0   Wall-bbox corners — for vertical planes (tilt ≥ 72°), 4
//         synthetic concave corners from the group's (h,v) bbox.
//   D.1a  RI anchor enrichment — fold the per-plane RI corners out
//         of the seed pool into local anchors with dirs derived
//         from the ridge midpoint vectors. ridgeCovered tracks
//         which endpoints are already covered.
//   D.1b  Ridge-end corners — for each ridge endpoint not covered
//         by an RI corner, sample (gField outU/outV − nField outU/
//         outV) for a CoM direction; reflect ridgeDir across that
//         direction to get the second outgoing tangent.
//   D.2   Ridge snap — for each non-ridge anchor with a ridge within
//         SNAP_RADIUS (0.30 m), snap to the closest point.
//   D.2b  Ridge-side filter — kill convex anchors on the wrong side
//         of any ridge (sideVec dot test, within RIDGE_SIDE_MAX_DIST
//         = 1.5 m).
//
// Then registerAnchorInPool() walks all surviving anchors and either
// hooks them into an existing pool entry within POOL_MERGE_DIST
// (0.20 m of any member) — super-merging multiple pool entries when
// the anchor straddles them — or starts a new entry.
//
// Coplanar import — second per-plane sweep: for each foreign corner
// (any pool entry not already an anchor of g), if it's within
// COPLANAR_DIST_THRESHOLD (0.12 m) of plane g AND the balField at
// its (u,v) sample is ≥ COPLANAR_BALANCE_MIN (0.3), it's projected
// onto g and registered as a 'coplanar' anchor — extending the
// foreign pool entry's planeIds.
//
// Differences from v2:
//   - useMrAnchors is GONE — MR points are an edge slide signal for
//     boundary-regularize, not corners; they're emitted by an
//     upstream step that boundary-regularize will own.
//   - PLANE_ANCHORS section is GONE (redundant with CORNERS).
//   - mrPointsByPlane is GONE (handled in boundary-regularize).

#ifndef V3_STEPS_V7_BOUNDARY_CORNERS_IMPL_H
#define V3_STEPS_V7_BOUNDARY_CORNERS_IMPL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Params ──────────────────────────────────────────────────────────

typedef struct v7_bc_params {
    double snap_radius;             // default 0.30  m  (D.2 ridge snap)
    double wall_tilt_min_deg;       // default 72.0  °  (D.0 wall plane gate)
    double wall_side_margin;        // default 0.30  m  (D.0 wall corner side band)
    double wall_min_h_extent;       // default 0.50  m  (D.0 wall bbox minimum width)
    double pool_merge_dist;         // default 0.20  m  (pool member nearest-radius)
    double coplanar_dist_threshold; // default 0.12  m  (coplanar plane-dist gate)
    double coplanar_balance_min;    // default 0.30     (coplanar balance gate)
    double ridge_side_max_dist;     // default 1.5   m  (D.2b convex-killing radius)
    int32_t up_axis;                // 1 = y-up (bench), 2 = z-up (v3 prod)
} v7_bc_params_t;

void v7_bc_params_default(v7_bc_params_t* p);

// Forward decl — v7_bc_input_corner_t carries a `members[]` array of
// this type (full definition is in the Outputs block below). Same
// struct is used for both directions: input corners that include the
// upstream pool-merge decomposition AND output corners that produce
// a fresh one.
struct v7_bc_member;

// ── Inputs ──────────────────────────────────────────────────────────

// Per-plane subset re-derived from upstream PLANES.
typedef struct v7_bc_input_plane {
    uint32_t id;
    bool     rejected;
    double   tilt_deg;
    double   normal[3];
    double   d;
    double   basis_e1[3];
    double   basis_e2[3];
    uint8_t  plane_type;   // rg_plane_type_t, read from the roof-graph (authoritative)
} v7_bc_input_plane_t;

// Per-ridge subset from upstream RIDGE_LINES.
//
// `side_vec` and `t_min/t_max` are float here, matching the wire TLV. We don't
// need to keep loader-time double precision: D.2b's side test now gates out
// every anchor that sits on a ridge by construction (via anchor_on_ridge()),
// so `side_d ≈ 0 → sign coin flip` never happens in practice. Float is fine
// at the 0.30 m–1.5 m scales the test operates at.
typedef struct v7_bc_input_ridge_line {
    uint32_t plane_a;
    uint32_t plane_b;
    bool     has_line;
    bool     has_side_a;            // side_vec_a valid (plane A's frame)
    bool     has_side_b;            // side_vec_b valid (plane B's frame)
    double   line_point[3];
    double   line_direction[3];
    float    t_min_a, t_max_a, t_min_b, t_max_b;
    // Per-plane interior-oriented side axes; D.2b tests a corner of plane P
    // with side_vec_{a|b} per whether P is plane_a or plane_b. Kill iff the
    // dot with (corner − line_point) is negative. See journal
    // 2026-06-11-d2b-side-vec-wrong-plane-frame.
    float    side_vec_a[3], side_vec_b[3];
} v7_bc_input_ridge_line_t;

// Per-plane signal grid input — we keep only the cells we read.
typedef struct v7_bc_input_grid_field {
    uint32_t n_cols, n_rows;
    // Promoted from float to double 2026-05-20 to match v2's JS Number
    // semantics (IEEE-754 f64) end-to-end. The TLV wire format still
    // stores these as f32 in §4.5 PLANE_GRIDS records (compact), but
    // every in-memory consumer (cell-floor lookups in walk_from_seed,
    // arc-length anchor projection, etc.) needs double precision to
    // match v2's per-cell decisions; even a single LSB drift in
    // `g_min_u / cell_size` slips floor(...) onto an adjacent cell
    // when the exact quotient lands within ~1e-7 of an integer.
    double   cell_size;
    double   g_min_u, g_min_v;
    const float* oa_field;          // NaN signals empty cell
    const float* out_u;
    const float* out_v;
    // ownership: cell pointers reference caller's buffers; not freed.
} v7_bc_input_grid_field_t;

typedef struct v7_bc_input_plane_grid {
    uint32_t plane_id;
    bool                       has_group;
    v7_bc_input_grid_field_t   group_field;
    bool                       has_bal;
    const float*               bal_field; // length nRows*nCols (matches group_field)
} v7_bc_input_plane_grid_t;

// Per-plane direction entry on a seed corner (mirrors CORNERS dirsByPlane).
typedef struct v7_bc_input_corner_dir {
    uint32_t plane_id;
    float    dir_a[3];
    float    dir_b[3];
} v7_bc_input_corner_dir_t;

// Per-plane CoM-bisector cache entry on a seed corner (mirrors CORNERS
// comBisectorByPlane — see RG_CORNER_HAS_COM_BISECTOR_BY_PLANE).
typedef struct v7_bc_input_corner_com_bisector {
    uint32_t plane_id;
    float    com_bisector[3];
} v7_bc_input_corner_com_bisector_t;

// Seed corner from upstream CORNERS section (in building-local coords).
typedef struct v7_bc_input_corner {
    uint32_t id;                    // upstream pool id (informational only)
    uint8_t  source;                // primary identity; 1 RI, 4 concave, 5 convex
    double   point[3];
    uint32_t* plane_ids; uint32_t n_plane_ids;
    // Parallel to plane_ids[]: per-plane source label. Boundary-corners
    // input doesn't carry a per-plane override (signals never produces one)
    // so its decoder leaves this NULL. Boundary-polygons input does carry
    // it — the same struct (typedef) is reused.
    uint8_t* source_per_plane;
    // GEOMETRIC on-ridge tag: the ridge id(s) this corner sits on. Set for RI
    // corners (by construction) AND for concave/convex/ridge-end anchors that
    // D.2-snapped onto a ridge (≤ snap_radius). NOT RI-only — a snapped CC/CV
    // corner carries it (anchor on_ridge_ids → pool entry ridge_ids → emit).
    // This is the FP-clean seed for "which corner is on which ridge"; downstream
    // it must be kept consistent with the corner's DOF (on-ridge ⟺ constrained).
    uint32_t* ridge_ids; uint32_t n_ridge_ids;
    char*    name;                  // optional, owned by caller
    v7_bc_input_corner_dir_t* dirs; uint32_t n_dirs;
    v7_bc_input_corner_com_bisector_t* com_bisectors; uint32_t n_com_bisectors;
    // Inline pool-merge decomposition carried in from CORNERS' has_members
    // tail (§4.6). Used by:
    //   - boundary-polygons D.3 NMS (Phase 2) to look up per-plane anchor
    //     positions (v2 planeAnchors[g] semantics) via members[k].plane_id.
    //   - boundary-merge to round-trip the upstream decomposition through
    //     its CORNERS rewrite, so downstream consumers (regularize) keep
    //     seeing the same members per input corner id.
    // Empty (NULL/0) for seed corners coming from boundary-signals — that
    // step doesn't produce a member decomposition.
    struct v7_bc_member* members; uint32_t n_members;
} v7_bc_input_corner_t;

// ── Outputs ─────────────────────────────────────────────────────────

// Pool member — one decomposition of a pool corner.
typedef enum {
    V7_BC_AUTHORITY_FULL  = 1,
    V7_BC_AUTHORITY_AXIAL = 2,
    V7_BC_AUTHORITY_NONE  = 3,
} v7_bc_authority_t;

typedef struct v7_bc_member {
    double   point[3];
    uint8_t  source;
    v7_bc_authority_t authority;
    bool     has_axis;
    float    axis[3];
    bool     has_ridge_id;
    uint32_t ridge_id;
    // The plane this anchor originated on (e.g. the plane g that
    // boundary-corners was iterating in D.0/D.1a/D.1b when this anchor
    // was registered). 0xFFFFFFFF when unknown (mostly: polygon-merge
    // members that boundary-merge synthesizes from a polygon-graph
    // corner with no plane context). Wire-encoded with each member in
    // the inline members block of the CORNERS record (§4.6).
    uint32_t plane_id;
} v7_bc_member_t;

typedef struct v7_bc_corner_dir {
    uint32_t plane_id;
    float    dir_a[3];
    float    dir_b[3];
} v7_bc_corner_dir_t;

typedef struct v7_bc_corner_com_bisector {
    uint32_t plane_id;
    float    com_bisector[3];
} v7_bc_corner_com_bisector_t;

typedef struct v7_bc_corner {
    uint32_t id;
    uint8_t  source;                // cached / primary identity. equals
                                    // max-priority(source_per_plane[]).
                                    // see ROOF-GRAPH-FORMAT.md §5.3
                                    //   1 RI, 2 ridge-end, 3 polygon-merge,
                                    //   4 concave, 5 convex, 8 coplanar
    double   point[3];
    uint16_t n_merged;
    char*    name;                  // owned (NUL-term UTF-8) or NULL
    uint32_t* plane_ids; uint32_t n_plane_ids;
    uint8_t*  source_per_plane;     // parallel to plane_ids[]; length =
                                    // n_plane_ids. The per-plane source
                                    // label — concave in origin plane,
                                    // coplanar in imported plane, etc.
    uint32_t* ridge_ids; uint32_t n_ridge_ids;
    v7_bc_corner_dir_t* dirs;       // per-plane outgoing edge dirs
    uint32_t n_dirs;
    v7_bc_corner_com_bisector_t* com_bisectors;   // per-plane CoM-bisector cache
    uint32_t n_com_bisectors;
    v7_bc_member_t* members;        // pool-merge decomposition
    uint32_t n_members;
} v7_bc_corner_t;

typedef struct v7_bc_outputs {
    v7_bc_corner_t* corners;
    size_t          n_corners;
    // Per-upstream-plane cornerIds list (pool indices) — convenience for
    // the wrapper's plane-record patch step.
    uint32_t** plane_corner_ids;
    uint32_t*  plane_corner_ids_count;
    size_t     n_planes;            // sized to params/topology
    // Stats for the wrapper log + bench summary.
    uint32_t   wall_corners_added;
    uint32_t   coplanar_imported;
} v7_bc_outputs_t;

void v7_bc_outputs_init(v7_bc_outputs_t* o);
void v7_bc_outputs_free(v7_bc_outputs_t* o);

// ── Compute ─────────────────────────────────────────────────────────
//
// Inputs are BUILDING-LOCAL (positions are the building's points;
// ridge lines, plane equations, corner points have all been unshifted
// by the wrapper). The compute does no coordinate shifting itself;
// owned outputs (corner positions) remain building-local.
//
// Ownership: arrays inside v7_bc_input_corner_t (plane_ids, ridge_ids,
// dirs, name) are NOT taken — caller must keep them alive for the
// duration of the call and free afterwards. The compute deep-copies
// what it keeps.
//
// Returns 0 on success, -1 on alloc failure.
//
// ACSL (audit). Each `n_X == 0 || \valid_read(X + (0..n_X-1))` clause
// matches the "NULL pointer is legal when its count is 0" calling
// convention the tests exercise (e.g. positions==NULL, n_pts_local==0).
// `up_axis ∈ {1,2}` is the y-up / z-up selector — any other value is a
// caller bug. Verified EVA-only at the bench call sites; the bounded-
// output postcondition (out->n_corners is O(n_planes²) — bounded by
// ridge-pair intersections + seeds) is a future WP target, not asserted
// here since EVA can't discharge it.
/*@
  requires \valid_read(params);
  requires \valid(out);
  requires params->up_axis == 1 || params->up_axis == 2;
  requires n_pts_local    == 0 || \valid_read(positions    + (0 .. 3 * n_pts_local - 1));
  requires n_pts_local    == 0 || owner == \null
                               || \valid_read(owner        + (0 .. n_pts_local - 1));
  requires n_planes       == 0 || \valid_read(planes       + (0 .. n_planes - 1));
  requires n_ridge_lines  == 0 || \valid_read(ridge_lines  + (0 .. n_ridge_lines - 1));
  requires n_grids        == 0 || \valid_read(grids        + (0 .. n_grids - 1));
  requires n_seed_corners == 0 || \valid_read(seed_corners + (0 .. n_seed_corners - 1));
  assigns *out;
  ensures \result == 0 || \result == -1;
*/
int v7_bc_compute(const double* positions, size_t n_pts_local,
                   const int16_t* owner,
                   const v7_bc_input_plane_t* planes, size_t n_planes,
                   const v7_bc_input_ridge_line_t* ridge_lines, size_t n_ridge_lines,
                   const v7_bc_input_plane_grid_t* grids, size_t n_grids,
                   const v7_bc_input_corner_t* seed_corners, size_t n_seed_corners,
                   const v7_bc_params_t* params,
                   v7_bc_outputs_t* out);

#ifdef __cplusplus
}
#endif
#endif

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-boundary-polygons-input.h — input types shared by the
// boundary-polygons step family (currently: only bp2 / v7-boundary-
// polygons-2-impl).
//
// History: these types used to live in v7-boundary-polygons-impl.h
// (bp1's header) and were named v7_bp_input_*. When bp1 was retired
// (task #71), the types stayed — they're the canonical "input to the
// per-plane polygon assembly step", and downstream consumers (bench-v2
// loaders, the boundary-merge / boundary-regularize bench drivers)
// kept their existing references to the v7_bp_input_* names.
//
// Two kinds of declaration live here:
//   1) Typedef aliases over the upstream v7-boundary-corners output
//      structs (planes, corners, plane grids, ridge lines, corner dirs,
//      com_bisectors). The corners-step OWNS the struct definitions;
//      we just rename them locally because callers think in
//      "polygon-input" terms.
//   2) Two genuinely-new structs for the foreign-in WSD "nearby
//      projections" that the polygon step needs (filtered points from
//      OTHER planes that get pulled in as additional support).
#ifndef V3_STEPS_V7_BOUNDARY_POLYGONS_INPUT_H
#define V3_STEPS_V7_BOUNDARY_POLYGONS_INPUT_H

#include <stddef.h>
#include <stdint.h>

#include "v7-boundary-corners-impl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Per-plane signal-grid inputs are identical to the corners step's
// outputs (we read groupField.outU/V, balField, etc.). Re-use those
// types.
typedef v7_bc_input_corner_t              v7_bp_input_corner_t;
typedef v7_bc_input_corner_dir_t          v7_bp_input_corner_dir_t;
typedef v7_bc_input_corner_com_bisector_t v7_bp_input_corner_com_bisector_t;
typedef v7_bc_input_plane_t               v7_bp_input_plane_t;
typedef v7_bc_input_plane_grid_t          v7_bp_input_plane_grid_t;
typedef v7_bc_input_ridge_line_t          v7_bp_input_ridge_line_t;

// One per-plane nearby-projection record (filtered |sd|>=plane_dist_filter).
// Mirrors v7_bs_nearby_item_t in the signals impl but flattens to F64+u32
// for the polygon step's use (matches what we'd parse out of the
// roof-graph TLV NEARBY_PROJECTIONS section).
typedef struct v7_bp_input_nearby_item {
    double   position[3];
    float    signed_dist;
    uint32_t src_idx;
} v7_bp_input_nearby_item_t;

typedef struct v7_bp_input_nearby_per_plane {
    uint32_t plane_id;
    v7_bp_input_nearby_item_t* items;
    uint32_t n_items;
} v7_bp_input_nearby_per_plane_t;

#ifdef __cplusplus
}
#endif
#endif

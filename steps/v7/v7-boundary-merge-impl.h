// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-boundary-merge-impl.h — types-only stub after task #76.
//
// History: this header was the full impl interface of v7-boundary-merge
// (Pass 3 of bench-v2's boundary-3d pipeline). When v2-divergence
// tooling retired in #76, v7-boundary-merge was decommissioned —
// it was the v2-parity-only step (Path-B reorder dropped it from
// every production pipeline; commit 0ca5a27). The step wrapper,
// impl, doc, unit test, and benches were all deleted.
//
// What survives here: the FIVE typedefs + TWO struct definitions
// for "input record types the post-polygons downstream still reads"
// — v7_bm_input_{plane,ridge_line,corner,corner_dir,corner_com_bisector,
// edge,polygon_path}_t. They're consumed by
//   - v7-boundary-classify-impl.h
//   - v7-wall-detect-impl.h
//   - v7-boundary-regularize-impl.h (which uses them as
//     v7_br_input_*-typedef aliases for byte-identical payloads).
//
// The names keep the v7_bm_input_* prefix despite boundary-merge
// being gone — renaming is a follow-up sweep, not in scope here.
// Functionally these are "roof-graph TLV record runtime structs"
// shared by every downstream step.

#ifndef V3_STEPS_V7_BOUNDARY_MERGE_IMPL_H
#define V3_STEPS_V7_BOUNDARY_MERGE_IMPL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "v7-boundary-corners-impl.h"  // typedefs for plane/ridge/grid inputs

#ifdef __cplusplus
extern "C" {
#endif

// Aliases over v7-boundary-corners' input types — PLANES/RIDGE_LINES/
// CORNERS payloads are bit-identical here.
typedef v7_bc_input_plane_t               v7_bm_input_plane_t;
typedef v7_bc_input_ridge_line_t          v7_bm_input_ridge_line_t;
typedef v7_bc_input_corner_t              v7_bm_input_corner_t;
typedef v7_bc_input_corner_dir_t          v7_bm_input_corner_dir_t;
typedef v7_bc_input_corner_com_bisector_t v7_bm_input_corner_com_bisector_t;

// One edge record carried in from boundary-polygons' EDGES section.
typedef struct v7_bm_input_edge {
    uint32_t id;
    uint32_t corner_a;
    uint32_t corner_b;
    float    direction[3];
    float    length;
    uint32_t* plane_ids;             // owned
    uint32_t  n_plane_ids;
} v7_bm_input_edge_t;

// One polygon path carried in from POLYGON_PATHS (corner-id cycle per plane).
typedef struct v7_bm_input_polygon_path {
    uint32_t plane_id;
    uint32_t* corner_ids;            // owned
    uint32_t  n_corners;
} v7_bm_input_polygon_path_t;

#ifdef __cplusplus
}
#endif
#endif

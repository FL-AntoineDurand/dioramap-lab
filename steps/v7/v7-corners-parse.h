// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-corners-parse.h — shared RG_SECT_CORNERS reader for downstream
// steps that consume the basic corner payload (id, point, plane_ids,
// ridge_ids, name) and SKIP the optional dirs / com_bisectors / members
// tails.
//
// Extracted in task #72c Family D from two byte-identical 65-line
// parsers in v7-boundary-classify.c (parse_corners) and
// v7-boundary-regularize.c (parse_corners). PMD CPD flagged the cluster
// at 416 tokens / 45 lines — top remaining offender after Family A.
//
// bp2 (v7-boundary-polygons-2.c) ALSO has a CORNERS parser but it
// additionally consumes the members[] tail (flag 0x40) and the
// source_per_plane[] for member-source overrides. It stays inline —
// adding a "consume_members" callback here would just push the
// per-step variation across an abstraction layer for no win.
//
// Axis swap (z-up wire → y-up impl) is applied inline by this helper;
// both callers want it. If a future caller needs raw z-up coords,
// inline the swap there instead of teaching the helper a flag.
#ifndef V3_STEPS_V7_CORNERS_PARSE_H
#define V3_STEPS_V7_CORNERS_PARSE_H

#include <stddef.h>
#include <stdint.h>

#include "v7-boundary-merge-impl.h"  // v7_bm_input_corner_t
#include "v7-roof-graph-format.h"    // rg_reader_t

#ifdef __cplusplus
extern "C" {
#endif

// Read the CORNERS section pointed at by `r` into a freshly-allocated
// (*corners) array of length *n_corners. Each corner's world-coord point
// is centroid-subtracted (cx, cy, cz) and axis-swapped from z-up to y-up.
//
// Owned heap allocations on each corner: plane_ids, source_per_plane,
// ridge_ids, name. Caller frees in upstream_free (existing pattern in
// classify + regularize).
//
// Returns 0 on success (including "section missing" → leaves *corners
// = NULL, *n_corners = 0), -1 on allocation failure. Partial state on
// failure: anything already added to (*corners) is reachable via the
// (*corners, *n_corners) pair and will be reclaimed by caller's
// upstream_free.
int v7_parse_corners_section(rg_reader_t* r,
                             v7_bm_input_corner_t** corners, size_t* n_corners,
                             double cx, double cy, double cz);

#ifdef __cplusplus
}
#endif
#endif

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-plane-grids-parse.h — shared RG_SECT_PLANE_GRIDS reader.
//
// Extracted in task #72c Family A from two byte-identical 50-line
// parsers that lived in v7-boundary-corners.c (parse_upstream_plane_grids)
// and v7-boundary-polygons-2.c (parse_plane_grids). PMD CPD flagged the
// cluster at 566 tokens / 61 lines and it was the top remaining offender
// after #72a/b and Families B/C landed.
//
// The two callers used type names that differ in spelling only
// (v7_bc_input_plane_grid_t vs v7_bp_input_plane_grid_t). The bp_* form
// is a typedef alias of the bc_* form — see v7-boundary-polygons-input.h
// — so the helper takes the canonical bc name and both callers compile.
//
// The helper appends to four caller-owned heap-pointer arrays
// (oa/outu/outv/bal). Each tracked pointer lifetime is owned by the
// caller; callers iterate the array and free() at upstream_free time.
// Pass NULL/0 for the initial array state; the helper realloc-grows it.
//
// On failure (any malloc/realloc returning NULL) the helper returns -1
// WITHOUT freeing partially-allocated state — the caller's upstream_free
// will reclaim everything that was already tracked at the point of the
// failure (this mirrors the original inline behavior exactly).
#ifndef V3_STEPS_V7_PLANE_GRIDS_PARSE_H
#define V3_STEPS_V7_PLANE_GRIDS_PARSE_H

#include <stddef.h>
#include <stdint.h>

#include "v7-boundary-corners-impl.h"  // v7_bc_input_plane_grid_t
#include "v7-roof-graph-format.h"      // rg_reader_t

#ifdef __cplusplus
extern "C" {
#endif

// Parse the PLANE_GRIDS section pointed at by `r`. For each record with
// has_group | has_bal set:
//   - appends a v7_bc_input_plane_grid_t to *grids (realloc-grow on demand)
//   - mallocs has_group ? 3 : 0 + has_bal ? 1 : 0 cell-arrays, copies
//     bytes in from the wire, and registers each in owned_* for later
//     free
//
// Initial state for an empty result set:
//   *grids == NULL && *n_grids == 0
//   *owned_X == NULL && *n_owned_X == 0      (each of oa, outu, outv, bal)
//
// Returns 0 on success (including the "section missing" case), -1 on
// allocation failure.
int v7_parse_plane_grids_section(rg_reader_t* r,
                                 v7_bc_input_plane_grid_t** grids, size_t* n_grids,
                                 float*** owned_oa,   size_t* n_owned_oa,
                                 float*** owned_outu, size_t* n_owned_outu,
                                 float*** owned_outv, size_t* n_owned_outv,
                                 float*** owned_bal,  size_t* n_owned_bal);

#ifdef __cplusplus
}
#endif
#endif

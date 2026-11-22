// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-perbldg-gather.h — per-building point gather + centroid-subtract +
// F32→F64 promotion. Extracted in task #72a from the duplicated 20-line
// block that lived in 5 v7 step wrappers (build-building-kd-trees,
// v7-tile-signals, v7-hemisphere-segment, v7-density-voids-2,
// v7-normal-cascade-seed). PMD CPD flagged the cluster at 283 tokens /
// 102 lines summed across the 5 sites.
//
// Threading model: single caller per invocation (each wrapper iterates
// buildings sequentially). The buffer pair (`*local_f32`, `*local_f64`)
// is OWNED by the caller and reused across buildings — the helper
// realloc-grows them and updates `*local_cap` (in F32 elements, NOT
// bytes; matches the original sites' semantics).
//
// Also fixes a latent realloc-failure bug that lived in all 5 sites: the
// inline code did
//   p32 = realloc(*local_f32, ...);
//   p64 = realloc(*local_f64, ...);
//   if (!p32 || !p64) { free(p32); free(p64); ...; break; }
// followed by `free(local_f32)` at loop exit. If p32 succeeded (so the
// OLD local_f32 was freed by realloc per the C standard) but p64 failed,
// the loop-exit `free(local_f32)` was a double-free of the now-dangling
// old pointer. The helper updates *local_f32 IMMEDIATELY after a
// successful realloc so the caller's cleanup always sees a valid pointer.
#ifndef V3_STEPS_V7_PERBLDG_GATHER_H
#define V3_STEPS_V7_PERBLDG_GATHER_H

#include <stddef.h>
#include <stdint.h>

#include "v3/step.h"  // step_context_t for v7_per_bldg_load_frame (#72c Family B)

#ifdef __cplusplus
extern "C" {
#endif

// Gather n_pts points into building-local frame.
//
// For each i in [0, n_pts):
//   g = gidx[i]
//   if g >= total_points:
//     (*local_f32)[3i .. 3i+2] = {0, 0, 0}
//   else:
//     (*local_f32)[3i .. 3i+2] = (positions[3g..3g+2] - centroid) as float
//   (*local_f64)[3i .. 3i+2] = (double)(*local_f32)[3i .. 3i+2]
//
// Realloc-grows (*local_f32) and (*local_f64) to hold n_pts * 3 elements
// each if (*local_cap) is too small; updates (*local_cap) in elements.
//
// Returns 0 on success, -1 on allocation failure. On failure the caller's
// (*local_f32) and (*local_f64) are always valid pointers (NULL or
// successfully-realloc'd buffer) — never dangling.
//
// Pre: positions, gidx must be valid for [0..total_points*3) and [0..n_pts).
//      local_f32, local_f64, local_cap must be non-NULL out-params.
//      Pass *local_f32 = *local_f64 = NULL and *local_cap = 0 on first call.
/*@ requires \valid(local_f32) && \valid(local_f64) && \valid(local_cap);
    requires positions == \null || \valid_read(positions + (0 .. 3 * total_points - 1));
    requires n_pts == 0 || \valid_read(gidx + (0 .. n_pts - 1));
    assigns *local_f32, *local_f64, *local_cap;
    assigns *local_f32[0 .. 3 * n_pts - 1] \from positions[0 ..], gidx[0 ..], cx, cy, cz;
    assigns *local_f64[0 .. 3 * n_pts - 1] \from positions[0 ..], gidx[0 ..], cx, cy, cz;
    ensures \result == 0 || \result == -1;
*/
int v7_gather_building_points_f32_f64(
    const double* positions, size_t total_points,
    const uint32_t* gidx, uint32_t n_pts,
    double cx, double cy, double cz,
    float** local_f32, double** local_f64, size_t* local_cap);

// Parse the per-building-records + building-local-frames artifact pair for
// building index `bi`. This is the per-bldg loop preamble shared by 3 v7
// step wrappers (v7-grow-refit-merge, v7-boundary-regularize, v7-wall-detect);
// other step wrappers do similar reads but with caller-specific extra checks
// (e.g. n_pts >= 4 in boundary-corners) so they stay inline.
//
// Reads via step_array_read_bytes which aliases the storage; the returned
// `*out_gidx` pointer is valid for the lifetime of the step call.
//
// On success: returns 0, fills:
//   *out_gidx  → uint32_t × *out_n_pts, indices into the global LiDAR array
//   *out_n_pts → point count for this building
//   *out_cx/y/z → building-local-frame centroid (world coords)
//
// On any missing or malformed artifact: returns -1. Caller's typical
// response is `goto skip_bldg` or `blob_push(&b, NULL, 0); continue`.
// Validates: per-bldg record has schema == 1, length ≥ 68 (header through
// the trailing global-indices array), idx_off + n_pts*4 ≤ rec_len; frame
// length ≥ BLF_RECORD_BYTES (72).
//
// Extracted in task #72c — Family B (per-bldg loop dispatch).
int v7_per_bldg_load_frame(step_context_t* ctx, size_t bi,
                           const uint32_t** out_gidx, uint32_t* out_n_pts,
                           double* out_cx, double* out_cy, double* out_cz);

#ifdef __cplusplus
}
#endif
#endif

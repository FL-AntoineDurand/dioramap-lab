// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-score-intrinsic — intrinsic (no-GT) model-quality metrics for a v7
// roof-recon result. Computed PER BUILDING from a plane set + per-point
// owner assignment. Cheap, ground-truth-free, audit-friendly: no concave
// polygon clipping, no Hungarian matching — just owner counting and
// point-to-plane distance math.
//
// This is the in-pipeline half of the hybrid scoring design
// (docs/quality/SCORING-DESIGN.md §6.2):
//
//   "A small C step computing only the no-GT intrinsic metrics
//    (completeness, residual RMS, ...). Wireable into production
//    pipelines for a served-model confidence score, and repeatable in
//    a test graph (score after merge vs after regularize in one run)."
//
// Scope of THIS file: completeness + residualRMS (+ derived composite).
// The rasterisation-based metrics (footprint coverage / overshoot / plane
// overlap) are a follow-up; the schema below leaves their fields in place
// returning NaN, so adding them later is a non-breaking change.
//
// Pure compute (no v3 runtime deps): takes raw arrays in, writes a result
// struct out — same shape as every other v7-*-impl.h. Wrapper in
// v7-score-intrinsic.c maps it onto the artifact world.

#ifndef V7_SCORE_INTRINSIC_IMPL_H
#define V7_SCORE_INTRINSIC_IMPL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Plane equation (Hessian normal form: n·x + d = 0, |n| = 1). ──
// We don't pull v7-plane-fit.h to keep this impl self-contained and
// independently linkable from tests/harnesses.
typedef struct v7_score_plane {
    double normal[3];  // unit normal
    double d;          // signed offset
} v7_score_plane_t;

// "Metric not computed" sentinel. v3 audit forbids NaN writes
// (`-warn-special-float nan` per docs/quality/REVIEW-HEURISTICS.md), and
// all our metrics live in [0, +∞), so −1 is unambiguous. The visualiser
// + unit tests check `v < 0` to detect "n/a".
#define V7_SCORE_NA (-1.0)

// ── Result for ONE building. V7_SCORE_NA (= −1) = metric is not yet
//    implemented or not computable on this input (no points / no planes). ──
//
// Versioned: bump V7_SCORE_INTRINSIC_VERSION when ANY field's computation
// changes, even if the shape doesn't — same discipline as v2's per-metric
// VERSION (lidar-algos-bench-v2/CLAUDE.md). Stored as the first u32 of
// the serialised record so post-hoc consumers can branch on it.
#define V7_SCORE_INTRINSIC_VERSION 1u

typedef struct v7_score_intrinsic_result {
    uint32_t version;          // V7_SCORE_INTRINSIC_VERSION
    uint32_t n_planes;         // planes considered
    uint32_t n_points_total;   // building points (denominator of completeness)
    uint32_t n_points_assigned;// owner != -1

    double completeness;       // assigned / total ∈ [0,1]
    double residual_rms;       // weighted RMS point-to-plane distance (m)
    double residual_score;     // 1 - residual_rms / RMS_REFERENCE, clamped ≥ 0

    // Rasterisation metrics — placeholders until follow-up; NaN today.
    double footprint_coverage; // ∈ [0,1]   higher = better
    double footprint_overshoot;// ∈ [0,1]   higher = better (1 - overshoot ratio)
    double plane_overlap;      // ∈ [0,1]   higher = better (1 - overlap ratio)

    // Single ranking composite (0-100) — adaptive: only blends the
    // currently-implemented metrics. Maps onto SCORING-DESIGN §6.4's
    // "one composite for ranking only — never a gate".
    double composite;
} v7_score_intrinsic_result_t;

// RMS at which the residual_score hits 0 (matches v2's residual.js
// constant — change in lockstep if v2's reference moves).
#define V7_SCORE_RMS_REFERENCE 0.15

// ── Compute: one building, one call. ──────────────────────────────
//
//   positions:    [3·n_points_total] doubles, LOCAL coords (any frame;
//                 plane equations must be in the same frame). Required.
//   owner:        [n_points_total] int16; pi ∈ [0, n_planes) or -1 = unassigned.
//                 Required.
//   planes:       [n_planes] equations. Required if n_planes > 0.
//                 May be NULL when n_planes == 0 (no plane → all points
//                 unassigned, completeness 0, residual NaN).
//   out:          filled on success. NaN-initialised first, so the caller
//                 can read any field even if compute returns early.
//
// Returns 0 on success, -1 on invalid input (NULL out, NULL positions,
// NULL owner). Empty point set or no planes is NOT an error — out is
// populated with the documented edge values (composite = 0).
/*@
  requires positions == \null || \valid_read(positions + (0 .. 3 * n_points_total - 1));
  requires owner == \null || \valid_read(owner + (0 .. n_points_total - 1));
  requires n_planes == 0 || (planes != \null && \valid_read(planes + (0 .. n_planes - 1)));
  requires out == \null || \valid(out);
  assigns *out \from positions, owner, n_points_total, planes, n_planes;
  ensures \result == 0 || \result == -1;
*/
int v7_score_intrinsic_compute(const double* positions,
                                const int16_t* owner,
                                size_t n_points_total,
                                const v7_score_plane_t* planes,
                                size_t n_planes,
                                v7_score_intrinsic_result_t* out);

// ── Pack/unpack the on-disk record (the AK_ARRAY_INLINE payload). ──
//
// Layout (fixed-size, naturally aligned, 64 bytes):
//   u32 version
//   u32 n_planes
//   u32 n_points_total
//   u32 n_points_assigned
//   f64 completeness
//   f64 residual_rms
//   f64 residual_score
//   f64 footprint_coverage
//   f64 footprint_overshoot
//   f64 plane_overlap
//   f64 composite
//                                          = 4·4 + 7·8 = 72 bytes
//
// Total wire size: 72 bytes per building. Keep in lockstep with the
// reader in the visualiser and any downstream tooling.
#define V7_SCORE_INTRINSIC_RECORD_BYTES 72

// Serialise `r` into the 72-byte fixed buffer `out_buf`. Returns the
// number of bytes written (always V7_SCORE_INTRINSIC_RECORD_BYTES).
/*@ requires \valid_read(r);
    requires \valid(out_buf + (0 .. V7_SCORE_INTRINSIC_RECORD_BYTES - 1));
    assigns out_buf[0 .. V7_SCORE_INTRINSIC_RECORD_BYTES - 1] \from *r;
    ensures \result == V7_SCORE_INTRINSIC_RECORD_BYTES;
*/
size_t v7_score_intrinsic_pack(const v7_score_intrinsic_result_t* r,
                                uint8_t* out_buf);

// Parse one 72-byte record. Returns 0 on success, -1 if `len < 72`.
/*@ requires buf == \null || \valid_read(buf + (0 .. len - 1));
    requires out == \null || \valid(out);
    assigns *out \from buf, len;
    ensures \result == 0 || \result == -1;
*/
int v7_score_intrinsic_unpack(const uint8_t* buf, size_t len,
                               v7_score_intrinsic_result_t* out);

#ifdef __cplusplus
}
#endif

#endif  // V7_SCORE_INTRINSIC_IMPL_H

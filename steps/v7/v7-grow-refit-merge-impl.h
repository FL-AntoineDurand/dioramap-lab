// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-grow-refit-merge — pure compute. Direct port of bench-v2
// lib/pipeline/steps/grow-refit-merge.js (meta.version=8). Iterative
// plane segmentation:
//
//   outer iter ≤ maxIterations:
//     mini-resolve  — best-residual owner per claimed point
//     grow          — diffusion-limited radius expand from best-owner pts
//     refit         — per-PSID LSQ + equal-weight strip averaging,
//                     RMS-revert if RMS jumps > prev + maxDegradation
//     merge         — coplanar pairs (angle + offset + connexity + RMS cap)
//     stable check  — Δclaims ≤ stableThreshold × n_pool, no merges/reverts
//
//   resolve         — each pt → plane with smallest residual
//   coherence       — kNN majority vote (≤5 rounds, ≥70% majority,
//                                          residual gate)
//   final refit     — LSQ on resolved owner sets
//   ridge mask      — point has ≥1 neighbour with different owner
//   residual array  — per-pt |signed-dist| to assigned plane
//
// Per the V7 simplification policy + Phase 5.5 audit:
//   - Drops `lidar-density` + `lidar-z-noise` reads. `density`,
//     `dist_threshold` (default 0.12), `max_degradation` (default 0.04)
//     are direct step params.
//   - Drops `plane-equations` read (equation prefix lives in seed-plane
//     records emitted by v7-normal-cascade-seed).
//   - Outputs all 6 v2 artifacts; consumers are downstream boundary
//     chain steps. None gated as viz-only — `point-plane-residual`
//     is load-bearing for boundary-polygons + boundary-regularize WSD
//     trust weighting (added in bench-v2 commit 251efb6 2026-05-08).

#ifndef V3_STEPS_V7_GROW_REFIT_MERGE_IMPL_H
#define V3_STEPS_V7_GROW_REFIT_MERGE_IMPL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kd_tree.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct v7_grm_params {
    // Distance scales (m).
    double dist_threshold;        // default 0.12 (v2 hardcoded literal)
    double max_degradation;       // default 0.04 (= 2 × 0.02 σ_z)
    // Iteration control.
    int32_t max_iterations;       // default 5  (note: v2 meta says 10, body uses 5; bake the body's actual value)
    double  stable_threshold;     // default 0.002 (fraction of n_pool)
    // Search radius.
    double  search_radius_scale;  // default 3.0
    double  search_radius_max;    // default 1.0 (m)
    double  density;              // pts/m². For radius = scale·√weight/√density.
    // Merge.
    double  merge_angle_deg;      // default 10
    double  merge_wall_angle_deg; // default 15  (looser threshold for tilt>70° pairs)
    double  merge_offset_scale;   // default 2.0 (× dist_threshold)
    int32_t merge_min_shared_pts; // default 3
    double  wall_tilt_deg;        // default 70  (above → wall-wall thresholds apply)
    // Coherence.
    double  coherence_majority;   // default 0.70
    int32_t coherence_max_rounds; // default 5
    int32_t coherence_min_votes;  // default 3
    // Redundant-plane prune: dissolve a plane that a neighbour explains almost
    // as well. redundancy = (alt_RMS − own_RMS)/dist_threshold; prune if below
    // tau. far = stand-in alt residual for points with no competing claimer.
    double  redundancy_tau;       // default 0.5  (0 disables the prune)
    double  redundancy_far;       // default 1.0 m
    // Frame.
    int32_t up_axis;              // 1 = y-up bench, 2 = z-up LAZ
} v7_grm_params_t;

void v7_grm_params_default(v7_grm_params_t* p);

// Input seed plane (we only need normal+d+seedIndices+source-tag for ghost).
typedef struct v7_grm_input_seed {
    double normal[3];
    double d;
    const uint32_t* seed_indices;   // length n_seeds, LOCAL into pool
    uint32_t        n_seeds;
    bool            is_ghost;       // tags ghost-planes-2 entries for the
                                    //   per-iter log + final seed-planes-out
    uint32_t        ghost_void_id;  // only meaningful when is_ghost
    double          seed_tilt;      // pass-through field (v2 stores it)
} v7_grm_input_seed_t;

// Output per-plane record (matches v2 seed-planes-out + plane-equations
// fused; v3 emits ONE artifact `grm-seed-planes` with this shape, the
// {normal,d} prefix doubling for `grm-plane-equations` consumers).
typedef struct v7_grm_plane_record {
    double normal[3];
    double d;
    double tilt;
    double azimuth;
    double rms;
    uint32_t inlier_count;
    uint32_t source_seed_index;     // index into the input seed array
    uint8_t  is_ghost;              // 1 if originated as a ghost seed
    uint8_t  tombstoned;            // 1 if dissolved during merge
    uint8_t  _reserved[6];
} v7_grm_plane_record_t;

typedef struct v7_grm_plane {
    v7_grm_plane_record_t hdr;
    uint32_t* seed_indices;         // owned; length hdr.inlier_count
    // Void IDs (from dv2-ghost-planes) this plane owns. A plane owns a
    // void if it originated as that ghost OR absorbed a ghost plane
    // during the iterative merge. Empty for planes never touched by a
    // ghost. Stable across merges by set-union (see compute loop).
    uint32_t* void_ids;             // owned; sorted ascending
    uint32_t  n_void_ids;
} v7_grm_plane_t;

typedef struct v7_grm_outputs {
    int16_t* owner;             // length n_pts, -1 = unassigned
    uint8_t* ridge_mask;        // length n_pts (0/1)
    float*   residual;          // length n_pts; +inf for unassigned
    double   distance_threshold;// echoes params.dist_threshold (load-bearing)
    v7_grm_plane_t* planes;     // length n_planes — alive + dead (tombstoned)
    size_t  n_planes;
    size_t  n_pts;

    // Optional per-iter log (heap-allocated string array). NULL when
    // params.emit_log == false. Useful for the bench-harness's
    // round-by-round diff against v2's ctx.log entries.
    char**  log_lines;
    size_t  n_log_lines;
} v7_grm_outputs_t;

void v7_grm_outputs_init(v7_grm_outputs_t* out);
void v7_grm_outputs_free(v7_grm_outputs_t* out);

// Compute. All input arrays and indices LOCAL to the building (positions
// in F64 frame matching params->up_axis; `pool_indices` selects which
// global-into-positions belong to the building — typically [0..n_pts)
// when called per-bldg with extracted positions).
//
//   positions:      F64×3·n_pts (interleaved)
//   point_weights:  F32×n_pts. REQUIRED — must NOT be NULL. Pass a
//                              uniform-1.0 array to get the legacy
//                              unweighted radius = scale/√density; silent
//                              fallback is forbidden so the call site is
//                              explicit.
//   psids:          U16×n_pts (NULL → single-strip refit fallback)
//   seeds:          length n_seeds (cascade + ghost combined; ghosts last)
//   tree:           kd_tree built over `positions`
// Returns 0 on success, -1 on alloc failure.
int v7_grm_compute(const double* positions,
                   const float*  point_weights,
                   const uint16_t* psids,
                   size_t n_pts,
                   const uint32_t* pool_indices, size_t n_pool,
                   const kd_tree_t* tree,
                   const v7_grm_input_seed_t* seeds, size_t n_seeds,
                   const v7_grm_params_t* params,
                   v7_grm_outputs_t* out);

#ifdef __cplusplus
}
#endif
#endif

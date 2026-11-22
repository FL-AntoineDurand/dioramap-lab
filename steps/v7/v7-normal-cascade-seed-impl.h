// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-normal-cascade-seed — pure compute. Direct port of bench-v2
// lib/pipeline/steps/normal-cascade-seed.js (meta.version=13).
//
// Per V7 DAG-scope policy + v6b → v7 simplifications, this port drops
// from the v2 surface:
//   - cascadeExcludeRidges param   (V7 always sets false — pipelines.js)
//   - cascade-segments input        (vestigial — read but never indexed)
//   - ridge-candidates input        (only the cascadeExcludeRidges branch
//                                    uses it; with that gone, dead read)
//   - lidar-z-noise artifact        (every test case has noise_sigma=null;
//                                    distThreshold falls out as constant
//                                    0.135 m — bake as default param)
//   - hull / area in seed-planes    (only fit-solar-panels reads them,
//                                    off the boundary-regularize path —
//                                    eliminates 501 LOC alpha-shape port)
//   - separate plane-equations      (duplicates the equation prefix of
//                                    seed-planes; downstream consumers
//                                    can read the {normal,d} prefix
//                                    directly from the seed-planes
//                                    record's fixed-stride header)
//   - seed-dist-threshold + seed-min-seed-area metadata outputs (logged
//     only, never read downstream)
//
// What this step actually does, per cascade segment (sorted largest-
// first by sum-of-point-weights, with original segment id as stable
// tiebreaker):
//   S1: curvature gate — fit plane, compute tilt; if tilt < 70°
//       (non-wall) and weighted-mean curvature > maxSeedCurvature,
//       skip (= ridge/valley transition; GRM will absorb downstream).
//   S2: LSQ plane fit on the segment's point indices.
//   S3: outlier rejection — drop points with |signed_distance| >
//       distThreshold.
//   S4: refit on the clean indices if any were dropped.
//   S5: orient normal up along the active axis.
//   S6: emit seed plane.
//
// Final pass builds the global `owner` Int16 array (-1 unassigned)
// from the surviving planes' seed-index sets.

#ifndef V3_STEPS_V7_NORMAL_CASCADE_SEED_IMPL_H
#define V3_STEPS_V7_NORMAL_CASCADE_SEED_IMPL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct v7_seed_params {
    double dist_threshold;        // default 0.135 m  (= max(0.05, 4.5×0.03)
                                  //   the only value ever used in v2)
    double max_seed_curvature;    // default 0.03 (1/m). 0 disables the gate.
    double wall_tilt_deg;         // default 70.0 — gate exempts walls
    int32_t up_axis;              // 1 = y-up (bench frame), 2 = z-up (LAZ)
} v7_seed_params_t;

void v7_seed_params_default(v7_seed_params_t* p);

// One emitted seed plane (load-bearing). Fixed-stride 56-byte header;
// per-plane variable seed_indices follow on the wire (length =
// inlier_count, u32 globals).
typedef struct v7_seed_plane_record {
    double normal[3];             // unit, oriented up along up_axis
    double d;                     // n·p + d = 0
    double tilt;                  // degrees, 0 = horizontal
    double azimuth;               // degrees, 0 = +H1 (north of horizontal)
    double rms;                   // sqrt(smallest covariance eigenvalue)
    uint32_t inlier_count;
    uint32_t source_segment_id;   // input cascade-segment id (traceability +
                                  //   cross-viz with cascade-lambert-grid)
} v7_seed_plane_record_t;

typedef struct v7_seed_plane {
    v7_seed_plane_record_t hdr;
    uint32_t* seed_indices;       // length = hdr.inlier_count, owner of buffer
} v7_seed_plane_t;

typedef struct v7_seed_outputs {
    v7_seed_plane_t* planes;      // length n_planes; each owns seed_indices
    size_t n_planes;
    int16_t* owner;               // length n_pts; -1 = unassigned
    size_t n_pts;
} v7_seed_outputs_t;

void v7_seed_outputs_init(v7_seed_outputs_t* out);
void v7_seed_outputs_free(v7_seed_outputs_t* out);

// One input cascade segment as the seed step sees it (after the
// wrapper has globalised pt_indices via building-indices).
typedef struct v7_seed_input_segment {
    uint32_t source_segment_id;   // segment's id from cascade-segment-list
                                  //   (= position-in-list; needed because
                                  //   we sort by area)
    const uint32_t* pt_indices;   // global indices into `positions`
    uint32_t n_pts;
} v7_seed_input_segment_t;

// Compute. Inputs:
//   positions:     F64×3·n_pts (interleaved, in whatever frame the
//                                segments were extracted from — must
//                                match `params->up_axis`)
//   curvatures:    F32×n_pts global (typically curvatures-k16). NULL
//                                disables the curvature gate (= same
//                                effect as max_seed_curvature=0).
//   point_weights: F32×n_pts global. REQUIRED — must NOT be NULL. Pass a
//                                uniform-1.0 array to get the legacy
//                                unweighted semantics; silent fallback is
//                                forbidden so the call site is explicit.
//   segments:      length n_segments
// Returns 0 on success, -1 on alloc failure or point_weights == NULL.
int v7_seed_compute(const double* positions,
                    const float* curvatures,
                    const float* point_weights,
                    size_t n_pts,
                    const v7_seed_input_segment_t* segments,
                    size_t n_segments,
                    const v7_seed_params_t* params,
                    v7_seed_outputs_t* out);

#ifdef __cplusplus
}
#endif
#endif

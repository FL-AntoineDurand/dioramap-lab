// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-hemisphere-segment — pure compute interface.
//
// Direct port of bench-v2 lib/pipeline/steps/hemisphere-normals-density-aware
// -segment.js (meta.version=5). Three stages, all density-weighted:
//   S1: Lambert hemisphere accumulation → normal-orientation groups
//   S1b: Antipodal-merge (PCA normals are unoriented; near-horizontal
//        normals can land on opposite Lambert hemisphere — merge groups
//        with peak-normal dot < -0.85).
//   S2: Per-group weighted offset KDE → parallel-plane separation
//   S3: Per-cluster radius-graph connected components → spatial split
//
// Pure-compute interface mirrors v7-tile-signals-impl: callable from
// the v3 step wrapper (consumes runtime artifacts, packs outputs) and
// from the standalone bench-harness CLI (consumes v2 dump bytes
// directly, validates byte-against-v2).

#ifndef V3_STEPS_V7_HEMI_SEG_IMPL_H
#define V3_STEPS_V7_HEMI_SEG_IMPL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kd_tree.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct v7_hs_params {
    int32_t lambert_resolution;        // default 128
    double  lambert_smooth_sigma;      // default 2.0 (grid cells)
    double  lambert_min_basin_area;    // default 0.5 m²
    double  stage2_iqr_bandwidth;      // 0 = silverman (default), 1 = IQR
    int32_t kde_n_eval;                // default 200
    double  connect_radius_scale;      // default 3.0
    double  connect_radius_max;        // default 2.0 m
    double  min_segment_area;          // default 1.0 m²
    int32_t up_axis;                   // 1 = bench y-up, 2 = LAZ z-up
    bool    emit_viz_artifacts;        // emit Lambert grid + KDE for viz
} v7_hs_params_t;

void v7_hs_params_default(v7_hs_params_t* p);

// One emitted segment record (load-bearing).
typedef struct v7_hs_segment {
    int32_t id;                  // global segment id (0..n_segments-1)
    int32_t normal_group_id;     // Lambert peak idx
    int32_t offset_cluster_id;   // S2 KDE peak idx within group
    int32_t spatial_cluster_id;  // S3 CC idx within offset cluster
    double  peak_offset;         // S2 cluster center
    double  mean_normal[3];      // group-orientation-flipped weighted mean
    uint32_t n_pts;
    uint32_t* pt_indices;        // local indices into the input n_pts (caller
                                 // re-maps to global as needed)
} v7_hs_segment_t;

// One emitted Lambert peak (viz, gated on emit_viz_artifacts).
// Fixed-stride 56-byte record (6 doubles + 2 u32 — natural alignment
// gives no internal padding since 6×8 + 2×4 = 56 is already 8-aligned).
// _reserved is part of the on-wire schema; consumers must respect it.
typedef struct v7_hs_lambert_peak_record {
    double tilt;
    double azimuth;
    double basin_weight;
    double peak_normal[3];
    uint32_t n_assigned_pts;
    uint32_t _reserved;
} v7_hs_lambert_peak_record_t;

// One emitted offset KDE record per (Lambert group). Variable-length
// (n_eval and n_peaks/n_valleys vary).
typedef struct v7_hs_offset_kde_record {
    int32_t  normal_group_id;
    int32_t  n_eval;
    int32_t  n_peaks;
    int32_t  n_valleys;
    double   bandwidth;
    // followed by:
    //   double eval_pts[n_eval]
    //   double densities[n_eval]
    //   double peaks[n_peaks][2]    (center, density)
    //   double valleys[n_valleys][2]
} v7_hs_offset_kde_record_t;

typedef struct v7_hs_outputs {
    // Always emitted.
    int16_t* cascade_segments;        // length n_pts; -1 = unassigned
    uint8_t* ridge_candidates;        // length n_pts; bit 7 = orphan
    int16_t* owner;                   // length n_pts; pass-through or -1 (we
                                      //   don't change owner in this step)
    v7_hs_segment_t* segments;        // length n_segments; each owns its pt_indices
    size_t n_segments;
    size_t n_pts;

    // Viz extras (NULL when emit_viz_artifacts=false).
    double*  lambert_grid;            // F64 N×N (N = lambert_resolution)
    int32_t  lambert_grid_resolution; // copy of params for self-describing viz
    v7_hs_lambert_peak_record_t* lambert_peaks;
    size_t n_lambert_peaks;

    // Offset KDE: ARRAY_INLINE-style flat blob, one record per Lambert group
    // that produced a KDE. The blob is `[u32 record_len][record bytes]…` —
    // the step wrapper hands it to step_write_array_inline directly.
    uint8_t* offset_kde_blob;
    size_t   offset_kde_blob_len;
    size_t   offset_kde_n_records;
} v7_hs_outputs_t;

void v7_hs_outputs_init(v7_hs_outputs_t* out);
void v7_hs_outputs_free(v7_hs_outputs_t* out);

// Compute. Inputs:
//   positions:       F64×3·n_pts (interleaved, in whatever frame the kd-tree
//                                  was built over — typically the building
//                                  centroid-shifted F32-cast coords from
//                                  v7-tile-signals' cast)
//   normals:         F32×3·n_pts (interleaved unit normals — typically the
//                                  raw `tile-normals` output of v7-tile-signals)
//   point_weights:   F32×n_pts (Voronoi-area weights, m² — from v7-tile-signals)
//   tree:            kd_tree_t built over `positions` (typically deserialised
//                                  from `building-kd-trees` artifact)
//   owner_in:        Int16×n_pts (pass-through owner for progressive mode;
//                                 NULL → all -1 / all eligible)
// Returns 0 on success, -1 on alloc failure.
int v7_hs_compute(const double* positions,
                  const float* normals,
                  const float* point_weights,
                  size_t n_pts,
                  const kd_tree_t* tree,
                  const int16_t* owner_in,
                  const v7_hs_params_t* params,
                  v7_hs_outputs_t* out);

#ifdef __cplusplus
}
#endif
#endif

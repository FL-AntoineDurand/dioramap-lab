// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-tile-signals — pure compute interface.
//
// Direct C port of bench-v2 lib/pipeline/steps/tile-signals.js (meta.version=10).
// Matches v2's algorithm at every step:
//   - Float32-precision positions (v2 uses Float32Array; we accept float* so
//     callers feeding F64 lidar-positions must cast to float first to match).
//   - kd-tree + Cardano PCA from steps/normals/{kd_tree,pca} (themselves
//     direct ports of bench-v2 lib/geometry/{kd-tree,pca}.js).
//   - Spherical-KDE-weighted PCA at multiple K slices (curvatures-k8/16/32).
//   - Progressive K expansion when the K=16 neighbourhood is scan-line-elongated.
//   - Voronoi-area point weights (π·d_k²/k) from k-NN at K_STAGES[0]=32.
//
// Output shape: 12 per-point arrays. All allocated by `v7_ts_compute`;
// caller frees via `v7_ts_outputs_free`. Layout matches v2's reference dump
// byte-for-byte (modulo libm vs JS Math floating-point rounding differences,
// which are typically within 1 ULP per operation).

#ifndef V3_STEPS_V7_TILE_SIGNALS_IMPL_H
#define V3_STEPS_V7_TILE_SIGNALS_IMPL_H

#include <stddef.h>
#include <stdint.h>

#include "kd_tree.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct v7_ts_params {
    int eigen_k;                  // default 16
    double max_radius;            // default 1.0 m (radius cap for reliability density factor)
    double angular_bandwidth;     // default π/8 (Gaussian σ for spherical KDE)
    double linearity_threshold;   // default 0.5 (above which K is progressively expanded)
    int weight_k;                 // default 8 (k for Voronoi-area point weight)
    int up_axis;                  // 0=x, 1=y, 2=z. v2 uses y-up (default 1) — bench
                                  // harness keeps the default for byte-identity with
                                  // v2 dumps. v3 pipeline uses z-up (LAZ + altitude).
                                  // Affects only the upward-flip + degenerate-default
                                  // normal; eigenvalues / curvatures / weights are
                                  // rotation-invariant.
} v7_ts_params_t;

typedef struct v7_ts_outputs {
    // Allocated by v7_ts_compute(). Each Float32* is length n_pts; tile_normals
    // and eigen_ternary are 3*n_pts (interleaved). All zeroed before fill so
    // empty-cloud and degenerate-point fallbacks land at well-defined values.
    float* eigenvalue_l1;
    float* eigenvalue_l2;
    float* eigenvalue_l3;
    float* planarity;
    float* linearity;
    float* scattering;
    float* curvatures_k8;
    float* curvatures_k16;
    float* curvatures_k32;
    float* tile_normals;       // 3*n_pts — raw Float32 (matches v2 point-normals.bin)
    float* eigen_reliability;
    float* point_weights;
    uint8_t* eigen_ternary;    // 3*n_pts — RGB packed lin/plan/scat (matches v2 eigen-ternary.bin)
    size_t n_pts;
} v7_ts_outputs_t;

void v7_ts_params_default(v7_ts_params_t* p);
void v7_ts_outputs_init(v7_ts_outputs_t* out);
void v7_ts_outputs_free(v7_ts_outputs_t* out);

// Compute all per-point signals. `positions` is interleaved (x,y,z) Float64
// of length 3*n_pts. `tree` must be a kd-tree previously built over those
// positions (see kd_tree.h — kd_build or kd_deserialize). Caller retains
// ownership of both `positions` and `tree`.
//
// Phase 5.1c — kd-tree was previously built internally; factored out so it
// can be reused across the v7 chain (Phase 5.2+ steps consume the same
// `building-kd-trees` artifact).
int v7_ts_compute(const double* positions, size_t n_pts,
                  const kd_tree_t* tree,
                  const v7_ts_params_t* params,
                  v7_ts_outputs_t* out);

#ifdef __cplusplus
}
#endif

#endif

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-ransac-plane — 3-point cross-product plane RANSAC. Direct port
// of bench-v2 lib/algorithms/_common.js::_ransacPlaneGlobal (the
// Float32Array global-index path that density-voids-2 uses).
//
// Determinism: the RNG must be Mulberry32 (v7-mulberry32.h) seeded
// the same way as v2 (default 42). Skip-on-degenerate (cross-product
// length < 1e-10) is the part that makes the RNG sequence sensitive
// to small numeric drift — the cross-product is computed in F64, so
// fine.

#ifndef V3_STEPS_V7_RANSAC_PLANE_H
#define V3_STEPS_V7_RANSAC_PLANE_H

#include <stddef.h>
#include <stdint.h>

#include "v7-mulberry32.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct v7_ransac_result {
    double normal[3];
    double d;
    uint32_t* inlier_indices;   // INTO the input `indices` array (NOT global)
    size_t n_inliers;
} v7_ransac_result_t;

// Init/free.
void v7_ransac_result_init(v7_ransac_result_t* r);
void v7_ransac_result_free(v7_ransac_result_t* r);

// RANSAC over `positions[indices[*]]`. `positions` is F64 globals
// (interleaved x,y,z); `indices` is U32 globals (length n_indices).
// Returns 0 on success (out->n_inliers >= 3), -1 if no plane found
// (or out of memory). Out's inlier_indices are LOCAL indices into
// `indices[]`, matching v2's API.
int v7_ransac_plane(const double* positions,
                    const uint32_t* indices, size_t n_indices,
                    int iterations,
                    double dist_threshold,
                    v7_rng_t* rng,
                    v7_ransac_result_t* out);

#ifdef __cplusplus
}
#endif
#endif

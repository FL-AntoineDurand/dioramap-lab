// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Sobel edge magnitude + adaptive normalisation helpers for
// coregister-ortho-lidar. Direct C port of bench-v2
// viewer/backend/registration/edges.js.
//
// Every helper operates on F32 rasters with NaN-as-no-data sentinel.

#ifndef V3_STEPS_REGISTRATION_EDGES_H
#define V3_STEPS_REGISTRATION_EDGES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Fill NaN cells in `src` with `fill_val`. If `fill_val` is NAN (any NaN
// bit-pattern), use the global min of valid cells instead. Returns a
// freshly-allocated heap buffer of length `cells` on success, NULL on OOM.
float* edges_fill_nodata(const float* src, size_t cells, float fill_val);

// Sobel gradient magnitude over a cols×rows F32 raster. Writes into
// caller-allocated `out` (same size as input). Border cells (1-cell ring)
// are set to 0.
void edges_sobel_magnitude(const float* img, uint32_t cols, uint32_t rows, float* out);

// Adaptive contrast normalisation in-place: subtract global mean, divide
// by global stddev, then clip negatives to 0 and rescale the top 4σ tail
// to 1. All other values end up in [0, 1].
void edges_adaptive_normalize(float* arr, size_t n);

// Zero the outer `margin` cells of the edge map. Used for binary-mask pairs
// where the no-data boundary itself is the signal being correlated.
void edges_mask_border_only(float* edges, uint32_t cols, uint32_t rows, int margin);

// Zero the border ring AND any edge cell whose 3×3 neighbourhood in
// `source` touches a NaN. Used for regular data rasters to suppress tile-
// edge and no-data-circle artefacts before scoring.
void edges_mask_boundaries(float* edges, const float* source, uint32_t cols, uint32_t rows,
                           int margin);

#ifdef __cplusplus
}
#endif

#endif

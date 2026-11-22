// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Small helper shared by the ortho raster steps: read canonical-location
// + a `cell_size_m` param and compute the target (cols, rows) grid that
// matches what `build-lidar-rasters` would produce. Keeps the ortho and
// LiDAR raster shapes identical so coregistration can pixel-align them.

#ifndef V3_STEPS_ORTHO_GRID_SHAPE_H
#define V3_STEPS_ORTHO_GRID_SHAPE_H

#include "v3/step.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct grid_shape {
    double bbox[4];   // L93 (xmin, ymin, xmax, ymax)
    double cell_size; // metres
    uint32_t cols;
    uint32_t rows;
} grid_shape_t;

// Reads `canonical-location` from the runner artifact map and resolves the
// cell-size param (default 0.5 m). Computes (cols, rows) the same way
// build-lidar-rasters does. Returns 0 on success, -1 on bad input.
int grid_shape_resolve(step_context_t* ctx, grid_shape_t* out);

#ifdef __cplusplus
}
#endif

#endif

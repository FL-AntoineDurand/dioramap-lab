// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#ifndef V3_STEPS_SHADOW_CAST_KERNEL_H
#define V3_STEPS_SHADOW_CAST_KERNEL_H

// Inner ray-march shadow kernel — shared between cast-shadows-from-dsm
// (one cast per pipeline run) and find-best-sun-time (hundreds of casts
// during the coarse-to-fine sun search).
//
// Header-only inline. One call per output cell. Self-contained, no
// allocations, no branches that depend on per-thread state. Exact 1:1
// mapping target for a future CUDA kernel: each (r, c) becomes one CUDA
// thread, the dsm + step constants are read-only inputs, the output is
// a single float per cell.

#include <math.h>
#include <stddef.h>
#include <stdint.h>

static inline float cast_shadow_kernel(const float* maxZ, uint32_t cols, uint32_t rows,
                                       int c, int r, float cell_size, float step_x,
                                       float step_y, float step_z, int max_steps,
                                       float epsilon) {
    float z0 = maxZ[(size_t)r * cols + c];
    if (isnan(z0)) {
        return 0.0f; // no data → mark as lit so NaN cells don't look shadowy
    }
    float x = ((float)c + 0.5f) * cell_size;
    float y = ((float)r + 0.5f) * cell_size;
    float z = z0 + epsilon;
    for (int s = 0; s < max_steps; s++) {
        x += step_x;
        y += step_y;
        z += step_z;
        int cc = (int)floorf(x / cell_size);
        int rr = (int)floorf(y / cell_size);
        if (cc < 0 || cc >= (int)cols || rr < 0 || rr >= (int)rows) {
            return 0.0f;
        }
        float dsmZ = maxZ[(size_t)rr * cols + cc];
        if (!isnan(dsmZ) && dsmZ > z) {
            return 1.0f;
        }
    }
    return 0.0f;
}

// Convenience wrapper: casts the whole grid using the given sun direction
// (in L93 axes, x=east, y=north, z=up). Returns the number of cells in
// shadow on success, -1 if the sun is below horizon or overhead and the
// caller should fast-path. The caller owns `out` (cells × sizeof(float)).
//
// The rules below match cast-shadows-from-dsm.c and findBestSunTime in
// the JS bench:
//   - dz <= 0: all cells in shadow.
//   - horizontal magnitude < 1e-6: no shadows possible.
//   - otherwise: per-cell ray march with 1-cellSize horizontal step.
static inline void cast_shadow_grid(const float* maxZ, uint32_t cols, uint32_t rows,
                                    double sun_dx, double sun_dy, double sun_dz,
                                    double cell_size_m, double epsilon_m, float* out) {
    size_t cells = (size_t)cols * rows;
    if (sun_dz <= 0) {
        for (size_t i = 0; i < cells; i++) out[i] = 1.0f;
        return;
    }
    double horiz = sqrt(sun_dx * sun_dx + sun_dy * sun_dy);
    if (horiz < 1e-6) {
        for (size_t i = 0; i < cells; i++) out[i] = 0.0f;
        return;
    }
    float cell = (float)cell_size_m;
    float eps = (float)epsilon_m;
    float step_x = (float)((sun_dx / horiz) * cell_size_m);
    float step_y = (float)((sun_dy / horiz) * cell_size_m);
    float step_z = (float)((sun_dz / horiz) * cell_size_m);
    int max_steps = (int)ceil(sqrt((double)cols * cols + (double)rows * rows));
    for (uint32_t r = 0; r < rows; r++) {
        for (uint32_t c = 0; c < cols; c++) {
            out[(size_t)r * cols + c] = cast_shadow_kernel(maxZ, cols, rows, (int)c, (int)r,
                                                            cell, step_x, step_y, step_z,
                                                            max_steps, eps);
        }
    }
}

#endif // V3_STEPS_SHADOW_CAST_KERNEL_H

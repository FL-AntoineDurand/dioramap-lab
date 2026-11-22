// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// compute-normal-scatter
// Reads:  lidar-positions (F64), lidar-normals (F32), canonical-location
// Params: { "cell_size_m": float (default 1.0) }
// Writes: normal-scatter — 2-channel F32 raster:
//           channel 0 = average per-point angular deviation from the cell
//                       mean normal, in degrees. 0 for cells with <2 points.
//           channel 1 = cell min-Z (metres). INFINITY for empty cells.
//
// Two-pass binning. Pass 1: accumulate normal sums + count + min-z per
// cell. Pass 2: compute angular deviation of each point from the
// cell's (normalised) mean normal, then average. The two channels are
// co-produced because both need the same bin-and-scan over the point
// cloud; sharing it halves the I/O.
//
// Matches the "normal scatter raster" block of bench-v2
// classify-vegetation.js.

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "v3/artifact.h"
#include "v3/safe_alloc.h"
#include "v3/step.h"
#include "v3/step_bbox_params.h"

#include "../registration/raster_format.h"
#include "classification-steps.h"

#define DEG_PER_RAD 57.29577951308232

static int exec_fn(step_context_t* ctx) {
    double cell_size = step_parse_cell_size_m(step_get_params(ctx), 1.0);

    double bbox[4];
    if (step_read_bbox_l93(ctx, bbox) != 0) return -1;

    size_t pos_bytes = 0;
    const double* positions = (const double*)step_read(ctx, "lidar-positions", &pos_bytes);
    if (!positions || pos_bytes % (3 * sizeof(double)) != 0) return -1;
    size_t N = pos_bytes / (3 * sizeof(double));

    size_t nrm_bytes = 0;
    const float* normals = (const float*)step_read(ctx, "lidar-normals", &nrm_bytes);
    if (!normals || nrm_bytes != N * 3 * sizeof(float)) return -1;

    double width_m = bbox[2] - bbox[0];
    double height_m = bbox[3] - bbox[1];
    if (!(width_m > 0) || !(height_m > 0)) return -1;
    int64_t cols_i = (int64_t)round(width_m / cell_size);
    int64_t rows_i = (int64_t)round(height_m / cell_size);
    if (cols_i < 2) cols_i = 2;
    if (rows_i < 2) rows_i = 2;
    if (cols_i > 65535 || rows_i > 65535) return -1;
    uint32_t cols = (uint32_t)cols_i;
    uint32_t rows = (uint32_t)rows_i;
    size_t cells = (size_t)cols * rows;

    // Per-cell running state.
    double* sum_nx = safe_calloc_array(cells, sizeof(double));
    double* sum_ny = safe_calloc_array(cells, sizeof(double));
    double* sum_nz = safe_calloc_array(cells, sizeof(double));
    uint32_t* count = safe_calloc_array(cells, sizeof(uint32_t));
    float* min_z = safe_malloc_array(cells, sizeof(float));
    if (!sum_nx || !sum_ny || !sum_nz || !count || !min_z) {
        free(sum_nx); free(sum_ny); free(sum_nz); free(count); free(min_z);
        return -1;
    }
    for (size_t i = 0; i < cells; i++) min_z[i] = INFINITY;

    double xmin = bbox[0];
    double ymin = bbox[1];
    double inv_cell = 1.0 / cell_size;

    // Pass 1: bin + accumulate.
    for (size_t i = 0; i < N; i++) {
        double x = positions[i * 3 + 0];
        double y = positions[i * 3 + 1];
        double z = positions[i * 3 + 2];
        int64_t c = (int64_t)floor((x - xmin) * inv_cell);
        int64_t r = (int64_t)floor((y - ymin) * inv_cell);
        if (c < 0 || c >= (int64_t)cols || r < 0 || r >= (int64_t)rows) continue;
        size_t k = (size_t)r * cols + (size_t)c;
        sum_nx[k] += normals[i * 3 + 0];
        sum_ny[k] += normals[i * 3 + 1];
        sum_nz[k] += normals[i * 3 + 2];
        count[k]++;
        if ((float)z < min_z[k]) min_z[k] = (float)z;
    }

    // Normalise per-cell mean normals (needed for pass 2 dot products).
    double* mean_nx = safe_calloc_array(cells, sizeof(double));
    double* mean_ny = safe_calloc_array(cells, sizeof(double));
    double* mean_nz = safe_calloc_array(cells, sizeof(double));
    if (!mean_nx || !mean_ny || !mean_nz) {
        free(sum_nx); free(sum_ny); free(sum_nz); free(count); free(min_z);
        free(mean_nx); free(mean_ny); free(mean_nz);
        return -1;
    }
    for (size_t k = 0; k < cells; k++) {
        if (count[k] < 2) continue;
        double len = sqrt(sum_nx[k] * sum_nx[k] + sum_ny[k] * sum_ny[k] +
                          sum_nz[k] * sum_nz[k]);
        if (len < 1e-12) len = 1.0;
        mean_nx[k] = sum_nx[k] / len;
        mean_ny[k] = sum_ny[k] / len;
        mean_nz[k] = sum_nz[k] / len;
    }
    free(sum_nx); free(sum_ny); free(sum_nz);

    // Pass 2: angular deviation sum.
    double* dev_sum = safe_calloc_array(cells, sizeof(double));
    if (!dev_sum) {
        free(count); free(min_z); free(mean_nx); free(mean_ny); free(mean_nz);
        return -1;
    }
    for (size_t i = 0; i < N; i++) {
        double x = positions[i * 3 + 0];
        double y = positions[i * 3 + 1];
        int64_t c = (int64_t)floor((x - xmin) * inv_cell);
        int64_t r = (int64_t)floor((y - ymin) * inv_cell);
        if (c < 0 || c >= (int64_t)cols || r < 0 || r >= (int64_t)rows) continue;
        size_t k = (size_t)r * cols + (size_t)c;
        if (count[k] < 2) continue;
        double dot = (double)normals[i * 3 + 0] * mean_nx[k] +
                     (double)normals[i * 3 + 1] * mean_ny[k] +
                     (double)normals[i * 3 + 2] * mean_nz[k];
        if (dot > 1.0) dot = 1.0;
        else if (dot < -1.0) dot = -1.0;
        dev_sum[k] += acos(dot) * DEG_PER_RAD;
    }

    uint8_t* out_buf = raster_alloc(cols, rows, 2);
    if (!out_buf) {
        free(count); free(min_z); free(mean_nx); free(mean_ny); free(mean_nz);
        free(dev_sum);
        return -1;
    }
    float* out_scatter = raster_channel_ptr(out_buf, cols, rows, 0);
    float* out_min_z = raster_channel_ptr(out_buf, cols, rows, 1);
    for (size_t k = 0; k < cells; k++) {
        out_scatter[k] = count[k] >= 2 ? (float)(dev_sum[k] / count[k]) : 0.0f;
        out_min_z[k] = min_z[k];
    }
    free(count); free(min_z); free(mean_nx); free(mean_ny); free(mean_nz);
    free(dev_sum);

    return step_write_output_owned(ctx, "normal-scatter", out_buf,
                                   raster_total_bytes(cols, rows, 2));
}

static const step_read_t reads_arr[] = {
    {.name = "lidar-positions"},
    {.name = "lidar-normals"},
    {.name = "canonical-location"},
};
static const step_write_t writes_arr[] = {
    {.name = "normal-scatter", .kind = AK_RASTER_F32, .visibility = STEP_VIS_DEBUG},
};

const step_def_t compute_normal_scatter_def = {
    .name = "compute-normal-scatter",
    .version = 1,
    .reads = reads_arr,
    .n_reads = 3,
    .writes = writes_arr,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};

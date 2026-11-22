// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// build-lidar-rasters
// Reads:  lidar-positions       (F64 interleaved x,y,z, L93 metres)
//         lidar-intensities     (U16 per point)
//         lidar-num-returns     (U8 per point)
//         canonical-location    (JSON, supplies bbox_l93)
// Params: { "cell_size_m": float (default 0.5) }
// Writes: lidar-rasters — multi-channel F32 raster artifact (raster_format.h).
//         Channel order: maxZ, minZ, meanZ, density, intensity, roughness,
//         multiReturn — see lidar_raster_channel_t.
//
// Single-pass binning over the disc point cloud. NaN sentinel for cells that
// received zero points (matches bench-v2 backend/registration/rasters.js).

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "v3/artifact.h"
#include "v3/safe_alloc.h"
#include "v3/step.h"
#include "v3/step_bbox_params.h"

#include "raster_format.h"
#include "registration-steps.h"

static int exec_fn(step_context_t* ctx) {
    size_t pos_bytes = 0;
    const double* positions = (const double*)step_read(ctx, "lidar-positions", &pos_bytes);
    if (!positions || pos_bytes % (3 * sizeof(double)) != 0) {
        return -1;
    }
    size_t N = pos_bytes / (3 * sizeof(double));

    size_t int_bytes = 0;
    const uint16_t* intensities = (const uint16_t*)step_read(ctx, "lidar-intensities", &int_bytes);
    if (!intensities || int_bytes != N * sizeof(uint16_t)) {
        return -1;
    }

    size_t nr_bytes = 0;
    const uint8_t* num_returns = (const uint8_t*)step_read(ctx, "lidar-num-returns", &nr_bytes);
    if (!num_returns || nr_bytes != N * sizeof(uint8_t)) {
        return -1;
    }

    double bbox[4];
    if (step_read_bbox_l93(ctx, bbox) != 0) {
        return -1;
    }

    double cell_size = step_parse_cell_size_m(step_get_params(ctx), 0.5);

    double width_m = bbox[2] - bbox[0];
    double height_m = bbox[3] - bbox[1];
    if (!(width_m > 0) || !(height_m > 0)) {
        return -1;
    }
    int64_t cols_i = (int64_t)floor(width_m / cell_size);
    int64_t rows_i = (int64_t)floor(height_m / cell_size);
    if (cols_i < 2) {
        cols_i = 2;
    }
    if (rows_i < 2) {
        rows_i = 2;
    }
    if (cols_i > 65535 || rows_i > 65535) {
        return -1; // would overflow practical limits
    }
    uint32_t cols = (uint32_t)cols_i;
    uint32_t rows = (uint32_t)rows_i;
    size_t cells = (size_t)cols * rows;

    // Per-cell accumulators in higher-precision types so we don't lose
    // information when cells gather many points.
    double* sum_z = safe_calloc_array(cells, sizeof(double));
    double* sum_z2 = safe_calloc_array(cells, sizeof(double));
    double* sum_i = safe_calloc_array(cells, sizeof(double));
    uint32_t* sum_mr = safe_calloc_array(cells, sizeof(uint32_t));
    uint32_t* cnt = safe_calloc_array(cells, sizeof(uint32_t));
    float* max_z = safe_malloc_array(cells, sizeof(float));
    float* min_z = safe_malloc_array(cells, sizeof(float));
    if (!sum_z || !sum_z2 || !sum_i || !sum_mr || !cnt || !max_z || !min_z) {
        free(sum_z); free(sum_z2); free(sum_i); free(sum_mr); free(cnt);
        free(max_z); free(min_z);
        return -1;
    }

    double xmin = bbox[0];
    double ymin = bbox[1];
    double inv_cell = 1.0 / cell_size;

    // Single-pass binning. Track maxZ/minZ inline (fewer passes), running
    // sums for the post-pass averages + variance.
    for (size_t i = 0; i < N; i++) {
        double x = positions[i * 3 + 0];
        double y = positions[i * 3 + 1];
        double z = positions[i * 3 + 2];
        int64_t c = (int64_t)floor((x - xmin) * inv_cell);
        int64_t r = (int64_t)floor((y - ymin) * inv_cell);
        if (c < 0 || c >= (int64_t)cols || r < 0 || r >= (int64_t)rows) {
            continue;
        }
        size_t k = (size_t)r * cols + (size_t)c;
        if (cnt[k] == 0) {
            max_z[k] = (float)z;
            min_z[k] = (float)z;
        } else {
            if ((float)z > max_z[k]) max_z[k] = (float)z;
            if ((float)z < min_z[k]) min_z[k] = (float)z;
        }
        sum_z[k] += z;
        sum_z2[k] += z * z;
        sum_i[k] += (double)intensities[i];
        if (num_returns[i] > 1) {
            sum_mr[k]++;
        }
        cnt[k]++;
    }

    // Allocate the output artifact buffer; we'll fill 7 channels in place.
    uint8_t* buf = raster_alloc(cols, rows, LIDAR_RC__COUNT);
    if (!buf) {
        free(sum_z); free(sum_z2); free(sum_i); free(sum_mr); free(cnt);
        free(max_z); free(min_z);
        return -1;
    }
    float* out_max_z = raster_channel_ptr(buf, cols, rows, LIDAR_RC_MAX_Z);
    float* out_min_z = raster_channel_ptr(buf, cols, rows, LIDAR_RC_MIN_Z);
    float* out_mean_z = raster_channel_ptr(buf, cols, rows, LIDAR_RC_MEAN_Z);
    float* out_density = raster_channel_ptr(buf, cols, rows, LIDAR_RC_DENSITY);
    float* out_intensity = raster_channel_ptr(buf, cols, rows, LIDAR_RC_INTENSITY);
    float* out_roughness = raster_channel_ptr(buf, cols, rows, LIDAR_RC_ROUGHNESS);
    float* out_mr = raster_channel_ptr(buf, cols, rows, LIDAR_RC_MULTI_RETURN);

    // Reduce: sum/cnt → mean, sqrt(variance) → roughness, etc. NaN sentinel
    // for empty cells (matches bench backend rasters.js).
    float nan = (float)NAN;
    for (size_t k = 0; k < cells; k++) {
        uint32_t n = cnt[k];
        out_density[k] = (float)n;
        if (n == 0) {
            out_max_z[k] = nan;
            out_min_z[k] = nan;
            out_mean_z[k] = nan;
            out_intensity[k] = nan;
            out_roughness[k] = nan;
            out_mr[k] = nan;
            continue;
        }
        out_max_z[k] = max_z[k];
        out_min_z[k] = min_z[k];
        double m = sum_z[k] / n;
        out_mean_z[k] = (float)m;
        out_intensity[k] = (float)(sum_i[k] / n);
        if (n >= 2) {
            double var = sum_z2[k] / n - m * m;
            if (var < 0) {
                var = 0;
            }
            out_roughness[k] = (float)sqrt(var);
        } else {
            out_roughness[k] = 0.0f;
        }
        out_mr[k] = (float)sum_mr[k] / (float)n;
    }

    free(sum_z); free(sum_z2); free(sum_i); free(sum_mr); free(cnt);
    free(max_z); free(min_z);

    return step_write_output_owned(ctx, "lidar-rasters", buf,
                                   raster_total_bytes(cols, rows, LIDAR_RC__COUNT));
}

static const step_read_t reads_arr[] = {
    {.name = "lidar-positions"},
    {.name = "lidar-intensities"},
    {.name = "lidar-num-returns"},
    {.name = "canonical-location"},
};
static const step_write_t writes_arr[] = {
    {.name = "lidar-rasters", .kind = AK_RASTER_F32, .visibility = STEP_VIS_DEBUG},
};

const step_def_t build_lidar_rasters_def = {
    .name = "build-lidar-rasters",
    .version = 1,
    .reads = reads_arr,
    .n_reads = 4,
    .writes = writes_arr,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// classify-vegetation
// Reads:  lidar-positions, lidar-normals,
//         tile-ndvi, normal-scatter (2 channels: scatter + cell_min_z),
//         lidar-near-building-mask, canonical-location
// Params: { "cell_size_m": float (default 1.0), thresholds see below }
// Writes: canopy-flags — U8 per point (0/1), same point order as lidar-positions.
//         grass-flags  — U8 per point (0/1).
//
// Per-point canopy + grass classification. Canopy requires:
//   |normal.z| >= canopy_normal_up_min
//   height-above-cell-min-z >= canopy_height_min
//   (height > height_relax_hi AND not near-building) OR
//   (ndvi > ndvi_threshold * relax  AND  scatter > scatter_threshold * relax)
// where `relax` is 1.0 at low height, dropping linearly to thresh_min_frac at
// high height (outside near-building cells only). Grass is: NDVI > threshold,
// not canopy, not near-building.
//
// Direct port of the per-point classification block in bench-v2
// classify-vegetation.js.

#include <jansson.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "v3/artifact.h"
#include "v3/step.h"
#include "v3/step_bbox_params.h"

#include "../registration/raster_format.h"
#include "classification-steps.h"

typedef struct classify_params {
    double cell_size_m;
    double ndvi_threshold;
    double scatter_threshold_deg;
    double canopy_height_min;
    double canopy_normal_up_min;
    double height_relax_lo;
    double height_relax_hi;
    double thresh_min_frac;
} classify_params_t;

static void parse_params(const char* s, classify_params_t* o) {
    // Defaults match bench-v2 classify-vegetation.js constants.
    o->cell_size_m = 1.0;
    o->ndvi_threshold = 0.1;
    o->scatter_threshold_deg = 10.0;
    o->canopy_height_min = 1.0;
    o->canopy_normal_up_min = 0.3;
    o->height_relax_lo = 1.0;
    o->height_relax_hi = 4.0;
    o->thresh_min_frac = 0.15;
    if (!s) return;
    json_error_t je;
    json_t* p = json_loads(s, 0, &je);
    if (!p) return;
#define READ_DOUBLE(k, field)                                                                     \
    do {                                                                                          \
        json_t* j = json_object_get(p, k);                                                        \
        if (json_is_number(j)) o->field = json_number_value(j);                                   \
    } while (0)
    READ_DOUBLE("cell_size_m", cell_size_m);
    READ_DOUBLE("ndvi_threshold", ndvi_threshold);
    READ_DOUBLE("scatter_threshold_deg", scatter_threshold_deg);
    READ_DOUBLE("canopy_height_min", canopy_height_min);
    READ_DOUBLE("canopy_normal_up_min", canopy_normal_up_min);
    READ_DOUBLE("height_relax_lo", height_relax_lo);
    READ_DOUBLE("height_relax_hi", height_relax_hi);
    READ_DOUBLE("thresh_min_frac", thresh_min_frac);
#undef READ_DOUBLE
    json_decref(p);
}

static int open_raster(step_context_t* ctx, const char* name, raster_view_t* out) {
    size_t len = 0;
    const uint8_t* bytes = (const uint8_t*)step_read(ctx, name, &len);
    if (!bytes) return -1;
    return raster_view_open(bytes, len, out);
}

static int exec_fn(step_context_t* ctx) {
    classify_params_t params;
    parse_params(step_get_params(ctx), &params);

    double bbox[4];
    if (step_read_bbox_l93(ctx, bbox) != 0) return -1;

    size_t pos_bytes = 0;
    const double* positions = (const double*)step_read(ctx, "lidar-positions", &pos_bytes);
    if (!positions || pos_bytes % (3 * sizeof(double)) != 0) return -1;
    size_t N = pos_bytes / (3 * sizeof(double));

    size_t nrm_bytes = 0;
    const float* normals = (const float*)step_read(ctx, "lidar-normals", &nrm_bytes);
    if (!normals || nrm_bytes != N * 3 * sizeof(float)) return -1;

    raster_view_t ndvi_v, scatter_v, near_v;
    if (open_raster(ctx, "tile-ndvi", &ndvi_v) != 0) return -1;
    if (open_raster(ctx, "normal-scatter", &scatter_v) != 0) return -1;
    if (open_raster(ctx, "lidar-near-building-mask", &near_v) != 0) return -1;

    if (ndvi_v.n_channels < 1 || scatter_v.n_channels < 2 || near_v.n_channels < 1) {
        return -1;
    }
    uint32_t cols = ndvi_v.cols;
    uint32_t rows = ndvi_v.rows;
    if (scatter_v.cols != cols || scatter_v.rows != rows) return -1;
    if (near_v.cols != cols || near_v.rows != rows) return -1;

    const float* ndvi = ndvi_v.channels[0];
    const float* scatter = scatter_v.channels[0];
    const float* cell_min_z = scatter_v.channels[1];
    const float* near = near_v.channels[0];

    uint8_t* canopy = calloc(N, 1);
    uint8_t* grass = calloc(N, 1);
    if (!canopy || !grass) {
        free(canopy);
        free(grass);
        return -1;
    }

    double xmin = bbox[0];
    double ymin = bbox[1];
    double inv_cell = 1.0 / params.cell_size_m;

    for (size_t i = 0; i < N; i++) {
        double x = positions[i * 3 + 0];
        double y = positions[i * 3 + 1];
        double z = positions[i * 3 + 2];
        double nz = (double)normals[i * 3 + 2];

        int64_t c = (int64_t)floor((x - xmin) * inv_cell);
        int64_t r = (int64_t)floor((y - ymin) * inv_cell);
        if (c < 0 || c >= (int64_t)cols || r < 0 || r >= (int64_t)rows) continue;
        size_t k = (size_t)r * cols + (size_t)c;

        bool is_near = near[k] >= 0.5f;
        bool is_canopy = false;

        if (fabs(nz) >= params.canopy_normal_up_min) {
            float gz = cell_min_z[k];
            if (isfinite(gz)) {
                double height = z - gz;
                if (height >= params.canopy_height_min) {
                    // Compute relaxation factor: 1.0 at low height, falls to
                    // thresh_min_frac at high. No relaxation near buildings.
                    double relax = 1.0;
                    if (!is_near) {
                        double t = (height - params.height_relax_lo) /
                                   (params.height_relax_hi - params.height_relax_lo);
                        if (t < 0) t = 0;
                        else if (t > 1) t = 1;
                        relax = 1.0 - t * (1.0 - params.thresh_min_frac);
                    }
                    if (!is_near && height > 4.0) {
                        // Outside buildings, tall points skip NDVI.
                        is_canopy = scatter[k] > params.scatter_threshold_deg * relax;
                    } else {
                        double ndvi_val = ndvi[k];
                        if (ndvi_val > params.ndvi_threshold * relax &&
                            scatter[k] > params.scatter_threshold_deg * relax) {
                            is_canopy = true;
                        }
                    }
                }
            }
        }

        if (is_canopy) {
            canopy[i] = 1;
        } else if (ndvi[k] > params.ndvi_threshold && !is_near) {
            grass[i] = 1;
        }
    }

    if (step_write_output_owned(ctx, "canopy-flags", canopy, N) != 0) {
        // canopy has been transferred to the store on success; on failure the
        // helper frees it. grass still owned here.
        free(grass);
        return -1;
    }
    return step_write_output_owned(ctx, "grass-flags", grass, N);
}

static const step_read_t reads_arr[] = {
    {.name = "lidar-positions"},
    {.name = "lidar-normals"},
    {.name = "tile-ndvi"},
    {.name = "normal-scatter"},
    {.name = "lidar-near-building-mask"},
    {.name = "canonical-location"},
};
static const step_write_t writes_arr[] = {
    {.name = "canopy-flags", .kind = AK_POINT_COLUMN, .visibility = STEP_VIS_PUBLIC},
    {.name = "grass-flags", .kind = AK_POINT_COLUMN, .visibility = STEP_VIS_PUBLIC},
};

const step_def_t classify_vegetation_def = {
    .name = "classify-vegetation",
    .version = 1,
    .reads = reads_arr,
    .n_reads = 6,
    .writes = writes_arr,
    .n_writes = 2,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};

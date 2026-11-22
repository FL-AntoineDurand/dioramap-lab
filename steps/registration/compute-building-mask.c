// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// compute-building-mask
// Reads:  lidar-ndsm     (single-channel F32)
//         lidar-rasters  (uses roughness + multiReturn channels)
// Writes: lidar-building-mask — single-channel F32 raster (0.0 or 1.0).
//
// Building-likely test (per bench-v2 backend):
//   NDSM > 2 m  AND  roughness < 0.4 m  AND  multiReturn < 0.15
// Cells with NaN inputs default to 0 (not building). When the multiReturn
// channel is entirely NaN (e.g., source doesn't carry per-point return
// counts), the multiReturn condition is dropped — same fallback as bench.

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "v3/artifact.h"
#include "v3/step.h"

#include "raster_format.h"
#include "registration-steps.h"

static int exec_fn(step_context_t* ctx) {
    size_t ndsm_bytes = 0;
    const uint8_t* ndsm_in = (const uint8_t*)step_read(ctx, "lidar-ndsm", &ndsm_bytes);
    if (!ndsm_in) {
        return -1;
    }
    raster_view_t ndsm_view;
    if (raster_view_open(ndsm_in, ndsm_bytes, &ndsm_view) != 0) {
        return -1;
    }
    if (ndsm_view.n_channels != 1) {
        return -1;
    }
    const float* ndsm = ndsm_view.channels[0];
    uint32_t cols = ndsm_view.cols;
    uint32_t rows = ndsm_view.rows;

    size_t lr_bytes = 0;
    const uint8_t* lr_in = (const uint8_t*)step_read(ctx, "lidar-rasters", &lr_bytes);
    if (!lr_in) {
        return -1;
    }
    raster_view_t lr_view;
    if (raster_view_open(lr_in, lr_bytes, &lr_view) != 0 ||
        lr_view.n_channels < LIDAR_RC__COUNT) {
        return -1;
    }
    if (lr_view.cols != cols || lr_view.rows != rows) {
        return -1;
    }
    const float* roughness = lr_view.channels[LIDAR_RC_ROUGHNESS];
    const float* multi_return = lr_view.channels[LIDAR_RC_MULTI_RETURN];
    size_t cells = (size_t)cols * rows;

    // Whole-raster scan: are there ANY non-NaN multiReturn cells? Bench's
    // fallback covers source data without per-point return counts.
    bool has_mr = false;
    for (size_t i = 0; i < cells; i++) {
        if (!isnan(multi_return[i])) {
            has_mr = true;
            break;
        }
    }

    uint8_t* buf = raster_alloc(cols, rows, 1);
    if (!buf) {
        return -1;
    }
    float* out = raster_channel_ptr(buf, cols, rows, 0);

    for (size_t i = 0; i < cells; i++) {
        float h = ndsm[i];
        float rh = roughness[i];
        if (isnan(h) || isnan(rh)) {
            out[i] = 0.0f;
            continue;
        }
        bool ok = (h > 2.0f) && (rh < 0.4f);
        if (ok && has_mr) {
            float mr = multi_return[i];
            ok = !isnan(mr) && (mr < 0.15f);
        }
        out[i] = ok ? 1.0f : 0.0f;
    }

    return step_write_output_owned(ctx, "lidar-building-mask", buf,
                                   raster_total_bytes(cols, rows, 1));
}

static const step_read_t reads_arr[] = {
    {.name = "lidar-ndsm"},
    {.name = "lidar-rasters"},
};
static const step_write_t writes_arr[] = {
    {.name = "lidar-building-mask", .kind = AK_RASTER_F32, .visibility = STEP_VIS_DEBUG},
};

const step_def_t compute_building_mask_def = {
    .name = "compute-building-mask",
    .version = 1,
    .reads = reads_arr,
    .n_reads = 2,
    .writes = writes_arr,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};

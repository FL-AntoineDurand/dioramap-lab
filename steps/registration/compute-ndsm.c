// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// compute-ndsm
// Reads:  lidar-rasters (uses maxZ channel)
// Params: { "local_min_window": int (default 3) — window radius k; window
//           is (2k+1)² cells }
// Writes: lidar-ndsm — single-channel F32 raster (height above local ground).
//
// NDSM = maxZ minus a morphological local-min over a (2k+1)² window. NaN
// cells are skipped in both directions (don't contribute to local-min;
// stay NaN in output). Direct port of bench-v2 backend computeNDSM.

#include <jansson.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "v3/artifact.h"
#include "v3/json_int.h"
#include "v3/step.h"

#include "raster_format.h"
#include "registration-steps.h"

static int parse_window(const char* params_str) {
    int k = 3;
    if (!params_str) {
        return k;
    }
    json_error_t je;
    json_t* params = json_loads(params_str, 0, &je);
    if (!params) {
        return k;
    }
    json_t* j = json_object_get(params, "local_min_window");
    int cand = k;
    if (json_int_or_default(j, 0, 64, "local_min_window", &cand) != 0) {
        json_decref(params);
        return -1;  // pass error through; caller propagates from exec.
    }
    k = cand;
    json_decref(params);
    return k;
}

static int exec_fn(step_context_t* ctx) {
    size_t in_bytes = 0;
    const uint8_t* in = (const uint8_t*)step_read(ctx, "lidar-rasters", &in_bytes);
    if (!in) {
        return -1;
    }
    raster_view_t v;
    if (raster_view_open(in, in_bytes, &v) != 0) {
        return -1;
    }
    if (v.n_channels < LIDAR_RC__COUNT) {
        return -1;
    }
    const float* max_z = v.channels[LIDAR_RC_MAX_Z];
    uint32_t cols = v.cols;
    uint32_t rows = v.rows;
    int k = parse_window(step_get_params(ctx));

    uint8_t* buf = raster_alloc(cols, rows, 1);
    if (!buf) {
        return -1;
    }
    float* out = raster_channel_ptr(buf, cols, rows, 0);

    // For each output cell, scan its (2k+1)² window for the min of valid maxZ
    // values, then output (maxZ - localMin). Both endpoints must be valid.
    for (int r = 0; r < (int)rows; r++) {
        int r0 = r - k < 0 ? 0 : r - k;
        int r1 = r + k > (int)rows - 1 ? (int)rows - 1 : r + k;
        for (int c = 0; c < (int)cols; c++) {
            int c0 = c - k < 0 ? 0 : c - k;
            int c1 = c + k > (int)cols - 1 ? (int)cols - 1 : c + k;
            float local_min = INFINITY;
            for (int rr = r0; rr <= r1; rr++) {
                for (int cc = c0; cc <= c1; cc++) {
                    float z = max_z[(size_t)rr * cols + cc];
                    if (!isnan(z) && z < local_min) {
                        local_min = z;
                    }
                }
            }
            float self = max_z[(size_t)r * cols + c];
            float val;
            if (isnan(self) || isinf(local_min)) {
                val = NAN;
            } else {
                val = self - local_min;
            }
            out[(size_t)r * cols + c] = val;
        }
    }

    return step_write_output_owned(ctx, "lidar-ndsm", buf,
                                   raster_total_bytes(cols, rows, 1));
}

static const step_read_t reads_arr[] = {
    {.name = "lidar-rasters"},
};
static const step_write_t writes_arr[] = {
    {.name = "lidar-ndsm", .kind = AK_RASTER_F32, .visibility = STEP_VIS_DEBUG},
};

const step_def_t compute_ndsm_def = {
    .name = "compute-ndsm",
    .version = 1,
    .reads = reads_arr,
    .n_reads = 1,
    .writes = writes_arr,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};

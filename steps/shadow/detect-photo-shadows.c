// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// detect-photo-shadows
// Reads:  ortho-rgb-rasters (uses LUMINANCE channel)
// Params: { "radius_cells": int (default 12),
//           "threshold": float (default 0.65) — ratio below which a cell
//                                                counts as in-shadow }
// Writes: photo-shadow-mask — single-channel F32 raster in [0, 1].
//
// Local-relative shadow detector. For each cell, compare its luminance to
// the local maximum within a (2R+1)² window. Cells significantly darker
// than their local max get a non-zero shadow score:
//   ratio = luminance / local_max
//   if ratio < threshold  →  out = (threshold - ratio) / threshold ∈ (0, 1]
//   else                 →  out = 0
//
// Direct port of bench-v2 viewer/backend/preprocess/shadow-fit.js
// detectPhotoShadows. Uses two-pass separable max (horizontal then
// vertical) so each pass is O(N · radius) instead of O(N · radius²).

#include <jansson.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "v3/artifact.h"
#include "v3/safe_alloc.h"
#include "v3/json_int.h"
#include "v3/step.h"

#include "../ortho/ortho-steps.h"
#include "../registration/raster_format.h"
#include "shadow-steps.h"

typedef struct dps_params {
    int radius;
    double threshold;
} dps_params_t;

static void parse_params(const char* s, dps_params_t* o) {
    o->radius = 12;
    o->threshold = 0.65;
    if (!s) return;
    json_error_t je;
    json_t* p = json_loads(s, 0, &je);
    if (!p) return;
    json_t* j = json_object_get(p, "radius_cells");
    if (json_int_or_default(j, 0, 256, "radius_cells", &o->radius) != 0) {
        json_decref(p); return;
    }
    j = json_object_get(p, "threshold");
    if (json_is_number(j)) {
        double v = json_number_value(j);
        if (v > 0 && v < 1) o->threshold = v;
    }
    json_decref(p);
}

// Horizontal max over (2R+1) window. For each row, we use a brute-force
// inner loop instead of a deque-based monotonic queue because R is small
// (~12) and the brute-force is cache-friendly.
static void max_horizontal(const float* in, uint32_t cols, uint32_t rows, int R, float* out) {
    for (uint32_t r = 0; r < rows; r++) {
        size_t row = (size_t)r * cols;
        for (uint32_t c = 0; c < cols; c++) {
            int c0 = (int)c - R < 0 ? 0 : (int)c - R;
            int c1 = (int)c + R > (int)cols - 1 ? (int)cols - 1 : (int)c + R;
            float m = -INFINITY;
            for (int cc = c0; cc <= c1; cc++) {
                float v = in[row + cc];
                if (v > m) m = v;
            }
            out[row + c] = m;
        }
    }
}

// Vertical max over (2R+1) window. Reads from `in` (already horizontal-maxed),
// computes per-cell shadow score in `out`. Single fused pass: vertical-max
// + ratio test.
static void vertical_max_and_score(const float* h_max, const float* lum, uint32_t cols,
                                   uint32_t rows, int R, double threshold, float* out) {
    for (uint32_t c = 0; c < cols; c++) {
        for (uint32_t r = 0; r < rows; r++) {
            int r0 = (int)r - R < 0 ? 0 : (int)r - R;
            int r1 = (int)r + R > (int)rows - 1 ? (int)rows - 1 : (int)r + R;
            float m = -INFINITY;
            for (int rr = r0; rr <= r1; rr++) {
                float v = h_max[(size_t)rr * cols + c];
                if (v > m) m = v;
            }
            size_t k = (size_t)r * cols + c;
            float l = lum[k];
            float score = 0.0f;
            if (m >= 0.01f) {
                float ratio = l / m;
                if (ratio < threshold) {
                    score = (float)((threshold - ratio) / threshold);
                }
            }
            out[k] = score;
        }
    }
}

static int exec_fn(step_context_t* ctx) {
    dps_params_t params;
    parse_params(step_get_params(ctx), &params);

    size_t lr_bytes = 0;
    const uint8_t* lr_in = (const uint8_t*)step_read(ctx, "ortho-rgb-rasters", &lr_bytes);
    if (!lr_in) return -1;
    raster_view_t lr;
    if (raster_view_open(lr_in, lr_bytes, &lr) != 0 || lr.n_channels < ORGB__COUNT) {
        return -1;
    }
    uint32_t cols = lr.cols;
    uint32_t rows = lr.rows;
    size_t cells = (size_t)cols * rows;
    const float* luminance = lr.channels[ORGB_LUMINANCE];

    float* h_max = safe_malloc_array(cells, sizeof(float));
    if (!h_max) return -1;
    max_horizontal(luminance, cols, rows, params.radius, h_max);

    uint8_t* buf = raster_alloc(cols, rows, 1);
    if (!buf) {
        free(h_max);
        return -1;
    }
    float* out = raster_channel_ptr(buf, cols, rows, 0);
    vertical_max_and_score(h_max, luminance, cols, rows, params.radius, params.threshold, out);
    free(h_max);

    return step_write_output_owned(ctx, "photo-shadow-mask", buf,
                                   raster_total_bytes(cols, rows, 1));
}

static const step_read_t reads_arr[] = {
    {.name = "ortho-rgb-rasters"},
};
static const step_write_t writes_arr[] = {
    {.name = "photo-shadow-mask", .kind = AK_RASTER_F32, .visibility = STEP_VIS_DEBUG},
};

const step_def_t detect_photo_shadows_def = {
    .name = "detect-photo-shadows",
    .version = 1,
    .reads = reads_arr,
    .n_reads = 1,
    .writes = writes_arr,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};

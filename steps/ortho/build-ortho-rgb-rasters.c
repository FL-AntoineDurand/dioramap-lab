// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// build-ortho-rgb-rasters
// Reads:  ortho-rgb-jpeg, canonical-location
// Params: { "cell_size_m": float (default 0.5) — must match build-lidar-rasters }
// Writes: ortho-rgb-rasters — 7-channel F32 raster:
//           r, g, b, luminance, saturation, greenness, lumStddev
//
// Decode the JPEG, bilinear-resample + Y-flip to the LiDAR (cols, rows)
// grid, then compute the seven derived channels per cell.
//
// Direct port of bench-v2 viewer/backend/registration/rasters.js
// buildOrthoRGBRasters + computeLocalStddev.

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "v3/artifact.h"
#include "v3/step.h"

#include "../registration/raster_format.h"
#include "grid_shape.h"
#include "jpeg_decode.h"
#include "ortho-steps.h"

// (2k+1)² window local stddev. Skips no-data (NaN) inputs as a contribution
// — same NaN handling as bench computeLocalStddev.
static void compute_local_stddev(const float* img, uint32_t cols, uint32_t rows, int k,
                                 float* out) {
    for (uint32_t r = 0; r < rows; r++) {
        int r0 = (int)r - k < 0 ? 0 : (int)r - k;
        int r1 = (int)r + k > (int)rows - 1 ? (int)rows - 1 : (int)r + k;
        for (uint32_t c = 0; c < cols; c++) {
            int c0 = (int)c - k < 0 ? 0 : (int)c - k;
            int c1 = (int)c + k > (int)cols - 1 ? (int)cols - 1 : (int)c + k;
            double s = 0;
            double s2 = 0;
            int n = 0;
            for (int rr = r0; rr <= r1; rr++) {
                for (int cc = c0; cc <= c1; cc++) {
                    float v = img[(size_t)rr * cols + cc];
                    if (isnan(v)) continue;
                    s += v;
                    s2 += (double)v * v;
                    n++;
                }
            }
            float val = 0.0f;
            if (n > 1) {
                double m = s / n;
                double var = s2 / n - m * m;
                if (var < 0) var = 0;
                val = (float)sqrt(var);
            }
            out[(size_t)r * cols + c] = val;
        }
    }
}

static int exec_fn(step_context_t* ctx) {
    grid_shape_t shape;
    if (grid_shape_resolve(ctx, &shape) != 0) {
        return -1;
    }
    uint32_t cols = shape.cols;
    uint32_t rows = shape.rows;
    size_t cells = (size_t)cols * rows;

    size_t jpeg_len = 0;
    const uint8_t* jpeg_bytes = (const uint8_t*)step_read(ctx, "ortho-rgb-jpeg", &jpeg_len);
    if (!jpeg_bytes || jpeg_len == 0) {
        return -1;
    }
    decoded_image_t img;
    if (jpeg_decode_rgba(jpeg_bytes, jpeg_len, &img) != 0) {
        return -1;
    }
    uint8_t* rgba = jpeg_resample_rgba(img.rgba, img.w, img.h, (int)cols, (int)rows,
                                       /*flip_y=*/true);
    decoded_image_free(&img);
    if (!rgba) {
        return -1;
    }

    uint8_t* buf = raster_alloc(cols, rows, ORGB__COUNT);
    if (!buf) {
        free(rgba);
        return -1;
    }
    float* r = raster_channel_ptr(buf, cols, rows, ORGB_R);
    float* g = raster_channel_ptr(buf, cols, rows, ORGB_G);
    float* b = raster_channel_ptr(buf, cols, rows, ORGB_B);
    float* lum = raster_channel_ptr(buf, cols, rows, ORGB_LUMINANCE);
    float* sat = raster_channel_ptr(buf, cols, rows, ORGB_SATURATION);
    float* grn = raster_channel_ptr(buf, cols, rows, ORGB_GREENNESS);
    float* lstd = raster_channel_ptr(buf, cols, rows, ORGB_LUM_STDDEV);

    for (size_t i = 0; i < cells; i++) {
        float R = (float)rgba[i * 4 + 0] / 255.0f;
        float G = (float)rgba[i * 4 + 1] / 255.0f;
        float B = (float)rgba[i * 4 + 2] / 255.0f;
        r[i] = R;
        g[i] = G;
        b[i] = B;
        lum[i] = 0.299f * R + 0.587f * G + 0.114f * B;
        float mx = R > G ? (R > B ? R : B) : (G > B ? G : B);
        float mn = R < G ? (R < B ? R : B) : (G < B ? G : B);
        sat[i] = mx - mn;
        float mx_rb = R > B ? R : B;
        grn[i] = G - mx_rb;
    }
    free(rgba);
    compute_local_stddev(lum, cols, rows, 2, lstd);

    return step_write_output_owned(ctx, "ortho-rgb-rasters", buf,
                                   raster_total_bytes(cols, rows, ORGB__COUNT));
}

static const step_read_t reads_arr[] = {
    {.name = "ortho-rgb-jpeg"},
    {.name = "canonical-location"},
};
static const step_write_t writes_arr[] = {
    {.name = "ortho-rgb-rasters", .kind = AK_RASTER_F32, .visibility = STEP_VIS_DEBUG},
};

const step_def_t build_ortho_rgb_rasters_def = {
    .name = "build-ortho-rgb-rasters",
    .version = 1,
    .reads = reads_arr,
    .n_reads = 2,
    .writes = writes_arr,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};

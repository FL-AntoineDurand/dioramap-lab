// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// build-ortho-irc-rasters
// Reads:  ortho-irc-jpeg, ortho-rgb-rasters, canonical-location
// Params: { "cell_size_m": float (default 0.5) — must match build-lidar-rasters }
// Writes: ortho-irc-rasters — 2-channel F32 raster: nir, ndvi
//
// IGN's IRC ortho encodes NIR in the JPEG's R channel. NDVI is computed
// from NIR + the regular ortho's R channel:
//   NDVI = (NIR - R) / (NIR + R + 1e-6)
// Same formula as bench-v2 viewer/backend/registration/rasters.js.

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

static int exec_fn(step_context_t* ctx) {
    grid_shape_t shape;
    if (grid_shape_resolve(ctx, &shape) != 0) {
        return -1;
    }
    uint32_t cols = shape.cols;
    uint32_t rows = shape.rows;
    size_t cells = (size_t)cols * rows;

    size_t jpeg_len = 0;
    const uint8_t* jpeg_bytes = (const uint8_t*)step_read(ctx, "ortho-irc-jpeg", &jpeg_len);
    if (!jpeg_bytes || jpeg_len == 0) {
        return -1;
    }

    size_t rgb_bytes = 0;
    const uint8_t* rgb_in = (const uint8_t*)step_read(ctx, "ortho-rgb-rasters", &rgb_bytes);
    if (!rgb_in) {
        return -1;
    }
    raster_view_t rgb_view;
    if (raster_view_open(rgb_in, rgb_bytes, &rgb_view) != 0) {
        return -1;
    }
    if (rgb_view.cols != cols || rgb_view.rows != rows ||
        rgb_view.n_channels < ORGB__COUNT) {
        return -1;
    }
    const float* rgb_R = rgb_view.channels[ORGB_R];

    decoded_image_t img;
    if (jpeg_decode_rgba(jpeg_bytes, jpeg_len, &img) != 0) {
        return -1;
    }
    uint8_t* irc_rgba = jpeg_resample_rgba(img.rgba, img.w, img.h, (int)cols, (int)rows,
                                           /*flip_y=*/true);
    decoded_image_free(&img);
    if (!irc_rgba) {
        return -1;
    }

    uint8_t* buf = raster_alloc(cols, rows, OIRC__COUNT);
    if (!buf) {
        free(irc_rgba);
        return -1;
    }
    float* nir = raster_channel_ptr(buf, cols, rows, OIRC_NIR);
    float* ndvi = raster_channel_ptr(buf, cols, rows, OIRC_NDVI);

    for (size_t i = 0; i < cells; i++) {
        float NIR = (float)irc_rgba[i * 4 + 0] / 255.0f;
        nir[i] = NIR;
        float R = rgb_R[i];
        float denom = NIR + R + 1e-6f;
        ndvi[i] = (NIR - R) / denom;
    }
    free(irc_rgba);

    return step_write_output_owned(ctx, "ortho-irc-rasters", buf,
                                   raster_total_bytes(cols, rows, OIRC__COUNT));
}

static const step_read_t reads_arr[] = {
    {.name = "ortho-irc-jpeg"},
    {.name = "ortho-rgb-rasters"},
    {.name = "canonical-location"},
};
static const step_write_t writes_arr[] = {
    {.name = "ortho-irc-rasters", .kind = AK_RASTER_F32, .visibility = STEP_VIS_DEBUG},
};

const step_def_t build_ortho_irc_rasters_def = {
    .name = "build-ortho-irc-rasters",
    .version = 1,
    .reads = reads_arr,
    .n_reads = 3,
    .writes = writes_arr,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};

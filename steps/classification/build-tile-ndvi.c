// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// build-tile-ndvi
// Reads:  ortho-rgb-jpeg, ortho-irc-jpeg, canonical-location
// Params: { "cell_size_m": float (default 1.0 — the "canopy cell" grid),
//           "ndvi_threshold": float (default 0.1),
//           "boost_threshold": float (default 0.075),
//           "vote_min": int (default 5) }
// Writes: tile-ndvi — 1-channel F32 raster at the classification grid.
//
// NDVI = (NIR - R) / (NIR + R + 1e-6) computed at the classification
// grid resolution. Then an "ortho-neighbourhood vote boost" fills gaps
// inside vegetation patches: for each cell whose raw NDVI is below
// ndvi_threshold, count its 8-neighbours above boost_threshold. If
// vote_min or more pass, replace the cell with the neighbours' average.
//
// Direct port of the tile-NDVI section of bench-v2 classify-vegetation.js.
// At 1m cells this is a distinct artifact from the 0.5m ortho-irc-rasters
// used by coregister-ortho-lidar (different step identity = independent
// cache entry in the v3 content-hash store).

#include <jansson.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "v3/artifact.h"
#include "v3/safe_alloc.h"
#include "v3/json_int.h"
#include "v3/step.h"
#include "v3/step_bbox_params.h"

#include "../ortho/jpeg_decode.h"
#include "../registration/raster_format.h"
#include "classification-steps.h"

typedef struct ndvi_params {
    double cell_size_m;
    double ndvi_threshold;
    double boost_threshold;
    int vote_min;
} ndvi_params_t;

static void parse_params(const char* s, ndvi_params_t* o) {
    o->cell_size_m = 1.0;
    o->ndvi_threshold = 0.1;
    o->boost_threshold = 0.075;
    o->vote_min = 5;
    if (!s) return;
    json_error_t je;
    json_t* p = json_loads(s, 0, &je);
    if (!p) return;
    json_t* j = json_object_get(p, "cell_size_m");
    if (json_is_number(j)) {
        double v = json_number_value(j);
        if (v > 1e-6 && v < 100.0) o->cell_size_m = v;
    }
    j = json_object_get(p, "ndvi_threshold");
    if (json_is_number(j)) o->ndvi_threshold = json_number_value(j);
    j = json_object_get(p, "boost_threshold");
    if (json_is_number(j)) o->boost_threshold = json_number_value(j);
    j = json_object_get(p, "vote_min");
    if (json_int_or_default(j, 1, 100, "vote_min", &o->vote_min) != 0) {
        json_decref(p); return;  // parse_params returns void here; the
                                  // stderr message already told the user.
    }
    json_decref(p);
}

// Decode a JPEG + resample to (cols, rows) with Y-flip. Returns malloc'd
// RGBA buffer on success, NULL on failure.
static uint8_t* decode_and_resample(step_context_t* ctx, const char* artifact_name,
                                    uint32_t cols, uint32_t rows) {
    size_t jpeg_len = 0;
    const uint8_t* jpeg_bytes = (const uint8_t*)step_read(ctx, artifact_name, &jpeg_len);
    if (!jpeg_bytes) return NULL;
    decoded_image_t img;
    if (jpeg_decode_rgba(jpeg_bytes, jpeg_len, &img) != 0) return NULL;
    uint8_t* out = jpeg_resample_rgba(img.rgba, img.w, img.h, (int)cols, (int)rows, true);
    decoded_image_free(&img);
    return out;
}

static int exec_fn(step_context_t* ctx) {
    ndvi_params_t params;
    parse_params(step_get_params(ctx), &params);

    double bbox[4];
    if (step_read_bbox_l93(ctx, bbox) != 0) return -1;
    double width_m = bbox[2] - bbox[0];
    double height_m = bbox[3] - bbox[1];
    if (!(width_m > 0) || !(height_m > 0)) return -1;
    int64_t cols_i = (int64_t)round(width_m / params.cell_size_m);
    int64_t rows_i = (int64_t)round(height_m / params.cell_size_m);
    if (cols_i < 2) cols_i = 2;
    if (rows_i < 2) rows_i = 2;
    if (cols_i > 65535 || rows_i > 65535) return -1;
    uint32_t cols = (uint32_t)cols_i;
    uint32_t rows = (uint32_t)rows_i;
    size_t cells = (size_t)cols * rows;

    // Decode both orthos at the tile grid. Y-flip to L93-south orientation
    // matches build-ortho-rgb-rasters / build-ortho-irc-rasters.
    uint8_t* rgb = decode_and_resample(ctx, "ortho-rgb-jpeg", cols, rows);
    if (!rgb) return -1;
    uint8_t* irc = decode_and_resample(ctx, "ortho-irc-jpeg", cols, rows);
    if (!irc) {
        free(rgb);
        return -1;
    }

    // Compute raw NDVI per cell. IGN's IRC encodes NIR in the image R
    // channel; the regular ortho's R comes from rgb[i*4+0].
    float* raw = safe_malloc_array(cells, sizeof(float));
    if (!raw) {
        free(rgb);
        free(irc);
        return -1;
    }
    for (size_t i = 0; i < cells; i++) {
        float R = (float)rgb[i * 4] / 255.0f;
        float NIR = (float)irc[i * 4] / 255.0f;
        float denom = NIR + R + 1e-6f;
        raw[i] = (NIR - R) / denom;
    }
    free(rgb);
    free(irc);

    // Vote-boost pass: fill gaps inside vegetation patches. Only cells
    // whose raw NDVI is below the main threshold are candidates for boost;
    // if vote_min-or-more of their 8-neighbours are above boost_threshold,
    // the cell takes the neighbours' average.
    uint8_t* buf = raster_alloc(cols, rows, 1);
    if (!buf) {
        free(raw);
        return -1;
    }
    float* out = raster_channel_ptr(buf, cols, rows, 0);
    memcpy(out, raw, cells * sizeof(float));

    for (uint32_t r = 0; r < rows; r++) {
        for (uint32_t c = 0; c < cols; c++) {
            size_t k = (size_t)r * cols + c;
            if (raw[k] > params.ndvi_threshold) continue;
            int votes = 0;
            double sum = 0;
            for (int dr = -1; dr <= 1; dr++) {
                int nr = (int)r + dr;
                if (nr < 0 || nr >= (int)rows) continue;
                for (int dc = -1; dc <= 1; dc++) {
                    if (dr == 0 && dc == 0) continue;
                    int nc = (int)c + dc;
                    if (nc < 0 || nc >= (int)cols) continue;
                    float nv = raw[(size_t)nr * cols + nc];
                    if (nv > params.boost_threshold) {
                        votes++;
                        sum += nv;
                    }
                }
            }
            if (votes >= params.vote_min) {
                out[k] = (float)(sum / votes);
            }
        }
    }
    free(raw);

    return step_write_output_owned(ctx, "tile-ndvi", buf,
                                   raster_total_bytes(cols, rows, 1));
}

static const step_read_t reads_arr[] = {
    {.name = "ortho-rgb-jpeg"},
    {.name = "ortho-irc-jpeg"},
    {.name = "canonical-location"},
};
static const step_write_t writes_arr[] = {
    {.name = "tile-ndvi", .kind = AK_RASTER_F32, .visibility = STEP_VIS_DEBUG},
};

const step_def_t build_tile_ndvi_def = {
    .name = "build-tile-ndvi",
    .version = 1,
    .reads = reads_arr,
    .n_reads = 3,
    .writes = writes_arr,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};

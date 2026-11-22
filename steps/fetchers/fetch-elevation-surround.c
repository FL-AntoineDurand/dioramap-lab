// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// fetch-elevation-surround
// Reads:  canonical-location — JSON with center_l93_{x,y}
// Params: {"grid_size": int, "radius_m": number, "layer": string}
//           defaults: grid_size=256, radius_m=600,
//                     layer="ELEVATION.ELEVATIONGRIDCOVERAGE.HIGHRES"
// Writes: elevation-surround-f32 — F32 grid wrapped in v3 raster_format
//         header (kind AK_RASTER_F32).
//
// Wider-radius BD ALTI elevation grid for context. Default grid_size=256
// (vs 512 for the core) — surround only needs to reconstruct coarse
// terrain shape, not drive DSM/NDSM math. See PLAN-VIEWER-PORT §4.2.
//
// URL mechanics match fetch-elevation except the bbox comes from
// `center_l93_{x,y} ± radius_m` instead of `bbox_l93`. The shared body
// (params, URL, fetch, header-wrap, write) is in wms-elevation-fetch.c
// (#72c Family G).

#include <jansson.h>
#include <stdlib.h>

#include "v3/artifact.h"
#include "v3/step.h"
#include "v3/store.h"

#include "fetchers.h"
#include "wms-elevation-fetch.h"

// Reads center_l93_{x,y} from canonical-location and computes a
// `bbox = center ± radius`. Returns 0 on success, -1 on parse failure.
// `radius_m` defaults to 600.0; the `radius_m` step param can override.
static int compute_surround_bbox(step_context_t* ctx, double bbox[4]) {
    size_t in_len = 0;
    const char* in_bytes = (const char*)step_read(ctx, "canonical-location", &in_len);
    if (!in_bytes || in_len == 0) return -1;
    json_error_t je;
    json_t* input = json_loadb(in_bytes, in_len, 0, &je);
    if (!input) return -1;
    json_t* j_cx = json_object_get(input, "center_l93_x");
    json_t* j_cy = json_object_get(input, "center_l93_y");
    if (!json_is_number(j_cx) || !json_is_number(j_cy)) {
        json_decref(input);
        return -1;
    }
    double cx = json_number_value(j_cx);
    double cy = json_number_value(j_cy);
    json_decref(input);

    double radius_m = 600.0;
    const char* params_str = step_get_params(ctx);
    if (params_str) {
        json_t* params = json_loads(params_str, 0, &je);
        if (params) {
            json_t* j_radius = json_object_get(params, "radius_m");
            if (json_is_number(j_radius)) {
                double v = json_number_value(j_radius);
                if (v > 0 && v < 100000) radius_m = v;
            }
            json_decref(params);
        }
    }
    bbox[0] = cx - radius_m; bbox[1] = cy - radius_m;
    bbox[2] = cx + radius_m; bbox[3] = cy + radius_m;
    return 0;
}

static int exec_fn(step_context_t* ctx) {
    double bbox[4];
    if (compute_surround_bbox(ctx, bbox) != 0) return -1;
    return wms_elevation_fetch_exec(ctx, bbox,
        /*default_grid_size=*/256,
        "ELEVATION.ELEVATIONGRIDCOVERAGE.HIGHRES",
        "elevation-surround-f32");
}

static const step_read_t reads_arr[] = {{.name = "canonical-location"}};
static const step_write_t writes_arr[] = {
    {.name = "elevation-surround-f32",
     .kind = AK_RASTER_F32,
     .visibility = STEP_VIS_PUBLIC},
};

const step_def_t fetch_elevation_surround_def = {
    .name = "fetch-elevation-surround",
    // v2: wrap raw BIL F32 bytes in the standard v3 raster_format header
    // (u32 cols/rows/n_channels=1) so AK_RASTER_F32 consumers decode
    // without special-casing. v1 emitted bare BIL bytes which the
    // browser's decodeRasterF32 mis-parsed (cols/rows = garbage).
    .version = 2,
    .reads = reads_arr,
    .n_reads = 1,
    .writes = writes_arr,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};

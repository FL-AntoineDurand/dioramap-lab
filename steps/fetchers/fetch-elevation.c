// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// fetch-elevation
// Reads:  canonical-location — JSON with bbox_l93
// Params: {"grid_size": int, "layer": string}
//           defaults: grid_size=512, layer="ELEVATION.ELEVATIONGRIDCOVERAGE.HIGHRES"
// Writes: elevation-f32 — F32 grid wrapped in v3 raster_format header
//         (kind AK_RASTER_F32).
//
// URL is fully determined by canonical-location + params, so plain
// `fetch_get` + step-level intent-map cache (PLAN-FETCHERS F4b).
//
// Body is in wms-elevation-fetch.c (#72c Family G — shared with
// fetch-elevation-surround).

#include "v3/artifact.h"
#include "v3/step_bbox_params.h"
#include "v3/store.h"

#include "fetchers.h"
#include "wms-elevation-fetch.h"

static int exec_fn(step_context_t* ctx) {
    double bbox[4];
    if (step_read_bbox_l93(ctx, bbox) != 0)
        return step_fail(ctx, V3_ERR_INTERNAL, "canonical-location missing or has no bbox_l93");
    return wms_elevation_fetch_exec(ctx, bbox,
        /*default_grid_size=*/512,
        "ELEVATION.ELEVATIONGRIDCOVERAGE.HIGHRES",
        "elevation-f32");
}

static const step_read_t reads_arr[] = {{.name = "canonical-location"}};
static const step_write_t writes_arr[] = {
    {.name = "elevation-f32",
     .kind = AK_RASTER_F32,
     .visibility = STEP_VIS_PUBLIC},
};

const step_def_t fetch_elevation_def = {
    .name = "fetch-elevation",
    // v2: wrap raw BIL F32 bytes in the standard v3 raster_format header
    // (u32 cols/rows/n_channels=1) — see fetch-elevation-surround for the
    // rationale.
    .version = 2,
    .reads = reads_arr,
    .n_reads = 1,
    .writes = writes_arr,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};

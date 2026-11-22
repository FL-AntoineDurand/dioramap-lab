// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// compute-scene-origin
// Reads:  canonical-location (JSON)  + elevation-f32 (raw Float32 grid)
// Writes: scene-origin (JSON: {x, y, z} in L93 metres; z is midZ from elevation)
//
// The L93 center comes directly from canonical-location (no recomputation);
// the Z component is (min+max)/2 over the elevation Float32 grid, with NODATA
// sentinel (-99999) ignored. The elevation artifact is the raw BIL bytes —
// just a flat Float32 row-major grid — so we scan it directly as floats.

#include <jansson.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v3/artifact.h"
#include "v3/store.h"

#include "coord-steps.h"

static int scan_bil_minmax(const float* data, size_t n, float* out_min, float* out_max) {
    float mn = INFINITY, mx = -INFINITY;
    for (size_t i = 0; i < n; i++) {
        float v = data[i];
        if (v == -99999.0f)
            continue;
        if (v < mn)
            mn = v;
        if (v > mx)
            mx = v;
    }
    if (!isfinite(mn))
        return -1;
    *out_min = mn;
    *out_max = mx;
    return 0;
}

static int exec_fn(step_context_t* ctx) {
    size_t loc_len = 0;
    const char* loc_bytes = (const char*)step_read(ctx, "canonical-location", &loc_len);
    if (!loc_bytes)
        return -1;

    size_t elev_len = 0;
    const uint8_t* elev_bytes = (const uint8_t*)step_read(ctx, "elevation-f32", &elev_len);
    // fetch-elevation v2 wraps the BIL bytes in the standard 12-byte
    // raster header (u32 cols, u32 rows, u32 n_channels). Skip past it
    // before scanning the F32 grid.
    if (!elev_bytes || elev_len < 12 + sizeof(float))
        return -1;
    elev_bytes += 12;
    elev_len -= 12;

    json_error_t je;
    json_t* loc = json_loadb(loc_bytes, loc_len, 0, &je);
    if (!loc)
        return -1;
    double cx = json_number_value(json_object_get(loc, "center_l93_x"));
    double cy = json_number_value(json_object_get(loc, "center_l93_y"));
    json_decref(loc);

    size_t n_floats = elev_len / sizeof(float);
    float minZ, maxZ;
    if (scan_bil_minmax((const float*)elev_bytes, n_floats, &minZ, &maxZ) != 0)
        return -1;

    double midZ = (minZ + maxZ) / 2.0;
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
                     "{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f,\"min_z\":%.3f,\"max_z\":%.3f}",
                     cx, cy, midZ, (double)minZ, (double)maxZ);
    if (n < 0 || (size_t)n >= sizeof(buf))
        return -1;
    return step_write_output(ctx, "scene-origin", buf, (size_t)n);
}

static const step_read_t reads_arr[] = {
    {.name = "canonical-location"},
    {.name = "elevation-f32"},
};
static const step_write_t writes_arr[] = {
    {.name = "scene-origin",
     .kind = AK_OPAQUE_JSON,
     .visibility = STEP_VIS_PUBLIC},
};

const step_def_t compute_scene_origin_def = {
    .name = "compute-scene-origin",
    // v2: skip the 12-byte raster header that fetch-elevation v2 now
    // emits before the BIL F32 grid.
    .version = 2,
    .reads = reads_arr,
    .n_reads = 2,
    .writes = writes_arr,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};

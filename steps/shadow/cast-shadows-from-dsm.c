// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// cast-shadows-from-dsm
// Reads:  lidar-rasters (uses MAX_Z channel)
// Params: { "sun_dx": float, "sun_dy": float, "sun_dz": float
//                — sun direction unit vector in L93 axes (x=east, y=north,
//                  z=up). Should be unit-length but the kernel renormalises
//                  defensively. Required.,
//           "cell_size_m": float (default 0.5),
//           "epsilon_m": float (default 0.1) — lift the ray off the
//                                              starting surface to suppress
//                                              self-shadow noise }
// Writes: synth-shadow-mask — single-channel F32 raster, 1.0 = shadow,
//         0.0 = lit. NaN cells in maxZ are emitted as lit (0.0).
//
// Hard-edge ray-march shadow caster. For every cell (c, r) of the maxZ
// raster, march a ray from the cell's surface toward the sun, stepping
// one cellSize of horizontal motion per iteration. If any later cell's
// maxZ exceeds the ray altitude at that point, the cell is in shadow.
//
// Direct port of bench-v2 viewer/backend/preprocess/shadow-cast.js
// castShadowsFromDSM. This is the algorithmic hot loop the doc flagged as
// the first GPU-canary candidate (PLAN-VIEWER-PORT §11 B8). The inner
// `cast_shadow_kernel` is kept as a single self-contained function with
// no allocations or runtime branches so a future .cu wrapper can dispatch
// it 1-to-1 onto a CUDA kernel without restructuring.

#include <jansson.h>
#include <stdint.h>
#include <stdlib.h>

#include "v3/artifact.h"
#include "v3/step.h"

#include "../registration/raster_format.h"
#include "../registration/registration-steps.h"
#include "shadow-cast-kernel.h"
#include "shadow-steps.h"

typedef struct cast_params {
    double sun_dx;
    double sun_dy;
    double sun_dz;
    double cell_size_m;
    double epsilon_m;
} cast_params_t;

// Returns 0 on success, -1 if any sun_d* param is missing (those are required).
static int parse_params(const char* s, cast_params_t* o) {
    o->sun_dx = 0;
    o->sun_dy = 0;
    o->sun_dz = 0;
    o->cell_size_m = 0.5;
    o->epsilon_m = 0.1;
    if (!s) return -1;
    json_error_t je;
    json_t* p = json_loads(s, 0, &je);
    if (!p) return -1;

    int seen_dx = 0, seen_dy = 0, seen_dz = 0;
    json_t* j;
    j = json_object_get(p, "sun_dx");
    if (json_is_number(j)) { o->sun_dx = json_number_value(j); seen_dx = 1; }
    j = json_object_get(p, "sun_dy");
    if (json_is_number(j)) { o->sun_dy = json_number_value(j); seen_dy = 1; }
    j = json_object_get(p, "sun_dz");
    if (json_is_number(j)) { o->sun_dz = json_number_value(j); seen_dz = 1; }
    j = json_object_get(p, "cell_size_m");
    if (json_is_number(j)) {
        double v = json_number_value(j);
        if (v > 1e-6 && v < 100.0) o->cell_size_m = v;
    }
    j = json_object_get(p, "epsilon_m");
    if (json_is_number(j)) {
        double v = json_number_value(j);
        if (v >= 0 && v < 100.0) o->epsilon_m = v;
    }
    json_decref(p);
    return (seen_dx && seen_dy && seen_dz) ? 0 : -1;
}

// The inner ray-march kernel lives in shadow-cast-kernel.h so
// find-best-sun-time can reuse it without C linkage indirection. See that
// header for the GPU-canary discussion.

static int exec_fn(step_context_t* ctx) {
    cast_params_t params;
    if (parse_params(step_get_params(ctx), &params) != 0) return -1;

    size_t lr_bytes = 0;
    const uint8_t* lr_in = (const uint8_t*)step_read(ctx, "lidar-rasters", &lr_bytes);
    if (!lr_in) return -1;
    raster_view_t lr;
    if (raster_view_open(lr_in, lr_bytes, &lr) != 0 || lr.n_channels < LIDAR_RC__COUNT) {
        return -1;
    }
    uint32_t cols = lr.cols;
    uint32_t rows = lr.rows;
    size_t cells = (size_t)cols * rows;
    const float* max_z = lr.channels[LIDAR_RC_MAX_Z];

    uint8_t* buf = raster_alloc(cols, rows, 1);
    if (!buf) return -1;
    float* out = raster_channel_ptr(buf, cols, rows, 0);
    (void)cells;

    cast_shadow_grid(max_z, cols, rows,
                     params.sun_dx, params.sun_dy, params.sun_dz,
                     params.cell_size_m, params.epsilon_m, out);

    return step_write_output_owned(ctx, "synth-shadow-mask", buf,
                                   raster_total_bytes(cols, rows, 1));
}

static const step_read_t reads_arr[] = {
    {.name = "lidar-rasters"},
};
static const step_write_t writes_arr[] = {
    {.name = "synth-shadow-mask", .kind = AK_RASTER_F32, .visibility = STEP_VIS_DEBUG},
};

const step_def_t cast_shadows_from_dsm_def = {
    .name = "cast-shadows-from-dsm",
    .version = 1,
    .reads = reads_arr,
    .n_reads = 1,
    .writes = writes_arr,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};

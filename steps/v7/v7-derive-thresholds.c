// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-derive-thresholds — per-building threshold artifact.
//
// Reads:
//   per-building-records  (per-bldg — n_pts per building)
//   building-local-frames (per-bldg — bbox-min/max for density formula)
//
// Writes:
//   bldg-thresholds       (per-bldg ARRAY_INLINE, stride=48 — see
//                          v7-derive-thresholds.h for the record schema)
//
// Density formula matches v2 tile-signals.js#L242-L248 applied per-bldg:
//   area    = (max_h0 - min_h0) · (max_h1 - min_h1)  // h0,h1 = non-up axes
//   density = n_pts / max(1, area)
//   spacing = 1 / sqrt(density)
//
// Other fields are constants today (z_noise=0.02, distThreshold values
// 0.09 / 0.12) — see v7-derive-thresholds.h for justification. They live
// in this single artifact instead of being baked into each consumer.
//
// Consumers (replacing their sentinel-auto-compute / hardcoded defaults):
//   v7-grow-refit-merge   → density, dist_threshold_grm
//   v7-density-voids-2    → density, dist_threshold_seed
//   v7-boundary-topology  → spacing
//   v7-boundary-corners   → dist_threshold_grm (coplanar)
//   v7-boundary-signals   → spacing (TODO: not yet wired)
//   v7-normal-cascade-seed → dist_threshold_seed (TODO: not yet wired)
//   ...remaining boundary chain steps TBD per audit.

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jansson.h>

#include "v3/artifact.h"
#include "v3/json_int.h"
#include "v3/step.h"
#include "v7-derive-thresholds.h"
#include "v7-steps.h"

// building-local-frames record layout (matches build-building-kd-trees +
// extract-buildings emitter): 9 × F64 = 72 bytes/record, fixed-stride.
//   [0..23]  centroid_xyz (L93)
//   [24..47] bbox_min_xyz (L93)
//   [48..71] bbox_max_xyz (L93)
#define BLF_RECORD_BYTES 72

// per-building-records header: u32 schema, u32 n_verts, u32 n_pts, ...
#define PBR_HEADER_BYTES 12

typedef struct params {
    int up_axis;                       // 1 = y-up (bench), 2 = z-up (LAZ)
    double z_noise;                    // override; default 0.02
    double dist_threshold_seed;        // override; default 0.09
    double dist_threshold_grm;         // override; default 0.12
} params_t;

static void params_default(params_t* p) {
    p->up_axis = 2;
    p->z_noise = 0.02;
    p->dist_threshold_seed = 0.09;
    p->dist_threshold_grm = 0.12;
}

static int parse_params(const char* params_str, params_t* p) {
    params_default(p);
    if (!params_str) return 0;
    json_error_t je;
    json_t* obj = json_loads(params_str, 0, &je);
    if (!obj) return -1;
    json_t* j;
    if ((j = json_object_get(obj, "upAxis")) && json_is_integer(j))
        p->up_axis = (int)json_integer_value(j);
    if ((j = json_object_get(obj, "zNoise")) && (json_is_real(j) || json_is_integer(j)))
        p->z_noise = json_number_value(j);
    if ((j = json_object_get(obj, "distThresholdSeed")) && (json_is_real(j) || json_is_integer(j)))
        p->dist_threshold_seed = json_number_value(j);
    if ((j = json_object_get(obj, "distThresholdGrm")) && (json_is_real(j) || json_is_integer(j)))
        p->dist_threshold_grm = json_number_value(j);
    json_decref(obj);
    return 0;
}

static int v7_derive_thresholds_exec(step_context_t* ctx) {
    params_t params;
    if (parse_params(step_get_params(ctx), &params) != 0) return -1;

    size_t n_buildings = step_array_len(ctx, "per-building-records");
    if (step_array_len(ctx, "building-local-frames") != n_buildings) {
        fprintf(stderr, "[v7-derive-thresholds] per-bldg-records / local-frames count mismatch\n");
        return -1;
    }

    if (n_buildings == 0) {
        // No buildings → emit empty ARRAY_INLINE so consumers see the
        // step ran and array_len() returns 0 cleanly.
        return step_write_array_inline(ctx, "bldg-thresholds",
                                        NULL, 0, 0, V7_BLDG_THRESHOLDS_BYTES);
    }

    const size_t total_bytes = n_buildings * V7_BLDG_THRESHOLDS_BYTES;
    uint8_t* buf = (uint8_t*)calloc(1, total_bytes);
    if (!buf) return -1;

    // Non-up axes for the horizontal area formula.
    const int up = (params.up_axis == 1) ? 1 : 2;
    const int h0 = (up == 0) ? 1 : 0;
    const int h1 = (up == 2) ? 1 : 2;

    int rc = 0;
    for (size_t bi = 0; bi < n_buildings; bi++) {
        v7_bldg_thresholds_t rec;
        rec.z_noise              = params.z_noise;
        rec.dist_threshold_seed  = params.dist_threshold_seed;
        rec.dist_threshold_grm   = params.dist_threshold_grm;
        rec._reserved            = 0.0;

        // Read n_pts from per-bldg record header.
        const uint8_t* pbr = NULL; size_t pbr_len = 0;
        if (step_array_read_bytes(ctx, "per-building-records", bi,
                                   &pbr, &pbr_len) != 0
            || !pbr || pbr_len < PBR_HEADER_BYTES) {
            // Mark this bldg as zero-density (skip-able by consumers).
            rec.density = 0.0;
            rec.spacing = 0.0;
            memcpy(buf + bi * V7_BLDG_THRESHOLDS_BYTES, &rec, sizeof(rec));
            continue;
        }
        uint32_t n_pts;
        memcpy(&n_pts, pbr + 8, 4);

        // Read horiz-bbox from building-local-frames record.
        const uint8_t* blf = NULL; size_t blf_len = 0;
        if (step_array_read_bytes(ctx, "building-local-frames", bi,
                                   &blf, &blf_len) != 0
            || !blf || blf_len < BLF_RECORD_BYTES || n_pts < 3) {
            rec.density = 0.0;
            rec.spacing = 0.0;
            memcpy(buf + bi * V7_BLDG_THRESHOLDS_BYTES, &rec, sizeof(rec));
            continue;
        }
        double bbox_min[3], bbox_max[3];
        memcpy(bbox_min, blf + 24, 3 * sizeof(double));
        memcpy(bbox_max, blf + 48, 3 * sizeof(double));

        double area = (bbox_max[h0] - bbox_min[h0]) * (bbox_max[h1] - bbox_min[h1]);
        if (area < 1.0) area = 1.0;
        rec.density = (double)n_pts / area;
        rec.spacing = 1.0 / sqrt(rec.density);

        if (getenv("DERIV_LOG")) {
            fprintf(stderr, "[DERIV] step=v7-derive-thresholds scope=bldg%zu n_pts=%u density=%.6f spacing=%.6f z_noise=%.6f distTh_seed=%.6f distTh_grm=%.6f\n",
                    bi, n_pts, rec.density, rec.spacing,
                    rec.z_noise, rec.dist_threshold_seed, rec.dist_threshold_grm);
        }

        memcpy(buf + bi * V7_BLDG_THRESHOLDS_BYTES, &rec, sizeof(rec));
    }

    if (rc == 0) {
        rc = step_write_array_inline(ctx, "bldg-thresholds",
                                      buf, total_bytes,
                                      n_buildings, V7_BLDG_THRESHOLDS_BYTES);
    }
    free(buf);
    return rc;
}

static const step_read_t v7_derive_thresholds_reads[] = {
    {.name = "per-building-records",  .array = true},
    {.name = "building-local-frames", .array = true},
};

static const step_write_t v7_derive_thresholds_writes[] = {
    {.name = "bldg-thresholds",
     .kind = AK_ARRAY_INLINE,
     .visibility = STEP_VIS_PUBLIC,
     .array = true},
};

const step_def_t v7_derive_thresholds_def = {
    .name = "v7-derive-thresholds",
    .version = 1,
    .reads = v7_derive_thresholds_reads,
    .n_reads = sizeof(v7_derive_thresholds_reads) / sizeof(v7_derive_thresholds_reads[0]),
    .writes = v7_derive_thresholds_writes,
    .n_writes = sizeof(v7_derive_thresholds_writes) / sizeof(v7_derive_thresholds_writes[0]),
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = v7_derive_thresholds_exec,
};

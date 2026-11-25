// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// canonicalize-location
// Reads:  pipeline-input — JSON {"lat": float, "lon": float, "radius_m": float (optional)}
// Params: {"cell_size_m": int (default 20), "radius_buckets": [int,...] (default [50,75,100,150,200])}
// Writes: canonical-location — JSON metadata with quantized L93 center + bbox.
//
// Funnel step: two different raw inputs that quantize to the same cell
// produce byte-identical output bytes → same content hash → downstream
// steps' identities collide → automatic cache convergence (HASH-MODEL.md
// §7.3). The runner needed no special handling for this — it falls out of
// content-hash-everywhere.
//
// Quantization:
//   center_l93_x = floor(l93_x / cell_size) * cell_size + cell_size/2
//   radius = smallest bucket >= raw radius (clamp to max bucket)
//   bbox_l93 = center ± radius

#include <jansson.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v3/artifact.h"
#include "v3/coords.h"
#include "v3/json_int.h"

#include "coord-steps.h"

static int cmp_int(const void* a, const void* b) {
    int ia = *(const int*)a, ib = *(const int*)b;
    return (ia > ib) - (ia < ib);
}

static int exec_fn(step_context_t* ctx) {
    size_t in_len = 0;
    const char* in_bytes = (const char*)step_read(ctx, PIPELINE_INPUT_ARTIFACT, &in_len);
    if (!in_bytes || in_len == 0)
        return step_fail(ctx, V3_ERR_INVALID_INPUT, "RUN input is empty: expected {\"lat\":…,\"lon\":…}");
    json_error_t je;
    json_t* input = json_loadb(in_bytes, in_len, 0, &je);
    if (!input)
        return step_fail(ctx, V3_ERR_INVALID_INPUT, "RUN input is not valid JSON (line %d: %s)",
                         je.line, je.text);
    json_t* j_lat = json_object_get(input, "lat");
    json_t* j_lon = json_object_get(input, "lon");
    if (!json_is_number(j_lat) || !json_is_number(j_lon)) {
        json_decref(input);
        return step_fail(ctx, V3_ERR_INVALID_INPUT,
                         "RUN input needs numeric \"lat\" and \"lon\" (WGS84 degrees)");
    }
    double lat = json_number_value(j_lat);
    double lon = json_number_value(j_lon);
    if (!isfinite(lat) || !isfinite(lon) || lat < -90 || lat > 90 || lon < -180 || lon > 180) {
        json_decref(input);
        return step_fail(ctx, V3_ERR_INVALID_INPUT,
                         "lat/lon out of range: lat=%g lon=%g (expected WGS84 degrees)", lat, lon);
    }
    double raw_radius = 75.0;
    json_t* j_rad = json_object_get(input, "radius_m");
    if (j_rad)
        raw_radius = json_number_value(j_rad);
    json_decref(input);

    // --- params ---
    int cell_size = 20;
    int buckets_default[] = {50, 75, 100, 150, 200};
    int* buckets = buckets_default;
    int n_buckets = (int)(sizeof(buckets_default) / sizeof(buckets_default[0]));
    int* alloced_buckets = NULL;

    const char* params_str = step_get_params(ctx);
    if (params_str) {
        json_t* params = json_loads(params_str, 0, &je);
        if (params) {
            json_t* j_cell = json_object_get(params, "cell_size_m");
            if (json_int_or_default(j_cell, 1, 16384, "cell_size_m", &cell_size) != 0) {
                json_decref(params);
                return step_fail(ctx, V3_ERR_INVALID_INPUT,
                                 "param cell_size_m must be an integer in [1, 16384]");
            }
            json_t* j_buckets = json_object_get(params, "radius_buckets");
            if (json_is_array(j_buckets) && json_array_size(j_buckets) > 0) {
                size_t nb = json_array_size(j_buckets);
                alloced_buckets = calloc(nb, sizeof(int));
                if (alloced_buckets) {
                    for (size_t i = 0; i < nb; i++) {
                        json_t* je_b = json_array_get(j_buckets, i);
                        if (json_int_or_default(je_b, 0, 65535, "radius_buckets[i]",
                                                 &alloced_buckets[i]) != 0) {
                            free(alloced_buckets);
                            json_decref(params);
                            return step_fail(ctx, V3_ERR_INVALID_INPUT,
                                             "param radius_buckets[%zu] must be an integer in "
                                             "[0, 65535]", i);
                        }
                    }
                    qsort(alloced_buckets, nb, sizeof(int), cmp_int);
                    buckets = alloced_buckets;
                    n_buckets = (int)nb;
                }
            }
            json_decref(params);
        }
    }

    // --- WGS84 → L93 ---
    double cx, cy;
    if (wgs84_to_l93(lon, lat, &cx, &cy) != 0) {
        free(alloced_buckets);
        return step_fail(ctx, V3_ERR_INVALID_INPUT,
                         "cannot project lat=%.6f lon=%.6f to Lambert-93 (EPSG:2154); the "
                         "engine covers metropolitan France", lat, lon);
    }

    // Quantize center to a `cell_size`-metre grid; anchor to centre-of-cell so
    // coords exactly on a grid line don't bounce between cells on rounding.
    double q_cx = floor(cx / cell_size) * cell_size + cell_size / 2.0;
    double q_cy = floor(cy / cell_size) * cell_size + cell_size / 2.0;

    // Bucket radius: smallest bucket >= raw, clamped to max bucket.
    int chosen_radius = buckets[n_buckets - 1];
    for (int i = 0; i < n_buckets; i++) {
        if (buckets[i] >= raw_radius) {
            chosen_radius = buckets[i];
            break;
        }
    }
    free(alloced_buckets);

    // Inverse: quantized L93 back to WGS84 for human-readable output.
    double q_lon, q_lat;
    if (l93_to_wgs84(q_cx, q_cy, &q_lon, &q_lat) != 0)
        return step_fail(ctx, V3_ERR_INTERNAL, "Lambert-93 → WGS84 inverse projection failed "
                         "for x=%.1f y=%.1f (PROJ setup?)", q_cx, q_cy);

    char buf[1024];
    int n = snprintf(buf, sizeof(buf),
                     "{\"lat\":%.7f,\"lon\":%.7f,\"radius_m\":%d,"
                     "\"center_l93_x\":%.3f,\"center_l93_y\":%.3f,"
                     "\"bbox_l93\":[%.3f,%.3f,%.3f,%.3f],"
                     "\"cell_size_m\":%d}",
                     q_lat, q_lon, chosen_radius, q_cx, q_cy,
                     q_cx - chosen_radius, q_cy - chosen_radius,
                     q_cx + chosen_radius, q_cy + chosen_radius, cell_size);
    if (n < 0 || (size_t)n >= sizeof(buf))
        return -1;
    return step_write_output(ctx, "canonical-location", buf, (size_t)n);
}

static const step_read_t reads_arr[] = {{.name = PIPELINE_INPUT_ARTIFACT}};
static const step_write_t writes_arr[] = {
    // canonical-location: small OPAQUE_JSON {bbox_l93, lat, lon, ...}.
    // Originally tagged STEP_VIS_DEBUG on the assumption only downstream
    // C steps would read it. Reality drifted — multiple SDK composers
    // (canopy.viz, point-cloud-discs.viz, elevation-surround.viz,
    // deshadowed-ortho.viz, terrain.viz, ortho-surround-jpeg.viz) all
    // call decoded['canonical-location'] to position their meshes in
    // scene space. With D6.5's tier-gated SUBSCRIBE the public-tier SDK
    // got FORBIDDEN_ARTIFACT every time. Flipped to STEP_VIS_PUBLIC —
    // a coordinate normalisation isn't an algorithm trade secret.
    {.name = "canonical-location",
     .kind = AK_OPAQUE_JSON,
     .visibility = STEP_VIS_PUBLIC},
};

const step_def_t canonicalize_location_def = {
    .name = "canonicalize-location",
    .version = 2, // 1 → 2: write visibility flipped DEBUG → PUBLIC

    .reads = reads_arr,
    .n_reads = 1,
    .writes = writes_arr,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// rasterize-cadastre-edges
// Reads:  cadastre-buildings-geojson (raw GeoJSON bytes), canonical-location
// Params: { cell_size_m: float (default 0.5, must match lidar grid),
//           thickness: int (default 2) — dilation passes,
//           l93_offset_x: float (default 0), l93_offset_y: float (default 0)
//           — add to every vertex before rasterizing. Use these to chain the
//           ortho coreg-offset into the cadastre rasterisation so the
//           subsequent cadastre-fit step measures the RESIDUAL cadastre→
//           lidar offset rather than the combined ortho+cadastre offset.
//           Caller is responsible for threading coreg-offset values through. }
// Writes: cadastre-edges-raster — single-channel F32 raster, 0.0 / 1.0.
//
// Direct port of viewer/backend/features/cadastre-registration.js
// rasterizeCadastreBuildingEdges. Each polygon edge is subsampled at half
// the cell size so consecutive samples land in adjacent cells, then optional
// dilation thickens the line for cross-correlation.

#include <jansson.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "v3/artifact.h"
#include "v3/json_int.h"
#include "v3/step.h"
#include "v3/step_bbox_params.h"

#include "raster_format.h"
#include "registration-steps.h"

typedef struct rasterize_params {
    double cell_size_m;
    int thickness;
    double l93_offset_x;
    double l93_offset_y;
} rasterize_params_t;

static void parse_params(const char* params_str, rasterize_params_t* out) {
    out->cell_size_m = 0.5;
    out->thickness = 2;
    out->l93_offset_x = 0.0;
    out->l93_offset_y = 0.0;
    if (!params_str) return;
    json_error_t je;
    json_t* p = json_loads(params_str, 0, &je);
    if (!p) return;
    json_t* j = json_object_get(p, "cell_size_m");
    if (json_is_number(j)) {
        double v = json_number_value(j);
        if (v > 1e-6 && v < 100.0) out->cell_size_m = v;
    }
    j = json_object_get(p, "thickness");
    if (json_int_or_default(j, 1, 16, "thickness", &out->thickness) != 0) {
        json_decref(p); return;
    }
    j = json_object_get(p, "l93_offset_x");
    if (json_is_number(j)) out->l93_offset_x = json_number_value(j);
    j = json_object_get(p, "l93_offset_y");
    if (json_is_number(j)) out->l93_offset_y = json_number_value(j);
    json_decref(p);
}

// Rasterize one ring of (x, y) vertex pairs into the edge mask.
// `ring` is an array-of-arrays (each inner array is [lon/x, lat/y, ...]).
static void rasterize_ring(json_t* ring, const double bbox[4], double cell_size,
                           uint32_t cols, uint32_t rows, double off_x, double off_y,
                           float* mask) {
    size_t nv = json_array_size(ring);
    if (nv < 2) return;
    double sample_step = cell_size * 0.5;
    double xmin = bbox[0];
    double ymin = bbox[1];

    for (size_t i = 0; i < nv; i++) {
        json_t* v_a = json_array_get(ring, i);
        json_t* v_c = json_array_get(ring, (i + 1) % nv);
        if (!json_is_array(v_a) || !json_is_array(v_c) ||
            json_array_size(v_a) < 2 || json_array_size(v_c) < 2) {
            continue;
        }
        double ax = json_number_value(json_array_get(v_a, 0));
        double ay = json_number_value(json_array_get(v_a, 1));
        double cx = json_number_value(json_array_get(v_c, 0));
        double cy = json_number_value(json_array_get(v_c, 1));
        double dx = cx - ax;
        double dy = cy - ay;
        double seg_len = sqrt(dx * dx + dy * dy);
        int n_samples = (int)ceil(seg_len / sample_step);
        if (n_samples < 1) n_samples = 1;
        for (int k = 0; k <= n_samples; k++) {
            double t = (double)k / (double)n_samples;
            double x = ax + dx * t + off_x;
            double y = ay + dy * t + off_y;
            int col = (int)floor((x - xmin) / cell_size);
            int row = (int)floor((y - ymin) / cell_size);
            if (col < 0 || col >= (int)cols || row < 0 || row >= (int)rows) continue;
            mask[(size_t)row * cols + col] = 1.0f;
        }
    }
}

// Rasterize every building's outer ring. GeoJSON features may be Polygon
// or MultiPolygon; in both cases we take only the exterior ring (first
// coordinates array).
static int rasterize_features(const char* geojson_bytes, size_t geojson_len,
                              const double bbox[4], double cell_size, uint32_t cols,
                              uint32_t rows, double off_x, double off_y, float* mask) {
    json_error_t je;
    json_t* root = json_loadb(geojson_bytes, geojson_len, 0, &je);
    if (!root) return -1;
    json_t* features = json_object_get(root, "features");
    if (!json_is_array(features)) {
        json_decref(root);
        return -1;
    }
    size_t nf = json_array_size(features);
    for (size_t i = 0; i < nf; i++) {
        json_t* f = json_array_get(features, i);
        json_t* geom = json_object_get(f, "geometry");
        if (!json_is_object(geom)) continue;
        const char* type = json_string_value(json_object_get(geom, "type"));
        json_t* coords = json_object_get(geom, "coordinates");
        if (!type || !json_is_array(coords)) continue;
        if (strcmp(type, "Polygon") == 0) {
            // coords: [ring, hole, hole, ...] — take only the exterior ring.
            json_t* ring = json_array_get(coords, 0);
            if (json_is_array(ring)) {
                rasterize_ring(ring, bbox, cell_size, cols, rows, off_x, off_y, mask);
            }
        } else if (strcmp(type, "MultiPolygon") == 0) {
            // coords: [polygon, polygon, ...], each polygon: [ring, hole, ...].
            size_t np = json_array_size(coords);
            for (size_t pi = 0; pi < np; pi++) {
                json_t* poly = json_array_get(coords, pi);
                if (!json_is_array(poly)) continue;
                json_t* ring = json_array_get(poly, 0);
                if (json_is_array(ring)) {
                    rasterize_ring(ring, bbox, cell_size, cols, rows, off_x, off_y, mask);
                }
            }
        }
    }
    json_decref(root);
    return 0;
}

// 4-neighbour dilation pass. Reads from `cur` and writes to `next`, both
// length cells.
static void dilate_pass(const float* cur, uint32_t cols, uint32_t rows, float* next) {
    size_t cells = (size_t)cols * rows;
    memset(next, 0, cells * sizeof(float));
    for (uint32_t r = 0; r < rows; r++) {
        for (uint32_t c = 0; c < cols; c++) {
            size_t k = (size_t)r * cols + c;
            if (cur[k] > 0 ||
                (c > 0 && cur[k - 1] > 0) ||
                (c + 1 < cols && cur[k + 1] > 0) ||
                (r > 0 && cur[k - cols] > 0) ||
                (r + 1 < rows && cur[k + cols] > 0)) {
                next[k] = 1.0f;
            }
        }
    }
}

static int exec_fn(step_context_t* ctx) {
    rasterize_params_t params;
    parse_params(step_get_params(ctx), &params);

    double bbox[4];
    if (step_read_bbox_l93(ctx, bbox) != 0) return -1;

    double off_x = params.l93_offset_x;
    double off_y = params.l93_offset_y;

    size_t geojson_len = 0;
    const char* geojson_bytes =
        (const char*)step_read(ctx, "cadastre-buildings-geojson", &geojson_len);
    if (!geojson_bytes) return -1;

    double width_m = bbox[2] - bbox[0];
    double height_m = bbox[3] - bbox[1];
    if (!(width_m > 0) || !(height_m > 0)) return -1;
    int64_t cols_i = (int64_t)floor(width_m / params.cell_size_m);
    int64_t rows_i = (int64_t)floor(height_m / params.cell_size_m);
    if (cols_i < 2) cols_i = 2;
    if (rows_i < 2) rows_i = 2;
    if (cols_i > 65535 || rows_i > 65535) return -1;
    uint32_t cols = (uint32_t)cols_i;
    uint32_t rows = (uint32_t)rows_i;
    size_t cells = (size_t)cols * rows;

    float* mask = calloc(cells, sizeof(float));
    if (!mask) return -1;
    if (rasterize_features(geojson_bytes, geojson_len, bbox, params.cell_size_m,
                           cols, rows, off_x, off_y, mask) != 0) {
        free(mask);
        return -1;
    }

    // Dilate (thickness - 1) passes. Each pass expands the mask by one
    // 4-neighbour cell, matching the bench-v2 backend implementation.
    if (params.thickness > 1) {
        float* next = calloc(cells, sizeof(float));
        if (!next) {
            free(mask);
            return -1;
        }
        for (int pass = 1; pass < params.thickness; pass++) {
            dilate_pass(mask, cols, rows, next);
            float* tmp = mask;
            mask = next;
            next = tmp;
        }
        free(next);
    }

    uint8_t* buf = raster_alloc(cols, rows, 1);
    if (!buf) {
        free(mask);
        return -1;
    }
    float* out = raster_channel_ptr(buf, cols, rows, 0);
    memcpy(out, mask, cells * sizeof(float));
    free(mask);

    return step_write_output_owned(ctx, "cadastre-edges-raster", buf,
                                   raster_total_bytes(cols, rows, 1));
}

static const step_read_t reads_arr[] = {
    {.name = "cadastre-buildings-geojson"},
    {.name = "canonical-location"},
};
static const step_write_t writes_arr[] = {
    {.name = "cadastre-edges-raster", .kind = AK_RASTER_F32, .visibility = STEP_VIS_DEBUG},
};

const step_def_t rasterize_cadastre_edges_def = {
    .name = "rasterize-cadastre-edges",
    .version = 1,
    .reads = reads_arr,
    .n_reads = 2,
    .writes = writes_arr,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};

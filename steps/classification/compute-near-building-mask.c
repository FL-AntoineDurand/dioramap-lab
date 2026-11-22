// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// compute-near-building-mask
// Reads:  lidar-building-mask (0.5 m grid, 1-channel),
//         cadastre-buildings-geojson,
//         cadastre-fit (JSON with offsetX / offsetY),
//         canonical-location
// Params: { "cell_size_m": float (default 1.0),
//           "dilate_r": int (default 3) — (2R+1)² morphological max }
// Writes: lidar-near-building-mask — 1-channel F32 raster (0 or 1) at the
//         classification grid resolution.
//
// Combines the LiDAR building mask (projected from its 0.5 m grid) with
// the cadastre footprint polygons (edge-rasterised with cadastre-fit
// offset applied) into a single "building proximity" mask, then dilates
// by `dilate_r` cells. Classify-vegetation uses this to relax thresholds
// near buildings (close-enough points can't be grass / canopy through
// building walls, but the LiDAR may still return them).
//
// Direct port of the near-building section of bench-v2
// classify-vegetation.js.

#include <jansson.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "v3/artifact.h"
#include "v3/json_int.h"
#include "v3/step.h"
#include "v3/step_bbox_params.h"

#include "../registration/raster_format.h"
#include "classification-steps.h"

typedef struct near_params {
    double cell_size_m;
    int dilate_r;
} near_params_t;

static void parse_params(const char* s, near_params_t* o) {
    o->cell_size_m = 1.0;
    o->dilate_r = 3;
    if (!s) return;
    json_error_t je;
    json_t* p = json_loads(s, 0, &je);
    if (!p) return;
    json_t* j = json_object_get(p, "cell_size_m");
    if (json_is_number(j)) {
        double v = json_number_value(j);
        if (v > 1e-6 && v < 100.0) o->cell_size_m = v;
    }
    j = json_object_get(p, "dilate_r");
    if (json_int_or_default(j, 0, 32, "dilate_r", &o->dilate_r) != 0) {
        json_decref(p); return;  // parse_params void; stderr already showed why.
    }
    json_decref(p);
}

// Rasterize a single ring's edges at `cell_size` resolution, applying the
// (off_x, off_y) L93 offset.
static void rasterize_ring_onto(uint8_t* mask, uint32_t cols, uint32_t rows, const double bbox[4],
                                double cell_size, json_t* ring, double off_x, double off_y) {
    size_t nv = json_array_size(ring);
    if (nv < 2) return;
    double xmin = bbox[0];
    double ymin = bbox[1];
    for (size_t i = 0; i < nv; i++) {
        json_t* v_a = json_array_get(ring, i);
        json_t* v_b = json_array_get(ring, (i + 1) % nv);
        if (!json_is_array(v_a) || !json_is_array(v_b)) continue;
        double ax = json_number_value(json_array_get(v_a, 0));
        double ay = json_number_value(json_array_get(v_a, 1));
        double bx = json_number_value(json_array_get(v_b, 0));
        double by = json_number_value(json_array_get(v_b, 1));
        double dx = bx - ax;
        double dy = by - ay;
        double seg_len = sqrt(dx * dx + dy * dy);
        int steps = (int)ceil(seg_len / cell_size);
        if (steps < 1) steps = 1;
        for (int s = 0; s <= steps; s++) {
            double t = (double)s / (double)steps;
            double px = ax + dx * t + off_x;
            double py = ay + dy * t + off_y;
            int col = (int)floor((px - xmin) / cell_size);
            int row = (int)floor((py - ymin) / cell_size);
            if (col < 0 || col >= (int)cols || row < 0 || row >= (int)rows) continue;
            mask[(size_t)row * cols + col] = 1;
        }
    }
}

static int exec_fn(step_context_t* ctx) {
    near_params_t params;
    parse_params(step_get_params(ctx), &params);

    double bbox[4];
    if (step_read_bbox_l93(ctx, bbox) != 0) return -1;
    double width_m = bbox[2] - bbox[0];
    double height_m = bbox[3] - bbox[1];
    int64_t cols_i = (int64_t)round(width_m / params.cell_size_m);
    int64_t rows_i = (int64_t)round(height_m / params.cell_size_m);
    if (cols_i < 2) cols_i = 2;
    if (rows_i < 2) rows_i = 2;
    if (cols_i > 65535 || rows_i > 65535) return -1;
    uint32_t cols = (uint32_t)cols_i;
    uint32_t rows = (uint32_t)rows_i;
    size_t cells = (size_t)cols * rows;

    uint8_t* mask = calloc(cells, 1);
    if (!mask) return -1;

    // (a) Project the LiDAR building mask from its 0.5 m grid onto ours.
    size_t lb_len = 0;
    const uint8_t* lb_bytes = (const uint8_t*)step_read(ctx, "lidar-building-mask", &lb_len);
    if (lb_bytes) {
        raster_view_t lb_v;
        if (raster_view_open(lb_bytes, lb_len, &lb_v) == 0 && lb_v.n_channels == 1) {
            double lb_cell_w = width_m / lb_v.cols;
            double lb_cell_h = height_m / lb_v.rows;
            const float* lb = lb_v.channels[0];
            for (uint32_t br = 0; br < lb_v.rows; br++) {
                for (uint32_t bc = 0; bc < lb_v.cols; bc++) {
                    if (lb[(size_t)br * lb_v.cols + bc] < 0.5f) continue;
                    double lx = bbox[0] + (bc + 0.5) * lb_cell_w;
                    double ly = bbox[1] + (br + 0.5) * lb_cell_h;
                    int tc = (int)floor((lx - bbox[0]) / params.cell_size_m);
                    int tr = (int)floor((ly - bbox[1]) / params.cell_size_m);
                    if (tc >= 0 && tc < (int)cols && tr >= 0 && tr < (int)rows) {
                        mask[(size_t)tr * cols + tc] = 1;
                    }
                }
            }
        }
    }

    // (b) Cadastre buildings — edge-rasterise with the cadastre-fit offset
    // applied. If cadastre-fit isn't available we just use zero offset.
    size_t cf_len = 0;
    const char* cf_bytes = (const char*)step_read(ctx, "cadastre-fit", &cf_len);
    double off_x = 0, off_y = 0;
    if (cf_bytes) {
        json_error_t je;
        json_t* jf = json_loadb(cf_bytes, cf_len, 0, &je);
        if (jf) {
            json_t* jx = json_object_get(jf, "offsetX");
            json_t* jy = json_object_get(jf, "offsetY");
            if (json_is_number(jx)) off_x = json_number_value(jx);
            if (json_is_number(jy)) off_y = json_number_value(jy);
            json_decref(jf);
        }
    }
    size_t gj_len = 0;
    const char* gj_bytes = (const char*)step_read(ctx, "cadastre-buildings-geojson", &gj_len);
    if (gj_bytes) {
        json_error_t je;
        json_t* root = json_loadb(gj_bytes, gj_len, 0, &je);
        if (root) {
            json_t* features = json_object_get(root, "features");
            if (json_is_array(features)) {
                size_t nf = json_array_size(features);
                for (size_t i = 0; i < nf; i++) {
                    json_t* f = json_array_get(features, i);
                    json_t* geom = json_object_get(f, "geometry");
                    if (!json_is_object(geom)) continue;
                    const char* type = json_string_value(json_object_get(geom, "type"));
                    json_t* coords = json_object_get(geom, "coordinates");
                    if (!type || !json_is_array(coords)) continue;
                    if (strcmp(type, "Polygon") == 0) {
                        json_t* ring = json_array_get(coords, 0);
                        if (json_is_array(ring)) {
                            rasterize_ring_onto(mask, cols, rows, bbox, params.cell_size_m, ring,
                                                off_x, off_y);
                        }
                    } else if (strcmp(type, "MultiPolygon") == 0) {
                        size_t np = json_array_size(coords);
                        for (size_t pi = 0; pi < np; pi++) {
                            json_t* poly = json_array_get(coords, pi);
                            if (!json_is_array(poly)) continue;
                            json_t* ring = json_array_get(poly, 0);
                            if (json_is_array(ring)) {
                                rasterize_ring_onto(mask, cols, rows, bbox, params.cell_size_m,
                                                    ring, off_x, off_y);
                            }
                        }
                    }
                }
            }
            json_decref(root);
        }
    }

    // (c) Dilate by dilate_r cells using a (2R+1)² morphological max.
    uint8_t* dilated = calloc(cells, 1);
    if (!dilated) {
        free(mask);
        return -1;
    }
    int R = params.dilate_r;
    for (uint32_t r = 0; r < rows; r++) {
        int r0 = (int)r - R < 0 ? 0 : (int)r - R;
        int r1 = (int)r + R > (int)rows - 1 ? (int)rows - 1 : (int)r + R;
        for (uint32_t c = 0; c < cols; c++) {
            int c0 = (int)c - R < 0 ? 0 : (int)c - R;
            int c1 = (int)c + R > (int)cols - 1 ? (int)cols - 1 : (int)c + R;
            int found = 0;
            for (int rr = r0; rr <= r1 && !found; rr++) {
                for (int cc = c0; cc <= c1; cc++) {
                    if (mask[(size_t)rr * cols + cc]) {
                        found = 1;
                        break;
                    }
                }
            }
            dilated[(size_t)r * cols + c] = (uint8_t)found;
        }
    }
    free(mask);

    // Pack into a 1-channel F32 raster (0.0 or 1.0) for uniformity.
    uint8_t* buf = raster_alloc(cols, rows, 1);
    if (!buf) {
        free(dilated);
        return -1;
    }
    float* out = raster_channel_ptr(buf, cols, rows, 0);
    for (size_t i = 0; i < cells; i++) out[i] = dilated[i] ? 1.0f : 0.0f;
    free(dilated);

    return step_write_output_owned(ctx, "lidar-near-building-mask", buf,
                                   raster_total_bytes(cols, rows, 1));
}

static const step_read_t reads_arr[] = {
    {.name = "lidar-building-mask"},
    {.name = "cadastre-buildings-geojson"},
    {.name = "cadastre-fit"},
    {.name = "canonical-location"},
};
static const step_write_t writes_arr[] = {
    {.name = "lidar-near-building-mask", .kind = AK_RASTER_F32, .visibility = STEP_VIS_DEBUG},
};

const step_def_t compute_near_building_mask_def = {
    .name = "compute-near-building-mask",
    .version = 1,
    .reads = reads_arr,
    .n_reads = 4,
    .writes = writes_arr,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};

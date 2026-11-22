// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// build-parcel-lines
// Reads:  cadastre-parcels-geojson, lidar-ground-grid, elevation-f32,
//         scene-origin, canonical-location, cadastre-fit
// Params: { "height_offset_m": float (default 1.5 — fence height; lifts
//                                  parcel lines off the terrain so they
//                                  read as a property boundary fence
//                                  rather than a ground-flush line),
//           "subdivide_m": float (default 2.0),
//           "ground_cell_size_m": float (default 1.0 — must match
//                                  build-lidar-ground),
//           "clip_to_disc_radius": bool (default true) — clip segments to
//                                  the canonical-location disc when set;
//                                  the bench uses tile radius. }
// Writes: parcel-lines — raw F32 array of line-segment endpoints in
//         scene-local coordinates: [x1,y1,z1, x2,y2,z2, x1,y1,z1,
//         x2,y2,z2, ...] per segment. Frontends wrap this in
//         LineSegments2 (or equivalent) for rendering. No header — the
//         length is implicit in byte count (segments = bytes / 24).
//
// Each cadastre parcel polygon edge is subdivided into ~subdivide_m steps;
// each subdivision point gets its altitude from the LiDAR ground grid
// (bilinear) with fallback to the BIL elevation grid when the point is
// outside or in a no-data cell. Cadastre vertices are pre-shifted by the
// cadastre-fit (offsetX, offsetY) before sampling.
//
// Direct port of bench-v2 viewer/backend/features/parcels-renderer.js
// `buildParcelLines`.

#include <jansson.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "v3/artifact.h"
#include "v3/step.h"

#include "../registration/raster_format.h"
#include "ground-steps.h"

typedef struct parcel_params {
    double height_offset_m;
    double subdivide_m;
    double ground_cell_size_m;
    bool clip_to_disc_radius;
} parcel_params_t;

static void parse_params(const char* s, parcel_params_t* o) {
    o->height_offset_m = 1.5;
    o->subdivide_m = 2.0;
    o->ground_cell_size_m = 1.0;
    o->clip_to_disc_radius = true;
    if (!s) return;
    json_error_t je;
    json_t* p = json_loads(s, 0, &je);
    if (!p) return;
    json_t* j = json_object_get(p, "height_offset_m");
    if (json_is_number(j)) o->height_offset_m = json_number_value(j);
    j = json_object_get(p, "subdivide_m");
    if (json_is_number(j)) {
        double v = json_number_value(j);
        if (v > 1e-3 && v < 100.0) o->subdivide_m = v;
    }
    j = json_object_get(p, "ground_cell_size_m");
    if (json_is_number(j)) {
        double v = json_number_value(j);
        if (v > 1e-6 && v < 100.0) o->ground_cell_size_m = v;
    }
    j = json_object_get(p, "clip_to_disc_radius");
    if (json_is_boolean(j)) o->clip_to_disc_radius = json_is_true(j);
    json_decref(p);
}

// Read center + radius + bbox from canonical-location JSON.
typedef struct loc_info {
    double bbox[4];
    double center_x, center_y;
    double radius_m;
} loc_info_t;

static int read_loc(step_context_t* ctx, loc_info_t* out) {
    size_t len = 0;
    const char* bytes = (const char*)step_read(ctx, "canonical-location", &len);
    if (!bytes) return -1;
    json_error_t je;
    json_t* j = json_loadb(bytes, len, 0, &je);
    if (!j) return -1;
    json_t* jb = json_object_get(j, "bbox_l93");
    if (!json_is_array(jb) || json_array_size(jb) != 4) {
        json_decref(j);
        return -1;
    }
    for (int i = 0; i < 4; i++) out->bbox[i] = json_number_value(json_array_get(jb, i));
    out->center_x = json_number_value(json_object_get(j, "center_l93_x"));
    out->center_y = json_number_value(json_object_get(j, "center_l93_y"));
    out->radius_m = json_number_value(json_object_get(j, "radius_m"));
    json_decref(j);
    return 0;
}

typedef struct origin_info {
    double x, y, z;
} origin_info_t;

static int read_origin(step_context_t* ctx, origin_info_t* out) {
    size_t len = 0;
    const char* bytes = (const char*)step_read(ctx, "scene-origin", &len);
    if (!bytes) return -1;
    json_error_t je;
    json_t* j = json_loadb(bytes, len, 0, &je);
    if (!j) return -1;
    out->x = json_number_value(json_object_get(j, "x"));
    out->y = json_number_value(json_object_get(j, "y"));
    out->z = json_number_value(json_object_get(j, "z"));
    json_decref(j);
    return 0;
}

static int read_cadastre_fit(step_context_t* ctx, double* off_x, double* off_y) {
    *off_x = 0;
    *off_y = 0;
    size_t len = 0;
    const char* bytes = (const char*)step_read(ctx, "cadastre-fit", &len);
    if (!bytes) return 0;
    json_error_t je;
    json_t* j = json_loadb(bytes, len, 0, &je);
    if (!j) return -1;
    json_t* jx = json_object_get(j, "offsetX");
    json_t* jy = json_object_get(j, "offsetY");
    if (json_is_number(jx)) *off_x = json_number_value(jx);
    if (json_is_number(jy)) *off_y = json_number_value(jy);
    json_decref(j);
    return 0;
}

// Bilinear sample of the lidar ground grid at L93 (x, y). Returns the
// sampled altitude, or NAN if outside the bbox / cell coords are out of
// range. Mirrors bench's sampleLidarMinZ.
static float sample_lidar_ground(const float* grid, uint32_t cols, uint32_t rows,
                                 const double bbox[4], double cell, double x, double y) {
    double fx = (x - bbox[0]) / cell - 0.5;
    double fy = (y - bbox[1]) / cell - 0.5;
    int64_t c0 = (int64_t)floor(fx);
    int64_t r0 = (int64_t)floor(fy);
    int64_t c1 = c0 + 1;
    int64_t r1 = r0 + 1;
    if (c0 < 0 || c1 >= (int64_t)cols || r0 < 0 || r1 >= (int64_t)rows) return NAN;
    double wc = fx - c0;
    double wr = fy - r0;
    float a = grid[r0 * cols + c0];
    float b = grid[r0 * cols + c1];
    float c_ = grid[r1 * cols + c0];
    float d = grid[r1 * cols + c1];
    if (isnan(a) || isnan(b) || isnan(c_) || isnan(d)) return NAN;
    return (float)((1 - wc) * (1 - wr) * a + wc * (1 - wr) * b +
                   (1 - wc) * wr * c_ + wc * wr * d);
}

// Bilinear sample of the BIL elevation grid (square: assumed cols=rows
// inferred from byte count) over the canonical-location bbox.
static float sample_elevation(const float* grid, uint32_t side, const double bbox[4],
                              double x, double y, float fallback_min) {
    if (x <= bbox[0] || x >= bbox[2] || y <= bbox[1] || y >= bbox[3]) return fallback_min;
    double fx = ((x - bbox[0]) / (bbox[2] - bbox[0])) * (side - 1);
    double fy = ((y - bbox[1]) / (bbox[3] - bbox[1])) * (side - 1);
    int64_t c0 = (int64_t)floor(fx);
    int64_t r0 = (int64_t)floor(fy);
    int64_t c1 = c0 + 1 < (int64_t)side ? c0 + 1 : (int64_t)side - 1;
    int64_t r1 = r0 + 1 < (int64_t)side ? r0 + 1 : (int64_t)side - 1;
    if (c0 < 0) c0 = 0;
    if (r0 < 0) r0 = 0;
    double wc = fx - c0;
    double wr = fy - r0;
    float a = grid[r0 * side + c0];
    float b = grid[r0 * side + c1];
    float c_ = grid[r1 * side + c0];
    float d = grid[r1 * side + c1];
    return (float)((1 - wc) * (1 - wr) * a + wc * (1 - wr) * b +
                   (1 - wc) * wr * c_ + wc * wr * d);
}

// Walk every parcel ring once. If `out_segments` is NULL, only count
// segments that survive clipping. Otherwise, fill the segment buffer.
// Each segment writes 6 floats: x1, y1, z1, x2, y2, z2 (scene-local).
static int walk_parcels(json_t* features, const parcel_params_t* params,
                        const loc_info_t* loc, const origin_info_t* origin,
                        const float* lidar_grid, uint32_t lg_cols, uint32_t lg_rows,
                        const float* elev_grid, uint32_t elev_side, float elev_min,
                        double cad_off_x, double cad_off_y, float* out_segments,
                        size_t* out_seg_count) {
    bool count_only = (out_segments == NULL);
    double clip_r2 = loc->radius_m * loc->radius_m;
    bool clip = params->clip_to_disc_radius && loc->radius_m > 0;
    double ccx = loc->center_x;
    double ccy = loc->center_y;

    size_t emit_idx = 0;
    size_t n_segs = 0;
    size_t nf = json_array_size(features);
    for (size_t fi = 0; fi < nf; fi++) {
        json_t* f = json_array_get(features, fi);
        json_t* geom = json_object_get(f, "geometry");
        if (!json_is_object(geom)) continue;
        const char* type = json_string_value(json_object_get(geom, "type"));
        json_t* coords = json_object_get(geom, "coordinates");
        if (!type || !json_is_array(coords)) continue;
        // Collect all rings to walk: Polygon → 1 outer ring; MultiPolygon → N outer rings.
        size_t nrings = 0;
        json_t* rings[64]; // unlikely to exceed
        if (strcmp(type, "Polygon") == 0) {
            json_t* r = json_array_get(coords, 0);
            if (json_is_array(r) && nrings < 64) rings[nrings++] = r;
        } else if (strcmp(type, "MultiPolygon") == 0) {
            size_t np = json_array_size(coords);
            for (size_t pi = 0; pi < np && nrings < 64; pi++) {
                json_t* poly = json_array_get(coords, pi);
                if (!json_is_array(poly)) continue;
                json_t* r = json_array_get(poly, 0);
                if (json_is_array(r)) rings[nrings++] = r;
            }
        }
        for (size_t ri = 0; ri < nrings; ri++) {
            json_t* ring = rings[ri];
            size_t nv = json_array_size(ring);
            if (nv < 2) continue;
            for (size_t i = 0; i < nv; i++) {
                json_t* va = json_array_get(ring, i);
                json_t* vb = json_array_get(ring, (i + 1) % nv);
                if (!json_is_array(va) || !json_is_array(vb)) continue;
                double ax = json_number_value(json_array_get(va, 0));
                double ay = json_number_value(json_array_get(va, 1));
                double bx = json_number_value(json_array_get(vb, 0));
                double by = json_number_value(json_array_get(vb, 1));
                double dx = bx - ax;
                double dy = by - ay;
                double seg_len = sqrt(dx * dx + dy * dy);
                // Frama-C audit finding GROUND-1: guard against
                //  (a) params->subdivide_m being 0 → division by zero → inf
                //      → (int)ceil(inf) is UB.
                //  (b) seg_len/subdivide_m > INT_MAX → (int)cast is UB.
                int n;
                if (params->subdivide_m <= 0.0) {
                    n = 1; // pathological config; just one straight segment.
                } else {
                    double n_d = ceil(seg_len / params->subdivide_m);
                    if (!(n_d <= (double)INT_MAX)) {
                        n = INT_MAX; // saturate; the calling code clamps
                                     // to its own ceiling downstream.
                    } else {
                        n = (int)n_d;
                    }
                }
                if (n < 1) n = 1;

                double prev_px = ax + cad_off_x;
                double prev_py = ay + cad_off_y;
                float prev_z;
                {
                    float lz = sample_lidar_ground(lidar_grid, lg_cols, lg_rows, loc->bbox,
                                                   params->ground_cell_size_m, prev_px, prev_py);
                    prev_z = isfinite(lz) ? lz
                                          : sample_elevation(elev_grid, elev_side, loc->bbox,
                                                              prev_px, prev_py, elev_min);
                }
                // v3 Z-up convention (Lambert-93 native): X = east, Y = north,
                // Z = altitude. Same frame the recentered lidar-positions
                // artifact uses, so client viz can render directly without
                // any axis swap. (v1 used Y-up legacy bench-v2 convention.)
                double prev_sx = prev_px - origin->x;
                double prev_sy = prev_py - origin->y;
                double prev_sz = (double)prev_z + params->height_offset_m - origin->z;

                for (int k = 1; k <= n; k++) {
                    double t = (double)k / (double)n;
                    double px = ax + dx * t + cad_off_x;
                    double py = ay + dy * t + cad_off_y;
                    float z;
                    {
                        float lz = sample_lidar_ground(lidar_grid, lg_cols, lg_rows, loc->bbox,
                                                       params->ground_cell_size_m, px, py);
                        z = isfinite(lz) ? lz
                                         : sample_elevation(elev_grid, elev_side, loc->bbox, px,
                                                             py, elev_min);
                    }
                    double sx = px - origin->x;
                    double sy = py - origin->y;
                    double sz = (double)z + params->height_offset_m - origin->z;

                    bool in_a = !clip;
                    bool in_b = !clip;
                    if (clip) {
                        double pdx = prev_px - ccx;
                        double pdy = prev_py - ccy;
                        in_a = (pdx * pdx + pdy * pdy) <= clip_r2;
                        double cdx = px - ccx;
                        double cdy = py - ccy;
                        in_b = (cdx * cdx + cdy * cdy) <= clip_r2;
                    }
                    if (in_a && in_b) {
                        if (!count_only) {
                            out_segments[emit_idx++] = (float)prev_sx;
                            out_segments[emit_idx++] = (float)prev_sy;
                            out_segments[emit_idx++] = (float)prev_sz;
                            out_segments[emit_idx++] = (float)sx;
                            out_segments[emit_idx++] = (float)sy;
                            out_segments[emit_idx++] = (float)sz;
                        }
                        n_segs++;
                    }
                    prev_px = px;
                    prev_py = py;
                    prev_sx = sx;
                    prev_sy = sy;
                    prev_sz = sz;
                }
            }
        }
    }
    *out_seg_count = n_segs;
    return 0;
}

static int exec_fn(step_context_t* ctx) {
    parcel_params_t params;
    parse_params(step_get_params(ctx), &params);

    loc_info_t loc;
    if (read_loc(ctx, &loc) != 0) return -1;
    origin_info_t origin;
    if (read_origin(ctx, &origin) != 0) return -1;

    double cad_off_x = 0, cad_off_y = 0;
    if (read_cadastre_fit(ctx, &cad_off_x, &cad_off_y) != 0) return -1;

    // Lidar ground grid.
    size_t lg_bytes = 0;
    const uint8_t* lg_in = (const uint8_t*)step_read(ctx, "lidar-ground-grid", &lg_bytes);
    if (!lg_in) return -1;
    raster_view_t lg_v;
    if (raster_view_open(lg_in, lg_bytes, &lg_v) != 0 || lg_v.n_channels != 1) {
        return -1;
    }
    const float* lidar_grid = lg_v.channels[0];

    // Elevation grid: square F32 with `side = sqrt(N)` cells (caller's
    // grid_size param to fetch-elevation, default 128). Compute the side
    // and a min for the fallback path.
    size_t elev_bytes = 0;
    const uint8_t* elev_raw = (const uint8_t*)step_read(ctx, "elevation-f32", &elev_bytes);
    // fetch-elevation v2 wraps BIL bytes in a 12-byte raster header
    // (u32 cols/rows/n_channels). Skip past it.
    if (!elev_raw || elev_bytes < 12 + sizeof(float)) return -1;
    const float* elev_grid = (const float*)(elev_raw + 12);
    size_t elev_payload = elev_bytes - 12;
    if (elev_payload % sizeof(float) != 0) return -1;
    size_t elev_n = elev_payload / sizeof(float);
    uint32_t elev_side = (uint32_t)sqrt((double)elev_n);
    if ((size_t)elev_side * elev_side != elev_n) return -1;
    float elev_min = INFINITY;
    for (size_t i = 0; i < elev_n; i++) {
        if (elev_grid[i] < elev_min) elev_min = elev_grid[i];
    }
    if (!isfinite(elev_min)) elev_min = 0.0f;

    // Parcel polygons.
    size_t gj_bytes = 0;
    const char* gj_in = (const char*)step_read(ctx, "cadastre-parcels-geojson", &gj_bytes);
    if (!gj_in) return -1;
    json_error_t je;
    json_t* root = json_loadb(gj_in, gj_bytes, 0, &je);
    if (!root) return -1;
    json_t* features = json_object_get(root, "features");
    if (!json_is_array(features)) {
        json_decref(root);
        return -1;
    }

    // Two-pass: count segments, allocate, fill.
    size_t n_segs = 0;
    if (walk_parcels(features, &params, &loc, &origin, lidar_grid, lg_v.cols, lg_v.rows,
                     elev_grid, elev_side, elev_min, cad_off_x, cad_off_y, NULL, &n_segs) != 0) {
        json_decref(root);
        return -1;
    }
    size_t out_floats = n_segs * 6;
    size_t out_bytes = out_floats * sizeof(float);
    float* segments = NULL;
    if (out_floats > 0) {
        segments = malloc(out_bytes);
        if (!segments) {
            json_decref(root);
            return -1;
        }
        size_t verify = 0;
        if (walk_parcels(features, &params, &loc, &origin, lidar_grid, lg_v.cols, lg_v.rows,
                         elev_grid, elev_side, elev_min, cad_off_x, cad_off_y, segments,
                         &verify) != 0) {
            free(segments);
            json_decref(root);
            return -1;
        }
    }
    json_decref(root);

    // Allow zero segments (empty parcel set or fully clipped). Empty
    // artifact is a valid output here.
    if (out_floats == 0) {
        // step_write_output_owned with NULL bytes & len 0 isn't supported
        // (the buffer must be valid). Allocate a 1-byte placeholder; the
        // store handles len=1 fine, downstream sees byte_count/24 = 0.
        uint8_t* placeholder = calloc(1, 1);
        if (!placeholder) return -1;
        return step_write_output_owned(ctx, "parcel-lines", placeholder, 0);
    }
    return step_write_output_owned(ctx, "parcel-lines", (uint8_t*)segments, out_bytes);
}

static const step_read_t reads_arr[] = {
    {.name = "cadastre-parcels-geojson"},
    {.name = "lidar-ground-grid"},
    {.name = "elevation-f32"},
    {.name = "scene-origin"},
    {.name = "canonical-location"},
    {.name = "cadastre-fit"},
};
static const step_write_t writes_arr[] = {
    {.name = "parcel-lines", .kind = AK_POINT_COLUMN, .visibility = STEP_VIS_PUBLIC},
};

const step_def_t build_parcel_lines_def = {
    .name = "build-parcel-lines",
    // v2: switched output frame from legacy Y-up (sy=alt, sz=-north) to
    // v3 Z-up (sy=north, sz=alt) to match every other v3 producer. Bytes
    // shape unchanged (still 6 F32 per segment); semantics changed, hence
    // the version bump so cached v1 entries don't get served as v2.
    // v3: read past the 12-byte raster header that fetch-elevation v2
    // now emits before the BIL F32 grid.
    .version = 3,
    .reads = reads_arr,
    .n_reads = 6,
    .writes = writes_arr,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};

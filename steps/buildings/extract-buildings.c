// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// extract-buildings
// Reads:  cadastre-buildings-geojson, lidar-positions, canonical-location
// Params: { "min_points": int    (default 50)  — drop buildings with fewer
//             collected points than this; cuts empty / outside-disc
//             footprints and tiny structures we can't reconstruct anyway.
//           "buffer_m":   float  (default 5.0) — horizontal distance in
//             metres around each footprint within which lidar points are
//             also included. See "roof-entry semantics" below. }
// Writes: per-building-records — AK_ARRAY_INLINE, variable-length records
//         (stride=0). One record per surviving building.
//
// Phase 1 of the roof-recon evaluation. Slices the disc-wide point cloud
// + cadastre footprints into per-building inputs that each candidate
// roof-reconstruction pipeline (PolyFit, KSR, Roofer, V7) consumes
// identically. See v3/docs/research/roof-recon-survey.md (Phase 1).
//
// Per-record binary layout (little-endian, scene-local Z-up frame):
//
//   offset  type        field
//   ──────  ──────────  ────────────────────────────────────────────────
//   0       u32         schema_version = 1
//   4       u32         footprint_vertex_count       (= N, ring closed
//                                                    NOT duplicated)
//   8       u32         point_index_count            (= M)
//   12      f64 [6]     bbox  (xmin,ymin,zmin,xmax,ymax,zmax) in L93
//   60      f64         ground_z  (L93 altitude — min-Z of inlier points)
//   68      f64 [N][3]  footprint  (L93 x,y,z; z = ground_z so the ring
//                                  lies on the ground plane; ring is
//                                  OPEN — first vertex not repeated)
//   68+24N  u32 [M]     point_indices (indices into lidar-positions —
//                                   that artifact is f64×3 interleaved,
//                                   so byte offset of point i is 24*i)
//
// MultiPolygon features → one record per outer ring. Inner rings (holes)
// are ignored at this phase; courtyard handling is candidate-specific.
//
// Roof-entry semantics (the reason this step exists, not "exact slice"):
// IGN's cadastre-buildings-geojson is NOT well co-registered with the
// IGN lidar product — sub-metre to metre-scale lateral offsets between
// the cadastre footprint and the lidar return cluster are routine.
// `viewer-full` corrects this with rasterize-cadastre-edges +
// fit-cadastre-to-lidar (~5–10 mins of pre-processing), but for the
// roof-recon evaluation we treat the footprint as a *roof entry hint*
// rather than a strict polygon clip. Each candidate algorithm runs its
// own plane extraction / boundary detection on the collected points, so
// it doesn't need the polygon to be exact — it needs to KNOW where the
// roof is, then look around. We collect every lidar point within
// `buffer_m` (default 5 m) of the footprint in the horizontal plane
// (PIP-inside ∪ ≤ buffer_m to any polygon edge). All points are listed
// undifferentiated in `point_indices` — candidates that want the
// "definitely inside" subset can re-run PIP locally with the footprint
// (which is right there in the same record).
//
// Coordinate frame: everything stays in L93 metres + altitude (the same
// frame lidar-positions emits). Candidates that need scene-local can
// subtract a chosen origin themselves (the bbox centroid is a sensible
// per-building default). f64 throughout so accuracy survives across L93
// extents (~10 km eastings hit float32 precision limits).
//
// ground_z is the minimum-Z of the collected points. This deliberately
// avoids depending on lidar-ground-grid (which needs the full
// classification chain) or fetch-elevation; the eval pipelines just
// need a reasonable ground reference at the building's base, and
// min-z of collected points is that on every roof type. With a 5 m
// buffer the collected set will include some street/sidewalk hits
// near disc-edge buildings, which makes the ground reference more
// faithful than strict-PIP would.

#define _POSIX_C_SOURCE 200809L

#include <jansson.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "v3/artifact.h"
#include "v3/blob.h"
#include "v3/step.h"

#include "buildings-steps.h"

// ---------------------------------------------------------------------------
// Params
// ---------------------------------------------------------------------------

typedef struct extract_params {
    uint32_t min_points;
    double   buffer_m;
} extract_params_t;

static void parse_params(const char* s, extract_params_t* o) {
    o->min_points = 50;
    o->buffer_m = 5.0;
    if (!s) return;
    json_error_t je;
    json_t* p = json_loads(s, 0, &je);
    if (!p) return;
    json_t* j = json_object_get(p, "min_points");
    if (json_is_integer(j)) {
        json_int_t v = json_integer_value(j);
        if (v >= 0 && v < 1000000) o->min_points = (uint32_t)v;
    }
    j = json_object_get(p, "buffer_m");
    if (json_is_number(j)) {
        double v = json_number_value(j);
        if (v >= 0.0 && v < 100.0) o->buffer_m = v;
    }
    json_decref(p);
}

// ---------------------------------------------------------------------------
// Input readers (canonical-location, scene-origin, cadastre-fit)
// ---------------------------------------------------------------------------

typedef struct loc_info {
    double bbox[4];        // L93 xmin, ymin, xmax, ymax
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
    for (int i = 0; i < 4; i++)
        out->bbox[i] = json_number_value(json_array_get(jb, i));
    json_decref(j);
    return 0;
}

// ---------------------------------------------------------------------------
// Point-in-polygon (ray-casting). Footprint is a flat array of 3*N doubles
// (x, y, z per vertex); we use only x, y. Ring is open (first vertex not
// repeated at end). Returns true iff (px, py) is inside.
// ---------------------------------------------------------------------------

static bool point_in_poly(const double* fp, uint32_t n, double px, double py) {
    bool inside = false;
    uint32_t j = n - 1;
    for (uint32_t i = 0; i < n; i++) {
        double xi = fp[3 * i];
        double yi = fp[3 * i + 1];
        double xj = fp[3 * j];
        double yj = fp[3 * j + 1];
        // Standard horizontal-ray crossing-number test. Edge from (xj,yj)
        // to (xi,yi) crosses the ray at y=py iff one endpoint's y is on
        // each side of py and the intersection x exceeds px.
        bool cross = ((yi > py) != (yj > py)) &&
                     (px < (xj - xi) * (py - yi) / (yj - yi) + xi);
        if (cross) inside = !inside;
        j = i;
    }
    return inside;
}

// Squared distance from (px, py) to the segment [(ax,ay)–(bx,by)].
// Standard projection onto segment, clamped to [0,1]. xy only.
static double point_to_segment_dist2(double px, double py, double ax, double ay,
                                     double bx, double by) {
    double dx = bx - ax;
    double dy = by - ay;
    double len2 = dx * dx + dy * dy;
    if (len2 < 1e-12) {
        // Degenerate segment — distance to endpoint.
        double ex = px - ax, ey = py - ay;
        return ex * ex + ey * ey;
    }
    double tu = (px - ax) * dx + (py - ay) * dy;
    double t = tu / len2;
    if (t < 0.0) t = 0.0;
    else if (t > 1.0) t = 1.0;
    double cx = ax + t * dx;
    double cy = ay + t * dy;
    double ex = px - cx, ey = py - cy;
    return ex * ex + ey * ey;
}

// Squared minimum xy distance from (px, py) to any edge of the polygon.
// Returns 0 if the point is on an edge (rare). Caller still needs PIP if
// they want to distinguish inside-vs-outside; this helper just measures
// boundary proximity.
static double dist_to_polygon_xy_squared(const double* fp, uint32_t n, double px,
                                         double py) {
    double best = INFINITY;
    uint32_t j = n - 1;
    for (uint32_t i = 0; i < n; i++) {
        double d = point_to_segment_dist2(px, py, fp[3 * j], fp[3 * j + 1],
                                          fp[3 * i], fp[3 * i + 1]);
        if (d < best) best = d;
        j = i;
    }
    return best;
}

// ---------------------------------------------------------------------------
// Per-record builder. A "ring" is the outer ring of one Polygon (or one
// of a MultiPolygon's polygons). We allocate the record bytes once we
// know the surviving point count.
// ---------------------------------------------------------------------------

#define HEADER_BYTES 68

typedef struct ring_record_buf {
    uint8_t* bytes;
    size_t len;
} ring_record_buf_t;

// Collect footprint vertices from a GeoJSON ring (array of [x,y] pairs)
// into a freshly malloc'd double array of size 3*N. Returns N (vertex
// count). Skips the closing duplicate vertex if present. Vertices stay
// in raw L93 — z is set to `ground_z` (the eventual ground plane).
static double* footprint_doubles(json_t* ring, double ground_z, uint32_t* out_n) {
    size_t nv = json_array_size(ring);
    if (nv < 3) {
        *out_n = 0;
        return NULL;
    }
    // Detect & skip closing duplicate (GeoJSON rings are closed).
    {
        json_t* v0 = json_array_get(ring, 0);
        json_t* vN = json_array_get(ring, nv - 1);
        if (json_is_array(v0) && json_is_array(vN)) {
            double x0 = json_number_value(json_array_get(v0, 0));
            double y0 = json_number_value(json_array_get(v0, 1));
            double xN = json_number_value(json_array_get(vN, 0));
            double yN = json_number_value(json_array_get(vN, 1));
            if (fabs(x0 - xN) < 1e-9 && fabs(y0 - yN) < 1e-9) nv--;
        }
    }
    if (nv < 3) {
        *out_n = 0;
        return NULL;
    }
    double* fp = malloc(nv * 3 * sizeof(double));
    if (!fp) {
        *out_n = 0;
        return NULL;
    }
    for (size_t i = 0; i < nv; i++) {
        json_t* v = json_array_get(ring, i);
        fp[3 * i + 0] = json_number_value(json_array_get(v, 0));
        fp[3 * i + 1] = json_number_value(json_array_get(v, 1));
        fp[3 * i + 2] = ground_z;
    }
    *out_n = (uint32_t)nv;
    return fp;
}

static void footprint_bbox_xy(const double* fp, uint32_t n, double* xmin, double* ymin,
                              double* xmax, double* ymax) {
    *xmin = *ymin = INFINITY;
    *xmax = *ymax = -INFINITY;
    for (uint32_t i = 0; i < n; i++) {
        double x = fp[3 * i];
        double y = fp[3 * i + 1];
        if (x < *xmin) *xmin = x;
        if (y < *ymin) *ymin = y;
        if (x > *xmax) *xmax = x;
        if (y > *ymax) *ymax = y;
    }
}

// Pack one record. `idx_buf` and `idx_count` describe the surviving
// point indices; `footprint` and `n_verts` describe the ring (3*N
// doubles, L93); `bbox6` is the L93 bbox; `ground_z` is the L93
// altitude of the ground plane (= min-Z of inlier points).
// Returns malloc'd bytes + len in `out`. Caller frees `out->bytes`.
static int pack_record(const double* footprint, uint32_t n_verts,
                       const uint32_t* idx_buf, uint32_t idx_count,
                       const double bbox6[6], double ground_z,
                       ring_record_buf_t* out) {
    size_t fp_bytes = (size_t)n_verts * 3 * sizeof(double);
    size_t idx_bytes = (size_t)idx_count * sizeof(uint32_t);
    size_t total = HEADER_BYTES + fp_bytes + idx_bytes;
    uint8_t* buf = malloc(total);
    if (!buf) return -1;
    uint32_t schema_version = 1;
    memcpy(buf + 0, &schema_version, 4);
    memcpy(buf + 4, &n_verts, 4);
    memcpy(buf + 8, &idx_count, 4);
    memcpy(buf + 12, bbox6, 6 * sizeof(double));
    memcpy(buf + 60, &ground_z, sizeof(double));
    memcpy(buf + 68, footprint, fp_bytes);
    memcpy(buf + 68 + fp_bytes, idx_buf, idx_bytes);
    out->bytes = buf;
    out->len = total;
    return 0;
}

// ---------------------------------------------------------------------------
// Main exec
// ---------------------------------------------------------------------------

static int exec_fn(step_context_t* ctx) {
    extract_params_t params;
    parse_params(step_get_params(ctx), &params);

    // --- load context (canonical-location bbox is currently unused; kept
    // as a read so the step's identity hash invalidates when the disc
    // moves and so that no candidate downstream step finds it missing).
    loc_info_t loc;
    if (read_loc(ctx, &loc) != 0) return -1;
    (void)loc;

    // --- load lidar positions (f64 x,y,z interleaved, raw L93 / altitude)
    size_t pos_bytes = 0;
    const double* positions = (const double*)step_read(ctx, "lidar-positions", &pos_bytes);
    if (!positions || pos_bytes < sizeof(double) * 3) return -1;
    size_t n_points = pos_bytes / (3 * sizeof(double));

    // --- parse cadastre-buildings-geojson (FeatureCollection)
    size_t cad_bytes = 0;
    const char* cad_raw = (const char*)step_read(ctx, "cadastre-buildings-geojson", &cad_bytes);
    if (!cad_raw) return -1;
    json_error_t je;
    json_t* root = json_loadb(cad_raw, cad_bytes, 0, &je);
    if (!root) return -1;
    json_t* features = json_object_get(root, "features");
    if (!json_is_array(features)) {
        json_decref(root);
        return -1;
    }

    // --- accumulate surviving records
    ring_record_buf_t* records = NULL;
    size_t n_records = 0, cap_records = 0;
    // Per-record building-local-frames payload: 9 × F64 = 72 bytes per
    // record (centroid xyz + bbox_min xyz + bbox_max xyz). Fixed-stride
    // ARRAY_INLINE — same length as `records`.
    double* frames = NULL;

    size_t nf = json_array_size(features);
    for (size_t fi = 0; fi < nf; fi++) {
        json_t* f = json_array_get(features, fi);
        if (!json_is_object(f)) continue;
        json_t* geom = json_object_get(f, "geometry");
        if (!json_is_object(geom)) continue;
        const char* gtype = json_string_value(json_object_get(geom, "type"));
        json_t* coords = json_object_get(geom, "coordinates");
        if (!gtype || !json_is_array(coords)) continue;

        // Outer rings to walk. Polygon → coords[0]; MultiPolygon → coords[i][0].
        size_t n_rings = 0;
        json_t* rings[64];
        if (strcmp(gtype, "Polygon") == 0) {
            json_t* r = json_array_get(coords, 0);
            if (json_is_array(r) && n_rings < 64) rings[n_rings++] = r;
        } else if (strcmp(gtype, "MultiPolygon") == 0) {
            size_t np = json_array_size(coords);
            for (size_t pi = 0; pi < np && n_rings < 64; pi++) {
                json_t* poly = json_array_get(coords, pi);
                if (!json_is_array(poly)) continue;
                json_t* r = json_array_get(poly, 0);
                if (json_is_array(r)) rings[n_rings++] = r;
            }
        }

        for (size_t ri = 0; ri < n_rings; ri++) {
            json_t* ring = rings[ri];

            // Footprint in raw L93 with provisional z=0 — patched
            // to the inlier min-z once the PIP scan completes.
            uint32_t n_verts = 0;
            double* footprint = footprint_doubles(ring, 0.0, &n_verts);
            if (!footprint || n_verts < 3) {
                free(footprint);
                continue;
            }

            // Footprint xy bbox (L93) — used to compute the expanded
            // prefilter window (footprint + buffer_m on every side).
            double fxmin, fymin, fxmax, fymax;
            footprint_bbox_xy(footprint, n_verts, &fxmin, &fymin, &fxmax, &fymax);
            const double bxmin = fxmin - params.buffer_m;
            const double bymin = fymin - params.buffer_m;
            const double bxmax = fxmax + params.buffer_m;
            const double bymax = fymax + params.buffer_m;
            const double buffer2 = params.buffer_m * params.buffer_m;

            // Walk all points. Accept iff PIP-inside OR within buffer_m of
            // any footprint edge. Track collected-point xy bbox so the
            // record carries the actual extent of the included cloud
            // (which can exceed the footprint bbox by buffer_m).
            uint32_t* idx_buf = NULL;
            size_t idx_cap = 0;
            uint32_t idx_count = 0;
            double zmin = INFINITY, zmax = -INFINITY;
            double pt_xmin = INFINITY, pt_ymin = INFINITY;
            double pt_xmax = -INFINITY, pt_ymax = -INFINITY;
            // Centroid accumulator → emitted in building-local-frames
            // alongside the bbox (Phase 5.1c). Used by every downstream v7
            // step that does building-local geometry (kd-tree build,
            // coordinate-frame shift before F32 cast, plane equation
            // re-translation).
            double cx_sum = 0.0, cy_sum = 0.0, cz_sum = 0.0;

            for (size_t pi = 0; pi < n_points; pi++) {
                double px = positions[3 * pi];
                double py = positions[3 * pi + 1];
                if (px < bxmin || px > bxmax || py < bymin || py > bymax) continue;
                bool inside = point_in_poly(footprint, n_verts, px, py);
                if (!inside) {
                    // Boundary-proximity test. The xy bbox prefilter
                    // already cut points that can't be within buffer_m of
                    // the footprint, but the bbox is rectangular and the
                    // buffer is round — the per-edge distance test is
                    // what makes "5 m around the polygon" precise.
                    if (dist_to_polygon_xy_squared(footprint, n_verts, px, py)
                        > buffer2) continue;
                }
                if (idx_count == idx_cap) {
                    size_t nc = idx_cap ? idx_cap * 2 : 256;
                    uint32_t* nw = realloc(idx_buf, nc * sizeof(uint32_t));
                    if (!nw) {
                        free(idx_buf);
                        free(footprint);
                        idx_buf = NULL;
                        break;
                    }
                    idx_buf = nw;
                    idx_cap = nc;
                }
                idx_buf[idx_count++] = (uint32_t)pi;
                double pz = positions[3 * pi + 2];
                if (pz < zmin) zmin = pz;
                if (pz > zmax) zmax = pz;
                if (px < pt_xmin) pt_xmin = px;
                if (py < pt_ymin) pt_ymin = py;
                if (px > pt_xmax) pt_xmax = px;
                if (py > pt_ymax) pt_ymax = py;
                cx_sum += px;
                cy_sum += py;
                cz_sum += pz;
            }
            if (!idx_buf) continue; // OOM during walk → drop ring, keep going
            if (idx_count < params.min_points) {
                free(idx_buf);
                free(footprint);
                continue;
            }

            // ground_z = min-z of collected points. With the buffer this
            // typically includes some street/sidewalk hits near the
            // building, which is closer to the true ground than min-z of
            // strict-PIP would have given (roof points overhang the
            // walls; strict-PIP would miss the wall-base hits).
            double ground_z = zmin;
            for (uint32_t i = 0; i < n_verts; i++) footprint[3 * i + 2] = ground_z;

            // bbox xy reflects the actual collected-point extent (may
            // exceed the footprint bbox by up to buffer_m on each side
            // due to the boundary-proximity points).
            double bbox6[6];
            bbox6[0] = pt_xmin;
            bbox6[1] = pt_ymin;
            bbox6[2] = zmin;
            bbox6[3] = pt_xmax;
            bbox6[4] = pt_ymax;
            bbox6[5] = zmax;

            // Pack record.
            if (n_records == cap_records) {
                size_t nc = cap_records ? cap_records * 2 : 16;
                ring_record_buf_t* nw = realloc(records, nc * sizeof(ring_record_buf_t));
                double* nfr = realloc(frames, nc * 9 * sizeof(double));
                if (!nw || !nfr) {
                    free(nw); free(nfr);
                    free(idx_buf);
                    free(footprint);
                    goto out_fail;
                }
                records = nw;
                frames = nfr;
                cap_records = nc;
            }
            if (pack_record(footprint, n_verts, idx_buf, idx_count, bbox6, ground_z,
                            &records[n_records]) != 0) {
                free(idx_buf);
                free(footprint);
                goto out_fail;
            }
            // Stash centroid + bbox in the parallel frames buffer.
            double inv = 1.0 / (double)idx_count;
            frames[n_records * 9 + 0] = cx_sum * inv;
            frames[n_records * 9 + 1] = cy_sum * inv;
            frames[n_records * 9 + 2] = cz_sum * inv;
            frames[n_records * 9 + 3] = bbox6[0]; // xmin
            frames[n_records * 9 + 4] = bbox6[1]; // ymin
            frames[n_records * 9 + 5] = bbox6[2]; // zmin
            frames[n_records * 9 + 6] = bbox6[3]; // xmax
            frames[n_records * 9 + 7] = bbox6[4]; // ymax
            frames[n_records * 9 + 8] = bbox6[5]; // zmax
            n_records++;

            free(idx_buf);
            free(footprint);
        }
    }
    json_decref(root);

    // --- pack the array-inline blob via blob_push (8-byte-aligned framing).
    if (n_records == 0) {
        // Empty array is a valid output. step_write_array_inline accepts
        // total_len=0, n=0, stride=0. Emit both artifacts as zero-length
        // arrays so downstream sees consistent per-building counts.
        free(records);
        free(frames);
        if (step_write_array_inline(ctx, "per-building-records",
                                     NULL, 0, 0, 0) != 0) return -1;
        return step_write_array_inline(ctx, "building-local-frames",
                                        NULL, 0, 0, 0);
    }
    blob_t b = {0};
    int rc = 0;
    size_t i = 0;
    for (; i < n_records; i++) {
        if (blob_push(&b, records[i].bytes, records[i].len) != 0) {
            rc = -1;
            break;
        }
        free(records[i].bytes);
    }
    // If blob_push failed at index `i`, records[i..n) still own their bytes.
    if (rc != 0) {
        for (size_t k = i; k < n_records; k++) free(records[k].bytes);
    }
    free(records);

    if (rc == 0) {
        rc = step_write_array_inline(ctx, "per-building-records",
                                     b.bytes, b.len, b.n_records, 0);
    }
    blob_free(&b);
    if (rc != 0) { free(frames); return rc; }

    // building-local-frames: fixed-stride 72 B/record, packed back-to-back.
    rc = step_write_array_inline(ctx, "building-local-frames",
                                  (const uint8_t*)frames,
                                  n_records * 9 * sizeof(double),
                                  n_records,
                                  9 * sizeof(double));
    free(frames);
    return rc;

out_fail:
    if (records) {
        for (size_t k = 0; k < n_records; k++) free(records[k].bytes);
        free(records);
    }
    free(frames);
    return -1;
}

// ---------------------------------------------------------------------------
// Step def
// ---------------------------------------------------------------------------

static const step_read_t reads_arr[] = {
    {.name = "cadastre-buildings-geojson"},
    {.name = "lidar-positions"},
    {.name = "canonical-location"},
};
static const step_write_t writes_arr[] = {
    {.name = "per-building-records",
     .kind = AK_ARRAY_INLINE,
     .visibility = STEP_VIS_DEBUG,
     .array = true},
    // Phase 5.1c: building-local frame info — one fixed-stride record
    // per building (centroid xyz + bbox_min xyz + bbox_max xyz, 9 × F64
    // = 72 B). Pulled out of per-building-records so consumers don't
    // need to parse the variable-length record format just to read the
    // centroid. The bbox is duplicated from per-building-records' header
    // (bytes 12..59) on purpose — direct read here vs walking a record
    // schema there.
    {.name = "building-local-frames",
     .kind = AK_ARRAY_INLINE,
     .visibility = STEP_VIS_PUBLIC,
     .array = true},
};

const step_def_t extract_buildings_def = {
    .name = "extract-buildings",
    // v2: collected set is now PIP-inside ∪ within-buffer_m-of-edge,
    //     not strict-PIP. Bumped to invalidate v1 cache entries so a
    //     warm cache doesn't serve the old (smaller) point set.
    // v3: emits building-local-frames (Phase 5.1c). Bytes of
    //     per-building-records are unchanged; the version bump is
    //     mandatory because the writes set changed.
    .version = 4, // 5.1d: ARRAY_INLINE 8-byte aligned format
    .reads = reads_arr,
    .n_reads = sizeof(reads_arr) / sizeof(reads_arr[0]),
    .writes = writes_arr,
    .n_writes = sizeof(writes_arr) / sizeof(writes_arr[0]),
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};

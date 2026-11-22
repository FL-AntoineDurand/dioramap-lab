// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-load-real-test-case
// Reads:  pipeline-input
// Params: { "spec": "/abs/or/relative/path/to/real-foo.json" }
// Writes: lidar-positions             (F64×3N, L93 metres)
//         lidar-classifications       (U8×N)
//         scan-angles                 (F32×N, optional — flag 0)
//         point-source-ids            (U16×N, optional — flag 1; v7-grow-
//                                       refit-merge consumes it for per-
//                                       strip refit, beam-incidence will
//                                       too)
//         return-numbers              (U8×N, optional — flag 2; v7-detect-
//                                       boundary uses it to flag last-
//                                       return points)
//         number-of-returns           (U8×N, optional — flag 3; same)
//         cadastre-buildings-geojson  (FeatureCollection, single Polygon)
//         canonical-location          (JSON: {bbox_l93, lat, lon, ...})
//
// Bridge from bench-v2's "real" test-case files into the v3 step contract.
// Pairs the v2 `.json` spec (footprint, l93_center, metadata) with the
// `.bin` sidecar (TCBD format — see lidar-algos-bench-v2/lib/test-case-binary.js).
//
// Coordinate translation: v2's binary stores positions in a **local
// three.js frame** centred on `metadata.l93_center`, with a deliberate
// horizontal-axis flip:
//
//     v2.bin.x =  LAZ.x - center.x      (local easting)
//     v2.bin.y =  LAZ.z                  (altitude, raw)
//     v2.bin.z = -(LAZ.y - center.y)    (NEGATED local northing)
//
// (Source: lidar-algos-bench-v2/lib/real-buildings/building-importer.js,
//  `extractLidarPoints` — `positions.push(x - center.x, z, -(y - center.y))`.)
//
// Footprint vertices in the spec follow the same rule:
//     v2.spec.footprint[i].x =  L93.x - center.x
//     v2.spec.footprint[i].z = -(L93.y - center.y)
//
// We invert this on read so v3 sees standard LAZ-frame artefacts:
//     LAZ.x = v2.x + center.x
//     LAZ.y = -v2.z + center.y
//     LAZ.z = v2.y
//
// downstream extract-buildings, v7-tile-signals, etc. consume the LAZ
// frame just as they would after the real fetcher chain (canonicalize-
// location → fetch-copc → laz-extract-disc), with no v2-specific
// special-casing.

#define _POSIX_C_SOURCE 200809L
#include <jansson.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v3/artifact.h"
#include "v3/step.h"

#include "v7-steps.h"

// ── TCBD binary parser ────────────────────────────────────────────────

static const uint16_t TCBD_VERSION = 2;
static const size_t   TCBD_HEADER  = 16;

// Flag bits — bench-v2 lib/test-case-binary.js writeTestCaseBinary.
#define TCBD_FLAG_SCAN_ANGLES      0x01
#define TCBD_FLAG_POINT_SOURCE_IDS 0x02
#define TCBD_FLAG_RETURN_NUMBERS   0x04
#define TCBD_FLAG_NUMBER_OF_RETURNS 0x08
#define TCBD_FLAG_POINT_PLANE_IDS  0x10  /* variable-length (string table) */
#define TCBD_FLAG_INTENSITIES      0x20

typedef struct tcbd {
    uint32_t point_count;
    uint16_t flags;
    const float*    positions_local;     // Float32×3N — points into mmap'd buffer
    const uint8_t*  classifications;     // U8×N
    const float*    scan_angles;         // F32×N or NULL    (flag 0)
    const uint16_t* point_source_ids;    // U16×N or NULL    (flag 1)
    const uint8_t*  return_numbers;      // U8×N  or NULL    (flag 2)
    const uint8_t*  number_of_returns;   // U8×N  or NULL    (flag 3)
    // pointPlaneIds (flag 4) and intensities (flag 5) are skipped past
    // but not surfaced — extend here when a downstream step needs them.
} tcbd_t;

static int tcbd_parse(const uint8_t* buf, size_t len, tcbd_t* out) {
    if (len < TCBD_HEADER) return -1;
    if (memcmp(buf, "TCBD", 4) != 0) return -1;
    uint16_t v;
    memcpy(&v, buf + 4, 2);
    if (v != TCBD_VERSION) return -1;
    memcpy(&out->point_count, buf + 6, 4);
    memcpy(&out->flags,       buf + 10, 2);
    out->scan_angles       = NULL;
    out->point_source_ids  = NULL;
    out->return_numbers    = NULL;
    out->number_of_returns = NULL;
    size_t off = TCBD_HEADER;

    const size_t n = (size_t)out->point_count;
    const size_t pos_bytes = n * 3 * sizeof(float);
    if (off + pos_bytes > len) return -1;
    out->positions_local = (const float*)(buf + off);
    off += pos_bytes;
    if (off + n > len) return -1;
    out->classifications = buf + off;
    off += n;

    // Optional columns appear in fixed order: scanAngles, pointSourceIds,
    // returnNumbers, numberOfReturns, pointPlaneIds, intensities.
    if (out->flags & TCBD_FLAG_SCAN_ANGLES) {
        const size_t b = n * sizeof(float);
        if (off + b > len) return -1;
        out->scan_angles = (const float*)(buf + off);
        off += b;
    }
    if (out->flags & TCBD_FLAG_POINT_SOURCE_IDS) {
        const size_t b = n * sizeof(uint16_t);
        if (off + b > len) return -1;
        out->point_source_ids = (const uint16_t*)(buf + off);
        off += b;
    }
    if (out->flags & TCBD_FLAG_RETURN_NUMBERS) {
        if (off + n > len) return -1;
        out->return_numbers = buf + off;
        off += n;
    }
    if (out->flags & TCBD_FLAG_NUMBER_OF_RETURNS) {
        if (off + n > len) return -1;
        out->number_of_returns = buf + off;
        off += n;
    }
    if (out->flags & TCBD_FLAG_POINT_PLANE_IDS) {
        // [u32 totalBytes][u32 stringCount][u16 len + utf8]...[u16×n indices]
        if (off + 4 > len) return -1;
        uint32_t st_total;
        memcpy(&st_total, buf + off, 4);
        // Indices follow the string table (which is `st_total` bytes long).
        const size_t b = (size_t)st_total + n * sizeof(uint16_t);
        if (off + b > len) return -1;
        off += b;
    }
    if (out->flags & TCBD_FLAG_INTENSITIES) {
        const size_t b = n * sizeof(uint16_t);
        if (off + b > len) return -1;
        off += b;
    }
    return 0;
}

// ── slurp a small file ────────────────────────────────────────────────

static int slurp(const char* path, uint8_t** out_bytes, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    uint8_t* buf = (uint8_t*)malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    if ((long)fread(buf, 1, (size_t)sz, f) != sz) {
        fclose(f); free(buf); return -1;
    }
    fclose(f);
    *out_bytes = buf;
    *out_len = (size_t)sz;
    return 0;
}

// Resolve `dataFile` from spec relative to the spec's own directory.
static char* resolve_data_path(const char* spec_path, const char* data_file) {
    if (data_file[0] == '/') return strdup(data_file);
    const char* slash = strrchr(spec_path, '/');
    if (!slash) return strdup(data_file);
    size_t dir_len = (size_t)(slash - spec_path) + 1; // include trailing '/'
    size_t df_len = strlen(data_file);
    char* out = (char*)malloc(dir_len + df_len + 1);
    if (!out) return NULL;
    memcpy(out, spec_path, dir_len);
    memcpy(out + dir_len, data_file, df_len + 1);
    return out;
}

// ── exec ──────────────────────────────────────────────────────────────

static int exec_fn(step_context_t* ctx) {
    const char* params_str = step_get_params(ctx);
    if (!params_str) {
        fprintf(stderr, "[v7-load-real-test-case] missing params\n");
        return -1;
    }
    json_error_t je;
    json_t* params = json_loads(params_str, 0, &je);
    if (!params) {
        fprintf(stderr, "[v7-load-real-test-case] params not JSON: %s\n", je.text);
        return -1;
    }
    const char* spec_path =
        json_string_value(json_object_get(params, "spec"));
    if (!spec_path) {
        fprintf(stderr, "[v7-load-real-test-case] params.spec missing\n");
        json_decref(params);
        return -1;
    }
    char* spec_path_owned = strdup(spec_path); // params object is freed below

    // Read the spec.
    uint8_t* spec_bytes = NULL;
    size_t spec_len = 0;
    if (slurp(spec_path_owned, &spec_bytes, &spec_len) != 0) {
        fprintf(stderr, "[v7-load-real-test-case] cannot read spec %s\n", spec_path_owned);
        free(spec_path_owned); json_decref(params);
        return -1;
    }
    json_t* spec = json_loadb((const char*)spec_bytes, spec_len, 0, &je);
    free(spec_bytes);
    if (!spec) {
        fprintf(stderr, "[v7-load-real-test-case] spec not JSON: %s\n", je.text);
        free(spec_path_owned); json_decref(params);
        return -1;
    }

    // type must be "real". Synthetic regeneration is out of scope for the C
    // loader — bake them via the bench-v2 dump CLI if needed.
    const char* type = json_string_value(json_object_get(spec, "type"));
    if (!type || strcmp(type, "real") != 0) {
        fprintf(stderr, "[v7-load-real-test-case] spec type=%s, only 'real' supported\n",
                type ? type : "(missing)");
        json_decref(spec); free(spec_path_owned); json_decref(params);
        return -1;
    }

    // l93 center.
    json_t* meta = json_object_get(spec, "metadata");
    json_t* center = meta ? json_object_get(meta, "l93_center") : NULL;
    if (!json_is_object(center)) {
        fprintf(stderr, "[v7-load-real-test-case] missing metadata.l93_center\n");
        json_decref(spec); free(spec_path_owned); json_decref(params);
        return -1;
    }
    double cx = json_number_value(json_object_get(center, "x"));
    double cy = json_number_value(json_object_get(center, "y"));

    // dataFile (relative to spec dir).
    const char* data_file = json_string_value(json_object_get(spec, "dataFile"));
    if (!data_file) {
        fprintf(stderr, "[v7-load-real-test-case] missing dataFile\n");
        json_decref(spec); free(spec_path_owned); json_decref(params);
        return -1;
    }
    char* data_path = resolve_data_path(spec_path_owned, data_file);
    free(spec_path_owned);
    if (!data_path) {
        json_decref(spec); json_decref(params);
        return -1;
    }

    // Read TCBD binary.
    uint8_t* bin_bytes = NULL;
    size_t bin_len = 0;
    if (slurp(data_path, &bin_bytes, &bin_len) != 0) {
        fprintf(stderr, "[v7-load-real-test-case] cannot read %s\n", data_path);
        free(data_path); json_decref(spec); json_decref(params);
        return -1;
    }
    free(data_path);

    tcbd_t tc;
    if (tcbd_parse(bin_bytes, bin_len, &tc) != 0) {
        fprintf(stderr, "[v7-load-real-test-case] bad TCBD\n");
        free(bin_bytes); json_decref(spec); json_decref(params);
        return -1;
    }
    size_t n = tc.point_count;

    // Convert positions: v2.local → LAZ (L93 + altitude). See header comment.
    double* xyz = (double*)malloc(n * 3 * sizeof(double));
    if (!xyz) {
        free(bin_bytes); json_decref(spec); json_decref(params);
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        double vx = (double)tc.positions_local[3 * i + 0];
        double vy = (double)tc.positions_local[3 * i + 1];
        double vz = (double)tc.positions_local[3 * i + 2];
        xyz[3 * i + 0] = vx + cx;       // LAZ.x = v2.x + center.x
        xyz[3 * i + 1] = -vz + cy;      // LAZ.y = -v2.z + center.y
        xyz[3 * i + 2] = vy;            // LAZ.z = v2.y
    }
    int rc = step_write_output(ctx, "lidar-positions", xyz, n * 3 * sizeof(double));
    free(xyz);
    if (rc != 0) {
        free(bin_bytes); json_decref(spec); json_decref(params);
        return -1;
    }
    rc = step_write_output(ctx, "lidar-classifications", tc.classifications, n);
    if (rc != 0) {
        free(bin_bytes); json_decref(spec); json_decref(params);
        return -1;
    }
    // Emit optional TCBD columns when present. Consumers downstream
    // (v7-grow-refit-merge, v7-detect-boundary, v7-beam-incidence) handle
    // missing columns gracefully; v3 just doesn't emit them when the
    // sidecar lacks the corresponding flag bit.
    if (tc.scan_angles) {
        rc = step_write_output(ctx, "scan-angles",
                                tc.scan_angles, n * sizeof(float));
        if (rc != 0) goto bin_fail;
    }
    if (tc.point_source_ids) {
        rc = step_write_output(ctx, "point-source-ids",
                                tc.point_source_ids, n * sizeof(uint16_t));
        if (rc != 0) goto bin_fail;
    }
    if (tc.return_numbers) {
        rc = step_write_output(ctx, "return-numbers",
                                tc.return_numbers, n);
        if (rc != 0) goto bin_fail;
    }
    if (tc.number_of_returns) {
        rc = step_write_output(ctx, "number-of-returns",
                                tc.number_of_returns, n);
        if (rc != 0) goto bin_fail;
    }
    free(bin_bytes); // tc.* point into bin_bytes — done with both now.
    goto bin_ok;
bin_fail:
    free(bin_bytes); json_decref(spec); json_decref(params);
    return -1;
bin_ok:
    (void)0;

    // Build cadastre-buildings-geojson: a FeatureCollection with one
    // Polygon. Outer ring = the spec's footprint, inverse-transformed
    // back to L93. Z is forced to 0 — extract-buildings only consumes
    // x,y (it patches z to the inlier min-z post-PIP).
    json_t* footprint = json_object_get(spec, "footprint");
    if (!json_is_array(footprint) || json_array_size(footprint) < 3) {
        fprintf(stderr, "[v7-load-real-test-case] footprint missing or too small\n");
        json_decref(spec); json_decref(params);
        return -1;
    }
    json_t* outer_ring = json_array();
    size_t nf = json_array_size(footprint);
    double xmin = INFINITY, ymin = INFINITY, xmax = -INFINITY, ymax = -INFINITY;
    for (size_t i = 0; i < nf; i++) {
        json_t* v = json_array_get(footprint, i);
        if (!json_is_object(v)) continue;
        double vx = json_number_value(json_object_get(v, "x"));
        double vz = json_number_value(json_object_get(v, "z"));
        // Inverse of the building-importer's footprint local transform.
        double lx = vx + cx;
        double ly = -vz + cy;
        if (lx < xmin) xmin = lx;
        if (lx > xmax) xmax = lx;
        if (ly < ymin) ymin = ly;
        if (ly > ymax) ymax = ly;
        json_t* xy = json_array();
        json_array_append_new(xy, json_real(lx));
        json_array_append_new(xy, json_real(ly));
        json_array_append_new(outer_ring, xy);
    }
    json_t* coords = json_array();
    json_array_append_new(coords, outer_ring);
    json_t* geom = json_object();
    json_object_set_new(geom, "type", json_string("Polygon"));
    json_object_set_new(geom, "coordinates", coords);
    json_t* feat = json_object();
    json_object_set_new(feat, "type", json_string("Feature"));
    json_object_set_new(feat, "geometry", geom);
    json_object_set_new(feat, "properties", json_object());
    json_t* features = json_array();
    json_array_append_new(features, feat);
    json_t* fc = json_object();
    json_object_set_new(fc, "type", json_string("FeatureCollection"));
    json_object_set_new(fc, "features", features);
    char* fc_str = json_dumps(fc, JSON_COMPACT);
    json_decref(fc);
    if (!fc_str) {
        json_decref(spec); json_decref(params);
        return -1;
    }
    rc = step_write_output(ctx, "cadastre-buildings-geojson",
                           fc_str, strlen(fc_str));
    free(fc_str);
    if (rc != 0) {
        json_decref(spec); json_decref(params);
        return -1;
    }

    // canonical-location: bbox padded by 5 m so extract-buildings'
    // bbox-derived prefilter window stays sensible.
    json_t* lat_j = meta ? json_object_get(meta, "lat") : NULL;
    json_t* lng_j = meta ? json_object_get(meta, "lng") : NULL;
    double lat = json_is_number(lat_j) ? json_number_value(lat_j) : 0.0;
    double lng = json_is_number(lng_j) ? json_number_value(lng_j) : 0.0;
    char buf[1024];
    int nb = snprintf(buf, sizeof(buf),
                      "{\"lat\":%.7f,\"lon\":%.7f,\"radius_m\":%d,"
                      "\"center_l93_x\":%.3f,\"center_l93_y\":%.3f,"
                      "\"bbox_l93\":[%.3f,%.3f,%.3f,%.3f],"
                      "\"cell_size_m\":%d,"
                      "\"source\":\"v7-load-real-test-case\"}",
                      lat, lng, 50, cx, cy,
                      xmin - 5.0, ymin - 5.0, xmax + 5.0, ymax + 5.0, 1);
    if (nb < 0 || (size_t)nb >= sizeof(buf)) {
        json_decref(spec); json_decref(params);
        return -1;
    }
    rc = step_write_output(ctx, "canonical-location", buf, (size_t)nb);

    json_decref(spec);
    json_decref(params);
    return rc;
}

// No reads — the step loads from `params.spec` (a file path), not from
// the pipeline-input artifact. Previously declared PIPELINE_INPUT_ARTIFACT
// as a read but the exec never called step_read for it; the declaration
// only served to pull the input's content hash into the chain hash,
// which made every UI run miss cache when the textarea's `input` JSON
// differed from batch's literal `'{}'` (e.g. a localStorage-saved value
// with a trailing newline, or fields persisted from another pipeline).
// Whole cascade then re-ran because each downstream step's reads
// include this step's writes, which got fresh content hashes per miss.
static const step_write_t writes_arr[] = {
    {.name = "lidar-positions",
     .kind = AK_POINT_COLUMN,           .visibility = STEP_VIS_PUBLIC},
    {.name = "lidar-classifications",
     .kind = AK_POINT_COLUMN,           .visibility = STEP_VIS_PUBLIC},
    {.name = "scan-angles",
     .kind = AK_POINT_COLUMN,           .visibility = STEP_VIS_DEBUG},
    {.name = "point-source-ids",
     .kind = AK_POINT_COLUMN,           .visibility = STEP_VIS_DEBUG},
    {.name = "return-numbers",
     .kind = AK_POINT_COLUMN,           .visibility = STEP_VIS_DEBUG},
    {.name = "number-of-returns",
     .kind = AK_POINT_COLUMN,           .visibility = STEP_VIS_DEBUG},
    {.name = "cadastre-buildings-geojson",
     .kind = AK_OPAQUE_JSON,            .visibility = STEP_VIS_PUBLIC},
    {.name = "canonical-location",
     .kind = AK_OPAQUE_JSON,            .visibility = STEP_VIS_PUBLIC},
};

const step_def_t v7_load_real_test_case_def = {
    .name = "v7-load-real-test-case",
    .version = 4,  // v4: invalidate stale 12B-header cache; v3: also emit scan-angles, return-numbers, number-of-returns
    .reads = NULL,
    .n_reads = 0,
    .writes = writes_arr,
    .n_writes = sizeof(writes_arr) / sizeof(writes_arr[0]),
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};

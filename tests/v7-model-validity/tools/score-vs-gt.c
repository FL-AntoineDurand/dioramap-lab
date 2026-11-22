// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v3/tests/v7-model-validity/tools/score-vs-gt.c
//
// Compare a v3 v7 pipeline output against a bench-v2 analytic ground
// truth JSON. Reuses v3's own on-wire reader helpers so the metric
// stays in lock-step with the pipeline — no drift from parallel-
// language reimplementation (which was compare.py's failure mode).
//
// Usage:
//   score-vs-gt <roof-graph.bin> <ground-truth.json>
//   score-vs-gt <roof-graph.bin> <ground-truth.json> --wall-tilt-min <deg>
//
// Emits one JSON object on stdout with per-metric results:
//   {
//     "case_id": "...", "roof_type": "...",
//     "wall_tilt_min_deg": 72,
//     "metrics": [
//       {"metric": "roof_count_match",  "expected": 1, "actual": 2, "passed": false},
//       {"metric": "wall_count_match",  "expected": 4, "actual": 2, "passed": false},
//       {"metric": "plane_count_total_match", "expected": 5, "actual": 4, "passed": false}
//     ],
//     "passed": false
//   }
// Exits 0 iff every metric passes, 1 otherwise, 2 on argument / IO
// errors.
//
// Roof/wall discrimination reuses the wire-level `rg_plane_is_wall`
// helper (v7-roof-graph-format.h) — the same convention v3's
// boundary-classify and wall-detect apply. The "roof" count here
// currently DOES include horizontal ground planes (v3 emits one for
// the LiDAR ground around the building on synthetic cases with
// `addGround: true`). Filtering ground out cleanly needs a secondary
// pass on centroid z; deferred until we have a case where it changes
// the pass/fail verdict.

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jansson.h>

#include "v7-roof-graph-format.h"

// ─── file slurp ──────────────────────────────────────────────────────────
// fseek/ftell rather than fstat(fileno) — fileno requires _POSIX_C_SOURCE
// and this tool builds strict-ISO. Matches v7-roof-graph-inspect's slurp.
static int slurp(const char* path, uint8_t** out, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "score-vs-gt: open '%s' failed: %s\n", path, strerror(errno));
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long n = ftell(f);
    if (n < 0)                       { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0)  { fclose(f); return -1; }
    uint8_t* buf = (uint8_t*)malloc((size_t)n);
    if (!buf) { fclose(f); return -1; }
    if ((long)fread(buf, 1, (size_t)n, f) != n) { fclose(f); free(buf); return -1; }
    fclose(f);
    *out = buf;
    *out_len = (size_t)n;
    return 0;
}

// ─── PLANES walker ───────────────────────────────────────────────────────
//
// The pipeline emits an ARRAY_INLINE blob: one variable-length record
// per building, each record is a full RGRAPH TLV. Walk both levels
// and accumulate plane counts across buildings.
typedef struct plane_totals {
    size_t n_buildings;
    size_t total_records;
    size_t rejected;
    size_t walls;                // live && plane_type == WALL
    size_t ground;               // live && plane_type == GROUND (NOT counted as roof)
    size_t roof_candidates;      // live && roof/unclassified (ground now excluded)
    size_t live;                 // walls + ground + roof_candidates
} plane_totals_t;

// Iterate the ARRAY_INLINE outer blob (variable-length records; format:
// [u32 n_records][u32 stride=0][records...]; each record = [u32 len][u32
// _pad][len bytes][pad to 8]). Returns the per-record (bytes, len) pairs
// via the callback.
typedef void (*record_cb)(const uint8_t* rec, size_t rec_len, void* ctx);

static int walk_array_inline(const uint8_t* buf, size_t buf_len,
                             record_cb cb, void* ctx) {
    if (buf_len < 8) return -1;
    uint32_t n_records, stride;
    memcpy(&n_records, buf + 0, 4);
    memcpy(&stride,    buf + 4, 4);
    if (stride != 0) {
        fprintf(stderr, "score-vs-gt: ARRAY_INLINE stride=%u (expected 0 for var-len)\n",
                (unsigned)stride);
        return -1;
    }
    size_t off = 8;
    for (uint32_t k = 0; k < n_records; k++) {
        if (off + 8 > buf_len) return -1;
        uint32_t rec_len; memcpy(&rec_len, buf + off, 4);
        off += 8;
        if (off + rec_len > buf_len) return -1;
        cb(buf + off, rec_len, ctx);
        // 8-byte pad
        size_t padded = (rec_len + 7) & ~(size_t)7;
        off += padded;
    }
    return 0;
}

// Callback ctx for per-building record walk.
typedef struct {
    plane_totals_t* totals;
    uint16_t wall_tilt_min_deg;
} bldg_walk_ctx_t;

static void per_bldg_cb(const uint8_t* rec, size_t rec_len, void* vctx) {
    bldg_walk_ctx_t* c = (bldg_walk_ctx_t*)vctx;
    c->totals->n_buildings++;

    // Open the RGRAPH TLV inside this per-bldg record.
    rg_reader_t r;
    if (rg_reader_open(&r, rec, rec_len) != 0) return;
    const uint8_t* sect; size_t slen;
    if (rg_reader_section(&r, RG_SECT_PLANES, &sect, &slen) != 0) return;

    // Fixed-stride PLANES: [u32 n][u32 stride=128][n × 128B records].
    uint32_t nr, stride;
    memcpy(&nr,     sect + 0, 4);
    memcpy(&stride, sect + 4, 4);
    if (stride != sizeof(rg_plane_record_t)) {
        fprintf(stderr, "score-vs-gt: PLANES stride=%u expected %zu\n",
                (unsigned)stride, sizeof(rg_plane_record_t));
        return;
    }
    for (uint32_t k = 0; k < nr; k++) {
        const rg_plane_record_t* p =
            (const rg_plane_record_t*)(sect + 8 + (size_t)k * sizeof(rg_plane_record_t));
        c->totals->total_records++;
        if (p->flags & RG_PLANE_REJECTED) {
            c->totals->rejected++;
            continue;
        }
        c->totals->live++;
        if (rg_plane_is_wall(p)) {
            c->totals->walls++;
        } else if (rg_plane_is_ground(p)) {
            c->totals->ground++;   // ground is not a roof — the miscount this fixes
        } else {
            c->totals->roof_candidates++;
        }
    }
}

// ─── ground truth ────────────────────────────────────────────────────────

typedef struct gt_totals {
    const char* case_id;
    const char* roof_type;
    int roof_plane_count;
    int wall_count;
    int plane_count_total;
} gt_totals_t;

static int parse_gt(const char* path, gt_totals_t* out, uint16_t wall_tilt_min_deg) {
    json_error_t je;
    json_t* root = json_load_file(path, 0, &je);
    if (!root) {
        fprintf(stderr, "score-vs-gt: parse '%s': %s\n", path, je.text);
        return -1;
    }
    out->case_id   = json_string_value(json_object_get(root, "case_id"));
    out->roof_type = json_string_value(json_object_get(root, "roof_type"));
    // These fields carry as JSON integers in the generator's output.
    out->roof_plane_count  = (int)json_integer_value(json_object_get(root, "roof_plane_count"));
    out->plane_count_total = (int)json_integer_value(json_object_get(root, "plane_count_total"));
    // Wall count: derived from the planes[] array in GT — planes with
    // tilt >= wall_tilt_min. If planes[] is absent (older GT), fall back
    // to plane_count_total - roof_plane_count.
    json_t* jplanes = json_object_get(root, "planes");
    int walls = -1;
    if (json_is_array(jplanes)) {
        walls = 0;
        size_t idx; json_t* pj;
        json_array_foreach(jplanes, idx, pj) {
            json_t* jt = json_object_get(pj, "tilt_deg");
            if (json_is_number(jt) && json_number_value(jt) >= (double)wall_tilt_min_deg) {
                walls++;
            }
        }
    } else {
        walls = out->plane_count_total - out->roof_plane_count;
    }
    out->wall_count = walls;
    // Note: we intentionally leak `root` — case_id / roof_type strings
    // point INTO the jansson tree. Freed at process exit via _exit().
    (void)root;
    return 0;
}

// ─── output ─────────────────────────────────────────────────────────────

static void emit_metric(json_t* metrics, const char* name, int expected, int actual) {
    json_t* m = json_object();
    json_object_set_new(m, "metric",   json_string(name));
    json_object_set_new(m, "expected", json_integer(expected));
    json_object_set_new(m, "actual",   json_integer(actual));
    json_object_set_new(m, "passed",   json_boolean(expected == actual));
    json_array_append_new(metrics, m);
}

static bool metric_passed(json_t* metric) {
    json_t* p = json_object_get(metric, "passed");
    return json_is_true(p);
}

// ─── main ───────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr,
                "usage: score-vs-gt <roof-graph.bin> <ground-truth.json> "
                "[--wall-tilt-min <deg>]\n");
        return 2;
    }
    const char* bin_path = argv[1];
    const char* gt_path  = argv[2];
    uint16_t wall_tilt_min = RG_WALL_TILT_MIN_DEG_DEFAULT;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--wall-tilt-min") == 0 && i + 1 < argc) {
            long v = strtol(argv[i + 1], NULL, 10);
            if (v < 0 || v > 90) {
                fprintf(stderr, "score-vs-gt: --wall-tilt-min out of range 0..90\n");
                return 2;
            }
            wall_tilt_min = (uint16_t)v;
            i++;
        } else {
            fprintf(stderr, "score-vs-gt: unknown arg '%s'\n", argv[i]);
            return 2;
        }
    }

    uint8_t* buf = NULL; size_t buf_len = 0;
    if (slurp(bin_path, &buf, &buf_len) != 0) return 2;

    plane_totals_t totals = {0};
    bldg_walk_ctx_t wctx = { .totals = &totals, .wall_tilt_min_deg = wall_tilt_min };
    if (walk_array_inline(buf, buf_len, per_bldg_cb, &wctx) != 0) {
        fprintf(stderr, "score-vs-gt: ARRAY_INLINE walk failed on '%s'\n", bin_path);
        free(buf);
        return 2;
    }
    free(buf);

    gt_totals_t gt = {0};
    if (parse_gt(gt_path, &gt, wall_tilt_min) != 0) return 2;

    // Cast to int matches GT JSON's integer type; totals stay well under INT_MAX.
    int actual_roof = (int)totals.roof_candidates;
    int actual_wall = (int)totals.walls;
    int actual_all  = (int)totals.live;

    json_t* metrics = json_array();
    emit_metric(metrics, "roof_count_match",        gt.roof_plane_count,  actual_roof);
    emit_metric(metrics, "wall_count_match",        gt.wall_count,        actual_wall);
    emit_metric(metrics, "plane_count_total_match", gt.plane_count_total, actual_all);

    bool all_pass = true;
    size_t idx; json_t* m;
    json_array_foreach(metrics, idx, m) {
        if (!metric_passed(m)) { all_pass = false; break; }
    }

    json_t* root = json_object();
    json_object_set_new(root, "case_id",           json_string(gt.case_id ? gt.case_id : "unknown"));
    json_object_set_new(root, "roof_type",         json_string(gt.roof_type ? gt.roof_type : "unknown"));
    json_object_set_new(root, "wall_tilt_min_deg", json_integer(wall_tilt_min));
    json_object_set_new(root, "n_buildings",       json_integer((int)totals.n_buildings));
    json_object_set_new(root, "metrics",           metrics);
    json_object_set_new(root, "passed",            json_boolean(all_pass));

    char* s = json_dumps(root, JSON_COMPACT);
    puts(s);
    free(s);
    json_decref(root);

    return all_pass ? 0 : 1;
}

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-score-intrinsic — step wrapper.
//
// Per-building intrinsic (no-GT) model-quality scoring. Reads a plane set
// + per-point owner assignment + the building's points, computes
// completeness / residual RMS / a composite (rasterisation metrics are a
// follow-up — see impl header), and emits one 72-byte score record per
// building.
//
// This is the in-pipeline half of the hybrid scoring design
// (docs/quality/SCORING-DESIGN.md §6). It is purely additive — it doesn't
// consume any downstream output and writes a leaf artifact, so its
// version bumps never invalidate the rest of the pipeline cache (preserves
// v2's "scorer changes don't invalidate caches" property).
//
// Reads:
//   lidar-positions       (F64×3·N global) — point cloud
//   per-building-records  (per-bldg: header + globalIndices)
//   grm-plane-equations   (per-bldg ARRAY_INLINE of 32-byte plane records;
//                          normal[3] f64 + d f64 — exactly the layout
//                          pack_plane_eq() emits in v7-grow-refit-merge.c)
//   grm-owner             (per-bldg Int16×n_pts, plane-id LOCAL to the
//                          building; -1 = unassigned)
//
// Writes:
//   model-score           (per-bldg ARRAY_INLINE, ONE 72-byte record per
//                          building — fixed layout per
//                          v7_score_intrinsic_pack)
//
// Wiring: hardcoded to grow-refit-merge's outputs today. That is the
// final plane state for the CURRENT v7 pipelines — boundary-topology /
// -signals / -corners / -polygons-2 / -merge / -classify only build /
// refine the roof-graph (corners, edges, polygon paths), they do not
// touch grm-plane-equations or grm-owner.
//
// CAVEAT: v7-wall-detect (declared in v7-steps.h but NOT included in any
// current pipeline) writes wd-owner — a MUTATED grm-owner with wall-
// grabbed points reassigned to their wall plane id — and appends new
// wall planes to the roof-graph. Any pipeline that adds wall-detect
// MUST score against wd-owner (and the augmented plane set), not
// grm-owner. The clean fix when that happens is a sibling step
// (v7-score-intrinsic-postwall) hardcoded to wd-* — or per-step
// artifact-name aliasing in the v3 runtime if/when it grows that,
// which would also enable the SCORING-DESIGN.md §6.2 "wire as we wish,
// multiple times for comparison" lever.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jansson.h>

#include "v3/artifact.h"
#include "v3/blob.h"
#include "v3/safe_alloc.h"
#include "v3/step.h"
#include "v7-score-intrinsic-impl.h"

// ── Per-building plane extraction ──────────────────────────────────
//
// Walks a per-bldg blob_next-framed sequence of plane records and pulls
// out the first 32 bytes (normal[3] + d) of each. Tolerant of trailing
// data (only the equation prefix is load-bearing here). Returns a
// freshly-allocated array via *out_planes (caller frees) and the count
// via *out_n_planes. Returns 0 on success, -1 on alloc failure.
static int extract_plane_equations(const uint8_t* bytes, size_t len,
                                    v7_score_plane_t** out_planes,
                                    size_t* out_n_planes) {
    *out_planes = NULL;
    *out_n_planes = 0;
    if (!bytes || len < 8) return 0;

    // Pass 1: count.
    size_t n = 0;
    {
        size_t cursor = 0;
        const uint8_t* h; size_t rl;
        while (blob_next(bytes, len, &cursor, &h, &rl)) {
            if (rl >= 32) n++;
        }
    }
    if (n == 0) return 0;

    v7_score_plane_t* arr = (v7_score_plane_t*)safe_calloc_array(n, sizeof(v7_score_plane_t));
    if (!arr) return -1;

    // Pass 2: fill.
    size_t fill = 0;
    size_t cursor = 0;
    const uint8_t* h; size_t rl;
    while (blob_next(bytes, len, &cursor, &h, &rl) && fill < n) {
        if (rl < 32) continue;
        // memcpy avoids alignment UB on the inbound buffer.
        memcpy(arr[fill].normal, h + 0, 3 * sizeof(double));
        memcpy(&arr[fill].d,     h + 24,    sizeof(double));
        fill++;
    }

    *out_planes = arr;
    *out_n_planes = fill;
    return 0;
}

// ── exec ──────────────────────────────────────────────────────────

static int v7_score_intrinsic_exec(step_context_t* ctx) {
    // No tunable params yet — the scoring formula's constants (e.g.,
    // V7_SCORE_RMS_REFERENCE) live in the impl and version-bump as a unit.
    // If we expose any later (e.g., a per-pipeline composite-weight
    // override), parse them here via json_*_or_default.
    (void)step_get_params(ctx);

    size_t pos_bytes = 0;
    const double* positions = (const double*)step_read(ctx, "lidar-positions", &pos_bytes);
    if (!positions || pos_bytes % (3 * sizeof(double)) != 0) return -1;
    size_t total_points = pos_bytes / (3 * sizeof(double));

    size_t n_buildings   = step_array_len(ctx, "per-building-records");
    size_t n_planes_arr  = step_array_len(ctx, "grm-plane-equations");
    size_t n_owner_arr   = step_array_len(ctx, "grm-owner");
    if (n_planes_arr != n_buildings || n_owner_arr != n_buildings) {
        fprintf(stderr,
                "[v7-score-intrinsic] count mismatch: bldgs=%zu "
                "planes=%zu owner=%zu\n",
                n_buildings, n_planes_arr, n_owner_arr);
        return -1;
    }

    blob_t b_score = {0};

    int rc = 0;
    double* local_pos = NULL;
    size_t  local_cap = 0;

    for (size_t bi = 0; bi < n_buildings; bi++) {
        // ── per-building-records: gidx + n_pts ──
        // Same parse as v7-normal-cascade-seed.c, kept brief here.
        const uint8_t* rec = NULL;
        size_t rec_len = 0;
        if (step_array_read_bytes(ctx, "per-building-records", bi, &rec, &rec_len) != 0
            || !rec || rec_len < 68) {
            // Emit an empty per-bldg record so the output array indexing
            // stays aligned with the input array indexing.
            blob_push(&b_score, NULL, 0);
            continue;
        }
        uint32_t schema, n_verts, n_pts;
        memcpy(&schema,  rec + 0, 4);
        memcpy(&n_verts, rec + 4, 4);
        memcpy(&n_pts,   rec + 8, 4);
        size_t fp_bytes = (size_t)n_verts * 3 * sizeof(double);
        size_t idx_off = 68 + fp_bytes;
        if (schema != 1 || idx_off + (size_t)n_pts * sizeof(uint32_t) > rec_len) {
            blob_push(&b_score, NULL, 0);
            continue;
        }
        const uint32_t* gidx = (const uint32_t*)(rec + idx_off);

        // ── owner: n_pts × int16 ──
        const uint8_t* owner_bytes = NULL; size_t owner_len = 0;
        if (step_array_read_bytes(ctx, "grm-owner", bi, &owner_bytes, &owner_len) != 0
            || owner_len != (size_t)n_pts * sizeof(int16_t)) {
            blob_push(&b_score, NULL, 0);
            continue;
        }
        const int16_t* owner = (const int16_t*)owner_bytes;

        // ── planes: per-bldg blob of records, extract equations ──
        const uint8_t* planes_bytes = NULL; size_t planes_len = 0;
        if (step_array_read_bytes(ctx, "grm-plane-equations", bi, &planes_bytes, &planes_len) != 0) {
            blob_push(&b_score, NULL, 0);
            continue;
        }
        v7_score_plane_t* planes = NULL; size_t n_planes = 0;
        if (extract_plane_equations(planes_bytes, planes_len, &planes, &n_planes) != 0) {
            rc = -1; break;
        }

        // ── Build per-bldg local F64 positions[n_pts*3] from gidx ──
        if ((size_t)n_pts * 3 > local_cap) {
            double* p64 = (double*)realloc(local_pos, (size_t)n_pts * 3 * sizeof(double));
            if (!p64) { free(planes); rc = -1; break; }
            local_pos = p64;
            local_cap = (size_t)n_pts * 3;
        }
        for (uint32_t i = 0; i < n_pts; i++) {
            uint32_t g = gidx[i];
            if ((size_t)g >= total_points) {
                local_pos[3*i+0] = local_pos[3*i+1] = local_pos[3*i+2] = 0.0;
            } else {
                local_pos[3*i+0] = positions[3*g+0];
                local_pos[3*i+1] = positions[3*g+1];
                local_pos[3*i+2] = positions[3*g+2];
            }
        }

        // ── Compute the score ──
        v7_score_intrinsic_result_t r;
        int crc = v7_score_intrinsic_compute(local_pos, owner, (size_t)n_pts,
                                              planes, n_planes, &r);
        free(planes);
        if (crc != 0) { rc = -1; break; }

        // ── Pack the 72-byte record into the per-bldg blob slot ──
        uint8_t buf[V7_SCORE_INTRINSIC_RECORD_BYTES];
        v7_score_intrinsic_pack(&r, buf);
        if (blob_push(&b_score, buf, sizeof(buf)) != 0) { rc = -1; break; }
    }

    free(local_pos);

    if (rc == 0) rc |= emit_blob(ctx, "model-score", &b_score);
    blob_free(&b_score);
    return rc;
}

// ── Step descriptor ──

static const step_read_t v7_score_intrinsic_reads[] = {
    {.name = "lidar-positions"},
    {.name = "per-building-records", .array = true},
    {.name = "grm-plane-equations",         .array = true},
    {.name = "grm-owner",          .array = true},
};
static const step_write_t v7_score_intrinsic_writes[] = {
    {.name = "model-score",
     .kind = AK_ARRAY_INLINE,
     .visibility = STEP_VIS_PUBLIC,
     .array = true},
};

const step_def_t v7_score_intrinsic_def = {
    .name = "v7-score-intrinsic",
    .version = 1,
    .reads = v7_score_intrinsic_reads,
    .n_reads = sizeof(v7_score_intrinsic_reads) / sizeof(v7_score_intrinsic_reads[0]),
    .writes = v7_score_intrinsic_writes,
    .n_writes = sizeof(v7_score_intrinsic_writes) / sizeof(v7_score_intrinsic_writes[0]),
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = v7_score_intrinsic_exec,
};

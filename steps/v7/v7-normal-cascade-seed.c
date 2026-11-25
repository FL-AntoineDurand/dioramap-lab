// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-normal-cascade-seed — step wrapper.
//
// Reads:
//   lidar-positions       (F64×3·N global)
//   per-building-records  (per-bldg: header + globalIndices)
//   cascade-segment-list  (per-bldg ARRAY_INLINE of segment records, with
//                          LOCAL pt_indices into the building)
//   curvatures-k16        (per-bldg F32×n_pts_in_bldg)
//
// Writes:
//   seed-planes           (per-bldg ARRAY_INLINE of v7_seed_plane_record_t
//                          headers + LOCAL seed_indices[])
//   seed-owner            (per-bldg Int16×n_pts_in_bldg, building-local
//                          plane ids; -1 = unassigned). Renamed from
//                          v2's `owner` so each owner-emitting step
//                          (seed → grm → refit) gets a uniquely-named
//                          artifact: removes the inspector-time
//                          collision between layers, lets each step's
//                          viz file hardcode its own label/path.
//                          Downstream consumers wire `owner` →
//                          `<upstream-step>::<step's owner name>`
//                          (matches v2's `owner` accumulator pattern at
//                          the wiring level rather than the artifact
//                          name level).
//
// Per the audit: drops cascadeExcludeRidges (V7 always false), the
// cascade-segments + ridge-candidates reads (dead in V7), point-weights
// (v7's wiring doesn't propagate it to this step — the impl falls back
// to unweighted sums, which is what byte-validates against v2 dumps),
// the seed-dist-threshold + seed-min-seed-area metadata writes (never
// read downstream), and plane-equations (collapsed into seed-planes
// since the equation prefix is the first 32 bytes of each record).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jansson.h>

#include "v3/artifact.h"
#include "v3/safe_alloc.h"
#include "v3/json_int.h"
#include "v3/blob.h"
#include "v3/step.h"
#include "v7-normal-cascade-seed-impl.h"
#include "v7-perbldg-gather.h"

// blob accumulator (8-aligned variable-length records) is provided by
// <v3/blob.h> (included above) — the local copy was removed in the merge
// with main, which unified the framing across all v7 steps. The HS-2/V7-1
// integer-overflow guard that lived here now lives in blob_reserve() in
// blob.h itself, so every blob user inherits it.

// ── Params ──

static int parse_params(const char* params_str, v7_seed_params_t* p) {
    v7_seed_params_default(p);
    p->up_axis = 2; // v3 LAZ pipeline z-up
    if (!params_str) return 0;
    json_error_t je;
    json_t* obj = json_loads(params_str, 0, &je);
    if (!obj) return -1;
    json_t* j;
    if ((j = json_object_get(obj, "distThreshold")) && json_is_number(j))
        p->dist_threshold = json_number_value(j);
    if ((j = json_object_get(obj, "maxSeedCurvature")) && json_is_number(j))
        p->max_seed_curvature = json_number_value(j);
    if ((j = json_object_get(obj, "wallTiltDeg")) && json_is_number(j))
        p->wall_tilt_deg = json_number_value(j);
    j = json_object_get(obj, "upAxis");
    if (json_int32_or_default(j, 0, 2, "upAxis", &p->up_axis) != 0) { json_decref(obj); return -1; }
    json_decref(obj);
    return 0;
}

// ── Pack one seed plane as a flat record. Layout:
//      f64×3 normal
//      f64    d
//      f64    tilt
//      f64    azimuth
//      f64    rms
//      u32    inlier_count
//      u32    source_segment_id
//    + u32×inlier_count seed_indices (LOCAL to building)
//    Header is 8×8 = 64 bytes (forced 8-aligned via 56→64 with no pad
//    since 6×8 + 2×4 = 56 is itself 8-aligned, but we wire the record
//    as the on-disk v7_seed_plane_record_t struct which is exactly 56).
// ──

static int pack_seed_plane(const v7_seed_plane_t* sp, blob_t* out_blob) {
    const size_t header = sizeof(v7_seed_plane_record_t);  // 56
    const size_t rec_len = header + (size_t)sp->hdr.inlier_count * sizeof(uint32_t);
    uint8_t* rec = (uint8_t*)malloc(rec_len);
    if (!rec) return -1;
    memcpy(rec, &sp->hdr, header);
    memcpy(rec + header, sp->seed_indices, (size_t)sp->hdr.inlier_count * sizeof(uint32_t));
    int rc = blob_push(out_blob, rec, rec_len);
    free(rec);
    return rc;
}

// ── Parse cascade-segment-list per-bldg blob (one outer ARRAY_INLINE
//    record), extract segments with LOCAL pt_indices. The per-bldg blob
//    itself is a sequence of 8-aligned records framed via <v3/blob.h>
//    (matches v7-hemisphere-segment.c::pack_segment_record). ──

typedef struct seg_owner {
    uint32_t source_segment_id;
    uint32_t* pt_indices_local;  // owned; freed at end of building
    uint32_t n_pts;
} seg_owner_t;

static int parse_segment_list_blob(const uint8_t* bytes, size_t len,
                                    seg_owner_t** out_segs, size_t* out_n) {
    *out_segs = NULL; *out_n = 0;
    if (!bytes || len < 8) return 0;
    // Two-pass: count then fill (so we size the output array exactly).
    size_t n = 0;
    {
        size_t cursor = 0;
        const uint8_t* h; size_t rl;
        while (blob_next(bytes, len, &cursor, &h, &rl)) {
            // schema=1 in pack_segment_record
            if (rl >= 56) n++;
        }
    }
    if (n == 0) return 0;
    seg_owner_t* segs = (seg_owner_t*)safe_calloc_array(n, sizeof(seg_owner_t));
    if (!segs) return -1;
    size_t fill = 0;
    // The cascade-segment-list record's field 0..15 in pack_segment_record:
    //   u32 schema, u32 normal_group_id, u32 offset_cluster_id, u32 spatial_cluster_id
    //   f64 peak_offset, f64×3 mean_normal, u32 n_pts, u32 reserved
    //   u32 pt_idx[n_pts]
    // The "id" of v2 (original segment index in cascade-segment-list)
    // we approximate via fill position — segments are emitted in id
    // order by the C step (pack_segment_record is called in the same
    // order as the impl's seg_acc.segs[] which is emitted in
    // global_seg_id order).
    size_t cursor = 0;
    const uint8_t* h; size_t rl;
    while (blob_next(bytes, len, &cursor, &h, &rl)) {
        if (rl < 56) continue;
        uint32_t schema; memcpy(&schema, h + 0, 4);
        uint32_t n_pts;  memcpy(&n_pts,  h + 48, 4);
        if (schema == 1 && rl >= 56 + (size_t)n_pts * 4) {
            uint32_t* idx = (uint32_t*)malloc((size_t)n_pts * sizeof(uint32_t));
            if (!idx) {
                for (size_t f = 0; f < fill; f++) free(segs[f].pt_indices_local);
                free(segs);
                return -1;
            }
            memcpy(idx, h + 56, (size_t)n_pts * sizeof(uint32_t));
            segs[fill].source_segment_id = (uint32_t)fill;
            segs[fill].pt_indices_local = idx;
            segs[fill].n_pts = n_pts;
            fill++;
        }
    }
    *out_segs = segs;
    *out_n = fill;
    return 0;
}

static void free_seg_owners(seg_owner_t* segs, size_t n) {
    if (!segs) return;
    for (size_t i = 0; i < n; i++) free(segs[i].pt_indices_local);
    free(segs);
}

// ── exec ──

#define BLF_RECORD_BYTES 24  // f64×3 centroid (matches hemisphere-segment)

static int v7_normal_cascade_seed_exec(step_context_t* ctx) {
    v7_seed_params_t params;
    if (parse_params(step_get_params(ctx), &params) != 0) return -1;
    if (getenv("DERIV_LOG")) {
        fprintf(stderr, "[DERIV] step=v7-normal-cascade-seed scope=tile distThreshold=%.6f src_distThreshold=constant\n",
                params.dist_threshold);
    }

    size_t pos_bytes = 0;
    const double* positions = (const double*)step_read(ctx, "lidar-positions", &pos_bytes);
    if (!positions || pos_bytes % (3 * sizeof(double)) != 0) return -1;
    size_t total_points = pos_bytes / (3 * sizeof(double));

    size_t n_buildings = step_array_len(ctx, "per-building-records");
    size_t n_seg_arr   = step_array_len(ctx, "hs-segment-list");
    size_t n_curv_arr  = step_array_len(ctx, "ts-curv-k16");
    size_t n_frames    = step_array_len(ctx, "building-local-frames");
    if (n_seg_arr != n_buildings || n_curv_arr != n_buildings
        || n_frames != n_buildings) {
        fprintf(stderr,
                "[v7-normal-cascade-seed] count mismatch: bldgs=%zu segs=%zu "
                "curv=%zu frames=%zu\n",
                n_buildings, n_seg_arr, n_curv_arr, n_frames);
        return -1;
    }

    blob_t b_seed_planes = {0};
    blob_t b_owner       = {0};

    int rc = 0;
    double* local_f64 = NULL;
    float*  local_f32 = NULL;
    size_t  local_cap = 0;

    for (size_t bi = 0; bi < n_buildings; bi++) {
        const uint8_t* rec = NULL;
        size_t rec_len = 0;
        if (step_array_read_bytes(ctx, "per-building-records", bi, &rec, &rec_len) != 0
            || !rec || rec_len < 68) {
            blob_push(&b_seed_planes, NULL, 0);
            blob_push(&b_owner,       NULL, 0);
            continue;
        }
        uint32_t schema, n_verts, n_pts;
        memcpy(&schema,  rec + 0, 4);
        memcpy(&n_verts, rec + 4, 4);
        memcpy(&n_pts,   rec + 8, 4);
        size_t fp_bytes = (size_t)n_verts * 3 * sizeof(double);
        size_t idx_off = 68 + fp_bytes;
        if (schema != 1 || idx_off + (size_t)n_pts * sizeof(uint32_t) > rec_len) {
            blob_push(&b_seed_planes, NULL, 0);
            blob_push(&b_owner,       NULL, 0);
            continue;
        }
        const uint32_t* gidx = (const uint32_t*)(rec + idx_off);

        // Centroid from frames.
        const uint8_t* frame = NULL;
        size_t frame_len = 0;
        if (step_array_read_bytes(ctx, "building-local-frames", bi,
                                   &frame, &frame_len) != 0
            || !frame || frame_len < BLF_RECORD_BYTES) {
            blob_push(&b_seed_planes, NULL, 0);
            blob_push(&b_owner,       NULL, 0);
            continue;
        }
        double cx, cy, cz;
        memcpy(&cx, frame +  0, 8);
        memcpy(&cy, frame +  8, 8);
        memcpy(&cz, frame + 16, 8);

        // Per-bldg curvatures + cascade-segment-list.
        const uint8_t* curv_bytes = NULL; size_t curv_len = 0;
        const uint8_t* sl_bytes   = NULL; size_t sl_len   = 0;
        if (step_array_read_bytes(ctx, "ts-curv-k16", bi,
                                   &curv_bytes, &curv_len) != 0
            || curv_len != (size_t)n_pts * sizeof(float)
            || step_array_read_bytes(ctx, "hs-segment-list", bi,
                                      &sl_bytes, &sl_len) != 0) {
            blob_push(&b_seed_planes, NULL, 0);
            blob_push(&b_owner,       NULL, 0);
            continue;
        }

        // ── Per-building point-weights from ts-weights ARRAY_INLINE ──
        // REQUIRED — v7_seed_compute now enforces non-NULL via v7_plane_fit_lsq.
        const uint8_t* w_bytes = NULL; size_t w_len = 0;
        if (step_array_read_bytes(ctx, "ts-weights", bi, &w_bytes, &w_len) != 0
            || !w_bytes || w_len != (size_t)n_pts * sizeof(float)) {
            fprintf(stderr, "[v7-normal-cascade-seed] bldg%zu ts-weights missing or size mismatch (got %zu, want %zu)\n",
                    bi, w_len, (size_t)n_pts * sizeof(float));
            rc = -1; break;
        }
        const float* local_weights = (const float*)w_bytes;

        // Build per-bldg F64 positions (centroid-shifted F32-cast →
        // F64, matching hemisphere-segment exactly so segment-list's
        // LOCAL pt_indices line up with these coords byte-for-byte).
        if (v7_gather_building_points_f32_f64(
                positions, total_points, gidx, n_pts, cx, cy, cz,
                &local_f32, &local_f64, &local_cap) != 0) {
            rc = -1; break;
        }

        // Parse cascade-segment-list into v7_seed_input_segment_t[].
        seg_owner_t* segs = NULL; size_t n_segs = 0;
        if (parse_segment_list_blob(sl_bytes, sl_len, &segs, &n_segs) != 0) {
            rc = -1;
            break;
        }
        v7_seed_input_segment_t* in_segs = (v7_seed_input_segment_t*)
            safe_malloc_array(n_segs, sizeof(v7_seed_input_segment_t));
        if (!in_segs && n_segs > 0) {
            free_seg_owners(segs, n_segs);
            rc = -1; break;
        }
        for (size_t i = 0; i < n_segs; i++) {
            in_segs[i].source_segment_id = segs[i].source_segment_id;
            in_segs[i].pt_indices = segs[i].pt_indices_local;
            in_segs[i].n_pts = segs[i].n_pts;
        }

        v7_seed_outputs_t out;
        v7_seed_outputs_init(&out);
        if (v7_seed_compute(local_f64,
                             (const float*)curv_bytes,
                             local_weights,
                             (size_t)n_pts, in_segs, n_segs, &params, &out) != 0) {
            free(in_segs);
            free_seg_owners(segs, n_segs);
            v7_seed_outputs_free(&out);
            rc = -1; break;
        }

        // Emit per-bldg seed-planes (sub-blob of records). On failure the
        // per-building buffers are freed once, below, before leaving the
        // outer loop (the inner `break` alone used to fall through into a
        // second round of frees).
        blob_t per_bldg_seed_blob = {0};
        for (size_t i = 0; i < out.n_planes; i++) {
            if (pack_seed_plane(&out.planes[i], &per_bldg_seed_blob) != 0) {
                rc = -1;
                break;
            }
        }
        if (rc == 0) {
            blob_push(&b_seed_planes, per_bldg_seed_blob.bytes, per_bldg_seed_blob.len);
            // Emit per-bldg owner Int16 (LOCAL plane ids).
            blob_push(&b_owner, out.owner, (size_t)n_pts * sizeof(int16_t));
        }
        blob_free(&per_bldg_seed_blob);

        free(in_segs);
        free_seg_owners(segs, n_segs);
        v7_seed_outputs_free(&out);
        if (rc != 0) break;
    }

    free(local_f32);
    free(local_f64);

    if (rc == 0) rc |= emit_blob(ctx, "cs-planes", &b_seed_planes);
    if (rc == 0) rc |= emit_blob(ctx, "cs-owner",  &b_owner);

    blob_free(&b_seed_planes);
    blob_free(&b_owner);
    return rc;
}

// ── Step descriptor ──

static const step_read_t v7_normal_cascade_seed_reads[] = {
    {.name = "lidar-positions"},
    {.name = "per-building-records",  .array = true},
    {.name = "building-local-frames", .array = true},
    {.name = "hs-segment-list",  .array = true},
    {.name = "ts-curv-k16",        .array = true},
    {.name = "ts-weights",         .array = true},
};
static const step_write_t v7_normal_cascade_seed_writes[] = {
    {.name = "cs-planes", .kind = AK_ARRAY_INLINE, .visibility = STEP_VIS_PUBLIC, .array = true},
    {.name = "cs-owner",  .kind = AK_ARRAY_INLINE, .visibility = STEP_VIS_PUBLIC, .array = true},
};

const step_def_t v7_normal_cascade_seed_def = {
    .name = "v7-normal-cascade-seed",
    .version = 6,  // v6 (2026-06-02): curvature gate now uses MEDIAN point curvature
                   //     (border-robust), threshold 0.03→0.055. Recovers small legit
                   //     roofs the area-weighted mean over-rejected.
                   // v5 (2026-06-01): plane FIT/refit now UNWEIGHTED (passes
                   //     v7_fit_weights() → NULL). Curvature gate stays area-weighted.
                   // v4: ts-weights wired through (no silent NULL fallback);
                   //     v7_seed_compute now requires non-NULL point_weights.
                   // v3: inner blob_push framing → 8-byte aligned (see <v3/blob.h>)
    .reads = v7_normal_cascade_seed_reads,
    .n_reads = sizeof(v7_normal_cascade_seed_reads) / sizeof(v7_normal_cascade_seed_reads[0]),
    .writes = v7_normal_cascade_seed_writes,
    .n_writes = sizeof(v7_normal_cascade_seed_writes) / sizeof(v7_normal_cascade_seed_writes[0]),
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = v7_normal_cascade_seed_exec,
};

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-grow-refit-merge — step wrapper.
//
// Reads:
//   lidar-positions       (F64×3·N global)
//   per-building-records  (per-bldg: header + globalIndices)
//   building-local-frames (per-bldg: f64×3 centroid)
//   building-kd-trees     (per-bldg: serialised kd-tree bytes)
//   tile-normals          (per-bldg F32×3·n_pts) — wired by tile-signals;
//                          NOT consumed by GRM, but declared so the
//                          v7 pipeline keeps the dependency
//   point-source-ids      (U16 globals — per-strip refit input)
//   seed-owner            (per-bldg Int16, NOT consumed; v7 wiring quirk —
//                          we only need seed-planes for the actual seeds)
//   seed-planes           (per-bldg ARRAY_INLINE — seeds from cascade-seed)
//   ghost-planes-2        (per-bldg ARRAY_INLINE — extra ghost seeds)
//
// Writes (all prefixed grm- so multiple owner-emitting steps coexist
// in the visibility tree, mirroring the seed-step convention):
//   grm-owner                (per-bldg Int16 plane id)
//   grm-ridge-candidates     (per-bldg Uint8 mask)
//   grm-residual             (per-bldg F32 |signed-dist|)
//   grm-plane-equations      (per-bldg ARRAY_INLINE — fixed-stride 32-B
//                              record per plane: f64×3 normal + f64 d.
//                              Tombstoned planes keep their last-known
//                              equation; matches v2 layout.)
//   grm-seed-planes          (per-bldg ARRAY_INLINE — full record + LOCAL
//                              seedIndices, one per plane including
//                              tombstoned ones)
//   grm-distance-threshold   (per-bldg ARRAY_INLINE, single 8-byte F64
//                              record per building — picked up by the
//                              boundary-chain WSD trust normaliser)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jansson.h>

#include "v3/artifact.h"
#include "v3/json_int.h"
#include "v3/blob.h"
#include "v3/step.h"
#include "kd_tree.h"
#include "v7-derive-thresholds.h"
#include "v7-grow-refit-merge-impl.h"
#include "v7-perbldg-gather.h"  // v7_per_bldg_load_frame (#72c Family B)

#define BLF_RECORD_BYTES 24

// ── Params ──

static int parse_params(const char* params_str, v7_grm_params_t* p) {
    v7_grm_params_default(p);
    p->up_axis = 2;  // production v3 z-up LAZ
    // Sentinels: -1 means "load from bldg-thresholds artifact per-bldg".
    // Pipeline params (density / distThreshold) override below if set.
    p->density        = -1.0;
    p->dist_threshold = -1.0;
    if (!params_str) return 0;
    json_error_t je;
    json_t* obj = json_loads(params_str, 0, &je);
    if (!obj) return -1;
    json_t* j;
    if ((j = json_object_get(obj, "distThreshold"))   && json_is_number(j)) p->dist_threshold = json_number_value(j);
    if ((j = json_object_get(obj, "maxDegradation"))  && json_is_number(j)) p->max_degradation = json_number_value(j);
    j = json_object_get(obj, "maxIterations");
    if (json_int32_or_default(j, 1, 100000, "maxIterations", &p->max_iterations) != 0) { json_decref(obj); return -1; }
    if ((j = json_object_get(obj, "stableThreshold")) && json_is_number(j)) p->stable_threshold = json_number_value(j);
    if ((j = json_object_get(obj, "searchRadiusScale")) && json_is_number(j)) p->search_radius_scale = json_number_value(j);
    if ((j = json_object_get(obj, "searchRadiusMax"))   && json_is_number(j)) p->search_radius_max = json_number_value(j);
    if ((j = json_object_get(obj, "density"))         && (json_is_number(j)||json_is_integer(j))) p->density = json_number_value(j);
    if ((j = json_object_get(obj, "mergeAngleDeg"))   && json_is_number(j)) p->merge_angle_deg = json_number_value(j);
    if ((j = json_object_get(obj, "mergeWallAngleDeg")) && json_is_number(j)) p->merge_wall_angle_deg = json_number_value(j);
    if ((j = json_object_get(obj, "mergeOffsetScale"))  && json_is_number(j)) p->merge_offset_scale = json_number_value(j);
    j = json_object_get(obj, "mergeMinSharedPts");
    if (json_int32_or_default(j, 1, 1000000, "mergeMinSharedPts", &p->merge_min_shared_pts) != 0) { json_decref(obj); return -1; }
    if ((j = json_object_get(obj, "wallTiltDeg"))     && json_is_number(j)) p->wall_tilt_deg = json_number_value(j);
    if ((j = json_object_get(obj, "coherenceMajority")) && json_is_number(j)) p->coherence_majority = json_number_value(j);
    j = json_object_get(obj, "coherenceMaxRounds");
    if (json_int32_or_default(j, 1, 10000, "coherenceMaxRounds", &p->coherence_max_rounds) != 0) { json_decref(obj); return -1; }
    j = json_object_get(obj, "coherenceMinVotes");
    if (json_int32_or_default(j, 1, 1000000, "coherenceMinVotes", &p->coherence_min_votes) != 0) { json_decref(obj); return -1; }
    j = json_object_get(obj, "upAxis");
    if (json_int32_or_default(j, 0, 2, "upAxis", &p->up_axis) != 0) { json_decref(obj); return -1; }
    json_decref(obj);
    return 0;
}

// ── Parse seed-planes (per-bldg sub-blob from cascade-seed) ──

typedef struct local_seed {
    double normal[3];
    double d;
    double seed_tilt;
    uint32_t* seed_indices;   // local to building, owned
    uint32_t  n_seeds;
    uint8_t   is_ghost;
    uint32_t  ghost_void_id;
} local_seed_t;

// cascade-seed record layout (matches v7-normal-cascade-seed.c):
//   f64×3 normal, f64 d, f64 tilt, f64 azimuth, f64 rms,
//   u32 inlier_count, u32 source_segment_id  (header = 64 B)
//   u32×inlier_count seed_indices
#define SEED_HDR_BYTES 64
// ghost-planes-2 record layout (matches v7-density-voids-2.c):
//   f64×3 normal, f64 d, f64 tilt, f64 azimuth, f64 rms,
//   f64×3 centroid, f64 area,
//   u32 void_id, u32 inlier_count, u32 hull_vert_count, u32 _reserved
//   (header = 104 B)
//   f64×3·hull_vert_count hull, then u32×inlier_count seed_indices
#define GHOST_HDR_BYTES 104

static int parse_seed_blob(const uint8_t* bytes, size_t len,
                            local_seed_t** out, size_t* out_n) {
    *out = NULL; *out_n = 0;
    if (!bytes || len < 8) return 0;
    // Two-pass: count, alloc, fill.
    size_t n = 0;
    {
        size_t cursor = 0;
        const uint8_t* h; size_t rl;
        while (blob_next(bytes, len, &cursor, &h, &rl)) {
            if (rl >= SEED_HDR_BYTES) n++;
        }
    }
    if (n == 0) return 0;
    local_seed_t* arr = (local_seed_t*)calloc(n, sizeof(local_seed_t));
    if (!arr) return -1;
    size_t k = 0;
    size_t cursor = 0;
    const uint8_t* h; size_t rl;
    while (blob_next(bytes, len, &cursor, &h, &rl)) {
        if (rl < SEED_HDR_BYTES) continue;
        memcpy(&arr[k].normal[0], h +  0, 8);
        memcpy(&arr[k].normal[1], h +  8, 8);
        memcpy(&arr[k].normal[2], h + 16, 8);
        memcpy(&arr[k].d,         h + 24, 8);
        memcpy(&arr[k].seed_tilt, h + 32, 8);
        uint32_t inlier_count;
        memcpy(&inlier_count,     h + 56, 4);
        const size_t idx_off = SEED_HDR_BYTES;
        if (idx_off + (size_t)inlier_count * 4 > rl) continue;
        uint32_t* idx = (uint32_t*)malloc((size_t)inlier_count * sizeof(uint32_t));
        if (!idx) { for (size_t f = 0; f < k; f++) free(arr[f].seed_indices); free(arr); return -1; }
        memcpy(idx, h + idx_off, (size_t)inlier_count * 4);
        arr[k].seed_indices = idx;
        arr[k].n_seeds = inlier_count;
        arr[k].is_ghost = 0;
        k++;
    }
    *out = arr; *out_n = k;
    return 0;
}

static int append_ghost_blob(const uint8_t* bytes, size_t len,
                              local_seed_t** in_out, size_t* in_out_n) {
    if (!bytes || len < 8) return 0;
    size_t ng = 0;
    {
        size_t cursor = 0;
        const uint8_t* h; size_t rl;
        while (blob_next(bytes, len, &cursor, &h, &rl)) {
            if (rl >= GHOST_HDR_BYTES) ng++;
        }
    }
    if (ng == 0) return 0;
    const size_t old_n = *in_out_n;
    local_seed_t* arr = (local_seed_t*)realloc(*in_out, (old_n + ng) * sizeof(local_seed_t));
    if (!arr) return -1;
    *in_out = arr;
    size_t k = old_n;
    size_t cursor = 0;
    const uint8_t* h; size_t rl;
    while (blob_next(bytes, len, &cursor, &h, &rl)) {
        if (rl < GHOST_HDR_BYTES) continue;
        memset(&arr[k], 0, sizeof(arr[k]));
        memcpy(&arr[k].normal[0], h +  0, 8);
        memcpy(&arr[k].normal[1], h +  8, 8);
        memcpy(&arr[k].normal[2], h + 16, 8);
        memcpy(&arr[k].d,         h + 24, 8);
        memcpy(&arr[k].seed_tilt, h + 32, 8);
        uint32_t void_id, inlier_count, hull_count;
        memcpy(&void_id,      h + 88, 4);
        memcpy(&inlier_count, h + 92, 4);
        memcpy(&hull_count,   h + 96, 4);
        const size_t idx_off = GHOST_HDR_BYTES + (size_t)hull_count * 24;
        if (idx_off + (size_t)inlier_count * 4 > rl) continue;
        uint32_t* idx = (uint32_t*)malloc((size_t)inlier_count * sizeof(uint32_t));
        if (!idx) continue;
        memcpy(idx, h + idx_off, (size_t)inlier_count * 4);
        arr[k].seed_indices = idx;
        arr[k].n_seeds = inlier_count;
        arr[k].is_ghost = 1;
        arr[k].ghost_void_id = void_id;
        k++;
    }
    *in_out_n = k;
    return 0;
}

// ── Pack outputs ──

#define GRM_PLANE_REC_BYTES 32   // f64×3 normal + f64 d
#define GRM_SEED_HDR_BYTES   sizeof(v7_grm_plane_record_t)  // 64

static int pack_plane_eq(const v7_grm_plane_t* p, blob_t* out_blob) {
    uint8_t rec[GRM_PLANE_REC_BYTES];
    memcpy(rec +  0, &p->hdr.normal[0], 8);
    memcpy(rec +  8, &p->hdr.normal[1], 8);
    memcpy(rec + 16, &p->hdr.normal[2], 8);
    memcpy(rec + 24, &p->hdr.d,         8);
    return blob_push(out_blob, rec, sizeof(rec));
}

static int pack_seed_plane_grm(const v7_grm_plane_t* p, blob_t* out_blob) {
    const size_t hdr = sizeof(p->hdr);
    const size_t idx_bytes = (size_t)p->hdr.inlier_count * sizeof(uint32_t);
    uint8_t* rec = (uint8_t*)malloc(hdr + idx_bytes);
    if (!rec) return -1;
    memcpy(rec, &p->hdr, hdr);
    if (p->seed_indices && idx_bytes > 0) {
        memcpy(rec + hdr, p->seed_indices, idx_bytes);
    }
    int rc = blob_push(out_blob, rec, hdr + idx_bytes);
    free(rec);
    return rc;
}

// Pack per-plane void_ids into a fixed-stride 8-byte record array
// (u32 plane_id, u32 void_id). One record per (plane, void_id) pair —
// N:1 (multiple voids per plane) handled naturally. Planes with no
// voids contribute zero records. Output is the *building's* slice of
// the per-bldg ARRAY_INLINE; caller does the outer-array wrap.
#define GRM_PLANE_VOIDS_REC_BYTES 8
static int pack_plane_voids_bldg(const v7_grm_outputs_t* out_grm, blob_t* out_blob) {
    for (uint32_t pi = 0; pi < (uint32_t)out_grm->n_planes; pi++) {
        const v7_grm_plane_t* p = &out_grm->planes[pi];
        if (p->hdr.tombstoned) continue;
        for (uint32_t k = 0; k < p->n_void_ids; k++) {
            uint8_t rec[GRM_PLANE_VOIDS_REC_BYTES];
            memcpy(rec + 0, &pi,             4);
            memcpy(rec + 4, &p->void_ids[k], 4);
            if (blob_push(out_blob, rec, sizeof(rec)) != 0) return -1;
        }
    }
    return 0;
}

// ── exec ──

static int v7_grow_refit_merge_exec(step_context_t* ctx) {
    v7_grm_params_t params;
    if (parse_params(step_get_params(ctx), &params) != 0) return -1;

    size_t pos_bytes = 0;
    const double* positions = (const double*)step_read(ctx, "lidar-positions", &pos_bytes);
    if (!positions || pos_bytes % (3 * sizeof(double)) != 0) return -1;
    size_t total_points = pos_bytes / (3 * sizeof(double));

    size_t n_buildings = step_array_len(ctx, "per-building-records");
    if (step_array_len(ctx, "building-local-frames") != n_buildings ||
        step_array_len(ctx, "building-kd-trees")     != n_buildings ||
        step_array_len(ctx, "cs-planes")           != n_buildings ||
        step_array_len(ctx, "dv2-ghost-planes")        != n_buildings) {
        fprintf(stderr, "[v7-grow-refit-merge] count mismatch across per-bldg arrays\n");
        return -1;
    }

    size_t psid_bytes = 0;
    const uint16_t* psids = (const uint16_t*)step_read(ctx, "point-source-ids", &psid_bytes);
    if (psids && psid_bytes != total_points * sizeof(uint16_t)) psids = NULL;

    // density + distThreshold are sourced from v7-derive-thresholds
    // (per-bldg ARRAY_INLINE, stride=V7_BLDG_THRESHOLDS_BYTES) which
    // ran upstream. Single source of truth — see v7-derive-thresholds.c.
    // Pipeline params (`density`, `distThreshold` JSON keys) still win
    // if explicitly set, otherwise the artifact is used per-bldg.
    const bool density_auto       = (params.density <= 0.0);
    const bool dist_thresh_auto   = (params.dist_threshold <= 0.0);
    if ((density_auto || dist_thresh_auto)
        && step_array_len(ctx, "bldg-thresholds") != n_buildings) {
        fprintf(stderr, "[v7-grow-refit-merge] bldg-thresholds missing/mismatched — wire v7-derive-thresholds\n");
        return -1;
    }

    blob_t b_owner = {0}, b_ridge = {0}, b_residual = {0};
    blob_t b_eq = {0}, b_seed = {0}, b_thr = {0}, b_voids = {0};

    int rc = 0;
    double* local_f64 = NULL;
    float*  local_f32 = NULL;
    size_t  local_cap = 0;
    uint32_t* local_pool = NULL;

    for (size_t bi = 0; bi < n_buildings; bi++) {
        // Per-bldg record + local-frame parse (task #72c Family B helper).
        const uint32_t* gidx = NULL;
        uint32_t n_pts = 0;
        double cx, cy, cz;
        if (v7_per_bldg_load_frame(ctx, bi, &gidx, &n_pts, &cx, &cy, &cz) != 0) goto skip_bldg;

        const uint8_t* tree_bytes = NULL; size_t tree_len = 0;
        if (step_array_read_bytes(ctx, "building-kd-trees", bi, &tree_bytes, &tree_len) != 0)
            goto skip_bldg;

        const uint8_t* seed_bytes = NULL; size_t seed_len = 0;
        if (step_array_read_bytes(ctx, "cs-planes", bi, &seed_bytes, &seed_len) != 0)
            goto skip_bldg;
        const uint8_t* ghost_bytes = NULL; size_t ghost_len = 0;
        step_array_read_bytes(ctx, "dv2-ghost-planes", bi, &ghost_bytes, &ghost_len);

        // ── Build per-bldg F64 positions (centroid-shifted F32-cast) ──
        if ((size_t)n_pts * 3 > local_cap) {
            float*  p32 = (float*) realloc(local_f32, (size_t)n_pts * 3 * sizeof(float));
            double* p64 = (double*)realloc(local_f64, (size_t)n_pts * 3 * sizeof(double));
            uint32_t* pp = (uint32_t*)realloc(local_pool, (size_t)n_pts * sizeof(uint32_t));
            if (!p32 || !p64 || !pp) { free(p32); free(p64); free(pp); rc = -1; break; }
            local_f32 = p32; local_f64 = p64; local_pool = pp;
            local_cap = (size_t)n_pts * 3;
        }
        for (uint32_t i = 0; i < n_pts; i++) {
            const uint32_t g = gidx[i];
            if ((size_t)g >= total_points) {
                local_f32[3 * i + 0] = local_f32[3 * i + 1] = local_f32[3 * i + 2] = 0.0f;
            } else {
                local_f32[3 * i + 0] = (float)(positions[3 * g + 0] - cx);
                local_f32[3 * i + 1] = (float)(positions[3 * g + 1] - cy);
                local_f32[3 * i + 2] = (float)(positions[3 * g + 2] - cz);
            }
            local_pool[i] = i;
        }
        for (size_t i = 0; i < (size_t)n_pts * 3; i++) local_f64[i] = (double)local_f32[i];

        // ── Per-building density override (when not pinned via params).
        //    Bbox over the two non-up axes of the building's local frame;
        //    matches v2 tile-signals.js's density formula (n_pts / area)
        //    but applied to GRM's actual operating set instead of the full
        //    tile. Critical because v3 GRM is per-building while v2 GRM is
        //    tile-wide — using full-tile density here would mis-scale the
        //    search radius by sqrt(tile_area/bldg_area) (1.4× on dahouet).
        // Read per-bldg thresholds (single source of truth — see
        // v7-derive-thresholds.c). Pipeline params (density / distThreshold)
        // override per-bldg values when explicitly pinned.
        if (density_auto || dist_thresh_auto) {
            const uint8_t* th_bytes = NULL; size_t th_len = 0;
            if (step_array_read_bytes(ctx, "bldg-thresholds", bi,
                                       &th_bytes, &th_len) != 0
                || !th_bytes || th_len < V7_BLDG_THRESHOLDS_BYTES) {
                fprintf(stderr, "[v7-grow-refit-merge] bldg%zu missing bldg-thresholds record\n", bi);
                rc = -1; break;
            }
            v7_bldg_thresholds_t th;
            memcpy(&th, th_bytes, sizeof(th));
            if (density_auto)     params.density        = th.density;
            if (dist_thresh_auto) params.dist_threshold = th.dist_threshold_grm;
        }
        if (getenv("DERIV_LOG")) {
            fprintf(stderr, "[DERIV] step=v7-grow-refit-merge scope=bldg%zu n_pts=%u density=%.6f distThreshold=%.6f maxDegradation=%.6f radiusScale=%.3f src_density=%s src_distThreshold=%s\n",
                    bi, n_pts, params.density, params.dist_threshold, params.max_degradation, params.search_radius_scale,
                    density_auto ? "bldg-thresholds" : "param",
                    dist_thresh_auto ? "bldg-thresholds" : "param");
        }

        // ── Per-building PSIDs (extract from globals) ──
        uint16_t* local_psids = NULL;
        if (psids) {
            local_psids = (uint16_t*)malloc((size_t)n_pts * sizeof(uint16_t));
            if (!local_psids) { rc = -1; break; }
            for (uint32_t i = 0; i < n_pts; i++) {
                const uint32_t g = gidx[i];
                local_psids[i] = (g < total_points) ? psids[g] : (uint16_t)0;
            }
        }

        // ── Per-building point-weights from ts-weights ARRAY_INLINE ──
        // REQUIRED — v7_grm_compute now enforces non-NULL via v7_plane_fit_lsq.
        const uint8_t* w_bytes = NULL; size_t w_len = 0;
        if (step_array_read_bytes(ctx, "ts-weights", bi, &w_bytes, &w_len) != 0
            || !w_bytes || w_len != (size_t)n_pts * sizeof(float)) {
            fprintf(stderr, "[v7-grow-refit-merge] bldg%zu ts-weights missing or size mismatch (got %zu, want %zu)\n",
                    bi, w_len, (size_t)n_pts * sizeof(float));
            free(local_psids); rc = -1; break;
        }
        const float* local_weights = (const float*)w_bytes;

        // ── Parse seeds + ghosts ──
        local_seed_t* seeds = NULL; size_t n_seeds = 0;
        if (parse_seed_blob(seed_bytes, seed_len, &seeds, &n_seeds) != 0) {
            free(local_psids); rc = -1; break;
        }
        if (ghost_bytes && ghost_len > 0) {
            append_ghost_blob(ghost_bytes, ghost_len, &seeds, &n_seeds);
        }
        v7_grm_input_seed_t* in_seeds = (v7_grm_input_seed_t*)
            calloc(n_seeds ? n_seeds : 1, sizeof(v7_grm_input_seed_t));
        for (size_t i = 0; i < n_seeds; i++) {
            in_seeds[i].normal[0] = seeds[i].normal[0];
            in_seeds[i].normal[1] = seeds[i].normal[1];
            in_seeds[i].normal[2] = seeds[i].normal[2];
            in_seeds[i].d         = seeds[i].d;
            in_seeds[i].seed_tilt = seeds[i].seed_tilt;
            in_seeds[i].seed_indices = seeds[i].seed_indices;
            in_seeds[i].n_seeds      = seeds[i].n_seeds;
            in_seeds[i].is_ghost     = seeds[i].is_ghost;
            in_seeds[i].ghost_void_id = seeds[i].ghost_void_id;
        }

        // ── kd-tree ──
        kd_tree_t tree;
        if (kd_deserialize(&tree, local_f64, n_pts, tree_bytes, tree_len) != 0) {
            for (size_t i = 0; i < n_seeds; i++) free(seeds[i].seed_indices);
            free(seeds); free(in_seeds); free(local_psids);
            rc = -1; break;
        }

        v7_grm_outputs_t out;
        v7_grm_outputs_init(&out);
        const int compute_rc = v7_grm_compute(local_f64,
                                                local_weights,
                                                local_psids,
                                                (size_t)n_pts,
                                                local_pool, n_pts,
                                                &tree, in_seeds, n_seeds,
                                                &params, &out);
        kd_free(&tree);
        for (size_t i = 0; i < n_seeds; i++) free(seeds[i].seed_indices);
        free(seeds); free(in_seeds); free(local_psids);
        if (compute_rc != 0) {
            v7_grm_outputs_free(&out);
            rc = -1; break;
        }

        // ── Pack per-bldg outputs ──
        blob_push(&b_owner,    out.owner,      (size_t)n_pts * sizeof(int16_t));
        blob_push(&b_ridge,    out.ridge_mask, (size_t)n_pts);
        blob_push(&b_residual, out.residual,   (size_t)n_pts * sizeof(float));

        blob_t per_eq = {0}, per_seed = {0}, per_voids = {0};
        for (size_t i = 0; i < out.n_planes; i++) {
            pack_plane_eq(&out.planes[i], &per_eq);
            pack_seed_plane_grm(&out.planes[i], &per_seed);
        }
        pack_plane_voids_bldg(&out, &per_voids);
        blob_push(&b_eq,    per_eq.bytes,    per_eq.len);
        blob_push(&b_seed,  per_seed.bytes,  per_seed.len);
        blob_push(&b_voids, per_voids.bytes, per_voids.len);
        blob_free(&per_eq); blob_free(&per_seed); blob_free(&per_voids);
        blob_push(&b_thr, &out.distance_threshold, sizeof(double));

        v7_grm_outputs_free(&out);
        continue;

    skip_bldg:
        blob_push(&b_owner, NULL, 0);
        blob_push(&b_ridge, NULL, 0);
        blob_push(&b_residual, NULL, 0);
        blob_push(&b_eq,    NULL, 0);
        blob_push(&b_seed,  NULL, 0);
        blob_push(&b_thr,   NULL, 0);
        blob_push(&b_voids, NULL, 0);
    }

    free(local_f32); free(local_f64); free(local_pool);

    if (rc == 0) rc |= emit_blob(ctx, "grm-owner",            &b_owner);
    if (rc == 0) rc |= emit_blob(ctx, "grm-ridge-candidates", &b_ridge);
    if (rc == 0) rc |= emit_blob(ctx, "grm-residual",         &b_residual);
    if (rc == 0) rc |= emit_blob(ctx, "grm-plane-equations",  &b_eq);
    if (rc == 0) rc |= emit_blob(ctx, "grm-seed-planes",      &b_seed);
    if (rc == 0) rc |= emit_blob(ctx, "grm-distance-threshold", &b_thr);
    if (rc == 0) rc |= emit_blob(ctx, "grm-plane-voids",       &b_voids);

    blob_free(&b_owner); blob_free(&b_ridge); blob_free(&b_residual);
    blob_free(&b_eq); blob_free(&b_seed); blob_free(&b_thr); blob_free(&b_voids);
    return rc;
}

// ── Step descriptor ──

static const step_read_t v7_grow_refit_merge_reads[] = {
    {.name = "lidar-positions"},
    {.name = "per-building-records",  .array = true},
    {.name = "building-local-frames", .array = true},
    {.name = "building-kd-trees",     .array = true},
    {.name = "cs-planes",           .array = true},
    {.name = "dv2-ghost-planes",        .array = true},
    {.name = "ts-weights",            .array = true},  // REQUIRED for weighted PCA in plane fits
    {.name = "point-source-ids"},
    {.name = "bldg-thresholds",         .array = true},
};
static const step_write_t v7_grow_refit_merge_writes[] = {
    {.name = "grm-owner",             .kind = AK_ARRAY_INLINE, .visibility = STEP_VIS_PUBLIC, .array = true},
    {.name = "grm-ridge-candidates",  .kind = AK_ARRAY_INLINE, .visibility = STEP_VIS_PUBLIC, .array = true},
    {.name = "grm-residual",          .kind = AK_ARRAY_INLINE, .visibility = STEP_VIS_PUBLIC, .array = true},
    {.name = "grm-plane-equations",   .kind = AK_ARRAY_INLINE, .visibility = STEP_VIS_PUBLIC, .array = true},
    {.name = "grm-seed-planes",       .kind = AK_ARRAY_INLINE, .visibility = STEP_VIS_DEBUG,  .array = true},
    {.name = "grm-distance-threshold", .kind = AK_ARRAY_INLINE, .visibility = STEP_VIS_DEBUG, .array = true},
    // Per-plane void-id list: fixed-stride 8B (u32 plane_id, u32 void_id),
    // multiple records per plane when a plane absorbed >1 ghost during
    // merging. Consumed by v7-boundary-topology (repacks into roof-graph
    // RG_SECT_PLANE_VOIDS) and indirectly by v7-boundary-signals.
    {.name = "grm-plane-voids",        .kind = AK_ARRAY_INLINE, .visibility = STEP_VIS_DEBUG, .array = true},
};

const step_def_t v7_grow_refit_merge_def = {
    .name = "v7-grow-refit-merge",
    // v3 (2026-05-16): density auto-computed per-building from local
    //                   positions bbox (matches v2 tile-signals formula,
    //                   applied to GRM's actual per-bldg operating set
    //                   rather than the full tile). Default 25.0 dropped;
    //                   sentinel -1 in parse_params means "auto" — pipeline
    //                   params can still pin `density` explicitly.
    .version = 11,  // v11 (2026-06-09): REDUNDANT-PLANE PRUNE (dissolve a plane a
                    //   neighbour explains ~as well: (alt_RMS−own_RMS)/dist_threshold < tau)
                    //   — the real fix for spurious junction/valley planes (seg68 class)
                    //   no per-segment size/curvature scalar can catch. PLUS search radius
                    //   dimensional fix: scale·√w·(1/√d)=[L²] → scale·max(√w,1/√d).
                    //   (v8–10 were uncommitted A/B radius forms; numbers kept to avoid
                    //   cache reuse.) See 2026-06-09-grm-redundant-plane-prune journal.
                    // v7 (2026-06-01): plane FIT/refit now UNWEIGHTED too (passes v7_fit_weights() → NULL). Connectivity search radius stays weighted.
                    // v6 (2026-06-01): degradation/merge gates + reported rms use WEIGHT-FREE geometric RMS (fit stays weighted) — see geom_rms_plane.
                    // v5 (2026-05-24): emit grm-plane-voids — per-plane void_id list,
                    //                  preserved through merge by set-union (each
                    //                  surviving plane inherits absorbed ghosts' voids)
                    // v4: reject strip fits with |dot(n,ref)| < 0.5 from averaged path (FP-coin-flip guard, audit #274)
    .reads = v7_grow_refit_merge_reads,
    .n_reads = sizeof(v7_grow_refit_merge_reads) / sizeof(v7_grow_refit_merge_reads[0]),
    .writes = v7_grow_refit_merge_writes,
    .n_writes = sizeof(v7_grow_refit_merge_writes) / sizeof(v7_grow_refit_merge_writes[0]),
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = v7_grow_refit_merge_exec,
};

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-density-voids-2 — step wrapper.
//
// Reads:
//   lidar-positions       (F64×3·N global)
//   per-building-records  (per-bldg: header + globalIndices)
//   building-local-frames (per-bldg: f64×3 centroid)
//   tile-normals          (per-bldg F32×3·n_pts_in_bldg)
//
// Writes:
//   ghost-planes-2        (per-bldg ARRAY_INLINE, fixed-stride record
//                          + variable hull[] + inlier_indices[])
//   density-voids-2-2d    (per-bldg ARRAY_INLINE, packed binary blob,
//                          gated by params.emitVizArtifacts)
//
// Per the audit: drops `lidar-density` + `lidar-z-noise` reads in
// favour of `density` + `dist_threshold` step params (defaults baked
// to v7's actual values: 30 pts/m² → 0.36 m cell, σ_z=0.02 → 0.09 m
// dist threshold). The inputs that drove derivation in v2 are
// expressible as per-call params — see PHASE-5-V7-PORT.md
// Post-port reassessments.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jansson.h>

#include "v3/artifact.h"
#include "v3/json_int.h"
#include "v3/blob.h"
#include "v3/step.h"
#include "v7-derive-thresholds.h"
#include "v7-density-voids-2-impl.h"
#include "v7-perbldg-gather.h"

#define BLF_RECORD_BYTES 24

// ── Params ──

static int parse_params(const char* params_str, v7_dv2_params_t* p) {
    v7_dv2_params_default(p);
    p->up_axis = 2;  // production v3 z-up LAZ
    // Sentinel: -1 means "auto-compute per-bldg density from local positions"
    // in exec below (mirrors v7-grow-refit-merge). Pipeline params can pin
    // a value via `density` JSON key — that wins via the loop below.
    p->density = -1.0;
    if (!params_str) return 0;
    json_error_t je;
    json_t* obj = json_loads(params_str, 0, &je);
    if (!obj) return -1;
    json_t* j;
    if ((j = json_object_get(obj, "cellSize")) && json_is_number(j))
        p->cell_size = json_number_value(j);
    if ((j = json_object_get(obj, "density")) && (json_is_number(j) || json_is_integer(j)))
        p->density = json_number_value(j);
    if ((j = json_object_get(obj, "distThreshold")) && json_is_number(j))
        p->dist_threshold = json_number_value(j);
    // Frama-C audit pass 4: strict range-checking on int32 fields.
    j = json_object_get(obj, "occupiedMinCount");
    if (json_int32_or_default(j, 1, 1000000, "occupiedMinCount", &p->occupied_min_count) != 0)
        { json_decref(obj); return -1; }
    j = json_object_get(obj, "coreMinEmpty");
    if (json_int32_or_default(j, 1, 1000000, "coreMinEmpty", &p->core_min_empty) != 0)
        { json_decref(obj); return -1; }
    j = json_object_get(obj, "minVoidCells");
    if (json_int32_or_default(j, 1, 1000000, "minVoidCells", &p->min_void_cells) != 0)
        { json_decref(obj); return -1; }
    j = json_object_get(obj, "rimDilation");
    if (json_int32_or_default(j, 0, 64, "rimDilation", &p->rim_dilation) != 0)
        { json_decref(obj); return -1; }
    j = json_object_get(obj, "minFitPts");
    if (json_int32_or_default(j, 1, 1000000, "minFitPts", &p->min_fit_pts) != 0)
        { json_decref(obj); return -1; }
    j = json_object_get(obj, "ransacIterations");
    if (json_int32_or_default(j, 1, 1000000, "ransacIterations", &p->ransac_iterations) != 0)
        { json_decref(obj); return -1; }
    j = json_object_get(obj, "minInliers");
    if (json_int32_or_default(j, 1, 1000000, "minInliers", &p->min_inliers) != 0)
        { json_decref(obj); return -1; }
    // RNG seed: any int is meaningful; INT32_MIN..INT32_MAX is the full
    // representable range, so the helper effectively just validates type.
    j = json_object_get(obj, "seed");
    if (json_int32_or_default(j, INT32_MIN, INT32_MAX, "seed", &p->seed) != 0)
        { json_decref(obj); return -1; }
    if ((j = json_object_get(obj, "minNormalY")) && json_is_number(j))
        p->min_normal_y = json_number_value(j);
    if ((j = json_object_get(obj, "roofTiltMin")) && json_is_number(j))
        p->roof_tilt_min = json_number_value(j);
    if ((j = json_object_get(obj, "roofTiltMax")) && json_is_number(j))
        p->roof_tilt_max = json_number_value(j);
    j = json_object_get(obj, "upAxis");
    if (json_int32_or_default(j, 0, 2, "upAxis", &p->up_axis) != 0)
        { json_decref(obj); return -1; }
    if ((j = json_object_get(obj, "emitVizArtifacts")) && json_is_boolean(j))
        p->emit_viz_artifacts = json_is_true(j);
    json_decref(obj);
    return 0;
}

// ── Pack one ghost plane as a flat record. Layout (104-byte fixed
//    header + variable hull/inlier sections):
//
//      f64×3  normal       (24)
//      f64    d            ( 8)
//      f64    tilt         ( 8)
//      f64    azimuth      ( 8)
//      f64    rms          ( 8)
//      f64×3  centroid     (24)
//      f64    area         ( 8)
//      u32    void_id      ( 4)
//      u32    inlier_count ( 4)
//      u32    hull_vert_count ( 4)
//      u32    _reserved    ( 4)         → 104 bytes, 8-aligned
//      f64×3·hull_vert_count  hull verts
//      u32×inlier_count       inlier_indices (LOCAL to building)
// ──

#define GHOST_HDR_BYTES 104

static int pack_ghost_plane(const v7_dv2_ghost_plane_t* g, blob_t* out_blob) {
    const size_t hull_bytes = (size_t)g->hull_vert_count * 3 * sizeof(double);
    const size_t inlier_bytes = (size_t)g->inlier_count * sizeof(uint32_t);
    const size_t rec_len = GHOST_HDR_BYTES + hull_bytes + inlier_bytes;
    uint8_t* rec = (uint8_t*)malloc(rec_len);
    if (!rec) return -1;
    size_t off = 0;
    memcpy(rec + off, g->normal,   24); off += 24;
    memcpy(rec + off, &g->d,        8); off += 8;
    memcpy(rec + off, &g->tilt,     8); off += 8;
    memcpy(rec + off, &g->azimuth,  8); off += 8;
    memcpy(rec + off, &g->rms,      8); off += 8;
    memcpy(rec + off, g->centroid, 24); off += 24;
    memcpy(rec + off, &g->area,     8); off += 8;
    memcpy(rec + off, &g->void_id,         4); off += 4;
    memcpy(rec + off, &g->inlier_count,    4); off += 4;
    memcpy(rec + off, &g->hull_vert_count, 4); off += 4;
    uint32_t reserved = 0;
    memcpy(rec + off, &reserved, 4); off += 4;
    memcpy(rec + off, g->hull_xyz, hull_bytes);          off += hull_bytes;
    memcpy(rec + off, g->inlier_indices, inlier_bytes);  /* off += inlier_bytes; */
    int rc = blob_push(out_blob, rec, rec_len);
    free(rec);
    return rc;
}

// ── Pack the 2D viz blob as a fixed-then-variable byte sequence.
//    Header (56 B):
//      f64    bounds[4]            (32)
//      f64    cell_size            ( 8)
//      i32    cols                 ( 4)
//      i32    rows                 ( 4)
//      i32    rim_dilation         ( 4)
//      u32    n_points             ( 4)
//    Per-array sections, each prefixed with its u32 length:
//      counts:    cols*rows × i32
//      void_ids:  cols*rows × i16
//      rim_mask:  cols*rows × u8
//      voids:     n_voids × VOID_SUMMARY_BYTES (=120) per record
//      rim_xy:    n_rim × 2 × f64
//      rim_aux:   n_rim × 6 bytes (i16 void_id, u8 inlier, u8 survivor,
//                                   u8 dist, u8 filtered)
//    Each void summary (120 bytes, all 4-aligned, 8-aligned overall):
//      u32 cell_count, rim_pt_count, unowned, surv          (16)
//      f64 area, cx, cz                                     (24)
//      u8  has_fit, u8 _pad×7                               ( 8)
//      f64 fit_normal[3]                                    (24)
//      f64 fit_d, fit_rms, fit_tilt, fit_azimuth            (32)
//      u32 fit_n_candidates, fit_n_inliers, _pad×2          (16)
// ──

#define VOID_SUMMARY_BYTES 120
#define RIM_AUX_BYTES 6

static int pack_viz_2d(const v7_dv2_2d_t* v, blob_t* out_blob) {
    if (!v) return blob_push(out_blob, NULL, 0);  // empty record when gated off
    const size_t n_cells = (size_t)v->cols * (size_t)v->rows;
    size_t total =
        56 +                                    /* fixed header */
        4 + n_cells * sizeof(int32_t) +
        4 + n_cells * sizeof(int16_t) +
        4 + n_cells +
        4 + (size_t)v->n_voids * VOID_SUMMARY_BYTES +
        4 + (size_t)v->n_rim * 2 * sizeof(double) +
        4 + (size_t)v->n_rim * RIM_AUX_BYTES;
    uint8_t* rec = (uint8_t*)malloc(total);
    if (!rec) return -1;
    size_t off = 0;
    memcpy(rec + off, v->bounds,        32); off += 32;
    memcpy(rec + off, &v->cell_size,     8); off += 8;
    memcpy(rec + off, &v->cols,          4); off += 4;
    memcpy(rec + off, &v->rows,          4); off += 4;
    memcpy(rec + off, &v->rim_dilation,  4); off += 4;
    memcpy(rec + off, &v->n_points,      4); off += 4;

    uint32_t L;
    L = (uint32_t)n_cells;  memcpy(rec + off, &L, 4); off += 4;
    memcpy(rec + off, v->counts,   n_cells * sizeof(int32_t));  off += n_cells * sizeof(int32_t);
    L = (uint32_t)n_cells;  memcpy(rec + off, &L, 4); off += 4;
    memcpy(rec + off, v->void_ids, n_cells * sizeof(int16_t));  off += n_cells * sizeof(int16_t);
    L = (uint32_t)n_cells;  memcpy(rec + off, &L, 4); off += 4;
    memcpy(rec + off, v->rim_mask, n_cells);                     off += n_cells;

    L = v->n_voids;         memcpy(rec + off, &L, 4); off += 4;
    for (uint32_t i = 0; i < v->n_voids; i++) {
        const v7_dv2_void_summary_t* s = &v->voids[i];
        memset(rec + off, 0, VOID_SUMMARY_BYTES);
        memcpy(rec + off +   0, &s->cell_count, 4);
        memcpy(rec + off +   4, &s->rim_point_count, 4);
        memcpy(rec + off +   8, &s->unowned_rim_point_count, 4);
        memcpy(rec + off +  12, &s->interior_survivor_count, 4);
        memcpy(rec + off +  16, &s->area, 8);
        memcpy(rec + off +  24, &s->cx,   8);
        memcpy(rec + off +  32, &s->cz,   8);
        uint8_t hf = s->has_fit ? 1 : 0;
        rec[off + 40] = hf;
        memcpy(rec + off +  48, s->fit_normal, 24);
        memcpy(rec + off +  72, &s->fit_d,       8);
        memcpy(rec + off +  80, &s->fit_rms,     8);
        memcpy(rec + off +  88, &s->fit_tilt,    8);
        memcpy(rec + off +  96, &s->fit_azimuth, 8);
        memcpy(rec + off + 104, &s->fit_n_candidates, 4);
        memcpy(rec + off + 108, &s->fit_n_inliers,    4);
        off += VOID_SUMMARY_BYTES;
    }

    L = v->n_rim;           memcpy(rec + off, &L, 4); off += 4;
    memcpy(rec + off, v->rim_xy, (size_t)v->n_rim * 2 * sizeof(double));
    off += (size_t)v->n_rim * 2 * sizeof(double);
    L = v->n_rim;           memcpy(rec + off, &L, 4); off += 4;
    for (uint32_t i = 0; i < v->n_rim; i++) {
        memcpy(rec + off + 0, &v->rim_void_ids[i], 2);
        rec[off + 2] = v->rim_inlier[i];
        rec[off + 3] = v->rim_survivor[i];
        rec[off + 4] = v->rim_dist[i];
        rec[off + 5] = v->rim_filtered[i];
        off += RIM_AUX_BYTES;
    }
    int rc = blob_push(out_blob, rec, off);
    free(rec);
    return rc;
}

// ── exec ──

static int v7_density_voids_2_exec(step_context_t* ctx) {
    v7_dv2_params_t params;
    if (parse_params(step_get_params(ctx), &params) != 0) return -1;
    const bool density_auto = (params.density <= 0.0);

    size_t pos_bytes = 0;
    const double* positions = (const double*)step_read(ctx, "lidar-positions", &pos_bytes);
    if (!positions || pos_bytes % (3 * sizeof(double)) != 0) return -1;
    size_t total_points = pos_bytes / (3 * sizeof(double));

    size_t n_buildings = step_array_len(ctx, "per-building-records");
    size_t n_frames    = step_array_len(ctx, "building-local-frames");
    size_t n_normals_arr = step_array_len(ctx, "ts-normals");
    if (n_frames != n_buildings || n_normals_arr != n_buildings) {
        fprintf(stderr,
                "[v7-density-voids-2] count mismatch: bldgs=%zu frames=%zu normals=%zu\n",
                n_buildings, n_frames, n_normals_arr);
        return -1;
    }

    blob_t b_ghost = {0};
    blob_t b_viz   = {0};

    int rc = 0;
    double* local_f64 = NULL;
    float*  local_f32 = NULL;
    size_t  local_cap = 0;
    uint32_t* local_pool = NULL;
    size_t    local_pool_cap = 0;

    for (size_t bi = 0; bi < n_buildings; bi++) {
        const uint8_t* rec = NULL;
        size_t rec_len = 0;
        if (step_array_read_bytes(ctx, "per-building-records", bi, &rec, &rec_len) != 0
            || !rec || rec_len < 68) {
            blob_push(&b_ghost, NULL, 0);
            blob_push(&b_viz,   NULL, 0);
            continue;
        }
        uint32_t schema, n_verts, n_pts;
        memcpy(&schema,  rec + 0, 4);
        memcpy(&n_verts, rec + 4, 4);
        memcpy(&n_pts,   rec + 8, 4);
        size_t fp_bytes = (size_t)n_verts * 3 * sizeof(double);
        size_t idx_off = 68 + fp_bytes;
        if (schema != 1 || idx_off + (size_t)n_pts * sizeof(uint32_t) > rec_len) {
            blob_push(&b_ghost, NULL, 0);
            blob_push(&b_viz,   NULL, 0);
            continue;
        }
        const uint32_t* gidx = (const uint32_t*)(rec + idx_off);

        const uint8_t* frame = NULL;
        size_t frame_len = 0;
        if (step_array_read_bytes(ctx, "building-local-frames", bi,
                                   &frame, &frame_len) != 0
            || !frame || frame_len < BLF_RECORD_BYTES) {
            blob_push(&b_ghost, NULL, 0);
            blob_push(&b_viz,   NULL, 0);
            continue;
        }
        double cx, cy, cz;
        memcpy(&cx, frame +  0, 8);
        memcpy(&cy, frame +  8, 8);
        memcpy(&cz, frame + 16, 8);

        const uint8_t* nrm_bytes = NULL;
        size_t nrm_len = 0;
        if (step_array_read_bytes(ctx, "ts-normals", bi,
                                   &nrm_bytes, &nrm_len) != 0
            || nrm_len != (size_t)n_pts * 3 * sizeof(float)) {
            blob_push(&b_ghost, NULL, 0);
            blob_push(&b_viz,   NULL, 0);
            continue;
        }

        // ── Per-building point-weights from ts-weights ARRAY_INLINE ──
        // REQUIRED — v7_dv2_compute now enforces non-NULL via v7_plane_fit_lsq.
        const uint8_t* w_bytes = NULL; size_t w_len = 0;
        if (step_array_read_bytes(ctx, "ts-weights", bi, &w_bytes, &w_len) != 0
            || !w_bytes || w_len != (size_t)n_pts * sizeof(float)) {
            fprintf(stderr, "[v7-density-voids-2] bldg%zu ts-weights missing or size mismatch (got %zu, want %zu)\n",
                    bi, w_len, (size_t)n_pts * sizeof(float));
            rc = -1; break;
        }
        const float* local_weights = (const float*)w_bytes;

        // Build per-bldg F64 positions (centroid-shifted F32-cast, identical
        // to hemisphere-segment for byte consistency — segment-list LOCAL
        // pt_indices line up with these coords).
        if (v7_gather_building_points_f32_f64(
                positions, total_points, gidx, n_pts, cx, cy, cz,
                &local_f32, &local_f64, &local_cap) != 0) {
            rc = -1; break;
        }

        // Pool = local 0..n_pts-1 (tile-wide on the per-bldg slice).
        if ((size_t)n_pts > local_pool_cap) {
            uint32_t* p = (uint32_t*)realloc(local_pool, (size_t)n_pts * sizeof(uint32_t));
            if (!p) { rc = -1; break; }
            local_pool = p;
            local_pool_cap = n_pts;
        }
        for (uint32_t i = 0; i < n_pts; i++) local_pool[i] = i;

        // Per-building density + distThreshold sourced from
        // bldg-thresholds artifact (single source of truth — see
        // v7-derive-thresholds.c). Pipeline params override per-bldg.
        v7_dv2_params_t bldg_params = params;
        if (density_auto) {
            const uint8_t* th_bytes = NULL; size_t th_len = 0;
            if (step_array_read_bytes(ctx, "bldg-thresholds", bi,
                                       &th_bytes, &th_len) != 0
                || !th_bytes || th_len < V7_BLDG_THRESHOLDS_BYTES) {
                fprintf(stderr, "[v7-density-voids-2] bldg%zu missing bldg-thresholds record\n", bi);
                rc = -1; break;
            }
            v7_bldg_thresholds_t th;
            memcpy(&th, th_bytes, sizeof(th));
            bldg_params.density = th.density;
            // DV2's distThreshold semantically matches the seed-side value
            // (v2 calls deriveDistThreshold(zNoise) here too).
            if (bldg_params.dist_threshold <= 0.0)
                bldg_params.dist_threshold = th.dist_threshold_seed;
        }
        if (getenv("DERIV_LOG")) {
            fprintf(stderr, "[DERIV] step=v7-density-voids-2 scope=bldg%zu n_pts=%u density=%.6f distThreshold=%.6f src_density=%s\n",
                    bi, n_pts, bldg_params.density, bldg_params.dist_threshold,
                    density_auto ? "bldg-thresholds" : "param");
        }

        v7_dv2_outputs_t out;
        v7_dv2_outputs_init(&out);
        if (v7_dv2_compute(local_f64,
                            (const float*)nrm_bytes,
                            local_weights,
                            (size_t)n_pts,
                            local_pool, n_pts,
                            &bldg_params, &out) != 0) {
            v7_dv2_outputs_free(&out);
            rc = -1; break;
        }

        // Pack per-bldg ghost-planes-2 sub-blob.
        blob_t per_bldg_ghost = {0};
        for (size_t i = 0; i < out.n_ghost_planes; i++) {
            if (pack_ghost_plane(&out.ghost_planes[i], &per_bldg_ghost) != 0) {
                blob_free(&per_bldg_ghost);
                v7_dv2_outputs_free(&out);
                rc = -1; goto cleanup;
            }
        }
        blob_push(&b_ghost, per_bldg_ghost.bytes, per_bldg_ghost.len);
        blob_free(&per_bldg_ghost);

        // Pack 2D viz blob (gated).
        if (params.emit_viz_artifacts) {
            blob_t per_bldg_viz = {0};
            if (pack_viz_2d(out.viz_2d, &per_bldg_viz) != 0) {
                blob_free(&per_bldg_viz);
                v7_dv2_outputs_free(&out);
                rc = -1; goto cleanup;
            }
            // Per-building viz blob = its single packed record.
            blob_push(&b_viz, per_bldg_viz.bytes, per_bldg_viz.len);
            blob_free(&per_bldg_viz);
        } else {
            blob_push(&b_viz, NULL, 0);
        }

        v7_dv2_outputs_free(&out);
    }

cleanup:
    free(local_f32);
    free(local_f64);
    free(local_pool);

    if (rc == 0)                              rc |= emit_blob(ctx, "dv2-ghost-planes",    &b_ghost);
    if (rc == 0 && params.emit_viz_artifacts) rc |= emit_blob(ctx, "dv2-viz-2d", &b_viz);

    blob_free(&b_ghost);
    blob_free(&b_viz);
    return rc;
}

// ── Step descriptor ──

static const step_read_t v7_density_voids_2_reads[] = {
    {.name = "lidar-positions"},
    {.name = "per-building-records",  .array = true},
    {.name = "building-local-frames", .array = true},
    {.name = "ts-normals",          .array = true},
    {.name = "ts-weights",          .array = true},
    {.name = "bldg-thresholds",     .array = true},
};
static const step_write_t v7_density_voids_2_writes[] = {
    {.name = "dv2-ghost-planes",     .kind = AK_ARRAY_INLINE, .visibility = STEP_VIS_PUBLIC, .array = true},
    {.name = "dv2-viz-2d", .kind = AK_ARRAY_INLINE, .visibility = STEP_VIS_DEBUG,  .array = true},
};

const step_def_t v7_density_voids_2_def = {
    .name = "v7-density-voids-2",
    // v2 (2026-05-16): per-bldg density auto-compute (sentinel -1 in
    //                   parse_params). Default 25.0 dropped — same fix as
    //                   v7-grow-refit-merge so DV2's cell-size matches the
    //                   actual per-bldg point set.
    .version = 5,  // v5 (2026-06-01): post-RANSAC plane FIT now UNWEIGHTED
                   //     (passes v7_fit_weights() → NULL). RANSAC was already uniform.
                   // v4: ts-weights wired through (no silent NULL fallback);
                   //     v7_dv2_compute now requires non-NULL point_weights.
                   // v3: inner blob_push framing → 8-byte aligned (see <v3/blob.h>)
    .reads = v7_density_voids_2_reads,
    .n_reads = sizeof(v7_density_voids_2_reads) / sizeof(v7_density_voids_2_reads[0]),
    .writes = v7_density_voids_2_writes,
    .n_writes = sizeof(v7_density_voids_2_writes) / sizeof(v7_density_voids_2_writes[0]),
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = v7_density_voids_2_exec,
};

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-tile-signals
// Reads:  lidar-positions          (F64 interleaved x,y,z, L93)
//         per-building-records     (ARRAY_INLINE — global indices per building)
//         building-local-frames    (ARRAY_INLINE — centroid + bbox per building)
//         building-kd-trees        (ARRAY_INLINE — pre-built kd-tree per building)
// Writes: 13 per-building ARRAY_INLINE outputs:
//           eigenvalue-l1, eigenvalue-l2, eigenvalue-l3
//           planarity, linearity, scattering
//           curvatures-k8, curvatures-k16, curvatures-k32
//           tile-normals       (Float32 3*n_pts, raw computed normals)
//           eigen-reliability
//           point-weights
//           eigen-ternary       (Uint8 3*n_pts, RGB packing of lin/plan/scat)
//
// Direct port of bench-v2 lib/pipeline/steps/tile-signals.js (meta.version=10).
// Numerical algorithm lives in v7-tile-signals-impl.{h,c} (callable both
// from this step wrapper and the standalone v7-tile-signals-bench CLI).
//
// Phase 5.1c — kd-tree build was factored out into `build-building-kd-trees`
// (mirrors v2's transient `'kd-tree'` artifact). Building centroid + bbox
// were factored out of extract-buildings into `building-local-frames`.
// This step now consumes both, eliminating the redundant per-pipeline
// kd-tree builds the v7 chain would otherwise do (each downstream step
// would have built its own).
//
// Per-building processing: gather positions from lidar-positions via the
// global indices in per-building-records, subtract the centroid from
// building-local-frames, F32-cast for v2-precision parity, deserialise the
// kd-tree from building-kd-trees, run the pure compute. Pack each output
// into the corresponding ARRAY_INLINE blob (one variable-length record per
// building, prefixed by a u32 length).

#include <jansson.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v3/artifact.h"
#include "v3/json_int.h"
#include "v3/blob.h"
#include "v3/step.h"

#include "kd_tree.h"
#include "v7-steps.h"
#include "v7-tile-signals-impl.h"
#include "v7-perbldg-gather.h"

// building-local-frames record layout (matches build-building-kd-trees +
// extract-buildings emitter): 9 × F64 = 72 bytes/record, fixed-stride.
//   [0..23]  centroid_xyz (L93)
//   [24..47] bbox_min_xyz (L93, points bbox)
//   [48..71] bbox_max_xyz (L93)
#define BLF_RECORD_BYTES 72

// per-building-records header (matches the schema produced by extract-buildings,
// see e.g. polyfit/polyfit-extract-planes.cpp). We only need n_verts and
// n_pts to locate the inlier-indices array.
typedef struct pbr_header {
    uint32_t schema;     // expected = 1
    uint32_t n_verts;    // footprint vertex count
    uint32_t n_pts;      // inlier point count
    // ... 56 bytes of additional header (centroid, bbox, ...) we ignore.
} pbr_header_t;

static int parse_params(const char* params_str, v7_ts_params_t* p) {
    v7_ts_params_default(p);
    // v3 lidar-positions are LAZ frame (east, north, alt) — altitude is z.
    p->up_axis = 2;
    if (!params_str) return 0;
    json_error_t je;
    json_t* obj = json_loads(params_str, 0, &je);
    if (!obj) return -1;
    json_t* j;
    // Frama-C audit pass 4: replace silent (int)json_integer_value casts
    // with json_int_or_default — strict range-check; out-of-range fails the
    // run rather than truncating and silently producing wrong results.
    j = json_object_get(obj, "eigenK");
    if (json_int_or_default(j, 1, 256, "eigenK", &p->eigen_k) != 0) {
        json_decref(obj); return -1;
    }
    if ((j = json_object_get(obj, "eigenMaxRadius")) && json_is_number(j))
        p->max_radius = json_number_value(j);
    if ((j = json_object_get(obj, "eigenAngularBandwidth")) && json_is_number(j))
        p->angular_bandwidth = json_number_value(j);
    if ((j = json_object_get(obj, "eigenSpatialLinearityThreshold")) && json_is_number(j))
        p->linearity_threshold = json_number_value(j);
    j = json_object_get(obj, "weightK");
    if (json_int_or_default(j, 1, 256, "weightK", &p->weight_k) != 0) {
        json_decref(obj); return -1;
    }
    j = json_object_get(obj, "upAxis");
    if (json_int_or_default(j, 0, 2, "upAxis", &p->up_axis) != 0) {
        json_decref(obj); return -1;
    }
    json_decref(obj);
    return 0;
}

// Push 13 empty placeholders so all output blobs stay aligned with bi.
static void push_empty_all(blob_t* b_l1, blob_t* b_l2, blob_t* b_l3,
                           blob_t* b_plan, blob_t* b_lin, blob_t* b_scat,
                           blob_t* b_c8, blob_t* b_c16, blob_t* b_c32,
                           blob_t* b_norm, blob_t* b_rel, blob_t* b_pw,
                           blob_t* b_tern) {
    blob_push(b_l1, NULL, 0); blob_push(b_l2, NULL, 0); blob_push(b_l3, NULL, 0);
    blob_push(b_plan, NULL, 0); blob_push(b_lin, NULL, 0); blob_push(b_scat, NULL, 0);
    blob_push(b_c8, NULL, 0); blob_push(b_c16, NULL, 0); blob_push(b_c32, NULL, 0);
    blob_push(b_norm, NULL, 0); blob_push(b_rel, NULL, 0); blob_push(b_pw, NULL, 0);
    blob_push(b_tern, NULL, 0);
}

static int v7_tile_signals_exec(step_context_t* ctx) {
    v7_ts_params_t params;
    if (parse_params(step_get_params(ctx), &params) != 0) return -1;

    size_t pos_bytes = 0;
    const double* positions = (const double*)step_read(ctx, "lidar-positions", &pos_bytes);
    if (!positions || pos_bytes % (3 * sizeof(double)) != 0) return -1;
    size_t total_points = pos_bytes / (3 * sizeof(double));

    size_t n_buildings  = step_array_len(ctx, "per-building-records");
    size_t n_frames     = step_array_len(ctx, "building-local-frames");
    size_t n_kd_trees   = step_array_len(ctx, "building-kd-trees");
    if (n_frames != n_buildings || n_kd_trees != n_buildings) {
        fprintf(stderr,
                "[v7-tile-signals] n_buildings=%zu but n_frames=%zu, n_trees=%zu — "
                "extract-buildings + build-building-kd-trees must run first.\n",
                n_buildings, n_frames, n_kd_trees);
        return -1;
    }

    blob_t b_l1 = {0}, b_l2 = {0}, b_l3 = {0};
    blob_t b_plan = {0}, b_lin = {0}, b_scat = {0};
    blob_t b_c8 = {0}, b_c16 = {0}, b_c32 = {0};
    blob_t b_norm = {0}, b_rel = {0}, b_pw = {0}, b_tern = {0};

    int rc = 0;
    float* local_f32 = NULL;
    double* local_f64 = NULL;
    size_t local_cap = 0;

    for (size_t bi = 0; bi < n_buildings; bi++) {
        const uint8_t* rec_bytes = NULL;
        size_t rec_len = 0;
        if (step_array_read_bytes(ctx, "per-building-records", bi,
                                   &rec_bytes, &rec_len) != 0
            || !rec_bytes || rec_len < 68) {
            push_empty_all(&b_l1,&b_l2,&b_l3,&b_plan,&b_lin,&b_scat,
                           &b_c8,&b_c16,&b_c32,&b_norm,&b_rel,&b_pw,&b_tern);
            continue;
        }
        uint32_t schema, n_verts, n_pts;
        memcpy(&schema,  rec_bytes + 0, 4);
        memcpy(&n_verts, rec_bytes + 4, 4);
        memcpy(&n_pts,   rec_bytes + 8, 4);
        size_t fp_bytes = (size_t)n_verts * 3 * sizeof(double);
        size_t idx_off = 68 + fp_bytes;
        if (schema != 1 || idx_off + (size_t)n_pts * sizeof(uint32_t) > rec_len) {
            push_empty_all(&b_l1,&b_l2,&b_l3,&b_plan,&b_lin,&b_scat,
                           &b_c8,&b_c16,&b_c32,&b_norm,&b_rel,&b_pw,&b_tern);
            continue;
        }
        const uint32_t* gidx = (const uint32_t*)(rec_bytes + idx_off);

        // Centroid from building-local-frames.
        const uint8_t* frame_bytes = NULL;
        size_t frame_len = 0;
        if (step_array_read_bytes(ctx, "building-local-frames", bi,
                                   &frame_bytes, &frame_len) != 0
            || !frame_bytes || frame_len < BLF_RECORD_BYTES) {
            push_empty_all(&b_l1,&b_l2,&b_l3,&b_plan,&b_lin,&b_scat,
                           &b_c8,&b_c16,&b_c32,&b_norm,&b_rel,&b_pw,&b_tern);
            continue;
        }
        double cx, cy, cz;
        memcpy(&cx, frame_bytes +  0, 8);
        memcpy(&cy, frame_bytes +  8, 8);
        memcpy(&cz, frame_bytes + 16, 8);

        // Pre-built kd-tree node array for this building.
        const uint8_t* tree_bytes = NULL;
        size_t tree_len = 0;
        if (step_array_read_bytes(ctx, "building-kd-trees", bi,
                                   &tree_bytes, &tree_len) != 0) {
            push_empty_all(&b_l1,&b_l2,&b_l3,&b_plan,&b_lin,&b_scat,
                           &b_c8,&b_c16,&b_c32,&b_norm,&b_rel,&b_pw,&b_tern);
            continue;
        }

        // Gather building points + centroid-shift + F32-cast for v2-precision
        // parity. The kd-tree was built on the F32-truncated F64 values
        // produced here (or rather, by build-building-kd-trees applying the
        // identical transform on identical inputs) — same byte values.
        if (v7_gather_building_points_f32_f64(
                positions, total_points, gidx, n_pts, cx, cy, cz,
                &local_f32, &local_f64, &local_cap) != 0) {
            rc = -1; break;
        }

        kd_tree_t tree;
        if (kd_deserialize(&tree, local_f64, n_pts, tree_bytes, tree_len) != 0) {
            fprintf(stderr,
                    "[v7-tile-signals] bi=%zu: kd_deserialize failed (n_pts=%u, "
                    "tree_len=%zu, expected=%zu)\n",
                    bi, n_pts, tree_len, (size_t)n_pts * sizeof(kd_node_t));
            rc = -1;
            break;
        }

        v7_ts_outputs_t out;
        v7_ts_outputs_init(&out);
        if (v7_ts_compute(local_f64, n_pts, &tree, &params, &out) != 0) {
            v7_ts_outputs_free(&out);
            kd_free(&tree);
            rc = -1;
            break;
        }
        kd_free(&tree); // borrowed nodes — no-op besides clearing

        size_t f1 = (size_t)n_pts * sizeof(float);
        size_t f3 = (size_t)n_pts * 3 * sizeof(float);
        size_t u3 = (size_t)n_pts * 3;
        blob_push(&b_l1,   out.eigenvalue_l1,    f1);
        blob_push(&b_l2,   out.eigenvalue_l2,    f1);
        blob_push(&b_l3,   out.eigenvalue_l3,    f1);
        blob_push(&b_plan, out.planarity,        f1);
        blob_push(&b_lin,  out.linearity,        f1);
        blob_push(&b_scat, out.scattering,       f1);
        blob_push(&b_c8,   out.curvatures_k8,    f1);
        blob_push(&b_c16,  out.curvatures_k16,   f1);
        blob_push(&b_c32,  out.curvatures_k32,   f1);
        blob_push(&b_norm, out.tile_normals,     f3);
        blob_push(&b_rel,  out.eigen_reliability, f1);
        blob_push(&b_pw,   out.point_weights,    f1);
        blob_push(&b_tern, out.eigen_ternary,    u3);

        v7_ts_outputs_free(&out);
    }
    free(local_f32);
    free(local_f64);

    // ts-lidar-density was emitted here briefly (2026-05-16) but never
    // consumed in earnest. v3's per-building architecture computes
    // density per-bldg in v7-derive-thresholds → bldg-thresholds, which
    // is the single source of truth. Removed 2026-05-22 to avoid
    // miswiring (tile-wide density mis-scales per-bldg algorithms by
    // sqrt(tile_area / bldg_area)).

    if (rc == 0) rc |= emit_blob(ctx, "ts-eigen-l1",     &b_l1);
    if (rc == 0) rc |= emit_blob(ctx, "ts-eigen-l2",     &b_l2);
    if (rc == 0) rc |= emit_blob(ctx, "ts-eigen-l3",     &b_l3);
    if (rc == 0) rc |= emit_blob(ctx, "ts-planarity",         &b_plan);
    if (rc == 0) rc |= emit_blob(ctx, "ts-linearity",         &b_lin);
    if (rc == 0) rc |= emit_blob(ctx, "ts-scattering",        &b_scat);
    if (rc == 0) rc |= emit_blob(ctx, "ts-curv-k8",     &b_c8);
    if (rc == 0) rc |= emit_blob(ctx, "ts-curv-k16",    &b_c16);
    if (rc == 0) rc |= emit_blob(ctx, "ts-curv-k32",    &b_c32);
    if (rc == 0) rc |= emit_blob(ctx, "ts-normals",      &b_norm);
    if (rc == 0) rc |= emit_blob(ctx, "ts-eigen-rel", &b_rel);
    if (rc == 0) rc |= emit_blob(ctx, "ts-weights",     &b_pw);
    if (rc == 0) rc |= emit_blob(ctx, "ts-eigen-ternary",     &b_tern);

    blob_free(&b_l1); blob_free(&b_l2); blob_free(&b_l3);
    blob_free(&b_plan); blob_free(&b_lin); blob_free(&b_scat);
    blob_free(&b_c8); blob_free(&b_c16); blob_free(&b_c32);
    blob_free(&b_norm); blob_free(&b_rel); blob_free(&b_pw); blob_free(&b_tern);

    return rc;
}

static const step_read_t v7_tile_signals_reads[] = {
    {.name = "lidar-positions"},
    {.name = "per-building-records",  .array = true},
    {.name = "building-local-frames", .array = true},
    {.name = "building-kd-trees",     .array = true},
};
static const step_write_t v7_tile_signals_writes[] = {
    {.name = "ts-eigen-l1",     .kind = AK_ARRAY_INLINE, .visibility = STEP_VIS_DEBUG, .array = true},
    {.name = "ts-eigen-l2",     .kind = AK_ARRAY_INLINE, .visibility = STEP_VIS_DEBUG, .array = true},
    {.name = "ts-eigen-l3",     .kind = AK_ARRAY_INLINE, .visibility = STEP_VIS_DEBUG, .array = true},
    {.name = "ts-planarity",         .kind = AK_ARRAY_INLINE, .visibility = STEP_VIS_DEBUG, .array = true},
    {.name = "ts-linearity",         .kind = AK_ARRAY_INLINE, .visibility = STEP_VIS_DEBUG, .array = true},
    {.name = "ts-scattering",        .kind = AK_ARRAY_INLINE, .visibility = STEP_VIS_DEBUG, .array = true},
    {.name = "ts-curv-k8",     .kind = AK_ARRAY_INLINE, .visibility = STEP_VIS_DEBUG, .array = true},
    {.name = "ts-curv-k16",    .kind = AK_ARRAY_INLINE, .visibility = STEP_VIS_DEBUG, .array = true},
    {.name = "ts-curv-k32",    .kind = AK_ARRAY_INLINE, .visibility = STEP_VIS_DEBUG, .array = true},
    {.name = "ts-normals",      .kind = AK_ARRAY_INLINE, .visibility = STEP_VIS_PUBLIC, .array = true},
    {.name = "ts-eigen-rel", .kind = AK_ARRAY_INLINE, .visibility = STEP_VIS_DEBUG, .array = true},
    {.name = "ts-weights",     .kind = AK_ARRAY_INLINE, .visibility = STEP_VIS_DEBUG, .array = true},
    {.name = "ts-eigen-ternary",     .kind = AK_ARRAY_INLINE, .visibility = STEP_VIS_DEBUG, .array = true},
};

const step_def_t v7_tile_signals_def = {
    .name = "v7-tile-signals",
    // v2 (Phase 5.1c): reads building-local-frames + building-kd-trees instead
    //                  of computing centroid + kd-tree internally.
    // v3 (Phase 5.1d): ARRAY_INLINE writer now pads records to 8-byte
    //                  alignment — output bytes change → version bump.
    // v4 (2026-05-16): emit ts-lidar-density (mirrors v2 lidar-density);
    //                  v7-grow-refit-merge now requires it.
    // v5 (2026-05-22): drop ts-lidar-density — vestigial (never consumed
    //                  by GRM in practice; per-bldg density comes from
    //                  v7-derive-thresholds → bldg-thresholds).
    .version = 5,
    .reads = v7_tile_signals_reads,
    .n_reads = sizeof(v7_tile_signals_reads) / sizeof(v7_tile_signals_reads[0]),
    .writes = v7_tile_signals_writes,
    .n_writes = sizeof(v7_tile_signals_writes) / sizeof(v7_tile_signals_writes[0]),
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = v7_tile_signals_exec,
};

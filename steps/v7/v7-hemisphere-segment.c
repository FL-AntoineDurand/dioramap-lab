// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-hemisphere-segment
// Reads:  lidar-positions          (F64 LAZ, interleaved x,y,z)
//         per-building-records     (ARRAY_INLINE — global indices per building)
//         building-local-frames    (ARRAY_INLINE — centroid per building)
//         building-kd-trees        (ARRAY_INLINE — kd-tree node array per building)
//         tile-normals             (ARRAY_INLINE — F32×3·n_pts per building)
//         point-weights            (ARRAY_INLINE — F32×n_pts per building)
//         (optional) owner         (ARRAY_INLINE — Int16×n_pts per building)
// Writes (always):
//         cascade-segments         (ARRAY_INLINE — Int16×n_pts per building)
//         cascade-segment-list     (ARRAY_INLINE — variable-length per building)
//         ridge-candidates         (ARRAY_INLINE — Uint8×n_pts per building)
// (owner is NOT emitted by this step — hemisphere-segment doesn't
//  populate it; downstream `grow-refit-merge` will be its first emitter.
//  v2 re-emitted owner here defensively because its pipeline runs in
//  object-graph mode and missing artifacts crash; v3's content-addressed
//  store doesn't need that.)
// Writes (gated on emitVizArtifacts param, default true):
//         cascade-lambert-grid     (ARRAY_INLINE — F64 N×N per building)
//         cascade-lambert-peaks    (ARRAY_INLINE — fixed-stride peak records per building)
//         cascade-offset-kde       (ARRAY_INLINE — variable-length KDE records per building)
//
// Direct port of bench-v2 lib/pipeline/steps/hemisphere-normals-density-aware
// -segment.js (meta.version=5).
//
// See PHASE-5-V7-PORT.md "Policy" section for the load-bearing-vs-viz
// classification we apply here.

#define _POSIX_C_SOURCE 200809L
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
#include "v7-hemisphere-segment-impl.h"
#include "v7-perbldg-gather.h"
#include "v7-steps.h"

#define BLF_RECORD_BYTES 72

// ── Params ──

static int parse_params(const char* params_str, v7_hs_params_t* p) {
    v7_hs_params_default(p);
    p->up_axis = 2; // v3 LAZ pipeline z-up
    if (!params_str) return 0;
    json_error_t je;
    json_t* obj = json_loads(params_str, 0, &je);
    if (!obj) return -1;
    json_t* j;
    j = json_object_get(obj, "lambertResolution");
    if (json_int32_or_default(j, 8, 1024, "lambertResolution", &p->lambert_resolution) != 0)
        { json_decref(obj); return -1; }
    if ((j = json_object_get(obj, "lambertSmoothSigma")) && json_is_number(j))
        p->lambert_smooth_sigma = json_number_value(j);
    if ((j = json_object_get(obj, "lambertMinBasinArea")) && json_is_number(j))
        p->lambert_min_basin_area = json_number_value(j);
    if ((j = json_object_get(obj, "stage2Mode")) && json_is_string(j))
        p->stage2_iqr_bandwidth = (strcmp(json_string_value(j), "kde-iqr") == 0) ? 1.0 : 0.0;
    if ((j = json_object_get(obj, "connectRadiusScale")) && json_is_number(j))
        p->connect_radius_scale = json_number_value(j);
    if ((j = json_object_get(obj, "connectRadiusMax")) && json_is_number(j))
        p->connect_radius_max = json_number_value(j);
    if ((j = json_object_get(obj, "minSegmentArea")) && json_is_number(j))
        p->min_segment_area = json_number_value(j);
    j = json_object_get(obj, "upAxis");
    if (json_int32_or_default(j, 0, 2, "upAxis", &p->up_axis) != 0)
        { json_decref(obj); return -1; }
    if ((j = json_object_get(obj, "emitVizArtifacts")) && json_is_boolean(j))
        p->emit_viz_artifacts = json_is_true(j);
    json_decref(obj);
    return 0;
}

// ── Pack a v7_hs_segment as a flat record (matches segment-record schema
// in v7-hemisphere-segment-impl.h docstring) ──

static int pack_segment_record(const v7_hs_segment_t* s,
                                blob_t* out_blob) {
    // Layout:
    //   u32 schema = 1
    //   u32 normal_group_id
    //   u32 offset_cluster_id
    //   u32 spatial_cluster_id
    //   f64 peak_offset
    //   f64 mean_normal[3]
    //   u32 n_pts
    //   u32 reserved (=0)
    //   u32 pt_idx[n_pts]
    size_t header = 16 + 32 + 8;     // 4×u32 + 4×f64 + 2×u32 = 56
    size_t rec_len = header + (size_t)s->n_pts * 4;
    uint8_t* rec = (uint8_t*)malloc(rec_len);
    if (!rec) return -1;
    uint32_t schema = 1;
    uint32_t reserved = 0;
    size_t off = 0;
    memcpy(rec + off, &schema, 4); off += 4;
    uint32_t ng = (uint32_t)s->normal_group_id;
    memcpy(rec + off, &ng, 4); off += 4;
    uint32_t oc = (uint32_t)s->offset_cluster_id;
    memcpy(rec + off, &oc, 4); off += 4;
    uint32_t sc = (uint32_t)s->spatial_cluster_id;
    memcpy(rec + off, &sc, 4); off += 4;
    memcpy(rec + off, &s->peak_offset, 8); off += 8;
    memcpy(rec + off, s->mean_normal, 24); off += 24;
    memcpy(rec + off, &s->n_pts, 4); off += 4;
    memcpy(rec + off, &reserved, 4); off += 4;
    memcpy(rec + off, s->pt_indices, (size_t)s->n_pts * 4);
    int rc = blob_push(out_blob, rec, rec_len);
    free(rec);
    return rc;
}

// ── exec ──

static int v7_hemisphere_segment_exec(step_context_t* ctx) {
    v7_hs_params_t params;
    if (parse_params(step_get_params(ctx), &params) != 0) return -1;

    size_t pos_bytes = 0;
    const double* positions = (const double*)step_read(ctx, "lidar-positions", &pos_bytes);
    if (!positions || pos_bytes % (3 * sizeof(double)) != 0) return -1;
    size_t total_points = pos_bytes / (3 * sizeof(double));

    size_t n_buildings = step_array_len(ctx, "per-building-records");
    size_t n_frames    = step_array_len(ctx, "building-local-frames");
    size_t n_kd_trees  = step_array_len(ctx, "building-kd-trees");
    size_t n_normals_arr   = step_array_len(ctx, "ts-normals");
    size_t n_weights_arr   = step_array_len(ctx, "ts-weights");
    if (n_frames != n_buildings || n_kd_trees != n_buildings
        || n_normals_arr != n_buildings || n_weights_arr != n_buildings) {
        fprintf(stderr,
                "[v7-hemisphere-segment] count mismatch: buildings=%zu frames=%zu "
                "trees=%zu normals=%zu weights=%zu\n",
                n_buildings, n_frames, n_kd_trees, n_normals_arr, n_weights_arr);
        return -1;
    }

    blob_t b_segs   = {0}, b_seg_list = {0};
    blob_t b_ridge  = {0};
    blob_t b_grid   = {0}, b_lpeaks = {0}, b_kde = {0};

    int rc = 0;
    float* local_f32 = NULL;
    double* local_f64 = NULL;
    size_t local_cap = 0;

    for (size_t bi = 0; bi < n_buildings; bi++) {
        const uint8_t* rec = NULL;
        size_t rec_len = 0;
        if (step_array_read_bytes(ctx, "per-building-records", bi, &rec, &rec_len) != 0
            || !rec || rec_len < 68) {
            blob_push(&b_segs, NULL, 0); blob_push(&b_seg_list, NULL, 0);
            blob_push(&b_ridge, NULL, 0);
            if (params.emit_viz_artifacts) {
                blob_push(&b_grid, NULL, 0); blob_push(&b_lpeaks, NULL, 0);
                blob_push(&b_kde, NULL, 0);
            }
            continue;
        }
        uint32_t schema, n_verts, n_pts;
        memcpy(&schema, rec + 0, 4);
        memcpy(&n_verts, rec + 4, 4);
        memcpy(&n_pts, rec + 8, 4);
        size_t fp_bytes = (size_t)n_verts * 3 * sizeof(double);
        size_t idx_off = 68 + fp_bytes;
        if (schema != 1 || idx_off + (size_t)n_pts * sizeof(uint32_t) > rec_len) {
            blob_push(&b_segs, NULL, 0); blob_push(&b_seg_list, NULL, 0);
            blob_push(&b_ridge, NULL, 0);
            if (params.emit_viz_artifacts) {
                blob_push(&b_grid, NULL, 0); blob_push(&b_lpeaks, NULL, 0);
                blob_push(&b_kde, NULL, 0);
            }
            continue;
        }
        const uint32_t* gidx = (const uint32_t*)(rec + idx_off);

        // Centroid from frames.
        const uint8_t* frame = NULL;
        size_t frame_len = 0;
        if (step_array_read_bytes(ctx, "building-local-frames", bi,
                                   &frame, &frame_len) != 0
            || !frame || frame_len < BLF_RECORD_BYTES) {
            blob_push(&b_segs, NULL, 0); blob_push(&b_seg_list, NULL, 0);
            blob_push(&b_ridge, NULL, 0);
            if (params.emit_viz_artifacts) {
                blob_push(&b_grid, NULL, 0); blob_push(&b_lpeaks, NULL, 0);
                blob_push(&b_kde, NULL, 0);
            }
            continue;
        }
        double cx, cy, cz;
        memcpy(&cx, frame +  0, 8);
        memcpy(&cy, frame +  8, 8);
        memcpy(&cz, frame + 16, 8);

        // kd-tree node array.
        const uint8_t* tree_bytes = NULL;
        size_t tree_len = 0;
        if (step_array_read_bytes(ctx, "building-kd-trees", bi,
                                   &tree_bytes, &tree_len) != 0) {
            blob_push(&b_segs, NULL, 0); blob_push(&b_seg_list, NULL, 0);
            blob_push(&b_ridge, NULL, 0);
            if (params.emit_viz_artifacts) {
                blob_push(&b_grid, NULL, 0); blob_push(&b_lpeaks, NULL, 0);
                blob_push(&b_kde, NULL, 0);
            }
            continue;
        }

        // Per-building tile-normals (raw F32×3·n_pts) and point-weights (F32×n_pts).
        const uint8_t* normals_bytes = NULL;
        size_t normals_len = 0;
        const uint8_t* weights_bytes = NULL;
        size_t weights_len = 0;
        if (step_array_read_bytes(ctx, "ts-normals", bi,
                                   &normals_bytes, &normals_len) != 0
            || step_array_read_bytes(ctx, "ts-weights", bi,
                                      &weights_bytes, &weights_len) != 0
            || normals_len != (size_t)n_pts * 3 * sizeof(float)
            || weights_len != (size_t)n_pts * sizeof(float)) {
            blob_push(&b_segs, NULL, 0); blob_push(&b_seg_list, NULL, 0);
            blob_push(&b_ridge, NULL, 0);
            if (params.emit_viz_artifacts) {
                blob_push(&b_grid, NULL, 0); blob_push(&b_lpeaks, NULL, 0);
                blob_push(&b_kde, NULL, 0);
            }
            continue;
        }

        // Gather building positions, centroid-shift, F32-cast, F64-promote
        // (matches build-building-kd-trees so the deserialised tree's node
        // indices resolve to identical coord values).
        if (v7_gather_building_points_f32_f64(
                positions, total_points, gidx, n_pts, cx, cy, cz,
                &local_f32, &local_f64, &local_cap) != 0) {
            rc = -1; break;
        }

        kd_tree_t tree;
        if (kd_deserialize(&tree, local_f64, n_pts, tree_bytes, tree_len) != 0) {
            rc = -1;
            break;
        }

        v7_hs_outputs_t out;
        v7_hs_outputs_init(&out);
        if (v7_hs_compute(local_f64,
                           (const float*)normals_bytes,
                           (const float*)weights_bytes,
                           n_pts, &tree, NULL, &params, &out) != 0) {
            v7_hs_outputs_free(&out);
            kd_free(&tree);
            rc = -1;
            break;
        }
        kd_free(&tree);

        // Emit per-building records.
        size_t i16_len = (size_t)n_pts * sizeof(int16_t);
        blob_push(&b_segs, out.cascade_segments, i16_len);
        blob_push(&b_ridge, out.ridge_candidates, (size_t)n_pts);

        // segment-list as a sub-blob: each sub-record = one segment.
        blob_t per_bldg_seg_blob = {0};
        for (size_t i = 0; i < out.n_segments; i++) {
            pack_segment_record(&out.segments[i], &per_bldg_seg_blob);
        }
        // Wrap the per-building seg-blob as one record in the outer blob.
        blob_push(&b_seg_list, per_bldg_seg_blob.bytes, per_bldg_seg_blob.len);
        blob_free(&per_bldg_seg_blob);

        if (params.emit_viz_artifacts) {
            // Lambert grid — raw F64 N×N (caller knows N from params).
            size_t grid_sz = out.lambert_grid
                ? (size_t)out.lambert_grid_resolution * out.lambert_grid_resolution * sizeof(double)
                : 0;
            blob_push(&b_grid, out.lambert_grid, grid_sz);
            // Lambert peaks — fixed-stride records, packed.
            size_t lp_len = out.n_lambert_peaks * sizeof(v7_hs_lambert_peak_record_t);
            blob_push(&b_lpeaks, out.lambert_peaks, lp_len);
            // Offset KDE — pre-built blob is already length-prefixed records;
            // we wrap the whole thing as one outer record per building.
            blob_push(&b_kde, out.offset_kde_blob, out.offset_kde_blob_len);
        }

        v7_hs_outputs_free(&out);
    }
    free(local_f32);
    free(local_f64);

    if (rc == 0) rc |= emit_blob(ctx, "hs-segments",     &b_segs);
    if (rc == 0) rc |= emit_blob(ctx, "hs-segment-list", &b_seg_list);
    if (rc == 0) rc |= emit_blob(ctx, "hs-ridge-candidates",     &b_ridge);
    if (rc == 0 && params.emit_viz_artifacts) {
        // Emit order doesn't matter — lab-ui's inspector auto-rerenders
        // step-visualizers when sibling artifacts they requested via
        // getSiblingDecoded() arrive later (Phase 5.2-followup-E). Cache-
        // hit runs that deliver artifacts in arbitrary order work the
        // same way.
        if (rc == 0) rc |= emit_blob(ctx, "hs-lambert-grid",  &b_grid);
        if (rc == 0) rc |= emit_blob(ctx, "hs-lambert-peaks", &b_lpeaks);
        if (rc == 0) rc |= emit_blob(ctx, "hs-offset-kde",    &b_kde);
    }

    blob_free(&b_segs); blob_free(&b_seg_list);
    blob_free(&b_ridge);
    blob_free(&b_grid); blob_free(&b_lpeaks); blob_free(&b_kde);

    return rc;
}

static const step_read_t v7_hemisphere_segment_reads[] = {
    {.name = "lidar-positions"},
    {.name = "per-building-records",  .array = true},
    {.name = "building-local-frames", .array = true},
    {.name = "building-kd-trees",     .array = true},
    {.name = "ts-normals",          .array = true},
    {.name = "ts-weights",         .array = true},
};
static const step_write_t v7_hemisphere_segment_writes[] = {
    {.name = "hs-segments",     .kind = AK_ARRAY_INLINE, .visibility = STEP_VIS_DEBUG, .array = true},
    {.name = "hs-segment-list", .kind = AK_ARRAY_INLINE, .visibility = STEP_VIS_PUBLIC, .array = true},
    {.name = "hs-ridge-candidates",     .kind = AK_ARRAY_INLINE, .visibility = STEP_VIS_DEBUG, .array = true},
    {.name = "hs-lambert-grid", .kind = AK_ARRAY_INLINE, .visibility = STEP_VIS_DEBUG, .array = true},
    {.name = "hs-lambert-peaks", .kind = AK_ARRAY_INLINE, .visibility = STEP_VIS_DEBUG, .array = true},
    {.name = "hs-offset-kde",   .kind = AK_ARRAY_INLINE, .visibility = STEP_VIS_DEBUG, .array = true},
};

const step_def_t v7_hemisphere_segment_def = {
    .name = "v7-hemisphere-segment",
    .version = 3, // v3: inner blob_push framing → 8-byte aligned (see <v3/blob.h>)
    .reads = v7_hemisphere_segment_reads,
    .n_reads = sizeof(v7_hemisphere_segment_reads) / sizeof(v7_hemisphere_segment_reads[0]),
    .writes = v7_hemisphere_segment_writes,
    .n_writes = sizeof(v7_hemisphere_segment_writes) / sizeof(v7_hemisphere_segment_writes[0]),
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = v7_hemisphere_segment_exec,
};

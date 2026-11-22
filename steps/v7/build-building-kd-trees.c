// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// build-building-kd-trees
// Reads:  lidar-positions          (F64 interleaved x,y,z, L93)
//         per-building-records     (ARRAY_INLINE — global indices per building)
//         building-local-frames    (ARRAY_INLINE — centroid per building)
// Writes: building-kd-trees        (ARRAY_INLINE — kd_node_t[] per building)
//
// Phase 5.1c. Mirrors bench-v2's pattern where `tile-signals` emitted a
// transient `'kd-tree'` artifact reused by the entire downstream chain
// (16+ consumers). Splitting it out as its own v3 step:
//   - eliminates redundant O(N log N) builds in every tree-using step
//   - gives v3's content-hash cache a stable handle (same inputs ⇒ same
//     bytes ⇒ free re-runs)
//   - removes a drift surface (no risk of two steps building "almost
//     identical" trees with different tiebreaks)
//
// For each building bi we apply the same gather + centroid-shift +
// Float32-cast transform that v7-tile-signals uses internally, then
// `kd_build` over the resulting F64-from-F32 buffer. Serialising the tree
// is just `memcpy` of the node array (kd_node_t is a flat POD; see
// kd_tree.h). The downstream consumer passes the same gathered+shifted+
// cast buffer to `kd_deserialize` so node.point_idx values resolve to the
// same coordinate values as during the build.
//
// Centroid is read from building-local-frames (emitted by extract-
// buildings) so this step doesn't have to re-traverse the points to
// compute it — both consumers (build-building-kd-trees and v7-tile-
// signals) pull from the same source of truth.

#include <jansson.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v3/artifact.h"
#include "v3/blob.h"
#include "v3/step.h"

#include "kd_tree.h"
#include "v7-steps.h"
#include "v7-perbldg-gather.h"

#define BLF_RECORD_BYTES 72

static int exec_fn(step_context_t* ctx) {
    size_t pos_bytes = 0;
    const double* positions =
        (const double*)step_read(ctx, "lidar-positions", &pos_bytes);
    if (!positions || pos_bytes % (3 * sizeof(double)) != 0) return -1;
    size_t total_points = pos_bytes / (3 * sizeof(double));

    size_t n_buildings = step_array_len(ctx, "per-building-records");
    size_t n_frames    = step_array_len(ctx, "building-local-frames");
    if (n_frames != n_buildings) {
        fprintf(stderr,
                "[build-building-kd-trees] frames=%zu vs buildings=%zu\n",
                n_frames, n_buildings);
        return -1;
    }

    blob_t out = {0};
    int rc = 0;
    float*  local_f32 = NULL;
    double* local_f64 = NULL;
    size_t  local_cap = 0;

    for (size_t bi = 0; bi < n_buildings; bi++) {
        const uint8_t* rec_bytes = NULL;
        size_t rec_len = 0;
        if (step_array_read_bytes(ctx, "per-building-records", bi,
                                   &rec_bytes, &rec_len) != 0
            || !rec_bytes || rec_len < 68) {
            blob_push(&out, NULL, 0);
            continue;
        }
        uint32_t schema, n_verts, n_pts;
        memcpy(&schema,  rec_bytes + 0, 4);
        memcpy(&n_verts, rec_bytes + 4, 4);
        memcpy(&n_pts,   rec_bytes + 8, 4);
        size_t fp_bytes = (size_t)n_verts * 3 * sizeof(double);
        size_t idx_off = 68 + fp_bytes;
        if (schema != 1 || idx_off + (size_t)n_pts * sizeof(uint32_t) > rec_len) {
            blob_push(&out, NULL, 0);
            continue;
        }
        const uint32_t* gidx = (const uint32_t*)(rec_bytes + idx_off);

        const uint8_t* frame = NULL;
        size_t frame_len = 0;
        if (step_array_read_bytes(ctx, "building-local-frames", bi,
                                   &frame, &frame_len) != 0
            || !frame || frame_len < BLF_RECORD_BYTES) {
            blob_push(&out, NULL, 0);
            continue;
        }
        double cx, cy, cz;
        memcpy(&cx, frame +  0, 8);
        memcpy(&cy, frame +  8, 8);
        memcpy(&cz, frame + 16, 8);

        if (n_pts == 0) { blob_push(&out, NULL, 0); continue; }

        if (v7_gather_building_points_f32_f64(
                positions, total_points, gidx, n_pts, cx, cy, cz,
                &local_f32, &local_f64, &local_cap) != 0) {
            rc = -1; break;
        }

        kd_tree_t tree;
        if (kd_build(&tree, local_f64, n_pts) != 0) {
            rc = -1;
            break;
        }
        uint8_t* tree_bytes = NULL;
        size_t   tree_len = 0;
        if (kd_serialize(&tree, &tree_bytes, &tree_len) != 0) {
            kd_free(&tree);
            rc = -1;
            break;
        }
        kd_free(&tree);

        if (blob_push(&out, tree_bytes, tree_len) != 0) {
            free(tree_bytes);
            rc = -1;
            break;
        }
        free(tree_bytes);
    }
    free(local_f32);
    free(local_f64);

    if (rc == 0) rc = emit_blob(ctx, "building-kd-trees", &out);
    blob_free(&out);
    return rc;
}

static const step_read_t reads_arr[] = {
    {.name = "lidar-positions"},
    {.name = "per-building-records",  .array = true},
    {.name = "building-local-frames", .array = true},
};
static const step_write_t writes_arr[] = {
    {.name = "building-kd-trees",
     .kind = AK_ARRAY_INLINE, .visibility = STEP_VIS_DEBUG, .array = true},
};

const step_def_t build_building_kd_trees_def = {
    .name = "build-building-kd-trees",
    .version = 2, // 5.1d: ARRAY_INLINE 8-byte aligned format
    .reads = reads_arr,
    .n_reads = sizeof(reads_arr) / sizeof(reads_arr[0]),
    .writes = writes_arr,
    .n_writes = sizeof(writes_arr) / sizeof(writes_arr[0]),
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};

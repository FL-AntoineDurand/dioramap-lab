// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// laz-extract-disc
// Reads:  copc-raw-tiles (ARRAY_BY_HASH of LAZ blobs)
//         canonical-location (JSON with center_l93_{x,y}, radius_m, bbox_l93)
// Writes: lidar-positions       — F64, interleaved (x,y,z) in L93 metres
//                                 (STREAMING — emitted in CHUNK_POINTS-sized
//                                 chunks via step_write_output_stream so the
//                                 viewer can render a first preview within
//                                 ~500ms — PLAN-VIEWER-PORT §7)
//         lidar-classifications — U8 per point
//         lidar-intensities     — U16 per point
//         lidar-num-returns     — U8 per point
//
// Loops internally over array elements (§6 of PLAN-VIEWER-PORT). For each
// tile blob: mmap via store_lookup, pass to copc-lib via dioramap_lazperf,
// filter to disc, append to shared output buffers. Emits one combined
// point cloud per column — downstream sees no tile seams.
//
// Streaming protocol for `lidar-positions`: after each tile's points land
// in the pointbuf, while there's at least CHUNK_POINTS new points available,
// emit a chunk pointing into pb.xyz at the high-water mark. After the last
// tile, the remaining tail (possibly < CHUNK_POINTS) goes out as the final
// chunk. The other three columns stay non-streaming (one write per column
// at end of exec) — multiplexing four parallel streams over WS would add
// complexity without helping first-pixel latency.

#include <jansson.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "v3/artifact.h"
#include "v3/lazperf.h"
#include "v3/store.h"

#include "copc-steps.h"

static int exec_fn(step_context_t* ctx) {
    // --- canonical-location → disc center + radius + bbox ---
    size_t loc_len = 0;
    const char* loc_bytes = (const char*)step_read(ctx, "canonical-location", &loc_len);
    if (!loc_bytes)
        return -1;
    json_error_t je;
    json_t* loc = json_loadb(loc_bytes, loc_len, 0, &je);
    if (!loc)
        return -1;
    double disc_cx = json_number_value(json_object_get(loc, "center_l93_x"));
    double disc_cy = json_number_value(json_object_get(loc, "center_l93_y"));
    double radius = json_number_value(json_object_get(loc, "radius_m"));
    json_t* j_bbox = json_object_get(loc, "bbox_l93");
    if (!json_is_array(j_bbox) || json_array_size(j_bbox) != 4) {
        json_decref(loc);
        return -1;
    }
    double bbox[4];
    for (size_t i = 0; i < 4; i++)
        bbox[i] = json_number_value(json_array_get(j_bbox, i));
    json_decref(loc);

    // --- loop over the by-hash array; each element is a full LAZ tile ---
    size_t n_tiles = step_array_len(ctx, "copc-raw-tiles");
    if (n_tiles == 0)
        return -1;

    store_t* store = step_runner_store(ctx);
    laz_pointbuf_t pb;
    laz_pointbuf_init(&pb);

    // Stream `lidar-positions` in chunks of CHUNK_POINTS. Calibrated against
    // the JS bench's POINTS_PREVIEW cadence: 20k points × 3 doubles = 480 KB
    // per chunk → small enough to render in one rAF tick, large enough that
    // chunk overhead stays under 1% of total bytes for a 1M-point disc.
    enum { CHUNK_POINTS = 20000 };
    size_t emitted_points = 0;

    int rc = 0;
    for (size_t i = 0; i < n_tiles; i++) {
        store_handle_t h;
        if (step_array_open(ctx, "copc-raw-tiles", i, &h) != 0) {
            rc = -1;
            break;
        }
        int one = laz_extract_disc_from_tile(h.bytes, h.len, bbox, disc_cx, disc_cy,
                                             radius, /*max_depth=*/8, &pb);
        store_release(store, &h);
        if (one != 0) {
            rc = -1;
            break;
        }
        // Emit any whole CHUNK_POINTS-sized chunks accumulated so far.
        while (rc == 0 && pb.n_points - emitted_points >= CHUNK_POINTS) {
            const double* base = pb.xyz + emitted_points * 3;
            size_t bytes_to_emit = (size_t)CHUNK_POINTS * 3 * sizeof(double);
            // Non-final chunks; the runner's accumulator copies from the
            // pointer, so a subsequent realloc inside the next tile's
            // extract is safe.
            rc = step_write_output_stream(ctx, "lidar-positions", base, bytes_to_emit, false);
            if (rc == 0) emitted_points += CHUNK_POINTS;
        }
    }

    // Final chunk: the tail of the pointbuf (possibly empty if the total
    // happened to be a multiple of CHUNK_POINTS — still send final=true so
    // the artifact gets committed).
    if (rc == 0) {
        const double* tail_base = pb.xyz + emitted_points * 3;
        size_t tail_points = pb.n_points - emitted_points;
        size_t tail_bytes = tail_points * 3 * sizeof(double);
        rc = step_write_output_stream(ctx, "lidar-positions", tail_base, tail_bytes, true);
    }

    if (rc == 0) {
        size_t bytes = pb.n_points * sizeof(uint8_t);
        rc = step_write_output_owned(ctx, "lidar-classifications", pb.classifications, bytes);
        if (rc == 0) {
            pb.classifications = NULL;
        }
    }
    if (rc == 0) {
        size_t bytes = pb.n_points * sizeof(uint16_t);
        rc = step_write_output_owned(ctx, "lidar-intensities", (uint8_t*)pb.intensities, bytes);
        if (rc == 0) {
            pb.intensities = NULL;
        }
    }
    if (rc == 0) {
        size_t bytes = pb.n_points * sizeof(uint8_t);
        rc = step_write_output_owned(ctx, "lidar-num-returns", pb.num_returns, bytes);
        if (rc == 0) {
            pb.num_returns = NULL;
        }
    }
    laz_pointbuf_free(&pb);
    return rc;
}

static const step_read_t reads_arr[] = {
    {.name = "copc-raw-tiles", .array = true},
    {.name = "canonical-location"},
};
static const step_write_t writes_arr[] = {
    // Per-point columns: raw bytes, one column per artifact. Kind carries
    // the semantic type; the payload dtype is implicit in the step name
    // (F64 for positions, U8 for classifications, U16 for intensities, U8
    // for num-returns). Downstream steps know the dtype by reads[] name.
    {.name = "lidar-positions",
     .kind = AK_POINT_COLUMN,
     .visibility = STEP_VIS_PUBLIC,
     .streaming = true},
    {.name = "lidar-classifications",
     .kind = AK_POINT_COLUMN,
     .visibility = STEP_VIS_PUBLIC},
    {.name = "lidar-intensities",
     .kind = AK_POINT_COLUMN,
     .visibility = STEP_VIS_PUBLIC},
    {.name = "lidar-num-returns",
     .kind = AK_POINT_COLUMN,
     .visibility = STEP_VIS_PUBLIC},
};

const step_def_t laz_extract_disc_def = {
    .name = "laz-extract-disc",
    .version = 1,
    .reads = reads_arr,
    .n_reads = 2,
    .writes = writes_arr,
    .n_writes = 4,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};

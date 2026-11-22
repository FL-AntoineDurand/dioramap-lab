// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// build-lidar-ground
// Reads:  lidar-positions, lidar-classifications, canonical-location
// Params: { "cell_size_m": float (default 1.0) }
// Writes: lidar-ground-grid — 1-channel F32 raster: per-cell minimum Z
//         restricted to ASPRS class-2 (ground) points, with empty cells
//         filled by BFS from their nearest non-empty neighbour.
//
// IGN LiDAR HD ships pre-classified points; strictly accepting only
// class 2 keeps low/medium/high vegetation, buildings, water and noise
// out of the per-cell min-Z. (Earlier versions filtered by the custom
// canopy-flags classifier, which only catches tall leafy canopy and
// leaves shrubs / winter trees / rooflines free to contaminate the
// ground surface.) Cells with no ground hit get the nearest non-empty
// neighbour's value via 4-connected BFS; the resulting raster is fully
// dense and bilinear-samplable for downstream parcel draping
// (build-parcel-lines).

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "v3/artifact.h"
#include "v3/safe_alloc.h"
#include "v3/step.h"
#include "v3/step_bbox_params.h"

#include "../registration/raster_format.h"
#include "ground-steps.h"

static int exec_fn(step_context_t* ctx) {
    double cell_size = step_parse_cell_size_m(step_get_params(ctx), 1.0);

    double bbox[4];
    if (step_read_bbox_l93(ctx, bbox) != 0) return -1;

    size_t pos_bytes = 0;
    const double* positions = (const double*)step_read(ctx, "lidar-positions", &pos_bytes);
    if (!positions || pos_bytes % (3 * sizeof(double)) != 0) return -1;
    size_t N = pos_bytes / (3 * sizeof(double));

    size_t cls_bytes = 0;
    const uint8_t* classes =
        (const uint8_t*)step_read(ctx, "lidar-classifications", &cls_bytes);
    if (!classes || cls_bytes != N) return -1;

    double width_m = bbox[2] - bbox[0];
    double height_m = bbox[3] - bbox[1];
    if (!(width_m > 0) || !(height_m > 0)) return -1;
    int64_t cols_i = (int64_t)round(width_m / cell_size);
    int64_t rows_i = (int64_t)round(height_m / cell_size);
    if (cols_i < 2) cols_i = 2;
    if (rows_i < 2) rows_i = 2;
    if (cols_i > 65535 || rows_i > 65535) return -1;
    uint32_t cols = (uint32_t)cols_i;
    uint32_t rows = (uint32_t)rows_i;
    size_t cells = (size_t)cols * rows;

    // Bin non-canopy points; track per-cell min Z.
    float* min_z = safe_malloc_array(cells, sizeof(float));
    if (!min_z) return -1;
    for (size_t i = 0; i < cells; i++) min_z[i] = NAN;

    double xmin = bbox[0];
    double ymin = bbox[1];
    double inv_cell = 1.0 / cell_size;

    for (size_t i = 0; i < N; i++) {
        if (classes[i] != 2) continue;
        double x = positions[i * 3 + 0];
        double y = positions[i * 3 + 1];
        float z = (float)positions[i * 3 + 2];
        int64_t c = (int64_t)floor((x - xmin) * inv_cell);
        int64_t r = (int64_t)floor((y - ymin) * inv_cell);
        if (c < 0 || c >= (int64_t)cols || r < 0 || r >= (int64_t)rows) continue;
        size_t k = (size_t)r * cols + (size_t)c;
        // NaN-safe min: NaN >= z is false → first valid z replaces NaN.
        // Subsequent: only smaller values overwrite.
        if (!(z >= min_z[k])) min_z[k] = z;
    }

    // BFS-fill missing cells with the nearest non-empty neighbour. Seed
    // queue with all non-NaN cells, expand 4-neighbours.
    uint8_t* visited = calloc(cells, 1);
    size_t* queue = safe_malloc_array(cells, sizeof(size_t));
    if (!visited || !queue) {
        free(min_z); free(visited); free(queue);
        return -1;
    }
    size_t qh = 0, qt = 0;
    for (size_t k = 0; k < cells; k++) {
        if (!isnan(min_z[k])) {
            visited[k] = 1;
            queue[qt++] = k;
        }
    }
    while (qh < qt) {
        size_t k = queue[qh++];
        int64_t r = (int64_t)(k / cols);
        int64_t c = (int64_t)(k % cols);
        static const int dirs[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
        for (int d = 0; d < 4; d++) {
            int64_t nr = r + dirs[d][0];
            int64_t nc = c + dirs[d][1];
            if (nr < 0 || nr >= (int64_t)rows || nc < 0 || nc >= (int64_t)cols) continue;
            size_t nk = (size_t)nr * cols + (size_t)nc;
            if (visited[nk]) continue;
            visited[nk] = 1;
            min_z[nk] = min_z[k];
            queue[qt++] = nk;
        }
    }
    free(visited);
    free(queue);

    // If the bin pass produced no valid cells (e.g. every point is canopy),
    // fall back to 0 everywhere — same intent as the bench: better than
    // emitting NaN that downstream samplers will choke on.
    int any_valid = 0;
    for (size_t k = 0; k < cells; k++) {
        if (!isnan(min_z[k])) { any_valid = 1; break; }
    }
    if (!any_valid) {
        for (size_t k = 0; k < cells; k++) min_z[k] = 0.0f;
    }

    uint8_t* buf = raster_alloc(cols, rows, 1);
    if (!buf) {
        free(min_z);
        return -1;
    }
    float* out = raster_channel_ptr(buf, cols, rows, 0);
    memcpy(out, min_z, cells * sizeof(float));
    free(min_z);

    return step_write_output_owned(ctx, "lidar-ground-grid", buf,
                                   raster_total_bytes(cols, rows, 1));
}

static const step_read_t reads_arr[] = {
    {.name = "lidar-positions"},
    {.name = "lidar-classifications"},
    {.name = "canonical-location"},
};
static const step_write_t writes_arr[] = {
    {.name = "lidar-ground-grid", .kind = AK_RASTER_F32, .visibility = STEP_VIS_DEBUG},
};

const step_def_t build_lidar_ground_def = {
    .name = "build-lidar-ground",
    // v2: filter by ASPRS class 2 (ground) instead of the old canopy-flags
    // anti-mask. Catches low/medium veg, buildings and noise that the canopy
    // classifier missed, eliminating the BFS-fill dent under dense forest.
    .version = 2,
    .reads = reads_arr,
    .n_reads = 3,
    .writes = writes_arr,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};

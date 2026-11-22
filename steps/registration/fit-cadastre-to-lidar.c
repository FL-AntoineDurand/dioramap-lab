// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// fit-cadastre-to-lidar
// Reads:  cadastre-edges-raster (1-channel binary mask),
//         lidar-building-mask (1-channel 0/1)
// Params: { cell_size_m: float (default 0.5),
//           search_range_m: float (default 15) }
// Writes: cadastre-fit — JSON { offsetX, offsetY, score, cell_size, cols, rows }
//
// Port of viewer/backend/features/cadastre-registration.js findCadastreOffset.
// The cadastre edge raster is already a binary mask of outlines. The lidar
// building mask is FILLED, so Sobel gives its boundary; adaptive normalise
// then cross-correlate over a 2D offset grid, sub-cell parabola refinement.

#include <jansson.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v3/artifact.h"
#include "v3/safe_alloc.h"
#include "v3/step.h"

#include "edges.h"
#include "raster_format.h"
#include "registration-steps.h"

typedef struct fit_params {
    double cell_size_m;
    double search_range_m;
} fit_params_t;

static void parse_params(const char* params_str, fit_params_t* out) {
    out->cell_size_m = 0.5;
    out->search_range_m = 15.0;
    if (!params_str) return;
    json_error_t je;
    json_t* p = json_loads(params_str, 0, &je);
    if (!p) return;
    json_t* j = json_object_get(p, "cell_size_m");
    if (json_is_number(j)) {
        double v = json_number_value(j);
        if (v > 1e-6 && v < 100.0) out->cell_size_m = v;
    }
    j = json_object_get(p, "search_range_m");
    if (json_is_number(j)) {
        double v = json_number_value(j);
        if (v > 0 && v < 1000.0) out->search_range_m = v;
    }
    json_decref(p);
}

static int exec_fn(step_context_t* ctx) {
    fit_params_t params;
    parse_params(step_get_params(ctx), &params);

    size_t cad_bytes = 0;
    const uint8_t* cad_in = (const uint8_t*)step_read(ctx, "cadastre-edges-raster", &cad_bytes);
    if (!cad_in) return -1;
    raster_view_t cad_v;
    if (raster_view_open(cad_in, cad_bytes, &cad_v) != 0 || cad_v.n_channels != 1) {
        return -1;
    }
    uint32_t cols = cad_v.cols;
    uint32_t rows = cad_v.rows;
    size_t cells = (size_t)cols * rows;

    size_t lm_bytes = 0;
    const uint8_t* lm_in = (const uint8_t*)step_read(ctx, "lidar-building-mask", &lm_bytes);
    if (!lm_in) return -1;
    raster_view_t lm_v;
    if (raster_view_open(lm_in, lm_bytes, &lm_v) != 0 || lm_v.n_channels != 1 ||
        lm_v.cols != cols || lm_v.rows != rows) {
        return -1;
    }

    const float* cad_edges = cad_v.channels[0];
    const float* lidar_mask = lm_v.channels[0];

    // Sobel + adaptive normalise the lidar mask; the cadastre is already an
    // edge mask so it's used directly.
    float* lid_edges = safe_malloc_array(cells, sizeof(float));
    if (!lid_edges) return -1;
    edges_sobel_magnitude(lidar_mask, cols, rows, lid_edges);
    edges_adaptive_normalize(lid_edges, cells);

    int range_cells = (int)round(params.search_range_m / params.cell_size_m);
    if (range_cells < 1) range_cells = 1;
    int score_size = 2 * range_cells + 1;
    size_t map_cells = (size_t)score_size * score_size;
    double* score_map = safe_calloc_array(map_cells, sizeof(double));
    if (!score_map) {
        free(lid_edges);
        return -1;
    }

    double best_score = -INFINITY;
    int best_dx = 0;
    int best_dy = 0;
    for (int dy = -range_cells; dy <= range_cells; dy++) {
        for (int dx = -range_cells; dx <= range_cells; dx++) {
            int r0 = dy > 0 ? dy : 0;
            int r1 = dy < 0 ? (int)rows + dy : (int)rows;
            int c0 = dx > 0 ? dx : 0;
            int c1 = dx < 0 ? (int)cols + dx : (int)cols;
            double s = 0;
            size_t n = 0;
            for (int r = r0; r < r1; r++) {
                size_t lRow = (size_t)r * cols;
                size_t cRow = (size_t)(r - dy) * cols;
                for (int c = c0; c < c1; c++) {
                    s += (double)lid_edges[lRow + c] * (double)cad_edges[cRow + (c - dx)];
                    n++;
                }
            }
            if (n > 0) s /= (double)n;
            size_t idx = (size_t)(dy + range_cells) * score_size + (dx + range_cells);
            score_map[idx] = s;
            if (s > best_score) {
                best_score = s;
                best_dx = dx;
                best_dy = dy;
            }
        }
    }

    // Sub-cell parabola refinement.
    double sub_dx = best_dx;
    double sub_dy = best_dy;
    int cx_idx = best_dx + range_cells;
    int cy_idx = best_dy + range_cells;
    if (cx_idx > 0 && cx_idx < score_size - 1) {
        double L = score_map[(size_t)cy_idx * score_size + (cx_idx - 1)];
        double C = score_map[(size_t)cy_idx * score_size + cx_idx];
        double R = score_map[(size_t)cy_idx * score_size + (cx_idx + 1)];
        double denom = L - 2.0 * C + R;
        if (fabs(denom) > 1e-9) sub_dx = (double)best_dx + (L - R) / (2.0 * denom);
    }
    if (cy_idx > 0 && cy_idx < score_size - 1) {
        double T = score_map[(size_t)(cy_idx - 1) * score_size + cx_idx];
        double C = score_map[(size_t)cy_idx * score_size + cx_idx];
        double B = score_map[(size_t)(cy_idx + 1) * score_size + cx_idx];
        double denom = T - 2.0 * C + B;
        if (fabs(denom) > 1e-9) sub_dy = (double)best_dy + (T - B) / (2.0 * denom);
    }

    free(lid_edges);
    free(score_map);

    double offset_x = sub_dx * params.cell_size_m;
    double offset_y = sub_dy * params.cell_size_m;

    json_t* root = json_object();
    json_object_set_new(root, "offsetX", json_real(offset_x));
    json_object_set_new(root, "offsetY", json_real(offset_y));
    json_object_set_new(root, "score", json_real(best_score));
    json_object_set_new(root, "cell_size", json_real(params.cell_size_m));
    json_object_set_new(root, "cols", json_integer(cols));
    json_object_set_new(root, "rows", json_integer(rows));
    char* s = json_dumps(root, JSON_COMPACT);
    json_decref(root);
    if (!s) return -1;
    size_t slen = strlen(s);
    return step_write_output_owned(ctx, "cadastre-fit", (uint8_t*)s, slen);
}

static const step_read_t reads_arr[] = {
    {.name = "cadastre-edges-raster"},
    {.name = "lidar-building-mask"},
};
static const step_write_t writes_arr[] = {
    {.name = "cadastre-fit", .kind = AK_OPAQUE_JSON, .visibility = STEP_VIS_PUBLIC},
};

const step_def_t fit_cadastre_to_lidar_def = {
    .name = "fit-cadastre-to-lidar",
    .version = 1,
    .reads = reads_arr,
    .n_reads = 2,
    .writes = writes_arr,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};

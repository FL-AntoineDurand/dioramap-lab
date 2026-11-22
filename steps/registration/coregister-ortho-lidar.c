// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// coregister-ortho-lidar
// Reads:  lidar-rasters (7 channels), lidar-ndsm (1), lidar-building-mask (1),
//         ortho-rgb-rasters (7), ortho-irc-rasters (2), canonical-location
// Params: { cell_size_m: float (default 0.5), search_range_m: float (default 5),
//           weights: { A, AStar, B, C, D } (optional, defaults from bench) }
// Writes: coreg-offset — JSON { offsetX, offsetY, score, cell_size, cols, rows,
//                                pair_scores: [{ name, score, weight }, ...] }
//
// Multi-pair Sobel edge correlation. Each pair scores a LiDAR-side F32 raster
// against a matching ortho-side F32 raster via Sobel → adaptive normalisation
// → edge-weighted cross-correlation. The weighted sum peaks at the true (dx,
// dy) offset. Sub-cell parabola fit refines to ≈0.1-cell precision.
//
// Direct port of bench-v2 viewer/backend/registration/ortho-lidar.js
// runOnePass. Same pair configuration, same weights, same math.

#include <jansson.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v3/artifact.h"
#include "v3/safe_alloc.h"
#include "v3/step.h"

#include "../ortho/ortho-steps.h"
#include "edges.h"
#include "raster_format.h"
#include "registration-steps.h"

typedef struct coreg_weights {
    double A;
    double AStar;
    double B;
    double C;
    double D;
} coreg_weights_t;

static const coreg_weights_t DEFAULT_WEIGHTS = {
    .A = 0.30, .AStar = 0.20, .B = 0.25, .C = 0.20, .D = 0.05,
};

typedef struct coreg_params {
    double cell_size_m;
    double search_range_m;
    coreg_weights_t weights;
} coreg_params_t;

static void parse_params(const char* params_str, coreg_params_t* out) {
    out->cell_size_m = 0.5;
    out->search_range_m = 5.0;
    out->weights = DEFAULT_WEIGHTS;
    if (!params_str) {
        return;
    }
    json_error_t je;
    json_t* p = json_loads(params_str, 0, &je);
    if (!p) {
        return;
    }
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
    json_t* w = json_object_get(p, "weights");
    if (json_is_object(w)) {
        json_t* wi;
        wi = json_object_get(w, "A");
        if (json_is_number(wi)) out->weights.A = json_number_value(wi);
        wi = json_object_get(w, "AStar");
        if (json_is_number(wi)) out->weights.AStar = json_number_value(wi);
        wi = json_object_get(w, "B");
        if (json_is_number(wi)) out->weights.B = json_number_value(wi);
        wi = json_object_get(w, "C");
        if (json_is_number(wi)) out->weights.C = json_number_value(wi);
        wi = json_object_get(w, "D");
        if (json_is_number(wi)) out->weights.D = json_number_value(wi);
    }
    json_decref(p);
}

// One layer pair after edge + normalisation prep.
typedef struct prepared_pair {
    char name[64];
    float* lidar_edges;   // owned
    float* ortho_edges;   // owned
    double weight;
    double best_score;    // filled after search
} prepared_pair_t;

static void free_pairs(prepared_pair_t* pairs, size_t n) {
    for (size_t i = 0; i < n; i++) {
        free(pairs[i].lidar_edges);
        free(pairs[i].ortho_edges);
    }
    free(pairs);
}

// Prepare one pair: fillNoData → sobel → mask → normalise, on both sides.
// Returns 0 on success; caller still needs to free lidar_edges / ortho_edges
// on failure.
static int prepare_pair(prepared_pair_t* pp, const float* lidar_src, const float* ortho_src,
                        uint32_t cols, uint32_t rows, bool mask_no_data, int margin,
                        double weight, const char* name) {
    size_t cells = (size_t)cols * rows;
    snprintf(pp->name, sizeof(pp->name), "%s", name);
    pp->weight = weight;
    pp->best_score = 0;
    pp->lidar_edges = NULL;
    pp->ortho_edges = NULL;

    float* l_filled = edges_fill_nodata(lidar_src, cells,
                                        mask_no_data ? NAN : 0.0f);
    float* o_filled = edges_fill_nodata(ortho_src, cells, NAN);
    if (!l_filled || !o_filled) {
        free(l_filled); free(o_filled);
        return -1;
    }
    pp->lidar_edges = safe_malloc_array(cells, sizeof(float));
    pp->ortho_edges = safe_malloc_array(cells, sizeof(float));
    if (!pp->lidar_edges || !pp->ortho_edges) {
        free(l_filled); free(o_filled);
        return -1;
    }
    edges_sobel_magnitude(l_filled, cols, rows, pp->lidar_edges);
    edges_sobel_magnitude(o_filled, cols, rows, pp->ortho_edges);
    if (mask_no_data) {
        edges_mask_boundaries(pp->lidar_edges, lidar_src, cols, rows, margin);
        edges_mask_boundaries(pp->ortho_edges, ortho_src, cols, rows, margin);
    } else {
        edges_mask_border_only(pp->lidar_edges, cols, rows, margin);
        edges_mask_border_only(pp->ortho_edges, cols, rows, margin);
    }
    edges_adaptive_normalize(pp->lidar_edges, cells);
    edges_adaptive_normalize(pp->ortho_edges, cells);
    free(l_filled);
    free(o_filled);
    return 0;
}

// Helper: open a raster read, validate its shape, return a view.
// Sets *out_view if OK; returns 0 on success, -1 otherwise.
static int open_raster_read(step_context_t* ctx, const char* name, uint32_t cols, uint32_t rows,
                            uint32_t min_channels, raster_view_t* out_view) {
    size_t len = 0;
    const uint8_t* bytes = (const uint8_t*)step_read(ctx, name, &len);
    if (!bytes) {
        return -1;
    }
    if (raster_view_open(bytes, len, out_view) != 0) {
        return -1;
    }
    if (out_view->cols != cols || out_view->rows != rows ||
        out_view->n_channels < min_channels) {
        return -1;
    }
    return 0;
}

// Encode result as a compact JSON object.
static char* encode_result_json(double offset_x, double offset_y, double total_score,
                                double cell_size, uint32_t cols, uint32_t rows,
                                const prepared_pair_t* pairs, size_t n_pairs, size_t* out_len) {
    json_t* root = json_object();
    json_object_set_new(root, "offsetX", json_real(offset_x));
    json_object_set_new(root, "offsetY", json_real(offset_y));
    json_object_set_new(root, "score", json_real(total_score));
    json_object_set_new(root, "cell_size", json_real(cell_size));
    json_object_set_new(root, "cols", json_integer(cols));
    json_object_set_new(root, "rows", json_integer(rows));
    json_t* arr = json_array();
    for (size_t i = 0; i < n_pairs; i++) {
        json_t* pj = json_object();
        json_object_set_new(pj, "name", json_string(pairs[i].name));
        json_object_set_new(pj, "score", json_real(pairs[i].best_score));
        json_object_set_new(pj, "weight", json_real(pairs[i].weight));
        json_array_append_new(arr, pj);
    }
    json_object_set_new(root, "pair_scores", arr);
    char* s = json_dumps(root, JSON_COMPACT);
    json_decref(root);
    if (!s) return NULL;
    *out_len = strlen(s);
    return s;
}

static int exec_fn(step_context_t* ctx) {
    coreg_params_t params;
    parse_params(step_get_params(ctx), &params);

    // Read lidar-rasters first to lock in (cols, rows).
    size_t lr_bytes = 0;
    const uint8_t* lr_in = (const uint8_t*)step_read(ctx, "lidar-rasters", &lr_bytes);
    if (!lr_in) return -1;
    raster_view_t lr;
    if (raster_view_open(lr_in, lr_bytes, &lr) != 0 || lr.n_channels < LIDAR_RC__COUNT) {
        return -1;
    }
    uint32_t cols = lr.cols;
    uint32_t rows = lr.rows;
    size_t cells = (size_t)cols * rows;

    raster_view_t ndsm_v, mask_v, orgb_v, oirc_v;
    if (open_raster_read(ctx, "lidar-ndsm", cols, rows, 1, &ndsm_v) != 0) return -1;
    if (open_raster_read(ctx, "lidar-building-mask", cols, rows, 1, &mask_v) != 0) return -1;
    if (open_raster_read(ctx, "ortho-rgb-rasters", cols, rows, ORGB__COUNT, &orgb_v) != 0) return -1;
    if (open_raster_read(ctx, "ortho-irc-rasters", cols, rows, OIRC__COUNT, &oirc_v) != 0) return -1;

    const float* ndsm = ndsm_v.channels[0];
    const float* mask = mask_v.channels[0];
    const float* luminance = orgb_v.channels[ORGB_LUMINANCE];
    const float* lum_std = orgb_v.channels[ORGB_LUM_STDDEV];
    const float* intensity = lr.channels[LIDAR_RC_INTENSITY];
    const float* roughness = lr.channels[LIDAR_RC_ROUGHNESS];
    const float* multi_return = lr.channels[LIDAR_RC_MULTI_RETURN];
    const float* ndvi = oirc_v.channels[OIRC_NDVI];

    // Build NDSM-building-only (NaN where mask == 0).
    float* ndsm_building = safe_malloc_array(cells, sizeof(float));
    if (!ndsm_building) return -1;
    size_t n_building = 0;
    for (size_t i = 0; i < cells; i++) {
        if (mask[i] == 1.0f && !isnan(ndsm[i])) {
            ndsm_building[i] = ndsm[i];
            n_building++;
        } else {
            ndsm_building[i] = NAN;
        }
    }

    // Check if multi-return is usable. If entirely NaN, fall back to pair D.
    bool mr_usable = false;
    for (size_t i = 0; i < cells; i++) {
        if (!isnan(multi_return[i])) {
            mr_usable = true;
            break;
        }
    }

    // Assemble pair list.
    int margin = (int)round(2.0 / params.cell_size_m);
    if (margin < 4) margin = 4;

    // Over-allocate for max 4 pairs (A, A*, B, C or D).
    prepared_pair_t* pairs = calloc(4, sizeof(prepared_pair_t));
    if (!pairs) {
        free(ndsm_building);
        return -1;
    }
    size_t n_pairs = 0;

    // Pair A — NDSM × luminance.
    if (prepare_pair(&pairs[n_pairs], ndsm, luminance, cols, rows, true, margin, params.weights.A,
                     "A (NDSM x luminance)") != 0) {
        free(ndsm_building);
        free_pairs(pairs, 4);
        return -1;
    }
    n_pairs++;

    // Pair A* — building-only × luminance (conditional on sufficient buildings).
    if ((double)n_building / cells > 0.02) {
        if (prepare_pair(&pairs[n_pairs], ndsm_building, luminance, cols, rows, false, margin,
                         params.weights.AStar, "A* (building x luminance)") != 0) {
            free(ndsm_building);
            free_pairs(pairs, 4);
            return -1;
        }
        n_pairs++;
    }

    // Pair B — intensity × luminance.
    if (prepare_pair(&pairs[n_pairs], intensity, luminance, cols, rows, true, margin,
                     params.weights.B, "B (intensity x luminance)") != 0) {
        free(ndsm_building);
        free_pairs(pairs, 4);
        return -1;
    }
    n_pairs++;

    // Pair C (multi-return × NDVI) or fallback D (roughness × lumStddev).
    if (mr_usable) {
        if (prepare_pair(&pairs[n_pairs], multi_return, ndvi, cols, rows, true, margin,
                         params.weights.C, "C (multiReturn x NDVI)") != 0) {
            free(ndsm_building);
            free_pairs(pairs, 4);
            return -1;
        }
    } else {
        if (prepare_pair(&pairs[n_pairs], roughness, lum_std, cols, rows, true, margin,
                         params.weights.C + params.weights.D,
                         "D (roughness x lumStddev)") != 0) {
            free(ndsm_building);
            free_pairs(pairs, 4);
            return -1;
        }
    }
    n_pairs++;

    free(ndsm_building);

    // Search for the best (dx, dy). Centre = (0, 0) cells.
    int range_cells = (int)round(params.search_range_m / params.cell_size_m);
    if (range_cells < 1) range_cells = 1;
    int score_size = 2 * range_cells + 1;
    size_t map_cells = (size_t)score_size * score_size;
    double* total_map = safe_calloc_array(map_cells, sizeof(double));
    // Per-pair score maps: [pair_idx][cell_idx]
    double** per_pair_maps = safe_calloc_array(n_pairs, sizeof(double*));
    if (!total_map || !per_pair_maps) {
        free(total_map); free(per_pair_maps);
        free_pairs(pairs, 4);
        return -1;
    }
    for (size_t p = 0; p < n_pairs; p++) {
        per_pair_maps[p] = safe_calloc_array(map_cells, sizeof(double));
        if (!per_pair_maps[p]) {
            for (size_t j = 0; j < p; j++) free(per_pair_maps[j]);
            free(per_pair_maps);
            free(total_map);
            free_pairs(pairs, 4);
            return -1;
        }
    }

    double best_total = -INFINITY;
    int best_dx_cell = 0;
    int best_dy_cell = 0;

    for (int dy = -range_cells; dy <= range_cells; dy++) {
        for (int dx = -range_cells; dx <= range_cells; dx++) {
            // Overlap window: lidar[r, c] correlates with ortho[r-dy, c-dx].
            int r0 = dy > 0 ? dy : 0;
            int r1 = dy < 0 ? (int)rows + dy : (int)rows;
            int c0 = dx > 0 ? dx : 0;
            int c1 = dx < 0 ? (int)cols + dx : (int)cols;

            double total = 0;
            for (size_t p = 0; p < n_pairs; p++) {
                const float* le = pairs[p].lidar_edges;
                const float* oe = pairs[p].ortho_edges;
                double s = 0;
                size_t n = 0;
                for (int r = r0; r < r1; r++) {
                    size_t e_row = (size_t)r * cols;
                    size_t o_row = (size_t)(r - dy) * cols;
                    for (int c = c0; c < c1; c++) {
                        double a = le[e_row + c];
                        double b = oe[o_row + (c - dx)];
                        s += a * b;
                        n++;
                    }
                }
                if (n > 0) s /= (double)n;
                size_t map_idx = (size_t)(dy + range_cells) * score_size + (dx + range_cells);
                per_pair_maps[p][map_idx] = s;
                total += s * pairs[p].weight;
            }
            total_map[(size_t)(dy + range_cells) * score_size + (dx + range_cells)] = total;
            if (total > best_total) {
                best_total = total;
                best_dx_cell = dx;
                best_dy_cell = dy;
                for (size_t p = 0; p < n_pairs; p++) {
                    pairs[p].best_score = per_pair_maps[p]
                        [(size_t)(dy + range_cells) * score_size + (dx + range_cells)];
                }
            }
        }
    }

    // Sub-cell parabola refinement on the total score map.
    double sub_dx = best_dx_cell;
    double sub_dy = best_dy_cell;
    int cx_idx = best_dx_cell + range_cells;
    int cy_idx = best_dy_cell + range_cells;
    if (cx_idx > 0 && cx_idx < score_size - 1) {
        double L = total_map[(size_t)cy_idx * score_size + (cx_idx - 1)];
        double C = total_map[(size_t)cy_idx * score_size + cx_idx];
        double R = total_map[(size_t)cy_idx * score_size + (cx_idx + 1)];
        double denom = L - 2.0 * C + R;
        if (fabs(denom) > 1e-9) {
            sub_dx = (double)best_dx_cell + (L - R) / (2.0 * denom);
        }
    }
    if (cy_idx > 0 && cy_idx < score_size - 1) {
        double T = total_map[(size_t)(cy_idx - 1) * score_size + cx_idx];
        double C = total_map[(size_t)cy_idx * score_size + cx_idx];
        double B = total_map[(size_t)(cy_idx + 1) * score_size + cx_idx];
        double denom = T - 2.0 * C + B;
        if (fabs(denom) > 1e-9) {
            sub_dy = (double)best_dy_cell + (T - B) / (2.0 * denom);
        }
    }

    double offset_x = sub_dx * params.cell_size_m;
    double offset_y = sub_dy * params.cell_size_m;

    for (size_t p = 0; p < n_pairs; p++) free(per_pair_maps[p]);
    free(per_pair_maps);
    free(total_map);

    size_t json_len = 0;
    char* json_str = encode_result_json(offset_x, offset_y, best_total, params.cell_size_m,
                                        cols, rows, pairs, n_pairs, &json_len);
    free_pairs(pairs, 4);
    if (!json_str) {
        return -1;
    }
    int rc = step_write_output_owned(ctx, "coreg-offset", (uint8_t*)json_str, json_len);
    return rc;
}

static const step_read_t reads_arr[] = {
    {.name = "lidar-rasters"},
    {.name = "lidar-ndsm"},
    {.name = "lidar-building-mask"},
    {.name = "ortho-rgb-rasters"},
    {.name = "ortho-irc-rasters"},
};
static const step_write_t writes_arr[] = {
    {.name = "coreg-offset", .kind = AK_OPAQUE_JSON, .visibility = STEP_VIS_PUBLIC},
};

const step_def_t coregister_ortho_lidar_def = {
    .name = "coregister-ortho-lidar",
    .version = 1,
    .reads = reads_arr,
    .n_reads = 5,
    .writes = writes_arr,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};

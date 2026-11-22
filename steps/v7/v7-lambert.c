// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Lambert grid — direct port of bench-v2 lib/geometry/lambert.js.

#include "v7-lambert.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef V7_PI
#define V7_PI 3.14159265358979323846
#endif

static const double DISC_R = 1.4142135623730951; // sqrt(2)

int lambert_grid_init(lambert_grid_t* g, int32_t resolution, int32_t up_axis) {
    if (!g || resolution <= 0 || up_axis < 0 || up_axis > 2) return -1;
    g->resolution = resolution;
    g->R = DISC_R + 0.05;
    g->cell_size = 2.0 * g->R / (double)resolution;
    g->grid = (double*)calloc((size_t)resolution * resolution, sizeof(double));
    g->disc_mask = NULL;
    g->labels = NULL;
    g->up_axis = up_axis;
    return g->grid ? 0 : -1;
}

void lambert_grid_free(lambert_grid_t* g) {
    if (!g) return;
    free(g->grid);
    free(g->disc_mask);
    free(g->labels);
    g->grid = NULL;
    g->disc_mask = NULL;
    g->labels = NULL;
    g->resolution = 0;
}

// ── Disc mask (lazy) ──

static const uint8_t* get_disc_mask(lambert_grid_t* g) {
    if (g->disc_mask) return g->disc_mask;
    int32_t res = g->resolution;
    g->disc_mask = (uint8_t*)calloc((size_t)res * res, sizeof(uint8_t));
    if (!g->disc_mask) return NULL;
    for (int32_t r = 0; r < res; r++) {
        for (int32_t c = 0; c < res; c++) {
            // Cell centre (u,v).
            double u = (c + 0.5) * g->cell_size - g->R;
            double v = (r + 0.5) * g->cell_size - g->R;
            g->disc_mask[r * res + c] = (u * u + v * v <= DISC_R * DISC_R + 0.01) ? 1 : 0;
        }
    }
    return g->disc_mask;
}

// ── Projection (typed-array form, matches v2's lambertProjectTyped) ──

void lambert_project_typed(const float* normals, size_t index, int32_t up_axis,
                            double* out_u, double* out_v) {
    size_t o = index * 3;
    double n3[3] = {(double)normals[o], (double)normals[o + 1], (double)normals[o + 2]};
    int up = (up_axis >= 0 && up_axis <= 2) ? up_axis : 1;
    // Other two axes — for up_axis=1 (v2 y-up): u from x, v from z.
    // For up_axis=2 (LAZ z-up): u from x, v from y.
    int u_axis, v_axis;
    if (up == 0)      { u_axis = 1; v_axis = 2; }
    else if (up == 1) { u_axis = 0; v_axis = 2; }
    else              { u_axis = 0; v_axis = 1; }

    // Canonicalise: flip all 3 if the up component is negative.
    double sign = n3[up] < 0.0 ? -1.0 : 1.0;
    double nu = n3[u_axis] * sign;
    double nv = n3[v_axis] * sign;
    double nup_v = n3[up] * sign;
    double denom = 1.0 + (nup_v < 0.0 ? 0.0 : nup_v); // v2 lambertProjectTyped uses Math.max(0, ny)
    if (denom < 1e-12) {
        *out_u = 0.0;
        *out_v = 0.0;
        return;
    }
    double k = sqrt(2.0 / denom);
    *out_u = k * nu;
    *out_v = k * nv;
}

// Inverse projection (only used for peak-normal recovery in find_peaks).
static void lambert_inverse(double u, double v, int32_t up_axis,
                             double* out_n) {
    double r_sq = u * u + v * v;
    int up = (up_axis >= 0 && up_axis <= 2) ? up_axis : 1;
    int u_axis, v_axis;
    if (up == 0)      { u_axis = 1; v_axis = 2; }
    else if (up == 1) { u_axis = 0; v_axis = 2; }
    else              { u_axis = 0; v_axis = 1; }

    if (r_sq > 2.0 + 1e-8) {
        // Outside disc — return up-axis fallback.
        out_n[0] = out_n[1] = out_n[2] = 0.0;
        out_n[up] = 1.0;
        return;
    }
    double n_up = 1.0 - r_sq / 2.0;
    if (r_sq < 1e-14) {
        out_n[0] = out_n[1] = out_n[2] = 0.0;
        out_n[up] = 1.0;
        return;
    }
    double factor = sqrt(((1.0 + n_up) / 2.0) > 0.0 ? (1.0 + n_up) / 2.0 : 0.0);
    double nu = u * factor;
    double nv = v * factor;
    double nu_clamped = n_up > 0.0 ? n_up : 0.0;
    double len = sqrt(nu * nu + nu_clamped * nu_clamped + nv * nv);
    if (len <= 0.0) {
        out_n[0] = out_n[1] = out_n[2] = 0.0;
        out_n[up] = 1.0;
        return;
    }
    out_n[u_axis] = nu / len;
    out_n[v_axis] = nv / len;
    out_n[up] = nu_clamped / len;
}

// Tilt (degrees from up-axis). Matches v2 tiltFromNormal but axis-aware.
static double tilt_deg(const double n[3], int up_axis) {
    double up_abs = fabs(n[up_axis]);
    if (up_abs > 1.0) up_abs = 1.0;
    return acos(up_abs) * (180.0 / V7_PI);
}

// Azimuth (degrees, 0=North along v_axis, CW). Matches v2 azimuthFromNormal.
static double azimuth_deg(const double n[3], int up_axis) {
    int u_axis, v_axis;
    if (up_axis == 0)      { u_axis = 1; v_axis = 2; }
    else if (up_axis == 1) { u_axis = 0; v_axis = 2; }
    else                   { u_axis = 0; v_axis = 1; }
    double hx = n[u_axis];
    double hz = n[v_axis];
    if (fabs(hx) < 1e-10 && fabs(hz) < 1e-10) return 0.0;
    double az = atan2(hx, hz) * (180.0 / V7_PI);
    if (az < 0.0) az += 360.0;
    return az;
}

// ── Bilinear splat ──

static void splat_bilinear(lambert_grid_t* g, double u, double v, double w) {
    int32_t res = g->resolution;
    double col = (u + g->R) / g->cell_size;
    double row = (v + g->R) / g->cell_size;

    int32_t c0 = (int32_t)floor(col - 0.5);
    int32_t r0 = (int32_t)floor(row - 0.5);
    double fc = col - 0.5 - (double)c0;
    double fr = row - 0.5 - (double)r0;

    double w00 = (1.0 - fc) * (1.0 - fr);
    double w10 = fc * (1.0 - fr);
    double w01 = (1.0 - fc) * fr;
    double w11 = fc * fr;

    if (c0 >= 0 && c0 < res && r0 >= 0 && r0 < res)
        g->grid[r0 * res + c0] += w * w00;
    if (c0 + 1 >= 0 && c0 + 1 < res && r0 >= 0 && r0 < res)
        g->grid[r0 * res + c0 + 1] += w * w10;
    if (c0 >= 0 && c0 < res && r0 + 1 >= 0 && r0 + 1 < res)
        g->grid[(r0 + 1) * res + c0] += w * w01;
    if (c0 + 1 >= 0 && c0 + 1 < res && r0 + 1 >= 0 && r0 + 1 < res)
        g->grid[(r0 + 1) * res + c0 + 1] += w * w11;
}

void lambert_grid_accumulate(lambert_grid_t* g,
                             const float* normals,
                             const float* weights,
                             const uint32_t* eligible_indices,
                             size_t n_eligible) {
    for (size_t k = 0; k < n_eligible; k++) {
        double w = (double)weights[k];
        if (w <= 0.0) continue;
        size_t idx = eligible_indices ? (size_t)eligible_indices[k] : k;
        double u, v;
        lambert_project_typed(normals, idx, g->up_axis, &u, &v);
        splat_bilinear(g, u, v, w);
    }
}

// ── Gaussian smooth ──

void lambert_grid_gaussian_smooth(lambert_grid_t* g, double sigma) {
    int32_t res = g->resolution;
    const uint8_t* mask = get_disc_mask(g);
    if (!mask) return;

    int32_t k_radius = (int32_t)ceil(3.0 * sigma);
    int32_t k_size = 2 * k_radius + 1;

    double* kernel = (double*)malloc((size_t)k_size * sizeof(double));
    if (!kernel) return;
    double k_sum = 0.0;
    for (int32_t i = 0; i < k_size; i++) {
        double d = (double)(i - k_radius);
        kernel[i] = exp(-0.5 * (d / sigma) * (d / sigma));
        k_sum += kernel[i];
    }
    for (int32_t i = 0; i < k_size; i++) kernel[i] /= k_sum;

    double* tmp = (double*)calloc((size_t)res * res, sizeof(double));
    if (!tmp) { free(kernel); return; }

    // Horizontal pass.
    for (int32_t r = 0; r < res; r++) {
        for (int32_t c = 0; c < res; c++) {
            if (!mask[r * res + c]) continue;
            double sum = 0.0, w_sum = 0.0;
            for (int32_t ki = 0; ki < k_size; ki++) {
                int32_t sc = c + ki - k_radius;
                if (sc >= 0 && sc < res && mask[r * res + sc]) {
                    sum += g->grid[r * res + sc] * kernel[ki];
                    w_sum += kernel[ki];
                }
            }
            tmp[r * res + c] = w_sum > 0.0 ? sum / w_sum : 0.0;
        }
    }

    // Vertical pass — write back into g->grid; cells outside disc → 0.
    for (int32_t c = 0; c < res; c++) {
        for (int32_t r = 0; r < res; r++) {
            if (!mask[r * res + c]) {
                g->grid[r * res + c] = 0.0;
                continue;
            }
            double sum = 0.0, w_sum = 0.0;
            for (int32_t ki = 0; ki < k_size; ki++) {
                int32_t sr = r + ki - k_radius;
                if (sr >= 0 && sr < res && mask[sr * res + c]) {
                    sum += tmp[sr * res + c] * kernel[ki];
                    w_sum += kernel[ki];
                }
            }
            g->grid[r * res + c] = w_sum > 0.0 ? sum / w_sum : 0.0;
        }
    }

    free(tmp);
    free(kernel);
}

// ── Find peaks ──

typedef struct peak_internal {
    int32_t row, col;
    double weight;
    double sub_col, sub_row; // sub-pixel refined
} peak_internal_t;

static int cmp_peak_desc(const void* a, const void* b) {
    double da = ((const lambert_peak_t*)a)->weight_sum;
    double db = ((const lambert_peak_t*)b)->weight_sum;
    if (db > da) return 1;
    if (db < da) return -1;
    return 0;
}

lambert_peak_t* lambert_grid_find_peaks(const lambert_grid_t* g,
                                        double min_weight,
                                        size_t* out_n_peaks) {
    int32_t res = g->resolution;
    const uint8_t* mask = ((lambert_grid_t*)g)->disc_mask
                          ? ((lambert_grid_t*)g)->disc_mask
                          : get_disc_mask((lambert_grid_t*)g);
    if (!mask) {
        *out_n_peaks = 0;
        return NULL;
    }

    // Two passes: count, then fill.
    size_t n_peaks = 0;
    for (int32_t r = 1; r < res - 1; r++) {
        for (int32_t c = 1; c < res - 1; c++) {
            int32_t idx = r * res + c;
            if (!mask[idx]) continue;
            double val = g->grid[idx];
            if (val < min_weight) continue;
            bool is_max = true;
            for (int dr = -1; dr <= 1 && is_max; dr++) {
                for (int dc = -1; dc <= 1 && is_max; dc++) {
                    if (dr == 0 && dc == 0) continue;
                    if (g->grid[(r + dr) * res + (c + dc)] > val) is_max = false;
                }
            }
            if (is_max) n_peaks++;
        }
    }

    if (n_peaks == 0) {
        *out_n_peaks = 0;
        return NULL;
    }

    lambert_peak_t* peaks = (lambert_peak_t*)calloc(n_peaks, sizeof(lambert_peak_t));
    if (!peaks) {
        *out_n_peaks = 0;
        return NULL;
    }

    size_t pi = 0;
    for (int32_t r = 1; r < res - 1; r++) {
        for (int32_t c = 1; c < res - 1; c++) {
            int32_t idx = r * res + c;
            if (!mask[idx]) continue;
            double val = g->grid[idx];
            if (val < min_weight) continue;
            bool is_max = true;
            for (int dr = -1; dr <= 1 && is_max; dr++) {
                for (int dc = -1; dc <= 1 && is_max; dc++) {
                    if (dr == 0 && dc == 0) continue;
                    if (g->grid[(r + dr) * res + (c + dc)] > val) is_max = false;
                }
            }
            if (!is_max) continue;

            // Sub-pixel refinement (quadratic interpolation along each axis).
            double v_l = c > 0 ? g->grid[r * res + c - 1] : val;
            double v_r = c < res - 1 ? g->grid[r * res + c + 1] : val;
            double v_u = r > 0 ? g->grid[(r - 1) * res + c] : val;
            double v_d = r < res - 1 ? g->grid[(r + 1) * res + c] : val;
            double denom_c = 2.0 * (v_l + v_r - 2.0 * val);
            double denom_r = 2.0 * (v_u + v_d - 2.0 * val);
            double d_col = denom_c != 0.0 ? (v_l - v_r) / denom_c : 0.0;
            double d_row = denom_r != 0.0 ? (v_u - v_d) / denom_r : 0.0;
            double peak_col = (double)c + (fabs(d_col) < 1.0 ? d_col : 0.0);
            double peak_row = (double)r + (fabs(d_row) < 1.0 ? d_row : 0.0);

            double u = (peak_col + 0.5) * g->cell_size - g->R;
            double v = (peak_row + 0.5) * g->cell_size - g->R;
            double n3[3];
            lambert_inverse(u, v, g->up_axis, n3);

            peaks[pi].normal_x = n3[0];
            peaks[pi].normal_y = n3[1];
            peaks[pi].normal_z = n3[2];
            peaks[pi].tilt = tilt_deg(n3, g->up_axis);
            peaks[pi].azimuth = azimuth_deg(n3, g->up_axis);
            peaks[pi].weight_sum = val;
            peaks[pi].row = r;
            peaks[pi].col = c;
            pi++;
        }
    }

    qsort(peaks, pi, sizeof(lambert_peak_t), cmp_peak_desc);
    *out_n_peaks = pi;
    return peaks;
}

// ── Watershed ──

static int16_t resolve_label(lambert_grid_t* g, int32_t r, int32_t c,
                              int16_t* labels, const uint8_t* mask,
                              int32_t* visited, int32_t* visited_top) {
    int32_t res = g->resolution;
    if (labels[r * res + c] >= 0) return labels[r * res + c];
    int32_t cr = r, cc = c;
    int32_t local_top = *visited_top;
    while (labels[cr * res + cc] < 0) {
        visited[local_top++] = cr * res + cc;
        int32_t best_r = cr, best_c = cc;
        double best_val = g->grid[cr * res + cc];
        for (int dr = -1; dr <= 1; dr++) {
            for (int dc = -1; dc <= 1; dc++) {
                if (dr == 0 && dc == 0) continue;
                int32_t nr = cr + dr, nc = cc + dc;
                if (nr < 0 || nr >= res || nc < 0 || nc >= res) continue;
                if (!mask[nr * res + nc]) continue;
                if (g->grid[nr * res + nc] > best_val) {
                    best_val = g->grid[nr * res + nc];
                    best_r = nr;
                    best_c = nc;
                }
            }
        }
        if (best_r == cr && best_c == cc) break;
        cr = best_r;
        cc = best_c;
    }
    int16_t label = labels[cr * res + cc];
    for (int32_t i = *visited_top; i < local_top; i++) {
        labels[visited[i]] = label;
    }
    *visited_top = local_top;
    return label;
}

int lambert_grid_build_watershed_labels(lambert_grid_t* g,
                                         const lambert_peak_t* all_peaks,
                                         size_t n_all_peaks) {
    int32_t res = g->resolution;
    const uint8_t* mask = get_disc_mask(g);
    if (!mask) return -1;

    free(g->labels);
    g->labels = (int16_t*)malloc((size_t)res * res * sizeof(int16_t));
    if (!g->labels) return -1;
    for (int32_t i = 0; i < res * res; i++) g->labels[i] = -1;

    for (size_t pi = 0; pi < n_all_peaks; pi++) {
        int32_t r = all_peaks[pi].row, c = all_peaks[pi].col;
        g->labels[r * res + c] = (int16_t)pi;
    }

    int32_t* visited = (int32_t*)malloc((size_t)res * res * sizeof(int32_t));
    if (!visited) return -1;

    for (int32_t r = 0; r < res; r++) {
        for (int32_t c = 0; c < res; c++) {
            if (!mask[r * res + c]) continue;
            if (g->labels[r * res + c] >= 0) continue;
            int32_t top = 0;
            resolve_label(g, r, c, g->labels, mask, visited, &top);
        }
    }

    free(visited);
    return 0;
}

void lambert_grid_assign_watershed(const lambert_grid_t* g,
                                    const float* normals,
                                    const uint32_t* eligible_indices,
                                    size_t n_eligible,
                                    int16_t* out_assignment) {
    int32_t res = g->resolution;
    for (size_t k = 0; k < n_eligible; k++) {
        size_t idx = eligible_indices ? (size_t)eligible_indices[k] : k;
        double u, v;
        lambert_project_typed(normals, idx, g->up_axis, &u, &v);
        int32_t col = (int32_t)floor((u + g->R) / g->cell_size);
        int32_t row = (int32_t)floor((v + g->R) / g->cell_size);
        if (col >= 0 && col < res && row >= 0 && row < res && g->labels) {
            out_assignment[k] = g->labels[row * res + col];
        } else {
            out_assignment[k] = -1;
        }
    }
}

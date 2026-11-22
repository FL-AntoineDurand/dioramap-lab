// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Weighted Gaussian KDE + peak/valley detection + bandwidth selection.

#include "v7-histogram.h"
#include "v3/safe_alloc.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef V7_PI
#define V7_PI 3.14159265358979323846
#endif

static const double SQRT_2PI = 2.5066282746310002;

int v7_weighted_gaussian_kde(const double* values, const double* weights,
                              size_t n, double bandwidth,
                              kde_result_t* out) {
    if (!out || !out->eval_points || !out->densities || out->n_eval == 0) return -1;
    size_t n_eval = out->n_eval;
    if (n == 0) {
        for (size_t e = 0; e < n_eval; e++) {
            out->eval_points[e] = 0.0;
            out->densities[e] = 0.0;
        }
        return 0;
    }

    double total_weight = 0.0;
    for (size_t i = 0; i < n; i++) total_weight += weights[i];
    int uniform_fallback = total_weight < 1e-30;
    if (uniform_fallback) total_weight = (double)n;

    double v_min = INFINITY, v_max = -INFINITY;
    for (size_t i = 0; i < n; i++) {
        if (values[i] < v_min) v_min = values[i];
        if (values[i] > v_max) v_max = values[i];
    }

    double lo = v_min - 3.0 * bandwidth;
    double hi = v_max + 3.0 * bandwidth;
    double step = (hi - lo) / (double)(n_eval - 1);

    double inv_h = 1.0 / bandwidth;

    for (size_t e = 0; e < n_eval; e++) {
        out->eval_points[e] = lo + (double)e * step;
        out->densities[e] = 0.0;
    }

    for (size_t i = 0; i < n; i++) {
        double y_i = values[i];
        double w_i = uniform_fallback ? 1.0 : weights[i];
        for (size_t e = 0; e < n_eval; e++) {
            double u = (out->eval_points[e] - y_i) * inv_h;
            out->densities[e] += w_i * exp(-0.5 * u * u);
        }
    }

    double norm = inv_h / (total_weight * SQRT_2PI);
    for (size_t e = 0; e < n_eval; e++) out->densities[e] *= norm;
    return 0;
}

double v7_silverman_bandwidth(const double* values, size_t n) {
    if (n < 2) return 0.1;
    double sum = 0.0;
    for (size_t i = 0; i < n; i++) sum += values[i];
    double mean = sum / (double)n;
    double sum_sq = 0.0;
    for (size_t i = 0; i < n; i++) {
        double d = values[i] - mean;
        sum_sq += d * d;
    }
    double sigma = sqrt(sum_sq / (double)n);
    if (sigma < 1e-12) return 0.1;
    return 1.06 * sigma * pow((double)n, -0.2);
}

static int cmp_double(const void* a, const void* b) {
    double da = *(const double*)a;
    double db = *(const double*)b;
    return (da > db) - (da < db);
}

double v7_iqr_bandwidth(const double* values, size_t n) {
    if (n < 4) return v7_silverman_bandwidth(values, n);
    double* sorted = (double*)safe_malloc_array(n, sizeof(double));
    if (!sorted) return v7_silverman_bandwidth(values, n);
    memcpy(sorted, values, n * sizeof(double));
    qsort(sorted, n, sizeof(double), cmp_double);
    double q1 = sorted[(size_t)floor((double)n * 0.25)];
    double q3 = sorted[(size_t)floor((double)n * 0.75)];
    double iqr = q3 - q1;
    free(sorted);

    double sum = 0.0;
    for (size_t i = 0; i < n; i++) sum += values[i];
    double mean = sum / (double)n;
    double sum_sq = 0.0;
    for (size_t i = 0; i < n; i++) {
        double d = values[i] - mean;
        sum_sq += d * d;
    }
    double sigma = sqrt(sum_sq / (double)n);

    double sigma_hat = sigma;
    if (iqr > 1e-12 && iqr / 1.34 < sigma) sigma_hat = iqr / 1.34;
    if (sigma_hat < 1e-12) return 0.1;
    return 1.06 * sigma_hat * pow((double)n, -0.2);
}

void v7_find_kde_peaks(const double* eval_points, const double* densities,
                        size_t n_eval, double min_density,
                        kde_peak_t* out_peaks, size_t* out_n_peaks) {
    size_t n_p = 0;
    if (n_eval >= 3) {
        for (size_t i = 1; i < n_eval - 1; i++) {
            if (densities[i] > densities[i - 1] && densities[i] >= densities[i + 1]
                && densities[i] > min_density) {
                out_peaks[n_p].center = eval_points[i];
                out_peaks[n_p].density = densities[i];
                n_p++;
            }
        }
    }
    *out_n_peaks = n_p;
}

void v7_find_kde_valleys(const double* eval_points, const double* densities,
                          size_t n_eval,
                          kde_peak_t* out_valleys, size_t* out_n_valleys) {
    size_t n_v = 0;
    if (n_eval >= 3) {
        for (size_t i = 1; i < n_eval - 1; i++) {
            if (densities[i] < densities[i - 1] && densities[i] <= densities[i + 1]) {
                out_valleys[n_v].center = eval_points[i];
                out_valleys[n_v].density = densities[i];
                n_v++;
            }
        }
    }
    *out_n_valleys = n_v;
}

// Internal: sort_peak_internal_t with original index for stable origIdx
// resolution after sorting.
typedef struct sort_peak {
    double center;
    int16_t orig_idx;
} sort_peak_t;

static int cmp_sort_peak(const void* a, const void* b) {
    double da = ((const sort_peak_t*)a)->center;
    double db = ((const sort_peak_t*)b)->center;
    return (da > db) - (da < db);
}

void v7_assign_points_by_valley(const double* values, size_t n,
                                  const kde_peak_t* peaks, size_t n_peaks,
                                  const kde_peak_t* valleys, size_t n_valleys,
                                  int16_t* out_assignments) {
    if (n_peaks <= 1) {
        for (size_t i = 0; i < n; i++) out_assignments[i] = 0;
        return;
    }

    sort_peak_t* sorted = (sort_peak_t*)safe_malloc_array(n_peaks, sizeof(sort_peak_t));
    if (!sorted) {
        for (size_t i = 0; i < n; i++) out_assignments[i] = 0;
        return;
    }
    for (size_t i = 0; i < n_peaks; i++) {
        sorted[i].center = peaks[i].center;
        sorted[i].orig_idx = (int16_t)i;
    }
    qsort(sorted, n_peaks, sizeof(sort_peak_t), cmp_sort_peak);

    // Find deepest valley between each adjacent sorted-peak pair.
    double* split_points = (double*)malloc((n_peaks - 1) * sizeof(double));
    if (!split_points) {
        free(sorted);
        for (size_t i = 0; i < n; i++) out_assignments[i] = 0;
        return;
    }
    for (size_t pi = 0; pi < n_peaks - 1; pi++) {
        double left_c = sorted[pi].center;
        double right_c = sorted[pi + 1].center;
        const kde_peak_t* best_v = NULL;
        double best_d = INFINITY;
        for (size_t vi = 0; vi < n_valleys; vi++) {
            if (valleys[vi].center > left_c && valleys[vi].center < right_c
                && valleys[vi].density < best_d) {
                best_d = valleys[vi].density;
                best_v = &valleys[vi];
            }
        }
        split_points[pi] = best_v ? best_v->center : (left_c + right_c) / 2.0;
    }

    for (size_t i = 0; i < n; i++) {
        double v = values[i];
        size_t peak_idx = 0;
        for (size_t s = 0; s < n_peaks - 1; s++) {
            if (v > split_points[s]) peak_idx = s + 1;
            else break;
        }
        out_assignments[i] = sorted[peak_idx].orig_idx;
    }

    free(split_points);
    free(sorted);
}

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Weighted Gaussian KDE + peak/valley detection + bandwidth selection.
// Direct port of the bench-v2 lib/geometry/histogram.js subset that
// hemisphere-segment uses (weightedGaussianKDE, findKDEPeaks,
// findKDEValleys, computeSilvermanBandwidth, computeIQRBandwidth,
// assignPointsByValley). Other histogram.js helpers (gaussianKDE,
// circularKDE, classifyBins, adaptiveKDE, posterior assignment) are
// not used in the v7 main path and are not ported.

#ifndef V3_STEPS_V7_HISTOGRAM_H
#define V3_STEPS_V7_HISTOGRAM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// KDE result. Caller-allocated buffers (eval_points, densities), each
// of size n_eval. Computed in [min(values) - 3·h, max(values) + 3·h].
typedef struct kde_result {
    double* eval_points; // F64×n_eval
    double* densities;   // F64×n_eval
    size_t n_eval;
} kde_result_t;

// Weighted Gaussian KDE. Writes into caller-allocated `out` (must have
// out->eval_points and out->densities both of size out->n_eval).
// Returns 0 on success.
int v7_weighted_gaussian_kde(const double* values, const double* weights,
                              size_t n, double bandwidth,
                              kde_result_t* out);

// Silverman's rule-of-thumb bandwidth. h = 1.06·σ·N^(-1/5).
double v7_silverman_bandwidth(const double* values, size_t n);

// IQR-robust Silverman variant. σ̂ = min(σ, IQR/1.34); fallback to
// plain Silverman for n < 4.
double v7_iqr_bandwidth(const double* values, size_t n);

// Find local maxima in (eval_points, densities) above min_density.
// Caller-allocated out_centers/out_densities/out_n_peaks. Buffers
// must have capacity ≥ floor(n_eval/2) (worst-case alternating pattern).
typedef struct kde_peak {
    double center;
    double density;
} kde_peak_t;

void v7_find_kde_peaks(const double* eval_points, const double* densities,
                        size_t n_eval, double min_density,
                        kde_peak_t* out_peaks, size_t* out_n_peaks);

// Find local minima (valleys) — same shape as v7_find_kde_peaks.
void v7_find_kde_valleys(const double* eval_points, const double* densities,
                          size_t n_eval,
                          kde_peak_t* out_valleys, size_t* out_n_valleys);

// Assign each value to a peak via valley splitting. peaks[] contains
// the already-found local maxima; valleys[] the local minima between
// them. Output: assignments[i] = peak index (0..n_peaks-1) for values[i].
// Caller-allocated assignments buffer (size n).
void v7_assign_points_by_valley(const double* values, size_t n,
                                  const kde_peak_t* peaks, size_t n_peaks,
                                  const kde_peak_t* valleys, size_t n_valleys,
                                  int16_t* out_assignments);

#ifdef __cplusplus
}
#endif
#endif

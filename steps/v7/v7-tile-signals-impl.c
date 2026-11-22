// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-tile-signals — pure compute. Direct port of bench-v2 tile-signals.js.

#define _USE_MATH_DEFINES
#include "v7-tile-signals-impl.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "kd_tree.h"   // from steps/normals — bench-v2 lib/geometry/kd-tree.js port
#include "pca.h"       // from steps/normals — bench-v2 lib/geometry/pca.js port

#ifndef V7_PI
#define V7_PI 3.14159265358979323846
#endif

static const int K_STAGES[5] = {32, 48, 64, 96, 128};
static const int N_STAGES = 5;
static const int K_MAX = 128;
static const int CURVATURE_KS[3] = {8, 16, 32};
static const int N_CURVS = 3;

void v7_ts_params_default(v7_ts_params_t* p) {
    p->eigen_k = 16;
    p->max_radius = 1.0;
    p->angular_bandwidth = V7_PI / 8.0;
    p->linearity_threshold = 0.5;
    p->weight_k = 8;
    p->up_axis = 1; // y-up (v2 convention) — keeps bench harness byte-identical
}

void v7_ts_outputs_init(v7_ts_outputs_t* out) {
    memset(out, 0, sizeof(*out));
}

void v7_ts_outputs_free(v7_ts_outputs_t* out) {
    if (!out) return;
    free(out->eigenvalue_l1);
    free(out->eigenvalue_l2);
    free(out->eigenvalue_l3);
    free(out->planarity);
    free(out->linearity);
    free(out->scattering);
    free(out->curvatures_k8);
    free(out->curvatures_k16);
    free(out->curvatures_k32);
    free(out->tile_normals);
    free(out->eigen_reliability);
    free(out->point_weights);
    free(out->eigen_ternary);
    memset(out, 0, sizeof(*out));
}

static int alloc_f32(float** dst, size_t n) {
    *dst = (float*)calloc(n, sizeof(float));
    return *dst ? 0 : -1;
}

static int alloc_outputs(v7_ts_outputs_t* o, size_t n_pts) {
    o->n_pts = n_pts;
    if (n_pts == 0) return 0;
    if (alloc_f32(&o->eigenvalue_l1,    n_pts) < 0) return -1;
    if (alloc_f32(&o->eigenvalue_l2,    n_pts) < 0) return -1;
    if (alloc_f32(&o->eigenvalue_l3,    n_pts) < 0) return -1;
    if (alloc_f32(&o->planarity,        n_pts) < 0) return -1;
    if (alloc_f32(&o->linearity,        n_pts) < 0) return -1;
    if (alloc_f32(&o->scattering,       n_pts) < 0) return -1;
    if (alloc_f32(&o->curvatures_k8,    n_pts) < 0) return -1;
    if (alloc_f32(&o->curvatures_k16,   n_pts) < 0) return -1;
    if (alloc_f32(&o->curvatures_k32,   n_pts) < 0) return -1;
    if (alloc_f32(&o->tile_normals,     n_pts * 3) < 0) return -1;
    if (alloc_f32(&o->eigen_reliability, n_pts) < 0) return -1;
    if (alloc_f32(&o->point_weights,    n_pts) < 0) return -1;
    o->eigen_ternary = (uint8_t*)calloc(n_pts * 3, sizeof(uint8_t));
    if (!o->eigen_ternary) return -1;
    return 0;
}

// JS Math.round: round-half-toward-+Infinity. Differs from C lround()
// (round-half-away-from-zero) for negative half-integers — for our values
// in [0, 1] the difference rarely bites, but matching v2 byte-for-byte
// requires the JS semantics.
static long js_round(double x) {
    return (long)floor(x + 0.5);
}

// (λ1 - λ2)/λ1 of the unweighted neighbour covariance. High = scan-line.
static double compute_spatial_linearity(const kd_neighbor_t* neighbors, int n,
                                         const double* points) {
    double cx = 0, cy = 0, cz = 0;
    for (int j = 0; j < n; j++) {
        const double* p = &points[(size_t)neighbors[j].idx * 3];
        cx += p[0]; cy += p[1]; cz += p[2];
    }
    double inv = 1.0 / n;
    cx *= inv; cy *= inv; cz *= inv;
    double c00 = 0, c01 = 0, c02 = 0, c11 = 0, c12 = 0, c22 = 0;
    for (int j = 0; j < n; j++) {
        const double* p = &points[(size_t)neighbors[j].idx * 3];
        double dx = p[0] - cx, dy = p[1] - cy, dz = p[2] - cz;
        c00 += dx * dx; c01 += dx * dy; c02 += dx * dz;
        c11 += dy * dy; c12 += dy * dz; c22 += dz * dz;
    }
    double cov[9] = {c00 * inv, c01 * inv, c02 * inv,
                     c01 * inv, c11 * inv, c12 * inv,
                     c02 * inv, c12 * inv, c22 * inv};
    double evs[3];
    pca_cardano_eigenvalues(cov, evs);
    return (evs[0] - evs[1]) / (evs[0] + 1e-15);
}

// Spherical-KDE weights normalised to sum to 1.
static void spherical_kde_weights(const double* dirBuf, int n,
                                   double inv_two_sigma_sq,
                                   double* weights /* len n */) {
    double w_sum = 0.0;
    for (int j = 0; j < n; j++) {
        double jx = dirBuf[j * 3 + 0];
        double jy = dirBuf[j * 3 + 1];
        double jz = dirBuf[j * 3 + 2];
        double density = 0.0;
        for (int k = 0; k < n; k++) {
            double dx = jx - dirBuf[k * 3 + 0];
            double dy = jy - dirBuf[k * 3 + 1];
            double dz = jz - dirBuf[k * 3 + 2];
            double chord_sq = dx * dx + dy * dy + dz * dz;
            density += exp(-chord_sq * inv_two_sigma_sq);
        }
        weights[j] = 1.0 / density;
        w_sum += weights[j];
    }
    double inv = 1.0 / w_sum;
    for (int j = 0; j < n; j++) weights[j] *= inv;
}

// Weighted PCA. Writes eigenvalues (descending) to `evs` and the smallest
// eigenvector (the surface normal) to `normal`. Returns 0 on success, -1 if
// PCA degenerated (sphere-like multiplicity, no meaningful normal).
static int weighted_pca(const kd_neighbor_t* neighbors, int n,
                         const double* points, const double* weights,
                         double evs[3], double normal[3]) {
    double cx = 0, cy = 0, cz = 0;
    for (int j = 0; j < n; j++) {
        const double* p = &points[(size_t)neighbors[j].idx * 3];
        double w = weights[j];
        cx += p[0] * w; cy += p[1] * w; cz += p[2] * w;
    }
    double c00 = 0, c01 = 0, c02 = 0, c11 = 0, c12 = 0, c22 = 0;
    for (int j = 0; j < n; j++) {
        const double* p = &points[(size_t)neighbors[j].idx * 3];
        double w = weights[j];
        double dx = p[0] - cx, dy = p[1] - cy, dz = p[2] - cz;
        c00 += w * dx * dx; c01 += w * dx * dy; c02 += w * dx * dz;
        c11 += w * dy * dy; c12 += w * dy * dz; c22 += w * dz * dz;
    }
    double cov[9] = {c00, c01, c02, c01, c11, c12, c02, c12, c22};
    pca_cardano_eigenvalues(cov, evs);
    return pca_smallest_eigenvector(cov, normal);
}

// Fill dirBuf [0..n) with unit direction vectors from qi to neighbors[j].
// Self-loop (zero-length) gets the v2 sentinel direction (1,0,0).
static void fill_dir_buf(double* dirBuf, const kd_neighbor_t* neighbors, int n,
                          const double* points, const double qi[3]) {
    for (int j = 0; j < n; j++) {
        const double* p = &points[(size_t)neighbors[j].idx * 3];
        double dx = p[0] - qi[0], dy = p[1] - qi[1], dz = p[2] - qi[2];
        double len = sqrt(dx * dx + dy * dy + dz * dz);
        if (len > 1e-12) {
            double inv = 1.0 / len;
            dirBuf[j * 3 + 0] = dx * inv;
            dirBuf[j * 3 + 1] = dy * inv;
            dirBuf[j * 3 + 2] = dz * inv;
        } else {
            dirBuf[j * 3 + 0] = 1.0;
            dirBuf[j * 3 + 1] = 0.0;
            dirBuf[j * 3 + 2] = 0.0;
        }
    }
}

int v7_ts_compute(const double* positions, size_t n_pts,
                  const kd_tree_t* tree,
                  const v7_ts_params_t* params,
                  v7_ts_outputs_t* out) {
    if (alloc_outputs(out, n_pts) < 0) {
        v7_ts_outputs_free(out);
        return -1;
    }
    if (n_pts == 0) return 0;
    if (!tree || tree->n_points != n_pts || tree->positions != positions) {
        // The tree must be built over exactly these `positions`. The
        // caller-provided contract caught a copy-paste bug during the
        // 5.1c refactor — keep it.
        v7_ts_outputs_free(out);
        return -1;
    }

    const int K       = params->eigen_k;
    const double max_radius_sq = params->max_radius * params->max_radius;
    const double sigma         = params->angular_bandwidth;
    const double inv_two_sigma_sq = 1.0 / (2.0 * sigma * sigma);
    const double linearity_threshold = params->linearity_threshold;
    const int weight_k = params->weight_k;
    const int up = (params->up_axis >= 0 && params->up_axis <= 2) ? params->up_axis : 1;

    const double* pts_d = positions;

    kd_neighbor_t* neighbors = (kd_neighbor_t*)malloc((size_t)K_MAX * sizeof(kd_neighbor_t));
    double* dirBuf  = (double*)malloc((size_t)K_MAX * 3 * sizeof(double));
    double* weights = (double*)malloc((size_t)K_MAX * sizeof(double));
    if (!neighbors || !dirBuf || !weights) {
        free(neighbors); free(dirBuf); free(weights);
        v7_ts_outputs_free(out);
        return -1;
    }

    float* curv_arrs[3] = {out->curvatures_k8, out->curvatures_k16, out->curvatures_k32};

    for (size_t i = 0; i < n_pts; i++) {
        const double qi[3] = {pts_d[i * 3 + 0], pts_d[i * 3 + 1], pts_d[i * 3 + 2]};

        int n_total = 0;
        if (kd_knn(tree, qi, K_STAGES[0], neighbors, &n_total) != 0) {
            // kd_knn failure is a real error — don't fabricate signals from an
            // empty neighbour set (the old code zeroed n_total and silently
            // produced garbage eigen/curv/normal/weight for this point).
            free(neighbors); free(dirBuf); free(weights);
            v7_ts_outputs_free(out);
            return -1;
        }
        // A successful k-NN on a non-empty tree always yields ≥1 neighbour,
        // so n_total >= 1 below is an invariant (see the weight fallback).

        // Voronoi-area point weight from k-NN dist²: w = π·d_k²/k.
        //
        // w is the per-point AREA (m²) — the patch of surface a point owns
        // (≈ 1/local_density ≈ spacing²). It is NOT a dimensionless ≈1 factor:
        // typical w ≈ 0.05 m² (≈21× smaller than 1), √w ≈ 0.2 m ≈ local
        // spacing. Consumers must treat it by dimensional class — raw w for
        // area, √w (×a pure number) for length, w/Σw for ratios — and an
        // "unweighted" baseline should use w = 1/density (the mean area), NOT
        // 1. Full per-consumer audit + distribution:
        //   bench-v2 docs/journal/2026/06/2026-06-02-ts-weights-dimensional-audit.md
        if (n_total >= weight_k) {
            out->point_weights[i] =
                (float)(V7_PI * neighbors[weight_k - 1].dist_sq / weight_k);
        } else if (n_total >= 1) {
            out->point_weights[i] =
                (float)(V7_PI * neighbors[n_total - 1].dist_sq / n_total);
        } else {
            // Unreachable: n_total >= 1 is guaranteed (kd_knn failure returns
            // -1 above; a non-empty tree always yields ≥1 neighbour). Assert
            // the invariant in debug; the assignment only keeps the value
            // defined under NDEBUG.
            assert(0 && "v7-tile-signals: n_total == 0 with successful kd_knn");
            out->point_weights[i] = 1.0f;
        }

        if (n_total < 3) {
            // Degenerate point — default upward normal along `up_axis`,
            // full scattering, blue ternary.
            out->tile_normals[i * 3 + 0] = 0.0f;
            out->tile_normals[i * 3 + 1] = 0.0f;
            out->tile_normals[i * 3 + 2] = 0.0f;
            out->tile_normals[i * 3 + up] = 1.0f;
            out->scattering[i] = 1.0f;
            for (int c = 0; c < N_CURVS; c++) curv_arrs[c][i] = 1.0f;
            out->eigen_ternary[i * 3 + 2] = 255;
            continue;
        }

        // Progressive K expansion. Decide on the K=eigen_k slice's spatial
        // linearity, then walk up K_STAGES until linearity drops below the
        // threshold or we exhaust stages. NB: matching v2, `expanded` is set
        // the moment lin0 trips the trigger — even if no actual K_STAGES
        // upgrade fires (lin at K=32 may already pass). Downstream K-loop
        // uses n_total (≥ K_STAGES[0]) instead of the per-slice ck cap when
        // expanded is set.
        int expanded = 0;
        int kN_at_K = (K < n_total) ? K : n_total;
        if (kN_at_K >= 3) {
            double lin0 = compute_spatial_linearity(neighbors, kN_at_K, pts_d);
            if (lin0 > linearity_threshold) {
                expanded = 1;
                double lin = compute_spatial_linearity(neighbors, n_total, pts_d);
                for (int si = 1; si < N_STAGES && lin > linearity_threshold; si++) {
                    int got = 0;
                    if (kd_knn(tree, qi, K_STAGES[si], neighbors, &got) != 0) {
                        got = n_total;
                    }
                    n_total = got;
                    lin = compute_spatial_linearity(neighbors, n_total, pts_d);
                }
            }
        }

        // dirBuf computed once after expansion settles — independent of any
        // tie-ordering quirks of the kd-tree, since (dirBuf[j], neighbors[j])
        // pairing is internally consistent.
        fill_dir_buf(dirBuf, neighbors, n_total, pts_d, qi);

        // Multi-K curvature loop. Slices ≥ K reuse the (possibly expanded)
        // neighbour set; smaller slices stay at their own K and so don't
        // benefit from expansion.
        for (int c = 0; c < N_CURVS; c++) {
            int ck = CURVATURE_KS[c];
            int n = (expanded && ck >= K) ? n_total : ((ck < n_total) ? ck : n_total);
            if (n < 3) { curv_arrs[c][i] = 1.0f; continue; }

            spherical_kde_weights(dirBuf, n, inv_two_sigma_sq, weights);

            double evs[3], normal[3];
            int pca_rc = weighted_pca(neighbors, n, pts_d, weights, evs, normal);

            if (pca_rc != 0 || evs[0] < 1e-12) {
                curv_arrs[c][i] = 1.0f;
                if (ck == K) {
                    out->tile_normals[i * 3 + 0] = 0.0f;
                    out->tile_normals[i * 3 + 1] = 0.0f;
                    out->tile_normals[i * 3 + 2] = 0.0f;
                    out->tile_normals[i * 3 + up] = 1.0f;
                    out->scattering[i] = 1.0f;
                    out->eigen_ternary[i * 3 + 2] = 255;
                }
                continue;
            }

            double e1 = evs[0], e2 = evs[1], e3 = evs[2];
            double e_sum = e1 + e2 + e3;
            curv_arrs[c][i] = (float)(e_sum > 1e-12 ? e3 / e_sum : 1.0);

            if (ck == K) {
                out->eigenvalue_l1[i] = (float)e1;
                out->eigenvalue_l2[i] = (float)e2;
                out->eigenvalue_l3[i] = (float)e3;

                double plan = (e2 - e3) / e1;
                double lin  = (e1 - e2) / e1;
                double scat = e3 / e1;
                out->planarity[i]  = (float)plan;
                out->linearity[i]  = (float)lin;
                out->scattering[i] = (float)scat;

                // Normal: smallest eigenvector, oriented along +up_axis.
                double n3[3] = {normal[0], normal[1], normal[2]};
                if (n3[up] < 0.0) { n3[0] = -n3[0]; n3[1] = -n3[1]; n3[2] = -n3[2]; }
                out->tile_normals[i * 3 + 0] = (float)n3[0];
                out->tile_normals[i * 3 + 1] = (float)n3[1];
                out->tile_normals[i * 3 + 2] = (float)n3[2];

                // Reliability = density factor × angular isotropy.
                int within = 0;
                for (int j = 0; j < n; j++) {
                    if (neighbors[j].dist_sq <= max_radius_sq) within++;
                }
                double density_factor = (double)within / (double)n;
                double sum_w_sq = 0.0;
                for (int j = 0; j < n; j++) sum_w_sq += weights[j] * weights[j];
                // Frama-C audit finding TS-1: sum_w_sq is mathematically
                // non-negative and ≥ 1/n only when weights are well-behaved;
                // degenerate neighborhoods (all-zero spherical-KDE weights
                // when neighbor directions cluster opposite the query)
                // produce sum_w_sq == 0 and `1.0/0.0` is UB. Clamp to a
                // small floor so the division stays finite. The downstream
                // (float) cast at the multiply line then sees a large but
                // finite isotropy, propagating to eigen_reliability — which
                // is conceptually meaningless for such a degenerate
                // neighborhood but doesn't crash the run.
                double isotropy;
                if (sum_w_sq <= 1e-300) {
                    isotropy = 1.0;  // degenerate — neutral reliability.
                } else {
                    isotropy = (1.0 / sum_w_sq) / (double)n;
                }
                out->eigen_reliability[i] = (float)(density_factor * isotropy);

                // Ternary RGB (clamped 0..255).
                long rL = js_round(lin  * 255.0);
                long rP = js_round(plan * 255.0);
                long rS = js_round(scat * 255.0);
                if (rL > 255) rL = 255;
                if (rL < 0)   rL = 0;
                if (rP > 255) rP = 255;
                if (rP < 0)   rP = 0;
                if (rS > 255) rS = 255;
                if (rS < 0)   rS = 0;
                out->eigen_ternary[i * 3 + 0] = (uint8_t)rL;
                out->eigen_ternary[i * 3 + 1] = (uint8_t)rP;
                out->eigen_ternary[i * 3 + 2] = (uint8_t)rS;
            }
        }
    }

    free(weights);
    free(dirBuf);
    free(neighbors);
    return 0;
}

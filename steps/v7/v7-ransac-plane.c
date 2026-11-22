// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-ransac-plane — see header.

#include "v7-ransac-plane.h"
#include "v3/safe_alloc.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void v7_ransac_result_init(v7_ransac_result_t* r) {
    if (!r) return;
    memset(r, 0, sizeof(*r));
}

void v7_ransac_result_free(v7_ransac_result_t* r) {
    if (!r) return;
    free(r->inlier_indices);
    memset(r, 0, sizeof(*r));
}

int v7_ransac_plane(const double* positions,
                    const uint32_t* indices, size_t n_indices,
                    int iterations,
                    double dist_threshold,
                    v7_rng_t* rng,
                    v7_ransac_result_t* out) {
    if (!out) return -1;
    v7_ransac_result_init(out);
    if (n_indices < 3) return -1;
    const size_t n = n_indices;

    // Scratch buffers reused across iterations to avoid alloc churn.
    uint32_t* cand = (uint32_t*)safe_malloc_array(n, sizeof(uint32_t));
    if (!cand) return -1;
    uint32_t* best = (uint32_t*)safe_malloc_array(n, sizeof(uint32_t));
    if (!best) { free(cand); return -1; }
    size_t n_best = 0;
    double best_n[3] = {0, 0, 0};
    double best_d = 0.0;

    for (int iter = 0; iter < iterations; iter++) {
        // Three distinct samples — match v2's reject-and-resample loop
        // bit-for-bit, including how many RNG calls each iteration consumes.
        const size_t i0 = (size_t)floor((double)v7_rng_next(rng) * (double)n);
        size_t i1, i2;
        do { i1 = (size_t)floor((double)v7_rng_next(rng) * (double)n); }
        while (i1 == i0);
        do { i2 = (size_t)floor((double)v7_rng_next(rng) * (double)n); }
        while (i2 == i0 || i2 == i1);

        const double* p0 = positions + (size_t)indices[i0] * 3;
        const double* p1 = positions + (size_t)indices[i1] * 3;
        const double* p2 = positions + (size_t)indices[i2] * 3;

        const double v1x = p1[0] - p0[0], v1y = p1[1] - p0[1], v1z = p1[2] - p0[2];
        const double v2x = p2[0] - p0[0], v2y = p2[1] - p0[1], v2z = p2[2] - p0[2];
        double nx = v1y * v2z - v1z * v2y;
        double ny = v1z * v2x - v1x * v2z;
        double nz = v1x * v2y - v1y * v2x;
        const double len = sqrt(nx * nx + ny * ny + nz * nz);
        if (len < 1e-10) continue;          // degenerate triple — RNG continues
        nx /= len; ny /= len; nz /= len;
        const double d = -(nx * p0[0] + ny * p0[1] + nz * p0[2]);

        size_t n_cand = 0;
        for (size_t i = 0; i < n; i++) {
            const double* p = positions + (size_t)indices[i] * 3;
            double dist = nx * p[0] + ny * p[1] + nz * p[2] + d;
            if (dist < 0) dist = -dist;
            if (dist <= dist_threshold) cand[n_cand++] = (uint32_t)i;
        }

        if (n_cand > n_best) {
            n_best = n_cand;
            memcpy(best, cand, n_cand * sizeof(uint32_t));
            best_n[0] = nx; best_n[1] = ny; best_n[2] = nz;
            best_d = d;
        }
    }

    free(cand);
    if (n_best < 3) {
        free(best);
        return -1;
    }
    // Trim the best buffer.
    uint32_t* shrunk = (uint32_t*)safe_realloc_array(best, n_best, sizeof(uint32_t));
    out->inlier_indices = shrunk ? shrunk : best;
    out->n_inliers = n_best;
    out->normal[0] = best_n[0]; out->normal[1] = best_n[1]; out->normal[2] = best_n[2];
    out->d = best_d;
    return 0;
}

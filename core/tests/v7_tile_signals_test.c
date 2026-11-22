// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// core/tests/v7_tile_signals_test.c
//
// Implementation tests for v7-tile-signals. Each test asserts an
// invariant the algorithm spec claims to hold. Inputs are hand-crafted,
// minimal, deterministic. Tolerances are tight (binary pass/fail).
//
// Spec source: bench-v2 test/tile-signals.test.js (the JS unit tests
// encode the same invariants we want v3's port to satisfy).
//
// Implementation reference: bench-v2 lib/pipeline/steps/tile-signals.js
// + v3 steps/v7/v7-tile-signals-impl.c.
//
// We include the v7-tile-signals-impl.c source directly to access its
// file-static helpers (spherical_kde_weights, weighted_pca). The test
// is then a pure compile unit that links against pca + kd_tree from
// dioramap_steps_normals.

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE   700        // so <math.h> exposes M_PI / M_SQRT1_2

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Fallbacks in case the system <math.h> still doesn't expose them.
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_SQRT1_2
#define M_SQRT1_2 0.70710678118654752440
#endif

// Pull in the impl as part of this TU so we can call its statics.
#include "v7-tile-signals-impl.c"

// ─── harness ────────────────────────────────────────────────────────
static int g_checks = 0;
static int g_failed = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        g_checks++;                                                                                \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                        \
            g_failed++;                                                                            \
        }                                                                                          \
    } while (0)

#define CHECK_CLOSE(a, b, eps)                                                                     \
    do {                                                                                           \
        double _a = (a), _b = (b);                                                                 \
        g_checks++;                                                                                \
        if (fabs(_a - _b) > (eps)) {                                                               \
            fprintf(stderr, "FAIL %s:%d: %s ≈ %s (got %.12g, want %.12g, eps=%g)\n",               \
                    __FILE__, __LINE__, #a, #b, _a, _b, (eps));                                    \
            g_failed++;                                                                            \
        }                                                                                          \
    } while (0)

// ─── helpers ────────────────────────────────────────────────────────

// Matches the bench-v2 test setup: σ = π/8, INV_TWO_SIGMA_SQ = 1/(2σ²).
static const double SIGMA = M_PI / 8.0;
#define INV_TWO_SIGMA_SQ (1.0 / (2.0 * SIGMA * SIGMA))

// Build a dirBuf (Float64 unit directions) from arrays of {x, y, z} offsets.
// Length-zero offsets produce the (1,0,0) sentinel (mirrors v2's fill_dir_buf).
static void build_dir_buf(double* dirBuf, const double* offsets, int n) {
    for (int j = 0; j < n; j++) {
        double x = offsets[j * 3 + 0];
        double y = offsets[j * 3 + 1];
        double z = offsets[j * 3 + 2];
        double len = sqrt(x * x + y * y + z * z);
        if (len > 1e-12) {
            dirBuf[j * 3 + 0] = x / len;
            dirBuf[j * 3 + 1] = y / len;
            dirBuf[j * 3 + 2] = z / len;
        } else {
            dirBuf[j * 3 + 0] = 1.0;
            dirBuf[j * 3 + 1] = 0.0;
            dirBuf[j * 3 + 2] = 0.0;
        }
    }
}

// Sum of weights[0..n).
static double sum_weights(const double* w, int n) {
    double s = 0;
    for (int i = 0; i < n; i++) s += w[i];
    return s;
}

// ─── spherical_kde_weights: basic invariants ────────────────────────

static void test_kde_weights_sum_to_one(void) {
    // Spec: weights are a normalized KDE — must sum to 1.0 exactly
    // (modulo accumulated rounding), for any non-degenerate input.
    const double offsets[15] = {
         1, 0, 0,    0, 1, 0,    0, 0, 1,
        -1, 0, 0,    0,-1, 0,
    };
    double dirBuf[15];
    double w[5];
    build_dir_buf(dirBuf, offsets, 5);
    spherical_kde_weights(dirBuf, 5, INV_TWO_SIGMA_SQ, w);
    CHECK_CLOSE(sum_weights(w, 5), 1.0, 1e-10);
}

static void test_kde_weights_all_positive(void) {
    // Spec: density is exp(positive) — sum is positive — 1/sum is positive.
    const double offsets[12] = {
         1, 0.1, 0,    1,-0.1, 0,
         0, 1,   0,   -1, 0,   0.5,
    };
    double dirBuf[12];
    double w[4];
    build_dir_buf(dirBuf, offsets, 4);
    spherical_kde_weights(dirBuf, 4, INV_TWO_SIGMA_SQ, w);
    for (int i = 0; i < 4; i++) CHECK(w[i] > 0);
}

static void test_kde_n_equals_1(void) {
    // Spec: single direction → density = exp(0) = 1 → weight = 1.
    const double offsets[3] = { 0, 1, 0 };
    double dirBuf[3];
    double w[1];
    build_dir_buf(dirBuf, offsets, 1);
    spherical_kde_weights(dirBuf, 1, INV_TWO_SIGMA_SQ, w);
    CHECK_CLOSE(w[0], 1.0, 1e-12);
}

static void test_kde_uniform_distribution_near_equal_weights(void) {
    // Spec: directions evenly distributed over the sphere should get
    // near-equal weights (KDE is symmetric under spherical rotation).
    // We use the 6 axes of the cube as a coarse-but-deterministic
    // approximation of uniform.
    const double offsets[18] = {
         1, 0, 0,   -1, 0, 0,
         0, 1, 0,    0,-1, 0,
         0, 0, 1,    0, 0,-1,
    };
    double dirBuf[18];
    double w[6];
    build_dir_buf(dirBuf, offsets, 6);
    spherical_kde_weights(dirBuf, 6, INV_TWO_SIGMA_SQ, w);
    // All weights should be 1/6 within tolerance (KDE under cube-axis
    // symmetry: each pair sees identical density).
    for (int i = 0; i < 6; i++) {
        CHECK_CLOSE(w[i], 1.0 / 6.0, 1e-10);
    }
}

static void test_kde_clustered_lower_than_isolated(void) {
    // Spec: KDE weights down-weight directions that cluster (their density
    // is high) and up-weight isolated directions (low density).  10 tightly
    // clustered along +X, 2 sparse off-axis.  The clustered ones must each
    // get LESS weight than each isolated one.
    double offsets[36];
    // 10 clustered around (1, 0, 0) with small jitter:
    for (int i = 0; i < 10; i++) {
        double a = (i - 4.5) * 0.005;
        offsets[i * 3 + 0] = 1.0;
        offsets[i * 3 + 1] = a;
        offsets[i * 3 + 2] = 0.0;
    }
    // 2 isolated:
    offsets[10 * 3 + 0] = 0; offsets[10 * 3 + 1] = 1; offsets[10 * 3 + 2] = 0;
    offsets[11 * 3 + 0] = 0; offsets[11 * 3 + 1] = 0; offsets[11 * 3 + 2] = 1;

    double dirBuf[36];
    double w[12];
    build_dir_buf(dirBuf, offsets, 12);
    spherical_kde_weights(dirBuf, 12, INV_TWO_SIGMA_SQ, w);

    // Clustered indices [0..9] each should be < either isolated weight.
    for (int i = 0; i < 10; i++) {
        CHECK(w[i] < w[10]);
        CHECK(w[i] < w[11]);
    }
    // Sanity: still sums to 1.
    CHECK_CLOSE(sum_weights(w, 12), 1.0, 1e-10);
}

static void test_kde_slope_invariance(void) {
    // Spec: the spherical KDE is rotation-invariant — feed the same angular
    // pattern but rotated, expect the same weights.  Bench-v2's test rotates
    // by 45° around the X axis to simulate a sloped surface.
    //
    // 4 directions in the YZ plane (around a flat surface):
    const double flat[12] = {
         1, 0, 0,    0, 0, 1,
        -1, 0, 0,    0, 0,-1,
    };
    double dirBuf_flat[12], w_flat[4];
    build_dir_buf(dirBuf_flat, flat, 4);
    spherical_kde_weights(dirBuf_flat, 4, INV_TWO_SIGMA_SQ, w_flat);

    // Same pattern rotated 45° about X: (x, y, z) → (x, (y-z)/√2, (y+z)/√2)
    double sloped[12];
    const double s = M_SQRT1_2;
    for (int j = 0; j < 4; j++) {
        double x = flat[j * 3 + 0];
        double y = flat[j * 3 + 1];
        double z = flat[j * 3 + 2];
        sloped[j * 3 + 0] = x;
        sloped[j * 3 + 1] = (y - z) * s;
        sloped[j * 3 + 2] = (y + z) * s;
    }
    double dirBuf_sloped[12], w_sloped[4];
    build_dir_buf(dirBuf_sloped, sloped, 4);
    spherical_kde_weights(dirBuf_sloped, 4, INV_TWO_SIGMA_SQ, w_sloped);

    for (int i = 0; i < 4; i++) {
        CHECK_CLOSE(w_flat[i], w_sloped[i], 1e-12);
    }
}

static void test_kde_identical_directions_equal_weights(void) {
    // Spec: n copies of the same direction → equal weight 1/n.
    const double offsets[15] = {
        0, 1, 0,   0, 1, 0,   0, 1, 0,
        0, 1, 0,   0, 1, 0,
    };
    double dirBuf[15];
    double w[5];
    build_dir_buf(dirBuf, offsets, 5);
    spherical_kde_weights(dirBuf, 5, INV_TWO_SIGMA_SQ, w);
    for (int i = 0; i < 5; i++) {
        CHECK_CLOSE(w[i], 0.2, 1e-12);
    }
}

// ─── weighted_pca: plane-normal recovery ────────────────────────────

// Build a kd_neighbor_t array referencing `n` consecutive indices, each
// with dist_sq = 0 (which the PCA path doesn't read).
static void build_neighbors_identity(kd_neighbor_t* nbrs, int n) {
    for (int i = 0; i < n; i++) {
        nbrs[i].idx = i;
        nbrs[i].dist_sq = 0.0;
    }
}

static void test_pca_recovers_upward_normal_on_flat(void) {
    // Spec: PCA on coplanar points sampled from y = 5 yields a normal aligned
    // with ±y to within tight tolerance, and λ3 << λ2 (rank-2 covariance).
    const int N = 10;
    double pts[30];
    for (int i = 0; i < N; i++) {
        // Distribute points in a 2×2 patch at y=5:
        double x = (i % 5) - 2.0;
        double z = (i / 5) - 0.5;
        pts[i * 3 + 0] = x;
        pts[i * 3 + 1] = 5.0;
        pts[i * 3 + 2] = z;
    }
    kd_neighbor_t nbrs[10];
    build_neighbors_identity(nbrs, N);
    double w[10];
    for (int i = 0; i < N; i++) w[i] = 1.0 / N;

    double evs[3], normal[3];
    int rc = weighted_pca(nbrs, N, pts, w, evs, normal);
    CHECK(rc == 0);

    // Normal aligned with ±y:
    CHECK_CLOSE(fabs(normal[1]), 1.0, 1e-6);
    CHECK_CLOSE(normal[0], 0.0, 1e-6);
    CHECK_CLOSE(normal[2], 0.0, 1e-6);

    // λ3 (smallest) should be ~0 for a perfectly planar set.
    CHECK(evs[2] < evs[1] * 1e-6);
}

static void test_pca_recovers_tilted_plane_normal(void) {
    // Spec: PCA on points sampled from a 45° tilted plane y = 5 + x yields
    // a normal in the XY plane with |nx| and |ny| both ≈ 1/√2.
    const int N = 16;
    double pts[48];
    int idx = 0;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            double x = (double)i;
            double z = (double)j;
            pts[idx++] = x;
            pts[idx++] = 5.0 + x;   // plane: y = 5 + x
            pts[idx++] = z;
        }
    }
    kd_neighbor_t nbrs[16];
    build_neighbors_identity(nbrs, N);
    double w[16];
    for (int i = 0; i < N; i++) w[i] = 1.0 / N;

    double evs[3], normal[3];
    int rc = weighted_pca(nbrs, N, pts, w, evs, normal);
    CHECK(rc == 0);

    // |nx| and |ny| both significant; nz ~ 0.  Per v2 tolerances.
    CHECK(fabs(normal[0]) > 0.4);
    CHECK(fabs(normal[1]) > 0.4);
    CHECK_CLOSE(normal[2], 0.0, 0.15);
}

static void test_pca_planar_eigenvalues_ordered(void) {
    // Spec: weighted_pca returns eigenvalues in descending order
    // (evs[0] >= evs[1] >= evs[2]).  Verify on a non-degenerate input.
    const int N = 10;
    double pts[30];
    for (int i = 0; i < N; i++) {
        pts[i * 3 + 0] = (i % 5);
        pts[i * 3 + 1] = 5.0;
        pts[i * 3 + 2] = (i / 5) * 3.0;   // stretched along z to make λ1 > λ2
    }
    kd_neighbor_t nbrs[10];
    build_neighbors_identity(nbrs, N);
    double w[10];
    for (int i = 0; i < N; i++) w[i] = 1.0 / N;

    double evs[3], normal[3];
    int rc = weighted_pca(nbrs, N, pts, w, evs, normal);
    CHECK(rc == 0);
    CHECK(evs[0] >= evs[1]);
    CHECK(evs[1] >= evs[2]);
    CHECK(evs[2] >= 0.0);
    (void)normal;
}

// ─── main ───────────────────────────────────────────────────────────

int main(void) {
    test_kde_weights_sum_to_one();
    test_kde_weights_all_positive();
    test_kde_n_equals_1();
    test_kde_uniform_distribution_near_equal_weights();
    test_kde_clustered_lower_than_isolated();
    test_kde_slope_invariance();
    test_kde_identical_directions_equal_weights();

    test_pca_recovers_upward_normal_on_flat();
    test_pca_recovers_tilted_plane_normal();
    test_pca_planar_eigenvalues_ordered();

    printf("%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}

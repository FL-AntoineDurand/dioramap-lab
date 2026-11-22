// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Unit + integration tests for the compute-normals step (B4).
//
// Three layers:
//   1. kd-tree correctness vs brute-force on random clouds.
//   2. PCA correctness on synthetic covariance matrices with known
//      eigenstructure.
//   3. compute-normals end-to-end on synthetic geometries (plane, tilted
//      plane, sphere) — verifies the assembled algorithm produces the
//      expected normal at every point within a tight tolerance.

#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v3/safe_alloc.h"
#include <unistd.h>

#include "v3/runner.h"
#include "v3/store.h"

#include "kd_tree.h"
#include "normals-steps.h"
#include "pca.h"

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

#define CHECK_NEAR(a, b, tol)                                                                      \
    do {                                                                                           \
        g_checks++;                                                                                \
        double _diff = fabs((double)(a) - (double)(b));                                            \
        if (_diff > (tol)) {                                                                       \
            fprintf(stderr, "FAIL %s:%d: |%.6g - %.6g| = %.6g > %.6g\n", __FILE__, __LINE__,       \
                    (double)(a), (double)(b), _diff, (double)(tol));                               \
            g_failed++;                                                                            \
        }                                                                                          \
    } while (0)

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------

static void rm_rf(const char* path) {
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    (void)!system(cmd);
}

static void make_tmpdir(char* out, size_t out_sz) {
    snprintf(out, out_sz, "/tmp/dioramap_normals_test_XXXXXX");
    if (mkdtemp(out) == NULL) {
        exit(1);
    }
}

// Deterministic LCG so tests are reproducible without depending on libc rand.
typedef struct rng {
    uint64_t state;
} rng_t;

static double rng_uniform(rng_t* r) {
    r->state = r->state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (double)((r->state >> 11) & ((1ULL << 53) - 1)) / (double)(1ULL << 53);
}

// ----------------------------------------------------------------------------
// kd-tree tests
// ----------------------------------------------------------------------------

static void brute_force_knn(const double* positions, size_t n_points, const double q[3], int k,
                            kd_neighbor_t* out, int* out_count) {
    kd_neighbor_t* all = safe_malloc_array(n_points, sizeof(kd_neighbor_t));
    for (size_t i = 0; i < n_points; i++) {
        double dx = positions[i * 3 + 0] - q[0];
        double dy = positions[i * 3 + 1] - q[1];
        double dz = positions[i * 3 + 2] - q[2];
        all[i].idx = (int32_t)i;
        all[i].dist_sq = dx * dx + dy * dy + dz * dz;
    }
    // Selection-sort first k entries.
    int actual = (int)n_points < k ? (int)n_points : k;
    for (int i = 0; i < actual; i++) {
        int min_j = i;
        for (size_t j = i + 1; j < n_points; j++) {
            if (all[j].dist_sq < all[min_j].dist_sq) {
                min_j = (int)j;
            }
        }
        kd_neighbor_t tmp = all[i];
        all[i] = all[min_j];
        all[min_j] = tmp;
    }
    for (int i = 0; i < actual; i++) {
        out[i] = all[i];
    }
    *out_count = actual;
    free(all);
}

static void test_kdtree_random_matches_brute(void) {
    rng_t r = {.state = 0xABCDEF1234567890ULL};
    const size_t N = 500;
    double* positions = malloc(N * 3 * sizeof(double));
    for (size_t i = 0; i < N; i++) {
        positions[i * 3 + 0] = rng_uniform(&r) * 100.0;
        positions[i * 3 + 1] = rng_uniform(&r) * 100.0;
        positions[i * 3 + 2] = rng_uniform(&r) * 100.0;
    }
    kd_tree_t tree;
    CHECK(kd_build(&tree, positions, N) == 0);

    // 10 random query points, k=8. Compare kd-tree answer against brute.
    const int K = 8;
    kd_neighbor_t kdr[K], bfr[K];
    int kn, bn;
    for (int q = 0; q < 10; q++) {
        double query[3] = {rng_uniform(&r) * 100.0, rng_uniform(&r) * 100.0,
                           rng_uniform(&r) * 100.0};
        CHECK(kd_knn(&tree, query, K, kdr, &kn) == 0);
        brute_force_knn(positions, N, query, K, bfr, &bn);
        CHECK(kn == bn);
        for (int i = 0; i < kn; i++) {
            // Same dist_sq even if ties resolve differently.
            CHECK_NEAR(kdr[i].dist_sq, bfr[i].dist_sq, 1e-12);
        }
    }
    kd_free(&tree);
    free(positions);
}

static void test_kdtree_query_at_existing_point(void) {
    // A query at point i must include i itself with dist_sq == 0 as the first hit.
    rng_t r = {.state = 0xDEADBEEF12345678ULL};
    const size_t N = 100;
    double* positions = malloc(N * 3 * sizeof(double));
    for (size_t i = 0; i < N; i++) {
        positions[i * 3 + 0] = rng_uniform(&r);
        positions[i * 3 + 1] = rng_uniform(&r);
        positions[i * 3 + 2] = rng_uniform(&r);
    }
    kd_tree_t tree;
    CHECK(kd_build(&tree, positions, N) == 0);
    kd_neighbor_t r4[4];
    int n;
    double q[3] = {positions[10 * 3 + 0], positions[10 * 3 + 1], positions[10 * 3 + 2]};
    CHECK(kd_knn(&tree, q, 4, r4, &n) == 0);
    CHECK(n == 4);
    CHECK(r4[0].dist_sq < 1e-20); // self
    CHECK(r4[0].idx == 10);
    kd_free(&tree);
    free(positions);
}

static void test_kdtree_empty_and_single(void) {
    kd_tree_t tree;
    CHECK(kd_build(&tree, NULL, 0) == 0);
    kd_neighbor_t out[4];
    int n;
    double q[3] = {0, 0, 0};
    CHECK(kd_knn(&tree, q, 4, out, &n) == 0);
    CHECK(n == 0);
    kd_free(&tree);

    double one[3] = {1, 2, 3};
    CHECK(kd_build(&tree, one, 1) == 0);
    CHECK(kd_knn(&tree, q, 4, out, &n) == 0);
    CHECK(n == 1);
    CHECK(out[0].idx == 0);
    kd_free(&tree);
}

// ----------------------------------------------------------------------------
// PCA tests
// ----------------------------------------------------------------------------

static void test_pca_diagonal_matrix(void) {
    // Diagonal — eigenvalues are the diagonal entries (sorted descending),
    // eigenvectors are world axes.
    double cov[9] = {3, 0, 0, 0, 7, 0, 0, 0, 1};
    double evs[3];
    pca_cardano_eigenvalues(cov, evs);
    CHECK_NEAR(evs[0], 7, 1e-12);
    CHECK_NEAR(evs[1], 3, 1e-12);
    CHECK_NEAR(evs[2], 1, 1e-12);

    double smallest[3];
    CHECK(pca_smallest_eigenvector(cov, smallest) == 0);
    // Smallest eigenvalue 1 → axis z.
    CHECK_NEAR(fabs(smallest[2]), 1.0, 1e-9);
    CHECK_NEAR(fabs(smallest[0]), 0.0, 1e-9);
    CHECK_NEAR(fabs(smallest[1]), 0.0, 1e-9);
}

static void test_pca_known_plane_normal(void) {
    // Synthetic: 100 random points on the z=0 plane (jittered slightly to avoid
    // exact-degenerate PCA). Smallest eigenvector should align with +/- z.
    rng_t r = {.state = 0x12349876ABCDEF00ULL};
    const int N = 100;
    double cx = 0, cy = 0, cz = 0;
    double pts[300];
    for (int i = 0; i < N; i++) {
        pts[i * 3 + 0] = rng_uniform(&r) * 10.0;
        pts[i * 3 + 1] = rng_uniform(&r) * 10.0;
        pts[i * 3 + 2] = (rng_uniform(&r) - 0.5) * 1e-6; // tiny noise
        cx += pts[i * 3 + 0];
        cy += pts[i * 3 + 1];
        cz += pts[i * 3 + 2];
    }
    cx /= N;
    cy /= N;
    cz /= N;
    double c00 = 0, c01 = 0, c02 = 0, c11 = 0, c12 = 0, c22 = 0;
    for (int i = 0; i < N; i++) {
        double dx = pts[i * 3 + 0] - cx;
        double dy = pts[i * 3 + 1] - cy;
        double dz = pts[i * 3 + 2] - cz;
        c00 += dx * dx;
        c01 += dx * dy;
        c02 += dx * dz;
        c11 += dy * dy;
        c12 += dy * dz;
        c22 += dz * dz;
    }
    double cov[9] = {c00 / N, c01 / N, c02 / N,
                     c01 / N, c11 / N, c12 / N,
                     c02 / N, c12 / N, c22 / N};
    double n[3];
    CHECK(pca_smallest_eigenvector(cov, n) == 0);
    // Normal aligns with z (sign may flip — caller picks orientation).
    CHECK_NEAR(fabs(n[2]), 1.0, 1e-3);
    CHECK_NEAR(fabs(n[0]), 0.0, 1e-3);
    CHECK_NEAR(fabs(n[1]), 0.0, 1e-3);
}

// ----------------------------------------------------------------------------
// compute-normals end-to-end via the runner
// ----------------------------------------------------------------------------

typedef struct cap_state {
    hash_t normals_hash;
    hash_t spacing_hash;
    bool got_normals;
    bool got_spacing;
} cap_state_t;

static void cap_artifact(void* ctx, const char* run_id, const char* name, const hash_t* h) {
    (void)run_id;
    cap_state_t* s = ctx;
    if (strcmp(name, "lidar-normals") == 0) {
        s->normals_hash = *h;
        s->got_normals = true;
    } else if (strcmp(name, "lidar-spacing") == 0) {
        s->spacing_hash = *h;
        s->got_spacing = true;
    }
}

// Dummy "load-positions" step: writes a caller-provided positions buffer as
// "lidar-positions" (kind irrelevant for the test). The test installs the
// buffer pointer into a global before running; the step reads it.
static const double* g_positions = NULL;
static size_t g_n_points = 0;
static int load_exec(step_context_t* ctx) {
    return step_write_output(ctx, "lidar-positions", g_positions,
                             g_n_points * 3 * sizeof(double));
}
static const step_read_t load_reads[] = {{.name = PIPELINE_INPUT_ARTIFACT}};
static const step_write_t load_writes[] = {
    {.name = "lidar-positions", .kind = 0, .visibility = STEP_VIS_DEBUG},
};
static const step_def_t LOAD_DEF = {
    .name = "load-positions",
    .version = 1,
    .reads = load_reads,
    .n_reads = 1,
    .writes = load_writes,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = load_exec,
};

static intent_map_t* open_test_im(const char* root) {
    char path[512];
    snprintf(path, sizeof(path), "%s/intent.log", root);
    return intent_map_open(path);
}

// Run compute-normals on the given point cloud, return normals via `out`.
// If `out_spacing` is non-NULL, also subscribe to lidar-spacing and copy
// the per-point spacing values into it.
static int run_normals(const double* positions, size_t n_points, int k, float* out,
                       float* out_spacing) {
    g_positions = positions;
    g_n_points = n_points;

    char root[256];
    make_tmpdir(root, sizeof(root));
    store_t* store = store_open(&(store_config_t){.root_dir = root, .hot_budget_bytes = 64 * 1024 * 1024});
    intent_map_t* im = open_test_im(root);
    runner_t* r = runner_new(&(runner_config_t){.store = store, .intent_map = im});

    pipeline_t* p = pipeline_new("normals-test");
    pipeline_add_step(p, "load-positions", NULL);
    char pjson[64];
    snprintf(pjson, sizeof(pjson), "{\"k\":%d}", k);
    pipeline_add_step(p, "compute-normals", pjson);

    int dummy = 0;
    const char* subs_normals_only[]   = {"lidar-normals"};
    const char* subs_normals_spacing[] = {"lidar-normals", "lidar-spacing"};
    const char* const* subs = out_spacing ? subs_normals_spacing : subs_normals_only;
    size_t n_subs = out_spacing ? 2 : 1;
    cap_state_t cs = {0};
    runner_callbacks_t cb = {.ctx = &cs, .on_artifact_ready = cap_artifact};
    int rc = runner_run(r, p, &dummy, sizeof(dummy), subs, n_subs, &cb);

    int ok = (rc == 0 && cs.got_normals && (!out_spacing || cs.got_spacing)) ? 0 : -1;
    if (ok == 0) {
        store_handle_t h;
        if (store_lookup(store, &cs.normals_hash, &h) == 0) {
            if (h.len == n_points * 3 * sizeof(float)) {
                memcpy(out, h.bytes, h.len);
            } else {
                ok = -1;
            }
            store_release(store, &h);
        } else {
            ok = -1;
        }
    }
    if (ok == 0 && out_spacing) {
        store_handle_t h;
        if (store_lookup(store, &cs.spacing_hash, &h) == 0) {
            if (h.len == n_points * sizeof(float)) {
                memcpy(out_spacing, h.bytes, h.len);
            } else {
                ok = -1;
            }
            store_release(store, &h);
        } else {
            ok = -1;
        }
    }

    pipeline_free(p);
    runner_free(r);
    intent_map_close(im);
    store_close(store);
    rm_rf(root);
    return ok;
}

static void register_test_steps(void) {
    step_registry_reset();
    CHECK(step_register(&LOAD_DEF) == 0);
    CHECK(normals_register_steps() == 0);
}

static void test_normals_horizontal_plane(void) {
    register_test_steps();
    // 64 × 64 grid of points at z = 5, spaced 0.1 m. 4096 points.
    const int side = 64;
    const size_t N = side * side;
    double* positions = malloc(N * 3 * sizeof(double));
    size_t i = 0;
    for (int y = 0; y < side; y++) {
        for (int x = 0; x < side; x++) {
            positions[i * 3 + 0] = x * 0.1;
            positions[i * 3 + 1] = y * 0.1;
            positions[i * 3 + 2] = 5.0;
            i++;
        }
    }
    float* normals = malloc(N * 3 * sizeof(float));
    CHECK(run_normals(positions, N, 16, normals, NULL) == 0);

    // Every normal must be very close to (0, 0, 1).
    size_t bad = 0;
    for (size_t j = 0; j < N; j++) {
        if (fabsf(normals[j * 3 + 2] - 1.0f) > 1e-3f) {
            bad++;
        }
    }
    if (bad > 0) {
        fprintf(stderr, "horizontal_plane: %zu/%zu off-normal\n", bad, N);
    }
    CHECK(bad == 0);

    free(positions);
    free(normals);
}

static void test_normals_tilted_plane(void) {
    register_test_steps();
    // Plane: z = 0.3*x + 0.4*y + 1. Normal direction = (-0.3, -0.4, 1) / |.|;
    // upward (z > 0) is preserved.
    const int side = 48;
    const size_t N = side * side;
    double* positions = malloc(N * 3 * sizeof(double));
    size_t i = 0;
    for (int y = 0; y < side; y++) {
        for (int x = 0; x < side; x++) {
            double xp = x * 0.1;
            double yp = y * 0.1;
            positions[i * 3 + 0] = xp;
            positions[i * 3 + 1] = yp;
            positions[i * 3 + 2] = 0.3 * xp + 0.4 * yp + 1.0;
            i++;
        }
    }
    float* normals = malloc(N * 3 * sizeof(float));
    CHECK(run_normals(positions, N, 16, normals, NULL) == 0);

    double inv = 1.0 / sqrt(0.3 * 0.3 + 0.4 * 0.4 + 1.0);
    double expect_x = -0.3 * inv;
    double expect_y = -0.4 * inv;
    double expect_z = 1.0 * inv;

    size_t good = 0;
    for (size_t j = 0; j < N; j++) {
        double diff = fabs((double)normals[j * 3 + 0] - expect_x) +
                      fabs((double)normals[j * 3 + 1] - expect_y) +
                      fabs((double)normals[j * 3 + 2] - expect_z);
        if (diff < 5e-3) {
            good++;
        }
    }
    fprintf(stderr, "tilted_plane: %zu/%zu within tol\n", good, N);
    // Allow boundary points (kNN dominated by one side) to be slightly off.
    CHECK(good >= N * 95 / 100);

    free(positions);
    free(normals);
}

static void test_normals_sphere_radial(void) {
    register_test_steps();
    // 1000 random points on a sphere of radius 5 centred at origin. Normals
    // should be radial — equal to (x, y, z) / |·| for each point. Z-flip
    // means lower-hemisphere normals get inverted; we test against |dot| = 1.
    rng_t r = {.state = 0xCAFEBABE12345678ULL};
    const size_t N = 1000;
    double* positions = malloc(N * 3 * sizeof(double));
    for (size_t i = 0; i < N; i++) {
        // Marsaglia: uniform on unit sphere.
        double u, v, w;
        for (;;) {
            u = 2.0 * rng_uniform(&r) - 1.0;
            v = 2.0 * rng_uniform(&r) - 1.0;
            w = u * u + v * v;
            if (w < 1.0) {
                break;
            }
        }
        double s = 2.0 * sqrt(1.0 - w);
        double x = u * s, y = v * s, z = 1.0 - 2.0 * w;
        positions[i * 3 + 0] = x * 5.0;
        positions[i * 3 + 1] = y * 5.0;
        positions[i * 3 + 2] = z * 5.0;
    }
    float* normals = malloc(N * 3 * sizeof(float));
    CHECK(run_normals(positions, N, 16, normals, NULL) == 0);

    // For each point, |dot(normal, radial_direction)| should be ~1.
    size_t good = 0;
    for (size_t i = 0; i < N; i++) {
        double rx = positions[i * 3 + 0] / 5.0;
        double ry = positions[i * 3 + 1] / 5.0;
        double rz = positions[i * 3 + 2] / 5.0;
        double dot = (double)normals[i * 3 + 0] * rx + (double)normals[i * 3 + 1] * ry +
                     (double)normals[i * 3 + 2] * rz;
        if (fabs(fabs(dot) - 1.0) < 0.05) { // 5% tolerance — finite k on sphere
            good++;
        }
    }
    fprintf(stderr, "sphere: %zu/%zu radial within tol\n", good, N);
    CHECK(good >= N * 90 / 100);

    free(positions);
    free(normals);
}

static void test_normals_z_orientation(void) {
    register_test_steps();
    // Plane through z = -10 (below origin). Without z-flip the smallest
    // eigenvector could point either +z or -z; the step must always pick +z.
    const int side = 32;
    const size_t N = side * side;
    double* positions = malloc(N * 3 * sizeof(double));
    size_t i = 0;
    for (int y = 0; y < side; y++) {
        for (int x = 0; x < side; x++) {
            positions[i * 3 + 0] = x * 0.1;
            positions[i * 3 + 1] = y * 0.1;
            positions[i * 3 + 2] = -10.0;
            i++;
        }
    }
    float* normals = malloc(N * 3 * sizeof(float));
    CHECK(run_normals(positions, N, 16, normals, NULL) == 0);
    for (size_t j = 0; j < N; j++) {
        CHECK(normals[j * 3 + 2] >= 0.0f);
    }
    free(positions);
    free(normals);
}

static void test_normals_too_few_points_default(void) {
    register_test_steps();
    // 2 points, min_neighbors default 3. All normals must default to (0,0,1).
    double positions[6] = {0, 0, 0, 1, 1, 1};
    float normals[6];
    CHECK(run_normals(positions, 2, 16, normals, NULL) == 0);
    for (size_t j = 0; j < 2; j++) {
        CHECK(normals[j * 3 + 0] == 0.0f);
        CHECK(normals[j * 3 + 1] == 0.0f);
        CHECK(normals[j * 3 + 2] == 1.0f);
    }
}

// ----------------------------------------------------------------------------
// lidar-spacing tests
// ----------------------------------------------------------------------------

static void test_compute_spacing_uniform_grid(void) {
    register_test_steps();
    // 32x32 grid spacing 0.5 m at z=0. With k=16, the median (8th) neighbour
    // sits ~2 cells away in this regular lattice, so spacing should be a
    // positive, finite, roughly-constant value across interior points.
    const int side = 32;
    const size_t N = side * side;
    double* positions = malloc(N * 3 * sizeof(double));
    size_t i = 0;
    for (int y = 0; y < side; y++) {
        for (int x = 0; x < side; x++) {
            positions[i * 3 + 0] = x * 0.5;
            positions[i * 3 + 1] = y * 0.5;
            positions[i * 3 + 2] = 0.0;
            i++;
        }
    }
    float* normals = malloc(N * 3 * sizeof(float));
    float* spacing = safe_malloc_array(N, sizeof(float));
    CHECK(run_normals(positions, N, 16, normals, spacing) == 0);

    // Every spacing is positive and finite.
    for (size_t j = 0; j < N; j++) {
        CHECK(isfinite(spacing[j]));
        CHECK(spacing[j] > 0.0f);
    }

    // Interior points: spacing ~uniform. Pick the centre point and bound it.
    // The 8th k-NN distance on a unit grid (cells = 0.5 m) is ~sqrt(5)*0.5
    // ≈ 1.118 m — but tie-breaking varies, so just bound to a reasonable
    // window around the cell size.
    size_t centre = (side / 2) * side + (side / 2);
    CHECK(spacing[centre] > 0.5f);
    CHECK(spacing[centre] < 2.0f);

    free(positions);
    free(normals);
    free(spacing);
}

static void test_compute_spacing_dense_vs_sparse(void) {
    register_test_steps();
    // Two clusters: 64 dense points at (0,0,0) within a 0.1 m cube, and 64
    // sparse points at (100, 0, 0) within a 5 m cube. Spacing in the dense
    // cluster must be strictly smaller than spacing in the sparse cluster.
    rng_t r = {.state = 0xFEEDFACE12345678ULL};
    const size_t N = 128;
    double* positions = malloc(N * 3 * sizeof(double));
    for (size_t j = 0; j < 64; j++) {
        positions[j * 3 + 0] = rng_uniform(&r) * 0.1;
        positions[j * 3 + 1] = rng_uniform(&r) * 0.1;
        positions[j * 3 + 2] = rng_uniform(&r) * 0.1;
    }
    for (size_t j = 64; j < 128; j++) {
        positions[j * 3 + 0] = 100.0 + rng_uniform(&r) * 5.0;
        positions[j * 3 + 1] = rng_uniform(&r) * 5.0;
        positions[j * 3 + 2] = rng_uniform(&r) * 5.0;
    }
    float* normals = malloc(N * 3 * sizeof(float));
    float* spacing = safe_malloc_array(N, sizeof(float));
    CHECK(run_normals(positions, N, 16, normals, spacing) == 0);

    // Median spacing in each cluster.
    float dense_max = 0.0f, sparse_min = 1e30f;
    for (size_t j = 0; j < 64; j++) {
        if (spacing[j] > dense_max) dense_max = spacing[j];
    }
    for (size_t j = 64; j < 128; j++) {
        if (spacing[j] < sparse_min) sparse_min = spacing[j];
    }
    fprintf(stderr, "spacing: dense_max=%.4f sparse_min=%.4f\n", dense_max, sparse_min);
    CHECK(dense_max < sparse_min);

    free(positions);
    free(normals);
    free(spacing);
}

static void test_compute_spacing_few_points_fallback(void) {
    register_test_steps();
    // 2 points with default min_neighbors=3 → the early-exit path. Spacing
    // must default to 1.0 m for every point.
    double positions[6] = {0, 0, 0, 1, 1, 1};
    float normals[6];
    float spacing[2];
    CHECK(run_normals(positions, 2, 16, normals, spacing) == 0);
    for (size_t j = 0; j < 2; j++) {
        CHECK(spacing[j] == 1.0f);
    }
}

// ----------------------------------------------------------------------------

int main(void) {
    test_kdtree_random_matches_brute();
    test_kdtree_query_at_existing_point();
    test_kdtree_empty_and_single();

    test_pca_diagonal_matrix();
    test_pca_known_plane_normal();

    test_normals_horizontal_plane();
    test_normals_tilted_plane();
    test_normals_sphere_radial();
    test_normals_z_orientation();
    test_normals_too_few_points_default();

    test_compute_spacing_uniform_grid();
    test_compute_spacing_dense_vs_sparse();
    test_compute_spacing_few_points_fallback();

    printf("%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}

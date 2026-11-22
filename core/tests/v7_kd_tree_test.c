// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// core/tests/v7_kd_tree_test.c
//
// Implementation tests for the kd-tree (`steps/normals/kd_tree.{h,c}`).
// Each test asserts an invariant the algorithm spec claims to hold.
// Inputs are hand-crafted, minimal, deterministic. Tolerances are tight
// (binary pass/fail).
//
// Spec source: bench-v2 test/kd-tree-global.test.js (the JS unit tests
// encode the same invariants we want v3's port to satisfy).
//
// Implementation reference: bench-v2 lib/geometry/kd-tree.js +
// v3 steps/normals/kd_tree.c.
//
// Unlike the v7-*-impl test files we do NOT #include the .c source —
// kd_tree.h is the full public API (build, serialize/deserialize, knn,
// radius_search), and the test links against dioramap_steps_normals
// for the implementation.
//
// JS-vs-C API note: the bench-v2 typed-array API supports a *subset*
// build (`buildKdTree(positions, indices)` with indices ≠ 0..n-1)
// that returns global indices from queries. The v3 C API has no such
// subset variant — it builds over all positions in the supplied
// buffer. We cover the equivalent invariant (queries return indices
// into the caller's positions buffer) by constructing dedicated
// positions arrays per test rather than passing subset indices.

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE   700        // so <math.h> exposes M_PI / M_SQRT1_2

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kd_tree.h"

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

// Deterministic LCG so tests are reproducible without depending on libc rand.
typedef struct rng {
    uint64_t state;
} rng_t;

static double rng_uniform(rng_t* r) {
    r->state = r->state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (double)((r->state >> 11) & ((1ULL << 53) - 1)) / (double)(1ULL << 53);
}

// Brute-force k-NN reference. Returns the actual count (≤ k) sorted
// ascending by dist_sq. Stable selection sort; ties are resolved in
// insertion order, identical to how the kd-tree falls back when
// dist_sq are equal — but we compare on dist_sq, never on idx ties.
static void brute_force_knn(const double* positions, size_t n, const double q[3], int k,
                            kd_neighbor_t* out, int* out_count) {
    kd_neighbor_t* all = (kd_neighbor_t*)malloc(n * sizeof(kd_neighbor_t));
    for (size_t i = 0; i < n; i++) {
        double dx = positions[i * 3 + 0] - q[0];
        double dy = positions[i * 3 + 1] - q[1];
        double dz = positions[i * 3 + 2] - q[2];
        all[i].idx = (int32_t)i;
        all[i].dist_sq = dx * dx + dy * dy + dz * dz;
    }
    int actual = (int)n < k ? (int)n : k;
    for (int i = 0; i < actual; i++) {
        int min_j = i;
        for (size_t j = (size_t)i + 1; j < n; j++) {
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

// ─── build correctness ──────────────────────────────────────────────

static void test_build_three_points(void) {
    // Spec (JS: "builds a tree from Float32Array + Uint32Array"):
    // build on a 3-point cloud returns success and a non-empty tree
    // with a valid root index and three nodes.
    const double positions[9] = {
        0, 0, 0,
        1, 0, 0,
        0, 1, 0,
    };
    kd_tree_t tree;
    CHECK(kd_build(&tree, positions, 3) == 0);
    CHECK(tree.n_points == 3);
    CHECK(tree.root >= 0);
    CHECK(tree.nodes != NULL);
    // Every node's axis must be 0..2.
    for (size_t i = 0; i < tree.n_points; i++) {
        CHECK(tree.nodes[i].axis <= 2);
    }
    kd_free(&tree);
}

static void test_build_empty_tree(void) {
    // Spec (JS: "returns null for empty indices"): build with n_points=0
    // succeeds, leaves an empty tree (root = -1, no nodes allocated).
    kd_tree_t tree;
    CHECK(kd_build(&tree, NULL, 0) == 0);
    CHECK(tree.n_points == 0);
    CHECK(tree.root == -1);
    CHECK(tree.nodes == NULL);
    kd_free(&tree);
}

static void test_build_single_point(void) {
    // Spec (JS: "handles single-point subset"): a 1-point tree builds,
    // root points at the sole point.
    const double positions[3] = {5, 5, 5};
    kd_tree_t tree;
    CHECK(kd_build(&tree, positions, 1) == 0);
    CHECK(tree.root == 0);
    CHECK(tree.nodes[0].point_idx == 0);
    CHECK(tree.nodes[0].left == -1);
    CHECK(tree.nodes[0].right == -1);
    kd_free(&tree);
}

static void test_build_n_points_in_tree(void) {
    // Spec: every point in `positions` appears exactly once in the tree
    // (the build is a permutation of point indices).
    rng_t r = {.state = 0xC0FFEE1234567890ULL};
    const size_t N = 100;
    double* positions = (double*)malloc(N * 3 * sizeof(double));
    for (size_t i = 0; i < N; i++) {
        positions[i * 3 + 0] = rng_uniform(&r);
        positions[i * 3 + 1] = rng_uniform(&r);
        positions[i * 3 + 2] = rng_uniform(&r);
    }
    kd_tree_t tree;
    CHECK(kd_build(&tree, positions, N) == 0);

    // Collect every point_idx referenced in the node array; each must
    // appear exactly once.
    int seen[100] = {0};
    for (size_t i = 0; i < N; i++) {
        int32_t pi = tree.nodes[i].point_idx;
        CHECK(pi >= 0 && pi < (int32_t)N);
        seen[pi]++;
    }
    for (size_t i = 0; i < N; i++) {
        CHECK(seen[i] == 1);
    }
    kd_free(&tree);
    free(positions);
}

// ─── kNN correctness ─────────────────────────────────────────────────

static void test_knn_returns_k_neighbors(void) {
    // Spec (JS: "returns correct number of neighbors"): for k ≤ n,
    // kd_knn returns exactly k neighbors.
    const double positions[15] = {
        0, 0, 0,
        1, 0, 0,
        0, 1, 0,
        0, 0, 1,
        10, 10, 10,
    };
    kd_tree_t tree;
    CHECK(kd_build(&tree, positions, 5) == 0);

    double q[3] = {0, 0, 0};
    kd_neighbor_t out[3];
    int n;
    CHECK(kd_knn(&tree, q, 3, out, &n) == 0);
    CHECK(n == 3);
    kd_free(&tree);
}

static void test_knn_finds_closest(void) {
    // Spec (JS: "finds the closest point"): for k=1 on a query slightly
    // off point 0, the single neighbor returned is point 0.
    const double positions[15] = {
        0, 0, 0,
        1, 0, 0,
        0, 1, 0,
        0, 0, 1,
        10, 10, 10,
    };
    kd_tree_t tree;
    CHECK(kd_build(&tree, positions, 5) == 0);

    double q[3] = {0.1, 0, 0};
    kd_neighbor_t out[1];
    int n;
    CHECK(kd_knn(&tree, q, 1, out, &n) == 0);
    CHECK(n == 1);
    CHECK(out[0].idx == 0);
    CHECK_CLOSE(out[0].dist_sq, 0.01, 1e-15);
    kd_free(&tree);
}

static void test_knn_sorted_ascending(void) {
    // Spec (JS: "results are sorted ascending by distance"): kd_knn
    // populates `out` in ascending dist_sq order.
    const double positions[15] = {
        0, 0, 0,
        1, 0, 0,
        0, 1, 0,
        0, 0, 1,
        10, 10, 10,
    };
    kd_tree_t tree;
    CHECK(kd_build(&tree, positions, 5) == 0);

    double q[3] = {0, 0, 0};
    kd_neighbor_t out[4];
    int n;
    CHECK(kd_knn(&tree, q, 4, out, &n) == 0);
    CHECK(n == 4);
    for (int i = 1; i < n; i++) {
        CHECK(out[i].dist_sq >= out[i - 1].dist_sq);
    }
    kd_free(&tree);
}

static void test_knn_k_greater_than_n_returns_all(void) {
    // Spec: k > n_points must return all n points (no error, no overrun).
    // JS equivalent: kNearest on a 5-point tree with k=10 yields 5
    // results (its return-length matches `best.length`).
    const double positions[9] = {
        0, 0, 0,
        1, 0, 0,
        0, 1, 0,
    };
    kd_tree_t tree;
    CHECK(kd_build(&tree, positions, 3) == 0);

    double q[3] = {0, 0, 0};
    kd_neighbor_t out[10];
    int n;
    CHECK(kd_knn(&tree, q, 10, out, &n) == 0);
    CHECK(n == 3);
    kd_free(&tree);
}

static void test_knn_matches_brute_force(void) {
    // Spec (JS: implicit — the whole point of a kd-tree): kd_knn gives
    // the same dist_sq sequence as a brute-force O(nk) search on 50
    // random points and 5 random queries with k=7.
    rng_t r = {.state = 0xFEEDFACECAFEBEEFULL};
    const size_t N = 50;
    double* positions = (double*)malloc(N * 3 * sizeof(double));
    for (size_t i = 0; i < N; i++) {
        positions[i * 3 + 0] = rng_uniform(&r) * 10.0;
        positions[i * 3 + 1] = rng_uniform(&r) * 10.0;
        positions[i * 3 + 2] = rng_uniform(&r) * 10.0;
    }
    kd_tree_t tree;
    CHECK(kd_build(&tree, positions, N) == 0);

    const int K = 7;
    kd_neighbor_t kd_out[7], bf_out[7];
    for (int q = 0; q < 5; q++) {
        double query[3] = {rng_uniform(&r) * 10.0, rng_uniform(&r) * 10.0,
                           rng_uniform(&r) * 10.0};
        int kn, bn;
        CHECK(kd_knn(&tree, query, K, kd_out, &kn) == 0);
        brute_force_knn(positions, N, query, K, bf_out, &bn);
        CHECK(kn == bn);
        for (int i = 0; i < kn; i++) {
            // Ties (equal dist_sq) may resolve to different indices,
            // so we only check distances.
            CHECK_CLOSE(kd_out[i].dist_sq, bf_out[i].dist_sq, 1e-12);
        }
    }
    kd_free(&tree);
    free(positions);
}

static void test_knn_on_empty_tree(void) {
    // Spec (JS: "returns null tree gracefully" → []): kd_knn on an
    // empty tree yields 0 neighbours, returns success.
    kd_tree_t tree;
    CHECK(kd_build(&tree, NULL, 0) == 0);

    double q[3] = {0, 0, 0};
    kd_neighbor_t out[4];
    int n = -1;
    CHECK(kd_knn(&tree, q, 4, out, &n) == 0);
    CHECK(n == 0);
    kd_free(&tree);
}

static void test_knn_all_same_coordinates(void) {
    // Spec (JS: "handles all-same coordinates"): three identical points
    // at (5,5,5) — kNN of size 3 at (5,5,5) returns all three with
    // dist_sq = 0.
    const double positions[9] = {
        5, 5, 5,
        5, 5, 5,
        5, 5, 5,
    };
    kd_tree_t tree;
    CHECK(kd_build(&tree, positions, 3) == 0);

    double q[3] = {5, 5, 5};
    kd_neighbor_t out[3];
    int n;
    CHECK(kd_knn(&tree, q, 3, out, &n) == 0);
    CHECK(n == 3);
    for (int i = 0; i < n; i++) {
        CHECK_CLOSE(out[i].dist_sq, 0.0, 1e-15);
    }
    kd_free(&tree);
}

static void test_knn_100_grid_nearest_corner(void) {
    // Spec (JS: "handles large subset correctly"): 10×10 grid of points
    // at integer (x,y,0).  k=1 query at (4.5, 4.5, 0) must return a
    // grid point with |x−4.5| ≤ 0.6 and |y−4.5| ≤ 0.6.
    const int W = 10;
    const int N = W * W;
    double positions[300]; // 10×10×3
    for (int i = 0; i < N; i++) {
        positions[i * 3 + 0] = (double)(i % W);
        positions[i * 3 + 1] = (double)(i / W);
        positions[i * 3 + 2] = 0.0;
    }
    kd_tree_t tree;
    CHECK(kd_build(&tree, positions, (size_t)N) == 0);

    double q[3] = {4.5, 4.5, 0.0};
    kd_neighbor_t out[1];
    int n;
    CHECK(kd_knn(&tree, q, 1, out, &n) == 0);
    CHECK(n == 1);
    int32_t gi = out[0].idx;
    double x = positions[gi * 3 + 0];
    double y = positions[gi * 3 + 1];
    CHECK(fabs(x - 4.5) <= 0.6);
    CHECK(fabs(y - 4.5) <= 0.6);
    kd_free(&tree);
}

// ─── radius search ──────────────────────────────────────────────────

static void test_radius_finds_all_within(void) {
    // Spec (JS: "finds all points within radius"): the canonical 5-point
    // fixture at (0,0,0)+axes+(10,10,10), radius 1.5 around origin must
    // return 4 points (origin + 3 axis points; the far point is at dist
    // √300 ≈ 17.3).
    const double positions[15] = {
        0, 0, 0,
        1, 0, 0,
        0, 1, 0,
        0, 0, 1,
        10, 10, 10,
    };
    kd_tree_t tree;
    CHECK(kd_build(&tree, positions, 5) == 0);

    double q[3] = {0, 0, 0};
    kd_neighbor_t out[5];
    int n;
    CHECK(kd_radius_search(&tree, q, 1.5, out, 5, &n) == 0);
    CHECK(n == 4);
    kd_free(&tree);
}

static void test_radius_small_only_origin(void) {
    // Spec (JS: "finds only nearby points with small radius"): radius
    // 0.5 around origin must return exactly point 0 (the unit-axis
    // points are at dist 1).
    const double positions[15] = {
        0, 0, 0,
        1, 0, 0,
        0, 1, 0,
        0, 0, 1,
        10, 10, 10,
    };
    kd_tree_t tree;
    CHECK(kd_build(&tree, positions, 5) == 0);

    double q[3] = {0, 0, 0};
    kd_neighbor_t out[5];
    int n;
    CHECK(kd_radius_search(&tree, q, 0.5, out, 5, &n) == 0);
    CHECK(n == 1);
    CHECK(out[0].idx == 0);
    CHECK_CLOSE(out[0].dist_sq, 0.0, 1e-15);
    kd_free(&tree);
}

static void test_radius_on_empty_tree(void) {
    // Spec (JS: "returns empty for null tree"): radius search on an
    // empty tree returns 0 neighbours with success.
    kd_tree_t tree;
    CHECK(kd_build(&tree, NULL, 0) == 0);

    double q[3] = {0, 0, 0};
    kd_neighbor_t out[5];
    int n = -1;
    CHECK(kd_radius_search(&tree, q, 10.0, out, 5, &n) == 0);
    CHECK(n == 0);
    kd_free(&tree);
}

static void test_radius_matches_brute_count(void) {
    // Spec: radius_search returns exactly the count of points whose
    // squared distance to query is ≤ radius². Cross-check vs a
    // brute scan.
    rng_t r = {.state = 0x1234567890ABCDEFULL};
    const size_t N = 80;
    double* positions = (double*)malloc(N * 3 * sizeof(double));
    for (size_t i = 0; i < N; i++) {
        positions[i * 3 + 0] = rng_uniform(&r) * 5.0;
        positions[i * 3 + 1] = rng_uniform(&r) * 5.0;
        positions[i * 3 + 2] = rng_uniform(&r) * 5.0;
    }
    kd_tree_t tree;
    CHECK(kd_build(&tree, positions, N) == 0);

    double q[3] = {2.5, 2.5, 2.5};
    const double radius = 1.0;
    const double r2 = radius * radius;

    int expected = 0;
    for (size_t i = 0; i < N; i++) {
        double dx = positions[i * 3 + 0] - q[0];
        double dy = positions[i * 3 + 1] - q[1];
        double dz = positions[i * 3 + 2] - q[2];
        if (dx * dx + dy * dy + dz * dz <= r2) expected++;
    }

    kd_neighbor_t out[80];
    int n;
    CHECK(kd_radius_search(&tree, q, radius, out, 80, &n) == 0);
    CHECK(n == expected);
    // Every reported neighbour must be within radius.
    int reported = n < 80 ? n : 80;
    for (int i = 0; i < reported; i++) {
        CHECK(out[i].dist_sq <= r2 + 1e-12);
    }
    kd_free(&tree);
    free(positions);
}

// ─── serialize / deserialize round-trip ─────────────────────────────

static void test_serialize_deserialize_roundtrip(void) {
    // Spec: kd_serialize + kd_deserialize on the same positions buffer
    // must produce a tree whose k-NN answers are byte-identical to the
    // original tree's. This is the core artifact-cache invariant.
    rng_t r = {.state = 0xBADDCAFEDEADBEEFULL};
    const size_t N = 60;
    double* positions = (double*)malloc(N * 3 * sizeof(double));
    for (size_t i = 0; i < N; i++) {
        positions[i * 3 + 0] = rng_uniform(&r) * 100.0;
        positions[i * 3 + 1] = rng_uniform(&r) * 100.0;
        positions[i * 3 + 2] = rng_uniform(&r) * 100.0;
    }

    kd_tree_t orig;
    CHECK(kd_build(&orig, positions, N) == 0);

    uint8_t* bytes = NULL;
    size_t len = 0;
    CHECK(kd_serialize(&orig, &bytes, &len) == 0);
    CHECK(bytes != NULL);
    CHECK(len == N * sizeof(kd_node_t));

    kd_tree_t restored;
    CHECK(kd_deserialize(&restored, positions, N, bytes, len) == 0);
    CHECK(restored.n_points == N);
    CHECK(restored.root == 0);
    CHECK(restored.nodes_owned == false);
    // Node array bytes must be identical (it was a memcpy out and a
    // pointer-cast back).
    CHECK(memcmp(orig.nodes, restored.nodes, len) == 0);

    // K-NN answers from both trees must agree exactly.
    const int K = 5;
    double q[3] = {37.0, 51.0, 19.0};
    kd_neighbor_t a[5], b[5];
    int na, nb;
    CHECK(kd_knn(&orig, q, K, a, &na) == 0);
    CHECK(kd_knn(&restored, q, K, b, &nb) == 0);
    CHECK(na == nb);
    for (int i = 0; i < na; i++) {
        CHECK(a[i].idx == b[i].idx);
        CHECK_CLOSE(a[i].dist_sq, b[i].dist_sq, 1e-15);
    }

    kd_free(&orig);
    kd_free(&restored);
    free(bytes);
    free(positions);
}

static void test_serialize_empty_tree(void) {
    // Spec: serialising an empty tree yields a zero-length buffer
    // (kd_serialize must not allocate for the empty case).
    kd_tree_t empty;
    CHECK(kd_build(&empty, NULL, 0) == 0);
    uint8_t* bytes = (uint8_t*)0x1; // non-null sentinel
    size_t len = 99;
    CHECK(kd_serialize(&empty, &bytes, &len) == 0);
    CHECK(bytes == NULL);
    CHECK(len == 0);
    kd_free(&empty);
}

// ─── determinism ────────────────────────────────────────────────────

static void test_build_deterministic(void) {
    // Spec: building twice over the same positions yields the same tree
    // structure (same node array). The bench-v2 algorithm is purely
    // deterministic — qsort on a stable axis key — so this catches any
    // dependence on heap addresses or uninitialised memory.
    const size_t N = 30;
    rng_t r = {.state = 0xA1B2C3D4E5F60718ULL};
    double* positions = (double*)malloc(N * 3 * sizeof(double));
    for (size_t i = 0; i < N; i++) {
        positions[i * 3 + 0] = rng_uniform(&r);
        positions[i * 3 + 1] = rng_uniform(&r);
        positions[i * 3 + 2] = rng_uniform(&r);
    }

    kd_tree_t t1, t2;
    CHECK(kd_build(&t1, positions, N) == 0);
    CHECK(kd_build(&t2, positions, N) == 0);
    CHECK(t1.root == t2.root);
    CHECK(memcmp(t1.nodes, t2.nodes, N * sizeof(kd_node_t)) == 0);

    kd_free(&t1);
    kd_free(&t2);
    free(positions);
}

// ─── main ───────────────────────────────────────────────────────────

int main(void) {
    // Build correctness
    test_build_three_points();
    test_build_empty_tree();
    test_build_single_point();
    test_build_n_points_in_tree();

    // kNN
    test_knn_returns_k_neighbors();
    test_knn_finds_closest();
    test_knn_sorted_ascending();
    test_knn_k_greater_than_n_returns_all();
    test_knn_matches_brute_force();
    test_knn_on_empty_tree();
    test_knn_all_same_coordinates();
    test_knn_100_grid_nearest_corner();

    // Radius search
    test_radius_finds_all_within();
    test_radius_small_only_origin();
    test_radius_on_empty_tree();
    test_radius_matches_brute_count();

    // Serialize / deserialize
    test_serialize_deserialize_roundtrip();
    test_serialize_empty_tree();

    // Determinism
    test_build_deterministic();

    printf("%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// core/tests/v7_hemisphere_segment_test.c
//
// Implementation tests for v7-hemisphere-segment. Each test asserts a
// spec invariant ported from bench-v2's test/hemisphere-segment.test.js
// + lib/pipeline/steps/hemisphere-normals-density-aware-segment.js
// (meta.version=5). Inputs are hand-crafted, minimal, deterministic.
//
// We include v7-hemisphere-segment-impl.c directly to keep the test
// in a single TU (matching v7_tile_signals_test.c). The impl pulls in
// v7-lambert + v7-histogram + safe_alloc + kd_tree via its own header;
// link against dioramap_steps_normals for the kd-tree symbols.
//
// Scenarios kept from the JS suite focus on segmentation INVARIANTS
// (cluster count, partition completeness, area threshold, empty input).
// Skipped: anything that exercises end-to-end pipeline plumbing
// (artifact wiring, global-vs-local index mapping, point-weights
// computation) — those belong to integration / 3D-validity tests.
//
// All synthetic point generation uses a deterministic Mulberry32 PRNG.
// Tests are reproducible bit-for-bit across the three build flavours.

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE   700        // for M_PI in <math.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Pull in the impl as part of this TU. The hemisphere-segment impl
// also depends on v7-lambert + v7-histogram (file-static helpers
// live there); inline them so the test stays single-source-file (the
// CMake foreach in core/tests/CMakeLists.txt only knows about
// v7_<step>_test.c, not its transitive deps).
#include "v7-lambert.c"
#include "v7-histogram.c"
#include "v7-hemisphere-segment-impl.c"
#include "v7-mulberry32.h"

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
        g_failed++;                                                                                \
        }                                                                                          \
    } while (0)

// ─── synthetic-plane generator ──────────────────────────────────────
//
// Mirrors bench-v2's generatePlane(n, tiltDeg, azDeg, offset, opts):
// uniform u,v in [-width/2, +width/2] × [-depth/2, +depth/2], small
// gaussian-ish position jitter, ±0.01 normal jitter. We use Mulberry32
// for byte-determinism across builds (rather than rand()).

typedef struct gen_opts {
    double width;
    double depth;
    double noise_std;   // position jitter half-range
} gen_opts_t;

// Generate `n` points on a tilted plane. Outputs interleaved positions
// (F64×3·n) and normals (F32×3·n) into caller buffers. `tilt_deg=0`
// gives flat y-up; `tilt_deg=90` gives a vertical wall. `az_deg` is the
// azimuth in the XZ plane.
static void gen_plane(v7_rng_t* rng,
                      int n, double tilt_deg, double az_deg, double offset,
                      gen_opts_t opts,
                      double* positions, float* normals) {
    double t = tilt_deg * M_PI / 180.0;
    double a = az_deg * M_PI / 180.0;
    double sinT = sin(t), cosT = cos(t);
    double sinA = sin(a), cosA = cos(a);

    // Plane normal:
    double nx = sinT * sinA;
    double ny = cosT;
    double nz = sinT * cosA;

    // Two tangent vectors perpendicular to normal (Gram-Schmidt'ish):
    double arb_x, arb_y, arb_z;
    if (fabs(ny) < 0.9) { arb_x = 0; arb_y = 1; arb_z = 0; }
    else                { arb_x = 1; arb_y = 0; arb_z = 0; }
    double t1x = ny * arb_z - nz * arb_y;
    double t1y = nz * arb_x - nx * arb_z;
    double t1z = nx * arb_y - ny * arb_x;
    double l1 = sqrt(t1x * t1x + t1y * t1y + t1z * t1z);
    if (l1 > 0) { t1x /= l1; t1y /= l1; t1z /= l1; }
    double t2x = ny * t1z - nz * t1y;
    double t2y = nz * t1x - nx * t1z;
    double t2z = nx * t1y - ny * t1x;
    double l2 = sqrt(t2x * t2x + t2y * t2y + t2z * t2z);
    if (l2 > 0) { t2x /= l2; t2y /= l2; t2z /= l2; }

    for (int i = 0; i < n; i++) {
        double u = v7_rng_next(rng) * opts.width - opts.width / 2.0;
        double v = v7_rng_next(rng) * opts.depth - opts.depth / 2.0;
        double jx = (v7_rng_next(rng) - 0.5) * opts.noise_std * 2.0;
        double jy = (v7_rng_next(rng) - 0.5) * opts.noise_std * 2.0;
        double jz = (v7_rng_next(rng) - 0.5) * opts.noise_std * 2.0;
        positions[i * 3 + 0] = offset * nx + u * t1x + v * t2x + jx;
        positions[i * 3 + 1] = offset * ny + u * t1y + v * t2y + jy;
        positions[i * 3 + 2] = offset * nz + u * t1z + v * t2z + jz;
        // Normals with small jitter:
        float jn_x = (float)((v7_rng_next(rng) - 0.5) * 0.02);
        float jn_y = (float)((v7_rng_next(rng) - 0.5) * 0.02);
        float jn_z = (float)((v7_rng_next(rng) - 0.5) * 0.02);
        normals[i * 3 + 0] = (float)nx + jn_x;
        normals[i * 3 + 1] = (float)ny + jn_y;
        normals[i * 3 + 2] = (float)nz + jn_z;
    }
}

// ─── runner: build kd-tree, call v7_hs_compute, return outputs ──────
//
// On entry the caller has filled positions/normals/weights for n_pts.
// On exit, `out` holds the results; caller must call v7_hs_outputs_free.
// Tree is owned here and freed after — outputs hold copies / their own
// pt_indices arrays so the tree can disappear.

static int run_step(const double* positions,
                    const float* normals,
                    const float* weights,
                    size_t n_pts,
                    const v7_hs_params_t* params,
                    v7_hs_outputs_t* out) {
    kd_tree_t tree;
    memset(&tree, 0, sizeof(tree));
    if (n_pts >= 3) {
        if (kd_build(&tree, positions, n_pts) != 0) {
            return -1;
        }
    }
    int rc = v7_hs_compute(positions, normals, weights, n_pts,
                            n_pts >= 3 ? &tree : NULL,
                            NULL, params, out);
    if (n_pts >= 3) kd_free(&tree);
    return rc;
}

static void params_for_tests(v7_hs_params_t* p) {
    v7_hs_params_default(p);
    // Match the JS test config (smaller lambert for speed, lower area floor):
    p->lambert_resolution = 64;
    p->min_segment_area = 0.5;
    p->emit_viz_artifacts = false;   // we don't assert on viz here
}

// ─── shared per-point uniform weight buffer ─────────────────────────
//
// In v3, point weights are an input — emitted by v7-tile-signals
// upstream. For a w×d plane sampled with n points, the per-point
// Voronoi area is roughly (w·d)/n. We approximate with a uniform
// weight = area/n. The exact value matters only insofar as the sum
// crosses min_segment_area: tests pick numbers so the answer is
// unambiguous.

static void fill_uniform_weights(float* w, size_t n, double total_area) {
    float per = (float)(total_area / (double)n);
    for (size_t i = 0; i < n; i++) w[i] = per;
}

// ─── tests ──────────────────────────────────────────────────────────

// "Scenario A": 500 points on a single flat plane → exactly 1 segment.
// Spec invariant: a single roof plane is one segment.
static void test_single_flat_plane_one_segment(void) {
    const int N = 500;
    double* positions = (double*)malloc(N * 3 * sizeof(double));
    float*  normals   = (float*) malloc(N * 3 * sizeof(float));
    float*  weights   = (float*) malloc(N * sizeof(float));

    v7_rng_t rng; v7_rng_init(&rng, 1);
    gen_opts_t opts = { .width = 5, .depth = 5, .noise_std = 0.005 };
    gen_plane(&rng, N, 0.0, 0.0, 5.0, opts, positions, normals);
    fill_uniform_weights(weights, N, 5.0 * 5.0);

    v7_hs_params_t params; params_for_tests(&params);
    v7_hs_outputs_t out; v7_hs_outputs_init(&out);
    CHECK(run_step(positions, normals, weights, N, &params, &out) == 0);
    CHECK(out.n_segments == 1);

    v7_hs_outputs_free(&out);
    free(positions); free(normals); free(weights);
}

// "Scenario B": gable — two 25° planes with opposite azimuths →
// 2 segments, each containing points from only one side.
static void test_simple_gable_two_segments(void) {
    const int NSIDE = 500;
    const int N = NSIDE * 2;
    double* positions = (double*)malloc(N * 3 * sizeof(double));
    float*  normals   = (float*) malloc(N * 3 * sizeof(float));
    float*  weights   = (float*) malloc(N * sizeof(float));

    v7_rng_t rng; v7_rng_init(&rng, 2);
    gen_opts_t opts = { .width = 5, .depth = 5, .noise_std = 0.005 };
    gen_plane(&rng, NSIDE, 25.0,  90.0, 5.0, opts, positions,                 normals);
    gen_plane(&rng, NSIDE, 25.0, 270.0, 5.0, opts, positions + NSIDE * 3,     normals + NSIDE * 3);
    fill_uniform_weights(weights, N, 5.0 * 5.0 * 2.0);

    v7_hs_params_t params; params_for_tests(&params);
    v7_hs_outputs_t out; v7_hs_outputs_init(&out);
    CHECK(run_step(positions, normals, weights, N, &params, &out) == 0);
    CHECK(out.n_segments == 2);

    // Per-segment purity: each segment is dominantly from one side.
    for (size_t s = 0; s < out.n_segments; s++) {
        uint32_t from_left = 0, from_right = 0;
        for (uint32_t k = 0; k < out.segments[s].n_pts; k++) {
            uint32_t idx = out.segments[s].pt_indices[k];
            if (idx < (uint32_t)NSIDE) from_left++;
            else                        from_right++;
        }
        uint32_t dom = from_left > from_right ? from_left : from_right;
        double purity = (double)dom / (double)out.segments[s].n_pts;
        CHECK(purity > 0.85);
    }

    v7_hs_outputs_free(&out);
    free(positions); free(normals); free(weights);
}

// "Scenario C": flat + tilted (a hip corner) → ≥2 segments.
static void test_flat_plus_tilted_at_least_two_segments(void) {
    const int NSIDE = 300;
    const int N = NSIDE * 2;
    double* positions = (double*)malloc(N * 3 * sizeof(double));
    float*  normals   = (float*) malloc(N * 3 * sizeof(float));
    float*  weights   = (float*) malloc(N * sizeof(float));

    v7_rng_t rng; v7_rng_init(&rng, 3);
    gen_opts_t opts = { .width = 5, .depth = 5, .noise_std = 0.005 };
    gen_plane(&rng, NSIDE,  0.0, 0.0, 8.0, opts, positions,                 normals);
    gen_plane(&rng, NSIDE, 30.0, 0.0, 5.0, opts, positions + NSIDE * 3,     normals + NSIDE * 3);
    fill_uniform_weights(weights, N, 5.0 * 5.0 * 2.0);

    v7_hs_params_t params; params_for_tests(&params);
    v7_hs_outputs_t out; v7_hs_outputs_init(&out);
    CHECK(run_step(positions, normals, weights, N, &params, &out) == 0);
    CHECK(out.n_segments >= 2);

    v7_hs_outputs_free(&out);
    free(positions); free(normals); free(weights);
}

// "Scenario D": two parallel planes, different offsets → 2 segments.
// Spec invariant for Stage 2 (weighted offset KDE): same normal,
// disparate offsets must split into two clusters.
static void test_two_parallel_planes_different_offsets(void) {
    const int NSIDE = 500;
    const int N = NSIDE * 2;
    double* positions = (double*)malloc(N * 3 * sizeof(double));
    float*  normals   = (float*) malloc(N * 3 * sizeof(float));
    float*  weights   = (float*) malloc(N * sizeof(float));

    v7_rng_t rng; v7_rng_init(&rng, 4);
    gen_opts_t opts = { .width = 5, .depth = 5, .noise_std = 0.005 };
    gen_plane(&rng, NSIDE, 20.0, 45.0, 3.0, opts, positions,                 normals);
    gen_plane(&rng, NSIDE, 20.0, 45.0, 7.0, opts, positions + NSIDE * 3,     normals + NSIDE * 3);
    fill_uniform_weights(weights, N, 5.0 * 5.0 * 2.0);

    v7_hs_params_t params; params_for_tests(&params);
    v7_hs_outputs_t out; v7_hs_outputs_init(&out);
    CHECK(run_step(positions, normals, weights, N, &params, &out) == 0);
    CHECK(out.n_segments == 2);

    v7_hs_outputs_free(&out);
    free(positions); free(normals); free(weights);
}

// "Scenario H": area threshold — small noise cluster rejected, sparse
// plane kept. Spec invariant: min_segment_area floor.
static void test_area_threshold_noise_rejected(void) {
    // 50 plane points (2×2 patch → 4 m² total).
    const int N_PLANE = 50;
    // 20 noise points jammed into a tiny 0.1m³ box, all with a
    // different normal so they form their own Lambert basin.
    const int N_NOISE = 20;
    const int N = N_PLANE + N_NOISE;

    double* positions = (double*)malloc(N * 3 * sizeof(double));
    float*  normals   = (float*) malloc(N * 3 * sizeof(float));
    float*  weights   = (float*) malloc(N * sizeof(float));

    v7_rng_t rng; v7_rng_init(&rng, 5);
    gen_opts_t opts_plane = { .width = 2, .depth = 2, .noise_std = 0.005 };
    gen_plane(&rng, N_PLANE, 30.0, 180.0, 0.0, opts_plane,
              positions, normals);

    // Noise cluster: tight 0.1 m box near (10, 5, 10), all with
    // normal ≈ (0.3, 0.9, 0.1) — well separated from the plane's
    // normal so it lands on a different Lambert peak.
    for (int i = 0; i < N_NOISE; i++) {
        positions[(N_PLANE + i) * 3 + 0] = 10.0 + (v7_rng_next(&rng) - 0.5) * 0.1;
        positions[(N_PLANE + i) * 3 + 1] =  5.0 + (v7_rng_next(&rng) - 0.5) * 0.1;
        positions[(N_PLANE + i) * 3 + 2] = 10.0 + (v7_rng_next(&rng) - 0.5) * 0.1;
        normals  [(N_PLANE + i) * 3 + 0] = 0.3f;
        normals  [(N_PLANE + i) * 3 + 1] = 0.9f;
        normals  [(N_PLANE + i) * 3 + 2] = 0.1f;
    }

    // Plane weight = ~4 m² over 50 pts; noise weight tiny per point.
    for (int i = 0; i < N_PLANE; i++) weights[i] = (float)(4.0 / N_PLANE);
    for (int i = 0; i < N_NOISE; i++) weights[N_PLANE + i] = (float)(0.01 / N_NOISE);

    v7_hs_params_t params; params_for_tests(&params);
    params.min_segment_area = 0.5;
    v7_hs_outputs_t out; v7_hs_outputs_init(&out);
    CHECK(run_step(positions, normals, weights, N, &params, &out) == 0);

    // The plane segment should survive. Find any segment that holds
    // a majority of plane indices.
    int plane_seg_found = 0;
    for (size_t s = 0; s < out.n_segments; s++) {
        uint32_t from_plane = 0;
        for (uint32_t k = 0; k < out.segments[s].n_pts; k++) {
            if (out.segments[s].pt_indices[k] < (uint32_t)N_PLANE) from_plane++;
        }
        if (from_plane > (uint32_t)(N_PLANE / 2)) plane_seg_found = 1;
    }
    CHECK(plane_seg_found);

    // Noise points must NOT all live in a segment of their own:
    // the noise cluster has ≈ 0.01 m² of weight, well below the
    // 0.5 m² floor — every noise point should either be unassigned
    // (cascade_segments == -1) or flagged orphan (ridge_candidates
    // bit 7 set), but NOT bundled into a single survived segment.
    int noise_in_segment = 0;
    for (int i = N_PLANE; i < N; i++) {
        if (out.cascade_segments[i] >= 0) noise_in_segment++;
    }
    CHECK(noise_in_segment == 0);

    v7_hs_outputs_free(&out);
    free(positions); free(normals); free(weights);
}

// "Partition completeness": every input point ends up in exactly
// one of {assigned segment, orphan}. Spec invariant: no point is
// silently lost (no "in segment AND orphaned" double-state for the
// load-bearing cascade-segments array).
static void test_partition_completeness(void) {
    const int NSIDE = 500;
    const int N = NSIDE * 2;
    double* positions = (double*)malloc(N * 3 * sizeof(double));
    float*  normals   = (float*) malloc(N * 3 * sizeof(float));
    float*  weights   = (float*) malloc(N * sizeof(float));

    v7_rng_t rng; v7_rng_init(&rng, 6);
    gen_opts_t opts = { .width = 5, .depth = 5, .noise_std = 0.005 };
    gen_plane(&rng, NSIDE, 25.0,  90.0, 5.0, opts, positions,                 normals);
    gen_plane(&rng, NSIDE, 25.0, 270.0, 5.0, opts, positions + NSIDE * 3,     normals + NSIDE * 3);
    fill_uniform_weights(weights, N, 5.0 * 5.0 * 2.0);

    v7_hs_params_t params; params_for_tests(&params);
    v7_hs_outputs_t out; v7_hs_outputs_init(&out);
    CHECK(run_step(positions, normals, weights, N, &params, &out) == 0);

    // Build a per-point "covered" array from the union of segment
    // point lists. Each index must appear in at most one segment.
    int* covered = (int*)calloc(N, sizeof(int));
    for (size_t s = 0; s < out.n_segments; s++) {
        for (uint32_t k = 0; k < out.segments[s].n_pts; k++) {
            uint32_t idx = out.segments[s].pt_indices[k];
            CHECK(idx < (uint32_t)N);
            covered[idx]++;
        }
    }
    // No double-coverage anywhere:
    for (int i = 0; i < N; i++) CHECK(covered[i] <= 1);

    // Every point either has cascade_segments[i] >= 0 OR is flagged
    // orphan (ridge_candidates[i] & 0x80). No "missing" points.
    for (int i = 0; i < N; i++) {
        int in_seg = out.cascade_segments[i] >= 0;
        int orphan = (out.ridge_candidates[i] & 0x80) != 0;
        CHECK(in_seg || orphan);
    }

    // Cross-check: cascade_segments[i] >= 0 iff index i appears in
    // exactly one segment's pt_indices.
    for (int i = 0; i < N; i++) {
        int in_seg = out.cascade_segments[i] >= 0;
        CHECK(in_seg == covered[i]);
    }

    free(covered);
    v7_hs_outputs_free(&out);
    free(positions); free(normals); free(weights);
}

// "Segment IDs are dense": ids run 0..n_segments-1 with no gaps.
// Spec invariant (cascade-segments storage convention).
static void test_segment_ids_dense(void) {
    const int NSIDE = 500;
    const int N = NSIDE * 2;
    double* positions = (double*)malloc(N * 3 * sizeof(double));
    float*  normals   = (float*) malloc(N * 3 * sizeof(float));
    float*  weights   = (float*) malloc(N * sizeof(float));

    v7_rng_t rng; v7_rng_init(&rng, 7);
    gen_opts_t opts = { .width = 5, .depth = 5, .noise_std = 0.005 };
    gen_plane(&rng, NSIDE, 25.0,  90.0, 5.0, opts, positions,                 normals);
    gen_plane(&rng, NSIDE, 25.0, 270.0, 5.0, opts, positions + NSIDE * 3,     normals + NSIDE * 3);
    fill_uniform_weights(weights, N, 5.0 * 5.0 * 2.0);

    v7_hs_params_t params; params_for_tests(&params);
    v7_hs_outputs_t out; v7_hs_outputs_init(&out);
    CHECK(run_step(positions, normals, weights, N, &params, &out) == 0);

    // Segment IDs are the index in `segments[]` (id == array slot).
    for (size_t s = 0; s < out.n_segments; s++) {
        CHECK(out.segments[s].id == (int32_t)s);
    }

    // cascade_segments only references valid IDs in [-1, n_segments).
    for (size_t i = 0; i < out.n_pts; i++) {
        int16_t v = out.cascade_segments[i];
        CHECK(v >= -1 && v < (int16_t)out.n_segments);
    }

    v7_hs_outputs_free(&out);
    free(positions); free(normals); free(weights);
}

// "Segment n_pts matches its pt_indices length, and the indexed
// global slot in cascade_segments points back to that segment id."
// Spec invariant: load-bearing two-way consistency between the
// per-point Int16Array and the per-segment metadata list.
static void test_segment_back_reference(void) {
    const int NSIDE = 500;
    const int N = NSIDE * 2;
    double* positions = (double*)malloc(N * 3 * sizeof(double));
    float*  normals   = (float*) malloc(N * 3 * sizeof(float));
    float*  weights   = (float*) malloc(N * sizeof(float));

    v7_rng_t rng; v7_rng_init(&rng, 8);
    gen_opts_t opts = { .width = 5, .depth = 5, .noise_std = 0.005 };
    gen_plane(&rng, NSIDE, 25.0,  90.0, 5.0, opts, positions,                 normals);
    gen_plane(&rng, NSIDE, 25.0, 270.0, 5.0, opts, positions + NSIDE * 3,     normals + NSIDE * 3);
    fill_uniform_weights(weights, N, 5.0 * 5.0 * 2.0);

    v7_hs_params_t params; params_for_tests(&params);
    v7_hs_outputs_t out; v7_hs_outputs_init(&out);
    CHECK(run_step(positions, normals, weights, N, &params, &out) == 0);

    for (size_t s = 0; s < out.n_segments; s++) {
        for (uint32_t k = 0; k < out.segments[s].n_pts; k++) {
            uint32_t idx = out.segments[s].pt_indices[k];
            CHECK(idx < (uint32_t)N);
            CHECK(out.cascade_segments[idx] == (int16_t)s);
        }
    }

    v7_hs_outputs_free(&out);
    free(positions); free(normals); free(weights);
}

// "Empty input handled": n_pts < 3 returns 0 segments cleanly.
// Spec invariant: early-exit path doesn't allocate junk or crash.
static void test_empty_input_handled(void) {
    v7_hs_params_t params; params_for_tests(&params);
    v7_hs_outputs_t out; v7_hs_outputs_init(&out);
    CHECK(run_step(NULL, NULL, NULL, 0, &params, &out) == 0);
    CHECK(out.n_segments == 0);
    CHECK(out.n_pts == 0);
    v7_hs_outputs_free(&out);
}

// "Below threshold (n_pts < 3) handled": 2 points → 0 segments,
// no crash. cascade_segments allocated & initialised to -1.
static void test_two_points_no_segments(void) {
    double positions[6] = {0, 5, 0,  1, 5, 0};
    float  normals[6]   = {0, 1, 0,  0, 1, 0};
    float  weights[2]   = {1.0f, 1.0f};

    v7_hs_params_t params; params_for_tests(&params);
    v7_hs_outputs_t out; v7_hs_outputs_init(&out);
    CHECK(run_step(positions, normals, weights, 2, &params, &out) == 0);
    CHECK(out.n_segments == 0);
    CHECK(out.n_pts == 2);
    CHECK(out.cascade_segments != NULL);
    CHECK(out.cascade_segments[0] == -1);
    CHECK(out.cascade_segments[1] == -1);
    v7_hs_outputs_free(&out);
}

// "All-identical normals → 1 segment".
// Spec invariant analogue of "100 normals all = (0,1,0) → 1 segment".
// We use ~ε normal noise so the kd-tree still gets distinct positions;
// the Lambert peak collapses to one bin and the offset KDE produces
// one cluster.
static void test_all_identical_normals_one_segment(void) {
    const int N = 200;
    double* positions = (double*)malloc(N * 3 * sizeof(double));
    float*  normals   = (float*) malloc(N * 3 * sizeof(float));
    float*  weights   = (float*) malloc(N * sizeof(float));

    v7_rng_t rng; v7_rng_init(&rng, 9);
    // Flat patch at y = 2; positions vary in x,z, normals all = (0,1,0).
    for (int i = 0; i < N; i++) {
        positions[i * 3 + 0] = (v7_rng_next(&rng) - 0.5) * 4.0;
        positions[i * 3 + 1] = 2.0;
        positions[i * 3 + 2] = (v7_rng_next(&rng) - 0.5) * 4.0;
        normals[i * 3 + 0] = 0.0f;
        normals[i * 3 + 1] = 1.0f;
        normals[i * 3 + 2] = 0.0f;
    }
    fill_uniform_weights(weights, N, 4.0 * 4.0);

    v7_hs_params_t params; params_for_tests(&params);
    v7_hs_outputs_t out; v7_hs_outputs_init(&out);
    CHECK(run_step(positions, normals, weights, N, &params, &out) == 0);
    CHECK(out.n_segments == 1);
    // And every point lands in that one segment.
    if (out.n_segments == 1) {
        CHECK(out.segments[0].n_pts == (uint32_t)N);
    }

    v7_hs_outputs_free(&out);
    free(positions); free(normals); free(weights);
}

// "Density gradient on a single plane → still 1 segment" (Scenario E,
// the KEY test in the JS suite). A pre-density-aware count-based
// implementation would split the sparse side off as its own group;
// the spec mandates the area-weighted path keep them together.
static void test_density_gradient_single_segment(void) {
    const int N = 1000;
    double* positions = (double*)malloc(N * 3 * sizeof(double));
    float*  normals   = (float*) malloc(N * 3 * sizeof(float));
    float*  weights   = (float*) malloc(N * sizeof(float));

    v7_rng_t rng; v7_rng_init(&rng, 10);
    // Single tilted plane (20°, az=45°, offset=5). Accept/reject
    // sample u with density 1 - 0.9 * (u + 2.5)/5: full density on
    // the left, 1/10th on the right.
    const double tilt_deg = 20.0, az_deg = 45.0, offset = 5.0;
    const double width = 5.0, depth = 5.0;
    double t = tilt_deg * M_PI / 180.0;
    double a = az_deg * M_PI / 180.0;
    double sinT = sin(t), cosT = cos(t);
    double sinA = sin(a), cosA = cos(a);
    double nx = sinT * sinA, ny = cosT, nz = sinT * cosA;
    // Tangents:
    double arb_x = 0, arb_y = 1, arb_z = 0;
    if (fabs(ny) >= 0.9) { arb_x = 1; arb_y = 0; arb_z = 0; }
    double t1x = ny * arb_z - nz * arb_y;
    double t1y = nz * arb_x - nx * arb_z;
    double t1z = nx * arb_y - ny * arb_x;
    double l1 = sqrt(t1x*t1x + t1y*t1y + t1z*t1z);
    t1x /= l1; t1y /= l1; t1z /= l1;
    double t2x = ny * t1z - nz * t1y;
    double t2y = nz * t1x - nx * t1z;
    double t2z = nx * t1y - ny * t1x;
    double l2 = sqrt(t2x*t2x + t2y*t2y + t2z*t2z);
    t2x /= l2; t2y /= l2; t2z /= l2;

    int filled = 0;
    // Loop guard prevents pathological infinite loop on bad density fn.
    for (int iter = 0; iter < N * 100 && filled < N; iter++) {
        double u = v7_rng_next(&rng) * width - width / 2.0;
        double v = v7_rng_next(&rng) * depth - depth / 2.0;
        double density = 1.0 - 0.9 * ((u + 2.5) / 5.0);
        if (v7_rng_next(&rng) >= density) continue;

        double jx = (v7_rng_next(&rng) - 0.5) * 0.01;
        double jy = (v7_rng_next(&rng) - 0.5) * 0.01;
        double jz = (v7_rng_next(&rng) - 0.5) * 0.01;
        positions[filled * 3 + 0] = offset * nx + u * t1x + v * t2x + jx;
        positions[filled * 3 + 1] = offset * ny + u * t1y + v * t2y + jy;
        positions[filled * 3 + 2] = offset * nz + u * t1z + v * t2z + jz;
        normals[filled * 3 + 0] = (float)nx + (float)((v7_rng_next(&rng) - 0.5) * 0.02);
        normals[filled * 3 + 1] = (float)ny + (float)((v7_rng_next(&rng) - 0.5) * 0.02);
        normals[filled * 3 + 2] = (float)nz + (float)((v7_rng_next(&rng) - 0.5) * 0.02);
        filled++;
    }
    CHECK(filled == N);
    // Per-point area = total plane area / N (uniform weights; the
    // density variation lives in the position distribution, not in
    // the weights — same as the JS test path, which lets KDE peaks
    // arbitrate density via per-point KDE bandwidth on offset).
    fill_uniform_weights(weights, N, width * depth);

    v7_hs_params_t params; params_for_tests(&params);
    v7_hs_outputs_t out; v7_hs_outputs_init(&out);
    CHECK(run_step(positions, normals, weights, N, &params, &out) == 0);
    CHECK(out.n_segments == 1);

    v7_hs_outputs_free(&out);
    free(positions); free(normals); free(weights);
}

// ─── main ───────────────────────────────────────────────────────────

int main(void) {
    test_empty_input_handled();
    test_two_points_no_segments();
    test_all_identical_normals_one_segment();
    test_single_flat_plane_one_segment();
    test_simple_gable_two_segments();
    test_flat_plus_tilted_at_least_two_segments();
    test_two_parallel_planes_different_offsets();
    test_density_gradient_single_segment();
    test_area_threshold_noise_rejected();
    test_partition_completeness();
    test_segment_ids_dense();
    test_segment_back_reference();

    printf("%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}

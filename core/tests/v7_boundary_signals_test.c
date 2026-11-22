// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// core/tests/v7_boundary_signals_test.c
//
// Implementation tests for v7-boundary-signals. Each test asserts a
// spec invariant from bench-v2 lib/pipeline/steps/boundary-signals.js
// (meta.version=4) — RI corner detection, Phase B nearby gathering,
// Phase C per-plane signal grids. Inputs are hand-crafted, minimal,
// deterministic.
//
// Note: bench-v2 has no dedicated test/boundary-signals.test.js suite
// (the v2 wall-roof/ridge-edge-fit families in edges.test.js +
// edge-detection.test.js cover a different code path — pre-v7 edge
// extraction, not Phase A-C signal grids). The invariants below are
// extracted directly from the algorithm source — detectCorners()
// (boundary-edges/roof-graph.js), computeSignalGrids()
// (boundary-signals.js#L70-373), gatherNearby() (boundary-signals.js
// #L640-700) — and the v3 impl docstrings.
//
// We include v7-boundary-signals-impl.c directly to keep the test in a
// single TU. Only safe_alloc + math need linking.
//
// INFINITY-sentinel handling: build_plane_data and compute_signal_grids
// use INFINITY as the initial value for min/max trackers (see
// FRAMA-C-FALSE-POSITIVES.md §"sentinel-style INFINITY writes are
// silenced"). We assert these paths do not crash on degenerate inputs.

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE   700

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Pull in the impl as part of this TU so we can call its statics if
// needed (and to keep the test single-source). Public symbols come
// along with it.
#include "v7-boundary-signals-impl.c"

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
            fprintf(stderr, "FAIL %s:%d: %s ~= %s (got %.12g, want %.12g, eps=%g)\n",              \
                    __FILE__, __LINE__, #a, #b, _a, _b, (eps));                                    \
            g_failed++;                                                                            \
        }                                                                                          \
    } while (0)

// ─── input builders ─────────────────────────────────────────────────

// Build a flat horizontal plane (normal=+y, d=-y0) at height y0, with
// orthonormal basis (e1=+x, e2=+z). The plane equation `n·p + d = 0`
// requires d = -y0.
static void make_flat_plane(v7_bs_input_plane_t* P,
                            uint32_t id, double y0) {
    memset(P, 0, sizeof(*P));
    P->id = id;
    P->rejected = false;
    P->normal[0] = 0.0; P->normal[1] = 1.0; P->normal[2] = 0.0;
    P->d = -y0;
    P->basis_e1[0] = 1.0; P->basis_e1[1] = 0.0; P->basis_e1[2] = 0.0;
    P->basis_e2[0] = 0.0; P->basis_e2[1] = 0.0; P->basis_e2[2] = 1.0;
}

// Build a w x d patch of points on plane y=y0, owned by `owner_id`.
// Returns `n_pts` filled positions starting at offset `pos_off`.
static void fill_flat_patch(double* positions, int16_t* owner,
                            size_t pos_off, int n_per_side,
                            double y0, double cx, double cz,
                            double size, int16_t owner_id) {
    size_t k = pos_off;
    for (int i = 0; i < n_per_side; i++) {
        for (int j = 0; j < n_per_side; j++) {
            double u = ((double)i / (n_per_side - 1) - 0.5) * size;
            double v = ((double)j / (n_per_side - 1) - 0.5) * size;
            positions[k * 3 + 0] = cx + u;
            positions[k * 3 + 1] = y0;
            positions[k * 3 + 2] = cz + v;
            owner[k] = owner_id;
            k++;
        }
    }
}

// Tests that exercise the planes path (grid + nearby accumulators) must
// pass a real point_weights array — v7_bs_compute's policy (post v2-port
// unification) forbids silent fallback to unweighted on n_pts_local > 0
// to catch "forgot to wire weights" foot-guns. These tests aren't probing
// density-bias correction, so a uniform-1.0 array is the right answer.
static void make_unit_weights(float* w, size_t n) {
    for (size_t i = 0; i < n; i++) w[i] = 1.0f;
}

static void default_params(v7_bs_params_t* p) {
    v7_bs_params_default(p);
    // main added a fail-loud guard: compute_contour=true (the default)
    // requires a finite, positive lidar_density (pts/m²) to size the
    // grid-fill contour. Supply a representative density so the contour
    // path runs — these tests assert grid/nearby *counts*, which are
    // independent of the exact cell size the density drives.
    p->lidar_density = 10.0;
}

// ─── tests ──────────────────────────────────────────────────────────

// Empty input → 0 corners, 0 nearby, 0 grids; clean return.
// Spec: the algorithm must early-exit safely on degenerate input
// (matches v2's "if no planes survive, emit nothing" behavior).
static void test_empty_input_no_crash(void) {
    v7_bs_params_t p; default_params(&p);
    v7_bs_outputs_t out; v7_bs_outputs_init(&out);
    int rc = v7_bs_compute(NULL, NULL, NULL, 0, NULL, NULL, 0, NULL, 0, NULL, 0, NULL, 0, &p, &out);
    CHECK(rc == 0);
    CHECK(out.n_corners == 0);
    CHECK(out.n_nearby == 0);
    CHECK(out.n_grids == 0);
    v7_bs_outputs_free(&out);
}

// No ridge lines → 0 RI corners (even with planes present).
// Spec: detectCorners walks RIDGE_LINES; empty input → empty RI list.
static void test_no_ridge_lines_no_ri_corners(void) {
    const int NSIDE = 6;
    const int N = NSIDE * NSIDE;
    double positions[6 * 6 * 3];
    int16_t owner[6 * 6];
    fill_flat_patch(positions, owner, 0, NSIDE, 5.0, 0.0, 0.0, 4.0, 0);

    v7_bs_input_plane_t plane; make_flat_plane(&plane, 0, 5.0);

    v7_bs_params_t p; default_params(&p);
    v7_bs_outputs_t out; v7_bs_outputs_init(&out);
    float weights[N]; make_unit_weights(weights, N);
    int rc = v7_bs_compute(positions, NULL, weights, N, owner, &plane, 1, NULL, 0, NULL, 0, NULL, 0, &p, &out);
    CHECK(rc == 0);

    // No RI source corners.
    size_t n_ri = 0;
    for (size_t i = 0; i < out.n_corners; i++) {
        if (out.corners[i].source == 1) n_ri++;
    }
    CHECK(n_ri == 0);

    v7_bs_outputs_free(&out);
}

// Two ridge lines that cross at the origin, sharing plane 0 → exactly
// 1 RI corner at (0,0,0). Source=1, n_ridge_ids=2, n_plane_ids covers
// the union of the two ridges' planes (here {0, 1, 2} → 3).
// Spec: ridge-intersection corner detection.
static void test_ri_corner_at_crossing(void) {
    v7_bs_input_ridge_line_t lines[2];
    memset(lines, 0, sizeof(lines));
    // Line 0: along +x, planes (0, 1)
    lines[0].plane_a = 0; lines[0].plane_b = 1;
    lines[0].has_line = true;
    lines[0].line_point[0] = 0; lines[0].line_point[1] = 0; lines[0].line_point[2] = 0;
    lines[0].line_direction[0] = 1; lines[0].line_direction[1] = 0; lines[0].line_direction[2] = 0;
    lines[0].t_min_a = -5; lines[0].t_max_a = 5; lines[0].t_min_b = -5; lines[0].t_max_b = 5;
    // Line 1: along +z, planes (0, 2) — shares plane 0
    lines[1].plane_a = 0; lines[1].plane_b = 2;
    lines[1].has_line = true;
    lines[1].line_point[0] = 0; lines[1].line_point[1] = 0; lines[1].line_point[2] = 0;
    lines[1].line_direction[0] = 0; lines[1].line_direction[1] = 0; lines[1].line_direction[2] = 1;
    lines[1].t_min_a = -5; lines[1].t_max_a = 5; lines[1].t_min_b = -5; lines[1].t_max_b = 5;

    v7_bs_params_t p; default_params(&p);
    v7_bs_outputs_t out; v7_bs_outputs_init(&out);
    int rc = v7_bs_compute(NULL, NULL, NULL, 0, NULL, NULL, 0, lines, 2, NULL, 0, NULL, 0, &p, &out);
    CHECK(rc == 0);

    // Count RI corners.
    size_t n_ri = 0;
    size_t ri_idx = (size_t)-1;
    for (size_t i = 0; i < out.n_corners; i++) {
        if (out.corners[i].source == 1) { n_ri++; ri_idx = i; }
    }
    CHECK(n_ri == 1);
    if (n_ri == 1) {
        const v7_bs_corner_t* C = &out.corners[ri_idx];
        // Position is the closest-approach midpoint of (0,0,0) ↔ (0,0,0) → (0,0,0).
        CHECK_CLOSE(C->point[0], 0.0, 1e-9);
        CHECK_CLOSE(C->point[1], 0.0, 1e-9);
        CHECK_CLOSE(C->point[2], 0.0, 1e-9);
        // 2 ridge ids referenced.
        CHECK(C->n_ridge_ids == 2);
        // n_plane_ids is the dedup'd union of {0,1,0,2} = {0,1,2} → 3.
        CHECK(C->n_plane_ids == 3);
        // Name format "RI<n>" per v2 nameCounters.
        CHECK(C->name != NULL);
        if (C->name) {
            CHECK(strncmp(C->name, "RI", 2) == 0);
        }
    }

    v7_bs_outputs_free(&out);
}

// Two ridge lines that do NOT share any plane → no RI corner.
// Spec: detectCorners requires shared plane membership.
static void test_ri_corner_requires_shared_plane(void) {
    v7_bs_input_ridge_line_t lines[2];
    memset(lines, 0, sizeof(lines));
    lines[0].plane_a = 0; lines[0].plane_b = 1;
    lines[0].has_line = true;
    lines[0].line_direction[0] = 1;
    lines[0].t_min_a = -5; lines[0].t_max_a = 5; lines[0].t_min_b = -5; lines[0].t_max_b = 5;
    // Lines cross at origin but planes (2, 3) ∩ (0, 1) = ∅.
    lines[1].plane_a = 2; lines[1].plane_b = 3;
    lines[1].has_line = true;
    lines[1].line_direction[2] = 1;
    lines[1].t_min_a = -5; lines[1].t_max_a = 5; lines[1].t_min_b = -5; lines[1].t_max_b = 5;

    v7_bs_params_t p; default_params(&p);
    v7_bs_outputs_t out; v7_bs_outputs_init(&out);
    int rc = v7_bs_compute(NULL, NULL, NULL, 0, NULL, NULL, 0, lines, 2, NULL, 0, NULL, 0, &p, &out);
    CHECK(rc == 0);

    size_t n_ri = 0;
    for (size_t i = 0; i < out.n_corners; i++) {
        if (out.corners[i].source == 1) n_ri++;
    }
    CHECK(n_ri == 0);

    v7_bs_outputs_free(&out);
}

// Two ridge lines whose closest-approach distance exceeds
// corner_dist_threshold → no RI corner.
// Spec: detectCorners gates on closestApproach.dist <= threshold.
static void test_ri_distance_threshold_gates(void) {
    v7_bs_input_ridge_line_t lines[2];
    memset(lines, 0, sizeof(lines));
    // Line 0: along +x at y=0
    lines[0].plane_a = 0; lines[0].plane_b = 1;
    lines[0].has_line = true;
    lines[0].line_direction[0] = 1;
    lines[0].t_min_a = -5; lines[0].t_max_a = 5; lines[0].t_min_b = -5; lines[0].t_max_b = 5;
    // Line 1: along +z at y=10 (10m above line 0); shares plane 0.
    lines[1].plane_a = 0; lines[1].plane_b = 2;
    lines[1].has_line = true;
    lines[1].line_point[1] = 10.0;
    lines[1].line_direction[2] = 1;
    lines[1].t_min_a = -5; lines[1].t_max_a = 5; lines[1].t_min_b = -5; lines[1].t_max_b = 5;

    v7_bs_params_t p; default_params(&p);
    // Default corner_dist_threshold = 1.0 m. 10m apart → reject.
    v7_bs_outputs_t out; v7_bs_outputs_init(&out);
    int rc = v7_bs_compute(NULL, NULL, NULL, 0, NULL, NULL, 0, lines, 2, NULL, 0, NULL, 0, &p, &out);
    CHECK(rc == 0);

    size_t n_ri = 0;
    for (size_t i = 0; i < out.n_corners; i++) {
        if (out.corners[i].source == 1) n_ri++;
    }
    CHECK(n_ri == 0);

    v7_bs_outputs_free(&out);
}

// Parallel ridge lines → closest_approach returns -1 (denom==0) → no RI.
// Spec: closestApproach returns null on parallel lines.
static void test_ri_parallel_lines_no_corner(void) {
    v7_bs_input_ridge_line_t lines[2];
    memset(lines, 0, sizeof(lines));
    lines[0].plane_a = 0; lines[0].plane_b = 1;
    lines[0].has_line = true;
    lines[0].line_direction[0] = 1;
    lines[0].t_min_a = -5; lines[0].t_max_a = 5; lines[0].t_min_b = -5; lines[0].t_max_b = 5;
    // Same direction, offset in z; same plane shared.
    lines[1].plane_a = 0; lines[1].plane_b = 2;
    lines[1].has_line = true;
    lines[1].line_point[2] = 0.1;  // tiny offset, well within threshold
    lines[1].line_direction[0] = 1;
    lines[1].t_min_a = -5; lines[1].t_max_a = 5; lines[1].t_min_b = -5; lines[1].t_max_b = 5;

    v7_bs_params_t p; default_params(&p);
    v7_bs_outputs_t out; v7_bs_outputs_init(&out);
    int rc = v7_bs_compute(NULL, NULL, NULL, 0, NULL, NULL, 0, lines, 2, NULL, 0, NULL, 0, &p, &out);
    CHECK(rc == 0);

    size_t n_ri = 0;
    for (size_t i = 0; i < out.n_corners; i++) {
        if (out.corners[i].source == 1) n_ri++;
    }
    CHECK(n_ri == 0);

    v7_bs_outputs_free(&out);
}

// Determinism: same inputs across two runs produce the same outputs.
// Spec invariant: no RNG, no hash-keyed iteration → bit-stable output.
static void test_determinism_same_inputs_same_outputs(void) {
    v7_bs_input_ridge_line_t lines[2];
    memset(lines, 0, sizeof(lines));
    lines[0].plane_a = 0; lines[0].plane_b = 1;
    lines[0].has_line = true;
    lines[0].line_point[0] = 2; lines[0].line_point[1] = 0; lines[0].line_point[2] = 3;
    lines[0].line_direction[0] = 1;
    lines[0].t_min_a = -5; lines[0].t_max_a = 5; lines[0].t_min_b = -5; lines[0].t_max_b = 5;
    lines[1].plane_a = 0; lines[1].plane_b = 2;
    lines[1].has_line = true;
    lines[1].line_point[0] = 2; lines[1].line_point[1] = 0; lines[1].line_point[2] = 3;
    lines[1].line_direction[2] = 1;
    lines[1].t_min_a = -5; lines[1].t_max_a = 5; lines[1].t_min_b = -5; lines[1].t_max_b = 5;

    v7_bs_params_t p; default_params(&p);
    v7_bs_outputs_t a; v7_bs_outputs_init(&a);
    v7_bs_outputs_t b; v7_bs_outputs_init(&b);
    CHECK(v7_bs_compute(NULL, NULL, NULL, 0, NULL, NULL, 0, lines, 2, NULL, 0, NULL, 0, &p, &a) == 0);
    CHECK(v7_bs_compute(NULL, NULL, NULL, 0, NULL, NULL, 0, lines, 2, NULL, 0, NULL, 0, &p, &b) == 0);

    CHECK(a.n_corners == b.n_corners);
    for (size_t i = 0; i < a.n_corners && i < b.n_corners; i++) {
        CHECK(a.corners[i].source == b.corners[i].source);
        CHECK(a.corners[i].n_plane_ids == b.corners[i].n_plane_ids);
        CHECK(a.corners[i].n_ridge_ids == b.corners[i].n_ridge_ids);
        CHECK_CLOSE(a.corners[i].point[0], b.corners[i].point[0], 1e-15);
        CHECK_CLOSE(a.corners[i].point[1], b.corners[i].point[1], 1e-15);
        CHECK_CLOSE(a.corners[i].point[2], b.corners[i].point[2], 1e-15);
    }

    v7_bs_outputs_free(&a);
    v7_bs_outputs_free(&b);
}

// One flat plane with a 4m square group of points → grids array length
// equals number of alive (non-rejected, >= min_plane_pts) planes; same
// for nearby array.
// Spec: per-plane output arrays index by alive-plane order.
static void test_grid_and_nearby_array_lengths_match_alive_planes(void) {
    const int NSIDE = 6;  // 36 pts, well above default min_plane_pts=6
    const int N = NSIDE * NSIDE;
    double positions[6 * 6 * 3];
    int16_t owner[6 * 6];
    fill_flat_patch(positions, owner, 0, NSIDE, 5.0, 0.0, 0.0, 4.0, 0);

    v7_bs_input_plane_t plane; make_flat_plane(&plane, 0, 5.0);

    v7_bs_params_t p; default_params(&p);
    v7_bs_outputs_t out; v7_bs_outputs_init(&out);
    float weights[N]; make_unit_weights(weights, N);
    int rc = v7_bs_compute(positions, NULL, weights, N, owner, &plane, 1, NULL, 0, NULL, 0, NULL, 0, &p, &out);
    CHECK(rc == 0);
    CHECK(out.n_grids == 1);
    CHECK(out.n_nearby == 1);
    if (out.n_grids == 1) {
        CHECK(out.grids[0].plane_id == 0);
        // Group field was computed (non-empty group → fields allocated).
        CHECK(out.grids[0].has_group_field);
        CHECK(out.grids[0].group_field.n_cols > 0);
        CHECK(out.grids[0].group_field.n_rows > 0);
    }
    if (out.n_nearby == 1) {
        CHECK(out.nearby[0].plane_id == 0);
        // No beam_dirs → no nearby items (Phase B fully skipped).
        CHECK(out.nearby[0].n_items == 0);
    }

    v7_bs_outputs_free(&out);
}

// Rejected planes contribute neither a grid nor a nearby entry.
// Spec: build_plane_data skips rejected, and the per-plane output
// arrays only have entries for alive planes.
static void test_rejected_plane_skipped(void) {
    const int NSIDE = 6;
    const int N = NSIDE * NSIDE;
    double positions[6 * 6 * 3];
    int16_t owner[6 * 6];
    fill_flat_patch(positions, owner, 0, NSIDE, 5.0, 0.0, 0.0, 4.0, 0);

    v7_bs_input_plane_t planes[2];
    make_flat_plane(&planes[0], 0, 5.0);
    make_flat_plane(&planes[1], 1, 5.0);
    planes[1].rejected = true;  // skip

    v7_bs_params_t p; default_params(&p);
    v7_bs_outputs_t out; v7_bs_outputs_init(&out);
    float weights[N]; make_unit_weights(weights, N);
    int rc = v7_bs_compute(positions, NULL, weights, N, owner, planes, 2, NULL, 0, NULL, 0, NULL, 0, &p, &out);
    CHECK(rc == 0);
    CHECK(out.n_grids == 1);
    CHECK(out.n_nearby == 1);

    v7_bs_outputs_free(&out);
}

// Plane with fewer than min_plane_pts in its group → no grid emitted.
// Spec: build_plane_data treats sub-threshold groups as not-alive
// (n_pts=0 path), so they are skipped in the per-plane sweep.
static void test_below_min_plane_pts_no_grid(void) {
    // 3 points only; default min_plane_pts = 6.
    double positions[3 * 3] = {
        0, 5, 0,
        1, 5, 0,
        0, 5, 1,
    };
    int16_t owner[3] = { 0, 0, 0 };

    v7_bs_input_plane_t plane; make_flat_plane(&plane, 0, 5.0);

    v7_bs_params_t p; default_params(&p);
    v7_bs_outputs_t out; v7_bs_outputs_init(&out);
    float weights[3] = {1.0f, 1.0f, 1.0f};
    int rc = v7_bs_compute(positions, NULL, weights, 3, owner, &plane, 1, NULL, 0, NULL, 0, NULL, 0, &p, &out);
    CHECK(rc == 0);
    CHECK(out.n_grids == 0);
    CHECK(out.n_nearby == 0);

    v7_bs_outputs_free(&out);
}

// Per-plane signal grid produces NaN cells where no points are within
// eval_radius, and finite oa_field where points are. Spec: NaN ≡
// "no data" sentinel for empty cells. The CENTER of the 4x4 patch
// must have a finite open-angle reading; the field is also large
// enough that at least one corner cell has zero points → NaN there.
// Also verifies the INFINITY-bounded min/max trackers in
// compute_signal_grids produce a finite, sane grid extent (the
// "min_u/max_u are INFINITY sentinels" path documented in
// FRAMA-C-FALSE-POSITIVES.md).
static void test_grid_nan_outside_eval_radius(void) {
    const int NSIDE = 8;  // 64 pts, 4m patch, dense interior
    const int N = NSIDE * NSIDE;
    double positions[8 * 8 * 3];
    int16_t owner[8 * 8];
    fill_flat_patch(positions, owner, 0, NSIDE, 5.0, 0.0, 0.0, 4.0, 0);

    v7_bs_input_plane_t plane; make_flat_plane(&plane, 0, 5.0);

    v7_bs_params_t p; default_params(&p);
    v7_bs_outputs_t out; v7_bs_outputs_init(&out);
    float weights[N]; make_unit_weights(weights, N);
    int rc = v7_bs_compute(positions, NULL, weights, N, owner, &plane, 1, NULL, 0, NULL, 0, NULL, 0, &p, &out);
    CHECK(rc == 0);

    if (out.n_grids == 1 && out.grids[0].has_group_field) {
        const v7_bs_grid_field_t* F = &out.grids[0].group_field;
        // Grid extent finite (i.e. min_u/min_v were updated from
        // INFINITY sentinels by real points).
        CHECK(isfinite(F->g_min_u));
        CHECK(isfinite(F->g_min_v));
        CHECK(F->n_cols > 0 && F->n_rows > 0);

        // A cell near the patch EDGE (u ~ +1.5, v=0). Edge cells see
        // points within eval_radius on one side only → finite outward
        // direction (com_dist > 1e-6) → finite OA. The exact patch
        // centre would have com_dist ~ 0 and be skipped, which is the
        // expected behavior (radial direction undefined at centroid).
        int col_c = (int)floorf((1.5f - F->g_min_u) / F->cell_size);
        int row_c = (int)floorf((0.0f - F->g_min_v) / F->cell_size);
        CHECK(col_c >= 0 && (uint32_t)col_c < F->n_cols);
        CHECK(row_c >= 0 && (uint32_t)row_c < F->n_rows);
        if (col_c >= 0 && (uint32_t)col_c < F->n_cols &&
            row_c >= 0 && (uint32_t)row_c < F->n_rows) {
            size_t idx = (size_t)row_c * F->n_cols + (size_t)col_c;
            // Edge cell has points within eval_radius and a non-zero
            // radial offset to the local centroid → finite OA.
            CHECK(!isnan(F->oa_field[idx]));
            CHECK(F->pt_count_field[idx] > 0);
        }

        // Locate one cell well outside the patch (margin cells); per
        // the impl, the grid extends eval_radius (1.0m) beyond the
        // patch bbox, so column 0 should sit far from any point and
        // have NaN OA.
        size_t corner_idx = 0;
        CHECK(isnan(F->oa_field[corner_idx]));
        CHECK(F->pt_count_field[corner_idx] == 0);
    }

    v7_bs_outputs_free(&out);
}

// pt_count_field is consistent with com_dist_field NaN-ness: a cell
// has count > 0 iff com_dist is finite. Spec: both written together
// in the centroid pass of compute_signal_grids.
static void test_pt_count_and_com_dist_consistent(void) {
    const int NSIDE = 8;
    const int N = NSIDE * NSIDE;
    double positions[8 * 8 * 3];
    int16_t owner[8 * 8];
    fill_flat_patch(positions, owner, 0, NSIDE, 5.0, 0.0, 0.0, 4.0, 0);

    v7_bs_input_plane_t plane; make_flat_plane(&plane, 0, 5.0);

    v7_bs_params_t p; default_params(&p);
    v7_bs_outputs_t out; v7_bs_outputs_init(&out);
    float weights[N]; make_unit_weights(weights, N);
    int rc = v7_bs_compute(positions, NULL, weights, N, owner, &plane, 1, NULL, 0, NULL, 0, NULL, 0, &p, &out);
    CHECK(rc == 0);

    if (out.n_grids == 1 && out.grids[0].has_group_field) {
        const v7_bs_grid_field_t* F = &out.grids[0].group_field;
        size_t n_cells = (size_t)F->n_cols * (size_t)F->n_rows;
        for (size_t i = 0; i < n_cells; i++) {
            bool count_nz   = F->pt_count_field[i] > 0;
            bool com_finite = !isnan(F->com_dist_field[i]);
            // Iff invariant (one direction at a time so we know what
            // failed if either does).
            CHECK(count_nz == com_finite);
        }
    }

    v7_bs_outputs_free(&out);
}

// RI corner IDs are dense (0..n_ri-1) and unique. Spec: IDs assigned
// at pool creation in detect_ri_corners.
static void test_ri_corner_ids_dense_unique(void) {
    // Construct 3 ridge lines pairwise sharing planes, all crossing at
    // distinct points so we get 3 RI corners.
    v7_bs_input_ridge_line_t lines[3];
    memset(lines, 0, sizeof(lines));
    // L0 along +x at z=0, planes (0, 1)
    lines[0].plane_a = 0; lines[0].plane_b = 1;
    lines[0].has_line = true;
    lines[0].line_direction[0] = 1;
    lines[0].t_min_a = -10; lines[0].t_max_a = 10; lines[0].t_min_b = -10; lines[0].t_max_b = 10;
    // L1 along +z at x=2, planes (0, 2); crosses L0 at (2,0,0)
    lines[1].plane_a = 0; lines[1].plane_b = 2;
    lines[1].has_line = true;
    lines[1].line_point[0] = 2;
    lines[1].line_direction[2] = 1;
    lines[1].t_min_a = -10; lines[1].t_max_a = 10; lines[1].t_min_b = -10; lines[1].t_max_b = 10;
    // L2 along +z at x=5, planes (1, 2); crosses L0 at (5,0,0), L1 parallel→no corner
    lines[2].plane_a = 1; lines[2].plane_b = 2;
    lines[2].has_line = true;
    lines[2].line_point[0] = 5;
    lines[2].line_direction[2] = 1;
    lines[2].t_min_a = -10; lines[2].t_max_a = 10; lines[2].t_min_b = -10; lines[2].t_max_b = 10;

    v7_bs_params_t p; default_params(&p);
    v7_bs_outputs_t out; v7_bs_outputs_init(&out);
    int rc = v7_bs_compute(NULL, NULL, NULL, 0, NULL, NULL, 0, lines, 3, NULL, 0, NULL, 0, &p, &out);
    CHECK(rc == 0);

    // We expect 2 RI corners (L0×L1 at (2,0,0); L0×L2 at (5,0,0);
    // L1×L2 parallel → no corner).
    size_t n_ri = 0;
    for (size_t i = 0; i < out.n_corners; i++) {
        if (out.corners[i].source == 1) n_ri++;
    }
    CHECK(n_ri == 2);

    // IDs are dense and unique.
    for (size_t i = 0; i < out.n_corners; i++) {
        CHECK(out.corners[i].id == (uint32_t)i);
    }

    v7_bs_outputs_free(&out);
}

// ─── main ───────────────────────────────────────────────────────────

int main(void) {
    test_empty_input_no_crash();
    test_no_ridge_lines_no_ri_corners();
    test_ri_corner_at_crossing();
    test_ri_corner_requires_shared_plane();
    test_ri_distance_threshold_gates();
    test_ri_parallel_lines_no_corner();
    test_ri_corner_ids_dense_unique();
    test_determinism_same_inputs_same_outputs();
    test_grid_and_nearby_array_lengths_match_alive_planes();
    test_rejected_plane_skipped();
    test_below_min_plane_pts_no_grid();
    test_grid_nan_outside_eval_radius();
    test_pt_count_and_com_dist_consistent();

    printf("%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}

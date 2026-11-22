// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// core/tests/v7_boundary_regularize_test.c
//
// Implementation tests for v7-boundary-regularize R.0 + R.0b + R.1
// single-corner subset (5.7g.1). Asserts spec invariants from bench-v2
// lib/pipeline/steps/boundary-regularize.js for the phases in scope.

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE   700

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Include the impl source directly so the test TU owns the compute symbols.
#include "v7-boundary-regularize-impl.c"

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
            fprintf(stderr, "FAIL %s:%d: %s ~= %s (got %.12g, want %.12g)\n",                       \
                    __FILE__, __LINE__, #a, #b, _a, _b);                                            \
            g_failed++;                                                                            \
        }                                                                                          \
    } while (0)

// ─── helpers ────────────────────────────────────────────────────────

static v7_br_input_plane_t mk_flat_plane(uint32_t id, double y) {
    v7_br_input_plane_t p; memset(&p, 0, sizeof(p));
    p.id = id; p.tilt_deg = 0.0; p.rejected = false;
    p.normal[0] = 0.0; p.normal[1] = 1.0; p.normal[2] = 0.0;
    p.d = -y;
    return p;
}

static v7_br_input_plane_t mk_slope_plane(uint32_t id, double nx, double ny, double nz, double d) {
    v7_br_input_plane_t p; memset(&p, 0, sizeof(p));
    p.id = id; p.rejected = false;
    p.normal[0] = nx; p.normal[1] = ny; p.normal[2] = nz; p.d = d;
    const double horiz = sqrt(nx*nx + nz*nz);
    p.tilt_deg = atan2(horiz, fabs(ny)) * 180.0 / M_PI;
    return p;
}

// Forward decl — definition lives with the A2 ridge tests below, but several
// earlier tests (the T6 ridge-cleanup) need it too.
static v7_br_input_ridge_t mk_ridge(uint32_t id, uint32_t pa, uint32_t pb,
                                    double fx, double fy, double fz,
                                    double tx, double ty, double tz);

static v7_br_input_corner_t mk_corner(uint32_t id, double x, double y, double z,
                                       uint32_t* planes_owned, uint32_t n_planes,
                                       uint32_t* ridges_owned, uint32_t n_ridges) {
    v7_br_input_corner_t c; memset(&c, 0, sizeof(c));
    c.id = id;
    c.point[0] = x; c.point[1] = y; c.point[2] = z;
    c.plane_ids = planes_owned;
    c.n_plane_ids = n_planes;
    c.ridge_ids = ridges_owned;
    c.n_ridge_ids = n_ridges;
    return c;
}

// ─── tests ──────────────────────────────────────────────────────────

// R.0: DOF tagging by PATH membership (SSOT, 2026-06-19). A corner's plane
// set = the planes of the polygon paths (n>=3) it appears in — NOT
// corners_in[].plane_ids. So we express the intended membership via paths:
//   corner 100 in planes {0,1,2} → DOF 0 (locked)
//   corner 101 in planes {0,1}   → DOF 1 (ridge slide)
//   corner 102 in plane  {0}     → DOF 2 (plane slide)
// Fillers (110/111/112) pad the per-plane triangles; each sits in one plane.
static void test_dof_tagging(void) {
    fprintf(stderr, "== test_dof_tagging\n");
    v7_br_input_plane_t planes[3] = {
        mk_flat_plane(0, 5.0),
        mk_slope_plane(1, 0.0, 0.707, 0.707, -5.0),
        mk_slope_plane(2, 0.707, 0.707, 0.0, -5.0),
    };
    // plane_ids on corners are now IGNORED by regularize; left set for realism.
    uint32_t* p3 = (uint32_t*)calloc(3, sizeof(uint32_t)); p3[0]=0; p3[1]=1; p3[2]=2;
    uint32_t* p2 = (uint32_t*)calloc(2, sizeof(uint32_t)); p2[0]=0; p2[1]=1;
    uint32_t* p1 = (uint32_t*)calloc(1, sizeof(uint32_t)); p1[0]=0;
    uint32_t* f1a = (uint32_t*)calloc(1, sizeof(uint32_t)); f1a[0]=1;
    uint32_t* f2a = (uint32_t*)calloc(1, sizeof(uint32_t)); f2a[0]=2;
    uint32_t* f2b = (uint32_t*)calloc(1, sizeof(uint32_t)); f2b[0]=2;
    v7_br_input_corner_t corners[6] = {
        mk_corner(100, 0, 5, 0, p3, 3, NULL, 0),  // {0,1,2} → DOF 0
        mk_corner(101, 1, 5, 0, p2, 2, NULL, 0),  // {0,1}   → DOF 1
        mk_corner(102, 2, 5, 0, p1, 1, NULL, 0),  // {0}     → DOF 2
        mk_corner(110, 3, 5, 1, f1a, 1, NULL, 0), // filler, plane 1
        mk_corner(111, 4, 5, 2, f2a, 1, NULL, 0), // filler, plane 2
        mk_corner(112, 5, 5, 3, f2b, 1, NULL, 0), // filler, plane 2
    };
    // One triangle per plane realizing the memberships above.
    uint32_t* path0 = (uint32_t*)calloc(3, sizeof(uint32_t)); path0[0]=100; path0[1]=101; path0[2]=102;
    uint32_t* path1 = (uint32_t*)calloc(3, sizeof(uint32_t)); path1[0]=100; path1[1]=101; path1[2]=110;
    uint32_t* path2 = (uint32_t*)calloc(3, sizeof(uint32_t)); path2[0]=100; path2[1]=111; path2[2]=112;
    v7_br_input_polygon_path_t paths[3] = {
        { .plane_id = 0, .corner_ids = path0, .n_corners = 3 },
        { .plane_id = 1, .corner_ids = path1, .n_corners = 3 },
        { .plane_id = 2, .corner_ids = path2, .n_corners = 3 },
    };
    int16_t owner_buf[1] = { -1 };
    float pos_buf[3] = { 0, 0, 0 };
    v7_br_params_t params; v7_br_params_default(&params);
    params.max_iterations = 0;  // skip R.1 — only verify R.0
    v7_br_outputs_t out; v7_br_outputs_init(&out);
    int rc = v7_br_compute(planes, 3, corners, 6, NULL, 0, paths, 3, NULL, 0, NULL, 0,
                            pos_buf, 1, owner_buf, NULL, 0.12, &params, &out);
    CHECK(rc == 0);
    CHECK(out.n_dof == 6);
    if (out.n_dof == 6) {
        CHECK(out.dof[0].dof == 0);  // {0,1,2} → locked
        CHECK(out.dof[1].dof == 1);  // {0,1} → ridge slide; axis_a = n0 × n1
        CHECK(out.dof[1].has_axis_a == true);
        CHECK(out.dof[2].dof == 2);  // {0} → 2 DOF on plane
        CHECK(out.dof[2].has_axis_a == true);
        CHECK(out.dof[2].has_axis_b == true);
    }
    CHECK(out.stats.dof0 == 1);  // corner 100
    CHECK(out.stats.dof1 == 1);  // corner 101
    CHECK(out.stats.dof2 == 4);  // corners 102 + 3 fillers (each single-plane)

    v7_br_outputs_free(&out);
    free(p3); free(p2); free(p1); free(f1a); free(f2a); free(f2b);
    free(path0); free(path1); free(path2);
}

// R.0 axis: ridge axis = n_a × n_b normalized. Membership is path-derived
// (SSOT): corner 0 sits in a plane-0 triangle AND a plane-1 triangle → {0,1}
// → DOF 1 with axis_a = n0 × n1.
static void test_dof1_ridge_axis(void) {
    fprintf(stderr, "== test_dof1_ridge_axis\n");
    // Two planes meeting along the X axis. plane 0: y=5 (n=(0,1,0));
    // plane 1: z=0 (n=(0,0,1)). Cross product (0,1,0) × (0,0,1) = (1,0,0).
    v7_br_input_plane_t planes[2] = {
        mk_flat_plane(0, 5.0),
        { .id = 1, .normal = {0, 0, 1}, .d = 0, .tilt_deg = 90, .rejected = false },
    };
    uint32_t* p2 = (uint32_t*)calloc(2, sizeof(uint32_t)); p2[0]=0; p2[1]=1;
    uint32_t* f0 = (uint32_t*)calloc(1, sizeof(uint32_t)); f0[0]=0;
    uint32_t* f1 = (uint32_t*)calloc(1, sizeof(uint32_t)); f1[0]=1;
    v7_br_input_corner_t corners[5] = {
        mk_corner(0, 0, 5, 0, p2, 2, NULL, 0),  // shared {0,1} → DOF 1
        mk_corner(10, 1, 5, 0, f0, 1, NULL, 0), // plane-0 fillers
        mk_corner(11, 2, 5, 0, f0, 1, NULL, 0),
        mk_corner(20, 1, 0, 0, f1, 1, NULL, 0), // plane-1 fillers
        mk_corner(21, 2, 0, 0, f1, 1, NULL, 0),
    };
    uint32_t* path0 = (uint32_t*)calloc(3, sizeof(uint32_t)); path0[0]=0; path0[1]=10; path0[2]=11;
    uint32_t* path1 = (uint32_t*)calloc(3, sizeof(uint32_t)); path1[0]=0; path1[1]=20; path1[2]=21;
    v7_br_input_polygon_path_t paths[2] = {
        { .plane_id = 0, .corner_ids = path0, .n_corners = 3 },
        { .plane_id = 1, .corner_ids = path1, .n_corners = 3 },
    };
    int16_t owner_buf[1] = { -1 };
    float pos_buf[3] = { 0, 0, 0 };
    v7_br_params_t params; v7_br_params_default(&params);
    params.max_iterations = 0;
    v7_br_outputs_t out; v7_br_outputs_init(&out);
    int rc = v7_br_compute(planes, 2, corners, 5, NULL, 0, paths, 2, NULL, 0, NULL, 0,
                            pos_buf, 1, owner_buf, NULL, 0.12, &params, &out);
    CHECK(rc == 0);
    if (out.n_dof == 5) {
        CHECK(out.dof[0].dof == 1);
        CHECK_CLOSE(out.dof[0].axis_a[0],  1.0, 1e-6);
        CHECK_CLOSE(out.dof[0].axis_a[1],  0.0, 1e-6);
        CHECK_CLOSE(out.dof[0].axis_a[2],  0.0, 1e-6);
    }
    v7_br_outputs_free(&out);
    free(p2); free(f0); free(f1); free(path0); free(path1);
}

// R.0b: unused corners get tagged.
static void test_r0b_unused_corner(void) {
    fprintf(stderr, "== test_r0b_unused_corner\n");
    v7_br_input_plane_t planes[1] = { mk_flat_plane(0, 5.0) };
    uint32_t* p1a = (uint32_t*)calloc(1, sizeof(uint32_t)); p1a[0]=0;
    uint32_t* p1b = (uint32_t*)calloc(1, sizeof(uint32_t)); p1b[0]=0;
    uint32_t* p1c = (uint32_t*)calloc(1, sizeof(uint32_t)); p1c[0]=0;
    uint32_t* p1d = (uint32_t*)calloc(1, sizeof(uint32_t)); p1d[0]=0;
    v7_br_input_corner_t corners[4] = {
        mk_corner(0, 0, 5, 0, p1a, 1, NULL, 0),   // used (in path)
        mk_corner(1, 4, 5, 0, p1b, 1, NULL, 0),   // used
        mk_corner(2, 4, 5, 4, p1c, 1, NULL, 0),   // used
        mk_corner(3, 8, 8, 8, p1d, 1, NULL, 0),   // UNUSED
    };
    uint32_t* path_ids = (uint32_t*)calloc(3, sizeof(uint32_t)); path_ids[0]=0; path_ids[1]=1; path_ids[2]=2;
    v7_br_input_polygon_path_t paths[1] = { { .plane_id = 0, .corner_ids = path_ids, .n_corners = 3 } };
    int16_t owner_buf[1] = { -1 };
    float pos_buf[3] = { 0, 0, 0 };
    v7_br_params_t params; v7_br_params_default(&params);
    params.max_iterations = 0;
    v7_br_outputs_t out; v7_br_outputs_init(&out);
    int rc = v7_br_compute(planes, 1, corners, 4, NULL, 0, paths, 1, NULL, 0, NULL, 0,
                            pos_buf, 1, owner_buf, NULL, 0.12, &params, &out);
    CHECK(rc == 0);
    CHECK(out.stats.unused_removed == 1);
    v7_br_outputs_free(&out);
    free(p1a); free(p1b); free(p1c); free(p1d); free(path_ids);
}

// Pre-loop ridge-cleanup (T6, all-polys-agree): 3 consecutive corners sharing a
// ridge in the ONLY polygon containing them → middle one pruned. Membership is
// single-plane here so "every containing polygon agrees" is trivially satisfied.
// NOTE (T6, 2026-06-19): the cleanup now reads the MAINTAINED cs[].ridge_ids,
// seeded (reality-intersected) from real ridge geometry — so this test supplies
// a ridges[] array (the old per-path pass read corners_in[].ridge_ids raw and
// needed no geometry). ridge_ids store INDICES into ridges[], so the tag is 0.
static void test_r0b_redundant_ridge_midpoint(void) {
    fprintf(stderr, "== test_r0b_redundant_ridge_midpoint\n");
    v7_br_input_plane_t planes[1] = { mk_flat_plane(0, 5.0) };
    // Ridge index 0 runs along +X through the colinear run (0,5,0)..(4,5,0).
    v7_br_input_ridge_t ridges[1] = { mk_ridge(0, 0, 1, -1, 5, 0, 5, 5, 0) };
    ridges[0].type = 1;   // ridge → allow-listed (not that the cleanup type-gates)
    uint32_t* p1a = (uint32_t*)calloc(1, sizeof(uint32_t)); p1a[0]=0;
    uint32_t* p1b = (uint32_t*)calloc(1, sizeof(uint32_t)); p1b[0]=0;
    uint32_t* p1c = (uint32_t*)calloc(1, sizeof(uint32_t)); p1c[0]=0;
    uint32_t* p1d = (uint32_t*)calloc(1, sizeof(uint32_t)); p1d[0]=0;
    uint32_t* r1a = (uint32_t*)calloc(1, sizeof(uint32_t)); r1a[0]=0;
    uint32_t* r1b = (uint32_t*)calloc(1, sizeof(uint32_t)); r1b[0]=0;
    uint32_t* r1c = (uint32_t*)calloc(1, sizeof(uint32_t)); r1c[0]=0;
    // 4 corners along path. corners 0,1,2 all share ridge 0 → middle (1) pruned.
    v7_br_input_corner_t corners[4] = {
        mk_corner(0, 0, 5, 0, p1a, 1, r1a, 1),
        mk_corner(1, 2, 5, 0, p1b, 1, r1b, 1),
        mk_corner(2, 4, 5, 0, p1c, 1, r1c, 1),
        mk_corner(3, 4, 5, 4, p1d, 1, NULL, 0),
    };
    uint32_t* path_ids = (uint32_t*)calloc(4, sizeof(uint32_t));
    path_ids[0]=0; path_ids[1]=1; path_ids[2]=2; path_ids[3]=3;
    v7_br_input_polygon_path_t paths[1] = { { .plane_id = 0, .corner_ids = path_ids, .n_corners = 4 } };
    int16_t owner_buf[1] = { -1 };
    float pos_buf[3] = { 0, 0, 0 };
    v7_br_params_t params; v7_br_params_default(&params);
    params.max_iterations = 0;
    v7_br_outputs_t out; v7_br_outputs_init(&out);
    int rc = v7_br_compute(planes, 1, corners, 4, NULL, 0, paths, 1, ridges, 1, NULL, 0,
                            pos_buf, 1, owner_buf, NULL, 0.12, &params, &out);
    CHECK(rc == 0);
    CHECK(out.stats.redundant_ridge_removed >= 1);
    CHECK(out.n_polygon_paths == 1);
    if (out.n_polygon_paths == 1) CHECK(out.polygon_paths[0].n_corners == 3);
    v7_br_outputs_free(&out);
    free(p1a); free(p1b); free(p1c); free(p1d); free(r1a); free(r1b); free(r1c); free(path_ids);
}

// R.1: locked corners (DOF=0) never move.
static void test_r1_locked_corners_stay(void) {
    fprintf(stderr, "== test_r1_locked_corners_stay\n");
    v7_br_input_plane_t planes[3] = {
        mk_flat_plane(0, 5.0),
        mk_slope_plane(1, 0.0, 0.707, 0.707, -5.0),
        mk_slope_plane(2, 0.707, 0.707, 0.0, -5.0),
    };
    uint32_t* p3a = (uint32_t*)calloc(3, sizeof(uint32_t)); p3a[0]=0; p3a[1]=1; p3a[2]=2;
    uint32_t* p3b = (uint32_t*)calloc(3, sizeof(uint32_t)); p3b[0]=0; p3b[1]=1; p3b[2]=2;
    uint32_t* p3c = (uint32_t*)calloc(3, sizeof(uint32_t)); p3c[0]=0; p3c[1]=1; p3c[2]=2;
    v7_br_input_corner_t corners[3] = {
        mk_corner(0, 0, 5, 0, p3a, 3, NULL, 0),
        mk_corner(1, 4, 5, 0, p3b, 3, NULL, 0),
        mk_corner(2, 2, 5, 4, p3c, 3, NULL, 0),
    };
    // Membership is path-derived (SSOT): to give all three corners the 3-plane
    // membership that makes them DOF-0, put the same triangle in all 3 planes.
    uint32_t* path_a = (uint32_t*)calloc(3, sizeof(uint32_t)); path_a[0]=0; path_a[1]=1; path_a[2]=2;
    uint32_t* path_b = (uint32_t*)calloc(3, sizeof(uint32_t)); path_b[0]=0; path_b[1]=1; path_b[2]=2;
    uint32_t* path_c = (uint32_t*)calloc(3, sizeof(uint32_t)); path_c[0]=0; path_c[1]=1; path_c[2]=2;
    v7_br_input_polygon_path_t paths[3] = {
        { .plane_id = 0, .corner_ids = path_a, .n_corners = 3 },
        { .plane_id = 1, .corner_ids = path_b, .n_corners = 3 },
        { .plane_id = 2, .corner_ids = path_c, .n_corners = 3 },
    };
    int16_t owner_buf[1] = { -1 };
    float pos_buf[3] = { 0, 0, 0 };
    v7_br_params_t params; v7_br_params_default(&params);
    v7_br_outputs_t out; v7_br_outputs_init(&out);
    int rc = v7_br_compute(planes, 3, corners, 3, NULL, 0, paths, 3, NULL, 0, NULL, 0,
                            pos_buf, 1, owner_buf, NULL, 0.12, &params, &out);
    CHECK(rc == 0);
    if (out.n_corner_positions == 3) {
        // All locked → no movement at all.
        CHECK_CLOSE(out.corner_positions[0].point[0], 0.0, 1e-12);
        CHECK_CLOSE(out.corner_positions[1].point[0], 4.0, 1e-12);
        CHECK_CLOSE(out.corner_positions[2].point[2], 4.0, 1e-12);
    }
    CHECK(out.stats.r1_total_moves == 0);
    v7_br_outputs_free(&out);
    free(p3a); free(p3b); free(p3c); free(path_a); free(path_b); free(path_c);
}

// R.1: descent converges (terminates without hitting max_iterations on a tiny case).
static void test_r1_converges(void) {
    fprintf(stderr, "== test_r1_converges\n");
    v7_br_input_plane_t planes[1] = { mk_flat_plane(0, 5.0) };
    // 4 single-plane (DOF=2) corners — but with no WSD points and no
    // angle targets near match → cost is 0 everywhere → no moves accepted.
    uint32_t* p1a = (uint32_t*)calloc(1, sizeof(uint32_t)); p1a[0]=0;
    uint32_t* p1b = (uint32_t*)calloc(1, sizeof(uint32_t)); p1b[0]=0;
    uint32_t* p1c = (uint32_t*)calloc(1, sizeof(uint32_t)); p1c[0]=0;
    uint32_t* p1d = (uint32_t*)calloc(1, sizeof(uint32_t)); p1d[0]=0;
    // Square with already-canonical 90° angles → angle cost = 0 at start.
    v7_br_input_corner_t corners[4] = {
        mk_corner(0, 0, 5, 0, p1a, 1, NULL, 0),
        mk_corner(1, 4, 5, 0, p1b, 1, NULL, 0),
        mk_corner(2, 4, 5, 4, p1c, 1, NULL, 0),
        mk_corner(3, 0, 5, 4, p1d, 1, NULL, 0),
    };
    uint32_t* path_ids = (uint32_t*)calloc(4, sizeof(uint32_t));
    path_ids[0]=0; path_ids[1]=1; path_ids[2]=2; path_ids[3]=3;
    v7_br_input_polygon_path_t paths[1] = { { .plane_id = 0, .corner_ids = path_ids, .n_corners = 4 } };
    int16_t owner_buf[1] = { -1 };
    float pos_buf[3] = { 0, 0, 0 };
    v7_br_params_t params; v7_br_params_default(&params);
    params.max_iterations = 10;
    params.use_wsd_cost = false;  // no input points → WSD pointless
    v7_br_outputs_t out; v7_br_outputs_init(&out);
    int rc = v7_br_compute(planes, 1, corners, 4, NULL, 0, paths, 1, NULL, 0, NULL, 0,
                            pos_buf, 1, owner_buf, NULL, 0.12, &params, &out);
    CHECK(rc == 0);
    // Convergence: terminates EARLIER than max_iterations (any_move=false → break).
    CHECK((int32_t)out.stats.r1_iterations <= params.max_iterations);
    // Square at canonical 90° angles → no moves should be needed.
    CHECK(out.stats.r1_total_moves == 0);
    v7_br_outputs_free(&out);
    free(p1a); free(p1b); free(p1c); free(p1d); free(path_ids);
}

// Joint-move PRNG: seeded mulberry32 must be deterministic. Two runs
// with the same seed on the same input should produce identical
// trajectories. We exercise this by checking r1_total_moves matches
// across two runs (a stable counter modulated entirely by the PRNG +
// cost-surface evaluation).
static void test_joint_moves_seed_deterministic(void) {
    fprintf(stderr, "== test_joint_moves_seed_deterministic\n");
    v7_br_input_plane_t planes[1] = { mk_flat_plane(0, 5.0) };
    uint32_t* p1a = (uint32_t*)calloc(1, sizeof(uint32_t)); p1a[0]=0;
    uint32_t* p1b = (uint32_t*)calloc(1, sizeof(uint32_t)); p1b[0]=0;
    uint32_t* p1c = (uint32_t*)calloc(1, sizeof(uint32_t)); p1c[0]=0;
    uint32_t* p1d = (uint32_t*)calloc(1, sizeof(uint32_t)); p1d[0]=0;
    // Slightly off-canonical square — angle terms will pull corners.
    v7_br_input_corner_t corners[4] = {
        mk_corner(0, 0.1, 5, 0.0, p1a, 1, NULL, 0),
        mk_corner(1, 4.0, 5, 0.1, p1b, 1, NULL, 0),
        mk_corner(2, 4.0, 5, 4.0, p1c, 1, NULL, 0),
        mk_corner(3, 0.0, 5, 4.0, p1d, 1, NULL, 0),
    };
    uint32_t* path_ids = (uint32_t*)calloc(4, sizeof(uint32_t));
    path_ids[0]=0; path_ids[1]=1; path_ids[2]=2; path_ids[3]=3;
    v7_br_input_polygon_path_t paths[1] = { { .plane_id = 0, .corner_ids = path_ids, .n_corners = 4 } };
    int16_t owner_buf[1] = { -1 };
    float pos_buf[3] = { 0, 0, 0 };

    v7_br_params_t params; v7_br_params_default(&params);
    params.use_wsd_cost = false;
    params.use_joint_moves = true;
    params.joint_move_seed = 42;
    params.joint_samples_per_edge = 20;
    params.max_iterations = 30;

    // Run 1.
    v7_br_outputs_t out1; v7_br_outputs_init(&out1);
    // Restore points before run (compute mutates them via cs[]; corners_in[]
    // is unchanged, so re-running is safe).
    int rc1 = v7_br_compute(planes, 1, corners, 4, NULL, 0, paths, 1, NULL, 0, NULL, 0,
                             pos_buf, 1, owner_buf, NULL, 0.12, &params, &out1);
    CHECK(rc1 == 0);

    // Run 2.
    v7_br_outputs_t out2; v7_br_outputs_init(&out2);
    int rc2 = v7_br_compute(planes, 1, corners, 4, NULL, 0, paths, 1, NULL, 0, NULL, 0,
                             pos_buf, 1, owner_buf, NULL, 0.12, &params, &out2);
    CHECK(rc2 == 0);

    // Same seed + same input → same iteration count + same total moves.
    CHECK(out1.stats.r1_iterations  == out2.stats.r1_iterations);
    CHECK(out1.stats.r1_total_moves == out2.stats.r1_total_moves);
    CHECK(out1.stats.r1_joint_moves == out2.stats.r1_joint_moves);
    // Same final corner positions.
    if (out1.n_corner_positions == 4 && out2.n_corner_positions == 4) {
        for (size_t i = 0; i < 4; i++) {
            CHECK_CLOSE(out1.corner_positions[i].point[0], out2.corner_positions[i].point[0], 1e-12);
            CHECK_CLOSE(out1.corner_positions[i].point[1], out2.corner_positions[i].point[1], 1e-12);
            CHECK_CLOSE(out1.corner_positions[i].point[2], out2.corner_positions[i].point[2], 1e-12);
        }
    }

    v7_br_outputs_free(&out1);
    v7_br_outputs_free(&out2);
    free(p1a); free(p1b); free(p1c); free(p1d); free(path_ids);
}

// R.2 and R.3 are exercised by the smoke bench against real bldgonly
// data (276 corners → 38 R.2 merges + 16 R.3 removals on dahouet).
// Constructing a focused synthetic test for either requires staging an
// R.1 move that triggers the chase, which depends on cost-surface
// geometry that's brittle to set up. Locking in via bench is sufficient.

// NULL params/out → rejected with rc=-1.
static void test_null_params_rejected(void) {
    fprintf(stderr, "== test_null_params_rejected\n");
    v7_br_outputs_t out; v7_br_outputs_init(&out);
    int rc = v7_br_compute(NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0,
                            NULL, 0, NULL, NULL, 0.12, NULL, &out);
    CHECK(rc == -1);
}

// All NULL with zero counts → rc=0, no-op (matches v3 framework convention).
static void test_all_null_zero_counts_noop(void) {
    fprintf(stderr, "== test_all_null_zero_counts_noop\n");
    v7_br_params_t params; v7_br_params_default(&params);
    v7_br_outputs_t out; v7_br_outputs_init(&out);
    int rc = v7_br_compute(NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0,
                            NULL, 0, NULL, NULL, 0.12, &params, &out);
    CHECK(rc == 0);
    CHECK(out.n_dof == 0);
}

// Zero-corner input → rc=0, no DOF records.
static void test_zero_corners_no_work(void) {
    fprintf(stderr, "== test_zero_corners_no_work\n");
    v7_br_input_plane_t planes[1] = { mk_flat_plane(0, 5.0) };
    v7_br_input_corner_t corners[1]; memset(corners, 0, sizeof(corners));
    int16_t owner_buf[1] = { -1 };
    float pos_buf[3] = { 0, 0, 0 };
    v7_br_params_t params; v7_br_params_default(&params);
    v7_br_outputs_t out; v7_br_outputs_init(&out);
    int rc = v7_br_compute(planes, 1, corners, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0,
                            pos_buf, 1, owner_buf, NULL, 0.12, &params, &out);
    CHECK(rc == 0);
    CHECK(out.n_dof == 0);
    CHECK(out.n_corner_positions == 0);
    v7_br_outputs_free(&out);
}

// ─── A2 (§4.6) ridge_ids maintenance + min-DOF coupling ─────────────

static v7_br_input_ridge_t mk_ridge(uint32_t id, uint32_t pa, uint32_t pb,
                                    double fx, double fy, double fz,
                                    double tx, double ty, double tz) {
    v7_br_input_ridge_t r; memset(&r, 0, sizeof(r));
    r.id = id; r.plane_a = pa; r.plane_b = pb;
    r.from[0] = fx; r.from[1] = fy; r.from[2] = fz;
    r.to[0]   = tx; r.to[1]   = ty; r.to[2]   = tz;
    return r;
}

// A single-plane corner sitting on a ridge crease → must become DOF-1 with
// axis_a = rg.rd (the ridge direction) and has_axis_a=true (NOT DOF-2/free,
// NOT frozen). Ridge 0 runs along +X through (0,5,0); corner at (0,5,0) tagged
// on ridge 0, with ONE plane (plane_dof=2). min(2,1)=1 → ridge-slide on +X.
static void test_a2_single_plane_on_ridge_dof1(void) {
    fprintf(stderr, "== test_a2_single_plane_on_ridge_dof1\n");
    v7_br_input_plane_t planes[1] = { mk_flat_plane(0, 5.0) };
    v7_br_input_ridge_t ridges[1] = { mk_ridge(0, 0, 1, -5, 5, 0, 5, 5, 0) };
    uint32_t* p1 = (uint32_t*)calloc(1, sizeof(uint32_t)); p1[0] = 0;
    uint32_t* r1 = (uint32_t*)calloc(1, sizeof(uint32_t)); r1[0] = 0;
    uint32_t* pf = (uint32_t*)calloc(1, sizeof(uint32_t)); pf[0] = 0;
    // Single-plane membership via one plane-0 triangle (corner 0 + 2 fillers).
    // Fillers sit OFF the ridge (z=2,3) so they stay plain DOF-2 (no coupling).
    v7_br_input_corner_t corners[3] = {
        mk_corner(0, 0, 5, 0, p1, 1, r1, 1),   // single-plane, ON ridge → DOF 1
        mk_corner(10, 3, 5, 2, pf, 1, NULL, 0),
        mk_corner(11, 4, 5, 3, pf, 1, NULL, 0),
    };
    uint32_t* path0 = (uint32_t*)calloc(3, sizeof(uint32_t)); path0[0]=0; path0[1]=10; path0[2]=11;
    v7_br_input_polygon_path_t paths[1] = { { .plane_id = 0, .corner_ids = path0, .n_corners = 3 } };
    int16_t owner_buf[1] = { -1 };
    float pos_buf[3] = { 0, 0, 0 };
    v7_br_params_t params; v7_br_params_default(&params);
    params.max_iterations = 0;
    v7_br_outputs_t out; v7_br_outputs_init(&out);
    int rc = v7_br_compute(planes, 1, corners, 3, NULL, 0, paths, 1, ridges, 1, NULL, 0,
                            pos_buf, 1, owner_buf, NULL, 0.12, &params, &out);
    CHECK(rc == 0);
    if (out.n_dof == 3) {
        CHECK(out.dof[0].dof == 1);            // plane-count said 2; ridge pulls to 1
        CHECK(out.dof[0].has_axis_a == true);  // MUST have an axis (else freeze)
        CHECK(out.dof[0].has_axis_b == false);
        CHECK_CLOSE(out.dof[0].axis_a[0], 1.0, 1e-9);  // == rg.rd (+X)
        CHECK_CLOSE(out.dof[0].axis_a[1], 0.0, 1e-9);
        CHECK_CLOSE(out.dof[0].axis_a[2], 0.0, 1e-9);
    }
    CHECK(out.stats.dof1 == 1);   // corner 0
    CHECK(out.stats.dof2 == 2);   // the two off-ridge fillers
    v7_br_outputs_free(&out);
    free(p1); free(r1); free(pf); free(path0);
}

// The min-DOF JUNCTION GUARD: a 3-plane corner (plane_dof=0) carrying TWO ridge
// tags where only ONE survives the reality-intersect (the other ridge's segment
// doesn't reach the corner) → ridge_dof=1, but min(0,1)=0 → STAYS DOF-0. The
// naive "ridge==1 → DOF-1" override would wrongly un-pin the junction.
static void test_a2_junction_stays_dof0(void) {
    fprintf(stderr, "== test_a2_junction_stays_dof0\n");
    v7_br_input_plane_t planes[3] = {
        mk_flat_plane(0, 5.0),
        mk_slope_plane(1, 0.0, 0.707, 0.707, -5.0),
        mk_slope_plane(2, 0.707, 0.707, 0.0, -5.0),
    };
    // Ridge 0 passes through the corner (survives); ridge 1's segment is FAR
    // away (t out of [0,tmax]) so its tag does NOT survive the reality-intersect.
    v7_br_input_ridge_t ridges[2] = {
        mk_ridge(0, 0, 1, -5, 5, 0,  5, 5, 0),     // through (0,5,0)
        mk_ridge(1, 0, 2, 20, 5, 0, 30, 5, 0),     // 20..30 — corner at x=0 is off-segment
    };
    uint32_t* p3 = (uint32_t*)calloc(3, sizeof(uint32_t)); p3[0]=0; p3[1]=1; p3[2]=2;
    uint32_t* r2 = (uint32_t*)calloc(2, sizeof(uint32_t)); r2[0]=0; r2[1]=1;
    uint32_t* f0 = (uint32_t*)calloc(1, sizeof(uint32_t)); f0[0]=0;
    uint32_t* f1 = (uint32_t*)calloc(1, sizeof(uint32_t)); f1[0]=1;
    uint32_t* f2 = (uint32_t*)calloc(1, sizeof(uint32_t)); f2[0]=2;
    // 3-plane membership (junction) via one triangle per plane (corner 0 + fillers).
    v7_br_input_corner_t corners[7] = {
        mk_corner(0, 0, 5, 0, p3, 3, r2, 2),    // junction → DOF 0
        mk_corner(10, 3, 5, 0, f0, 1, NULL, 0), mk_corner(11, 4, 5, 0, f0, 1, NULL, 0),
        mk_corner(20, 3, 5, 1, f1, 1, NULL, 0), mk_corner(21, 4, 5, 1, f1, 1, NULL, 0),
        mk_corner(30, 3, 5, 2, f2, 1, NULL, 0), mk_corner(31, 4, 5, 2, f2, 1, NULL, 0),
    };
    uint32_t* path0 = (uint32_t*)calloc(3, sizeof(uint32_t)); path0[0]=0; path0[1]=10; path0[2]=11;
    uint32_t* path1 = (uint32_t*)calloc(3, sizeof(uint32_t)); path1[0]=0; path1[1]=20; path1[2]=21;
    uint32_t* path2 = (uint32_t*)calloc(3, sizeof(uint32_t)); path2[0]=0; path2[1]=30; path2[2]=31;
    v7_br_input_polygon_path_t paths[3] = {
        { .plane_id = 0, .corner_ids = path0, .n_corners = 3 },
        { .plane_id = 1, .corner_ids = path1, .n_corners = 3 },
        { .plane_id = 2, .corner_ids = path2, .n_corners = 3 },
    };
    int16_t owner_buf[1] = { -1 };
    float pos_buf[3] = { 0, 0, 0 };
    v7_br_params_t params; v7_br_params_default(&params);
    params.max_iterations = 0;
    v7_br_outputs_t out; v7_br_outputs_init(&out);
    int rc = v7_br_compute(planes, 3, corners, 7, NULL, 0, paths, 3, ridges, 2, NULL, 0,
                            pos_buf, 1, owner_buf, NULL, 0.12, &params, &out);
    CHECK(rc == 0);
    if (out.n_dof == 7) {
        CHECK(out.dof[0].dof == 0);            // min(0,1)=0 — junction preserved
        CHECK(out.dof[0].has_axis_a == false);
    }
    CHECK(out.stats.dof0 == 1);                // only the junction corner
    v7_br_outputs_free(&out);
    free(p3); free(r2); free(f0); free(f1); free(f2);
    free(path0); free(path1); free(path2);
}

// F2 (2026-06-20): a COPLANAR ridge (type 4) must NOT count toward DOF — its
// crease line nA×nB is ill-conditioned. A single-plane corner sitting on ONE
// real ridge (type 1) + ONE coplanar ridge (type 4), both passing through it,
// must be DOF-1 (slide on the real ridge), NOT DOF-0. (A step type 5 WOULD count
// — that's the distinction; only type 4 is excluded.)
static void test_f2_coplanar_ridge_not_counted_for_dof(void) {
    fprintf(stderr, "== test_f2_coplanar_ridge_not_counted_for_dof\n");
    v7_br_input_plane_t planes[3] = {
        mk_flat_plane(0, 5.0),
        mk_slope_plane(1, 0.0, 0.707, 0.707, -5.0),
        mk_slope_plane(2, 0.707, 0.707, 0.0, -5.0),
    };
    // Both ridges pass through the corner (0,5,0) → both survive the reality-
    // intersect. Ridge 0 = real (type 1, along +X); ridge 1 = coplanar (type 4,
    // along +Z). Without the F2 exclusion this would be a 2-survivor → DOF-0.
    v7_br_input_ridge_t ridges[2] = {
        mk_ridge(0, 0, 1, -5, 5, 0,  5, 5, 0),     // +X through (0,5,0)
        mk_ridge(1, 0, 2,  0, 5, -5, 0, 5, 5),     // +Z through (0,5,0)
    };
    ridges[0].type = 1;   // real ridge → counts
    ridges[1].type = 4;   // coplanar → must NOT count
    uint32_t* p0  = (uint32_t*)calloc(1, sizeof(uint32_t)); p0[0]=0;
    uint32_t* r2  = (uint32_t*)calloc(2, sizeof(uint32_t)); r2[0]=0; r2[1]=1;
    uint32_t* f0a = (uint32_t*)calloc(1, sizeof(uint32_t)); f0a[0]=0;
    uint32_t* f0b = (uint32_t*)calloc(1, sizeof(uint32_t)); f0b[0]=0;
    // Single-plane corner (plane 0 only via the path) tagged on BOTH ridges.
    v7_br_input_corner_t corners[3] = {
        mk_corner(0, 0, 5, 0, p0, 1, r2, 2),       // single-plane, on ridges {0(real),1(coplanar)}
        mk_corner(10, 3, 5, 0, f0a, 1, NULL, 0),
        mk_corner(11, 4, 5, 0, f0b, 1, NULL, 0),
    };
    uint32_t* path0 = (uint32_t*)calloc(3, sizeof(uint32_t)); path0[0]=0; path0[1]=10; path0[2]=11;
    v7_br_input_polygon_path_t paths[1] = {
        { .plane_id = 0, .corner_ids = path0, .n_corners = 3 },
    };
    int16_t owner_buf[1] = { -1 };
    float pos_buf[3] = { 0, 0, 0 };
    v7_br_params_t params; v7_br_params_default(&params);
    params.max_iterations = 0;
    v7_br_outputs_t out; v7_br_outputs_init(&out);
    int rc = v7_br_compute(planes, 3, corners, 3, NULL, 0, paths, 1, ridges, 2, NULL, 0,
                            pos_buf, 1, owner_buf, NULL, 0.12, &params, &out);
    CHECK(rc == 0);
    if (out.n_dof == 3) {
        CHECK(out.dof[0].dof == 1);            // coplanar ignored → 1 real survivor → DOF-1, NOT 0
        CHECK(out.dof[0].has_axis_a == true);  // slides along the REAL ridge
    }
    CHECK(out.stats.dof0 == 0);                // nothing locked (the coplanar didn't pin it)
    v7_br_outputs_free(&out);
    free(p0); free(r2); free(f0a); free(f0b); free(path0);
}

// Degenerate ridge (tmax<=0, from==to) → its tag is IGNORED. Single-plane
// corner tagged only on a zero-length ridge → stays DOF-2/free (no coupling,
// no axis from the ridge).
static void test_a2_degenerate_ridge_ignored(void) {
    fprintf(stderr, "== test_a2_degenerate_ridge_ignored\n");
    v7_br_input_plane_t planes[1] = { mk_flat_plane(0, 5.0) };
    v7_br_input_ridge_t ridges[1] = { mk_ridge(0, 0, 1, 0, 5, 0, 0, 5, 0) }; // from==to
    uint32_t* p1 = (uint32_t*)calloc(1, sizeof(uint32_t)); p1[0] = 0;
    uint32_t* r1 = (uint32_t*)calloc(1, sizeof(uint32_t)); r1[0] = 0;
    uint32_t* pf = (uint32_t*)calloc(1, sizeof(uint32_t)); pf[0] = 0;
    // Single-plane membership via one plane-0 triangle (corner 0 + 2 fillers).
    v7_br_input_corner_t corners[3] = {
        mk_corner(0, 0, 5, 0, p1, 1, r1, 1),   // single-plane, degenerate ridge → DOF 2
        mk_corner(10, 3, 5, 2, pf, 1, NULL, 0),
        mk_corner(11, 4, 5, 3, pf, 1, NULL, 0),
    };
    uint32_t* path0 = (uint32_t*)calloc(3, sizeof(uint32_t)); path0[0]=0; path0[1]=10; path0[2]=11;
    v7_br_input_polygon_path_t paths[1] = { { .plane_id = 0, .corner_ids = path0, .n_corners = 3 } };
    int16_t owner_buf[1] = { -1 };
    float pos_buf[3] = { 0, 0, 0 };
    v7_br_params_t params; v7_br_params_default(&params);
    params.max_iterations = 0;
    v7_br_outputs_t out; v7_br_outputs_init(&out);
    int rc = v7_br_compute(planes, 1, corners, 3, NULL, 0, paths, 1, ridges, 1, NULL, 0,
                            pos_buf, 1, owner_buf, NULL, 0.12, &params, &out);
    CHECK(rc == 0);
    if (out.n_dof == 3) {
        CHECK(out.dof[0].dof == 2);            // degenerate tag ignored → plane-DOF
        CHECK(out.dof[0].has_axis_a == true);  // plane-derived e1/e2 axes
        CHECK(out.dof[0].has_axis_b == true);
    }
    CHECK(out.stats.dof2 == 3);                // corner 0 + 2 fillers, all single-plane
    v7_br_outputs_free(&out);
    free(p1); free(r1); free(pf); free(path0);
}

// Near-parallel / off-segment proj must not produce NaN in the coupled axis.
// A single-plane corner whose ridge passes nearby but the corner is well within
// the segment → DOF-1, axis finite (no NaN). Sanity on the projection math.
static void test_a2_no_nan(void) {
    fprintf(stderr, "== test_a2_no_nan\n");
    v7_br_input_plane_t planes[1] = { mk_flat_plane(0, 5.0) };
    // A skewed ridge direction (not axis-aligned) to exercise the normalize.
    v7_br_input_ridge_t ridges[1] = { mk_ridge(0, 0, 1, -3, 5, -3, 3, 5, 3) };
    uint32_t* p1 = (uint32_t*)calloc(1, sizeof(uint32_t)); p1[0] = 0;
    uint32_t* r1 = (uint32_t*)calloc(1, sizeof(uint32_t)); r1[0] = 0;
    v7_br_input_corner_t corners[1] = { mk_corner(0, 0, 5, 0, p1, 1, r1, 1) };
    int16_t owner_buf[1] = { -1 };
    float pos_buf[3] = { 0, 0, 0 };
    v7_br_params_t params; v7_br_params_default(&params);
    params.max_iterations = 0;
    v7_br_outputs_t out; v7_br_outputs_init(&out);
    int rc = v7_br_compute(planes, 1, corners, 1, NULL, 0, NULL, 0, ridges, 1, NULL, 0,
                            pos_buf, 1, owner_buf, NULL, 0.12, &params, &out);
    CHECK(rc == 0);
    if (out.n_dof == 1) {
        CHECK(out.dof[0].dof == 1);
        CHECK(out.dof[0].has_axis_a == true);
        // (0,5,0) projects to ridge midpoint (0,5,0), perp=0 → on segment.
        CHECK(isfinite(out.dof[0].axis_a[0]));
        CHECK(isfinite(out.dof[0].axis_a[1]));
        CHECK(isfinite(out.dof[0].axis_a[2]));
        // axis is the normalized ridge dir → unit length.
        double n = out.dof[0].axis_a[0]*out.dof[0].axis_a[0]
                 + out.dof[0].axis_a[1]*out.dof[0].axis_a[1]
                 + out.dof[0].axis_a[2]*out.dof[0].axis_a[2];
        CHECK_CLOSE(n, 1.0, 1e-9);
    }
    v7_br_outputs_free(&out);
    free(p1); free(r1);
}

// BR_DOF_COUPLE=0 must DISABLE the effect: the same single-plane-on-ridge
// corner stays DOF-2 (plane-count) when the env flag is off. (The store + seed
// are always built; only the DOF effect is gated.)
static void test_a2_gate_off_keeps_plane_dof(void) {
    fprintf(stderr, "== test_a2_gate_off_keeps_plane_dof\n");
    setenv("BR_DOF_COUPLE", "0", 1);
    v7_br_input_plane_t planes[1] = { mk_flat_plane(0, 5.0) };
    v7_br_input_ridge_t ridges[1] = { mk_ridge(0, 0, 1, -5, 5, 0, 5, 5, 0) };
    uint32_t* p1 = (uint32_t*)calloc(1, sizeof(uint32_t)); p1[0] = 0;
    uint32_t* r1 = (uint32_t*)calloc(1, sizeof(uint32_t)); r1[0] = 0;
    v7_br_input_corner_t corners[1] = { mk_corner(0, 0, 5, 0, p1, 1, r1, 1) };
    int16_t owner_buf[1] = { -1 };
    float pos_buf[3] = { 0, 0, 0 };
    v7_br_params_t params; v7_br_params_default(&params);
    params.max_iterations = 0;
    v7_br_outputs_t out; v7_br_outputs_init(&out);
    int rc = v7_br_compute(planes, 1, corners, 1, NULL, 0, NULL, 0, ridges, 1, NULL, 0,
                            pos_buf, 1, owner_buf, NULL, 0.12, &params, &out);
    CHECK(rc == 0);
    if (out.n_dof == 1) CHECK(out.dof[0].dof == 2);  // coupling off → plane-count
    v7_br_outputs_free(&out);
    free(p1); free(r1);
    unsetenv("BR_DOF_COUPLE");
}

// ── T4: snap-in + read-only share-guard preview ─────────────────────

// SNAP-IN: a FREE (single-plane, untagged) corner sitting within τ_in of an
// allow-listed ridge → the in-loop snap-in (in CHASE_AFTER_MOVE) tags it onto
// the ridge and RETAG_DOF makes it DOF-1 with axis_a = rg.rd + has_axis_a (no
// freeze). One-way: once snapped the tag persists (a later nudge can't un-snap).
// We force a SLIDE by giving the corner a non-canonical angle (angle cost pulls
// it), so the chase runs from it and the snap fires.
static void test_t4_snap_in_free_to_ridge(void) {
    fprintf(stderr, "== test_t4_snap_in_free_to_ridge\n");
    v7_br_input_plane_t planes[1] = { mk_flat_plane(0, 5.0) };
    // Ridge runs along +X at (y=5, z=0); type=1 (ridge) → allow-listed.
    v7_br_input_ridge_t ridges[1] = { mk_ridge(0, 0, 1, -5, 5, 0, 5, 5, 0) };
    ridges[0].type = 1;
    // Square-ish polygon; corner 1 starts at z=0.05 (within τ_in=0.30 perp of
    // the ridge at z=0) and slightly off a clean 90° so the optimizer moves it.
    uint32_t* p0 = (uint32_t*)calloc(1, sizeof(uint32_t)); p0[0]=0;
    uint32_t* p1 = (uint32_t*)calloc(1, sizeof(uint32_t)); p1[0]=0;
    uint32_t* p2 = (uint32_t*)calloc(1, sizeof(uint32_t)); p2[0]=0;
    uint32_t* p3 = (uint32_t*)calloc(1, sizeof(uint32_t)); p3[0]=0;
    v7_br_input_corner_t corners[4] = {
        mk_corner(0, -3, 5,  0.00, p0, 1, NULL, 0),
        mk_corner(1,  0, 5,  0.05, p1, 1, NULL, 0),  // FREE, on/near ridge, UNTAGGED
        mk_corner(2,  3, 5,  0.00, p2, 1, NULL, 0),
        mk_corner(3,  0, 5,  4.00, p3, 1, NULL, 0),
    };
    uint32_t* path_ids = (uint32_t*)calloc(4, sizeof(uint32_t));
    path_ids[0]=0; path_ids[1]=1; path_ids[2]=2; path_ids[3]=3;
    v7_br_input_polygon_path_t paths[1] = { { .plane_id = 0, .corner_ids = path_ids, .n_corners = 4 } };
    int16_t owner_buf[1] = { -1 };
    float pos_buf[3] = { 0, 0, 0 };
    v7_br_params_t params; v7_br_params_default(&params);
    params.max_iterations = 50;
    params.use_wsd_cost = false;          // no points
    params.use_proximity_merge = false;   // isolate the snap-in (no merges)
    params.use_collinear_removal = false;
    params.use_joint_moves = false;
    setenv("BR_SNAP_IN", "1", 1);
    v7_br_outputs_t out; v7_br_outputs_init(&out);
    int rc = v7_br_compute(planes, 1, corners, 4, NULL, 0, paths, 1, ridges, 1, NULL, 0,
                            pos_buf, 1, owner_buf, NULL, 0.12, &params, &out);
    CHECK(rc == 0);
    // Corner 1 must have snapped onto the ridge → DOF-1 with a ridge axis.
    if (out.n_dof == 4) {
        CHECK(out.dof[1].dof == 1);            // FREE(2) → RIDGE(1) by snap-in
        CHECK(out.dof[1].has_axis_a == true);  // axis present (no freeze)
        CHECK(out.dof[1].has_axis_b == false);
        // axis_a == ±rg.rd (+X); unit length.
        double n = out.dof[1].axis_a[0]*out.dof[1].axis_a[0]
                 + out.dof[1].axis_a[1]*out.dof[1].axis_a[1]
                 + out.dof[1].axis_a[2]*out.dof[1].axis_a[2];
        CHECK_CLOSE(n, 1.0, 1e-9);
        CHECK_CLOSE(fabs(out.dof[1].axis_a[0]), 1.0, 1e-9);  // along X
    }
    v7_br_outputs_free(&out);
    free(p0); free(p1); free(p2); free(p3); free(path_ids);
    unsetenv("BR_SNAP_IN");
}

// SNAP-IN gated OFF: with BR_SNAP_IN=0 the same FREE corner is NOT snapped (it
// stays DOF-2, plane-derived). Proves the EFFECT is suppressed (the byte-equiv
// proof at unit scale).
static void test_t4_snap_in_off_stays_free(void) {
    fprintf(stderr, "== test_t4_snap_in_off_stays_free\n");
    v7_br_input_plane_t planes[1] = { mk_flat_plane(0, 5.0) };
    v7_br_input_ridge_t ridges[1] = { mk_ridge(0, 0, 1, -5, 5, 0, 5, 5, 0) };
    ridges[0].type = 1;
    uint32_t* p0 = (uint32_t*)calloc(1, sizeof(uint32_t)); p0[0]=0;
    uint32_t* p1 = (uint32_t*)calloc(1, sizeof(uint32_t)); p1[0]=0;
    uint32_t* p2 = (uint32_t*)calloc(1, sizeof(uint32_t)); p2[0]=0;
    uint32_t* p3 = (uint32_t*)calloc(1, sizeof(uint32_t)); p3[0]=0;
    v7_br_input_corner_t corners[4] = {
        mk_corner(0, -3, 5,  0.00, p0, 1, NULL, 0),
        mk_corner(1,  0, 5,  0.05, p1, 1, NULL, 0),
        mk_corner(2,  3, 5,  0.00, p2, 1, NULL, 0),
        mk_corner(3,  0, 5,  4.00, p3, 1, NULL, 0),
    };
    uint32_t* path_ids = (uint32_t*)calloc(4, sizeof(uint32_t));
    path_ids[0]=0; path_ids[1]=1; path_ids[2]=2; path_ids[3]=3;
    v7_br_input_polygon_path_t paths[1] = { { .plane_id = 0, .corner_ids = path_ids, .n_corners = 4 } };
    int16_t owner_buf[1] = { -1 };
    float pos_buf[3] = { 0, 0, 0 };
    v7_br_params_t params; v7_br_params_default(&params);
    params.max_iterations = 50;
    params.use_wsd_cost = false;
    params.use_proximity_merge = false;
    params.use_collinear_removal = false;
    params.use_joint_moves = false;
    setenv("BR_SNAP_IN", "0", 1);
    v7_br_outputs_t out; v7_br_outputs_init(&out);
    int rc = v7_br_compute(planes, 1, corners, 4, NULL, 0, paths, 1, ridges, 1, NULL, 0,
                            pos_buf, 1, owner_buf, NULL, 0.12, &params, &out);
    CHECK(rc == 0);
    if (out.n_dof == 4) CHECK(out.dof[1].dof == 2);  // snap-in OFF → stays FREE
    v7_br_outputs_free(&out);
    free(p0); free(p1); free(p2); free(p3); free(path_ids);
    unsetenv("BR_SNAP_IN");
}

// GUARD cond-A geometry recognised, no SPLIT at max_iterations=0. A SINGLE-PLANE
// corner cA on ridge R sits inside a B path-edge whose BOTH endpoints carry
// ridge_id==R. With the loop disabled (max_iterations=0) the in-loop SPLIT op
// never runs, so n_split_share stays 0 (T7 re-pointed the stat to the REAL op —
// the post-loop census is preview-only and no longer mutates the stat) and cA
// stays single-plane. The REAL split (loop running) is exercised by the T7
// fixtures below (test_t7_split_forms_share et al.).
static void test_t4_guard_condA_bracket(void) {
    fprintf(stderr, "== test_t4_guard_condA_bracket\n");
    // Two planes meeting on a ridge along +X at y=5,z=0 (type=1 → allowed).
    v7_br_input_plane_t planes[2] = {
        mk_flat_plane(0, 5.0),
        mk_slope_plane(1, 0.0, 0.707, 0.707, -5.0),
    };
    v7_br_input_ridge_t ridges[1] = { mk_ridge(0, 0, 1, -5, 5, 0, 5, 5, 0) };
    ridges[0].type = 1;
    // Plane A (id 0): a triangle whose apex (corner 0) is a single-plane corner
    // sitting ON the ridge at x=0. Plane B (id 1): a polygon with two consecutive
    // corners (10,11) BOTH on the ridge (tagged R=0) bracketing x=0 → cond-A edge.
    uint32_t* pA0 = (uint32_t*)calloc(1, sizeof(uint32_t)); pA0[0]=0;  // single-plane, on ridge, NO tag (snap will tag)
    uint32_t* pA1 = (uint32_t*)calloc(1, sizeof(uint32_t)); pA1[0]=0;
    uint32_t* pA2 = (uint32_t*)calloc(1, sizeof(uint32_t)); pA2[0]=0;
    uint32_t* pB0 = (uint32_t*)calloc(1, sizeof(uint32_t)); pB0[0]=1;
    uint32_t* pB1 = (uint32_t*)calloc(1, sizeof(uint32_t)); pB1[0]=1;
    uint32_t* pB2 = (uint32_t*)calloc(1, sizeof(uint32_t)); pB2[0]=1;
    uint32_t* rB0 = (uint32_t*)calloc(1, sizeof(uint32_t)); rB0[0]=0;  // B endpoint on R
    uint32_t* rB1 = (uint32_t*)calloc(1, sizeof(uint32_t)); rB1[0]=0;  // B endpoint on R
    v7_br_input_corner_t corners[6] = {
        mk_corner(0,  0.0, 5,  0.0, pA0, 1, NULL, 0),   // cA: single-plane on ridge (snap tags it)
        mk_corner(1, -2.0, 5,  2.0, pA1, 1, NULL, 0),
        mk_corner(2,  2.0, 5,  2.0, pA2, 1, NULL, 0),
        mk_corner(10,-3.0, 5,  0.0, pB0, 1, rB0, 1),    // B edge endpoint, tagged R
        mk_corner(11, 3.0, 5,  0.0, pB1, 1, rB1, 1),    // B edge endpoint, tagged R
        mk_corner(12, 0.0, 5, -3.0, pB2, 1, NULL, 0),
    };
    uint32_t* pathA = (uint32_t*)calloc(3, sizeof(uint32_t)); pathA[0]=0; pathA[1]=1; pathA[2]=2;
    uint32_t* pathB = (uint32_t*)calloc(3, sizeof(uint32_t)); pathB[0]=10; pathB[1]=11; pathB[2]=12;
    v7_br_input_polygon_path_t paths[2] = {
        { .plane_id = 0, .corner_ids = pathA, .n_corners = 3 },
        { .plane_id = 1, .corner_ids = pathB, .n_corners = 3 },
    };
    int16_t owner_buf[1] = { -1 };
    float pos_buf[3] = { 0, 0, 0 };
    v7_br_params_t params; v7_br_params_default(&params);
    params.max_iterations = 0;            // no SLIDE; census runs on seeded state
    params.use_wsd_cost = false;
    setenv("BR_SNAP_IN", "1", 1);
    v7_br_outputs_t out; v7_br_outputs_init(&out);
    int rc = v7_br_compute(planes, 2, corners, 6, NULL, 0, paths, 2, ridges, 1, NULL, 0,
                            pos_buf, 1, owner_buf, NULL, 0.12, &params, &out);
    CHECK(rc == 0);
    // Loop disabled → the REAL SPLIT never runs → n_split_share stays 0 (T7
    // re-pointed it off the census). cond-B does not fire either (nearest B
    // corner is 3 m away). cA stays single-plane.
    CHECK(out.stats.n_split_share == 0);   // T7: stat owned by the real op (not run here)
    CHECK(out.stats.n_would_merge == 0);   // cond-B did NOT fire
    if (out.n_corner_planes >= 1) CHECK(out.corner_planes[0].n_plane_ids == 1);
    v7_br_outputs_free(&out);
    free(pA0); free(pA1); free(pA2); free(pB0); free(pB1); free(pB2);
    free(rB0); free(rB1); free(pathA); free(pathB);
    unsetenv("BR_SNAP_IN");
}

// GUARD allowlist (I1): a corner on a STEP ridge (type=5) is NOT shareable —
// the step ridge is excluded from the make-shared allowlist, so ridges_incident_to
// returns it for no purpose: no snap-in, no would-share. Same geometry as cond-A
// but with type=5 → both tallies stay 0.
static void test_t4_guard_step_ridge_not_shareable(void) {
    fprintf(stderr, "== test_t4_guard_step_ridge_not_shareable\n");
    v7_br_input_plane_t planes[2] = {
        mk_flat_plane(0, 5.0),
        mk_slope_plane(1, 0.0, 0.707, 0.707, -5.0),
    };
    v7_br_input_ridge_t ridges[1] = { mk_ridge(0, 0, 1, -5, 5, 0, 5, 5, 0) };
    ridges[0].type = 5;   // STEP — EXCLUDED from the allowlist
    uint32_t* pA0 = (uint32_t*)calloc(1, sizeof(uint32_t)); pA0[0]=0;
    uint32_t* pA1 = (uint32_t*)calloc(1, sizeof(uint32_t)); pA1[0]=0;
    uint32_t* pA2 = (uint32_t*)calloc(1, sizeof(uint32_t)); pA2[0]=0;
    uint32_t* pB0 = (uint32_t*)calloc(1, sizeof(uint32_t)); pB0[0]=1;
    uint32_t* pB1 = (uint32_t*)calloc(1, sizeof(uint32_t)); pB1[0]=1;
    uint32_t* pB2 = (uint32_t*)calloc(1, sizeof(uint32_t)); pB2[0]=1;
    uint32_t* rB0 = (uint32_t*)calloc(1, sizeof(uint32_t)); rB0[0]=0;
    uint32_t* rB1 = (uint32_t*)calloc(1, sizeof(uint32_t)); rB1[0]=0;
    v7_br_input_corner_t corners[6] = {
        mk_corner(0,  0.0, 5,  0.0, pA0, 1, NULL, 0),
        mk_corner(1, -2.0, 5,  2.0, pA1, 1, NULL, 0),
        mk_corner(2,  2.0, 5,  2.0, pA2, 1, NULL, 0),
        mk_corner(10,-3.0, 5,  0.0, pB0, 1, rB0, 1),
        mk_corner(11, 3.0, 5,  0.0, pB1, 1, rB1, 1),
        mk_corner(12, 0.0, 5, -3.0, pB2, 1, NULL, 0),
    };
    uint32_t* pathA = (uint32_t*)calloc(3, sizeof(uint32_t)); pathA[0]=0; pathA[1]=1; pathA[2]=2;
    uint32_t* pathB = (uint32_t*)calloc(3, sizeof(uint32_t)); pathB[0]=10; pathB[1]=11; pathB[2]=12;
    v7_br_input_polygon_path_t paths[2] = {
        { .plane_id = 0, .corner_ids = pathA, .n_corners = 3 },
        { .plane_id = 1, .corner_ids = pathB, .n_corners = 3 },
    };
    int16_t owner_buf[1] = { -1 };
    float pos_buf[3] = { 0, 0, 0 };
    v7_br_params_t params; v7_br_params_default(&params);
    params.max_iterations = 0;
    params.use_wsd_cost = false;
    setenv("BR_SNAP_IN", "1", 1);
    v7_br_outputs_t out; v7_br_outputs_init(&out);
    int rc = v7_br_compute(planes, 2, corners, 6, NULL, 0, paths, 2, ridges, 1, NULL, 0,
                            pos_buf, 1, owner_buf, NULL, 0.12, &params, &out);
    CHECK(rc == 0);
    CHECK(out.stats.n_split_share == 0);   // step ridge → NOT a SHARE candidate
    CHECK(out.stats.n_would_merge == 0);
    v7_br_outputs_free(&out);
    free(pA0); free(pA1); free(pA2); free(pB0); free(pB1); free(pB2);
    free(rB0); free(rB1); free(pathA); free(pathB);
    unsetenv("BR_SNAP_IN");
}

// ─── T5: MERGE_CORNER (cross-plane weld replacing R.4) ──────────────
//
// All four share a base fixture: planes A(flat y=5) and B(slope) meet on a
// ridge along +X at y=5,z=0. A free DOF-2 "mover" corner with a strongly
// non-canonical angle guarantees ≥1 accepted SLIDE → the chase fires →
// MERGE_CORNER_XPLANE scans every ridge (it isn't limited to the mover).
// (The chase only runs after an accepted move, so a mover is required.)

// Two cross-plane on-ridge corners (within τ_partner=0.5) → one survivor on
// the constrained meet, plane_ids={A,B}, in BOTH paths, DOF-1.
static void test_t5_merge_corner_xplane(void) {
    fprintf(stderr, "== test_t5_merge_corner_xplane\n");
    v7_br_input_plane_t planes[2] = {
        mk_flat_plane(0, 5.0),
        mk_slope_plane(1, 0.0, 0.707, 0.707, -5.0),
    };
    v7_br_input_ridge_t ridges[1] = { mk_ridge(0, 0, 1, -5, 5, 0, 5, 5, 0) };
    ridges[0].type = 1;   // ridge → allow-listed
    // Plane A: corner 0 ON the crease at x=0; plane B: corner 10 ON the crease
    // at x=0.4 (3D gap 0.40 < τ_partner=0.50, NOT sharing a plane → weld).
    // Each plane gets a 2-corner tail forming a triangle; A's corner 1 sits at
    // an obtuse angle so the optimizer accepts a move (kicks the chase).
    uint32_t* pA0=(uint32_t*)calloc(1,4); pA0[0]=0;
    uint32_t* pA1=(uint32_t*)calloc(1,4); pA1[0]=0;
    uint32_t* pA2=(uint32_t*)calloc(1,4); pA2[0]=0;
    uint32_t* pB0=(uint32_t*)calloc(1,4); pB0[0]=1;
    uint32_t* pB1=(uint32_t*)calloc(1,4); pB1[0]=1;
    uint32_t* pB2=(uint32_t*)calloc(1,4); pB2[0]=1;
    v7_br_input_corner_t corners[6] = {
        mk_corner(0,  0.0, 5,  0.0, pA0, 1, NULL, 0),  // cA on crease
        mk_corner(1, -2.3, 5,  1.7, pA1, 1, NULL, 0),  // mover (obtuse)
        mk_corner(2,  2.0, 5,  2.0, pA2, 1, NULL, 0),
        mk_corner(10, 0.4, 5,  0.0, pB0, 1, NULL, 0),  // cB on crease, ~0.4 from cA
        mk_corner(11, 3.0, 5, -2.5, pB1, 1, NULL, 0),
        mk_corner(12,-3.0, 5, -2.5, pB2, 1, NULL, 0),
    };
    uint32_t* pathA=(uint32_t*)calloc(3,4); pathA[0]=0; pathA[1]=1; pathA[2]=2;
    uint32_t* pathB=(uint32_t*)calloc(3,4); pathB[0]=10; pathB[1]=11; pathB[2]=12;
    v7_br_input_polygon_path_t paths[2] = {
        { .plane_id = 0, .corner_ids = pathA, .n_corners = 3 },
        { .plane_id = 1, .corner_ids = pathB, .n_corners = 3 },
    };
    int16_t owner_buf[1] = { -1 };
    float pos_buf[3] = { 0, 0, 0 };
    v7_br_params_t params; v7_br_params_default(&params);
    params.max_iterations = 30;
    params.use_wsd_cost = false;   // angle/nbr cost drives the mover
    setenv("BR_SNAP_IN", "1", 1);
    v7_br_outputs_t out; v7_br_outputs_init(&out);
    int rc = v7_br_compute(planes, 2, corners, 6, NULL, 0, paths, 2, ridges, 1, NULL, 0,
                            pos_buf, 1, owner_buf, NULL, 0.12, &params, &out);
    CHECK(rc == 0);
    // Exactly one cross-plane weld fired (the cA/cB pair).
    CHECK(out.stats.xplane_merged == 1);
    CHECK(out.stats.n_merge_corner >= 1);
    // Survivor carries BOTH planes; loser is dropped from output.
    int surv_shared = 0;
    for (size_t i = 0; i < out.n_corner_planes; i++) {
        if (out.corner_planes[i].n_plane_ids == 2) {
            uint32_t a = out.corner_planes[i].plane_ids[0];
            uint32_t b = out.corner_planes[i].plane_ids[1];
            if ((a==0&&b==1)||(a==1&&b==0)) surv_shared++;
        }
    }
    // Exactly one {A,B} shared corner. n_plane_ids==2 ⟹ RETAG_DOF set it
    // DOF-1 (slides on the crease) — the non-stale DOF signal (out->dof is a
    // pre-loop snapshot, §9, so we assert via the plane-count that DERIVES it).
    CHECK(surv_shared == 1);
    // The survivor appears in BOTH planes' emitted paths (one id, both rings).
    CHECK(out.n_polygon_paths == 2);
    if (out.n_polygon_paths == 2) {
        // find the shared corner id
        uint32_t shared_id = UINT32_MAX;
        for (size_t i = 0; i < out.n_corner_planes; i++)
            if (out.corner_planes[i].n_plane_ids == 2) shared_id = out.corner_planes[i].corner_id;
        int inA=0, inB=0;
        for (size_t p=0; p<out.n_polygon_paths; p++) {
            const v7_br_polygon_path_t* pp = &out.polygon_paths[p];
            for (uint32_t k=0;k<pp->n_corners;k++) if (pp->corner_ids[k]==shared_id) {
                if (pp->plane_id==0) inA=1; else if (pp->plane_id==1) inB=1;
            }
        }
        CHECK(inA == 1 && inB == 1);   // shared corner in both rings → shared edge potential
    }
    v7_br_outputs_free(&out);
    free(pA0);free(pA1);free(pA2);free(pB0);free(pB1);free(pB2);
    free(pathA);free(pathB);
    unsetenv("BR_SNAP_IN");
}

// A step-ridge pair: identical geometry but ridge type=5 (step) → the
// type-gate excludes it → NO cross-plane weld fires.
static void test_t5_merge_corner_step_gated(void) {
    fprintf(stderr, "== test_t5_merge_corner_step_gated\n");
    v7_br_input_plane_t planes[2] = {
        mk_flat_plane(0, 5.0),
        mk_slope_plane(1, 0.0, 0.707, 0.707, -5.0),
    };
    v7_br_input_ridge_t ridges[1] = { mk_ridge(0, 0, 1, -5, 5, 0, 5, 5, 0) };
    ridges[0].type = 5;   // STEP — EXCLUDED
    uint32_t* pA0=(uint32_t*)calloc(1,4); pA0[0]=0;
    uint32_t* pA1=(uint32_t*)calloc(1,4); pA1[0]=0;
    uint32_t* pA2=(uint32_t*)calloc(1,4); pA2[0]=0;
    uint32_t* pB0=(uint32_t*)calloc(1,4); pB0[0]=1;
    uint32_t* pB1=(uint32_t*)calloc(1,4); pB1[0]=1;
    uint32_t* pB2=(uint32_t*)calloc(1,4); pB2[0]=1;
    v7_br_input_corner_t corners[6] = {
        mk_corner(0,  0.0, 5,  0.0, pA0, 1, NULL, 0),
        mk_corner(1, -2.3, 5,  1.7, pA1, 1, NULL, 0),
        mk_corner(2,  2.0, 5,  2.0, pA2, 1, NULL, 0),
        mk_corner(10, 0.4, 5,  0.0, pB0, 1, NULL, 0),
        mk_corner(11, 3.0, 5, -2.5, pB1, 1, NULL, 0),
        mk_corner(12,-3.0, 5, -2.5, pB2, 1, NULL, 0),
    };
    uint32_t* pathA=(uint32_t*)calloc(3,4); pathA[0]=0; pathA[1]=1; pathA[2]=2;
    uint32_t* pathB=(uint32_t*)calloc(3,4); pathB[0]=10; pathB[1]=11; pathB[2]=12;
    v7_br_input_polygon_path_t paths[2] = {
        { .plane_id = 0, .corner_ids = pathA, .n_corners = 3 },
        { .plane_id = 1, .corner_ids = pathB, .n_corners = 3 },
    };
    int16_t owner_buf[1] = { -1 };
    float pos_buf[3] = { 0, 0, 0 };
    v7_br_params_t params; v7_br_params_default(&params);
    params.max_iterations = 30;
    params.use_wsd_cost = false;
    setenv("BR_SNAP_IN", "1", 1);
    v7_br_outputs_t out; v7_br_outputs_init(&out);
    int rc = v7_br_compute(planes, 2, corners, 6, NULL, 0, paths, 2, ridges, 1, NULL, 0,
                            pos_buf, 1, owner_buf, NULL, 0.12, &params, &out);
    CHECK(rc == 0);
    CHECK(out.stats.xplane_merged == 0);   // type-gate: step ridge never welds
    // No {A,B} shared corner created by a weld.
    int shared = 0;
    for (size_t i = 0; i < out.n_corner_planes; i++)
        if (out.corner_planes[i].n_plane_ids >= 2) shared++;
    CHECK(shared == 0);
    v7_br_outputs_free(&out);
    free(pA0);free(pA1);free(pA2);free(pB0);free(pB1);free(pB2);
    free(pathA);free(pathB);
    unsetenv("BR_SNAP_IN");
}

// The pre-commit self-int guard PRIMITIVE (merge_would_self_intersect, the
// M5 tier-1 gate the cross-plane weld calls before MERGE_CORNERS). Tested
// directly: a survivor position that folds the would-be ring → true (reject);
// a clean survivor position on the same ring → false (commit). Direct because
// driving the in-loop optimizer to a deterministic fold is brittle; the guard
// is the load-bearing unit and is exercised in isolation here.
static void test_t5_merge_self_int_guard(void) {
    fprintf(stderr, "== test_t5_merge_self_int_guard\n");
    // Flat plane z=0 (XY UV) so positions map 1:1 to UV.
    double n[3] = {0,0,1};
    plane_uv_basis_t basis; plane_basis(&basis, n, 0.0);
    // A convex quad ring: ids 10,11,12,13 at the corners of a 4×4 square.
    // The loser id is 99 (a stand-in vertex coincident with id 10's slot —
    // we substitute it to id 10 = survivor). We model "the survivor MOVES to
    // surv_pos" by giving id 10 a current position and passing surv_pos.
    corner_state_t cs[5]; memset(cs, 0, sizeof(cs));
    // index by array slot; corner_map maps id→slot.
    // slot0:id10 (0,0) slot1:id11 (4,0) slot2:id12 (4,4) slot3:id13 (0,4)
    // Ring order: P0(id10) → P1(id11=(0,0)) → P2(id12=(4,0)) → P3(id13=(4,4)).
    // P0's "home" is (0,4) — a simple square.
    cs[0].point[0]=0; cs[0].point[1]=4; cs[0].alive=true;   // P0 (survivor)
    cs[1].point[0]=0; cs[1].point[1]=0; cs[1].alive=true;   // P1
    cs[2].point[0]=4; cs[2].point[1]=0; cs[2].alive=true;   // P2
    cs[3].point[0]=4; cs[3].point[1]=4; cs[3].alive=true;   // P3
    id_map_t cmap; id_map_build(&cmap, 100);
    id_set(&cmap, 10, 0); id_set(&cmap, 11, 1);
    id_set(&cmap, 12, 2); id_set(&cmap, 13, 3);
    uint32_t ids[4] = {10,11,12,13};
    uint32_t idsc[8]; double usc[8], vsc[8];
    // (a) CLEAN: survivor (id 10) placed near its home corner (0.5,3.5) — the
    // square stays simple → guard returns false (allow weld).
    double clean_pos[3] = {0.5, 3.5, 0.0};
    bool folds_clean = merge_would_self_intersect(
        ids, 4, /*lose*/99, /*surv*/10, clean_pos, &cmap, cs, &basis,
        idsc, usc, vsc);
    CHECK(folds_clean == false);   // no fold → commit
    // (b) FOLD: survivor (id 10) yanked BELOW the P1→P2 base edge to (2,-2).
    // Edge P3→P0 = (4,4)→(2,-2) now crosses the non-adjacent base edge
    // P1→P2 = (0,0)→(4,0) (at x≈2.67) → self-intersection → guard true.
    double fold_pos[3] = {2.0, -2.0, 0.0};
    bool folds_bad = merge_would_self_intersect(
        ids, 4, 99, 10, fold_pos, &cmap, cs, &basis, idsc, usc, vsc);
    CHECK(folds_bad == true);      // fold detected → REJECT (no mutation)
    id_map_free(&cmap);
}

// ─── T6: DROP_REDUNDANT_ON_RIDGE (all-polys-agree ridge-cleanup) ────────────

// Helper: is `cid` a consecutive edge with `did` in the emitted path for plane
// `plane_id`? (either order). Used to assert a shared edge formed.
static int emitted_edge_present(const v7_br_outputs_t* out, uint32_t plane_id,
                                uint32_t cid, uint32_t did) {
    for (size_t p = 0; p < out->n_polygon_paths; p++) {
        const v7_br_polygon_path_t* pp = &out->polygon_paths[p];
        if (pp->plane_id != plane_id) continue;
        uint32_t n = pp->n_corners;
        for (uint32_t k = 0; k < n; k++) {
            uint32_t a = pp->corner_ids[k], b = pp->corner_ids[(k + 1) % n];
            if ((a == cid && b == did) || (a == did && b == cid)) return 1;
        }
    }
    return 0;
}
static int emitted_has_corner(const v7_br_outputs_t* out, uint32_t plane_id, uint32_t cid) {
    for (size_t p = 0; p < out->n_polygon_paths; p++) {
        const v7_br_polygon_path_t* pp = &out->polygon_paths[p];
        if (pp->plane_id != plane_id) continue;
        for (uint32_t k = 0; k < pp->n_corners; k++) if (pp->corner_ids[k] == cid) return 1;
    }
    return 0;
}
static uint32_t emitted_n_plane_ids(const v7_br_outputs_t* out, uint32_t cid) {
    for (size_t i = 0; i < out->n_corner_planes; i++)
        if (out->corner_planes[i].corner_id == cid) return out->corner_planes[i].n_plane_ids;
    return 0;
}

// (A) §4.4 shared-edge formation. Two planes share a ridge along +X. Each plane's
// path has on-ridge interior midpoints (plane-0-only A4/A7, plane-1-only B6) that
// are redundant + two shared on-ridge endpoints S0/S10. After the pre-loop
// all-polys-agree cleanup, the redundant interiors are dropped and S0→S10 becomes
// a consecutive (shared) edge in BOTH emitted paths — one shared edge formed.
static void test_t6_shared_edge_formation(void) {
    fprintf(stderr, "== test_t6_shared_edge_formation\n");
    v7_br_input_plane_t planes[2] = {
        mk_flat_plane(0, 5.0),
        mk_slope_plane(1, 0.0, 0.707, 0.707, -5.0),
    };
    // Ridge index 0 along +X, t∈[0,10], through y=5,z=0.
    v7_br_input_ridge_t ridges[1] = { mk_ridge(0, 0, 1, 0, 5, 0, 10, 5, 0) };
    ridges[0].type = 1;
    uint32_t* rS0 = (uint32_t*)calloc(1,4); rS0[0]=0;
    uint32_t* rA4 = (uint32_t*)calloc(1,4); rA4[0]=0;
    uint32_t* rA7 = (uint32_t*)calloc(1,4); rA7[0]=0;
    uint32_t* rS10= (uint32_t*)calloc(1,4); rS10[0]=0;
    uint32_t* rB6 = (uint32_t*)calloc(1,4); rB6[0]=0;
    uint32_t* p01a=(uint32_t*)calloc(2,4); p01a[0]=0; p01a[1]=1;   // S0 / S10 share {0,1}
    uint32_t* p01b=(uint32_t*)calloc(2,4); p01b[0]=0; p01b[1]=1;
    uint32_t* pA  =(uint32_t*)calloc(1,4); pA[0]=0;
    uint32_t* pB  =(uint32_t*)calloc(1,4); pB[0]=1;
    uint32_t* pFA =(uint32_t*)calloc(1,4); pFA[0]=0;
    uint32_t* pFB =(uint32_t*)calloc(1,4); pFB[0]=1;
    // ids: S0=0 A4=4 A7=7 S10=10 B6=6 FA=100 FB=101
    v7_br_input_corner_t corners[7] = {
        mk_corner(0,   0, 5,  0, p01a, 2, rS0,  1),  // S0  shared endpoint, on-R
        mk_corner(4,   4, 5,  0, pA,   1, rA4,  1),  // A4  plane0-only, on-R (redundant)
        mk_corner(7,   7, 5,  0, pA,   1, rA7,  1),  // A7  plane0-only, on-R (redundant)
        mk_corner(10, 10, 5,  0, p01b, 2, rS10, 1),  // S10 shared endpoint, on-R
        mk_corner(6,   6, 5,  0, pB,   1, rB6,  1),  // B6  plane1-only, on-R (redundant)
        mk_corner(100, 5, 5,  3, pFA,  1, NULL, 0),  // filler A, off-R
        mk_corner(101, 5, 5, -3, pFB,  1, NULL, 0),  // filler B, off-R
    };
    uint32_t* path0 = (uint32_t*)calloc(5,4); path0[0]=0; path0[1]=4; path0[2]=7; path0[3]=10; path0[4]=100;
    uint32_t* path1 = (uint32_t*)calloc(4,4); path1[0]=0; path1[1]=6; path1[2]=10; path1[3]=101;
    v7_br_input_polygon_path_t paths[2] = {
        { .plane_id = 0, .corner_ids = path0, .n_corners = 5 },
        { .plane_id = 1, .corner_ids = path1, .n_corners = 4 },
    };
    int16_t owner_buf[1] = { -1 };
    float pos_buf[3] = { 0, 0, 0 };
    v7_br_params_t params; v7_br_params_default(&params);
    params.max_iterations = 0;   // pre-loop cleanup only
    v7_br_outputs_t out; v7_br_outputs_init(&out);
    int rc = v7_br_compute(planes, 2, corners, 7, NULL, 0, paths, 2, ridges, 1, NULL, 0,
                            pos_buf, 1, owner_buf, NULL, 0.12, &params, &out);
    CHECK(rc == 0);
    // The three redundant on-ridge interiors A4, A7, B6 are dropped.
    CHECK(out.stats.redundant_ridge_removed == 3);
    CHECK(emitted_has_corner(&out, 0, 4) == 0);
    CHECK(emitted_has_corner(&out, 0, 7) == 0);
    CHECK(emitted_has_corner(&out, 1, 6) == 0);
    // The shared endpoints survive in BOTH planes.
    CHECK(emitted_has_corner(&out, 0, 0)  == 1);
    CHECK(emitted_has_corner(&out, 1, 0)  == 1);
    CHECK(emitted_has_corner(&out, 0, 10) == 1);
    CHECK(emitted_has_corner(&out, 1, 10) == 1);
    // S0→S10 is now a consecutive (shared) edge in BOTH emitted paths.
    CHECK(emitted_edge_present(&out, 0, 0, 10) == 1);
    CHECK(emitted_edge_present(&out, 1, 0, 10) == 1);
    v7_br_outputs_free(&out);
    free(rS0); free(rA4); free(rA7); free(rS10); free(rB6);
    free(p01a); free(p01b); free(pA); free(pB); free(pFA); free(pFB);
    free(path0); free(path1);
}

// (B) all-polys-agree — the DISCRIMINATING test. Shared corner X{0,1} is a
// redundant on-ridge midpoint in plane-0's (W,X,Z) run, but in plane-1 it is the
// shared-run ENDPOINT (plane-1 turns OFF the ridge at X — its neighbours there are
// off-R). The old per-path pass would drop X from plane-0 (destroying the would-be
// shared edge); the all-polys-agree rule KEEPS X in BOTH paths.
static void test_t6_all_polys_agree_keeps_shared(void) {
    fprintf(stderr, "== test_t6_all_polys_agree_keeps_shared\n");
    v7_br_input_plane_t planes[2] = {
        mk_flat_plane(0, 5.0),
        mk_slope_plane(1, 0.0, 0.707, 0.707, -5.0),
    };
    v7_br_input_ridge_t ridges[1] = { mk_ridge(0, 0, 1, 0, 5, 0, 10, 5, 0) };
    ridges[0].type = 1;
    uint32_t* rW = (uint32_t*)calloc(1,4); rW[0]=0;
    uint32_t* rX = (uint32_t*)calloc(1,4); rX[0]=0;
    uint32_t* rZ = (uint32_t*)calloc(1,4); rZ[0]=0;
    uint32_t* pX  = (uint32_t*)calloc(2,4); pX[0]=0; pX[1]=1;   // X is shared {0,1}
    uint32_t* pW  = (uint32_t*)calloc(1,4); pW[0]=0;
    uint32_t* pZ  = (uint32_t*)calloc(1,4); pZ[0]=0;
    uint32_t* pF0 = (uint32_t*)calloc(1,4); pF0[0]=0;
    uint32_t* pP1 = (uint32_t*)calloc(1,4); pP1[0]=1;
    uint32_t* pP2 = (uint32_t*)calloc(1,4); pP2[0]=1;
    // ids: X=5 W=2 Z=8 F0=100 P1=201 P2=202
    v7_br_input_corner_t corners[6] = {
        mk_corner(5,  5, 5,  0, pX,  2, rX, 1),   // X  shared, on-R
        mk_corner(2,  2, 5,  0, pW,  1, rW, 1),   // W  plane0, on-R
        mk_corner(8,  8, 5,  0, pZ,  1, rZ, 1),   // Z  plane0, on-R
        mk_corner(100,5, 5,  3, pF0, 1, NULL, 0), // F0 plane0 filler, off-R
        mk_corner(201,7, 5, -3, pP1, 1, NULL, 0), // P1 plane1, off-R
        mk_corner(202,3, 5, -3, pP2, 1, NULL, 0), // P2 plane1, off-R
    };
    // plane0: [W, X, Z, F0] — X's neighbours W,Z both on-R → redundant here.
    // plane1: [X, P1, P2]   — X's neighbours P1,P2 off-R → NOT redundant here.
    uint32_t* path0 = (uint32_t*)calloc(4,4); path0[0]=2; path0[1]=5; path0[2]=8; path0[3]=100;
    uint32_t* path1 = (uint32_t*)calloc(3,4); path1[0]=5; path1[1]=201; path1[2]=202;
    v7_br_input_polygon_path_t paths[2] = {
        { .plane_id = 0, .corner_ids = path0, .n_corners = 4 },
        { .plane_id = 1, .corner_ids = path1, .n_corners = 3 },
    };
    int16_t owner_buf[1] = { -1 };
    float pos_buf[3] = { 0, 0, 0 };
    v7_br_params_t params; v7_br_params_default(&params);
    params.max_iterations = 0;
    v7_br_outputs_t out; v7_br_outputs_init(&out);
    int rc = v7_br_compute(planes, 2, corners, 6, NULL, 0, paths, 2, ridges, 1, NULL, 0,
                            pos_buf, 1, owner_buf, NULL, 0.12, &params, &out);
    CHECK(rc == 0);
    // Nothing dropped (plane-1 disagrees about X; W/Z bounded by off-R fillers).
    CHECK(out.stats.redundant_ridge_removed == 0);
    // X is KEPT in BOTH paths → planes_of(X) stays {0,1}.
    CHECK(emitted_has_corner(&out, 0, 5) == 1);
    CHECK(emitted_has_corner(&out, 1, 5) == 1);
    CHECK(emitted_n_plane_ids(&out, 5) == 2);
    v7_br_outputs_free(&out);
    free(rW); free(rX); free(rZ);
    free(pX); free(pW); free(pZ); free(pF0); free(pP1); free(pP2);
    free(path0); free(path1);
}

// (C) junction-kept: a corner carrying TWO ridge tags whose prev sits on one
// ridge and next on the other → no SINGLE ridge is shared by both neighbours →
// the share-ONE test fails → the junction is never dropped.
static void test_t6_junction_kept(void) {
    fprintf(stderr, "== test_t6_junction_kept\n");
    v7_br_input_plane_t planes[1] = { mk_flat_plane(0, 5.0) };
    // Ridge 0 along +X; ridge 1 along +Z; they cross at (5,5,0).
    v7_br_input_ridge_t ridges[2] = {
        mk_ridge(0, 0, 1, 0, 5, 0, 10, 5, 0),
        mk_ridge(1, 0, 2, 5, 5, -5, 5, 5, 5),
    };
    ridges[0].type = 1; ridges[1].type = 1;
    uint32_t* rJ  = (uint32_t*)calloc(2,4); rJ[0]=0; rJ[1]=1;   // junction on both
    uint32_t* rPx = (uint32_t*)calloc(1,4); rPx[0]=0;           // prev on ridge 0
    uint32_t* rPz = (uint32_t*)calloc(1,4); rPz[0]=1;           // next on ridge 1
    uint32_t* p0a = (uint32_t*)calloc(1,4); p0a[0]=0;
    uint32_t* p0b = (uint32_t*)calloc(1,4); p0b[0]=0;
    uint32_t* p0c = (uint32_t*)calloc(1,4); p0c[0]=0;
    uint32_t* p0d = (uint32_t*)calloc(1,4); p0d[0]=0;
    // ids: J=50 Px=3 Pz=53 F=80
    v7_br_input_corner_t corners[4] = {
        mk_corner(50, 5, 5,  0, p0a, 1, rJ,  2),  // junction {ridge0, ridge1}
        mk_corner(3,  3, 5,  0, p0b, 1, rPx, 1),  // prev: on ridge 0
        mk_corner(53, 5, 5,  3, p0c, 1, rPz, 1),  // next: on ridge 1
        mk_corner(80, 8, 5, -3, p0d, 1, NULL, 0), // off both
    };
    // path: [Px, J, Pz, F] — J's prev=Px(R0), next=Pz(R1): no common ridge.
    uint32_t* path0 = (uint32_t*)calloc(4,4); path0[0]=3; path0[1]=50; path0[2]=53; path0[3]=80;
    v7_br_input_polygon_path_t paths[1] = { { .plane_id = 0, .corner_ids = path0, .n_corners = 4 } };
    int16_t owner_buf[1] = { -1 };
    float pos_buf[3] = { 0, 0, 0 };
    v7_br_params_t params; v7_br_params_default(&params);
    params.max_iterations = 0;
    v7_br_outputs_t out; v7_br_outputs_init(&out);
    int rc = v7_br_compute(planes, 1, corners, 4, NULL, 0, paths, 1, ridges, 2, NULL, 0,
                            pos_buf, 1, owner_buf, NULL, 0.12, &params, &out);
    CHECK(rc == 0);
    CHECK(out.stats.redundant_ridge_removed == 0);   // share-ONE fails → no drop
    CHECK(emitted_has_corner(&out, 0, 50) == 1);     // junction kept
    v7_br_outputs_free(&out);
    free(rJ); free(rPx); free(rPz);
    free(p0a); free(p0b); free(p0c); free(p0d); free(path0);
}

// ─── T7: SPLIT_TO_SHARE_CORNER (the only GROWING op) ─────────────────────────
//
// SPLIT runs in CHASE_AFTER_MOVE on the corner that just moved (the event-driven
// cascade is local to the moved corner). So the candidate cA must itself be the
// per-iteration global-best move. The fixtures make cA the single movable corner
// (a single-plane on-ridge DOF-1 corner started slightly off its angle-optimal
// position so the optimizer slides it ALONG the ridge); after the slide the
// chase fires SPLIT(cA). The other corners are positioned at their angle-optimal
// so cA is the dominant mover.

// Shared fixture builder for the T7 positive tests: plane A (flat y=5) and B
// (slope) meet on ridge 0 along +X, t∈[0,10] through (x,5,0). cA (id 0) is a
// single-plane plane-A corner ON the ridge, pre-tagged R=0 → DOF-1. B has a
// STRAIGHT edge p(id 20)→q(id 21) along R (both tagged R) bracketing cA, with NO
// B corner near cA's tip. `b_extra` adds a 4th B corner to widen the B ring.

// (A) SPLIT forms a share: cA inserted into B's straight edge → cA becomes
// {A,B}, B's path grows by 1, cA's POSITION is unchanged, cA stays DOF-1.
static void test_t7_split_forms_share(void) {
    fprintf(stderr, "== test_t7_split_forms_share\n");
    v7_br_input_plane_t planes[2] = {
        mk_flat_plane(0, 5.0),
        mk_slope_plane(1, 0.0, 0.707, 0.707, -5.0),
    };
    v7_br_input_ridge_t ridges[1] = { mk_ridge(0, 0, 1, 0, 5, 0, 10, 5, 0) };
    ridges[0].type = 1;   // allow-listed
    uint32_t* rA  = (uint32_t*)calloc(1,4); rA[0]=0;   // cA pre-tagged R
    uint32_t* r20 = (uint32_t*)calloc(1,4); r20[0]=0;  // B edge endpoint p, on R
    uint32_t* r21 = (uint32_t*)calloc(1,4); r21[0]=0;  // B edge endpoint q, on R
    uint32_t* pA0=(uint32_t*)calloc(1,4); pA0[0]=0;
    uint32_t* pA1=(uint32_t*)calloc(1,4); pA1[0]=0;
    uint32_t* pA2=(uint32_t*)calloc(1,4); pA2[0]=0;
    uint32_t* pB0=(uint32_t*)calloc(1,4); pB0[0]=1;
    uint32_t* pB1=(uint32_t*)calloc(1,4); pB1[0]=1;
    uint32_t* pB2=(uint32_t*)calloc(1,4); pB2[0]=1;
    // cA starts slightly off-center (x=5.3) so its apex angle isn't optimal →
    // the optimizer slides it along R toward the symmetric apex (x≈5).
    v7_br_input_corner_t corners[6] = {
        mk_corner(0,  5.3, 5,  0.0, pA0, 1, rA, 1),   // cA: single-plane on R (DOF-1), the mover
        mk_corner(1,  2.0, 5,  3.0, pA1, 1, NULL, 0), // A base
        mk_corner(2,  8.0, 5,  3.0, pA2, 1, NULL, 0), // A base
        mk_corner(20, 1.0, 5,  0.0, pB0, 1, r20, 1),  // B edge p, on R, x=1
        mk_corner(21, 9.0, 5,  0.0, pB1, 1, r21, 1),  // B edge q, on R, x=9
        mk_corner(22, 5.0, 5, -4.0, pB2, 1, NULL, 0), // B off-R, no corner near cA
    };
    uint32_t* pathA=(uint32_t*)calloc(3,4); pathA[0]=0; pathA[1]=1; pathA[2]=2;
    uint32_t* pathB=(uint32_t*)calloc(3,4); pathB[0]=20; pathB[1]=21; pathB[2]=22;
    v7_br_input_polygon_path_t paths[2] = {
        { .plane_id = 0, .corner_ids = pathA, .n_corners = 3 },
        { .plane_id = 1, .corner_ids = pathB, .n_corners = 3 },
    };
    int16_t owner_buf[1] = { -1 };
    float pos_buf[3] = { 0, 0, 0 };
    v7_br_params_t params; v7_br_params_default(&params);
    params.max_iterations = 50;
    params.use_wsd_cost = false;        // angle/nbr drives the slide
    params.use_proximity_merge = true;
    params.use_collinear_removal = true;
    setenv("BR_SNAP_IN", "1", 1);
    v7_br_outputs_t out; v7_br_outputs_init(&out);
    int rc = v7_br_compute(planes, 2, corners, 6, NULL, 0, paths, 2, ridges, 1, NULL, 0,
                            pos_buf, 1, owner_buf, NULL, 0.12, &params, &out);
    CHECK(rc == 0);
    // A SPLIT fired (the real op count).
    CHECK(out.stats.n_split_share >= 1);
    // cA (id 0) is now in BOTH planes' emitted rings → shared {A,B}.
    CHECK(emitted_has_corner(&out, 0, 0) == 1);
    CHECK(emitted_has_corner(&out, 1, 0) == 1);
    CHECK(emitted_n_plane_ids(&out, 0) == 2);
    // cA inserted BETWEEN p(20) and q(21) in B's ring → both 20-0 and 0-21 are
    // edges (the original 20-21 edge is split by cA).
    CHECK(emitted_edge_present(&out, 1, 20, 0) == 1);
    CHECK(emitted_edge_present(&out, 1, 0, 21) == 1);
    // cA's POSITION stayed ON the ridge (perp ~0): y=5, z=0 unchanged (it slid
    // only in x along R). Find cA's emitted position.
    for (size_t i = 0; i < out.n_corner_positions; i++) {
        if (out.corner_positions[i].corner_id == 0) {
            CHECK_CLOSE(out.corner_positions[i].point[1], 5.0, 1e-6);   // y on ridge
            CHECK_CLOSE(out.corner_positions[i].point[2], 0.0, 1e-6);   // z on ridge
        }
    }
    v7_br_outputs_free(&out);
    free(rA); free(r20); free(r21);
    free(pA0);free(pA1);free(pA2);free(pB0);free(pB1);free(pB2);
    free(pathA);free(pathB);
    unsetenv("BR_SNAP_IN");
}

// (B) cascade doesn't eat it: after the split, DROP_COLINEAR (single-plane gate)
// must NOT remove cA — it is now shared {A,B}, out of DROP_COLINEAR's domain. cA
// is collinear in B's straight edge (it lies ON p→q) yet survives. (Same fixture
// as A, with collinear-removal explicitly on; assert cA still shared at the end.)
static void test_t7_cascade_keeps_shared(void) {
    fprintf(stderr, "== test_t7_cascade_keeps_shared\n");
    v7_br_input_plane_t planes[2] = {
        mk_flat_plane(0, 5.0),
        mk_slope_plane(1, 0.0, 0.707, 0.707, -5.0),
    };
    v7_br_input_ridge_t ridges[1] = { mk_ridge(0, 0, 1, 0, 5, 0, 10, 5, 0) };
    ridges[0].type = 1;
    uint32_t* rA  = (uint32_t*)calloc(1,4); rA[0]=0;
    uint32_t* r20 = (uint32_t*)calloc(1,4); r20[0]=0;
    uint32_t* r21 = (uint32_t*)calloc(1,4); r21[0]=0;
    uint32_t* pA0=(uint32_t*)calloc(1,4); pA0[0]=0;
    uint32_t* pA1=(uint32_t*)calloc(1,4); pA1[0]=0;
    uint32_t* pA2=(uint32_t*)calloc(1,4); pA2[0]=0;
    uint32_t* pB0=(uint32_t*)calloc(1,4); pB0[0]=1;
    uint32_t* pB1=(uint32_t*)calloc(1,4); pB1[0]=1;
    uint32_t* pB2=(uint32_t*)calloc(1,4); pB2[0]=1;
    v7_br_input_corner_t corners[6] = {
        mk_corner(0,  5.3, 5,  0.0, pA0, 1, rA, 1),
        mk_corner(1,  2.0, 5,  3.0, pA1, 1, NULL, 0),
        mk_corner(2,  8.0, 5,  3.0, pA2, 1, NULL, 0),
        mk_corner(20, 1.0, 5,  0.0, pB0, 1, r20, 1),
        mk_corner(21, 9.0, 5,  0.0, pB1, 1, r21, 1),
        mk_corner(22, 5.0, 5, -4.0, pB2, 1, NULL, 0),
    };
    uint32_t* pathA=(uint32_t*)calloc(3,4); pathA[0]=0; pathA[1]=1; pathA[2]=2;
    uint32_t* pathB=(uint32_t*)calloc(3,4); pathB[0]=20; pathB[1]=21; pathB[2]=22;
    v7_br_input_polygon_path_t paths[2] = {
        { .plane_id = 0, .corner_ids = pathA, .n_corners = 3 },
        { .plane_id = 1, .corner_ids = pathB, .n_corners = 3 },
    };
    int16_t owner_buf[1] = { -1 };
    float pos_buf[3] = { 0, 0, 0 };
    v7_br_params_t params; v7_br_params_default(&params);
    params.max_iterations = 80;
    params.use_wsd_cost = false;
    params.use_collinear_removal = true;   // the op that would (wrongly) eat cA
    params.use_proximity_merge = true;
    setenv("BR_SNAP_IN", "1", 1);
    v7_br_outputs_t out; v7_br_outputs_init(&out);
    int rc = v7_br_compute(planes, 2, corners, 6, NULL, 0, paths, 2, ridges, 1, NULL, 0,
                            pos_buf, 1, owner_buf, NULL, 0.12, &params, &out);
    CHECK(rc == 0);
    CHECK(out.stats.n_split_share >= 1);
    // cA survived the full cascade as a shared corner (DROP_COLINEAR skipped it).
    CHECK(emitted_has_corner(&out, 0, 0) == 1);
    CHECK(emitted_has_corner(&out, 1, 0) == 1);
    CHECK(emitted_n_plane_ids(&out, 0) == 2);
    v7_br_outputs_free(&out);
    free(rA); free(r20); free(r21);
    free(pA0);free(pA1);free(pA2);free(pB0);free(pB1);free(pB2);
    free(pathA);free(pathB);
    unsetenv("BR_SNAP_IN");
}

// (D) GUARD / REFUSE: cond-A fails when a bracketing B-edge has an endpoint OFF
// ridge R (a void/U-shape). cA brackets B's edge p→q on the ridge-LINE arc, but
// q is NOT tagged R → the both-R test fails → NO split (cA stays single-plane).
static void test_t7_guard_off_ridge_endpoint_refuses(void) {
    fprintf(stderr, "== test_t7_guard_off_ridge_endpoint_refuses\n");
    v7_br_input_plane_t planes[2] = {
        mk_flat_plane(0, 5.0),
        mk_slope_plane(1, 0.0, 0.707, 0.707, -5.0),
    };
    v7_br_input_ridge_t ridges[1] = { mk_ridge(0, 0, 1, 0, 5, 0, 10, 5, 0) };
    ridges[0].type = 1;
    uint32_t* rA  = (uint32_t*)calloc(1,4); rA[0]=0;
    uint32_t* r20 = (uint32_t*)calloc(1,4); r20[0]=0;   // p tagged R
    // q (id 21) carries NO ridge tag → the both-R edge test fails.
    uint32_t* pA0=(uint32_t*)calloc(1,4); pA0[0]=0;
    uint32_t* pA1=(uint32_t*)calloc(1,4); pA1[0]=0;
    uint32_t* pA2=(uint32_t*)calloc(1,4); pA2[0]=0;
    uint32_t* pB0=(uint32_t*)calloc(1,4); pB0[0]=1;
    uint32_t* pB1=(uint32_t*)calloc(1,4); pB1[0]=1;
    uint32_t* pB2=(uint32_t*)calloc(1,4); pB2[0]=1;
    v7_br_input_corner_t corners[6] = {
        mk_corner(0,  5.3, 5,  0.0, pA0, 1, rA, 1),
        mk_corner(1,  2.0, 5,  3.0, pA1, 1, NULL, 0),
        mk_corner(2,  8.0, 5,  3.0, pA2, 1, NULL, 0),
        mk_corner(20, 1.0, 5,  0.0, pB0, 1, r20, 1),    // p on R
        mk_corner(21, 9.0, 5,  0.0, pB1, 1, NULL, 0),   // q NOT tagged R (off-ridge endpoint)
        mk_corner(22, 5.0, 5, -4.0, pB2, 1, NULL, 0),
    };
    uint32_t* pathA=(uint32_t*)calloc(3,4); pathA[0]=0; pathA[1]=1; pathA[2]=2;
    uint32_t* pathB=(uint32_t*)calloc(3,4); pathB[0]=20; pathB[1]=21; pathB[2]=22;
    v7_br_input_polygon_path_t paths[2] = {
        { .plane_id = 0, .corner_ids = pathA, .n_corners = 3 },
        { .plane_id = 1, .corner_ids = pathB, .n_corners = 3 },
    };
    int16_t owner_buf[1] = { -1 };
    float pos_buf[3] = { 0, 0, 0 };
    v7_br_params_t params; v7_br_params_default(&params);
    params.max_iterations = 50;
    params.use_wsd_cost = false;
    setenv("BR_SNAP_IN", "1", 1);
    v7_br_outputs_t out; v7_br_outputs_init(&out);
    int rc = v7_br_compute(planes, 2, corners, 6, NULL, 0, paths, 2, ridges, 1, NULL, 0,
                            pos_buf, 1, owner_buf, NULL, 0.12, &params, &out);
    CHECK(rc == 0);
    CHECK(out.stats.n_split_share == 0);          // off-ridge endpoint → cond-A fails
    CHECK(emitted_n_plane_ids(&out, 0) == 1);      // cA stays single-plane
    CHECK(emitted_has_corner(&out, 1, 0) == 0);    // cA NOT in B's ring
    v7_br_outputs_free(&out);
    free(rA); free(r20);
    free(pA0);free(pA1);free(pA2);free(pB0);free(pB1);free(pB2);
    free(pathA);free(pathB);
    unsetenv("BR_SNAP_IN");
}

// (D2) folded-ridge TIE → REFUSE. cA brackets TWO both-R B edges at (nearly) the
// same 3D perp distance → the op cannot tell which arm cA belongs to → it
// REFUSES (sp_tie_refuse) rather than guessing. We assert the SPLIT did not fire.
// Two B rings (two plane-1 paths) each present a both-R edge equidistant from cA.
static void test_t7_folded_ridge_tie_refuses(void) {
    fprintf(stderr, "== test_t7_folded_ridge_tie_refuses\n");
    v7_br_input_plane_t planes[2] = {
        mk_flat_plane(0, 5.0),
        mk_slope_plane(1, 0.0, 0.707, 0.707, -5.0),
    };
    v7_br_input_ridge_t ridges[1] = { mk_ridge(0, 0, 1, 0, 5, 0, 10, 5, 0) };
    ridges[0].type = 1;
    uint32_t* rA  = (uint32_t*)calloc(1,4); rA[0]=0;
    uint32_t* r20 = (uint32_t*)calloc(1,4); r20[0]=0;
    uint32_t* r21 = (uint32_t*)calloc(1,4); r21[0]=0;
    uint32_t* r30 = (uint32_t*)calloc(1,4); r30[0]=0;
    uint32_t* r31 = (uint32_t*)calloc(1,4); r31[0]=0;
    uint32_t* pA0=(uint32_t*)calloc(1,4); pA0[0]=0;
    uint32_t* pA1=(uint32_t*)calloc(1,4); pA1[0]=0;
    uint32_t* pA2=(uint32_t*)calloc(1,4); pA2[0]=0;
    uint32_t* pB0=(uint32_t*)calloc(1,4); pB0[0]=1;
    uint32_t* pB1=(uint32_t*)calloc(1,4); pB1[0]=1;
    uint32_t* pB2=(uint32_t*)calloc(1,4); pB2[0]=1;
    uint32_t* pC0=(uint32_t*)calloc(1,4); pC0[0]=1;
    uint32_t* pC1=(uint32_t*)calloc(1,4); pC1[0]=1;
    uint32_t* pC2=(uint32_t*)calloc(1,4); pC2[0]=1;
    // Two plane-1 rings, each with a both-R edge ALONG the same ridge line
    // bracketing cA at the same perp distance (both edges sit ON the ridge, d≈0).
    v7_br_input_corner_t corners[9] = {
        mk_corner(0,  6.5, 5,  0.0, pA0, 1, rA, 1),     // cA mover (off apex x=5)
        mk_corner(1,  2.0, 5,  3.0, pA1, 1, NULL, 0),
        mk_corner(2,  8.0, 5,  3.0, pA2, 1, NULL, 0),
        mk_corner(20, 1.0, 5,  0.0, pB0, 1, r20, 1),    // ring B edge p
        mk_corner(21, 9.0, 5,  0.0, pB1, 1, r21, 1),    // ring B edge q
        mk_corner(22, 5.0, 5, -4.0, pB2, 1, NULL, 0),
        mk_corner(30, 0.4, 5,  0.0, pC0, 1, r30, 1),    // ring C edge p' (also on R)
        mk_corner(31, 9.6, 5,  0.0, pC1, 1, r31, 1),    // ring C edge q' (also on R)
        mk_corner(32, 5.0, 5,  4.0, pC2, 1, NULL, 0),
    };
    uint32_t* pathA=(uint32_t*)calloc(3,4); pathA[0]=0; pathA[1]=1; pathA[2]=2;
    uint32_t* pathB=(uint32_t*)calloc(3,4); pathB[0]=20; pathB[1]=21; pathB[2]=22;
    uint32_t* pathC=(uint32_t*)calloc(3,4); pathC[0]=30; pathC[1]=31; pathC[2]=32;
    v7_br_input_polygon_path_t paths[3] = {
        { .plane_id = 0, .corner_ids = pathA, .n_corners = 3 },
        { .plane_id = 1, .corner_ids = pathB, .n_corners = 3 },
        { .plane_id = 1, .corner_ids = pathC, .n_corners = 3 },
    };
    int16_t owner_buf[1] = { -1 };
    float pos_buf[3] = { 0, 0, 0 };
    v7_br_params_t params; v7_br_params_default(&params);
    params.max_iterations = 50;
    params.use_wsd_cost = false;
    // Disable the proximity merge + collinear removal so the two B rings stay
    // distinct (otherwise their near-coincident on-R endpoints weld and the tie
    // never materialises) — we want SPLIT to SEE two equidistant both-R edges.
    params.use_proximity_merge = false;
    params.use_collinear_removal = false;
    setenv("BR_SNAP_IN", "1", 1);
    v7_br_outputs_t out; v7_br_outputs_init(&out);
    int rc = v7_br_compute(planes, 2, corners, 9, NULL, 0, paths, 3, ridges, 1, NULL, 0,
                            pos_buf, 1, owner_buf, NULL, 0.12, &params, &out);
    CHECK(rc == 0);
    // Tie → REFUSE: no split committed (sp_tie_refuse fired), cA single-plane.
    CHECK(out.stats.n_split_share == 0);
    CHECK(emitted_n_plane_ids(&out, 0) == 1);
    v7_br_outputs_free(&out);
    free(rA); free(r20); free(r21); free(r30); free(r31);
    free(pA0);free(pA1);free(pA2);free(pB0);free(pB1);free(pB2);
    free(pC0);free(pC1);free(pC2);
    free(pathA);free(pathB);free(pathC);
    unsetenv("BR_SNAP_IN");
}

// (C) edge-forming: cA splits onto B's straight edge p→q → the single B edge
// p→q is REPLACED by the two-segment chain p→cA→q with cA a SHARED {A,B} vertex.
// This is the edge-forming step: the split subdivides B's edge and plants the
// shared corner that a forming shared edge runs through. We assert the structural
// outcome deterministically (the §4.4 FURTHER collapse to one shared edge — which
// needs cA's A-neighbours ALSO on R — is proven separately by
// test_t6_shared_edge_formation; here cA's A-neighbours are off-R so cA is NOT
// pre-loop-redundant in A, which is exactly what lets the SPLIT run at all).
// Uses fixture A's symmetric-triangle driver so cA reliably slides + splits.
static void test_t7_split_forms_shared_edge(void) {
    fprintf(stderr, "== test_t7_split_forms_shared_edge\n");
    v7_br_input_plane_t planes[2] = {
        mk_flat_plane(0, 5.0),
        mk_slope_plane(1, 0.0, 0.707, 0.707, -5.0),
    };
    v7_br_input_ridge_t ridges[1] = { mk_ridge(0, 0, 1, 0, 5, 0, 10, 5, 0) };
    ridges[0].type = 1;
    uint32_t* rA  = (uint32_t*)calloc(1,4); rA[0]=0;
    uint32_t* r20 = (uint32_t*)calloc(1,4); r20[0]=0;
    uint32_t* r21 = (uint32_t*)calloc(1,4); r21[0]=0;
    uint32_t* pA0=(uint32_t*)calloc(1,4); pA0[0]=0;
    uint32_t* pA1=(uint32_t*)calloc(1,4); pA1[0]=0;
    uint32_t* pA2=(uint32_t*)calloc(1,4); pA2[0]=0;
    uint32_t* pB0=(uint32_t*)calloc(1,4); pB0[0]=1;
    uint32_t* pB1=(uint32_t*)calloc(1,4); pB1[0]=1;
    uint32_t* pB2=(uint32_t*)calloc(1,4); pB2[0]=1;
    // cA's A-neighbours are the LIFTED bases (off-R) so cA is not pre-loop
    // redundant in A; B's edge p(20)→q(21) is on R and brackets cA.
    v7_br_input_corner_t corners[6] = {
        mk_corner(0,  5.3, 5,  0.0, pA0, 1, rA, 1),   // cA mover (off apex)
        mk_corner(1,  2.0, 5,  3.0, pA1, 1, NULL, 0), // A base off-R
        mk_corner(2,  8.0, 5,  3.0, pA2, 1, NULL, 0), // A base off-R
        mk_corner(20, 1.0, 5,  0.0, pB0, 1, r20, 1),  // B edge p on R
        mk_corner(21, 9.0, 5,  0.0, pB1, 1, r21, 1),  // B edge q on R
        mk_corner(22, 5.0, 5, -4.0, pB2, 1, NULL, 0), // B off-R filler
    };
    uint32_t* pathA=(uint32_t*)calloc(3,4); pathA[0]=0; pathA[1]=1; pathA[2]=2;
    uint32_t* pathB=(uint32_t*)calloc(3,4); pathB[0]=20; pathB[1]=21; pathB[2]=22;
    v7_br_input_polygon_path_t paths[2] = {
        { .plane_id = 0, .corner_ids = pathA, .n_corners = 3 },
        { .plane_id = 1, .corner_ids = pathB, .n_corners = 3 },
    };
    int16_t owner_buf[1] = { -1 };
    float pos_buf[3] = { 0, 0, 0 };
    v7_br_params_t params; v7_br_params_default(&params);
    params.max_iterations = 50;
    params.use_wsd_cost = false;
    params.use_proximity_merge = true;
    params.use_collinear_removal = true;
    setenv("BR_SNAP_IN", "1", 1);
    v7_br_outputs_t out; v7_br_outputs_init(&out);
    int rc = v7_br_compute(planes, 2, corners, 6, NULL, 0, paths, 2, ridges, 1, NULL, 0,
                            pos_buf, 1, owner_buf, NULL, 0.12, &params, &out);
    CHECK(rc == 0);
    CHECK(out.stats.n_split_share >= 1);
    // B's single edge p→q is now the chain p→cA→q (cA shared, the edge-forming
    // subdivision); the original direct p(20)→q(21) edge is gone.
    CHECK(emitted_n_plane_ids(&out, 0) == 2);          // cA shared {A,B}
    CHECK(emitted_edge_present(&out, 1, 20, 0) == 1);  // p→cA
    CHECK(emitted_edge_present(&out, 1, 0, 21) == 1);  // cA→q
    CHECK(emitted_edge_present(&out, 1, 20, 21) == 0); // direct p→q replaced
    v7_br_outputs_free(&out);
    free(rA); free(r20); free(r21);
    free(pA0);free(pA1);free(pA2);free(pB0);free(pB1);free(pB2);
    free(pathA);free(pathB);
    unsetenv("BR_SNAP_IN");
}

// (E) GROWTH under ASan + OOM reject. Force polys[B].ids AND ctxs[] to grow by
// running the SPLIT on a SMALL B ring whose .cap == .n (so the insert needs a
// realloc). Built/run under ASan (build-dev) → MUST be heap-clean. The OOM half:
// with BR_TEST_RESERVE_FAIL the reserves are treated as failed → clean REJECT,
// nothing mutated (cA stays single-plane, B's ring unchanged).
static void test_t7_growth_and_oom(void) {
    fprintf(stderr, "== test_t7_growth_and_oom\n");
    // ── OOM half first: identical geometry to (A) but BR_TEST_RESERVE_FAIL on. ──
    {
        v7_br_input_plane_t planes[2] = {
            mk_flat_plane(0, 5.0),
            mk_slope_plane(1, 0.0, 0.707, 0.707, -5.0),
        };
        v7_br_input_ridge_t ridges[1] = { mk_ridge(0, 0, 1, 0, 5, 0, 10, 5, 0) };
        ridges[0].type = 1;
        uint32_t* rA  = (uint32_t*)calloc(1,4); rA[0]=0;
        uint32_t* r20 = (uint32_t*)calloc(1,4); r20[0]=0;
        uint32_t* r21 = (uint32_t*)calloc(1,4); r21[0]=0;
        uint32_t* pA0=(uint32_t*)calloc(1,4); pA0[0]=0;
        uint32_t* pA1=(uint32_t*)calloc(1,4); pA1[0]=0;
        uint32_t* pA2=(uint32_t*)calloc(1,4); pA2[0]=0;
        uint32_t* pB0=(uint32_t*)calloc(1,4); pB0[0]=1;
        uint32_t* pB1=(uint32_t*)calloc(1,4); pB1[0]=1;
        uint32_t* pB2=(uint32_t*)calloc(1,4); pB2[0]=1;
        v7_br_input_corner_t corners[6] = {
            mk_corner(0,  5.3, 5,  0.0, pA0, 1, rA, 1),
            mk_corner(1,  2.0, 5,  3.0, pA1, 1, NULL, 0),
            mk_corner(2,  8.0, 5,  3.0, pA2, 1, NULL, 0),
            mk_corner(20, 1.0, 5,  0.0, pB0, 1, r20, 1),
            mk_corner(21, 9.0, 5,  0.0, pB1, 1, r21, 1),
            mk_corner(22, 5.0, 5, -4.0, pB2, 1, NULL, 0),
        };
        uint32_t* pathA=(uint32_t*)calloc(3,4); pathA[0]=0; pathA[1]=1; pathA[2]=2;
        uint32_t* pathB=(uint32_t*)calloc(3,4); pathB[0]=20; pathB[1]=21; pathB[2]=22;
        v7_br_input_polygon_path_t paths[2] = {
            { .plane_id = 0, .corner_ids = pathA, .n_corners = 3 },
            { .plane_id = 1, .corner_ids = pathB, .n_corners = 3 },
        };
        int16_t owner_buf[1] = { -1 };
        float pos_buf[3] = { 0, 0, 0 };
        v7_br_params_t params; v7_br_params_default(&params);
        params.max_iterations = 50;
        params.use_wsd_cost = false;
        setenv("BR_SNAP_IN", "1", 1);
        setenv("BR_TEST_RESERVE_FAIL", "1", 1);
        v7_br_outputs_t out; v7_br_outputs_init(&out);
        int rc = v7_br_compute(planes, 2, corners, 6, NULL, 0, paths, 2, ridges, 1, NULL, 0,
                                pos_buf, 1, owner_buf, NULL, 0.12, &params, &out);
        CHECK(rc == 0);                                // OOM reject is clean, not fatal
        CHECK(out.stats.n_split_share == 0);           // nothing committed
        CHECK(emitted_n_plane_ids(&out, 0) == 1);      // cA still single-plane
        CHECK(emitted_has_corner(&out, 1, 0) == 0);    // B's ring unchanged (no cA)
        v7_br_outputs_free(&out);
        free(rA); free(r20); free(r21);
        free(pA0);free(pA1);free(pA2);free(pB0);free(pB1);free(pB2);
        free(pathA);free(pathB);
        unsetenv("BR_TEST_RESERVE_FAIL");
        unsetenv("BR_SNAP_IN");
    }
    // ── GROWTH half: BR_TEST_RESERVE_FAIL OFF → the SPLIT actually grows the
    //    pools (poly_reserve: B's cap 3→ >=4; ctx_reserve as needed). ASan is the
    //    catch-net for any heap overflow in the memmove / REBUILD_CTXS writes. ──
    {
        v7_br_input_plane_t planes[2] = {
            mk_flat_plane(0, 5.0),
            mk_slope_plane(1, 0.0, 0.707, 0.707, -5.0),
        };
        v7_br_input_ridge_t ridges[1] = { mk_ridge(0, 0, 1, 0, 5, 0, 10, 5, 0) };
        ridges[0].type = 1;
        uint32_t* rA  = (uint32_t*)calloc(1,4); rA[0]=0;
        uint32_t* r20 = (uint32_t*)calloc(1,4); r20[0]=0;
        uint32_t* r21 = (uint32_t*)calloc(1,4); r21[0]=0;
        uint32_t* pA0=(uint32_t*)calloc(1,4); pA0[0]=0;
        uint32_t* pA1=(uint32_t*)calloc(1,4); pA1[0]=0;
        uint32_t* pA2=(uint32_t*)calloc(1,4); pA2[0]=0;
        uint32_t* pB0=(uint32_t*)calloc(1,4); pB0[0]=1;
        uint32_t* pB1=(uint32_t*)calloc(1,4); pB1[0]=1;
        uint32_t* pB2=(uint32_t*)calloc(1,4); pB2[0]=1;
        v7_br_input_corner_t corners[6] = {
            mk_corner(0,  5.3, 5,  0.0, pA0, 1, rA, 1),
            mk_corner(1,  2.0, 5,  3.0, pA1, 1, NULL, 0),
            mk_corner(2,  8.0, 5,  3.0, pA2, 1, NULL, 0),
            mk_corner(20, 1.0, 5,  0.0, pB0, 1, r20, 1),
            mk_corner(21, 9.0, 5,  0.0, pB1, 1, r21, 1),
            mk_corner(22, 5.0, 5, -4.0, pB2, 1, NULL, 0),
        };
        uint32_t* pathA=(uint32_t*)calloc(3,4); pathA[0]=0; pathA[1]=1; pathA[2]=2;
        uint32_t* pathB=(uint32_t*)calloc(3,4); pathB[0]=20; pathB[1]=21; pathB[2]=22;
        v7_br_input_polygon_path_t paths[2] = {
            { .plane_id = 0, .corner_ids = pathA, .n_corners = 3 },
            { .plane_id = 1, .corner_ids = pathB, .n_corners = 3 },
        };
        int16_t owner_buf[1] = { -1 };
        float pos_buf[3] = { 0, 0, 0 };
        v7_br_params_t params; v7_br_params_default(&params);
        params.max_iterations = 50;
        params.use_wsd_cost = false;
        setenv("BR_SNAP_IN", "1", 1);
        v7_br_outputs_t out; v7_br_outputs_init(&out);
        int rc = v7_br_compute(planes, 2, corners, 6, NULL, 0, paths, 2, ridges, 1, NULL, 0,
                                pos_buf, 1, owner_buf, NULL, 0.12, &params, &out);
        CHECK(rc == 0);
        CHECK(out.stats.n_split_share >= 1);           // grew + committed
        CHECK(emitted_n_plane_ids(&out, 0) == 2);      // cA shared after growth
        v7_br_outputs_free(&out);
        free(rA); free(r20); free(r21);
        free(pA0);free(pA1);free(pA2);free(pB0);free(pB1);free(pB2);
        free(pathA);free(pathB);
        unsetenv("BR_SNAP_IN");
    }
}

int main(void) {
    test_null_params_rejected();
    test_all_null_zero_counts_noop();
    test_zero_corners_no_work();
    test_dof_tagging();
    test_dof1_ridge_axis();
    test_r0b_unused_corner();
    test_r0b_redundant_ridge_midpoint();
    test_r1_locked_corners_stay();
    test_r1_converges();
    test_joint_moves_seed_deterministic();
    test_a2_single_plane_on_ridge_dof1();
    test_a2_junction_stays_dof0();
    test_f2_coplanar_ridge_not_counted_for_dof();
    test_a2_degenerate_ridge_ignored();
    test_a2_no_nan();
    test_a2_gate_off_keeps_plane_dof();
    test_t4_snap_in_free_to_ridge();
    test_t4_snap_in_off_stays_free();
    test_t4_guard_condA_bracket();
    test_t4_guard_step_ridge_not_shareable();
    test_t5_merge_corner_xplane();
    test_t5_merge_corner_step_gated();
    test_t5_merge_self_int_guard();
    test_t6_shared_edge_formation();
    test_t6_all_polys_agree_keeps_shared();
    test_t6_junction_kept();
    test_t7_split_forms_share();
    test_t7_cascade_keeps_shared();
    test_t7_split_forms_shared_edge();
    test_t7_guard_off_ridge_endpoint_refuses();
    test_t7_folded_ridge_tie_refuses();
    test_t7_growth_and_oom();
    fprintf(stderr, "\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}

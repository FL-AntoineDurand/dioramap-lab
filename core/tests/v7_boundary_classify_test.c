// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// core/tests/v7_boundary_classify_test.c
//
// Implementation tests for v7-boundary-classify. Each test asserts a
// spec invariant from bench-v2 lib/pipeline/steps/boundary-classify.js
// (meta.version=5) — Phase F perimeter classification + level-transition
// detection + PIP overhang fallback.
//
// The impl exposes a single entry point (v7_bclass_compute) operating
// on hand-crafted plane/corner/edge/polygon_path/ridge inputs. We don't
// inline its source — the compute function and its inner helpers are
// reachable through the public header.

#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Include the impl source directly so the test TU owns the compute
// symbols (mirrors v7_boundary_corners_test.c — Don't link
// dioramap_steps_v7, that would duplicate the impl's public symbols).
#include "v7-boundary-classify-impl.c"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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

// ─── helpers to build minimal inputs ────────────────────────────────

// Build a flat roof plane at y = h, with id `pid` and a square polygon
// path of side `side` centered at (cx, cz). normal = (0, 1, 0), d = -h.
static void make_roof_plane(v7_bclass_input_plane_t* P, uint32_t pid,
                              double h) {
    memset(P, 0, sizeof(*P));
    P->id = pid;
    P->rejected = false;
    P->tilt_deg = 0.0;
    P->normal[0] = 0.0; P->normal[1] = 1.0; P->normal[2] = 0.0;
    P->d = -h;
}

static void make_wall_plane(v7_bclass_input_plane_t* P, uint32_t pid) {
    memset(P, 0, sizeof(*P));
    P->id = pid;
    P->rejected = false;
    P->tilt_deg = 90.0;
    P->normal[0] = 1.0; P->normal[1] = 0.0; P->normal[2] = 0.0;
    P->d = 0.0;
}

static v7_bclass_input_corner_t mk_corner(uint32_t id, double x, double y, double z) {
    v7_bclass_input_corner_t c;
    memset(&c, 0, sizeof(c));
    c.id = id;
    c.point[0] = x; c.point[1] = y; c.point[2] = z;
    return c;
}

static v7_bclass_input_edge_t mk_edge(uint32_t id, uint32_t ca, uint32_t cb,
                                       uint32_t* plane_ids_owned, uint32_t n_planes) {
    v7_bclass_input_edge_t e;
    memset(&e, 0, sizeof(e));
    e.id = id; e.corner_a = ca; e.corner_b = cb;
    e.plane_ids = plane_ids_owned;
    e.n_plane_ids = n_planes;
    return e;
}

// Find output edge by id.
static const v7_bclass_edge_t* find_out_edge(const v7_bclass_outputs_t* out, uint32_t id) {
    for (size_t i = 0; i < out->n_edges; i++) {
        if (out->edges[i].edge_id == id) return &out->edges[i];
    }
    return NULL;
}

// ─── tests ──────────────────────────────────────────────────────────

// Wall-only edge → tag = wall.
static void test_wall_only_edge_tagged_wall(void) {
    fprintf(stderr, "== test_wall_only_edge_tagged_wall\n");
    v7_bclass_input_plane_t planes[1];
    make_wall_plane(&planes[0], 10);
    v7_bclass_input_corner_t corners[2] = {
        mk_corner(0, 0.0, 0.0, 0.0),
        mk_corner(1, 1.0, 0.0, 0.0),
    };
    uint32_t* pids = (uint32_t*)malloc(sizeof(uint32_t));
    pids[0] = 10;
    v7_bclass_input_edge_t edges[1] = { mk_edge(100, 0, 1, pids, 1) };

    v7_bclass_params_t params; v7_bclass_params_default(&params); params.up_axis = 1;
    v7_bclass_outputs_t out; v7_bclass_outputs_init(&out);
    int rc = v7_bclass_compute(planes, 1, corners, 2, edges, 1, NULL, 0, NULL, 0, &params, &out);
    CHECK(rc == 0);
    const v7_bclass_edge_t* e = find_out_edge(&out, 100);
    CHECK(e != NULL);
    if (e) CHECK(e->tag == V7_BCLASS_TAG_WALL);
    CHECK(out.stats.wall == 1);

    v7_bclass_outputs_free(&out);
    free(pids);
}

// Multi-plane all-roof edge (no walls) → tag = interior-shared.
static void test_multi_plane_no_walls_interior_shared(void) {
    fprintf(stderr, "== test_multi_plane_no_walls_interior_shared\n");
    v7_bclass_input_plane_t planes[2];
    make_roof_plane(&planes[0], 1, 5.0);
    make_roof_plane(&planes[1], 2, 5.0);
    v7_bclass_input_corner_t corners[2] = {
        mk_corner(0, 0.0, 5.0, 0.0),
        mk_corner(1, 1.0, 5.0, 0.0),
    };
    uint32_t* pids = (uint32_t*)malloc(2 * sizeof(uint32_t));
    pids[0] = 1; pids[1] = 2;
    v7_bclass_input_edge_t edges[1] = { mk_edge(100, 0, 1, pids, 2) };

    v7_bclass_params_t params; v7_bclass_params_default(&params); params.up_axis = 1;
    v7_bclass_outputs_t out; v7_bclass_outputs_init(&out);
    int rc = v7_bclass_compute(planes, 2, corners, 2, edges, 1, NULL, 0, NULL, 0, &params, &out);
    CHECK(rc == 0);
    const v7_bclass_edge_t* e = find_out_edge(&out, 100);
    CHECK(e != NULL);
    if (e) CHECK(e->tag == V7_BCLASS_TAG_INTERIOR_SHARED);
    CHECK(out.stats.interior_shared == 1);
    CHECK(out.stats.wall == 0);
    v7_bclass_outputs_free(&out);
    free(pids);
}

// Edge whose corners share a ridge between two roof planes → tag = interior-shared
// (even if the edge's own planeIds only carry one of them).
static void test_ridge_shared_corners_interior_shared(void) {
    fprintf(stderr, "== test_ridge_shared_corners_interior_shared\n");
    v7_bclass_input_plane_t planes[2];
    make_roof_plane(&planes[0], 1, 5.0);
    make_roof_plane(&planes[1], 2, 5.0);

    v7_bclass_input_corner_t corners[2];
    corners[0] = mk_corner(0, 0.0, 5.0, 0.0);
    corners[1] = mk_corner(1, 1.0, 5.0, 0.0);
    // Both corners share ridge 42.
    uint32_t* r0 = (uint32_t*)malloc(sizeof(uint32_t)); r0[0] = 42;
    uint32_t* r1 = (uint32_t*)malloc(sizeof(uint32_t)); r1[0] = 42;
    corners[0].ridge_ids = r0; corners[0].n_ridge_ids = 1;
    corners[1].ridge_ids = r1; corners[1].n_ridge_ids = 1;

    uint32_t* pids = (uint32_t*)malloc(sizeof(uint32_t));
    pids[0] = 1;  // edge says single-plane, but ridge gate should override
    v7_bclass_input_edge_t edges[1] = { mk_edge(100, 0, 1, pids, 1) };

    v7_bclass_input_ridge_t ridges[1] = { { .id = 42, .plane_a = 1, .plane_b = 2 } };

    v7_bclass_params_t params; v7_bclass_params_default(&params); params.up_axis = 1;
    v7_bclass_outputs_t out; v7_bclass_outputs_init(&out);
    int rc = v7_bclass_compute(planes, 2, corners, 2, edges, 1, NULL, 0, ridges, 1, &params, &out);
    CHECK(rc == 0);
    const v7_bclass_edge_t* e = find_out_edge(&out, 100);
    CHECK(e != NULL);
    if (e) CHECK(e->tag == V7_BCLASS_TAG_INTERIOR_SHARED);
    CHECK(out.stats.interior_shared == 1);
    v7_bclass_outputs_free(&out);
    free(pids); free(r0); free(r1);
}

// Lone single-plane roof edge with no candidates → tag = perimeter, outwardXZ set.
static void test_lone_single_plane_roof_perimeter(void) {
    fprintf(stderr, "== test_lone_single_plane_roof_perimeter\n");
    v7_bclass_input_plane_t planes[1];
    make_roof_plane(&planes[0], 1, 5.0);

    // Square polygon path so centroid is well-defined, edge sits on the
    // y=5 plane.
    v7_bclass_input_corner_t corners[4];
    corners[0] = mk_corner(0, 0.0, 5.0, 0.0);
    corners[1] = mk_corner(1, 4.0, 5.0, 0.0);
    corners[2] = mk_corner(2, 4.0, 5.0, 4.0);
    corners[3] = mk_corner(3, 0.0, 5.0, 4.0);

    uint32_t* pids = (uint32_t*)malloc(sizeof(uint32_t));
    pids[0] = 1;
    v7_bclass_input_edge_t edges[1] = { mk_edge(100, 0, 1, pids, 1) };  // bottom edge

    // Polygon path so winding + centroid get computed.
    uint32_t* path = (uint32_t*)malloc(4 * sizeof(uint32_t));
    path[0] = 0; path[1] = 1; path[2] = 2; path[3] = 3;
    v7_bclass_input_polygon_path_t paths[1] = { { .plane_id = 1, .corner_ids = path, .n_corners = 4 } };

    v7_bclass_params_t params; v7_bclass_params_default(&params); params.up_axis = 1;
    v7_bclass_outputs_t out; v7_bclass_outputs_init(&out);
    int rc = v7_bclass_compute(planes, 1, corners, 4, edges, 1, paths, 1, NULL, 0, &params, &out);
    CHECK(rc == 0);
    const v7_bclass_edge_t* e = find_out_edge(&out, 100);
    CHECK(e != NULL);
    if (e) {
        CHECK(e->tag == V7_BCLASS_TAG_PERIMETER);
        CHECK(e->has_outward_xz);
        // Edge runs +X along z=0, centroid sits at z=2. Outward perpendicular
        // should point away from centroid: -z direction.
        CHECK_CLOSE(e->outward_xz[0],  0.0, 1e-5);
        CHECK_CLOSE(e->outward_xz[1], -1.0, 1e-5);
    }
    CHECK(out.stats.perimeter == 1);
    v7_bclass_outputs_free(&out);
    free(pids); free(path);
}

// Two single-plane parallel edges on DIFFERENT roof planes at DIFFERENT
// heights, overlapping in XZ → overhang. Top vs bottom decided by midpoint y.
static void test_overhang_top_xz_collinear_close(void) {
    fprintf(stderr, "== test_overhang_top_xz_collinear_close\n");
    v7_bclass_input_plane_t planes[2];
    make_roof_plane(&planes[0], 1, 5.0);   // upper roof at y=5
    make_roof_plane(&planes[1], 2, 2.0);   // lower roof at y=2

    // Plane 1 polygon (centroid at z=2): bottom edge along z=0 from x=0 to x=4 at y=5
    // Plane 2 polygon (centroid at z=-2): top edge along z=0 from x=0 to x=4 at y=2
    v7_bclass_input_corner_t corners[8];
    corners[0] = mk_corner(0, 0.0, 5.0, 0.0);
    corners[1] = mk_corner(1, 4.0, 5.0, 0.0);
    corners[2] = mk_corner(2, 4.0, 5.0, 4.0);
    corners[3] = mk_corner(3, 0.0, 5.0, 4.0);
    corners[4] = mk_corner(4, 0.0, 2.0, 0.0);
    corners[5] = mk_corner(5, 4.0, 2.0, 0.0);
    corners[6] = mk_corner(6, 4.0, 2.0, -4.0);
    corners[7] = mk_corner(7, 0.0, 2.0, -4.0);

    uint32_t* pidsA = (uint32_t*)malloc(sizeof(uint32_t)); pidsA[0] = 1;
    uint32_t* pidsB = (uint32_t*)malloc(sizeof(uint32_t)); pidsB[0] = 2;
    v7_bclass_input_edge_t edges[2];
    edges[0] = mk_edge(100, 0, 1, pidsA, 1);  // plane 1 bottom edge (upper)
    edges[1] = mk_edge(200, 4, 5, pidsB, 1);  // plane 2 top edge (lower)

    uint32_t* path1 = (uint32_t*)malloc(4 * sizeof(uint32_t));
    path1[0] = 0; path1[1] = 1; path1[2] = 2; path1[3] = 3;
    uint32_t* path2 = (uint32_t*)malloc(4 * sizeof(uint32_t));
    path2[0] = 4; path2[1] = 5; path2[2] = 6; path2[3] = 7;
    v7_bclass_input_polygon_path_t paths[2] = {
        { .plane_id = 1, .corner_ids = path1, .n_corners = 4 },
        { .plane_id = 2, .corner_ids = path2, .n_corners = 4 },
    };

    v7_bclass_params_t params; v7_bclass_params_default(&params); params.up_axis = 1;
    v7_bclass_outputs_t out; v7_bclass_outputs_init(&out);
    int rc = v7_bclass_compute(planes, 2, corners, 8, edges, 2, paths, 2, NULL, 0, &params, &out);
    CHECK(rc == 0);
    // Plane 1's bottom edge: midpoint y=5 > plane 2's edge y=2 → overhang-top
    const v7_bclass_edge_t* eA = find_out_edge(&out, 100);
    const v7_bclass_edge_t* eB = find_out_edge(&out, 200);
    CHECK(eA != NULL && eB != NULL);
    if (eA) CHECK(eA->tag == V7_BCLASS_TAG_OVERHANG_TOP);
    if (eB) CHECK(eB->tag == V7_BCLASS_TAG_OVERHANG_BOTTOM);
    v7_bclass_outputs_free(&out);
    free(pidsA); free(pidsB); free(path1); free(path2);
}

// Two single-plane edges collinear-and-close in 3D → tag = SHARED on
// both, and a new-adjacency emitted (deduped vs ridge set if any).
static void test_shared_emits_new_adjacency(void) {
    fprintf(stderr, "== test_shared_emits_new_adjacency\n");
    v7_bclass_input_plane_t planes[2];
    make_roof_plane(&planes[0], 1, 5.0);
    make_roof_plane(&planes[1], 2, 5.0);

    // Both planes have an edge from (0,5,0) to (4,5,0) — basically the
    // same eave. plane 1's polygon centroid at z=2, plane 2's centroid
    // at z=-2 so each edge sees the OTHER's centroid as on the outward
    // side.
    v7_bclass_input_corner_t corners[8];
    corners[0] = mk_corner(0, 0.0, 5.0, 0.0);
    corners[1] = mk_corner(1, 4.0, 5.0, 0.0);
    corners[2] = mk_corner(2, 4.0, 5.0, 4.0);
    corners[3] = mk_corner(3, 0.0, 5.0, 4.0);
    corners[4] = mk_corner(4, 0.0, 5.0, 0.0);
    corners[5] = mk_corner(5, 4.0, 5.0, 0.0);
    corners[6] = mk_corner(6, 4.0, 5.0, -4.0);
    corners[7] = mk_corner(7, 0.0, 5.0, -4.0);

    uint32_t* pidsA = (uint32_t*)malloc(sizeof(uint32_t)); pidsA[0] = 1;
    uint32_t* pidsB = (uint32_t*)malloc(sizeof(uint32_t)); pidsB[0] = 2;
    v7_bclass_input_edge_t edges[2];
    edges[0] = mk_edge(100, 0, 1, pidsA, 1);
    edges[1] = mk_edge(200, 4, 5, pidsB, 1);

    uint32_t* path1 = (uint32_t*)malloc(4 * sizeof(uint32_t));
    path1[0] = 0; path1[1] = 1; path1[2] = 2; path1[3] = 3;
    uint32_t* path2 = (uint32_t*)malloc(4 * sizeof(uint32_t));
    path2[0] = 4; path2[1] = 5; path2[2] = 6; path2[3] = 7;
    v7_bclass_input_polygon_path_t paths[2] = {
        { .plane_id = 1, .corner_ids = path1, .n_corners = 4 },
        { .plane_id = 2, .corner_ids = path2, .n_corners = 4 },
    };

    v7_bclass_params_t params; v7_bclass_params_default(&params); params.up_axis = 1;
    v7_bclass_outputs_t out; v7_bclass_outputs_init(&out);
    int rc = v7_bclass_compute(planes, 2, corners, 8, edges, 2, paths, 2, NULL, 0, &params, &out);
    CHECK(rc == 0);
    const v7_bclass_edge_t* eA = find_out_edge(&out, 100);
    const v7_bclass_edge_t* eB = find_out_edge(&out, 200);
    CHECK(eA != NULL && eB != NULL);
    if (eA) CHECK(eA->tag == V7_BCLASS_TAG_SHARED);
    if (eB) CHECK(eB->tag == V7_BCLASS_TAG_SHARED);
    // Each shared coverage emits one new-adjacency; with two edges
    // mutually pairing under the same plane pair, dedup leaves exactly
    // one entry.
    CHECK(out.n_new_adjacencies == 1);
    if (out.n_new_adjacencies >= 1) {
        uint32_t lo = out.new_adjacencies[0].plane_a < out.new_adjacencies[0].plane_b
                    ? out.new_adjacencies[0].plane_a : out.new_adjacencies[0].plane_b;
        uint32_t hi = out.new_adjacencies[0].plane_a > out.new_adjacencies[0].plane_b
                    ? out.new_adjacencies[0].plane_a : out.new_adjacencies[0].plane_b;
        CHECK(lo == 1 && hi == 2);
        CHECK(out.new_adjacencies[0].type == 1);  // eave-shared
    }
    v7_bclass_outputs_free(&out);
    free(pidsA); free(pidsB); free(path1); free(path2);
}

// Eave-shared candidate where the plane pair is already a ridge →
// new-adjacency must be deduped to 0.
static void test_dedup_against_existing_ridge(void) {
    fprintf(stderr, "== test_dedup_against_existing_ridge\n");
    v7_bclass_input_plane_t planes[2];
    make_roof_plane(&planes[0], 1, 5.0);
    make_roof_plane(&planes[1], 2, 5.0);

    v7_bclass_input_corner_t corners[8];
    corners[0] = mk_corner(0, 0.0, 5.0, 0.0);
    corners[1] = mk_corner(1, 4.0, 5.0, 0.0);
    corners[2] = mk_corner(2, 4.0, 5.0, 4.0);
    corners[3] = mk_corner(3, 0.0, 5.0, 4.0);
    corners[4] = mk_corner(4, 0.0, 5.0, 0.0);
    corners[5] = mk_corner(5, 4.0, 5.0, 0.0);
    corners[6] = mk_corner(6, 4.0, 5.0, -4.0);
    corners[7] = mk_corner(7, 0.0, 5.0, -4.0);

    uint32_t* pidsA = (uint32_t*)malloc(sizeof(uint32_t)); pidsA[0] = 1;
    uint32_t* pidsB = (uint32_t*)malloc(sizeof(uint32_t)); pidsB[0] = 2;
    v7_bclass_input_edge_t edges[2];
    edges[0] = mk_edge(100, 0, 1, pidsA, 1);
    edges[1] = mk_edge(200, 4, 5, pidsB, 1);

    uint32_t* path1 = (uint32_t*)malloc(4 * sizeof(uint32_t));
    path1[0] = 0; path1[1] = 1; path1[2] = 2; path1[3] = 3;
    uint32_t* path2 = (uint32_t*)malloc(4 * sizeof(uint32_t));
    path2[0] = 4; path2[1] = 5; path2[2] = 6; path2[3] = 7;
    v7_bclass_input_polygon_path_t paths[2] = {
        { .plane_id = 1, .corner_ids = path1, .n_corners = 4 },
        { .plane_id = 2, .corner_ids = path2, .n_corners = 4 },
    };
    v7_bclass_input_ridge_t ridges[1] = { { .id = 42, .plane_a = 1, .plane_b = 2 } };

    v7_bclass_params_t params; v7_bclass_params_default(&params); params.up_axis = 1;
    v7_bclass_outputs_t out; v7_bclass_outputs_init(&out);
    int rc = v7_bclass_compute(planes, 2, corners, 8, edges, 2, paths, 2, ridges, 1, &params, &out);
    CHECK(rc == 0);
    // Tag still SHARED, but plane pair (1,2) is already a ridge → no new adj.
    CHECK(out.n_new_adjacencies == 0);
    v7_bclass_outputs_free(&out);
    free(pidsA); free(pidsB); free(path1); free(path2);
}

// Two single-plane wall-roof edges that share the same wall and come
// from DIFFERENT roof planes (different heights) → level-transition.
static void test_level_transition_two_roof_one_wall(void) {
    fprintf(stderr, "== test_level_transition_two_roof_one_wall\n");
    v7_bclass_input_plane_t planes[3];
    make_roof_plane(&planes[0], 1, 5.0);   // upper roof
    make_roof_plane(&planes[1], 2, 2.0);   // lower roof
    make_wall_plane(&planes[2], 3);         // shared wall

    v7_bclass_input_corner_t corners[8];
    corners[0] = mk_corner(0,  0.0, 5.0, 0.0);
    corners[1] = mk_corner(1,  4.0, 5.0, 0.0);
    corners[2] = mk_corner(2,  4.0, 5.0, 4.0);
    corners[3] = mk_corner(3,  0.0, 5.0, 4.0);
    corners[4] = mk_corner(4,  6.0, 2.0, 0.0);
    corners[5] = mk_corner(5, 10.0, 2.0, 0.0);
    corners[6] = mk_corner(6, 10.0, 2.0,-4.0);
    corners[7] = mk_corner(7,  6.0, 2.0,-4.0);

    // Edge A: plane 1's bottom edge, in wall 3 too
    uint32_t* pidsA = (uint32_t*)malloc(2 * sizeof(uint32_t));
    pidsA[0] = 1; pidsA[1] = 3;
    // Edge B: plane 2's top edge, in wall 3 too
    uint32_t* pidsB = (uint32_t*)malloc(2 * sizeof(uint32_t));
    pidsB[0] = 2; pidsB[1] = 3;
    v7_bclass_input_edge_t edges[2];
    edges[0] = mk_edge(100, 0, 1, pidsA, 2);
    edges[1] = mk_edge(200, 4, 5, pidsB, 2);

    uint32_t* path1 = (uint32_t*)malloc(4 * sizeof(uint32_t));
    path1[0] = 0; path1[1] = 1; path1[2] = 2; path1[3] = 3;
    uint32_t* path2 = (uint32_t*)malloc(4 * sizeof(uint32_t));
    path2[0] = 4; path2[1] = 5; path2[2] = 6; path2[3] = 7;
    v7_bclass_input_polygon_path_t paths[2] = {
        { .plane_id = 1, .corner_ids = path1, .n_corners = 4 },
        { .plane_id = 2, .corner_ids = path2, .n_corners = 4 },
    };

    v7_bclass_params_t params; v7_bclass_params_default(&params); params.up_axis = 1;
    v7_bclass_outputs_t out; v7_bclass_outputs_init(&out);
    int rc = v7_bclass_compute(planes, 3, corners, 8, edges, 2, paths, 2, NULL, 0, &params, &out);
    CHECK(rc == 0);
    const v7_bclass_edge_t* eA = find_out_edge(&out, 100);
    const v7_bclass_edge_t* eB = find_out_edge(&out, 200);
    CHECK(eA != NULL && eB != NULL);
    if (eA) CHECK(eA->tag == V7_BCLASS_TAG_LEVEL_TRANSITION);
    if (eB) CHECK(eB->tag == V7_BCLASS_TAG_LEVEL_TRANSITION);
    CHECK(out.stats.level_transition == 2);
    v7_bclass_outputs_free(&out);
    free(pidsA); free(pidsB); free(path1); free(path2);
}

// Degenerate edge (len3d < min_edge_len_3d) → tag UNSET, no crash.
static void test_degenerate_edge_unset(void) {
    fprintf(stderr, "== test_degenerate_edge_unset\n");
    v7_bclass_input_plane_t planes[1];
    make_roof_plane(&planes[0], 1, 5.0);
    v7_bclass_input_corner_t corners[2] = {
        mk_corner(0, 0.0, 5.0, 0.0),
        mk_corner(1, 0.0001, 5.0, 0.0),  // below min_edge_len_3d=0.01
    };
    uint32_t* pids = (uint32_t*)malloc(sizeof(uint32_t)); pids[0] = 1;
    v7_bclass_input_edge_t edges[1] = { mk_edge(100, 0, 1, pids, 1) };

    v7_bclass_params_t params; v7_bclass_params_default(&params); params.up_axis = 1;
    v7_bclass_outputs_t out; v7_bclass_outputs_init(&out);
    int rc = v7_bclass_compute(planes, 1, corners, 2, edges, 1, NULL, 0, NULL, 0, &params, &out);
    CHECK(rc == 0);
    const v7_bclass_edge_t* e = find_out_edge(&out, 100);
    CHECK(e != NULL);
    if (e) CHECK(e->tag == V7_BCLASS_TAG_UNSET);
    v7_bclass_outputs_free(&out);
    free(pids);
}

// NULL inputs are rejected with rc=-1 (impl pre-condition: callers must
// supply real arrays even if n=0).
static void test_null_inputs_rejected(void) {
    fprintf(stderr, "== test_null_inputs_rejected\n");
    v7_bclass_params_t params; v7_bclass_params_default(&params);
    v7_bclass_outputs_t out; v7_bclass_outputs_init(&out);
    int rc = v7_bclass_compute(NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, &params, &out);
    CHECK(rc == -1);
    v7_bclass_outputs_free(&out);
}

// Zero-edge inputs (non-NULL arrays, n=0) → rc=0, no work.
static void test_zero_edges_no_work(void) {
    fprintf(stderr, "== test_zero_edges_no_work\n");
    v7_bclass_input_plane_t planes[1];
    make_roof_plane(&planes[0], 1, 5.0);
    v7_bclass_input_corner_t corners[1] = { mk_corner(0, 0.0, 5.0, 0.0) };
    v7_bclass_input_edge_t edges[1];
    memset(edges, 0, sizeof(edges));
    v7_bclass_params_t params; v7_bclass_params_default(&params);
    v7_bclass_outputs_t out; v7_bclass_outputs_init(&out);
    int rc = v7_bclass_compute(planes, 1, corners, 1, edges, 0, NULL, 0, NULL, 0, &params, &out);
    CHECK(rc == 0);
    CHECK(out.n_edges == 0);
    CHECK(out.n_new_adjacencies == 0);
    v7_bclass_outputs_free(&out);
}

int main(void) {
    test_null_inputs_rejected();
    test_zero_edges_no_work();
    test_wall_only_edge_tagged_wall();
    test_multi_plane_no_walls_interior_shared();
    test_ridge_shared_corners_interior_shared();
    test_lone_single_plane_roof_perimeter();
    test_overhang_top_xz_collinear_close();
    test_shared_emits_new_adjacency();
    test_dedup_against_existing_ridge();
    test_level_transition_two_roof_one_wall();
    test_degenerate_edge_unset();
    fprintf(stderr, "\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}

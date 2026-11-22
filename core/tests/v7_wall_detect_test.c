// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// core/tests/v7_wall_detect_test.c
//
// Implementation tests for v7-wall-detect. Each test asserts a spec
// invariant from bench-v2 lib/pipeline/steps/wall-detect.js (meta.version=4).
// Inputs are hand-crafted minimal roof-graphs + point clouds.

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE   700

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Include the impl source directly so the test TU owns the compute symbols.
#include "v7-wall-detect-impl.c"

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

static v7_wd_input_plane_t mk_roof_plane(uint32_t id, double y) {
    v7_wd_input_plane_t p; memset(&p, 0, sizeof(p));
    p.id = id; p.tilt_deg = 0.0; p.rejected = false;
    p.normal[0] = 0.0; p.normal[1] = 1.0; p.normal[2] = 0.0;
    p.d = -y;
    return p;
}

static v7_wd_input_plane_t mk_wall_plane(uint32_t id, double nx, double nz, double d) {
    v7_wd_input_plane_t p; memset(&p, 0, sizeof(p));
    p.id = id; p.tilt_deg = 90.0; p.rejected = false;
    p.normal[0] = nx; p.normal[1] = 0.0; p.normal[2] = nz;
    p.d = d;
    return p;
}

static v7_wd_input_corner_t mk_corner(uint32_t id, double x, double y, double z) {
    v7_wd_input_corner_t c; memset(&c, 0, sizeof(c));
    c.id = id;
    c.point[0] = x; c.point[1] = y; c.point[2] = z;
    return c;
}

static v7_wd_input_edge_t mk_edge(uint32_t id, uint32_t ca, uint32_t cb,
                                    uint32_t* pids_owned, uint32_t n_pids) {
    v7_wd_input_edge_t e; memset(&e, 0, sizeof(e));
    e.id = id; e.corner_a = ca; e.corner_b = cb;
    e.plane_ids = pids_owned; e.n_plane_ids = n_pids;
    return e;
}

static v7_wd_input_perim_tag_t mk_perim_perimeter(uint32_t edge_id, float ox, float oz) {
    v7_wd_input_perim_tag_t t; memset(&t, 0, sizeof(t));
    t.edge_id = edge_id; t.tag = V7_BCLASS_TAG_PERIMETER;
    t.has_outward_xz = true; t.outward_xz[0] = ox; t.outward_xz[1] = oz;
    return t;
}

// ─── tests ──────────────────────────────────────────────────────────

// Lone perimeter edge over a flat ground → wall created, 4 corners, owner stays unchanged.
static void test_lone_perimeter_creates_wall(void) {
    fprintf(stderr, "== test_lone_perimeter_creates_wall\n");
    v7_wd_input_plane_t planes[1] = { mk_roof_plane(0, 5.0) };
    v7_wd_input_corner_t corners[2] = {
        mk_corner(0, 0.0, 5.0, 0.0),
        mk_corner(1, 4.0, 5.0, 0.0),
    };
    uint32_t* pids = (uint32_t*)malloc(sizeof(uint32_t)); pids[0] = 0;
    v7_wd_input_edge_t edges[1] = { mk_edge(100, 0, 1, pids, 1) };
    v7_wd_input_perim_tag_t tags[1] = { mk_perim_perimeter(100, 0.0f, -1.0f) };
    // Two ground points so building_ground_y becomes 0.
    float positions[6] = { 1.0f, 0.0f, 0.5f,  3.0f, 0.0f, 0.5f };
    int32_t owner[2] = { 0, 0 };
    v7_wd_params_t params; v7_wd_params_default(&params);
    v7_wd_outputs_t out; v7_wd_outputs_init(&out);
    int rc = v7_wd_compute(planes, 1, corners, 2, edges, 1, tags, 1,
                            positions, 2, NULL, owner, 0.12, &params, &out);
    CHECK(rc == 0);
    CHECK(out.n_walls_created == 1);
    CHECK(out.n_walls_merged == 0);
    CHECK(out.n_wall_planes == 1);
    CHECK(out.n_wall_corners == 4);
    if (out.n_wall_planes >= 1) {
        const v7_wd_wall_plane_t* w = &out.wall_planes[0];
        CHECK(w->is_merged == false);
        CHECK(w->existing_plane_id == UINT32_MAX);
        CHECK(w->tilt_deg == 90.0);
        CHECK_CLOSE(w->normal[1], 0.0, 1e-9);
        CHECK_CLOSE(w->area_m2, 4.0 * 5.0, 1e-6);  // width=4 m, height=5-0=5 m
    }
    v7_wd_outputs_free(&out);
    free(pids);
}

// Existing wall plane near the eave → merge, no new plane.
static void test_existing_wall_merge(void) {
    fprintf(stderr, "== test_existing_wall_merge\n");
    // Roof at y=5 (id 0). Wall already present at id 1, normal=(0,0,-1), d=0
    // (the plane z=0 facing -z).
    v7_wd_input_plane_t planes[2] = {
        mk_roof_plane(0, 5.0),
        mk_wall_plane(1, 0.0, -1.0, 0.0),
    };
    v7_wd_input_corner_t corners[2] = {
        mk_corner(0, 0.0, 5.0, 0.0),
        mk_corner(1, 4.0, 5.0, 0.0),
    };
    uint32_t* pids = (uint32_t*)malloc(sizeof(uint32_t)); pids[0] = 0;
    v7_wd_input_edge_t edges[1] = { mk_edge(100, 0, 1, pids, 1) };
    // Outward = (0, -1) (away from polygon centroid at +z).
    v7_wd_input_perim_tag_t tags[1] = { mk_perim_perimeter(100, 0.0f, -1.0f) };
    float positions[6] = { 1.0f, 0.0f, 0.5f,  3.0f, 0.0f, 0.5f };
    int32_t owner[2] = { 0, 0 };
    v7_wd_params_t params; v7_wd_params_default(&params);
    v7_wd_outputs_t out; v7_wd_outputs_init(&out);
    int rc = v7_wd_compute(planes, 2, corners, 2, edges, 1, tags, 1,
                            positions, 2, NULL, owner, 0.12, &params, &out);
    CHECK(rc == 0);
    CHECK(out.n_walls_created == 0);
    CHECK(out.n_walls_merged == 1);
    if (out.n_wall_planes >= 1) {
        const v7_wd_wall_plane_t* w = &out.wall_planes[0];
        CHECK(w->is_merged == true);
        CHECK(w->existing_plane_id == 1);
        CHECK(w->id == 1);  // reuses existing id
    }
    v7_wd_outputs_free(&out);
    free(pids);
}

// Edge with non-perimeter tag (e.g. interior-shared) → ignored.
static void test_non_perimeter_tag_ignored(void) {
    fprintf(stderr, "== test_non_perimeter_tag_ignored\n");
    v7_wd_input_plane_t planes[1] = { mk_roof_plane(0, 5.0) };
    v7_wd_input_corner_t corners[2] = {
        mk_corner(0, 0.0, 5.0, 0.0),
        mk_corner(1, 4.0, 5.0, 0.0),
    };
    uint32_t* pids = (uint32_t*)malloc(sizeof(uint32_t)); pids[0] = 0;
    v7_wd_input_edge_t edges[1] = { mk_edge(100, 0, 1, pids, 1) };
    v7_wd_input_perim_tag_t tags[1] = { 0 };
    tags[0].edge_id = 100;
    tags[0].tag = V7_BCLASS_TAG_INTERIOR_SHARED;
    tags[0].has_outward_xz = false;
    float positions[6] = { 1.0f, 0.0f, 0.5f,  3.0f, 0.0f, 0.5f };
    int32_t owner[2] = { 0, 0 };
    v7_wd_params_t params; v7_wd_params_default(&params);
    v7_wd_outputs_t out; v7_wd_outputs_init(&out);
    int rc = v7_wd_compute(planes, 1, corners, 2, edges, 1, tags, 1,
                            positions, 2, NULL, owner, 0.12, &params, &out);
    CHECK(rc == 0);
    CHECK(out.n_walls_created == 0);
    CHECK(out.n_walls_merged == 0);
    CHECK(out.n_debug == 0);  // no debug entry; only perimeter/level-transition trigger walls
    v7_wd_outputs_free(&out);
    free(pids);
}

// Perimeter edge without outwardXZ → skipped (debug entry with skipped=true).
static void test_missing_outward_skipped(void) {
    fprintf(stderr, "== test_missing_outward_skipped\n");
    v7_wd_input_plane_t planes[1] = { mk_roof_plane(0, 5.0) };
    v7_wd_input_corner_t corners[2] = {
        mk_corner(0, 0.0, 5.0, 0.0),
        mk_corner(1, 4.0, 5.0, 0.0),
    };
    uint32_t* pids = (uint32_t*)malloc(sizeof(uint32_t)); pids[0] = 0;
    v7_wd_input_edge_t edges[1] = { mk_edge(100, 0, 1, pids, 1) };
    v7_wd_input_perim_tag_t tags[1] = { 0 };
    tags[0].edge_id = 100;
    tags[0].tag = V7_BCLASS_TAG_PERIMETER;
    tags[0].has_outward_xz = false;  // missing outwardXZ
    float positions[6] = { 1.0f, 0.0f, 0.5f,  3.0f, 0.0f, 0.5f };
    int32_t owner[2] = { 0, 0 };
    v7_wd_params_t params; v7_wd_params_default(&params);
    v7_wd_outputs_t out; v7_wd_outputs_init(&out);
    int rc = v7_wd_compute(planes, 1, corners, 2, edges, 1, tags, 1,
                            positions, 2, NULL, owner, 0.12, &params, &out);
    CHECK(rc == 0);
    CHECK(out.n_walls_created == 0);
    CHECK(out.n_edges_skipped == 1);
    CHECK(out.n_debug == 1);
    if (out.n_debug >= 1) CHECK(out.debug[0].skipped == true);
    v7_wd_outputs_free(&out);
    free(pids);
}

// Point-grab — unassigned point near the wall plane gets reassigned.
static void test_point_grab(void) {
    fprintf(stderr, "== test_point_grab\n");
    v7_wd_input_plane_t planes[1] = { mk_roof_plane(0, 5.0) };
    v7_wd_input_corner_t corners[2] = {
        mk_corner(0, 0.0, 5.0, 0.0),
        mk_corner(1, 4.0, 5.0, 0.0),
    };
    uint32_t* pids = (uint32_t*)malloc(sizeof(uint32_t)); pids[0] = 0;
    v7_wd_input_edge_t edges[1] = { mk_edge(100, 0, 1, pids, 1) };
    v7_wd_input_perim_tag_t tags[1] = { mk_perim_perimeter(100, 0.0f, -1.0f) };
    // 3 points: one unassigned ON the wall (should be grabbed), one
    // already owned (won't change), one unassigned far away outside
    // the wall's xz bbox (won't grab). NO inward witnesses so the
    // wall stays at its naive z=0 position.
    float positions[9] = {
        2.0f, 0.0f, 0.0f,   // unassigned, ON the wall plane (perp=0, z=0) — grab
        2.0f, 3.0f, 0.0f,   // owner=0 — untouched (and on the eave line P_p=0 → not a witness)
        5.0f, 2.0f, 5.0f,   // outside xz bbox — won't grab
    };
    int32_t owner[3] = { -1, 0, -1 };
    v7_wd_params_t params; v7_wd_params_default(&params);
    v7_wd_outputs_t out; v7_wd_outputs_init(&out);
    int rc = v7_wd_compute(planes, 1, corners, 2, edges, 1, tags, 1,
                            positions, 3, NULL, owner, 0.12, &params, &out);
    CHECK(rc == 0);
    CHECK(out.n_walls_created == 1);
    if (out.n_wall_planes >= 1) {
        const uint32_t wall_id = out.wall_planes[0].id;
        CHECK(owner[0] == (int32_t)wall_id);  // grabbed
        CHECK(owner[1] == 0);                  // untouched (had owner != -1)
        CHECK(owner[2] == -1);                 // untouched (outside bbox)
    }
    CHECK(out.n_owner_changes == 1);
    v7_wd_outputs_free(&out);
    free(pids);
}

// Null inputs → reject with rc=-1.
static void test_null_inputs_rejected(void) {
    fprintf(stderr, "== test_null_inputs_rejected\n");
    v7_wd_params_t params; v7_wd_params_default(&params);
    v7_wd_outputs_t out; v7_wd_outputs_init(&out);
    int rc = v7_wd_compute(NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, NULL, 0.12, &params, &out);
    CHECK(rc == -1);
    v7_wd_outputs_free(&out);
}

// Zero-edge inputs → rc=0, no work.
static void test_zero_edges_no_work(void) {
    fprintf(stderr, "== test_zero_edges_no_work\n");
    v7_wd_input_plane_t planes[1] = { mk_roof_plane(0, 5.0) };
    v7_wd_input_corner_t corners[1] = { mk_corner(0, 0.0, 5.0, 0.0) };
    v7_wd_input_edge_t edges[1]; memset(edges, 0, sizeof(edges));
    v7_wd_input_perim_tag_t tags[1]; memset(tags, 0, sizeof(tags));
    float positions[3] = { 0.0f, 0.0f, 0.0f };
    int32_t owner[1] = { 0 };
    v7_wd_params_t params; v7_wd_params_default(&params);
    v7_wd_outputs_t out; v7_wd_outputs_init(&out);
    int rc = v7_wd_compute(planes, 1, corners, 1, edges, 0, tags, 0,
                            positions, 1, NULL, owner, 0.12, &params, &out);
    CHECK(rc == 0);
    CHECK(out.n_walls_created == 0);
    v7_wd_outputs_free(&out);
}

// Beam-bound — point's beam crosses inside the building footprint, raises
// delta_min above the naive zero. Validates the back-projection math.
static void test_beam_bound_raises_delta(void) {
    fprintf(stderr, "== test_beam_bound_raises_delta\n");
    // Roof at y=5, edge along x from (0,5,0) to (4,5,0), outward=(0,-1).
    // Point at (2, 2, 1) — inside footprint by 1 m in z (P_p = 1). With
    // a vertical beam (0,-1,0), P_t = P_p so delta_this = 1 m. Capped
    // at OVERHANG_CAP=1.5.
    v7_wd_input_plane_t planes[1] = { mk_roof_plane(0, 5.0) };
    v7_wd_input_corner_t corners[2] = {
        mk_corner(0, 0.0, 5.0, 0.0),
        mk_corner(1, 4.0, 5.0, 0.0),
    };
    uint32_t* pids = (uint32_t*)malloc(sizeof(uint32_t)); pids[0] = 0;
    v7_wd_input_edge_t edges[1] = { mk_edge(100, 0, 1, pids, 1) };
    v7_wd_input_perim_tag_t tags[1] = { mk_perim_perimeter(100, 0.0f, -1.0f) };
    float positions[3] = { 2.0f, 2.0f, 1.0f };  // 1 m inward of eave
    int32_t owner[1] = { 0 };
    v7_wd_params_t params; v7_wd_params_default(&params);
    v7_wd_outputs_t out; v7_wd_outputs_init(&out);
    int rc = v7_wd_compute(planes, 1, corners, 2, edges, 1, tags, 1,
                            positions, 1, NULL, owner, 0.12, &params, &out);
    CHECK(rc == 0);
    CHECK(out.n_debug == 1);
    if (out.n_debug >= 1) {
        CHECK(out.debug[0].delta_source == V7_WD_DELTA_BEAM_BOUND);
        CHECK_CLOSE(out.debug[0].delta_min_m, 1.0, 1e-6);
        CHECK(out.debug[0].witness_count == 1);
    }
    v7_wd_outputs_free(&out);
    free(pids);
}

int main(void) {
    test_null_inputs_rejected();
    test_zero_edges_no_work();
    test_lone_perimeter_creates_wall();
    test_existing_wall_merge();
    test_non_perimeter_tag_ignored();
    test_missing_outward_skipped();
    test_point_grab();
    test_beam_bound_raises_delta();
    fprintf(stderr, "\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// core/tests/v7_boundary_corners_test.c
//
// Implementation tests for v7-boundary-corners. Each test asserts a
// spec invariant from bench-v2 lib/pipeline/steps/boundary-corners.js
// (meta.version=4) — Pass 1 corner discovery (D.0 wall-bbox, D.1a RI
// anchor enrichment, D.1b ridge-end, D.2 ridge snap, D.2b ridge-side
// filter) + coplanar import. Inputs are hand-crafted, minimal,
// deterministic.
//
// Note: bench-v2 has no dedicated test/boundary-corners.test.js. The
// JS files cited in V7-TEST-STRATEGY.md (test/edges.test.js +
// test/metrics-edges.test.js) cover edge-accuracy metrics, not the
// corner-pool topology that boundary-corners produces. The invariants
// below are extracted directly from the algorithm source — the v2
// boundary-corners.js JSDoc + the SRC_PRIORITY map (RI=4, RE=3, MRG=2,
// concave=1, convex=1, coplanar=0), the POOL_MERGE_DIST constant
// (0.20m), the WALL_TILT_MIN (72°), the COPLANAR_DIST_THRESHOLD (0.12m)
// + COPLANAR_BALANCE_MIN (0.30) gates — and the v3 impl docstrings.
//
// We include v7-boundary-corners-impl.c and v7-corner-pool.c directly
// to keep the test in a single TU (matching v7_hemisphere_segment_test
// pattern, which inlines its sibling .c sources). Only safe_alloc +
// math need linking — registered via dioramap_steps_normals.
//
// INFINITY-sentinel handling: the D.0 wall-bbox path uses INFINITY as
// the initial value for h_min / v_min and -INFINITY for h_max (see
// FRAMA-C-FALSE-POSITIVES.md §"sentinel-style INFINITY writes are
// silenced"). The wall-bbox test below drives that path and asserts
// finite corner positions on the wire.

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

// Pull in the impl + the corner-pool source as part of this TU. The
// pool module supplies v7p_* symbols the impl calls into; the CMake
// foreach in core/tests/CMakeLists.txt only knows about
// v7_<step>_test.c, not its transitive .c deps, so we inline them
// here in dependency order (pool first, then the impl that uses it).
#include "v7-corner-pool.c"
#include "v7-boundary-corners-impl.c"

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

static void default_params(v7_bc_params_t* p) {
    v7_bc_params_default(p);
    // Pin up_axis to y-up (matches v2 wall-bbox path and the bench
    // driver). The wall-bbox test relies on (eh, ev) being derived for
    // y-up.
    p->up_axis = 1;
}

// Flat horizontal plane at y = y0, with orthonormal basis (e1=+x, e2=+z),
// normal=+y. d = -y0 so n·p + d = 0 on the plane.
static void make_flat_plane(v7_bc_input_plane_t* P, uint32_t id, double y0) {
    memset(P, 0, sizeof(*P));
    P->id = id;
    P->rejected = false;
    P->tilt_deg = 0.0;
    P->normal[0] = 0.0; P->normal[1] = 1.0; P->normal[2] = 0.0;
    P->d = -y0;
    P->basis_e1[0] = 1.0; P->basis_e1[1] = 0.0; P->basis_e1[2] = 0.0;
    P->basis_e2[0] = 0.0; P->basis_e2[1] = 0.0; P->basis_e2[2] = 1.0;
}

// Vertical (wall) plane with normal=+x. tilt=90° engages D.0 wall-bbox.
// y-up convention so that ev (vertical) points +y. eh derived in v7-bc
// as (nz, 0, -nx) for y-up — i.e. (0, 0, -1) for normal=+x.
// basis_e1/e2 unused by D.0 itself but must be valid for coplanar-import
// path; we pick e1=+y (vertical), e2=+z (horizontal).
static void make_wall_plane(v7_bc_input_plane_t* P, uint32_t id) {
    memset(P, 0, sizeof(*P));
    P->id = id;
    P->rejected = false;
    P->tilt_deg = 90.0;
    P->normal[0] = 1.0; P->normal[1] = 0.0; P->normal[2] = 0.0;
    P->d = 0.0;
    P->basis_e1[0] = 0.0; P->basis_e1[1] = 1.0; P->basis_e1[2] = 0.0;
    P->basis_e2[0] = 0.0; P->basis_e2[1] = 0.0; P->basis_e2[2] = 1.0;
}

// Fill a flat patch of points on the wall plane (x=0). Used by the
// wall-bbox test. n × n grid spanning [-2, +2] in z and [-1.5, +1.5]
// in y (vertical band of a 3m × 4m wall, centred on origin).
static void fill_wall_patch(double* positions, int16_t* owner,
                            int n_per_side, int16_t owner_idx) {
    size_t k = 0;
    for (int i = 0; i < n_per_side; i++) {
        for (int j = 0; j < n_per_side; j++) {
            double y = -1.5 + 3.0 * ((double)i / (n_per_side - 1));
            double z = -2.0 + 4.0 * ((double)j / (n_per_side - 1));
            positions[k * 3 + 0] = 0.0;
            positions[k * 3 + 1] = y;
            positions[k * 3 + 2] = z;
            owner[k] = owner_idx;
            k++;
        }
    }
}

// Make a fresh RI seed corner at (x, y, z), pinned to planes {p0, p1}
// and ridge id r0. Caller owns the static arrays (the impl deep-copies).
static void make_ri_seed(v7_bc_input_corner_t* C, uint32_t id,
                         double x, double y, double z,
                         uint32_t* plane_ids_buf, uint32_t n_plane_ids,
                         uint32_t* ridge_ids_buf, uint32_t n_ridge_ids,
                         char* name_buf) {
    memset(C, 0, sizeof(*C));
    C->id = id;
    C->source = V7P_SRC_RIDGE_INTERSECTION;
    C->point[0] = x; C->point[1] = y; C->point[2] = z;
    C->plane_ids = plane_ids_buf;
    C->n_plane_ids = n_plane_ids;
    C->ridge_ids = ridge_ids_buf;
    C->n_ridge_ids = n_ridge_ids;
    C->name = name_buf;
}

// ─── tests ──────────────────────────────────────────────────────────

// Empty input → 0 corners, no crash.
// Spec: n_planes == 0 takes the early-exit path; outputs are zeroed.
static void test_empty_input_no_crash(void) {
    v7_bc_params_t p; default_params(&p);
    v7_bc_outputs_t out; v7_bc_outputs_init(&out);
    int rc = v7_bc_compute(NULL, 0, NULL, NULL, 0, NULL, 0, NULL, 0, NULL, 0, &p, &out);
    CHECK(rc == 0);
    CHECK(out.n_corners == 0);
    CHECK(out.n_planes == 0);
    CHECK(out.wall_corners_added == 0);
    CHECK(out.coplanar_imported == 0);
    v7_bc_outputs_free(&out);
}

// No seeds + no points + just 1 plane → 0 corners.
// Spec: each pass (D.0 wall-bbox, D.1a, D.1b) only emits when the
// plane has points or ridges; an empty plane yields no anchors and so
// no pool entries.
static void test_no_seeds_no_corners(void) {
    v7_bc_input_plane_t plane; make_flat_plane(&plane, 0, 5.0);
    v7_bc_params_t p; default_params(&p);
    v7_bc_outputs_t out; v7_bc_outputs_init(&out);
    int rc = v7_bc_compute(NULL, 0, NULL, &plane, 1, NULL, 0, NULL, 0, NULL, 0, &p, &out);
    CHECK(rc == 0);
    CHECK(out.n_corners == 0);
    // plane_corner_ids exists with length n_planes but each slot is empty.
    CHECK(out.n_planes == 1);
    CHECK(out.plane_corner_ids_count != NULL);
    if (out.plane_corner_ids_count) CHECK(out.plane_corner_ids_count[0] == 0);
    v7_bc_outputs_free(&out);
}

// Single RI seed corner is preserved as a pool entry with source=RI
// and the input position. ID is 0 (dense start).
static void test_single_ri_seed_preserved(void) {
    v7_bc_input_plane_t planes[2];
    make_flat_plane(&planes[0], 0, 5.0);
    make_flat_plane(&planes[1], 1, 5.0);

    uint32_t pids[2] = { 0, 1 };
    uint32_t rids[1] = { 0 };
    v7_bc_input_corner_t seed;
    make_ri_seed(&seed, 0, 1.0, 5.0, 2.0, pids, 2, rids, 1, NULL);

    v7_bc_params_t p; default_params(&p);
    v7_bc_outputs_t out; v7_bc_outputs_init(&out);
    int rc = v7_bc_compute(NULL, 0, NULL, planes, 2, NULL, 0, NULL, 0, &seed, 1, &p, &out);
    CHECK(rc == 0);
    CHECK(out.n_corners == 1);
    if (out.n_corners == 1) {
        const v7_bc_corner_t* C = &out.corners[0];
        CHECK(C->id == 0);
        CHECK(C->source == V7P_SRC_RIDGE_INTERSECTION);
        CHECK_CLOSE(C->point[0], 1.0, 1e-12);
        CHECK_CLOSE(C->point[1], 5.0, 1e-12);
        CHECK_CLOSE(C->point[2], 2.0, 1e-12);
        // The RI seed propagates its plane_ids ({0, 1}).
        CHECK(C->n_plane_ids == 2);
        CHECK(C->n_ridge_ids == 1);
    }
    v7_bc_outputs_free(&out);
}

// Two RI seeds within POOL_MERGE_DIST (0.20m default) MUST collapse
// into a single pool entry. Spec: pool registration runs a spatial
// merge — any new member within pool_merge_dist of an existing entry's
// member set joins that entry.
static void test_ri_seeds_within_merge_dist_dedup(void) {
    v7_bc_input_plane_t planes[2];
    make_flat_plane(&planes[0], 0, 5.0);
    make_flat_plane(&planes[1], 1, 5.0);

    uint32_t pids_a[1] = { 0 };
    uint32_t pids_b[1] = { 1 };
    uint32_t rids_a[1] = { 0 };
    uint32_t rids_b[1] = { 1 };
    v7_bc_input_corner_t seeds[2];
    // 0.10m apart — well under the 0.20m merge threshold.
    make_ri_seed(&seeds[0], 0, 1.00, 5.0, 2.0, pids_a, 1, rids_a, 1, NULL);
    make_ri_seed(&seeds[1], 1, 1.10, 5.0, 2.0, pids_b, 1, rids_b, 1, NULL);

    v7_bc_params_t p; default_params(&p);
    v7_bc_outputs_t out; v7_bc_outputs_init(&out);
    int rc = v7_bc_compute(NULL, 0, NULL, planes, 2, NULL, 0, NULL, 0, seeds, 2, &p, &out);
    CHECK(rc == 0);
    CHECK(out.n_corners == 1);
    if (out.n_corners == 1) {
        const v7_bc_corner_t* C = &out.corners[0];
        // Merged entry holds plane_ids from both seeds (union).
        CHECK(C->n_plane_ids == 2);
        // n_merged tracks the count of members folded in.
        CHECK(C->n_merged >= 2);
    }
    v7_bc_outputs_free(&out);
}

// Two RI seeds farther than POOL_MERGE_DIST (0.20m) stay distinct.
// Spec: 0.5m apart on the same axis → two separate pool entries with
// dense IDs {0, 1}.
static void test_ri_seeds_beyond_merge_dist_separate(void) {
    v7_bc_input_plane_t planes[2];
    make_flat_plane(&planes[0], 0, 5.0);
    make_flat_plane(&planes[1], 1, 5.0);

    uint32_t pids_a[1] = { 0 };
    uint32_t pids_b[1] = { 1 };
    uint32_t rids_a[1] = { 0 };
    uint32_t rids_b[1] = { 1 };
    v7_bc_input_corner_t seeds[2];
    make_ri_seed(&seeds[0], 0, 0.0, 5.0, 0.0, pids_a, 1, rids_a, 1, NULL);
    make_ri_seed(&seeds[1], 1, 5.0, 5.0, 0.0, pids_b, 1, rids_b, 1, NULL);

    v7_bc_params_t p; default_params(&p);
    v7_bc_outputs_t out; v7_bc_outputs_init(&out);
    int rc = v7_bc_compute(NULL, 0, NULL, planes, 2, NULL, 0, NULL, 0, seeds, 2, &p, &out);
    CHECK(rc == 0);
    CHECK(out.n_corners == 2);
    // IDs dense and unique.
    if (out.n_corners == 2) {
        CHECK(out.corners[0].id == 0);
        CHECK(out.corners[1].id == 1);
        CHECK(out.corners[0].id != out.corners[1].id);
    }
    v7_bc_outputs_free(&out);
}

// Corner IDs are dense (0..n-1) and unique even after merges.
// Spec: serialization walks alive entries in pool order and assigns
// out_idx as the running counter — gaps from consumed entries are
// skipped, so output IDs always pack to [0, n).
static void test_corner_ids_dense_and_unique(void) {
    v7_bc_input_plane_t planes[3];
    make_flat_plane(&planes[0], 0, 5.0);
    make_flat_plane(&planes[1], 1, 5.0);
    make_flat_plane(&planes[2], 2, 5.0);

    // Five widely-spaced RI seeds → five distinct pool entries.
    uint32_t pids[5][1] = { {0}, {1}, {2}, {0}, {1} };
    uint32_t rids[5][1] = { {0}, {1}, {2}, {3}, {4} };
    v7_bc_input_corner_t seeds[5];
    static const double xs[5] = { 0.0, 2.0, 4.0, 6.0, 8.0 };
    for (int i = 0; i < 5; i++) {
        make_ri_seed(&seeds[i], (uint32_t)i, xs[i], 5.0, 0.0,
                     pids[i], 1, rids[i], 1, NULL);
    }

    v7_bc_params_t p; default_params(&p);
    v7_bc_outputs_t out; v7_bc_outputs_init(&out);
    int rc = v7_bc_compute(NULL, 0, NULL, planes, 3, NULL, 0, NULL, 0, seeds, 5, &p, &out);
    CHECK(rc == 0);
    CHECK(out.n_corners == 5);
    // IDs equal their array index — i.e. dense, unique, monotonic.
    for (size_t i = 0; i < out.n_corners; i++) {
        CHECK(out.corners[i].id == (uint32_t)i);
    }
    // Cross-check uniqueness via pairwise comparison.
    for (size_t i = 0; i < out.n_corners; i++) {
        for (size_t j = i + 1; j < out.n_corners; j++) {
            CHECK(out.corners[i].id != out.corners[j].id);
        }
    }
    v7_bc_outputs_free(&out);
}

// plane_corner_ids per-plane counts match the actual cross-references
// in corners[].plane_ids. Spec: the wrapper convenience array is built
// by counting corners whose plane_ids contains plane.id.
static void test_plane_corner_ids_consistent(void) {
    v7_bc_input_plane_t planes[3];
    make_flat_plane(&planes[0], 0, 5.0);
    make_flat_plane(&planes[1], 1, 5.0);
    make_flat_plane(&planes[2], 2, 5.0);

    // Seed 0 touches planes {0, 1}; seed 1 touches plane {2}; seed 2 only {0}.
    uint32_t pids0[2] = { 0, 1 };
    uint32_t pids1[1] = { 2 };
    uint32_t pids2[1] = { 0 };
    uint32_t r0[1] = { 0 }, r1[1] = { 1 }, r2[1] = { 2 };
    v7_bc_input_corner_t seeds[3];
    make_ri_seed(&seeds[0], 0, 0.0, 5.0, 0.0, pids0, 2, r0, 1, NULL);
    make_ri_seed(&seeds[1], 1, 4.0, 5.0, 0.0, pids1, 1, r1, 1, NULL);
    make_ri_seed(&seeds[2], 2, 8.0, 5.0, 0.0, pids2, 1, r2, 1, NULL);

    v7_bc_params_t p; default_params(&p);
    v7_bc_outputs_t out; v7_bc_outputs_init(&out);
    int rc = v7_bc_compute(NULL, 0, NULL, planes, 3, NULL, 0, NULL, 0, seeds, 3, &p, &out);
    CHECK(rc == 0);
    CHECK(out.n_corners == 3);
    CHECK(out.n_planes == 3);
    // Plane 0 sees 2 corners (seeds 0 + 2); plane 1 sees 1; plane 2 sees 1.
    CHECK(out.plane_corner_ids_count[0] == 2);
    CHECK(out.plane_corner_ids_count[1] == 1);
    CHECK(out.plane_corner_ids_count[2] == 1);
    // Cross-check against the corners array: for each plane g, every
    // index in plane_corner_ids[g] is a valid corner whose plane_ids
    // contains planes[g].id.
    for (size_t g = 0; g < out.n_planes; g++) {
        uint32_t pid = planes[g].id;
        for (uint32_t k = 0; k < out.plane_corner_ids_count[g]; k++) {
            uint32_t ci = out.plane_corner_ids[g][k];
            CHECK(ci < out.n_corners);
            const v7_bc_corner_t* C = &out.corners[ci];
            bool found = false;
            for (uint32_t kk = 0; kk < C->n_plane_ids; kk++) {
                if (C->plane_ids[kk] == pid) { found = true; break; }
            }
            CHECK(found);
        }
    }
    v7_bc_outputs_free(&out);
}

// Determinism: same inputs → same outputs.
// Spec: no RNG, no hash-keyed iteration; pool merges are order-stable
// across two identical calls.
static void test_determinism_same_inputs_same_outputs(void) {
    v7_bc_input_plane_t planes[2];
    make_flat_plane(&planes[0], 0, 5.0);
    make_flat_plane(&planes[1], 1, 5.0);

    uint32_t pids0[2] = { 0, 1 };
    uint32_t pids1[1] = { 0 };
    uint32_t r0[1] = { 0 }, r1[1] = { 1 };
    v7_bc_input_corner_t seeds[2];
    make_ri_seed(&seeds[0], 0, 1.0, 5.0, 2.0, pids0, 2, r0, 1, NULL);
    make_ri_seed(&seeds[1], 1, 4.0, 5.0, 1.0, pids1, 1, r1, 1, NULL);

    v7_bc_params_t p; default_params(&p);
    v7_bc_outputs_t a; v7_bc_outputs_init(&a);
    v7_bc_outputs_t b; v7_bc_outputs_init(&b);
    CHECK(v7_bc_compute(NULL, 0, NULL, planes, 2, NULL, 0, NULL, 0, seeds, 2, &p, &a) == 0);
    CHECK(v7_bc_compute(NULL, 0, NULL, planes, 2, NULL, 0, NULL, 0, seeds, 2, &p, &b) == 0);

    CHECK(a.n_corners == b.n_corners);
    for (size_t i = 0; i < a.n_corners && i < b.n_corners; i++) {
        CHECK(a.corners[i].id == b.corners[i].id);
        CHECK(a.corners[i].source == b.corners[i].source);
        CHECK(a.corners[i].n_plane_ids == b.corners[i].n_plane_ids);
        CHECK_CLOSE(a.corners[i].point[0], b.corners[i].point[0], 1e-15);
        CHECK_CLOSE(a.corners[i].point[1], b.corners[i].point[1], 1e-15);
        CHECK_CLOSE(a.corners[i].point[2], b.corners[i].point[2], 1e-15);
    }
    v7_bc_outputs_free(&a);
    v7_bc_outputs_free(&b);
}

// Wall-bbox path (D.0): a vertical plane (tilt = 90° ≥ WALL_TILT_MIN
// = 72°) with a dense point patch and h-extent ≥ wall_min_h_extent
// emits 4 synthetic concave corners (BL, BR, LT, RT).
//
// Drives the INFINITY-sentinel code path: build_plane_data initializes
// h_min = INFINITY, h_max = -INFINITY, v_min = INFINITY, then folds in
// real point values — the assertion below that corner positions are
// finite confirms those sentinels were updated by real data, not
// left at the initial INFINITY (see FRAMA-C-FALSE-POSITIVES.md).
static void test_wall_bbox_emits_four_corners(void) {
    const int NSIDE = 8;
    const int N = NSIDE * NSIDE;
    double positions[8 * 8 * 3];
    int16_t owner[8 * 8];
    fill_wall_patch(positions, owner, NSIDE, 0);

    v7_bc_input_plane_t plane; make_wall_plane(&plane, 0);

    v7_bc_params_t p; default_params(&p);
    v7_bc_outputs_t out; v7_bc_outputs_init(&out);
    int rc = v7_bc_compute(positions, N, owner, &plane, 1, NULL, 0, NULL, 0, NULL, 0, &p, &out);
    CHECK(rc == 0);
    // 4 wall corners pushed as anchors → 4 pool entries
    // (they are spatially separated by the patch extent so no merging).
    CHECK(out.wall_corners_added == 4);
    CHECK(out.n_corners == 4);
    // All four positions are finite (INFINITY sentinels were updated).
    for (size_t i = 0; i < out.n_corners; i++) {
        CHECK(isfinite(out.corners[i].point[0]));
        CHECK(isfinite(out.corners[i].point[1]));
        CHECK(isfinite(out.corners[i].point[2]));
        // Source = concave (priority 1) per D.0 spec.
        CHECK(out.corners[i].source == V7P_SRC_CONCAVE);
        // Each wall corner lives only on plane 0.
        CHECK(out.corners[i].n_plane_ids == 1);
        if (out.corners[i].n_plane_ids == 1) {
            CHECK(out.corners[i].plane_ids[0] == 0);
        }
    }
    v7_bc_outputs_free(&out);
}

// Wall-bbox below WALL_TILT_MIN gate → no synthetic corners.
// Spec: D.0 only fires when tilt_deg ≥ wall_tilt_min_deg.
static void test_flat_plane_no_wall_bbox(void) {
    const int NSIDE = 8;
    const int N = NSIDE * NSIDE;
    double positions[8 * 8 * 3];
    int16_t owner[8 * 8];
    fill_wall_patch(positions, owner, NSIDE, 0);  // points on x=0

    v7_bc_input_plane_t plane;
    make_flat_plane(&plane, 0, 5.0);
    // Tilt=0 (horizontal) → wall-bbox gate refuses.

    v7_bc_params_t p; default_params(&p);
    v7_bc_outputs_t out; v7_bc_outputs_init(&out);
    int rc = v7_bc_compute(positions, N, owner, &plane, 1, NULL, 0, NULL, 0, NULL, 0, &p, &out);
    CHECK(rc == 0);
    CHECK(out.wall_corners_added == 0);
    CHECK(out.n_corners == 0);
    v7_bc_outputs_free(&out);
}

// Rejected planes contribute no anchors, no wall corners, no grids.
// Spec: the per-plane sweep early-exits on rejected==true.
static void test_rejected_plane_skipped(void) {
    const int NSIDE = 8;
    const int N = NSIDE * NSIDE;
    double positions[8 * 8 * 3];
    int16_t owner[8 * 8];
    fill_wall_patch(positions, owner, NSIDE, 0);

    v7_bc_input_plane_t planes[2];
    make_wall_plane(&planes[0], 0);
    make_wall_plane(&planes[1], 1);
    planes[0].rejected = true;
    planes[1].rejected = true;

    v7_bc_params_t p; default_params(&p);
    v7_bc_outputs_t out; v7_bc_outputs_init(&out);
    int rc = v7_bc_compute(positions, N, owner, planes, 2, NULL, 0, NULL, 0, NULL, 0, &p, &out);
    CHECK(rc == 0);
    CHECK(out.wall_corners_added == 0);
    CHECK(out.n_corners == 0);
    v7_bc_outputs_free(&out);
}

// Source priority: when an RI seed and a concave seed are spatially
// near (within pool_merge_dist), the merged pool entry's CACHED source
// is the highest-priority of its members. RI (priority 4) > concave
// (priority 1), so the entry surfaces source=RI.
// Spec: recompute_pool_entry picks best-priority member after each merge.
static void test_source_priority_ri_beats_concave(void) {
    v7_bc_input_plane_t planes[2];
    make_flat_plane(&planes[0], 0, 5.0);
    make_flat_plane(&planes[1], 1, 5.0);

    // Two seeds at the SAME location: one RI, one concave. They
    // collide within pool_merge_dist (0.20m).
    uint32_t pids_ri[2] = { 0, 1 };
    uint32_t rids[1]    = { 0 };
    uint32_t pids_cv[1] = { 0 };
    v7_bc_input_corner_t seeds[2];
    make_ri_seed(&seeds[0], 0, 1.0, 5.0, 2.0, pids_ri, 2, rids, 1, NULL);

    // Concave seed: same point, marked as concave.
    memset(&seeds[1], 0, sizeof(seeds[1]));
    seeds[1].id = 1;
    seeds[1].source = V7P_SRC_CONCAVE;
    seeds[1].point[0] = 1.0; seeds[1].point[1] = 5.0; seeds[1].point[2] = 2.0;
    seeds[1].plane_ids = pids_cv;
    seeds[1].n_plane_ids = 1;

    v7_bc_params_t p; default_params(&p);
    v7_bc_outputs_t out; v7_bc_outputs_init(&out);
    int rc = v7_bc_compute(NULL, 0, NULL, planes, 2, NULL, 0, NULL, 0, seeds, 2, &p, &out);
    CHECK(rc == 0);
    CHECK(out.n_corners == 1);
    if (out.n_corners == 1) {
        // The cached source equals the highest-priority member.
        CHECK(out.corners[0].source == V7P_SRC_RIDGE_INTERSECTION);
    }
    v7_bc_outputs_free(&out);
}

// Wall-bbox path on too-small a patch (h-extent < wall_min_h_extent
// = 0.50m) emits no corners.
// Spec: D.0 also gates on h_max - h_min ≥ wall_min_h_extent.
static void test_wall_bbox_tiny_patch_rejected(void) {
    // 3 points along a tiny 0.1m strip on plane x=0.
    double positions[9] = {
        0.0, 0.0, 0.00,
        0.0, 0.0, 0.05,
        0.0, 0.0, 0.10,
    };
    int16_t owner[3] = { 0, 0, 0 };

    v7_bc_input_plane_t plane; make_wall_plane(&plane, 0);

    v7_bc_params_t p; default_params(&p);
    v7_bc_outputs_t out; v7_bc_outputs_init(&out);
    int rc = v7_bc_compute(positions, 3, owner, &plane, 1, NULL, 0, NULL, 0, NULL, 0, &p, &out);
    CHECK(rc == 0);
    // Patch h-extent (0.10m in z) < wall_min_h_extent (0.50m) → no corners.
    CHECK(out.wall_corners_added == 0);
    CHECK(out.n_corners == 0);
    v7_bc_outputs_free(&out);
}

// ─── on-ridge property + recompute invariants (added 2026-05-26) ──────
//
// Tests for the post-#9ffb041 invariants:
//   - anchor.on_ridge_ids[] is the canonical "this anchor sits on these
//     ridges by construction" set; anchor_on_ridge() is a pure property
//     query (no FP geometry).
//   - v7p_recompute_entry derives e->source from max(source_per_plane[]),
//     NOT from members[i].source — closes the BM full-tile
//     v7p_entry_validate assertion + the BC FP-coin-flip class.
//   - Coplanar-import path runs recompute after expanding plane_ids /
//     source_per_plane on an existing pool entry.

// anchor_on_ridge: pure property check, no FP involved.
static void test_anchor_on_ridge_property_query(void) {
    anchor_t a = {0};
    // Empty set: returns false for any ridge.
    CHECK(!anchor_on_ridge(&a, 0));
    CHECK(!anchor_on_ridge(&a, 7));

    anchor_push_on_ridge(&a, 3);
    anchor_push_on_ridge(&a, 5);
    anchor_push_on_ridge(&a, 3);  // dup, no-op

    CHECK(a.n_on_ridge_ids == 2);
    CHECK(anchor_on_ridge(&a, 3));
    CHECK(anchor_on_ridge(&a, 5));
    CHECK(!anchor_on_ridge(&a, 4));
    CHECK(!anchor_on_ridge(&a, 0));

    anchor_free(&a);
}

// v7p_recompute_entry derives e->source from max(source_per_plane[]),
// not from members[i].source. Construct an entry whose member has a
// LOWER-priority source than its per-plane slots — recompute must take
// the per-plane max, not the member source. This is the BM full-tile
// invariant bug (closed in 9ffb041).
static void test_recompute_source_from_source_per_plane(void) {
    v7p_entry_t e = {0};
    e.id = 0;
    e.x = e.y = e.z = 0.0;

    // Member with low-priority source (concave = 1).
    v7p_member_t m = {0};
    m.source = V7P_SRC_CONCAVE;
    m.authority = V7P_AUTHORITY_FULL;
    CHECK(v7p_entry_push_member(&e, &m) == 0);

    // Register two planes: one concave, one ridge-end (priority 3).
    CHECK(v7p_entry_register_plane(&e, 4, V7P_SRC_CONCAVE) == 0);
    CHECK(v7p_entry_register_plane(&e, 7, V7P_SRC_RIDGE_END) == 0);

    // Force recompute. Pre-fix, this would set e->source = max(member.source)
    // = concave(1), violating the invariant pri(e->source) == pri(max(spp)).
    // Post-fix: e->source = ridge-end(3), matching the per-plane max.
    v7p_recompute_entry(&e);
    CHECK(e.source == V7P_SRC_RIDGE_END);
    CHECK(v7p_src_priority(e.source) ==
          v7p_src_priority(e.source_per_plane[1]));

    v7p_entry_free(&e);
}

// register_plane on an already-registered plane with a higher-priority
// source bumps source_per_plane[i] AND e->source. Recompute afterwards
// must still find consistency.
static void test_register_plane_bumps_then_recompute_consistent(void) {
    v7p_entry_t e = {0};
    e.x = e.y = e.z = 0.0;
    v7p_member_t m = {0};
    m.source = V7P_SRC_CONCAVE;
    m.authority = V7P_AUTHORITY_FULL;
    CHECK(v7p_entry_push_member(&e, &m) == 0);

    CHECK(v7p_entry_register_plane(&e, 4, V7P_SRC_CONCAVE) == 0);
    CHECK(e.source == V7P_SRC_CONCAVE);

    // Re-register plane 4 with higher-priority ridge-intersection: slot
    // bumps to RI, e->source bumps to RI.
    CHECK(v7p_entry_register_plane(&e, 4, V7P_SRC_RIDGE_INTERSECTION) == 0);
    CHECK(e.source_per_plane[0] == V7P_SRC_RIDGE_INTERSECTION);
    CHECK(e.source == V7P_SRC_RIDGE_INTERSECTION);

    // Recompute does NOT regress e->source back to the member's source.
    v7p_recompute_entry(&e);
    CHECK(e.source == V7P_SRC_RIDGE_INTERSECTION);

    v7p_entry_free(&e);
}

// D.2b property gate: an anchor that sits on a ridge by construction
// (on_ridge_ids[] contains it) must be skipped from the side test,
// regardless of which side the FP geometric test would assign. We
// drive this through v7_bc_compute with an RI seed and a ridge whose
// side-vector would otherwise kill it. Expected: corner survives.
static void test_d2b_property_gate_protects_on_ridge_anchor(void) {
    // Two planes meeting at a ridge. Both flat (tilt 0) → no D.0 path.
    v7_bc_input_plane_t planes[2];
    make_flat_plane(&planes[0], 0, 5.0);
    make_flat_plane(&planes[1], 1, 5.0);

    // One ridge line, planes 0 and 1, line along +z.
    v7_bc_input_ridge_line_t ridge = {0};
    ridge.plane_a = 0; ridge.plane_b = 1;
    ridge.has_line = true; ridge.has_side_a = true; ridge.has_side_b = true;
    ridge.line_point[0] = 0; ridge.line_point[1] = 0; ridge.line_point[2] = 0;
    ridge.line_direction[0] = 0; ridge.line_direction[1] = 0; ridge.line_direction[2] = 1;
    ridge.t_min_a = -10; ridge.t_max_a = 10;
    ridge.t_min_b = -10; ridge.t_max_b = 10;
    // Per-plane interior-oriented axes: plane-0's body is on −x, plane-1's on
    // +x. A plane-0 anchor at x>0 would be wrong-side (dot < 0) and killed —
    // but the RI seed sits at x=0 ON the ridge, so the property gate must skip
    // the side test entirely before any sign is taken.
    ridge.side_vec_a[0] = -1; ridge.side_vec_a[1] = 0; ridge.side_vec_a[2] = 0;
    ridge.side_vec_b[0] = +1; ridge.side_vec_b[1] = 0; ridge.side_vec_b[2] = 0;

    // RI seed exactly on the ridge (x=y=0). Side test on this ridge
    // would coin-flip; the property gate must skip it.
    uint32_t pids[2] = { 0, 1 };
    uint32_t rids[1] = { 0 };
    v7_bc_input_corner_t seed;
    make_ri_seed(&seed, 0, 0.0, 0.0, 2.0, pids, 2, rids, 1, NULL);

    v7_bc_params_t p; default_params(&p);
    v7_bc_outputs_t out; v7_bc_outputs_init(&out);
    int rc = v7_bc_compute(NULL, 0, NULL, planes, 2, &ridge, 1, NULL, 0,
                            &seed, 1, &p, &out);
    CHECK(rc == 0);
    // RI sits on the ridge by construction → property gate skips D.2b
    // side test → RI survives. D.1b additionally synthesizes ridge-end
    // anchors at the two uncovered endpoints (also on-ridge → both also
    // survive). We assert specifically that the RI is in the output;
    // the ridge-ends are a side-effect of the synthetic setup.
    bool ri_survived = false;
    for (size_t i = 0; i < out.n_corners; i++) {
        if (out.corners[i].source == V7P_SRC_RIDGE_INTERSECTION
                && fabs(out.corners[i].point[0] - 0.0) < 1e-9
                && fabs(out.corners[i].point[1] - 0.0) < 1e-9
                && fabs(out.corners[i].point[2] - 2.0) < 1e-9) {
            ri_survived = true;
        }
    }
    CHECK(ri_survived);
    // Pre-fix (before the property gate), the RI at perp=0 to its own
    // ridge would coin-flip the side test → 50/50 killed. With the
    // property gate, kill is impossible regardless of FP residue.
    CHECK(out.n_corners >= 1);
    v7_bc_outputs_free(&out);
}

// ─── main ───────────────────────────────────────────────────────────

int main(void) {
    test_empty_input_no_crash();
    test_no_seeds_no_corners();
    test_single_ri_seed_preserved();
    test_ri_seeds_within_merge_dist_dedup();
    test_ri_seeds_beyond_merge_dist_separate();
    test_corner_ids_dense_and_unique();
    test_plane_corner_ids_consistent();
    test_determinism_same_inputs_same_outputs();
    test_wall_bbox_emits_four_corners();
    test_flat_plane_no_wall_bbox();
    test_rejected_plane_skipped();
    test_source_priority_ri_beats_concave();
    test_wall_bbox_tiny_patch_rejected();
    test_anchor_on_ridge_property_query();
    test_recompute_source_from_source_per_plane();
    test_register_plane_bumps_then_recompute_consistent();
    test_d2b_property_gate_protects_on_ridge_anchor();

    printf("%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// core/tests/v7_polygon_primitives_test.c
//
// Implementation tests for the shared 2D polygon primitives
// (v7-polygon-primitives.c): ray-cast point-in-polygon and
// point-to-edge distance. These are pure geometry helpers with
// analytic ground truth, so every assertion is exact or to a tight
// float tolerance — no spec/oracle dependency.
//
// White-box convention (docs/quality/V7-TEST-STRATEGY.md §3.3.1): the
// impl .c is #included directly; the foreach links dioramap_steps_normals
// + m. polygon-primitives needs only libm, but the uniform link is
// harmless.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "v7-polygon-primitives.c"

// ─── harness ────────────────────────────────────────────────────────
static int g_checks = 0; static int g_failed = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failed++; } \
} while (0)

#define CHECK_CLOSE(a, b, eps) do { \
    double _a = (a), _b = (b); \
    g_checks++; \
    if (fabs(_a - _b) > (eps)) { \
        fprintf(stderr, "FAIL %s:%d: %s ≈ %s (got %.12g, want %.12g, eps=%g)\n", \
            __FILE__, __LINE__, #a, #b, _a, _b, (eps)); \
        g_failed++; \
    } \
} while (0)

// Unit square CCW: (0,0) (1,0) (1,1) (0,1).
static const double SQ_U[4] = { 0.0, 1.0, 1.0, 0.0 };
static const double SQ_V[4] = { 0.0, 0.0, 1.0, 1.0 };

// ─── point-in-polygon ───────────────────────────────────────────────

// Interior points test true; exterior points (on every side) test false.
static void test_pip_square_basic(void) {
    CHECK(v7_pt_in_poly(0.5, 0.5, SQ_U, SQ_V, 4) == true);
    CHECK(v7_pt_in_poly(0.01, 0.01, SQ_U, SQ_V, 4) == true);
    CHECK(v7_pt_in_poly(0.99, 0.99, SQ_U, SQ_V, 4) == true);

    CHECK(v7_pt_in_poly(1.5,  0.5, SQ_U, SQ_V, 4) == false);  // right
    CHECK(v7_pt_in_poly(-0.5, 0.5, SQ_U, SQ_V, 4) == false);  // left
    CHECK(v7_pt_in_poly(0.5,  1.5, SQ_U, SQ_V, 4) == false);  // above
    CHECK(v7_pt_in_poly(0.5, -0.5, SQ_U, SQ_V, 4) == false);  // below
    CHECK(v7_pt_in_poly(5.0,  5.0, SQ_U, SQ_V, 4) == false);  // far
}

// Winding-rule independence: the ray cast must give the same answer for a
// polygon listed clockwise as counter-clockwise.
static void test_pip_orientation_independent(void) {
    const double cwU[4] = { 0.0, 0.0, 1.0, 1.0 };
    const double cwV[4] = { 0.0, 1.0, 1.0, 0.0 };
    CHECK(v7_pt_in_poly(0.5, 0.5, cwU, cwV, 4) == true);
    CHECK(v7_pt_in_poly(1.5, 0.5, cwU, cwV, 4) == false);
}

// Concave L-shape: the re-entrant notch is OUTSIDE even though it lies
// within the bounding box. Polygon (CCW):
//   (0,0)(2,0)(2,1)(1,1)(1,2)(0,2)  — an L occupying the lower and left.
static void test_pip_concave_L(void) {
    const double lu[6] = { 0.0, 2.0, 2.0, 1.0, 1.0, 0.0 };
    const double lv[6] = { 0.0, 0.0, 1.0, 1.0, 2.0, 2.0 };
    CHECK(v7_pt_in_poly(0.5, 0.5, lu, lv, 6) == true);   // lower-left arm
    CHECK(v7_pt_in_poly(1.5, 0.5, lu, lv, 6) == true);   // lower-right arm
    CHECK(v7_pt_in_poly(0.5, 1.5, lu, lv, 6) == true);   // upper-left arm
    CHECK(v7_pt_in_poly(1.5, 1.5, lu, lv, 6) == false);  // the notch (outside)
}

// ─── point-to-edge distance ─────────────────────────────────────────

// Interior point: distance is to the nearest of the 4 sides.
static void test_edge_dist_interior(void) {
    CHECK_CLOSE(v7_pt_edge_dist(0.5, 0.5, SQ_U, SQ_V, 4), 0.5, 1e-12);
    // Closer to the bottom edge.
    CHECK_CLOSE(v7_pt_edge_dist(0.5, 0.1, SQ_U, SQ_V, 4), 0.1, 1e-12);
    CHECK_CLOSE(v7_pt_edge_dist(0.2, 0.5, SQ_U, SQ_V, 4), 0.2, 1e-12);
}

// Point on an edge → distance 0.
static void test_edge_dist_on_edge(void) {
    CHECK_CLOSE(v7_pt_edge_dist(0.5, 0.0, SQ_U, SQ_V, 4), 0.0, 1e-12);
    CHECK_CLOSE(v7_pt_edge_dist(1.0, 0.5, SQ_U, SQ_V, 4), 0.0, 1e-12);
}

// Exterior point off a face → perpendicular distance to that face.
static void test_edge_dist_exterior_face(void) {
    CHECK_CLOSE(v7_pt_edge_dist(2.0, 0.5, SQ_U, SQ_V, 4), 1.0, 1e-12);  // right of x=1
    CHECK_CLOSE(v7_pt_edge_dist(0.5, 3.0, SQ_U, SQ_V, 4), 2.0, 1e-12);  // above y=1
}

// Exterior point off a corner → clamps to the vertex (Euclidean to corner).
static void test_edge_dist_exterior_corner(void) {
    // Beyond corner (0,0): nearest point on either incident edge clamps to
    // the shared vertex, so distance is |(-1,-1)| = sqrt(2).
    CHECK_CLOSE(v7_pt_edge_dist(-1.0, -1.0, SQ_U, SQ_V, 4), sqrt(2.0), 1e-12);
    // Beyond corner (1,1): distance to (2,2) is sqrt(2).
    CHECK_CLOSE(v7_pt_edge_dist(2.0, 2.0, SQ_U, SQ_V, 4), sqrt(2.0), 1e-12);
}

// Degenerate edges (zero length) are skipped, not divided-by-zero: a square
// with a repeated vertex still yields the correct distance.
static void test_edge_dist_degenerate_edge_skipped(void) {
    const double du[5] = { 0.0, 1.0, 1.0, 1.0, 0.0 };  // (1,0) repeated
    const double dv[5] = { 0.0, 0.0, 0.0, 1.0, 1.0 };
    double d = v7_pt_edge_dist(0.5, 0.5, du, dv, 5);
    CHECK(isfinite(d));
    CHECK_CLOSE(d, 0.5, 1e-12);
}

int main(void) {
    test_pip_square_basic();
    test_pip_orientation_independent();
    test_pip_concave_L();
    test_edge_dist_interior();
    test_edge_dist_on_edge();
    test_edge_dist_exterior_face();
    test_edge_dist_exterior_corner();
    test_edge_dist_degenerate_edge_skipped();
    printf("%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}

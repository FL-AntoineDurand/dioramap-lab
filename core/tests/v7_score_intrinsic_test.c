// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// core/tests/v7_score_intrinsic_test.c
//
// Implementation tests for v7-score-intrinsic. Asserts the math
// invariants the algorithm spec promises — completeness ratio, residual
// RMS, composite redistribution over finite metrics, and pack/unpack
// round-trip preservation.
//
// Spec source: docs/quality/SCORING-DESIGN.md §6.2 + the per-function
// header comments in steps/v7/v7-score-intrinsic-impl.h.
//
// v2 origin: lib/metrics/index.js::computeNoGTMetrics (no separate
// .test.js; this implementation is the v3 spec).
//
// Implementation reference: v3 steps/v7/v7-score-intrinsic-impl.c.
//
// We include the impl source directly so the test reaches its internals
// without linking dioramap_steps_v7 (which would duplicate symbols).
// Matches the convention in v7_normal_cascade_seed_test.c.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Pull the impl into this TU. No helper sources needed — the impl is
// self-contained (just stdlib + math).
#include "v7-score-intrinsic-impl.c"

static int n_pass = 0, n_fail = 0;

#define CHECK(cond, msg) do {                                                  \
    if (cond) { n_pass++; }                                                    \
    else { fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); n_fail++; } \
} while (0)

#define CHECK_NEAR(actual, expected, tol, msg) do {                            \
    double _a = (actual), _e = (expected), _t = (tol);                         \
    if (fabs(_a - _e) <= _t) { n_pass++; }                                     \
    else {                                                                     \
        fprintf(stderr, "FAIL: %s (actual=%g expected=%g tol=%g, line %d)\n",  \
                msg, _a, _e, _t, __LINE__);                                    \
        n_fail++;                                                              \
    }                                                                          \
} while (0)

// 1. NULL-positions returns -1; n=0 with valid pointers returns 0 +
//    completeness=0 (no points → no work, sensible defaults).
static void test_degenerate_inputs(void) {
    v7_score_intrinsic_result_t r;
    int rc = v7_score_intrinsic_compute(/*positions=*/NULL,
                                          /*owner=*/NULL,
                                          /*n_pts=*/0,
                                          /*planes=*/NULL,
                                          /*n_planes=*/0, &r);
    CHECK(rc == -1, "compute(NULL positions, NULL owner) returns -1");

    double pos[3] = {0};
    int16_t own[1] = {-1};
    rc = v7_score_intrinsic_compute(pos, own, /*n=*/0, NULL, 0, &r);
    CHECK(rc == 0, "compute(n=0) returns 0");
    CHECK(r.completeness == 0.0, "completeness = 0 for empty input");
    CHECK(r.n_points_assigned == 0, "n_points_assigned = 0");
    CHECK(r.version == V7_SCORE_INTRINSIC_VERSION, "version stamped");
}

// 2. All owners -1 (none assigned) with n_planes > 0 → completeness 0,
//    residual_rms NaN (no points to sum), residual_score NaN per spec.
static void test_all_unassigned(void) {
    double pos[12] = {0,0,0, 1,0,0, 0,1,0, 1,1,0};
    int16_t own[4] = {-1,-1,-1,-1};
    v7_score_plane_t pl = {.normal={0,0,1}, .d=0};
    v7_score_intrinsic_result_t r;

    int rc = v7_score_intrinsic_compute(pos, own, 4, &pl, 1, &r);
    CHECK(rc == 0, "compute returns 0");
    CHECK(r.completeness == 0.0, "0/4 assigned → completeness = 0");
    CHECK(r.n_points_assigned == 0, "n_assigned = 0");
    CHECK(r.residual_rms < 0.0,   "residual_rms = V7_SCORE_NA (no assigned points)");
    CHECK(r.residual_score < 0.0, "residual_score = V7_SCORE_NA (planes exist, no inliers)");
}

// 3. Perfect fit — 4 coplanar points on z=0 owned by plane (0,0,1) d=0.
//    completeness 1, residual 0, residual_score 1, composite 100.
static void test_perfect_fit(void) {
    double pos[12] = {0,0,0, 1,0,0, 0,1,0, 1,1,0};
    int16_t own[4] = {0,0,0,0};
    v7_score_plane_t pl = {.normal={0,0,1}, .d=0};
    v7_score_intrinsic_result_t r;

    int rc = v7_score_intrinsic_compute(pos, own, 4, &pl, 1, &r);
    CHECK(rc == 0, "compute returns 0");
    CHECK(r.completeness == 1.0, "4/4 assigned → completeness = 1");
    CHECK_NEAR(r.residual_rms, 0.0, 1e-12, "perfect fit → RMS = 0");
    CHECK_NEAR(r.residual_score, 1.0, 1e-12, "perfect fit → score = 1");
    // composite = weighted blend over (residual_score=1, completeness=1)
    //           = (12*1 + 8*1) / (12+8) * 100 = 100.
    CHECK_NEAR(r.composite, 100.0, 1e-9, "perfect fit → composite 100");
}

// 4. Single outlier — same as test 3 but one point lifted 0.10 m.
//    RMS = sqrt(0.01/4) = 0.05; residual_score = 1 - 0.05/0.15 ≈ 0.667.
static void test_with_outlier(void) {
    double pos[12] = {0,0,0, 1,0,0, 0,1,0, 1,1,0.10};
    int16_t own[4] = {0,0,0,0};
    v7_score_plane_t pl = {.normal={0,0,1}, .d=0};
    v7_score_intrinsic_result_t r;

    int rc = v7_score_intrinsic_compute(pos, own, 4, &pl, 1, &r);
    CHECK(rc == 0, "compute returns 0");
    CHECK_NEAR(r.residual_rms, 0.05, 1e-12, "RMS = 0.05 m");
    CHECK_NEAR(r.residual_score, 1.0 - 0.05 / 0.15, 1e-12, "score = 1 - 0.05/0.15");
    // composite = (12*(1-0.05/0.15) + 8*1) / 20 * 100
    //           = (12*0.6667 + 8) / 20 * 100 = 80.0.
    CHECK_NEAR(r.composite, 80.0, 1e-9, "composite ≈ 80");
}

// 5. Out-of-range owner — pid >= n_planes is defensively treated as
//    unassigned. (Mirrors v3's "don't trust the upstream owner blindly"
//    discipline.)
static void test_owner_out_of_range(void) {
    double pos[6] = {0,0,0, 1,0,0};
    int16_t own[2] = {5, -1};
    v7_score_plane_t pl = {.normal={0,0,1}, .d=0};
    v7_score_intrinsic_result_t r;

    int rc = v7_score_intrinsic_compute(pos, own, 2, &pl, 1, &r);
    CHECK(rc == 0, "compute returns 0");
    CHECK(r.n_points_assigned == 0, "out-of-range + -1 → both unassigned");
    CHECK(r.completeness == 0.0, "completeness = 0");
}

// 6. Pack/unpack roundtrip — every field is preserved exactly, including
//    NaN sentinels for the not-yet-implemented rasterisation metrics.
static void test_pack_roundtrip(void) {
    v7_score_intrinsic_result_t r = {0};
    r.version = V7_SCORE_INTRINSIC_VERSION;
    r.n_planes = 3;
    r.n_points_total = 100;
    r.n_points_assigned = 87;
    r.completeness = 0.87;
    r.residual_rms = 0.042;
    r.residual_score = 0.72;
    r.footprint_coverage  = V7_SCORE_NA;
    r.footprint_overshoot = V7_SCORE_NA;
    r.plane_overlap       = V7_SCORE_NA;
    r.composite = 73.4;

    uint8_t buf[V7_SCORE_INTRINSIC_RECORD_BYTES];
    size_t bytes = v7_score_intrinsic_pack(&r, buf);
    CHECK(bytes == V7_SCORE_INTRINSIC_RECORD_BYTES, "pack returns 72");

    v7_score_intrinsic_result_t r2;
    int rc = v7_score_intrinsic_unpack(buf, sizeof(buf), &r2);
    CHECK(rc == 0, "unpack returns 0");
    CHECK(r2.version == r.version, "version preserved");
    CHECK(r2.n_planes == r.n_planes, "n_planes preserved");
    CHECK(r2.n_points_total == r.n_points_total, "n_points_total preserved");
    CHECK(r2.n_points_assigned == r.n_points_assigned, "n_points_assigned preserved");
    CHECK(r2.completeness == r.completeness, "completeness preserved bit-for-bit");
    CHECK(r2.residual_rms == r.residual_rms, "residual_rms preserved");
    CHECK(r2.residual_score == r.residual_score, "residual_score preserved");
    CHECK(r2.footprint_coverage  == V7_SCORE_NA, "V7_SCORE_NA preserved: footprint_coverage");
    CHECK(r2.footprint_overshoot == V7_SCORE_NA, "V7_SCORE_NA preserved: footprint_overshoot");
    CHECK(r2.plane_overlap       == V7_SCORE_NA, "V7_SCORE_NA preserved: plane_overlap");
    CHECK(r2.composite == r.composite, "composite preserved");

    // Short-buffer unpack rejected.
    rc = v7_score_intrinsic_unpack(buf, /*len=*/10, &r2);
    CHECK(rc == -1, "unpack(len<72) returns -1");
    // NULL pointers rejected.
    rc = v7_score_intrinsic_unpack(/*buf=*/NULL, sizeof(buf), &r2);
    CHECK(rc == -1, "unpack(NULL buf) returns -1");
}

// 7. n_planes > INT16_MAX rejected (owner is int16; larger ids would
//    overflow). Spec invariant from impl.h.
static void test_too_many_planes(void) {
    v7_score_intrinsic_result_t r;
    double pos[3] = {0};
    int16_t own[1] = {-1};
    // We don't actually allocate 32K planes — we just hand NULL with
    // n_planes > INT16_MAX which trips the bounds check before any
    // dereference. (Documented behaviour: compute requires n_planes ≤ INT16_MAX.)
    int rc = v7_score_intrinsic_compute(pos, own, 1,
                                          /*planes=*/NULL,
                                          /*n_planes=*/(size_t)INT16_MAX + 1, &r);
    CHECK(rc == -1, "n_planes > INT16_MAX returns -1");
}

int main(void) {
    test_degenerate_inputs();
    test_all_unassigned();
    test_perfect_fit();
    test_with_outlier();
    test_owner_out_of_range();
    test_pack_roundtrip();
    test_too_many_planes();
    printf("v7_score_intrinsic_test: %d pass, %d fail\n", n_pass, n_fail);
    return n_fail == 0 ? 0 : 1;
}

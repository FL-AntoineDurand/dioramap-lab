// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// core/tests/v7_perbldg_gather_test.c
//
// Unit tests for v7_gather_building_points_f32_f64 — the helper
// extracted in task #72a from the 5 v7-step per-building gather sites.
// Covers the four corner cases the duplicated inline code had to handle
// (and one the helper newly handles correctly: realloc-failure cleanup).

#include "v7-perbldg-gather.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// Case A: cold call (no pre-existing buffers, n_pts > 0).
static void test_cold_call(void) {
    double positions[] = {
        // 4 points × 3 doubles each; centroid will subtract (1,2,3)
        1.0, 2.0, 3.0,
        2.0, 4.0, 6.0,
        4.0, 8.0, 12.0,
        8.0, 16.0, 24.0,
    };
    uint32_t gidx[] = {0, 1, 2, 3};
    float*  f32 = NULL;
    double* f64 = NULL;
    size_t  cap = 0;

    int rc = v7_gather_building_points_f32_f64(
        positions, 4, gidx, 4, /*cx*/1.0, /*cy*/2.0, /*cz*/3.0,
        &f32, &f64, &cap);
    CHECK(rc == 0);
    CHECK(cap == 12);          // 4 pts × 3 = 12 F32 slots
    CHECK(f32 != NULL && f64 != NULL);

    // First point becomes origin (1-1, 2-2, 3-3).
    CHECK(f32[0] == 0.0f && f32[1] == 0.0f && f32[2] == 0.0f);
    // Last point (8,16,24) → (7,14,21).
    CHECK(f32[9] == 7.0f && f32[10] == 14.0f && f32[11] == 21.0f);
    // F64 is the exact F32→F64 promotion (not the raw double from positions).
    CHECK(f64[0] == 0.0 && f64[11] == 21.0);

    free(f32); free(f64);
}

// Case B: warm call grows the buffer (re-used across buildings).
static void test_warm_grow(void) {
    double positions[3 * 8];
    for (int i = 0; i < 8; i++) {
        positions[3 * i + 0] = (double)i;
        positions[3 * i + 1] = (double)i * 2;
        positions[3 * i + 2] = (double)i * 3;
    }
    uint32_t small_idx[] = {0, 1};
    uint32_t large_idx[] = {0, 1, 2, 3, 4, 5, 6, 7};
    float*  f32 = NULL;
    double* f64 = NULL;
    size_t  cap = 0;

    int rc = v7_gather_building_points_f32_f64(
        positions, 8, small_idx, 2, 0.0, 0.0, 0.0,
        &f32, &f64, &cap);
    CHECK(rc == 0 && cap == 6);
    float*  f32_after_first = f32;
    (void)f32_after_first;

    // Grow to 8 points → cap should be 24.
    rc = v7_gather_building_points_f32_f64(
        positions, 8, large_idx, 8, 0.0, 0.0, 0.0,
        &f32, &f64, &cap);
    CHECK(rc == 0 && cap == 24);
    // Last point (7,14,21) preserved.
    CHECK(f32[3 * 7 + 0] == 7.0f && f32[3 * 7 + 2] == 21.0f);

    // Shrinking call (n_pts smaller) should NOT shrink the buffer.
    rc = v7_gather_building_points_f32_f64(
        positions, 8, small_idx, 2, 0.0, 0.0, 0.0,
        &f32, &f64, &cap);
    CHECK(rc == 0 && cap == 24);  // still 24 — no realloc-shrink

    free(f32); free(f64);
}

// Case C: OOB gidx entries become (0,0,0).
static void test_oob_gidx_writes_zero(void) {
    double positions[] = { 10.0, 20.0, 30.0, 40.0, 50.0, 60.0 };  // 2 pts
    uint32_t gidx[] = { 0, 99 /* OOB */, 1, 100 /* OOB */ };
    float*  f32 = NULL;
    double* f64 = NULL;
    size_t  cap = 0;

    int rc = v7_gather_building_points_f32_f64(
        positions, 2, gidx, 4, 0.0, 0.0, 0.0,
        &f32, &f64, &cap);
    CHECK(rc == 0);
    // p0: pt 0 → (10, 20, 30)
    CHECK(f32[0] == 10.0f && f32[1] == 20.0f && f32[2] == 30.0f);
    // p1: OOB → zeros
    CHECK(f32[3] == 0.0f && f32[4] == 0.0f && f32[5] == 0.0f);
    // p2: pt 1 → (40, 50, 60)
    CHECK(f32[6] == 40.0f && f32[7] == 50.0f && f32[8] == 60.0f);
    // p3: OOB → zeros
    CHECK(f32[9] == 0.0f && f32[10] == 0.0f && f32[11] == 0.0f);

    free(f32); free(f64);
}

// Case D: n_pts == 0 should leave buffers untouched + succeed (no allocs).
static void test_empty_n_pts(void) {
    float*  f32 = NULL;
    double* f64 = NULL;
    size_t  cap = 0;

    int rc = v7_gather_building_points_f32_f64(
        NULL, 0, NULL, 0, 0.0, 0.0, 0.0,
        &f32, &f64, &cap);
    CHECK(rc == 0);
    CHECK(f32 == NULL && f64 == NULL && cap == 0);
}

// Case E: ensures *local_f32 is always free()-safe — the no-double-free
// invariant the helper newly guarantees (the pre-#72a inline code had a
// dangling-pointer bug if the F64 realloc failed after F32 succeeded).
// We can't easily inject realloc failure portably, so this is a smoke
// test: many cold+warm calls, all freed at the end. ASan would catch a
// double-free.
static void test_no_double_free_on_repeat(void) {
    double positions[3 * 4] = {1,2,3, 4,5,6, 7,8,9, 10,11,12};
    uint32_t idx[] = {0, 1, 2, 3};
    float*  f32 = NULL;
    double* f64 = NULL;
    size_t  cap = 0;

    for (int round = 0; round < 5; round++) {
        int rc = v7_gather_building_points_f32_f64(
            positions, 4, idx, 4, 0.0, 0.0, 0.0,
            &f32, &f64, &cap);
        CHECK(rc == 0);
    }
    free(f32); free(f64);  // ASan would flag any double-free here
}

int main(void) {
    test_cold_call();
    test_warm_grow();
    test_oob_gidx_writes_zero();
    test_empty_n_pts();
    test_no_double_free_on_repeat();
    fprintf(stderr, "%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

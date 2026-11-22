// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// core/tests/v7_rg_parse_test.c
//
// Unit tests for rg_parse_padded_u32_array — the helper extracted in
// task #72b from the 4 corner-record parsers that had the same
// 8-byte-aligned `count, alloc, memcpy` for ridge_ids.

#include "v7-roof-graph-format.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;
#define CHECK(cond) do { \
    if (cond) g_pass++; else { g_fail++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// Construct a record buffer with a padded count + array, parse it, verify.
static void test_nonempty(void) {
    // Record shape: [0..7] padded count=3, then 3 × u32 ids, then 4 bytes pad
    // to land at the next 8-aligned offset (count*4 = 12, +4 pad = 16 total).
    uint8_t rec[24] = {0};
    uint32_t count = 3;
    memcpy(rec + 0, &count, 4);            // count at off 0..3
    // rec[4..7] = pad (already zero)
    uint32_t ids[3] = {0x11111111, 0x22222222, 0x33333333};
    memcpy(rec + 8, ids, 12);              // ids at off 8..19

    size_t off = 0;
    uint32_t* out_arr = NULL;
    uint32_t  out_n   = 0xdeadbeef;

    int rc = rg_parse_padded_u32_array(rec, &off, &out_arr, &out_n);
    CHECK(rc == 0);
    CHECK(out_n == 3);
    CHECK(out_arr != NULL);
    if (out_arr) {
        CHECK(out_arr[0] == 0x11111111);
        CHECK(out_arr[1] == 0x22222222);
        CHECK(out_arr[2] == 0x33333333);
    }
    // off should be aligned to 8 after consuming 8 (header) + 12 (3 × u32) = 20,
    // post-aligned → 24.
    CHECK(off == 24);
    free(out_arr);
}

static void test_empty(void) {
    // count = 0 → out_arr = NULL, out_n = 0, off advances by 8 (header only).
    uint8_t rec[16] = {0};   // count=0 at off 0; no array bytes after
    size_t off = 0;
    uint32_t* out_arr = (uint32_t*)0x1234;  // garbage sentinel
    uint32_t  out_n   = 99;
    int rc = rg_parse_padded_u32_array(rec, &off, &out_arr, &out_n);
    CHECK(rc == 0);
    CHECK(out_n == 0);
    CHECK(out_arr == NULL);   // helper must overwrite, not preserve sentinel
    CHECK(off == 8);
}

static void test_pre_alignment(void) {
    // Start with off=3 (not 8-aligned). Helper should pre-align to 8 before
    // reading the count.
    uint8_t rec[40] = {0};
    uint32_t count = 2;
    memcpy(rec + 8, &count, 4);            // count starts at 8 after pre-align
    uint32_t ids[2] = {0xAAAAAAAA, 0xBBBBBBBB};
    memcpy(rec + 16, ids, 8);              // ids at 16..23

    size_t off = 3;
    uint32_t* out_arr = NULL;
    uint32_t  out_n = 0;
    int rc = rg_parse_padded_u32_array(rec, &off, &out_arr, &out_n);
    CHECK(rc == 0);
    CHECK(out_n == 2);
    CHECK(out_arr != NULL);
    if (out_arr) {
        CHECK(out_arr[0] == 0xAAAAAAAA);
        CHECK(out_arr[1] == 0xBBBBBBBB);
    }
    // After: pre-aligned 3→8, header 8 → off 16, +8 bytes ids → 24 (already aligned).
    CHECK(off == 24);
    free(out_arr);
}

int main(void) {
    test_nonempty();
    test_empty();
    test_pre_alignment();
    fprintf(stderr, "%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

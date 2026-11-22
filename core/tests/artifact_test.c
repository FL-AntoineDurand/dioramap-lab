// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v3/artifact.h"

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

// ----------------------------------------------------------------------------

static void test_dtype_sizes(void) {
    CHECK(dtype_size(DT_U8) == 1);
    CHECK(dtype_size(DT_I16) == 2);
    CHECK(dtype_size(DT_F32) == 4);
    CHECK(dtype_size(DT_F64) == 8);
    CHECK(dtype_size(DT_INVALID) == 0);
}

static void test_column_name_hash_is_stable(void) {
    CHECK(artifact_column_name_hash("x") == artifact_column_name_hash("x"));
    CHECK(artifact_column_name_hash("x") != artifact_column_name_hash("y"));
}

// Regression for Frama-C audit finding A-1: NULL input must not crash.
// Before the fix the loop `for (; *name; ...)` dereferenced NULL on
// the first iteration. The defensive guard returns the FNV-1a init
// value (matching the result for the empty string).
static void test_column_name_hash_null_input(void) {
    CHECK(artifact_column_name_hash(NULL) == 0x811c9dc5u);
    CHECK(artifact_column_name_hash(NULL) == artifact_column_name_hash(""));
}

static void test_empty_artifact(void) {
    artifact_builder_t* ab = ab_new(AK_OPAQUE_JSON, 0);
    CHECK(ab != NULL);

    uint8_t* bytes = NULL;
    size_t len = 0;
    hash_t hash;
    CHECK(ab_finalize(ab, &bytes, &len, &hash) == 0);
    CHECK(len == ARTIFACT_HEADER_SIZE);

    artifact_view_t v;
    CHECK(av_open(bytes, len, &v) == 0);
    CHECK(v.header->kind == AK_OPAQUE_JSON);
    CHECK(v.header->n_columns == 0);
    CHECK(v.header->count_n == 0);

    free(bytes);
    ab_free(ab);
}

static void test_roundtrip_pointcloud(void) {
    const float xs[5] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
    const float ys[5] = {10.0f, 20.0f, 30.0f, 40.0f, 50.0f};
    const uint8_t cls[5] = {1, 2, 3, 4, 5};

    artifact_builder_t* ab = ab_new(AK_POINTCLOUD_SOA, 5);
    CHECK(ab_add_column(ab, "x", DT_F32, xs, sizeof(xs)) == 0);
    CHECK(ab_add_column(ab, "y", DT_F32, ys, sizeof(ys)) == 0);
    CHECK(ab_add_column(ab, "class", DT_U8, cls, sizeof(cls)) == 0);

    uint8_t* bytes = NULL;
    size_t len = 0;
    hash_t hash;
    CHECK(ab_finalize(ab, &bytes, &len, &hash) == 0);
    ab_free(ab);

    artifact_view_t v;
    CHECK(av_open(bytes, len, &v) == 0);
    CHECK(v.header->kind == AK_POINTCLOUD_SOA);
    CHECK(v.header->count_n == 5);
    CHECK(v.header->n_columns == 3);

    size_t nb = 0;
    const float* rx = av_column(&v, "x", &nb);
    CHECK(rx != NULL);
    CHECK(nb == sizeof(xs));
    CHECK(memcmp(rx, xs, sizeof(xs)) == 0);
    CHECK(av_column_dtype(&v, "x") == DT_F32);

    const float* ry = av_column(&v, "y", &nb);
    CHECK(memcmp(ry, ys, sizeof(ys)) == 0);

    const uint8_t* rc = av_column(&v, "class", &nb);
    CHECK(memcmp(rc, cls, sizeof(cls)) == 0);
    CHECK(av_column_dtype(&v, "class") == DT_U8);

    CHECK(av_column(&v, "missing", NULL) == NULL);

    // View hash should match builder hash.
    hash_t h2 = av_hash(&v);
    CHECK(hash_eq(&hash, &h2));

    free(bytes);
}

static void test_metadata_json(void) {
    const char* meta = "{\"layers\":[{\"id\":\"ridges\"}]}";
    artifact_builder_t* ab = ab_new(AK_GEOMETRY_PRIMITIVES, 0);
    CHECK(ab_set_metadata_json(ab, meta, strlen(meta)) == 0);

    uint8_t* bytes = NULL;
    size_t len = 0;
    hash_t hash;
    CHECK(ab_finalize(ab, &bytes, &len, &hash) == 0);
    ab_free(ab);

    artifact_view_t v;
    CHECK(av_open(bytes, len, &v) == 0);

    size_t ml = 0;
    const char* got = av_metadata_json(&v, &ml);
    CHECK(got != NULL);
    CHECK(ml == strlen(meta));
    CHECK(memcmp(got, meta, ml) == 0);

    free(bytes);
}

static void test_determinism(void) {
    // Same input → same bytes → same hash.
    const int32_t data[] = {1, 2, 3, 4, 5};

    artifact_builder_t* ab1 = ab_new(AK_PER_POINT_ARRAY, 5);
    ab_add_column(ab1, "v", DT_I32, data, sizeof(data));
    uint8_t* b1;
    size_t l1;
    hash_t h1;
    ab_finalize(ab1, &b1, &l1, &h1);
    ab_free(ab1);

    artifact_builder_t* ab2 = ab_new(AK_PER_POINT_ARRAY, 5);
    ab_add_column(ab2, "v", DT_I32, data, sizeof(data));
    uint8_t* b2;
    size_t l2;
    hash_t h2;
    ab_finalize(ab2, &b2, &l2, &h2);
    ab_free(ab2);

    CHECK(l1 == l2);
    CHECK(memcmp(b1, b2, l1) == 0);
    CHECK(hash_eq(&h1, &h2));

    free(b1);
    free(b2);
}

static void test_different_inputs_different_hashes(void) {
    const int32_t a[] = {1, 2, 3};
    const int32_t b[] = {1, 2, 4};

    artifact_builder_t* ab1 = ab_new(AK_PER_POINT_ARRAY, 3);
    ab_add_column(ab1, "v", DT_I32, a, sizeof(a));
    uint8_t* bytes1;
    size_t l1;
    hash_t h1;
    ab_finalize(ab1, &bytes1, &l1, &h1);
    ab_free(ab1);

    artifact_builder_t* ab2 = ab_new(AK_PER_POINT_ARRAY, 3);
    ab_add_column(ab2, "v", DT_I32, b, sizeof(b));
    uint8_t* bytes2;
    size_t l2;
    hash_t h2;
    ab_finalize(ab2, &bytes2, &l2, &h2);
    ab_free(ab2);

    CHECK(!hash_eq(&h1, &h2));

    free(bytes1);
    free(bytes2);
}

static void test_reject_duplicate_column(void) {
    artifact_builder_t* ab = ab_new(AK_POINTCLOUD_SOA, 0);
    const float x = 1.0f;
    CHECK(ab_add_column(ab, "x", DT_F32, &x, sizeof(x)) == 0);
    CHECK(ab_add_column(ab, "x", DT_F32, &x, sizeof(x)) == -1);
    ab_free(ab);
}

static void test_reject_bad_magic(void) {
    uint8_t buf[ARTIFACT_HEADER_SIZE] = {0};
    memcpy(buf, "NOPE", 4);
    artifact_view_t v;
    CHECK(av_open(buf, sizeof(buf), &v) == -1);
}

static void test_reject_bad_version(void) {
    // Build a valid artifact, then corrupt the version field.
    artifact_builder_t* ab = ab_new(AK_OPAQUE_JSON, 0);
    uint8_t* bytes;
    size_t len;
    hash_t h;
    ab_finalize(ab, &bytes, &len, &h);
    ab_free(ab);

    artifact_header_t* hdr = (artifact_header_t*)bytes;
    hdr->format_version = 999;

    artifact_view_t v;
    CHECK(av_open(bytes, len, &v) == -1);
    free(bytes);
}

static void test_reject_truncated(void) {
    artifact_builder_t* ab = ab_new(AK_PER_POINT_ARRAY, 3);
    const int32_t data[] = {1, 2, 3};
    ab_add_column(ab, "v", DT_I32, data, sizeof(data));
    uint8_t* bytes;
    size_t len;
    hash_t h;
    ab_finalize(ab, &bytes, &len, &h);
    ab_free(ab);

    // Chop off the column data.
    artifact_view_t v;
    CHECK(av_open(bytes, len - 4, &v) == -1);
    free(bytes);
}

// ----------------------------------------------------------------------------

int main(void) {
    test_dtype_sizes();
    test_column_name_hash_is_stable();
    test_column_name_hash_null_input();
    test_empty_artifact();
    test_roundtrip_pointcloud();
    test_metadata_json();
    test_determinism();
    test_different_inputs_different_hashes();
    test_reject_duplicate_column();
    test_reject_bad_magic();
    test_reject_bad_version();
    test_reject_truncated();

    printf("%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}

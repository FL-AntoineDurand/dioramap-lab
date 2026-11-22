// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#include <stdio.h>
#include <string.h>

#include "v3/hash.h"

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

static void test_determinism(void) {
    const char* s = "hello, world";
    hash_t a = hash_bytes(s, strlen(s));
    hash_t b = hash_bytes(s, strlen(s));
    CHECK(hash_eq(&a, &b));
}

static void test_different_inputs_different_hashes(void) {
    hash_t a = hash_bytes("hello", 5);
    hash_t b = hash_bytes("world", 5);
    CHECK(!hash_eq(&a, &b));
}

static void test_empty_input(void) {
    hash_t e1 = hash_bytes("", 0);
    hash_t e2 = hash_bytes(NULL, 0);
    CHECK(hash_eq(&e1, &e2));
}

static void test_hex_format(void) {
    hash_t h = hash_bytes("x", 1);
    char hex[65];
    hash_format_hex(&h, hex);
    CHECK(strlen(hex) == 64);
    // Last 32 bytes (16 bytes × 2 hex chars) are zero padding (xxh3 is 128 bits).
    CHECK(strncmp(hex + 32, "00000000000000000000000000000000", 32) == 0);
}

static void test_namespaced_differs_from_bytes(void) {
    // Same data bytes but with a tag → different identity from a raw content hash.
    const char* data = "hello";
    hash_t raw = hash_bytes(data, 5);
    hash_t tagged = hash_namespaced("url", data, 5);
    CHECK(!hash_eq(&raw, &tagged));
}

static void test_namespaced_tag_isolation(void) {
    // Different tags with the same data → different identities.
    const char* data = "example.com/foo";
    hash_t a = hash_namespaced("url", data, strlen(data));
    hash_t b = hash_namespaced("tile", data, strlen(data));
    CHECK(!hash_eq(&a, &b));
}

static void test_namespaced_determinism(void) {
    const char* u = "http://example.com/x.laz";
    hash_t a = hash_namespaced("url", u, strlen(u));
    hash_t b = hash_namespaced("url", u, strlen(u));
    CHECK(hash_eq(&a, &b));
}

static void test_namespaced_null_data(void) {
    // NULL data with len 0 is a well-defined input: hash of (tag || \0).
    hash_t a = hash_namespaced("tag", NULL, 0);
    hash_t b = hash_namespaced("tag", "", 0);
    CHECK(hash_eq(&a, &b));
}

static void test_namespaced_separator_prevents_concat_collision(void) {
    // Without the \0 separator, ("ab", "cd") would collide with ("a", "bcd").
    // The separator ensures they don't.
    hash_t a = hash_namespaced("ab", "cd", 2);
    hash_t b = hash_namespaced("a", "bcd", 3);
    CHECK(!hash_eq(&a, &b));
}

static void test_stream_matches_one_shot(void) {
    const char* s = "the quick brown fox jumps over the lazy dog";
    hash_t one = hash_bytes(s, strlen(s));

    hash_state_t* st = hash_stream_open();
    hash_stream_update(st, s, 4);
    hash_stream_update(st, s + 4, 10);
    hash_stream_update(st, s + 14, strlen(s) - 14);
    hash_t stream = hash_stream_finalize(st);

    CHECK(hash_eq(&one, &stream));
}

int main(void) {
    test_determinism();
    test_different_inputs_different_hashes();
    test_empty_input();
    test_hex_format();
    test_namespaced_differs_from_bytes();
    test_namespaced_tag_isolation();
    test_namespaced_determinism();
    test_namespaced_null_data();
    test_namespaced_separator_prevents_concat_collision();
    test_stream_matches_one_shot();

    printf("%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}

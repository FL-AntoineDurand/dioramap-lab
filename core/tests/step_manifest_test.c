// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Unit tests for the step-output manifest packer/walker (HASH-MODEL.md §3).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v3/hash.h"

// The manifest API is runner-private; include the header directly.
#include "../src/runner/step_manifest.h"

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

static hash_t mk_hash(const char* s) {
    return hash_bytes(s, strlen(s));
}

typedef struct walk_ctx {
    size_t n_visited;
    const char* expected_names[8];
    hash_t expected_hashes[8];
    size_t expected_count;
    bool ok;
} walk_ctx_t;

static int check_entry_cb(void* vctx, const char* name, size_t name_len, const hash_t* h) {
    walk_ctx_t* c = vctx;
    if (c->n_visited >= c->expected_count) {
        c->ok = false;
        return -1;
    }
    size_t want_len = strlen(c->expected_names[c->n_visited]);
    if (name_len != want_len || memcmp(name, c->expected_names[c->n_visited], name_len) != 0) {
        c->ok = false;
        return -1;
    }
    if (!hash_eq(h, &c->expected_hashes[c->n_visited])) {
        c->ok = false;
        return -1;
    }
    c->n_visited++;
    return 0;
}

static void test_roundtrip_empty(void) {
    uint8_t* buf = NULL;
    size_t len = 0;
    CHECK(step_manifest_pack(NULL, 0, &buf, &len) == 0);
    CHECK(len == 4);

    walk_ctx_t ctx = {.ok = true, .expected_count = 0};
    CHECK(step_manifest_walk(buf, len, check_entry_cb, &ctx) == 0);
    CHECK(ctx.n_visited == 0);
    CHECK(ctx.ok);
    free(buf);
}

static void test_roundtrip_single(void) {
    step_manifest_entry_t entries[] = {
        {.name = "output-a", .content_hash = mk_hash("a-bytes")},
    };
    uint8_t* buf = NULL;
    size_t len = 0;
    CHECK(step_manifest_pack(entries, 1, &buf, &len) == 0);
    // 4 (n) + 4 (name_len) + 8 (name) + 32 (hash) = 48
    CHECK(len == 48);

    walk_ctx_t ctx = {.ok = true, .expected_count = 1};
    ctx.expected_names[0] = "output-a";
    ctx.expected_hashes[0] = mk_hash("a-bytes");
    CHECK(step_manifest_walk(buf, len, check_entry_cb, &ctx) == 0);
    CHECK(ctx.n_visited == 1);
    CHECK(ctx.ok);
    free(buf);
}

static void test_roundtrip_multi(void) {
    step_manifest_entry_t entries[] = {
        {.name = "lidar-positions", .content_hash = mk_hash("pos-bytes")},
        {.name = "lidar-classifications", .content_hash = mk_hash("cls-bytes")},
        {.name = "lidar-intensities", .content_hash = mk_hash("int-bytes")},
        {.name = "lidar-num-returns", .content_hash = mk_hash("ret-bytes")},
    };
    uint8_t* buf = NULL;
    size_t len = 0;
    CHECK(step_manifest_pack(entries, 4, &buf, &len) == 0);

    walk_ctx_t ctx = {.ok = true, .expected_count = 4};
    for (size_t i = 0; i < 4; i++) {
        ctx.expected_names[i] = entries[i].name;
        ctx.expected_hashes[i] = entries[i].content_hash;
    }
    CHECK(step_manifest_walk(buf, len, check_entry_cb, &ctx) == 0);
    CHECK(ctx.n_visited == 4);
    CHECK(ctx.ok);
    free(buf);
}

static void test_walk_rejects_truncated(void) {
    step_manifest_entry_t entries[] = {
        {.name = "only-output", .content_hash = mk_hash("only")},
    };
    uint8_t* buf = NULL;
    size_t len = 0;
    CHECK(step_manifest_pack(entries, 1, &buf, &len) == 0);

    walk_ctx_t ctx = {.ok = true, .expected_count = 1};
    ctx.expected_names[0] = "only-output";
    ctx.expected_hashes[0] = entries[0].content_hash;

    // Truncate by 1 byte — walker must reject.
    CHECK(step_manifest_walk(buf, len - 1, check_entry_cb, &ctx) == -1);

    // Trailing garbage (extend by 1 byte) — walker must also reject (strict).
    uint8_t* buf2 = realloc(buf, len + 1);
    buf = buf2;
    buf[len] = 0xFF;
    walk_ctx_t ctx2 = {.ok = true, .expected_count = 1};
    ctx2.expected_names[0] = "only-output";
    ctx2.expected_hashes[0] = entries[0].content_hash;
    CHECK(step_manifest_walk(buf, len + 1, check_entry_cb, &ctx2) == -1);
    free(buf);
}

static void test_walk_rejects_zero_length_buffer(void) {
    walk_ctx_t ctx = {.ok = true, .expected_count = 0};
    CHECK(step_manifest_walk(NULL, 0, check_entry_cb, &ctx) == -1);
}

static void test_callback_abort_propagates(void) {
    // When the callback returns -1 on a legitimate entry, walk must surface -1.
    step_manifest_entry_t entries[] = {
        {.name = "x", .content_hash = mk_hash("x")},
    };
    uint8_t* buf = NULL;
    size_t len = 0;
    step_manifest_pack(entries, 1, &buf, &len);

    int (*abort_cb)(void*, const char*, size_t, const hash_t*) =
        (int (*)(void*, const char*, size_t, const hash_t*))(intptr_t) - 1;
    (void)abort_cb; // just being explicit the cast is intentional in the real impl

    // Use a capturing ctx that rejects.
    walk_ctx_t ctx = {.ok = true, .expected_count = 0};
    CHECK(step_manifest_walk(buf, len, check_entry_cb, &ctx) == -1);
    free(buf);
}

int main(void) {
    test_roundtrip_empty();
    test_roundtrip_single();
    test_roundtrip_multi();
    test_walk_rejects_truncated();
    test_walk_rejects_zero_length_buffer();
    test_callback_abort_propagates();

    printf("%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}

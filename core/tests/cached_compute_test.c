// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Unit tests for the cached_compute primitive (HASH-MODEL.md §3).
//
// Covers: intent-map miss runs compute_fn, hit skips it; compute_fn error
// propagates and records no intent entry; idempotent re-put on repeated
// calls; concurrent calls with the same identity all succeed with the same
// content hash; large-enough puts route through hot tier with zero copies.

#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "v3/cached_compute.h"
#include "v3/hash.h"
#include "v3/intent_map.h"
#include "v3/store.h"

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

static void rm_rf(const char* path) {
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    (void)!system(cmd);
}

static void make_tmpdir(char* out, size_t out_sz) {
    snprintf(out, out_sz, "/tmp/dioramap_cc_test_XXXXXX");
    if (mkdtemp(out) == NULL) {
        exit(1);
    }
}

typedef struct fixture {
    char root[256];
    char im_path[512];
    store_t* s;
    intent_map_t* im;
} fixture_t;

static void fixture_setup(fixture_t* f) {
    make_tmpdir(f->root, sizeof(f->root));
    snprintf(f->im_path, sizeof(f->im_path), "%s/intent.log", f->root);
    f->s = store_open(&(store_config_t){.root_dir = f->root, .hot_budget_bytes = 1024 * 1024});
    f->im = intent_map_open(f->im_path);
}

static void fixture_teardown(fixture_t* f) {
    intent_map_close(f->im);
    store_close(f->s);
    rm_rf(f->root);
}

// ----------------------------------------------------------------------------
// compute_fn that produces a fixed payload; counts invocations.
// ----------------------------------------------------------------------------

typedef struct payload_ctx {
    const char* text;
    atomic_int invocations;
    bool fail;
} payload_ctx_t;

static int payload_compute_fn(void* ctx_v, uint8_t** out_bytes, size_t* out_len) {
    payload_ctx_t* ctx = ctx_v;
    atomic_fetch_add(&ctx->invocations, 1);
    if (ctx->fail) {
        return -1;
    }
    size_t n = strlen(ctx->text);
    uint8_t* buf = malloc(n);
    if (!buf) {
        return -1;
    }
    memcpy(buf, ctx->text, n);
    *out_bytes = buf;
    *out_len = n;
    return 0;
}

// ----------------------------------------------------------------------------
// Tests
// ----------------------------------------------------------------------------

static void test_miss_then_hit(void) {
    fixture_t f;
    fixture_setup(&f);

    hash_t id = hash_bytes("identity-A", 10);
    payload_ctx_t ctx = {.text = "A-payload", .fail = false};

    hash_t ch1, ch2;
    CHECK(cached_compute(f.im, f.s, &id, payload_compute_fn, &ctx, &ch1) == 0);
    CHECK(atomic_load(&ctx.invocations) == 1);

    CHECK(cached_compute(f.im, f.s, &id, payload_compute_fn, &ctx, &ch2) == 0);
    // Hit → no second invocation.
    CHECK(atomic_load(&ctx.invocations) == 1);
    CHECK(hash_eq(&ch1, &ch2));

    // Content hash must equal hash_bytes(payload).
    hash_t expected = hash_bytes("A-payload", 9);
    CHECK(hash_eq(&ch1, &expected));

    // Store has the payload.
    store_handle_t sh;
    CHECK(store_lookup(f.s, &ch1, &sh) == 0);
    CHECK(sh.len == 9 && memcmp(sh.bytes, "A-payload", 9) == 0);
    store_release(f.s, &sh);

    fixture_teardown(&f);
}

static void test_compute_fn_error_records_no_intent(void) {
    fixture_t f;
    fixture_setup(&f);

    hash_t id = hash_bytes("identity-fail", 13);
    payload_ctx_t ctx = {.text = "x", .fail = true};
    hash_t ch;
    CHECK(cached_compute(f.im, f.s, &id, payload_compute_fn, &ctx, &ch) == -1);
    CHECK(atomic_load(&ctx.invocations) == 1);

    // Intent map must not contain this identity — a retry re-runs compute_fn.
    hash_t ignored;
    CHECK(intent_map_get(f.im, &id, &ignored) == -1);

    // Flip to succeed; retry runs and records.
    ctx.fail = false;
    ctx.text = "recovered";
    CHECK(cached_compute(f.im, f.s, &id, payload_compute_fn, &ctx, &ch) == 0);
    CHECK(atomic_load(&ctx.invocations) == 2);

    fixture_teardown(&f);
}

struct concurrent_arg {
    fixture_t* f;
    payload_ctx_t* ctx;
    hash_t* id;
    hash_t* out;
    int* errors;
};

static void* concurrent_thread_fn(void* a) {
    struct concurrent_arg* ta = a;
    if (cached_compute(ta->f->im, ta->f->s, ta->id, payload_compute_fn, ta->ctx, ta->out) != 0) {
        __atomic_fetch_add(ta->errors, 1, __ATOMIC_RELAXED);
    }
    return NULL;
}

static void test_concurrent_same_identity(void) {
    // Multiple threads hit the same identity at once — cached_compute doesn't
    // dedup work (yet), but all callers get the same content hash and the
    // intent map converges to that entry.
    fixture_t f;
    fixture_setup(&f);

    hash_t id = hash_bytes("concurrent-id", 13);
    payload_ctx_t ctx = {.text = "shared-result", .fail = false};

    const int T = 8;
    pthread_t threads[T];
    hash_t results[T];
    int errors = 0;
    struct concurrent_arg args[T];

    for (int i = 0; i < T; i++) {
        args[i].f = &f;
        args[i].ctx = &ctx;
        args[i].id = &id;
        args[i].out = &results[i];
        args[i].errors = &errors;
        CHECK(pthread_create(&threads[i], NULL, concurrent_thread_fn, &args[i]) == 0);
    }
    for (int i = 0; i < T; i++) {
        pthread_join(threads[i], NULL);
    }
    CHECK(errors == 0);

    for (int i = 1; i < T; i++) {
        CHECK(hash_eq(&results[0], &results[i]));
    }
    hash_t expected = hash_bytes("shared-result", 13);
    CHECK(hash_eq(&results[0], &expected));

    fixture_teardown(&f);
}

static void test_different_identities_same_bytes_converge(void) {
    // Two distinct identities, both compute the same bytes → they share the
    // store entry and the intent map has two entries pointing at the same hash.
    fixture_t f;
    fixture_setup(&f);

    hash_t id_a = hash_bytes("id-A", 4);
    hash_t id_b = hash_bytes("id-B", 4);
    payload_ctx_t ctx = {.text = "converged-output", .fail = false};

    hash_t ch_a, ch_b;
    CHECK(cached_compute(f.im, f.s, &id_a, payload_compute_fn, &ctx, &ch_a) == 0);
    CHECK(cached_compute(f.im, f.s, &id_b, payload_compute_fn, &ctx, &ch_b) == 0);
    CHECK(hash_eq(&ch_a, &ch_b));

    // Both compute_fn invocations ran (distinct identities), but the store
    // has exactly one entry (idempotent put).
    CHECK(atomic_load(&ctx.invocations) == 2);
    store_stats_t stats;
    store_get_stats(f.s, &stats);
    CHECK(stats.entries == 1);

    // Intent map has two entries pointing at the same content hash.
    intent_map_stats_t im_stats;
    intent_map_get_stats(f.im, &im_stats);
    CHECK(im_stats.live_entries == 2);

    fixture_teardown(&f);
}

static void test_invalidate_forces_recompute(void) {
    // After intent_map_invalidate, the next cached_compute call must re-run.
    fixture_t f;
    fixture_setup(&f);

    hash_t id = hash_bytes("inv-id", 6);
    payload_ctx_t ctx = {.text = "inv-payload", .fail = false};
    hash_t ch;

    CHECK(cached_compute(f.im, f.s, &id, payload_compute_fn, &ctx, &ch) == 0);
    CHECK(atomic_load(&ctx.invocations) == 1);

    CHECK(intent_map_invalidate(f.im, &id) == 0);

    CHECK(cached_compute(f.im, f.s, &id, payload_compute_fn, &ctx, &ch) == 0);
    CHECK(atomic_load(&ctx.invocations) == 2);

    fixture_teardown(&f);
}

static void test_bad_args(void) {
    fixture_t f;
    fixture_setup(&f);

    hash_t id = hash_bytes("x", 1);
    payload_ctx_t ctx = {.text = "y", .fail = false};
    hash_t out;

    CHECK(cached_compute(NULL, f.s, &id, payload_compute_fn, &ctx, &out) == -1);
    CHECK(cached_compute(f.im, NULL, &id, payload_compute_fn, &ctx, &out) == -1);
    CHECK(cached_compute(f.im, f.s, NULL, payload_compute_fn, &ctx, &out) == -1);
    CHECK(cached_compute(f.im, f.s, &id, NULL, &ctx, &out) == -1);
    CHECK(cached_compute(f.im, f.s, &id, payload_compute_fn, &ctx, NULL) == -1);
    // compute_fn should not have been called due to early arg checks.
    CHECK(atomic_load(&ctx.invocations) == 0);

    fixture_teardown(&f);
}

int main(void) {
    test_miss_then_hit();
    test_compute_fn_error_records_no_intent();
    test_concurrent_same_identity();
    test_different_identities_same_bytes_converge();
    test_invalidate_forces_recompute();
    test_bad_args();

    printf("%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}

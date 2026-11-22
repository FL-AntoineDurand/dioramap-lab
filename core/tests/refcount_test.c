// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "v3/refcount.h"

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

static void test_basic(void) {
    refcount_t r;
    refcount_init(&r, 1);
    CHECK(refcount_get(&r) == 1);

    refcount_acquire(&r);
    CHECK(refcount_get(&r) == 2);

    CHECK(!refcount_release(&r));
    CHECK(refcount_get(&r) == 1);

    CHECK(refcount_release(&r));
    CHECK(refcount_get(&r) == 0);
}

static void test_initial_greater(void) {
    refcount_t r;
    refcount_init(&r, 3);
    CHECK(!refcount_release(&r));
    CHECK(!refcount_release(&r));
    CHECK(refcount_release(&r));
}

// ----------------------------------------------------------------------------
// Concurrency: N threads each do M acquire-then-release pairs on a shared ref.
// Final count must equal the main thread's initial ref.
// ----------------------------------------------------------------------------

#define N_THREADS 8
#define N_OPS 10000

typedef struct {
    refcount_t* r;
} rc_arg_t;

static void* rc_worker(void* arg) {
    rc_arg_t* a = (rc_arg_t*)arg;
    for (int i = 0; i < N_OPS; i++) {
        refcount_acquire(a->r);
    }
    for (int i = 0; i < N_OPS; i++) {
        refcount_release(a->r);
    }
    return NULL;
}

static void test_concurrent_balanced(void) {
    refcount_t r;
    refcount_init(&r, 1); // main thread's ref

    pthread_t threads[N_THREADS];
    rc_arg_t args[N_THREADS];
    for (int i = 0; i < N_THREADS; i++) {
        args[i].r = &r;
        pthread_create(&threads[i], NULL, rc_worker, &args[i]);
    }
    for (int i = 0; i < N_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    CHECK(refcount_get(&r) == 1);
    CHECK(refcount_release(&r));
    CHECK(refcount_get(&r) == 0);
}

// Concurrent acquire-only and release-only scheme: thread A starts with N refs,
// does N releases; thread B acquires N times. At the end, count should be N
// (main's initial + B's N - A's N, which cancels except for the initial).
static void* acquire_only(void* arg) {
    refcount_t* r = (refcount_t*)arg;
    for (int i = 0; i < N_OPS; i++) {
        refcount_acquire(r);
    }
    return NULL;
}

static void* release_only(void* arg) {
    refcount_t* r = (refcount_t*)arg;
    for (int i = 0; i < N_OPS; i++) {
        refcount_release(r);
    }
    return NULL;
}

static void test_concurrent_asymmetric(void) {
    refcount_t r;
    // Initial count must cover release_only's N_OPS releases without underflow.
    refcount_init(&r, 1 + N_OPS);

    pthread_t tA, tB;
    CHECK(pthread_create(&tA, NULL, release_only, &r) == 0);
    CHECK(pthread_create(&tB, NULL, acquire_only, &r) == 0);
    pthread_join(tA, NULL);
    pthread_join(tB, NULL);

    // release_only removed N_OPS, acquire_only added N_OPS → net zero.
    // Any lost update would show a count != (1 + N_OPS).
    CHECK(refcount_get(&r) == 1 + N_OPS);
}

// ----------------------------------------------------------------------------

int main(void) {
    test_basic();
    test_initial_greater();
    test_concurrent_balanced();
    test_concurrent_asymmetric();

    printf("%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}

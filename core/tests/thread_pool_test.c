// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "v3/thread_pool.h"

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

static atomic_uint g_counter;

static void increment_work(void* arg) {
    (void)arg;
    atomic_fetch_add_explicit(&g_counter, 1, memory_order_relaxed);
}

static void test_basic_submit(void) {
    atomic_store_explicit(&g_counter, 0, memory_order_relaxed);
    thread_pool_t* p = pool_create(4, 16);
    CHECK(p != NULL);
    CHECK(pool_size(p) == 4);

    for (int i = 0; i < 100; i++) {
        CHECK(pool_submit(p, increment_work, NULL) == 0);
    }
    pool_shutdown(p); // drains before returning
    CHECK(atomic_load_explicit(&g_counter, memory_order_acquire) == 100);
    pool_destroy(p);
}

static void test_shutdown_rejects_submit(void) {
    thread_pool_t* p = pool_create(2, 4);
    pool_shutdown(p);
    CHECK(pool_submit(p, increment_work, NULL) == -1);
    pool_destroy(p);
}

static void test_double_shutdown_and_destroy(void) {
    thread_pool_t* p = pool_create(2, 4);
    pool_shutdown(p);
    pool_shutdown(p); // no-op
    pool_destroy(p);  // also safe after shutdown
}

static void test_destroy_without_shutdown(void) {
    thread_pool_t* p = pool_create(2, 4);
    // Skip shutdown intentionally; destroy should cover.
    pool_destroy(p);
}

static void test_invalid_args(void) {
    CHECK(pool_create(0, 4) == NULL);
    CHECK(pool_create(4, 0) == NULL);
    CHECK(pool_submit(NULL, increment_work, NULL) == -1);

    thread_pool_t* p = pool_create(2, 4);
    CHECK(pool_submit(p, NULL, NULL) == -1);
    pool_destroy(p);
}

// Submit 10 slow jobs through a pool with 1 worker + queue capacity 2.
// Exercises the "submit blocks when full" path.
static void slow_work(void* arg) {
    (void)arg;
    struct timespec ts = {.tv_nsec = 1000000}; // 1 ms
    nanosleep(&ts, NULL);
    atomic_fetch_add_explicit(&g_counter, 1, memory_order_relaxed);
}

static void test_submit_blocks_when_full(void) {
    atomic_store_explicit(&g_counter, 0, memory_order_relaxed);
    thread_pool_t* p = pool_create(1, 2);
    for (int i = 0; i < 10; i++) {
        CHECK(pool_submit(p, slow_work, NULL) == 0);
    }
    pool_shutdown(p);
    CHECK(atomic_load_explicit(&g_counter, memory_order_acquire) == 10);
    pool_destroy(p);
}

// ----------------------------------------------------------------------------
// Stress: multiple submitters racing into a single pool.
// ----------------------------------------------------------------------------

#define STRESS_N_SUBMITTERS 4
#define STRESS_WORK_PER_SUBMITTER 1000

typedef struct {
    thread_pool_t* pool;
} submitter_arg_t;

static void* submitter_thread(void* arg) {
    submitter_arg_t* sa = (submitter_arg_t*)arg;
    for (int i = 0; i < STRESS_WORK_PER_SUBMITTER; i++) {
        if (pool_submit(sa->pool, increment_work, NULL) != 0) {
            break;
        }
    }
    return NULL;
}

static void test_stress(void) {
    atomic_store_explicit(&g_counter, 0, memory_order_relaxed);
    thread_pool_t* p = pool_create(4, 32);
    pthread_t submitters[STRESS_N_SUBMITTERS];
    submitter_arg_t args[STRESS_N_SUBMITTERS];
    for (int i = 0; i < STRESS_N_SUBMITTERS; i++) {
        args[i].pool = p;
        CHECK(pthread_create(&submitters[i], NULL, submitter_thread, &args[i]) == 0);
    }
    for (int i = 0; i < STRESS_N_SUBMITTERS; i++) {
        pthread_join(submitters[i], NULL);
    }
    pool_shutdown(p);
    CHECK(atomic_load_explicit(&g_counter, memory_order_acquire) ==
          STRESS_N_SUBMITTERS * STRESS_WORK_PER_SUBMITTER);
    pool_destroy(p);
}

// ----------------------------------------------------------------------------

int main(void) {
    test_basic_submit();
    test_shutdown_rejects_submit();
    test_double_shutdown_and_destroy();
    test_destroy_without_shutdown();
    test_invalid_args();
    test_submit_blocks_when_full();
    test_stress();

    printf("%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}

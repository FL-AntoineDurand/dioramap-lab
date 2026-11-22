// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Frama-C audit harness for `dioramap_thread_pool`.

#include "v3/thread_pool.h"
#include <__fc_builtin.h>

static void work_noop(void* arg) {
    (void)arg;
}

int main(void) {
    // happy path
    thread_pool_t* p = pool_create(4, 16);
    if (p) {
        for (int i = 0; i < 8; i++) (void)pool_submit(p, work_noop, NULL);
        (void)pool_size(p);
        (void)pool_queued(p);
        pool_shutdown(p);
        pool_destroy(p);
    }

    // invalid args
    (void)pool_create(0, 16);
    (void)pool_create(4, 0);
    (void)pool_create(0, 0);

    // submit to NULL pool — public API doesn't doc but defensive callers may try
    (void)pool_submit((thread_pool_t*)0, work_noop, NULL);

    // destroy without shutdown
    thread_pool_t* q = pool_create(2, 4);
    if (q) {
        (void)pool_submit(q, work_noop, NULL);
        pool_destroy(q);
    }

    pool_destroy((thread_pool_t*)0);
    pool_shutdown((thread_pool_t*)0);

    return 0;
}

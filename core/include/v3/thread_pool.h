// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#ifndef V3_THREAD_POOL_H
#define V3_THREAD_POOL_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct thread_pool thread_pool_t;
typedef void (*pool_work_fn)(void* arg);

/*@ assigns \nothing;
    ensures \result == \null || \valid(\result);
*/
thread_pool_t* pool_create(size_t n_threads, size_t queue_cap);

/*@ requires pool == \null || \valid(pool);
    assigns *pool;
    ensures \result == 0 || \result == -1;
*/
int pool_submit(thread_pool_t* pool, pool_work_fn fn, void* arg);

/*@ requires pool == \null || \valid(pool);
    assigns *pool;
*/
void pool_shutdown(thread_pool_t* pool);

/*@ requires pool == \null || \valid(pool);
    assigns *pool;
*/
void pool_destroy(thread_pool_t* pool);

/*@ requires pool == \null || \valid(pool);
    assigns \nothing;
*/
size_t pool_size(thread_pool_t* pool);

/*@ requires pool == \null || \valid(pool);
    assigns \nothing;
*/
size_t pool_queued(thread_pool_t* pool);

#ifdef __cplusplus
}
#endif

#endif

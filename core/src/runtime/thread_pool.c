// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Bounded-queue thread pool.
//
// Design:
//   - Fixed number of worker threads spawned at create.
//   - Ring-buffered work queue of fixed capacity.
//   - One mutex protects all queue state.
//   - Two condvars: not_empty (workers wait) and not_full (submitters wait).
//   - Shutdown drains queued work; new submits after shutdown are rejected.

#define _POSIX_C_SOURCE 200809L

#include "v3/thread_pool.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct work_item {
    pool_work_fn fn;
    void* arg;
} work_item_t;

struct thread_pool {
    pthread_mutex_t lock;
    pthread_cond_t not_empty; // workers wait here when queue is empty
    pthread_cond_t not_full;  // submitters wait here when queue is full

    size_t n_threads;
    pthread_t* threads;

    // Ring buffer of pending work items.
    work_item_t* items;
    size_t cap;
    size_t head;  // next slot to pop (read)
    size_t tail;  // next slot to push (write)
    size_t count; // items currently queued

    bool shutdown;
};

static void* worker_loop(void* arg) {
    thread_pool_t* p = (thread_pool_t*)arg;
    for (;;) {
        pthread_mutex_lock(&p->lock);
        while (p->count == 0 && !p->shutdown) {
            pthread_cond_wait(&p->not_empty, &p->lock);
        }
        if (p->count == 0 && p->shutdown) {
            pthread_mutex_unlock(&p->lock);
            return NULL;
        }
        work_item_t w = p->items[p->head];
        p->head = (p->head + 1) % p->cap;
        p->count--;
        pthread_cond_signal(&p->not_full);
        pthread_mutex_unlock(&p->lock);

        w.fn(w.arg);
    }
}

thread_pool_t* pool_create(size_t n_threads, size_t queue_cap) {
    if (n_threads == 0 || queue_cap == 0) {
        return NULL;
    }

    thread_pool_t* p = calloc(1, sizeof(thread_pool_t));
    if (!p) {
        return NULL;
    }

    p->n_threads = n_threads;
    p->cap = queue_cap;
    p->threads = calloc(n_threads, sizeof(pthread_t));
    p->items = calloc(queue_cap, sizeof(work_item_t));
    if (!p->threads || !p->items) {
        free(p->threads);
        free(p->items);
        free(p);
        return NULL;
    }

    if (pthread_mutex_init(&p->lock, NULL) != 0) {
        goto err_no_sync;
    }
    if (pthread_cond_init(&p->not_empty, NULL) != 0) {
        pthread_mutex_destroy(&p->lock);
        goto err_no_sync;
    }
    if (pthread_cond_init(&p->not_full, NULL) != 0) {
        pthread_cond_destroy(&p->not_empty);
        pthread_mutex_destroy(&p->lock);
        goto err_no_sync;
    }

    // Spawn workers. If one fails, shut down the ones we have and fail.
    size_t spawned = 0;
    for (spawned = 0; spawned < n_threads; spawned++) {
        if (pthread_create(&p->threads[spawned], NULL, worker_loop, p) != 0) {
            break;
        }
    }
    if (spawned < n_threads) {
        pthread_mutex_lock(&p->lock);
        p->shutdown = true;
        pthread_cond_broadcast(&p->not_empty);
        pthread_mutex_unlock(&p->lock);
        for (size_t i = 0; i < spawned; i++) {
            pthread_join(p->threads[i], NULL);
        }
        pthread_cond_destroy(&p->not_full);
        pthread_cond_destroy(&p->not_empty);
        pthread_mutex_destroy(&p->lock);
        goto err_no_sync;
    }

    return p;

err_no_sync:
    free(p->threads);
    free(p->items);
    free(p);
    return NULL;
}

int pool_submit(thread_pool_t* p, pool_work_fn fn, void* arg) {
    if (!p || !fn) {
        return -1;
    }
    pthread_mutex_lock(&p->lock);
    while (p->count == p->cap && !p->shutdown) {
        pthread_cond_wait(&p->not_full, &p->lock);
    }
    if (p->shutdown) {
        pthread_mutex_unlock(&p->lock);
        return -1;
    }
    p->items[p->tail].fn = fn;
    p->items[p->tail].arg = arg;
    p->tail = (p->tail + 1) % p->cap;
    p->count++;
    pthread_cond_signal(&p->not_empty);
    pthread_mutex_unlock(&p->lock);
    return 0;
}

void pool_shutdown(thread_pool_t* p) {
    if (!p) {
        return;
    }
    pthread_mutex_lock(&p->lock);
    if (p->shutdown) {
        pthread_mutex_unlock(&p->lock);
        return;
    }
    p->shutdown = true;
    pthread_cond_broadcast(&p->not_empty);
    pthread_cond_broadcast(&p->not_full);
    pthread_mutex_unlock(&p->lock);

    for (size_t i = 0; i < p->n_threads; i++) {
        pthread_join(p->threads[i], NULL);
    }
}

void pool_destroy(thread_pool_t* p) {
    if (!p) {
        return;
    }
    pool_shutdown(p);
    pthread_cond_destroy(&p->not_full);
    pthread_cond_destroy(&p->not_empty);
    pthread_mutex_destroy(&p->lock);
    free(p->threads);
    free(p->items);
    free(p);
}

size_t pool_size(thread_pool_t* p) {
    return p ? p->n_threads : 0;
}

size_t pool_queued(thread_pool_t* p) {
    if (!p) {
        return 0;
    }
    pthread_mutex_lock(&p->lock);
    size_t c = p->count;
    pthread_mutex_unlock(&p->lock);
    return c;
}

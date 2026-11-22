// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// batch_ctx_t implementation.  Owns: produced[] list, error counter,
// current run_id.  Internal mutex serialises every mutation; runner
// callbacks fire from worker threads (CONCURRENCY.md §11) so every
// access goes through the lock.
//
// Lock-ordering: batch_ctx_t::lock is a leaf — no nested locks, no
// cross-subsystem calls under the lock.

#define _POSIX_C_SOURCE 200809L

#include "v3/batch_ctx.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "v3/store.h"
#include "v3/thread_safety.h"

// ─── private types ─────────────────────────────────────────────────────

typedef struct {
    char*   name;       // strdup
    hash_t  hash;
    size_t  size;
    char*   path;       // strdup
} produced_entry_t;

struct batch_ctx {
    // Set once at construction; immutable thereafter.
    struct store*       store;
    const char*         output_dir;       // borrowed
    batch_subscribe_fn  subscribe_fn;
    void*               subscribe_ctx;

    mutex_t             lock;
    produced_entry_t*   produced     GUARDED_BY(lock);
    size_t              n_produced   GUARDED_BY(lock);
    size_t              cap_produced GUARDED_BY(lock);
    int                 error_count  GUARDED_BY(lock);
    char                run_id[24]   GUARDED_BY(lock);
};

// ─── helper: write file ────────────────────────────────────────────────

static int write_file(const char* path, const void* bytes, size_t len) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    const uint8_t* p = bytes;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t nw = write(fd, p, remaining);
        if (nw < 0) {
            if (errno == EINTR) continue;
            close(fd); return -1;
        }
        p += nw;
        remaining -= (size_t)nw;
    }
    close(fd);
    return 0;
}

// ─── lifecycle ─────────────────────────────────────────────────────────

batch_ctx_t* batch_ctx_new(struct store* store, const char* output_dir,
                           batch_subscribe_fn subscribe_fn, void* subscribe_ctx) {
    if (!store || !output_dir || !subscribe_fn) return NULL;
    batch_ctx_t* bc = calloc(1, sizeof(*bc));
    if (!bc) return NULL;
    bc->store         = store;
    bc->output_dir    = output_dir;
    bc->subscribe_fn  = subscribe_fn;
    bc->subscribe_ctx = subscribe_ctx;
    if (pthread_mutex_init(&bc->lock, NULL) != 0) {
        // Frama-C audit follow-up: glibc never fails default-attr mutex_init
        // in practice but the API allows it; roll back the partial alloc.
        free(bc);
        return NULL;
    }
    return bc;
}

void batch_ctx_free(batch_ctx_t* bc) {
    if (!bc) return;
    // Single-owner at this point (no run thread).  Per-entry strings
    // were strdup'd by us; free them.
    for (size_t i = 0; i < bc->n_produced; i++) {
        free(bc->produced[i].name);
        free(bc->produced[i].path);
    }
    free(bc->produced);
    pthread_mutex_destroy(&bc->lock);
    free(bc);
}

// ─── error / counter accessors ─────────────────────────────────────────

int batch_ctx_error_count(batch_ctx_t* bc) {
    MUTEX_LOCK(&bc->lock);
    int v = bc->error_count;
    MUTEX_UNLOCK(&bc->lock);
    return v;
}

size_t batch_ctx_n_produced(batch_ctx_t* bc) {
    MUTEX_LOCK(&bc->lock);
    size_t n = bc->n_produced;
    MUTEX_UNLOCK(&bc->lock);
    return n;
}

void batch_ctx_get_run_id(batch_ctx_t* bc, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    MUTEX_LOCK(&bc->lock);
    snprintf(out, out_size, "%s", bc->run_id);
    MUTEX_UNLOCK(&bc->lock);
}

void batch_ctx_for_each_produced(batch_ctx_t* bc, batch_produced_visit_fn fn,
                                 void* cb_ctx) {
    if (!fn) return;
    // We hold the lock for the whole iteration.  Callers' fn must not
    // re-enter the batch_ctx API (would deadlock) — documented in the
    // header.  The visit fn is the caller's, so they own that contract.
    MUTEX_LOCK(&bc->lock);
    for (size_t i = 0; i < bc->n_produced; i++) {
        batch_produced_t e = {
            .name = bc->produced[i].name,
            .hash = bc->produced[i].hash,
            .size = bc->produced[i].size,
            .path = bc->produced[i].path,
        };
        fn(cb_ctx, &e);
    }
    MUTEX_UNLOCK(&bc->lock);
}

// ─── internal: increment error count under lock ────────────────────────

static void incr_error(batch_ctx_t* bc) {
    MUTEX_LOCK(&bc->lock);
    bc->error_count++;
    MUTEX_UNLOCK(&bc->lock);
}

// ─── runner callbacks ──────────────────────────────────────────────────

static void cb_run_started(void* vctx, const char* run_id, const char* pipeline_name,
                           const char* const* plan, size_t n_plan) {
    batch_ctx_t* bc = vctx;
    (void)pipeline_name;
    MUTEX_LOCK(&bc->lock);
    snprintf(bc->run_id, sizeof(bc->run_id), "%s", run_id);
    MUTEX_UNLOCK(&bc->lock);
    fprintf(stderr, "[batch] run %s started, %zu step(s):\n", run_id, n_plan);
    for (size_t i = 0; i < n_plan; i++) {
        fprintf(stderr, "  %zu. %s\n", i + 1, plan[i]);
    }
}

static void cb_step_complete(void* vctx, const char* run_id, const char* step,
                             bool cache_hit, uint64_t dt_us) {
    (void)vctx; (void)run_id;
    fprintf(stderr, "[batch] step %-20s %s in %.2f ms\n", step,
            cache_hit ? "(cache hit)" : "executed", dt_us / 1000.0);
}

static void cb_artifact_ready(void* vctx, const char* run_id, const char* name,
                              const hash_t* h) {
    batch_ctx_t* bc = vctx;
    (void)run_id;
    // Subscribe predicate is the caller's; runs lock-free in their
    // closure.  Treat it as immutable for the run's duration
    // (caller's contract).
    if (!bc->subscribe_fn(bc->subscribe_ctx, name)) return;

    // store_lookup OUTSIDE our lock — store has its own mutex.
    store_handle_t handle;
    if (store_lookup(bc->store, h, &handle) != 0) {
        fprintf(stderr, "[batch] error: artifact '%s' missing from store\n", name);
        incr_error(bc);
        return;
    }
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/%s.bin", bc->output_dir, name);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        store_release(bc->store, &handle);
        incr_error(bc);
        return;
    }
    if (write_file(path, handle.bytes, handle.len) != 0) {
        fprintf(stderr, "[batch] error: writing '%s'\n", path);
        store_release(bc->store, &handle);
        incr_error(bc);
        return;
    }
    // ALLOC: strdup name and path BEFORE taking the lock so the
    // critical section is short.  If either fails, count as error and
    // drop without aliasing.
    char* dn = strdup(name);
    char* dp = strdup(path);
    if (!dn || !dp) {
        free(dn); free(dp);
        store_release(bc->store, &handle);
        incr_error(bc);
        return;
    }

    MUTEX_LOCK(&bc->lock);
    if (bc->n_produced == bc->cap_produced) {
        size_t nc = bc->cap_produced ? bc->cap_produced * 2 : 8;
        // realloc under lock — small struct array.  On failure we
        // count as an error and drop the entry (already-strdup'd
        // strings get freed below).
        produced_entry_t* nw = realloc(bc->produced, nc * sizeof(*nw));
        if (!nw) {
            bc->error_count++;
            MUTEX_UNLOCK(&bc->lock);
            free(dn); free(dp);
            store_release(bc->store, &handle);
            return;
        }
        bc->produced     = nw;
        bc->cap_produced = nc;
    }
    produced_entry_t* e = &bc->produced[bc->n_produced++];
    e->name = dn;
    e->hash = *h;
    e->size = handle.len;
    e->path = dp;
    MUTEX_UNLOCK(&bc->lock);

    store_release(bc->store, &handle);
}

static void cb_run_complete(void* vctx, const char* run_id, uint64_t dt_us,
                            size_t executed, size_t hits) {
    (void)vctx; (void)run_id;
    fprintf(stderr, "[batch] run complete in %.2f ms (%zu step%s executed, %zu cache hit%s)\n",
            dt_us / 1000.0, executed, executed == 1 ? "" : "s", hits, hits == 1 ? "" : "s");
}

static void cb_error(void* vctx, const char* run_id, const char* step, const char* msg) {
    batch_ctx_t* bc = vctx;
    (void)run_id;
    fprintf(stderr, "[batch] ERROR step=%s msg=%s\n", step ? step : "(none)", msg);
    incr_error(bc);
}

void batch_ctx_install_callbacks(batch_ctx_t* bc, runner_callbacks_t* cb) {
    if (!bc || !cb) return;
    *cb = (runner_callbacks_t){
        .ctx               = bc,
        .on_run_started    = cb_run_started,
        .on_step_complete  = cb_step_complete,
        .on_artifact_ready = cb_artifact_ready,
        .on_run_complete   = cb_run_complete,
        .on_error          = cb_error,
    };
}

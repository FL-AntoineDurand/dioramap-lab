// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#define _POSIX_C_SOURCE 200809L

#include "v3/runner.h"
#include "v3/safe_alloc.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "v3/cached_compute.h"
#include "v3/log.h"
#include "v3/thread_pool.h"

#include "step_manifest.h"

// ----------------------------------------------------------------------------
// Per-run state
// ----------------------------------------------------------------------------

typedef struct run_artifact {
    char* name;
    hash_t hash;
} run_artifact_t;

typedef struct run_state {
    const pipeline_t* pipeline;
    size_t* plan; // indices into pipeline->steps, in topological order
    size_t plan_len;
    run_artifact_t* artifacts;
    size_t n_artifacts;
    size_t cap_artifacts;
    char run_id[24];
    const runner_callbacks_t* cb;

    // Concurrency state ----------------------------------------------------
    pthread_mutex_t lock; // guards artifact map, in_degree, n_pending, cancel, run_rc
    pthread_cond_t done_cv;

    // For each plan entry (0..plan_len-1): how many upstream reads are not
    // yet produced. Step becomes runnable when this drops to 0. Indexed by
    // position in the topo plan, not pipeline step index.
    size_t* in_degree;
    // For each plan entry: list of plan-entry indices that depend on it.
    size_t** successors;
    size_t* n_successors;

    size_t n_pending;    // tasks submitted but not yet completed
    size_t n_executed;   // cache misses
    size_t n_cache_hits; // cache hits

    bool cancel;           // set on first failure; blocks new task submissions
    bool cancel_external;  // set by runner_cancel(); distinguishes cancel-from-outside
    int run_rc;            // 0 on success, -1 on any step failure or external cancel
} run_state_t;

struct runner {
    store_t* store;
    intent_map_t* intent_map;
    atomic_uint_least64_t next_run_seq;
    thread_pool_t* pool;

    // Active-runs registry: every run_state_t currently inside runner_run()
    // is registered here so runner_cancel() can find it by run_id string.
    // Lock order: active_lock acquired first, then the per-run rs->lock.
    pthread_mutex_t active_lock;
    run_state_t** active_runs;
    size_t n_active;
    size_t cap_active;
};

// Caller must NOT hold any rs->lock.
static int active_runs_register(runner_t* r, run_state_t* rs) {
    pthread_mutex_lock(&r->active_lock);
    if (r->n_active == r->cap_active) {
        size_t nc = r->cap_active ? r->cap_active * 2 : 8;
        run_state_t** na = realloc(r->active_runs, nc * sizeof(*na));
        if (!na) {
            pthread_mutex_unlock(&r->active_lock);
            return -1;
        }
        r->active_runs = na;
        r->cap_active = nc;
    }
    r->active_runs[r->n_active++] = rs;
    pthread_mutex_unlock(&r->active_lock);
    return 0;
}

static void active_runs_unregister(runner_t* r, run_state_t* rs) {
    pthread_mutex_lock(&r->active_lock);
    for (size_t i = 0; i < r->n_active; i++) {
        if (r->active_runs[i] == rs) {
            r->active_runs[i] = r->active_runs[r->n_active - 1];
            r->n_active--;
            break;
        }
    }
    pthread_mutex_unlock(&r->active_lock);
}

// One heap-allocated task per step submission.
typedef struct step_task {
    runner_t* r;
    run_state_t* rs;
    size_t plan_pos; // index into rs->plan[]
} step_task_t;

// ----------------------------------------------------------------------------
// Step execution context (passed to step->exec)
// ----------------------------------------------------------------------------

// Accumulator slot for a step's writes — populated by step_write_output /
// step_write_array_*, consumed after exec returns to build the manifest.
typedef struct write_slot {
    const char* name;    // borrowed from step_def_t
    hash_t content_hash; // set after the write lands
    bool populated;
} write_slot_t;

// In-process accumulator for a streaming write (PLAN-VIEWER-PORT §7).
// One slot per write; only the slot for a `streaming = true` write is ever
// touched. Lives across step_write_output_stream calls; freed at exec end
// (whether finalized or not — partial buffers are dropped on error/cancel).
typedef struct stream_slot {
    uint8_t* buf;        // grows via realloc; NULL until first chunk
    size_t len;
    size_t cap;
    uint32_t seq;        // next chunk seq number to emit
    bool finalized;      // true after a final=true call
    bool started;        // true once the first chunk has arrived
} stream_slot_t;

struct step_context {
    runner_t* runner;
    run_state_t* rs;
    const step_def_t* def;
    const char* params_json;
    // One slot per declared write; populated as exec calls step_write_output.
    write_slot_t* write_slots;
    // Same arity as write_slots; only used for writes whose def has
    // `streaming = true`. Each slot accumulates chunks until a final=true call
    // commits the artifact via the normal finalize_write tail.
    stream_slot_t* stream_slots;
    // Open read handles (for release on return).
    store_handle_t* read_handles;
    size_t n_read_handles;
    int had_error;
};

// ----------------------------------------------------------------------------
// Helpers: runner + run_state
// ----------------------------------------------------------------------------

static void gen_run_id(char* out, size_t out_sz, uint64_t seq) {
    snprintf(out, out_sz, "r_%llu", (unsigned long long)seq);
}

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

// Caller must hold rs->lock.
static int artifact_map_put_locked(run_state_t* rs, const char* name, const hash_t* h) {
    for (size_t i = 0; i < rs->n_artifacts; i++) {
        if (strcmp(rs->artifacts[i].name, name) == 0) {
            rs->artifacts[i].hash = *h;
            return 0;
        }
    }
    if (rs->n_artifacts == rs->cap_artifacts) {
        size_t new_cap = rs->cap_artifacts ? rs->cap_artifacts * 2 : 8;
        run_artifact_t* arr = realloc(rs->artifacts, new_cap * sizeof(*arr));
        if (!arr) {
            return -1;
        }
        rs->artifacts = arr;
        rs->cap_artifacts = new_cap;
    }
    rs->artifacts[rs->n_artifacts].name = strdup(name);
    if (!rs->artifacts[rs->n_artifacts].name) {
        return -1;
    }
    rs->artifacts[rs->n_artifacts].hash = *h;
    rs->n_artifacts++;
    return 0;
}

// Caller must hold rs->lock. Returns hash BY VALUE (not pointer) so the
// pointer isn't invalidated by later realloc from another thread's put.
static bool artifact_map_get_locked(run_state_t* rs, const char* name, hash_t* out) {
    for (size_t i = 0; i < rs->n_artifacts; i++) {
        if (strcmp(rs->artifacts[i].name, name) == 0) {
            *out = rs->artifacts[i].hash;
            return true;
        }
    }
    return false;
}

static size_t find_writer_step(const pipeline_t* p, const char* artifact_name) {
    size_t n = pipeline_step_count(p);
    for (size_t i = 0; i < n; i++) {
        const step_def_t* def = pipeline_step_def_at(p, i);
        for (size_t w = 0; w < def->n_writes; w++) {
            if (strcmp(def->writes[w].name, artifact_name) == 0) {
                return i;
            }
        }
    }
    return (size_t)-1;
}

// ----------------------------------------------------------------------------
// DAG walk: mark steps reachable backward from subscribed artifacts,
// then topo-sort them.
// ----------------------------------------------------------------------------

static int dag_plan(const pipeline_t* p, const char* const* subscribe, size_t n_sub,
                    size_t** out_plan, size_t* out_plan_len) {
    size_t n_steps = pipeline_step_count(p);
    if (n_steps == 0) {
        *out_plan = NULL;
        *out_plan_len = 0;
        return 0;
    }
    bool* marked = safe_calloc_array(n_steps, sizeof(bool));
    size_t* stack = safe_calloc_array(n_steps, sizeof(size_t));
    if (!marked || !stack) {
        free(marked);
        free(stack);
        return -1;
    }
    size_t stack_len = 0;

    // Seed: producers of subscribed artifacts.
    for (size_t i = 0; i < n_sub; i++) {
        if (strcmp(subscribe[i], PIPELINE_INPUT_ARTIFACT) == 0) {
            continue;
        }
        size_t idx = find_writer_step(p, subscribe[i]);
        if (idx == (size_t)-1) {
            free(marked);
            free(stack);
            return -1;
        }
        if (!marked[idx]) {
            marked[idx] = true;
            stack[stack_len++] = idx;
        }
    }
    // BFS/DFS backward.
    while (stack_len > 0) {
        size_t idx = stack[--stack_len];
        const step_def_t* def = pipeline_step_def_at(p, idx);
        for (size_t r = 0; r < def->n_reads; r++) {
            if (strcmp(def->reads[r].name, PIPELINE_INPUT_ARTIFACT) == 0) {
                continue;
            }
            size_t prod = find_writer_step(p, def->reads[r].name);
            if (prod == (size_t)-1) {
                free(marked);
                free(stack);
                return -1;
            }
            if (!marked[prod]) {
                marked[prod] = true;
                stack[stack_len++] = prod;
            }
        }
    }

    // Topo sort marked steps. Kahn's algorithm.
    size_t* in_deg = safe_calloc_array(n_steps, sizeof(size_t));
    if (!in_deg) {
        free(marked);
        free(stack);
        return -1;
    }
    for (size_t i = 0; i < n_steps; i++) {
        if (!marked[i]) {
            continue;
        }
        const step_def_t* di = pipeline_step_def_at(p, i);
        for (size_t r = 0; r < di->n_reads; r++) {
            if (strcmp(di->reads[r].name, PIPELINE_INPUT_ARTIFACT) == 0) {
                continue;
            }
            size_t prod = find_writer_step(p, di->reads[r].name);
            if (prod != (size_t)-1 && marked[prod]) {
                in_deg[i]++;
            }
        }
    }
    size_t* order = safe_calloc_array(n_steps, sizeof(size_t));
    size_t n_order = 0;
    size_t* queue = safe_calloc_array(n_steps, sizeof(size_t));
    if (!order || !queue) {
        free(marked);
        free(stack);
        free(in_deg);
        free(order);
        free(queue);
        return -1;
    }
    size_t qhead = 0, qtail = 0;
    for (size_t i = 0; i < n_steps; i++) {
        if (marked[i] && in_deg[i] == 0) {
            queue[qtail++] = i;
        }
    }
    while (qhead < qtail) {
        size_t idx = queue[qhead++];
        order[n_order++] = idx;
        const step_def_t* di = pipeline_step_def_at(p, idx);
        for (size_t w = 0; w < di->n_writes; w++) {
            // Decrement in-degree of any marked step that reads this write.
            for (size_t k = 0; k < n_steps; k++) {
                if (!marked[k] || k == idx) {
                    continue;
                }
                const step_def_t* dk = pipeline_step_def_at(p, k);
                for (size_t r = 0; r < dk->n_reads; r++) {
                    if (strcmp(dk->reads[r].name, di->writes[w].name) == 0) {
                        if (in_deg[k] > 0 && --in_deg[k] == 0) {
                            queue[qtail++] = k;
                        }
                        break;
                    }
                }
            }
        }
    }
    size_t marked_count = 0;
    for (size_t i = 0; i < n_steps; i++) {
        if (marked[i]) {
            marked_count++;
        }
    }
    free(marked);
    free(stack);
    free(in_deg);
    free(queue);

    if (n_order != marked_count) {
        // Cycle detected.
        free(order);
        return -1;
    }
    *out_plan = order;
    *out_plan_len = n_order;
    return 0;
}

// ----------------------------------------------------------------------------
// Step identity hashing
// ----------------------------------------------------------------------------

static int cmp_hash_bytes(const void* a, const void* b) {
    return memcmp(((const hash_t*)a)->bytes, ((const hash_t*)b)->bytes,
                  sizeof(((hash_t*)0)->bytes));
}

// Compute the step identity hash per arch doc §9.1:
//   hash = xxh3(name, version, params, sorted_read_hashes_joined)
// Reads are sorted by hash bytes so the step author can reorder reads[] in
// meta.json without invalidating existing cached outputs.
static hash_t compute_step_identity_hash(const step_def_t* def, const char* params_json,
                                         const hash_t* read_hashes, size_t n_reads) {
    hash_t* sorted = NULL;
    if (n_reads > 0) {
        sorted = safe_malloc_array(n_reads, sizeof(hash_t));
        if (!sorted) {
            hash_t zero = {{0}};
            return zero; // caller will fail downstream lookups
        }
        memcpy(sorted, read_hashes, n_reads * sizeof(hash_t));
        qsort(sorted, n_reads, sizeof(hash_t), cmp_hash_bytes);
    }

    hash_state_t* s = hash_stream_open();
    hash_stream_update(s, "step-id-v1\0", 11);
    hash_stream_update(s, def->name, strlen(def->name));
    hash_stream_update(s, "\0", 1);
    uint32_t v = def->version;
    hash_stream_update(s, &v, sizeof(v));
    hash_stream_update(s, "\0", 1);
    if (params_json) {
        hash_stream_update(s, params_json, strlen(params_json));
    }
    hash_stream_update(s, "\0", 1);
    for (size_t i = 0; i < n_reads; i++) {
        hash_stream_update(s, sorted[i].bytes, sizeof(sorted[i].bytes));
    }
    hash_t out = hash_stream_finalize(s);
    free(sorted);
    return out;
}


// ----------------------------------------------------------------------------
// Step context impl
// ----------------------------------------------------------------------------

// Forward decl — F2's recovery path needs to re-execute the producer step
// when a content blob has been evicted underneath us.
static int execute_step(runner_t* r, run_state_t* rs, size_t step_idx,
                        bool* out_cache_hit);

// Open `h` from the store and append the handle to ctx->read_handles.
// Returns 0 on success, -1 on lookup failure (content blob missing).
static int try_open_read_locked(step_context_t* ctx, const hash_t* h, size_t* out_len) {
    store_handle_t* nh =
        realloc(ctx->read_handles, (ctx->n_read_handles + 1) * sizeof(store_handle_t));
    if (!nh) {
        ctx->had_error = 1;
        return -1;
    }
    ctx->read_handles = nh;
    if (store_lookup(ctx->runner->store, h, &ctx->read_handles[ctx->n_read_handles]) != 0) {
        return -1; // missing content — caller decides whether to recover
    }
    store_handle_t* opened = &ctx->read_handles[ctx->n_read_handles++];
    if (out_len) {
        *out_len = opened->len;
    }
    return 0;
}

const void* step_read(step_context_t* ctx, const char* name, size_t* out_len) {
    if (!ctx || !name) {
        return NULL;
    }
    bool is_declared = false;
    for (size_t i = 0; i < ctx->def->n_reads; i++) {
        if (strcmp(ctx->def->reads[i].name, name) == 0) {
            is_declared = true;
            break;
        }
    }
    if (!is_declared) {
        // Step author bug — declared reads should match step_def. Fail loud:
        // a silently-returned NULL here is exactly the "missing read silently
        // returns 0 bytes" trap that bit v7-boundary-signals (2026-05-22).
        // Print to stderr (always visible, no logging-level dependency),
        // log structured, and abort so this surfaces at first invocation
        // rather than cascading into bogus algorithm output.
        fprintf(stderr,
                "\n[runner] FATAL: step '%s' tried to read undeclared artifact '%s'.\n"
                "  Declared reads (%zu):\n",
                ctx->def->name, name, ctx->def->n_reads);
        for (size_t i = 0; i < ctx->def->n_reads; i++) {
            fprintf(stderr, "    - %s%s\n",
                    ctx->def->reads[i].name,
                    ctx->def->reads[i].array ? " (array)" : "");
        }
        fprintf(stderr,
                "  Fix: add the artifact to the step's reads[] (and its .doc.json),\n"
                "       or remove the read call if it's leftover.\n\n");
        LOG_EVENT(SUB_RUNNER, LOG_ERROR, EV_RUNNER_STEP_FAILED,
                  LF_RUN_ID(ctx->rs->run_id),
                  LF_STEP(ctx->def->name),
                  LF_STR("artifact", name),
                  LF_STR("reason", "undeclared_read"));
        abort();
    }

    // First attempt: read from the artifact_map populated when an upstream
    // step (or the pipeline-input bootstrap) ran. Most calls succeed here.
    hash_t h;
    pthread_mutex_lock(&ctx->rs->lock);
    bool found = artifact_map_get_locked(ctx->rs, name, &h);
    pthread_mutex_unlock(&ctx->rs->lock);

    if (found && try_open_read_locked(ctx, &h, out_len) == 0) {
        return ctx->read_handles[ctx->n_read_handles - 1].bytes;
    }

    // F2 — desync recovery. Either the artifact_map didn't have an entry
    // (upstream's manifest walk silently failed?) or the entry pointed
    // at a content hash whose .bin was unlinked behind the store's back
    // (manual rm, container restart with tmpfs, eviction race). Both
    // cases are recoverable: invalidate the producer's intent entry,
    // re-execute the producer (cache miss → runs body → writes to
    // store → repopulates artifact_map), retry the lookup.
    //
    // We bound recovery to one attempt: if the producer can't be
    // identified or the re-execute also fails, give up and return NULL
    // (legitimate failure that the runner will surface as step_failed).
    size_t producer_idx = find_writer_step(ctx->rs->pipeline, name);
    if (producer_idx == (size_t)-1) {
        // Special case: pipeline-input is bootstrapped at run start with
        // a synthesised hash; if it's missing we can't regenerate it.
        return NULL;
    }
    const step_def_t* producer_def = pipeline_step_def_at(ctx->rs->pipeline, producer_idx);

    char hex[65] = "<missing>";
    if (found) {
        hash_format_hex(&h, hex);
    }
    LOG_EVENT(SUB_RUNNER, LOG_WARN, EV_RUNNER_CACHE_DESYNC,
              LF_RUN_ID(ctx->rs->run_id),
              LF_STEP(ctx->def->name),
              LF_STR("artifact", name),
              LF_STR("producer", producer_def->name),
              LF_STR("hash", hex),
              LF_BOOL("artifact_map_had_entry", found));

    // Step 1: purge the lying in-memory entry. Without this, the
    // producer's manifest walk via resolve_manifest would see
    // `store_contains == true` and happily reuse the stale entry
    // (whose .bin file is gone) — defeating the recovery.
    if (found) {
        if (store_forget(ctx->runner->store, &h) != 0) {
            // Pinned by some other read — can't recover safely. Bail
            // and let the runner surface the failure.
            return NULL;
        }
    }

    // Step 2: re-execute the producer. execute_step will see the
    // intent entry, walk the manifest, find the now-missing content
    // (we just unlinked it), return rv==1 (stale), invalidate the
    // intent, retry, run the producer's body, write fresh content,
    // populate artifact_map. That's the existing self-heal path —
    // now it actually fires because store_contains stops lying.
    bool unused_cache_hit;
    if (execute_step(ctx->runner, ctx->rs, producer_idx, &unused_cache_hit) != 0) {
        return NULL; // recovery failed — let the caller bail
    }

    // Retry the lookup post-recovery.
    pthread_mutex_lock(&ctx->rs->lock);
    found = artifact_map_get_locked(ctx->rs, name, &h);
    pthread_mutex_unlock(&ctx->rs->lock);
    if (!found) {
        return NULL;
    }
    if (try_open_read_locked(ctx, &h, out_len) != 0) {
        ctx->had_error = 1;
        return NULL;
    }
    return ctx->read_handles[ctx->n_read_handles - 1].bytes;
}

// Locate the write index by name; common preamble for the two write helpers.
// Returns the write index, or -1 on lookup failure.
static ssize_t find_write_index(step_context_t* ctx, const char* name) {
    for (size_t i = 0; i < ctx->def->n_writes; i++) {
        if (strcmp(ctx->def->writes[i].name, name) == 0) {
            return (ssize_t)i;
        }
    }
    return -1;
}

// Common write tail: hash, store_put (ownership transfer), record in manifest
// + artifact map. `bytes` ownership transfers to the store on success and
// is freed by us on failure — caller must not touch `bytes` after this call.
static int finalize_write(step_context_t* ctx, ssize_t wi, const char* name, uint8_t* bytes,
                          size_t len) {
    hash_t key = hash_bytes(bytes, len);
    if (store_put(ctx->runner->store, &key, bytes, len) != 0) {
        ctx->had_error = 1;
        return -1;
    }
    ctx->write_slots[wi].name = ctx->def->writes[wi].name;
    ctx->write_slots[wi].content_hash = key;
    ctx->write_slots[wi].populated = true;
    pthread_mutex_lock(&ctx->rs->lock);
    int rc = artifact_map_put_locked(ctx->rs, name, &key);
    pthread_mutex_unlock(&ctx->rs->lock);
    if (rc != 0) {
        ctx->had_error = 1;
        return -1;
    }
    return 0;
}

int step_write_output(step_context_t* ctx, const char* name, const void* bytes, size_t len) {
    if (!ctx || !name) {
        return -1;
    }
    ssize_t wi = find_write_index(ctx, name);
    if (wi < 0) {
        return -1;
    }
    // Copy semantics: malloc + memcpy then transfer ownership to the store.
    uint8_t* owned = NULL;
    if (len > 0) {
        owned = malloc(len);
        if (!owned) {
            ctx->had_error = 1;
            return -1;
        }
        memcpy(owned, bytes, len);
    }
    return finalize_write(ctx, wi, name, owned, len);
}

int step_write_output_owned(step_context_t* ctx, const char* name, uint8_t* bytes, size_t len) {
    if (!ctx || !name) {
        if (bytes) {
            free(bytes);
        }
        return -1;
    }
    ssize_t wi = find_write_index(ctx, name);
    if (wi < 0) {
        free(bytes);
        return -1;
    }
    // Ownership-transfer semantics: hand the caller's heap buffer straight
    // to the store. No extra copy.
    return finalize_write(ctx, wi, name, bytes, len);
}

int step_write_output_stream(step_context_t* ctx, const char* name, const void* bytes, size_t len,
                             bool final) {
    if (!ctx || !name) {
        return -1;
    }
    ssize_t wi = find_write_index(ctx, name);
    if (wi < 0) {
        ctx->had_error = 1;
        return -1;
    }
    // Streaming declared per-write; reject if the def didn't opt in or if the
    // step already used a non-streaming write helper for the same artifact.
    if (!ctx->def->writes[wi].streaming) {
        ctx->had_error = 1;
        return -1;
    }
    if (ctx->write_slots[wi].populated) {
        // Already finalised via a previous final=true call (or somehow a
        // non-streaming helper wrote it).
        ctx->had_error = 1;
        return -1;
    }
    stream_slot_t* s = &ctx->stream_slots[wi];
    if (s->finalized) {
        ctx->had_error = 1;
        return -1;
    }
    s->started = true;
    if (len > 0 && bytes) {
        size_t need = s->len + len;
        if (need > s->cap) {
            // Grow geometrically; start at 64KiB so small streams don't
            // pay for many tiny reallocs.
            size_t new_cap = s->cap ? s->cap * 2 : (64 * 1024);
            while (new_cap < need) new_cap *= 2;
            uint8_t* nb = realloc(s->buf, new_cap);
            if (!nb) {
                ctx->had_error = 1;
                return -1;
            }
            s->buf = nb;
            s->cap = new_cap;
        }
        memcpy(s->buf + s->len, bytes, len);
        s->len += len;
    }
    uint32_t this_seq = s->seq++;

    // Fire the on_stream_chunk callback BEFORE finalisation so the server can
    // forward the chunk to clients in real time. On the final chunk the hash
    // is computed below and passed alongside.
    hash_t final_hash = {{0}};
    if (final) {
        final_hash = hash_bytes(s->buf, s->len);
    }
    const runner_callbacks_t* cb = ctx->rs && ctx->rs->cb ? ctx->rs->cb : NULL;
    if (cb && cb->on_stream_chunk) {
        cb->on_stream_chunk(cb->ctx, ctx->rs->run_id, name, this_seq, final, bytes, len,
                            final ? &final_hash : NULL);
    }

    if (!final) {
        return 0;
    }

    // Finalisation: hand the accumulator to the store and run the normal
    // finalize_write tail. The store_put copies via the same ownership-
    // transfer path as step_write_output_owned, so the buf pointer is
    // either taken by the store on success or freed on failure — null it
    // here so the post-exec cleanup loop doesn't double-free.
    uint8_t* buf = s->buf;
    size_t total = s->len;
    s->buf = NULL;
    s->len = 0;
    s->cap = 0;
    s->finalized = true;
    return finalize_write(ctx, wi, name, buf, total);
}

const char* step_get_params(step_context_t* ctx) {
    return ctx ? ctx->params_json : NULL;
}

store_t* step_runner_store(step_context_t* ctx) {
    return ctx ? ctx->runner->store : NULL;
}

intent_map_t* step_runner_intent_map(step_context_t* ctx) {
    return ctx ? ctx->runner->intent_map : NULL;
}

// ---------------------------------------------------------------------------
// Array-artifact helpers (T13 / §5.3 of UNIFIED-ARCHITECTURE.md).
//
// On-disk layouts (raw bytes; the artifact_kind_t is declared in step
// metadata, not stored in the bytes themselves):
//
// ARRAY_INLINE  (Phase 5.1d format — record bytes 8-byte aligned):
//   u32  n_items
//   u32  stride_bytes    (0 → variable-length, see below)
//   bytes...
//
//   stride > 0  (fixed-stride):
//     Item k starts at offset 8 + stride*k. Records contain typed fields,
//     so the caller MUST pass a stride that's a multiple of the field's
//     alignment requirement (typically 8 for any record holding a double).
//     The runtime cannot enforce this — picking an aligned stride is part
//     of the writer's contract.
//
//   stride == 0 (variable-length):
//     Records are emitted with a fixed 8-byte header + their bytes +
//     trailing padding to the next 8-byte boundary, so every record's
//     payload starts at an 8-aligned offset:
//        [u32 record_len][u32 _pad = 0][record bytes][0..7 B trailing pad]
//     The pre-Phase-5.1d format ([u32 len][bytes] back-to-back, no
//     padding) is no longer accepted by the reader. Step versions that
//     write ARRAY_INLINE were bumped on the 5.1d migration so existing
//     intent-map entries get rejected; the new bytes content-hash
//     differently anyway.
//
// ARRAY_BY_HASH:
//   u32  n_items
//   u32  reserved (0)
//   hash_t[0]            (32 bytes)
//   hash_t[1]
//   ...
// ---------------------------------------------------------------------------

// 8-byte alignment for ARRAY_INLINE record bytes — large enough for
// `double` (the most-aligned scalar in our typed records).
#define ARRAY_INLINE_RECORD_ALIGN 8u
static inline size_t align_up_8(size_t x) {
    return (x + (ARRAY_INLINE_RECORD_ALIGN - 1u)) & ~(size_t)(ARRAY_INLINE_RECORD_ALIGN - 1u);
}

// Read little-endian u32 without worrying about alignment.
static uint32_t read_u32_le(const uint8_t* p) {
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static void write_u32_le(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

// Locate an array-typed read by name; return 0 with out-params set, -1 on
// "not declared as array" or missing. Caller must have already called
// step_read once for this name so the artifact is open in ctx->read_handles.
static int array_read_open(step_context_t* ctx, const char* name,
                           const uint8_t** out_bytes, size_t* out_len,
                           bool* out_is_by_hash) {
    if (!ctx || !name)
        return -1;
    bool found = false;
    bool is_array = false;
    for (size_t i = 0; i < ctx->def->n_reads; i++) {
        if (strcmp(ctx->def->reads[i].name, name) == 0) {
            found = true;
            is_array = ctx->def->reads[i].array;
            break;
        }
    }
    if (!found || !is_array)
        return -1;

    size_t len = 0;
    const void* bytes = step_read(ctx, name, &len);
    if (!bytes || len < 8)
        return -1;
    // Caller is responsible for the distinction; we don't store kind in the
    // bytes. But we can sanity-check the header in each specific helper.
    *out_bytes = bytes;
    *out_len = len;
    (void)out_is_by_hash; // layer-distinction is done per helper
    return 0;
}

size_t step_array_len(step_context_t* ctx, const char* name) {
    const uint8_t* bytes = NULL;
    size_t len = 0;
    if (array_read_open(ctx, name, &bytes, &len, NULL) != 0) {
        return 0;
    }
    if (len < 4)
        return 0;
    return (size_t)read_u32_le(bytes);
}

int step_array_read_bytes(step_context_t* ctx, const char* name, size_t i,
                          const uint8_t** out_bytes, size_t* out_len) {
    const uint8_t* bytes = NULL;
    size_t total = 0;
    if (array_read_open(ctx, name, &bytes, &total, NULL) != 0 || total < 8) {
        return -1;
    }
    uint32_t n = read_u32_le(bytes);
    uint32_t stride = read_u32_le(bytes + 4);
    if (i >= n)
        return -1;
    if (stride > 0) {
        size_t off = 8 + (size_t)stride * i;
        if (off + stride > total)
            return -1;
        if (out_bytes)
            *out_bytes = bytes + off;
        if (out_len)
            *out_len = stride;
        return 0;
    }
    // Variable-length items: walk prefixes. Phase 5.1d layout — each
    // record header is 8 bytes ([u32 len][u32 pad]), record payload is
    // 8-aligned, trailing pad rounds up to the next 8-boundary so the
    // next record header is again 8-aligned.
    size_t cursor = 8;
    for (uint32_t k = 0; k <= i; k++) {
        if (cursor + 8 > total)
            return -1;
        uint32_t item_len = read_u32_le(bytes + cursor);
        // bytes + cursor + 4 holds the 4-byte zero pad — we don't read it.
        size_t payload_off = cursor + 8;
        if (payload_off + item_len > total)
            return -1;
        if (k == i) {
            if (out_bytes)
                *out_bytes = bytes + payload_off;
            if (out_len)
                *out_len = item_len;
            return 0;
        }
        cursor = payload_off + align_up_8(item_len);
    }
    return -1;
}

int step_array_open(step_context_t* ctx, const char* name, size_t i,
                    store_handle_t* out_handle) {
    const uint8_t* bytes = NULL;
    size_t total = 0;
    if (array_read_open(ctx, name, &bytes, &total, NULL) != 0 || total < 8) {
        return -1;
    }
    uint32_t n = read_u32_le(bytes);
    if (i >= n)
        return -1;
    // Manifest layout: 4 bytes n + 4 bytes reserved + N × 32 bytes hash.
    size_t off = 8 + sizeof(hash_t) * i;
    if (off + sizeof(hash_t) > total)
        return -1;
    hash_t h;
    memcpy(h.bytes, bytes + off, sizeof(h.bytes));
    return store_lookup(ctx->runner->store, &h, out_handle);
}

// Common write path shared by the two array-write helpers. Serialises the
// assembled manifest / inline-blob and routes through step_write_output.
static int array_write_common(step_context_t* ctx, const char* name,
                              const uint8_t* bytes, size_t len, bool expect_array) {
    if (!ctx || !name)
        return -1;
    ssize_t wi = -1;
    for (size_t i = 0; i < ctx->def->n_writes; i++) {
        if (strcmp(ctx->def->writes[i].name, name) == 0) {
            wi = (ssize_t)i;
            break;
        }
    }
    if (wi < 0)
        return -1;
    if (expect_array && !ctx->def->writes[wi].array)
        return -1;
    return step_write_output(ctx, name, bytes, len);
}

int step_write_array_inline(step_context_t* ctx, const char* name,
                            const uint8_t* item_bytes, size_t total_len,
                            size_t n, size_t stride) {
    // Header is 8 bytes: [u32 n][u32 stride]. Items follow.
    if (stride > 0) {
        // Fixed-stride: caller laid out the records and is responsible
        // for the stride being a multiple of the record's alignment (see
        // the format spec at the top of this file). Pass through verbatim.
        // Frama-C audit finding RUN-1: guard `8 + total_len` overflow.
        if (total_len > SIZE_MAX - 8) return -1;
        size_t out_len = 8 + total_len;
        uint8_t* buf = malloc(out_len);
        if (!buf)
            return -1;
        write_u32_le(buf, (uint32_t)n);
        write_u32_le(buf + 4, (uint32_t)stride);
        if (total_len > 0 && item_bytes)
            memcpy(buf + 8, item_bytes, total_len);
        int rc = array_write_common(ctx, name, buf, out_len, true);
        free(buf);
        return rc;
    }

    // Variable-length: caller passed a blob already framed for the wire
    // — `{u32 record_length, u32 zero_pad, payload, trailing_pad_to_8}` ×
    // `n` — typically built via blob_push() in <v3/blob.h>. Walk once for
    // validation, then prepend the artifact header and emit verbatim.
    size_t cursor = 0;
    for (size_t k = 0; k < n; k++) {
        if (cursor + 8 > total_len)
            return -1;
        uint32_t item_len = read_u32_le(item_bytes + cursor);
        size_t framed = 8 + align_up_8(item_len);
        if (cursor + framed > total_len)
            return -1;
        cursor += framed;
    }
    if (cursor != total_len)
        return -1; // caller's blob length doesn't match its declared records

    size_t out_len = 8 + total_len;
    uint8_t* buf = malloc(out_len);
    if (!buf)
        return -1;
    write_u32_le(buf, (uint32_t)n);
    write_u32_le(buf + 4, 0); // stride=0
    if (total_len > 0 && item_bytes) {
        memcpy(buf + 8, item_bytes, total_len);
    }
    int rc = array_write_common(ctx, name, buf, out_len, true);
    free(buf);
    return rc;
}

int step_write_array_by_hash(step_context_t* ctx, const char* name,
                             const hash_t* element_hashes, size_t n) {
    // Frama-C audit finding RUN-2: guard `n * sizeof(hash_t)` and
    // `8 + ...` overflow chain.
    if (n > (SIZE_MAX - 8) / sizeof(hash_t)) return -1;
    size_t out_len = 8 + n * sizeof(hash_t);
    uint8_t* buf = malloc(out_len);
    if (!buf)
        return -1;
    write_u32_le(buf, (uint32_t)n);
    write_u32_le(buf + 4, 0);
    if (n > 0 && element_hashes)
        memcpy(buf + 8, element_hashes, n * sizeof(hash_t));
    int rc = array_write_common(ctx, name, buf, out_len, true);
    free(buf);
    return rc;
}

// ----------------------------------------------------------------------------
// Runner
// ----------------------------------------------------------------------------

runner_t* runner_new(const runner_config_t* cfg) {
    if (!cfg || !cfg->store || !cfg->intent_map) {
        return NULL;
    }
    runner_t* r = calloc(1, sizeof(runner_t));
    if (!r) {
        return NULL;
    }
    r->store = cfg->store;
    r->intent_map = cfg->intent_map;
    atomic_store_explicit(&r->next_run_seq, 1, memory_order_relaxed);
    int threads = cfg->thread_count > 0 ? cfg->thread_count : 4;
    r->pool = pool_create((size_t)threads, 256);
    if (!r->pool) {
        free(r);
        return NULL;
    }
    // Frama-C audit follow-up: rollback on mutex_init failure.
    if (pthread_mutex_init(&r->active_lock, NULL) != 0) {
        pool_destroy(r->pool);
        free(r);
        return NULL;
    }
    return r;
}

void runner_free(runner_t* r) {
    if (!r) {
        return;
    }
    pool_destroy(r->pool);
    pthread_mutex_destroy(&r->active_lock);
    free(r->active_runs);
    free(r);
}

intent_map_t* runner_intent_map(runner_t* r) {
    return r ? r->intent_map : NULL;
}

int runner_cancel(runner_t* r, const char* run_id) {
    if (!r || !run_id) {
        return -1;
    }
    int rc = -1;
    pthread_mutex_lock(&r->active_lock);
    for (size_t i = 0; i < r->n_active; i++) {
        run_state_t* rs = r->active_runs[i];
        if (strcmp(rs->run_id, run_id) != 0) {
            continue;
        }
        pthread_mutex_lock(&rs->lock);
        if (!rs->cancel) {
            rs->cancel = true;
            rs->cancel_external = true;
            rs->run_rc = -1;
        }
        pthread_cond_broadcast(&rs->done_cv);
        pthread_mutex_unlock(&rs->lock);
        rc = 0;
        break;
    }
    pthread_mutex_unlock(&r->active_lock);
    return rc;
}

// ----------------------------------------------------------------------------
// Step body trampoline + manifest validate-on-use
// ----------------------------------------------------------------------------

typedef struct body_ctx {
    runner_t* r;
    run_state_t* rs;
    size_t step_idx;
    const step_def_t* def;
    const char* params_json;
    // Set true by body_trampoline as its FIRST action when cached_compute
    // calls it on an intent-map miss. execute_step reads this AFTER
    // cached_compute returns to report cache_hit accurately. This
    // replaces a pre-probe `intent_map_get` call that raced with the
    // probe inside cached_compute under ctest -j load — under contention
    // an intent_map_put on a peer step's entry could land between the
    // two gets, flipping our pre-probe to "miss" while the inner probe
    // still hit, so cache_hit was reported false even though the body
    // didn't run. See task #82.
    bool body_ran;
} body_ctx_t;

// body_trampoline: invoked by cached_compute on an intent-map miss. Runs the
// step's exec_fn, collects each declared write's content hash, packs them into
// a step-output manifest (HASH-MODEL.md §3). The manifest bytes become the
// cached_compute result — content-hashed, stored, and recorded against the
// step identity in the intent map.
//
// Returns 0 on success (with *out_bytes / *out_len set to the malloc'd manifest
// buffer), -1 on any failure.
static int body_trampoline(void* vctx, uint8_t** out_bytes, size_t* out_len) {
    body_ctx_t* bc = vctx;
    bc->body_ran = true;
    const step_def_t* def = bc->def;

    write_slot_t* slots = def->n_writes ? safe_calloc_array(def->n_writes, sizeof(write_slot_t)) : NULL;
    if (def->n_writes && !slots) {
        return -1;
    }
    stream_slot_t* streams = def->n_writes ? safe_calloc_array(def->n_writes, sizeof(stream_slot_t)) : NULL;
    if (def->n_writes && !streams) {
        free(slots);
        return -1;
    }

    step_context_t ctx = {
        .runner = bc->r,
        .rs = bc->rs,
        .def = def,
        .params_json = bc->params_json,
        .write_slots = slots,
        .stream_slots = streams,
        .read_handles = NULL,
        .n_read_handles = 0,
        .had_error = 0,
    };
    int rc = def->exec(&ctx);

    for (size_t i = 0; i < ctx.n_read_handles; i++) {
        store_release(bc->r->store, &ctx.read_handles[i]);
    }
    free(ctx.read_handles);

    // Free any streaming accumulators that didn't reach final=true. A
    // finalized stream's buffer was already transferred to the store via
    // step_write_output_owned in the finalisation path, so streams[i].buf
    // is NULL by then.
    for (size_t i = 0; i < def->n_writes; i++) {
        free(streams[i].buf);
    }
    free(streams);

    if (rc != 0 || ctx.had_error) {
        free(slots);
        return -1;
    }

    // Every declared write must have been produced.
    for (size_t i = 0; i < def->n_writes; i++) {
        if (!slots[i].populated) {
            free(slots);
            return -1;
        }
    }

    // Pack manifest.
    step_manifest_entry_t* entries =
        def->n_writes ? safe_calloc_array(def->n_writes, sizeof(step_manifest_entry_t)) : NULL;
    if (def->n_writes && !entries) {
        free(slots);
        return -1;
    }
    for (size_t i = 0; i < def->n_writes; i++) {
        entries[i].name = slots[i].name;
        entries[i].content_hash = slots[i].content_hash;
    }
    int pack_rc = step_manifest_pack(entries, def->n_writes, out_bytes, out_len);
    free(entries);
    free(slots);
    return pack_rc;
}

// Context for the manifest-walk callback that populates the run-local artifact
// map AND validates every referenced content hash is still in the store
// (HASH-MODEL.md §11.2).
typedef struct resolve_ctx {
    run_state_t* rs;
    store_t* store;
    bool stale;
    bool err;
} resolve_ctx_t;

static int resolve_cb(void* vctx, const char* name, size_t name_len, const hash_t* content_hash) {
    resolve_ctx_t* rc = vctx;
    if (!store_contains(rc->store, content_hash)) {
        rc->stale = true;
        return -1; // stop walking
    }
    // Need a NUL-terminated name.
    char* tmp = malloc(name_len + 1);
    if (!tmp) {
        rc->err = true;
        return -1;
    }
    memcpy(tmp, name, name_len);
    tmp[name_len] = 0;
    pthread_mutex_lock(&rc->rs->lock);
    int put_rc = artifact_map_put_locked(rc->rs, tmp, content_hash);
    pthread_mutex_unlock(&rc->rs->lock);
    free(tmp);
    if (put_rc != 0) {
        rc->err = true;
        return -1;
    }
    return 0;
}

// Walk a step-output manifest: check every referenced content hash is present
// (store_contains) and populate the run-local artifact_map. Returns 0 on OK,
// 1 on "stale — some referenced content is missing", -1 on error.
static int resolve_manifest(run_state_t* rs, store_t* store, const hash_t* manifest_hash) {
    store_handle_t h;
    if (store_lookup(store, manifest_hash, &h) != 0) {
        return 1; // the manifest itself is gone
    }
    resolve_ctx_t rc = {.rs = rs, .store = store, .stale = false, .err = false};
    int walk_rc = step_manifest_walk(h.bytes, h.len, resolve_cb, &rc);
    store_release(store, &h);
    if (rc.stale) {
        return 1;
    }
    if (walk_rc != 0 || rc.err) {
        return -1;
    }
    return 0;
}

// Is the step non-deterministic? If any declared write opts out of
// identity-based skip, we bypass the intent map entirely.
static bool step_is_nondeterministic(const step_def_t* def) {
    for (size_t i = 0; i < def->n_writes; i++) {
        if (def->writes[i].nondeterministic) {
            return true;
        }
    }
    return false;
}

// Execute one step: resolve read hashes, compute identity, consult the intent
// map via cached_compute, parse the returned manifest and validate-on-use. On
// a hit with stale manifest entries (evicted content), invalidate the intent
// entry and retry once.
//
// Returns 0 on success, -1 on any failure. *out_cache_hit is set to true iff
// the exec_fn did NOT run.
static int execute_step(runner_t* r, run_state_t* rs, size_t step_idx, bool* out_cache_hit) {
    const step_def_t* def = pipeline_step_def_at(rs->pipeline, step_idx);
    const char* params = pipeline_step_params_at(rs->pipeline, step_idx);

    hash_t* read_hashes = def->n_reads ? safe_calloc_array(def->n_reads, sizeof(hash_t)) : NULL;
    if (def->n_reads && !read_hashes) {
        return -1;
    }
    pthread_mutex_lock(&rs->lock);
    for (size_t i = 0; i < def->n_reads; i++) {
        if (!artifact_map_get_locked(rs, def->reads[i].name, &read_hashes[i])) {
            pthread_mutex_unlock(&rs->lock);
            free(read_hashes);
            return -1;
        }
    }
    pthread_mutex_unlock(&rs->lock);

    hash_t id_hash = compute_step_identity_hash(def, params, read_hashes, def->n_reads);
    free(read_hashes);

    body_ctx_t bc = {.r = r, .rs = rs, .step_idx = step_idx, .def = def, .params_json = params,
                     .body_ran = false};

    // Non-deterministic step: skip the intent map entirely; always run exec
    // (HASH-MODEL.md §9). body_trampoline still content-hashes outputs and
    // stores them, but we don't record an intent mapping.
    if (step_is_nondeterministic(def)) {
        uint8_t* manifest_bytes = NULL;
        size_t manifest_len = 0;
        if (body_trampoline(&bc, &manifest_bytes, &manifest_len) != 0) {
            return -1;
        }
        free(manifest_bytes);
        // step_write_output already populated artifact_map for each write.
        *out_cache_hit = false;
        return 0;
    }

    // Deterministic: go through cached_compute. On hit → validate manifest.
    // On stale (evicted content) → invalidate and retry (bounded to 1).
    //
    // cache_hit reporting: we DON'T pre-probe the intent map and compare —
    // that races with peer-step intent_map_put under ctest -j load (#82).
    // Instead, body_trampoline sets bc.body_ran = true as its first action,
    // so after cached_compute returns we know exactly whether the body
    // executed in *this* iteration, regardless of intent-map churn.
    for (int retry = 0; retry <= 1; retry++) {
        bc.body_ran = false;

        hash_t manifest_hash;
        if (cached_compute(r->intent_map, r->store, &id_hash, body_trampoline, &bc,
                           &manifest_hash) != 0) {
            return -1;
        }

        int rv = resolve_manifest(rs, r->store, &manifest_hash);
        if (rv == 0) {
            *out_cache_hit = !bc.body_ran;
            return 0;
        }
        if (rv < 0) {
            return -1;
        }
        // rv == 1: the cached manifest references missing content. Invalidate
        // and retry. On retry cached_compute re-runs the body.
        intent_map_invalidate(r->intent_map, &id_hash);
        (void)retry; // retry loop terminates after one extra iteration
    }
    return -1;
}

// Forward decl — the worker submits successors by calling back into the pool.
static void step_task_fn(void* arg);

// Submit a task for plan_pos onto the runner's pool. Caller must hold rs->lock
// around the n_pending bump; pool_submit is called outside the lock.
static int submit_step_task_locked(runner_t* r, run_state_t* rs, size_t plan_pos) {
    step_task_t* t = malloc(sizeof(step_task_t));
    if (!t) {
        return -1;
    }
    t->r = r;
    t->rs = rs;
    t->plan_pos = plan_pos;
    rs->n_pending++;
    pthread_mutex_unlock(&rs->lock);
    int rc = pool_submit(r->pool, step_task_fn, t);
    pthread_mutex_lock(&rs->lock);
    if (rc != 0) {
        rs->n_pending--;
        free(t);
        return -1;
    }
    return 0;
}

static void step_task_fn(void* arg) {
    step_task_t* t = (step_task_t*)arg;
    runner_t* r = t->r;
    run_state_t* rs = t->rs;
    size_t plan_pos = t->plan_pos;
    size_t step_idx = rs->plan[plan_pos];
    const step_def_t* def = pipeline_step_def_at(rs->pipeline, step_idx);

    // Skip if the run is already cancelled (a peer failed).
    pthread_mutex_lock(&rs->lock);
    bool cancelled = rs->cancel;
    pthread_mutex_unlock(&rs->lock);

    if (cancelled) {
        pthread_mutex_lock(&rs->lock);
        rs->n_pending--;
        pthread_cond_broadcast(&rs->done_cv);
        pthread_mutex_unlock(&rs->lock);
        free(t);
        return;
    }

    const runner_callbacks_t* cb = rs->cb;
    if (cb && cb->on_step_started) {
        cb->on_step_started(cb->ctx, rs->run_id, def->name);
    }
    LOG_EVENT(SUB_RUNNER, LOG_INFO, EV_RUNNER_STEP_STARTED, LF_RUN_ID(rs->run_id),
              LF_STEP(def->name));

    uint64_t t_start = now_us();
    bool cache_hit = false;
    int rc = execute_step(r, rs, step_idx, &cache_hit);
    uint64_t dt = now_us() - t_start;

    if (rc != 0) {
        LOG_EVENT(SUB_RUNNER, LOG_ERROR, EV_RUNNER_STEP_FAILED, LF_RUN_ID(rs->run_id),
                  LF_STEP(def->name), LF_UINT("duration_us", dt));
        if (cb && cb->on_error) {
            cb->on_error(cb->ctx, rs->run_id, def->name, "step failed");
        }
        pthread_mutex_lock(&rs->lock);
        rs->cancel = true;
        rs->run_rc = -1;
        rs->n_pending--;
        pthread_cond_broadcast(&rs->done_cv);
        pthread_mutex_unlock(&rs->lock);
        free(t);
        return;
    }

    // Success. Under lock: bookkeep + decrement successors + collect ready.
    // IMPORTANT: do not decrement rs->n_pending yet. Main blocks on n_pending
    // dropping to 0; we must submit all newly-ready successors first, so their
    // own n_pending bumps happen before our decrement. Otherwise main can
    // wake and tear down rs mid-submit.
    pthread_mutex_lock(&rs->lock);
    if (cache_hit) {
        rs->n_cache_hits++;
    } else {
        rs->n_executed++;
    }
    size_t ready_stack[32];
    size_t ready_n = 0;
    size_t* ready_heap = NULL;
    for (size_t i = 0; i < rs->n_successors[plan_pos]; i++) {
        size_t succ = rs->successors[plan_pos][i];
        if (rs->in_degree[succ] == 0) {
            continue;
        }
        if (--rs->in_degree[succ] == 0) {
            if (ready_n < 32) {
                ready_stack[ready_n++] = succ;
            } else {
                if (!ready_heap) {
                    ready_heap = malloc(rs->n_successors[plan_pos] * sizeof(size_t));
                    if (ready_heap) {
                        memcpy(ready_heap, ready_stack, ready_n * sizeof(size_t));
                    }
                }
                if (ready_heap) {
                    ready_heap[ready_n++] = succ;
                }
            }
        }
    }
    pthread_mutex_unlock(&rs->lock);

    // Callbacks fire without holding rs->lock so a slow callback doesn't
    // block peers' step execution. rs is safe to access because our own
    // n_pending contribution hasn't been decremented — main is still waiting.
    LOG_EVENT(SUB_RUNNER, LOG_INFO, EV_RUNNER_STEP_COMPLETE, LF_RUN_ID(rs->run_id),
              LF_STEP(def->name), LF_BOOL("cache_hit", cache_hit), LF_UINT("duration_us", dt));
    if (cb && cb->on_step_complete) {
        cb->on_step_complete(cb->ctx, rs->run_id, def->name, cache_hit, dt);
    }
    if (cb && cb->on_artifact_ready) {
        for (size_t w = 0; w < def->n_writes; w++) {
            hash_t h;
            pthread_mutex_lock(&rs->lock);
            bool found = artifact_map_get_locked(rs, def->writes[w].name, &h);
            pthread_mutex_unlock(&rs->lock);
            if (found) {
                cb->on_artifact_ready(cb->ctx, rs->run_id, def->writes[w].name, &h);
            }
        }
    }

    // Submit newly-ready successors. Each submit bumps n_pending under lock;
    // our own n_pending is still set so main can't exit.
    size_t* ready = ready_heap ? ready_heap : ready_stack;
    for (size_t i = 0; i < ready_n; i++) {
        pthread_mutex_lock(&rs->lock);
        if (!rs->cancel) {
            if (submit_step_task_locked(r, rs, ready[i]) != 0) {
                rs->cancel = true;
                rs->run_rc = -1;
            }
        }
        pthread_mutex_unlock(&rs->lock);
    }
    free(ready_heap);

    // Now decrement our own pending count. Must happen after all submissions
    // so main sees n_pending drop to 0 only when every new task is in flight.
    // Broadcast under lock so main's cond_wait serializes with us.
    pthread_mutex_lock(&rs->lock);
    rs->n_pending--;
    pthread_cond_broadcast(&rs->done_cv);
    pthread_mutex_unlock(&rs->lock);

    free(t);
}

// Compute in-degree and successor list for each plan entry. "Successor" means
// another plan entry that reads one of this plan entry's writes. The dag_plan
// already guarantees a consistent topological ordering.
static int build_dependency_graph(run_state_t* rs) {
    size_t n = rs->plan_len;
    rs->in_degree = n ? safe_calloc_array(n, sizeof(size_t)) : NULL;
    rs->successors = n ? safe_calloc_array(n, sizeof(size_t*)) : NULL;
    rs->n_successors = n ? safe_calloc_array(n, sizeof(size_t)) : NULL;
    if (n && (!rs->in_degree || !rs->successors || !rs->n_successors)) {
        return -1;
    }
    // First pass: count successors for each plan entry so we can size allocs.
    for (size_t i = 0; i < n; i++) {
        size_t i_step = rs->plan[i];
        const step_def_t* di = pipeline_step_def_at(rs->pipeline, i_step);
        for (size_t j = 0; j < n; j++) {
            if (j == i) {
                continue;
            }
            size_t j_step = rs->plan[j];
            const step_def_t* dj = pipeline_step_def_at(rs->pipeline, j_step);
            for (size_t r = 0; r < dj->n_reads; r++) {
                for (size_t w = 0; w < di->n_writes; w++) {
                    if (strcmp(dj->reads[r].name, di->writes[w].name) == 0) {
                        rs->n_successors[i]++;
                        rs->in_degree[j]++;
                    }
                }
            }
        }
    }
    // Second pass: fill the successor arrays.
    for (size_t i = 0; i < n; i++) {
        if (rs->n_successors[i] > 0) {
            rs->successors[i] = calloc(rs->n_successors[i], sizeof(size_t));
            if (!rs->successors[i]) {
                return -1;
            }
        }
    }
    size_t* fill_idx = n ? safe_calloc_array(n, sizeof(size_t)) : NULL;
    if (n && !fill_idx) {
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        size_t i_step = rs->plan[i];
        const step_def_t* di = pipeline_step_def_at(rs->pipeline, i_step);
        for (size_t j = 0; j < n; j++) {
            if (j == i) {
                continue;
            }
            size_t j_step = rs->plan[j];
            const step_def_t* dj = pipeline_step_def_at(rs->pipeline, j_step);
            for (size_t rdx = 0; rdx < dj->n_reads; rdx++) {
                for (size_t w = 0; w < di->n_writes; w++) {
                    if (strcmp(dj->reads[rdx].name, di->writes[w].name) == 0) {
                        rs->successors[i][fill_idx[i]++] = j;
                    }
                }
            }
        }
    }
    free(fill_idx);
    return 0;
}

int runner_run(runner_t* r, const pipeline_t* pipeline, const void* input_bytes, size_t input_len,
               const char* const* subscribe, size_t n_subscribe,
               const runner_callbacks_t* callbacks) {
    if (!r || !pipeline) {
        return -1;
    }
    if (pipeline_validate(pipeline) != 0) {
        return -1;
    }

    run_state_t rs = {0};
    rs.pipeline = pipeline;
    rs.cb = callbacks;
    // Frama-C audit follow-up: pthread init rc checks.
    if (pthread_mutex_init(&rs.lock, NULL) != 0) {
        return -1;
    }
    if (pthread_cond_init(&rs.done_cv, NULL) != 0) {
        pthread_mutex_destroy(&rs.lock);
        return -1;
    }
    uint64_t seq = atomic_fetch_add_explicit(&r->next_run_seq, 1, memory_order_relaxed);
    gen_run_id(rs.run_id, sizeof(rs.run_id), seq);

    uint64_t t_run_start = now_us();

    // Inject pipeline-input as a content-hashed artifact. store_put takes
    // ownership of a heap buffer; copy the caller's bytes.
    hash_t input_hash = hash_bytes(input_bytes, input_len);
    uint8_t* input_dup = NULL;
    if (input_len > 0) {
        input_dup = malloc(input_len);
        if (!input_dup) {
            if (callbacks && callbacks->on_error) {
                callbacks->on_error(callbacks->ctx, rs.run_id, NULL, "OOM on pipeline-input copy");
            }
            pthread_cond_destroy(&rs.done_cv);
            pthread_mutex_destroy(&rs.lock);
            return -1;
        }
        memcpy(input_dup, input_bytes, input_len);
    }
    if (store_put(r->store, &input_hash, input_dup, input_len) != 0) {
        if (callbacks && callbacks->on_error) {
            callbacks->on_error(callbacks->ctx, rs.run_id, NULL, "failed to store pipeline-input");
        }
        pthread_cond_destroy(&rs.done_cv);
        pthread_mutex_destroy(&rs.lock);
        return -1;
    }
    pthread_mutex_lock(&rs.lock);
    int insert_rc = artifact_map_put_locked(&rs, PIPELINE_INPUT_ARTIFACT, &input_hash);
    pthread_mutex_unlock(&rs.lock);
    if (insert_rc != 0) {
        pthread_cond_destroy(&rs.done_cv);
        pthread_mutex_destroy(&rs.lock);
        return -1;
    }

    // DAG walk + topo sort.
    size_t* plan = NULL;
    size_t plan_len = 0;
    if (dag_plan(pipeline, subscribe, n_subscribe, &plan, &plan_len) != 0) {
        LOG_EVENT(SUB_RUNNER, LOG_ERROR, EV_RUNNER_RUN_FAILED, LF_RUN_ID(rs.run_id),
                  LF_STR("pipeline", pipeline_name(pipeline)),
                  LF_STR("reason", "dag walk / topo sort failed"));
        if (callbacks && callbacks->on_error) {
            callbacks->on_error(callbacks->ctx, rs.run_id, NULL, "dag walk / topo sort failed");
        }
        for (size_t i = 0; i < rs.n_artifacts; i++) {
            free(rs.artifacts[i].name);
        }
        free(rs.artifacts);
        pthread_cond_destroy(&rs.done_cv);
        pthread_mutex_destroy(&rs.lock);
        return -1;
    }
    rs.plan = plan;
    rs.plan_len = plan_len;

    if (build_dependency_graph(&rs) != 0) {
        LOG_EVENT(SUB_RUNNER, LOG_ERROR, EV_RUNNER_RUN_FAILED, LF_RUN_ID(rs.run_id),
                  LF_STR("reason", "OOM building dependency graph"));
        goto cleanup_fail;
    }

    LOG_EVENT(SUB_RUNNER, LOG_DEBUG, EV_RUNNER_DAG_PLAN, LF_RUN_ID(rs.run_id),
              LF_STR("pipeline", pipeline_name(pipeline)), LF_UINT("plan_len", (uint64_t)plan_len));

    // Register in the active-runs table BEFORE we start dispatching work so
    // runner_cancel() can find us for the entire body of the run.
    if (active_runs_register(r, &rs) != 0) {
        goto cleanup_fail;
    }

    LOG_EVENT(SUB_RUNNER, LOG_INFO, EV_RUNNER_RUN_STARTED, LF_RUN_ID(rs.run_id),
              LF_STR("pipeline", pipeline_name(pipeline)), LF_UINT("plan_len", (uint64_t)plan_len));

    if (callbacks && callbacks->on_run_started) {
        const char** names = plan_len ? safe_calloc_array(plan_len, sizeof(const char*)) : NULL;
        if (plan_len == 0 || names) {
            for (size_t i = 0; i < plan_len; i++) {
                names[i] = pipeline_step_def_at(pipeline, plan[i])->name;
            }
            callbacks->on_run_started(callbacks->ctx, rs.run_id, pipeline_name(pipeline), names,
                                      plan_len);
            free(names);
        }
    }

    // Submit all initially-ready steps (in_degree == 0).
    //
    // IMPORTANT — snapshot before submit. submit_step_task_locked drops
    // rs.lock around pool_submit; while it's dropped, a worker that's
    // already picked up an earlier task can complete and decrement the
    // in_degree of later plan entries. If we then re-check in_degree[i]
    // for those later entries, they look spuriously ready and we submit
    // them a second time (task #97: sum-two ran twice in
    // test_*_second_run, n_cache_hits=4). Collect the initial ready set
    // in a single locked pass FIRST, then iterate the snapshot — the
    // snapshot doesn't reflect peer decrements, so each plan entry is
    // submitted at most once via this path. step_task_fn's successor
    // submission uses the same snapshot pattern (ready_stack/heap).
    size_t* initial_ready =
        plan_len ? safe_calloc_array(plan_len, sizeof(size_t)) : NULL;
    size_t initial_n = 0;
    pthread_mutex_lock(&rs.lock);
    if (plan_len && !initial_ready) {
        rs.cancel = true;
        rs.run_rc = -1;
    } else {
        for (size_t i = 0; i < plan_len; i++) {
            if (rs.in_degree[i] == 0) {
                initial_ready[initial_n++] = i;
            }
        }
        for (size_t k = 0; k < initial_n; k++) {
            if (submit_step_task_locked(r, &rs, initial_ready[k]) != 0) {
                rs.cancel = true;
                rs.run_rc = -1;
                break;
            }
        }
    }
    // Wait for all submitted tasks to complete (n_pending drops to 0).
    while (rs.n_pending > 0) {
        pthread_cond_wait(&rs.done_cv, &rs.lock);
    }
    int final_rc = rs.run_rc;
    bool was_cancelled_external = rs.cancel_external;
    size_t executed = rs.n_executed;
    size_t cache_hits = rs.n_cache_hits;
    pthread_mutex_unlock(&rs.lock);
    free(initial_ready);

    // Unregister before the callbacks fire so a slow client can't make another
    // thread successfully runner_cancel() us after we've already completed.
    active_runs_unregister(r, &rs);

    uint64_t total_dt = now_us() - t_run_start;
    if (final_rc == 0) {
        LOG_EVENT(SUB_RUNNER, LOG_INFO, EV_RUNNER_RUN_COMPLETE, LF_RUN_ID(rs.run_id),
                  LF_STR("pipeline", pipeline_name(pipeline)), LF_UINT("duration_us", total_dt),
                  LF_UINT("steps_executed", executed), LF_UINT("cache_hits", cache_hits));
        if (callbacks && callbacks->on_run_complete) {
            callbacks->on_run_complete(callbacks->ctx, rs.run_id, total_dt, executed, cache_hits);
        }
    } else {
        LOG_EVENT(SUB_RUNNER, LOG_ERROR, EV_RUNNER_RUN_FAILED, LF_RUN_ID(rs.run_id),
                  LF_STR("pipeline", pipeline_name(pipeline)), LF_UINT("duration_us", total_dt),
                  LF_BOOL("external_cancel", was_cancelled_external));
        if (was_cancelled_external && callbacks && callbacks->on_error) {
            callbacks->on_error(callbacks->ctx, rs.run_id, NULL, "cancelled");
        }
    }

    int run_rc = final_rc;
    goto cleanup_ok;

cleanup_fail:
    run_rc = -1;
cleanup_ok:
    // Single teardown path for both success and post-DAG-build failure.
    // Invariant by the time we reach here:
    //   - rs.lock + rs.done_cv are initialised (allocated up-front).
    //   - rs.artifacts may hold n_artifacts entries; each `name` is an
    //     owned strdup. Hashes are stored by value, no per-entry free.
    //   - rs.plan, rs.in_degree, rs.successors[], rs.n_successors are
    //     either NULL (early-fail before dag_plan / build_dependency_graph)
    //     or fully populated. free(NULL) is safe; the per-row loop
    //     over successors is gated by the NULL check.
    //   - rs.successors[i] is a heap array of size_t per plan position,
    //     allocated by build_dependency_graph; one free per row.
    //   - n_pending is 0 by construction (cond_wait drained it).
    //   - Workers have all returned (no thread holds rs.lock).
    // Early-return failure paths above (input_dup OOM, store_put fail,
    // artifact_map_put fail, dag_plan fail) bypass this block but each
    // explicitly destroys cv+lock and frees what they allocated; rs.
    // artifacts is empty in those cases so no leak.
    for (size_t i = 0; i < rs.n_artifacts; i++) {
        free(rs.artifacts[i].name);
    }
    free(rs.artifacts);
    free(rs.plan);
    free(rs.in_degree);
    for (size_t i = 0; i < rs.plan_len; i++) {
        free(rs.successors ? rs.successors[i] : NULL);
    }
    free(rs.successors);
    free(rs.n_successors);
    pthread_cond_destroy(&rs.done_cv);
    pthread_mutex_destroy(&rs.lock);
    return run_rc;
}

// ----------------------------------------------------------------------------
// Out-of-band: compute a step's identity without executing it.
// ----------------------------------------------------------------------------

// Small open-addressed name → hash map, local to runner_compute_step_identity.
// We can't use the run_state_t artifact_map because we don't have a run here
// and this call happens outside any runner thread. Linear probing over a
// modest capacity (pipelines are small; ≤ 256 distinct artifact names).
struct identity_amap {
    char** names;
    hash_t* hashes;
    size_t n;
    size_t cap;
};

static int amap_put(struct identity_amap* m, const char* name, size_t name_len,
                    const hash_t* h) {
    if (m->n == m->cap) return -1;
    char* copy = malloc(name_len + 1);
    if (!copy) return -1;
    memcpy(copy, name, name_len);
    copy[name_len] = '\0';
    m->names[m->n] = copy;
    m->hashes[m->n] = *h;
    m->n++;
    return 0;
}

static bool amap_get(const struct identity_amap* m, const char* name, hash_t* out) {
    for (size_t i = 0; i < m->n; i++) {
        if (strcmp(m->names[i], name) == 0) { *out = m->hashes[i]; return true; }
    }
    return false;
}

static int amap_cb(void* vctx, const char* name, size_t name_len, const hash_t* h) {
    return amap_put((struct identity_amap*)vctx, name, name_len, h);
}

int runner_compute_step_identity(runner_t* r, const pipeline_t* pipeline,
                                 const void* input_bytes, size_t input_len,
                                 const char* step_name, hash_t* out_identity) {
    if (!r || !pipeline || !step_name || !out_identity) return -1;

    // Locate the target step.
    size_t n_steps = pipeline_step_count(pipeline);
    size_t target_idx = (size_t)-1;
    for (size_t i = 0; i < n_steps; i++) {
        const step_def_t* def = pipeline_step_def_at(pipeline, i);
        if (strcmp(def->name, step_name) == 0) { target_idx = i; break; }
    }
    if (target_idx == (size_t)-1) return -1;

    // Local artifact-name → content-hash map. Capped at 256 distinct names;
    // pipelines in flight today top out around 40.
    struct identity_amap amap = {
        .names = calloc(256, sizeof(char*)),
        .hashes = calloc(256, sizeof(hash_t)),
        .n = 0, .cap = 256,
    };
    bool* done = safe_calloc_array(n_steps, sizeof(bool));
    int result = -1;
    if (!amap.names || !amap.hashes || !done) goto cleanup;

    // Seed with pipeline-input.
    hash_t input_hash = hash_bytes(input_bytes, input_len);
    if (amap_put(&amap, PIPELINE_INPUT_ARTIFACT,
                 strlen(PIPELINE_INPUT_ARTIFACT), &input_hash) != 0) {
        goto cleanup;
    }

    // Iterate-until-stable: each round, advance any step whose reads are
    // all in the amap. On reaching the target, compute its identity and
    // return. Bounded to n_steps passes (each pass completes ≥1 step or
    // stalls).
    bool progress = true;
    while (progress) {
        progress = false;
        for (size_t si = 0; si < n_steps; si++) {
            if (done[si]) continue;
            const step_def_t* def = pipeline_step_def_at(pipeline, si);

            // Try to resolve all reads.
            hash_t* reads = def->n_reads ? safe_calloc_array(def->n_reads, sizeof(hash_t)) : NULL;
            if (def->n_reads && !reads) goto cleanup;
            bool ready = true;
            for (size_t ri = 0; ri < def->n_reads; ri++) {
                if (!amap_get(&amap, def->reads[ri].name, &reads[ri])) {
                    ready = false; break;
                }
            }
            if (!ready) { free(reads); continue; }

            const char* params = pipeline_step_params_at(pipeline, si);
            hash_t id = compute_step_identity_hash(def, params, reads, def->n_reads);
            free(reads);

            if (si == target_idx) {
                *out_identity = id;
                result = 0;
                goto cleanup;
            }

            // Not the target → look up manifest, populate amap with this
            // step's writes' content hashes so downstream steps can resolve.
            hash_t manifest_hash;
            if (intent_map_get(r->intent_map, &id, &manifest_hash) != 0) {
                // Upstream not in cache — target can't be cached either.
                goto cleanup;
            }
            store_handle_t sh;
            if (store_lookup(r->store, &manifest_hash, &sh) != 0) {
                goto cleanup;
            }
            int walk_rc = step_manifest_walk(sh.bytes, sh.len, amap_cb, &amap);
            store_release(r->store, &sh);
            if (walk_rc != 0) goto cleanup;

            done[si] = true;
            progress = true;
        }
    }

cleanup:
    if (amap.names) {
        for (size_t i = 0; i < amap.n; i++) free(amap.names[i]);
        free(amap.names);
    }
    free(amap.hashes);
    free(done);
    return result;
}

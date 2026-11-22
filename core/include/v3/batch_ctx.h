// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// batch_ctx_t — opaque per-batch-run context for the CLI's `batch`
// subcommand.
//
// Threading
// ─────────
// Runner callbacks installed via batch_ctx_install_callbacks fire from
// runner-pool worker threads (CONCURRENCY.md §11) and may overlap.  The
// internal mutex serialises every mutation; callers see a thread-safe
// API surface.  No caller takes the internal mutex.
//
// Lock ordering: leaf.  Never acquired while holding any other lock.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>   // ACSL valid_read_string

#include "v3/hash.h"
#include "v3/runner.h"

struct store;

typedef struct batch_ctx batch_ctx_t;

// One produced artifact entry (read via batch_ctx_for_each_produced).
typedef struct batch_produced {
    const char* name;       // borrowed from ctx, valid until batch_ctx_free
    hash_t      hash;
    size_t      size;
    const char* path;       // borrowed
} batch_produced_t;

// Subscribe-set membership predicate (caller provides the closure).
typedef bool (*batch_subscribe_fn)(void* ctx, const char* artifact_name);

// Allocate.  `subscribe_fn` is called from worker threads to decide
// whether to dump an artifact; caller's `subscribe_ctx` should be
// thread-safe-readable for the duration of the run (typically an
// immutable produce-list parsed at startup).  `output_dir` is borrowed
// — must outlive the batch_ctx.
/*@ requires store == \null || \valid(store);
    requires output_dir == \null || valid_read_string(output_dir);
    assigns \nothing;
    ensures \result == \null || \valid(\result);
*/
batch_ctx_t* batch_ctx_new(struct store* store, const char* output_dir,
                           batch_subscribe_fn subscribe_fn, void* subscribe_ctx);

/*@ requires bc == \null || \valid(bc);
    assigns *bc;
*/
void batch_ctx_free(batch_ctx_t* bc);

/*@ requires bc == \null || \valid(bc);
    requires \valid(cb);
    assigns *cb;
*/
void batch_ctx_install_callbacks(batch_ctx_t* bc, runner_callbacks_t* cb);

/*@ requires bc == \null || \valid(bc);
    assigns \nothing;
*/
int          batch_ctx_error_count(batch_ctx_t* bc);

/*@ requires bc == \null || \valid(bc);
    assigns \nothing;
*/
size_t       batch_ctx_n_produced(batch_ctx_t* bc);

/*@ requires bc == \null || \valid(bc);
    requires \valid(out + (0 .. out_size - 1));
    assigns *(out + (0 .. out_size - 1));
*/
void         batch_ctx_get_run_id(batch_ctx_t* bc, char* out, size_t out_size);

typedef void (*batch_produced_visit_fn)(void* cb_ctx, const batch_produced_t* e);

/*@ requires bc == \null || \valid(bc);
    assigns *bc;
*/
void batch_ctx_for_each_produced(batch_ctx_t* bc, batch_produced_visit_fn fn,
                                 void* cb_ctx);

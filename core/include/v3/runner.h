// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#ifndef V3_RUNNER_H
#define V3_RUNNER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>   // ACSL valid_read_string

#include "v3/error.h"
#include "v3/hash.h"
#include "v3/intent_map.h"
#include "v3/pipeline.h"
#include "v3/store.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct runner_callbacks {
    void* ctx;
    void (*on_run_started)(void* ctx, const char* run_id, const char* pipeline_name,
                           const char* const* step_plan, size_t n_plan);
    void (*on_step_started)(void* ctx, const char* run_id, const char* step_name);
    void (*on_step_complete)(void* ctx, const char* run_id, const char* step_name, bool cache_hit,
                             uint64_t duration_us);
    void (*on_artifact_ready)(void* ctx, const char* run_id, const char* artifact_name,
                              const hash_t* hash);
    // Fires once per `step_write_output_stream` call, BEFORE the chunk is
    // committed to the in-process accumulator. `seq` starts at 0 and
    // increments per chunk for the same artifact. The final chunk has
    // `final = true`; `hash` is non-NULL only on that final chunk and
    // points to the artifact's content hash. The `bytes`/`len` view is
    // valid only for the duration of the callback.
    void (*on_stream_chunk)(void* ctx, const char* run_id, const char* artifact_name,
                            uint32_t seq, bool final, const void* bytes, size_t len,
                            const hash_t* hash);
    void (*on_run_complete)(void* ctx, const char* run_id, uint64_t duration_us,
                            size_t steps_executed, size_t cache_hits);
    // Non-terminal notification: a step failed (fires once per failing
    // step, as soon as it fails) or a run-level problem occurred. `msg` is
    // "[CODE] message". Kept for wire compatibility (ERROR frame).
    void (*on_error)(void* ctx, const char* run_id, const char* step_name, const char* msg);
    // Terminal: the run did not complete. Fires exactly once per failed
    // run, LAST, instead of on_run_complete. `f` describes the FIRST
    // failure of the run (peers cancelled after it are not reported) and
    // is valid only for the duration of the callback. See docs/ERRORS.md.
    void (*on_run_failed)(void* ctx, const char* run_id, const v3_failure_t* f,
                          uint64_t duration_us);
} runner_callbacks_t;

typedef struct runner_config {
    store_t* store;
    intent_map_t* intent_map; // required; HASH-MODEL.md §6
    // Number of compute threads in the runner's pool.
    // 0 = default (4). 1 = sequential execution (still uses the pool).
    int thread_count;
} runner_config_t;

typedef struct runner runner_t;

/*@ requires cfg == \null || \valid_read(cfg);
    assigns \nothing;
    ensures \result == \null || \valid(\result);
*/
runner_t* runner_new(const runner_config_t* cfg);

/*@ requires r == \null || \valid(r);
    assigns *r;
*/
void runner_free(runner_t* r);

// Execute `pipeline` with the given input payload and subscriptions.
// The input bytes become the "pipeline-input" artifact (content-hashed).
// The runner walks the DAG backward from `subscribe` and executes only the
// required steps. Callbacks fire as the run progresses.
// Returns 0 on success, -1 on any failure (cycle, missing producer, step error,
// or externally triggered cancel via runner_cancel).
/*@ requires r == \null || \valid(r);
    requires pipeline == \null || \valid_read(pipeline);
    requires input_len == 0 || \valid_read(((char*)input_bytes) + (0 .. input_len - 1));
    requires n_subscribe == 0 || \valid_read(subscribe + (0 .. n_subscribe - 1));
    requires callbacks == \null || \valid_read(callbacks);
    assigns *r;
    ensures \result == 0 || \result == -1;
*/
int runner_run(runner_t* r, const pipeline_t* pipeline, const void* input_bytes, size_t input_len,
               const char* const* subscribe, size_t n_subscribe,
               const runner_callbacks_t* callbacks);

// Externally cancel an in-flight run. Idempotent. Thread-safe. Returns 0 if
// the run was found and flagged, -1 if no active run matches the given run_id.
// The matching runner_run() will unblock and return -1; its on_error callback
// fires once with step=NULL and msg="cancelled".
/*@ requires r == \null || \valid(r);
    requires run_id == \null || valid_read_string(run_id);
    assigns *r;
    ensures \result == 0 || \result == -1;
*/
int runner_cancel(runner_t* r, const char* run_id);

// Compute the `step_identity` hash that `runner_run` would compute for
// `step_name` under (pipeline, input_bytes). Walks the DAG in topo order
// from pipeline-input, resolving each upstream step's identity → manifest
// through the intent map to recover the read-artifact content hashes. Does
// not execute any step body. Intended for out-of-band identity queries —
// e.g. to invalidate a cached step's intent-map entry without re-running.
//
// Returns 0 on success (fills *out_identity). Returns -1 if:
//   - step_name isn't in the pipeline
//   - any upstream step has no intent-map entry yet (meaning the target
//     was never cached under this input either, so there is nothing to
//     invalidate downstream)
//   - manifest parsing fails for any upstream step
/*@ requires r == \null || \valid(r);
    requires pipeline == \null || \valid_read(pipeline);
    requires input_len == 0 || \valid_read(((char*)input_bytes) + (0 .. input_len - 1));
    requires step_name == \null || valid_read_string(step_name);
    requires out_identity == \null || \valid(out_identity);
    assigns *out_identity;
    ensures \result == 0 || \result == -1;
*/
int runner_compute_step_identity(runner_t* r, const pipeline_t* pipeline,
                                 const void* input_bytes, size_t input_len,
                                 const char* step_name, hash_t* out_identity);

// Borrow the runner's intent map. Used by callers that need to issue
// get/invalidate/put calls at step-identity granularity (e.g. the server's
// INVALIDATE handler). The returned pointer is owned by the runner and
// must not be closed by the caller.
/*@ requires r == \null || \valid(r);
    assigns \nothing;
    ensures \result == \null || \valid(\result);
*/
intent_map_t* runner_intent_map(runner_t* r);

#ifdef __cplusplus
}
#endif
#endif

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#ifndef V3_STEP_H
#define V3_STEP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>   // brings in Frama-C's ACSL logic for valid_read_string

#include "v3/error.h"
#include "v3/hash.h"
#include "v3/intent_map.h"
#include "v3/store.h"

#ifdef __cplusplus
extern "C" {
#endif

// Reserved artifact name: the RUN payload becomes this artifact at the head of
// every run's DAG (§9.2 of UNIFIED-ARCHITECTURE.md). Steps read it like any
// other artifact. Users must not use this name as a step output.
#define PIPELINE_INPUT_ARTIFACT "pipeline-input"

typedef struct step_context step_context_t;
typedef int (*step_exec_fn)(step_context_t* ctx);

typedef enum step_visibility {
    STEP_VIS_DEBUG = 0,
    STEP_VIS_PUBLIC = 1,
} step_visibility_t;

typedef enum step_compute_location {
    STEP_CL_CPU = 0,
    STEP_CL_GPU = 1,
} step_compute_location_t;

typedef struct step_write {
    const char* name;
    uint8_t kind; // artifact_kind_t from v3/artifact.h
    step_visibility_t visibility;
    // Set to `true` if this write produces an array artifact (kind must be
    // AK_ARRAY_INLINE or AK_ARRAY_BY_HASH). See §5.3 of UNIFIED-ARCHITECTURE.md
    // and PLAN-VIEWER-PORT.md §6.
    bool array;
    // Escape hatch for non-deterministic steps (HASH-MODEL.md §9). Default 0
    // (treated as `true` — identity-based skip enabled). Set to `true` to opt
    // out: runner bypasses the intent map, step runs every time. Rarely used —
    // the vast majority of steps are deterministic functions of their inputs.
    bool nondeterministic;
    // Set to `true` if this write is produced incrementally via
    // `step_write_output_stream` (PLAN-VIEWER-PORT §7). Required to enable
    // chunked emission. Mixing streaming and non-streaming writes for the
    // same artifact in one exec is rejected.
    bool streaming;
} step_write_t;

typedef struct step_read {
    const char* name;
    // Set to `true` if this read consumes an array artifact. The step helpers
    // `step_array_len`, `step_array_read_bytes`, and `step_array_open` apply.
    // Validated at pipeline load against the producer's `step_write_t.array`.
    bool array;
} step_read_t;

typedef struct step_def {
    const char* name;
    uint32_t version;

    const step_read_t* reads;
    size_t n_reads;

    const step_write_t* writes;
    size_t n_writes;

    step_compute_location_t compute_location;
    bool streaming;

    step_exec_fn exec;
} step_def_t;

// Register a step. The pointer is retained; lifetime must outlive the registry
// (static storage is typical). Returns 0 on success, -1 on error (duplicate
// name, full registry, invalid def).
int step_register(const step_def_t* def);

// Lookup by name. Returns NULL if not registered.
//
// On non-NULL return, the result points to a valid step_def_t — the
// registry only ever stores pointers to caller-owned (typically static)
// step_def_t structures, never NULL (step_register rejects NULL). Eva
// can't derive that from the body across the global g_defs[] array, so
// we expose it explicitly: without this ensures, pipeline_validate's
// `for (w < di->n_writes)` produced a sure alarm because Eva couldn't
// prove `\valid_read(&di->n_writes)` even after a runtime `if (!di)
// return -1` guard — the guard only filters NULL, not "non-NULL but
// pointing to unmapped memory".
/*@ requires name == \null || valid_read_string(name);
    assigns \nothing;
    ensures \result == \null || \valid_read(\result);
*/
const step_def_t* step_lookup(const char* name);

// Clear the registry. For tests — don't use in prod.
void step_registry_reset(void);

size_t step_registry_size(void);

// ---------------------------------------------------------------------------
// Step execution context. Implemented by the runner.
// ---------------------------------------------------------------------------

// Read an input artifact by name. Returns bytes + length (zero-copy view into
// the store's mmap'd file), or NULL if not available. The pointer is valid
// until the step's exec fn returns.
/*@ requires \valid(ctx);
    requires valid_read_string(name);
    requires out_len == \null || \valid(out_len);
    assigns *out_len \from indirect:ctx, indirect:name;
    ensures \result == \null
         || (out_len != \null && \valid_read(((char*)\result) + (0 .. *out_len - 1)));
    ensures out_len != \null
         ==> (\result == \null ==> *out_len == 0);
*/
const void* step_read(step_context_t* ctx, const char* name, size_t* out_len);

// Write an output artifact. Runner content-hashes the bytes (HASH-MODEL.md §5)
// and transfers ownership of an internal copy to the store. Caller retains
// ownership of the original `bytes` buffer. Returns 0 on success.
/*@ requires \valid(ctx);
    requires valid_read_string(name);
    requires len == 0 || \valid_read(((char*)bytes) + (0 .. len - 1));
    assigns indirect:*ctx, indirect:*((char*)bytes + (0 .. len - 1));
    ensures \result == 0 || \result == -1;
*/
int step_write_output(step_context_t* ctx, const char* name, const void* bytes, size_t len);

// Same as step_write_output, but TRANSFERS OWNERSHIP of `bytes` to the
// store. `bytes` must be a heap buffer allocated with malloc/calloc.
// On success the caller MUST NOT touch or free `bytes` afterwards (the
// store frees it on demote/evict). On failure the caller must free `bytes`.
//
// Use this for large writes (point clouds, rasters, manifests) where the
// step has already heap-allocated the buffer. Saves one full-buffer copy
// vs `step_write_output`.
/*@ requires \valid(ctx);
    requires valid_read_string(name);
    requires len == 0 || \valid(bytes + (0 .. len - 1));
    // Ownership transfer on success: the bytes pointer is consumed,
    // becomes invalid for the caller. On failure caller retains it.
    assigns indirect:*ctx, *(bytes + (0 .. len - 1));
    ensures \result == 0 || \result == -1;
*/
int step_write_output_owned(step_context_t* ctx, const char* name, uint8_t* bytes, size_t len);

// Append a chunk to a streaming output (PLAN-VIEWER-PORT §7). Multiple calls
// may be made for the same `name` with `final=false`; the run completes
// when one call has `final=true`. The runner accumulates the chunks
// in-process, content-hashes the full buffer at finalisation, and commits
// it to the store atomically. Downstream steps cannot read the artifact
// until `final=true`.
//
// Each call also fires the runner's on_stream_chunk callback (if set) BEFORE
// the bytes are committed, so the WS server can forward the chunk to
// connected clients in real time.
//
// The artifact's declared write must have `streaming = true`. Mixing
// `step_write_output*` and `step_write_output_stream` for the same artifact
// in one exec is an error.
/*@ requires \valid(ctx);
    requires valid_read_string(name);
    requires len == 0 || \valid_read(((char*)bytes) + (0 .. len - 1));
    assigns indirect:*ctx;
    ensures \result == 0 || \result == -1;
*/
int step_write_output_stream(step_context_t* ctx, const char* name, const void* bytes, size_t len,
                             bool final);

// Report WHY the step is failing (docs/ERRORS.md). Always returns -1 so a
// step can write `return step_fail(ctx, V3_ERR_NO_DATA, "...", ...);`.
// The message is free text shown to end users and developers — say what
// was expected, what was found, and the key input (coordinates, URL host,
// counts). Use step_fail_detail to attach a JSON object with structured
// evidence. An HTTP failure inside fetch_get/fetch_and_cache is recorded
// automatically and attached as detail.upstream; the step does not need to
// forward it.
int step_fail(step_context_t* ctx, v3_error_code_t code, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));
int step_fail_detail(step_context_t* ctx, v3_error_code_t code, const char* detail_json,
                     const char* fmt, ...) __attribute__((format(printf, 4, 5)));

// Get the step's params JSON string. May be NULL if not set.
/*@ requires \valid(ctx);
    assigns \nothing;
    ensures \result == \null || valid_read_string(\result);
*/
const char* step_get_params(step_context_t* ctx);

// ---------------------------------------------------------------------------
// Array artifact helpers (§5.3 of UNIFIED-ARCHITECTURE.md). All require that
// the step declared the corresponding read/write with `array: true`.
// ---------------------------------------------------------------------------

// Number of items in an array artifact. Works for both ARRAY_INLINE and
// ARRAY_BY_HASH reads.
/*@ requires \valid(ctx);
    requires valid_read_string(name);
    assigns \nothing;
*/
size_t step_array_len(step_context_t* ctx, const char* name);

// Read the i-th element of an ARRAY_INLINE artifact. Returns 0 on success.
// Zero-copy: `*out_bytes` points into the step's mmap'd artifact and is
// valid until step exec returns.
/*@ requires \valid(ctx);
    requires valid_read_string(name);
    requires \valid(out_bytes);
    requires \valid(out_len);
    assigns *out_bytes, *out_len \from indirect:ctx, indirect:name, indirect:i;
    ensures \result == 0 || \result == -1;
    ensures \result == 0 ==>
        (*out_bytes == \null || \valid_read(*out_bytes + (0 .. *out_len - 1)));
*/
int step_array_read_bytes(step_context_t* ctx, const char* name, size_t i,
                          const uint8_t** out_bytes, size_t* out_len);

// Open the i-th element of an ARRAY_BY_HASH artifact. The element lives as
// its own content-addressed store entry. Caller must release the handle
// with store_release after use.
/*@ requires \valid(ctx);
    requires valid_read_string(name);
    requires \valid(out_handle);
    assigns *out_handle \from indirect:ctx, indirect:name, indirect:i;
    ensures \result == 0 || \result == -1;
*/
int step_array_open(step_context_t* ctx, const char* name, size_t i,
                    store_handle_t* out_handle);

// Write an ARRAY_INLINE artifact. Items are packed into `item_bytes`
// back-to-back. If `stride` is non-zero, items are fixed-size; the
// CALLER is responsible for picking a stride that's a multiple of any
// alignment its records require (e.g. 8 for any record holding a
// `double`). If `stride` is 0, the caller passes a blob ALREADY framed
// for the wire — `{u32 record_length, u32 zero_pad, payload,
// trailing_pad_to_8_bytes} × n`. The runtime validates the stream and
// emits it verbatim after prepending the `{u32 n, u32 stride}` artifact
// header. Build the blob with the blob_t/blob_push helpers in
// <v3/blob.h> — those produce the right framing automatically.
/*@ requires \valid(ctx);
    requires valid_read_string(name);
    requires total_len == 0 || \valid_read(item_bytes + (0 .. total_len - 1));
    assigns indirect:*ctx;
    ensures \result == 0 || \result == -1;
*/
int step_write_array_inline(step_context_t* ctx, const char* name,
                            const uint8_t* item_bytes, size_t total_len,
                            size_t n, size_t stride);

// Write an ARRAY_BY_HASH artifact. The caller is responsible for having
// already stored each element under its content hash (e.g. via store_put).
// The manifest artifact is content-hashed by the runner like any other write.
/*@ requires \valid(ctx);
    requires valid_read_string(name);
    requires n == 0 || \valid_read(element_hashes + (0 .. n - 1));
    assigns indirect:*ctx;
    ensures \result == 0 || \result == -1;
*/
int step_write_array_by_hash(step_context_t* ctx, const char* name,
                             const hash_t* element_hashes, size_t n);

// Borrow a pointer to the store for direct puts/reads. The main use case
// is fan-out steps that materialize individual elements into the store
// before writing an ARRAY_BY_HASH manifest that references them. Caller
// must not free or close the store. Stays valid for the step's exec duration.
/*@ requires \valid(ctx);
    assigns \nothing;
    ensures \result == \null || \valid(\result);
*/
store_t* step_runner_store(step_context_t* ctx);

// Borrow the runner's intent map. Used by steps that invoke `cached_compute`
// at sub-step scope (e.g. fetch_and_cache). Caller must not free or close.
/*@ requires \valid(ctx);
    assigns \nothing;
    ensures \result == \null || \valid(\result);
*/
intent_map_t* step_runner_intent_map(step_context_t* ctx);

#ifdef __cplusplus
}
#endif
#endif

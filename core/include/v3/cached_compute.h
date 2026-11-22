// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#ifndef V3_CACHED_COMPUTE_H
#define V3_CACHED_COMPUTE_H

#include <stddef.h>
#include <stdint.h>

#include "v3/hash.h"
#include "v3/intent_map.h"
#include "v3/store.h"

#ifdef __cplusplus
extern "C" {
#endif

// Content-addressed cache-or-compute primitive. See HASH-MODEL.md §3.
//
// On call:
//   1. intent_map_get(identity) → if hit, return stored content_hash.
//   2. Miss: invoke compute_fn to produce fresh bytes. The callee ALLOCATES
//      the buffer (malloc) and hands back ownership via out_bytes/out_len.
//   3. Hash the bytes, store_put them (ownership transfers to the store),
//      record identity → content_hash in the intent map.
//
// compute_fn returns 0 on success, non-zero on error. On error, cached_compute
// returns -1 and records no intent-map entry (retries are idempotent).
typedef int (*compute_fn_t)(void* ctx, uint8_t** out_bytes, size_t* out_len);

// Explicit store + intent_map: the runtime composes them, but cached_compute
// itself is orthogonal (used at step-scope by the runner, at sub-scope by
// fetcher steps, etc.). No hidden globals.
/*@ requires im == \null || \valid(im);
    requires store == \null || \valid(store);
    requires identity == \null || \valid_read(identity);
    requires out_content_hash == \null || \valid(out_content_hash);
    assigns *im, *store, *out_content_hash;
    ensures \result == 0 || \result == -1;
*/
int cached_compute(intent_map_t* im, store_t* store, const hash_t* identity,
                   compute_fn_t compute_fn, void* ctx, hash_t* out_content_hash);

#ifdef __cplusplus
}
#endif

#endif

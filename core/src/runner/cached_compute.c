// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#define _POSIX_C_SOURCE 200809L

#include "v3/cached_compute.h"

#include <stdlib.h>

int cached_compute(intent_map_t* im, store_t* store, const hash_t* identity,
                   compute_fn_t compute_fn, void* ctx, hash_t* out_content_hash) {
    if (!im || !store || !identity || !compute_fn || !out_content_hash) {
        return -1;
    }

    // Intent-map hit: the identity has been computed before. Validation that
    // the referenced content is still in the store is the caller's job
    // (see runner-level validate-on-use, HASH-MODEL.md §11.2).
    hash_t hit;
    if (intent_map_get(im, identity, &hit) == 0) {
        *out_content_hash = hit;
        return 0;
    }

    // Miss: run compute_fn. Callee allocates on success; caller frees on error.
    uint8_t* bytes = NULL;
    size_t len = 0;
    if (compute_fn(ctx, &bytes, &len) != 0) {
        // Don't record an intent-map entry — retries with the same identity
        // should re-run compute_fn.
        free(bytes);
        return -1;
    }
    // Frama-C audit finding CC-1: callee contract says success → bytes
    // is a heap buffer owned by us. Defend against a misbehaving callee
    // that returns 0 with bytes==NULL while claiming len>0 — that would
    // make the next line's XXH3_128bits(NULL, len) UB per xxhash docs.
    if (bytes == NULL && len > 0) {
        return -1;
    }

    hash_t content_hash = hash_bytes(bytes, len);
    // store_put takes ownership of `bytes` on success. On failure the caller
    // must free. The key is the content hash; same bytes from two different
    // chains converge automatically.
    int rc = store_put(store, &content_hash, bytes, len);
    if (rc != 0) {
        free(bytes);
        return -1;
    }
    // `bytes` is now owned by the store; the caller must not free it.
    // (This contract is enforced by the Wave B1 store_put refactor below;
    //  in the current transitional state store_put's signature matches
    //  the target contract.)

    if (intent_map_put(im, identity, &content_hash) != 0) {
        // Store has the content; the intent entry failed to record. Surface
        // as error so caller can retry (next attempt will re-fetch intent map
        // miss, re-run compute_fn, hit idempotent store_put, retry intent_map_put).
        return -1;
    }

    *out_content_hash = content_hash;
    return 0;
}

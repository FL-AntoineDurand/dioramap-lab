// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// cube-memoized
// Reads:  range-array (ARRAY_INLINE, i32 fixed stride)
// Writes: cube-array (ARRAY_BY_HASH; each element is its own store entry
//                     holding a single i64 cube).
//
// Same shape as fan-process, but demonstrates sub-scope `cached_compute`:
// each i³ computation is keyed on a namespaced identity `("cube", i32 i)`.
// Running the step against two different range-arrays that happen to share
// values produces zero duplicate cube-compute work across runs — each (i, i³)
// pair is recorded in the intent map once and reused.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "v3/artifact.h"
#include "v3/cached_compute.h"
#include "v3/hash.h"
#include "v3/intent_map.h"
#include "v3/store.h"

#include "demo.h"

typedef struct cube_ctx {
    int32_t value;
} cube_ctx_t;

// compute_fn for one cube. Allocates an 8-byte buffer, fills it with i³,
// hands ownership to cached_compute → store_put.
static int cube_compute_fn(void* vctx, uint8_t** out_bytes, size_t* out_len) {
    cube_ctx_t* cc = vctx;
    int64_t cubed = (int64_t)cc->value * (int64_t)cc->value * (int64_t)cc->value;
    uint8_t* buf = malloc(sizeof(int64_t));
    if (!buf) {
        return -1;
    }
    memcpy(buf, &cubed, sizeof(int64_t));
    *out_bytes = buf;
    *out_len = sizeof(int64_t);
    return 0;
}

static int exec_fn(step_context_t* ctx) {
    size_t n = step_array_len(ctx, "range-array");
    if (n == 0) {
        return step_write_array_by_hash(ctx, "cube-array", NULL, 0);
    }

    hash_t* element_hashes = calloc(n, sizeof(hash_t));
    if (!element_hashes) {
        return -1;
    }
    store_t* store = step_runner_store(ctx);
    intent_map_t* im = step_runner_intent_map(ctx);

    int rc = 0;
    for (size_t i = 0; i < n; i++) {
        const uint8_t* item_bytes = NULL;
        size_t item_len = 0;
        if (step_array_read_bytes(ctx, "range-array", i, &item_bytes, &item_len) != 0 ||
            item_len != sizeof(int32_t)) {
            rc = -1;
            break;
        }
        int32_t value;
        memcpy(&value, item_bytes, sizeof(value));
        // Per-value sub-scope identity. Two ranges that both contain `v` will
        // converge on the same intent entry → same content hash → no re-compute.
        hash_t identity = hash_namespaced("cube", &value, sizeof(value));
        cube_ctx_t cc = {.value = value};
        if (cached_compute(im, store, &identity, cube_compute_fn, &cc, &element_hashes[i]) != 0) {
            rc = -1;
            break;
        }
    }

    if (rc == 0) {
        rc = step_write_array_by_hash(ctx, "cube-array", element_hashes, n);
    }
    free(element_hashes);
    return rc;
}

static const step_read_t reads_arr[] = {
    {.name = "range-array", .array = true},
};
static const step_write_t writes_arr[] = {
    {.name = "cube-array",
     .kind = AK_ARRAY_BY_HASH,
     .visibility = STEP_VIS_DEBUG,
     .array = true},
};

const step_def_t demo_cube_memoized_def = {
    .name = "cube-memoized",
    .version = 1,
    .reads = reads_arr,
    .n_reads = 1,
    .writes = writes_arr,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};

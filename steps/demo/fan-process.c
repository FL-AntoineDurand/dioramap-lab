// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// fan-process
// Reads:  range-array (ARRAY_INLINE, i32 fixed stride)
// Writes: square-array (ARRAY_BY_HASH; each element is its own store entry
//                       holding a single i64 square).
//
// Exercises: step_array_len, step_array_read_bytes, store_put,
//            step_write_array_by_hash.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "v3/artifact.h"
#include "v3/store.h"

#include "demo.h"

static int exec_fn(step_context_t* ctx) {
    size_t n = step_array_len(ctx, "range-array");
    if (n == 0) {
        // Empty input → empty output manifest.
        return step_write_array_by_hash(ctx, "square-array", NULL, 0);
    }

    hash_t* element_hashes = calloc(n, sizeof(hash_t));
    if (!element_hashes)
        return -1;

    store_t* store = step_runner_store(ctx);
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
        int64_t sq = (int64_t)value * (int64_t)value;
        // Store each square as its own content-addressed artifact. Ownership-
        // transfer put: malloc, fill, hand to the store.
        uint8_t* buf = malloc(sizeof(sq));
        if (!buf) {
            rc = -1;
            break;
        }
        memcpy(buf, &sq, sizeof(sq));
        element_hashes[i] = hash_bytes(buf, sizeof(sq));
        if (store_put(store, &element_hashes[i], buf, sizeof(sq)) != 0) {
            rc = -1;
            break;
        }
    }

    if (rc == 0) {
        rc = step_write_array_by_hash(ctx, "square-array", element_hashes, n);
    }
    free(element_hashes);
    return rc;
}

static const step_read_t reads_arr[] = {
    {.name = "range-array", .array = true},
};
static const step_write_t writes_arr[] = {
    {.name = "square-array",
     .kind = AK_ARRAY_BY_HASH,
     .visibility = STEP_VIS_DEBUG,
     .array = true},
};

const step_def_t demo_fan_process_def = {
    .name = "fan-process",
    .version = 1,
    .reads = reads_arr,
    .n_reads = 1,
    .writes = writes_arr,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};

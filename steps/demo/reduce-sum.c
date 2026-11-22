// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// reduce-sum
// Reads:  square-array (ARRAY_BY_HASH of i64 elements)
// Writes: square-sum (i64, little-endian) — sum of all elements.
// Exercises step_array_len + step_array_open + store_release.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "v3/artifact.h"
#include "v3/store.h"

#include "demo.h"

static int exec_fn(step_context_t* ctx) {
    size_t n = step_array_len(ctx, "square-array");
    int64_t sum = 0;
    store_t* store = step_runner_store(ctx);
    for (size_t i = 0; i < n; i++) {
        store_handle_t h;
        if (step_array_open(ctx, "square-array", i, &h) != 0)
            return -1;
        if (h.len != sizeof(int64_t)) {
            store_release(store, &h);
            return -1;
        }
        int64_t value;
        memcpy(&value, h.bytes, sizeof(value));
        store_release(store, &h);
        sum += value;
    }
    return step_write_output(ctx, "square-sum", &sum, sizeof(sum));
}

static const step_read_t reads_arr[] = {
    {.name = "square-array", .array = true},
};
static const step_write_t writes_arr[] = {
    {.name = "square-sum",
     .kind = AK_OPAQUE_JSON,
     .visibility = STEP_VIS_DEBUG},
};

const step_def_t demo_reduce_sum_def = {
    .name = "reduce-sum",
    .version = 1,
    .reads = reads_arr,
    .n_reads = 1,
    .writes = writes_arr,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// split-range
// Reads:  pipeline-input (JSON object, must have integer "count" ≥ 0)
// Writes: range-array (ARRAY_INLINE of i32, fixed stride = 4)
//           content = [0, 1, 2, ..., count-1]
// Exercises step_write_array_inline.

#include <jansson.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "v3/artifact.h"

#include "demo.h"

static int exec_fn(step_context_t* ctx) {
    size_t in_len = 0;
    const char* in_bytes = (const char*)step_read(ctx, PIPELINE_INPUT_ARTIFACT, &in_len);
    if (!in_bytes || in_len == 0)
        return -1;
    json_error_t je;
    json_t* input = json_loadb(in_bytes, in_len, 0, &je);
    if (!input)
        return -1;
    json_t* j_count = json_object_get(input, "count");
    if (!json_is_integer(j_count)) {
        json_decref(input);
        return -1;
    }
    int64_t raw = json_integer_value(j_count);
    json_decref(input);
    if (raw < 0 || raw > INT32_MAX)
        return -1;
    size_t n = (size_t)raw;

    int32_t* items = NULL;
    if (n > 0) {
        items = calloc(n, sizeof(int32_t));
        if (!items)
            return -1;
        for (size_t i = 0; i < n; i++) {
            items[i] = (int32_t)i;
        }
    }
    int rc = step_write_array_inline(ctx, "range-array", (const uint8_t*)items,
                                     n * sizeof(int32_t), n, sizeof(int32_t));
    free(items);
    return rc;
}

static const step_read_t reads_arr[] = {{.name = PIPELINE_INPUT_ARTIFACT}};
static const step_write_t writes_arr[] = {
    {.name = "range-array",
     .kind = AK_ARRAY_INLINE,
     .visibility = STEP_VIS_DEBUG,
     .array = true},
};

const step_def_t demo_split_range_def = {
    .name = "split-range",
    .version = 2, // 5.1d: ARRAY_INLINE 8-byte aligned format
    .reads = reads_arr,
    .n_reads = 1,
    .writes = writes_arr,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};

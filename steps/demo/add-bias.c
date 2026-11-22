// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// add-bias
// Reads:  pipeline-input (JSON object, must have integer "base")
// Params: {"bias": int} (default 0)
// Writes: biased (int32 LE) = base + bias

#include <jansson.h>
#include <stdint.h>
#include <string.h>

#include "v3/json_int.h"
#include "demo.h"

static int exec_fn(step_context_t* ctx) {
    size_t in_len = 0;
    const char* in_bytes = (const char*)step_read(ctx, PIPELINE_INPUT_ARTIFACT, &in_len);
    if (!in_bytes || in_len == 0) {
        return -1;
    }
    json_error_t je;
    json_t* input = json_loadb(in_bytes, in_len, 0, &je);
    if (!input) {
        return -1;
    }
    // Frama-C audit pass 4: strict range-check on JSON-derived int32s.
    // Demo step: caller supplies arbitrary ints to demonstrate the
    // accumulator pattern. Range chosen so `base + bias` can't overflow
    // int32 (each side bounded to half the int32 range).
    json_t* j_base = json_object_get(input, "base");
    int32_t base = 0;
    if (json_int32_or_default(j_base, -INT32_MAX/2, INT32_MAX/2, "base", &base) != 0
        || !json_is_integer(j_base)) {
        json_decref(input);
        return -1;
    }
    json_decref(input);

    int32_t bias = 0;
    const char* params_str = step_get_params(ctx);
    if (params_str) {
        json_t* params = json_loads(params_str, 0, &je);
        if (params) {
            json_t* j_bias = json_object_get(params, "bias");
            if (json_int32_or_default(j_bias, -INT32_MAX/2, INT32_MAX/2, "bias", &bias) != 0) {
                json_decref(params);
                return -1;
            }
            json_decref(params);
        }
    }

    int32_t biased = base + bias;
    return step_write_output(ctx, "biased", &biased, sizeof(biased));
}

static const step_read_t reads_arr[] = {{.name = PIPELINE_INPUT_ARTIFACT}};
static const step_write_t writes_arr[] = {
    {.name = "biased",
     .kind = AK_OPAQUE_JSON,
     .visibility = STEP_VIS_DEBUG},
};

const step_def_t demo_add_bias_def = {
    .name = "add-bias",
    .version = 1,
    .reads = reads_arr,
    .n_reads = 1,
    .writes = writes_arr,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};

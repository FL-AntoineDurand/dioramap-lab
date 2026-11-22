// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// scale-by-factor
// Reads:  pipeline-input (JSON object, must have integer "base")
// Params: {"factor": int} (default 1)
// Writes: scaled (int32 LE) = base * factor

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
    // Frama-C audit pass 4: range bounds chosen so base * factor can't
    // overflow int32 (|base| <= 46340 and |factor| <= 46340 gives a max
    // |product| of just under INT32_MAX ≈ 2.15e9).
    json_t* j_base = json_object_get(input, "base");
    int32_t base = 0;
    if (json_int32_or_default(j_base, -46340, 46340, "base", &base) != 0
        || !json_is_integer(j_base)) {
        json_decref(input);
        return -1;
    }
    json_decref(input);

    int32_t factor = 1;
    const char* params_str = step_get_params(ctx);
    if (params_str) {
        json_t* params = json_loads(params_str, 0, &je);
        if (params) {
            json_t* j_factor = json_object_get(params, "factor");
            if (json_int32_or_default(j_factor, -46340, 46340, "factor", &factor) != 0) {
                json_decref(params);
                return -1;
            }
            json_decref(params);
        }
    }

    int32_t scaled = base * factor;
    return step_write_output(ctx, "scaled", &scaled, sizeof(scaled));
}

static const step_read_t reads_arr[] = {{.name = PIPELINE_INPUT_ARTIFACT}};
static const step_write_t writes_arr[] = {
    {.name = "scaled",
     .kind = AK_OPAQUE_JSON,
     .visibility = STEP_VIS_DEBUG},
};

const step_def_t demo_scale_by_factor_def = {
    .name = "scale-by-factor",
    .version = 1,
    .reads = reads_arr,
    .n_reads = 1,
    .writes = writes_arr,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// report
// Reads:  scaled, biased, product (all int32)
// Params: (none)
// Writes: report (JSON object, opaque-json kind)
//
// Composes a JSON summary of the pipeline's intermediate values.

#include <jansson.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "demo.h"

static int read_int32(step_context_t* ctx, const char* name, int32_t* out) {
    size_t n = 0;
    const void* p = step_read(ctx, name, &n);
    if (!p || n != sizeof(int32_t)) {
        return -1;
    }
    memcpy(out, p, sizeof(int32_t));
    return 0;
}

static int exec_fn(step_context_t* ctx) {
    int32_t scaled, biased, product;
    if (read_int32(ctx, "scaled", &scaled) != 0) {
        return -1;
    }
    if (read_int32(ctx, "biased", &biased) != 0) {
        return -1;
    }
    if (read_int32(ctx, "product", &product) != 0) {
        return -1;
    }

    json_t* obj = json_object();
    json_object_set_new(obj, "scaled", json_integer(scaled));
    json_object_set_new(obj, "biased", json_integer(biased));
    json_object_set_new(obj, "product", json_integer(product));

    char* serialized = json_dumps(obj, JSON_COMPACT | JSON_SORT_KEYS);
    json_decref(obj);
    if (!serialized) {
        return -1;
    }
    size_t len = strlen(serialized);
    int rc = step_write_output(ctx, "report", serialized, len);
    free(serialized);
    return rc;
}

static const step_read_t reads_arr[] = {{.name = "scaled"}, {.name = "biased"}, {.name = "product"}};
static const step_write_t writes_arr[] = {
    {.name = "report",
     .kind = AK_OPAQUE_JSON,
     .visibility = STEP_VIS_DEBUG},
};

const step_def_t demo_report_def = {
    .name = "report",
    .version = 1,
    .reads = reads_arr,
    .n_reads = 3,
    .writes = writes_arr,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};

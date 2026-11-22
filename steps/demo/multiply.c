// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// multiply
// Reads:  scaled (int32), biased (int32)
// Params: (none)
// Writes: product (int32 LE) = scaled * biased

#include <stdint.h>
#include <string.h>

#include "demo.h"

static int exec_fn(step_context_t* ctx) {
    size_t n_a = 0, n_b = 0;
    const void* a = step_read(ctx, "scaled", &n_a);
    const void* b = step_read(ctx, "biased", &n_b);
    if (!a || !b || n_a != sizeof(int32_t) || n_b != sizeof(int32_t)) {
        return -1;
    }
    int32_t va, vb;
    memcpy(&va, a, sizeof(va));
    memcpy(&vb, b, sizeof(vb));
    int32_t product = va * vb;
    return step_write_output(ctx, "product", &product, sizeof(product));
}

static const step_read_t reads_arr[] = {{.name = "scaled"}, {.name = "biased"}};
static const step_write_t writes_arr[] = {
    {.name = "product",
     .kind = AK_OPAQUE_JSON,
     .visibility = STEP_VIS_DEBUG},
};

const step_def_t demo_multiply_def = {
    .name = "multiply",
    .version = 1,
    .reads = reads_arr,
    .n_reads = 2,
    .writes = writes_arr,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// stream-demo (B9 — PLAN-VIEWER-PORT §7)
// Reads:  pipeline-input (JSON object, may set "n_chunks" int default 4 and
//                                          "chunk_bytes" int default 1024)
// Writes: stream-bytes — STREAMING. Emits n_chunks chunks of chunk_bytes
//                       each via step_write_output_stream; the last has
//                       final=true. Chunk content is a deterministic
//                       byte pattern (i*31 + k) so consumers can verify
//                       arrival order.
//
// Used by serve-level tests to exercise the STREAM_CHUNK protocol path
// without spinning up the full COPC fetch chain. Production streaming
// runs over laz-extract-disc (which streams ~20k-point chunks of LiDAR
// positions); this step has the same wire shape but no I/O dependencies.

#include <jansson.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "demo.h"

static int exec_fn(step_context_t* ctx) {
    int n_chunks = 4;
    int chunk_bytes = 1024;
    size_t in_len = 0;
    const char* in_bytes = (const char*)step_read(ctx, PIPELINE_INPUT_ARTIFACT, &in_len);
    if (in_bytes && in_len > 0) {
        json_error_t je;
        json_t* in = json_loadb(in_bytes, in_len, 0, &je);
        if (in) {
            json_t* j;
            j = json_object_get(in, "n_chunks");
            if (json_is_integer(j)) {
                long v = (long)json_integer_value(j);
                if (v > 0 && v < 1000) n_chunks = (int)v;
            }
            j = json_object_get(in, "chunk_bytes");
            if (json_is_integer(j)) {
                long v = (long)json_integer_value(j);
                if (v > 0 && v < (1 << 20)) chunk_bytes = (int)v;
            }
            json_decref(in);
        }
    }

    uint8_t* buf = malloc((size_t)chunk_bytes);
    if (!buf) return -1;
    int rc = 0;
    for (int i = 0; i < n_chunks; i++) {
        for (int k = 0; k < chunk_bytes; k++) {
            buf[k] = (uint8_t)(i * 31 + k);
        }
        bool final = (i == n_chunks - 1);
        if (step_write_output_stream(ctx, "stream-bytes", buf, (size_t)chunk_bytes, final) != 0) {
            rc = -1;
            break;
        }
    }
    free(buf);
    return rc;
}

static const step_read_t reads_arr[] = {{.name = PIPELINE_INPUT_ARTIFACT}};
static const step_write_t writes_arr[] = {
    // AK_OPAQUE_JSON kind here is just a placeholder — the bytes are raw
    // and the lab UI will receive them via STREAM_CHUNK frames; downstream
    // steps that read this artifact get the concatenated buffer.
    {.name = "stream-bytes", .kind = AK_OPAQUE_JSON, .visibility = STEP_VIS_PUBLIC,
     .streaming = true},
};

const step_def_t demo_stream_def = {
    .name = "stream-demo",
    .version = 1,
    .reads = reads_arr,
    .n_reads = 1,
    .writes = writes_arr,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};

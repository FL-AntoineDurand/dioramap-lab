// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// fetch-copc-tiles
// Reads:  copc-tile-urls (ARRAY_INLINE of {url, name} JSON objects)
// Writes: copc-raw-tiles — ARRAY_BY_HASH. Each element is a whole COPC-LAZ
//         blob stored under its content hash.
//
// Loops over the input array; sub-scope `fetch_and_cache` per URL records
// each (url-identity → content-hash) pair in the intent map so that two
// pipelines processing overlapping tile sets share per-URL cached blobs.

#include <jansson.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v3/artifact.h"
#include "v3/fetch.h"
#include "v3/store.h"

#include "copc-steps.h"

extern fetch_client_t* fetchers_shared_client(void);

static int exec_fn(step_context_t* ctx) {
    size_t n = step_array_len(ctx, "copc-tile-urls");
    if (n == 0) {
        // Legitimate edge: no COPC tiles cover the requested area. Emit an
        // empty manifest; downstream steps can detect and handle. Matches
        // the fan-process pattern in demo/fan-process.c.
        return step_write_array_by_hash(ctx, "copc-raw-tiles", NULL, 0);
    }

    fetch_client_t* client = fetchers_shared_client();
    if (!client)
        return -1;
    store_t* store = step_runner_store(ctx);
    intent_map_t* im = step_runner_intent_map(ctx);

    hash_t* element_hashes = calloc(n, sizeof(hash_t));
    if (!element_hashes)
        return -1;

    int rc = 0;
    for (size_t i = 0; i < n; i++) {
        const uint8_t* item_bytes = NULL;
        size_t item_len = 0;
        if (step_array_read_bytes(ctx, "copc-tile-urls", i, &item_bytes, &item_len) != 0) {
            rc = -1;
            break;
        }
        // Item is a JSON object {"url": "...", "name": "..."}.
        json_error_t je;
        json_t* obj = json_loadb((const char*)item_bytes, item_len, 0, &je);
        if (!obj) {
            rc = -1;
            break;
        }
        const char* url = json_string_value(json_object_get(obj, "url"));
        if (!url) {
            json_decref(obj);
            rc = -1;
            break;
        }
        if (fetch_and_cache(client, store, im, url, &element_hashes[i]) != 0) {
            json_decref(obj);
            rc = -1;
            break;
        }
        json_decref(obj);
    }

    if (rc == 0)
        rc = step_write_array_by_hash(ctx, "copc-raw-tiles", element_hashes, n);
    free(element_hashes);
    return rc;
}

static const step_read_t reads_arr[] = {
    {.name = "copc-tile-urls", .array = true},
};
static const step_write_t writes_arr[] = {
    // Raw LAZ blobs. Lab-UI debug only; the viewer consumes the
    // disc-filtered point cloud downstream, never the compressed tiles.
    {.name = "copc-raw-tiles",
     .kind = AK_ARRAY_BY_HASH,
     .visibility = STEP_VIS_DEBUG,
     .array = true},
};

const step_def_t fetch_copc_tiles_def = {
    .name = "fetch-copc-tiles",
    .version = 1,
    .reads = reads_arr,
    .n_reads = 1,
    .writes = writes_arr,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};

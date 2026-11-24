// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// find-copc-tiles
// Reads:  canonical-location (JSON with bbox_l93)
// Writes: copc-tile-urls — ARRAY_INLINE. Each element is a variable-length
//         JSON object: {"url":"...","name":"..."}.
//
// Calls the IGN WFS "IGNF_LIDAR-HD_METADONNEE:metadata" layer to enumerate
// tiles overlapping the bbox (COPC link in property "url_npl"). Result count
// is typically 1–4. The former "IGNF_NUAGES-DE-POINTS-LIDAR-HD:dalle" layer
// (props "url"/"name") was withdrawn by IGN in Nov 2026; its property names
// are still accepted as a fallback.
// Uses sub-scope `fetch_and_cache` for the WFS GET so the same WFS URL
// hits the URL-keyed intent map across pipelines.

#define _POSIX_C_SOURCE 200809L

#include <jansson.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v3/artifact.h"
#include "v3/blob.h"
#include "v3/fetch.h"
#include "v3/store.h"

#include "copc-steps.h"

extern fetch_client_t* fetchers_shared_client(void); // from steps/fetchers/fetchers.c

static int cmp_cstr(const void* a, const void* b) {
    const char* const* sa = a;
    const char* const* sb = b;
    return strcmp(*sa, *sb);
}

static int exec_fn(step_context_t* ctx) {
    // --- input bbox ---
    size_t in_len = 0;
    const char* in_bytes = (const char*)step_read(ctx, "canonical-location", &in_len);
    if (!in_bytes)
        return -1;
    json_error_t je;
    json_t* loc = json_loadb(in_bytes, in_len, 0, &je);
    if (!loc)
        return -1;
    json_t* j_bbox = json_object_get(loc, "bbox_l93");
    if (!json_is_array(j_bbox) || json_array_size(j_bbox) != 4) {
        json_decref(loc);
        return -1;
    }
    double bbox[4];
    for (size_t i = 0; i < 4; i++)
        bbox[i] = json_number_value(json_array_get(j_bbox, i));
    json_decref(loc);

    // --- WFS query ---
    char url[1024];
    int n = snprintf(url, sizeof(url),
                     "https://data.geopf.fr/wfs/ows?SERVICE=WFS&VERSION=2.0.0"
                     "&REQUEST=GetFeature&TYPENAMES=IGNF_LIDAR-HD_METADONNEE:metadata"
                     "&COUNT=9&BBOX=%.3f,%.3f,%.3f,%.3f,EPSG:2154"
                     "&SRSNAME=EPSG:2154&OUTPUTFORMAT=application/json",
                     bbox[0], bbox[1], bbox[2], bbox[3]);
    if (n < 0 || (size_t)n >= sizeof(url))
        return -1;

    // Route through fetch_and_cache — same URL → same cached blob → second
    // run of find-copc-tiles skips the HTTP round-trip entirely (PLAN-FETCHERS §F4b).
    fetch_client_t* client = fetchers_shared_client();
    if (!client)
        return -1;
    store_t* store = step_runner_store(ctx);
    intent_map_t* im = step_runner_intent_map(ctx);
    hash_t blob_hash;
    if (fetch_and_cache(client, store, im, url, &blob_hash) != 0)
        return -1;
    store_handle_t wfs_handle;
    if (store_lookup(store, &blob_hash, &wfs_handle) != 0)
        return -1;

    json_t* doc = json_loadb((const char*)wfs_handle.bytes, wfs_handle.len, 0, &je);
    store_release(store, &wfs_handle);
    if (!doc)
        return -1;
    json_t* features = json_object_get(doc, "features");
    if (!json_is_array(features)) {
        json_decref(doc);
        return -1;
    }
    size_t nf = json_array_size(features);
    if (nf == 0) {
        json_decref(doc);
        return -1;
    }

    // Collect into a serialized-JSON blob per element, then sort by URL for
    // canonical ordering (the §6 contract).
    char** items = calloc(nf, sizeof(char*));
    if (!items) {
        json_decref(doc);
        return -1;
    }
    size_t n_items = 0;
    for (size_t i = 0; i < nf; i++) {
        json_t* f = json_array_get(features, i);
        json_t* props = json_object_get(f, "properties");
        if (!props)
            continue;
        const char* tile_url = json_string_value(json_object_get(props, "url_npl"));
        if (!tile_url)
            tile_url = json_string_value(json_object_get(props, "url"));
        if (!tile_url)
            continue;
        const char* name = json_string_value(json_object_get(props, "name"));
        if (!name)
            name = json_string_value(json_object_get(props, "nom_pkk"));
        if (!name) {
            // metadata layer has no name property: use the URL's file name.
            const char* slash = strrchr(tile_url, '/');
            name = slash ? slash + 1 : tile_url;
        }
        char buf[1024];
        int m = snprintf(buf, sizeof(buf), "{\"url\":\"%s\",\"name\":\"%s\"}",
                         tile_url, name ? name : "");
        if (m < 0 || (size_t)m >= sizeof(buf))
            continue;
        items[n_items++] = strdup(buf);
    }
    json_decref(doc);
    if (n_items == 0) {
        free(items);
        return -1;
    }
    qsort(items, n_items, sizeof(char*), cmp_cstr);

    // --- serialize as ARRAY_INLINE variable-length (stride=0) via
    //     blob_push (8-byte-aligned framing).
    blob_t b = {0};
    int rc = 0;
    for (size_t i = 0; i < n_items; i++) {
        if (rc == 0) {
            if (blob_push(&b, items[i], strlen(items[i])) != 0) rc = -1;
        }
        free(items[i]);
    }
    free(items);

    if (rc == 0) {
        rc = step_write_array_inline(ctx, "copc-tile-urls",
                                     b.bytes, b.len, b.n_records, 0);
    }
    blob_free(&b);
    return rc;
}

static const step_read_t reads_arr[] = {{.name = "canonical-location"}};
static const step_write_t writes_arr[] = {
    // Internal plumbing — lab UI may inspect; viewer SDK never does.
    {.name = "copc-tile-urls",
     .kind = AK_ARRAY_INLINE,
     .visibility = STEP_VIS_DEBUG,
     .array = true},
};

const step_def_t find_copc_tiles_def = {
    .name = "find-copc-tiles",
    .version = 2, // 5.1d: ARRAY_INLINE 8-byte aligned format
    .reads = reads_arr,
    .n_reads = 1,
    .writes = writes_arr,
    .n_writes = 1,
    .compute_location = STEP_CL_CPU,
    .streaming = false,
    .exec = exec_fn,
};

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Shared infrastructure for the fetcher step family: the process-wide
// fetch_client_t lives here, created lazily on first use. Individual
// fetchers only own URL construction + param parsing.

#include "fetchers.h"

#include <jansson.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v3/fetch.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static fetch_client_t* g_client = NULL;

fetch_client_t* fetchers_shared_client(void); // exposed to fetcher step .c files

fetch_client_t* fetchers_shared_client(void) {
    pthread_mutex_lock(&g_lock);
    if (!g_client) {
        g_client = fetch_client_new(&(fetch_client_config_t){
            .per_host_concurrency = 4,
            // 180 s per request: IGN COPC LiDAR HD tiles are ~100 MB each
            // and the data.geopf.fr CDN throttles to 1 req/s per client
            // (x-ratelimit-limit-second header). With 3 retries, the
            // worst-case wall-clock for a single tile is 9 min — loud
            // failure if IGN goes deeply slow, no false negatives on
            // healthy-but-slow first-loads.
            // Proper per-host policy (token bucket + ratelimit-header
            // honoring) is tracked as F9 in PLAN-FETCHERS.md.
            .default_timeout_sec = 180,
        });
    }
    fetch_client_t* c = g_client;
    pthread_mutex_unlock(&g_lock);
    return c;
}

int fetchers_register_steps(void) {
    if (step_register(&fetch_ortho_rgb_def) != 0)
        return -1;
    if (step_register(&fetch_ortho_irc_def) != 0)
        return -1;
    if (step_register(&fetch_elevation_def) != 0)
        return -1;
    if (step_register(&fetch_cadastre_parcels_def) != 0)
        return -1;
    if (step_register(&fetch_cadastre_buildings_def) != 0)
        return -1;
    if (step_register(&fetch_ortho_surround_def) != 0)
        return -1;
    if (step_register(&fetch_elevation_surround_def) != 0)
        return -1;
    return 0;
}

void fetchers_shutdown(void) {
    pthread_mutex_lock(&g_lock);
    if (g_client) {
        fetch_client_free(g_client);
        g_client = NULL;
    }
    pthread_mutex_unlock(&g_lock);
}

int fetchers_fail_bad_body(step_context_t* ctx, const char* what, const char* url,
                           const uint8_t* body, size_t len, const char* expected) {
    char excerpt[401] = {0};
    size_t o = 0;
    for (size_t i = 0; body && i < len && o + 1 < sizeof(excerpt); i++) {
        unsigned char ch = body[i];
        excerpt[o++] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : (ch == '\n' ? ' ' : '.');
    }
    // OGC exception text, when present, is the useful bit.
    char why[200];
    fetch_ogc_exception_text((const char*)body, body ? len : 0, why, sizeof(why));
    json_t* d = json_object();
    char* dtxt = NULL;
    if (d) {
        json_object_set_new(d, "url", json_string(url ? url : ""));
        json_object_set_new(d, "expected", json_string(expected));
        json_object_set_new(d, "body_bytes", json_integer((json_int_t)len));
        json_object_set_new(d, "body_excerpt", json_string(excerpt));
        dtxt = json_dumps(d, JSON_COMPACT);
        json_decref(d);
    }
    int rc;
    if (why[0]) {
        rc = step_fail_detail(ctx, V3_ERR_UPSTREAM_FORMAT, dtxt,
                              "%s: expected %s, the service answered: %s", what, expected, why);
    } else {
        rc = step_fail_detail(ctx, V3_ERR_UPSTREAM_FORMAT, dtxt,
                              "%s: expected %s, got %zu bytes starting with \"%.60s\"", what,
                              expected, len, excerpt);
    }
    free(dtxt);
    return rc;
}

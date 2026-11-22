// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Shared infrastructure for the fetcher step family: the process-wide
// fetch_client_t lives here, created lazily on first use. Individual
// fetchers only own URL construction + param parsing.

#include "fetchers.h"

#include <pthread.h>
#include <stdlib.h>

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

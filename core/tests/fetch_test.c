// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Exercise fetch_client_t + fetch_and_cache against a local HTTP server
// started by the ctest wrapper. The wrapper passes the base URL + fixture
// path via env vars.

#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v3/fetch.h"
#include "v3/intent_map.h"
#include "v3/store.h"

static void check(int cond, const char* msg) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        exit(1);
    }
}

int main(void) {
    const char* base = getenv("TEST_URL");
    const char* cache_dir = getenv("TEST_CACHE_DIR");
    check(base != NULL && cache_dir != NULL, "TEST_URL + TEST_CACHE_DIR required");

    // Two distinct URLs served by the fixture. Sizes are arbitrary —
    // we just need byte equality across calls.
    char url_a[512], url_b[512];
    snprintf(url_a, sizeof(url_a), "%s/hello.txt", base);
    snprintf(url_b, sizeof(url_b), "%s/goodbye.txt", base);

    fetch_client_t* c = fetch_client_new(&(fetch_client_config_t){
        .per_host_concurrency = 2, .default_timeout_sec = 5,
    });
    check(c != NULL, "fetch_client_new");

    // 1. Basic GET.
    fetch_response_t r;
    int rc = fetch_get(c, url_a, &r);
    check(rc == 0, "fetch_get url_a");
    check(r.status_code == 200, "200 for url_a");
    check(r.body_len > 0, "body_len > 0");
    check(strstr((const char*)r.body, "hello") != NULL, "body contains hello");
    free(r.body);

    // 2. Error: 404.
    char url_404[512];
    snprintf(url_404, sizeof(url_404), "%s/missing-on-purpose", base);
    rc = fetch_get(c, url_404, &r);
    check(rc == -1, "fetch_get 404 returns -1");
    check(r.error == FETCH_4XX, "error code FETCH_4XX");
    check(r.status_code == 404, "status 404");
    free(r.body);

    // 3. fetch_and_cache: two calls with same URL → first miss, second hit.
    store_config_t scfg = {.root_dir = cache_dir, .hot_budget_bytes = 1024 * 1024};
    store_t* store = store_open(&scfg);
    check(store != NULL, "store_open");

    char im_path[1024];
    snprintf(im_path, sizeof(im_path), "%s/intent.log", cache_dir);
    intent_map_t* im = intent_map_open(im_path);
    check(im != NULL, "intent_map_open");

    hash_t h1, h2;
    check(fetch_and_cache(c, store, im, url_a, &h1) == 0, "fetch_and_cache 1");
    check(fetch_and_cache(c, store, im, url_a, &h2) == 0, "fetch_and_cache 2");
    check(hash_eq(&h1, &h2), "same URL → same hash");

    fetch_stats_t stats;
    fetch_client_get_stats(c, &stats);
    check(stats.cache_hits == 1, "cache_hits == 1");
    check(stats.cache_misses == 1, "cache_misses == 1");

    // 4. Different URL → different hash.
    hash_t h3;
    check(fetch_and_cache(c, store, im, url_b, &h3) == 0, "fetch_and_cache url_b");
    check(!hash_eq(&h1, &h3), "different URL → different hash");

    // 5. Store actually contains the bytes.
    store_handle_t sh;
    check(store_lookup(store, &h1, &sh) == 0, "store_lookup hit");
    check(sh.len > 0, "stored body non-empty");
    check(strstr((const char*)sh.bytes, "hello") != NULL, "stored body has hello");
    store_release(store, &sh);

    // 6. Intent map persists the URL → content mapping so a fresh process
    //    would cache-hit. Verify the record exists in-memory.
    intent_map_stats_t im_stats;
    intent_map_get_stats(im, &im_stats);
    check(im_stats.live_entries >= 2, "intent map recorded both URLs");

    intent_map_close(im);
    store_close(store);
    fetch_client_free(c);
    printf("OK\n");
    return 0;
}

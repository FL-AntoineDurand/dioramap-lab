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

// docs/ERRORS.md: the exception text of IGN's two OGC error shapes (real
// responses from data.geopf.fr, 2026-11-24) ends up in the failure message.
static void test_ogc_exception_text(void) {
    const char* wfs =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?><ows:ExceptionReport version=\"2.0.0\">\n"
        "  <ows:Exception exceptionCode=\"InvalidParameterValue\" locator=\"namespace\">\n"
        "    <ows:ExceptionText>Unknown namespace [IGNF_NUAGES-DE-POINTS-LIDAR-HD]</ows:ExceptionText>\n"
        "  </ows:Exception>\n</ows:ExceptionReport>";
    const char* wms =
        "<ServiceExceptionReport version=\"1.3.0\" xmlns=\"http://www.opengis.net/ogc\">"
        "<ServiceException code=\"LayerNotDefined\">Layer X unknown</ServiceException>"
        "</ServiceExceptionReport>";
    char out[128];
    fetch_ogc_exception_text(wfs, strlen(wfs), out, sizeof(out));
    check(strcmp(out, "Unknown namespace [IGNF_NUAGES-DE-POINTS-LIDAR-HD]") == 0, out);
    fetch_ogc_exception_text(wms, strlen(wms), out, sizeof(out));
    check(strcmp(out, "Layer X unknown") == 0, out);
    fetch_ogc_exception_text("\xff\xd8\xff\xe0 jpeg", 8, out, sizeof(out));
    check(out[0] == '\0', "no exception text in a JPEG");
    char tiny[6];
    fetch_ogc_exception_text(wms, strlen(wms), tiny, sizeof(tiny));
    check(strcmp(tiny, "Layer") == 0, "truncates to the buffer");
}

int main(void) {
    test_ogc_exception_text();
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

    // 7. fetch_cache_forget: the next fetch_and_cache goes back to the
    //    network (an empty IGN tile index must not stick after IGN publishes).
    check(fetch_cache_forget(im, url_a) == 0, "fetch_cache_forget");
    check(fetch_cache_forget(im, url_a) == 0, "forget twice is fine");
    hash_t h4;
    check(fetch_and_cache(c, store, im, url_a, &h4) == 0, "fetch_and_cache after forget");
    fetch_client_get_stats(c, &stats);
    check(stats.cache_misses == 3, "forgotten URL is a miss again");
    check(hash_eq(&h1, &h4), "same bytes → same content hash");

    intent_map_close(im);
    store_close(store);
    fetch_client_free(c);
    printf("OK\n");
    return 0;
}

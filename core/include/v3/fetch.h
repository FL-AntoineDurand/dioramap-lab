// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#ifndef V3_FETCH_H
#define V3_FETCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>   // ACSL valid_read_string

#include "v3/hash.h"
#include "v3/intent_map.h"
#include "v3/store.h"

#ifdef __cplusplus
extern "C" {
#endif

// Outbound HTTPS library (PLAN-FETCHERS). Endpoint-agnostic — URL
// construction, payload parsing, and coordinate math live in the steps
// that call this library.
//
// Threading model: one dedicated fetch thread drives libcurl's multi-handle
// via epoll. Callers submit async requests and wait on a future/cv. The
// fetch thread is separate from the compute pool and the server's service
// thread.

typedef struct fetch_client fetch_client_t;

typedef enum fetch_error_code {
    FETCH_OK = 0,
    FETCH_TIMEOUT,
    FETCH_DNS_FAIL,
    FETCH_TLS_FAIL,
    FETCH_4XX,
    FETCH_5XX,
    FETCH_429_RATE_LIMIT,
    FETCH_ABORTED,
    FETCH_UNKNOWN,
    FETCH_TRUNCATED,  // 2xx body shorter than Content-Length (mid-stream cut)
} fetch_error_code_t;

typedef struct fetch_response {
    uint32_t status_code;     // HTTP status (0 on transport error)
    uint8_t* body;            // Owned; caller must free unless passed to store_put (ownership transfer)
    size_t body_len;
    fetch_error_code_t error; // FETCH_OK on success
    double duration_sec;
} fetch_response_t;

typedef struct fetch_client_config {
    int thread_count;             // default 1 (one multi-handle driver)
    int max_connections_per_host; // default 4
    double default_timeout_sec;   // default 30
    // Per-host token bucket: max in-flight requests at any moment.
    int per_host_concurrency;     // default 4
    const char* ca_bundle_path;   // NULL → OS default (/etc/ssl/certs)
    bool verbose_logging;
} fetch_client_config_t;

/*@ requires cfg == \null || \valid_read(cfg);
    assigns \nothing;
    ensures \result == \null || \valid(\result);
*/
fetch_client_t* fetch_client_new(const fetch_client_config_t* cfg);

/*@ requires c == \null || \valid(c);
    assigns *c;
*/
void fetch_client_free(fetch_client_t* c);

// Synchronous GET. Blocks the calling thread until complete (internally
// driven by the fetch thread's multi-handle so other fetches can overlap).
// Returns 0 on FETCH_OK (HTTP 2xx); -1 otherwise. On any return, caller owns
// response->body and must free() it unless it was handed off.
/*@ requires c == \null || \valid(c);
    requires url == \null || valid_read_string(url);
    requires out == \null || \valid(out);
    assigns *c, *out;
    ensures \result == 0 || \result == -1;
*/
int fetch_get(fetch_client_t* c, const char* url, fetch_response_t* out);

// Caching helper: URL-keyed identity → store content hash. Thin wrapper
// over `cached_compute` (HASH-MODEL.md §3, PLAN-FETCHERS F4b). On an
// intent-map hit: returns the stored content hash with zero HTTP. On miss:
// HTTP-fetches the bytes, content-hashes + stores them, records the identity
// mapping, returns the content hash. On success, *out_hash is the store
// key (content hash) for the cached bytes. Returns 0 on success, -1 on any
// fetch/store failure.
/*@ requires c == \null || \valid(c);
    requires store == \null || \valid(store);
    requires im == \null || \valid(im);
    requires url == \null || valid_read_string(url);
    requires out_hash == \null || \valid(out_hash);
    assigns *c, *store, *im, *out_hash;
    ensures \result == 0 || \result == -1;
*/
int fetch_and_cache(fetch_client_t* c, store_t* store, intent_map_t* im, const char* url,
                    hash_t* out_hash);

// Stats for diagnostics.
typedef struct fetch_stats {
    uint64_t requests_total;
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t retries;
    uint64_t rate_limit_waits;
    uint64_t errors;
} fetch_stats_t;

/*@ requires c == \null || \valid(c);
    requires \valid(out);
    assigns *out;
*/
void fetch_client_get_stats(fetch_client_t* c, fetch_stats_t* out);

#ifdef __cplusplus
}
#endif
#endif

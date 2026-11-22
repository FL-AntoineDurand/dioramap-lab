# Fetchers Subsystem Plan

*Companion to [UNIFIED-ARCHITECTURE.md](../UNIFIED-ARCHITECTURE.md). A **domain-agnostic** outbound HTTPS library. All endpoint-specific knowledge (IGN WMS query params, WFS typenames, COPC URL shapes, etc.) lives in the steps that use this library — see [PLAN-VIEWER-PORT.md](PLAN-VIEWER-PORT.md#3-external-data-sources-a9-scope) for the step-side consumer list.*

## Scope

- **libcurl + mbedTLS** glue: easy/multi handle management, TLS context, root certs.
- **Per-host connection pool** with keep-alive, configurable concurrency.
- **Retry / backoff** logic for transient failures (policy configurable per request).
- **Per-host rate limiting**: shared token bucket across concurrent callers, `Retry-After` honoring.
- **Store-integrated caching helper** (`fetch_and_cache`) — a thin wrapper over the runtime's `cached_compute` primitive (see HASH-MODEL.md), specialized for URL-keyed identities. Fetcher steps never touch libcurl directly regardless of which helper they use.
- **Fetch thread pool**: I/O-bound, separate from compute pool.
- **Control-plane client**: API key verification (cached), usage event batching + flush. Built on top of the generic fetcher, not beside it.

**Explicitly out of scope** for this library:
- URL construction for any third-party service. The step that fetches (e.g. `fetch-ortho-rgb`) builds its own URL from its params + `canonical-location`.
- Response-format parsing (JPEG / BIL / GeoJSON / LAZ). Steps own payload interpretation.
- Coordinate-system math, bbox serialization, or anything that requires knowing what the fetch is *for*.
- Per-endpoint daily-quota accounting. That's operator/step policy; the library only knows about per-host concurrency + rate limits.

## Key decisions

1. **libcurl multi handle + epoll**, not per-request blocking. Single fetch thread drives many concurrent requests.
2. **mbedTLS for outbound TLS.** Inbound TLS is terminated by nginx (§D14), so libwebsockets is built without TLS. mbedTLS is only linked to libcurl.
3. **Retry policy is per-request, set by the caller** via `fetch_request_opts_t`. The library exposes sane defaults (idempotent GETs: 3× exponential backoff up to 30s; everything else: no auto-retry) but the caller can override. Control-plane writes use a different policy than IGN GETs; the library doesn't need to know about "kinds".
4. **Per-host rate-limiting is library-scope.** Observed 429 responses from IGN under burst — the shared token bucket lives here (not in each step) so concurrent fetcher steps on the same host cooperate automatically.
5. **Control-plane fails open for a grace window** after its TTL, then fails closed. Usage events buffer locally during outage.
6. **Connection reuse.** Per-host keep-alive pool in libcurl multi. One TLS handshake, many requests.

## Data structures

### Fetch API (C)

```c
typedef struct fetch_client fetch_client_t;
typedef struct fetch_request fetch_request_t;

fetch_client_t* fetch_client_create(fetch_client_config_t);
void fetch_client_destroy(fetch_client_t*);

typedef void (*fetch_callback_t)(void* ctx, fetch_result_t*);

fetch_request_t* fetch_async(fetch_client_t*,
                              const char* url,
                              fetch_method_t method,
                              const http_headers_t* headers,
                              const void* body,
                              size_t body_len,
                              fetch_callback_t cb,
                              void* ctx);

void fetch_cancel(fetch_request_t*);

typedef struct fetch_result {
    uint32_t status_code;
    const uint8_t* body;
    size_t body_len;
    const http_headers_t* response_headers;
    error_t error;              // non-null on transport error
    double duration_sec;
} fetch_result_t;
```

Callbacks fire on the fetch thread; handlers dispatch to compute or main threads via thread-safe queues.

### Control-plane client API

```c
typedef struct cp_client cp_client_t;

cp_client_t* cp_client_create(cp_config_t);

typedef struct cp_key_info {
    bool valid;
    tier_t tier;
    rate_limits_t rate_limits;
    uint64_t cached_until_us;
} cp_key_info_t;

result_t cp_verify_key(cp_client_t*, const char* key, cp_key_info_t* out);
// Cache-hit: instant, in-memory.
// Cache-miss: blocks on fetch (called from session-accept path).
// During CP outage + grace window: returns last-known value with note.
// After grace: returns error, caller fails closed.

void cp_record_usage(cp_client_t*, const usage_event_t*);
// Buffers locally, flushes in batches every 5 seconds or 100 events.
// During CP outage: buffer keeps growing up to a cap, then oldest dropped with log warning.
```

### Fetch config

```c
typedef struct fetch_client_config {
    int thread_count;                // default 2
    int max_connections_per_host;    // default 4
    double default_timeout_sec;      // default 30
    const char* ca_bundle_path;      // root certs
    bool verbose_logging;            // logs each request
} fetch_client_config_t;
```

## Status (2026-04-28)

| Task | State | Notes |
|---|---|---|
| F1 Bare libcurl wrapper | ✅ done (B1, `dff4fef`) | Synchronous `curl_easy_*` per call (`core/src/fetch/fetch.c`). |
| F2 Multi-handle async | ❌ deferred | Implementation chose synchronous easy-handles + a per-host concurrency limiter; parallelism comes from the runner's worker threads. The `fetch_async` / multi-handle + epoll design was set aside as not-needed for current load. See `fetch.c` header comment. |
| F3 Retry + backoff | ✅ done (B1, `dff4fef`) | 3× exponential backoff (250ms/500ms/1s) on 5xx + transport errors + 429-with-Retry-After in `fetch_get`. |
| F4 Connection reuse + keep-alive | ❌ not done | Each call does `curl_easy_init` / `curl_easy_cleanup`; no per-host connection pool. Acceptable cost given current call rate; revisit if profiling shows handshake pressure. |
| F4b `fetch_and_cache` helper | ✅ done (B3.5, `f359b50`) | Thin wrapper over `cached_compute` with `hash_namespaced("url", url)` identity. |
| F5 Control-plane verify-key skeleton | ✅ done (A10.1, `d0efd5e`) | `cp_client.c` — libcurl wrapper + in-memory verify cache (60s TTL, 300s grace). |
| F6 Control-plane usage batching | ✅ done (A10.1, `d0efd5e`) | Ring buffer + dedicated flush thread (5s/100-event); validated via `cp_stub_server.py`. |
| F7 Control-plane grace window | ✅ done (A10.1, `d0efd5e`) | 60s TTL + 300s grace — see `cp_client.c` cache header. |
| F8 Cert pinning | ❌ deferred | Not Phase A blocking; not implemented. |
| F9 Per-host rate limiting | ⚠️ partial (B1, `dff4fef`) | Per-host concurrency cap + 429-Retry-After cooldown shared across callers (`host_limit_acquire`). True token-bucket with configurable rate+burst not implemented. |
| F10 Error classification | ✅ done (B1, `dff4fef`) | `fetch_error_code_t` enum + `classify(rc, http_status)` mapping. |

## Implementation tasks (ordered)

### F1. Bare libcurl wrapper ✅ done (B1, commit `dff4fef`)
- Single-threaded blocking GET for initial smoke.
- mbedTLS backend configured.
- Unit test hitting `https://httpbin.org/get`.

### F2. Multi-handle async ❌ deferred
- epoll-based loop.
- Fetch thread owns the loop.
- `fetch_async` submits, callback fires on completion.
- Unit test: 100 concurrent requests.

### F3. Retry + backoff ✅ done (B1, commit `dff4fef`)
- Exponential backoff for 5xx + transport errors.
- Configurable per request.
- Max attempts + total budget.
- Unit test with a stub server returning 503 then 200.

### F4. Connection reuse + keep-alive ❌ not done
- Verify libcurl multi reuses connections for same host.
- TLS handshake count test: 100 requests → few handshakes.

### F4b. `fetch_and_cache` helper — thin wrapper over `cached_compute` ✅ done (B3.5, commit `f359b50`)
*Reframed 2026-04-21 under UNIFIED N9.*

- Signature: `int fetch_and_cache(fetch_client_t*, store_t*, const char* url, hash_t* out)`.
- Implementation is a thin wrapper over the runtime's `cached_compute` primitive (PLAN-RUNNER T15):
  ```c
  int fetch_and_cache(fetch_client_t* c, store_t* store, const char* url, hash_t* out) {
      hash_t url_identity = hash_namespaced("url", url, strlen(url));
      return cached_compute(&url_identity, http_fetch_compute_fn, /*ctx=*/url, out);
  }
  ```
  where `http_fetch_compute_fn` calls `fetch_get` and returns the response bytes (ownership transferred to `cached_compute` → `store_put`).
- **Not an architectural primitive.** The architectural caching primitive is `cached_compute`; `fetch_and_cache` is a convenience for step code doing URL-keyed caching at sub-step scope. Other sub-step caching patterns (per-tile, per-region) would use `cached_compute` directly with their own identity construction.
- Every fetcher step that wants per-URL sharing across pipeline invocations uses this helper. Fetcher steps whose URL is uniquely determined by the step's declared inputs (e.g. `fetch-ortho-rgb` — same canonical-location → same URL) can use plain `fetch_get` instead; the runner's step-level `cached_compute` caches the result via the step identity.
- No step calls libcurl directly, regardless of which helper they use.
- Unit test: two calls with the same URL produce one HTTP fetch + one intent-map entry keyed by the URL identity. Different-URL calls produce distinct intent-map entries but converge on the same store entry if the response bytes happen to be identical (rare but automatic).

### F5. Control-plane client skeleton ✅ done (A10.1, commit `d0efd5e`)
- `cp_verify_key` with in-memory TTL cache.
- Points at a local stub HTTP server for tests.
- Unit test: cached hit, cache miss + fetch, cache expiry.

### F6. Control-plane usage batching ✅ done (A10.1, commit `d0efd5e`)
- `cp_record_usage` buffers events.
- Background flush thread: every 5 seconds or 100 events.
- POST to `/internal/usage` endpoint.
- Unit test with counted flushes.

### F7. Control-plane grace window ✅ done (A10.1, commit `d0efd5e`)
- Track "last successful verify" per key.
- During CP outage: keys within grace serve last-known tier.
- After grace: error; WS handler rejects new sessions.
- Usage events: buffer to cap (configurable, default 10k events); drop oldest with a warning beyond.

### F8. Cert pinning (optional, deferred) ❌ deferred
- Pin the control-plane cert for extra security.
- Not Phase A blocking.

### F9. Per-host rate limiting ⚠️ partial (B1, commit `dff4fef`)
*Per-host concurrency cap + 429-Retry-After cooldown shipped (`host_limit_acquire`/`host_limit_release` in `fetch.c`). True token-bucket with configurable rate+burst not implemented; current shape is "per-host concurrency limit + 429-driven cooldown" rather than a continuously-refilling bucket.*

- Shared token bucket keyed by hostname, configurable rate + burst.
- On 429 response: honor `Retry-After` header; extend the bucket's cooldown accordingly.
- **Proactively honor non-429 rate-limit headers when present.** Concrete motivating case observed 2026-04-29 against IGN's COPC LiDAR HD endpoint (`data.geopf.fr`): the 200 responses carry `x-ratelimit-limit-second: 1`, communicating "1 req/s per client" without ever returning a 429. Today's fetcher ignores this and runs 4-way concurrent through a 1-req/s server — wasted threads, wasted connections, no actual parallelism. F9 should parse such headers and slot the bucket accordingly. Bonus: per-host concurrency caps that go below the configured global default when a server publishes a tighter limit.
- Concurrent callers on the same host see each other's depletion — no per-step duplication of limiter state.
- Log rate-limit events via the log subsystem so operators can see if a host is starving.
- Daily-quota accounting (e.g. "IGN allows N requests/day") is NOT the library's concern — that's step or operator policy, enforced at a layer that knows what the fetch is for.

**Workaround in place (2026-04-29)**: shared `fetchers_shared_client` runs with `default_timeout_sec=180` (was 60) — `v3/steps/fetchers/fetchers.c`. Big-tile cold loads (IGN COPC ~113 MB tiles + 1-req/s throttle) need >60 s wall-clock per request. Bumping the timeout is a correctness band-aid; F9's proper rate-limit-aware bucket would let us drop the timeout AND parallelize cleanly across hosts that actually allow it.

### F10. Error classification ✅ done (B1, commit `dff4fef`)
- Map libcurl errors to a stable enum (`FETCH_TIMEOUT`, `FETCH_TLS_FAIL`, `FETCH_DNS_FAIL`, `FETCH_5XX`, `FETCH_4XX`, `FETCH_429_RATE_LIMIT`, `FETCH_ABORTED`, etc.).
- Callbacks surface the code, not the raw libcurl text.
- Step authors get a narrow, stable surface to branch on.

## Dependencies

- **Before**: logging, runtime primitives (thread pool), protocol (for invocation from steps).
- **After**: steps in Phase B use the fetcher API.

## Definition of done

- `fetch_async` handles 100 concurrent requests without resource exhaustion.
- `fetch_and_cache` dedups identical-URL fetches (two concurrent calls → one HTTP request + two cache hits).
- Retry logic verified against simulated transient failures via a local stub HTTP server.
- Per-host rate limiter honors `Retry-After` and starves requests correctly when the bucket is empty.
- Control-plane cache + grace-mode behaviour matches spec in a scripted outage test.
- Usage event batching flushes reliably.
- No TLS errors under well-configured cert bundle.
- Fetch failures propagate as typed `fetch_error_code_t` through step → runner → client ERROR message correctly.
- The library is demonstrably endpoint-agnostic: a test suite hits at least two unrelated real endpoints (e.g. `httpbin.org` and an IGN GetCapabilities URL) through the same `fetch_client_t` with no library-side changes.
- **Integration matrix coverage** (per [docs/quality/README.md](docs/quality/README.md), 2026-05-04): the cp_client portion of `core/src/fetch/` is exercised whenever scenarios run with `--control-plane-url` set (default in tests is no CP, so cp_client is dormant — see docs/quality/README.md for how to add the env vars).  The 2026-05-01 audit fix to `cp_verify_key` (re-look-up cache slot after re-acquiring `cache_lock` to avoid pointer reuse across the unlocked HTTP-roundtrip window) is regression-protected by clang `-Wthread-safety` annotations on `cache_lock` + the documented anti-pattern in CONCURRENCY.md #12.  Outbound fetcher coverage (libcurl, retries, rate limiter) currently lives in unit tests + the per-step batch ctests (`batch_viewer_fetchers` etc.); a dedicated integration scenario hitting a real endpoint is not in the matrix today (out of scope per the scenario-discipline rule — added when there's a regression to prevent).

## Open questions

- **libcurl HTTP/2 vs HTTP/1.1.** HTTP/2 multiplexes over one connection; could reduce handshakes. Not critical Phase A; default HTTP/1.1 with keep-alive is fine.
- **Cookies.** If any endpoint ever requires session cookies, libcurl handles it via `CURLOPT_COOKIEJAR` — expose as an opt in `fetch_request_opts_t`. Step-level concern when it arrives.
- **IPv6 dual-stack.** libcurl handles automatically; test on IPv6-only network.
- **Request body compression.** POSTs to control plane may benefit from gzip; negligible Phase A.

## Risk / unknowns

- **Root certificate bundle management.** Need a strategy: bundle a Mozilla cert list at build time, or trust the OS. On prod Linux: trust OS cert store (`/etc/ssl/certs`). In container: may need to COPY certs in. Document both.
- **Network interruptions.** Long-running sessions must survive transient network flaps. Fetch retries handle short outages; WS reconnect handles longer ones.
- **Endpoint-URL stability is a step concern, not a library concern.** IGN endpoints have historically moved; steps keep their URLs in step params (configurable via pipeline JSON), not hardcoded in C.

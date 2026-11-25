// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Outbound HTTPS library (A9 / B1). See PLAN-FETCHERS.
//
// Implementation choice for the first iteration: synchronous libcurl easy
// handles, one per caller thread, with a shared per-host concurrency
// limiter. Parallelism comes from the runner's worker threads each making
// their own curl_easy_perform() call — not from libcurl multi-handle +
// epoll, which we'll add later if it's needed. This keeps the first cut
// straightforward: no event loop, no fetch-thread ownership transitions,
// no async callbacks in the step author's code.
//
// Retry policy: idempotent GETs retry up to 3× with exponential backoff
// (250ms, 500ms, 1s) on transport errors or 5xx. Rate-limited (429)
// responses honor Retry-After up to the request timeout.

#define _POSIX_C_SOURCE 200809L

#include "v3/fetch.h"

#include "v3/cached_compute.h"
#include "v3/error.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <curl/curl.h>
#include <jansson.h>

#include "v3/log.h"
#include "v3/log_events.h"

// ----------------------------------------------------------------------------
// Per-host concurrency limiter
// ----------------------------------------------------------------------------

typedef struct host_limit {
    char* host;
    int in_flight;
    // Frama-C audit findings FETCH-2 + FETCH-3: was `int` which truncated
    // uint64 monotonic seconds at line 133 (~68-year uptime wrap) and forced
    // a signed/unsigned mix at the timedwait-deadline calculation. Widening
    // to int64_t lifts both ceilings and removes the (int)now / time_t
    // cast at the use sites.
    int64_t cooldown_until_sec; // monotonic seconds; honored on 429 Retry-After
    struct host_limit* next;
} host_limit_t;

// ----------------------------------------------------------------------------
// Client
// ----------------------------------------------------------------------------

struct fetch_client {
    fetch_client_config_t cfg;

    pthread_mutex_t lock;
    pthread_cond_t limit_cv;
    host_limit_t* hosts; // linked list (one entry per unique host)

    // Stats.
    atomic_uint_least64_t stats_requests_total;
    atomic_uint_least64_t stats_cache_hits;
    atomic_uint_least64_t stats_cache_misses;
    atomic_uint_least64_t stats_retries;
    atomic_uint_least64_t stats_rate_limit_waits;
    atomic_uint_least64_t stats_errors;
};

// ----------------------------------------------------------------------------
// Utility
// ----------------------------------------------------------------------------

static uint64_t monotonic_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec;
}

static void sleep_ms(int ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

// Extract hostname (between "://" and next "/" or ":"). Returns a newly
// allocated lowercase copy. NULL on parse failure.
static char* url_host(const char* url) {
    if (!url) return NULL;  // Frama-C audit finding FETCH-1.
    const char* p = strstr(url, "://");
    if (!p)
        return NULL;
    p += 3;
    const char* q = p;
    while (*q && *q != '/' && *q != ':' && *q != '?')
        q++;
    size_t n = (size_t)(q - p);
    if (n == 0)
        return NULL;
    char* out = malloc(n + 1);
    if (!out)
        return NULL;
    for (size_t i = 0; i < n; i++) {
        out[i] = (p[i] >= 'A' && p[i] <= 'Z') ? (char)(p[i] + 32) : p[i];
    }
    out[n] = '\0';
    return out;
}

static host_limit_t* host_limit_get_locked(fetch_client_t* c, const char* host) {
    for (host_limit_t* h = c->hosts; h; h = h->next) {
        if (strcmp(h->host, host) == 0)
            return h;
    }
    host_limit_t* h = calloc(1, sizeof(*h));
    if (!h)
        return NULL;
    h->host = strdup(host);
    if (!h->host) {
        free(h);
        return NULL;
    }
    h->next = c->hosts;
    c->hosts = h;
    return h;
}

// Wait until a slot is free for `host` (and any 429 cooldown has elapsed).
// On success, the caller holds the slot and must call host_limit_release.
static int host_limit_acquire(fetch_client_t* c, const char* host) {
    pthread_mutex_lock(&c->lock);
    host_limit_t* h = host_limit_get_locked(c, host);
    if (!h) {
        pthread_mutex_unlock(&c->lock);
        return -1;
    }
    for (;;) {
        uint64_t now = monotonic_sec();
        bool cooldown = h->cooldown_until_sec > (int64_t)now;
        bool over_limit = h->in_flight >= c->cfg.per_host_concurrency;
        if (!cooldown && !over_limit)
            break;
        if (cooldown) {
            atomic_fetch_add(&c->stats_rate_limit_waits, 1);
            struct timespec abs;
            clock_gettime(CLOCK_REALTIME, &abs);
            abs.tv_sec += (time_t)(h->cooldown_until_sec - (int64_t)now) + 1;
            pthread_cond_timedwait(&c->limit_cv, &c->lock, &abs);
        } else {
            pthread_cond_wait(&c->limit_cv, &c->lock);
        }
    }
    h->in_flight++;
    pthread_mutex_unlock(&c->lock);
    return 0;
}

static void host_limit_release(fetch_client_t* c, const char* host, long http_status,
                               const char* retry_after) {
    pthread_mutex_lock(&c->lock);
    for (host_limit_t* h = c->hosts; h; h = h->next) {
        if (strcmp(h->host, host) != 0)
            continue;
        h->in_flight--;
        if (http_status == 429 && retry_after) {
            int seconds = atoi(retry_after);
            if (seconds > 0) {
                uint64_t now = monotonic_sec();
                int64_t until = (int64_t)now + seconds;
                if (until > h->cooldown_until_sec)
                    h->cooldown_until_sec = until;
            }
        }
        break;
    }
    pthread_cond_broadcast(&c->limit_cv);
    pthread_mutex_unlock(&c->lock);
}

// ----------------------------------------------------------------------------
// Write callback — collects bytes into a growable buffer.
// ----------------------------------------------------------------------------

typedef struct write_ctx {
    uint8_t* buf;
    size_t len;
    size_t cap;
} write_ctx_t;

static size_t write_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    // Frama-C audit finding FETCH-4: defend against attacker-controlled
    // size_t overflows in the curl write-callback. libcurl is generally
    // careful about size*nmemb but for HTTP responses from an
    // untrusted server the input bytes are attacker-controlled.
    if (size != 0 && nmemb > SIZE_MAX / size) return 0;
    size_t incoming = size * nmemb;
    write_ctx_t* w = userdata;
    if (incoming > SIZE_MAX - w->len) return 0;
    if (w->len + incoming > w->cap) {
        size_t nc = w->cap ? w->cap : 4096;
        while (nc < w->len + incoming) {
            if (nc > SIZE_MAX / 2) return 0;
            nc *= 2;
        }
        uint8_t* nb = realloc(w->buf, nc);
        if (!nb)
            return 0;
        w->buf = nb;
        w->cap = nc;
    }
    memcpy(w->buf + w->len, ptr, incoming);
    w->len += incoming;
    return incoming;
}

// Inspect response headers for Retry-After + Content-Length / Content-Encoding
// (latter two feed the truncated-body guard in fetch_get).
typedef struct header_ctx {
    char retry_after[32];
    int64_t content_length;  // -1 = header absent
    bool content_encoded;    // true if Content-Encoding is anything but "identity"
} header_ctx_t;

static size_t header_cb(char* buf, size_t size, size_t nmemb, void* userdata) {
    header_ctx_t* h = userdata;
    size_t len = size * nmemb;
    // Headers fire once per response; on redirects (FOLLOWLOCATION) and
    // after a 1xx interim, libcurl invokes us again — reset so only the
    // final response's values survive.
    const char* status_prefix = "HTTP/";
    if (len > strlen(status_prefix) &&
        strncasecmp(buf, status_prefix, strlen(status_prefix)) == 0) {
        h->content_length = -1;
        h->content_encoded = false;
    }
    const char* ra = "Retry-After:";
    if (len > strlen(ra) && strncasecmp(buf, ra, strlen(ra)) == 0) {
        const char* v = buf + strlen(ra);
        while (*v == ' ')
            v++;
        size_t vlen = 0;
        while (vlen < sizeof(h->retry_after) - 1 && vlen < len && v[vlen] != '\r' &&
               v[vlen] != '\n')
            vlen++;
        memcpy(h->retry_after, v, vlen);
        h->retry_after[vlen] = '\0';
    }
    const char* cl = "Content-Length:";
    if (len > strlen(cl) && strncasecmp(buf, cl, strlen(cl)) == 0) {
        const char* v = buf + strlen(cl);
        while (*v == ' ')
            v++;
        char tmp[32] = {0};
        size_t vlen = 0;
        while (vlen < sizeof(tmp) - 1 && vlen < len && v[vlen] >= '0' && v[vlen] <= '9') {
            tmp[vlen] = v[vlen];
            vlen++;
        }
        if (vlen > 0)
            h->content_length = strtoll(tmp, NULL, 10);
    }
    const char* ce = "Content-Encoding:";
    if (len > strlen(ce) && strncasecmp(buf, ce, strlen(ce)) == 0) {
        const char* v = buf + strlen(ce);
        while (*v == ' ')
            v++;
        // Anything other than "identity" (or whitespace) means the body
        // arriving in write_cb is decoded — Content-Length describes the
        // wire bytes, not the decoded bytes, so the size check would be
        // bogus. Skip it.
        if (!(strncasecmp(v, "identity", 8) == 0 &&
              (v[8] == '\0' || v[8] == '\r' || v[8] == '\n' || v[8] == ' '))) {
            h->content_encoded = true;
        }
    }
    return len;
}

// ----------------------------------------------------------------------------
// One-shot libcurl request (no retry, no limit — internal). Returns curl code.
// ----------------------------------------------------------------------------

static CURLcode do_request_once(const fetch_client_t* c, const char* url,
                                write_ctx_t* wc, header_ctx_t* hc, long* out_http_status,
                                double* out_duration) {
    CURL* curl = curl_easy_init();
    if (!curl)
        return CURLE_FAILED_INIT;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, wc);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, hc);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)(c->cfg.default_timeout_sec * 1000));
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""); // enable gzip
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "dioramap-processor/0");
    if (c->cfg.ca_bundle_path) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, c->cfg.ca_bundle_path);
    }
    if (c->cfg.verbose_logging) {
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    }

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    *out_http_status = status;
    curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, out_duration);
    curl_easy_cleanup(curl);
    return rc;
}

// Map libcurl/HTTP outcome to our typed error code.
static fetch_error_code_t classify(CURLcode rc, long http_status) {
    if (rc == CURLE_OK) {
        if (http_status >= 200 && http_status < 300)
            return FETCH_OK;
        if (http_status == 429)
            return FETCH_429_RATE_LIMIT;
        if (http_status >= 400 && http_status < 500)
            return FETCH_4XX;
        if (http_status >= 500)
            return FETCH_5XX;
        return FETCH_UNKNOWN;
    }
    switch (rc) {
    case CURLE_OPERATION_TIMEDOUT:
        return FETCH_TIMEOUT;
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
        return FETCH_DNS_FAIL;
    case CURLE_COULDNT_CONNECT:
        return FETCH_CONNECT_FAIL;
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_PEER_FAILED_VERIFICATION:
        return FETCH_TLS_FAIL;
    case CURLE_ABORTED_BY_CALLBACK:
        return FETCH_ABORTED;
    default:
        return FETCH_UNKNOWN;
    }
}

// ----------------------------------------------------------------------------
// Failure evidence (docs/ERRORS.md). Every failed fetch_get logs a
// fetch_failed event and records upstream evidence into the caller's error
// sink, so the runner can report "HTTP 400 from data.geopf.fr: Unknown
// namespace …" instead of a bare "step failed".
// ----------------------------------------------------------------------------

static const char* fetch_error_name(fetch_error_code_t e) {
    switch (e) {
    case FETCH_OK: return "ok";
    case FETCH_TIMEOUT: return "timeout";
    case FETCH_DNS_FAIL: return "dns_fail";
    case FETCH_TLS_FAIL: return "tls_fail";
    case FETCH_4XX: return "http_4xx";
    case FETCH_5XX: return "http_5xx";
    case FETCH_429_RATE_LIMIT: return "http_429_rate_limit";
    case FETCH_ABORTED: return "aborted";
    case FETCH_TRUNCATED: return "truncated_body";
    case FETCH_CONNECT_FAIL: return "connect_failed";
    case FETCH_UNKNOWN:
    default: return "unknown";
    }
}

#define BODY_EXCERPT_MAX 800

// Printable excerpt of a response body: control bytes and non-UTF-8-looking
// bytes become '.', whitespace runs collapse to one space.
static void body_excerpt(const uint8_t* body, size_t len, char* out, size_t out_sz) {
    size_t o = 0;
    bool last_space = false;
    for (size_t i = 0; i < len && o + 1 < out_sz; i++) {
        uint8_t ch = body[i];
        if (ch == '\n' || ch == '\r' || ch == '\t' || ch == ' ') {
            if (!last_space && o > 0) {
                out[o++] = ' ';
            }
            last_space = true;
            continue;
        }
        last_space = false;
        out[o++] = (ch >= 0x20 && ch < 0x7f) || ch >= 0x80 ? (char)ch : '.';
    }
    // Do not end on a truncated UTF-8 sequence.
    while (o > 0 && ((uint8_t)out[o - 1] & 0xC0) == 0x80) {
        o--;
    }
    if (o > 0 && ((uint8_t)out[o - 1] & 0xC0) == 0xC0) {
        o--;
    }
    out[o] = '\0';
}

// OGC services (WFS/WMS/WMTS) answer errors with an XML exception report:
//   <ows:ExceptionReport><ows:Exception><ows:ExceptionText>…</…>  (WFS 2.0)
//   <ServiceExceptionReport><ServiceException code="…">…</…>       (WMS 1.3)
// Pull out the first non-empty human text so the one-line message carries it.
void fetch_ogc_exception_text(const char* body, size_t len, char* out, size_t out_sz) {
    if (!out || out_sz == 0) {
        return;
    }
    out[0] = '\0';
    if (!body) {
        return;
    }
    static const char* const k_tags[] = {"ExceptionText", "ServiceException"};
    for (size_t t = 0; t < sizeof(k_tags) / sizeof(k_tags[0]); t++) {
        size_t tl = strlen(k_tags[t]);
        const char* end = body + len;
        const char* p = body;
        while (p < end) {
            const char* hit = NULL;
            for (const char* q = p; q + tl < end; q++) {
                if (memcmp(q, k_tags[t], tl) == 0 && q > body && (q[-1] == '<' || q[-1] == ':')) {
                    hit = q;
                    break;
                }
            }
            if (!hit) {
                break;
            }
            p = hit + tl;
            if (p < end && *p != '>' && *p != ' ') {
                continue; // e.g. ServiceExceptionReport
            }
            const char* gt = memchr(p, '>', (size_t)(end - p));
            if (!gt) {
                break;
            }
            const char* txt = gt + 1;
            const char* lt = memchr(txt, '<', (size_t)(end - txt));
            size_t n = lt ? (size_t)(lt - txt) : (size_t)(end - txt);
            while (n > 0 && (*txt == ' ' || *txt == '\n' || *txt == '\r' || *txt == '\t')) {
                txt++;
                n--;
            }
            while (n > 0 && (txt[n - 1] == ' ' || txt[n - 1] == '\n' || txt[n - 1] == '\r' ||
                             txt[n - 1] == '\t')) {
                n--;
            }
            if (n == 0) {
                p = txt;
                continue;
            }
            if (n >= out_sz) {
                n = out_sz - 1;
            }
            memcpy(out, txt, n);
            out[n] = '\0';
            return;
        }
    }
}

static void record_fetch_failure(const char* url, const char* host, long status,
                                 fetch_error_code_t err, CURLcode curl_rc, int attempts,
                                 double duration, const uint8_t* body, size_t body_len) {
    char excerpt[BODY_EXCERPT_MAX + 1] = {0};
    if (body && body_len > 0) {
        body_excerpt(body, body_len, excerpt, sizeof(excerpt));
    }
    char ogc[256];
    fetch_ogc_exception_text((const char*)body, body ? body_len : 0, ogc, sizeof(ogc));
    const char* curl_msg = curl_rc != CURLE_OK ? curl_easy_strerror(curl_rc) : NULL;

    LOG_EVENT(SUB_FETCH, LOG_ERROR, EV_FETCH_FAILED,
              LF_STR("url", url),
              LF_UINT("status", (uint64_t)status),
              LF_STR("error", fetch_error_name(err)),
              LF_STR("curl_error", curl_msg ? curl_msg : ""),
              LF_INT("attempts", attempts),
              LF_STR("body_excerpt", excerpt));

    if (!v3_error_current()) {
        return;
    }

    v3_error_code_t code;
    switch (err) {
    case FETCH_TIMEOUT: code = V3_ERR_TIMEOUT; break;
    case FETCH_DNS_FAIL:
    case FETCH_CONNECT_FAIL:
    case FETCH_TLS_FAIL:
    case FETCH_ABORTED: code = V3_ERR_UPSTREAM_UNREACHABLE; break;
    case FETCH_UNKNOWN: code = status ? V3_ERR_UPSTREAM_HTTP : V3_ERR_UPSTREAM_UNREACHABLE; break;
    default: code = V3_ERR_UPSTREAM_HTTP; break;
    }

    json_t* d = json_object();
    char* detail = NULL;
    if (d) {
        json_object_set_new(d, "url", json_string(url));
        if (host) json_object_set_new(d, "host", json_string(host));
        json_object_set_new(d, "http_status", json_integer((json_int_t)status));
        json_object_set_new(d, "fetch_error", json_string(fetch_error_name(err)));
        if (curl_msg) json_object_set_new(d, "curl_error", json_string(curl_msg));
        json_object_set_new(d, "attempts", json_integer(attempts));
        json_object_set_new(d, "duration_s", json_real(duration));
        if (excerpt[0]) {
            json_t* jb = json_string(excerpt);
            // json_string rejects invalid UTF-8; fall back to a note.
            json_object_set_new(d, "body_excerpt",
                                jb ? jb : json_string("(binary or non-UTF-8 body)"));
            json_object_set_new(d, "body_bytes", json_integer((json_int_t)body_len));
        }
        detail = json_dumps(d, JSON_COMPACT | JSON_REAL_PRECISION(12));
        json_decref(d);
    }

    const char* h = host ? host : "upstream";
    if (status > 0 && ogc[0]) {
        v3_error_set_upstream(code, detail, "HTTP %ld from %s: %s", status, h, ogc);
    } else if (status > 0 && err != FETCH_TRUNCATED) {
        v3_error_set_upstream(code, detail, "HTTP %ld from %s", status, h);
    } else if (err == FETCH_TRUNCATED) {
        v3_error_set_upstream(code, detail, "truncated response from %s", h);
    } else {
        v3_error_set_upstream(code, detail, "%s unreachable: %s", h,
                              curl_msg ? curl_msg : fetch_error_name(err));
    }
    free(detail);
}

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------

static void once_init_curl(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

fetch_client_t* fetch_client_new(const fetch_client_config_t* user_cfg) {
    // Wrapping curl_global_init in a void(void) function avoids the
    // (void(*)(void))curl_global_init cast the old code used to satisfy
    // pthread_once's prototype — that cast was UB per the C standard
    // (different argument types) and Frama-C 25's kernel refused to
    // parse it. The wrapper is the standard idiom; flag arg moved
    // inside.
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, once_init_curl);

    fetch_client_t* c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    if (user_cfg)
        c->cfg = *user_cfg;
    if (c->cfg.thread_count <= 0)
        c->cfg.thread_count = 1;
    if (c->cfg.max_connections_per_host <= 0)
        c->cfg.max_connections_per_host = 4;
    if (c->cfg.default_timeout_sec <= 0)
        c->cfg.default_timeout_sec = 30;
    if (c->cfg.per_host_concurrency <= 0)
        c->cfg.per_host_concurrency = 4;
    pthread_mutex_init(&c->lock, NULL);
    pthread_cond_init(&c->limit_cv, NULL);
    atomic_init(&c->stats_requests_total, 0);
    atomic_init(&c->stats_cache_hits, 0);
    atomic_init(&c->stats_cache_misses, 0);
    atomic_init(&c->stats_retries, 0);
    atomic_init(&c->stats_rate_limit_waits, 0);
    atomic_init(&c->stats_errors, 0);
    return c;
}

void fetch_client_free(fetch_client_t* c) {
    if (!c)
        return;
    pthread_mutex_destroy(&c->lock);
    pthread_cond_destroy(&c->limit_cv);
    host_limit_t* h = c->hosts;
    while (h) {
        host_limit_t* next = h->next;
        free(h->host);
        free(h);
        h = next;
    }
    free(c);
}

int fetch_get(fetch_client_t* c, const char* url, fetch_response_t* out) {
    if (!c || !url || !out)
        return -1;
    memset(out, 0, sizeof(*out));

    char* host = url_host(url);
    if (!host) {
        out->error = FETCH_UNKNOWN;
        return -1;
    }
    if (host_limit_acquire(c, host) != 0) {
        free(host);
        out->error = FETCH_UNKNOWN;
        return -1;
    }

    const int max_attempts = 3;
    const int backoff_ms[] = {250, 500, 1000};
    CURLcode rc = CURLE_OK;
    long status = 0;
    write_ctx_t wc = {0};
    header_ctx_t hc = {0};
    double duration = 0;
    fetch_error_code_t final_code = FETCH_UNKNOWN;

    int attempts_made = 0;
    for (int attempt = 0; attempt < max_attempts; attempt++) {
        attempts_made = attempt + 1;
        // Reset per-attempt state.
        free(wc.buf);
        memset(&wc, 0, sizeof(wc));
        memset(&hc, 0, sizeof(hc));
        hc.content_length = -1;
        rc = do_request_once(c, url, &wc, &hc, &status, &duration);
        atomic_fetch_add(&c->stats_requests_total, 1);
        fetch_error_code_t code = classify(rc, status);
        // Truncated-body guard: server advertised a body length but we
        // received fewer bytes (mid-stream drop, gateway timeout, HTTP/2
        // RST_STREAM). Skip when the body was content-encoded — then
        // Content-Length describes wire bytes, not what landed in write_cb.
        if (code == FETCH_OK && hc.content_length >= 0 && !hc.content_encoded &&
            (int64_t)wc.len != hc.content_length) {
            code = FETCH_TRUNCATED;
        }
        final_code = code;

        bool retry = false;
        if (code == FETCH_OK) {
            break;
        }
        if (code == FETCH_5XX || code == FETCH_TIMEOUT || code == FETCH_DNS_FAIL ||
            code == FETCH_CONNECT_FAIL ||
            code == FETCH_429_RATE_LIMIT || code == FETCH_TRUNCATED) {
            retry = (attempt + 1 < max_attempts);
        }
        if (!retry) {
            break;
        }
        atomic_fetch_add(&c->stats_retries, 1);
        int wait_ms = backoff_ms[attempt];
        if (code == FETCH_429_RATE_LIMIT && hc.retry_after[0]) {
            int s = atoi(hc.retry_after);
            if (s > 0)
                wait_ms = s * 1000;
        }
        sleep_ms(wait_ms);
    }

    host_limit_release(c, host, status, hc.retry_after[0] ? hc.retry_after : NULL);

    out->status_code = (uint32_t)status;
    out->duration_sec = duration;
    out->body = wc.buf;
    out->body_len = wc.len;
    out->error = final_code;
    if (out->error != FETCH_OK) {
        atomic_fetch_add(&c->stats_errors, 1);
        record_fetch_failure(url, host, status, final_code, rc, attempts_made, duration,
                             wc.buf, wc.len);
        free(host);
        return -1;
    }
    free(host);
    return 0;
}

// Thin wrapper over cached_compute with a URL-derived identity. See
// PLAN-FETCHERS F4b. The identity is hash_namespaced("url", url) — NOT
// hash(url) — so it can't collide with content hashes in the same store.
static int http_fetch_compute_fn(void* ctx_void, uint8_t** out_bytes, size_t* out_len) {
    struct {
        fetch_client_t* c;
        const char* url;
    }* ctx = ctx_void;
    fetch_response_t resp;
    int rc = fetch_get(ctx->c, ctx->url, &resp);
    if (rc != 0) {
        free(resp.body);
        return -1;
    }
    // resp.body is malloc'd; transfer ownership to cached_compute → store_put.
    *out_bytes = resp.body;
    *out_len = resp.body_len;
    return 0;
}

int fetch_and_cache(fetch_client_t* c, store_t* store, intent_map_t* im, const char* url,
                    hash_t* out_hash) {
    if (!c || !store || !im || !url || !out_hash)
        return -1;

    hash_t url_identity = hash_namespaced("url", url, strlen(url));

    // Check hit before bumping the internal miss counter so we can report
    // accurate stats. cached_compute's own accounting is in the intent map's.
    hash_t maybe_hit;
    if (intent_map_get(im, &url_identity, &maybe_hit) == 0 &&
        store_contains(store, &maybe_hit)) {
        atomic_fetch_add(&c->stats_cache_hits, 1);
        *out_hash = maybe_hit;
        return 0;
    }
    atomic_fetch_add(&c->stats_cache_misses, 1);

    struct {
        fetch_client_t* c;
        const char* url;
    } ctx = {c, url};
    return cached_compute(im, store, &url_identity, http_fetch_compute_fn, &ctx, out_hash);
}

int fetch_cache_forget(intent_map_t* im, const char* url) {
    if (!im || !url)
        return -1;
    hash_t url_identity = hash_namespaced("url", url, strlen(url));
    hash_t existing;
    if (intent_map_get(im, &url_identity, &existing) != 0)
        return 0; // not cached
    return intent_map_invalidate(im, &url_identity);
}

void fetch_client_get_stats(fetch_client_t* c, fetch_stats_t* out) {
    if (!c || !out)
        return;
    out->requests_total = atomic_load(&c->stats_requests_total);
    out->cache_hits = atomic_load(&c->stats_cache_hits);
    out->cache_misses = atomic_load(&c->stats_cache_misses);
    out->retries = atomic_load(&c->stats_retries);
    out->rate_limit_waits = atomic_load(&c->stats_rate_limit_waits);
    out->errors = atomic_load(&c->stats_errors);
}

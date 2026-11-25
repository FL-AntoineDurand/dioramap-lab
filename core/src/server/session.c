// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// session_t implementation.  All field access goes through the public
// API in `v3/session.h`; this file is the only place that knows the
// struct layout and the only place that takes session_t::lock.
//
// Threading model documented in:
//   - v3/docs/CONCURRENCY.md §3, §11, §12
//   - v3/docs/BUG-cache-hit-runner-cleanup.md (root cause for the
//     encapsulation push)

#define _POSIX_C_SOURCE 200809L

#include "v3/session.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

#include "server_internal.h"
#include "v3/hash.h"
#include "v3/log.h"
#include "v3/pipeline.h"
#include "v3/thread_safety.h"

// ─── token generation ──────────────────────────────────────────────────

#define SESSION_TOKEN_LEN     32   // bytes of entropy
#define SESSION_TOKEN_STRLEN  44   // ceil(32/3)*4; used 43 chars + NUL

static const char b64url_tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static void base64url_encode_nopad(const uint8_t* in, size_t in_len, char* out) {
    size_t i = 0, j = 0;
    while (i + 3 <= in_len) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | (uint32_t)in[i + 2];
        out[j++] = b64url_tab[(v >> 18) & 0x3F];
        out[j++] = b64url_tab[(v >> 12) & 0x3F];
        out[j++] = b64url_tab[(v >> 6) & 0x3F];
        out[j++] = b64url_tab[v & 0x3F];
        i += 3;
    }
    size_t rem = in_len - i;
    if (rem == 1) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[j++] = b64url_tab[(v >> 18) & 0x3F];
        out[j++] = b64url_tab[(v >> 12) & 0x3F];
    } else if (rem == 2) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[j++] = b64url_tab[(v >> 18) & 0x3F];
        out[j++] = b64url_tab[(v >> 12) & 0x3F];
        out[j++] = b64url_tab[(v >> 6) & 0x3F];
    }
    out[j] = '\0';
}

// ─── outbound message ──────────────────────────────────────────────────
// Private to this file.  Each message owns one heap buffer of size
// (LWS_PRE + payload_len) — the LWS_PRE bytes are scratch space that
// lws_write() may scribble into.

typedef struct out_msg {
    int                 lws_write_mode;   // LWS_WRITE_TEXT or LWS_WRITE_BINARY
    unsigned char*      buf;              // LWS_PRE + payload_len bytes
    size_t              payload_len;
    struct out_msg*     next;
} out_msg_t;

// Frama-C audit finding SESSION-1: guard against `LWS_PRE + len`
// overflow before malloc, otherwise an attacker-controlled `len`
// near SIZE_MAX would wrap to a tiny allocation followed by a
// `len`-byte memcpy → heap buffer overflow. `len` here flows from
// network paths (BINARY message frames, JSON dumps of streamed
// artifacts) so it must be treated as untrusted.
static out_msg_t* out_msg_new_text(const char* text, size_t len) {
    if (len > SIZE_MAX - LWS_PRE) return NULL;
    out_msg_t* m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    // ALLOC: (LWS_PRE + len) one-shot heap buffer.  Owned by the
    // out_msg_t.  Freed in out_msg_free.  Never aliased.
    m->buf = malloc(LWS_PRE + len);
    if (!m->buf) { free(m); return NULL; }
    memcpy(m->buf + LWS_PRE, text, len);
    m->payload_len    = len;
    m->lws_write_mode = LWS_WRITE_TEXT;
    return m;
}

static out_msg_t* out_msg_new_binary(const void* bytes, size_t len) {
    if (len > SIZE_MAX - LWS_PRE) return NULL;  // SESSION-1
    out_msg_t* m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->buf = malloc(LWS_PRE + len);
    if (!m->buf) { free(m); return NULL; }
    memcpy(m->buf + LWS_PRE, bytes, len);
    m->payload_len    = len;
    m->lws_write_mode = LWS_WRITE_BINARY;
    return m;
}

static void out_msg_free_(out_msg_t* m) {
    if (!m) return;
    free(m->buf);
    free(m);
}

// ─── session_t struct ──────────────────────────────────────────────────

// Thread-safety: every mutable field carries GUARDED_BY(lock).  On
// clang with -Wthread-safety, accessing any of them outside an
// MUTEX_LOCK/MUTEX_UNLOCK pair on `lock` produces a compile error.
// On gcc the annotations are no-ops (documentation only).
struct session {
    char            token[SESSION_TOKEN_STRLEN];   // immutable
    struct server*  server;                         // immutable backref

    mutex_t         lock;

    struct lws*     wsi GUARDED_BY(lock);           // NULL after disconnect

    // outbound queue (FIFO, single-consumer = service thread)
    out_msg_t*      out_head GUARDED_BY(lock);
    out_msg_t*      out_tail GUARDED_BY(lock);

    // run thread
    bool            run_active             GUARDED_BY(lock);
    bool            late_subscribe_active  GUARDED_BY(lock);
    pthread_t       run_thread             GUARDED_BY(lock);
    bool            run_thread_joinable    GUARDED_BY(lock);
    char            run_id[24]             GUARDED_BY(lock);

    // subscribe list (heap-strdup'd entries)
    char**          subscribe   GUARDED_BY(lock);
    size_t          n_subscribe GUARDED_BY(lock);

    // last RUN replay state
    const struct pipeline*  last_pipeline    GUARDED_BY(lock);
    struct pipeline*        last_owned_clone GUARDED_BY(lock);
    char*                   last_input_json  GUARDED_BY(lock);
    size_t                  last_input_len   GUARDED_BY(lock);

    // store pin list (de-duped)
    hash_t*         pins     GUARDED_BY(lock);
    size_t          n_pins   GUARDED_BY(lock);
    size_t          cap_pins GUARDED_BY(lock);

    // grace state
    bool            disconnected             GUARDED_BY(lock);
    uint64_t        disconnect_us_monotonic  GUARDED_BY(lock);

    // auth-fail close flag (D6.5)
    bool            close_after_flush GUARDED_BY(lock);

    // auth result.  Written once at ESTABLISHED via session_set_authed.
    bool            authed      GUARDED_BY(lock);
    // extension identity (v3/extension.h), written once at ESTABLISHED
    char            principal[96] GUARDED_BY(lock);
    char            account[64]   GUARDED_BY(lock);
    v3_access_t     access        GUARDED_BY(lock);

    // L6 journal accumulator
    char*           journal_pipeline_name    GUARDED_BY(lock);
    char*           journal_input_json       GUARDED_BY(lock);
    char*           journal_input_hash       GUARDED_BY(lock);
    char*           journal_test_case_label  GUARDED_BY(lock);
    uint64_t        journal_started_at_us    GUARDED_BY(lock);
    char**          journal_artifact_names   GUARDED_BY(lock);
    char**          journal_artifact_hashes  GUARDED_BY(lock);
    size_t          journal_artifact_n       GUARDED_BY(lock);
    size_t          journal_artifact_cap     GUARDED_BY(lock);
};

// ─── helpers ───────────────────────────────────────────────────────────

static uint64_t monotonic_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static void session_generate_token(session_t* s) {
    uint8_t raw[SESSION_TOKEN_LEN];
    size_t got = 0;
    while (got < sizeof(raw)) {
        ssize_t rc = getrandom(raw + got, sizeof(raw) - got, 0);
        if (rc < 0) {
            if (errno == EINTR) continue;
            // Best-effort fallback — should never happen on Linux.
            uint64_t seed = (uint64_t)getpid() ^ (uint64_t)(uintptr_t)s;
            memcpy(raw + got, &seed, sizeof(raw) - got);
            break;
        }
        got += (size_t)rc;
    }
    base64url_encode_nopad(raw, sizeof(raw), s->token);
}

// Free a single `n`-entry strdup'd array.  Used for subscribe and
// journal arrays.
static void free_str_array(char** a, size_t n) {
    if (!a) return;
    for (size_t i = 0; i < n; i++) free(a[i]);
    free(a);
}

// ─── lifecycle ─────────────────────────────────────────────────────────

session_t* session_new(struct server* sv, struct lws* wsi) {
    session_t* s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->wsi    = wsi;
    s->server = sv;
    // Frama-C audit follow-up: check pthread_mutex_init rc so a failure
    // doesn't leave a session with an uninitialized lock that the next
    // session_lock() would UB-deref.
    if (pthread_mutex_init(&s->lock, NULL) != 0) {
        free(s);
        return NULL;
    }
    session_generate_token(s);
    return s;
}

// Internal: drop all queued messages.  Caller MUST hold s->lock.
static void drain_outbound_locked(session_t* s) {
    out_msg_t* m = s->out_head;
    while (m) {
        out_msg_t* next = m->next;
        out_msg_free_(m);
        m = next;
    }
    s->out_head = s->out_tail = NULL;
}

void session_destroy(session_t* s) {
    if (!s) return;
    // ORDER: cancel + join the run thread BEFORE touching anything that
    // workers might still be reaching through s->lock.  Once joined, no
    // worker can call back into s.  See CONCURRENCY.md §12.5.
    if (s->run_thread_joinable) {
        char run_id[sizeof(s->run_id)];
        MUTEX_LOCK(&s->lock);
        snprintf(run_id, sizeof(run_id), "%s", s->run_id);
        MUTEX_UNLOCK(&s->lock);
        if (run_id[0] != '\0') {
            runner_cancel(s->server->runner, run_id);
        }
        pthread_join(s->run_thread, NULL);
        s->run_thread_joinable = false;
    }

    // Drain queue under lock (workers are gone but the service thread
    // could have armed a final write before we got here).
    MUTEX_LOCK(&s->lock);
    drain_outbound_locked(s);
    MUTEX_UNLOCK(&s->lock);

    // Subscribe list — single-owner now (no run thread).
    free_str_array(s->subscribe, s->n_subscribe);
    s->subscribe = NULL;
    s->n_subscribe = 0;

    // Pin list — release every store pin we took on behalf of this session.
    for (size_t i = 0; i < s->n_pins; i++) {
        store_unpin(s->server->store, &s->pins[i]);
    }
    free(s->pins);

    // Last-run state.
    free(s->last_input_json);
    if (s->last_owned_clone) pipeline_free(s->last_owned_clone);

    // Journal accumulator.
    free(s->journal_pipeline_name);
    free(s->journal_input_json);
    free(s->journal_input_hash);
    free(s->journal_test_case_label);
    free_str_array(s->journal_artifact_names,  s->journal_artifact_n);
    free_str_array(s->journal_artifact_hashes, s->journal_artifact_n);

    pthread_mutex_destroy(&s->lock);
    free(s);
}


// ─── identity ──────────────────────────────────────────────────────────

const char* session_token(const session_t* s) { return s->token; }
struct server* session_server(const session_t* s) { return s->server; }


// ─── wsi management ────────────────────────────────────────────────────

struct lws* session_wsi(session_t* s) {
    MUTEX_LOCK(&s->lock);
    struct lws* w = s->wsi;
    MUTEX_UNLOCK(&s->lock);
    return w;
}

void session_set_wsi(session_t* s, struct lws* wsi) {
    MUTEX_LOCK(&s->lock);
    s->wsi          = wsi;
    s->disconnected = false;
    // Drop anything queued from the previous attachment — a reconnecting
    // client is expected to re-SUBSCRIBE for any artifacts it wants.
    drain_outbound_locked(s);
    MUTEX_UNLOCK(&s->lock);
}

void session_clear_wsi(session_t* s) {
    MUTEX_LOCK(&s->lock);
    s->wsi = NULL;
    MUTEX_UNLOCK(&s->lock);
}

struct lws* session_wsi_for_arm(session_t* s) {
    // Same as session_wsi.  Separate name so the manager's call-site is
    // greppable.
    return session_wsi(s);
}

bool session_token_eq(session_t* s, const char* token) {
    MUTEX_LOCK(&s->lock);
    bool eq = (strcmp(s->token, token) == 0);
    MUTEX_UNLOCK(&s->lock);
    return eq;
}

// ─── outbound queue ────────────────────────────────────────────────────

// Enqueue a fully-formed out_msg under lock.  Returns 0 on success, -1
// if the session is closed (caller's msg is freed in that case).
static int enqueue_locked(session_t* s, out_msg_t* m) {
    MUTEX_LOCK(&s->lock);
    if (!s->wsi) {
        MUTEX_UNLOCK(&s->lock);
        out_msg_free_(m);
        return -1;
    }
    if (s->out_tail) {
        s->out_tail->next = m;
        s->out_tail = m;
    } else {
        s->out_head = s->out_tail = m;
    }
    MUTEX_UNLOCK(&s->lock);
    // Wake the service thread so it can arm a writable callback.  Safe
    // to call without the lock (lws_cancel_service is thread-safe).
    lws_cancel_service(s->server->ctx);
    return 0;
}

// Atomic header+binary pair (for ARTIFACT_HEADER + payload).
// CONCURRENCY: parallel cb_on_artifact_ready callbacks could otherwise
// interleave hdr_A → hdr_B → bin_A → bin_B; the client mis-pairs.
// Append both inserts inside one critical section.
static int enqueue_pair_locked(session_t* s, out_msg_t* hdr, out_msg_t* bin) {
    MUTEX_LOCK(&s->lock);
    if (!s->wsi) {
        MUTEX_UNLOCK(&s->lock);
        out_msg_free_(hdr); out_msg_free_(bin);
        return -1;
    }
    if (s->out_tail) s->out_tail->next = hdr;
    else             s->out_head       = hdr;
    s->out_tail = hdr;
    s->out_tail->next = bin;
    s->out_tail = bin;
    MUTEX_UNLOCK(&s->lock);
    lws_cancel_service(s->server->ctx);
    return 0;
}

int session_enqueue_text(session_t* s, const char* text, size_t len) {
    out_msg_t* m = out_msg_new_text(text, len);
    if (!m) return -1;
    return enqueue_locked(s, m);   // frees m on -1
}

int session_enqueue_json(session_t* s, json_t* obj) {
    char* txt = json_dumps(obj, JSON_COMPACT);
    json_decref(obj);
    if (!txt) return -1;
    out_msg_t* m = out_msg_new_text(txt, strlen(txt));
    free(txt);
    if (!m) return -1;
    return enqueue_locked(s, m);
}

int session_enqueue_json_with_binary(session_t* s, json_t* hdr_obj,
                                     const void* bytes, size_t len) {
    char* txt = json_dumps(hdr_obj, JSON_COMPACT);
    json_decref(hdr_obj);
    if (!txt) return -1;
    out_msg_t* hdr = out_msg_new_text(txt, strlen(txt));
    free(txt);
    if (!hdr) return -1;
    out_msg_t* bin = out_msg_new_binary(bytes, len);
    if (!bin) {
        out_msg_free_(hdr);
        return -1;
    }
    return enqueue_pair_locked(s, hdr, bin);
}

int session_enqueue_error(session_t* s, const char* code, const char* msg) {
    json_t* obj = json_object();
    if (!obj) return -1;
    json_object_set_new(obj, "t",    json_string("ERROR"));
    json_object_set_new(obj, "code", json_string(code ? code : "INTERNAL"));
    json_object_set_new(obj, "msg",  json_string(msg ? msg : ""));
    return session_enqueue_json(s, obj);
}

int session_write_one_pending(session_t* s, struct lws* wsi) {
    MUTEX_LOCK(&s->lock);
    out_msg_t* m = s->out_head;
    if (m) {
        s->out_head = m->next;
        if (!s->out_head) s->out_tail = NULL;
    }
    bool has_more = (s->out_head != NULL);
    bool close_after = s->close_after_flush;
    MUTEX_UNLOCK(&s->lock);

    if (m) {
        // lws_write can take a long time; do it OUTSIDE the lock.  No
        // other thread will touch m (we popped it), so this is safe.
        int n = lws_write(wsi, m->buf + LWS_PRE, m->payload_len,
                          (enum lws_write_protocol)m->lws_write_mode);
        out_msg_free_(m);
        if (n < 0) return -1;
    }
    if (has_more)   return 1;
    if (close_after) return -2;
    return 0;
}

bool session_has_pending(session_t* s) {
    MUTEX_LOCK(&s->lock);
    bool b = (s->out_head != NULL);
    MUTEX_UNLOCK(&s->lock);
    return b;
}

void session_drain_outbound(session_t* s) {
    MUTEX_LOCK(&s->lock);
    drain_outbound_locked(s);
    MUTEX_UNLOCK(&s->lock);
}


// ─── run thread state ──────────────────────────────────────────────────

bool session_is_run_active(session_t* s) {
    MUTEX_LOCK(&s->lock);
    bool b = s->run_active;
    MUTEX_UNLOCK(&s->lock);
    return b;
}

bool session_is_late_subscribe(session_t* s) {
    MUTEX_LOCK(&s->lock);
    bool b = s->late_subscribe_active;
    MUTEX_UNLOCK(&s->lock);
    return b;
}

int session_acquire_run_slot(session_t* s, bool late_subscribe) {
    MUTEX_LOCK(&s->lock);
    if (s->run_active) {
        MUTEX_UNLOCK(&s->lock);
        return -1;
    }
    s->run_active            = true;
    s->late_subscribe_active = late_subscribe;
    // run_thread + run_thread_joinable stay at their previous values —
    // they'll be overwritten by session_attach_run_thread once
    // pthread_create succeeds.  Joinable=false in this window means a
    // concurrent session_join_run_thread is a safe no-op.
    s->run_thread_joinable   = false;
    MUTEX_UNLOCK(&s->lock);
    return 0;
}

void session_attach_run_thread(session_t* s, pthread_t run_tid) {
    MUTEX_LOCK(&s->lock);
    s->run_thread          = run_tid;
    s->run_thread_joinable = true;
    MUTEX_UNLOCK(&s->lock);
}

void session_end_run(session_t* s) {
    MUTEX_LOCK(&s->lock);
    s->run_active = false;
    MUTEX_UNLOCK(&s->lock);
}

void session_join_run_thread(session_t* s) {
    // Read the joinable flag + thread handle under lock, then join
    // OUTSIDE the lock — pthread_join can block arbitrarily long if the
    // run thread is mid-compute.  Holding s->lock across the join would
    // wedge every callback the run is firing.
    MUTEX_LOCK(&s->lock);
    bool      joinable = s->run_thread_joinable;
    pthread_t tid      = s->run_thread;
    s->run_thread_joinable = false;   // clear up-front so a concurrent
                                       // call is a no-op
    MUTEX_UNLOCK(&s->lock);
    if (joinable) {
        pthread_join(tid, NULL);
    }
}

void session_get_run_id(session_t* s, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    MUTEX_LOCK(&s->lock);
    snprintf(out, out_size, "%s", s->run_id);
    MUTEX_UNLOCK(&s->lock);
}

void session_set_run_id(session_t* s, const char* run_id) {
    MUTEX_LOCK(&s->lock);
    snprintf(s->run_id, sizeof(s->run_id), "%s", run_id ? run_id : "");
    MUTEX_UNLOCK(&s->lock);
}


// ─── subscribe list ────────────────────────────────────────────────────

int session_set_subscribe(session_t* s, char** list, size_t n) {
    // Defensive: if an OOM caller hands us a null list with n>0, refuse.
    if (n > 0 && !list) return -1;
    MUTEX_LOCK(&s->lock);
    free_str_array(s->subscribe, s->n_subscribe);
    s->subscribe   = list;
    s->n_subscribe = n;
    MUTEX_UNLOCK(&s->lock);
    return 0;
}

void session_clear_subscribe(session_t* s) {
    MUTEX_LOCK(&s->lock);
    free_str_array(s->subscribe, s->n_subscribe);
    s->subscribe   = NULL;
    s->n_subscribe = 0;
    MUTEX_UNLOCK(&s->lock);
}

bool session_subscribe_contains(session_t* s, const char* name) {
    if (!name) return false;
    MUTEX_LOCK(&s->lock);
    bool found = false;
    for (size_t i = 0; i < s->n_subscribe; i++) {
        if (strcmp(s->subscribe[i], name) == 0) { found = true; break; }
    }
    MUTEX_UNLOCK(&s->lock);
    return found;
}

int session_subscribe_snapshot(session_t* s, char*** out_list, size_t* out_n) {
    MUTEX_LOCK(&s->lock);
    size_t n = s->n_subscribe;
    char** copy = NULL;
    if (n > 0) {
        // ALLOC: array + n strdup's.  Fully-constructed-or-fully-rolled-back:
        // any strdup failure unwinds the partial.  Caller frees via
        // session_subscribe_snapshot_free.
        copy = calloc(n, sizeof(char*));
        if (!copy) {
            MUTEX_UNLOCK(&s->lock);
            return -1;
        }
        for (size_t i = 0; i < n; i++) {
            copy[i] = strdup(s->subscribe[i]);
            if (!copy[i]) {
                for (size_t j = 0; j < i; j++) free(copy[j]);
                free(copy);
                MUTEX_UNLOCK(&s->lock);
                return -1;
            }
        }
    }
    MUTEX_UNLOCK(&s->lock);
    *out_list = copy;
    *out_n    = n;
    return 0;
}

void session_subscribe_snapshot_free(char** list, size_t n) {
    free_str_array(list, n);
}


// ─── pin list ──────────────────────────────────────────────────────────

void session_pin_artifact(session_t* s, const hash_t* h) {
    if (!h) return;
    MUTEX_LOCK(&s->lock);
    // Idempotent: linear scan is fine (sessions rarely accumulate >100
    // pins; if that ever changes, switch to a hash set).
    for (size_t i = 0; i < s->n_pins; i++) {
        if (hash_eq(&s->pins[i], h)) {
            MUTEX_UNLOCK(&s->lock);
            return;
        }
    }
    if (s->n_pins == s->cap_pins) {
        size_t nc = s->cap_pins ? s->cap_pins * 2 : 8;
        // ALLOC: realloc the pin array.  On failure, drop the pin
        // silently — the artifact remains in the store (still ref'd
        // until the run handle releases it), just not preserved past
        // the run.  Better than aborting; the alternative is dropping
        // a frame the client expects.
        hash_t* arr = realloc(s->pins, nc * sizeof(*arr));
        if (!arr) {
            MUTEX_UNLOCK(&s->lock);
            return;
        }
        s->pins = arr;
        s->cap_pins = nc;
    }
    s->pins[s->n_pins++] = *h;
    MUTEX_UNLOCK(&s->lock);

    // store_pin OUTSIDE our lock — the store has its own mutex and we
    // never hold session_t::lock across a foreign-subsystem call.
    // (CONCURRENCY.md §3 invariant 2.)
    store_pin(s->server->store, h);
}


// ─── grace / disconnect ────────────────────────────────────────────────

void session_mark_disconnected(session_t* s) {
    MUTEX_LOCK(&s->lock);
    drain_outbound_locked(s);
    s->wsi                     = NULL;
    s->disconnected            = true;
    s->disconnect_us_monotonic = monotonic_us();
    MUTEX_UNLOCK(&s->lock);
}

bool session_is_disconnected(session_t* s) {
    MUTEX_LOCK(&s->lock);
    bool b = s->disconnected;
    MUTEX_UNLOCK(&s->lock);
    return b;
}

uint64_t session_disconnect_us(session_t* s) {
    MUTEX_LOCK(&s->lock);
    uint64_t v = s->disconnect_us_monotonic;
    MUTEX_UNLOCK(&s->lock);
    return v;
}

bool session_is_expired(session_t* s, uint64_t now_us, uint64_t grace_us) {
    MUTEX_LOCK(&s->lock);
    bool exp = s->disconnected && ((now_us - s->disconnect_us_monotonic) > grace_us);
    MUTEX_UNLOCK(&s->lock);
    return exp;
}


// ─── auth ──────────────────────────────────────────────────────────────

void session_set_authed(session_t* s, bool v) {
    MUTEX_LOCK(&s->lock);
    s->authed = v;
    MUTEX_UNLOCK(&s->lock);
}

bool session_is_authed(session_t* s) {
    MUTEX_LOCK(&s->lock);
    bool v = s->authed;
    MUTEX_UNLOCK(&s->lock);
    return v;
}

void session_set_identity(session_t* s, const char* principal, const char* account,
                          v3_access_t access) {
    MUTEX_LOCK(&s->lock);
    snprintf(s->principal, sizeof(s->principal), "%s", principal ? principal : "");
    snprintf(s->account, sizeof(s->account), "%s", account ? account : "");
    s->access = access;
    MUTEX_UNLOCK(&s->lock);
}

// Written once before the session is authed and never again, so the
// buffers are stable for the session's lifetime (same contract as
// session_token).
const char* session_principal(session_t* s) {
    return s->principal;
}

const char* session_account(session_t* s) {
    return s->account;
}

v3_access_t session_access(session_t* s) {
    MUTEX_LOCK(&s->lock);
    v3_access_t a = s->access;
    MUTEX_UNLOCK(&s->lock);
    return a;
}

void session_set_close_after_flush(session_t* s, bool v) {
    MUTEX_LOCK(&s->lock);
    s->close_after_flush = v;
    MUTEX_UNLOCK(&s->lock);
}

bool session_close_after_flush(session_t* s) {
    MUTEX_LOCK(&s->lock);
    bool b = s->close_after_flush;
    MUTEX_UNLOCK(&s->lock);
    return b;
}


// ─── last RUN state ────────────────────────────────────────────────────

int session_save_last_run(session_t* s, const struct pipeline* p,
                          struct pipeline* owned_clone,
                          const char* input_json, size_t input_len) {
    if (!p || !input_json) return -1;
    // Frama-C audit finding SESSION-2: guard `input_len + 1` overflow.
    // input_len flows from network paths; an attacker-controlled size
    // near SIZE_MAX wraps to 0, malloc(0) is implementation-defined,
    // then `dup[input_len] = 0` indexes out of bounds.
    if (input_len > SIZE_MAX - 1) return -1;
    // ALLOC: deep copy of input_json into a session-owned buffer.
    // Allocate FIRST so a failure leaves the session unchanged.
    char* dup = malloc(input_len + 1);
    if (!dup) {
        // Caller-supplied owned_clone is on the failure path: caller
        // owns it on -1.
        return -1;
    }
    memcpy(dup, input_json, input_len);
    dup[input_len] = '\0';

    MUTEX_LOCK(&s->lock);
    // Free previous owned clone if any AND it isn't the same pointer
    // as the new one (covers both new-different-clone and
    // new-borrowed-pipeline cases).  CONCURRENCY: pipeline_free is
    // safe under our lock — pipelines don't take other locks.
    if (s->last_owned_clone && s->last_owned_clone != owned_clone) {
        pipeline_free(s->last_owned_clone);
    }
    free(s->last_input_json);
    s->last_pipeline    = p;
    s->last_owned_clone = owned_clone;
    s->last_input_json  = dup;
    s->last_input_len   = input_len;
    MUTEX_UNLOCK(&s->lock);
    return 0;
}

const struct pipeline* session_last_pipeline(session_t* s) {
    // Borrowed pointer.  Safety relies on the "no concurrent
    // save_last_run during late-SUBSCRIBE" invariant enforced by
    // handle_subscribe_message rejecting RUN_BUSY before reading this.
    MUTEX_LOCK(&s->lock);
    const struct pipeline* p = s->last_pipeline;
    MUTEX_UNLOCK(&s->lock);
    return p;
}

int session_last_input_snapshot(session_t* s, char** out_buf, size_t* out_len) {
    MUTEX_LOCK(&s->lock);
    if (!s->last_input_json) {
        MUTEX_UNLOCK(&s->lock);
        return -1;
    }
    size_t n = s->last_input_len;
    char*  copy = malloc(n + 1);
    if (!copy) {
        MUTEX_UNLOCK(&s->lock);
        return -1;
    }
    memcpy(copy, s->last_input_json, n);
    copy[n] = '\0';
    MUTEX_UNLOCK(&s->lock);
    *out_buf = copy;
    *out_len = n;
    return 0;
}


// ─── L6 journal accumulator ────────────────────────────────────────────

void session_journal_reset(session_t* s) {
    MUTEX_LOCK(&s->lock);
    free(s->journal_pipeline_name);   s->journal_pipeline_name   = NULL;
    free(s->journal_input_json);      s->journal_input_json      = NULL;
    free(s->journal_input_hash);      s->journal_input_hash      = NULL;
    free(s->journal_test_case_label); s->journal_test_case_label = NULL;
    s->journal_started_at_us = 0;
    free_str_array(s->journal_artifact_names,  s->journal_artifact_n);
    free_str_array(s->journal_artifact_hashes, s->journal_artifact_n);
    s->journal_artifact_names  = NULL;
    s->journal_artifact_hashes = NULL;
    s->journal_artifact_n   = 0;
    s->journal_artifact_cap = 0;
    MUTEX_UNLOCK(&s->lock);
}

int session_journal_set_identity(session_t* s, const char* pipeline_name,
                                 const char* input_json, size_t input_len,
                                 const char* input_hash_hex,
                                 const char* test_case_label) {
    if (!pipeline_name || !input_json || !input_hash_hex) return -1;
    // ALLOC: build all four strings BEFORE taking the lock.  This
    // way the OOM path doesn't touch session state at all (atomic
    // success, atomic rollback on failure).
    char* dpn = strdup(pipeline_name);
    char* dij = strndup(input_json, input_len);
    char* dih = strdup(input_hash_hex);
    char* dtl = test_case_label ? strdup(test_case_label) : NULL;
    if (!dpn || !dij || !dih || (test_case_label && !dtl)) {
        free(dpn); free(dij); free(dih); free(dtl);
        return -1;
    }
    MUTEX_LOCK(&s->lock);
    free(s->journal_pipeline_name);   s->journal_pipeline_name   = dpn;
    free(s->journal_input_json);      s->journal_input_json      = dij;
    free(s->journal_input_hash);      s->journal_input_hash      = dih;
    free(s->journal_test_case_label); s->journal_test_case_label = dtl;
    MUTEX_UNLOCK(&s->lock);
    return 0;
}

void session_journal_stamp_started(session_t* s) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t us = (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
    MUTEX_LOCK(&s->lock);
    s->journal_started_at_us = us;
    MUTEX_UNLOCK(&s->lock);
}

void session_journal_append_artifact(session_t* s, const char* name,
                                     const char* hash_hex) {
    if (!name || !hash_hex) return;
    // ALLOC: strdup OUTSIDE the lock to keep the critical section short.
    // If both succeed, take the lock and either grow the array (with
    // pre-locked realloc) and store the pair, or drop both on growth
    // failure.  Without the lock, parallel runner-worker callbacks
    // would race the realloc + slot-write — see
    // BUG-cache-hit-runner-cleanup.md.
    char* dn = strdup(name);
    char* dh = strdup(hash_hex);
    if (!dn || !dh) { free(dn); free(dh); return; }

    MUTEX_LOCK(&s->lock);
    if (s->journal_artifact_n == s->journal_artifact_cap) {
        size_t cap = s->journal_artifact_cap ? s->journal_artifact_cap * 2 : 16;
        // Realloc PER SLOT and assign immediately on success: if the
        // first realloc returned a moved pointer and the second fails,
        // the original storage was already freed, so we MUST publish
        // the new pointer before attempting the sibling realloc.
        // (Same fix shape as run_journal.c parse_produced_json.)
        char** nn = realloc(s->journal_artifact_names, cap * sizeof(char*));
        if (!nn) {
            MUTEX_UNLOCK(&s->lock);
            free(dn); free(dh);
            return;
        }
        s->journal_artifact_names = nn;
        char** nh = realloc(s->journal_artifact_hashes, cap * sizeof(char*));
        if (!nh) {
            // names was already grown; that's harmless — the unused
            // tail entries are uninitialised but n_artifacts caps reads.
            MUTEX_UNLOCK(&s->lock);
            free(dn); free(dh);
            return;
        }
        s->journal_artifact_hashes = nh;
        s->journal_artifact_cap    = cap;
    }
    s->journal_artifact_names [s->journal_artifact_n] = dn;
    s->journal_artifact_hashes[s->journal_artifact_n] = dh;
    s->journal_artifact_n++;
    MUTEX_UNLOCK(&s->lock);
}

int session_journal_snapshot(session_t* s, session_journal_snapshot_t* out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));

    MUTEX_LOCK(&s->lock);
    if (!s->journal_pipeline_name || !s->journal_input_json || !s->journal_input_hash) {
        MUTEX_UNLOCK(&s->lock);
        return -1;
    }
    // ALLOC: clone every field into caller-owned storage.  Any failure
    // in the chain unwinds everything allocated so far.
    char*  pn = strdup(s->journal_pipeline_name);
    char*  ij = strdup(s->journal_input_json);
    char*  ih = strdup(s->journal_input_hash);
    char*  tl = s->journal_test_case_label ? strdup(s->journal_test_case_label) : NULL;
    size_t n  = s->journal_artifact_n;
    char** an = NULL;
    char** ah = NULL;
    if (n > 0) {
        an = calloc(n, sizeof(char*));
        ah = calloc(n, sizeof(char*));
    }
    if (!pn || !ij || !ih || (s->journal_test_case_label && !tl) ||
        (n > 0 && (!an || !ah))) {
        goto fail;
    }
    for (size_t i = 0; i < n; i++) {
        an[i] = strdup(s->journal_artifact_names[i]);
        ah[i] = strdup(s->journal_artifact_hashes[i]);
        if (!an[i] || !ah[i]) goto fail;
    }
    uint64_t started = s->journal_started_at_us;
    MUTEX_UNLOCK(&s->lock);

    out->pipeline_name    = pn;
    out->input_json       = ij;
    out->input_hash       = ih;
    out->test_case_label  = tl;
    out->started_at_us    = started;
    out->artifact_names   = an;
    out->artifact_hashes  = ah;
    out->n_artifacts      = n;
    return 0;

fail:
    MUTEX_UNLOCK(&s->lock);
    free(pn); free(ij); free(ih); free(tl);
    if (an) for (size_t i = 0; i < n; i++) free(an[i]);
    if (ah) for (size_t i = 0; i < n; i++) free(ah[i]);
    free(an); free(ah);
    return -1;
}

void session_journal_snapshot_free(session_journal_snapshot_t* snap) {
    if (!snap) return;
    free(snap->pipeline_name);
    free(snap->input_json);
    free(snap->input_hash);
    free(snap->test_case_label);
    free_str_array(snap->artifact_names,  snap->n_artifacts);
    free_str_array(snap->artifact_hashes, snap->n_artifacts);
    memset(snap, 0, sizeof(*snap));
}

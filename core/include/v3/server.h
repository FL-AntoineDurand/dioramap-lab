// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#ifndef V3_SERVER_H
#define V3_SERVER_H

#include <stddef.h>

#include "v3/pipeline.h"
#include "v3/runner.h"
#include "v3/store.h"

#ifdef __cplusplus
extern "C" {
#endif

// WebSocket server (§8.1 of UNIFIED-ARCHITECTURE.md).
//
// Threading model:
//   - One "service" thread runs the libwebsockets event loop. All WS I/O and
//     protocol-callback work happens here.
//   - Each RUN (or late SUBSCRIBE) spawns a detached "run" thread that drives
//     runner_run(). Runner callbacks fire from the runner's worker pool;
//     they enqueue outbound messages onto the per-session output queue and
//     wake the service thread via lws_cancel_service().
//   - The service thread drains the queues using lws_callback_on_writable().
//   - A "session janitor" thread (if grace_sec > 0) sweeps expired
//     disconnected sessions.
//
// A7 shipped scope:
//   - SESSION_STARTED / RECONNECTED on connect (optionally preserved session).
//   - RUN → run thread → RUN_STARTED / STEP_STARTED / STEP_COMPLETE
//     / ARTIFACT_HEADER+binary / RUN_COMPLETE.
//   - Late SUBSCRIBE → re-run with [artifact] subscription; runner's
//     cache-skip means only the tail recomputes. Terminates with
//     SUBSCRIBE_ACK (no RUN_STARTED/RUN_COMPLETE wire echoes).
//   - CANCEL → runner_cancel(); surfaces as ERROR {msg:"cancelled"}.
//   - CATALOG (bare: pipeline names; with "pipeline" arg: per-write detail).
//   - Disconnect-keeps-session: grace period (default 600s), pins stay held
//     so RECONNECT+SUBSCRIBE serves from cache.
//
// Deferred (post-A7):
//   - HTTP endpoints (/healthz, static file serving).
//   - Routing libwebsockets's own log output through the `log` subsystem.
//   - TLS: terminated upstream at nginx (D14).

typedef struct server server_t;

// Resolver callback: given a pipeline name (as sent in the RUN message),
// return a loaded pipeline_t* or NULL if unknown. The server does NOT take
// ownership; the resolver keeps the pipeline alive for the server's lifetime.
typedef const pipeline_t* (*server_pipeline_resolver_fn)(void* ctx, const char* name);

// List all known pipeline names into `out_names` (an array the callback fills;
// ownership of the strings stays with the caller). Returns the count. If
// out_names is NULL, return the count only (the server uses this to size its
// allocation). Used by the bare CATALOG message.
typedef size_t (*server_pipeline_list_fn)(void* ctx, const char** out_names, size_t cap);

typedef struct run_journal run_journal_t;

typedef struct server_config {
    int port;                                 // TCP port to bind (e.g. 7788)
    const char* iface;                        // NULL = all interfaces (main.c defaults to 127.0.0.1)
    runner_t* runner;                         // required
    store_t* store;                           // required (for artifact reads)
    server_pipeline_resolver_fn resolve;      // required
    server_pipeline_list_fn list;             // optional; NULL → CATALOG without 'pipeline' returns empty
    void* resolve_ctx;                        // opaque ctx for resolve() and list()

    // Optional run journal (L6). When non-NULL, completed runs are
    // recorded here; LIST_RUNS / REHYDRATE_RUN messages query it.
    // When NULL, the journal-related WS messages return an empty list /
    // a NOT_AVAILABLE error.
    run_journal_t* journal;

    // Optional shared token. When NULL or empty (default), every connection
    // is accepted — intended for localhost use. When set, clients must
    // connect with `?token=<value>`; others get ERROR AUTH_FAILED and are
    // closed, and nothing they send is dispatched.
    const char* token;


    // Grace period (seconds) for disconnected sessions. A session whose WS
    // connection closes keeps its pinned artifacts in the store for this
    // long; a RECONNECT with the session_token reattaches. 0 disables the
    // grace period (disconnect → immediate teardown). Default (unset) = 600.
    double grace_sec;
} server_config_t;

/*@ requires cfg == \null || \valid_read(cfg);
    assigns \nothing;
    ensures \result == \null || \valid(\result);
*/
server_t* server_new(const server_config_t* cfg);

/*@ requires s == \null || \valid(s);
    assigns *s;
*/
void server_free(server_t* s);

// Runs the libwebsockets event loop on the calling thread until server_stop()
// is called from another thread (or a signal handler). Returns 0 on clean
// shutdown, -1 on init failure.
/*@ requires s == \null || \valid(s);
    assigns *s;
    ensures \result == 0 || \result == -1;
*/
int server_run(server_t* s);

// Thread-safe. Signals the service loop to exit.
/*@ requires s == \null || \valid(s);
    assigns *s;
*/
void server_stop(server_t* s);

#ifdef __cplusplus
}
#endif
#endif

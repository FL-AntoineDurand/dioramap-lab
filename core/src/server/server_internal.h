// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Private cross-module header for the server subsystem (server.c,
// session.c, session_mgr.c).  These three files build into one library
// (libdioramap_server.a) and share state that is opaque to outside
// callers.
//
// External callers must continue to use the public API in
// `v3/server.h`, `v3/session.h`, and `v3/session_mgr.h`.  Including
// this header outside the server/ source directory is a layering
// violation.

#pragma once

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>

#include <libwebsockets.h>

#include "v3/run_journal.h"
#include "v3/runner.h"
#include "v3/server.h"
#include "v3/store.h"

typedef struct session_mgr session_mgr_t;

// server_t — defined here so session.c can read fields without a
// long chain of accessors.  Mutability rules:
//
//   - `runner`, `store`, `journal`, `token`,
//     `resolve`, `list`, `resolve_ctx`,
//     `grace_sec` are set once in server_new and immutable thereafter.
//     Safe to read from any thread without a lock.
//   - `mgr` is set once and immutable.
//   - `ctx` is set once in server_new and torn down in server_free
//     (after janitor join, before session destroys).
//   - `should_stop` is atomic.
//
// This struct has no mutex of its own.  Sub-state synchronization
// lives in the components: session_mgr_t::sessions_lock, store_t's
// own lock, etc.
struct server {
    struct lws_context*           ctx;
    runner_t*                     runner;
    store_t*                      store;
    run_journal_t*                journal;        // L6 — optional
    char                          token[128];     // "" = no auth (default)

    server_pipeline_resolver_fn   resolve;
    server_pipeline_list_fn       list;
    void*                         resolve_ctx;

    session_mgr_t*                mgr;            // owns sessions[] + janitor

    atomic_int                    should_stop;
};

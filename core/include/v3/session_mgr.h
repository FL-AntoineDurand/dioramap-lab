// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// session_mgr_t — owns the sessions[] table, the per-server sessions
// mutex, and the optional grace-period janitor thread.
//
// Threading
// ─────────
// All public functions are thread-safe.  The manager's mutex
// (`sessions_lock` in the implementation) is acquired internally; no
// caller sees it.
//
// Lock ordering
// ─────────────
// Per CONCURRENCY.md §12.3:
//   sessions_lock → session_t::lock     (manager iteration only)
//
// The manager NEVER calls a session function that itself takes
// session_t::lock while still holding sessions_lock — the sequence is
// always: scan under sessions_lock, copy out a list of victims, drop
// sessions_lock, then call into the per-session API.

#pragma once

#include <stdbool.h>

#include <libwebsockets.h>

#include "v3/session.h"

struct server;
typedef struct session_mgr session_mgr_t;

// Create a manager.  If grace_sec > 0, also spawns a janitor thread
// that wakes ~once a second and expires disconnected sessions whose
// grace window has elapsed.
//
// Returns NULL on OOM or pthread init failure.
session_mgr_t* session_mgr_new(struct server* sv, double grace_sec);

// Free the manager.  Joins the janitor first (if any), then destroys
// every session still in the table.  Caller-owned-thread: must NOT be
// called while any runner callback could fire into a session under
// management — typical call path is `server_free` after the lws
// context has been torn down.
void session_mgr_free(session_mgr_t* mgr);

// Add a freshly created session to the table.  Thread-safe.
void session_mgr_register(session_mgr_t* mgr, session_t* s);

// Remove a session from the table (does NOT destroy it).  Thread-safe.
void session_mgr_unregister(session_mgr_t* mgr, session_t* s);

// Find a disconnected session matching `token`, set its wsi to
// `new_wsi`, mark it reattached, and return it.  Returns NULL if no
// session matches or the session is still attached.  Thread-safe.
session_t* session_mgr_reattach(session_mgr_t* mgr, const char* token,
                                struct lws* new_wsi);

// Walk every session with pending output and invoke
// lws_callback_on_writable on its wsi.  Called from the service thread
// in response to LWS_CALLBACK_EVENT_WAIT_CANCELLED.  Thread-safe.
void session_mgr_arm_pending_writes(session_mgr_t* mgr);

// Wake the janitor (e.g. after a disconnect, so it re-evaluates its
// wait deadline).  No-op if no janitor is running.  Thread-safe.
void session_mgr_wake_janitor(session_mgr_t* mgr);

// Mark a session as disconnected (drains its queue, clears wsi,
// stamps disconnect_us) AND signals the janitor so the grace timer
// starts ticking from `now`.  Thread-safe.
void session_mgr_disconnect(session_mgr_t* mgr, session_t* s);

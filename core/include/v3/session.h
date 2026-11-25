// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Per-WebSocket-connection session state, encapsulated behind a
// thread-safe accessor API.
//
// Threading model
// ───────────────
// `session_t` is opaque. The mutex that guards its mutable fields is
// internal; **no caller takes session_t::lock directly**. Every public
// function in this header either:
//   (a) is thread-safe and takes the lock internally, OR
//   (b) is documented as "caller-owned-thread" (e.g. session_destroy
//       must be called after the run thread is joined).
//
// This is the recovery from BUG-cache-hit-runner-cleanup.md, which
// happened because the previous design exposed session_t fields
// directly and required every call site to remember the locking
// convention.  Encapsulation closes that drift surface.
//
// Lock ordering
// ─────────────
// `session_t::lock` is a **leaf**: no session_* function ever acquires
// another mutex while holding it.  Sessions and the manager observe the
// global order documented in CONCURRENCY.md §12.3:
//
//   sessions_lock  →  session_t::lock        (manager iteration only)
//
// Callers must respect the same order.
//
// Memory ownership
// ────────────────
// Strings handed in to session_set_* are strdup'd internally.  Strings
// returned by session_get_*_snapshot are caller-owned heap copies and
// must be freed via the matching session_*_snapshot_free.  Strings
// returned by simple accessors (session_token) are
// internal pointers, valid for the session's lifetime, and must not be
// freed by the caller.

#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <jansson.h>
#include <libwebsockets.h>

#include "v3/extension.h"
#include "v3/hash.h"

struct server;
struct pipeline;

typedef struct session session_t;

// ─── lifecycle ─────────────────────────────────────────────────────────

// Allocate a fresh session bound to (sv, wsi).  Generates a session
// token internally.  Returns NULL on OOM.  The session is NOT yet
// registered with any session_mgr — caller does that.
session_t* session_new(struct server* sv, struct lws* wsi);

// Destroy a session and free everything it owns.  This:
//   1. cancels + joins any in-flight run thread,
//   2. drains the outbound queue,
//   3. unpins all store pins,
//   4. frees the journal accumulator + last-run state + subscribe list,
//   5. destroys the lock.
//
// Caller-owned-thread: must be called from a thread that is NOT a
// runner worker (workers may still be holding session_t::lock via a
// callback in flight).  In practice: service thread (LWS_CALLBACK_CLOSED
// when grace_sec == 0) or janitor thread (grace expiry).
void session_destroy(session_t* s);


// ─── identity (immutable for session lifetime) ─────────────────────────

// 44-char NUL-terminated base64url session token.
const char*    session_token(const session_t* s);

struct server* session_server(const session_t* s);


// ─── wsi management ────────────────────────────────────────────────────

// Snapshot the wsi pointer under lock.  Returns NULL after disconnect.
// Useful for "is this session still attached?" decisions.
struct lws* session_wsi(session_t* s);

// For RECONNECT: clear `disconnected`, install new wsi.  Atomic.
void session_set_wsi(session_t* s, struct lws* wsi);

// Atomic wsi=NULL.  Producers that try to enqueue after this call get
// -1 back and drop their msg (no use-after-free on lws_write).
void session_clear_wsi(session_t* s);


// ─── outbound queue ────────────────────────────────────────────────────

// All enqueue functions are thread-safe and wake the service thread
// internally via lws_cancel_service.  Return 0 on success, -1 if the
// session is closed (wsi == NULL) — in that case the function still
// frees any caller-supplied bytes and consumes any json_t reference.

int session_enqueue_text(session_t* s, const char* text, size_t len);
int session_enqueue_json(session_t* s, json_t* obj);   // consumes ref
int session_enqueue_json_with_binary(session_t* s, json_t* hdr_obj,
                                     const void* bytes, size_t len);
int session_enqueue_error(session_t* s, const char* code, const char* msg);

// SERVER_WRITEABLE handler: pop one msg, lws_write it, free it.
// Return code:
//   1  more msgs queued — caller should re-arm via lws_callback_on_writable
//   0  queue drained, session healthy
//  -1  lws_write failed (caller returns -1 from protocol_cb to close)
//  -2  queue drained AND close_after_flush is set (auth-fail path)
int session_write_one_pending(session_t* s, struct lws* wsi);

// True if at least one msg is queued.  Snapshot read under lock.
bool session_has_pending(session_t* s);

// Drop all queued messages.  Used on disconnect (the wsi is gone, so
// the queue would otherwise replay stale state on reconnect).
void session_drain_outbound(session_t* s);


// ─── run thread state ──────────────────────────────────────────────────

bool session_is_run_active(session_t* s);
bool session_is_late_subscribe(session_t* s);

// Acquire the run slot atomically (sets run_active + late_subscribe_active)
// WITHOUT recording a thread yet.  Pair with session_attach_run_thread
// after pthread_create succeeds, or session_end_run if it fails.
//
// Why split this from the old session_start_run (which took the tid):
// callers used to do `pthread_create(...) ; session_start_run(s, tid, ...)`,
// which let the new run thread fire cb_on_run_started BEFORE the
// late_subscribe_active flag was set — a late-SUBSCRIBE then leaked
// RUN_STARTED to the wire (task #84, surfaced by serve_full under TSan).
// Acquiring the slot up-front fixes the ordering: by the time
// pthread_create's thread can call back into the session, the flag
// is already correct.
//
// Returns 0 on success, -1 if a run is already active.
int session_acquire_run_slot(session_t* s, bool late_subscribe);

// Attach the pthread_t to a slot already acquired via
// session_acquire_run_slot.  Sets run_thread + run_thread_joinable=true.
// Caller must have called session_acquire_run_slot successfully first.
void session_attach_run_thread(session_t* s, pthread_t run_tid);

// End a run (called from cb_on_run_complete BEFORE the RUN_COMPLETE /
// SUBSCRIBE_ACK enqueue, so the client that receives RUN_COMPLETE sees
// run_active==false on its next request — task #84, was previously called
// from run_thread_main after runner_run returned, leaving a window where
// the next client RUN got RUN_BUSY).  Flips run_active to false; the
// thread is still joinable until session_join_run_thread.
void session_end_run(session_t* s);

// Join the lingering run thread if any.  Idempotent.  Caller-owned-
// thread: must NOT be called from inside a runner callback.
void session_join_run_thread(session_t* s);

// Snapshot the runner-issued run_id (e.g. "r_42").  Writes "" when no
// run is active.  Buffer should be at least 24 bytes.
void session_get_run_id(session_t* s, char* out, size_t out_size);

// Set the runner-issued run_id under lock.  Called from
// cb_on_run_started.
void session_set_run_id(session_t* s, const char* run_id);


// ─── subscribe list ────────────────────────────────────────────────────

// Replace the subscribe list under lock.  Takes ownership of `list`
// and its strings; the function frees the previous list.  On rc != 0
// `list` is freed and the previous list is unchanged.
int  session_set_subscribe(session_t* s, char** list, size_t n);

void session_clear_subscribe(session_t* s);

// Thread-safe membership test under lock.
bool session_subscribe_contains(session_t* s, const char* name);

// Snapshot the subscribe list for handing to runner_run.  *out_list is
// a caller-owned array of strdup'd strings; free with the matching
// snapshot_free.  Returns 0 on success, -1 on OOM (out params unset).
int  session_subscribe_snapshot(session_t* s, char*** out_list, size_t* out_n);
void session_subscribe_snapshot_free(char** list, size_t n);


// ─── pin list ──────────────────────────────────────────────────────────

// Idempotent: if `h` already pinned, no-op.  Otherwise: store_pin once,
// remember in the session pin list.  All pins released on
// session_destroy.
void session_pin_artifact(session_t* s, const hash_t* h);


// ─── grace / disconnect ────────────────────────────────────────────────

// Mark this session as disconnected (wsi is gone but session retained
// for grace_sec).  Atomic: clears wsi, sets disconnected = true,
// stamps disconnect_us_monotonic, drains the outbound queue.
void session_mark_disconnected(session_t* s);

bool     session_is_disconnected(session_t* s);
uint64_t session_disconnect_us(session_t* s);

// True if the session has been disconnected longer than grace_us.
// Used by the janitor's expiry sweep.  Reads under lock.
bool session_is_expired(session_t* s, uint64_t now_us, uint64_t grace_us);


// ─── auth (set once, read many) ────────────────────────────────────────

// Mark the connection as authenticated (set once at ESTABLISHED). Messages
// from a session that is not authed are never dispatched.
void session_set_authed(session_t* s, bool v);
bool session_is_authed(session_t* s);

// Identity granted by the processor extension (v3/extension.h). Set once at
// ESTABLISHED, before session_set_authed. Defaults: "", "", V3_ACCESS_FULL.
// The returned strings live as long as the session.
void        session_set_identity(session_t* s, const char* principal, const char* account,
                                 v3_access_t access);
const char* session_principal(session_t* s);
const char* session_account(session_t* s);
v3_access_t session_access(session_t* s);

void session_set_close_after_flush(session_t* s, bool v);
bool session_close_after_flush(session_t* s);


// ─── last RUN state (for late-SUBSCRIBE replay) ────────────────────────

// Replace the saved (pipeline, owned_clone, input) under lock.  Frees
// any previous owned_clone and the previous input copy.  `owned_clone`
// may be NULL when the run used a borrowed registry pipeline; in that
// case `p` is the borrowed pointer.  When `owned_clone` is non-NULL,
// `p` MUST equal `owned_clone` (the session takes ownership of the
// clone and frees it on next save_last_run or session_destroy).
int session_save_last_run(session_t* s, const struct pipeline* p,
                          struct pipeline* owned_clone,
                          const char* input_json, size_t input_len);

// Borrowed pointer into the saved pipeline.  Valid until the next
// session_save_last_run call.  NULL if no run has happened yet.
// Callers that hand this off across thread boundaries must respect
// the "no concurrent save during late-SUBSCRIBE" invariant
// (handle_subscribe_message rejects RUN_BUSY first).
const struct pipeline* session_last_pipeline(session_t* s);

// Caller-owned heap copy of the saved input.  Returns 0 on success
// with *out_buf NUL-terminated and *out_len set; -1 on OOM or no
// previous run (out params unset).  Caller frees *out_buf.
int session_last_input_snapshot(session_t* s, char** out_buf, size_t* out_len);


// ─── L6 journal accumulator ────────────────────────────────────────────

// Reset to empty: frees all owned strings.  Called at RUN start.
void session_journal_reset(session_t* s);

// Set per-run identity fields.  All inputs are strdup'd internally.
// `test_case_label` may be NULL; the others are required.  Returns -1
// on OOM (identity left in a partially-set state — caller should treat
// the run as having no journal record).
int session_journal_set_identity(session_t* s, const char* pipeline_name,
                                 const char* input_json, size_t input_len,
                                 const char* input_hash_hex,
                                 const char* test_case_label);

// Stamp wall-clock now into started_at_us under lock.  Called from
// cb_on_run_started.
void session_journal_stamp_started(session_t* s);

// Append a (name, hex_hash) pair from a runner callback.  Thread-safe.
// `name` and `hash_hex` are borrowed; both strdup'd internally.  On
// OOM the entry is dropped silently (correctness > completeness for
// the journal).  This is the canonical cb_on_artifact_ready hot path
// — keep it small.
void session_journal_append_artifact(session_t* s, const char* name,
                                     const char* hash_hex);

// Snapshot for run_journal_record.  All strings in the snapshot are
// caller-owned heap copies; free the whole snapshot via
// session_journal_snapshot_free.
//
// Returns 0 on success; -1 if the identity wasn't set (caller skips
// the journal write).  Snapshot fields:
typedef struct {
    char*    pipeline_name;     // strdup
    char*    input_json;        // strdup
    char*    input_hash;        // strdup
    char*    test_case_label;   // strdup or NULL
    uint64_t started_at_us;
    char**   artifact_names;    // strdup'd, n entries
    char**   artifact_hashes;   // strdup'd, n entries
    size_t   n_artifacts;
} session_journal_snapshot_t;

int  session_journal_snapshot(session_t* s, session_journal_snapshot_t* out);
void session_journal_snapshot_free(session_journal_snapshot_t* snap);


// ─── manager hooks (for session_mgr.c — opaque to other callers) ───────
// These exist so session_mgr can implement its iteration without
// touching session_t internals.  Not intended for general use.

// Snapshot of the wsi for arm_pending_writes (under lock; may be NULL).
struct lws* session_wsi_for_arm(session_t* s);

// Token equality test under lock.
bool session_token_eq(session_t* s, const char* token);

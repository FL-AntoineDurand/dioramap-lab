// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// WebSocket server (A7).  Public API in v3/server.h.
//
// This file is now the lws-glue layer.  Per-session state and locking
// live in session.c (`v3/session.h`); the session table + janitor live
// in session_mgr.c (`v3/session_mgr.h`).  See CONCURRENCY.md §12.
//
// Threading
// ─────────
//   Service thread    : drives lws_service(); owns all WS I/O.
//   Run thread(s)     : detached; one per RUN. Calls runner_run() which
//                       dispatches steps onto the runner's worker pool.
//   Runner workers    : fire runner callbacks (cb_on_*) which enqueue
//                       outbound messages onto the session's queue and
//                       wake the service thread.
//   Janitor thread    : owned by session_mgr; expires disconnected
//                       sessions past their grace window.
//
// Cross-thread wakeup
// ───────────────────
//   Foreign thread wants to write on a wsi:
//     1. session_enqueue_*  →  session_t::lock briefly  →  append msg
//     2. lws_cancel_service(ctx).
//   Service thread (LWS_CALLBACK_EVENT_WAIT_CANCELLED):
//     1. session_mgr_arm_pending_writes() → arm writable on each
//        session with queued output.
//   Service thread (LWS_CALLBACK_SERVER_WRITEABLE):
//     1. session_write_one_pending() → pop one msg, lws_write, free.

#define _POSIX_C_SOURCE 200809L

#include "v3/server.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jansson.h>
#include <libwebsockets.h>

#include "server_internal.h"
#include "v3/hash.h"
#include "v3/log.h"
#include "v3/run_journal.h"
#include "v3/session.h"
#include "v3/session_mgr.h"

// ---------------------------------------------------------------------------
// Pipeline cloning with per-step params overrides (L5)
// ---------------------------------------------------------------------------

static pipeline_t* clone_pipeline_with_overrides(const pipeline_t* src, json_t* overrides) {
    pipeline_t* clone = pipeline_new(pipeline_name(src));
    if (!clone) return NULL;
    size_t n = pipeline_step_count(src);
    for (size_t i = 0; i < n; i++) {
        const step_def_t* def = pipeline_step_def_at(src, i);
        const char*       base_params  = pipeline_step_params_at(src, i);
        json_t*           override_obj = json_object_get(overrides, def->name);

        char* final_params = NULL;
        if (json_is_object(override_obj)) {
            json_error_t je;
            json_t* merged = base_params ? json_loads(base_params, 0, &je) : json_object();
            if (!merged) { pipeline_free(clone); return NULL; }
            const char* k; json_t* v;
            json_object_foreach(override_obj, k, v) {
                json_object_set(merged, k, v);    // borrows v
            }
            final_params = json_dumps(merged, JSON_COMPACT);
            json_decref(merged);
            if (!final_params) { pipeline_free(clone); return NULL; }
        }
        const char* pass = final_params ? final_params : base_params;
        if (pipeline_add_step(clone, def->name, pass) != 0) {
            free(final_params); pipeline_free(clone); return NULL;
        }
        free(final_params);
    }
    if (pipeline_validate(clone) != 0) { pipeline_free(clone); return NULL; }
    return clone;
}

static int pipeline_visibility_of(const pipeline_t* p, const char* artifact_name,
                                  step_visibility_t* out_vis) {
    size_t n = pipeline_step_count(p);
    for (size_t i = 0; i < n; i++) {
        const step_def_t* def = pipeline_step_def_at(p, i);
        for (size_t w = 0; w < def->n_writes; w++) {
            if (strcmp(def->writes[w].name, artifact_name) == 0) {
                if (out_vis) *out_vis = def->writes[w].visibility;
                return 0;
            }
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Hex helper for L6 rehydrate
// ---------------------------------------------------------------------------

static int hash_from_hex64(const char* in, hash_t* out) {
    if (!in) return -1;
    if (strlen(in) != 64) return -1;
    for (size_t i = 0; i < 32; i++) {
        char hi = in[2*i], lo = in[2*i + 1];
        int hv = (hi >= '0' && hi <= '9') ? hi - '0'
               : (hi >= 'a' && hi <= 'f') ? 10 + (hi - 'a')
               : (hi >= 'A' && hi <= 'F') ? 10 + (hi - 'A') : -1;
        int lv = (lo >= '0' && lo <= '9') ? lo - '0'
               : (lo >= 'a' && lo <= 'f') ? 10 + (lo - 'a')
               : (lo >= 'A' && lo <= 'F') ? 10 + (lo - 'A') : -1;
        if (hv < 0 || lv < 0) return -1;
        out->bytes[i] = (uint8_t)((hv << 4) | lv);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Run-thread context + runner callbacks
// ---------------------------------------------------------------------------

typedef struct run_ctx {
    server_t*               server;
    session_t*              session;
    const pipeline_t*       pipeline;
    char*                   input_json;     // owned
    size_t                  input_len;
    char**                  subscribe;      // snapshot, caller-owned
    size_t                  n_subscribe;
} run_ctx_t;

static void cb_on_run_started(void* vctx, const char* run_id, const char* pipeline_name,
                              const char* const* plan, size_t n_plan) {
    session_t* s = vctx;
    // Stamp run_id and started_at_us under the session lock (each
    // accessor takes s->lock briefly).  Reset the journal accumulator
    // to drop any leftover state from a prior run.
    session_set_run_id(s, run_id);
    session_journal_stamp_started(s);
    // Suppress wire-level RUN_STARTED for late subscribes.
    if (session_is_late_subscribe(s)) return;

    json_t* obj = json_object();
    json_object_set_new(obj, "t",        json_string("RUN_STARTED"));
    json_object_set_new(obj, "run_id",   json_string(run_id));
    json_object_set_new(obj, "pipeline", json_string(pipeline_name));
    json_t* arr = json_array();
    for (size_t i = 0; i < n_plan; i++) {
        json_array_append_new(arr, json_string(plan[i]));
    }
    json_object_set_new(obj, "step_plan", arr);
    session_enqueue_json(s, obj);
}

static void cb_on_step_started(void* vctx, const char* run_id, const char* step) {
    session_t* s = vctx;
    json_t* obj = json_object();
    json_object_set_new(obj, "t",      json_string("STEP_STARTED"));
    json_object_set_new(obj, "run_id", json_string(run_id));
    json_object_set_new(obj, "step",   json_string(step));
    session_enqueue_json(s, obj);
}

static void cb_on_step_complete(void* vctx, const char* run_id, const char* step,
                                bool cache_hit, uint64_t dt_us) {
    session_t* s = vctx;
    json_t* obj = json_object();
    json_object_set_new(obj, "t",           json_string("STEP_COMPLETE"));
    json_object_set_new(obj, "run_id",      json_string(run_id));
    json_object_set_new(obj, "step",        json_string(step));
    json_object_set_new(obj, "cache_hit",   json_boolean(cache_hit));
    json_object_set_new(obj, "duration_us", json_integer((json_int_t)dt_us));
    session_enqueue_json(s, obj);
}

static void cb_on_artifact_ready(void* vctx, const char* run_id, const char* name,
                                 const hash_t* hash) {
    session_t* s = vctx;
    // L6 journal accumulator — append even if not subscribed (rehydrate
    // needs the full produced list).  Skip for late subscribes; they
    // piggyback on the parent run's record.
    if (!session_is_late_subscribe(s) && hash) {
        char hex[65];
        hash_format_hex(hash, hex);
        session_journal_append_artifact(s, name, hex);
    }
    if (!session_subscribe_contains(s, name)) return;

    // Pin first so the entry can't be evicted while we read + ship.
    session_pin_artifact(s, hash);

    server_t* sv = session_server(s);
    store_handle_t h;
    if (store_lookup(sv->store, hash, &h) != 0) {
        json_t* err = json_object();
        json_object_set_new(err, "t",        json_string("ERROR"));
        json_object_set_new(err, "run_id",   json_string(run_id));
        json_object_set_new(err, "code",     json_string("ARTIFACT_MISSING"));
        json_object_set_new(err, "artifact", json_string(name));
        session_enqueue_json(s, err);
        return;
    }
    char hex[65];
    hash_format_hex(hash, hex);
    json_t* hdr = json_object();
    json_object_set_new(hdr, "t",        json_string("ARTIFACT_HEADER"));
    json_object_set_new(hdr, "run_id",   json_string(run_id));
    json_object_set_new(hdr, "artifact", json_string(name));
    json_object_set_new(hdr, "size",     json_integer((json_int_t)h.len));
    json_object_set_new(hdr, "hash",     json_string(hex));
    (void)session_enqueue_json_with_binary(s, hdr, h.bytes, h.len);
    store_release(sv->store, &h);
}

static void cb_on_stream_chunk(void* vctx, const char* run_id, const char* name, uint32_t seq,
                               bool final, const void* bytes, size_t len, const hash_t* hash) {
    session_t* s = vctx;
    if (!session_subscribe_contains(s, name)) return;

    json_t* hdr = json_object();
    json_object_set_new(hdr, "t",        json_string("STREAM_CHUNK"));
    json_object_set_new(hdr, "run_id",   json_string(run_id));
    json_object_set_new(hdr, "artifact", json_string(name));
    json_object_set_new(hdr, "seq",      json_integer((json_int_t)seq));
    json_object_set_new(hdr, "final",    json_boolean(final));
    json_object_set_new(hdr, "size",     json_integer((json_int_t)len));
    if (final && hash) {
        char hex[65];
        hash_format_hex(hash, hex);
        json_object_set_new(hdr, "hash", json_string(hex));
        session_pin_artifact(s, hash);
    } else {
        json_object_set_new(hdr, "hash", json_null());
    }
    if (len == 0 || !bytes) {
        (void)session_enqueue_json(s, hdr);
        return;
    }
    (void)session_enqueue_json_with_binary(s, hdr, bytes, len);
}

static void cb_on_run_complete(void* vctx, const char* run_id, uint64_t dt_us, size_t executed,
                               size_t hits) {
    session_t* s  = vctx;
    server_t*  sv = session_server(s);
    bool late = session_is_late_subscribe(s);

    // L6 journal write.  Snapshot the accumulator under the session
    // lock (inside session_journal_snapshot), then call into the
    // journal OUTSIDE — the snapshot is self-contained on the heap so
    // run_journal_record never needs s->lock.
    if (sv->journal) {
        session_journal_snapshot_t snap;
        if (session_journal_snapshot(s, &snap) == 0) {
            // Build run_artifact_t[] from the snapshot.  Lifetime: the
            // snapshot's strings outlive the call.
            run_artifact_t* arts = NULL;
            if (snap.n_artifacts > 0) {
                arts = calloc(snap.n_artifacts, sizeof(*arts));
                if (arts) {
                    for (size_t i = 0; i < snap.n_artifacts; i++) {
                        arts[i].name     = snap.artifact_names[i];
                        arts[i].hash_hex = snap.artifact_hashes[i];
                    }
                }
            }
            // Snapshot subscribe[] separately for the journal record.
            char**  sub = NULL;
            size_t  n_sub = 0;
            (void)session_subscribe_snapshot(s, &sub, &n_sub);
            run_record_t rec = {
                .run_id          = run_id,
                .pipeline_name   = snap.pipeline_name,
                .input_json      = snap.input_json,
                .input_hash      = snap.input_hash,
                .test_case_label = snap.test_case_label,
                .started_at_us   = snap.started_at_us,
                .duration_us     = dt_us,
                .success         = true,
                .error_kind      = NULL,
                .error_message   = NULL,
                .n_executed      = (uint32_t)executed,
                .n_cache_hits    = (uint32_t)hits,
                .subscribe       = (const char* const*)sub,
                .n_subscribe     = n_sub,
                .produced        = arts,
                .n_produced      = arts ? snap.n_artifacts : 0,
            };
            (void)run_journal_record(sv->journal, &rec);
            free(arts);
            session_subscribe_snapshot_free(sub, n_sub);
            session_journal_snapshot_free(&snap);
        }
    }

    // Flip run_active=false BEFORE enqueuing the terminal frame
    // (RUN_COMPLETE / SUBSCRIBE_ACK).  Doing it in run_thread_main
    // after runner_run returns (the prior shape) raced: the client
    // received RUN_COMPLETE, fired the next RUN, and the new handler
    // saw run_active still true → RUN_BUSY.  Task #84 (serve_all_-
    // cache_hit_burst under cov was the canonical repro).
    //
    // Safe because:
    //  - The run thread won't issue any more callbacks after this
    //    one (runner.c calls on_run_complete last).
    //  - run_thread_joinable stays true, so session_join_run_thread
    //    still blocks the next start until this thread truly exits.
    //  - cb_on_artifact_ready reads late_subscribe_active off the
    //    session but only DURING the run — by the time end_run runs
    //    here, no more artifacts are arriving for this run.
    session_end_run(s);

    if (late) {
        // SUBSCRIBE_ACK instead of RUN_COMPLETE.  Read subscribe[0] via
        // a snapshot to avoid pointer-aliasing on the (immutable-during-
        // run) list.
        char**  sub = NULL;
        size_t  n_sub = 0;
        (void)session_subscribe_snapshot(s, &sub, &n_sub);
        json_t* ack = json_object();
        json_object_set_new(ack, "t", json_string("SUBSCRIBE_ACK"));
        if (n_sub == 1 && sub && sub[0]) {
            json_object_set_new(ack, "artifact", json_string(sub[0]));
        }
        json_object_set_new(ack, "duration_us",     json_integer((json_int_t)dt_us));
        json_object_set_new(ack, "steps_executed",  json_integer((json_int_t)executed));
        json_object_set_new(ack, "cache_hits",      json_integer((json_int_t)hits));
        session_enqueue_json(s, ack);
        session_subscribe_snapshot_free(sub, n_sub);
        return;
    }
    json_t* obj = json_object();
    json_object_set_new(obj, "t",      json_string("RUN_COMPLETE"));
    json_object_set_new(obj, "run_id", json_string(run_id));
    json_t* summ = json_object();
    json_object_set_new(summ, "duration_us",    json_integer((json_int_t)dt_us));
    json_object_set_new(summ, "steps_executed", json_integer((json_int_t)executed));
    json_object_set_new(summ, "cache_hits",     json_integer((json_int_t)hits));
    json_object_set_new(obj, "summary", summ);
    session_enqueue_json(s, obj);
}

static void cb_on_error(void* vctx, const char* run_id, const char* step, const char* msg) {
    session_t* s = vctx;
    json_t* obj = json_object();
    json_object_set_new(obj, "t", json_string("ERROR"));
    if (run_id) json_object_set_new(obj, "run_id", json_string(run_id));
    if (step)   json_object_set_new(obj, "step",   json_string(step));
    json_object_set_new(obj, "msg", json_string(msg ? msg : ""));
    session_enqueue_json(s, obj);
}

// ---------------------------------------------------------------------------
// Run thread entry
// ---------------------------------------------------------------------------

static void* run_thread_main(void* vctx) {
    run_ctx_t* rc = vctx;
    session_t* s  = rc->session;

    runner_callbacks_t cb = {
        .ctx               = s,
        .on_run_started    = cb_on_run_started,
        .on_step_started   = cb_on_step_started,
        .on_step_complete  = cb_on_step_complete,
        .on_artifact_ready = cb_on_artifact_ready,
        .on_stream_chunk   = cb_on_stream_chunk,
        .on_run_complete   = cb_on_run_complete,
        .on_error          = cb_on_error,
    };

    runner_run(rc->server->runner, rc->pipeline, rc->input_json, rc->input_len,
               (const char* const*)rc->subscribe, rc->n_subscribe, &cb);

    // Safety net: cb_on_run_complete already called session_end_run on
    // the success path (BEFORE enqueuing RUN_COMPLETE — task #84).
    // session_end_run is idempotent, so calling it again here is a
    // no-op on success but still releases the slot on failure paths
    // where on_run_complete never fires (runner.c only invokes
    // on_run_complete when final_rc == 0; on step failure it logs
    // EV_RUNNER_RUN_FAILED but issues NO terminating callback, and
    // on external cancel it fires on_error instead).
    session_end_run(s);

    // Free the run context — including the subscribe snapshot that
    // we owned for the run's lifetime.
    free(rc->input_json);
    session_subscribe_snapshot_free(rc->subscribe, rc->n_subscribe);
    free(rc);
    return NULL;
}

// ---------------------------------------------------------------------------
// Message handlers
// ---------------------------------------------------------------------------

static void send_error(session_t* s, const char* code, const char* msg) {
    (void)session_enqueue_error(s, code, msg);
}

static void handle_catalog_message(session_t* s, json_t* root) {
    server_t* sv = session_server(s);
    json_t* jp = json_object_get(root, "pipeline");
    json_t* resp = json_object();
    json_object_set_new(resp, "t", json_string("CATALOG_RESPONSE"));
    if (!jp || !json_is_string(jp)) {
        // Bare form: enumerate pipeline names.
        json_t* pipelines = json_array();
        if (sv->list) {
            size_t n = sv->list(sv->resolve_ctx, NULL, 0);
            if (n > 0) {
                const char** names = calloc(n, sizeof(char*));
                if (names) {
                    size_t got = sv->list(sv->resolve_ctx, names, n);
                    for (size_t i = 0; i < got; i++) {
                        json_array_append_new(pipelines, json_string(names[i]));
                    }
                    free(names);
                }
            }
        }
        json_object_set_new(resp, "pipelines", pipelines);
        session_enqueue_json(s, resp);
        return;
    }
    const char* pipeline_name_arg = json_string_value(jp);
    const pipeline_t* p = sv->resolve(sv->resolve_ctx, pipeline_name_arg);
    if (!p) {
        json_decref(resp);
        send_error(s, "UNKNOWN_PIPELINE", pipeline_name_arg);
        return;
    }
    json_object_set_new(resp, "pipeline", json_string(pipeline_name_arg));

    // D0 — pipeline-declared data-source attributions.
    json_t* attrs_arr = json_array();
    size_t n_attrs = pipeline_attribution_count(p);
    for (size_t a = 0; a < n_attrs; a++) {
        const pipeline_attribution_t* pa = pipeline_attribution_at(p, a);
        json_t* o = json_object();
        json_object_set_new(o, "label", json_string(pa->label));
        json_object_set_new(o, "url",   pa->url ? json_string(pa->url) : json_null());
        json_array_append_new(attrs_arr, o);
    }
    json_object_set_new(resp, "attributions", attrs_arr);

    json_t* arr = json_array();        json_object_set_new(resp, "artifacts", arr);
    json_t* steps_arr = json_array();  json_object_set_new(resp, "steps", steps_arr);
    size_t n_steps = pipeline_step_count(p);
    for (size_t i = 0; i < n_steps; i++) {
        const step_def_t* def = pipeline_step_def_at(p, i);
        for (size_t w = 0; w < def->n_writes; w++) {
            const step_write_t* wr = &def->writes[w];
            json_t* entry = json_object();
            json_object_set_new(entry, "name",       json_string(wr->name));
            json_object_set_new(entry, "kind",       json_integer(wr->kind));
            json_object_set_new(entry, "producer",   json_string(def->name));
            json_object_set_new(entry, "visibility",
                                json_string(wr->visibility == STEP_VIS_PUBLIC ? "public" : "debug"));
            json_array_append_new(arr, entry);
        }
        if (def->n_writes == 0) continue;
        json_t* step_entry = json_object();
        json_object_set_new(step_entry, "name",    json_string(def->name));
        json_object_set_new(step_entry, "version", json_integer((json_int_t)def->version));
        json_t* reads_arr = json_array();
        for (size_t r = 0; r < def->n_reads; r++) {
            json_array_append_new(reads_arr, json_string(def->reads[r].name));
        }
        json_object_set_new(step_entry, "reads", reads_arr);
        json_t* writes_arr = json_array();
        for (size_t w = 0; w < def->n_writes; w++) {
            const step_write_t* wr = &def->writes[w];
            json_t* we = json_object();
            json_object_set_new(we, "name",       json_string(wr->name));
            json_object_set_new(we, "kind",       json_integer(wr->kind));
            json_object_set_new(we, "visibility",
                                json_string(wr->visibility == STEP_VIS_PUBLIC ? "public" : "debug"));
            json_array_append_new(writes_arr, we);
        }
        json_object_set_new(step_entry, "writes", writes_arr);
        const char* params_str = pipeline_step_params_at(p, i);
        if (params_str) {
            json_error_t je;
            json_t* params_obj = json_loads(params_str, 0, &je);
            json_object_set_new(step_entry, "params", params_obj ? params_obj : json_null());
        } else {
            json_object_set_new(step_entry, "params", json_null());
        }
        json_array_append_new(steps_arr, step_entry);
    }
    session_enqueue_json(s, resp);
}

static void handle_subscribe_message(session_t* s, json_t* root) {
    const pipeline_t* p = session_last_pipeline(s);
    if (!p) {
        send_error(s, "NO_RUN", "SUBSCRIBE requires a prior RUN");
        return;
    }
    if (session_is_run_active(s)) {
        send_error(s, "RUN_BUSY", "cannot SUBSCRIBE while a run is active");
        return;
    }

    json_t* ja = json_object_get(root, "artifact");
    if (!ja || !json_is_string(ja)) {
        send_error(s, "BAD_SUBSCRIBE", "missing 'artifact' (string)");
        return;
    }
    const char* artifact = json_string_value(ja);

    step_visibility_t vis;
    if (pipeline_visibility_of(p, artifact, &vis) != 0) {
        send_error(s, "UNKNOWN_ARTIFACT", artifact);
        return;
    }

    // Replace subscribe[] with [artifact].
    char** sub = calloc(1, sizeof(char*));
    if (!sub) { send_error(s, "INTERNAL", "oom"); return; }
    sub[0] = strdup(artifact);
    if (!sub[0]) { free(sub); send_error(s, "INTERNAL", "oom"); return; }
    if (session_set_subscribe(s, sub, 1) != 0) {
        free(sub[0]); free(sub);
        send_error(s, "INTERNAL", "set subscribe failed");
        return;
    }

    // Deep-copy the saved input.
    char*  input_copy = NULL;
    size_t input_len  = 0;
    if (session_last_input_snapshot(s, &input_copy, &input_len) != 0) {
        send_error(s, "INTERNAL", "oom");
        return;
    }

    // Subscribe snapshot for the run thread (separate from the session
    // copy so the run thread frees its own at end-of-run).
    char**  sub_snap = NULL;
    size_t  n_sub    = 0;
    if (session_subscribe_snapshot(s, &sub_snap, &n_sub) != 0) {
        free(input_copy);
        send_error(s, "INTERNAL", "oom");
        return;
    }

    run_ctx_t* rc = calloc(1, sizeof(*rc));
    if (!rc) {
        free(input_copy);
        session_subscribe_snapshot_free(sub_snap, n_sub);
        send_error(s, "INTERNAL", "oom");
        return;
    }
    rc->server      = session_server(s);
    rc->session     = s;
    rc->pipeline    = p;
    rc->input_json  = input_copy;
    rc->input_len   = input_len;
    rc->subscribe   = sub_snap;
    rc->n_subscribe = n_sub;

    // Join the previous run thread (if any) BEFORE starting a new one.
    session_join_run_thread(s);

    // Acquire the run slot BEFORE pthread_create so the new thread's
    // cb_on_run_started sees late_subscribe_active=true and suppresses
    // the wire-level RUN_STARTED frame.  Doing this after pthread_create
    // (the prior shape) raced — the new thread could enter the callback
    // before the flag was set, leaking RUN_STARTED for late-SUBSCRIBE
    // (serve_full assertion under TSan, task #84).
    if (session_acquire_run_slot(s, /*late_subscribe=*/true) != 0) {
        free(rc->input_json);
        session_subscribe_snapshot_free(rc->subscribe, rc->n_subscribe);
        free(rc);
        send_error(s, "RUN_BUSY", "race with another run");
        return;
    }

    pthread_t tid;
    if (pthread_create(&tid, NULL, run_thread_main, rc) != 0) {
        // Roll back the slot we just acquired.
        session_end_run(s);
        free(rc->input_json);
        session_subscribe_snapshot_free(rc->subscribe, rc->n_subscribe);
        free(rc);
        send_error(s, "INTERNAL", "failed to spawn run thread");
        return;
    }
    session_attach_run_thread(s, tid);
}

static void handle_invalidate_message(session_t* s, json_t* root) {
    const pipeline_t* p = session_last_pipeline(s);
    if (!p) {
        send_error(s, "NO_RUN", "INVALIDATE requires a prior RUN");
        return;
    }
    json_t* ja = json_object_get(root, "artifact");
    if (!ja || !json_is_string(ja)) {
        send_error(s, "BAD_INVALIDATE", "missing 'artifact' (string)");
        return;
    }
    const char* artifact = json_string_value(ja);

    const step_def_t* producer = NULL;
    size_t n_steps = pipeline_step_count(p);
    for (size_t i = 0; i < n_steps && !producer; i++) {
        const step_def_t* def = pipeline_step_def_at(p, i);
        for (size_t w = 0; w < def->n_writes; w++) {
            if (strcmp(def->writes[w].name, artifact) == 0) { producer = def; break; }
        }
    }
    if (!producer) { send_error(s, "UNKNOWN_ARTIFACT", artifact); return; }

    char*  input = NULL;
    size_t input_len = 0;
    if (session_last_input_snapshot(s, &input, &input_len) != 0) {
        send_error(s, "INTERNAL", "no last input");
        return;
    }

    server_t* sv = session_server(s);
    hash_t identity;
    if (runner_compute_step_identity(sv->runner, p, input, input_len,
                                     producer->name, &identity) != 0) {
        free(input);
        send_error(s, "NOT_CACHED",
                   "cannot compute step identity (upstream not in cache)");
        return;
    }
    free(input);

    intent_map_t* im = runner_intent_map(sv->runner);
    hash_t prev;
    if (!im || intent_map_get(im, &identity, &prev) != 0) {
        send_error(s, "NOT_CACHED",
                   "step was not cached under the current input — nothing to invalidate");
        return;
    }
    intent_map_invalidate(im, &identity);

    char prev_hex[65]; hash_format_hex(&prev, prev_hex);
    json_t* resp = json_object();
    json_object_set_new(resp, "t",         json_string("INVALIDATED"));
    json_object_set_new(resp, "artifact",  json_string(artifact));
    json_object_set_new(resp, "step",      json_string(producer->name));
    json_object_set_new(resp, "previous_manifest_hash", json_string(prev_hex));
    session_enqueue_json(s, resp);
}

static void handle_list_runs_message(session_t* s, json_t* root) {
    server_t* sv = session_server(s);
    if (!sv->journal) {
        send_error(s, "NOT_AVAILABLE", "run journal disabled");
        return;
    }
    json_t* j_limit      = json_object_get(root, "limit");
    json_t* j_before     = json_object_get(root, "before");
    json_t* j_pipeline   = json_object_get(root, "pipeline");
    json_t* j_input_hash = json_object_get(root, "input_hash");
    json_t* j_success    = json_object_get(root, "success");

    uint32_t limit     = (j_limit  && json_is_integer(j_limit))
                            ? (uint32_t)json_integer_value(j_limit)  : 50;
    uint64_t before_us = (j_before && json_is_integer(j_before))
                            ? (uint64_t)json_integer_value(j_before) : 0;
    const char* pipeline   = (j_pipeline   && json_is_string(j_pipeline))
                                ? json_string_value(j_pipeline)   : NULL;
    const char* input_hash = (j_input_hash && json_is_string(j_input_hash))
                                ? json_string_value(j_input_hash) : NULL;
    int success_filter = -1;
    if (j_success && json_is_boolean(j_success)) {
        success_filter = json_is_true(j_success) ? 1 : 0;
    }

    run_journal_row_t* rows = NULL;
    size_t n = 0;
    if (run_journal_list(sv->journal, limit, before_us, pipeline,
                         input_hash, success_filter, &rows, &n) != 0) {
        send_error(s, "INTERNAL", "journal list failed");
        return;
    }

    json_t* resp = json_object();
    json_object_set_new(resp, "t", json_string("LIST_RUNS_RESPONSE"));
    json_t* arr = json_array();
    uint64_t oldest = UINT64_MAX;
    for (size_t i = 0; i < n; i++) {
        run_journal_row_t* r = &rows[i];
        json_t* o = json_object();
        json_object_set_new(o, "journal_id",   json_integer((json_int_t)r->journal_id));
        json_object_set_new(o, "run_id",       json_string(r->run_id ? r->run_id : ""));
        json_object_set_new(o, "pipeline",     json_string(r->pipeline_name ? r->pipeline_name : ""));
        if (r->test_case_label)
            json_object_set_new(o, "test_case_label", json_string(r->test_case_label));
        json_object_set_new(o, "input_hash",   json_string(r->input_hash ? r->input_hash : ""));
        json_object_set_new(o, "started_at",   json_integer((json_int_t)r->started_at_us));
        json_object_set_new(o, "duration_us",  json_integer((json_int_t)r->duration_us));
        json_object_set_new(o, "success",      json_boolean(r->success != 0));
        json_object_set_new(o, "n_executed",   json_integer((json_int_t)r->n_executed));
        json_object_set_new(o, "n_cache_hits", json_integer((json_int_t)r->n_cache_hits));
        json_error_t je;
        json_t* sub = r->subscribe_set_json
                        ? json_loads(r->subscribe_set_json, 0, &je) : json_array();
        if (!sub) sub = json_array();
        json_object_set_new(o, "subscribe_set", sub);
        json_array_append_new(arr, o);
        if (r->started_at_us < oldest) oldest = r->started_at_us;
    }
    json_object_set_new(resp, "runs", arr);
    if (n == limit) json_object_set_new(resp, "next_before", json_integer((json_int_t)oldest));
    else            json_object_set_new(resp, "next_before", json_null());
    session_enqueue_json(s, resp);
    run_journal_rows_free(rows, n);
}

static void handle_rehydrate_run_message(session_t* s, json_t* root) {
    server_t* sv = session_server(s);
    if (!sv->journal) { send_error(s, "NOT_AVAILABLE", "run journal disabled"); return; }
    json_t* j_jid = json_object_get(root, "journal_id");
    if (!j_jid || !json_is_integer(j_jid)) {
        send_error(s, "BAD_MSG", "REHYDRATE_RUN missing integer 'journal_id'");
        return;
    }
    int64_t journal_id = (int64_t)json_integer_value(j_jid);

    char** names = NULL; char** hashes = NULL; size_t n = 0;
    if (run_journal_get_produced(sv->journal, journal_id, &names, &hashes, &n) != 0) {
        send_error(s, "UNKNOWN_RUN", "no journal entry for journal_id");
        return;
    }

    char synth_run_id[32];
    snprintf(synth_run_id, sizeof(synth_run_id), "rehy_%lld", (long long)journal_id);

    size_t n_missing = 0;
    char** missing = calloc(n, sizeof(char*));

    for (size_t i = 0; i < n; i++) {
        if (!names[i] || !hashes[i]) continue;
        hash_t h;
        if (hash_from_hex64(hashes[i], &h) != 0) {
            if (missing) missing[n_missing++] = strdup(names[i]);
            continue;
        }
        store_handle_t sh;
        if (store_lookup(sv->store, &h, &sh) != 0) {
            if (missing) missing[n_missing++] = strdup(names[i]);
            continue;
        }
        session_pin_artifact(s, &h);

        char hex[65]; hash_format_hex(&h, hex);
        json_t* hdr = json_object();
        json_object_set_new(hdr, "t",          json_string("ARTIFACT_HEADER"));
        json_object_set_new(hdr, "run_id",     json_string(synth_run_id));
        json_object_set_new(hdr, "artifact",   json_string(names[i]));
        json_object_set_new(hdr, "size",       json_integer((json_int_t)sh.len));
        json_object_set_new(hdr, "hash",       json_string(hex));
        json_object_set_new(hdr, "rehydrated", json_true());
        json_object_set_new(hdr, "journal_id", json_integer((json_int_t)journal_id));
        (void)session_enqueue_json_with_binary(s, hdr, sh.bytes, sh.len);
        store_release(sv->store, &sh);
    }

    json_t* done = json_object();
    json_object_set_new(done, "t", json_string(n_missing > 0 ? "REHYDRATE_PARTIAL" : "REHYDRATE_COMPLETE"));
    json_object_set_new(done, "journal_id", json_integer((json_int_t)journal_id));
    json_object_set_new(done, "n_emitted",  json_integer((json_int_t)(n - n_missing)));
    if (n_missing > 0 && missing) {
        json_t* miss = json_array();
        for (size_t i = 0; i < n_missing; i++) {
            json_array_append_new(miss, json_string(missing[i]));
        }
        json_object_set_new(done, "missing", miss);
    }
    session_enqueue_json(s, done);

    if (missing) {
        for (size_t i = 0; i < n_missing; i++) free(missing[i]);
        free(missing);
    }
    run_journal_produced_free(names, hashes, n);
}

static void handle_cancel_message(session_t* s, json_t* root) {
    (void)root;
    char run_id[24];
    session_get_run_id(s, run_id, sizeof(run_id));
    if (!session_is_run_active(s) || run_id[0] == '\0') {
        send_error(s, "NO_ACTIVE_RUN", "nothing to cancel");
        return;
    }
    runner_cancel(session_server(s)->runner, run_id);
}

static void handle_run_message(session_t* s, json_t* root) {
    if (session_is_run_active(s)) {
        send_error(s, "RUN_BUSY", "a run is already active on this session");
        return;
    }
    server_t* sv = session_server(s);

    json_t* jp     = json_object_get(root, "pipeline");
    json_t* ji     = json_object_get(root, "input");
    json_t* jsub   = json_object_get(root, "subscribe");
    json_t* jlabel = json_object_get(root, "test_case_label");
    if (!jp || !json_is_string(jp))
        { send_error(s, "BAD_RUN", "missing 'pipeline'"); return; }
    if (!jsub || !json_is_array(jsub) || json_array_size(jsub) == 0)
        { send_error(s, "BAD_RUN", "missing 'subscribe' (non-empty array)"); return; }

    const char* pipeline_name = json_string_value(jp);
    const pipeline_t* resolved = sv->resolve(sv->resolve_ctx, pipeline_name);
    if (!resolved) { send_error(s, "UNKNOWN_PIPELINE", pipeline_name); return; }

    // Per-step params overrides.
    const pipeline_t* pipeline    = resolved;
    pipeline_t*       owned_clone = NULL;
    json_t*           jp_params   = json_object_get(root, "params");
    if (json_is_object(jp_params) && json_object_size(jp_params) > 0) {
        owned_clone = clone_pipeline_with_overrides(resolved, jp_params);
        if (!owned_clone) {
            send_error(s, "BAD_RUN", "failed to clone pipeline with overrides");
            return;
        }
        pipeline = owned_clone;
    }

    // Serialize input JSON — CANONICAL form (sorted keys + compact). The
    // pipeline-input bytes feed into every step's chain-hash via the
    // standard read-hashes inclusion, so any semantically-equal input
    // must hash to the same bytes or cache misses cascade. SORT_KEYS
    // makes object-key order irrelevant; COMPACT strips whitespace; the
    // parse-then-dump round-trip absorbs leading/trailing whitespace
    // from the UI textarea. Batch CLI applies the same canonicalisation
    // in main.c so both modes converge on identical bytes.
    char*  input_json = NULL;
    size_t input_len  = 0;
    if (ji) {
        input_json = json_dumps(ji, JSON_COMPACT | JSON_SORT_KEYS);
        if (!input_json) {
            if (owned_clone) pipeline_free(owned_clone);
            send_error(s, "BAD_RUN", "failed to serialize 'input'");
            return;
        }
        input_len = strlen(input_json);
    } else {
        input_json = strdup("{}");
        if (!input_json) {
            if (owned_clone) pipeline_free(owned_clone);
            send_error(s, "BAD_RUN", "oom");
            return;
        }
        input_len = 2;
    }

    // Build subscribe[] with per-entry visibility gating.
    size_t n_sub = json_array_size(jsub);
    char** sub = calloc(n_sub, sizeof(char*));
    if (!sub) {
        free(input_json);
        if (owned_clone) pipeline_free(owned_clone);
        send_error(s, "BAD_RUN", "oom");
        return;
    }
    for (size_t i = 0; i < n_sub; i++) {
        json_t* e = json_array_get(jsub, i);
        if (!json_is_string(e)) {
            for (size_t j = 0; j < i; j++) free(sub[j]);
            free(sub); free(input_json);
            if (owned_clone) pipeline_free(owned_clone);
            send_error(s, "BAD_RUN", "subscribe entries must be strings");
            return;
        }
        const char* name = json_string_value(e);
        step_visibility_t vis;
        if (pipeline_visibility_of(pipeline, name, &vis) != 0) {
            for (size_t j = 0; j < i; j++) free(sub[j]);
            free(sub); free(input_json);
            if (owned_clone) pipeline_free(owned_clone);
            send_error(s, "UNKNOWN_ARTIFACT", name);
            return;
        }
        sub[i] = strdup(name);
        if (!sub[i]) {
            for (size_t j = 0; j < i; j++) free(sub[j]);
            free(sub); free(input_json);
            if (owned_clone) pipeline_free(owned_clone);
            send_error(s, "BAD_RUN", "oom");
            return;
        }
    }

    // Install subscribe[] in the session (takes ownership).
    if (session_set_subscribe(s, sub, n_sub) != 0) {
        for (size_t i = 0; i < n_sub; i++) free(sub[i]);
        free(sub); free(input_json);
        if (owned_clone) pipeline_free(owned_clone);
        send_error(s, "BAD_RUN", "set subscribe failed");
        return;
    }

    // Snapshot subscribe[] for the run thread (independent of session
    // copy — run thread frees its own).
    char**  sub_snap = NULL;
    size_t  n_snap   = 0;
    if (session_subscribe_snapshot(s, &sub_snap, &n_snap) != 0) {
        free(input_json);
        if (owned_clone) pipeline_free(owned_clone);
        send_error(s, "BAD_RUN", "oom");
        return;
    }

    // Reset the journal accumulator and stamp the new identity.
    session_journal_reset(s);
    {
        hash_t ih = hash_bytes(input_json, input_len);
        char hex[65]; hash_format_hex(&ih, hex);
        const char* label = (jlabel && json_is_string(jlabel))
                                ? json_string_value(jlabel) : NULL;
        if (session_journal_set_identity(s, pipeline_name, input_json, input_len,
                                         hex, label) != 0) {
            // OOM: identity not set, journal write will be skipped at
            // run-complete.  Run still proceeds.
        }
    }

    // Save (pipeline, owned_clone, input) for late-SUBSCRIBE replay.
    // session_save_last_run takes ownership of owned_clone on success
    // (frees the previous one if any).  On -1, owned_clone is still
    // ours — leak rather than risk double-free; alternative is to
    // pipeline_free here and let the late-subscribe path fail.
    if (session_save_last_run(s, pipeline, owned_clone, input_json, input_len) != 0) {
        if (owned_clone) pipeline_free(owned_clone);
        free(input_json);
        session_subscribe_snapshot_free(sub_snap, n_snap);
        send_error(s, "INTERNAL", "save last run failed");
        return;
    }

    // Build run_ctx for the run thread.
    run_ctx_t* rc = calloc(1, sizeof(*rc));
    if (!rc) {
        free(input_json);
        session_subscribe_snapshot_free(sub_snap, n_snap);
        send_error(s, "BAD_RUN", "oom");
        return;
    }
    rc->server      = sv;
    rc->session     = s;
    rc->pipeline    = pipeline;
    rc->input_json  = input_json;
    rc->input_len   = input_len;
    rc->subscribe   = sub_snap;
    rc->n_subscribe = n_snap;

    // Join previous run thread (if any).
    session_join_run_thread(s);

    // Acquire the run slot BEFORE pthread_create — the new thread's
    // callbacks read run_active / late_subscribe_active off the session,
    // and acquiring after pthread_create raced (task #84). For a regular
    // RUN the late_subscribe flag value MATTERS too: if the previous run
    // was a late-subscribe (flag=true) and we acquire after spawning,
    // the new thread's cb_on_run_started can see the stale true and
    // silently drop the wire-level RUN_STARTED.
    if (session_acquire_run_slot(s, /*late_subscribe=*/false) != 0) {
        free(rc->input_json);
        session_subscribe_snapshot_free(rc->subscribe, rc->n_subscribe);
        free(rc);
        send_error(s, "RUN_BUSY", "race with another run");
        return;
    }

    pthread_t tid;
    if (pthread_create(&tid, NULL, run_thread_main, rc) != 0) {
        // Roll back the slot we just acquired.
        session_end_run(s);
        free(rc->input_json);
        session_subscribe_snapshot_free(rc->subscribe, rc->n_subscribe);
        free(rc);
        send_error(s, "INTERNAL", "failed to spawn run thread");
        return;
    }
    session_attach_run_thread(s, tid);
}

// RECONNECT — service-thread only; needs wsi + user pointer access.
static session_t* handle_reconnect_message(server_t* sv, struct lws* wsi,
                                           session_t* fresh, json_t* root) {
    json_t* jt = json_object_get(root, "session_token");
    if (!jt || !json_is_string(jt)) {
        send_error(fresh, "BAD_RECONNECT", "missing 'session_token'");
        return NULL;
    }
    const char* token = json_string_value(jt);
    session_t* preserved = session_mgr_reattach(sv->mgr, token, wsi);
    if (!preserved) {
        send_error(fresh, "UNKNOWN_TOKEN", "no session for token or still attached");
        return NULL;
    }
    // Tear down the ephemeral session created on ESTABLISHED.
    session_mgr_unregister(sv->mgr, fresh);
    session_destroy(fresh);
    json_t* obj = json_object();
    json_object_set_new(obj, "t",             json_string("RECONNECTED"));
    json_object_set_new(obj, "session_token", json_string(session_token(preserved)));
    session_enqueue_json(preserved, obj);
    return preserved;
}

// ---------------------------------------------------------------------------
// Constant-time token comparison (avoids leaking the token via timing).
// ---------------------------------------------------------------------------
static bool token_equal(const char* a, const char* b) {
    size_t la = strlen(a), lb = strlen(b);
    unsigned char diff = (unsigned char)(la != lb);
    size_t n = la < lb ? la : lb;
    for (size_t i = 0; i < n; i++) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

// ---------------------------------------------------------------------------
// libwebsockets protocol callback
// ---------------------------------------------------------------------------

static int protocol_cb(struct lws* wsi, enum lws_callback_reasons reason, void* user, void* in,
                       size_t len) {
    struct lws_context* ctx = wsi ? lws_get_context(wsi) : NULL;
    server_t* sv = ctx ? (server_t*)lws_context_user(ctx) : NULL;
    session_t* s = (user && (reason == LWS_CALLBACK_ESTABLISHED
                             || reason == LWS_CALLBACK_RECEIVE
                             || reason == LWS_CALLBACK_SERVER_WRITEABLE
                             || reason == LWS_CALLBACK_CLOSED
                             || reason == LWS_CALLBACK_WSI_DESTROY))
                       ? *(session_t**)user
                       : NULL;

    switch (reason) {
    case LWS_CALLBACK_ESTABLISHED: {
        if (!sv) return -1;
        s = session_new(sv, wsi);
        if (!s) return -1;
        *(session_t**)user = s;
        session_mgr_register(sv->mgr, s);
        LOG_EVENT(SUB_TRANSPORT, LOG_INFO, EV_LOG_STARTUP, LF_SESSION(session_token(s)));

        // Optional shared-token auth. With no token configured (the default,
        // localhost-only lab use) every connection is accepted. With a token
        // set, the client must connect with ?token=<value>; anything else
        // gets AUTH_FAILED and the connection is closed. Messages received
        // before authentication succeeds are rejected in RECEIVE below.
        if (sv->token[0]) {
            char tok_buf[sizeof(sv->token)] = {0};
            int tlen = lws_get_urlarg_by_name_safe(wsi, "token", tok_buf, sizeof(tok_buf));
            if (tlen <= 0 || !token_equal(tok_buf, sv->token)) {
                json_t* err = json_object();
                json_object_set_new(err, "t",    json_string("ERROR"));
                json_object_set_new(err, "code", json_string("AUTH_FAILED"));
                json_object_set_new(err, "msg",
                    json_string("invalid or missing token: connect with ?token=<value>"));
                session_enqueue_json(s, err);
                session_set_close_after_flush(s, true);
                lws_callback_on_writable(wsi);
                return 0;
            }
        }
        session_set_authed(s, true);

        json_t* obj = json_object();
        json_object_set_new(obj, "t",             json_string("SESSION_STARTED"));
        json_object_set_new(obj, "session_token", json_string(session_token(s)));
        session_enqueue_json(s, obj);
        break;
    }

    case LWS_CALLBACK_RECEIVE: {
        if (!s) break;
        if (!session_is_authed(s)) {
            // Connection failed auth at ESTABLISHED and is closing; never
            // dispatch anything it sends in the meantime.
            break;
        }
        if (lws_frame_is_binary(wsi)) {
            send_error(s, "UNSUPPORTED", "binary inbound frames not supported");
            break;
        }
        json_error_t je;
        json_t* root = json_loadb((const char*)in, len, 0, &je);
        if (!root) { send_error(s, "BAD_JSON", je.text); break; }
        json_t* jt = json_object_get(root, "t");
        if (jt && json_is_string(jt) && strcmp(json_string_value(jt), "RECONNECT") == 0) {
            session_t* swapped = handle_reconnect_message(sv, wsi, s, root);
            if (swapped) {
                *(session_t**)user = swapped;
                s = swapped;
            }
            json_decref(root);
            break;
        }
        if (!jt || !json_is_string(jt)) {
            send_error(s, "BAD_MSG", "missing 't' field");
            json_decref(root);
            break;
        }
        const char* t = json_string_value(jt);
        if      (strcmp(t, "RUN")           == 0) handle_run_message(s, root);
        else if (strcmp(t, "CANCEL")        == 0) handle_cancel_message(s, root);
        else if (strcmp(t, "CATALOG")       == 0) handle_catalog_message(s, root);
        else if (strcmp(t, "SUBSCRIBE")     == 0) handle_subscribe_message(s, root);
        else if (strcmp(t, "INVALIDATE")    == 0) handle_invalidate_message(s, root);
        else if (strcmp(t, "LIST_RUNS")     == 0) handle_list_runs_message(s, root);
        else if (strcmp(t, "REHYDRATE_RUN") == 0) handle_rehydrate_run_message(s, root);
        else                                       send_error(s, "UNKNOWN_MSG", t);
        json_decref(root);
        break;
    }

    case LWS_CALLBACK_SERVER_WRITEABLE: {
        if (!s) break;
        int rc = session_write_one_pending(s, wsi);
        if (rc == 1)        lws_callback_on_writable(wsi);
        else if (rc == -1)  return -1;   // lws_write failed
        else if (rc == -2)  return -1;   // close_after_flush
        break;
    }

    case LWS_CALLBACK_CLOSED:
    case LWS_CALLBACK_WSI_DESTROY: {
        if (!s) break;
        if (sv && sv->mgr) {
            // Cancel any active run BEFORE disconnect — holding a run
            // slot through grace ties up runner workers.  Reading the
            // run id requires the session lock (handled inside the
            // accessor); runner_cancel is safe under no lock.
            char run_id[24];
            session_get_run_id(s, run_id, sizeof(run_id));
            if (session_is_run_active(s) && run_id[0] != '\0') {
                runner_cancel(sv->runner, run_id);
            }
            // mgr decides grace-vs-destroy based on its grace_sec.
            session_mgr_disconnect(sv->mgr, s);
        }
        *(session_t**)user = NULL;
        break;
    }

    case LWS_CALLBACK_EVENT_WAIT_CANCELLED: {
        if (sv && sv->mgr) session_mgr_arm_pending_writes(sv->mgr);
        break;
    }

    // PLAN-PROTOCOL P8 (partial) — minimal HTTP handler for /healthz,
    // plus a fast-fail 426 for plain GETs to /ws (which previously
    // hung 15s waiting for a never-arriving WS upgrade, surfacing as
    // nginx 502 to anything not speaking the WS handshake).  The rest
    // of P8 (catalog/schema/static) stays deferred — nginx in front
    // handles static; CATALOG over WS already serves the lab UI.
    case LWS_CALLBACK_HTTP: {
        const char* uri = (const char*)in;
        const char* body = NULL;
        int status = HTTP_STATUS_NOT_FOUND;
        const char* ctype = "text/plain";
        if (uri && strcmp(uri, "/healthz") == 0) {
            status = HTTP_STATUS_OK;
            body   = "ok\n";
        } else if (uri && (strcmp(uri, "/ws") == 0 ||
                           strncmp(uri, "/ws?", 4) == 0 ||
                           strncmp(uri, "/ws/", 4) == 0)) {
            // 426 — endpoint exists but requires WS upgrade.  Faster
            // + clearer signal than the default 15s timeout-then-502.
            status = 426;
            body   = "use WebSocket\n";
        } else {
            body   = "not found\n";
        }
        size_t blen = strlen(body);
        unsigned char hdr[LWS_RECOMMENDED_MIN_HEADER_SPACE];
        unsigned char* p = hdr;
        unsigned char* end = hdr + sizeof(hdr) - 1;
        if (lws_add_http_common_headers(wsi, (unsigned int)status, ctype,
                                        (unsigned long)blen, &p, end))
            return 1;
        if (lws_finalize_write_http_header(wsi, hdr, &p, end))
            return 1;
        // Body write — needs LWS_PRE bytes of padding before the payload.
        unsigned char buf[LWS_PRE + 32];
        if (blen > sizeof(buf) - LWS_PRE) blen = sizeof(buf) - LWS_PRE;
        memcpy(buf + LWS_PRE, body, blen);
        if (lws_write(wsi, buf + LWS_PRE, blen, LWS_WRITE_HTTP_FINAL) != (int)blen)
            return 1;
        if (lws_http_transaction_completed(wsi))
            return -1;
        return 0;
    }

    default:
        break;
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Server lifecycle
// ---------------------------------------------------------------------------

static struct lws_protocols PROTOCOLS[] = {
    {
        .name                  = "dioramap-v1",
        .callback              = protocol_cb,
        .per_session_data_size = sizeof(session_t*),
        .rx_buffer_size        = 1 << 20,
        .id                    = 0,
        .user                  = NULL,
        .tx_packet_size        = 0,
    },
    {NULL, NULL, 0, 0, 0, NULL, 0},
};

server_t* server_new(const server_config_t* cfg) {
    if (!cfg || !cfg->runner || !cfg->store || !cfg->resolve || cfg->port <= 0)
        return NULL;
    server_t* sv = calloc(1, sizeof(*sv));
    if (!sv) return NULL;
    sv->runner    = cfg->runner;
    sv->store     = cfg->store;
    sv->journal   = cfg->journal;
    if (cfg->token && cfg->token[0])
        snprintf(sv->token, sizeof(sv->token), "%s", cfg->token);
    sv->resolve     = cfg->resolve;
    sv->list        = cfg->list;
    sv->resolve_ctx = cfg->resolve_ctx;
    atomic_init(&sv->should_stop, 0);

    sv->mgr = session_mgr_new(sv, cfg->grace_sec > 0.0 ? cfg->grace_sec : 600.0);
    if (!sv->mgr) { free(sv); return NULL; }

    lws_set_log_level(LLL_ERR | LLL_WARN, NULL);
    struct lws_context_creation_info info = {0};
    info.port      = cfg->port;
    info.iface     = cfg->iface;
    info.protocols = PROTOCOLS;
    info.gid       = (unsigned int)-1;
    info.uid       = (unsigned int)-1;
    info.user      = sv;
    info.options   = LWS_SERVER_OPTION_VALIDATE_UTF8 | LWS_SERVER_OPTION_DISABLE_IPV6;

    sv->ctx = lws_create_context(&info);
    if (!sv->ctx) { session_mgr_free(sv->mgr); free(sv); return NULL; }
    return sv;
}

int server_run(server_t* sv) {
    if (!sv || !sv->ctx) return -1;
    while (!atomic_load(&sv->should_stop)) {
        int rc = lws_service(sv->ctx, 0);
        if (rc < 0) break;
    }
    return 0;
}

void server_stop(server_t* sv) {
    if (!sv) return;
    atomic_store(&sv->should_stop, 1);
    if (sv->ctx) lws_cancel_service(sv->ctx);
}

void server_free(server_t* sv) {
    if (!sv) return;
    // Tear down lws first — fires CLOSED for every still-attached wsi,
    // which detaches sessions to the manager (grace).  Then free the
    // manager, which joins the janitor and destroys remaining sessions.
    if (sv->ctx) {
        lws_context_destroy(sv->ctx);
        sv->ctx = NULL;
    }
    if (sv->mgr) {
        session_mgr_free(sv->mgr);
        sv->mgr = NULL;
    }
    free(sv);
}

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// session_mgr_t implementation.  Owns the sessions[] table, the per-
// server sessions mutex, and the optional grace-period janitor.
//
// Lock-ordering invariant (CONCURRENCY.md §12.3):
//   sessions_lock → session_t::lock      (iteration only)
//
// Acquired ONLY via the strict-LIFO pattern:
//     lock(sessions_lock); lock(s->lock); ...; unlock(s->lock); unlock(sessions_lock);
// Lock hand-off (lock A, lock B, unlock A, unlock B) is forbidden.

#define _POSIX_C_SOURCE 200809L

#include "v3/session_mgr.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>

#include "server_internal.h"
#include "v3/session.h"
#include "v3/thread_safety.h"

// ─── struct ────────────────────────────────────────────────────────────

struct session_mgr {
    struct server*      server;          // immutable
    double              grace_sec;        // immutable post-construction

    mutex_t             sessions_lock;
    session_t**         sessions     GUARDED_BY(sessions_lock);
    size_t              n_sessions   GUARDED_BY(sessions_lock);
    size_t              cap_sessions GUARDED_BY(sessions_lock);

    // Janitor lifecycle.  `janitor_running` and `janitor_thread` are
    // set once at startup and read at teardown — no concurrent
    // mutation, so they're not GUARDED_BY.  `janitor_stop` is the
    // shutdown flag, set + signalled under janitor_lock.
    pthread_t           janitor_thread;
    bool                janitor_running;
    mutex_t             janitor_lock;
    pthread_cond_t      janitor_cv;
    bool                janitor_stop GUARDED_BY(janitor_lock);
};

// ─── helpers ───────────────────────────────────────────────────────────

static uint64_t monotonic_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

// ─── janitor ───────────────────────────────────────────────────────────

static void* janitor_thread_main(void* vmgr) {
    session_mgr_t* mgr = vmgr;
    for (;;) {
        // Wait up to 1s for either shutdown or a disconnect signal.
        // pthread_cond_timedwait is the canonical lock-drop-relock
        // pattern (CONCURRENCY.md §3 invariant 9): the kernel atomically
        // releases the mutex while sleeping and reacquires on wake, so
        // there is no missed-wake race.
        MUTEX_LOCK(&mgr->janitor_lock);
        if (mgr->janitor_stop) {
            MUTEX_UNLOCK(&mgr->janitor_lock);
            return NULL;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        pthread_cond_timedwait(&mgr->janitor_cv, &mgr->janitor_lock, &ts);
        bool stop = mgr->janitor_stop;
        MUTEX_UNLOCK(&mgr->janitor_lock);
        if (stop) return NULL;

        uint64_t now      = monotonic_us();
        uint64_t grace_us = (uint64_t)(mgr->grace_sec * 1e6);

        // Collect victims under sessions_lock (briefly), then destroy
        // outside any lock.  session_destroy joins a possibly long-
        // running run thread — never do that under our table lock.
        // Cap per-pass at 16 to keep the lock-held window bounded; any
        // remaining expirees get picked up next tick.
        session_t* victims[16];
        size_t n_victims = 0;
        MUTEX_LOCK(&mgr->sessions_lock);
        for (size_t i = 0; i < mgr->n_sessions && n_victims < 16; ) {
            session_t* s = mgr->sessions[i];
            // session_is_expired internally takes s->lock — strict LIFO
            // (sessions_lock → s->lock → unlock s->lock → unlock sessions_lock).
            if (session_is_expired(s, now, grace_us)) {
                victims[n_victims++] = s;
                mgr->sessions[i] = mgr->sessions[mgr->n_sessions - 1];
                mgr->n_sessions--;
            } else {
                i++;
            }
        }
        MUTEX_UNLOCK(&mgr->sessions_lock);

        for (size_t i = 0; i < n_victims; i++) {
            session_destroy(victims[i]);
        }
    }
}

// ─── lifecycle ─────────────────────────────────────────────────────────

session_mgr_t* session_mgr_new(struct server* sv, double grace_sec) {
    if (!sv) return NULL;
    session_mgr_t* mgr = calloc(1, sizeof(*mgr));
    if (!mgr) return NULL;
    mgr->server    = sv;
    mgr->grace_sec = grace_sec > 0.0 ? grace_sec : 0.0;
    // Frama-C audit follow-up: pthread init rc checks with rollback chain.
    if (pthread_mutex_init(&mgr->sessions_lock, NULL) != 0) {
        free(mgr);
        return NULL;
    }
    if (pthread_mutex_init(&mgr->janitor_lock, NULL) != 0) {
        pthread_mutex_destroy(&mgr->sessions_lock);
        free(mgr);
        return NULL;
    }
    if (pthread_cond_init(&mgr->janitor_cv, NULL) != 0) {
        pthread_mutex_destroy(&mgr->janitor_lock);
        pthread_mutex_destroy(&mgr->sessions_lock);
        free(mgr);
        return NULL;
    }
    if (mgr->grace_sec > 0.0) {
        if (pthread_create(&mgr->janitor_thread, NULL,
                           janitor_thread_main, mgr) == 0) {
            mgr->janitor_running = true;
        }
        // If pthread_create failed we still return a usable manager —
        // sessions just get torn down immediately on disconnect by the
        // server's CLOSED handler.
    }
    return mgr;
}

void session_mgr_free(session_mgr_t* mgr) {
    if (!mgr) return;

    // Stop the janitor first so it doesn't race the table teardown.
    if (mgr->janitor_running) {
        MUTEX_LOCK(&mgr->janitor_lock);
        mgr->janitor_stop = true;
        pthread_cond_signal(&mgr->janitor_cv);
        MUTEX_UNLOCK(&mgr->janitor_lock);
        pthread_join(mgr->janitor_thread, NULL);
    }

    // Snapshot the table under the lock, then destroy outside.  Same
    // reasoning as the janitor: session_destroy joins run threads.
    MUTEX_LOCK(&mgr->sessions_lock);
    session_t** victims = mgr->sessions;
    size_t      n       = mgr->n_sessions;
    mgr->sessions     = NULL;
    mgr->n_sessions   = 0;
    mgr->cap_sessions = 0;
    MUTEX_UNLOCK(&mgr->sessions_lock);

    for (size_t i = 0; i < n; i++) {
        session_destroy(victims[i]);
    }
    free(victims);

    pthread_cond_destroy (&mgr->janitor_cv);
    pthread_mutex_destroy(&mgr->janitor_lock);
    pthread_mutex_destroy(&mgr->sessions_lock);
    free(mgr);
}

// ─── table ops ─────────────────────────────────────────────────────────

void session_mgr_register(session_mgr_t* mgr, session_t* s) {
    if (!mgr || !s) return;
    MUTEX_LOCK(&mgr->sessions_lock);
    if (mgr->n_sessions == mgr->cap_sessions) {
        size_t nc = mgr->cap_sessions ? mgr->cap_sessions * 2 : 8;
        // ALLOC: realloc the session pointer table.  On failure, drop
        // the registration silently — the session is still alive but
        // won't be reachable via reattach/janitor.  The CLOSED handler
        // will free it eventually.  Better than aborting.
        session_t** na = realloc(mgr->sessions, nc * sizeof(*na));
        if (!na) {
            MUTEX_UNLOCK(&mgr->sessions_lock);
            return;
        }
        mgr->sessions     = na;
        mgr->cap_sessions = nc;
    }
    mgr->sessions[mgr->n_sessions++] = s;
    MUTEX_UNLOCK(&mgr->sessions_lock);
}

void session_mgr_unregister(session_mgr_t* mgr, session_t* s) {
    if (!mgr || !s) return;
    MUTEX_LOCK(&mgr->sessions_lock);
    for (size_t i = 0; i < mgr->n_sessions; i++) {
        if (mgr->sessions[i] == s) {
            mgr->sessions[i] = mgr->sessions[mgr->n_sessions - 1];
            mgr->n_sessions--;
            break;
        }
    }
    MUTEX_UNLOCK(&mgr->sessions_lock);
}

session_t* session_mgr_reattach(session_mgr_t* mgr, const char* token,
                                struct lws* new_wsi) {
    if (!mgr || !token) return NULL;

    // Strict LIFO: sessions_lock → s->lock (via session_token_eq +
    // session_is_disconnected + session_set_wsi).  Each per-session
    // call takes s->lock briefly and releases before the next.
    MUTEX_LOCK(&mgr->sessions_lock);
    session_t* found = NULL;
    for (size_t i = 0; i < mgr->n_sessions; i++) {
        session_t* s = mgr->sessions[i];
        if (!session_token_eq(s, token)) continue;
        if (session_is_disconnected(s)) {
            // session_set_wsi clears `disconnected` and drains the
            // out-queue from the previous attachment.
            session_set_wsi(s, new_wsi);
            found = s;
        }
        break;   // matched (or not): only one session per token
    }
    MUTEX_UNLOCK(&mgr->sessions_lock);
    return found;
}

void session_mgr_arm_pending_writes(session_mgr_t* mgr) {
    if (!mgr) return;
    MUTEX_LOCK(&mgr->sessions_lock);
    for (size_t i = 0; i < mgr->n_sessions; i++) {
        session_t* s = mgr->sessions[i];
        // session_has_pending and session_wsi_for_arm each take
        // s->lock briefly and release.  Strict LIFO with sessions_lock.
        // No I/O, no alloc here — short scan.
        if (session_has_pending(s)) {
            struct lws* wsi = session_wsi_for_arm(s);
            if (wsi) lws_callback_on_writable(wsi);
        }
    }
    MUTEX_UNLOCK(&mgr->sessions_lock);
}

void session_mgr_wake_janitor(session_mgr_t* mgr) {
    if (!mgr || !mgr->janitor_running) return;
    MUTEX_LOCK(&mgr->janitor_lock);
    pthread_cond_signal(&mgr->janitor_cv);
    MUTEX_UNLOCK(&mgr->janitor_lock);
}

void session_mgr_disconnect(session_mgr_t* mgr, session_t* s) {
    if (!mgr || !s) return;
    if (mgr->grace_sec > 0.0) {
        // Grace path: retain in table for reattach.  No nested locks
        // here — session_mark_disconnected takes only s->lock;
        // session_mgr_wake_janitor takes only janitor_lock.
        session_mark_disconnected(s);
        session_mgr_wake_janitor(mgr);
    } else {
        // No grace: unregister + destroy now.
        session_mgr_unregister(mgr, s);
        session_destroy(s);
    }
}

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Frama-C audit harness for `dioramap_log`.
//
// Eva analyzes only the calling thread's path. The drain thread spawned
// by log_init() is opaque (pthread_create is a stdlib stub) — concurrent
// race analysis is TSan's job. We exercise the producer-side state
// machine, the synchronous fallback (before log_init / after shutdown),
// and the sink-swap path used by tests.

#include "v3/log.h"
#include "v3/log_events.h"

#include <__fc_builtin.h>
#include <stddef.h>
#include <stdint.h>

int main(void) {
    // ---- synchronous-fallback path (before log_init) ----
    log_emit_(SUB_LOG, LOG_INFO, EV_LOG_STARTUP, NULL, 0);

    log_field_t fs1[] = {
        LF_INT("a", 42),
        LF_UINT("b", 1u << 31),
        LF_FLOAT("c", 3.14),
        LF_STR("d", "hello"),
        LF_BOOL("e", true),
    };
    log_emit_(SUB_LOG, LOG_DEBUG, EV_LOG_STARTUP, fs1, 5);

    // ---- initialized path ----
    log_init();
    log_set_level(LOG_INFO);
    if (log_get_level() != LOG_INFO) { /* unreachable */ }

    log_field_t fs2[] = {
        LF_STR("run_id", "abc123"),
        LF_INT("count", Frama_C_int_interval(-100, 100)),
    };
    log_emit_(SUB_RUNNER, LOG_WARN, EV_RUNNER_RUN_STARTED, fs2, 2);

    // No-fields macro form
    LOG_EVENT0(SUB_RUNNER, LOG_ERROR, EV_RUNNER_RUN_FAILED);

    // Below-threshold event should be filtered by the macro guard.
    LOG_EVENT(SUB_RUNNER, LOG_DEBUG, EV_RUNNER_STEP_STARTED, LF_INT("x", 1));

    log_flush();

    // ---- sink swap (test harness usage) ----
    log_sink_t* sink = log_sink_new_buffer();
    log_sink_t* prev = log_sink_swap_for_testing(sink);
    log_emit_(SUB_LOG, LOG_INFO, EV_LOG_STARTUP, NULL, 0);
    log_flush();
    const char* buf = log_sink_buffer_contents(sink);
    (void)buf;
    (void)log_sink_swap_for_testing(prev);
    log_sink_free(sink);

    log_shutdown();
    return 0;
}

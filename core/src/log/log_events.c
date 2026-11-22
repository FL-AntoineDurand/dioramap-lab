// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#include "v3/log_events.h"

static const char* const SUBSYSTEM_NAMES[SUB__COUNT] = {
    [SUB_LOG] = "log",
    [SUB_RUNNER] = "runner",
    [SUB_STORE] = "store",
    [SUB_SESSION] = "session",
    [SUB_TRANSPORT] = "transport",
    [SUB_FETCH] = "fetch",
    [SUB_CONTROLPLANE] = "controlplane",
    [SUB_CUDA] = "cuda",
    [SUB_STEP] = "step",
};

static const char* const EVENT_KIND_NAMES[EV__COUNT] = {
    [EV_LOG_STARTUP] = "startup",
    [EV_LOG_SHUTDOWN] = "shutdown",
    [EV_LOG_DROPPED] = "dropped",

    [EV_STORE_OPENED] = "opened",
    [EV_STORE_CLOSED] = "closed",
    [EV_STORE_SCAN_COMPLETE] = "scan_complete",
    [EV_STORE_WRITE] = "write",
    [EV_STORE_EVICT] = "evict",
    [EV_STORE_JANITOR_CYCLE] = "janitor_cycle",
    [EV_STORE_DEMOTE] = "demote",
    [EV_INTENT_MAP_OPENED] = "intent_map_opened",
    [EV_INTENT_MAP_COMPACTED] = "intent_map_compacted",
    [EV_INTENT_MAP_TAIL_TRUNC] = "intent_map_tail_truncated",

    [EV_RUNNER_DAG_PLAN] = "dag_plan",
    [EV_RUNNER_RUN_STARTED] = "run_started",
    [EV_RUNNER_RUN_COMPLETE] = "run_complete",
    [EV_RUNNER_RUN_FAILED] = "run_failed",
    [EV_RUNNER_STEP_STARTED] = "step_started",
    [EV_RUNNER_STEP_COMPLETE] = "step_complete",
    [EV_RUNNER_STEP_FAILED] = "step_failed",
    [EV_RUNNER_CACHE_DESYNC] = "cache_desync",

    [EV_FETCH_FAILED] = "fetch_failed",
};

const char* subsystem_name(subsystem_t s) {
    if ((int)s < 0 || s >= SUB__COUNT) {
        return "unknown";
    }
    const char* name = SUBSYSTEM_NAMES[s];
    return name ? name : "unknown";
}

const char* event_kind_name(event_kind_t k) {
    if ((int)k < 0 || k >= EV__COUNT) {
        return "unknown";
    }
    const char* name = EVENT_KIND_NAMES[k];
    return name ? name : "unknown";
}

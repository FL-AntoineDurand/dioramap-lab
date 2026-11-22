// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#ifndef V3_LOG_EVENTS_H
#define V3_LOG_EVENTS_H

// Subsystems that emit log events. Fixed set; extend only by explicit decision.
typedef enum subsystem {
    SUB_LOG = 0,
    SUB_RUNNER,
    SUB_STORE,
    SUB_SESSION,
    SUB_TRANSPORT,
    SUB_FETCH,
    SUB_CONTROLPLANE,
    SUB_CUDA,
    SUB_STEP,
    SUB__COUNT,
} subsystem_t;

// Event kinds. Grow as subsystems land. Names in log_events.c must stay in sync.
// New entries go to the END of their subsystem's block (stability of numeric value
// is not required for log consumption since we serialize by name).
typedef enum event_kind {
    // log subsystem (A2)
    EV_LOG_STARTUP = 0,
    EV_LOG_SHUTDOWN,
    EV_LOG_DROPPED, // reserved for backpressure accounting (L8)

    // store subsystem (A5 + B3.5 hot tier)
    EV_STORE_OPENED,
    EV_STORE_CLOSED,
    EV_STORE_SCAN_COMPLETE,
    EV_STORE_WRITE,
    EV_STORE_EVICT,
    EV_STORE_JANITOR_CYCLE,
    EV_STORE_DEMOTE,           // hot -> warm tier demotion
    EV_INTENT_MAP_OPENED,      // intent map startup, log scan complete
    EV_INTENT_MAP_COMPACTED,   // compaction rewrote the log
    EV_INTENT_MAP_TAIL_TRUNC,  // partial trailing record discarded at startup

    // runner subsystem (A6)
    EV_RUNNER_DAG_PLAN,
    EV_RUNNER_RUN_STARTED,
    EV_RUNNER_RUN_COMPLETE,
    EV_RUNNER_RUN_FAILED,
    EV_RUNNER_STEP_STARTED,
    EV_RUNNER_STEP_COMPLETE,
    EV_RUNNER_STEP_FAILED,

    // F3 (D6.5 follow-up): in-run cache desync — a step's read returned
    // NULL even though the upstream producer reported cache_hit=true.
    // Runner recovers by invalidating the producer's intent entry and
    // re-executing it; this event fires once per recovery attempt with
    // the affected artifact + producer step name.
    EV_RUNNER_CACHE_DESYNC,

    // fetch subsystem (PLAN-FETCHERS — emitted from steps/fetchers/* on
    // upstream HTTP/transport failure so the structured log carries the
    // status code, error code, and a snippet of the response body).
    EV_FETCH_FAILED,

    // Other subsystems register their event kinds here as they land.

    EV__COUNT,
} event_kind_t;

const char* subsystem_name(subsystem_t s);
const char* event_kind_name(event_kind_t k);

#endif

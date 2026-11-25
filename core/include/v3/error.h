// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#ifndef V3_ERROR_H
#define V3_ERROR_H

// Structured failure reasons (docs/ERRORS.md).
//
// A step's exec fn still returns 0 / -1. What this module adds is a way for
// the code that fails to say WHY, so the reason travels end to end:
//
//   step_fail() / fetch layer ──▶ thread-local sink ──▶ runner
//        ──▶ STEP_FAILED log event + RUN_FAILED wire message ──▶ SDK onError / UI
//
// Two independent slots are kept per step execution:
//
//   - `step`     — set by the step itself via step_fail() (v3/step.h). The
//                  step knows the domain meaning ("no LiDAR tile published
//                  here").
//   - `upstream` — set automatically by the fetch layer on any failed HTTP
//                  request (URL, status, body excerpt). The step does not
//                  have to forward it.
//
// The runner combines them when the step returns -1: the step's reason wins
// for code + message, the upstream evidence is attached as detail.upstream.
// A step that returns a bare -1 still gets a usable report: the upstream
// failure if there was one, else INTERNAL "<step> failed without a reason".
//
// The sink is thread-local and bound by the runner around each step exec.
// Steps run their exec on the calling worker thread and the fetch layer is
// synchronous on the caller's thread, so both land in the right sink. Code
// running with no bound sink (tests, tools) records nothing — every call is
// a safe no-op.

#include <stdarg.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum v3_error_code {
    V3_ERR_NONE = 0,
    V3_ERR_INTERNAL,             // bug or unexpected state in the engine
    V3_ERR_NO_DATA,              // the place has no data for this input (normal outcome)
    V3_ERR_INVALID_INPUT,        // the request itself is wrong (bad coords, params)
    V3_ERR_UPSTREAM_HTTP,        // an external service answered with an HTTP error
    V3_ERR_UPSTREAM_UNREACHABLE, // DNS / TLS / connection failure to an external service
    V3_ERR_UPSTREAM_FORMAT,      // an external service answered 2xx but with unusable content
    V3_ERR_TIMEOUT,              // an external service or the step took too long
    V3_ERR_CANCELLED,            // the run was cancelled (client request or disconnect)
    V3_ERR__COUNT,
} v3_error_code_t;

// Stable wire name ("NO_DATA", "UPSTREAM_HTTP", ...). Never NULL.
const char* v3_error_code_name(v3_error_code_t code);

#define V3_ERROR_MSG_MAX 512

typedef struct v3_error_slot {
    v3_error_code_t code;          // V3_ERR_NONE = slot empty
    char message[V3_ERROR_MSG_MAX];
    char* detail_json;             // owned; a JSON object as text, or NULL
} v3_error_slot_t;

typedef struct v3_error_sink {
    v3_error_slot_t step;
    v3_error_slot_t upstream;
} v3_error_sink_t;

// Bind `sink` as the current thread's sink; returns the previous binding so
// callers can restore it. Passing NULL unbinds.
v3_error_sink_t* v3_error_bind(v3_error_sink_t* sink);

// Current thread's sink (NULL when unbound).
v3_error_sink_t* v3_error_current(void);

// Record the step-level reason. The last call wins for code and message; a
// call with detail_json == NULL keeps the detail of an earlier call.
// `detail_json` is copied. No-op without a bound sink.
void v3_error_set_step(v3_error_code_t code, const char* detail_json, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));
void v3_error_set_stepv(v3_error_code_t code, const char* detail_json, const char* fmt,
                        va_list ap);

// Record upstream evidence (fetch layer). Last call wins. `detail_json` is
// copied. No-op without a bound sink.
void v3_error_set_upstream(v3_error_code_t code, const char* detail_json, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

// Free owned detail strings and reset both slots to empty.
void v3_error_sink_clear(v3_error_sink_t* sink);

// Final, combined failure report handed to runner callbacks.
typedef struct v3_failure {
    v3_error_code_t code;
    const char* code_name;     // v3_error_code_name(code)
    const char* message;       // human-readable, one line
    const char* step;          // friendly step name, NULL for run-level failures
    const char* step_token;    // name as registered (obfuscated token in prod), may == step
    const char* detail_json;   // JSON object text or NULL
} v3_failure_t;

#ifdef __cplusplus
}
#endif
#endif

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#ifndef V3_LOG_H
#define V3_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>   // ACSL valid_read_string

#include "v3/log_events.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum log_level {
    LOG_DEBUG = 0,
    LOG_INFO = 1,
    LOG_WARN = 2,
    LOG_ERROR = 3,
    LOG_FATAL = 4,
} log_level_t;

typedef enum log_value_type {
    LV_INT = 0,
    LV_UINT,
    LV_FLOAT,
    LV_STR,
    LV_BOOL,
} log_value_type_t;

typedef struct log_value {
    log_value_type_t type;
    union {
        int64_t i;
        uint64_t u;
        double f;
        const char* s;
        bool b;
    };
} log_value_t;

typedef struct log_field {
    const char* key;
    log_value_t value;
} log_field_t;

// ------------------------------------------------------------
// Lifecycle. Call log_init() at process start, log_shutdown() at exit.
// log_init() reads DIORAMAP_LOG_LEVEL env var; default is "info".
// ------------------------------------------------------------

/*@ assigns \nothing; */
void log_init(void);
/*@ assigns \nothing; */
void log_shutdown(void);

// Block until all events submitted before the call have been written to the sink.
// No-op if log_init() has not been called (emit runs synchronously in that case).
/*@ assigns \nothing; */
void log_flush(void);

/*@ assigns \nothing; */
void log_set_level(log_level_t level);
/*@ assigns \nothing; */
log_level_t log_get_level(void);

// ------------------------------------------------------------
// Low-level emit. Prefer the LOG_EVENT macro below.
// ------------------------------------------------------------

/*@ requires n_fields == 0 || \valid_read(fields + (0 .. n_fields - 1));
    assigns \nothing;
*/
void log_emit_(subsystem_t subsystem, log_level_t level, event_kind_t event_kind,
               const log_field_t* fields, size_t n_fields);

// ------------------------------------------------------------
// Ergonomic macros
// ------------------------------------------------------------

#define LOG_EVENT(SUBSYS, LEVEL, KIND, ...)                                                        \
    do {                                                                                           \
        if ((LEVEL) >= log_get_level()) {                                                          \
            const log_field_t _fs[] = {__VA_ARGS__};                                               \
            log_emit_((SUBSYS), (LEVEL), (KIND), _fs, sizeof(_fs) / sizeof(_fs[0]));               \
        }                                                                                          \
    } while (0)

#define LOG_EVENT0(SUBSYS, LEVEL, KIND)                                                            \
    do {                                                                                           \
        if ((LEVEL) >= log_get_level()) {                                                          \
            log_emit_((SUBSYS), (LEVEL), (KIND), NULL, 0);                                         \
        }                                                                                          \
    } while (0)

// Field constructors. C11 compound literals with anonymous-union designated init.
#define LF_INT(K, V) ((log_field_t){.key = (K), .value = {.type = LV_INT, .i = (int64_t)(V)}})
#define LF_UINT(K, V) ((log_field_t){.key = (K), .value = {.type = LV_UINT, .u = (uint64_t)(V)}})
#define LF_FLOAT(K, V) ((log_field_t){.key = (K), .value = {.type = LV_FLOAT, .f = (double)(V)}})
#define LF_STR(K, V) ((log_field_t){.key = (K), .value = {.type = LV_STR, .s = (V)}})
#define LF_BOOL(K, V) ((log_field_t){.key = (K), .value = {.type = LV_BOOL, .b = (V)}})

// Conventional correlation-field shortcuts.
#define LF_RUN_ID(V) LF_STR("run_id", (V))
#define LF_SESSION(V) LF_STR("session", (V))
#define LF_STEP(V) LF_STR("step", (V))
#define LF_ARTIFACT(V) LF_STR("artifact", (V))

// ------------------------------------------------------------
// Sink swap for testing. In prod, sink is stdout.
// ------------------------------------------------------------

typedef struct log_sink log_sink_t;

/*@ assigns \nothing;
    ensures \result == \null || \valid(\result);
*/
log_sink_t* log_sink_new_buffer(void);

/*@ requires sink == \null || \valid(sink);
    assigns *sink;
*/
void log_sink_free(log_sink_t* sink);

/*@ requires sink == \null || \valid(sink);
    assigns \nothing;
    ensures \result == \null || valid_read_string(\result);
*/
const char* log_sink_buffer_contents(log_sink_t* sink);

// Returns the previous sink so tests can restore it. Pass NULL to reset to stdout.
/*@ requires new_sink == \null || \valid(new_sink);
    assigns \nothing;
    ensures \result == \null || \valid(\result);
*/
log_sink_t* log_sink_swap_for_testing(log_sink_t* new_sink);

#ifdef __cplusplus
}
#endif

#endif

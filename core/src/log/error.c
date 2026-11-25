// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Structured failure reasons — thread-local sink. See v3/error.h.

#define _POSIX_C_SOURCE 200809L

#include "v3/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Thread_local v3_error_sink_t* tl_sink = NULL;

static const char* const k_names[V3_ERR__COUNT] = {
    [V3_ERR_NONE] = "NONE",
    [V3_ERR_INTERNAL] = "INTERNAL",
    [V3_ERR_NO_DATA] = "NO_DATA",
    [V3_ERR_INVALID_INPUT] = "INVALID_INPUT",
    [V3_ERR_UPSTREAM_HTTP] = "UPSTREAM_HTTP",
    [V3_ERR_UPSTREAM_UNREACHABLE] = "UPSTREAM_UNREACHABLE",
    [V3_ERR_UPSTREAM_FORMAT] = "UPSTREAM_FORMAT",
    [V3_ERR_TIMEOUT] = "TIMEOUT",
    [V3_ERR_CANCELLED] = "CANCELLED",
};

const char* v3_error_code_name(v3_error_code_t code) {
    if ((int)code < 0 || code >= V3_ERR__COUNT || !k_names[code]) {
        return "INTERNAL";
    }
    return k_names[code];
}

v3_error_sink_t* v3_error_bind(v3_error_sink_t* sink) {
    v3_error_sink_t* prev = tl_sink;
    tl_sink = sink;
    return prev;
}

v3_error_sink_t* v3_error_current(void) {
    return tl_sink;
}

static void slot_set_detail(v3_error_slot_t* slot, const char* detail_json) {
    if (!detail_json) {
        return;
    }
    char* dup = strdup(detail_json);
    if (!dup) {
        return; // OOM: keep the previous detail rather than lose the report
    }
    free(slot->detail_json);
    slot->detail_json = dup;
}

void v3_error_set_stepv(v3_error_code_t code, const char* detail_json, const char* fmt,
                        va_list ap) {
    v3_error_sink_t* s = tl_sink;
    if (!s) {
        return;
    }
    if (code == V3_ERR_NONE) {
        code = V3_ERR_INTERNAL;
    }
    s->step.code = code;
    if (fmt) {
        vsnprintf(s->step.message, sizeof(s->step.message), fmt, ap);
    }
    slot_set_detail(&s->step, detail_json);
}

void v3_error_set_step(v3_error_code_t code, const char* detail_json, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    v3_error_set_stepv(code, detail_json, fmt, ap);
    va_end(ap);
}

void v3_error_set_upstream(v3_error_code_t code, const char* detail_json, const char* fmt, ...) {
    v3_error_sink_t* s = tl_sink;
    if (!s) {
        return;
    }
    s->upstream.code = code == V3_ERR_NONE ? V3_ERR_UPSTREAM_HTTP : code;
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(s->upstream.message, sizeof(s->upstream.message), fmt, ap);
        va_end(ap);
    }
    free(s->upstream.detail_json);
    s->upstream.detail_json = NULL;
    slot_set_detail(&s->upstream, detail_json);
}

void v3_error_sink_clear(v3_error_sink_t* sink) {
    if (!sink) {
        return;
    }
    free(sink->step.detail_json);
    free(sink->upstream.detail_json);
    memset(sink, 0, sizeof(*sink));
}

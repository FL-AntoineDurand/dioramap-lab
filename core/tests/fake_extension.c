// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Test double for the processor extension seam (v3/extension.h), linked
// into dioramap_processor_fakeext and driven by serve_extension.py.
//
//   ?key=full-key-0001    → accepted, V3_ACCESS_FULL, tier "internal"
//   ?key=public-key-0001  → accepted, V3_ACCESS_PUBLIC_ONLY, tier "public"
//   no key                → AUTH_REQUIRED
//   any other key         → AUTH_FAILED "unknown_key"
//   Origin: http://evil.example → AUTH_FAILED "origin_not_allowed"
//
// Every finished run is appended as one JSON line to $FAKE_EXT_REPORTS;
// shutdown appends {"event":"shutdown"}. init() refuses to start when
// FAKE_EXT_REPORTS is unset.

#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jansson.h>

#include "v3/extension.h"

typedef struct {
    FILE* out;
    pthread_mutex_t lock;
} fake_state_t;

static void* fake_init(const v3_ext_env_t* env) {
    const char* path = getenv("FAKE_EXT_REPORTS");
    if (!path || !path[0] || !env || !env->cache_dir) {
        fprintf(stderr, "fake-extension: FAKE_EXT_REPORTS is not set\n");
        return NULL;
    }
    fake_state_t* st = calloc(1, sizeof(*st));
    if (!st) return NULL;
    st->out = fopen(path, "a");
    if (!st->out) {
        free(st);
        return NULL;
    }
    pthread_mutex_init(&st->lock, NULL);
    return st;
}

static void fake_authenticate(void* state, const v3_ext_auth_request_t* req,
                              v3_ext_auth_result_t* out) {
    (void)state;
    if (req->origin && strcmp(req->origin, "http://evil.example") == 0) {
        snprintf(out->reason, sizeof(out->reason), "origin_not_allowed");
        return;
    }
    if (!req->credential) {
        snprintf(out->error_code, sizeof(out->error_code), "AUTH_REQUIRED");
        snprintf(out->reason, sizeof(out->reason), "key required: connect with ?key=<value>");
        return;
    }
    if (strcmp(req->credential, "full-key-0001") == 0) {
        out->ok = true;
        out->access = V3_ACCESS_FULL;
        snprintf(out->tier_label, sizeof(out->tier_label), "internal");
        snprintf(out->account, sizeof(out->account), "org-full");
    } else if (strcmp(req->credential, "public-key-0001") == 0) {
        out->ok = true;
        out->access = V3_ACCESS_PUBLIC_ONLY;
        snprintf(out->tier_label, sizeof(out->tier_label), "public");
        snprintf(out->account, sizeof(out->account), "org-public");
    } else {
        snprintf(out->reason, sizeof(out->reason), "unknown_key");
        return;
    }
    snprintf(out->principal, sizeof(out->principal), "%s", req->credential);
}

static json_t* str_or_null(const char* s) {
    return s ? json_string(s) : json_null();
}

static void fake_on_run_finished(void* state, const v3_run_report_t* r) {
    fake_state_t* st = state;
    json_t* o = json_object();
    json_object_set_new(o, "event", json_string(r->event));
    json_object_set_new(o, "principal", json_string(r->principal));
    json_object_set_new(o, "account", json_string(r->account));
    json_object_set_new(o, "run_id", str_or_null(r->run_id));
    json_object_set_new(o, "pipeline", str_or_null(r->pipeline));
    json_object_set_new(o, "input_hash", str_or_null(r->input_hash));
    json_object_set_new(o, "success", json_boolean(r->success));
    json_object_set_new(o, "error_code", str_or_null(r->error_code));
    json_object_set_new(o, "error_message", str_or_null(r->error_message));
    json_object_set_new(o, "n_executed", json_integer((json_int_t)r->n_executed));
    json_object_set_new(o, "n_cache_hits", json_integer((json_int_t)r->n_cache_hits));
    json_object_set_new(o, "duration_us", json_integer((json_int_t)r->duration_us));
    if (r->has_latlon) {
        json_object_set_new(o, "lat", json_real(r->lat));
        json_object_set_new(o, "lon", json_real(r->lon));
    }
    char* line = json_dumps(o, JSON_COMPACT);
    json_decref(o);
    if (!line) return;
    pthread_mutex_lock(&st->lock);
    fprintf(st->out, "%s\n", line);
    fflush(st->out);
    pthread_mutex_unlock(&st->lock);
    free(line);
}

static void fake_shutdown(void* state) {
    fake_state_t* st = state;
    if (!st) return;
    fprintf(st->out, "{\"event\":\"shutdown\"}\n");
    fclose(st->out);
    pthread_mutex_destroy(&st->lock);
    free(st);
}

static const v3_extension_t k_fake = {
    .name = "fake-extension",
    .credential_param = "key",
    .init = fake_init,
    .authenticate = fake_authenticate,
    .on_run_finished = fake_on_run_finished,
    .shutdown = fake_shutdown,
};

const v3_extension_t* v3_extension(void) {
    return &k_fake;
}

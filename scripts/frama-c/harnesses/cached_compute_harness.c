// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Frama-C audit harness for `dioramap_cached_compute`.

#include "v3/cached_compute.h"
#include "v3/hash.h"
#include "v3/intent_map.h"
#include "v3/store.h"

#include <__fc_builtin.h>
#include <stdlib.h>
#include <string.h>

static int compute_ok(void* ctx, uint8_t** out, size_t* out_len) {
    (void)ctx;
    *out = (uint8_t*)malloc(16);
    if (!*out) return -1;
    *out_len = 16;
    return 0;
}

static int compute_fail(void* ctx, uint8_t** out, size_t* out_len) {
    (void)ctx;
    *out = NULL;
    *out_len = 0;
    return -1;
}

int main(void) {
    intent_map_t* im = intent_map_open("/tmp/audit-cc-intent");
    store_config_t cfg = {
        .root_dir = "/tmp/audit-cc-store",
        .budget_bytes = 1024,
        .hot_budget_bytes = 1024,
        .janitor_interval_sec = 0.0,
    };
    store_t* s = store_open(&cfg);
    if (!im || !s) return 0;

    hash_t id;
    Frama_C_make_unknown((char*)id.bytes, 32);
    hash_t out;

    (void)cached_compute(im, s, &id, compute_ok, NULL, &out);
    (void)cached_compute(im, s, &id, compute_ok, NULL, &out);   // second call: hit
    (void)cached_compute(im, s, &id, compute_fail, NULL, &out); // failure path

    // NULL-arg paths
    (void)cached_compute(NULL, s, &id, compute_ok, NULL, &out);
    (void)cached_compute(im, NULL, &id, compute_ok, NULL, &out);
    (void)cached_compute(im, s, NULL, compute_ok, NULL, &out);
    (void)cached_compute(im, s, &id, NULL, NULL, &out);
    (void)cached_compute(im, s, &id, compute_ok, NULL, NULL);

    store_close(s);
    intent_map_close(im);
    return 0;
}

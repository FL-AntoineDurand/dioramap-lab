// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Frama-C audit harness for `dioramap_store`.
// store_open touches mkdir/opendir/etc; abstract path returns NULL.
// We exercise as much of the heap-tier path as we can.

#include "v3/store.h"
#include "v3/hash.h"

#include <__fc_builtin.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    store_config_t cfg = {
        .root_dir = "/tmp/audit-store",
        .budget_bytes = 1024 * 1024,
        .hot_budget_bytes = 64 * 1024,
        .janitor_interval_sec = 0.0, // no background janitor
    };
    store_t* s = store_open(&cfg);
    if (!s) return 0;

    hash_t key;
    Frama_C_make_unknown((char*)key.bytes, 32);

    size_t len = Frama_C_unsigned_int_interval(0, 256);
    uint8_t* buf = (uint8_t*)malloc(len);
    if (!buf) { store_close(s); return 0; }
    Frama_C_make_unknown((char*)buf, len);

    int rc = store_put(s, &key, buf, len);
    if (rc != 0) {
        free(buf); // ownership returned to caller on failure
    }

    (void)store_contains(s, &key);
    store_pin(s, &key);

    store_handle_t h;
    if (store_lookup(s, &key, &h) == 0) {
        store_release(s, &h);
    }

    store_unpin(s, &key);
    (void)store_forget(s, &key);

    store_stats_t st;
    store_get_stats(s, &st);
    (void)store_evict_to(s, 0);

    // NULL paths
    (void)store_open((const store_config_t*)0);
    store_close((store_t*)0);
    (void)store_contains((store_t*)0, &key);
    (void)store_put((store_t*)0, &key, NULL, 0);

    store_close(s);
    return 0;
}

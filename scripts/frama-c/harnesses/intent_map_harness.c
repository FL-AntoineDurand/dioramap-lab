// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Frama-C audit harness for `dioramap_intent_map`.

#include "v3/intent_map.h"

#include <__fc_builtin.h>
#include <stddef.h>
#include <stdint.h>

static void walk_cb(const hash_t* identity, const hash_t* content_hash, void* ctx) {
    (void)identity;
    (void)content_hash;
    (void)ctx;
}

int main(void) {
    intent_map_t* m = intent_map_open("/tmp/audit-intent.log");
    if (!m) {
        return 0;
    }

    hash_t id;
    hash_t ch;
    Frama_C_make_unknown((char*)id.bytes, 32);
    Frama_C_make_unknown((char*)ch.bytes, 32);

    (void)intent_map_put(m, &id, &ch);
    hash_t got;
    (void)intent_map_get(m, &id, &got);
    (void)intent_map_invalidate(m, &id);
    (void)intent_map_get(m, &id, &got);

    intent_map_stats_t st;
    intent_map_get_stats(m, &st);

    intent_map_for_each(m, walk_cb, (void*)0);

    (void)intent_map_flush(m);
    (void)intent_map_compact(m);

    // NULL-pointer paths the API doc allows.
    (void)intent_map_get(m, &id, (hash_t*)0); // out_content_hash NULL → API doesn't say
    (void)intent_map_open((const char*)0);

    intent_map_close(m);
    return 0;
}

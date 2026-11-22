// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Frama-C audit harness for `dioramap_hash`.
//
// Exercises every public function declared in <v3/hash.h> with
// non-deterministic inputs (Frama_C_make_unknown). Eva treats each call
// as an opaque function on abstract values; the goal is RTE-level proof:
// no UB (signed overflow, OOB, null-deref, divide-by-zero) regardless of
// input.
//
// Build: not built by CMake. Driven via:
//   scripts/frama-c/audit-module.sh hash
//
// See docs/quality/README.md.

#include "v3/hash.h"

#include <__fc_builtin.h>     // Frama_C_make_unknown
#include <stddef.h>
#include <stdint.h>

// Fuzz-style inputs — bounded sizes so Eva can unroll. Real callers can
// of course pass arbitrary sizes; if a finding requires inputs larger
// than these, bump the constants and re-run.
#define AUD_BUF_SZ 64
#define AUD_TAG_SZ 16

int main(void) {
    uint8_t buf[AUD_BUF_SZ];
    char    tag[AUD_TAG_SZ];
    char    hex[65];

    Frama_C_make_unknown((char*)buf, AUD_BUF_SZ);
    Frama_C_make_unknown(tag, AUD_TAG_SZ - 1);
    tag[AUD_TAG_SZ - 1] = '\0'; // ensure tag is a valid C string

    size_t len = Frama_C_unsigned_int_interval(0, AUD_BUF_SZ);

    hash_t h1 = hash_bytes(buf, len);
    hash_t h2 = hash_namespaced(tag, buf, len);
    hash_t h3 = hash_namespaced((const char*)0, buf, len);   // tag may be NULL
    hash_t h4 = hash_namespaced(tag, (const void*)0, 0);     // data may be NULL+zero
    (void)hash_eq(&h1, &h2);
    (void)hash_eq(&h3, &h4);
    hash_format_hex(&h1, hex);

    hash_state_t* st = hash_stream_open();
    hash_stream_update(st, buf, len);
    hash_stream_update(st, (const void*)0, 0); // null+zero is documented as no-op
    hash_t h5 = hash_stream_finalize(st);
    (void)hash_eq(&h5, &h1);

    // Finalize-on-null path (state allocation failed). Documented:
    // returns zero-init hash, frees state if non-null.
    hash_state_t* st2 = (hash_state_t*)0;
    (void)hash_stream_finalize(st2);

    return 0;
}

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Content-hash wrapper.
//
// Uses xxh3 128-bit for now: fast (~30 GB/s), well-tested, single apt package.
// Our threat model has no adversarial input (the cache is internal, we control
// what goes in), so cryptographic collision resistance isn't required.
// If we later need cryptographic strength, swap to blake3 behind this same
// interface with no caller changes — hash_t is 32 bytes wide exactly so a
// 256-bit hash will fit with zero-padding removed.

#include "v3/hash.h"

#define XXH_STATIC_LINKING_ONLY
#define XXH_INLINE_ALL
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xxhash.h>

hash_t hash_bytes(const void* data, size_t len) {
    XXH128_hash_t x = XXH3_128bits(data, len);
    hash_t h = {{0}};
    // Big-endian store so hex formatting reads "naturally".
    for (int i = 7; i >= 0; i--) {
        h.bytes[7 - i] = (uint8_t)(x.high64 >> (i * 8));
        h.bytes[15 - i] = (uint8_t)(x.low64 >> (i * 8));
    }
    return h;
}

hash_t hash_namespaced(const char* tag, const void* data, size_t len) {
    XXH3_state_t* st = XXH3_createState();
    hash_t h = {{0}};
    if (!st) {
        return h;
    }
    XXH3_128bits_reset(st);
    if (tag) {
        XXH3_128bits_update(st, tag, strlen(tag));
    }
    static const uint8_t sep = 0;
    XXH3_128bits_update(st, &sep, 1);
    if (data && len > 0) {
        XXH3_128bits_update(st, data, len);
    }
    XXH128_hash_t x = XXH3_128bits_digest(st);
    XXH3_freeState(st);
    for (int i = 7; i >= 0; i--) {
        h.bytes[7 - i] = (uint8_t)(x.high64 >> (i * 8));
        h.bytes[15 - i] = (uint8_t)(x.low64 >> (i * 8));
    }
    return h;
}

bool hash_eq(const hash_t* a, const hash_t* b) {
    return memcmp(a->bytes, b->bytes, sizeof(a->bytes)) == 0;
}

void hash_format_hex(const hash_t* h, char out[65]) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(h->bytes); i++) {
        out[i * 2] = hex[h->bytes[i] >> 4];
        out[i * 2 + 1] = hex[h->bytes[i] & 0x0F];
    }
    out[64] = '\0';
}

// ----------------------------------------------------------------------------
// Streaming API
// ----------------------------------------------------------------------------

struct hash_state {
    XXH3_state_t* xx;
};

hash_state_t* hash_stream_open(void) {
    hash_state_t* s = calloc(1, sizeof(hash_state_t));
    if (!s) {
        return NULL;
    }
    s->xx = XXH3_createState();
    if (!s->xx) {
        free(s);
        return NULL;
    }
    XXH3_128bits_reset(s->xx);
    return s;
}

void hash_stream_update(hash_state_t* s, const void* data, size_t len) {
    if (!s || !s->xx || !data || len == 0) {
        return;
    }
    XXH3_128bits_update(s->xx, data, len);
}

hash_t hash_stream_finalize(hash_state_t* s) {
    hash_t h = {{0}};
    if (!s || !s->xx) {
        if (s) {
            free(s);
        }
        return h;
    }
    XXH128_hash_t x = XXH3_128bits_digest(s->xx);
    for (int i = 7; i >= 0; i--) {
        h.bytes[7 - i] = (uint8_t)(x.high64 >> (i * 8));
        h.bytes[15 - i] = (uint8_t)(x.low64 >> (i * 8));
    }
    XXH3_freeState(s->xx);
    free(s);
    return h;
}

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#ifndef V3_HASH_H
#define V3_HASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Content-addressing hash.
//
// Storage is fixed-width 32 bytes. The current hash function (xxh3 128-bit)
// only fills the first 16; remaining bytes are zero. Keeping the type
// 32-bytes-wide leaves room to swap in blake3 (256-bit) later without
// changing any caller or on-disk layout that references hash_t.
typedef struct hash {
    uint8_t bytes[32];
} hash_t;

/*@ requires len == 0 || \valid_read(((char*)data) + (0 .. len - 1));
    assigns \nothing;
*/
hash_t hash_bytes(const void* data, size_t len);

// Namespaced identity: hash(tag || "\0" || data). Used to construct sub-scope
// identities (HASH-MODEL.md §4.2) — the tag prevents cross-domain collisions
// between e.g. URL-keyed caches and tile-keyed caches. `tag` is a short ASCII
// string, NOT included as a NUL terminator in the hash; the NUL separator
// between tag and data is explicit.
/*@ requires tag == \null || \valid_read(tag) && \initialized(tag);
    requires len == 0 || \valid_read(((char*)data) + (0 .. len - 1));
    assigns \nothing;
*/
hash_t hash_namespaced(const char* tag, const void* data, size_t len);

/*@ requires \valid_read(a);
    requires \valid_read(b);
    assigns \nothing;
    ensures \result == 0 || \result == 1;
*/
bool hash_eq(const hash_t* a, const hash_t* b);

// Writes 64 hex chars + NUL. Buffer must be at least 65 bytes.
/*@ requires \valid_read(h);
    requires \valid(out + (0 .. 64));
    assigns out[0 .. 64] \from *h;
    ensures out[64] == '\0';
*/
void hash_format_hex(const hash_t* h, char out[65]);

// Streaming API — for hashing data that isn't contiguous. Finalize gives the
// hash_t. Same function as hash_bytes when used as open/update(all)/final.
typedef struct hash_state hash_state_t;

/*@ assigns \nothing;
    ensures \result == \null || \valid(\result);
*/
hash_state_t* hash_stream_open(void);

/*@ requires s == \null || \valid(s);
    requires len == 0 || \valid_read(((char*)data) + (0 .. len - 1));
    assigns *s;
*/
void hash_stream_update(hash_state_t* s, const void* data, size_t len);

/*@ requires s == \null || \valid(s);
    assigns *s;
*/
hash_t hash_stream_finalize(hash_state_t* s); // frees the state

#ifdef __cplusplus
}
#endif

#endif

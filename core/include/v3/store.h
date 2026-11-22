// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#ifndef V3_STORE_H
#define V3_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>   // for ACSL valid_read_string

#include "v3/hash.h"

#ifdef __cplusplus
extern "C" {
#endif

// Content-addressed two-tier artifact store (HASH-MODEL.md §5).
//
// - Entries are keyed by content hash.
// - Two tiers:
//     * HOT:  heap-resident buffer, producer → consumer with zero copies.
//     * WARM: disk-resident at <root>/<hex[0:2]>/<hex[2:4]>/<full-hex>.bin,
//             mmap'd on read (zero-copy from OS page cache).
// - Puts take ownership of the heap buffer; callers that need to preserve
//   their bytes do `malloc + memcpy` at the call site.
// - Puts land in hot tier when budget allows; under pressure the store
//   demotes LRU hot entries to warm, synchronously if needed.
// - Entries larger than the hot-tier budget skip hot and write straight to warm.
// - Writes to warm are atomic (temp + fsync + rename).
// - Pins (session refs, active reads) guard entries against LRU eviction
//   on either tier.
// - Disk janitor runs periodically and evicts LRU warm entries over the
//   warm-tier budget.
// - On open, scans the directory and rebuilds the warm-tier index from file
//   metadata.

typedef struct store store_t;

typedef struct store_config {
    const char* root_dir;        // cache root; created if missing
    uint64_t budget_bytes;       // warm-tier disk budget; 0 = no disk eviction
    uint64_t hot_budget_bytes;   // hot-tier heap budget; 0 = hot tier disabled
    double janitor_interval_sec; // 0 = no background janitor (manual eviction only)
} store_config_t;

/*@ requires cfg == \null || \valid_read(cfg);
    assigns \nothing;
    ensures \result == \null || \valid(\result);
*/
store_t* store_open(const store_config_t* cfg);

/*@ requires s == \null || \valid(s);
    assigns *s;
*/
void store_close(store_t* s);

// ---------------------------------------------------------------------------
// Write path
// ---------------------------------------------------------------------------

// Store bytes under an explicit caller-supplied content-hash key.
//
// OWNERSHIP: `bytes` MUST be a heap buffer allocated with malloc/calloc.
// After a successful return the store owns it and will free() it when the
// entry is demoted, evicted, or when the store is closed. Callers that need
// to retain the bytes do `malloc + memcpy` at the call site before the put.
// On failure the caller still owns `bytes` and must free it.
//
// If the key is already present the put is a dedupe no-op: the caller's
// `bytes` is freed (the stored bytes are assumed byte-identical — which is
// guaranteed when `key == hash(bytes)`, i.e. content-hashing).
/*@ requires s == \null || \valid(s);
    requires key == \null || \valid_read(key);
    requires len == 0 || \valid(bytes + (0 .. len - 1));
    assigns *s, *(bytes + (0 .. len - 1));
    ensures \result == 0 || \result == -1;
*/
int store_put(store_t* s, const hash_t* key, uint8_t* bytes, size_t len);

// Lookup-only check: does the store contain this hash? Cheaper than
// store_lookup because it doesn't open the file. Returns true if present.
/*@ requires s == \null || \valid(s);
    requires h == \null || \valid_read(h);
    assigns \nothing;
    ensures \result == 0 || \result == 1;
*/
bool store_contains(store_t* s, const hash_t* h);

// Forget an entry in the in-memory index AND attempt to unlink any
// on-disk .bin file. Used by the runner's F2 cache-desync recovery
// path when a downstream step's read failed despite store_contains
// having reported the hash as present (the in-memory index was lying
// — file gone, evicted underneath the store, etc.). Idempotent: returns
// 0 whether or not the entry actually existed. Pinned entries are NOT
// forgotten; the call returns -1 in that case so the caller can wait.
/*@ requires s == \null || \valid(s);
    requires h == \null || \valid_read(h);
    assigns *s;
    ensures \result == 0 || \result == -1;
*/
int store_forget(store_t* s, const hash_t* h);

// ---------------------------------------------------------------------------
// Read path — zero-copy via mmap
// ---------------------------------------------------------------------------

// Handle to a looked-up entry. `bytes` points into either the hot-tier heap
// buffer (direct pointer, zero copy) or a warm-tier mmap (kernel-managed,
// also zero copy). The caller cannot tell the difference from the `bytes`
// pointer alone. Must be released via store_release.
typedef struct store_handle {
    const uint8_t* bytes;
    size_t len;
    hash_t hash;
    // Private:
    void* _map;       // non-NULL iff the entry was warm (mmap'd file)
    size_t _map_len;
    int _fd;          // >= 0 iff the entry was warm
    void* _entry;
} store_handle_t;

// Lookup by hash. Hit → opens a read handle (mmap) and returns 0.
// Miss → returns -1. On hit, the entry is pinned until store_release().
/*@ requires s == \null || \valid(s);
    requires h == \null || \valid_read(h);
    requires out == \null || \valid(out);
    assigns *s, *out;
    ensures \result == 0 || \result == -1;
*/
int store_lookup(store_t* s, const hash_t* h, store_handle_t* out);

/*@ requires s == \null || \valid(s);
    requires h == \null || \valid(h);
    assigns *s, *h;
*/
void store_release(store_t* s, store_handle_t* h);

// ---------------------------------------------------------------------------
// Pinning — session refs against eviction
// ---------------------------------------------------------------------------

// Increment the entry's pin count. No-op if the entry isn't in the store.
/*@ requires s == \null || \valid(s);
    requires h == \null || \valid_read(h);
    assigns *s;
*/
void store_pin(store_t* s, const hash_t* h);

// Decrement. Must be paired with a prior pin.
/*@ requires s == \null || \valid(s);
    requires h == \null || \valid_read(h);
    assigns *s;
*/
void store_unpin(store_t* s, const hash_t* h);

// ---------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------

typedef struct store_stats {
    uint64_t entries;       // total entries (hot + warm)
    uint64_t hot_entries;   // entries in hot tier
    uint64_t total_bytes;   // warm-tier disk footprint
    uint64_t hot_bytes;     // hot-tier heap footprint
    uint64_t budget_bytes;       // warm budget (config)
    uint64_t hot_budget_bytes;   // hot budget (config)
    uint64_t hits;
    uint64_t misses;
    uint64_t writes;
    uint64_t evictions;     // warm-tier entries unlinked
    uint64_t demotions;     // hot -> warm transfers
} store_stats_t;

/*@ requires s == \null || \valid(s);
    requires \valid(out);
    assigns *out;
*/
void store_get_stats(store_t* s, store_stats_t* out);

// ---------------------------------------------------------------------------
// Manual eviction — normally the janitor handles this, but tests and ops
// may call directly. Evicts LRU entries until total_bytes <= target_bytes
// (or no more unpinned entries are available). Returns number evicted.
// ---------------------------------------------------------------------------

/*@ requires s == \null || \valid(s);
    assigns *s;
*/
size_t store_evict_to(store_t* s, uint64_t target_bytes);

#ifdef __cplusplus
}
#endif
#endif

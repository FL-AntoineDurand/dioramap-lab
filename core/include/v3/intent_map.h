// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#ifndef V3_INTENT_MAP_H
#define V3_INTENT_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>   // for ACSL valid_read_string

#include "v3/hash.h"

#ifdef __cplusplus
extern "C" {
#endif

// Persistent identity → content_hash map. See HASH-MODEL.md §6.
//
// - Answers exactly one question: given `identity`, what content hash did its
//   computation produce?
// - Backed by an append-only log on disk; in-memory hashmap serves reads.
// - 65-byte records: 32 B identity, 32 B content_hash, 1 B flags (bit 0 =
//   tombstone). `write(2)` is atomic at this size under PIPE_BUF.
// - Crash-safe: a partial trailing record (file size not a multiple of 65) is
//   detected and discarded on startup.
// - Compacts on startup if the log is 2× larger than live entries.

typedef struct intent_map intent_map_t;

// Open or create the intent map at `log_path`. The parent directory is
// created if it doesn't exist. Returns NULL on failure.
/*@ requires log_path == \null || valid_read_string(log_path);
    assigns \nothing;
    ensures \result == \null || \valid(\result);
*/
intent_map_t* intent_map_open(const char* log_path);

// Close and release. Flushes the log via fsync before returning.
/*@ requires m == \null || \valid(m);
    assigns *m;
*/
void intent_map_close(intent_map_t* m);

// Lookup. Returns 0 on hit (fills out_content_hash), -1 on miss.
/*@ requires m == \null || \valid(m);
    requires identity == \null || \valid_read(identity);
    requires out_content_hash == \null || \valid(out_content_hash);
    assigns *out_content_hash, *m;
    ensures \result == 0 || \result == -1;
*/
int intent_map_get(intent_map_t* m, const hash_t* identity, hash_t* out_content_hash);

// Record identity → content_hash. Appends to the log and updates the
// in-memory hashmap. Later entries supersede earlier ones for the same
// identity. Returns 0 on success, -1 on I/O / allocation failure.
/*@ requires m == \null || \valid(m);
    requires identity == \null || \valid_read(identity);
    requires content_hash == \null || \valid_read(content_hash);
    assigns *m;
    ensures \result == 0 || \result == -1;
*/
int intent_map_put(intent_map_t* m, const hash_t* identity, const hash_t* content_hash);

// Invalidate an identity. Appends a tombstone; the in-memory hashmap
// behaves as if the entry was never written. Used by validate-on-use when
// a manifest references content that's been evicted.
/*@ requires m == \null || \valid(m);
    requires identity == \null || \valid_read(identity);
    assigns *m;
    ensures \result == 0 || \result == -1;
*/
int intent_map_invalidate(intent_map_t* m, const hash_t* identity);

// Force the log to disk. Called at shutdown; otherwise puts are not fsync'd.
/*@ requires m == \null || \valid(m);
    assigns \nothing;
    ensures \result == 0 || \result == -1;
*/
int intent_map_flush(intent_map_t* m);

// Rewrite the on-disk log keeping only live entries. Not thread-safe with
// concurrent put/get; typically called during shutdown or startup of a
// lone process.
/*@ requires m == \null || \valid(m);
    assigns *m;
    ensures \result == 0 || \result == -1;
*/
int intent_map_compact(intent_map_t* m);

// Stats.
typedef struct intent_map_stats {
    uint64_t live_entries;   // in-memory hashmap size
    uint64_t log_records;    // total records appended to the log this session
    uint64_t log_bytes;      // log file size
    uint64_t hits;           // successful lookups since open
    uint64_t misses;         // unsuccessful lookups since open
    uint64_t invalidations;  // tombstones recorded since open
} intent_map_stats_t;

/*@ requires m == \null || \valid(m);
    requires \valid(out);
    assigns *out;
*/
void intent_map_get_stats(intent_map_t* m, intent_map_stats_t* out);

// Walk every live (non-tombstoned) entry once, in unspecified order.
// `cb` receives each (identity, content_hash) pair plus the caller-
// provided ctx. Used by the startup integrity audit (B) to cross-check
// each intent entry against the store. Caller must not call any other
// intent_map_* function from inside cb.
typedef void (*intent_map_walk_cb)(const hash_t* identity, const hash_t* content_hash,
                                   void* ctx);
/*@ requires m == \null || \valid(m);
    requires cb == \null || \valid_function(cb);
    assigns *m;
*/
void intent_map_for_each(intent_map_t* m, intent_map_walk_cb cb, void* ctx);

#ifdef __cplusplus
}
#endif

#endif

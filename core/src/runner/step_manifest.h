// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Step-output manifest: {write_name → content_hash}.
//
// A step's exec_fn produces one or more named writes. Rather than the runner
// tracking (name, hash) pairs out-of-band, each step's `cached_compute` call
// produces a single manifest artifact that lists those pairs; the manifest
// itself is content-addressed in the store. See HASH-MODEL.md §3 / §7.1.
//
// On-disk format (packed, little-endian):
//   u32  n_entries
//   per entry (n_entries times):
//     u32        name_len
//     u8[name_len]  name_bytes (no NUL)
//     u8[32]     content_hash
//
// This format is private to the runner/intent-map code; external code reads
// manifests through the runner's artifact-map API, not by parsing bytes.

#ifndef V3_RUNNER_STEP_MANIFEST_H
#define V3_RUNNER_STEP_MANIFEST_H

#include <stddef.h>
#include <stdint.h>

#include "v3/hash.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct step_manifest_entry {
    const char* name;
    hash_t content_hash;
} step_manifest_entry_t;

// Pack entries into a freshly malloc'd buffer. Returns 0 on success; on
// return *out_bytes is owned by the caller (malloc'd). On failure returns -1
// and *out_bytes is NULL.
int step_manifest_pack(const step_manifest_entry_t* entries, size_t n,
                       uint8_t** out_bytes, size_t* out_len);

// Walk a packed manifest. For each entry, invokes cb(ctx, name, &hash).
// Returns 0 on success, -1 on parse error (corrupt manifest).
typedef int (*step_manifest_cb)(void* ctx, const char* name, size_t name_len,
                                const hash_t* content_hash);
int step_manifest_walk(const uint8_t* bytes, size_t len, step_manifest_cb cb, void* ctx);

#ifdef __cplusplus
}
#endif

#endif

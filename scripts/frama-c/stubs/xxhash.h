// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Frama-C audit stub for <xxhash.h>.
//
// The real xxhash.h (apt: libxxhash-dev) uses SSE/AVX intrinsics and
// _Alignas, both of which Frama-C 25 (Manganese) cannot parse. For the
// audit we only need ACSL-friendly extern declarations of the API our
// code calls — Eva treats each call as an opaque function returning an
// unconstrained value of the declared type, which is the right
// abstraction for content hashing (no semantic property of the hash
// is relied on by callers under analysis).
//
// If new XXH3_* symbols appear in src code, add a declaration here.
#ifndef DIORAMAP_AUDIT_XXHASH_H
#define DIORAMAP_AUDIT_XXHASH_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t low64;
    uint64_t high64;
} XXH128_hash_t;

typedef struct XXH3_state_s XXH3_state_t;

XXH128_hash_t XXH3_128bits(const void* data, size_t len);
XXH3_state_t* XXH3_createState(void);
void          XXH3_freeState(XXH3_state_t* state);
void          XXH3_128bits_reset(XXH3_state_t* state);
void          XXH3_128bits_update(XXH3_state_t* state, const void* data, size_t len);
XXH128_hash_t XXH3_128bits_digest(const XXH3_state_t* state);

// Macros the source file defines before including the real xxhash.h.
// They drive code paths in the real header; here they are no-ops.
#ifndef XXH_INLINE_ALL
#define XXH_INLINE_ALL
#endif
#ifndef XXH_STATIC_LINKING_ONLY
#define XXH_STATIC_LINKING_ONLY
#endif

#endif // DIORAMAP_AUDIT_XXHASH_H

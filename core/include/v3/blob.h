// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Variable-length record blob — packs N variable-length records into a
// single byte buffer using 8-byte-aligned framing:
//
//   {u32 record_length, u32 zero_pad, payload, trailing_pad_to_8_bytes} × N
//
// This is the SAME framing the framework's step_write_array_inline emits
// on disk and step_array_read_bytes walks. Steps use it in two ways:
//
//   (1) As input to step_write_array_inline. The framework validates the
//       stream, prepends the {u32 n, u32 stride=0} artifact header and
//       writes it out verbatim — no repacking.
//
//   (2) As a private per-record stream nested inside a single outer
//       per-building artifact record (e.g. dv2-ghost-planes packs N ghost
//       records inside one per-bldg byte slice).
//
// Why 8-byte alignment matters: any consumer can dereference F64 / pointer
// fields at well-known offsets inside a record's payload without UBSan
// trips. Previously inner records used a 4-byte length prefix; the second
// payload onward landed at a 4-aligned-but-not-8-aligned offset, and
// dereferencing an F64 there is undefined behaviour.

#ifndef DIORAMAP_V3_BLOB_H
#define DIORAMAP_V3_BLOB_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "v3/step.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct blob {
    uint8_t* bytes;
    size_t   len;
    size_t   cap;
    size_t   n_records;
} blob_t;

static inline size_t blob_align_up_8(size_t n) {
    return (n + 7u) & ~(size_t)7u;
}

static inline int blob_reserve(blob_t* b, size_t extra) {
    // Frama-C audit findings V7-1 / HS-2 (carried over from the per-step
    // local blob accumulators the merge unified into this shared header):
    // guard both the b->len + extra sum and the capacity-doubling against
    // size_t overflow. Practically unreachable today (SIZE_MAX ~ 18 EB) but
    // a textbook integer-overflow shape, and now every blob user inherits
    // the guard.
    if (extra > SIZE_MAX - b->len) return -1;
    if (b->len + extra <= b->cap) return 0;
    if (b->cap > SIZE_MAX / 2) return -1;
    size_t nc = b->cap ? b->cap * 2 : 4096;
    while (nc < b->len + extra) {
        if (nc > SIZE_MAX / 2) return -1;
        nc *= 2;
    }
    uint8_t* p = (uint8_t*)realloc(b->bytes, nc);
    if (!p) return -1;
    b->bytes = p;
    b->cap = nc;
    return 0;
}

// Append one record. Frames as {u32 record_length, u32 pad=0, payload,
// payload-trailing-padding-to-next-8-byte-boundary}. `rec` may be NULL
// when `rec_len == 0` (empty record — 8 bytes total on the wire).
static inline int blob_push(blob_t* b, const void* rec, size_t rec_len) {
    size_t framed = 8 + blob_align_up_8(rec_len);
    if (blob_reserve(b, framed) != 0) return -1;
    uint8_t* p = b->bytes + b->len;
    uint32_t L = (uint32_t)rec_len;
    memcpy(p, &L, 4);
    memset(p + 4, 0, 4);
    if (rec_len > 0 && rec) {
        memcpy(p + 8, rec, rec_len);
    }
    // Zero the trailing pad so artifact bytes are deterministic across
    // realloc fill patterns.
    size_t tail_pad = framed - 8 - rec_len;
    if (tail_pad > 0) {
        memset(p + 8 + rec_len, 0, tail_pad);
    }
    b->len += framed;
    b->n_records++;
    return 0;
}

static inline void blob_free(blob_t* b) {
    free(b->bytes);
    b->bytes = NULL;
    b->len = b->cap = b->n_records = 0;
}

// Convenience: emit a packed blob as an ARRAY_INLINE artifact.
static inline int emit_blob(step_context_t* ctx, const char* name, blob_t* b) {
    return step_write_array_inline(ctx, name, b->bytes, b->len, b->n_records, 0);
}

// Walk a packed blob one record at a time. Initialize *cursor=0, then
// call repeatedly until it returns 0 (clean exhaustion or malformed
// truncation). On success: *out_payload points into `bytes`, *out_len
// holds the record's length. Returns 1 on success, 0 on end, -1 on
// parameter error.
static inline int blob_next(const uint8_t* bytes, size_t total,
                            size_t* cursor,
                            const uint8_t** out_payload,
                            size_t* out_len) {
    if (!cursor) return -1;
    size_t off = *cursor;
    if (!bytes || off + 8 > total) return 0;
    uint32_t L;
    memcpy(&L, bytes + off, 4);
    if (off + 8 + L > total) return 0;
    if (out_payload) *out_payload = bytes + off + 8;
    if (out_len)     *out_len = L;
    *cursor = off + 8 + blob_align_up_8(L);
    return 1;
}

#ifdef __cplusplus
}
#endif

#endif // DIORAMAP_V3_BLOB_H

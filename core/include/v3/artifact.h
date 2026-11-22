// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#ifndef V3_ARTIFACT_H
#define V3_ARTIFACT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>   // ACSL valid_read_string

#include "v3/hash.h"

#ifdef __cplusplus
extern "C" {
#endif

// ----------------------------------------------------------------------------
// Artifact binary layout (§5 of UNIFIED-ARCHITECTURE.md).
//
// Layout:
//   [ header (128 B, natural-aligned) ]
//   [ column descriptor 0 (32 B) ]
//   [ column descriptor 1 (32 B) ]
//   ...
//   [ optional metadata JSON blob ]
//   [ column 0 data ]
//   [ column 1 data ]
//   ...
//
// All multi-byte fields are little-endian.
// ----------------------------------------------------------------------------

#define ARTIFACT_MAGIC "DMAP"
#define ARTIFACT_FORMAT_VERSION 1u
#define ARTIFACT_HEADER_SIZE 128
#define COLUMN_DESC_SIZE 32

typedef enum artifact_kind {
    AK_POINTCLOUD_SOA = 0,
    AK_PER_POINT_ARRAY = 1,
    AK_GEOMETRY_PRIMITIVES = 2,
    AK_RASTER_2D = 3,
    AK_POLYGONS_3D = 4,
    AK_PLANE_RECORDS = 5,
    AK_MASK_BITSET = 6,
    AK_OPAQUE_JSON = 7,
    // Array artifacts (§5.3 of UNIFIED-ARCHITECTURE.md). Bytes are a simple
    // raw layout, not the DMAP header format above — an array artifact is
    // content-addressed bytes whose internal structure is defined in
    // `v3/array.h`.
    AK_ARRAY_INLINE = 8,
    AK_ARRAY_BY_HASH = 9,
    // PLAN-VIEWER-PORT §9 extensions. These tag semantic type for steps
    // that emit raw bytes (not the DMAP header layout). Downstream tooling
    // and the lab UI use `kind` to pick the right renderer.
    AK_POINT_COLUMN = 10,  // single per-point column (F32/F64/U8/U16)
    AK_RASTER_F32 = 11,    // Float32 grid (row-major), e.g. BIL elevation
    AK_IMAGE_JPEG = 12,    // compressed JPEG bytes
} artifact_kind_t;

typedef enum dtype {
    DT_INVALID = 0,
    DT_U8 = 1,
    DT_I8 = 2,
    DT_U16 = 3,
    DT_I16 = 4,
    DT_U32 = 5,
    DT_I32 = 6,
    DT_U64 = 7,
    DT_I64 = 8,
    DT_F32 = 9,
    DT_F64 = 10,
} dtype_t;

/*@ assigns \nothing; */
size_t dtype_size(dtype_t t);
/*@ assigns \nothing;
    ensures \result == \null || valid_read_string(\result); */
const char* dtype_name(dtype_t t);
/*@ assigns \nothing;
    ensures \result == \null || valid_read_string(\result); */
const char* artifact_kind_name(artifact_kind_t k);

// Header laid out at offset 0. Field order chosen so every u64 sits on an
// 8-byte boundary — no packed attribute needed. Verified by static_assert in
// the .c file.
typedef struct artifact_header {
    char magic[4];           // "DMAP"
    uint32_t format_version; // ARTIFACT_FORMAT_VERSION
    uint32_t kind;           // artifact_kind_t
    uint32_t n_columns;
    uint64_t count_n;
    uint64_t metadata_offset;
    uint64_t metadata_len;
    uint32_t flags; // reserved (0)
    uint32_t _pad;
    uint8_t reserved[80];
} artifact_header_t;

typedef struct column_desc {
    uint32_t name_hash; // FNV-1a of column name (stable, portable)
    uint8_t dtype;      // dtype_t
    uint8_t stride;     // element size in bytes (redundant but handy)
    uint16_t flags;     // reserved (0)
    uint64_t offset;    // bytes from start of artifact
    uint64_t length;    // bytes of column data
    uint8_t reserved[8];
} column_desc_t;

// Column name hash used in descriptors. Separate from content-hash; this is
// just for fast name → descriptor lookup on the read side.
/*@ requires name == \null || valid_read_string(name);
    assigns \nothing;
*/
uint32_t artifact_column_name_hash(const char* name);

// ----------------------------------------------------------------------------
// Writer (builder)
// ----------------------------------------------------------------------------

typedef struct artifact_builder artifact_builder_t;

/*@ assigns \nothing;
    ensures \result == \null || \valid(\result);
*/
artifact_builder_t* ab_new(artifact_kind_t kind, uint64_t count_n);

/*@ requires ab == \null || \valid(ab);
    assigns *ab;
*/
void ab_free(artifact_builder_t* ab);

/*@ requires ab == \null || \valid(ab);
    requires name == \null || valid_read_string(name);
    requires bytes == 0 || \valid_read(((char*)data) + (0 .. bytes - 1));
    assigns *ab;
    ensures \result == 0 || \result == -1;
*/
int ab_add_column(artifact_builder_t* ab, const char* name, dtype_t dtype, const void* data,
                  size_t bytes);

/*@ requires ab == \null || \valid(ab);
    requires len == 0 || \valid_read(json + (0 .. len - 1));
    assigns *ab;
    ensures \result == 0 || \result == -1;
*/
int ab_set_metadata_json(artifact_builder_t* ab, const char* json, size_t len);

/*@ requires ab == \null || \valid(ab);
    requires out_bytes == \null || \valid(out_bytes);
    requires out_len == \null || \valid(out_len);
    requires out_hash == \null || \valid(out_hash);
    assigns *ab, *out_bytes, *out_len, *out_hash;
    ensures \result == 0 || \result == -1;
*/
int ab_finalize(artifact_builder_t* ab, uint8_t** out_bytes, size_t* out_len, hash_t* out_hash);

// ----------------------------------------------------------------------------
// Reader (view) — zero-copy over a memory region
// ----------------------------------------------------------------------------

typedef struct artifact_view {
    const uint8_t* base;
    size_t total_len;
    const artifact_header_t* header;
    const column_desc_t* columns; // pointer into `base`
} artifact_view_t;

// Parse an artifact from a memory region (mmap'd file or in-memory buffer).
// Validates magic, version, header size, column offsets, bounds.
// Returns 0 on success, -1 on invalid/corrupt input.
/*@ requires bytes == \null || \valid_read(bytes + (0 .. len - 1));
    requires out == \null || \valid(out);
    assigns *out;
    ensures \result == 0 || \result == -1;
*/
int av_open(const uint8_t* bytes, size_t len, artifact_view_t* out);

/*@ requires v == \null || \valid_read(v);
    requires name == \null || valid_read_string(name);
    requires out_bytes == \null || \valid(out_bytes);
    assigns *out_bytes;
    ensures \result == \null || (out_bytes != \null
                                  && \valid_read(((char*)\result) + (0 .. *out_bytes - 1)));
*/
const void* av_column(const artifact_view_t* v, const char* name, size_t* out_bytes);

/*@ requires v == \null || \valid_read(v);
    requires name == \null || valid_read_string(name);
    assigns \nothing;
*/
dtype_t av_column_dtype(const artifact_view_t* v, const char* name);

/*@ requires v == \null || \valid_read(v);
    requires out_len == \null || \valid(out_len);
    assigns *out_len;
    ensures \result == \null || valid_read_string(\result);
*/
const char* av_metadata_json(const artifact_view_t* v, size_t* out_len);

/*@ requires v == \null || \valid_read(v);
    assigns \nothing;
*/
hash_t av_hash(const artifact_view_t* v);

#ifdef __cplusplus
}
#endif

#endif

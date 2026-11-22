// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#define _POSIX_C_SOURCE 200809L

#include "v3/artifact.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

// Verify header/column_desc sizes match the spec at compile time.
_Static_assert(sizeof(artifact_header_t) == ARTIFACT_HEADER_SIZE,
               "artifact_header_t must be exactly 128 bytes");
_Static_assert(sizeof(column_desc_t) == COLUMN_DESC_SIZE, "column_desc_t must be exactly 32 bytes");
_Static_assert(sizeof(char) == 1, "char must be 1 byte");

// ----------------------------------------------------------------------------
// Dtype helpers
// ----------------------------------------------------------------------------

size_t dtype_size(dtype_t t) {
    switch (t) {
    case DT_U8:
    case DT_I8:
        return 1;
    case DT_U16:
    case DT_I16:
        return 2;
    case DT_U32:
    case DT_I32:
    case DT_F32:
        return 4;
    case DT_U64:
    case DT_I64:
    case DT_F64:
        return 8;
    case DT_INVALID:
    default:
        return 0;
    }
}

const char* dtype_name(dtype_t t) {
    switch (t) {
    case DT_U8:
        return "u8";
    case DT_I8:
        return "i8";
    case DT_U16:
        return "u16";
    case DT_I16:
        return "i16";
    case DT_U32:
        return "u32";
    case DT_I32:
        return "i32";
    case DT_U64:
        return "u64";
    case DT_I64:
        return "i64";
    case DT_F32:
        return "f32";
    case DT_F64:
        return "f64";
    case DT_INVALID:
    default:
        return "invalid";
    }
}

const char* artifact_kind_name(artifact_kind_t k) {
    switch (k) {
    case AK_POINTCLOUD_SOA:
        return "pointcloud-soa";
    case AK_PER_POINT_ARRAY:
        return "per-point-array";
    case AK_GEOMETRY_PRIMITIVES:
        return "geometry-primitives";
    case AK_RASTER_2D:
        return "raster-2d";
    case AK_POLYGONS_3D:
        return "polygons3d";
    case AK_PLANE_RECORDS:
        return "plane-records";
    case AK_MASK_BITSET:
        return "mask-bitset";
    case AK_OPAQUE_JSON:
        return "opaque-json";
    default:
        return "unknown";
    }
}

// FNV-1a 32-bit. Stable, portable, no deps. Good enough for a sparse
// column-name space where collisions are caught at build time (duplicate add).
//
// NULL input returns the empty-string FNV-1a init value (0x811c9dc5).
// This is a defensive guard for the public ABI: every in-tree caller
// null-checks first, but the symbol is exported and out-of-tree callers
// would otherwise segfault on the first `*name` deref.
uint32_t artifact_column_name_hash(const char* name) {
    uint32_t h = 0x811c9dc5u;
    if (!name) return h;
    for (; *name; name++) {
        h ^= (uint32_t)(uint8_t)*name;
        h *= 0x01000193u;
    }
    return h;
}

// ----------------------------------------------------------------------------
// Align helper — column data starts on its dtype's natural alignment boundary
// ----------------------------------------------------------------------------

static size_t align_up(size_t x, size_t a) {
    if (a <= 1)
        return x;
    return (x + a - 1) & ~(a - 1);
}

// ----------------------------------------------------------------------------
// Builder
// ----------------------------------------------------------------------------

typedef struct pending_column {
    char* name;
    dtype_t dtype;
    void* data; // owned copy
    size_t bytes;
} pending_column_t;

struct artifact_builder {
    artifact_kind_t kind;
    uint64_t count_n;

    pending_column_t* columns;
    size_t n_columns;
    size_t cap_columns;

    char* metadata_json; // NULL if unset
    size_t metadata_len;
};

artifact_builder_t* ab_new(artifact_kind_t kind, uint64_t count_n) {
    artifact_builder_t* ab = calloc(1, sizeof(artifact_builder_t));
    if (!ab)
        return NULL;
    ab->kind = kind;
    ab->count_n = count_n;
    return ab;
}

void ab_free(artifact_builder_t* ab) {
    if (!ab)
        return;
    for (size_t i = 0; i < ab->n_columns; i++) {
        free(ab->columns[i].name);
        free(ab->columns[i].data);
    }
    free(ab->columns);
    free(ab->metadata_json);
    free(ab);
}

int ab_add_column(artifact_builder_t* ab, const char* name, dtype_t dtype, const void* data,
                  size_t bytes) {
    if (!ab || !name || !*name || dtype_size(dtype) == 0)
        return -1;
    if (bytes > 0 && !data)
        return -1;

    // Reject duplicate name (by both string and hash).
    uint32_t nh = artifact_column_name_hash(name);
    for (size_t i = 0; i < ab->n_columns; i++) {
        if (strcmp(ab->columns[i].name, name) == 0)
            return -1;
        if (artifact_column_name_hash(ab->columns[i].name) == nh) {
            // Hash collision with a different name — rare with FNV-1a, but
            // refuse because lookups would ambiguous.
            return -1;
        }
    }

    if (ab->n_columns == ab->cap_columns) {
        size_t new_cap = ab->cap_columns ? ab->cap_columns * 2 : 4;
        pending_column_t* nc = realloc(ab->columns, new_cap * sizeof(*nc));
        if (!nc)
            return -1;
        ab->columns = nc;
        ab->cap_columns = new_cap;
    }

    pending_column_t* c = &ab->columns[ab->n_columns];
    c->name = strdup(name);
    if (!c->name)
        return -1;
    c->dtype = dtype;
    c->bytes = bytes;
    c->data = NULL;
    if (bytes > 0) {
        c->data = malloc(bytes);
        if (!c->data) {
            free(c->name);
            return -1;
        }
        memcpy(c->data, data, bytes);
    }
    ab->n_columns++;
    return 0;
}

int ab_set_metadata_json(artifact_builder_t* ab, const char* json, size_t len) {
    if (!ab)
        return -1;
    free(ab->metadata_json);
    ab->metadata_json = NULL;
    ab->metadata_len = 0;
    if (!json || len == 0)
        return 0;
    ab->metadata_json = malloc(len);
    if (!ab->metadata_json)
        return -1;
    memcpy(ab->metadata_json, json, len);
    ab->metadata_len = len;
    return 0;
}

int ab_finalize(artifact_builder_t* ab, uint8_t** out_bytes, size_t* out_len, hash_t* out_hash) {
    if (!ab || !out_bytes || !out_len || !out_hash)
        return -1;

    // Layout plan:
    //   0..128               header
    //   128 + n*32           column descriptors
    //   then metadata blob (if any)
    //   then column data, each aligned to its dtype size
    size_t cursor = ARTIFACT_HEADER_SIZE + ab->n_columns * COLUMN_DESC_SIZE;

    uint64_t metadata_offset = 0;
    if (ab->metadata_len > 0) {
        metadata_offset = cursor;
        cursor += ab->metadata_len;
    }

    // Reserve column data offsets
    uint64_t* column_offsets = ab->n_columns ? calloc(ab->n_columns, sizeof(uint64_t)) : NULL;
    if (ab->n_columns && !column_offsets)
        return -1;

    for (size_t i = 0; i < ab->n_columns; i++) {
        size_t a = dtype_size(ab->columns[i].dtype);
        cursor = align_up(cursor, a);
        column_offsets[i] = cursor;
        cursor += ab->columns[i].bytes;
    }

    size_t total = cursor;
    uint8_t* buf = calloc(1, total);
    if (!buf) {
        free(column_offsets);
        return -1;
    }

    // Header
    artifact_header_t* hdr = (artifact_header_t*)buf;
    memcpy(hdr->magic, ARTIFACT_MAGIC, 4);
    hdr->format_version = ARTIFACT_FORMAT_VERSION;
    hdr->kind = (uint32_t)ab->kind;
    hdr->n_columns = (uint32_t)ab->n_columns;
    hdr->count_n = ab->count_n;
    hdr->metadata_offset = metadata_offset;
    hdr->metadata_len = ab->metadata_len;
    hdr->flags = 0;
    hdr->_pad = 0;

    // Column descriptors
    column_desc_t* cds = (column_desc_t*)(buf + ARTIFACT_HEADER_SIZE);
    for (size_t i = 0; i < ab->n_columns; i++) {
        pending_column_t* pc = &ab->columns[i];
        cds[i].name_hash = artifact_column_name_hash(pc->name);
        cds[i].dtype = (uint8_t)pc->dtype;
        cds[i].stride = (uint8_t)dtype_size(pc->dtype);
        cds[i].flags = 0;
        cds[i].offset = column_offsets[i];
        cds[i].length = pc->bytes;
    }

    // Metadata blob
    if (ab->metadata_len > 0) {
        memcpy(buf + metadata_offset, ab->metadata_json, ab->metadata_len);
    }

    // Column data
    for (size_t i = 0; i < ab->n_columns; i++) {
        if (ab->columns[i].bytes > 0) {
            memcpy(buf + column_offsets[i], ab->columns[i].data, ab->columns[i].bytes);
        }
    }

    free(column_offsets);

    *out_bytes = buf;
    *out_len = total;
    *out_hash = hash_bytes(buf, total);
    return 0;
}

// ----------------------------------------------------------------------------
// View (zero-copy reader)
// ----------------------------------------------------------------------------

int av_open(const uint8_t* bytes, size_t len, artifact_view_t* out) {
    if (!bytes || !out)
        return -1;
    if (len < ARTIFACT_HEADER_SIZE)
        return -1;

    const artifact_header_t* hdr = (const artifact_header_t*)bytes;
    if (memcmp(hdr->magic, ARTIFACT_MAGIC, 4) != 0)
        return -1;
    if (hdr->format_version != ARTIFACT_FORMAT_VERSION)
        return -1;

    size_t n_col = hdr->n_columns;
    // Frama-C audit finding A-2: bound n_col before the multiplication so
    // it can't wrap size_t. On 64-bit n_col ≤ UINT32_MAX so the product is
    // ≤ 2^37 — safely below SIZE_MAX — but on 32-bit it wraps. Guard for
    // platform portability and defense in depth.
    if (n_col > (SIZE_MAX - ARTIFACT_HEADER_SIZE) / COLUMN_DESC_SIZE)
        return -1;
    size_t desc_end = ARTIFACT_HEADER_SIZE + n_col * COLUMN_DESC_SIZE;
    if (desc_end > len)
        return -1;

    const column_desc_t* cds = (const column_desc_t*)(bytes + ARTIFACT_HEADER_SIZE);

    // Validate each column descriptor's offset+length fits within the buffer.
    for (size_t i = 0; i < n_col; i++) {
        uint64_t end = cds[i].offset + cds[i].length;
        if (cds[i].offset < desc_end)
            return -1;
        if (end > len)
            return -1;
        if (end < cds[i].offset)
            return -1; // overflow
        if (cds[i].dtype == 0 || cds[i].dtype > DT_F64)
            return -1;
        // A-3: stride must agree with the natural size of the declared dtype.
        // No in-tree consumer indexes by `stride` today, but the field is
        // publicly readable; reject inconsistencies up-front.
        if (cds[i].stride != dtype_size((dtype_t)cds[i].dtype))
            return -1;
    }

    if (hdr->metadata_len > 0) {
        uint64_t md_end = hdr->metadata_offset + hdr->metadata_len;
        if (hdr->metadata_offset < desc_end)
            return -1;
        if (md_end > len || md_end < hdr->metadata_offset)
            return -1;
    }

    out->base = bytes;
    out->total_len = len;
    out->header = hdr;
    out->columns = cds;
    return 0;
}

const void* av_column(const artifact_view_t* v, const char* name, size_t* out_bytes) {
    if (!v || !name)
        return NULL;
    uint32_t nh = artifact_column_name_hash(name);
    for (size_t i = 0; i < v->header->n_columns; i++) {
        if (v->columns[i].name_hash == nh) {
            if (out_bytes)
                *out_bytes = (size_t)v->columns[i].length;
            return v->base + v->columns[i].offset;
        }
    }
    if (out_bytes)
        *out_bytes = 0;
    return NULL;
}

dtype_t av_column_dtype(const artifact_view_t* v, const char* name) {
    if (!v || !name)
        return DT_INVALID;
    uint32_t nh = artifact_column_name_hash(name);
    for (size_t i = 0; i < v->header->n_columns; i++) {
        if (v->columns[i].name_hash == nh) {
            return (dtype_t)v->columns[i].dtype;
        }
    }
    return DT_INVALID;
}

const char* av_metadata_json(const artifact_view_t* v, size_t* out_len) {
    if (!v || v->header->metadata_len == 0) {
        if (out_len)
            *out_len = 0;
        return NULL;
    }
    if (out_len)
        *out_len = (size_t)v->header->metadata_len;
    return (const char*)(v->base + v->header->metadata_offset);
}

hash_t av_hash(const artifact_view_t* v) {
    if (!v) {
        hash_t zero = {{0}};
        return zero;
    }
    return hash_bytes(v->base, v->total_len);
}

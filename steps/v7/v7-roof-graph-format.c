// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-roof-graph-format — TLV reader + writer implementation.
// See v7-roof-graph-format.h for the public contract.
// Spec: v3/docs/plan/ROOF-GRAPH-FORMAT.md.

#include "v7-roof-graph-format.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "v3/safe_alloc.h"  // safe_malloc_array — used by rg_parse_padded_u32_array (#72b)

// ── Endian helpers ──────────────────────────────────────────────────
//
// Wire format is little-endian. Most platforms we run on are LE so
// these are memcpy-noops, but stay portable.

static inline uint32_t le_load_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t le_load_u64(const uint8_t* p) {
    return (uint64_t)le_load_u32(p) | ((uint64_t)le_load_u32(p + 4) << 32);
}
static inline void le_store_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v); p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline void le_store_u64(uint8_t* p, uint64_t v) {
    le_store_u32(p, (uint32_t)v);
    le_store_u32(p + 4, (uint32_t)(v >> 32));
}

static inline size_t align_up_8(size_t n) { return (n + 7) & ~(size_t)7; }

// ── Reader ──────────────────────────────────────────────────────────

int rg_reader_open(rg_reader_t* r, const uint8_t* bytes, size_t len) {
    if (!r || !bytes) return -2;
    memset(r, 0, sizeof(*r));
    if (len < RG_FILE_HEADER_BYTES) return -2;
    if (memcmp(bytes, RG_MAGIC, 8) != 0) return -1;
    uint32_t flags = le_load_u32(bytes + 8);
    uint32_t n     = le_load_u32(bytes + 12);
    if (flags != 0) return -2;
    size_t dir_len = (size_t)n * RG_DIR_ENTRY_BYTES;
    if (RG_FILE_HEADER_BYTES + dir_len > len) return -2;

    // Validate every directory entry's offset+length lies within the file.
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t* e = bytes + RG_FILE_HEADER_BYTES + (size_t)i * RG_DIR_ENTRY_BYTES;
        uint64_t off = le_load_u64(e + 8);
        uint64_t L   = le_load_u64(e + 16);
        if (off > (uint64_t)len || off + L > (uint64_t)len) return -2;
        if (off < RG_FILE_HEADER_BYTES + dir_len) return -2;
    }

    // Reject duplicate type IDs.
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t* e_i = bytes + RG_FILE_HEADER_BYTES + (size_t)i * RG_DIR_ENTRY_BYTES;
        uint32_t t_i = le_load_u32(e_i);
        for (uint32_t j = i + 1; j < n; j++) {
            const uint8_t* e_j = bytes + RG_FILE_HEADER_BYTES + (size_t)j * RG_DIR_ENTRY_BYTES;
            uint32_t t_j = le_load_u32(e_j);
            if (t_i == t_j) return -2;
        }
    }

    r->bytes      = bytes;
    r->len        = len;
    r->n_sections = n;
    r->dir        = bytes + RG_FILE_HEADER_BYTES;
    return 0;
}

int rg_reader_section(const rg_reader_t* r, uint32_t type,
                      const uint8_t** out_bytes, size_t* out_len) {
    if (out_bytes) *out_bytes = NULL;
    if (out_len)   *out_len = 0;
    if (!r) return -1;
    for (uint32_t i = 0; i < r->n_sections; i++) {
        const uint8_t* e = r->dir + (size_t)i * RG_DIR_ENTRY_BYTES;
        uint32_t t = le_load_u32(e);
        if (t != type) continue;
        uint64_t off = le_load_u64(e + 8);
        uint64_t L   = le_load_u64(e + 16);
        if (off + L > (uint64_t)r->len) return -1;
        if (out_bytes) *out_bytes = r->bytes + off;
        if (out_len)   *out_len   = (size_t)L;
        return 0;
    }
    return 1;
}

int rg_reader_iter(const rg_reader_t* r, uint32_t* iter,
                   uint32_t* out_type,
                   const uint8_t** out_bytes, size_t* out_len) {
    if (!r || !iter) return -1;
    if (*iter >= r->n_sections) return 1;
    const uint8_t* e = r->dir + (size_t)(*iter) * RG_DIR_ENTRY_BYTES;
    uint32_t t   = le_load_u32(e);
    uint64_t off = le_load_u64(e + 8);
    uint64_t L   = le_load_u64(e + 16);
    if (off + L > (uint64_t)r->len) return -1;
    if (out_type)  *out_type  = t;
    if (out_bytes) *out_bytes = r->bytes + off;
    if (out_len)   *out_len   = (size_t)L;
    (*iter)++;
    return 0;
}

int rg_section_header(const uint8_t* sect_bytes, size_t sect_len,
                      uint32_t* out_n_records, uint32_t* out_record_stride) {
    if (!sect_bytes || sect_len < RG_SECTION_HEADER_BYTES) return -1;
    if (out_n_records)     *out_n_records = le_load_u32(sect_bytes);
    if (out_record_stride) *out_record_stride = le_load_u32(sect_bytes + 4);
    return 0;
}

// ── Writer ──────────────────────────────────────────────────────────

void rg_writer_init(rg_writer_t* w) {
    memset(w, 0, sizeof(*w));
}

void rg_writer_free(rg_writer_t* w) {
    if (!w) return;
    free(w->dir);
    free(w->payload);
    memset(w, 0, sizeof(*w));
}

static int payload_reserve(rg_writer_t* w, size_t extra) {
    if (w->payload_len + extra <= w->payload_cap) return 0;
    size_t nc = w->payload_cap ? w->payload_cap * 2 : 4096;
    while (nc < w->payload_len + extra) nc *= 2;
    uint8_t* p = (uint8_t*)realloc(w->payload, nc);
    if (!p) return -1;
    w->payload     = p;
    w->payload_cap = nc;
    return 0;
}

static int dir_reserve(rg_writer_t* w) {
    if (w->n_dir < w->dir_cap) return 0;
    size_t nc = w->dir_cap ? w->dir_cap * 2 : 8;
    void* p = realloc(w->dir, nc * sizeof(*w->dir));
    if (!p) return -1;
    w->dir     = p;
    w->dir_cap = nc;
    return 0;
}

int rg_writer_section_begin(rg_writer_t* w, uint32_t type, uint32_t record_stride) {
    if (!w) return -1;
    if (w->in_section) return -1;
    if (type == RG_SECT_NONE) return -1;
    // Reject duplicate type — must be unique per file.
    for (size_t i = 0; i < w->n_dir; i++) {
        if (w->dir[i].type == type) return -2;
    }
    if (payload_reserve(w, RG_SECTION_HEADER_BYTES) != 0) return -1;
    w->cur_type             = type;
    w->cur_record_stride    = record_stride;
    w->cur_section_start    = (uint64_t)w->payload_len;
    w->cur_n_records        = 0;
    // Reserve section header (fill on section_end).
    memset(w->payload + w->payload_len, 0, RG_SECTION_HEADER_BYTES);
    w->payload_len += RG_SECTION_HEADER_BYTES;
    w->in_section = true;
    return 0;
}

int rg_writer_record(rg_writer_t* w, const void* bytes, size_t len) {
    if (!w || !w->in_section) return -1;
    if (w->cur_record_stride != 0 && len != w->cur_record_stride) return -1;
    if (w->cur_record_stride != 0) {
        if (payload_reserve(w, len) != 0) return -1;
        memcpy(w->payload + w->payload_len, bytes, len);
        w->payload_len += len;
    } else {
        // Variable-length: write {u32 len, u32 zero_pad, payload, pad_to_8}
        size_t padded = align_up_8(len);
        if (padded < len) return -1;  // overflow
        if (payload_reserve(w, 8 + padded) != 0) return -1;
        le_store_u32(w->payload + w->payload_len, (uint32_t)len);
        le_store_u32(w->payload + w->payload_len + 4, 0);
        w->payload_len += 8;
        memcpy(w->payload + w->payload_len, bytes, len);
        if (padded > len) {
            memset(w->payload + w->payload_len + len, 0, padded - len);
        }
        w->payload_len += padded;
    }
    w->cur_n_records++;
    return 0;
}

int rg_writer_section_end(rg_writer_t* w) {
    if (!w || !w->in_section) return -1;
    // Fill the deferred section header.
    le_store_u32(w->payload + w->cur_section_start,     w->cur_n_records);
    le_store_u32(w->payload + w->cur_section_start + 4, w->cur_record_stride);
    // Pad payload to 8-byte boundary so next section starts aligned.
    size_t pad = align_up_8(w->payload_len) - w->payload_len;
    if (pad > 0) {
        if (payload_reserve(w, pad) != 0) return -1;
        memset(w->payload + w->payload_len, 0, pad);
        w->payload_len += pad;
    }
    // Append directory entry.
    if (dir_reserve(w) != 0) return -1;
    uint64_t length = (uint64_t)w->payload_len - w->cur_section_start - pad;
    w->dir[w->n_dir].type   = w->cur_type;
    w->dir[w->n_dir].offset = w->cur_section_start;  // to be biased on finalize
    w->dir[w->n_dir].length = length;
    w->n_dir++;
    w->in_section = false;
    return 0;
}

int rg_writer_passthrough_n(rg_writer_t* w, const rg_reader_t* upstream,
                            const uint32_t* skip_types, size_t n_skip) {
    if (!w || !upstream) return -1;
    if (w->in_section) return -1;
    uint32_t iter = 0;
    while (1) {
        uint32_t type;
        const uint8_t* sect_bytes;
        size_t sect_len;
        int rc = rg_reader_iter(upstream, &iter, &type, &sect_bytes, &sect_len);
        if (rc == 1) break;
        if (rc < 0) return -1;
        bool skip = false;
        for (size_t i = 0; i < n_skip; i++) {
            if (skip_types[i] == type) { skip = true; break; }
        }
        if (skip) continue;
        // Reject if already-present (caller bug — should be the FIRST writer call).
        for (size_t j = 0; j < w->n_dir; j++) {
            if (w->dir[j].type == type) return -2;
        }
        // Copy bytes verbatim.
        if (dir_reserve(w) != 0) return -1;
        size_t pad = align_up_8(sect_len) - sect_len;
        if (payload_reserve(w, sect_len + pad) != 0) return -1;
        size_t start = w->payload_len;
        memcpy(w->payload + start, sect_bytes, sect_len);
        w->payload_len += sect_len;
        if (pad > 0) {
            memset(w->payload + w->payload_len, 0, pad);
            w->payload_len += pad;
        }
        w->dir[w->n_dir].type   = type;
        w->dir[w->n_dir].offset = (uint64_t)start;
        w->dir[w->n_dir].length = (uint64_t)sect_len;
        w->n_dir++;
    }
    return 0;
}

int rg_writer_passthrough(rg_writer_t* w, const rg_reader_t* upstream,
                          uint32_t skip_type) {
    if (skip_type == RG_SECT_NONE) {
        return rg_writer_passthrough_n(w, upstream, NULL, 0);
    }
    uint32_t skips[1] = { skip_type };
    return rg_writer_passthrough_n(w, upstream, skips, 1);
}

int rg_writer_finalize(rg_writer_t* w, uint8_t** out_bytes, size_t* out_len) {
    if (!w || w->in_section) return -1;
    if (out_bytes) *out_bytes = NULL;
    if (out_len)   *out_len   = 0;

    size_t hdr_len = RG_FILE_HEADER_BYTES;
    size_t dir_len = w->n_dir * RG_DIR_ENTRY_BYTES;
    size_t prefix  = hdr_len + dir_len;
    // Pad prefix to 8 — header+dir entries are already 8-aligned but be defensive.
    size_t prefix_padded = align_up_8(prefix);
    size_t total = prefix_padded + w->payload_len;
    uint8_t* buf = (uint8_t*)malloc(total ? total : 1);
    if (!buf) return -1;

    // File header.
    memcpy(buf, RG_MAGIC, 8);
    le_store_u32(buf + 8, 0);                  // flags
    le_store_u32(buf + 12, (uint32_t)w->n_dir);
    memset(buf + 16, 0, 16);                   // reserved

    // Directory — bias each entry's offset by prefix_padded.
    for (size_t i = 0; i < w->n_dir; i++) {
        uint8_t* e = buf + RG_FILE_HEADER_BYTES + i * RG_DIR_ENTRY_BYTES;
        le_store_u32(e,     w->dir[i].type);
        le_store_u32(e + 4, 0);                // flags
        le_store_u64(e + 8, w->dir[i].offset + prefix_padded);
        le_store_u64(e + 16, w->dir[i].length);
    }
    // Pad between dir and payload (if any).
    if (prefix_padded > prefix) {
        memset(buf + prefix, 0, prefix_padded - prefix);
    }

    if (w->payload_len > 0) {
        memcpy(buf + prefix_padded, w->payload, w->payload_len);
    }

    if (out_bytes) *out_bytes = buf;
    if (out_len)   *out_len   = total;
    return 0;
}

// ── Generic record iterator ─────────────────────────────────────────

int rg_record_iter_init(rg_record_iter_t* it,
                        const uint8_t* sect_bytes, size_t sect_len) {
    if (!it || !sect_bytes || sect_len < RG_SECTION_HEADER_BYTES) return -1;
    memset(it, 0, sizeof(*it));
    it->sect_bytes    = sect_bytes;
    it->sect_len      = sect_len;
    it->n_records     = le_load_u32(sect_bytes);
    it->record_stride = le_load_u32(sect_bytes + 4);
    it->off           = RG_SECTION_HEADER_BYTES;
    it->i             = 0;
    return 0;
}

int rg_record_iter_next(rg_record_iter_t* it,
                        const uint8_t** out_record, uint32_t* out_record_len) {
    if (!it) return 0;
    if (it->i >= it->n_records) return 0;
    if (it->record_stride != 0) {
        if (it->off + it->record_stride > it->sect_len) return 0;
        if (out_record)     *out_record = it->sect_bytes + it->off;
        if (out_record_len) *out_record_len = it->record_stride;
        it->off += it->record_stride;
    } else {
        if (it->off + 8 > it->sect_len) return 0;
        uint32_t L = le_load_u32(it->sect_bytes + it->off);
        // u32 _pad at off+4 is ignored.
        size_t payload_off = it->off + 8;
        if (payload_off + L > it->sect_len) return 0;
        if (out_record)     *out_record = it->sect_bytes + payload_off;
        if (out_record_len) *out_record_len = L;
        it->off = payload_off + align_up_8(L);
    }
    it->i++;
    return 1;
}

// ── Variable-length record subsection parser (task #72b) ──────────────
//
// See header for the full contract. Uses safe_malloc_array — same
// overflow guard the original 4 inline sites used (well, 3 — bp2 used
// plain malloc; this helper upgrades it).

int rg_parse_padded_u32_array(const uint8_t* rec, size_t* off,
                              uint32_t** out_arr, uint32_t* out_n) {
    *off = (*off + 7) & ~(size_t)7;            // pre-align
    uint32_t n;
    memcpy(&n, rec + *off, sizeof n);
    *off += 8;                                  // count is padded to 8
    *out_n   = n;
    *out_arr = NULL;
    if (n > 0) {
        uint32_t* arr = (uint32_t*)safe_malloc_array(n, sizeof(uint32_t));
        if (!arr) return -1;
        memcpy(arr, rec + *off, (size_t)n * 4);
        *off += (size_t)n * 4;
        *out_arr = arr;
    }
    *off = (*off + 7) & ~(size_t)7;            // post-align
    return 0;
}

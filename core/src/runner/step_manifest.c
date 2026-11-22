// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#include "step_manifest.h"

#include <stdlib.h>
#include <string.h>

static void write_u32_le(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t read_u32_le(const uint8_t* p) {
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

int step_manifest_pack(const step_manifest_entry_t* entries, size_t n, uint8_t** out_bytes,
                       size_t* out_len) {
    if (!out_bytes || !out_len) {
        return -1;
    }
    *out_bytes = NULL;
    *out_len = 0;

    size_t total = 4; // u32 n_entries
    for (size_t i = 0; i < n; i++) {
        if (!entries[i].name) {
            return -1;
        }
        total += 4 + strlen(entries[i].name) + sizeof(hash_t);
    }
    uint8_t* buf = malloc(total > 0 ? total : 1);
    if (!buf) {
        return -1;
    }
    size_t off = 0;
    write_u32_le(buf + off, (uint32_t)n);
    off += 4;
    for (size_t i = 0; i < n; i++) {
        size_t nlen = strlen(entries[i].name);
        write_u32_le(buf + off, (uint32_t)nlen);
        off += 4;
        memcpy(buf + off, entries[i].name, nlen);
        off += nlen;
        memcpy(buf + off, entries[i].content_hash.bytes, sizeof(hash_t));
        off += sizeof(hash_t);
    }
    *out_bytes = buf;
    *out_len = total;
    return 0;
}

int step_manifest_walk(const uint8_t* bytes, size_t len, step_manifest_cb cb, void* ctx) {
    if (!bytes || len < 4 || !cb) {
        return -1;
    }
    uint32_t n = read_u32_le(bytes);
    size_t off = 4;
    for (uint32_t i = 0; i < n; i++) {
        if (off + 4 > len) {
            return -1;
        }
        uint32_t nlen = read_u32_le(bytes + off);
        off += 4;
        if (off + nlen + sizeof(hash_t) > len) {
            return -1;
        }
        const char* name = (const char*)(bytes + off);
        off += nlen;
        hash_t h;
        memcpy(h.bytes, bytes + off, sizeof(hash_t));
        off += sizeof(hash_t);
        if (cb(ctx, name, nlen, &h) != 0) {
            return -1;
        }
    }
    if (off != len) {
        return -1;
    }
    return 0;
}

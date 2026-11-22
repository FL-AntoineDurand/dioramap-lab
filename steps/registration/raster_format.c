// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#include "raster_format.h"

#include <stdlib.h>
#include <string.h>

static uint32_t read_u32_le(const uint8_t* p) {
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void write_u32_le(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

int raster_view_open(const uint8_t* bytes, size_t len, raster_view_t* out) {
    if (!bytes || !out || len < RASTER_HEADER_BYTES) {
        return -1;
    }
    out->cols = read_u32_le(bytes);
    out->rows = read_u32_le(bytes + 4);
    out->n_channels = read_u32_le(bytes + 8);
    if (out->cols == 0 || out->rows == 0 || out->n_channels == 0 ||
        out->n_channels > 16) {
        return -1;
    }
    // Frama-C audit finding REG-1: bound (cols * rows * n_channels * sizeof(float))
    // before computing raster_total_bytes — otherwise an attacker-crafted set
    // of dimensions can wrap size_t, satisfy the `len == expected` check on a
    // tiny len, and let `channels[i] = base + i * cells` index anywhere.
    if (out->cols > SIZE_MAX / out->rows) return -1;                       // cells overflow
    size_t cells = (size_t)out->cols * out->rows;
    if (cells > (SIZE_MAX - RASTER_HEADER_BYTES) /
                (out->n_channels * sizeof(float))) {
        return -1;                                                          // total_bytes overflow
    }
    size_t expected = raster_total_bytes(out->cols, out->rows, out->n_channels);
    if (len != expected) {
        return -1;
    }
    const float* base = (const float*)(bytes + RASTER_HEADER_BYTES);
    for (uint32_t i = 0; i < out->n_channels; i++) {
        out->channels[i] = base + i * cells;
    }
    for (uint32_t i = out->n_channels; i < 16; i++) {
        out->channels[i] = NULL;
    }
    return 0;
}

uint8_t* raster_alloc(uint32_t cols, uint32_t rows, uint32_t n_channels) {
    if (cols == 0 || rows == 0 || n_channels == 0 || n_channels > 16) {
        return NULL;
    }
    size_t total = raster_total_bytes(cols, rows, n_channels);
    uint8_t* buf = malloc(total);
    if (!buf) {
        return NULL;
    }
    write_u32_le(buf, cols);
    write_u32_le(buf + 4, rows);
    write_u32_le(buf + 8, n_channels);
    // Don't pre-fill channel data — the caller writes per-cell, including
    // any NaN sentinel. memset to zero would be misleading for per-cell
    // "no underlying data" semantics; force the caller to be explicit.
    return buf;
}

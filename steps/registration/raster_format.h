// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Multi-channel raster artifact format used by the B5 registration chain.
//
// On-disk layout (packed, little-endian — same byte order across all v3
// artifacts since we only target x86-64 + ARM64 LE):
//
//   u32 cols
//   u32 rows
//   u32 n_channels
//   F32[cols * rows]  channel 0
//   F32[cols * rows]  channel 1
//   ...
//   F32[cols * rows]  channel n_channels-1
//
// Channels are stored row-major, row 0 at L93 ymin (south) and column 0 at
// L93 xmin (west). NaN is the no-data sentinel for cells that received no
// underlying input. Channel order is fixed per producing step — see each
// step's writes contract.
//
// Total byte size = 12 + n_channels * cols * rows * 4.
//
// Consumers parse via `raster_view_open(bytes, len, &v)` then access channels
// directly through the returned pointers (zero-copy from the store's hot or
// warm tier).

#ifndef V3_STEPS_REGISTRATION_RASTER_FORMAT_H
#define V3_STEPS_REGISTRATION_RASTER_FORMAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RASTER_HEADER_BYTES 12

// View into a raster artifact's bytes. All pointers borrow from the
// caller-supplied buffer.
typedef struct raster_view {
    uint32_t cols;
    uint32_t rows;
    uint32_t n_channels;
    const float* channels[16]; // up to 16 channels per artifact (plenty)
} raster_view_t;

// Parse a raster artifact and populate `out`. Returns 0 on success, -1 on
// header truncation / size mismatch / too many channels.
int raster_view_open(const uint8_t* bytes, size_t len, raster_view_t* out);

// Total bytes for a raster of given shape.
static inline size_t raster_total_bytes(uint32_t cols, uint32_t rows, uint32_t n_channels) {
    return RASTER_HEADER_BYTES + (size_t)cols * rows * n_channels * sizeof(float);
}

// Allocate a heap buffer of the right size, write the header. Returns NULL
// on alloc failure or if n_channels > 16.
//
// Caller fills the channel data starting at `out_buffer + RASTER_HEADER_BYTES`,
// channels back-to-back. Passes the buffer to `step_write_output_owned`
// (or frees it) when done.
uint8_t* raster_alloc(uint32_t cols, uint32_t rows, uint32_t n_channels);

// Per-channel pointer arithmetic — returns &buffer[channel start].
static inline float* raster_channel_ptr(uint8_t* buffer, uint32_t cols, uint32_t rows,
                                        uint32_t channel_idx) {
    return (float*)(buffer + RASTER_HEADER_BYTES) + (size_t)channel_idx * cols * rows;
}

#ifdef __cplusplus
}
#endif

#endif

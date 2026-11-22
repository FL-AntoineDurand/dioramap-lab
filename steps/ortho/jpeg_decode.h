// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// JPEG decode + bilinear resample helper for the ortho-raster steps.
//
// Wraps libjpeg-turbo. Decodes JPEG bytes in memory to a heap RGBA buffer
// at the source resolution; a separate helper resamples that source RGBA
// down to a target (cols, rows) grid using bilinear interpolation, with
// optional Y-flip so the output's row 0 corresponds to L93 ymin (south)
// — matching the orientation convention used by the LiDAR rasters.

#ifndef V3_STEPS_ORTHO_JPEG_DECODE_H
#define V3_STEPS_ORTHO_JPEG_DECODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct decoded_image {
    uint8_t* rgba;        // owned heap buffer, length = w * h * 4
    int w;
    int h;
} decoded_image_t;

// Decode JPEG bytes → RGBA at the source resolution. The alpha channel is
// always set to 255. Returns 0 on success (`out->rgba` owned), -1 on
// libjpeg parse error / OOM (`out` cleared).
int jpeg_decode_rgba(const uint8_t* bytes, size_t len, decoded_image_t* out);

void decoded_image_free(decoded_image_t* img);

// Resample `src` (source RGBA, src_w × src_h) to a freshly-allocated heap
// buffer at (dst_w × dst_h × 4 = dst_w * dst_h * 4) using bilinear
// interpolation. If `flip_y` is true, the destination row 0 reads from the
// source's bottom row (L93 ymin orientation).
//
// Returns the heap buffer on success, NULL on OOM. Caller frees with `free()`.
uint8_t* jpeg_resample_rgba(const uint8_t* src, int src_w, int src_h,
                            int dst_w, int dst_h, bool flip_y);

#ifdef __cplusplus
}
#endif

#endif

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
#ifndef V3_LAZPERF_H
#define V3_LAZPERF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Thin C wrapper over copc-lib + laz-perf. See PLAN-VIEWER-PORT §8.
//
// Given a whole COPC-LAZ tile in memory, decompress the nodes that overlap
// a bbox_l93 rectangle and append the points whose (x,y) falls inside a
// disc into caller-provided growable buffers.
//
// The caller builds one output struct per tile (or reuses across tiles to
// collect a union). We own the typed-array memory; caller frees via
// laz_pointbuf_free.

typedef struct laz_pointbuf {
    // Positions in Lambert-93 metres, interleaved (x, y, z, x, y, z, ...).
    double* xyz;
    size_t n_points;
    size_t cap_points;
    // Per-point columns (length n_points each).
    uint8_t* classifications;
    uint16_t* intensities;
    uint8_t* num_returns;
} laz_pointbuf_t;

void laz_pointbuf_init(laz_pointbuf_t* pb);
void laz_pointbuf_free(laz_pointbuf_t* pb);

// Decompress nodes of the COPC tile (bytes / len) that intersect
// bbox_l93 = {xmin, ymin, xmax, ymax}, and append points whose (x,y) lies
// inside the disc centred at (disc_cx, disc_cy) with radius disc_r.
// Appends to `out` — repeat calls across multiple tiles accumulate.
// max_depth bounds the octree descent (deeper = more points; typical 8).
// Returns 0 on success, -1 on parse / decompress error.
int laz_extract_disc_from_tile(const uint8_t* bytes, size_t len,
                               const double bbox_l93[4],
                               double disc_cx, double disc_cy, double disc_r,
                               int max_depth,
                               laz_pointbuf_t* out);

#ifdef __cplusplus
}
#endif
#endif

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-perbldg-gather.c — see header for rationale.
#include "v7-perbldg-gather.h"

#include <stdlib.h>
#include <string.h>

// building-local-frames record layout (matches the BLF_RECORD_BYTES macro
// duplicated in build-building-kd-trees.c / v7-boundary-classify.c et al;
// invariant 72-byte record: centroid (3×8B doubles) + per-axis basis or pad).
#define V7_BLF_RECORD_BYTES 72

int v7_gather_building_points_f32_f64(
    const double* positions, size_t total_points,
    const uint32_t* gidx, uint32_t n_pts,
    double cx, double cy, double cz,
    float** local_f32, double** local_f64, size_t* local_cap)
{
    const size_t need = (size_t)n_pts * 3;
    if (need > *local_cap) {
        // Realloc the F32 buffer FIRST and commit to *local_f32 immediately on
        // success — that's how we avoid the pre-#72a latent double-free
        // (header comment).
        float* p32 = (float*)realloc(*local_f32, need * sizeof(float));
        if (!p32) return -1;
        *local_f32 = p32;

        double* p64 = (double*)realloc(*local_f64, need * sizeof(double));
        if (!p64) return -1;   // *local_f32 = p32 already; caller's free(*local_f32) is safe
        *local_f64 = p64;

        *local_cap = need;
    }

    float* f32 = *local_f32;
    for (uint32_t i = 0; i < n_pts; i++) {
        uint32_t g = gidx[i];
        if ((size_t)g >= total_points) {
            f32[3 * i + 0] = f32[3 * i + 1] = f32[3 * i + 2] = 0.0f;
        } else {
            f32[3 * i + 0] = (float)(positions[3 * g + 0] - cx);
            f32[3 * i + 1] = (float)(positions[3 * g + 1] - cy);
            f32[3 * i + 2] = (float)(positions[3 * g + 2] - cz);
        }
    }
    double* f64 = *local_f64;
    for (size_t i = 0; i < need; i++) {
        f64[i] = (double)f32[i];
    }
    return 0;
}

int v7_per_bldg_load_frame(step_context_t* ctx, size_t bi,
                           const uint32_t** out_gidx, uint32_t* out_n_pts,
                           double* out_cx, double* out_cy, double* out_cz)
{
    // ── per-building-records: schema (4B) + n_verts (4B) + n_pts (4B) +
    //    ... (header pad to 68B) + per-vertex bytes + gidx[n_pts] (4B each).
    const uint8_t* rec = NULL; size_t rec_len = 0;
    if (step_array_read_bytes(ctx, "per-building-records", bi, &rec, &rec_len) != 0
        || !rec || rec_len < 68) return -1;
    uint32_t schema, n_verts, n_pts;
    memcpy(&schema,  rec + 0, 4);
    memcpy(&n_verts, rec + 4, 4);
    memcpy(&n_pts,   rec + 8, 4);
    const size_t fp_bytes = (size_t)n_verts * 24;
    const size_t idx_off  = 68 + fp_bytes;
    if (schema != 1 || idx_off + (size_t)n_pts * 4 > rec_len) return -1;

    // ── building-local-frames: 72B record, centroid is the leading 3×8B.
    const uint8_t* frame = NULL; size_t frame_len = 0;
    if (step_array_read_bytes(ctx, "building-local-frames", bi, &frame, &frame_len) != 0
        || !frame || frame_len < V7_BLF_RECORD_BYTES) return -1;

    *out_gidx  = (const uint32_t*)(rec + idx_off);
    *out_n_pts = n_pts;
    memcpy(out_cx, frame +  0, 8);
    memcpy(out_cy, frame +  8, 8);
    memcpy(out_cz, frame + 16, 8);
    return 0;
}

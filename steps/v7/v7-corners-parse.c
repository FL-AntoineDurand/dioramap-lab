// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-corners-parse.c — see header for rationale.
#include "v7-corners-parse.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "v3/safe_alloc.h"

int v7_parse_corners_section(rg_reader_t* r,
                             v7_bm_input_corner_t** corners, size_t* n_corners,
                             double cx, double cy, double cz)
{
    *corners = NULL;
    *n_corners = 0;
    const uint8_t* sect; size_t slen;
    if (rg_reader_section(r, RG_SECT_CORNERS, &sect, &slen) != 0) return 0;
    rg_record_iter_t it;
    if (rg_record_iter_init(&it, sect, slen) != 0) return 0;

    size_t cap = 16;
    *corners = (v7_bm_input_corner_t*)safe_calloc_array(cap, sizeof(**corners));
    if (!*corners) return -1;
    const uint8_t* rec; uint32_t rec_len;
    while (rg_record_iter_next(&it, &rec, &rec_len)) {
        if (rec_len < 32) continue;
        if (*n_corners == cap) {
            cap *= 2;
            v7_bm_input_corner_t* nb = (v7_bm_input_corner_t*)safe_realloc_array(
                *corners, cap, sizeof(*nb));
            if (!nb) return -1;
            memset(nb + *n_corners, 0, (cap - *n_corners) * sizeof(*nb));
            *corners = nb;
        }
        v7_bm_input_corner_t* C = &(*corners)[(*n_corners)++];
        memcpy(&C->id, rec + 0, 4);
        C->source = rec[4];
        uint8_t flags = rec[5];
        double pw[3];
        memcpy(&pw[0], rec + 8,  8);
        memcpy(&pw[1], rec + 16, 8);
        memcpy(&pw[2], rec + 24, 8);
        // Centroid-subtract then z-up → y-up axis swap inline:
        //   (X, Y, Z) → (X, Z, -Y)
        const double lx = pw[0] - cx;
        const double ly = pw[1] - cy;
        const double lz = pw[2] - cz;
        C->point[0] = lx;
        C->point[1] = lz;
        C->point[2] = -ly;
        size_t off = 32;
        uint32_t pc; memcpy(&pc, rec + off, 4); off += 8;
        if (pc > 0) {
            C->plane_ids = (uint32_t*)safe_malloc_array(pc, sizeof(uint32_t));
            if (!C->plane_ids) return -1;
            memcpy(C->plane_ids, rec + off, pc * 4);
            C->n_plane_ids = pc;
            off += pc * 4;
            C->source_per_plane = (uint8_t*)malloc(pc);
            if (!C->source_per_plane) return -1;
            memcpy(C->source_per_plane, rec + off, pc);
            off += pc;
        }
        if (rg_parse_padded_u32_array(rec, &off, &C->ridge_ids, &C->n_ridge_ids) != 0) return -1;
        if (flags & 0x01u) {
            uint32_t nl; memcpy(&nl, rec + off, 4); off += 8;
            if (nl > 0) {
                C->name = (char*)malloc(nl + 1);
                if (!C->name) return -1;
                memcpy(C->name, rec + off, nl); C->name[nl] = '\0';
                off += (nl + 7) & ~(size_t)7;
            }
        }
        // dirs (0x04) / com_bisectors (0x20) / members (0x40) tails — both
        // downstream callers skip these. We don't even advance off past
        // them because the iterator moves to the next record by stride,
        // not by our local offset.
    }
    return 0;
}

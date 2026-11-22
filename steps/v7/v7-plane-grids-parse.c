// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-plane-grids-parse.c — see header for rationale.
#include "v7-plane-grids-parse.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// Append a heap pointer to a growable ptr-array. Returns 0 / -1.
static int track(float*** arr, size_t* n, float* p) {
    float** np = (float**)realloc(*arr, (*n + 1) * sizeof(float*));
    if (!np) return -1;
    *arr = np;
    (*arr)[*n] = p;
    (*n)++;
    return 0;
}

int v7_parse_plane_grids_section(rg_reader_t* r,
                                 v7_bc_input_plane_grid_t** grids, size_t* n_grids,
                                 float*** owned_oa,   size_t* n_owned_oa,
                                 float*** owned_outu, size_t* n_owned_outu,
                                 float*** owned_outv, size_t* n_owned_outv,
                                 float*** owned_bal,  size_t* n_owned_bal)
{
    const uint8_t* sect; size_t slen;
    if (rg_reader_section(r, RG_SECT_PLANE_GRIDS, &sect, &slen) != 0) return 0;
    rg_record_iter_t it;
    if (rg_record_iter_init(&it, sect, slen) != 0) return 0;

    size_t cap = (*grids != NULL) ? (*n_grids > 8 ? *n_grids : 8) : 8;
    if (*grids == NULL) {
        *grids = (v7_bc_input_plane_grid_t*)calloc(cap, sizeof(**grids));
        if (!*grids) return -1;
    }
    const uint8_t* rec; uint32_t rec_len;
    while (rg_record_iter_next(&it, &rec, &rec_len)) {
        if (rec_len < 136) continue;
        uint32_t flags; memcpy(&flags, rec + 4, 4);
        bool has_group = (flags & 0x01u) != 0;
        bool has_bal   = (flags & 0x04u) != 0;
        // Skip viz-only nearby-grid records — only group/bal carry data
        // the boundary-corners + bp2 callers consume.
        if (!has_group && !has_bal) continue;
        if (*n_grids == cap) {
            cap *= 2;
            v7_bc_input_plane_grid_t* nb = (v7_bc_input_plane_grid_t*)realloc(
                *grids, cap * sizeof(*nb));
            if (!nb) return -1;
            memset(nb + *n_grids, 0, (cap - *n_grids) * sizeof(*nb));
            *grids = nb;
        }
        v7_bc_input_plane_grid_t* G = &(*grids)[(*n_grids)++];
        memcpy(&G->plane_id, rec + 0, 4);
        uint32_t n_cols, n_rows;
        memcpy(&n_cols, rec + 8, 4); memcpy(&n_rows, rec + 12, 4);
        float cell_size, g_min_u, g_min_v;
        memcpy(&cell_size, rec + 16, 4);
        memcpy(&g_min_u,   rec + 20, 4);
        memcpy(&g_min_v,   rec + 24, 4);
        size_t n_cells = (size_t)n_cols * n_rows;
        size_t off = 136;
        if (has_group && n_cells > 0) {
            // Wire layout per RG §4.5 PLANE_GRIDS:
            //   oa, oaSpan, comDist  (3 × f32 × n_cells)
            //   ptCount (u16 × n_cells)
            //   [pad 2B if n_cells odd]
            //   outU, outV, bestLeft, bestRight, nodeX, nodeY, nodeZ
            const size_t f32n = n_cells * 4;
            float* oa = (float*)malloc(f32n);
            if (!oa) return -1;
            memcpy(oa, rec + off, f32n);
            size_t after_pre = off + 3 * f32n + n_cells * 2;
            if (n_cells & 1) after_pre += 2;
            float* outu = (float*)malloc(f32n);
            float* outv = (float*)malloc(f32n);
            if (!outu || !outv) { free(oa); free(outu); free(outv); return -1; }
            memcpy(outu, rec + after_pre + 0 * f32n, f32n);
            memcpy(outv, rec + after_pre + 1 * f32n, f32n);
            if (track(owned_oa,   n_owned_oa,   oa)   != 0) { free(outu); free(outv); return -1; }
            if (track(owned_outu, n_owned_outu, outu) != 0) { free(outv); return -1; }
            if (track(owned_outv, n_owned_outv, outv) != 0) return -1;
            G->has_group = true;
            G->group_field.n_cols    = n_cols;
            G->group_field.n_rows    = n_rows;
            G->group_field.cell_size = cell_size;
            G->group_field.g_min_u   = g_min_u;
            G->group_field.g_min_v   = g_min_v;
            G->group_field.oa_field  = oa;
            G->group_field.out_u     = outu;
            G->group_field.out_v     = outv;
            size_t group_bytes = n_cells * (10 * 4 + 2);
            if (n_cells & 1) group_bytes += 2;
            group_bytes = (group_bytes + 7) & ~(size_t)7;
            off += group_bytes;
        }
        if (has_bal && n_cells > 0) {
            const size_t f32n = n_cells * 4;
            float* bal = (float*)malloc(f32n);
            if (!bal) return -1;
            memcpy(bal, rec + off, f32n);
            if (track(owned_bal, n_owned_bal, bal) != 0) return -1;
            G->has_bal   = true;
            G->bal_field = bal;
        }
    }
    return 0;
}

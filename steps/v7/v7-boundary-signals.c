// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-boundary-signals — step wrapper. Second sub-step of split
// boundary-signals; consumes v7-boundary-topology's TLV, passes its
// sections through verbatim, and appends:
//
//   - CORNERS (0x06)             — RI corners re-derived from
//                                  upstream RIDGE_LINES. Phase C
//                                  concave/convex anchors land
//                                  in a follow-up commit.
//   - NEARBY_PROJECTIONS (0x04)  — per-plane filtered beam-projected
//                                  non-group points (|sd|>0.20m).
//
// Per ROOF-GRAPH-FORMAT.md, the upstream PLANES/RIDGES/RIDGE_LINES
// sections are copied byte-for-byte via rg_writer_passthrough.
//
// Reads:
//   lidar-positions                       (F64×3·N tile-wide)
//   bi-directions                         (F32×3·N tile-wide, beam dirs)
//   per-building-records                  (per-bldg)
//   building-local-frames                 (per-bldg — centroid for shift)
//   grm-owner                             (per-bldg Int16)
//   boundary-topology-roof-graph          (per-bldg ARRAY_INLINE of TLV)
//
// Writes:
//   boundary-signals-roof-graph           (per-bldg ARRAY_INLINE of TLV)

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jansson.h>

#include "v3/artifact.h"
#include "v3/json_int.h"
#include "v3/blob.h"
#include "v3/step.h"
#include "v7-boundary-signals-impl.h"
#include "v7-derive-thresholds.h"
#include "v7-roof-graph-format.h"

#define BLF_RECORD_BYTES 72

// ── Params ──

static int parse_params(const char* params_str, v7_bs_params_t* p) {
    v7_bs_params_default(p);
    p->up_axis = 2;  // production v3 z-up LAZ
    if (!params_str) return 0;
    json_error_t je;
    json_t* obj = json_loads(params_str, 0, &je);
    if (!obj) return -1;
    json_t* j;
    if ((j = json_object_get(obj, "nearbyRadius"))         && json_is_number(j))    p->nearby_radius          = json_number_value(j);
    if ((j = json_object_get(obj, "planeDistFilter"))      && json_is_number(j))    p->plane_dist_filter      = json_number_value(j);
    if ((j = json_object_get(obj, "cornerDistThreshold"))  && json_is_number(j))    p->corner_dist_threshold  = json_number_value(j);
    if ((j = json_object_get(obj, "cornerMergeThreshold")) && json_is_number(j))    p->corner_merge_threshold = json_number_value(j);
    j = json_object_get(obj, "upAxis");
    if (json_int32_or_default(j, 0, 2, "upAxis", &p->up_axis) != 0) { json_decref(obj); return -1; }
    json_decref(obj);
    return 0;
}

// ── Parse upstream PLANES + RIDGE_LINES from a single-bldg TLV ──
//
// Returns 0 on success with malloc'd arrays in *out_planes / *out_lines.
// Caller frees both.

static int parse_upstream(const uint8_t* tlv_bytes, size_t tlv_len,
                          double cx, double cy, double cz,
                          v7_bs_input_plane_t** out_planes, size_t* out_n_planes,
                          v7_bs_input_ridge_line_t** out_lines, size_t* out_n_lines) {
    *out_planes = NULL; *out_n_planes = 0;
    *out_lines  = NULL; *out_n_lines  = 0;
    rg_reader_t r;
    if (rg_reader_open(&r, tlv_bytes, tlv_len) != 0) return -1;

    // PLANES.
    const uint8_t* sect; size_t slen;
    if (rg_reader_section(&r, RG_SECT_PLANES, &sect, &slen) == 0) {
        uint32_t n, stride;
        rg_section_header(sect, slen, &n, &stride);
        if (stride == sizeof(rg_plane_record_t) && n > 0) {
            v7_bs_input_plane_t* arr = (v7_bs_input_plane_t*)calloc(n, sizeof(*arr));
            if (!arr) return -1;
            const uint8_t* base = sect + RG_SECTION_HEADER_BYTES;
            for (uint32_t i = 0; i < n; i++) {
                const rg_plane_record_t* P = (const rg_plane_record_t*)(base + (size_t)i * stride);
                arr[i].id = P->id;
                arr[i].rejected = (P->flags & RG_PLANE_REJECTED) != 0;
                // Upstream planes were emitted in TILE-WORLD coords (topology
                // wrapper post-shifts). We unshift back to building-local for
                // the compute (which projects against building-local positions).
                //   normal — translation-invariant
                //   d_local = d_world + n·c
                arr[i].normal[0] = P->normal[0];
                arr[i].normal[1] = P->normal[1];
                arr[i].normal[2] = P->normal[2];
                arr[i].d = P->d + (P->normal[0] * cx + P->normal[1] * cy + P->normal[2] * cz);
                arr[i].basis_e1[0] = P->basis_e1[0]; arr[i].basis_e1[1] = P->basis_e1[1]; arr[i].basis_e1[2] = P->basis_e1[2];
                arr[i].basis_e2[0] = P->basis_e2[0]; arr[i].basis_e2[1] = P->basis_e2[1]; arr[i].basis_e2[2] = P->basis_e2[2];
            }
            *out_planes = arr; *out_n_planes = n;
        }
    }

    // RIDGE_LINES.
    if (rg_reader_section(&r, RG_SECT_RIDGE_LINES, &sect, &slen) == 0) {
        uint32_t n, stride;
        rg_section_header(sect, slen, &n, &stride);
        if (stride == sizeof(rg_ridge_line_record_t) && n > 0) {
            v7_bs_input_ridge_line_t* arr = (v7_bs_input_ridge_line_t*)calloc(n, sizeof(*arr));
            if (!arr) { free(*out_planes); *out_planes = NULL; return -1; }
            // For plane_a/plane_b we need a parallel walk of RIDGES section.
            const uint8_t* sect_r; size_t slen_r;
            const uint8_t* base_r = NULL; uint32_t n_r = 0, stride_r = 0;
            if (rg_reader_section(&r, RG_SECT_RIDGES, &sect_r, &slen_r) == 0) {
                rg_section_header(sect_r, slen_r, &n_r, &stride_r);
                base_r = sect_r + RG_SECTION_HEADER_BYTES;
            }
            const uint8_t* base = sect + RG_SECTION_HEADER_BYTES;
            for (uint32_t i = 0; i < n; i++) {
                const rg_ridge_line_record_t* L = (const rg_ridge_line_record_t*)(base + (size_t)i * stride);
                arr[i].has_line = (L->flags & 0x01u) != 0;
                // line_point is in tile-world; unshift to building-local for
                // closest-approach math (t parameters stay invariant).
                arr[i].line_point[0]     = L->line_point[0] - cx;
                arr[i].line_point[1]     = L->line_point[1] - cy;
                arr[i].line_point[2]     = L->line_point[2] - cz;
                arr[i].line_direction[0] = L->line_direction[0];
                arr[i].line_direction[1] = L->line_direction[1];
                arr[i].line_direction[2] = L->line_direction[2];
                arr[i].t_min_a = L->t_min_a;
                arr[i].t_max_a = L->t_max_a;
                arr[i].t_min_b = L->t_min_b;
                arr[i].t_max_b = L->t_max_b;
                // RIDGES is parallel to RIDGE_LINES by construction (topology
                // emits them with matching ids). Use the same index.
                if (base_r && stride_r == sizeof(rg_ridge_record_t) && (uint32_t)i < n_r) {
                    const rg_ridge_record_t* R = (const rg_ridge_record_t*)(base_r + (size_t)i * stride_r);
                    arr[i].plane_a = R->plane_a;
                    arr[i].plane_b = R->plane_b;
                }
            }
            *out_lines = arr; *out_n_lines = n;
        }
    }
    return 0;
}

// ── Pack CORNERS + NEARBY_PROJECTIONS as new sections (var-stride) ──

static int pack_corner_record(rg_writer_t* w, const v7_bs_corner_t* C,
                               double cx, double cy, double cz) {
    // Variable-length corner record per ROOF-GRAPH-FORMAT.md §4.6.
    // Layout: header (32B) + planeIds prefix + ridgeIds prefix
    //         + optional name string (flag 0x01)
    //         + optional dirs_by_plane (flag 0x04)
    //         + optional com_bisector_by_plane (flag 0x20).
    //
    // dirs_by_plane carries two outgoing edge directions per (plane, corner)
    // for concave/convex corners; downstream regularize + polygon extraction
    // consume them. com_bisector_by_plane is the CoM-direction blended
    // bisector cache that lets v7-boundary-polygons skip its D.4 phase.
    size_t hdr_bytes = 32;
    // Plane block: 8B header + u32×n_plane_ids + u8×n_plane_ids (source_per_plane) + align8.
    size_t plane_pfx = 8 + C->n_plane_ids * 4 + C->n_plane_ids;
    plane_pfx = (plane_pfx + 7) & ~(size_t)7;
    size_t ridge_pfx = 8 + C->n_ridge_ids * 4;
    ridge_pfx = (ridge_pfx + 7) & ~(size_t)7;
    size_t name_len = C->name ? strlen(C->name) : 0;
    bool has_name = (name_len > 0);
    size_t name_block = 0;
    if (has_name) {
        // §2 padded-string layout: u32 byte_len + u32 _pad + bytes,
        // total padded to 8.
        name_block = 8 + name_len;
        name_block = (name_block + 7) & ~(size_t)7;
    }
    bool has_dirs = (C->n_dirs > 0 && C->dirs != NULL);
    size_t dirs_block = 0;
    if (has_dirs) {
        // 8B prefix + 32B per entry (already 8-aligned).
        dirs_block = 8 + (size_t)C->n_dirs * 32;
    }
    bool has_com_bis = (C->n_com_bisectors > 0 && C->com_bisectors != NULL);
    size_t com_bis_block = 0;
    if (has_com_bis) {
        // 8B prefix + 16B per entry (already 8-aligned).
        com_bis_block = 8 + (size_t)C->n_com_bisectors * 16;
    }
    size_t rec_len = hdr_bytes + plane_pfx + ridge_pfx + name_block + dirs_block + com_bis_block;

    uint8_t* rec = (uint8_t*)calloc(rec_len, 1);
    if (!rec) return -1;

    memcpy(rec + 0, &C->id, 4);
    rec[4] = C->source;
    rec[5] = (uint8_t)((has_name ? 0x01u : 0u)
                       | (has_dirs ? 0x04u : 0u)
                       | (has_com_bis ? 0x20u : 0u));
    memcpy(rec + 6, &C->n_merged, 2);
    double pw[3] = { C->point[0] + cx, C->point[1] + cy, C->point[2] + cz };
    memcpy(rec + 8,  &pw[0], 8);
    memcpy(rec + 16, &pw[1], 8);
    memcpy(rec + 24, &pw[2], 8);

    size_t off = hdr_bytes;
    uint32_t pcount = C->n_plane_ids, pad = 0;
    memcpy(rec + off, &pcount, 4); memcpy(rec + off + 4, &pad, 4); off += 8;
    for (uint32_t k = 0; k < C->n_plane_ids; k++) {
        memcpy(rec + off, &C->plane_ids[k], 4); off += 4;
    }
    // source_per_plane[] — signals corners have no per-plane override yet,
    // so emit the global source repeated. The block's align8 pad is already
    // zeroed by the calloc above.
    for (uint32_t k = 0; k < C->n_plane_ids; k++) {
        rec[off++] = C->source;
    }
    off = hdr_bytes + plane_pfx;
    uint32_t rcount = C->n_ridge_ids;
    memcpy(rec + off, &rcount, 4); memcpy(rec + off + 4, &pad, 4); off += 8;
    for (uint32_t k = 0; k < C->n_ridge_ids; k++) {
        memcpy(rec + off, &C->ridge_ids[k], 4); off += 4;
    }
    if (has_name) {
        off = hdr_bytes + plane_pfx + ridge_pfx;
        uint32_t nl = (uint32_t)name_len;
        memcpy(rec + off, &nl, 4); memcpy(rec + off + 4, &pad, 4); off += 8;
        memcpy(rec + off, C->name, name_len);
        // remaining bytes already zero from calloc
    }
    if (has_dirs) {
        off = hdr_bytes + plane_pfx + ridge_pfx + name_block;
        uint32_t dcount = C->n_dirs;
        memcpy(rec + off, &dcount, 4); memcpy(rec + off + 4, &pad, 4); off += 8;
        for (uint32_t k = 0; k < C->n_dirs; k++) {
            const v7_bs_corner_dir_t* D = &C->dirs[k];
            memcpy(rec + off + 0,  &D->plane_id, 4);
            // rec[4..7] _pad
            memcpy(rec + off + 8,  D->dir_a, 12);
            memcpy(rec + off + 20, D->dir_b, 12);
            off += 32;
        }
    }
    if (has_com_bis) {
        off = hdr_bytes + plane_pfx + ridge_pfx + name_block + dirs_block;
        uint32_t ccount = C->n_com_bisectors;
        memcpy(rec + off, &ccount, 4); memcpy(rec + off + 4, &pad, 4); off += 8;
        // Entry layout: {u32 plane_id; f32×3 com_bisector} — stride 16 B.
        // 4-byte natural alignment is enough for f32; an extra _pad would
        // bump the stride to 20 B and the block math (8 + N*16) would
        // overflow the rec buffer.
        for (uint32_t k = 0; k < C->n_com_bisectors; k++) {
            const v7_bs_corner_com_bisector_t* B = &C->com_bisectors[k];
            memcpy(rec + off + 0, &B->plane_id, 4);
            memcpy(rec + off + 4, B->com_bisector, 12);
            off += 16;
        }
    }

    int rc = rg_writer_record(w, rec, rec_len);
    free(rec);
    return rc;
}

// PLANE_GRIDS record per ROOF-GRAPH-FORMAT.md §4.5. Emits GROUP_FIELD
// (flag bit 0) + BAL_FIELD (flag bit 2). NEARBY_FIELD (bit 1) NOT
// emitted — its bbox/dimensions differ from GROUP's in general, and
// the format puts both fields under one set of dimensions in the
// header. We can revisit (and possibly relax the spec) when a
// downstream step actually needs nearby's heat maps.
//
// COORDS — building-local on the wire. The header now carries the
// per-record `building_centroid` (f64×3 cx/cy/cz); the JS renderer
// applies the F64 shift at render time. Earlier attempt pre-shifted
// to tile-world in F32, which collapsed the float32 mantissa near
// L93 (~289550) to ~3 cm precision → visible staircase artifact on
// the heat-map cells.
//
// GROUP_FIELD layout (per spec §4.5):
//   10 × float32×nCells:  oa, oaSpan, comDist, outU, outV,
//                          bestLeft, bestRight, nodeX, nodeY, nodeZ
//   1  × uint16×nCells:   ptCount
// Total bytes per cell = 10*4 + 2 = 42; block padded to 8 at end.
// Spec parenthetical comment had ptCount(u16) appearing between
// comDist and outU — we follow that order for forward-compat.
// Pack ONE PLANE_GRIDS record with a single field block (GROUP_FIELD
// xor NEARBY_FIELD, set via `field_flag` = 0x01 or 0x02) and an optional
// BAL_FIELD trailer. The record's header carries `field`'s own
// n_cols/n_rows/g_min_u/g_min_v so each record is self-contained and
// downstream consumers don't need to know which field set is inside —
// they just read the dims and the field arrays in the documented order.
//
// Per the format spec, the plane block in CORNERS is independent of
// PLANE_GRIDS, so emitting multiple records per plane_id is fine: each
// is treated like any other variable-length record.
static int pack_one_grid_record(rg_writer_t* w,
                                  uint32_t plane_id,
                                  const v7_bs_grid_field_t* field,
                                  uint32_t field_flag,
                                  const float* bal_field,
                                  const v7_bs_input_plane_t* P_local,
                                  double cx, double cy, double cz) {
    if (!field) return 0;
    if (field->n_cols == 0 || field->n_rows == 0) return 0;
    size_t n_cells = (size_t)field->n_cols * (size_t)field->n_rows;
    bool emit_bal = (bal_field != NULL);
    uint32_t flags = field_flag;
    if (emit_bal) flags |= 0x04u;

    size_t field_bytes = n_cells * (10 * sizeof(float) + sizeof(uint16_t));
    if (n_cells & 1) field_bytes += 2;
    field_bytes = (field_bytes + 7) & ~(size_t)7;

    size_t bal_bytes = 0;
    if (emit_bal) {
        bal_bytes = n_cells * sizeof(float);
        bal_bytes = (bal_bytes + 7) & ~(size_t)7;
    }
    const size_t header_bytes = 136;
    size_t rec_len = header_bytes + field_bytes + bal_bytes;
    uint8_t* rec = (uint8_t*)calloc(rec_len, 1);
    if (!rec) return -1;

    double n[3] = { P_local->normal[0], P_local->normal[1], P_local->normal[2] };
    double e1[3] = { P_local->basis_e1[0], P_local->basis_e1[1], P_local->basis_e1[2] };
    double e2[3] = { P_local->basis_e2[0], P_local->basis_e2[1], P_local->basis_e2[2] };
    double d_local      = P_local->d;
    float g_min_u_local = field->g_min_u;
    float g_min_v_local = field->g_min_v;

    memcpy(rec +   0, &plane_id,           4);
    memcpy(rec +   4, &flags,              4);
    memcpy(rec +   8, &field->n_cols,      4);
    memcpy(rec +  12, &field->n_rows,      4);
    memcpy(rec +  16, &field->cell_size,   4);
    memcpy(rec +  20, &g_min_u_local,      4);
    memcpy(rec +  24, &g_min_v_local,      4);
    // rec[28..31] = _pad
    memcpy(rec +  32, &n[0], 8); memcpy(rec +  40, &n[1], 8); memcpy(rec +  48, &n[2], 8);
    memcpy(rec +  56, &d_local,            8);
    memcpy(rec +  64, &e1[0], 8); memcpy(rec +  72, &e1[1], 8); memcpy(rec +  80, &e1[2], 8);
    memcpy(rec +  88, &e2[0], 8); memcpy(rec +  96, &e2[1], 8); memcpy(rec + 104, &e2[2], 8);
    memcpy(rec + 112, &cx, 8); memcpy(rec + 120, &cy, 8); memcpy(rec + 128, &cz, 8);

    size_t off = header_bytes;
    // Field arrays in spec order: oa, oaSpan, comDist, ptCount(u16),
    // outU, outV, bestLeft, bestRight, nodeX, nodeY, nodeZ. The u16
    // ptCount can leave the next f32 array at 2 (mod 4); 2 B re-align
    // when n_cells is odd. node_x/y/z stay BUILDING-LOCAL — the renderer
    // shifts to tile-world in F64 using the header centroid.
    memcpy(rec + off, field->oa_field,        n_cells * 4); off += n_cells * 4;
    memcpy(rec + off, field->oa_span_field,   n_cells * 4); off += n_cells * 4;
    memcpy(rec + off, field->com_dist_field,  n_cells * 4); off += n_cells * 4;
    memcpy(rec + off, field->pt_count_field,  n_cells * 2); off += n_cells * 2;
    if (n_cells & 1) off += 2;
    memcpy(rec + off, field->out_u,           n_cells * 4); off += n_cells * 4;
    memcpy(rec + off, field->out_v,           n_cells * 4); off += n_cells * 4;
    memcpy(rec + off, field->best_left_field, n_cells * 4); off += n_cells * 4;
    memcpy(rec + off, field->best_right_field,n_cells * 4); off += n_cells * 4;
    memcpy(rec + off, field->node_x,          n_cells * 4); off += n_cells * 4;
    memcpy(rec + off, field->node_y,          n_cells * 4); off += n_cells * 4;
    memcpy(rec + off, field->node_z,          n_cells * 4); off += n_cells * 4;
    off = (off + 7) & ~(size_t)7;

    if (emit_bal) {
        memcpy(rec + off, bal_field, n_cells * sizeof(float));
        off += n_cells * sizeof(float);
        off = (off + 7) & ~(size_t)7;
    }

    int rc = rg_writer_record(w, rec, rec_len);
    free(rec);
    return rc;
}

// Per-plane PLANE_GRIDS emission. Emits up to two records:
//   (a) GROUP_FIELD (+ BAL_FIELD trailer when present) — always
//   (b) NEARBY_FIELD — when has_nearby_field, with nearby's own
//       independent dims/origin in its own header. BAL is not duplicated;
//       it's emitted with the group record only.
//
// The two records share plane_id but are otherwise independent — each is
// a complete, decode-without-context grid. Downstream consumers that only
// need group filter to records where flag bit 0x01 is set; the lab UI
// renderer reads both for the per-plane viz tree.
static int pack_plane_grid_record(rg_writer_t* w,
                                   const v7_bs_plane_grid_t* G,
                                   const v7_bs_input_plane_t* P_local,
                                   double cx, double cy, double cz) {
    if (!G->has_group_field) return 0;
    const float* bal_field = (G->has_bal_field && G->bal_field != NULL)
                                ? G->bal_field : NULL;
    if (pack_one_grid_record(w, G->plane_id, &G->group_field, 0x01u,
                              bal_field, P_local, cx, cy, cz) != 0) return -1;
    if (G->has_nearby_field && G->nearby_field.n_cols > 0) {
        if (pack_one_grid_record(w, G->plane_id, &G->nearby_field, 0x02u,
                                  NULL, P_local, cx, cy, cz) != 0) return -1;
    }
    return 0;
}

static int pack_nearby_record(rg_writer_t* w, const v7_bs_nearby_per_plane_t* npp,
                               double cx, double cy, double cz,
                               const uint32_t* global_indices, size_t n_pts_local) {
    // Variable-length record per §4.4: u32 plane_id, u32 n_items,
    // items × 40 B each (24 B position + 4 sd + 4 src_idx + 8 pad).
    size_t rec_len = 8 + (size_t)npp->n_items * 40;
    uint8_t* rec = (uint8_t*)calloc(rec_len, 1);
    if (!rec) return -1;
    memcpy(rec + 0, &npp->plane_id, 4);
    memcpy(rec + 4, &npp->n_items,  4);
    size_t off = 8;
    for (uint32_t k = 0; k < npp->n_items; k++) {
        const v7_bs_nearby_item_t* it = &npp->items[k];
        double pw[3] = { it->position[0] + cx, it->position[1] + cy, it->position[2] + cz };
        memcpy(rec + off + 0,  &pw[0], 8);
        memcpy(rec + off + 8,  &pw[1], 8);
        memcpy(rec + off + 16, &pw[2], 8);
        memcpy(rec + off + 24, &it->signed_dist, 4);
        // src_idx: convert building-local → global if mapping provided.
        uint32_t global = it->src_idx;
        if (global_indices && it->src_idx < n_pts_local) {
            global = global_indices[it->src_idx];
        }
        memcpy(rec + off + 28, &global, 4);
        // off + 32..40 is padding (kept at zero).
        off += 40;
    }
    int rc = rg_writer_record(w, rec, rec_len);
    free(rec);
    return rc;
}

// ── Per-bldg exec ──

static int process_bldg(rg_writer_t* w,
                        const uint8_t* upstream_bytes, size_t upstream_len,
                        const double* positions, size_t total_points,
                        const float* beam_dirs_global,
                        const float* pt_weights_bldg,    // per-bldg-local; nullable
                        const uint32_t* global_indices, uint32_t n_pts_local,
                        double cx, double cy, double cz,
                        const int16_t* owner,
                        // Optional dv2 void info for void-fill in contour
                        // (both NULL/0 → contour from own+nearby only).
                        const v7_bs_void_hull_t* voids, size_t n_voids,
                        const v7_bs_plane_voids_t* plane_voids, size_t n_plane_voids,
                        const v7_bs_params_t* params) {
    // 1. Build building-local positions + beam dirs (same shift convention
    // as build-building-kd-trees).
    double* local_pos = (double*)malloc((size_t)n_pts_local * 3 * sizeof(double));
    float*  local_f32 = (float*) malloc((size_t)n_pts_local * 3 * sizeof(float));
    float*  local_beam = beam_dirs_global ? (float*)malloc((size_t)n_pts_local * 3 * sizeof(float)) : NULL;
    if (!local_pos || !local_f32 || (beam_dirs_global && !local_beam)) {
        free(local_pos); free(local_f32); free(local_beam);
        return -1;
    }
    for (uint32_t i = 0; i < n_pts_local; i++) {
        uint32_t gi = global_indices[i];
        if ((size_t)gi >= total_points) { free(local_pos); free(local_f32); free(local_beam); return -1; }
        local_f32[i * 3 + 0] = (float)(positions[gi * 3 + 0] - cx);
        local_f32[i * 3 + 1] = (float)(positions[gi * 3 + 1] - cy);
        local_f32[i * 3 + 2] = (float)(positions[gi * 3 + 2] - cz);
    }
    for (size_t i = 0; i < (size_t)n_pts_local * 3; i++) {
        local_pos[i] = (double)local_f32[i];
    }
    free(local_f32);
    if (local_beam) {
        for (uint32_t i = 0; i < n_pts_local; i++) {
            uint32_t gi = global_indices[i];
            // Beam direction is rotation-invariant under pure translation, but
            // we still copy per-bldg to align indexing with local positions.
            local_beam[i * 3 + 0] = beam_dirs_global[gi * 3 + 0];
            local_beam[i * 3 + 1] = beam_dirs_global[gi * 3 + 1];
            local_beam[i * 3 + 2] = beam_dirs_global[gi * 3 + 2];
        }
    }

    // 2. Parse upstream PLANES + RIDGE_LINES.
    v7_bs_input_plane_t* planes = NULL; size_t n_planes = 0;
    v7_bs_input_ridge_line_t* lines = NULL; size_t n_lines = 0;
    if (parse_upstream(upstream_bytes, upstream_len, cx, cy, cz,
                       &planes, &n_planes, &lines, &n_lines) != 0) {
        free(local_pos); free(local_beam);
        return -1;
    }

    // 3. Compute.
    v7_bs_outputs_t out;
    v7_bs_outputs_init(&out);
    int compute_rc = v7_bs_compute(local_pos, local_beam, pt_weights_bldg, n_pts_local,
                                    owner, planes, n_planes,
                                    lines, n_lines,
                                    voids, n_voids, plane_voids, n_plane_voids,
                                    params, &out);
    free(local_pos); free(local_beam); free(planes); free(lines);
    if (compute_rc != 0) { v7_bs_outputs_free(&out); return -1; }

    // 4. Pass through upstream sections.
    rg_reader_t up;
    if (rg_reader_open(&up, upstream_bytes, upstream_len) != 0) {
        v7_bs_outputs_free(&out); return -1;
    }
    if (rg_writer_passthrough(w, &up, RG_SECT_NONE) != 0) {
        v7_bs_outputs_free(&out); return -1;
    }

    // 5. CORNERS section.
    if (out.n_corners > 0) {
        if (rg_writer_section_begin(w, RG_SECT_CORNERS, 0) != 0) {
            v7_bs_outputs_free(&out); return -1;
        }
        for (size_t i = 0; i < out.n_corners; i++) {
            if (pack_corner_record(w, &out.corners[i], cx, cy, cz) != 0) {
                v7_bs_outputs_free(&out); return -1;
            }
        }
        if (rg_writer_section_end(w) != 0) {
            v7_bs_outputs_free(&out); return -1;
        }
    }

    // 6. NEARBY_PROJECTIONS section.
    if (out.n_nearby > 0) {
        if (rg_writer_section_begin(w, RG_SECT_NEARBY_PROJECTIONS, 0) != 0) {
            v7_bs_outputs_free(&out); return -1;
        }
        for (size_t i = 0; i < out.n_nearby; i++) {
            const v7_bs_nearby_per_plane_t* npp = &out.nearby[i];
            if (npp->n_items == 0) continue;
            if (pack_nearby_record(w, npp, cx, cy, cz,
                                    global_indices, n_pts_local) != 0) {
                v7_bs_outputs_free(&out); return -1;
            }
        }
        if (rg_writer_section_end(w) != 0) {
            v7_bs_outputs_free(&out); return -1;
        }
    }

    // 7. PLANE_GRIDS section — one record per alive plane with a grid.
    //    First pass emits only BAL_FIELD; GROUP/NEARBY heat maps land later.
    if (out.n_grids > 0) {
        // We need the (cx, cy, cz) shift and the per-plane source eq to
        // pre-shift the on-wire header. Look up the input plane by id.
        // (Build a tiny lookup over the upstream planes we parsed earlier.)
        v7_bs_input_plane_t* in_planes = NULL; size_t n_in_planes = 0;
        v7_bs_input_ridge_line_t* in_lines = NULL; size_t n_in_lines = 0;
        if (parse_upstream(upstream_bytes, upstream_len, cx, cy, cz,
                           &in_planes, &n_in_planes,
                           &in_lines, &n_in_lines) == 0 && in_planes) {
            free(in_lines);  // not needed here
            if (rg_writer_section_begin(w, RG_SECT_PLANE_GRIDS, 0) != 0) {
                free(in_planes); v7_bs_outputs_free(&out); return -1;
            }
            for (size_t i = 0; i < out.n_grids; i++) {
                const v7_bs_plane_grid_t* G = &out.grids[i];
                if (!G->has_bal_field) continue;
                // Find matching input plane (linear scan — n_planes is small).
                const v7_bs_input_plane_t* P_local = NULL;
                for (size_t j = 0; j < n_in_planes; j++) {
                    if (in_planes[j].id == G->plane_id) { P_local = &in_planes[j]; break; }
                }
                if (!P_local) continue;
                if (pack_plane_grid_record(w, G, P_local, cx, cy, cz) != 0) {
                    free(in_planes); v7_bs_outputs_free(&out); return -1;
                }
            }
            if (rg_writer_section_end(w) != 0) {
                free(in_planes); v7_bs_outputs_free(&out); return -1;
            }
            free(in_planes);
        }
    }

    // 8. PLANE_CONTOURS (0x11) — per-plane grid-fill topology.
    //    One record per plane that produced a usable contour. See
    //    ROOF-GRAPH-FORMAT.md §4.17 for the wire layout.
    {
        bool any = false;
        for (size_t i = 0; i < out.n_grids; i++) {
            if (out.grids[i].contour.has_contour) { any = true; break; }
        }
        if (any) {
            if (rg_writer_section_begin(w, RG_SECT_PLANE_CONTOURS, 0) != 0) {
                v7_bs_outputs_free(&out); return -1;
            }
            for (size_t i = 0; i < out.n_grids; i++) {
                const v7_bs_plane_grid_t* G = &out.grids[i];
                const v7_bs_plane_contour_t* C = &G->contour;
                if (!C->has_contour) continue;
                uint32_t flags = 0;
                if (C->has_grids)        flags |= 0x01;
                if (C->has_contour)      flags |= 0x02;
                if (C->n_holes > 0)      flags |= 0x04;
                if (C->has_nearby_grids) flags |= 0x08;
                size_t n_cells = (size_t)C->n_cols * (size_t)C->n_rows;
                // Header is 40 bytes (10 × 4-byte fields incl. _pad).
                size_t sz = 40;
                if (flags & 0x01) {
                    // density f32×n_cells + smoothed f32×n_cells + binary u8×n_cells (pad to 8).
                    sz += n_cells * 4;          // density
                    sz += n_cells * 4;          // smoothed
                    size_t bin_padded = (n_cells + 7) & ~(size_t)7;
                    sz += bin_padded;
                }
                if (flags & 0x08) {
                    sz += n_cells * 4;          // density_nearby
                    sz += n_cells * 4;          // smoothed_nearby
                }
                if (flags & 0x02) {
                    sz += 8;                    // n_outer + _pad
                    sz += (size_t)C->outer.n_pts * 2 * 4;
                }
                if (flags & 0x04) {
                    sz += 8;                    // n_holes + _pad
                    for (uint32_t h = 0; h < C->n_holes; h++) {
                        sz += 8;                // per-hole n_pts + _pad
                        sz += (size_t)C->holes[h].n_pts * 2 * 4;
                    }
                }
                size_t pad_tail = (8 - (sz & 7)) & 7;
                size_t rec_len = sz + pad_tail;
                uint8_t* rec = (uint8_t*)calloc(rec_len, 1);
                if (!rec) { v7_bs_outputs_free(&out); return -1; }
                size_t off = 0;
                memcpy(rec + off, &G->plane_id, 4); off += 4;
                memcpy(rec + off, &flags,       4); off += 4;
                memcpy(rec + off, &C->n_cols,   4); off += 4;
                memcpy(rec + off, &C->n_rows,   4); off += 4;
                memcpy(rec + off, &C->cell_size,4); off += 4;
                memcpy(rec + off, &C->g_min_u,  4); off += 4;
                memcpy(rec + off, &C->g_min_v,  4); off += 4;
                memcpy(rec + off, &C->max_density, 4); off += 4;
                memcpy(rec + off, &C->threshold,4); off += 4;
                uint32_t pad32 = 0;
                memcpy(rec + off, &pad32,       4); off += 4;
                if (flags & 0x01) {
                    memcpy(rec + off, C->density,  n_cells * 4); off += n_cells * 4;
                    memcpy(rec + off, C->smoothed, n_cells * 4); off += n_cells * 4;
                    memcpy(rec + off, C->binary,   n_cells);     off += n_cells;
                    size_t bin_pad = ((n_cells + 7) & ~(size_t)7) - n_cells;
                    off += bin_pad;
                }
                if (flags & 0x08) {
                    memcpy(rec + off, C->density_nearby,  n_cells * 4); off += n_cells * 4;
                    memcpy(rec + off, C->smoothed_nearby, n_cells * 4); off += n_cells * 4;
                }
                if (flags & 0x02) {
                    memcpy(rec + off, &C->outer.n_pts, 4); off += 4;
                    memcpy(rec + off, &pad32,          4); off += 4;
                    memcpy(rec + off, C->outer.pts, (size_t)C->outer.n_pts * 2 * 4);
                    off += (size_t)C->outer.n_pts * 2 * 4;
                }
                if (flags & 0x04) {
                    memcpy(rec + off, &C->n_holes, 4); off += 4;
                    memcpy(rec + off, &pad32,      4); off += 4;
                    for (uint32_t h = 0; h < C->n_holes; h++) {
                        memcpy(rec + off, &C->holes[h].n_pts, 4); off += 4;
                        memcpy(rec + off, &pad32,             4); off += 4;
                        memcpy(rec + off, C->holes[h].pts,
                               (size_t)C->holes[h].n_pts * 2 * 4);
                        off += (size_t)C->holes[h].n_pts * 2 * 4;
                    }
                }
                if (rg_writer_record(w, rec, rec_len) != 0) {
                    free(rec); v7_bs_outputs_free(&out); return -1;
                }
                free(rec);
            }
            if (rg_writer_section_end(w) != 0) {
                v7_bs_outputs_free(&out); return -1;
            }
        }
    }

    v7_bs_outputs_free(&out);
    return 0;
}

// ── Exec ──

static int v7_boundary_signals_exec(step_context_t* ctx) {
    v7_bs_params_t params;
    if (parse_params(step_get_params(ctx), &params) != 0) return -1;
    if (getenv("DERIV_LOG")) {
        fprintf(stderr, "[DERIV] step=v7-boundary-signals scope=tile plane_dist_filter=%.6f corner_dist_threshold=%.6f corner_merge_threshold=%.6f anchor_dedup_radius=%.6f anchor_valid_radius=%.6f anchor_balance_min=%.6f grid_cell_size=%.6f grid_eval_radius=%.6f nearby_radius=%.6f\n",
                params.plane_dist_filter, params.corner_dist_threshold,
                params.corner_merge_threshold, params.anchor_dedup_radius,
                params.anchor_valid_radius, params.anchor_balance_min,
                params.grid_cell_size, params.grid_eval_radius,
                params.nearby_radius);
    }

    size_t pos_bytes = 0;
    const double* positions = (const double*)step_read(ctx, "lidar-positions", &pos_bytes);
    if (!positions || pos_bytes % (3 * sizeof(double)) != 0) return -1;
    size_t total_points = pos_bytes / (3 * sizeof(double));

    size_t beam_bytes = 0;
    const float* beam_dirs = (const float*)step_read(ctx, "bi-directions", &beam_bytes);
    if (beam_dirs && beam_bytes != total_points * 3 * sizeof(float)) beam_dirs = NULL;

    size_t n_buildings = step_array_len(ctx, "per-building-records");
    if (step_array_len(ctx, "grm-owner")                    != n_buildings ||
        step_array_len(ctx, "building-local-frames")        != n_buildings ||
        step_array_len(ctx, "boundary-topology-roof-graph") != n_buildings ||
        step_array_len(ctx, "bldg-thresholds")              != n_buildings) {
        fprintf(stderr, "[v7-boundary-signals] count mismatch across per-bldg arrays (need bldg-thresholds from v7-derive-thresholds)\n");
        return -1;
    }

    blob_t b_roof = {0};
    int rc = 0;

    for (size_t bi = 0; bi < n_buildings; bi++) {
        // per-building-records → globalIndices.
        const uint8_t* rec_bytes = NULL; size_t rec_len = 0;
        if (step_array_read_bytes(ctx, "per-building-records", bi, &rec_bytes, &rec_len) != 0
            || !rec_bytes || rec_len < 12) { blob_push(&b_roof, NULL, 0); continue; }
        uint32_t schema, n_verts, n_pts_local;
        memcpy(&schema,      rec_bytes + 0, 4);
        memcpy(&n_verts,     rec_bytes + 4, 4);
        memcpy(&n_pts_local, rec_bytes + 8, 4);
        if (schema != 1) { blob_push(&b_roof, NULL, 0); continue; }
        const size_t idx_off = 68 + (size_t)n_verts * 24;
        if (idx_off + (size_t)n_pts_local * 4 > rec_len) { blob_push(&b_roof, NULL, 0); continue; }
        const uint32_t* global_indices = (const uint32_t*)(rec_bytes + idx_off);
        if (n_pts_local < 4) { blob_push(&b_roof, NULL, 0); continue; }

        // building-local-frames → centroid.
        const uint8_t* frame_bytes = NULL; size_t frame_len = 0;
        if (step_array_read_bytes(ctx, "building-local-frames", bi, &frame_bytes, &frame_len) != 0
            || !frame_bytes || frame_len < BLF_RECORD_BYTES) { blob_push(&b_roof, NULL, 0); continue; }
        double cx, cy, cz;
        memcpy(&cx, frame_bytes +  0, 8);
        memcpy(&cy, frame_bytes +  8, 8);
        memcpy(&cz, frame_bytes + 16, 8);

        // grm-owner.
        const uint8_t* own_bytes = NULL; size_t own_len = 0;
        if (step_array_read_bytes(ctx, "grm-owner", bi, &own_bytes, &own_len) != 0
            || !own_bytes || own_len != (size_t)n_pts_local * sizeof(int16_t)) {
            blob_push(&b_roof, NULL, 0); continue;
        }
        const int16_t* owner = (const int16_t*)own_bytes;

        // ts-weights (per-bldg point Voronoi areas; optional).
        const uint8_t* tsw_bytes = NULL; size_t tsw_len = 0;
        const float* pt_weights_bldg = NULL;
        if (step_array_read_bytes(ctx, "ts-weights", bi, &tsw_bytes, &tsw_len) == 0
            && tsw_bytes && tsw_len == (size_t)n_pts_local * sizeof(float)) {
            pt_weights_bldg = (const float*)tsw_bytes;
        }

        // boundary-topology-roof-graph (per-bldg TLV blob).
        const uint8_t* topo_bytes = NULL; size_t topo_len = 0;
        if (step_array_read_bytes(ctx, "boundary-topology-roof-graph", bi, &topo_bytes, &topo_len) != 0
            || !topo_bytes || topo_len < 32) {
            blob_push(&b_roof, NULL, 0); continue;
        }

        // Per-bldg thresholds (density + spacing for this building). Single
        // source of truth — see v7-derive-thresholds.c. Computed per-bldg
        // because v3's per-building architecture differs from v2's tile-wide
        // approach: using the tile-wide density here would mis-scale the
        // contour cell-size by sqrt(tile_area / bldg_area) (1.4× on dahouet).
        const uint8_t* th_bytes = NULL; size_t th_len = 0;
        if (step_array_read_bytes(ctx, "bldg-thresholds", bi,
                                   &th_bytes, &th_len) != 0
            || !th_bytes || th_len < V7_BLDG_THRESHOLDS_BYTES) {
            fprintf(stderr, "[v7-boundary-signals] bldg%zu missing bldg-thresholds record\n", bi);
            rc = -1; break;
        }
        v7_bldg_thresholds_t th;
        memcpy(&th, th_bytes, sizeof(th));
        if (!(th.density > 0.0) || !isfinite(th.density)) {
            fprintf(stderr, "[v7-boundary-signals] bldg%zu invalid density %.6f from bldg-thresholds\n",
                    bi, th.density);
            rc = -1; break;
        }
        // Per-bldg params clone — only lidar_density varies per-bldg.
        v7_bs_params_t bldg_params = params;
        bldg_params.lidar_density = th.density;

        // ── Parse dv2 void info for this bldg (hulls + per-plane membership). ──
        // Both arrays are heap-allocated, freed after process_bldg. NULL/0 when
        // dv2 didn't run upstream → contour falls back to own+nearby only.
        v7_bs_void_hull_t*   bldg_voids        = NULL;
        size_t               n_bldg_voids      = 0;
        v7_bs_plane_voids_t* bldg_plane_voids  = NULL;
        size_t               n_bldg_plane_voids= 0;
        uint32_t*            bldg_pv_storage   = NULL;  // owns void_id arrays
        const uint8_t*       ghost_bytes_keep  = NULL;  // need to keep alive across process_bldg
        size_t               ghost_len_keep    = 0;

        // Hulls: dv2-ghost-planes per-bldg blob. Frame is
        //   {u32 rec_len, payload, rec_len, payload, …}
        // (custom intra-bldg framing — see v7-density-voids-2.c
        // pack_ghost_plane). Each payload is 104B header + hull + inliers.
        if (step_array_len(ctx, "dv2-ghost-planes") == n_buildings) {
            if (step_array_read_bytes(ctx, "dv2-ghost-planes", bi,
                                       &ghost_bytes_keep, &ghost_len_keep) != 0) {
                ghost_bytes_keep = NULL; ghost_len_keep = 0;
            }
        }
        if (ghost_bytes_keep && ghost_len_keep > 0) {
            // First pass: count.
            size_t count = 0;
            {
                size_t cursor = 0;
                const uint8_t* h; size_t rl;
                while (blob_next(ghost_bytes_keep, ghost_len_keep, &cursor, &h, &rl)) {
                    if (rl >= 104) count++;
                }
            }
            if (count > 0) {
                bldg_voids = (v7_bs_void_hull_t*)calloc(count, sizeof(*bldg_voids));
            }
            // Second pass: populate. The 8-aligned inner framing (see
            // <v3/blob.h>) places every record's payload at an 8-byte
            // boundary inside the framework-aligned per-bldg outer
            // record, so hull_xyz (at intra-payload offset 104) is
            // naturally 8-aligned. Borrow the pointer; the framework
            // keeps `ghost_bytes_keep` alive across process_bldg.
            size_t cursor = 0;
            const uint8_t* h; size_t rl;
            while (blob_next(ghost_bytes_keep, ghost_len_keep, &cursor, &h, &rl)
                   && bldg_voids && n_bldg_voids < count) {
                if (rl < 104) continue;
                uint32_t void_id, inlier_count, hull_vc;
                memcpy(&void_id,      h + 88, 4);
                memcpy(&inlier_count, h + 92, 4);
                memcpy(&hull_vc,      h + 96, 4);
                (void)inlier_count;
                bldg_voids[n_bldg_voids].void_id      = void_id;
                bldg_voids[n_bldg_voids].n_hull_verts = hull_vc;
                bldg_voids[n_bldg_voids].hull_xyz     = (const double*)(h + 104);
                n_bldg_voids++;
            }
        }

        // Membership: parse RG_SECT_PLANE_VOIDS from the topo blob.
        // Variable-stride records: {u32 rec_len, u32 zero_pad, payload, pad_to_8}.
        // Payload = u32 plane_id, u32 n_void_ids, u32×n_void_ids.
        {
            rg_reader_t r;
            if (rg_reader_open(&r, topo_bytes, topo_len) == 0) {
                const uint8_t* sect; size_t slen;
                if (rg_reader_section(&r, RG_SECT_PLANE_VOIDS, &sect, &slen) == 0) {
                    uint32_t n, stride;
                    rg_section_header(sect, slen, &n, &stride);
                    bldg_plane_voids = (v7_bs_plane_voids_t*)calloc(n ? n : 1, sizeof(*bldg_plane_voids));
                    // void_id arrays are inline in the section payload — we
                    // alias them via uint32_t* pointers into `sect` (the
                    // framework keeps sect alive for the duration of this step).
                    size_t off = RG_SECTION_HEADER_BYTES;
                    for (uint32_t k = 0; k < n && off + 8 <= slen; k++) {
                        uint32_t pv_rec_len;
                        memcpy(&pv_rec_len, sect + off, 4);
                        off += 8;
                        if (off + pv_rec_len > slen) break;
                        if (pv_rec_len >= 8 && bldg_plane_voids) {
                            uint32_t plane_id, n_void_ids;
                            memcpy(&plane_id,   sect + off + 0, 4);
                            memcpy(&n_void_ids, sect + off + 4, 4);
                            bldg_plane_voids[n_bldg_plane_voids].plane_id    = plane_id;
                            bldg_plane_voids[n_bldg_plane_voids].n_void_ids  = n_void_ids;
                            bldg_plane_voids[n_bldg_plane_voids].void_ids    =
                                (const uint32_t*)(sect + off + 8);
                            n_bldg_plane_voids++;
                        }
                        off += (pv_rec_len + 7) & ~(size_t)7;
                    }
                }
            }
        }

        // Optional verification log — `V7_VOID_LOG=1` to enable.
        if (getenv("V7_VOID_LOG")) {
            for (size_t qi = 0; qi < n_bldg_plane_voids; qi++) {
                const v7_bs_plane_voids_t* pv = &bldg_plane_voids[qi];
                fprintf(stderr, "[v7-bs-voids] bldg%zu plane=%u voids=[", bi, pv->plane_id);
                for (uint32_t vi = 0; vi < pv->n_void_ids; vi++) {
                    uint32_t void_id = pv->void_ids[vi];
                    uint32_t hull_v = 0;
                    for (size_t hi = 0; hi < n_bldg_voids; hi++) {
                        if (bldg_voids[hi].void_id == void_id) { hull_v = bldg_voids[hi].n_hull_verts; break; }
                    }
                    fprintf(stderr, "%s%u(hull=%uv)", vi?",":"", void_id, hull_v);
                }
                fprintf(stderr, "]\n");
            }
            fprintf(stderr, "[v7-bs-voids] bldg%zu summary: %zu hulls available, %zu planes have voids\n",
                    bi, n_bldg_voids, n_bldg_plane_voids);
        }

        // Build output writer per-bldg.
        rg_writer_t w; rg_writer_init(&w);
        int prc = process_bldg(&w, topo_bytes, topo_len,
                                positions, total_points,
                                beam_dirs, pt_weights_bldg,
                                global_indices, n_pts_local,
                                cx, cy, cz, owner,
                                bldg_voids, n_bldg_voids,
                                bldg_plane_voids, n_bldg_plane_voids,
                                &bldg_params);
        // hull_xyz pointers alias into ghost_bytes_keep — nothing per-hull to free.
        free(bldg_voids); free(bldg_plane_voids); free(bldg_pv_storage);
        if (prc != 0) {
            rg_writer_free(&w);
            blob_push(&b_roof, NULL, 0);
            continue;
        }
        uint8_t* tlv_bytes = NULL; size_t tlv_len = 0;
        if (rg_writer_finalize(&w, &tlv_bytes, &tlv_len) != 0) {
            rg_writer_free(&w);
            blob_push(&b_roof, NULL, 0);
            continue;
        }
        rg_writer_free(&w);
        if (blob_push(&b_roof, tlv_bytes, tlv_len) != 0) {
            free(tlv_bytes); rc = -1; break;
        }
        free(tlv_bytes);
    }

    if (rc == 0) rc = emit_blob(ctx, "boundary-signals-roof-graph", &b_roof);
    blob_free(&b_roof);
    return rc;
}

// ── Step descriptor ──

static const step_read_t v7_boundary_signals_reads[] = {
    {.name = "lidar-positions"},
    {.name = "bi-directions"},
    {.name = "ts-weights",                    .array = true},
    {.name = "per-building-records",          .array = true},
    {.name = "building-local-frames",         .array = true},
    {.name = "grm-owner",                     .array = true},
    {.name = "boundary-topology-roof-graph",  .array = true},
    {.name = "bldg-thresholds",               .array = true},
    // For void-aware grid-fill contour: hulls from dv2 + the per-plane
    // void-id list from the topology roof-graph's RG_SECT_PLANE_VOIDS.
    // Optional — when dv2 didn't run upstream both are empty and the
    // contour falls back to the own-vs-nearby-only path.
    {.name = "dv2-ghost-planes",              .array = true},
};

static const step_write_t v7_boundary_signals_writes[] = {
    {.name = "boundary-signals-roof-graph", .kind = AK_ARRAY_INLINE,
     .visibility = STEP_VIS_PUBLIC, .array = true},
};

const step_def_t v7_boundary_signals_def = {
    .name             = "v7-boundary-signals",
    .version          = 7,  // v7 (2026-05-24): consume dv2 void hulls — for each plane,
                            //                  inject synthetic own-points (regular grid at
                            //                  expected spacing, weight = 1/density) inside
                            //                  each owned void hull. Cells inside known voids
                            //                  win the contest naturally; hull edges smoothed
                            //                  by Gaussian + ICM. Reads dv2-ghost-planes +
                            //                  parses RG_SECT_PLANE_VOIDS from topo roof-graph.
                            // v6 (2026-05-23): grid-fill contour now consumes
                            //                  nearby points (own>nearby per-cell);
                            //                  emits density_nearby + smoothed_nearby
                            //                  via PLANE_CONTOURS flag 0x08.
                            // v5 (2026-05-22): emit RG_SECT_PLANE_CONTOURS (grid-fill topology for bp2)
    .reads            = v7_boundary_signals_reads,
    .n_reads          = sizeof(v7_boundary_signals_reads) / sizeof(v7_boundary_signals_reads[0]),
    .writes           = v7_boundary_signals_writes,
    .n_writes         = sizeof(v7_boundary_signals_writes) / sizeof(v7_boundary_signals_writes[0]),
    .compute_location = STEP_CL_CPU,
    .streaming        = false,
    .exec             = v7_boundary_signals_exec,
};

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// v7-boundary-corners — step wrapper. Consumes the per-bldg TLV from
// v7-boundary-signals, runs Phase D (wall-bbox / RI / ridge-end / snap
// / side-filter / pool merge / coplanar) per building, emits:
//
//   - CORNERS (0x06) replaced — unified pool with planeIds, ridgeIds,
//     dirsByPlane, source, name, n_merged, AND an inline per-corner
//     pool-merge decomposition (members[] block; flag 0x40) carrying
//     source + authority + axis + ridge_id + plane_id per anchor for
//     boundary-regularize and for boundary-polygons D.3 per-plane NMS.
//     (Was section 0x07 CORNER_MEMBERS before 2026-05-19.)
//
// All other upstream sections (PLANES, RIDGES, RIDGE_LINES,
// NEARBY_PROJECTIONS, PLANE_GRIDS) flow through unchanged via
// rg_writer_passthrough.
//
// Reads:
//   lidar-positions                       (F64×3·N tile-wide)
//   per-building-records                  (per-bldg)
//   building-local-frames                 (per-bldg — centroid)
//   grm-owner                             (per-bldg Int16)
//   boundary-signals-roof-graph           (per-bldg TLV)
//
// Writes:
//   boundary-corners-roof-graph           (per-bldg ARRAY_INLINE of TLV)

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jansson.h>

#include "v3/artifact.h"
#include "v3/safe_alloc.h"
#include "v3/json_int.h"
#include "v3/blob.h"
#include "v3/step.h"
#include "v7-boundary-corners-impl.h"
#include "v7-plane-grids-parse.h"
#include "v7-roof-graph-format.h"

#define BLF_RECORD_BYTES 72

// ── Params ──

static int parse_params(const char* params_str, v7_bc_params_t* p) {
    v7_bc_params_default(p);
    p->up_axis = 2;
    if (!params_str) return 0;
    json_error_t je;
    json_t* obj = json_loads(params_str, 0, &je);
    if (!obj) return -1;
    json_t* j;
    if ((j = json_object_get(obj, "snapRadius"))            && json_is_number(j)) p->snap_radius             = json_number_value(j);
    if ((j = json_object_get(obj, "wallTiltMinDeg"))        && json_is_number(j)) p->wall_tilt_min_deg       = json_number_value(j);
    if ((j = json_object_get(obj, "poolMergeDist"))         && json_is_number(j)) p->pool_merge_dist         = json_number_value(j);
    if ((j = json_object_get(obj, "coplanarDistThreshold")) && json_is_number(j)) p->coplanar_dist_threshold = json_number_value(j);
    if ((j = json_object_get(obj, "coplanarBalanceMin"))    && json_is_number(j)) p->coplanar_balance_min    = json_number_value(j);
    if ((j = json_object_get(obj, "ridgeSideMaxDist"))      && json_is_number(j)) p->ridge_side_max_dist     = json_number_value(j);
    j = json_object_get(obj, "upAxis");
    if (json_int32_or_default(j, 0, 2, "upAxis", &p->up_axis) != 0) { json_decref(obj); return -1; }
    json_decref(obj);
    return 0;
}

// ── Upstream parse: PLANES, RIDGES, RIDGE_LINES, CORNERS, PLANE_GRIDS ──

typedef struct upstream {
    v7_bc_input_plane_t* planes; size_t n_planes;
    v7_bc_input_ridge_line_t* lines; size_t n_lines;
    v7_bc_input_corner_t* corners; size_t n_corners;
    v7_bc_input_plane_grid_t* grids; size_t n_grids;
    // Backing buffers for grid pointers + corner allocations.
    float** owned_oa;          size_t n_owned_oa;
    float** owned_outu;        size_t n_owned_outu;
    float** owned_outv;        size_t n_owned_outv;
    float** owned_bal;         size_t n_owned_bal;
    uint32_t* owned_planeids_block;  // single big chunk; we don't track per-corner
    uint32_t* owned_ridgeids_block;
} upstream_t;

static void upstream_free(upstream_t* u) {
    if (!u) return;
    free(u->planes);
    free(u->lines);
    for (size_t i = 0; i < u->n_corners; i++) {
        free(u->corners[i].plane_ids);
        free(u->corners[i].ridge_ids);
        free(u->corners[i].dirs);
        free(u->corners[i].com_bisectors);
        free(u->corners[i].name);
    }
    free(u->corners);
    free(u->grids);
    for (size_t i = 0; i < u->n_owned_oa; i++)   free(u->owned_oa[i]);
    for (size_t i = 0; i < u->n_owned_outu; i++) free(u->owned_outu[i]);
    for (size_t i = 0; i < u->n_owned_outv; i++) free(u->owned_outv[i]);
    for (size_t i = 0; i < u->n_owned_bal; i++)  free(u->owned_bal[i]);
    free(u->owned_oa); free(u->owned_outu); free(u->owned_outv); free(u->owned_bal);
    memset(u, 0, sizeof(*u));
}

static int parse_upstream_corners_block(rg_reader_t* r, upstream_t* u,
                                          double cx, double cy, double cz) {
    const uint8_t* sect; size_t slen;
    if (rg_reader_section(r, RG_SECT_CORNERS, &sect, &slen) != 0) return 0;
    // Variable-stride section — iterate via reader API.
    rg_record_iter_t it;
    if (rg_record_iter_init(&it, sect, slen) != 0) return 0;
    // Pre-count records.
    size_t cap = 16;
    u->corners = (v7_bc_input_corner_t*)calloc(cap, sizeof(*u->corners));
    if (!u->corners) return -1;
    const uint8_t* rec; uint32_t rec_len;
    while (rg_record_iter_next(&it, &rec, &rec_len)) {
        if (rec_len < 32) continue;
        if (u->n_corners == cap) {
            cap *= 2;
            v7_bc_input_corner_t* nb = (v7_bc_input_corner_t*)realloc(u->corners, cap * sizeof(*nb));
            if (!nb) return -1;
            memset(nb + u->n_corners, 0, (cap - u->n_corners) * sizeof(*nb));
            u->corners = nb;
        }
        v7_bc_input_corner_t* C = &u->corners[u->n_corners++];
        memcpy(&C->id,     rec + 0, 4);
        C->source = rec[4];
        uint8_t flags = rec[5];
        uint16_t n_merged; memcpy(&n_merged, rec + 6, 2); (void)n_merged;
        double pw[3]; memcpy(&pw[0], rec + 8, 8); memcpy(&pw[1], rec + 16, 8); memcpy(&pw[2], rec + 24, 8);
        // Unshift to building-local.
        C->point[0] = pw[0] - cx; C->point[1] = pw[1] - cy; C->point[2] = pw[2] - cz;
        size_t off = 32;
        // plane_ids prefix + source_per_plane[] trailing parallel array.
        // boundary-corners' input (from boundary-signals) carries no per-
        // plane source override, so we skip source_per_plane bytes — the
        // pool will rebuild them via v7p_entry_register_plane.
        uint32_t pc; memcpy(&pc, rec + off, 4); off += 8;
        if (pc > 0) {
            C->plane_ids = (uint32_t*)safe_malloc_array(pc, sizeof(uint32_t));
            if (!C->plane_ids) return -1;
            memcpy(C->plane_ids, rec + off, pc * 4);
            C->n_plane_ids = pc;
            off += pc * 4;
            off += pc;          // source_per_plane[] u8×pc — skipped on input
        }
        if (rg_parse_padded_u32_array(rec, &off, &C->ridge_ids, &C->n_ridge_ids) != 0) return -1;
        if (flags & 0x01u) {
            uint32_t nl; memcpy(&nl, rec + off, 4); off += 8;
            if (nl > 0) {
                C->name = (char*)malloc(nl + 1);
                if (!C->name) return -1;
                memcpy(C->name, rec + off, nl);
                C->name[nl] = '\0';
                off += (nl + 7) & ~(size_t)7;
            }
        }
        if (flags & 0x04u) {
            uint32_t dc; memcpy(&dc, rec + off, 4); off += 8;
            if (dc > 0) {
                C->dirs = (v7_bc_input_corner_dir_t*)safe_malloc_array(dc, sizeof(v7_bc_input_corner_dir_t));
                if (!C->dirs) return -1;
                for (uint32_t k = 0; k < dc; k++) {
                    memcpy(&C->dirs[k].plane_id, rec + off + 0, 4);
                    memcpy(C->dirs[k].dir_a,      rec + off + 8, 12);
                    memcpy(C->dirs[k].dir_b,      rec + off + 20, 12);
                    off += 32;
                }
                C->n_dirs = dc;
            }
        }
        if (flags & 0x20u) {
            // com_bisector_by_plane block — 16-byte entries
            // (u32 plane_id + f32×3 com_bisector, no _pad).
            uint32_t cc; memcpy(&cc, rec + off, 4); off += 8;
            if (cc > 0) {
                C->com_bisectors = (v7_bc_input_corner_com_bisector_t*)safe_malloc_array(cc, sizeof(v7_bc_input_corner_com_bisector_t));
                if (!C->com_bisectors) return -1;
                for (uint32_t k = 0; k < cc; k++) {
                    memcpy(&C->com_bisectors[k].plane_id,    rec + off + 0, 4);
                    memcpy(C->com_bisectors[k].com_bisector, rec + off + 4, 12);
                    off += 16;
                }
                C->n_com_bisectors = cc;
            }
        }
    }
    return 0;
}

static int parse_upstream_plane_grids(rg_reader_t* r, upstream_t* u) {
    // #72c Family A — extracted to v7-plane-grids-parse.c (shared with bp2).
    return v7_parse_plane_grids_section(r,
        &u->grids,     &u->n_grids,
        &u->owned_oa,   &u->n_owned_oa,
        &u->owned_outu, &u->n_owned_outu,
        &u->owned_outv, &u->n_owned_outv,
        &u->owned_bal,  &u->n_owned_bal);
}

static int parse_upstream_full(const uint8_t* tlv_bytes, size_t tlv_len,
                                 double cx, double cy, double cz,
                                 upstream_t* u) {
    memset(u, 0, sizeof(*u));
    rg_reader_t r;
    if (rg_reader_open(&r, tlv_bytes, tlv_len) != 0) return -1;
    // PLANES.
    const uint8_t* sect; size_t slen;
    if (rg_reader_section(&r, RG_SECT_PLANES, &sect, &slen) == 0) {
        uint32_t n, stride;
        rg_section_header(sect, slen, &n, &stride);
        if (stride == sizeof(rg_plane_record_t) && n > 0) {
            u->planes = (v7_bc_input_plane_t*)calloc(n, sizeof(*u->planes));
            if (!u->planes) return -1;
            const uint8_t* base = sect + RG_SECTION_HEADER_BYTES;
            for (uint32_t i = 0; i < n; i++) {
                const rg_plane_record_t* P = (const rg_plane_record_t*)(base + (size_t)i * stride);
                u->planes[i].id = P->id;
                u->planes[i].rejected = (P->flags & RG_PLANE_REJECTED) != 0;
                u->planes[i].tilt_deg = (double)P->tilt_deg;
                u->planes[i].plane_type = P->plane_type;  // authoritative type — for the decouple gate
                u->planes[i].normal[0] = P->normal[0];
                u->planes[i].normal[1] = P->normal[1];
                u->planes[i].normal[2] = P->normal[2];
                u->planes[i].d = P->d + (P->normal[0] * cx + P->normal[1] * cy + P->normal[2] * cz);
                memcpy(u->planes[i].basis_e1, P->basis_e1, 24);
                memcpy(u->planes[i].basis_e2, P->basis_e2, 24);
            }
            u->n_planes = n;
        }
    }
    // RIDGE_LINES + RIDGES (for plane_a/b).
    if (rg_reader_section(&r, RG_SECT_RIDGE_LINES, &sect, &slen) == 0) {
        uint32_t n, stride;
        rg_section_header(sect, slen, &n, &stride);
        if (stride == sizeof(rg_ridge_line_record_t) && n > 0) {
            u->lines = (v7_bc_input_ridge_line_t*)calloc(n, sizeof(*u->lines));
            if (!u->lines) return -1;
            const uint8_t* sect_r; size_t slen_r;
            const uint8_t* base_r = NULL; uint32_t n_r = 0, stride_r = 0;
            if (rg_reader_section(&r, RG_SECT_RIDGES, &sect_r, &slen_r) == 0) {
                rg_section_header(sect_r, slen_r, &n_r, &stride_r);
                base_r = sect_r + RG_SECTION_HEADER_BYTES;
            }
            const uint8_t* base = sect + RG_SECTION_HEADER_BYTES;
            for (uint32_t i = 0; i < n; i++) {
                const rg_ridge_line_record_t* L = (const rg_ridge_line_record_t*)(base + (size_t)i * stride);
                u->lines[i].has_line = (L->flags & 0x01u) != 0;
                u->lines[i].has_side_a = (L->flags & 0x02u) != 0;
                u->lines[i].has_side_b = (L->flags & 0x04u) != 0;
                u->lines[i].line_point[0] = L->line_point[0] - cx;
                u->lines[i].line_point[1] = L->line_point[1] - cy;
                u->lines[i].line_point[2] = L->line_point[2] - cz;
                u->lines[i].line_direction[0] = L->line_direction[0];
                u->lines[i].line_direction[1] = L->line_direction[1];
                u->lines[i].line_direction[2] = L->line_direction[2];
                u->lines[i].t_min_a = L->t_min_a;
                u->lines[i].t_max_a = L->t_max_a;
                u->lines[i].t_min_b = L->t_min_b;
                u->lines[i].t_max_b = L->t_max_b;
                memcpy(u->lines[i].side_vec_a, L->side_vec_a, 12);
                memcpy(u->lines[i].side_vec_b, L->side_vec_b, 12);
                if (base_r && stride_r == sizeof(rg_ridge_record_t) && (uint32_t)i < n_r) {
                    const rg_ridge_record_t* R = (const rg_ridge_record_t*)(base_r + (size_t)i * stride_r);
                    u->lines[i].plane_a = R->plane_a;
                    u->lines[i].plane_b = R->plane_b;
                }
            }
            u->n_lines = n;
        }
    }
    // CORNERS.
    if (parse_upstream_corners_block(&r, u, cx, cy, cz) != 0) return -1;
    // PLANE_GRIDS.
    if (parse_upstream_plane_grids(&r, u) != 0) return -1;
    return 0;
}

// ── Pack CORNERS (replacement) — same layout as v7-boundary-signals,
//    plus an inline members[] tail block when n_members > 0 (flag 0x40).
//    See ROOF-GRAPH-FORMAT.md §4.6. ──

static int pack_corner_record(rg_writer_t* w, const v7_bc_corner_t* C,
                                double cx, double cy, double cz) {
    size_t hdr_bytes = 32;
    // Plane block: 8B header + u32×n_plane_ids + u8×n_plane_ids + align8.
    // The trailing u8 array is `source_per_plane[]`, parallel to plane_ids[].
    // See ROOF-GRAPH-FORMAT.md §4.6.
    size_t plane_pfx = 8 + C->n_plane_ids * 4 + C->n_plane_ids;
    plane_pfx = (plane_pfx + 7) & ~(size_t)7;
    size_t ridge_pfx = 8 + C->n_ridge_ids * 4;
    ridge_pfx = (ridge_pfx + 7) & ~(size_t)7;
    size_t name_len = C->name ? strlen(C->name) : 0;
    bool has_name = (name_len > 0);
    size_t name_block = 0;
    if (has_name) {
        name_block = 8 + name_len;
        name_block = (name_block + 7) & ~(size_t)7;
    }
    bool has_dirs = (C->n_dirs > 0 && C->dirs != NULL);
    size_t dirs_block = 0;
    if (has_dirs) dirs_block = 8 + (size_t)C->n_dirs * 32;
    bool has_com_bis = (C->n_com_bisectors > 0 && C->com_bisectors != NULL);
    size_t com_bis_block = 0;
    if (has_com_bis) com_bis_block = 8 + (size_t)C->n_com_bisectors * 16;
    // Inline members tail (was the separate CORNER_MEMBERS section before
    // 2026-05-19). 8 B prefix + 48 B per member; member layout in §4.6.
    bool has_members = (C->n_members > 0 && C->members != NULL);
    size_t members_block = 0;
    if (has_members) members_block = 8 + (size_t)C->n_members * 48;
    size_t rec_len = hdr_bytes + plane_pfx + ridge_pfx + name_block + dirs_block + com_bis_block + members_block;
    uint8_t* rec = (uint8_t*)calloc(rec_len, 1);
    if (!rec) return -1;
    memcpy(rec + 0, &C->id, 4);
    rec[4] = C->source;
    rec[5] = (uint8_t)((has_name ? 0x01u : 0u)
                       | (has_dirs ? 0x04u : 0u)
                       | (has_com_bis ? 0x20u : 0u)
                       | (has_members ? 0x40u : 0u));
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
    if (C->n_plane_ids > 0 && C->source_per_plane) {
        memcpy(rec + off, C->source_per_plane, C->n_plane_ids);
        // align-8 padding already zeroed by calloc.
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
    }
    if (has_dirs) {
        off = hdr_bytes + plane_pfx + ridge_pfx + name_block;
        uint32_t dcount = C->n_dirs;
        memcpy(rec + off, &dcount, 4); memcpy(rec + off + 4, &pad, 4); off += 8;
        for (uint32_t k = 0; k < C->n_dirs; k++) {
            memcpy(rec + off + 0,  &C->dirs[k].plane_id, 4);
            memcpy(rec + off + 8,  C->dirs[k].dir_a, 12);
            memcpy(rec + off + 20, C->dirs[k].dir_b, 12);
            off += 32;
        }
    }
    if (has_com_bis) {
        off = hdr_bytes + plane_pfx + ridge_pfx + name_block + dirs_block;
        uint32_t ccount = C->n_com_bisectors;
        memcpy(rec + off, &ccount, 4); memcpy(rec + off + 4, &pad, 4); off += 8;
        // Entry stride 16 B = u32 plane_id + f32×3 com_bisector (no _pad).
        for (uint32_t k = 0; k < C->n_com_bisectors; k++) {
            memcpy(rec + off + 0, &C->com_bisectors[k].plane_id, 4);
            memcpy(rec + off + 4, C->com_bisectors[k].com_bisector, 12);
            off += 16;
        }
    }
    if (has_members) {
        off = hdr_bytes + plane_pfx + ridge_pfx + name_block + dirs_block + com_bis_block;
        uint32_t mcount = C->n_members;
        memcpy(rec + off, &mcount, 4); memcpy(rec + off + 4, &pad, 4); off += 8;
        // 48-B member stride — see §4.6.
        for (uint32_t k = 0; k < C->n_members; k++) {
            const v7_bc_member_t* m = &C->members[k];
            double mpw[3] = { m->point[0] + cx, m->point[1] + cy, m->point[2] + cz };
            memcpy(rec + off + 0,  &mpw[0], 8);
            memcpy(rec + off + 8,  &mpw[1], 8);
            memcpy(rec + off + 16, &mpw[2], 8);
            rec[off + 24] = m->source;
            rec[off + 25] = (uint8_t)m->authority;
            rec[off + 26] = m->has_axis ? 1u : 0u;
            rec[off + 27] = m->has_ridge_id ? 1u : 0u;
            if (m->has_axis) memcpy(rec + off + 28, m->axis, 12);
            if (m->has_ridge_id) memcpy(rec + off + 40, &m->ridge_id, 4);
            memcpy(rec + off + 44, &m->plane_id, 4);
            off += 48;
        }
    }
    int rc = rg_writer_record(w, rec, rec_len);
    free(rec);
    return rc;
}

// ── Per-bldg exec ──

static int process_bldg(rg_writer_t* w,
                        const uint8_t* upstream_bytes, size_t upstream_len,
                        const double* positions, size_t total_points,
                        const uint32_t* global_indices, uint32_t n_pts_local,
                        double cx, double cy, double cz,
                        const int16_t* owner,
                        const v7_bc_params_t* params) {
    // 1. Build building-local positions.
    double* local_pos = (double*)malloc((size_t)n_pts_local * 3 * sizeof(double));
    float*  local_f32 = (float*) malloc((size_t)n_pts_local * 3 * sizeof(float));
    if (!local_pos || !local_f32) { free(local_pos); free(local_f32); return -1; }
    for (uint32_t i = 0; i < n_pts_local; i++) {
        uint32_t gi = global_indices[i];
        if ((size_t)gi >= total_points) { free(local_pos); free(local_f32); return -1; }
        local_f32[i*3 + 0] = (float)(positions[gi*3 + 0] - cx);
        local_f32[i*3 + 1] = (float)(positions[gi*3 + 1] - cy);
        local_f32[i*3 + 2] = (float)(positions[gi*3 + 2] - cz);
    }
    for (size_t i = 0; i < (size_t)n_pts_local * 3; i++) local_pos[i] = (double)local_f32[i];
    free(local_f32);

    // 2. Parse upstream — all sections we need.
    upstream_t up = {0};
    if (parse_upstream_full(upstream_bytes, upstream_len, cx, cy, cz, &up) != 0) {
        free(local_pos);
        upstream_free(&up);
        return -1;
    }
    // 3. Compute.
    v7_bc_outputs_t out;
    v7_bc_outputs_init(&out);
    int compute_rc = v7_bc_compute(local_pos, n_pts_local,
                                    owner,
                                    up.planes, up.n_planes,
                                    up.lines, up.n_lines,
                                    up.grids, up.n_grids,
                                    up.corners, up.n_corners,
                                    params, &out);
    free(local_pos);
    if (compute_rc != 0) {
        upstream_free(&up); v7_bc_outputs_free(&out);
        return -1;
    }

    // 4. Pass through upstream sections EXCEPT CORNERS (we replace it).
    rg_reader_t r;
    if (rg_reader_open(&r, upstream_bytes, upstream_len) != 0) {
        upstream_free(&up); v7_bc_outputs_free(&out); return -1;
    }
    if (rg_writer_passthrough(w, &r, RG_SECT_CORNERS) != 0) {
        upstream_free(&up); v7_bc_outputs_free(&out); return -1;
    }

    // 5. CORNERS section (replacement).
    if (out.n_corners > 0) {
        if (rg_writer_section_begin(w, RG_SECT_CORNERS, 0) != 0) {
            upstream_free(&up); v7_bc_outputs_free(&out); return -1;
        }
        for (size_t i = 0; i < out.n_corners; i++) {
            if (pack_corner_record(w, &out.corners[i], cx, cy, cz) != 0) {
                upstream_free(&up); v7_bc_outputs_free(&out); return -1;
            }
        }
        if (rg_writer_section_end(w) != 0) {
            upstream_free(&up); v7_bc_outputs_free(&out); return -1;
        }
    }

    upstream_free(&up); v7_bc_outputs_free(&out);
    return 0;
}

// ── Exec ──

static int v7_boundary_corners_exec(step_context_t* ctx) {
    v7_bc_params_t params;
    if (parse_params(step_get_params(ctx), &params) != 0) return -1;
    if (getenv("DERIV_LOG")) {
        fprintf(stderr, "[DERIV] step=v7-boundary-corners scope=tile snap_radius=%.6f wall_tilt_min_deg=%.3f wall_side_margin=%.6f wall_min_h_extent=%.6f pool_merge_dist=%.6f coplanar_dist_threshold=%.6f coplanar_balance_min=%.6f ridge_side_max_dist=%.6f\n",
                params.snap_radius, params.wall_tilt_min_deg,
                params.wall_side_margin, params.wall_min_h_extent,
                params.pool_merge_dist, params.coplanar_dist_threshold,
                params.coplanar_balance_min, params.ridge_side_max_dist);
    }

    size_t pos_bytes = 0;
    const double* positions = (const double*)step_read(ctx, "lidar-positions", &pos_bytes);
    if (!positions || pos_bytes % (3 * sizeof(double)) != 0) return -1;
    size_t total_points = pos_bytes / (3 * sizeof(double));

    size_t n_buildings = step_array_len(ctx, "per-building-records");
    if (step_array_len(ctx, "grm-owner")                    != n_buildings ||
        step_array_len(ctx, "building-local-frames")        != n_buildings ||
        step_array_len(ctx, "boundary-signals-roof-graph") != n_buildings) {
        fprintf(stderr, "[v7-boundary-corners] count mismatch across per-bldg arrays\n");
        return -1;
    }

    blob_t b_roof = {0};
    int rc = 0;

    for (size_t bi = 0; bi < n_buildings; bi++) {
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

        const uint8_t* frame_bytes = NULL; size_t frame_len = 0;
        if (step_array_read_bytes(ctx, "building-local-frames", bi, &frame_bytes, &frame_len) != 0
            || !frame_bytes || frame_len < BLF_RECORD_BYTES) { blob_push(&b_roof, NULL, 0); continue; }
        double cx, cy, cz;
        memcpy(&cx, frame_bytes +  0, 8);
        memcpy(&cy, frame_bytes +  8, 8);
        memcpy(&cz, frame_bytes + 16, 8);

        const uint8_t* own_bytes = NULL; size_t own_len = 0;
        if (step_array_read_bytes(ctx, "grm-owner", bi, &own_bytes, &own_len) != 0
            || !own_bytes || own_len != (size_t)n_pts_local * sizeof(int16_t)) {
            blob_push(&b_roof, NULL, 0); continue;
        }
        const int16_t* owner = (const int16_t*)own_bytes;

        const uint8_t* up_bytes = NULL; size_t up_len = 0;
        if (step_array_read_bytes(ctx, "boundary-signals-roof-graph", bi, &up_bytes, &up_len) != 0
            || !up_bytes || up_len < 32) {
            blob_push(&b_roof, NULL, 0); continue;
        }

        rg_writer_t w; rg_writer_init(&w);
        int prc = process_bldg(&w, up_bytes, up_len,
                                positions, total_points,
                                global_indices, n_pts_local,
                                cx, cy, cz, owner, &params);
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

    if (rc == 0) rc = emit_blob(ctx, "boundary-corners-roof-graph", &b_roof);
    blob_free(&b_roof);
    return rc;
}

// ── Step descriptor ──

static const step_read_t v7_boundary_corners_reads[] = {
    {.name = "lidar-positions"},
    {.name = "per-building-records",         .array = true},
    {.name = "building-local-frames",        .array = true},
    {.name = "grm-owner",                    .array = true},
    {.name = "boundary-signals-roof-graph",  .array = true},
};

static const step_write_t v7_boundary_corners_writes[] = {
    {.name = "boundary-corners-roof-graph", .kind = AK_ARRAY_INLINE,
     .visibility = STEP_VIS_PUBLIC, .array = true},
};

const step_def_t v7_boundary_corners_def = {
    .name             = "v7-boundary-corners",
    .version          = 10,  // v10: type-gated corner merge (refuse roof/wall/ground fusion in pool super-merge + coplanar import) — 2026-07-07. v9: D.2b judges each corner in ITS OWN plane's frame (per-plane side_vec_a/b, kill iff dot<0); fixes steep-dihedral blindness — 2026-06-11
    .reads            = v7_boundary_corners_reads,
    .n_reads          = sizeof(v7_boundary_corners_reads) / sizeof(v7_boundary_corners_reads[0]),
    .writes           = v7_boundary_corners_writes,
    .n_writes         = sizeof(v7_boundary_corners_writes) / sizeof(v7_boundary_corners_writes[0]),
    .compute_location = STEP_CL_CPU,
    .streaming        = false,
    .exec             = v7_boundary_corners_exec,
};
